# Hardware verification for Stage 1 — ground-truth audit

Before any code for the cry-detect project (Stage 1 per `cry-detect-starter-plan.md`), this doc verifies every peripheral the pipeline touches against the vendor BSP source. Especially covers the **SD card story** (C6 had bus-sharing issues — do they recur here?) and the **runtime UI story** (this board has no onboard display — how do we get feedback?).

Source-of-truth files audited:
- `ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/02_esp_sr/managed_components/waveshare__esp32_s3_cam_ovxxxx/include/bsp/esp32_s3_cam_ovxxxx.h`
- Same dir `.../esp32_s3_cam_ovxxxx.c` (BSP implementation)
- `02_esp_sr/main/speech_det_driver/mic_speech.c` (closest reference pipeline)
- `02_esp_sr/partitions.csv` and `02_esp_sr/sdkconfig`
- Custom IO-expander driver under `managed_components/.../custom_io_expander_ch32v003.c`

---

## 1. SD card — does the C6 SPI-sharing problem recur here?

**No.** The root cause on C6 was *"C6 has no SDMMC peripheral and only one user SPI host (SPI2), so SD and display both forced onto SPI2 and had to time-multiplex."* That premise is false on this board.

### Side-by-side

| Dimension | **ESP32-C6-Touch-AMOLED-1.8** | **ESP32-S3-CAM-GC2145** (this) |
|---|---|---|
| SD protocol | **SPI** (forced) | **SDMMC** (native) |
| SD host | `SPI2_HOST` shared with QSPI display | Dedicated SDMMC host, not shared with anything |
| Display on board | Onboard QSPI AMOLED, same SPI host as SD | **No onboard display.** FPC optional. |
| Bus contention | Yes — display vs SD, required release/reclaim sequence | **None** — SDMMC and camera/audio/display are independent peripherals |
| Data lines | 1-bit SPI | 1-bit SDMMC (D0 only; D1–D3 NC on PCB) |
| Realistic throughput | ~1–2 MB/s | **~2–5 MB/s** (1-bit SDMMC) |
| Time-multiplexing required | Yes (release/reclaim state machine) | **No** |
| Kind of failure on C6 | Corrupted frames / FAT crashes if the two drivers co-held SPI2 | N/A — doesn't apply |

Verified from `esp32_s3_cam_ovxxxx.h:131-135` (pinout macros) and `esp32_s3_cam_ovxxxx.c:131-205` (mount impl via `esp_vfs_fat_sdmmc_mount` — pure SDMMC, no SPI driver touched).

### Pinout (verified vs. spec)

| Signal | GPIO | Conflict risk |
|---|---|---|
| SD CLK | 16 | None — not a strapping pin, not used by audio/camera/USB |
| SD CMD | 43 | This is U0TXD (UART0 default). **But** the board uses native USB-Serial/JTAG (GPIO 19/20) for console; UART0 is free for SD reuse. Logged as a note below. |
| SD D0 | 44 | This is U0RXD. Same reasoning — freed by USB-CDC console. |

**UART0 repurposing note:** On ESP32-S3, GPIO 43/44 default to U0TXD/U0RXD. This board remaps them to SDMMC because the console is on the native USB port (GPIO 19/20). If a future project needs classic UART0 console *and* SD at the same time, there is a conflict — but for Stage 1 we use USB-CDC for logs, so this is fine. The vendor BSP commits to this assignment in `esp32_s3_cam_ovxxxx.h:131-135`.

### SD remaining limitations (not blockers for Stage 1)

- **1-bit only.** D1–D3 not wired → capped at ~20 Mbps SDMMC theoretical, ~2–5 MB/s in practice. More than enough for cry-detect logging (raw 16 kHz 16-bit mono = 32 KB/s = 1 % of SD capacity).
- **No hot-swap detection** in the BSP. The `bsp_sdcard_sdmmc_mount()` fails if no card is present; no interrupt on insertion. Plan: fail-soft at boot, log-only-to-console if SD absent.
- **Mount point:** `/sdcard` (default `CONFIG_BSP_SD_MOUNT_POINT`).
- **File system:** FAT32 via `esp_vfs_fat_sdmmc_mount()` with 16 KB cluster, max 5 open files (defaults from `esp32_s3_cam_ovxxxx.c:163-171`).
- **Write pattern for Stage 1:** Append to a rotating log file (`/sdcard/cry_YYYYMMDD.log`); close and reopen every N lines to ensure flush; no audio-raw dump by default (too large over Wi-Fi + SD combined).

