# Spoken Players Voicepack: engine and installer

The Windows speech engine that lets World of Warcraft speak with the Spoken Players race voices, and
the installer that sets it up. This is everything needed to build both yourself, sign the engine with
your own certificate and install that instead of the published setup. How it works: [DOCS.md](DOCS.md).

## What you need

- Windows 10 or 11, 64-bit, and an administrator account (to sign and install)
- [Rust](https://rustup.rs) with the MSVC toolchain, and the Visual Studio Build Tools (C++)
- [Inno Setup 6](https://jrsoftware.org/isdl.php), for the installer

## 1. Build the engine

```
cd engine
cargo build --release
copy target\release\spoken_voice.dll dist\
```

The first build downloads sherpa-onnx's prebuilt static libraries (its release 1.13.8).

## 2. Sign it

WoW only loads signed voice engines. In an administrator PowerShell, from the repository's folder:

```
powershell -ExecutionPolicy Bypass -File engine\tools\Sign-Engine.ps1
```

It makes a certificate good for code signing only, trusts it on this PC, signs
`engine\dist\spoken_voice.dll`, then deletes the certificate's private key, so nothing else can ever be
signed with it. Its public part goes to `engine\dist\spoken_voice_engine.cer`, which the setup has
the PCs it installs on trust. `-Remove` takes the certificates out again. Sign again after every build.

## 3. Build the installer

Double-click `Build Installer.cmd`, or run `tools\Build-Installer.ps1`. The setup lands in `out\`.
It downloads the voice models from Hugging Face, https://huggingface.co/sahnetee/spoken-players, as
the published setup does (`engine\dist\voices-info` names them, with their SHA-256; the build checks
that Hugging Face has each, and the setup downloads them from the commit it saw). To install models of
your own, or changed ones, put each in its own folder in `engine\dist\voices-multi\` (`model.onnx`,
`tokens.txt`, `voice.json`) and build with `-Local`: the setup in `out\local\` installs those from the
`voices` folder beside it, and the rest from Hugging Face. Before installing, the setup lists the
full address of every file it will download, and shows each one's as it downloads.

## 4. Install

Close WoW and run `out\SpokenPlayersVoicepack-Setup.exe`. It asks which game you play (retail, WoW
Forever or both) and installs that game's voices, replacing any installed voicepack.

To try one voice without installing anything: `engine\tools\Test-Voice.ps1 -Voice orc_male`
(registers it for your user only and writes WAV files; `-Cleanup` removes it).

## License

GPL-3.0-or-later ([LICENSE](LICENSE)), as espeak-ng, which is built into the engine. The other
components and their licenses: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The voice models are
published separately, under their own terms: https://huggingface.co/sahnetee/spoken-players
