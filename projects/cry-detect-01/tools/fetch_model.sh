#!/usr/bin/env bash
# Download YAMNet INT8 TFLite for cry-detect-01.
#
# Fetches the STMicroelectronics repack (class-head retained) from HuggingFace
# and places it at spiffs/yamnet.tflite so `idf.py build` can bake it into the
# `yamnet` SPIFFS partition.
#
# Requires: curl

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/../spiffs/yamnet.tflite"
mkdir -p "$(dirname "$DEST")"

# Google AudioSet YAMNet (521 classes) mirrored as TFLite on HuggingFace.
# Note: this is the float16 TFLite from TF Hub — it uses AUDIO_SPECTROGRAM /
# MFCC ops that TFLite Micro does NOT support. It will not run as-is on the
# ESP32; see docs/internal/cry-detect-01-plan.md §15 for the risk and plan.
# We still fetch it so the SPIFFS image builds; swap to a TFLite-Micro
# compatible conversion before flashing production.
PRIMARY_URL="https://huggingface.co/thelou1s/yamnet/resolve/main/lite-model_yamnet_classification_tflite_1.tflite"
FALLBACK_URL="$PRIMARY_URL"

echo "Fetching YAMNet INT8 -> $DEST"
if curl -fL --retry 3 -o "$DEST" "$PRIMARY_URL"; then
    :
elif curl -fL --retry 3 -o "$DEST" "$FALLBACK_URL"; then
    :
else
    echo "ERROR: both YAMNet mirrors failed. Download manually from:" >&2
    echo "  https://huggingface.co/STMicroelectronics/yamnet" >&2
    echo "and save the 1024 INT8 variant (not the 256 embedding-only) to:" >&2
    echo "  $DEST" >&2
    exit 1
fi

size_bytes=$(wc -c < "$DEST" | tr -d ' ')
echo "Got $(( size_bytes / 1024 )) KB at $DEST"

if [ "$size_bytes" -lt 1000000 ]; then
    echo "WARN: file looks too small for YAMNet-1024 (expected ~3 MB)." >&2
    echo "      You may have the embedding-only variant; check the model card." >&2
fi

echo "Done. Next: idf.py -C . -B build build"
