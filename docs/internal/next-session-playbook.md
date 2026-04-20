# Next-session playbook

*Written 2026-04-17 with device deployed; expected return in a few hours. Follow this top-to-bottom when bench access is back.*

## Before anything — harvest data (don't reset)

The goal: capture what actually happened during the offline window. Do **not** reflash or power-cycle before extracting.

### Step 0.1 — monitor log snapshot
```bash
cp ./logs/monitor-$(date +%Y%m%d).log \
   ./logs/monitor-session1-snapshot.log
wc -l ./logs/monitor-session1-snapshot.log
```
Expect ~1 row per minute. Scan for `REBOOT`, `NEW-ALERT`, `WIFI-DROP`, `UNREACHABLE` / `RECOVERED` markers.

### Step 0.2 — live device state (if still up)
```bash
curl -s http://cry-detect-01.local/metrics | python3 -m json.tool > /tmp/final-metrics.json
cat /tmp/final-metrics.json
```
Confirm: `uptime_s`, `alert_count`, `inference_count`, heap/psram (leak check), `noise_floor_p95`.

### Step 0.3 — pull SD log while device runs
The device may have `SD_MOUNT_POINT` at `/sdcard`. We don't have a file-download endpoint yet, so physical card removal is the path:
1. Power off device (unplug).
2. Remove SD card.
3. Insert into Mac → copy `/Volumes/<sd>/cry-*.log` and `/Volumes/<sd>/events/` to `./logs/sd-session1/`.
4. **Don't put the card back yet** — analyse first.

### Step 0.4 — cross-reference monitor log vs SD log
The monitor row's `up=Xs` should match a row in the SD file at uptime_s=X. Sanity-check one ping-able event (e.g. a REBOOT, or `wifi_up`):
```bash
grep "^up=" ./logs/sd-session1/cry-*.log | head -20
grep REBOOT ./logs/monitor-session1-snapshot.log
```

## Things already staged for flash (built, not flashed)

Held in the current tree and known to compile. When you flash next, these go in automatically:

| Change | File(s) | Purpose |
|---|---|---|
| **v2 SD log schema** | `main/sd_logger.c` | Row now has 15 columns: wallclock, uptime_s, event, cry_conf, max_cry_conf_1s, rms, nf_p95, nf_warm, latency_ms, inference_count, inference_fps, free_heap, free_psram, rssi, state. Matches monitor schema one-to-one. |
| `RING_LINE_BYTES` 120 → 192 | `main/sd_logger.c` | To hold the longer row. |
| **LED brightness + night mode** | `main/led_alert.{c,h}`, `main/web_ui.c`, `www/*` | Software-PWM dim (0–100 %) over the state machine; persists in NVS (`led/bright`). New endpoint `GET /led/brightness[?pct=N]`; dashboard gets a slider + Night/Off/Full presets. User feedback from real-world deployment: default 100 % is too bright at night. After flash, set **Night (5 %)** from the web UI or `curl http://cry-detect-01.local/led/brightness?pct=5`. Survives reboot. |

Commits covering these: `c071d2b` (schema + plan), plus the next commit (brightness).

## Flash sequence (when you return to bench)

```bash
# 1. Device is on the bench + USB-C plugged. If it's still running from
#    before, check it responds on serial before forcing BOOT mode:
stty -f /dev/cu.usbmodem3101 -hupcl 2>/dev/null
dd if=/dev/cu.usbmodem3101 of=/tmp/pre-flash.log bs=1 count=10000 2>/dev/null &
DD=$!; sleep 5; kill $DD
head -5 /tmp/pre-flash.log

# 2. If serial gave boot banner, a regular flash probably works:
. ~/esp/esp-idf/export.sh
idf.py -C projects/cry-detect-01 -B /tmp/ws-cry-detect-01-build \
       -p /dev/cu.usbmodem3101 flash

# 3. If flash fails ("Failed to connect"), force bootloader mode:
#    Unplug → hold BOOT → plug USB → release BOOT, then retry step 2.

# 4. Physical unplug + replug after flash → clean boot (NOT a DTR/RTS
#    toggle; the macOS trap will bite every time).
```

## Prioritised to-do for next working session

Ordered by value-per-time. Stop after what you have time for.

### High — Stage 2.0 hardware validation (~4 h total)

