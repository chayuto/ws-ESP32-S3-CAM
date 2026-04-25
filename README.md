# ws-ESP32-S3-CAM

Workspace for the **Waveshare ESP32-S3-CAM-GC2145** (SKU 33702, P/N `ESP32-S3-CAM-GC2145`) — the GC2145 DVP sensor variant of Waveshare's ESP32-S3-CAM-OVxxxx carrier board.

Full context, pinout, and vendor quirks live in [`CLAUDE.md`](./CLAUDE.md).

## Repo philosophy

This repo publishes **firmware + tooling only** — recordings, derived
labels, trained classifier weights, and analyses that narrate real
captures stay local. The `datasets/` tree, lab notebooks under
`projects/*/ml-experiments/`, and date-stamped narrative docs are
gitignored. See `CLAUDE.md` "Publish boundary" for the full list.

## Repo layout

```
ws-ESP32-S3-CAM/
├── CLAUDE.md                 # authoritative board notes + publish boundary
├── README.md                 # this file
├── .claude/
│   └── commands/             # /build /flash /monitor /ml-researcher /restore-factory ...
├── docs/
│   ├── blog/                 # public-facing writeups
│   └── research/             # public-info-sourced surveys, design + method docs
├── projects/
│   └── cry-detect-01/        # YAMNet-on-S3 baby-cry detector + auto-ensemble tooling
└── ref/                      # vendor docs + repos (gitignored, ~1.2 GB)
    ├── datasheets/           # ESP32-S3, GC2145, ES8311, ES7210
    ├── schematic/            # ESP32-S3-CAM schematic
    ├── wiki/                 # offline docs.waveshare.com snapshot
    └── demo/                 # ESP32-S3-CAM-OVxxxx + display-support vendor clones
```

## Active projects

### [`projects/cry-detect-01`](./projects/cry-detect-01/README.md)
Discreet bedroom baby-cry monitor. **No camera, no screen** — ES7210 mic → YAMNet-1024 INT8 (TFLite Micro) → SD-card CSV + SSE web UI + red-LED alert. Host-side data tools turn the resulting captures into auto-labelled training material via a 4-oracle ensemble (no human in the label loop).

**Status (2026-04-25):**
- Detector firing on real cries. After two firmware fixes —
  *double-sigmoid removal* in the YAMNet output path and a
  *mel-feature magnitude-vs-power correction* — the on-device
  `cry_conf` reaches 0.934 on real cries (matching FP32 YAMNet)
  versus 0.000 on silence. First `ALERT_FIRED` events (n=6 in one
  night) landed under the fixed firmware.
- Stage 2.7 file API live: remote `/files/ls`, `/files/get`,
  `/files/tail`, `/files/df` — pull logs without pulling the SD card.
- Local timezone: Sydney AEST/AEDT. Timestamps RFC-3339 with numeric
  offset (e.g. `2026-04-18T08:13:53.726+10:00`).
- Host-side **auto-ensemble** label pipeline (`tools/ensemble_audit.py`)
  merges YAMNet wide-class scores, an acoustic-feature classifier, an
  embedding classifier on YAMNet's 2048-d meanmax, a sub-type cluster,
  and temporal context into per-capture confidence tiers. Method
  documented in `docs/research/host-side-auto-ensemble-method.md`.
- ML work follows the `/ml-researcher` discipline: pre-register
  hypothesis, stamp model versions, lab-notebook gitignored,
  conclusions only land as research notes.

## Research notes

Public-eligible surveys, design docs, and method writeups under `docs/research/`:

| Document | Scope |
|---|---|
| `host-side-auto-ensemble-method.md` | The 4-oracle no-human label pipeline + LOSO discipline + ablation findings |
| `data-vault-redesign-20260425.md` | Why we removed humans from label production |
| `cry-detect-data-program-plan.md` | Long-term roadmap to first real training run |
| `cry-detect-starter-plan.md` | Why pretrained YAMNet over ESPDet/custom training |
| `crying-detection-s3-ml-alternatives.md` | Alternative ML approaches considered + rejected |
| `yamnet-class-exploitation.md` | 20 watched AudioSet classes: cry spectrum, joy context, FP sources |
| `retraining-roi-analysis.md` | Recalibration vs. head-retraining vs. full-finetune ROI |
| `log-management-design-20260423.md` | Vault layout + extraction protocol design |
| `classification-logging-plan.md` | Stage 2.6a: log all watched-class confidences per inference |
| `file-api-plan.md` | Stage 2.7 spec |
| `runtime-robustness-plan.md` | Six bug classes + prevention catalogue |
| `feasibility-s3-camera-project.md` | Vision-side feasibility (de-prioritised; cataloguing only) |
| `prior-art-survey.md`, `pretrained-espdl-inventory.md`, `yolo26-and-esp-dl-2026.md`, `yolo26-s3-port-plan.md` | Vision-side surveys (archived) |
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
