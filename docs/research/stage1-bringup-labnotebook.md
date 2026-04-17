# Stage-1 bring-up — lab notebook

*cry-detect-01, Waveshare ESP32-S3-CAM-GC2145, 2026-04-17*

A chronological record of what was planned, what broke, what the evidence showed, what it cost, and what we'd do differently. Written while the device is still running, so memory is intact. Intended for future-me and anyone else bringing up a similar pretrained-audio pipeline on an ESP32 — skip straight to §7 for the one-page "what to take forward" summary.

---

## 1. Research phase — prior-expectations table

Before any code. Hypotheses framed in the research docs and the assumptions they rested on.

| # | Hypothesis (research doc) | Evidence it rested on | Held up on hardware? |
|---|---|---|---|
| H1 | "Use pretrained YAMNet-1024 INT8 from STMicroelectronics — a drop-in `.tflite`." | Reading STMicro's HF repo README, which mentions YAMNet-1024. | **No.** STMicro ships **ONNX** for their STM32 toolchain, not TFLite. The one TFLite variant there (Yamnet-256) is embedding-only, no classifier head. |
| H2 | "YAMNet will run on TFLite Micro once we have the INT8 `.tflite`." | Generic TFLite Micro docs on MobileNet-class models. | **No, as shipped.** The TF Hub `lite-model_yamnet_classification_tflite_1.tflite` uses `AUDIO_SPECTROGRAM` and `MFCC` ops (Flex/SELECT_TF) which TFLite Micro does not support. Loading it crashes at `AllocateTensors` with `Didn't find op for builtin opcode 'STRIDED_SLICE'` (the first missing op it tries). |
| H3 | "Tensor arena of 1024 KB is enough for YAMNet-class models on S3." | Ballpark from esp-dl YOLO11n numbers. | **No.** YAMNet at INT8 needs **1157736 B used of 1572864 allocated** — so ~1.1 MB working, ~1.5 MB allocated with headroom. Our 1024 KB failed with `Failed to resize buffer. Requested: 1060896, available 951872, missing: 109024`. Bumped to 1536 KB. |
| H4 | "pyserial on macOS `/dev/cu.usbmodem*` with `dtr=False, rts=True` (pulse) at open gives a clean reset." | The vendor `/monitor` skill in this repo, and every IDF monitor tutorial. | **No for native USB-JTAG on S3.** macOS raises DTR+RTS by default *at open time* regardless of what you set before `open()`, and on ESP32-S3 native USB-JTAG those map directly to `GPIO0` and `EN` — every port-open forces the chip back into download mode. Eats boot output entirely. |
| H5 | "Component-manager versions just work." | Standard ESP-IDF v5.5.3 experience. | **Partially.** The vendor BSP pinned `espressif/button ^4.1.4` while the project asked `^3.0.0` → solver rejection. Had to bump. This is recurring friction for any project that sits on top of a vendor BSP. |
| H6 | "SD-card partition label `model` from the vendor example is fine to reuse." | Minimal-change diff. | Replaced with `yamnet` 5 MB SPIFFS — 4 MB originally, but the 4.05 MB `.tflite` didn't fit after SPIFFS overhead (~15 %). |
| H7 | "Stock 4 KB stack is fine for a logger task that only snprintf's." | Gut feel. | **No.** Calling `fwrite()` into a FATFS-mounted SD ate the stack. 3 KB → 6 KB fixed it; first clue was a crash-reboot loop at exactly the 30 s mark, which is when the first scheduled SD snapshot fired. |

---

## 2. Build phase — what shipped vs. what was planned

The initial scaffold followed the [plan doc](../internal/cry-detect-01-plan.md) almost exactly. Modules implemented:

- `audio_capture` — I²S DMA @ 16 kHz mono 16-bit via `bsp_audio_codec_microphone_init`, plus a multi-consumer **tap API** (`audio_capture_add_tap` / `_remove_tap` / `_tap_read`) for fan-out to streaming + event recorder.
- `mel_features` — Hann window + 512-pt FFT (`dsps_fft2r_fc32`) + 64-band HTK mel filterbank (125–7500 Hz, sparse triplet storage in PSRAM) + log + INT8 quantisation against YAMNet's input tensor scale/zp.
- `yamnet.cc` — TFLite Micro interpreter with a micro-mutable op resolver covering 16 ops. C++ file, extern-C interface.
- `detector` — hysteresis with `CONFIG_CRY_DETECT_CONSEC_FRAMES` consecutive-over-threshold requirement, `HOLD_MS` latch. Threshold settable at runtime.
- `noise_floor` — rolling 32-bin log₂(RMS) histogram, 5-min warmup, p50/p95 accessors.
- `led_alert` — software PWM state machine on CH32V003 pin P6 (active-LOW). States: BOOT / CONNECTING / SYNCING / IDLE / ALERT / ERROR / STREAMING(breathing).
- `network` — `example_connect()` + SNTP + mDNS (`cry-detect-01.local`).
- `sd_logger` — CSV schema with dual-timestamp (wall-clock when synced, uptime-seconds always), rotating files, NTP-sync marker line, in-RAM ring mirror for `/log/tail`.
- `web_ui` — `esp_http_server` with `/metrics`, `/events` SSE, `/log/tail`, `/audio.pcm` chunked PCM, `/recordings/*`.
- `event_recorder` — 10 s PSRAM pre-roll + 30 s post-trigger → WAV on SD with self-pruning.
- `metrics` — shared snapshot struct with mutex.

