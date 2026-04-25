# CLAUDE.md — ws-ESP32-S3-CAM

Workspace for the **Waveshare ESP32-S3-CAM-GC2145** (SKU 33702, P/N `ESP32-S3-CAM-GC2145`).

The board is the GC2145 sensor variant of Waveshare's ESP32-S3-CAM-OVxxxx platform — same
carrier board, only the DVP camera module differs (OV5640 / OV3660 / GC2145 / GC0308).

## Publish boundary — read this before committing

This repo is public. We publish **firmware + tooling only.** We do not
publish recordings, derived labels, trained models, or analyses that
reference real captures, and nobody is meant to retrain from this data.

**Public-eligible (commit freely):** firmware C/C++/Python under
`projects/*/main`, `projects/*/tools`, `projects/*/www`, the blog post
at `docs/blog/`, slash commands under `.claude/commands/`, and
**`docs/research/`** — survey, prior-art, design, and planning docs
sourced from public information are fine to commit. The only docs
gitignored under `docs/research/` are the date-stamped patterns that
narrate real captures (see below).

**Private (kept local, gitignored — never `git add -f`):**
- `datasets/` — captures, labels, releases, inventories.
- `projects/*/logs/` — raw device logs.
- `projects/*/hf/*.pkl`, `projects/*/hf/*.tflite` — models trained on
  or derived from private audio.
- `projects/*/ml-experiments/*` — lab notebooks (only README/.gitkeep).
- `docs/internal/` — working plans, todos, audits.
- `docs/private/` — personal drafts.
- `docs/research/{night-session,deployment,deep-analysis,embed-clf,data-reassessment}-*.md`
  — narrative reports referencing capture timestamps.

**When writing a new research note**, ask: would this be meaningful
to a stranger without seeing the user's data? If no → name it to fit
one of the private patterns above, or place it in `docs/internal/`.
If unsure, ask the user before committing.

**When updating tooling that auto-generates a report** (e.g.
`build_inventory.py` → `INVENTORY.md`, `freeze_release.py` →
`releases/*.json`), the *tool* is public, but its *output* is private
and stays gitignored under `datasets/`.

## Repo Layout

```
ws-ESP32-S3-CAM/
├── CLAUDE.md
├── .claude/commands/        # /build /flash /monitor /restore-factory /hardware-specs
├── .gitignore               # excludes /ref/, build/, managed_components/
├── ref/                     # vendor docs + vendor git clones (gitignored, 1.2 GB)
│   ├── README.md            # index of sources
│   ├── datasheets/          # ESP32-S3, GC2145, ES8311, ES7210 PDFs
│   ├── schematic/           # ESP32-S3-CAM schematic PDF
│   ├── wiki/                # offline HTML snapshot of docs.waveshare.com
│   └── demo/
│       ├── ESP32-S3-CAM-OVxxxx/       # official vendor repo (examples, Firmware/*.bin)
│       └── ESP32-display-support/     # vendor display driver repo
└── projects/                # YOUR projects — one directory each (not yet created)
```

## Board Facts (verified on this unit)

- **Chip:** ESP32-S3 rev v0.2, QFN56, dual-core Xtensa LX7 @ 240 MHz
- **PSRAM:** 8 MB OPI (AP Memory gen-3, 80 MHz, ~7.5 MB free to heap)
- **Flash:** 16 MB quad (3.3V), QIO @ 80 MHz
- **USB:** Native USB-Serial/JTAG on GPIO 19/20 (port shows as `/dev/cu.usbmodemXXXX` directly)
- **Camera:** 24-pin DVP, GC2145 sensor (Galaxycore 2MP UXGA, PID `0x2145`, SCCB 0x3C, shares main I²C)
- **Audio codec:** ES8311 (I²C default 0x30 8-bit / 0x18 7-bit) — playback / DAC → speaker
- **Audio ADC:** ES7210 (I²C default 0x40 7-bit, I²S slave, **dual-mic array** MIC1+MIC2)
- **I/O expander:** CH32V003 RISC-V (via `waveshare/custom_io_expander_ch32v003`) — controls LCD reset (P2), backlight PWM (P1), touch reset (P0), red LED (P6), battery ADC
- **Display:** external only — FPC accepts 1.83" / 2" / 2.8" / 3.5" touch LCDs (ST7796 / ILI9341 + CST816S / CST328 / FT6336 touch)
- **SD card:** 1-bit SDMMC (CLK=16, CMD=43, D0=44)
- **Buttons:** BOOT on GPIO 0 + **user button on GPIO 15**
- **Power:** single-cell 3.7V Li via GH1.25, max ~2000 mAh; no PMIC
- **BOOT recovery:** hold BOOT + plug USB to force download mode

