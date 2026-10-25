#!/usr/bin/env bash
# What the installer's build needs of the packed voices, without the models: for each finished model
# folder in VOICES (a voices-multi folder), its voice.json and tokens.txt, and model.onnx.sha256 (the
# model's SHA-256 and size in bytes) instead of model.onnx. A few MB in all, where the models are GBs.
#
#   tools/voice-info.sh VOICES OUT
#
# OUT is engine/dist/voices-info: what describes the published models, for the installer's build to
# ask for a model it has no pack of. Written on Linux from the published packs each time models are
# published, and committed.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 VOICES OUT" >&2
    exit 2
fi
voices=$1
out=$2

count=0
for folder in "$voices"/*/; do
    model=$(basename "$folder")
    if [ ! -f "$folder/model.onnx" ] || [ ! -f "$folder/tokens.txt" ] || [ ! -f "$folder/voice.json" ]; then
        echo "leaving out $model: not finished packing" >&2
        continue
    fi
    mkdir -p "$out/$model"
    cp "$folder/voice.json" "$folder/tokens.txt" "$out/$model/"
    hash=$(sha256sum "$folder/model.onnx" | cut -d' ' -f1)
    size=$(stat -c %s "$folder/model.onnx")
    printf '%s %s\n' "$hash" "$size" > "$out/$model/model.onnx.sha256"
    count=$((count + 1))
done
echo "$count models' information in $out"