1. **Flash the v2 schema change** (15 min including monitor tee restart).
2. **Listen test via web UI** (5 min): tap 🔊 Listen, tap near MIC1 hole vs MIC2 hole, note RMS response asymmetry. Answers "which mic is actually wired to slot 0".
3. **Bench record via new `/test/record` endpoint** (~1 h to code): `POST /test/record` triggers the event_recorder, record 30-60 s of bench audio, download via `/recordings/<name>.wav`, open in Audacity. Confirm frequency response + no DC offset + no clipping at 36 dB PGA.
4. **Enable 2-ch capture** (~2 h to code + test): `fs.channel = 2` in `audio_capture.c`, downmix in capture task. Repeat tap-test — both mics should register. If one doesn't, we have a hardware issue to chase.

### Medium — Stage 2.1 real-audio INT8 calibration (~half day)

Only after §High above. Prereq: 100+ real 16 kHz mono WAV clips from the actual deployment room + ESC-50 cry clips.

```bash
source /tmp/tfvenv/bin/activate
cd projects/cry-detect-01
TF_USE_LEGACY_KERAS=1 python tools/convert_yamnet.py \
    --audio-dir /path/to/collected_wavs \
    --calib-count 500 \
    --dest spiffs/yamnet.tflite
idf.py build flash   # flash only spiffs partition if model changed
```

Expected: `cry_conf` quiet-room p99 drops from ~0.62 to < 0.2. Then lower `base_threshold` from 0.85 to ~0.5 in `main.c`.

If improvement is material, re-upload to HF (token already in `/tmp/hf.env` if set; see §HF refresh below).

### Medium — /log/tail bug fix (~30 min)

`web_ui.c` `handler_log_tail` has an 8 KB char buffer on stack, occasionally corrupts. Move to `heap_caps_malloc(8192, MALLOC_CAP_SPIRAM)` + free at end. Patch is trivial, I'll apply it when the next fix batch lands.

### Medium — Stage 2.4 Telegram push (~half day)

Bedroom monitor is useless without off-device notifications. Spec in `docs/internal/stage2-plan.md` §2.4. Needs user to get a bot token from @BotFather. Can code-complete without the token, test with a dummy.

### Low — Stage 2.2 AFE (ESP-SR) (~2-3 days)

Only after calibration shows it's the actual lever. Adds beamforming + noise suppression but large effort. Don't start before we have evidence it's needed.

## HF model refresh (only if calibration improves materially)

Repo already at `chayuto/yamnet-mel-int8-tflm`. To push an updated model:

```bash
source /tmp/tfvenv/bin/activate
# Replace the model + update model card with date + calibration detail
cp projects/cry-detect-01/spiffs/yamnet.tflite projects/cry-detect-01/hf/yamnet.tflite
# Edit hf/README.md calibration section
HF_TOKEN=<user's token, NOT committed> python -c "
from huggingface_hub import upload_folder
upload_folder(repo_id='chayuto/yamnet-mel-int8-tflm', repo_type='model',
              folder_path='projects/cry-detect-01/hf',
              commit_message='Recalibrated with N real-audio patches')
"
```

## Known-failure cookbook (so we don't rediscover)

| Symptom | Cause | Fix |
|---|---|---|
| Serial returns 0 bytes after flash | pyserial DTR-on-open forced chip to download mode | Use `stty -hupcl` + `dd` read; physical unplug+replug to recover |
| `Failed to connect` on flash | Device crash-rebooting faster than esptool reset window | Force BOOT mode: unplug → hold BOOT → plug USB → release BOOT → retry |
| `SpiffsFullError` at build | SD SPIFFS partition too small for model | Grow `yamnet` partition in `partitions.csv` |
| `assert failed: xQueueSemaphoreTake queue.c:1709` | NULL mutex — module used before init | NULL-guard all public accessors; see §4.4 of lab notebook |
| Crash loop at exactly 30 s intervals | Stack overflow in housekeeping task | Bump stack ≥6 KB for any task that writes SD |
| UI blank | Browser cache / mDNS not resolving on caller's subnet | Hard-refresh; try IP directly |

## Research angles parked for observation

After the SD log lands, answer:

1. What's the `cry_conf` distribution over hours of real ambient? Histogram from SD rows. Confirms whether 0.85 threshold is tight or slack.
2. How often did Wi-Fi drop? Count `WIFI-DROP` / `UNREACHABLE` in monitor log vs `wifi_up`/`wifi_down` in SD log.
3. Did noise-floor p95 move with the time of day? Should track HVAC/ambient patterns.
4. Any reboot markers? Device should survive overnight clean.

## Ground rules that survived Stage 1

(Carry-forward from `docs/research/stage1-bringup-labnotebook.md` §7.)

- Web UI is the primary diagnostic channel; serial is supplementary.
- Every task that writes to SD gets ≥ 6 KB stack.
- NULL-guard every module accessor.
- Physical power cycle is the reliable recovery, not DTR/RTS toggles.
- Synthetic calibration is a debug stop-gap, not a ship state.
