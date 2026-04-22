#!/bin/bash
# cry-detect-01 night monitor
#
# Polls device /metrics at 60s cadence, writes per-poll log,
# and escalates anomalies to $ANOM so the Monitor tool can stream events.
#
# Anomalies emitted:
#   - NORESP_FIRST              /metrics silent (1 poll)
#   - OUTAGE_CONFIRMED          3 consecutive NORESP (~180 s)
#   - OUTAGE_SUSTAINED          30 consecutive NORESP (~30 min)
#   - OUTAGE_RECOVERED          /metrics back after ≥3 silent polls
#   - REBOOT                    uptime reset (prev_up > now)
#   - INFERENCE_STALLED         HTTP alive but inference_count not growing 2 polls
#   - NEW_WAV                   wav_count delta (capture fired)
#   - OVRGROW_BIG               audio_overrun_events burst > 100
#   - STATE=<x>                 state leaves idle/LISTEN
#   - CRY_SIGNAL                max_cry_conf_1s > 0.55 (works on current model
#                               ceiling 0.72 — raise threshold if/when model
#                               output range is fixed via re-PTQ)
#   - ALERT_FIRED               device-side alert counter grew
#
# Usage:
#   nohup bash tools/cry_monitor.sh > /tmp/cry-monitor.stdout 2>&1 &
#
# Env knobs:
#   CRY_HOST   device HTTP root        default http://192.168.1.100
#   LOG        per-poll log path       default /tmp/cry-monitor.log
#   ANOM       anomaly stream path     default /tmp/cry-monitor.anomalies
#   POLL       poll interval seconds   default 60
#   MAX_ITER   stop after N iter       default 960 (16 h at POLL=60)

set -u
HOST="${CRY_HOST:-http://192.168.1.100}"
LOG="${LOG:-/tmp/cry-monitor.log}"
ANOM="${ANOM:-/tmp/cry-monitor.anomalies}"
STATE="${STATE:-/tmp/cry-monitor.state}"
POLL="${POLL:-60}"
MAX_ITER="${MAX_ITER:-960}"

echo "# start $(date) host=$HOST poll=${POLL}s max_iter=$MAX_ITER" > "$LOG"
: > "$ANOM"

prev_uptime=0
prev_overrun_events=0
prev_alert_count=0
prev_infer_count=0
prev_wav_count=""
consec_noresp=0
consec_stall=0

anom() {
    local ts="$1"; shift
    echo "$ts $*" | tee -a "$ANOM" >> "$LOG"
}