## Authoritative Pinout (verified via vendor BSP `waveshare__esp32_s3_cam_ovxxxx`)

| Bus / Function | GPIO(s) |
|---|---|
| I²C SCL / SDA (shared: ES8311, ES7210, CH32V003, GC2145 SCCB, touch) | 7 / 8 |
| I²S MCLK / BCLK / LRCLK | 10 / 11 / 12 |
| I²S DOUT (→ ES8311 DAC) / DSIN (← ES7210 mic ADC) | 14 / 13 |
| Camera XCLK / PCLK / VSYNC / HSYNC | 38 / 41 / 17 / 18 |
| Camera D0..D7 | 45, 47, 48, 46, 42, 40, 39, 21 |
| LCD D0 / PCLK / DC / CS (SPI2 8-bit parallel @ 80 MHz) | 1 / 5 / 3 / 6 |
| LCD touch INT | 9 |
| SD card D0 / CMD / CLK (SDMMC 1-bit) | 44 / 43 / 16 |
| Buttons: BOOT / user | 0 / 15 |
| USB D- / D+ | 19 / 20 |
| LCD RST / backlight / touch RST / red LED | via CH32V003 P2 / P1 / P0 / P6 |

## Key Differences from Sibling C6 Repos

| | ESP32-S3-CAM (this) | ESP32-C6-AMOLED/LCD |
|---|---|---|
| Target | `esp32s3` | `esp32c6` |
| Arch | Xtensa, dual-core | RISC-V, single-core |
| PSRAM | 8 MB OPI ✓ | None |
| Wi-Fi | Wi-Fi 4 + BLE 5 | Wi-Fi 6 + BLE 5 + 802.15.4 |
| Display | External via FPC | Onboard SH8601 / LCD |
| Camera | GC2145 DVP onboard | None |
| USB | Native USB-CDC + USB-OTG | Native USB-CDC only |
| `xTaskCreate` | can pin to core (dual-core) | must not pin (single-core) |

## ESP-IDF Environment

- **Version required:** ≥ v5.5.1 (we use 5.5.3, installed at `~/esp/esp-idf`)
- **Activate:** `. ~/esp/esp-idf/export.sh`
- **Python venv:** `~/.espressif/python_env/idf5.5_py3.14_env/bin/python` (has pyserial; system python does not)

## Build & Flash

```zsh
# Activate IDF (once per shell)
. ~/esp/esp-idf/export.sh

# Build a vendor example (out-of-tree build dir keeps the clone clean)
idf.py -C ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/01_simple_video_server \
       -B /tmp/ws-esp32s3-build/01 build

# Flash (native USB port auto-enters bootloader on DTR/RTS)
idf.py -C <proj> -B <build> -p /dev/cu.usbmodem3101 flash

# Monitor (idf.py monitor does NOT work in non-TTY — use pyserial dance, see /monitor)
```

## Critical Rules (learned the hard way)

- **Always set `-C <project>`** — `idf.py` alone operates on cwd, not necessarily the project.
- **Delete `dependencies.lock` before first build** — vendor ships it with their build-machine's absolute `IDF_PATH` (`/System/Volumes/Data/home/wxggc/...`) baked in. Fails with *"path field in the manifest file does not point to a directory"*.
- **Never `rm sdkconfig` on a vendor example** — Waveshare tracks their hand-tuned `sdkconfig` in git (16 MB flash, OPI PSRAM, custom partition table). Regenerating from their minimal `sdkconfig.defaults` drops critical settings and breaks the build. To restore: `git checkout sdkconfig`.
- **Patch creds in sdkconfig in place**, don't delete and rebuild from defaults. Use `sed -i ''` on macOS.
- **Camera auto-probes via SCCB** — expect noisy `E (560) ov2640: get sensor ID failed` / `ov5640: PID=0x9090` / `gc0308: Get sensor ID failed` logs BEFORE the success line `I (569) gc2145: Detected Camera sensor PID=0x2145`. Not errors — just failed probes for absent sensors.
- **Port number is stable per USB cable** — `/dev/cu.usbmodem3101` on this host. Check `ls /dev/cu.usbmodem*` on fresh connection.
- **Factory firmware is reversible** — `ref/demo/ESP32-S3-CAM-OVxxxx/Firmware/ESP32-S3-CAM-XXXX-Factory.bin` restores the shipped multi-app launcher (Settings/Camera/Music/XiaoZhi AI). Also variants for each LCD size.
- **ESP32-S3 is dual-core** — `xTaskCreatePinnedToCore()` is valid here (unlike C6). Typical split: `tskNO_AFFINITY` for app tasks, pin Wi-Fi to core 0 by default.
- **PSRAM exists** — full framebuffers, JPEG encoder buffers, and Wi-Fi RX can all live in PSRAM. Set `CONFIG_SPIRAM_USE_MALLOC=y` (or equivalent) to auto-place.

