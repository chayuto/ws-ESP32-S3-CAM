# Project-wide hygiene audit — cry-detect-01

**Date:** 2026-04-18 (Sydney AEST)
**Trigger:** Escalating bootloops as features grew (file API → TZ → metrics_logger). User asked for a deep validation pass before adding more.
**Method:** Explore agent read every source file under `projects/cry-detect-01/main/` (excluding `managed_components/`) against a structured checklist: concurrency/lifecycle, error handling, sdkconfig safety, debuggability, code smells.

## Executive summary

**35 issues** found. The ones that explain *today's* crash and the ones that will save us the most future cycles are concentrated at the top.

- **9 P0** — crashes waiting to happen
- **9 P1** — reliability / silent failure
- **7 P2** — debuggability upgrades
- **10 P3** — code quality

**Root cause of today's bootloop (high confidence):** `main.c on_net_state()` runs on `esp_event_loop_run_task` context and calls `sd_logger_event()` + `sd_logger_ntp_sync_marker()` which do blocking SD I/O under a mutex. With LFN enabled (our recent change) the SD path got slower; the event-loop task is now blocking long enough to trip `xQueueGiveMutexRecursive` asserts from elsewhere in the event pipeline. This reproduces the backtrace we captured: `xQueueGenericSend` assert at `queue.c:937` inside `esp_event_loop_run_task`.

This is the fix path that unblocks everything else.

## P0 — Fix before next flash

| # | Where | Issue | Fix |
|---|---|---|---|
| 1 | `main.c:49` `on_net_state` | **Blocking SD I/O on esp_event task.** `sd_logger_event` / `sd_logger_ntp_sync_marker` call `fopen`/`fwrite`/`fclose` under a mutex from the event-loop callback. LFN made this slow enough to wedge the loop. | Defer: handler posts to a queue; `housekeeping_task` drains it and does the SD writes |
| 2 | `sd_logger.c` all public fns | `xSemaphoreTake(s_lock, portMAX_DELAY)` with no NULL-guard. Any pre-init caller deadlocks | NULL-guard every public fn: `if (!s_lock) return;` |
| 3 | `web_ui.c:177` | Pre-init SSE events silently dropped. Defensive but undocumented. | Document ordering constraint with a comment |
| 4 | `metrics_logger.c:279` | `metrics_logger_publish_inference` drops data silently if init never ran | Either assert init-ran-first or promote init failure to visible state |
| 5 | `event_recorder.c:169-199` | Static `FILE *f` inside the task loop; on partial-write failure, `s_recording` may not atomically reset → stale FP | Reset `s_recording = false` before continue on any write failure |
| 6 | `event_recorder.c:83` | Pre-NTP filename `cry-boot%04u.wav` using `now & 0xffff` collides every 65k boots (silently overwrites) | Use a NVS-persisted boot counter |
| 7 | `main.c:127-134` inference alloc | Partial-success leak: if 1st malloc ok, 2nd fails, 1st leaks | Free all non-NULL allocations before bail |
| 8 | `audio_stream.c:52,64,78` | `s_listener_count++/--` unsynchronised | Guard with mutex or use `atomic_fetch_add` |
| 9 | `metrics_logger.c:231-246` | `ts_arr` size clamped to `STACK_MAX=20` but loop uses `got` (task count); if `ntasks > STACK_MAX`, `got` may exceed allocated buffer | Clamp loop bound to `min(got, STACK_MAX)` |

## P1 — Reliability / silent failure

