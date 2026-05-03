#!/usr/bin/env bash
# Fetch model artifacts for cry-detect-01 into spiffs/.
#
# Pulls:
#   yamnet.tflite   — YAMNet teacher (existing, ~4 MB INT8 with 521-class head)
#   student.tflite  — distilled INT8 student from chayuto/yamnet-cry-distill-int8
#                     (only when CONFIG_CRY_STUDENT_ENABLED=y in firmware build,
#                      but downloaded unconditionally — leaving the file present
#                      is harmless when the build doesn't reference it.)
#
# Both land in spiffs/ alongside each other.  spiffs_create_partition_image
# in CMakeLists bakes the directory into the yamnet partition (5 MB) at build
# time.
#
# Requires: curl. No huggingface-cli needed (uses HF resolve URLs).
#
# Usage:
#   ./fetch_model.sh                       # fetch both, default tags
#   ./fetch_model.sh --student-tag v0.2.0  # pin student version
#   ./fetch_model.sh --skip-yamnet         # only refresh the student
#   ./fetch_model.sh --skip-student        # only refresh the teacher

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SPIFFS="$HERE/../spiffs"
mkdir -p "$SPIFFS"

YAMNET_DEST="$SPIFFS/yamnet.tflite"
STUDENT_DEST="$SPIFFS/student.tflite"

YAMNET_URL="https://huggingface.co/thelou1s/yamnet/resolve/main/lite-model_yamnet_classification_tflite_1.tflite"
STUDENT_TAG="v0.2.0"
STUDENT_REPO="chayuto/yamnet-cry-distill-int8"

DO_YAMNET=1
DO_STUDENT=1

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-yamnet)  DO_YAMNET=0 ; shift ;;
        --skip-student) DO_STUDENT=0 ; shift ;;
        --student-tag)  STUDENT_TAG="$2" ; shift 2 ;;
        -h|--help)
            sed -n '1,/^set -euo/p' "$0" | head -25
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2 ; exit 2 ;;
    esac
done

fetch_or_die() {
    local url="$1"
    local dest="$2"
    local label="$3"
    echo "Fetching $label -> $dest"
    echo "  $url"
    if ! curl -fL --retry 3 -o "$dest" "$url"; then
        echo "ERROR: $label fetch failed" >&2
        return 1
    fi
    local size_bytes
    size_bytes=$(wc -c < "$dest" | tr -d ' ')
    echo "  Got $(( size_bytes / 1024 )) KB"
}

if [ "$DO_YAMNET" = 1 ]; then
    fetch_or_die "$YAMNET_URL" "$YAMNET_DEST" "YAMNet teacher"
    size_bytes=$(wc -c < "$YAMNET_DEST" | tr -d ' ')
    if [ "$size_bytes" -lt 1000000 ]; then
        echo "WARN: yamnet.tflite looks too small (expected ~3-4 MB)." >&2
    fi
fi

if [ "$DO_STUDENT" = 1 ]; then
    # HF resolve URL pattern: /resolve/<rev>/<file>. v0.2.0 is the published
    # tag; main also works if a newer rev has been pushed without a tag bump.
    STUDENT_URL="https://huggingface.co/${STUDENT_REPO}/resolve/${STUDENT_TAG}/model.tflite"
    fetch_or_die "$STUDENT_URL" "$STUDENT_DEST" "Distilled student (${STUDENT_TAG})"
    size_bytes=$(wc -c < "$STUDENT_DEST" | tr -d ' ')
    if [ "$size_bytes" -lt 50000 ] || [ "$size_bytes" -gt 500000 ]; then
        echo "WARN: student.tflite is ${size_bytes} bytes, expected ~110 KB." >&2
    fi

    # Also fetch the model card + eval JSON for local reference. Optional —
    # firmware doesn't read these, but they document what was flashed.
    curl -fL --retry 3 -o "$SPIFFS/student.config.json" \
        "https://huggingface.co/${STUDENT_REPO}/resolve/${STUDENT_TAG}/config.json" 2>/dev/null \
        && echo "  + student.config.json (recommended threshold + class indices)" \
        || echo "  (skipped student.config.json)"
fi

echo
echo "Contents of $SPIFFS:"
ls -la "$SPIFFS"
echo
echo "Next: idf.py build  (the spiffs/ contents are baked into the yamnet partition)"
