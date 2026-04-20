# ESP32-S3-CAM-GC2145 Hardware Specs (authoritative)

Read this before writing any code that touches peripherals, memory, Wi-Fi, camera, or audio. All GPIO and address numbers below are verified against the vendor BSP (`ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/04_dvp_camera_display/components/waveshare__esp32_s3_cam_ovxxxx/include/bsp/esp32_s3_cam_ovxxxx.h`) and this unit's boot logs.

---

## Chip

- **ESP32-S3** Xtensa LX7, **dual-core** @ 240 MHz — confirmed `cpu_freq: 240000000 Hz`
- Chip rev **v0.2**, efuse block rev **v1.3**, QFN56
- FPU (single-precision), SIMD (PIE vector), WiFi 4 + BLE 5. **No 802.15.4**
- Dual-core → `xTaskCreatePinnedToCore()` is valid; pin Wi-Fi to 0, app to 1 for jitter isolation

## Memory

- **SRAM:** 512 KB on-chip. After boot: ~316 KiB main + ~21 KiB + ~7 KiB RTCRAM available for dynamic alloc
- **PSRAM:** **8 MB OPI** (AP Memory gen-3, 64 Mbit density, **80 MHz**, hybrid-wrap 32-byte burst, latency 10 cycles)
  - Boot log: `Adding pool of 7488K of PSRAM memory to heap allocator` (+33 K gap) ≈ **7.5 MB free to heap**
- **Flash:** **16 MB** external NOR, QIO @ 80 MHz, 3.3 V
- Wi-Fi/LWIP prefer SPIRAM (`WiFi/LWIP prefer SPIRAM` in boot log)
- Full 320×240 RGB565 fits trivially; full 1600×1200 UXGA raw (3.84 MB) fits in PSRAM

## I²C Bus (single shared bus)

**SCL = GPIO 7, SDA = GPIO 8, 100 kHz** (camera example) / 400 kHz (audio example).

| Device | I²C address | Role |
|---|---|---|
| ES8311 | `ES8311_CODEC_DEFAULT_ADDR` (0x18 7-bit / 0x30 8-bit) | Audio codec control (playback path) |
| ES7210 | `ES7210_CODEC_DEFAULT_ADDR` (0x40 7-bit typical) | 4-ch ADC control (mic array) |
| GC2145 (SCCB) | 0x3C 7-bit | Camera sensor control |
| CH32V003 IO Expander | `CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS` | LCD reset/backlight, touch reset, battery ADC, red LED |
| LCD Touch controller (if LCD fitted) | 0x15 (CST816S/CST328) or 0x38 (FT6336) | Depends on LCD variant |

The `esp_codec_dev` library uses the 8-bit convention (0x30 for ES8311); driver internally does `addr >> 1`.

## I²S Audio

| Signal | GPIO | Direction |
|---|---|---|
| MCLK | 10 | ESP32 → codec master clock |
| BCLK (SCLK) | 11 | Bit clock |
| LRCLK (WS) | 12 | Word select |
| DOUT | 14 | ESP32 → ES8311 DAC (speaker out) |
| DSIN | 13 | ES7210 mic → ESP32 (record in) |

Boot log confirms: ES7210 is **slave**, 32-bit data / 32-bit slot, stereo (slot_mask 0x3), 16 kHz default sample rate, duplex mono. Onboard dual-mic (MIC1 + MIC2 enabled) — designed for beamforming/noise-cancel.

## Camera (DVP via LCD_CAM peripheral)

| Signal | GPIO |
|---|---|
| XCLK | 38 |
| PCLK | 41 |
| VSYNC | 17 |
| HSYNC | 18 |
| D0 | 45 |
| D1 | 47 |
| D2 | 48 |
| D3 | 46 |
| D4 | 42 |
| D5 | 40 |
| D6 | 39 |
| D7 | 21 |
| PWDN | NC (not wired) |
| RESET | NC (not wired) |
| SCCB SCL/SDA | **shared main I²C** (GPIO 7/8) |

- Sensor on this SKU: **GC2145** (Galaxycore 2MP UXGA, PID `0x2145`) — SCCB 0x3C
- Default XCLK 16–20 MHz; default fmt RGB565 @ QVGA, double-buffered in PSRAM, EDMA-friendly
- Auto-probe order: OV2640 → OV5640 → GC0308 → GC2145. Expect **three `E (…)` lines** in boot before the GC2145 success log. Those are expected probe misses, NOT errors.

## External LCD (optional, via FPC)

BSP supports 1.83" / 2" / 2.8" / 3.5" touch LCDs — pins below are shared; which LCD controller is used depends on the fitted FPC.

| Signal | GPIO | Notes |
|---|---|---|
| LCD D0 / PCLK / DC / CS | 1 / 5 / 3 / 6 | 8-bit SPI on **SPI2_HOST** @ 80 MHz |
| LCD RST | via CH32V003 pin P2 | Not a real GPIO |
| LCD Backlight PWM | via CH32V003 pin P1 | Not a real GPIO |
| Touch RST | via CH32V003 pin P0 | Not a real GPIO |
| Touch INT | 9 | Real GPIO |

Candidate controllers (all included by BSP): **ST7796**, **ILI9341**. Touch controllers: **CST816S / CST328 / FT6336**.

## SD Card (SDMMC 1-bit mode)

| Signal | GPIO |
|---|---|
| CLK | 16 |
| CMD | 43 |
| D0  | 44 |

Only 1-bit SDMMC (no D1-D3 wired). Can also run as SDSPI.

## Buttons

| Function | GPIO |
|---|---|
| BOOT button | 0 (strapping pin; active LOW, external pull-up) |
| User button | **15** |

