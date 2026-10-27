//! Spoken Voice: a Windows speech (SAPI 5) engine that speaks with small Piper models, so WoW's
//! text to speech (C_VoiceChat.SpeakText) can use them like any installed voice.
//!
//! Each voice is a SAPI voice token in the registry whose CLSID is this engine's and which names
//! its model folder (`ModelDir`: model.onnx, tokens.txt), espeak-ng's data (`DataDir`) and, for a
//! model with several speakers, the speaker (`SpeakerId`). Models are loaded once per process and
//! shared by every utterance that uses them; speech is written sentence by sentence as it is made.
//!
//! Model loads and anything that went wrong are always logged. With the log's `verbose` switch on,
//! every call the program makes into the engine is too, with how long it took, on which thread, and
//! from where (the modules on the calling thread's stack, as module+offset): what the engine is asked
//! to do while a game loads, and how long it holds it up.

mod log;

use std::collections::HashMap;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Instant;

use sherpa_onnx::{GenerationConfig, OfflineTts, OfflineTtsConfig, OfflineTtsModelConfig, OfflineTtsVitsModelConfig};
use windows::Win32::Foundation::{CLASS_E_CLASSNOTAVAILABLE, CLASS_E_NOAGGREGATION, E_FAIL, E_POINTER, HMODULE, S_FALSE};
use windows::Win32::Media::Audio::{WAVE_FORMAT_PCM, WAVEFORMATEX};
use windows::Win32::Media::Speech::*;
use windows::Win32::System::Com::{CoTaskMemAlloc, CoTaskMemFree, IClassFactory, IClassFactory_Impl};
use windows::Win32::System::Diagnostics::Debug::RtlCaptureStackBackTrace;
use windows::Win32::System::LibraryLoader::{GetModuleFileNameW, GetModuleHandleExW};
use windows::Win32::System::ProcessStatus::{GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS};
use windows::Win32::System::Registry::{HKEY_LOCAL_MACHINE, RRF_RT_REG_DWORD, RegGetValueW};
use windows::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::Security::{GetSidSubAuthority, GetSidSubAuthorityCount, GetTokenInformation, TOKEN_MANDATORY_LABEL,
    TOKEN_QUERY, TokenIntegrityLevel};
use windows::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
use windows::core::{BOOL, GUID, HRESULT, IUnknown, Interface, PCWSTR, Ref, Result, implement, w};

/// This DLL, set when it is loaded: its frames are left out of the callers logged.
static MODULE: AtomicUsize = AtomicUsize::new(0);
static FACTORIES: AtomicU32 = AtomicU32::new(0);
static INSTANCES: AtomicU32 = AtomicU32::new(0);
static TOKENS: AtomicU32 = AtomicU32::new(0);

/// Who called: the modules on this thread's stack outside the engine, nearest first, as
/// module+offset (Wow.exe+0x1a2b3c < sapi.dll+0x4d5e). Only addresses, read from the engine's own
/// call; nothing of the program's is changed.
fn caller() -> String {
    let mut frames = [std::ptr::null_mut(); 32];
    let count = unsafe { RtlCaptureStackBackTrace(1, &mut frames, None) } as usize;
    let ours = MODULE.load(Ordering::Relaxed);
    let mut chain: Vec<String> = Vec::new();
    for &address in &frames[..count] {
        let mut module = HMODULE::default();
        // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
        if unsafe { GetModuleHandleExW(4 | 2, PCWSTR(address as *const u16), &mut module) }.is_err() {
            chain.push(format!("{address:p}"));
            continue;
        }
        if module.0 as usize == ours {
            continue;
        }
        let mut name = [0u16; 260];
        let length = unsafe { GetModuleFileNameW(Some(module), &mut name) } as usize;
        let path = String::from_utf16_lossy(&name[..length]);
        let file = path.rsplit('\\').next().unwrap_or(&path).to_string();
        chain.push(format!("{file}+{:#x}", address as usize - module.0 as usize));
        if chain.len() >= 10 {
            break;
        }
    }
    chain.join(" < ")
}

/// The program's memory: what it has in RAM and what it has committed, in MB.
fn memory() -> String {
    let mut counters = PROCESS_MEMORY_COUNTERS { cb: size_of::<PROCESS_MEMORY_COUNTERS>() as u32, ..Default::default() };
    match unsafe { GetProcessMemoryInfo(GetCurrentProcess(), &mut counters, counters.cb) } {
        Ok(()) => format!("{} MB in RAM, {} MB committed", counters.WorkingSetSize >> 20, counters.PagefileUsage >> 20),
        Err(_) => "memory unknown".into(),
    }
}

