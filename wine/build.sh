#!/usr/bin/env bash
# Builds what WoW players on Linux need into out/wine/: SpokenPlayersVoicepack-Wine.exe (it carries
# sapifix.dll, signed by wine/sign.py, which needs Python 3 and its cryptography package) and SpokenPlayersVoicepack-Linux.zip (the .exe, install.sh and README.md, to hand
# out), with Zig's C compiler for 64-bit Windows: zig from PATH, else Zig 0.14.1 downloaded once
# into ~/.cache/spoken-players-voicepack. Zig runs on Windows too: the same two commands build there.
set -euo pipefail

cd "$(dirname "$0")/.."
ZIG_VERSION=0.14.1
cache="$HOME/.cache/spoken-players-voicepack"

if ! python3 -c 'import cryptography' 2>/dev/null; then
  echo "Signing SapiFix needs Python 3 with its cryptography package (python-cryptography," >&2
  echo "python3-cryptography, or: pip install cryptography)." >&2
  exit 1
fi

zig=$(command -v zig || true)
if [[ -z $zig ]]; then
  zig="$cache/zig-x86_64-linux-$ZIG_VERSION/zig"
  if [[ ! -x $zig ]]; then
    mkdir -p "$cache"
    echo "Downloading Zig $ZIG_VERSION (the compiler, once) into $cache ..."
    curl -fsSL "https://ziglang.org/download/$ZIG_VERSION/zig-x86_64-linux-$ZIG_VERSION.tar.xz" | tar -xJ -C "$cache"
  fi
fi

out=out/wine
mkdir -p "$out"
cc=("$zig" cc -target x86_64-windows-gnu -O2 -Wall -Wno-unused-parameter)
"${cc[@]}" -shared wine/sapifix.c wine/sapifix.def \
  -lole32 -loleaut32 -luuid -lshlwapi -luser32 -ladvapi32 -o "$out/sapifix.dll"
# WoW loads no DLL whose signature it does not trust: signed with a certificate made for this build
# (its private key never saved), whose public part the setup carries and has the prefix trust.
python3 wine/sign.py "$out/sapifix.dll" "$out/sapifix.cer"
"${cc[@]}" -municode wine/setup.c wine/setup.rc \
  -lole32 -luuid -luser32 -ladvapi32 -lcrypt32 -lurlmon -lwininet -lwinmm -o "$out/SpokenPlayersVoicepack-Wine.exe"
rm -f "$out"/*.pdb "$out"/*.lib

# The zip: one folder with what a player needs.
stage="$out/zip/SpokenPlayersVoicepack-Linux"
rm -rf "$out/zip" "$out/SpokenPlayersVoicepack-Linux.zip"
mkdir -p "$stage"
cp "$out/SpokenPlayersVoicepack-Wine.exe" wine/install.sh wine/README.md "$stage/"
(cd "$out/zip" && zip -qr ../SpokenPlayersVoicepack-Linux.zip SpokenPlayersVoicepack-Linux)
rm -rf "$out/zip"
echo "Built $out/SpokenPlayersVoicepack-Wine.exe and $out/SpokenPlayersVoicepack-Linux.zip"
