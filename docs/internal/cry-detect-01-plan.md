# cry-detect-01 — Implementation plan

Stage-1 deliverable per `docs/research/cry-detect-starter-plan.md` + `docs/research/hw-verification-stage1.md`. Project lives at `projects/cry-detect-01/`.

**Scope**: pretrained YAMNet-1024 INT8 → on-device class-20 (`Baby cry, infant cry`) threshold → LED + web UI + SD + NTP-stamped logs. No fine-tuning. No vision. No AFE.

**Out of scope (deferred)**: AFE preprocessing (Stage 2), vision verification (Stage 3), linear-head fine-tune (Stage 4).

---

## 1. Data flow

```
  ES7210 ADC
       │  I²S DMA, 16 kHz / 16-bit / mono
       ▼
  capture_task  (core 1)  ──► ring buffer (2 s of audio)
                                        │
                                        ▼
                                  feature_task  (core 1)
                                        │  Hanning + 512-pt FFT
                                        │  40-mel? → YAMNet expects 64 mel, 96 frames
                                        │  log-mel patch (96×64 INT8)
                                        ▼
                                  yamnet_task  (core 1)
                                        │  TFLite Micro invoke
                                        │  read output_tensor[20]
                                        ▼
                                  detector (hysteresis)
                                        │  N consecutive frames over threshold
                                        ▼
                                  metrics store  (atomic)
                                   │      │       │
                              ┌────┘      │       └───┐
                              ▼           ▼           ▼
                           LED P6    sd_logger   web_ui (SSE)
                          (active-   (NTP-       (/events,
                           LOW)       stamped)    /metrics, /)

  Wi-Fi STA + SNTP  (core 0)  ─► sets g_ntp_synced; provides wall-clock
```

Threads:
- Core 0: Wi-Fi / LWIP / `esp_http_server` / SNTP
- Core 1: audio DMA reader, mel extractor, YAMNet inference

Inter-thread: FreeRTOS stream buffer for PCM; atomic-snapshot metrics struct for everything else.

---

## 2. Module layout

```
projects/cry-detect-01/
├── CMakeLists.txt                  # project top-level
├── partitions.csv                  # custom 16 MB layout
├── sdkconfig.defaults              # baseline + placeholder creds
├── sdkconfig.defaults.template     # committed template, real creds in sdkconfig.defaults (gitignored)
├── README.md                       # build / flash / model prep
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml           # esp-tflite-micro, esp-dsp, button, BSP
│   ├── Kconfig.projbuild           # CRY_* config symbols (threshold, gain, GPIO, etc.)
│   ├── main.c                      # app_main, boot sequence, task startup
│   ├── audio_capture.{c,h}         # ES7210 init, I²S read loop → stream buffer
│   ├── mel_features.{c,h}          # STFT + mel filterbank, INT8 quantize
│   ├── yamnet.{c,h}                # TFLite Micro interpreter, invoke, read class 20
│   ├── detector.{c,h}              # hysteresis + temporal smoothing
│   ├── led_alert.{c,h}             # CH32V003 P6 states (idle/connecting/syncing/alert/error)
│   ├── network.{c,h}               # Wi-Fi STA + SNTP + got-ip event
│   ├── sd_logger.{c,h}             # mount SD, rotating log file, timestamped append
│   ├── metrics.{c,h}               # shared counters/gauges, atomic snapshot for UI
│   ├── web_ui.{c,h}                # HTTP server, /, /metrics, /events (SSE), /log/tail
│   └── timing.h                    # esp_timer-based perf helpers (macros)
├── www/
│   ├── index.html                  # embedded via EMBED_TXTFILES
│   └── app.js                      # SSE client, charts, poll
├── spiffs/
│   └── PLACE_YAMNET_HERE.md        # instructions; real yamnet.tflite fetched by script
└── tools/
    └── fetch_model.sh              # download YAMNet INT8 from HuggingFace into spiffs/
```

---

## 3. Partition table

```csv
nvs,        data, nvs,     0x9000,  0x6000
factory,    0,    0,       0x10000, 5M
yamnet,     data, spiffs,  ,        4M
logs_fat,   data, fat,     ,        1M
```

