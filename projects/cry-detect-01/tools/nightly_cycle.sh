#!/usr/bin/env bash
# nightly_cycle.sh — one-shot extract → canonical → prune for cry-detect-01.
#
# Wraps three steps that previously had to be remembered separately. Designed
# for **manual** invocation (the user explicitly does not want this scheduled).
#
#   1. extract_session.sh logs/night-YYYYMMDD       (today's date by default)
#   2. build_canonical.py                           (refresh logs/canonical/)
#   3. device_prune.sh <retain_days> --apply        (delete old buckets on device)
#
# Bails on the first failure so you don't accidentally prune against a stale
# canonical or extract against a sick device. All output tee'd to a timestamped
# log under logs/nightly-cycle-logs/.
#
# Usage:
#   tools/nightly_cycle.sh                       # retain_days=2, today's date
#   tools/nightly_cycle.sh --retain-days 7
#   tools/nightly_cycle.sh --session-name night-20260502
#   tools/nightly_cycle.sh --dry-run             # extract+canonical, prune dry-run only
#   tools/nightly_cycle.sh --host http://192.168.1.100
#
# Exit codes:
#   0  success (all three stages green)
#   1  device unreachable / extract failed
#   2  canonical build failed
#   3  prune failed
#   4  bad arguments

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOGS_ROOT="$PROJECT_ROOT/logs"

RETAIN_DAYS=2
SESSION_NAME=""
HOST="http://192.168.1.100"
DRY_RUN=0

usage() { sed -n '2,/^$/p' "$0" >&2; exit 4; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --retain-days) RETAIN_DAYS="$2"; shift 2 ;;
        --session-name) SESSION_NAME="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

if [[ -z "$SESSION_NAME" ]]; then
    SESSION_NAME="night-$(date +%Y%m%d)"
fi

SESSION_DIR="$LOGS_ROOT/$SESSION_NAME"
RUN_TS="$(date +%Y%m%dT%H%M%S)"
RUN_LOG_DIR="$LOGS_ROOT/nightly-cycle-logs"
RUN_LOG="$RUN_LOG_DIR/$RUN_TS-$SESSION_NAME.log"
mkdir -p "$RUN_LOG_DIR"

# Tee everything into the run log.
exec > >(tee -a "$RUN_LOG") 2>&1

banner() {
    printf '\n=== %s ===\n' "$1"
}

banner "nightly_cycle"
echo "  session       = $SESSION_DIR"
echo "  host          = $HOST"
echo "  retain_days   = $RETAIN_DAYS"
echo "  dry_run       = $DRY_RUN"
echo "  log           = $RUN_LOG"

# 1. extract
banner "[1/3] extract_session"
if ! "$SCRIPT_DIR/extract_session.sh" "$SESSION_DIR" "$HOST"; then
    echo "extract_session failed" >&2
    exit 1
fi

# 2. canonical refresh
banner "[2/3] build_canonical"
if ! "$SCRIPT_DIR/build_canonical.py" --logs-dir "$LOGS_ROOT"; then
    echo "build_canonical failed" >&2
    exit 2
fi

# 3. prune (dry-run or apply)
banner "[3/3] device_prune"
if [[ "$DRY_RUN" == "1" ]]; then
    echo "(dry-run mode — not applying)"
    if ! "$SCRIPT_DIR/device_prune.sh" "$RETAIN_DAYS" --host "$HOST"; then
        echo "device_prune dry-run failed" >&2
        exit 3
    fi
else
    if ! "$SCRIPT_DIR/device_prune.sh" "$RETAIN_DAYS" --apply --host "$HOST"; then
        echo "device_prune failed" >&2
        exit 3
    fi
fi

banner "done"
echo "  log: $RUN_LOG"
