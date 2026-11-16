# Spoken Players Voicepack on Linux

World of Warcraft on Linux runs through Wine or Proton, and so does the voicepack: it goes into the
same Wine prefix as WoW. `SpokenPlayersVoicepack-Wine.exe` sets that up, with SapiFix, which WoW's
text to speech needs there, and tells WoW to speak with these voices: under Wine it speaks with two
voices of its own instead ("en_US male" and "en_US female").

## Set it up

1. Install the [Spoken Players addon](https://www.curseforge.com/wow/addons/spoken-players-chat-tts),
   as on Windows.
2. Close World of Warcraft (and Battle.net).
3. Unzip `SpokenPlayersVoicepack-Linux.zip`, open a terminal in its folder, and run:

   ```
   bash install.sh
   ```

   It finds the Wine prefix WoW is installed in (Steam, also as a Flatpak or Snap, Lutris, Heroic,
   Faugus, umu-launcher, plain Wine) and runs the setup in it, with the Proton or Wine that prefix
   already uses. Nothing else needs installing: it works the same on Arch, Fedora, Ubuntu and the like.
4. It asks which WoW you play: retail, WoW Forever or both. Only that game's voices are downloaded,
   from Hugging Face (about 1.5 GB for WoW Forever, 3.5 GB for both), showing each as it comes.
5. It checks that the voices speak, silently, and says Done. Start WoW (restart it if it was open)
   and type `/sp`.

When the voicepack has new voices (the addon says so), run `bash install.sh` again: it offers the
game you chose last time, and downloads only the voices that changed.

| | |
|---|---|
| `bash install.sh --game forever` | no question asked (or `retail`, `all`) |
| `bash install.sh --wizard` | the voicepack's setup window instead, to pick races one by one |
| `bash install.sh --prefix DIR` | WoW's prefix, when it is not found or there are several (the folder with `drive_c` in it) |
| `bash install.sh --test` | checks again that the voices speak (silently) |
| `bash install.sh --uninstall` | takes it all out: the voices, their engine, SapiFix and WoW's setting |
| `bash install.sh --dry-run` | says what it would run, and runs nothing |

### Without the terminal

Run `SpokenPlayersVoicepack-Wine.exe` inside WoW's prefix the way your launcher runs other programs
there, with Battle.net closed:

| Launcher | How |
|---|---|
| Lutris | select Battle.net, then "Run EXE inside Wine prefix" in its Wine menu |
| Bottles | the bottle's "Run Executable..." |
| Heroic | the game's settings: "Run EXE on Prefix" |
| Steam (Battle.net as a non-Steam game) | `protontricks-launch --appid <its id> SpokenPlayersVoicepack-Wine.exe` |

## Needs

Proton 10 or newer, or Wine 10 or newer: tried with GE-Proton 11, Proton Experimental,
UMU-Proton 10 and Wine 11.20, and in game with WoW Forever on GE-Proton 11.

## When it says nothing

- Restart WoW after setting it up: WoW looks for voices when it starts.
- WoW lists only "en_US male" and "en_US female" (Options, Accessibility, Text to Speech): it is
  not told to speak with Windows' voices. Run `bash install.sh` again with WoW closed, or type
  `/console winePlatformTTS 1` in WoW and restart it.
- `bash install.sh --test`: says the voices speak, or what went wrong.
- What the last run printed: `~/.cache/spoken-players-voicepack/install.log` (Wine's output).
- The logs, in the prefix under `drive_c/users/<you or steamuser>/AppData/Local/`:
  `SapiFix/setup.log` (the setup), `SapiFix/sapifix.log` (what WoW asks of it; the text of each line too while an empty file
  named `verbose` is beside it) and
  `SpokenVoice/engine.log` (the voice engine).

## What it does

WoW's text to speech uses Windows' speech API (SAPI), except under Wine: there it speaks with two
voices of its own (Vivox's), unless its setting `winePlatformTTS` is on ("Use Windows TTS on Wine.
Disabled by default as it can cause crashes"). Wine has a SAPI, but not all that WoW uses:

| WoW does | Wine's SAPI |
|---|---|
| says each line as SAPI XML, `<bookmark mark='Start'/>...<bookmark mark='End'/>` around the text (and the addon's `<pitch>`, `<rate>`) | reads SSML only: the line is refused |
| writes the line to a WAV file (`ISpStream::BindToFile`) | not there |
| follows the speech by its events (`SetInterest`, `GetEvents`, notifications) | not there |
| (a WAV in another format than the voice's) | converts with a resampler Proton lacks: nothing is written |

`SpokenPlayersVoicepack-Wine.exe`, in the prefix:

1. runs the voicepack's setup (beside it, else downloaded from spokenplayers.sahnet.ee);
2. trusts the engine's certificate, which the setup does with `certutil`, a stub in Wine;
3. trusts SapiFix's certificate (it is signed as the engine is: WoW loads no SAPI engine whose
   signature it does not trust) and installs SapiFix (`sapifix.dll`, in `C:\Program Files\SapiFix`), the *TreatAs* class of SAPI's
   SpVoice and SpStream, so WoW gets it where it asks for them. Its voice passes the speaking on
   to Wine's, the SAPI XML turned into SSML that Wine reads into the same rate, pitch and volume,
   split at the bookmarks, and raises the events itself; its stream writes the WAV, converting the
   voice's format to the file's. Wine registers its SAPI again on every Proton update; that keeps
   a TreatAs, so SapiFix stays;
4. sets WoW up, in each of its game folders (`Wow.exe`, `WowT.exe`, `WowB.exe`: retail, its test
   realms and betas, WoW Forever): `winePlatformTTS` on in `WTF\Config.wtf`, and Wine's DXCore off
   for the game. The engine's ONNX Runtime looks for GPUs through DXCore as it loads a voice; Wine's
   DXCore opens an OpenGL context to do it, which fails in WoW's process, and Wine stops the game.
   The engine speaks on the CPU, and loads a voice quicker without it;
5. says a line the way WoW does, into a file, to check it works (silently: never played).

The voicepack itself, the engine and the voices, is the Windows one, unchanged.

## Building

On Windows, with Visual Studio's C++ build tools (the "Desktop development with C++" workload):

```
powershell -ExecutionPolicy Bypass -File wine\Build-Wine.ps1
```

builds `SpokenPlayersVoicepack-Linux.zip` (`SpokenPlayersVoicepack-Wine.exe`, with `sapifix.dll`
in it, `install.sh` and this file) into `out\`, its parts in `out\wine\`. It signs
`sapifix.dll` as the engine is signed, with a certificate made for the build, its private key
deleted straight after; no administrator, and this PC never trusts it.

| File | What it is |
|---|---|
| `sapifix.c`, `sapifix.def` | SapiFix: the voice, the stream, their registration, the log |
| `setup.c`, `setup.rc` | `SpokenPlayersVoicepack-Wine.exe` (`/quiet`, `/game=`, `/test`, `/uninstall`, `/novoicepack`) |
| `install.sh` | finds WoW's prefix and its Proton or Wine, and runs the .exe in it |
| `Build-Wine.ps1` | the build |