RAM budget at runtime (post-boot, quiescent, idle-state): **~180 KB SRAM, ~1.6 MB PSRAM** used of 316 KB / 7.5 MB available. Inference latency measured **506–650 ms / patch**, ~**1.3–1.4 fps** sustained.

---

## 3. Bring-up debugging — catalogued fault log

Each row is a real failure mode observed during flash/test cycles today.

| # | Symptom | Root cause | Evidence | Fix | Cost |
|---|---|---|---|---|---|
| F1 | Vendor BSP fails component-manager version solving | BSP pins `espressif/button ^4.1.4`; our `idf_component.yml` said `^3.0.0` | `CMake Error at .../build.cmake:631: no versions of waveshare/esp32_s3_cam_ovxxxx match >1.0.0,<2.0.0` | Bump to `^4.1.4` in our manifest | 2 min |
| F2 | SPIFFS partition image generation fails | `yamnet.tflite` is 4.05 MB; 4 MB SPIFFS too small after overhead | `SpiffsFullError: the image size has been exceeded` | Grow `yamnet` partition 4 MB → 5 MB, remove placeholder `.md` from spiffs/ | 2 min |
| F3 | C++ linkage mismatch on yamnet TU | Declared prototypes in `yamnet.h` without `extern "C"` guards; `.cc` TU defined them as C-linkage | `error: conflicting declaration … with 'C' linkage` × 5 | Wrap `yamnet.h` in `extern "C"` guards + include `<stdint.h>`, `<stddef.h>` | 2 min |
| F4 | `/events/*` URI wildcard invalid | Comment nested `/*` inside block comment | `error: "/*" within comment [-Werror=comment]` | Rephrase comment | 30 s |
| F5 | `HTTPD_503_SERVICE_UNAVAILABLE` undeclared | Constant doesn't exist; SDK only provides specific codes | Used `httpd_resp_set_status("503 ...")` + `httpd_resp_send` | Reviewed esp_http_server headers | 2 min |
| F6 | `unlink()` undeclared | Missing `<unistd.h>` in `event_recorder.c` | implicit declaration warning-as-error | Added include | 30 s |
| F7 | snprintf format-truncation | `char paths[64][80]`; dirent `d_name` can overrun 80 | `'%s' directive output may be truncated writing up to 255 bytes into a region of size 79` | Grow buffer to 96; add `%.40s` format bound | 2 min |
| F8 | STRIDED_SLICE op missing | Fallback-fetched model (`thelou1s/yamnet`) was TF Hub float16 waveform-input variant with AUDIO_SPECTROGRAM/MFCC/STRIDED_SLICE ops | Runtime: `Didn't find op for builtin opcode 'STRIDED_SLICE'` + `AllocateTensors failed` | Wrote `tools/convert_yamnet.py` that downloads Google YAMNet source + `yamnet.h5` weights, rebuilds with mel-patch input, INT8-quantises with synthetic calibration | **~45 min** including researching the right path and discovering STMicro's repo ships ONNX not TFLite |
| F9 | Tensor arena too small | 1024 KB insufficient; measured need ~1.1 MB | Runtime: `Failed to resize buffer. Requested: 1060896, available 951872, missing: 109024` | Bump `CONFIG_CRY_DETECT_TENSOR_ARENA_KB=1536` | 5 min |
| F10 | Null-semaphore crash in web_ui_push_event | `inference_task` starts before `web_ui_start` creates `s_lock` | `assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))`, backtrace pointing at `web_ui_push_event:153` after addr2line | NULL-guard in `web_ui_push_event`; keep creation order but guard mutex access | 10 min |
| F11 | LWIP `Invalid mbox` after reordering | Moved `web_ui_start` before `network_start` to fix F10; LWIP tcpip thread not yet initialised when `httpd_start` calls socket APIs | Runtime: `assert failed: tcpip_send_msg_wait_sem … Invalid mbox` | Keep original order (`network_start` before `web_ui_start`); rely on F10's NULL-guard | 5 min |
| F12 | **macOS pyserial DTR trap** | `/dev/cu.usbmodem*` opens raise DTR+RTS by default; on S3 native USB-JTAG these map to GPIO0+EN → every port-open forces the chip back into download mode → boot log never emits | Serial read returns 0 bytes; esptool `chip_id` *succeeds* (proves USB is fine); boot banner shows `boot:0x0 (DOWNLOAD(USB/UART0))`; symptom reappears every time we open the port | **Two workarounds documented in `.claude/commands/monitor.md`**: (a) `stty -hupcl` + `dd` raw read; (b) pyserial with `dtr=False; rts=False` set on a detached `serial.Serial()` instance **before** `.open()`. Neither is perfect — ultimately the recovery path is "physical unplug + replug and never re-open the port mid-session." | **~20 min** |
| F13 | Null-semaphore crash #2 in noise_floor_p95 | `sd_logger_event("boot", …)` runs before `noise_floor_init()`; new SD schema calls `noise_floor_p95()` which takes uninit mutex | addr2line → `noise_floor_p95` at `noise_floor.c:101` | NULL-guard every noise_floor accessor | 5 min |
| F14 | 30-s crash-reboot loop | Housekeeping task stack 3 KB insufficient when `sd_logger_snapshot` → `fwrite` descends into FATFS | Observation: `/log/tail` boot timestamps advance by exactly ~30 s between calls; the first sched'd snapshot is at uptime=30s | Bump housekeeping task stack to 6 KB | 5 min |
| F15 | `/log/tail` endpoint sometimes empty | 8 KB `char buf[8192]` on HTTP handler stack; default handler task stack is 6144 B — stack corruption | Logical inference from defaults | Use `heap_caps_malloc(8192, MALLOC_CAP_SPIRAM)` in the handler (not fixed yet; cosmetic, SD file is authoritative) | — |