- `factory` 5 MB: app + TFLite Micro + Wi-Fi + HTTP + FATFS + SPIFFS drivers
- `yamnet` 4 MB SPIFFS: holds `yamnet.tflite` (~3.2 MB INT8) + headroom for a future model
- `logs_fat` 1 MB FAT: fallback when SD card is absent; ~5 days of rotated 200 KB logs

Requires Kconfig: `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`, `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`.

---

## 4. Kconfig (`main/Kconfig.projbuild`) summary

| Symbol | Default | Purpose |
|---|---|---|
| `CRY_DETECT_THRESHOLD` | 40 (INT8; ≈ 0.31 logit) | Minimum class-20 INT8 score to count as "loud enough" |
| `CRY_DETECT_CONSEC_FRAMES` | 3 | Frames over threshold before alerting (smoothing) |
| `CRY_DETECT_HOLD_MS` | 5000 | LED hold time after alert |
| `CRY_DETECT_MIC_GAIN_DB` | 36 | ES7210 analog PGA gain |
| `CRY_DETECT_SAMPLE_RATE` | 16000 | I²S sample rate |
| `CRY_DETECT_MODEL_PATH` | `/yamnet/yamnet.tflite` | SPIFFS path |
| `CRY_DETECT_TENSOR_ARENA_KB` | 1024 | TFLite Micro arena size |
| `CRY_DETECT_WEB_UI_ENABLED` | y | Turn the HTTP server on/off |
| `CRY_DETECT_SSE_MAX_CLIENTS` | 2 | Cap for SSE connections |
| `CRY_DETECT_LOG_ROTATE_KB` | 256 | Rotate log every N KB |
| `CRY_DETECT_SD_ENABLED` | y | Log to SD (fallback to internal FAT if false or absent) |

---

## 5. Audio → mel → YAMNet parameters

From YAMNet model card (TFHub) + STMicro repack:
- Sample rate: **16 000 Hz**
- STFT window: **25 ms** = 400 samples; **hop 10 ms** = 160 samples
- FFT size: **512** (next pow2 ≥ 400), Hanning window
- Mel bands: **64**; 125 Hz – 7500 Hz
- Patch: **96 frames × 64 mels** (≈ 0.975 s of audio; hops produce frames at 100 fps)
- Input quantization: INT8, zero-point + scale from `.tflite` metadata
- Output: (quantized) 521-class logits; `output[20]` = `Baby cry, infant cry`

Exact STFT/mel math lives in `mel_features.c` using ESP-DSP's `dsps_fft2r_fc32` for FFT and a precomputed Hanning table (512 floats, 2 KB in flash). Mel filterbank coefficients: precomputed at startup into a PSRAM-resident `float mel_weights[64][257]` (≈ 64 KB), sparse-multiply style (store only non-zero weights per band).

Frame cadence: compute one mel column every 10 ms → fill a circular 96-column ring buffer → once per 480 ms (48 new frames), copy the ring into a 96×64 patch and enqueue for YAMNet. This gives ~2 inferences/s — matches YAMNet's intended operating point and leaves plenty of CPU.

---

## 6. YAMNet integration

### Runtime: TFLite Micro (`espressif/esp-tflite-micro`)
- Load `.tflite` from `/yamnet/yamnet.tflite` (SPIFFS mount point `/yamnet`) into a PSRAM buffer.
- Build interpreter with a micro-mutable-op-resolver listing:
  - `CONV_2D`, `DEPTHWISE_CONV_2D`, `FULLY_CONNECTED`
  - `AVERAGE_POOL_2D`, `MAX_POOL_2D`
  - `RESHAPE`, `QUANTIZE`, `DEQUANTIZE`
  - `SOFTMAX`, `LOGISTIC` (if used post-classifier)
  - `MEAN` (global pool if present)
- Tensor arena: 1 MB PSRAM (over-provisioned; will trim after first successful `AllocateTensors`).

### Model expectation
- Input: `[1, 96, 64, 1]` INT8.
- Output: `[1, 521]` INT8 (logits); class index 20 is `Baby cry, infant cry`.
- If STMicro's repack returns embeddings instead of logits, the `yamnet.c` module detects at init (checks output tensor shape) and sets `g_yamnet_has_classifier = false`. Stage-1 hard-fails with a clear error pointing at the starter-plan note — the user has to fetch the right variant.