for i in $(seq 1 "$MAX_ITER"); do
    ts=$(date +%H:%M:%S)
    r=$(curl -s -m 5 "$HOST/metrics" 2>/dev/null)

    # ---------- NORESP escalation ----------
    if [ -z "$r" ]; then
        consec_noresp=$((consec_noresp + 1))
        echo "$ts NORESP (consec=$consec_noresp)" >> "$LOG"
        if [ "$consec_noresp" -eq 1 ]; then
            anom "$ts" "NORESP_FIRST — device quiet, will confirm at consec=3"
        elif [ "$consec_noresp" -eq 3 ]; then
            anom "$ts" "OUTAGE_CONFIRMED — 3 consecutive NORESP (~180s). Device is down."
        elif [ "$consec_noresp" -eq 30 ]; then
            anom "$ts" "OUTAGE_SUSTAINED — 30 min silent"
        fi
        sleep "$POLL"
        continue
    fi
    if [ "$consec_noresp" -ge 3 ]; then
        anom "$ts" "OUTAGE_RECOVERED — /metrics responding after ${consec_noresp} silent polls"
    fi
    consec_noresp=0

    # ---------- parse key fields (single python call) ----------
    parsed=$(echo "$r" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
except Exception as e:
    print('PARSE_ERR', e); sys.exit(0)
print(d.get('uptime_s',0), d.get('state','?'),
      f\"{d.get('inference_fps',0):.2f}\",
      f\"{d.get('last_cry_conf',0):.3f}\",
      f\"{d.get('max_cry_conf_1s',0):.3f}\",
      f\"{d.get('input_rms',0):.0f}\",
      f\"{d.get('noise_floor_p95',0):.0f}\",
      d.get('alert_count',0),
      d.get('audio_overrun_events',0),
      d.get('audio_overrun_bytes',0),
      d.get('free_heap',0)//1024,
      d.get('inference_count',0),
      d.get('build_sha','?'))
" 2>/dev/null)

    if [ -z "$parsed" ] || [[ "$parsed" == PARSE_ERR* ]]; then
        anom "$ts" "METRICS_PARSE_ERR — $parsed"
        sleep "$POLL"; continue
    fi

    # shellcheck disable=SC2086
    set -- $parsed
    up=$1; state=$2; fps=$3; cc=$4; cm1s=$5; rms=$6; nf=$7
    alerts=$8; ovr_ev=$9; ovr_b=${10}; heap=${11}; infer_count=${12}; build=${13}

    line="up=$up state=$state fps=$fps cc=$cc cm1s=$cm1s rms=$rms fl95=$nf alerts=$alerts ovr_ev=$ovr_ev ovr_b=$ovr_b heap=${heap}KB ic=$infer_count"
    echo "$ts $line" >> "$LOG"

    # ---------- reboot ----------
    if [ "$prev_uptime" -ne 0 ] && [ "$up" -lt "$prev_uptime" ]; then
        anom "$ts" "REBOOT prev_up=$prev_uptime now=$up build=$build"
        prev_infer_count=0
        consec_stall=0
    fi

    # ---------- inference stall ----------
    if [ "$prev_infer_count" -ne 0 ] && [ "$infer_count" -le "$prev_infer_count" ]; then
        consec_stall=$((consec_stall + 1))
        if [ "$consec_stall" -eq 2 ]; then
            anom "$ts" "INFERENCE_STALLED — count stuck at $infer_count for 2 polls"
        fi
    else
        consec_stall=0
    fi

    # ---------- new WAV captured ----------
    # Fire only on increments; decrements mean ring-retention deleted an
    # old WAV (benign, not worth paging on).
    rs=$(curl -s -m 3 "$HOST/record/status" 2>/dev/null)
    if [ -n "$rs" ]; then
        wav_count=$(echo "$rs" | python3 -c "import sys,json; print(json.load(sys.stdin).get('wav_count','?'))" 2>/dev/null)
        if [ -n "$prev_wav_count" ] && [ "$wav_count" != "?" ] && [ "$prev_wav_count" != "?" ] \
           && [ "$wav_count" -gt "$prev_wav_count" ] 2>/dev/null; then
            anom "$ts" "NEW_WAV count $prev_wav_count → $wav_count (rms=$rms cm1s=$cm1s state=$state)"
        fi
        prev_wav_count=$wav_count
    fi

    # ---------- overrun burst ----------
    dev=$((ovr_ev - prev_overrun_events))
    if [ "$prev_overrun_events" -ne 0 ] && [ "$dev" -gt 100 ]; then
        anom "$ts" "OVRGROW_BIG +$dev events (now $ovr_ev)"
    fi

    # ---------- state leave idle ----------
    if [ "$state" != "idle" ] && [ "$state" != "LISTEN" ]; then
        anom "$ts" "STATE=$state $line"
    fi

    # ---------- cry signal ----------
    if python3 -c "import sys; sys.exit(0 if float('$cm1s') > 0.55 else 1)" 2>/dev/null; then
        anom "$ts" "CRY_SIGNAL cm1s=$cm1s $line"
    fi

    # ---------- alert fired ----------
    if [ "$alerts" -gt "$prev_alert_count" ]; then
        anom "$ts" "ALERT_FIRED alerts=$alerts (was $prev_alert_count) $line"
    fi

    echo "ts=$ts up=$up infer_count=$infer_count alerts=$alerts build=$build" > "$STATE"

    prev_uptime=$up
    prev_overrun_events=$ovr_ev
    prev_alert_count=$alerts
    prev_infer_count=$infer_count
    sleep "$POLL"
done
echo "# end $(date) (reached MAX_ITER)" >> "$LOG"