## Wi-Fi Credentials Pattern (protocol_examples_common)

The Waveshare examples use ESP-IDF's `example_connect` which reads these Kconfig symbols:

```
CONFIG_EXAMPLE_WIFI_SSID="<ssid>"
CONFIG_EXAMPLE_WIFI_PASSWORD="<password>"
```

For credential hygiene **when the example lives outside `ref/`**:
- `sdkconfig.defaults` — real creds, gitignored
- `sdkconfig.defaults.template` — placeholder, committed

Inside `ref/` the whole directory is gitignored, so direct edits are safe.

## Vendor Example Index (`ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/`)

| # | Example | Needs LCD? | Purpose |
|---|---|---|---|
| 01 | `01_simple_video_server` | No | Wi-Fi MJPEG server — control on :80, stream on :81 |
| 02 | `02_esp_sr` | No | ESP-SR wake-word + command recognition (needs mic) |
| 03 | `03_audio_play` | No | ES8311 DAC playback |
| 04 | `04_dvp_camera_display` | Yes | DVP camera → LCD passthrough |
| 05 | `05_lvgl_brookesia` | Yes | LVGL + Brookesia launcher (factory firmware source) |
| 06 | `06_usb_host_uvc` | No | USB Host UVC webcam ingest |

GC2145 driver lives at `examples/ESP-IDF-v5.5.1/04_dvp_camera_display/components/espressif__esp32-camera/sensors/gc2145.{c,h}`.

## Agent Slash Commands

| Command | Description |
|---|---|
| `/build <path>` | Activate IDF and build out-of-tree with known-failure recovery |
| `/flash <path>` | Auto-detect port and flash to the connected board |
| `/monitor` | Non-TTY-safe serial reader (15 s capture) |
| `/restore-factory` | Re-flash the shipped Waveshare factory firmware |
| `/hardware-specs` | Full authoritative hardware reference (CPU, memory, camera, audio, I/O) |
| `/peripherals` | Cookbook for camera / ES8311 / ES7210 / ESP-SR via the BSP |

## ESP-SR (verified working)

Vendor example `02_esp_sr` flashed and booted OK on this unit. Boot confirmed:

- **WakeNet wake word**: `wn9_hiesp` ("Hi ESP"), quantized `wakeNet9_v1h24_Hi,ESP_3_0.63_0.635`
- **AFE pipeline**: input → AEC(SR_LOW_COST) → SE(BSS) → VAD(WebRTC) → WakeNet
- **MultiNet** command model: `mn5q8_en` (English 8-bit quantized) — 4 default "backlight" intents
- **Input**: 16 kHz PCM, 4 channels (2 mic + 1 playback ref + 1 spare)
- ES7210 configured as I²S slave, 32-bit data/slot, MIC1+MIC2 enabled

Change wake word via menuconfig → `CONFIG_SR_WN_*` (Hi ESP / Hi Lexin / Alexa / Jarvis / …). ESP-SR 2.1.5+ bundles many prebuilt models.

## What Works Right Now (verified 2026-04-17)

- ESP-IDF v5.5.3 toolchain builds vendor examples 01 and 02 after deleting the stale `dependencies.lock`
- Board at `/dev/cu.usbmodem3101`, chip probe confirms ESP32-S3 rev v0.2 + 8 MB PSRAM + 16 MB flash
- `01_simple_video_server` — GC2145 detected, joined Wi-Fi, DHCP on local LAN, MJPEG stream live on port 81 @ 320×240 @ ~13 fps
- `02_esp_sr` — ES7210 detected, WakeNet wn9_hiesp loaded from SPIFFS `model` partition, AFE pipeline running, listening for "Hi ESP"