/// The program's integrity level (low means sandboxed: it may write to AppData\LocalLow only).
fn integrity() -> String {
    unsafe {
        let mut token = HANDLE::default();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token).is_err() {
            return "unknown".into();
        }
        let mut buffer = [0u8; 256];
        let mut length = 0u32;
        let read = GetTokenInformation(token, TokenIntegrityLevel, Some(buffer.as_mut_ptr() as *mut c_void),
            buffer.len() as u32, &mut length);
        let _ = CloseHandle(token);
        if read.is_err() {
            return "unknown".into();
        }
        let label = &*(buffer.as_ptr() as *const TOKEN_MANDATORY_LABEL);
        let count = *GetSidSubAuthorityCount(label.Label.Sid) as u32;
        let level = *GetSidSubAuthority(label.Label.Sid, count.saturating_sub(1));
        let name = match level {
            0..=0x0fff => "untrusted",
            0x1000..=0x1fff => "low",
            0x2000..=0x2fff => "medium",
            0x3000..=0x3fff => "high",
            _ => "system",
        };
        format!("{name} ({level:#x})")
    }
}

/// Logs, once, which program loaded the engine: the first sign it is in use there at all.
fn announce() {
    static DONE: OnceLock<()> = OnceLock::new();
    DONE.get_or_init(|| {
        let exe = std::env::current_exe().map(|p| p.display().to_string()).unwrap_or_default();
        log::write(&format!("engine loaded into {exe} ({:?} before its first use; {}; integrity {}; log in {:?})",
            log::start().elapsed(), memory(), integrity(), log::folder()));
    });
}

/// This engine's class: the CLSID the voice tokens name.
pub const CLSID_SPOKEN_VOICE: GUID = GUID::from_u128(0x055bf9e8_68b3_4cb2_8090_50895f4bcc68);
/// SAPI's format ID for "a WAVEFORMATEX follows" (SPDFID_WaveFormatEx).
const SPDFID_WAVEFORMATEX: GUID = GUID::from_u128(0xc31adbae_527f_4ff5_a230_f62bb61ff70c);
const DEFAULT_RATE: u32 = 22050;

/// A loaded model, shared by every engine instance (every SpVoice) that speaks with it. One
/// utterance at a time per model: each takes a fraction of its length to make, so lines that
/// overlap in the game only wait a moment for each other. Up to max_models of them stay loaded.
struct Model {
    tts: OfflineTts,
    sample_rate: u32,
    speakers: i32,
    busy: Mutex<()>,
}

/// A speaker code at the start of the text, `{sp:3}`: the model's speaker to say it with, in place of
/// the token's. The voicepack registers one voice per model (WoW looks at every installed voice
/// several times over for each one while it loads, so a voice per speaker held it up for half a
/// minute); the addon picks the speaker this way. Taken out of the text either way; a speaker the
/// model does not have is ignored.
fn take_speaker(parts: &mut Vec<Part>, speakers: i32) -> Option<i32> {
    let index = parts.iter().position(|part| matches!(part, Part::Say(run) if !run.text.trim().is_empty()))?;
    let Part::Say(run) = &mut parts[index] else { return None };
    let text = run.text.trim_start();
    let rest = text.strip_prefix("{sp:")?;
    let end = rest.find('}')?;
    let speaker = rest[..end].parse::<i32>().ok();
    run.text = rest[end + 1..].trim_start().to_string();
    if run.text.is_empty() {
        parts.remove(index);
    }
    speaker.filter(|s| (0..speakers).contains(s))
}

/// One model's place in the cache: the model once loaded (None before, or after it was unloaded to
/// make room), and when it was last asked for. Each model loads under its own lock, so a model being
/// loaded holds up only the lines waiting for that model, never a line of one already loaded.
struct Slot {
    model: Mutex<Option<Arc<Model>>>,
    used: AtomicU64,
}

fn slots() -> &'static Mutex<HashMap<String, Arc<Slot>>> {
    static SLOTS: OnceLock<Mutex<HashMap<String, Arc<Slot>>>> = OnceLock::new();
    SLOTS.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Ticks for "last asked for": a count, not a time.
