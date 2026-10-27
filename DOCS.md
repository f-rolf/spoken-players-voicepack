# How it works

The Spoken Players addon speaks chat through WoW's own text to speech (`C_VoiceChat.SpeakText`),
which uses Windows' speech API, SAPI 5. Every SAPI voice names an engine; this repository's engine
speaks with small neural voice models, one per race and body type, each with several speakers
(character voices). To WoW and to Windows they are ordinary installed voices.

## Layout

| Path | What it is |
|---|---|
| `engine/src/lib.rs` | the engine: the COM class, the SAPI interfaces, the model cache, speaking |
| `engine/src/log.rs` | its log |
| `engine/tools/Sign-Engine.ps1` | signs the built DLL with a certificate made for it (README) |
| `engine/tools/Test-Voice.ps1` | tries a voice without installing anything |
| `engine/tools/VoiceNames.ps1` | the voices' and speakers' names, from the models' `voice.json` |
| `engine/dist/espeak-ng-data/` | espeak-ng's data, which the engine reads at run time |
| `engine/dist/voices-info/` | what the build needs of each published model: `voice.json`, `tokens.txt`, `model.onnx.sha256` |
| `installer/` | the Inno Setup script (`SpokenPlayersVoicepack.iss`), its name (`common.iss`) and pictures |
| `tools/Build-Installer.ps1` | builds the setup into `out/`, the models downloaded from Hugging Face |
| `tools/voice-info.sh` | writes `voices-info` from a folder of model packs |

## The engine

`spoken_voice.dll` is one self-contained 64-bit DLL: Rust, with sherpa-onnx, ONNX Runtime and espeak-ng
linked in statically, and the C runtime too, so it needs nothing beyond Windows.

### As a SAPI engine

- **A COM class**, CLSID `{055BF9E8-68B3-4CB2-8090-50895F4BCC68}`, registered as an in-process server
  (`HKLM\SOFTWARE\Classes\CLSID\{…}\InprocServer32`, threading model `Both`). The DLL exports
  `DllGetClassObject` and `DllCanUnloadNow` (always "no": the models it holds are what makes the next
  line quick). Windows loads it into a program when that program speaks with one of its voices,
  not when it lists them.
- **The interfaces SAPI asks of an engine:** `ISpObjectWithToken` (SAPI hands over the voice's token;
  the engine reads `ModelDir`, `DataDir` and `SpeakerId` from it and loads that model) and
  `ISpTTSEngine` (`GetOutputFormat`: 16-bit mono PCM at the model's rate, 22,050 Hz; `Speak`: SAPI's
  text fragments, already parsed out of any XML).
- **Speaking.** The fragments are joined into runs that share their rate, pitch and volume, and each
  run is synthesised a sentence at a time, every sentence written to SAPI as soon as it is made, so
  speech starts at once. A line takes about a tenth of its length to make, on two CPU threads. SAPI's
  rate is the speed (a third to three times as fast), its pitch a shift of a few semitones
  (resampling, which is how two characters of one model can still sound different), its volume the
  gain. `SPVES_ABORT` (a program stopping speech) is checked between sentences and in every audio chunk.
- **The speaker code.** A line that starts with `{sp:N}` is said by the model's speaker N instead of
  the token's `SpeakerId`; the code is taken out, and a speaker the model lacks is ignored. That is
  how the addon picks a character's voice: see "Why one voice per model".

### The voices (registry)

