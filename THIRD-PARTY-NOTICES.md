# Third-party notices

The Spoken Players voice engine (`spoken_voice.dll`) and its installer are free software under the
GNU General Public License, version 3 or (at your option) any later version: [LICENSE](LICENSE).
They have to be: the engine has espeak-ng, which is GPL-3.0, built into it. Their complete source code:
https://github.com/f-rolf/spoken-players-voicepack

The engine is one DLL with the following built into it, each under its own license. The full texts are
in [licenses/](licenses) (and, installed, in `C:\Program Files\Spoken Players\licenses`).

## Built into the engine

| Component | License | Source |
|---|---|---|
| **espeak-ng** (text to phonemes, with **ucd-tools**), by Jonathan Duddington, Reece H. Dunn and the espeak-ng contributors | GPL-3.0-or-later ([LICENSE](LICENSE)) | https://github.com/espeak-ng/espeak-ng |
| **espeak-ng's data** (`engine/dist/espeak-ng-data`, installed beside the engine), as packaged by the sherpa-onnx project | GPL-3.0-or-later ([LICENSE](LICENSE)) | https://github.com/espeak-ng/espeak-ng, https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models |
| **sherpa-onnx** 1.13.8 (speech synthesis around the models), by Xiaomi Corporation and the k2-fsa contributors, with its Rust bindings (`sherpa-onnx`, `sherpa-onnx-sys`) | Apache-2.0 ([licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)) | https://github.com/k2-fsa/sherpa-onnx |
| **ONNX Runtime** (runs the models), by Microsoft Corporation | MIT ([licenses/MIT-onnxruntime.txt](licenses/MIT-onnxruntime.txt)) | https://github.com/microsoft/onnxruntime |
| **piper-phonemize** (Piper's phonemes from espeak-ng), by Michael Hansen | MIT ([licenses/MIT-piper-phonemize.txt](licenses/MIT-piper-phonemize.txt)) | https://github.com/rhasspy/piper-phonemize |
| **kaldi-native-fbank**, **kaldi-decoder**, **kaldifst** (parts of sherpa-onnx's build), by Xiaomi Corporation and contributors | Apache-2.0 ([licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)) | https://github.com/csukuangfj/kaldi-native-fbank, https://github.com/k2-fsa/kaldi-decoder, https://github.com/k2-fsa/kaldifst |
| **OpenFst** (in sherpa-onnx's build), by Google and contributors | Apache-2.0 ([licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)) | https://www.openfst.org |
| **simple-sentencepiece** (in sherpa-onnx's build), by the k2-fsa contributors | Apache-2.0 ([licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)) | https://github.com/pkufool/simple-sentencepiece |
| **KISS FFT** (in sherpa-onnx's build), by Mark Borgerding | BSD-3-Clause ([licenses/BSD-3-Clause-kissfft.txt](licenses/BSD-3-Clause-kissfft.txt)) | https://github.com/mborgerding/kissfft |
| **windows-rs** (`windows`, `windows-core` and their companion crates: the Windows and COM bindings), by Microsoft Corporation | MIT or Apache-2.0, used under MIT ([licenses/MIT-windows-rs.txt](licenses/MIT-windows-rs.txt)) | https://github.com/microsoft/windows-rs |
| `serde`, `serde_core`, `serde_json`, `itoa`, `memchr`, `zmij` (Rust crates used by the bindings) | MIT or Apache-2.0 (memchr: Unlicense or MIT), used under MIT ([licenses/MIT-rust-crates.txt](licenses/MIT-rust-crates.txt)) | https://crates.io |
| The Rust standard library | MIT or Apache-2.0 | https://github.com/rust-lang/rust |

The C runtime is Microsoft's, linked statically as Visual Studio's license allows. The sherpa-onnx
libraries are the project's own prebuilt static release (`sherpa-onnx-v1.13.8-win-x64-static-MT-Release-lib`),
which the `sherpa-onnx-sys` crate downloads at the first build; the components above are what that
release is made of.

## Not built into the engine

- **The voice models** are separate files the engine loads (`model.onnx`, `tokens.txt`, `voice.json`
  per race), published on their own, under their own terms. They are Piper (VITS) models.
- **The installer** is compiled by Inno Setup (https://jrsoftware.org/isinfo.php), whose license lets
  setups made with it be distributed freely.
- **The Spoken Players addon** is a separate program (on CurseForge): it talks to WoW, not to the engine.