### Verdict

SD card usage on this board is **straightforward and safer than the C6 path**. No shared-SPI dance, no release/reclaim state machine, no concurrent-access issues. Stage 1 can write logs continuously while Wi-Fi + audio DMA run.

---

## 2. No onboard screen — runtime UI via Wi-Fi

**Fact:** The Waveshare ESP32-S3-CAM-GC2145 has **no onboard display** in the default SKU. Display is a separate FPC add-on (1.83" / 2" / 2.8" / 3.5" variants); we don't have one fitted. This deletes the display-driven UI path and **forces runtime feedback over Wi-Fi**.

### Runtime UI plan

Build a small web server that delivers:

| Endpoint | Type | Content |
|---|---|---|
| `GET /` | `text/html` | Single-page UI (vanilla HTML + JS, <20 KB) |
| `GET /metrics` | `application/json` | Live counters: `cry_conf`, `frame_count`, `fps`, `uptime`, `heap_free`, `psram_free`, `ntp_synced`, `wifi_rssi` |
| `GET /events` | `text/event-stream` | Server-Sent Events stream of detections + timing |
| `GET /log/tail` | `text/plain` | Last N log lines (from SD or ring buffer) |
| `GET /mel.png` (optional, later) | `image/png` | Current mel-spectrogram patch rendered as PNG |

### Why SSE over WebSocket

- One-way push (server → browser) is all we need — matches SSE's model exactly.
- SSE works over plain HTTP, no handshake upgrade, trivial in `esp_http_server`.
- Automatic reconnect in the browser.
- Lower server-side memory than a WebSocket session.

### RAM budget for the web UI

| Item | Cost |
|---|---|
| `esp_http_server` core | ~8 KB code + ~4 KB per active connection |
| SSE connection state | ~2 KB per open client |
| HTML/JS payload (served from flash) | 0 RAM |
| Metrics snapshot JSON | <1 KB |

Cap at 2 concurrent SSE clients → ~16 KB total. With 8 MB PSRAM and 316 KB internal SRAM available, this is trivial.

### Why *not* mDNS/Bonjour

Nice-to-have (reach device as `cry-detect.local`) but adds ~15 KB flash + a service task. Defer unless the IP-in-serial-log pattern becomes annoying.

### LED is the offline fallback

If Wi-Fi is down, the red LED on CH32V003 P6 is the only feedback channel. Stage 1 wires this regardless:
- LED OFF: idle
- LED blink 1 Hz: Wi-Fi connecting
- LED blink 4 Hz: NTP syncing
- LED SOLID ON: cry detected (latched for N seconds)

Verified: P6 is active-LOW on this board (see §6 below).

---

## 3. Audio capture (ES7210 → I²S)

**Verified from** `esp32_s3_cam_ovxxxx.h:81-85` and `mic_speech.c:173-192`.

| Parameter | Value |
|---|---|
| I²S port | `I2S_NUM_1` (`CONFIG_BSP_I2S_NUM=1` in sdkconfig) |
| Role | ESP32-S3 **master**; ES7210 slave |
| Standard mode | Philips I²S (`I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG`) |
| MCLK / BCLK / LRCLK | GPIO 10 / 11 / 12 |
| DSIN (mic in) / DOUT (speaker) | GPIO 13 / 14 |
| BSP default sample rate | 22 050 Hz — **must override** |
| Slot width | 32-bit on the wire |
| Data width | 16-bit per sample |
| Mono/stereo at I²S | Mono (`I2S_SLOT_MODE_MONO`); ES7210 actually exposes 4 channels via TDM, BSP config is minimal mono |
| DMA buffer placement | PSRAM (`MALLOC_CAP_SPIRAM`) per `mic_speech.c:63` |

### How to get 16 kHz mono PCM (Stage 1 target)

```c
bsp_i2c_init();                                  // idempotent
bsp_io_expander_init();                          // idempotent; needed for LED
esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
esp_codec_dev_sample_info_t fs = {
    .sample_rate    = 16000,                     // override BSP default
    .channel        = 1,                         // mono
    .bits_per_sample = 16,
};
esp_codec_dev_set_in_gain(mic, 36.0f);           // start modest; tune after first capture
ESP_ERROR_CHECK(esp_codec_dev_open(mic, &fs));
// ... then repeatedly: esp_codec_dev_read(mic, buf, len);
```

