//! A small log file, %LOCALAPPDATA%\SpokenVoice\engine.log: models loaded and anything that went
//! wrong, always; every line spoken too while the file `verbose` sits next to it. Each line has the
//! time to the millisecond, the process, the thread (and its name, where the program gave it one)
//! and the milliseconds since the engine was loaded. Every line also goes to OutputDebugString, so
//! a debug-output viewer (Sysinternals DebugView) sees it even where no file can be written.
//!
//! The folder comes from Windows (the user's known folder), not from the environment, which a
//! program can start another without. A program running at low integrity (sandboxed) may write
//! only to AppData\LocalLow, so that comes next, then %TEMP%\SpokenVoice, C:\Users\Public\SpokenVoice.
//! Over 8 MB, the log moves to engine.old.log and starts again.

use std::fs::{File, OpenOptions, create_dir_all, rename};
use std::io::Write;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::Instant;

use windows::Win32::Foundation::{HLOCAL, LocalFree};
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::System::Diagnostics::Debug::OutputDebugStringW;
use windows::Win32::System::SystemInformation::GetLocalTime;
use windows::Win32::System::Threading::{GetCurrentThread, GetCurrentThreadId, GetThreadDescription};
use windows::Win32::UI::Shell::{FOLDERID_LocalAppData, FOLDERID_LocalAppDataLow, KF_FLAG_DEFAULT, SHGetKnownFolderPath};
use windows::core::{GUID, HSTRING};

const MAX_BYTES: u64 = 8 * 1024 * 1024;

/// When the engine was loaded (DllMain), for the milliseconds since.
pub fn start() -> Instant {
    static START: OnceLock<Instant> = OnceLock::new();
    *START.get_or_init(Instant::now)
}

fn known_folder(id: &GUID) -> Option<PathBuf> {
    unsafe {
        let path = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, None).ok()?;
        let text = path.to_string().ok();
        CoTaskMemFree(Some(path.0 as *const _));
        text.map(PathBuf::from)
    }
}

fn open(folder: PathBuf) -> Option<(PathBuf, File)> {
    create_dir_all(&folder).ok()?;
    let path = folder.join("engine.log");
    if path.metadata().is_ok_and(|m| m.len() > MAX_BYTES) {
        let _ = rename(&path, folder.join("engine.old.log"));
    }
    let file = OpenOptions::new().create(true).append(true).open(&path).ok()?;
    Some((folder, file))
}

fn log() -> &'static Option<(PathBuf, Mutex<File>)> {
    static LOG: OnceLock<Option<(PathBuf, Mutex<File>)>> = OnceLock::new();
    LOG.get_or_init(|| {
        let candidates = [
            known_folder(&FOLDERID_LocalAppData),
            std::env::var_os("LOCALAPPDATA").map(PathBuf::from),
            known_folder(&FOLDERID_LocalAppDataLow),
            std::env::var_os("TEMP").map(PathBuf::from),
            Some(PathBuf::from(r"C:\Users\Public")),
        ];
        candidates.into_iter().flatten().find_map(|base| open(base.join("SpokenVoice")))
            .map(|(folder, file)| (folder, Mutex::new(file)))
    })
}

/// The folder the log went to, if any.
pub fn folder() -> Option<&'static PathBuf> {
    log().as_ref().map(|(folder, _)| folder)
}

/// Whether every spoken line is logged.
pub fn enabled() -> bool {
    log().as_ref().is_some_and(|(folder, _)| folder.join("verbose").exists())
}

/// The calling thread: its ID and, if it has one, its name.
pub fn thread() -> String {
    unsafe {
        let id = GetCurrentThreadId();
        let name = GetThreadDescription(GetCurrentThread()).ok().and_then(|text| {
            let name = text.to_string().ok();
            LocalFree(Some(HLOCAL(text.0 as *mut _)));
            name.filter(|n| !n.is_empty())
        });
        match name {
            Some(name) => format!("{id} {name}"),
            None => id.to_string(),
        }
    }
}

pub fn write(line: &str) {
    let time = unsafe { GetLocalTime() };
    let since = start().elapsed().as_millis();
    let text = format!("{}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}:{}] +{since}ms {line}",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
        std::process::id(), thread());
    unsafe { OutputDebugStringW(&HSTRING::from(format!("SpokenVoice: {text}\n"))) };
    if let Some((_, file)) = log() {
        if let Ok(mut file) = file.lock() {
            let _ = writeln!(file, "{text}");
        }
    }
}
