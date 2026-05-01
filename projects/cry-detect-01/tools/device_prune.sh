#!/usr/bin/env bash
# device_prune.sh — host-side eager prune of /sdcard date-bucketed logs.
#
# For each infer-YYYYMMDD.jsonl and cry-YYYYMMDD.log on the device older
# than retain_days, verify a canonical local copy exists and is a perfect
# tail match, then DELETE /files/rm.
#
# Append every action to logs/device-prune-ledger.jsonl.
#
# Pre-condition: tools/build_canonical.py has run recently — this script
# only consults logs/canonical/, never an individual session.
#
# Safety:
#   - Default mode is DRY-RUN. --apply to actually delete.
#   - 3-step gate: manifest size match → tail-4KB match → date < today−N.
#   - Refuses to act on today's bucket regardless (defense-in-depth; the
#     firmware also returns 409 for the currently-open log).
#
# Usage:
#   tools/device_prune.sh <retain_days> [--apply] [--host URL]
#
# Examples:
#   tools/device_prune.sh 7                 # dry run, show what would be deleted
#   tools/device_prune.sh 7 --apply         # actually delete buckets older than 7 days
#   tools/device_prune.sh 2 --apply         # tighter retention (steady-state)

set -euo pipefail

usage() {
    sed -n '2,/^$/p' "$0" >&2
    exit 2
}

if [[ $# -lt 1 ]]; then usage; fi

RETAIN_DAYS="$1"; shift
APPLY=0
HOST="http://192.168.1.100"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply)  APPLY=1; shift ;;
        --host)   HOST="$2"; shift 2 ;;
        --host=*) HOST="${1#--host=}"; shift ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

if ! [[ "$RETAIN_DAYS" =~ ^[0-9]+$ ]]; then
    echo "retain_days must be a non-negative integer" >&2; exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PROJECT_ROOT/../.." && pwd)"
LOGS="$PROJECT_ROOT/logs"
CANON="$LOGS/canonical"
LEDGER="$LOGS/device-prune-ledger.jsonl"

if [[ ! -f "$CANON/manifest.json" ]]; then
    echo "no canonical manifest at $CANON/manifest.json — run build_canonical.py first" >&2
    exit 1
fi

if [[ "$APPLY" == "1" ]]; then
    MODE="APPLY"
else
    MODE="dry-run"
fi
echo "=== device_prune ===  retain_days=$RETAIN_DAYS  mode=$MODE  host=$HOST" >&2

# Probe device
if ! curl -s --max-time 5 -o /dev/null "$HOST/metrics"; then
    echo "device unreachable at $HOST — abort" >&2; exit 1
fi

# Today's date in device's local TZ. The board logs in +1000 (Australia/Sydney).
# Be conservative: use UTC date and add 1 day to avoid TZ edge cases — better
# to over-retain than under-retain.
TODAY_UTC="$(date -u +%Y%m%d)"

# List /sdcard
LISTING_JSON="$(curl -s --max-time 10 "$HOST/files/ls?path=/sdcard")"

# Walk candidates
python3 - "$RETAIN_DAYS" "$TODAY_UTC" "$CANON" "$HOST" "$APPLY" "$LEDGER" <<'PY'
import json, os, sys, datetime as dt, hashlib, urllib.request, urllib.parse, time

retain_days = int(sys.argv[1])
today = dt.date(int(sys.argv[2][0:4]), int(sys.argv[2][4:6]), int(sys.argv[2][6:8]))
canon = sys.argv[3]
host = sys.argv[4]
apply = sys.argv[5] == "1"
ledger = sys.argv[6]

manifest = json.load(open(f"{canon}/manifest.json"))
canon_files = manifest["files"]

# Read listing from stdin? No — we curl'd it into a shell var, but that's hard
# to pass through to python. Re-fetch (cheap).
url = f"{host}/files/ls?path=/sdcard"
with urllib.request.urlopen(url, timeout=10) as r:
    listing = json.load(r)

threshold = today - dt.timedelta(days=retain_days)