static CLOCK: AtomicU64 = AtomicU64::new(0);

/// What a loaded model takes in RAM, about: 74 MB of weights and onnxruntime's graph and buffers
/// around them (measured 2026-10-25: 101 to 127 MB each, 48 models).
const MODEL_MB: u64 = 110;

/// How many models stay loaded at once: as many as fit in a tenth of the PC's RAM (8 GB: 7, 16 GB:
/// 14, 32 GB: 29, so about 0.8, 1.5 and 3.2 GB), at least 2. Past that, the one asked for longest ago
/// is unloaded. `MaxModels` (DWORD) under HKLM\SOFTWARE\Spoken Players sets it instead, as does the
/// environment variable SPOKEN_VOICE_MAX_MODELS (for trying it out).
fn max_models() -> usize {
    static MAX: OnceLock<usize> = OnceLock::new();
    *MAX.get_or_init(|| {
        let mut chosen = 0u32;
        let mut size = size_of::<u32>() as u32;
        let set = unsafe {
            RegGetValueW(HKEY_LOCAL_MACHINE, w!("SOFTWARE\\Spoken Players"), w!("MaxModels"), RRF_RT_REG_DWORD, None,
                Some(&mut chosen as *mut u32 as *mut c_void), Some(&mut size))
        };
        let (count, why) = if set.is_ok() && chosen > 0 {
            (chosen as usize, "MaxModels".to_string())
        } else if let Some(count) = std::env::var("SPOKEN_VOICE_MAX_MODELS").ok().and_then(|v| v.parse::<usize>().ok()) {
            (count.max(1), "SPOKEN_VOICE_MAX_MODELS".to_string())
        } else {
            let mut status = MEMORYSTATUSEX { dwLength: size_of::<MEMORYSTATUSEX>() as u32, ..Default::default() };
            let total = if unsafe { GlobalMemoryStatusEx(&mut status) }.is_ok() { status.ullTotalPhys } else { 8 << 30 };
            (((total >> 20) / 10 / MODEL_MB).max(2) as usize, format!("a tenth of {} GB of RAM", (total + (1 << 29)) >> 30))
        };
        log::write(&format!("keeps up to {count} model(s) loaded ({why}, ~{} MB)", count as u64 * MODEL_MB));
        count
    })
}

/// Over the limit (max_models), the models asked for longest ago are unloaded, but never `keep` (the
/// one just loaded), one being loaded or one speaking. A model an engine instance still holds stays
/// in memory until that instance lets go of it; it is only out of the cache.
fn make_room(keep: &str) {
    let Ok(map) = slots().lock() else { return };
    let loaded = |slot: &Slot| slot.model.try_lock().map(|model| model.is_some()).unwrap_or(true);
    let mut count = map.values().filter(|slot| loaded(slot)).count();
    let limit = max_models();
    while count > limit {
        let oldest = map.iter()
            .filter(|(key, slot)| key.as_str() != keep && slot.model.try_lock().is_ok_and(|model| {
                model.as_ref().is_some_and(|m| m.busy.try_lock().is_ok())
            }))
            .min_by_key(|(_, slot)| slot.used.load(Ordering::Relaxed));
        let Some((key, slot)) = oldest else { break };
        // Taken meanwhile (loading again): left as it is.
        let Ok(mut model) = slot.model.try_lock() else { break };
        *model = None;
        drop(model);
        count -= 1;
        log::write(&format!("unloaded {} to make room ({count} model(s) now, up to {limit}; {})",
            key.split('|').next().unwrap_or(key), memory()));
    }
}

/// Piper's sampling for a model whose voice.json does not set it: the noise of the sound itself
/// (`noise_scale`) and of each sound's length (`noise_w`), below Piper's own 0.667 and 0.8.
const NOISE_SCALE: f32 = 0.6;
const NOISE_W: f32 = 0.65;
const LENGTH_SCALE: f32 = 1.0;