| # | Where | Issue | Fix |
|---|---|---|---|
| 10 | `sd_logger.c:66-77` `reopen_locked` | Fopen failure only logs a warning; subsequent writes silently drop | Return `esp_err_t`; surface errors to metrics |
| 11 | `sd_logger.c:182-207` `write_row_locked` | `RING_LINE_BYTES=448` may truncate v3 schema with 20 watched floats | Validate snprintf return; widen buffer; log truncation |
| 12 | `metrics_logger.c:72-77` `format_timestamp` | `memmove` bounds check too loose — could be wrong buffer size | Tighten bounds check to account for final NUL |
| 13 | `event_recorder.c:59,66,68,137-139,191,197` | All `fwrite` unchecked — SD full → WAV header size wrong, file corrupt | Check fwrite returns; close file on short write |
| 14 | `sd_logger.c:211` | Fwrite failure still bumps `s_written_in_file` → rotation triggers incorrectly | Only increment on success |
| 15 | `metrics_logger.c:111` reopen fallback | Re-opens same file each tick if fopen failed — thrashing | Backoff on repeated failure |
| 16 | `audio_capture.c:147` stream buffer | Trigger level / ring size assumptions not asserted | Add compile-time or init-time assert |
| 17 | `main.c:299` `hk` task 6 KB | Locks metrics + noise_floor + logs in hot path; tight | Measure HWM, bump to 8 KB if close |
| 18 | `metrics.c:85-90` `fanout_locked` | Lock released between subscribers; stale metrics possible | Snapshot once, release, then fan out |

## P2 — Debuggability

| # | Where | Issue | Fix |
|---|---|---|---|
| 19 | `sdkconfig.defaults` | **No core dumps.** Every crash requires live addr2line against the exact elf. | `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` + `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y` + add `coredump` partition |
| 20 | `sdkconfig.defaults` | No `CONFIG_ESP_PANIC_PRINT_MEMADDR=y` or debug stubs | Enable both |
| 21 | `sdkconfig.defaults` | `FREERTOS_CHECK_MUTEX_GIVEN_BY_OWNER` off | Enable — would catch today's bug instantly |
| 22 | All modules | No runtime log-level control per TAG | Add `/log/level?tag=sdlog&level=debug` endpoint |
| 23 | `main.c`, `network.c` | No breadcrumb state in NVS — reboot loses "what happened" | Write last event + uptime to NVS on transitions |
| 24 | `main.c:304` | `metrics_logger` is `#if 0`'d out — killed our best telemetry | Resolve P0 #1; re-enable |
| — | — | `.claude/commands/monitor.md` DTR trap docs | Already present; cross-ref in safe-dev.md |

## P3 — Code quality (cleanup when touching)

Items 25–35 cover: LFN-aware filename handling in `event_recorder`, ring buffer read/write races in `sd_logger_tail`, unchecked snprintf truncations in `write_row` / inference payload, `detector_submit` / `detector_get_state` lacking synchronization, `led_task` handle never stored, qsort-per-inference in p95 metric, tight stack on `audio_capture` / `event_recorder` tasks, possible httpd-task stack pressure in `web_ui` SSE snapshot. None of these are acute, but worth sweeping when next in those files.

## Recommended fix order

1. **P0 #1 (esp_event SD I/O)** — unblocks the device *today*
2. **P2 #19+#20+#21 (core dumps + mutex-owner check)** — one sdkconfig change; every future crash is 10× faster to diagnose
3. **P0 #2 (NULL-guard sd_logger)** — cheap, prevents whole class of races
4. **P0 #7 (inference alloc leak)** — trivial
5. **Re-enable metrics_logger** (P2 #24) with staged wiring per safe-dev.md
6. **P1 #11, #13, #14** (SD write / fwrite error handling) — before the next overnight deployment
7. Rest as we touch files

## Suggested sdkconfig.defaults additions

```
# Core dumps (P2 #19): every crash persisted for post-mortem
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y
CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=16

# Panic output (P2 #20)
CONFIG_ESP_PANIC_PRINT_MEMADDR=y
CONFIG_ESP_DEBUG_STUBS_ENABLE=y

# Mutex ownership check (P2 #21): would catch today's bug on the spot
CONFIG_FREERTOS_CHECK_MUTEX_GIVEN_BY_OWNER=y
```

`partitions.csv` also needs a `coredump` line (default 64 KB) when core dumps are enabled. Placed after the existing partitions, before any free space.