### Latency budget
- Published CPU-ONNX YAMNet-1024: ~38 ms at 640×640 equivalent; audio YAMNet quoted ~200 ms on low-end ARM.
- S3 estimate via TFLite Micro: **150–300 ms per invoke**. At ~2 invokes/s this keeps core 1 around 30–60 % loaded.
- Real number measured via `timing.h` macros; reported live in `/metrics`.

---

## 7. Web UI

### Backend (`web_ui.c`)

- `esp_http_server` with `HTTPD_DEFAULT_CONFIG()`, `max_uri_handlers = 8`, `stack_size = 6144`.
- URIs:
  - `GET /` → serve embedded `www/index.html` (gzip'd at compile via CMake `EMBED_TXTFILES` + gzip).
  - `GET /app.js` → serve embedded `www/app.js`.
  - `GET /metrics` → JSON snapshot.
  - `GET /events` → SSE stream. Push on every inference + every detection. Heartbeat every 15 s.
  - `GET /log/tail?n=50` → last N lines from the in-RAM ring buffer (not SD — SD is the durable sink; RAM ring is for the UI).
  - `GET /healthz` → plain `OK`.

### SSE implementation note

`esp_http_server` doesn't ship an SSE helper. We hand-write it: set `Content-Type: text/event-stream`, disable chunked encoding via `httpd_resp_set_type` + `httpd_resp_send_chunk` with `data: {JSON}\n\n` framing. One handler task per client keeps the socket alive; a global `event_queue` FreeRTOS queue pushes events to all clients (fan-out via per-client queues).

Cap clients at `CRY_DETECT_SSE_MAX_CLIENTS` (default 2).

### Frontend (`www/index.html` + `www/app.js`)

Single-file dashboard. No framework. Shows:
- Big status card: Idle / Listening / **CRYING** — color-coded. Follows `/events` stream.
- Metrics strip: inference FPS, last latency (ms), heap / PSRAM free, Wi-Fi RSSI, SD state, NTP-synced flag, uptime.
- Rolling line chart of class-20 confidence (last 60 s).
- Detection log: last 20 events with timestamps.

Pure CSS, no external CDNs (offline-usable). Total payload target < 20 KB gzipped.

---

## 8. SD logging

- `bsp_sdcard_mount()` at boot; on failure, fall back to `logs_fat` partition mounted at `/logs`.
- Rotating file: `/sdcard/cry-YYYYMMDD.log` (daily), or `/sdcard/cry-NNNN.log` (sequence) if NTP not yet synced.
- Format: CSV-ish — `ISO8601,event,cry_conf,latency_ms,free_heap,rssi`.
- Flush every 10 events or 5 s, whichever first. Close + reopen on rotate.
- Duplicate log lines into an in-RAM ring buffer (8 KB, ~80 lines) for `GET /log/tail`.

---

## 9. LED states (CH32V003 P6, active-LOW)

| State | Pattern | Meaning |
|---|---|---|
| Boot | solid ON | Boot in progress |
| Idle | OFF | Running, nothing detected |
| Connecting | blink 1 Hz | Wi-Fi STA connecting |
| Syncing | blink 4 Hz | NTP sync in progress |
| Alert | solid ON for `CRY_DETECT_HOLD_MS` | Cry detected |
| Error | blink 0.5 Hz | Model load failed / unrecoverable |

Driven from a single state machine in `led_alert.c`, ticked on a 100 ms timer.

---

## 10. Boot sequence (`main.c`)

```
1. esp_log level = INFO globally, DEBUG on "cry"
2. NVS init (idempotent)
3. bsp_i2c_init
4. bsp_io_expander_init; vTaskDelay(100 ms); LED solid ON
5. led_alert_start_task (100 ms timer)
6. sd_logger_init (soft fail → logs_fat partition)
7. mount SPIFFS ("/yamnet" label "yamnet")
8. yamnet_init (load .tflite, build interpreter, pre-allocate tensors)
9. mel_features_init (precompute Hanning window + mel filterbank)
10. audio_capture_init (ES7210 + I²S, 16 kHz mono 16-bit)
11. metrics_init
12. start feature_task and yamnet_task on core 1
13. start capture_task on core 1 (begins streaming)
14. network_start (STA + SNTP; non-blocking, event-driven)
15. if CRY_DETECT_WEB_UI_ENABLED: web_ui_start on WIFI_EVENT_STA_GOT_IP
16. LED → idle (OFF)
```

Expected wall-clock: ~1 s from reset to audio flowing. Wi-Fi + NTP in parallel (seconds).

---

## 11. RAM budget (estimates)

| Item | SRAM | PSRAM |
|---|---|---|
| FreeRTOS + drivers | 50 KB | — |
| LWIP + Wi-Fi | 85 KB | 50 KB (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) |
| esp_http_server + 2 SSE | 20 KB | — |
| SPIFFS + FATFS | 10 KB | — |
| TFLite Micro interpreter core | 5 KB | — |
| YAMNet tensor arena | — | 1 024 KB |
| YAMNet model weights (mmap) | — | 0 (read via esp_partition) or 3 200 KB if loaded to heap |
| Audio stream buffer (2 s @ 16 kHz × 16 bit) | — | 64 KB |
| Mel ring (96×64 float for debug, INT8 in prod) | — | 64 KB (float debug) / 6 KB (INT8) |
| Log ring | 8 KB | — |
| Metrics + misc | 4 KB | — |
| **Total** | **~182 KB** of 316 KB | **~1.2 MB** of 7.5 MB |

Comfortable on both fronts.

---

## 12. Dependencies (`main/idf_component.yml`)

```yaml
dependencies:
  idf: ">=5.3"
  waveshare/esp32_s3_cam_ovxxxx: "^1.0.0"
  espressif/esp-tflite-micro: "^1.3.4"
  espressif/esp-dsp: "^1.5.0"
  espressif/button: "^3.0.0"
```

ESP-SR is *not* listed — deferred to Stage 2.

---

## 13. Model-prep workflow (user-side, one-time)

`tools/fetch_model.sh` downloads `yamnet_1024_int8.tflite` from `STMicroelectronics/yamnet` on HuggingFace and writes it to `spiffs/yamnet.tflite`. SPIFFS image is built at `idf.py build` via `spiffs_create_partition_image(yamnet spiffs FLASH_IN_PROJECT)`.

Flow:
1. `cd projects/cry-detect-01`
2. `./tools/fetch_model.sh`
3. `cp sdkconfig.defaults.template sdkconfig.defaults && edit creds`
4. `idf.py -C . -B build build`
5. `idf.py -C . -B build -p /dev/cu.usbmodem3101 flash`
6. `/monitor` or open `http://<device-ip>/`

---

## 14. Testing / validation plan

- **Smoke**: boot, see LED sequence ON → connect → sync → idle.
- **Offline**: unplug Wi-Fi → LED should stay idle (no sync blink forever); SD should still log; pressing user button should dump metrics to console.
- **Inference sanity**: play an ESC-50 `crying_baby` clip through a phone speaker next to the board; expect LED alert + SSE push; cry_conf > threshold.
- **False-positive sweep**: play UrbanSound8K clips (dog_bark, siren, street_music, vacuum_cleaner) → should not alert.
- **Latency**: `/metrics` inference_ms p50/p95 → target p95 < 350 ms.
- **Long-run**: 1 h of mixed household audio → count alerts; manually annotate; compute rough precision. Expect 85–90 % per starter plan.

---

## 15. Known risks carried from earlier docs

- **YAMNet STMicro `.tflite` variant uncertainty** — classifier-head vs embedding-only. Fail-fast at init if embedding-only; README directs user to the correct variant.
- **TFLite Micro op coverage for YAMNet layers** — if any op is missing in the default op resolver, interpreter fails at `AllocateTensors`. Error path logs the missing op name.
- **Mel-bank exactness** — YAMNet expects HTK mel, 125–7500 Hz, specific scale. Off-by-one mel bin is the classic bug. `mel_features.c` mirrors TensorFlow's `audio_ops.audio_spectrogram` + `mfcc` implementation to match.
- **ES7210 4-ch vs mono-slot** — BSP configures mono slot; we treat ES7210 as mono by using MIC1 only. Confirmed by `mic_speech.c:173-192` sequencing.
- **PSRAM + Wi-Fi contention** — keep audio DMA small (10 ms frames = 320 bytes); Wi-Fi buffers are already PSRAM. If latency spikes, pin Wi-Fi to core 0 explicitly.

---

## 16. Out-of-scope for Stage 1, revisit later

- AFE (AEC/BSS/NS/VADNet) replacing energy VAD.
- On-device fine-tune / linear head.
- HTTPS / auth on the web UI.
- Firmware OTA.
- Battery voltage reporting (ADC read via CH32V003 is a one-line add if needed).
- **Stage 3 vision (face detect)** — **scrapped 2026-04-17.** Use case is a discreet bedroom monitor — baby is in the dark, not looking at the camera. A vision gate buys nothing.

---

## 17. Stage 2 (added 2026-04-17) — onboard-hardware UX

Built on top of Stage 1; three additive modules, each off-by-default-at-boot where appropriate.

### 17.1 Live audio stream (`audio_stream.{c,h}`)
- Raw 16 kHz mono 16-bit L16 PCM over chunked HTTP at `GET /audio.pcm`.
- Multi-consumer tap registered on `audio_capture` (new `audio_tap_handle_t` API).
- Cap: `CONFIG_CRY_STREAM_MAX_LISTENERS` (default 2), per-listener ring `CONFIG_CRY_STREAM_RING_KB` (default 32 KB).
- Browser: Web Audio API + `AudioWorklet` PCM player (vanilla, no libs).
- **Privacy**: streaming **off at boot**; a browser must fetch `/audio.pcm` to enable. LED switches to slow breathing pattern whenever ≥1 listener is active — physical indicator nobody can miss.
- Easy toggle: single **🔊 Listen** button in the web UI; click to start/stop.

### 17.2 Event recorder (`event_recorder.{c,h}`)
- 10 s pre-roll ring (in PSRAM, 320 KB) + 30 s post-trigger WAV write on each detection.
- Files written to `/sdcard/events/cry-YYYYMMDDTHHMMSSZ.wav` (falls back to `/logs/events/...` on the internal FAT partition if SD absent).
- Web UI log gets a **▶ play** link per detection → `GET /recordings/<filename>`.
- Self-pruning: keeps last `CONFIG_CRY_REC_KEEP_FILES` (default 50) newest WAVs; oldest auto-deleted.
- Rationale: kills the "do I trust the model?" problem — user just listens to what fired the alert.

### 17.3 Adaptive noise floor (`noise_floor.{c,h}`)
- Rolling 32-bin histogram over log₂(RMS). p50/p95 readable at any time.
- After `CONFIG_CRY_NOISE_FLOOR_WARMUP_S` (default 5 min), threshold = base + margin × f(p95) where f maps p95 onto [0, margin].
- Surfaced in `/metrics` as `noise_floor_p50`, `noise_floor_p95`, `noise_floor_warm`, `noise_floor_remaining_s`.
- Detector gets `detector_set_threshold()` so threshold can change per-inference without recreating state.

### 17.4 Updated RAM budget

Added in Stage 2:
- Pre-roll ring: +320 KB PSRAM
- Per-listener stream ring × 2: +64 KB PSRAM
- Recorder chunk buf: +8 KB PSRAM
- Noise-floor state: +1 KB SRAM

Running total: ~185 KB SRAM (of 316), ~1.6 MB PSRAM (of 7.5). Still plenty of headroom.

### 17.5 LED state additions
- `LED_STATE_STREAMING`: slow breathing (~0.4 Hz software PWM), set when `audio_stream_listener_count() > 0` and the device is otherwise idle. Overrides `LED_STATE_IDLE` in idle-state branches of the state callback.

### 17.6 New web endpoints
- `GET /audio.pcm` — live L16 stream
- `GET /recordings/<file>.wav` — serve event WAV
- `GET /events/list` — JSON list of recorded WAVs

---

## Change log

- **2026-04-17 v0** — initial plan, derived from `cry-detect-starter-plan.md` + `hw-verification-stage1.md`.
- **2026-04-17 v1** — scrapped vision (Stage 3); added Stage 2 = live audio stream + event recorder + adaptive noise floor (§17). Driven by user feedback: bedroom-monitor, discreet, baby not camera-facing.
