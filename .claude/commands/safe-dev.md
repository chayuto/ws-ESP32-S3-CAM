# Safe feature-dev loop for ESP32-S3 cry-detect-01

Use this flow when adding a **new subsystem** (new task, new driver, new Kconfig symbol). Every crash we've had in this project came from skipping one of these checks. Usage: `/safe-dev <what-you're-adding>`.

## Principle: change one thing, verify, then change the next

The ESP32-S3 + native USB-JTAG makes iteration slow because:
- Every serial-monitor open briefly toggles DTR/RTS → chip resets
- A bootloop takes ≥5 s per cycle; multi-feature breakage is hard to bisect
- Assert/panic backtraces require addr2line — only useful if you saved the .elf from the exact build that crashed

So: add one subsystem, flash, verify, commit, then add the next.

## Pre-flash checklist

Before every flash, tick these:

1. **New Kconfig enabled?** Check if it changes stack sizes / requires companion symbols.
   - `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` → adds per-TCB bookkeeping; bump small task stacks
   - `CONFIG_FATFS_LFN_HEAP=y` → `fopen` of long names consumes extra heap; SD open can slow
   - Any new driver component (e.g. `driver/temperature_sensor.h`) → often pulls in event-loop handlers; do NOT install inside another driver's callback
2. **New task?** Start it with 6 KB stack minimum unless you have a reason to go smaller. Add to WDT if it runs > 1 Hz. Pin to core 0 (Wi-Fi/lwIP on 0; model inference on 1).
3. **New callback on event loop?** Anything in `esp_event_loop_run_task` context must be **non-blocking**. Do not:
   - call `fopen`/`fwrite`/SD I/O
   - take long-held mutexes (sd_logger, metrics, etc.)
   - call into `sd_logger_ntp_sync_marker` which opens+closes files
   
   Instead: signal a worker task via a queue/semaphore.
4. **New mutex?** Write down the lock order on paper. This project has mutexes in `sd_logger`, `web_ui`, `event_recorder`, `metrics_logger`, `noise_floor`. A handler that takes two of them in the wrong order will deadlock.
5. **New heap allocation > 1 KB?** Use `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)` not stack, not internal RAM. Our internal DRAM is ~250 KB and every task competes.
6. **Feature flag in Kconfig?** Add an `CONFIG_CRY_<X>_ENABLED` symbol defaulting OFF until verified. That way rollback is `sed` one line, not revert five files.

## Iteration cycle

```
1. Write code for ONE subsystem
2. Build → check warnings (promote warnings to errors only for project code, not vendor)
3. If serial is healthy pre-flash, snapshot it: this becomes your "known-good reference"
4. Flash
5. Wait 30 s WITHOUT touching serial (every serial-open resets the S3 via DTR)
6. Probe HTTP (/healthz or /metrics) — if HTTP up, system is alive enough
7. If HTTP down after 30 s, ONE serial read is justified — capture for 20 s to catch a crash cycle
8. If backtrace appears, addr2line it immediately:
   $HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line \
     -pfiaC -e /tmp/ws-cry-detect-01-build/cry-detect-01.elf <addr1> <addr2> ...
9. If stable, commit, then add next subsystem
```

## Things that have crashed us in this project (learn once)

| Crash | Root cause | Fix pattern |
|---|---|---|
| `esp_event_loop_run_task` assert in `xQueueGiveMutexRecursive` after IP event | Event-loop callback called a long blocking operation (SD write) that disturbed mutex bookkeeping | Defer SD writes out of `on_net_state`; signal a worker |
| Bootloop around NTP sync | `sd_logger_ntp_sync_marker` calls `fclose`+`fopen` from the event-loop context | Same — don't reopen files from handlers |
| 30 s crash-reboot cycle on web request | Handler stack too small, 8 KB buffer in handler lives on 6 KB task | Move buffers to PSRAM heap: `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)` |
| `mlog: fopen /sdcard/infer-boot.jsonl failed` | FATFS LFN disabled, name exceeds 8.3 | Set `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255` |
| `format_timestamp` garbage in SD log | memmove shifted 2 bytes but clobbered NUL; caller did `%s` | After memmove, explicitly `buf[n+m] = '\0'` |
| Null-mutex crash F10/F13 | Global mutex accessed before its init ran (race with Wi-Fi/noise_floor) | NULL-guard every accessor: `if (!s_lock) return;` |

## When device is unreachable

Order of checks (fastest first):
1. `ping -c 2 192.168.1.100` — if "Host is down", ARP failed → device off-network
2. `arp -a | grep "$DEVICE_MAC"` — if `(incomplete)`, device MAC not on LAN
3. `ping -c 2 cry-detect-01.local` — mDNS fallback
4. **Only then:** one controlled serial read (20 s window, DTR/RTS off in pyserial)

Opening serial monitor unnecessarily = you just extended every iteration by one reboot cycle.

## Staged wiring pattern

When adding a new task that both **runs standalone** and **hooks into the hot-path** (e.g. inference):

1. First flash: task running with mock/synthetic input; hot-path publish call `#if 0`-d out
2. Second flash: enable the publish call once standalone stability is proven

This catches init-order bugs without making them race with the main inference loop.
