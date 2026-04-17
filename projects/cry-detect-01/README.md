# cry-detect-01

Stage-1 pretrained baby-cry detector on the Waveshare ESP32-S3-CAM-GC2145.

Pipeline: ES7210 mic → I²S DMA (16 kHz mono) → log-mel patch (96×64) → **YAMNet-1024 INT8** (TFLite Micro) → threshold on class-20 (*Baby cry, infant cry*) → red LED + SD log + web UI over Wi-Fi.

No training. No fine-tuning. No vision. No AFE. See `docs/internal/cry-detect-01-plan.md` for the full plan and `docs/research/cry-detect-starter-plan.md` for the decisions behind it.

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
- **Format**: `ISO8601,event,cry_conf,latency_ms,free_heap,rssi`.

Pre-NTP lines are stamped with uptime-seconds and flagged `NOT_SYNCED`; they become queryable by wall-clock once NTP lands (historical lines are not rewritten).

## Tuning knobs

All in `idf.py -C . -B build menuconfig` → "Cry-detect-01":

| Symbol | Default | Tune if... |
|---|---|---|
| `CRY_DETECT_THRESHOLD` | 40 | Lower for more sensitivity; raise to suppress TV/pet false alarms |
| `CRY_DETECT_CONSEC_FRAMES` | 3 | Raise for more stability; lower for faster response |
| `CRY_DETECT_MIC_GAIN_DB` | 36 | Check `/metrics.input_rms`; raise if flat, lower if clipping |
| `CRY_DETECT_HOLD_MS` | 5000 | Alert latch duration |

## Partition layout

```
nvs         24 KB
factory      5 MB   app + runtime
yamnet       4 MB   SPIFFS, yamnet.tflite (~3 MB)
logs_fat     1 MB   FAT, SD-absent log fallback
```

## Files

See `docs/internal/cry-detect-01-plan.md` §2 for module layout and §10 for boot sequence.

## Troubleshooting

- **"Model load failed"** (LED 0.5 Hz blink): `spiffs/yamnet.tflite` not present or is the 256-d embedding-only variant. Re-run `./tools/fetch_model.sh`.
- **Wi-Fi won't associate**: check `CONFIG_EXAMPLE_WIFI_*`. Re-flash after changing.
- **No SD mount**: check card is formatted FAT32. If intentional, `CRY_DETECT_SD_ENABLED=n` silences the warning.
- **Inference very slow (>500 ms)**: `CONFIG_TFLITE_MICRO_USE_ESP_NN=y` must be set to enable the PIE-vector-accelerated kernels.
