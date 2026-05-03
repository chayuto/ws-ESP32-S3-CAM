#!/usr/bin/env bash
# Recursively delete all files under the device's /sdcard via the existing
# /files/* HTTP API. No firmware change required.
#
# Use BEFORE flashing Phase B to start with a clean card. The device must
# be running its current firmware and reachable over the network.
#
# IMPORTANT: this is a destructive operation — it deletes WAVs, JSONL logs,
# session markers, the ledger, everything writable under /sdcard. Always
# run with --dry-run first to preview the deletion list.
#
# Usage:
#   tools/wipe_sd.sh --dry-run                       # preview, no deletes
#   tools/wipe_sd.sh --dry-run http://192.168.1.50   # custom host
#   tools/wipe_sd.sh                                 # interactive confirm
#   tools/wipe_sd.sh --yes                           # non-interactive
#
# Exit codes:
#   0  ok
#   1  user cancelled / connection failed
#   2  invalid arguments

set -euo pipefail

DRY_RUN=0
ASSUME_YES=0
HOST="http://192.168.1.100"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --yes|-y)  ASSUME_YES=1; shift ;;
        -h|--help)
            sed -n '1,30p' "$0" | sed 's/^# //; s/^#//'
            exit 0 ;;
        http://*|https://*)
            HOST="$1"; shift ;;
        *)
            echo "unknown arg: $1" >&2
            echo "usage: $0 [--dry-run] [--yes] [host_url]" >&2
            exit 2 ;;
    esac
done

echo "=== wipe_sd ==="
echo "    host    = $HOST"
echo "    dry-run = $DRY_RUN"

if ! curl -s --max-time 5 -o /dev/null "$HOST/metrics"; then
    echo "device unreachable at $HOST" >&2
    exit 1
fi

# ---------------------------------------------------------------
# 1. Enumerate files: /sdcard root files + /sdcard/events/*
# ---------------------------------------------------------------

list_dir() {
    local path="$1"
    curl -s --max-time 10 "$HOST/files/ls?path=$path" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for e in d.get('entries', []):
    if e.get('type') != 'file':
        continue
    n = e['name']
    if n.startswith('.'):
        # Hidden files (e.g. .session-started, .sync-ledger.jsonl) ARE wipe targets
        pass
    print(f\"{e['size']}\t{e['name']}\")
" 2>/dev/null || true
}

echo "--- enumerating /sdcard ---"
ROOT_FILES=$(list_dir "/sdcard")
echo "--- enumerating /sdcard/events ---"
EVENT_FILES=$(list_dir "/sdcard/events")

ROOT_COUNT=$(printf '%s\n' "$ROOT_FILES" | grep -c . || true)
EVENT_COUNT=$(printf '%s\n' "$EVENT_FILES" | grep -c . || true)
TOTAL_BYTES=$(
    {
        printf '%s\n' "$ROOT_FILES"
        printf '%s\n' "$EVENT_FILES"
    } | awk -F'\t' 'NF==2 {s+=$1} END {print s+0}'
)

echo
echo "    root files:   $ROOT_COUNT"
echo "    event files:  $EVENT_COUNT"
echo "    total bytes:  $TOTAL_BYTES"
echo

if [[ "$ROOT_COUNT" -eq 0 && "$EVENT_COUNT" -eq 0 ]]; then
    echo "nothing to delete; SD already empty under /sdcard"
    exit 0
fi

# ---------------------------------------------------------------
# 2. Preview / confirm
# ---------------------------------------------------------------

print_plan() {
    if [[ -n "$ROOT_FILES" ]]; then
        echo "--- /sdcard/ ---"
        printf '%s\n' "$ROOT_FILES" | awk -F'\t' 'NF==2 {printf "  rm /sdcard/%s  (%s bytes)\n", $2, $1}'
    fi
    if [[ -n "$EVENT_FILES" ]]; then
        echo "--- /sdcard/events/ ---"
        printf '%s\n' "$EVENT_FILES" | awk -F'\t' 'NF==2 {printf "  rm /sdcard/events/%s  (%s bytes)\n", $2, $1}'
    fi
}

print_plan

if [[ "$DRY_RUN" -eq 1 ]]; then
    echo
    echo "(dry-run) no files deleted"
    exit 0
fi

if [[ "$ASSUME_YES" -ne 1 ]]; then
    echo
    read -r -p "delete the $((ROOT_COUNT + EVENT_COUNT)) files listed above? [type 'wipe' to confirm] " ans
    if [[ "$ans" != "wipe" ]]; then
        echo "cancelled"
        exit 1
    fi
fi

# ---------------------------------------------------------------
# 3. Delete (one HTTP DELETE per file)
# ---------------------------------------------------------------

rm_one() {
    local path="$1"
    local rc
    # /files/rm accepts both DELETE and (per file_api.h) GET; using DELETE is
    # the documented contract.
    rc=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE \
         --max-time 30 "$HOST/files/rm?path=$path")
    case "$rc" in
        200) echo "  ok  $path" ;;
        409) echo "  SKIP $path (currently open)" ;;
        404) echo "  miss $path" ;;
        *)   echo "  FAIL $path (http $rc)"; return 1 ;;
    esac
    return 0
}

failed=0
echo "--- deleting ---"
if [[ -n "$EVENT_FILES" ]]; then
    while IFS=$'\t' read -r _ name; do
        [[ -z "$name" ]] && continue
        rm_one "/sdcard/events/$name" || failed=$((failed + 1))
    done <<< "$EVENT_FILES"
fi
if [[ -n "$ROOT_FILES" ]]; then
    while IFS=$'\t' read -r _ name; do
        [[ -z "$name" ]] && continue
        rm_one "/sdcard/$name" || failed=$((failed + 1))
    done <<< "$ROOT_FILES"
fi

echo
df_after=$(curl -s --max-time 5 "$HOST/files/df" || echo "")
echo "post-wipe /files/df: $df_after"
echo
if [[ "$failed" -gt 0 ]]; then
    echo "completed with $failed failures (see above)"
    exit 1
fi
echo "wipe complete. Ready for Phase B flash."
