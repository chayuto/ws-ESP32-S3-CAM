# cry-detect-01

Pretrained baby-cry detector on the Waveshare ESP32-S3-CAM-GC2145, with
host-side data-labelling tooling for an end-to-end no-human-in-the-loop
training pipeline.

**On-device:** ES7210 mic → I²S DMA (16 kHz mono) → log-mel patch
(96×64) → **YAMNet-1024 INT8** (TFLite Micro) → threshold on class-20
(*Baby cry, infant cry*) → red LED + SD log + auto-recorded WAV +
web UI over Wi-Fi. No training, no fine-tuning, no vision, no AFE.

**Host-side:** captured WAVs flow into a 4-oracle auto-ensemble
(YAMNet wide-class FP32 + sklearn `feat_clf` + sklearn `embed_clf` on
2048-d meanmax embeddings + sub-type cluster + temporal context) that
produces per-capture confidence tiers. No human in the label loop —
the `low` tier is the research bucket where oracles disagree.

Method documented in
[`docs/research/host-side-auto-ensemble-method.md`](../../docs/research/host-side-auto-ensemble-method.md).
Decisions behind the on-device choices in
[`docs/research/cry-detect-starter-plan.md`](../../docs/research/cry-detect-starter-plan.md).
ML work follows the `/ml-researcher` discipline (pre-register, ablate,
gitignore the lab notebook, commit only durable conclusions) — see
[`.claude/commands/ml-researcher.md`](../../.claude/commands/ml-researcher.md).

**Privacy:** captures, derived labels, trained classifier weights, and
session-narrative analyses are all gitignored — see "Publish boundary"
in the repo-root `CLAUDE.md`.

## Prerequisites

- ESP-IDF ≥ 5.3 (tested on 5.5.3) — activate with `. ~/esp/esp-idf/export.sh`.
- The board flashed. Port is typically `/dev/cu.usbmodem3101` on macOS.

## One-time setup

```zsh
cd projects/cry-detect-01

# 1. Fetch YAMNet-1024 INT8 into spiffs/yamnet.tflite
./tools/fetch_model.sh

# 2. Edit Wi-Fi creds in sdkconfig.defaults (or use menuconfig)
$EDITOR sdkconfig.defaults
```

## Build & flash

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C . -B build set-target esp32s3
idf.py -C . -B build build
idf.py -C . -B build -p /dev/cu.usbmodem3101 flash
```

First build pulls managed components (BSP, esp-tflite-micro, esp-dsp, button) from the ESP Component Registry. Vendor `dependencies.lock` workaround from the repo-root `CLAUDE.md` does not apply here — we generate our own lock at first build.

## Monitor

`idf.py monitor` does not work in non-TTY shells. Use the pyserial snippet under `/monitor` (see `.claude/commands/monitor.md`).

## Runtime UI

Once the board joins Wi-Fi, the serial log prints its IP. Open `http://<ip>/` in a browser:

- Big status card: Idle / Listening / **CRYING** — pushed via SSE.
- Live metrics: inference ms, FPS, heap / PSRAM free, RSSI, NTP-synced, uptime.
- Rolling 60 s chart of class-20 confidence.
- Last 20 detections with ISO-8601 timestamps.

Offline fallback: red LED on CH32V003 P6 (active-LOW).

| LED | Meaning |
|---|---|
| Solid ON, then OFF after boot | Booting |
| OFF | Idle |
| 1 Hz blink | Wi-Fi connecting |
| 4 Hz blink | NTP syncing |
| Solid ON for `CRY_DETECT_HOLD_MS` | Cry detected |
| 0.5 Hz blink | Model load failed (fatal) |

## Logging

- **SD card**: if present and `CONFIG_CRY_DETECT_SD_ENABLED=y`, rotating CSV log at `/sdcard/cry-YYYYMMDD.log`.
- **Fallback**: internal `logs_fat` partition mounted at `/logs` if SD absent.
- **Ring buffer**: last ~80 lines in RAM, served by `GET /log/tail`.
- **Format (v3)**: `wallclock,uptime_s,event,cry_conf,max_cry_conf_1s,rms,nf_p95,nf_warm,latency_ms,inference_count,inference_fps,free_heap,free_psram,rssi,state,<20× watched_conf>`.
- **Timestamp**: RFC 3339 local time with numeric offset — e.g. `2026-04-18T08:13:53.726+10:00`. Timezone is Sydney AEST/AEDT (`AEST-10AEDT,M10.1.0,M4.1.0/3`) set at boot; change via `setenv("TZ", ...)` in `network.c`.

Pre-NTP lines are stamped with uptime-seconds and flagged `NOT_SYNCED`; they become queryable by wall-clock once NTP lands (historical lines are not rewritten).

## Remote file access (Stage 2.7)

Enabled by default. Whitelisted roots: `/sdcard`, `/logs`, `/yamnet`. Path traversal (`..`) is rejected.

| Method | Endpoint | Example |
|---|---|---|
| `GET` | `/files/df` | `curl http://cry-detect-01.local/files/df` |
| `GET` | `/files/ls?path=<dir>` | `curl 'http://.../files/ls?path=/sdcard'` |
| `GET` | `/files/stat?path=<file>` | size, mtime, is_dir |
| `GET` | `/files/get?path=<file>` | chunked streaming download |
| `GET` | `/files/head?path=<file>&bytes=N` | first N bytes (max 1 MiB) |
| `GET` | `/files/tail?path=<file>&bytes=N` | last N bytes (max 1 MiB) |
| `DELETE` | `/files/rm?path=<file>` | refuses the currently-open log file |