/// A number from the model's voice.json (`"inference": {"noise_scale": 0.6, ...}`, as its pack has
/// it), else `default`. The keys are unique in that file, so a plain search does; no JSON parser for
/// three numbers.
fn inference(info: &str, key: &str, default: f32) -> f32 {
    let Some(at) = info.find(&format!("\"{key}\"")) else { return default };
    let rest = info[at + key.len() + 2..].trim_start().strip_prefix(':').unwrap_or("").trim_start();
    let end = rest.find(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-')).unwrap_or(rest.len());
    rest[..end].parse().ok().filter(|v: &f32| v.is_finite() && *v >= 0.0).unwrap_or(default)
}

fn load_model(model_dir: &str, data_dir: &str) -> Option<Arc<Model>> {
    let key = format!("{model_dir}|{data_dir}");
    let slot = slots().lock().ok()?.entry(key.clone())
        .or_insert_with(|| Arc::new(Slot { model: Mutex::new(None), used: AtomicU64::new(0) })).clone();
    slot.used.store(CLOCK.fetch_add(1, Ordering::Relaxed) + 1, Ordering::Relaxed);
    let waited = Instant::now();
    let mut cell = slot.model.lock().ok()?;
    if waited.elapsed().as_millis() >= 10 {
        log::write(&format!("waited {:?} for {model_dir} to finish loading", waited.elapsed()));
    }
    if let Some(model) = cell.as_ref() {
        return Some(model.clone());
    }
    log::write(&format!("loading {model_dir} ..."));
    let started = Instant::now();
    let info = std::fs::read_to_string(format!("{model_dir}\\voice.json")).unwrap_or_default();
    let (noise_scale, noise_w, length_scale) = (inference(&info, "noise_scale", NOISE_SCALE),
        inference(&info, "noise_w", NOISE_W), inference(&info, "length_scale", LENGTH_SCALE));
    let config = OfflineTtsConfig {
        model: OfflineTtsModelConfig {
            vits: OfflineTtsVitsModelConfig {
                model: Some(format!("{model_dir}\\model.onnx")),
                tokens: Some(format!("{model_dir}\\tokens.txt")),
                data_dir: Some(data_dir.to_string()),
                noise_scale,
                noise_scale_w: noise_w,
                length_scale,
                ..Default::default()
            },
            num_threads: 2,
            debug: false,
            provider: Some("cpu".into()),
            ..Default::default()
        },
        // One sentence per callback, so speech starts as soon as the first sentence is made.
        max_num_sentences: 1,
        ..Default::default()
    };
    let Some(tts) = OfflineTts::create(&config) else {
        log::write(&format!("could not load the model in {model_dir} (espeak data: {data_dir})"));
        return None;
    };
    let sample_rate = u32::try_from(tts.sample_rate()).unwrap_or(DEFAULT_RATE);
    let speakers = tts.num_speakers().max(1);
    let model = Arc::new(Model { tts, sample_rate, speakers, busy: Mutex::new(()) });
    *cell = Some(model.clone());
    drop(cell);
    log::write(&format!("loaded {model_dir}: {sample_rate} Hz, {speakers} speaker(s), noise {noise_scale}/{noise_w}, {:?} ({})",
        started.elapsed(), memory()));
    make_room(&key);
    Some(model)
}

/// The voice a token names: its model and speaker.
#[derive(Clone)]
struct Voice {
    model: Arc<Model>,
    speaker: i32,
    name: String,
}

#[implement(ISpTTSEngine, ISpObjectWithToken)]
struct Engine {
    token: Mutex<Option<ISpObjectToken>>,
    voice: Mutex<Option<Voice>>,
}

fn token_string(token: &ISpObjectToken, name: PCWSTR) -> Option<String> {
    unsafe {
        let value = token.GetStringValue(name).ok()?;
        let text = value.to_string().ok();
        CoTaskMemFree(Some(value.0 as *const c_void));
        text
    }
}

fn token_dword(token: &ISpObjectToken, name: PCWSTR) -> Option<u32> {
    let mut value = 0u32;
    unsafe { token.GetDWORD(name, &mut value).ok()? };
    Some(value)
}

impl ISpObjectWithToken_Impl for Engine_Impl {
    fn SetObjectToken(&self, ptoken: Ref<ISpObjectToken>) -> Result<()> {
        let token = ptoken.ok()?.clone();
        let started = Instant::now();
        let number = TOKENS.fetch_add(1, Ordering::Relaxed) + 1;
        let result = catch_unwind(AssertUnwindSafe(|| {
            let name = token_string(&token, PCWSTR::null()).unwrap_or_else(|| "Spoken voice".into());
            let model_dir = token_string(&token, w!("ModelDir"));
            let data_dir = token_string(&token, w!("DataDir"));
            let (Some(model_dir), Some(data_dir)) = (model_dir, data_dir) else {
                log::write(&format!("{name}: the voice token has no ModelDir or DataDir"));
                return None;
            };
            let speaker = token_dword(&token, w!("SpeakerId")).unwrap_or(0) as i32;
            let model = load_model(&model_dir, &data_dir)?;
            if log::enabled() {
                log::write(&format!("voice #{number} {name}: {:?} (from {})", started.elapsed(), caller()));
            }
            Some(Voice { model, speaker, name })
        }));
        match result {
            Ok(Some(voice)) => {
                *self.voice.lock().map_err(|_| E_FAIL)? = Some(voice);
                *self.token.lock().map_err(|_| E_FAIL)? = Some(token);
                Ok(())
            }
            Ok(None) => Err(E_FAIL.into()),
            Err(_) => {
                log::write("panic while loading a voice");
                Err(E_FAIL.into())
            }
        }
    }

    fn GetObjectToken(&self) -> Result<ISpObjectToken> {
        self.token.lock().map_err(|_| E_FAIL)?.clone().ok_or_else(|| E_POINTER.into())
    }
}

/// A stretch of text to say, with SAPI's adjustments as they stood for it.
struct Run {
    text: String,
    rate: i32,
    pitch: i32,
    volume: u32,
}

enum Part {
    Say(Run),
    Silence(u32),
}

/// The text fragments SAPI hands over (it has already parsed any XML tags into their state),
/// joined into runs that share their rate, pitch and volume.
unsafe fn parts(mut fragment: *const SPVTEXTFRAG) -> Vec<Part> {
    let mut parts: Vec<Part> = Vec::new();
    while let Some(frag) = unsafe { fragment.as_ref() } {
        let state = &frag.State;
        let action = state.eAction;
        if action == SPVA_Speak || action == SPVA_SpellOut || action == SPVA_Pronounce {
            if !frag.pTextStart.is_null() && frag.ulTextLen > 0 {
                let units = unsafe { std::slice::from_raw_parts(frag.pTextStart.0, frag.ulTextLen as usize) };
                let text = String::from_utf16_lossy(units);
                let (rate, pitch, volume) = (state.RateAdj, state.PitchAdj.MiddleAdj, state.Volume);
                match parts.last_mut() {
                    Some(Part::Say(run)) if run.rate == rate && run.pitch == pitch && run.volume == volume => {
                        run.text.push(' ');
                        run.text.push_str(&text);
                    }
                    _ => parts.push(Part::Say(Run { text, rate, pitch, volume })),
                }
            }
        } else if action == SPVA_Silence {
            parts.push(Part::Silence(state.SilenceMSecs));
        }
        fragment = frag.pNext;
    }
    parts
}

/// Writes 16-bit samples to SAPI, all of them. False when SAPI will take no more.
fn write_samples(site: &ISpTTSEngineSite, samples: &[i16]) -> bool {
    let mut bytes: &[u8] = unsafe { std::slice::from_raw_parts(samples.as_ptr() as *const u8, samples.len() * 2) };
    while !bytes.is_empty() {
        match unsafe { site.Write(bytes.as_ptr() as *const c_void, bytes.len() as u32) } {
            Ok(0) | Err(_) => return false,
            Ok(written) => bytes = &bytes[(written as usize).min(bytes.len())..],
        }
    }
    true
}

fn aborted(site: &ISpTTSEngineSite) -> bool {
    unsafe { site.GetActions() as i32 & SPVES_ABORT.0 != 0 }
}

/// `samples` played `factor` times as fast (linear interpolation): higher by that factor, and
/// shorter; the caller made them that much slower to begin with.
fn resample(samples: &[f32], factor: f32) -> Vec<f32> {
    if (factor - 1.0).abs() < 1e-3 || samples.len() < 2 {
        return samples.to_vec();
    }
    let length = (samples.len() as f32 / factor) as usize;
    (0..length)
        .map(|i| {
            let position = i as f32 * factor;
            let index = position as usize;
            let next = (index + 1).min(samples.len() - 1);
            let weight = position - index as f32;
            samples[index] * (1.0 - weight) + samples[next] * weight
        })
        .collect()
}

impl Engine_Impl {
    fn speak(&self, mut parts: Vec<Part>, site: &ISpTTSEngineSite) -> Result<()> {
        let mut voice = self.voice.lock().map_err(|_| E_FAIL)?.clone().ok_or(E_FAIL)?;
        if let Some(speaker) = take_speaker(&mut parts, voice.model.speakers) {
            voice.speaker = speaker;
        }
        let base_rate = unsafe { site.GetRate() }.unwrap_or(0);
        let base_volume = unsafe { site.GetVolume() }.unwrap_or(100) as f32 / 100.0;
        for part in parts {
            if aborted(site) {
                return Ok(());
            }
            match part {
                Part::Silence(ms) => {
                    let count = (voice.model.sample_rate as u64 * ms as u64 / 1000) as usize;
                    if !write_samples(site, &vec![0i16; count]) {
                        return Ok(());
                    }
                }
                Part::Say(run) => {
                    if !self.say(&voice, &run, base_rate, base_volume, site) {
                        return Ok(());
                    }
                }
            }
        }
        Ok(())
    }

    /// One run, sentence by sentence. False when it was stopped or SAPI took no more.
    fn say(&self, voice: &Voice, run: &Run, base_rate: i32, base_volume: f32, site: &ISpTTSEngineSite) -> bool {
        // SAPI's rate is -10 to 10, a third to three times as fast; its pitch is taken as
        // semitones, a few at most, by which the speech is shifted.
        let rate = (base_rate + run.rate).clamp(-10, 10) as f32;
        let speed = 3f32.powf(rate / 10.0).clamp(0.4, 2.5);
        let semitones = run.pitch.clamp(-6, 6) as f32;
        let shift = 2f32.powf(semitones / 12.0);
        let gain = base_volume * (run.volume.min(100) as f32 / 100.0);

        let config = GenerationConfig { speed: speed / shift, sid: voice.speaker, ..Default::default() };
        let stopped = Arc::new(AtomicBool::new(false));
        let callback_site = site.clone();
        let callback_stopped = stopped.clone();
        let callback = move |chunk: &[f32], _progress: f32| -> bool {
            if aborted(&callback_site) {
                callback_stopped.store(true, Ordering::Relaxed);
                return false;
            }
            let pcm: Vec<i16> = resample(chunk, shift)
                .iter()
                .map(|s| (s * gain * 32767.0).clamp(-32768.0, 32767.0) as i16)
                .collect();
            if !write_samples(&callback_site, &pcm) {
                callback_stopped.store(true, Ordering::Relaxed);
                return false;
            }
            true
        };
        let started = Instant::now();
        let _turn = voice.model.busy.lock();
        let audio = voice.model.tts.generate_with_config(&run.text, &config, Some(callback));
        if audio.is_none() {
            log::write(&format!("{}: nothing made for {:?}", voice.name, run.text));
        } else if log::enabled() {
            log::write(&format!("{} (speaker {}): {:?} in {:?} (speed {speed:.2}, shift {semitones})",
                voice.name, voice.speaker, run.text, started.elapsed()));
        }
        !stopped.load(Ordering::Relaxed)
    }

    fn sample_rate(&self) -> u32 {
        self.voice.lock().ok().and_then(|v| v.as_ref().map(|v| v.model.sample_rate)).unwrap_or(DEFAULT_RATE)
    }
}

impl ISpTTSEngine_Impl for Engine_Impl {
    fn Speak(&self, flags: u32, format: *const GUID, wave: *const WAVEFORMATEX, fragments: *const SPVTEXTFRAG,
             site: Ref<ISpTTSEngineSite>) -> Result<()> {
        let rate = unsafe { wave.as_ref() }.map(|w| (w.nSamplesPerSec, w.wBitsPerSample, w.nChannels));
        if log::enabled() {
            log::write(&format!("speak: flags {flags:#x}, format {:?}, wave {rate:?} (from {})", unsafe { format.as_ref() },
                caller()));
        }
        let started = Instant::now();
        let site = site.ok()?.clone();
        if log::enabled() {
            // Each fragment as SAPI hands it over: what the program's text became on the way.
            let mut fragment = fragments;
            while let Some(frag) = unsafe { fragment.as_ref() } {
                let text = if frag.pTextStart.is_null() { String::new() } else {
                    String::from_utf16_lossy(unsafe { std::slice::from_raw_parts(frag.pTextStart.0, frag.ulTextLen as usize) })
                };
                log::write(&format!("  fragment: action {}, offset {}, {text:?}", frag.State.eAction.0, frag.ulTextSrcOffset));
                fragment = frag.pNext;
            }
        }
        let result = match catch_unwind(AssertUnwindSafe(|| {
            let parts = unsafe { parts(fragments) };
            self.speak(parts, &site)
        })) {
            Ok(result) => result,
            Err(_) => {
                log::write("panic while speaking");
                Err(E_FAIL.into())
            }
        };
        if log::enabled() {
            log::write(&format!("speak done in {:?}", started.elapsed()));
        }
        result
    }

    fn GetOutputFormat(&self, _target_format: *const GUID, _target_wave: *const WAVEFORMATEX,
                       output_format: *mut GUID, output_wave: *mut *mut WAVEFORMATEX) -> Result<()> {
        if output_format.is_null() || output_wave.is_null() {
            return Err(E_POINTER.into());
        }
        let rate = self.sample_rate();
        if log::enabled() {
            log::write(&format!("output format asked (target {:?}): giving PCM {rate} Hz 16-bit mono",
                unsafe { _target_format.as_ref() }));
        }
        unsafe {
            let wave = CoTaskMemAlloc(size_of::<WAVEFORMATEX>()) as *mut WAVEFORMATEX;
            if wave.is_null() {
                return Err(E_FAIL.into());
            }
            wave.write(WAVEFORMATEX {
                wFormatTag: WAVE_FORMAT_PCM as u16,
                nChannels: 1,
                nSamplesPerSec: rate,
                nAvgBytesPerSec: rate * 2,
                nBlockAlign: 2,
                wBitsPerSample: 16,
                cbSize: 0,
            });
            *output_format = SPDFID_WAVEFORMATEX;
            *output_wave = wave;
        }
        Ok(())
    }
}

#[implement(IClassFactory)]
struct Factory;

impl IClassFactory_Impl for Factory_Impl {
    fn CreateInstance(&self, outer: Ref<IUnknown>, iid: *const GUID, object: *mut *mut c_void) -> Result<()> {
        if object.is_null() {
            return Err(E_POINTER.into());
        }
        unsafe { *object = std::ptr::null_mut() };
        if outer.is_some() {
            return Err(CLASS_E_NOAGGREGATION.into());
        }
        let number = INSTANCES.fetch_add(1, Ordering::Relaxed) + 1;
        if log::enabled() {
            log::write(&format!("engine instance #{number} (from {})", caller()));
        }
        let engine: IUnknown = Engine { token: Mutex::new(None), voice: Mutex::new(None) }.into();
        unsafe { engine.query(iid, object).ok() }
    }

    fn LockServer(&self, _lock: BOOL) -> Result<()> {
        Ok(())
    }
}

/// Notes when and where the engine was loaded, nothing more: the loader lock is held here, so the
/// log line waits for the first call (`announce`).
#[unsafe(no_mangle)]
pub extern "system" fn DllMain(module: *mut c_void, reason: u32, _reserved: *mut c_void) -> BOOL {
    const DLL_PROCESS_ATTACH: u32 = 1;
    if reason == DLL_PROCESS_ATTACH {
        MODULE.store(module as usize, Ordering::Relaxed);
        log::start();
    }
    true.into()
}

/// COM's way in: the class factory for this engine's CLSID.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn DllGetClassObject(clsid: *const GUID, iid: *const GUID, object: *mut *mut c_void) -> HRESULT {
    if clsid.is_null() || iid.is_null() || object.is_null() {
        return E_POINTER;
    }
    announce();
    unsafe {
        *object = std::ptr::null_mut();
        if *clsid != CLSID_SPOKEN_VOICE {
            log::write(&format!("asked for an unknown class {:?}", *clsid));
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        let factory: IClassFactory = Factory.into();
        let result = factory.query(iid, object);
        let number = FACTORIES.fetch_add(1, Ordering::Relaxed) + 1;
        if log::enabled() {
            log::write(&format!("class factory #{number} requested (interface {:?}): {result:?} (from {})", *iid, caller()));
        }
        result
    }
}

/// Never unloaded once loaded: the models it holds are what makes the next line quick.
#[unsafe(no_mangle)]
pub extern "system" fn DllCanUnloadNow() -> HRESULT {
    S_FALSE
}
