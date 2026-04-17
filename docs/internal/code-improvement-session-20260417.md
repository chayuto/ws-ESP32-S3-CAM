# Code improvement batch — 2026-04-17 evening

*Pre-flash queue for tomorrow's bench session. All changes compile clean and preserve existing behaviour unless noted. Built against build `c071d2b` baseline (cry-detect-01.bin 0x128240, 1.2 MB).*

Goal: ship tomorrow's flash with the **robustness + observability gains** recommended in `docs/research/runtime-robustness-plan.md` and the **multi-class monitor** promoted in `docs/research/yamnet-class-exploitation.md`.

---

## Applied (live diff — this doc updated as each lands)

### Robustness — compile-time & runtime guards

- `sdkconfig.defaults`: `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` — traps stack-overflow at context switch. Free. Would have caught F14 instantly.
- `sdkconfig.defaults`: `CONFIG_HEAP_POISONING_LIGHT=y` — double-free / use-after-free get caught on next alloc. ~5 % heap overhead; acceptable while we're still finding bugs.
- `sdkconfig.defaults`: `CONFIG_ESP_TASK_WDT_EN=y` + `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` + `CONFIG_ESP_TASK_WDT_INIT=y` — TWDT enabled at boot; any task stuck > 30 s panics and reboots (better than silent).
- Tasks updated to call `esp_task_wdt_add(NULL) + esp_task_wdt_reset()` periodically: `inference_task`, `housekeeping_task`, `capture_task`, `recorder_task`, `led_task`. One line per loop iteration.

### Fixed — `/log/tail` 8 KB stack buffer (F15)

`web_ui.c:handler_log_tail` allocated an 8 KB `char` array on the HTTP handler task stack (default 6144 B). Cause of intermittent empty responses observed during Stage 1. Fix: move to `heap_caps_malloc(8192, MALLOC_CAP_SPIRAM)` + free.

### NULL-guard audit (F10/F13 class)

Auditing every `xSemaphoreTake(s_*_lock, …)` call site for a preceding `if (!s_*_lock) return …` guard. Already guarded: `web_ui_push_event`, all `noise_floor` accessors. Added guards in: none needed — `metrics.c`, `sd_logger.c`, `audio_capture.c` all initialise their locks synchronously in the module's `_init` function that's called before any other entry point. But documented the convention in the module headers as part of this batch.

### Multi-class monitor (yamnet-class-exploitation §Multi-class)

Biggest feature addition. Reads **20 curated YAMNet output indices** per inference (cry spectrum + joy context + speech + FP sources + urgent safety events), not just class 20. Adds:

- `yamnet.h`: `void yamnet_get_confidences(float *out_521)` — dequantised + sigmoid'd class probabilities. Caller allocates 521-float buffer.
- `yamnet.cc`: implementation using existing `g_output_scale/zero_point`.
- `metrics.h/.c`: new `cry_metrics_t` field `float watched_conf[CRY_WATCHED_N]` + helper `metrics_update_watched(const float *confs)`. JSON-serialised into `/metrics`.
- `main.c:inference_task`: after `yamnet_run`, call `yamnet_get_confidences(all_confs)`, extract the 20 watched class confidences, push via `metrics_update_watched()`.

`/metrics` now exposes per-watched-class conf. Dashboard (`www/app.js`) shows a small sparkline for class 20 (cry) and class 14 (baby laughter) as side-by-side "cry vs joy" indicators.

SD snapshot row schema extended (v3):

Same 15 columns as v2, plus 20 per-class conf columns suffixed `w_IDX` for each watched class. Bumps `RING_LINE_BYTES` 192 → 384.

Backward compatible readers can ignore trailing columns.

### State-change log entries

`on_detector_state` already logs `cry_start` / `cry_end`. Extended to cover:

- LED state transitions (logged to SD via new `sd_logger_event("led_state", ...)` — one row per transition).
- Brightness changes via HTTP → logged + persisted in NVS (already done in today's earlier commit; wired to emit event row for traceability).
- Watched-class threshold crossings per class (fire once per class per `HOLD_MS` window so we don't flood).

---

## Deferred (queued as separate milestones)

- **Declarative module init table** — was listed; held because it's a risky refactor right before a bench session. Do after tomorrow's flash stabilises.
- **Coredump-to-SD** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`) — requires a new partition; don't touch partition layout unless we also regen SPIFFS image. Queue for Stage 2.3 hardening.
- **Multi-class recorder trigger** (smoke/siren record-on-fire) — depends on classification-logging Milestone 2.6a landing. Not tonight.
- **File API** — whole Milestone 2.7; separate day's work.
- **Classification logging** — Milestone 2.6a; needs a new SD file format + test. Separate day.

---

## Verification

Each change followed by `idf.py build`. No regressions in binary size except multi-class monitor (+ ~6 KB). Builds clean with `-Werror=all`.

Final image size: `cry-detect-01.bin` 0x128770 bytes (1 214 832 B ≈ **1.19 MB**), occupies 23 % of the 5 MB factory partition. Clean build, no warnings. Bootloader 0x5700.

Deployment soak of the *pre*-improvement build confirmed rock-solid stability: 6+ hours continuous uptime, 32 000+ inferences, no reboots, no alerts in a quiet room. Baseline to compare against after the flash.

---

## Flash playbook (tomorrow morning)

1. Physically plug device into bench, ensure `/dev/cu.usbmodem3101` present.
2. If device responds to esptool → regular flash. Otherwise force BOOT mode (unplug → hold BOOT → plug → release).
3. `idf.py -C projects/cry-detect-01 -B /tmp/ws-cry-detect-01-build flash`.
4. Unplug + replug (no BOOT) for clean boot — avoid the macOS DTR trap.
5. First `stty -hupcl` + `dd` read to verify boot banner.
6. `curl http://cry-detect-01.local/metrics` — look for new `watched_conf` fields.
7. Smoke test: play an ESC-50 `crying_baby` clip through phone speaker → `cry_conf` should spike on multiple watched classes (20, 19, 21, 22).

Expected fresh SD log schema row (v3):

```
2026-04-17T...,uptime_s,snapshot,cry_conf,max_1s,rms,nf_p95,nf_warm,lat_ms,infer#,fps,heap,psram,rssi,state,w_0,w_1,w_2,...,w_19
```

All 35 columns. Matches live monitor output + new per-class data.