The installer registers one SAPI voice token per model under
`HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\SpokenPlayers_<model>`: its name ("Spoken Players Orc
Male"), `CLSID` (the engine), `ModelDir`, `DataDir`, `SpeakerId` (0), the attributes SAPI and WoW read
(`Name`, `Gender`, `Age`, `Language` 409, `Vendor`), and `SpokenGroupVoice` = 1, by which the setup
recognises its voices to remove them. SAPI lists machine-wide voices only, not a single user's.

### Models

A model is a folder: `model.onnx` (a Piper VITS model, about 74 MB, with the metadata sherpa-onnx
reads), `tokens.txt` (its phoneme table) and `voice.json` (its espeak-ng voice, such as `en-us` or
`en-029`, its speakers in ID order with their names and labels, and under `inference` how the model
is sampled: `noise_scale`, `noise_w`, `length_scale`, read when the model loads; 0.6, 0.65 and 1 when
missing). espeak-ng turns the text into phonemes with that voice, the model turns the phonemes into
audio.

### Memory

A loaded model takes about 110 MB of RAM (all 48 would be about 5.2 GB, and 90 s of loading), and
loads in 1.3 to 2 s. Each model is loaded once per process, the first time it is asked for, under a
lock of its own, so one loading never holds up a line of another. At most as many stay loaded as fit
in a tenth of the PC's RAM (8 GB: 7, 16 GB: 14, 32 GB: 29); past that, the one asked for longest ago
is unloaded, never one loading or speaking. `MaxModels` (DWORD) under `HKLM\SOFTWARE\Spoken Players`
sets the number instead (the environment variable `SPOKEN_VOICE_MAX_MODELS` too, for trying it out).

### Log

`%LOCALAPPDATA%\SpokenVoice\engine.log` (from Windows' known folder; failing that `AppData\LocalLow`,
`%TEMP%` or `C:\Users\Public`): which program loaded the engine, the models it loaded or unloaded, how
long that took, the memory then, and anything that went wrong, each line with the time to the
millisecond, the process and the thread. With a file named `verbose` beside it, every call too, with
its duration, thread and caller (as module+offset), and every text fragment SAPI handed over. Every
line also goes to `OutputDebugString`. Over 8 MB, it moves to `engine.old.log`.

## Signing

WoW only loads SAPI engines whose DLL has a trusted code signature; unsigned, the voices are listed but
stay silent. (Windows itself does not ask for one.) `Sign-Engine.ps1` makes a self-signed certificate
for code signing only, trusts it on the PC (Trusted Root and Trusted Publishers), signs the DLL, and
deletes the private key, so the trusted certificate can never sign anything else. The setup installs
that certificate's public part and trusts it on the PCs it installs on, and takes it out again on
uninstall. A build is signed by whoever built it: the published setup by its publisher, yours by you.

## The installer

- First it asks which game the player plays: retail (every race but WoW Forever's Skyborne), WoW
  Forever (its nine races) or both, each with the raid voices. Nothing is chosen for them; run again,
  last time's choice is. The races page (one component per race, by faction, plus leaders and
  murlocs) shows only when asked for, with the game's races ticked.
- The setup holds the engine, espeak-ng's data, each model's small files and the licenses; the models
  themselves are downloaded from Hugging Face (`resolve/<commit>/<model>/model.onnx`, the repository's
  commit when the setup was built, checked against their SHA-256), or taken from a `voices` folder
  beside the setup (`<model>-<sha256 prefix>.onnx`). The game page links the Hugging Face repository,
  the Ready page lists the full address of every file to download, and the installing page shows each
  one's as it downloads.
- Run again, it updates: a model already installed with the same SHA-256 is kept, so only changed
  models are downloaded. Each installed model keeps its hash beside it (`model.onnx.sha256`), so this
  check costs nothing; one installed by an older setup is hashed once, on a progress page.
- Before installing, it waits for WoW to let go of the engine: it tries to open `spoken_voice.dll`
  for writing, which Windows refuses while any program has it loaded.
- Every install first removes all of its voices (`SpokenGroupVoice` = 1) and the models of races left
  out, then registers the chosen ones, so nothing lingers. It remembers the game chosen and the races
  it offered (`HKLM\SOFTWARE\Spoken Players`: Game, Adjusted, Races), so a newer setup chooses the
  same and adds the game's races added since.
- Uninstalling removes the engine, the voices, the models and the certificate.

## How WoW uses it

Observed with Windows' own tools (event tracing, the engine's log, WoW's log files), not by touching
the game:

- WoW loads the engine on the first line spoken with one of its voices, on its `VoiceSpeakThread`, and
  creates a new engine instance (and hands it the voice's token) for every line. Thanks to the model
  cache, only a model's first line waits for it to load.
- The text arrives as the addon gave it, between two bookmarks WoW adds
  (`<bookmark mark="Start"/>…<bookmark mark="End"/>`).
- **Why one voice per model.** At every login and every `/reload`, WoW walks the full list of
  installed SAPI voices about four times for each installed voice. The time grows with the square of
  the number of voices: about 5 s with 61 voices, 14 s with 189, 33 s with 317, in which the game does
  not respond and, past about 29 s, voice chat fails to connect. It happens with any voices, also
  Microsoft's own. So the voicepack installs 48 voices, not one per speaker (300 and more), and the
  addon picks a speaker with the speaker code.
