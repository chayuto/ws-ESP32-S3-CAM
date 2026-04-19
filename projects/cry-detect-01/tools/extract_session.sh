#!/usr/bin/env bash
# End-of-session mirror + audit for cry-detect-01.
#
# Pulls everything from the deployed device (WAVs + triggers.jsonl from
# /sdcard/events, plus the /sdcard root logs), then runs the full audit
# pipeline on the local mirror.
#
# Idempotent — skips already-downloaded files based on presence. Safe to
# rerun.
#
# Usage:
#   tools/extract_session.sh <session_dir> [host]
#
# Example:
#   tools/extract_session.sh logs/night-20260419
#   tools/extract_session.sh logs/night-20260419 http://192.168.1.100
#
# Layout produced:
#   <session_dir>/
#   ├── wavs/*.wav
#   ├── triggers.jsonl
#   ├── device-logs/{infer-*.jsonl,cry-*.log,infer-boot.jsonl,CRY-0000.LOG}
#   └── (analysis artifacts from audit_pipeline.sh)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <session_dir> [host]" >&2
    exit 2
fi

SESSION_DIR="$1"
HOST="${2:-http://192.168.1.100}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -d "$SESSION_DIR" ]]; then
    echo "creating session dir: $SESSION_DIR" >&2
    mkdir -p "$SESSION_DIR"
fi

SESSION_DIR="$(cd "$SESSION_DIR" && pwd)"
WAV_DIR="$SESSION_DIR/wavs"
DEV_LOGS="$SESSION_DIR/device-logs"
mkdir -p "$WAV_DIR" "$DEV_LOGS"

echo "=== extract_session ===" >&2
echo "    host    = $HOST" >&2
echo "    session = $SESSION_DIR" >&2

# ---------------------------------------------------------------
# 1. Kill any running monitor (best-effort)
# ---------------------------------------------------------------
if [[ -f "$SESSION_DIR/monitor.sh" ]]; then
    pids=$(pgrep -f "$SESSION_DIR/monitor.sh" 2>/dev/null || true)
    if [[ -n "$pids" ]]; then
        echo "killing running monitor: $pids" >&2
        kill $pids 2>/dev/null || true
        sleep 1
    fi
fi

# ---------------------------------------------------------------
# 2. Probe device
# ---------------------------------------------------------------
if ! curl -s --max-time 5 -o /dev/null "$HOST/metrics"; then
    echo "device unreachable at $HOST — abort (this may be OK if the board was already powered off; re-run with the files you already have)" >&2
    echo "proceeding to audit step with whatever is on disk..." >&2
else
    # ---------------------------------------------------------------
    # 3. /sdcard/events/ → wavs/ + triggers.jsonl
    # ---------------------------------------------------------------
    echo "--- [1/3] mirroring /sdcard/events ---" >&2
    events_json=$(curl -s --max-time 10 "$HOST/files/ls?path=/sdcard/events" || echo '{"entries":[]}')

    # Extract filenames with Python (jq may not be present).
    files=$(echo "$events_json" | python3 -c "
import json, sys
d = json.load(sys.stdin)
for e in d.get('entries', []):
    print(e['name'])
" 2>/dev/null || true)

    for f in $files; do
        target=""
        if [[ "$f" == *.wav ]]; then
            target="$WAV_DIR/$f"
        elif [[ "$f" == "triggers.jsonl" ]]; then
            target="$SESSION_DIR/$f"
        else
            continue  # skip unknown entries
        fi
        if [[ -f "$target" ]]; then
            # Assume existing file is complete (we don't checksum here).
            continue
        fi
        sz=$(curl -sL --max-time 120 -o "$target" -w "%{size_download}" \
             "$HOST/files/get?path=/sdcard/events/$f") || { echo "FAIL $f"; continue; }
        echo "  got $f -> $sz bytes" >&2
    done

    # ---------------------------------------------------------------
    # 4. /sdcard/ root → device-logs/
    # ---------------------------------------------------------------
    echo "--- [2/3] mirroring /sdcard/ root logs ---" >&2
    root_json=$(curl -s --max-time 10 "$HOST/files/ls?path=/sdcard" || echo '{"entries":[]}')
    root_files=$(echo "$root_json" | python3 -c "
import json, sys
d = json.load(sys.stdin)
for e in d.get('entries', []):
    if e['type'] != 'file':
        continue
    n = e['name']
    if n.startswith('.'): continue
    # Log-like files only; skip anything unrecognized to avoid pulling
    # surprises (e.g. random macOS metadata if card was mounted on host).
    if (n.endswith('.jsonl') or n.endswith('.log') or n.endswith('.LOG')):
        print(n)
" 2>/dev/null || true)

    for f in $root_files; do
        target="$DEV_LOGS/$f"
        if [[ -f "$target" ]]; then
            # For day-bucketed files (infer-YYYYMMDD.jsonl, cry-YYYYMMDD.log)
            # we may need to refresh because the device is still appending.
            # Heuristic: re-download if the server's stat time is after the
            # local mtime. Cheaper: always re-download — these are <100 MB.
            rm "$target"
        fi
        sz=$(curl -sL --max-time 600 -o "$target" -w "%{size_download}" \
             "$HOST/files/get?path=/sdcard/$f") || { echo "FAIL $f"; continue; }
        echo "  got $f -> $sz bytes" >&2
    done
fi

# ---------------------------------------------------------------
# 5. Run audit pipeline
# ---------------------------------------------------------------
echo "--- [3/3] audit_pipeline ---" >&2
"$SCRIPT_DIR/audit_pipeline.sh" "$SESSION_DIR"

# ---------------------------------------------------------------
# 6. Prompt for README if missing
# ---------------------------------------------------------------
if [[ ! -f "$SESSION_DIR/README.md" ]]; then
    cat > "$SESSION_DIR/README.md" <<'EOF'
# <night-YYYYMMDD>

TODO: fill in after reviewing yamnet_files.csv + segments.csv.

## Dataset split
TODO

## Incident notes
TODO

See `docs/research/file-management-strategy-20260419.md` for the README
template contract.
EOF
    echo "wrote README.md stub — please fill in before archival" >&2
fi

echo "=== done ===" >&2
du -sh "$SESSION_DIR" 2>/dev/null >&2 || true