Gain reference: the `02_esp_sr` example uses `42.0 dB` for wake-word work; 36 dB is a quieter starting point for cry detection which typically hits 80+ dB SPL at the crib.

**Cannot directly set HPF / ALC** from BSP — `esp_codec_dev` exposes only analog gain. If Stage 2 needs HPF for rumble rejection, we'd write ES7210 registers directly over I²C to address 0x40 (7-bit).

---

## 4. I²C bus inventory

Shared 100/400 kHz bus on GPIO 7/8 (`CONFIG_BSP_I2C_CLK_SPEED_HZ=400000` in sdkconfig):

| Device | 7-bit | 8-bit (esp_codec_dev conv.) | Needed by Stage 1? |
|---|---|---|---|
| ES7210 (mic ADC) | 0x40 | 0x80 | ✓ |
| ES8311 (DAC/speaker) | 0x18 | 0x30 | ✗ |
| CH32V003 (IO expander) | 0x24 | 0x48 | ✓ (for LED P6) |
| GC2145 (camera SCCB) | 0x3C | 0x78 | ✗ |

`bsp_i2c_init()` is idempotent — safe to call from any init path. `bsp_audio_codec_microphone_init()` calls it internally.

---

## 5. CH32V003 IO expander (LED only for Stage 1)

Address 0x24 (7-bit). `bsp_io_expander_init()` must be called before LED toggling. Pin map verified in `esp32_s3_cam_ovxxxx.c:357-367`:

| Pin | Function | Stage-1 use |
|---|---|---|
| P0 | Touch reset | — |
| P1 | LCD backlight PWM | — |
| P2 | LCD reset | — |
| P4 | Touch INT routing | — |
| P6 | **Red LED** — active-LOW | ✓ |
| ADC | Battery voltage | Optional, nice for /metrics |

LED API:
```c
esp_io_expander_handle_t io = bsp_get_io_expander_handle();
esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_6, 0);   // LED ON (active-LOW)
esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_6, 1);   // LED OFF
```

Init requires ~100 ms settle delay (mirrored from `02_esp_sr/main/main.c:55`) — do this once at boot, before any LED calls.

---

## 6. Partition table — current and what Stage 1 needs

`02_esp_sr/partitions.csv`:

```csv
# Name,     Type,  SubType, Offset,    Size,  Flags
nvs,        data,  nvs,     0x9000,    0x6000
factory,    0,     0,       0x10000,   3M
flash_test, data,  fat,     ,          528K
model,      data,  spiffs,  ,          5900K
```

For Stage 1:
- **App size:** YAMNet-1024 weights (~3.2 MB) plus TFLite Micro runtime plus Wi-Fi/HTTP/SNTP/FAT/SPIFFS drivers will push past 3 MB. **Grow `factory` to 5 MB**.
- **Model storage:** YAMNet `.tflite` lives in a SPIFFS partition labeled `yamnet` (not `model`, to leave room if we bolt in ESP-SR later). Size it 4 MB; YAMNet-1024 INT8 fits comfortably, and we reserve headroom for optional AFE models at Stage 2.

Proposed Stage-1 table:

```csv
# Name,     Type,  SubType, Offset,    Size,  Flags
nvs,        data,  nvs,     0x9000,    0x6000
factory,    0,     0,       0x10000,   5M
yamnet,     data,  spiffs,  ,          4M
logs_fat,   data,  fat,     ,          1M
```

Why a dedicated `logs_fat` FAT partition: if the SD card is absent, log to internal flash instead — a small FAT partition is the simplest fallback. 1 MB holds ~5 days of rotated 200 KB logs.

Kconfig:
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

---

## 7. PSRAM

8 MB OPI @ 80 MHz, DMA-capable. sdkconfig settings relevant to us:

- `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`
- `CONFIG_SPIRAM_USE_MALLOC=y` — big allocations default to PSRAM
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` — <1 KB alloc goes to SRAM
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` — LWIP & Wi-Fi buffers prefer PSRAM

Stage-1 heap budget expected:
- YAMNet tensor arena: ~1 MB PSRAM
- I²S DMA ring buffers: 8–16 KB PSRAM
- LWIP + Wi-Fi: ~85 KB SRAM
- esp_http_server + 2 SSE clients: ~20 KB SRAM
- SD FATFS: ~10 KB SRAM
- SNTP client: <1 KB

Total roughly ~130 KB SRAM + ~1 MB PSRAM → comfortable.

---

## 8. Wi-Fi + NTP