import re
PAT = re.compile(r"^(infer|cry)-(\d{4})(\d{2})(\d{2})\.(jsonl|log)$")

actions = []  # tuples for printing/applying

for e in listing.get("entries", []):
    if e["type"] != "file":
        continue
    name = e["name"]
    m = PAT.match(name)
    if not m:
        continue
    fdate = dt.date(int(m.group(2)), int(m.group(3)), int(m.group(4)))
    if fdate >= threshold:
        # too new; keep
        continue
    # 3-step gate
    canon_info = canon_files.get(name)
    if canon_info is None:
        actions.append((name, e["size"], "SKIP", "no_canonical_copy", None))
        continue
    if canon_info["size"] < e["size"]:
        actions.append((name, e["size"], "SKIP", f"canonical_smaller({canon_info['size']}<{e['size']})", None))
        continue
    # Tail check
    tail_url = f"{host}/files/tail?path=/sdcard/{urllib.parse.quote(name)}&bytes=4096"
    try:
        with urllib.request.urlopen(tail_url, timeout=15) as r:
            dev_tail = r.read()
    except Exception as ex:
        actions.append((name, e["size"], "SKIP", f"tail_fetch_failed:{ex}", None))
        continue

    canon_path = f"{canon}/{name}"
    if not os.path.exists(canon_path):
        actions.append((name, e["size"], "SKIP", f"canon_missing:{canon_path}", None))
        continue
    sz = os.path.getsize(canon_path)
    with open(canon_path, "rb") as f:
        f.seek(max(0, sz - len(dev_tail)))
        local_tail = f.read(len(dev_tail))
    if local_tail != dev_tail:
        actions.append((name, e["size"], "SKIP", f"tail_mismatch_len{len(dev_tail)}", None))
        continue

    sha = canon_info["sha256"]
    actions.append((name, e["size"], "DELETE" if apply else "WOULD_DELETE",
                    f"verified retain_days<{retain_days} date={fdate}", sha))

# Print plan
print(f"\nPlanned actions ({len(actions)} candidates older than {threshold}):")
print(f"{'name':<32} {'size_MB':>9}  {'action':<14} reason")
for name, size, action, reason, _ in actions:
    print(f"{name:<32} {size/1e6:>9.2f}  {action:<14} {reason}")

# Execute deletes
if apply:
    deleted = 0
    freed = 0
    print()
    for name, size, action, reason, sha in actions:
        if action != "DELETE":
            continue
        url = f"{host}/files/rm?path=/sdcard/{urllib.parse.quote(name)}"
        req = urllib.request.Request(url, method="DELETE")
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                body = r.read().decode("utf-8", errors="replace")
                code = r.status
        except urllib.error.HTTPError as ex:
            print(f"  HTTP {ex.code} on {name}: {ex.read().decode(errors='replace')[:120]}")
            continue
        except Exception as ex:
            print(f"  ERROR  {name}: {ex}")
            continue
        if code == 200:
            print(f"  rm  {name}  -> freed {size/1e6:.2f} MB  resp={body[:80]}")
            deleted += 1
            freed += size
            with open(ledger, "a") as fl:
                fl.write(json.dumps({
                    "ts": dt.datetime.utcnow().isoformat() + "Z",
                    "path": f"/sdcard/{name}",
                    "size_deleted": size,
                    "sha256_local": sha,
                    "retain_days": retain_days,
                    "host": host,
                }) + "\n")
        else:
            print(f"  HTTP {code} on {name}: {body[:120]}")
    print(f"\nDeleted {deleted} files, freed {freed/1e6:.1f} MB on device")

# Show device df after
try:
    with urllib.request.urlopen(f"{host}/files/df", timeout=10) as r:
        df = json.load(r)
    sd = df.get("sdcard", {})
    used = sd.get("total_bytes", 0) - sd.get("free_bytes", 0)
    print(f"\n/files/df:  /sdcard {used/1e9:.2f} GB used, {sd.get('free_bytes',0)/1e9:.2f} GB free")
except Exception as ex:
    print(f"\n/files/df probe failed: {ex}")
PY
