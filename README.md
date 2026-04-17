# ws-ESP32-S3-CAM

Workspace for the **Waveshare ESP32-S3-CAM-GC2145** (SKU 33702, P/N `ESP32-S3-CAM-GC2145`) — the GC2145 DVP sensor variant of Waveshare's ESP32-S3-CAM-OVxxxx carrier board.

Full context, pinout, and vendor quirks live in [`CLAUDE.md`](./CLAUDE.md).

## Repo layout

```
ws-ESP32-S3-CAM/
├── CLAUDE.md                 # authoritative board notes + build rules
├── README.md                 # this file
├── .claude/
│   └── commands/             # /build /flash /monitor /restore-factory /hardware-specs /peripherals
├── docs/
│   ├── internal/             # plans for active projects
│   └── research/             # ML/hardware/robustness deep-dives (checked in)
├── projects/
│   └── cry-detect-01/        # Stage-1 pretrained YAMNet baby-cry detector
├── logs/
│   ├── cry-detect-01-export-<UTC-TS>/   # device log exports via /files API
│   └── deploy-01-final/      # pre-reflash snapshots
└── ref/                      # vendor docs + repos (gitignored, ~1.2 GB)
    ├── datasheets/           # ESP32-S3, GC2145, ES8311, ES7210
    ├── schematic/            # ESP32-S3-CAM schematic
    ├── wiki/                 # offline docs.waveshare.com snapshot
    └── demo/                 # ESP32-S3-CAM-OVxxxx + display-support vendor clones
```

## Active projects

### [`projects/cry-detect-01`](./projects/cry-detect-01/README.md)
Discreet bedroom baby-cry monitor. **No camera, no screen** — ES7210 mic → YAMNet-1024 INT8 (TFLite Micro) → SD-card CSV + SSE web UI + red-LED alert.

**Status (2026-04-17):**
- Stage 1 flashed and running at `cry-detect-01.local` / `192.168.1.100`
- Runtime robustness hardened (stack canaries, heap poisoning, WDT, null-guarded mutexes)
- Night-mode LED brightness control
- **Stage 2.7 file API live**: remote `/files/ls`, `/files/get`, `/files/tail`, `/files/df` etc. — pull logs without pulling the SD card
- Local timezone: Sydney AEST/AEDT. Timestamps RFC-3339 with numeric offset (`2026-04-18T08:13:53.726+10:00`)
- **Known blocker**: synthetic-waveform INT8 PTQ compressed all 521 YAMNet outputs into 0.56-0.64 band; real events are detected by mic+RMS (confirmed in `logs/.../ANALYSIS.md`) but not separable in model output. Stage 2.1 real-audio recalibration is the critical path.

See [`docs/internal/cry-detect-01-plan.md`](./docs/internal/cry-detect-01-plan.md) for the stage roadmap.

## Research notes

Permanent, citable deep-dives under `docs/research/`:

| Document | Scope |
|---|---|
| `cry-detect-starter-plan.md` | Why pretrained YAMNet over ESPDet/custom training |
| `deployment-01-log-analysis-20260418.md` | 8-hour overnight run — 14 elevated-RMS windows, 0 alerts, compression diagnosis |
| `retraining-roi-analysis.md` | Recalibration vs. head-retraining vs. full-finetune ROI |
| `classification-logging-plan.md` | Stage 2.6a: log all watched-class confidences per inference |
| `file-api-plan.md` | Stage 2.7 spec (this release) |
| `runtime-robustness-plan.md` | Six bug classes + prevention catalogue |
| `yamnet-class-exploitation.md` | 20 watched AudioSet classes: cry spectrum, joy context, FP sources |

## ESP-IDF

- **Version required:** ≥ v5.5.1 (we use 5.5.3 at `~/esp/esp-idf`)
- **Activate:** `. ~/esp/esp-idf/export.sh` (source it; Claude Code will prompt for the `.` builtin — answer yes once per session)

## Build / flash / monitor

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/cry-detect-01 -B /tmp/ws-cry-detect-01-build build
idf.py -C projects/cry-detect-01 -B /tmp/ws-cry-detect-01-build -p /dev/cu.usbmodem3101 flash
```

Monitoring: `idf.py monitor` is broken in non-TTY shells — use the pyserial snippet in `.claude/commands/monitor.md`.

## Hardware quick-reference

| | Value |
|---|---|
| CPU | ESP32-S3 rev v0.2, dual-core Xtensa LX7 @ 240 MHz |
| PSRAM | 8 MB OPI @ 80 MHz |
| Flash | 16 MB quad (3.3 V), QIO @ 80 MHz |
| Camera | GC2145 DVP (PID 0x2145, SCCB 0x3C) |
| Audio DAC | ES8311 (I²C 0x30 8-bit / 0x18 7-bit) |
| Audio ADC | ES7210 4-channel (I²C 0x40, I²S slave; dual MEMS mics) |
| I/O expander | CH32V003 over I²C (LED, LCD-reset, backlight PWM, battery ADC) |
| USB | Native USB-Serial/JTAG on GPIO 19/20 |
| SD | 1-bit SDMMC (CLK=16 / CMD=43 / D0=44) |
| Buttons | BOOT @ GPIO 0, user @ GPIO 15 |
| Built-in temp sensor | ESP32-S3 internal (die temp, via `driver/temperature_sensor.h`) |

Full authoritative pinout in [`CLAUDE.md`](./CLAUDE.md#authoritative-pinout).

## Differences from sibling repos

| | `ws-ESP32-S3-CAM` (this) | `ESP32-C6-Touch-AMOLED-1.8` |
|---|---|---|
| Target | `esp32s3` | `esp32c6` |
| Cores | 2 (Xtensa LX7) | 1 (RISC-V) |
| PSRAM | 8 MB OPI ✓ | None |
| Camera | GC2145 onboard | None |
| Screen | External FPC only | Onboard AMOLED |
| Prior baby-cry work | YAMNet ML (this repo) | RMS + ratio rule-based |

The sibling C6 project's rule-based detector serves as the empirical baseline for judging whether cry-detect-01's ML model is finding events that actually occurred.