Both built into ESP-IDF; no component-manager additions needed.

- Use `protocol_examples_common::example_connect()` for STA connect (as the vendor `01_simple_video_server` example does). Credentials from `CONFIG_EXAMPLE_WIFI_SSID` + `CONFIG_EXAMPLE_WIFI_PASSWORD` in sdkconfig.
- Once IP is up: `sntp_setoperatingmode(SNTP_OPMODE_POLL); sntp_setservername(0, "pool.ntp.org"); sntp_init();` with a sync callback that sets a `g_ntp_synced` flag for the logger to gate timestamps on.

---

## 9. Button (GPIO 15)

Active-LOW, external pull-up on PCB. Use `iot_button` component (`espressif/button: ^3.0.0`) for debouncing + long-press detection. Stage 1 wires a single short-press handler to dump `/metrics` snapshot to console for on-bench debugging without needing the web UI open.

---

## 10. Component manager additions

Extend `02_esp_sr/main/idf_component.yml`:

```yaml
dependencies:
  waveshare/esp32_s3_cam_ovxxxx: ^1.0.0          # BSP — keep
  espressif/esp-tflite-micro: ^1.3.4             # NEW — YAMNet runtime
  espressif/esp-dsp: ^1.5.0                      # NEW — STFT / mel filterbank
  espressif/button: ^3.0.0                       # NEW — user button
  espressif/sdmmc: "*"                           # transitive via BSP, make explicit
```

ESP-SR is **dropped from Stage 1** — we use YAMNet, not WakeNet/AFE. Re-add in Stage 2 if/when AFE is folded in.

---

## 11. Boot sequence (Stage 1)

```
1.  bsp_i2c_init()                                   ~1 ms
2.  bsp_io_expander_init()   + 100 ms settle delay   ~100 ms
3.  LED P6 → ON (boot indicator)                     ~0 ms
4.  bsp_sdcard_mount()       (fail-soft if no card)  ~200 ms
5.  Mount SPIFFS for yamnet                          ~50 ms
6.  Load yamnet.tflite into memory, build interpreter ~200 ms
7.  bsp_audio_codec_microphone_init()                ~20 ms
8.  esp_codec_dev_open(mic, 16 kHz / 1-ch / 16-bit)   ~10 ms
9.  Start audio capture task (core 1)                
10. example_connect() (non-blocking in a task)       seconds
11. SNTP init on Wi-Fi STA_GOT_IP                    seconds
12. HTTP server start                                ~10 ms
13. LED P6 → OFF (boot done, idle)
```

Total perceived boot: ~1 s before audio starts capturing; Wi-Fi/NTP come online in parallel.

---

## 12. What we confirmed can go wrong (logged as risks)

| Risk | Severity | Mitigation |
|---|---|---|
| SD card absent at boot | Low | Fail-soft: log-only-to-console, set `/metrics` flag, optionally fall back to `logs_fat` partition |
| ES7210 PGA clipping at high gain | Medium | Start at 36 dB, log a histogram of input RMS on `/metrics`, let operator tune |
| YAMNet `.tflite` corruption in SPIFFS | Low | SHA-256 check at load; bad model → LED 0.5 Hz blink error pattern |
| Wi-Fi flapping → SNTP drift | Low | Keep last-good synced time; log `NOT_SYNCED` flag alongside each line |
| HTTP SSE client leaks | Low | Cap at 2 concurrent; 30 s idle timeout; log client count on `/metrics` |
| UART0 reuse by SD | None for Stage 1 | Console is on USB-CDC; UART0 pins (43/44) free for SD |
| Combined Wi-Fi + audio DMA PSRAM contention | Low-Medium | Pin Wi-Fi to core 0, audio/inference to core 1 (both cores available on S3) |

---

## Sources

- Vendor BSP header: `ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/02_esp_sr/managed_components/waveshare__esp32_s3_cam_ovxxxx/include/bsp/esp32_s3_cam_ovxxxx.h`
- Vendor BSP impl: same dir, `esp32_s3_cam_ovxxxx.c`
- Reference audio pipeline: `02_esp_sr/main/speech_det_driver/mic_speech.c`
- Partition/kconfig: `02_esp_sr/partitions.csv`, `02_esp_sr/sdkconfig`
- C6 SD/display sharing post-mortem: `<sibling-C6-repo>/18-sd-display-spi-sharing.md`
- Hardware specs: `./.claude/commands/hardware-specs.md`