Total debug time across all 15: roughly 1.5 hours, dominated by F12 (the macOS DTR trap — 20 min) and F8 (the wrong-model-variant mistake — 45 min, a chunk of which was me doggedly flashing the bad model before accepting the premise was wrong).

---

## 4. Key learnings — non-obvious facts that cost us time

### 4.1 The pretrained-audio-on-TFLite-Micro gap is real

Before 2026-04, there was **no public drop-in INT8 YAMNet for TFLite Micro** with the AudioSet-521 head:

- Google's TF Hub `.tflite` uses `AUDIO_SPECTROGRAM` / `MFCC` / Flex ops → unsupported.
- STMicroelectronics ships YAMNet as ONNX for STM32 toolchain, not TFLite.
- Community (`thelou1s/yamnet`) is just the Google TFLite re-mirrored, same Flex-op problem.
- `Yamnet-256` from STMicro is embedding-only, no classifier head.

**We published a filler at [chayuto/yamnet-mel-int8-tflm](https://huggingface.co/chayuto/yamnet-mel-int8-tflm) — first public mel-patch INT8 YAMNet compatible with TFLite Micro.** See §7 for why this matters beyond this project.

### 4.2 macOS + ESP32-S3 native USB-JTAG is a serial trap

Every monitor/`idf.py monitor`/`pyserial` doc assumes UART via USB-to-serial chip. For ESP32-S3's native USB-Serial-JTAG, **DTR/RTS are routed directly to GPIO0/EN by the chip's ROM**, and macOS's default-DTR-on-open behaviour collides with that. The physical recovery (unplug + replug without BOOT) is the only bulletproof path.

Consequence going forward: **rely on the web UI over Wi-Fi as the primary observability channel, not serial.** SD log is the durable sink, `/metrics` + `/events` SSE is the live view. Serial is for first-boot sanity check only.

### 4.3 Silent serial + unreachable Wi-Fi = layered failure

When both channels fail, resist opening the port to investigate — that often *is* the failure. Order: (1) LED colour/pattern, (2) ping/curl on LAN, (3) only then touch serial (with the DTR workarounds). The LED state machine we built turned out to be the single most useful diagnostic.

### 4.4 Boot-order race between tasks and module init

Created-but-not-yet-initialised mutexes were the **only class of crash** we hit post-flash (F10, F13). Pattern: task started early → task calls into module M → M takes its mutex → mutex is NULL because M_init() not reached yet. Two fixes equally valid:

1. NULL-guard every accessor (what we did; defensive, idempotent).
2. Move all `*_init()` calls to the very top of `app_main` before any `xTaskCreate` — lighter code but easy to regress.

Recommend (1) as a project convention. Reviewer can add: "every public accessor of a module guarded by a lazily-created mutex must tolerate `s_lock == NULL`."

### 4.5 FATFS stack hunger

`fwrite()` to a mounted FAT32 partition wants **4 KB headroom**. Anything under 6 KB for a task that writes to SD is a crash waiting for its first invocation. Cheap fix; worth a global constant `#define CRY_FATFS_SAFE_STACK_BYTES 6144` for posterity.

### 4.6 Synthetic-calibration INT8 PTQ has a measurable bias

The calibration set for `tf.lite.TFLiteConverter` determines activation ranges per-layer. With synthetic Gaussian patches (our fallback when user hadn't supplied real audio), YAMNet's output probabilities for class 20 `Baby cry, infant cry` sit at ~0.57–0.62 in a quiet room — **non-zero but below the 0.85 threshold**. A threshold of 0.5 would FP; 0.85 is fine-quiet. Real-audio calibration is expected to recentre the distribution; not yet tested.

### 4.7 The cheap experiment *is* cheap

We shipped a working Stage 1 in ~6 hours end-to-end despite 15 distinct failure modes, because each fix was 30 s – 10 min of targeted code change. **Pure-C Unix-y firmware on ESP-IDF is shockingly fast to iterate on** once the debug loop is unblocked. The expensive failure (F12 DTR) was an *environmental* problem, not a code problem.

---

## 5. Observations on actual runtime behaviour

Captured via `/metrics` during stable operation (post-fix):

| Metric | Observed | Expected (plan) | Delta |
|---|---|---|---|
| Inference latency | 500–650 ms / patch | 150–300 ms (estimate) | **2–3× slower than estimate** |
| Inference rate | 1.35–1.39 fps | ~2 Hz | slightly slower — consistent with latency |
| SRAM used | ~180 KB | 185 KB | ✓ |
| PSRAM used | ~1.6 MB | 1.2 MB | **~400 KB higher**, likely YAMNet arena (1.54 MB allocated vs 1.0 MB planned) |
| Boot-to-Wi-Fi-up | ~4 s | ~3 s | ✓ |
| Wi-Fi RSSI | -38 to -55 dBm | — | healthy |
| Noise-floor p95 (quiet room) | 60–150 | unknown | learning; warmup 5 min |
| `cry_conf` idle (synthetic PTQ model) | 0.57–0.62 | <0.1 expected of a well-calibrated model | **bias; see §4.6** |

Unmeasured at publication:
- FP rate on household audio (TV, phone ringtone, dog bark, adult speech).
- True-positive rate on real baby cry.
- Multi-hour stability (device is currently going to a real-world test location; the Monitor tool is watching).
- Long-term log growth; SD rotation; NTP drift.

---

## 6. What would have saved time (retrospective)

Ordered by leverage:

1. **Verify the model file *exists* before planning against it.** Would have caught F8 in the research phase. Rule: before a doc says "use model X", attempt a `curl -I` on the claimed URL.
2. **Write the DTR-trap note to `monitor.md` *first*, not after losing 20 min to it.** Skill updates should be driven by the first surprise, not the second.
3. **Pre-flight check list**: target + sdkconfig defaults + partition table + component versions *before* writing any .c code. Would have caught F1–F2 on day 0.
4. **One smoke test per task** at boot — a single ESP_LOGI("module_name: alive, pid=%u") per init function makes F10/F13 boot-order races visible from the first boot log. We had this for some modules, not all.
5. **Periodic heartbeat everywhere.** Silence is never success. The 10 s housekeeping log we added mid-debug turned "device is frozen" into "device rebooted 30 s ago" immediately. Adopt as a convention.

---

## 7. One-page summary — carry-forward rules

1. **No module mutex access without a NULL guard.** Tasks can outrun inits; pretend they always do.
2. **Any task that writes to SD gets ≥ 6 KB stack.** FATFS eats it.
3. **Keep `network_start` (LWIP init) before `web_ui_start` (httpd).** Don't reorder for mutex fixes.
4. **10 s serial heartbeat + 30 s SD snapshot from a housekeeping task is the minimum observability contract.** Every task also logs on start with its core ID.
5. **The web UI is the primary diagnostic channel. Serial is supplementary.** Plan around this from day 0.
6. **macOS + ESP32-S3 native USB-JTAG**: open the port as rarely as possible; use `stty -hupcl` + `dd` when you do; physical unplug/replug is the reliable recovery.
7. **INT8 PTQ with synthetic calibration is a debug stop-gap, not a ship state.** Always re-quantise against real audio before claiming performance numbers.
8. **The pretrained-model-for-microcontroller supply chain is broken in subtle ways.** Don't trust an HF model card — download + sniff the `.tflite` with `flatc` or `tf.lite.Interpreter` first.
9. **When `docs/research/*.md` claims a fact, its corresponding `hf-check` / `github-check` / `curl -I` should be in the commit message.** The paper trail saves time-of-failure triage.

---

*(The device is currently in real-world deployment. Monitor task `bgsgf6dvd` is emitting 1 line / minute to this session. Findings will land in a successor doc once we have a 24 h trace.)*