Pull the day's log without removing the SD card:
```zsh
TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p logs/cry-detect-01-export-${TS}
curl -s "http://cry-detect-01.local/files/get?path=/sdcard/cry-$(date +%Y%m%d).log" \
  -o logs/cry-detect-01-export-${TS}/cry-today.log
```

## Tuning knobs

All in `idf.py -C . -B build menuconfig` → "Cry-detect-01":

| Symbol | Default | Tune if... |
|---|---|---|
| `CRY_DETECT_THRESHOLD` | 40 | Raw INT8 threshold (range -128..127). 40 ≈ 0.31 after dequantisation. Lower for sensitivity; raise to suppress TV/pet false alarms |
| `CRY_DETECT_CONSEC_FRAMES` | 6 | Raise for more stability; lower for faster response. With ~1.4 inferences/s, 6 ≈ 4.3 s of sustained evidence |
| `CRY_DETECT_MIC_GAIN_DB` | 36 | Check `/metrics.input_rms`; raise if flat, lower if clipping |
| `CRY_DETECT_HOLD_MS` | 5000 | Alert latch duration |

## Partition layout

```
nvs         24 KB
factory      5 MB   app + runtime
yamnet       4 MB   SPIFFS, yamnet.tflite (~3 MB)
logs_fat     1 MB   FAT, SD-absent log fallback
```

## Module layout

| File | Role |
|---|---|
| `main/main.c` | Boot sequence, task setup, BSP init |
| `main/audio_capture.{c,h}` | I²S DMA from ES7210, ring buffer |
| `main/mel_features.{c,h}` | Hanning + 512-pt FFT → 96×64 log-mel patch (magnitude spectrum) |
| `main/yamnet.{cc,h}` | TFLite Micro inference, INT8 quantization |
| `main/detector.{c,h}` | Threshold + consecutive-frame hysteresis |
| `main/event_recorder.{c,h}` | Pre-roll + post-roll WAV recording on detect |
| `main/auto_trigger.{c,h}` | RMS-vs-noise-floor fallback trigger for data collection |
| `main/noise_floor.{c,h}` | Adaptive ambient-noise tracking |
| `main/sd_logger.{c,h}` | Rotating CSV log on SD card |
| `main/metrics.{c,h}`, `metrics_logger.{c,h}` | `/metrics` JSON + 1 Hz JSONL inference log |
| `main/file_api.{c,h}` | Stage 2.7 remote file endpoints |
| `main/web_ui.{c,h}` | SSE + dashboard |
| `main/network.{c,h}` | Wi-Fi STA + SNTP + timezone |
| `main/log_retention.{c,h}` | Day-bucketed log GC |
| `main/breadcrumb.{c,h}` | NVS state markers across reboots |
| `main/led_alert.{c,h}` | CH32V003 P6 red LED, software PWM dim |

## Host-side tooling

Under `tools/` — used to turn captured WAVs into auto-labelled training
material. Method documented in
[`docs/research/host-side-auto-ensemble-method.md`](../../docs/research/host-side-auto-ensemble-method.md).

| Tool | Purpose |
|---|---|
| `ensemble_audit.py` | Run all oracles over the capture pool, write `master.csv` with per-capture scores + tier (gitignored output) |
| `freeze_release.py <id>` | Snapshot the current `master.csv` into a versioned release JSON with frozen splits |
| `build_inventory.py` | Auto-generate `INVENTORY.md` (date range, tier counts, by-day breakdown) |
| `extract_session.sh` | Pull a session's WAVs + JSONL infer log + triggers via the `/files` API |
| `audit_pipeline.sh` | Numeric features + YAMNet FP32 oracle on a WAV directory |
| `score_yamnet.py` | YAMNet FP32 inference on raw WAVs (host-side oracle) |
| `cry_monitor.sh` | Long-running heartbeat monitor with anomaly streaming |

The INT8 PTQ harness (`repTQ_yamnet.py`, documented negative result)
has migrated to the sibling training repo `yamnet-cry-distill-int8`
along with the `/ml-researcher` skill. See
[`docs/research/repo-boundary-yamnet-cry-distill.md`](../../docs/research/repo-boundary-yamnet-cry-distill.md)
for the full repo split.

For any local ML/sklearn work in this repo (e.g. fitting the auto-
ensemble's `feat_clf` and `embed_clf` LogReg oracles), the
`/ml-researcher` discipline still applies — pre-register, stamp model
versions, gitignore the lab notebook, commit only durable conclusions.
The canonical skill lives in the training repo. Experiments live
under `ml-experiments/<YYYY-MM-DD-topic>/` (gitignored).

## Troubleshooting

- **"Model load failed"** (LED 0.5 Hz blink): `spiffs/yamnet.tflite` not present or is the 256-d embedding-only variant. Re-run `./tools/fetch_model.sh`.
- **Wi-Fi won't associate**: check `CONFIG_EXAMPLE_WIFI_*`. Re-flash after changing.
- **No SD mount**: check card is formatted FAT32. If intentional, `CRY_DETECT_SD_ENABLED=n` silences the warning.
- **Inference very slow (>500 ms)**: `CONFIG_TFLITE_MICRO_USE_ESP_NN=y` must be set to enable the PIE-vector-accelerated kernels.