The second user button on GPIO 15 is easy to miss — it's on the BSP but not called out in the product page.

## IO Expander — CH32V003

A RISC-V microcontroller used as an I²C IO expander. Vendor ships a custom driver:
`waveshare/custom_io_expander_ch32v003`. Fixed firmware; treat as a dumb peripheral.

Known pin assignments (from BSP):
- P0 → touch reset
- P1 → LCD backlight (PWM)
- P2 → LCD reset
- P4 → touch INT routing
- P6 → onboard red LED (commented-out in BSP enum but wired)
- ADC → battery voltage sense (`bsp_get_io_expander_adc()`)

> *Older revisions may have used a different I/O expander — BSP comment hints at multiple HW revs. Current mass-produced units use CH32V003.*

## USB

- Native **USB-Serial/JTAG** on GPIO 19 (D-) / 20 (D+)
- Auto-enter download mode via DTR/RTS (no BOOT button needed for normal flash)
- Force-bootloader: unplug → hold BOOT → plug → release BOOT
- Also has **USB-OTG host** capability (example 06 uses it for UVC ingest)

## Wi-Fi / Bluetooth

- Wi-Fi 4 (802.11 b/g/n) @ 2.4 GHz, Tx up to +21 dBm
- BLE 5. Disable with `CONFIG_BT_ENABLED=n` for ~50 KB flash + 30 KB RAM savings if unused
- Boot log confirms: `wifi firmware version: 4df78f2`, `WiFi/LWIP prefer SPIRAM`, `WiFi IRAM OP enabled`

## Power

- USB-C 5 V input **OR** single-cell Li (3.7 V nominal, ≤ 2000 mAh) via GH1.25 connector
- **Do NOT parallel multiple batteries**
- No PMIC — simple linear regulators
- Battery voltage read via CH32V003 ADC (not direct on the ESP32-S3)

## Flash Partition Layout (vendor ESP-SR example, as installed now)

| # | Label | Type | Offset | Size | Notes |
|---|---|---|---|---|---|
| 0 | nvs | data:nvs | 0x9000 | 24 KB | Wi-Fi creds, NVS |
| 1 | factory | app:factory | 0x10000 | 3 MB | App binary |
| 2 | flash_test | data:fat | 0x310000 | 528 KB | FAT scratch |
| 3 | model | data:spiffs | 0x394000 | 5900 KB | ESP-SR wake/command models |

Plain video-server example uses a larger `factory` (5 MB) and no model partition. Custom partitions belong in `partitions.csv` and require `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`, `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`.

## Accelerators & Peripherals Summary

| Accelerator | Present | Notes |
|---|---|---|
| AES / SHA / RSA / ECC / HMAC / RNG / DS | ✓ | mbedTLS auto-uses |
| **LCD_CAM** peripheral | ✓ | DVP camera RX + LCD TX (8/16-bit parallel) |
| I²S (2 ports) | ✓ | `CONFIG_SOC_I2S_NUM=2`, HW v2, PDM RX/TX, PCM |
| JPEG (hardware) | ✗ | P4-only. Use `espressif/esp_new_jpeg` for SW encode |
| USB-OTG host | ✓ | UVC ingest, MSC, HID as host |
| USB-Serial/JTAG | ✓ | What the single USB-C uses |
| 802.15.4 (Thread/Zigbee) | ✗ | C6-only — not available here |
| PSRAM DMA capable | ✓ | `SOC_PSRAM_DMA_CAPABLE=y` |

## ESP-SR (speech recognition) — what's onboard

- WakeNet wake words supported in `esp-sr` v2.1.5+: `wn9_hiesp` ("Hi ESP" — factory default), `wn9_hilexin`, `wn9_nihaoxiaozhi`, `wn9s_hijason`, `wn9_alexa`, `wn9_jarvis_tts`, and many more — one-hot select via Kconfig `CONFIG_SR_WN_*`
- AFE pipeline on boot: **AEC (SR_LOW_COST) → SE (BSS) → VAD (WebRTC) → WakeNet**
- MultiNet command recognition: **mn5q8_en** (English 8-bit quantized, up to ~32 phrases)
- 16 kHz PCM input, 4 channels (2 mics + 1 playback echo-ref + 1 spare)
- **Works on ESP32-S3** (unlike C6 which only supports WakeNet standalone)

## Typical RAM Budget (verified, with Wi-Fi + MJPEG + PSRAM)

```
FreeRTOS + drivers:        ~50 KB SRAM
LWIP + WiFi stack:         ~85 KB SRAM
ESP-SR AFE pipeline:      ~110 KB SRAM (when active)
esp_video framebuffer:    ~600 KB PSRAM
Camera JPEG buffer:       ~100 KB PSRAM
```

With PSRAM in the mix, SRAM stays well under 50% even for heavy workloads.

## Gotchas Confirmed on This Hardware

- Vendor `dependencies.lock` has wrong absolute paths baked in (`/System/Volumes/Data/home/wxggc/…`) — **delete before first build**
- Vendor ships a hand-tuned `sdkconfig` in git — **never `rm sdkconfig`**, patch in place
- Camera SCCB auto-probe logs 3 expected `E (…)` errors before the success line
- `idf.py monitor` does NOT work in non-TTY shells — use the pyserial snippet in `/monitor`
- ESP7210 detected as I²S slave, not master — don't try to force master mode
- USB port shows only as `/dev/cu.usbmodemXXXX` on macOS (native USB-CDC, no CP2102 enumerated separately)
- `ES8311_CODEC_DEFAULT_ADDR` is **0x30 (8-bit)** not 0x18 (7-bit) — `esp_codec_dev` convention
