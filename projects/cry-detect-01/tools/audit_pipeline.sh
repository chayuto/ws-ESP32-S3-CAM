#!/usr/bin/env bash
# Raw-WAV audit pipeline for cry-detect-01.
#
# Runs three analysis stages against a directory of captured event WAVs
# and writes all artifacts alongside the source dir:
#
#   1. audit_wavs.py      → manifest.csv / .jsonl        (file-level numeric)
#                         → segments.csv / .jsonl        (0.96 s / 0.48 s)
#   2. render_specgrams   → specgrams/*.png              (visual audition)
#   3. score_yamnet       → yamnet_files.csv             (FP32 oracle, file)
#                         → yamnet_segments.csv          (FP32 oracle, seg)
#
# No per-file listening required. Segment IDs align with YAMNet frames, so
# heuristic + oracle rows join 1:1 on (file, seg_idx).
#
# Usage:
#   tools/audit_pipeline.sh <wav_dir> [venv_python]
#
# Example:
#   tools/audit_pipeline.sh logs/night-20260418
#
# If venv_python is omitted, defaults to <repo>/.venv-analysis/bin/python.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <dir-containing-wavs/-subdir> [venv_python]" >&2
    exit 2
fi

BASE_DIR="$(cd "$1" && pwd)"
WAV_DIR="$BASE_DIR/wavs"
TRIGGERS="$BASE_DIR/triggers.jsonl"
SPECGRAMS="$BASE_DIR/specgrams"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PROJECT_ROOT/../.." && pwd)"

PY="${2:-$REPO_ROOT/.venv-analysis/bin/python}"

if [[ ! -x "$PY" ]]; then
    echo "python interpreter not found: $PY" >&2
    echo "expected venv at $REPO_ROOT/.venv-analysis — create with:" >&2
    echo "  python3.13 -m venv .venv-analysis" >&2
    echo "  .venv-analysis/bin/pip install numpy scipy matplotlib tensorflow tensorflow-hub 'setuptools<81'" >&2
    exit 1
fi

if [[ ! -d "$WAV_DIR" ]]; then
    echo "no wavs dir: $WAV_DIR" >&2
    echo "expected layout: $BASE_DIR/wavs/*.wav + $BASE_DIR/triggers.jsonl (optional)" >&2
    exit 1
fi

echo "=== audit_pipeline: $BASE_DIR ===" >&2
echo "    python = $PY" >&2
echo "    wavs   = $WAV_DIR" >&2
echo "    spec   = $SPECGRAMS" >&2

echo "--- [1/3] numeric features ---" >&2
"$PY" "$SCRIPT_DIR/audit_wavs.py" \
    --wav-dir "$WAV_DIR" \
    ${TRIGGERS:+--triggers "$TRIGGERS"} \
    --out-csv "$BASE_DIR/manifest.csv" \
    --out-jsonl "$BASE_DIR/manifest.jsonl" \
    --out-segments-csv "$BASE_DIR/segments.csv" \
    --out-segments-jsonl "$BASE_DIR/segments.jsonl"

echo "--- [2/3] spectrograms ---" >&2
"$PY" "$SCRIPT_DIR/render_specgrams.py" \
    --wav-dir "$WAV_DIR" \
    --out-dir "$SPECGRAMS"

echo "--- [3/3] YAMNet oracle ---" >&2
"$PY" "$SCRIPT_DIR/score_yamnet.py" \
    --wav-dir "$WAV_DIR" \
    --class-map "$PROJECT_ROOT/hf/yamnet_class_map.csv" \
    --out-segments-csv "$BASE_DIR/yamnet_segments.csv" \
    --out-files-csv "$BASE_DIR/yamnet_files.csv"

echo "=== done ===" >&2
ls -1 "$BASE_DIR"/manifest.* "$BASE_DIR"/segments.* "$BASE_DIR"/yamnet_*.csv 2>/dev/null >&2 || true
echo "specgrams:  $(ls "$SPECGRAMS" 2>/dev/null | wc -l | tr -d ' ') files" >&2
