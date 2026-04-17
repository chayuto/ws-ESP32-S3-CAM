# Runtime robustness — preventing the bugs we hit in Stage 1

*Written 2026-04-17 after six hours of on-device bring-up. Every entry in this doc maps to a fault we actually encountered or narrowly avoided.*

Reference: `stage1-bringup-labnotebook.md` §3 (the catalogued fault log). This doc is the *prescription* side of that *diagnosis* doc.

---

## 1. The bug classes we hit — and why each happened

| Class | Example from Stage 1 | Why it happened |
|---|---|---|
| **NULL-mutex races** | F10 `web_ui_push_event` crashed because `inference_task` started before `web_ui_start` created `s_lock`. F13 `noise_floor_p95` same pattern. | Order of init vs order of task-start is implicit in `app_main` prose. Compiler can't see it. |
| **Stack overflows in I/O-heavy tasks** | F14 housekeeping task crashed writing SD every 30 s because 3 KB stack wasn't enough for FATFS descent. | ESP-IDF task stack is a hard-coded number per `xTaskCreate`. No visible signal until it overflows. |
| **C-vs-C++ linkage mismatches** | F3 yamnet.h had no `extern "C"` guards. | Headers used from both C and C++ TUs without an explicit contract. |
| **Format-string truncations forced to errors by -Werror** | F7 snprintf warning in `event_recorder.c`. | Compiler heuristics + `-Werror=all`. |
| **Wrong-variant data files** | F8 pulled a TF Hub waveform-input YAMNet instead of a mel-patch INT8 variant; crashed at `AllocateTensors` with unsupported op. | No integrity / shape check before loading. Fallback URL was a guess. |
| **Environmental state traps** | F12 macOS DTR-on-open trapped the chip in download mode → no output. | Host-side tooling behaviour collided with chip-side pin meaning. Serial appeared dead despite healthy firmware. |

The common thread: **Stage 1 had code that worked on the happy path, but gave no cheap signal when the happy path was broken.** The fixes were all trivial once the signal appeared. Getting to the signal was the cost.

---

## 2. Compile-time guardrails (already mostly on)

### Currently on (verified)

- `-Wall -Wextra -Werror=all` — ESP-IDF default. Caught F7.
- `-Wno-error=unused-function/-variable/-but-set-variable/-deprecated-declarations` — standard relaxations.
- `-std=gnu17` / `-std=gnu++2b` — modern stds.
- PIE-safe PC-relative code generation.

### Worth adding (not yet)

| Setting | Kconfig | Catches |
|---|---|---|
| `-Wformat=2 -Wformat-security` | menuconfig → Compiler → Warnings | Format string / %s-on-NULL issues |
| `CONFIG_COMPILER_STACK_CHECK_MODE_STRONG` | Compiler options → Stack smashing protection → Strong | F14-class bugs at function-entry boundaries |
| `CONFIG_COMPILER_WARN_WRITE_STRINGS` | Compiler → Warnings | "const char *" vs "char *" mistakes |
| `CONFIG_COMPILER_NO_MERGE_CONSTANTS` when debugging | — | So assert-failure backtraces point at the right file:line |
| **clang-tidy in CI** against the codebase | external | Whole class of issues compilers miss |

### Static analysis as a separate pass

`cppcheck projects/cry-detect-01/main/` in CI would have caught F3 (header declaration mismatch) and some NULL-deref candidates. Run in pre-commit hook or GH Action. Cost: one YAML file, near-zero maintenance.

---

## 3. Runtime guardrails (the big lever)

### 3.1 Task Watchdog (TWDT)

Currently **off** in our sdkconfig. Enable it:

```
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=15
CONFIG_ESP_TASK_WDT_PANIC=y
```

Every task-of-ours must periodically `esp_task_wdt_reset()` or the TWDT panics. This would have caught F14 (`housekeeping_task` stuck in `fwrite`) within 15 s instead of after a full crash cycle. Adds ~0.5 KB per task.

**Pattern to adopt:** every loop in a task body ends with `esp_task_wdt_reset();`. Ours don't; that's the Stage-2 change.

### 3.2 Interrupt Watchdog (IWDT)

Already on in ESP-IDF default. Catches ISR/stuck-critical-section bugs. Leave.

### 3.3 Stack overflow detection

```
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER=y
```

Puts a canary at the stack base; checked on every context switch. **Traps F14 immediately** with a nice panic instead of a silent corruption that reboots 30 s later. Free; turn it on.

### 3.4 Heap integrity checks

```
CONFIG_HEAP_POISONING_LIGHT=y
```

Poisons freed blocks; double-free + use-after-free get caught on the next `malloc`. Costs ~5 % heap overhead, worth it during development.

`CONFIG_HEAP_TASK_TRACKING=y` + periodically call `heap_caps_print_heap_info()` at a diagnostic endpoint → pinpoint slow leaks. Could replace the manual `free_heap` snapshot we take.

### 3.5 Null-mutex defensive pattern (project convention)

Every `xSemaphoreTake(s_lock, …)` in module code must be preceded by:

```c
if (!s_lock) return <sensible default>;
```

Applied in `web_ui.c`, `noise_floor.c`, `sd_logger.c`. **This should be a project convention, lint-enforced.** A 10-line grep in CI:

```
grep -rnE 'xSemaphoreTake\(s_[a-z_]+_lock' projects/cry-detect-01/main/ | \
  while read hit; do
    file=$(echo "$hit" | cut -d: -f1)
    line=$(echo "$hit" | cut -d: -f2)
    prev=$(sed -n "$((line-3)),$((line-1))p" "$file")
    if ! echo "$prev" | grep -q 'if (!s_'; then
      echo "MISSING NULL-GUARD: $hit"
    fi
  done
```

### 3.6 Module init ordering contract

Right now the ordering is implicit in `app_main`. Formalise with:

```c
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
} cry_module_init_t;

static const cry_module_init_t g_modules[] = {
    { "metrics",     metrics_init_wrapper },
    { "led_alert",   led_alert_init_wrapper },
    { "sd_logger",   sd_logger_init_wrapper },
    { "yamnet",      yamnet_init_wrapper },
    { "mel",         mel_features_init },
    { "audio",       audio_capture_init_wrapper },
    { "detector",    detector_init_wrapper },
    { "noise_floor", noise_floor_init_wrapper },
    { "audio_stream", audio_stream_init_wrapper },
    { "event_rec",   event_recorder_init_wrapper },
    { "network",     network_start_wrapper },
    { "web_ui",      web_ui_start_wrapper },
};
// app_main: for (each) { call init; log; check ESP_OK; }
```

Makes it impossible to accidentally reorder. Extracts the boot sequence into a declarative list. Stage 2.3 cleanup.

---

## 4. Testing — what would have caught what

Zero on-host tests exist right now. Proposed tiered approach:

### 4.1 Host-side unit tests (fast, per-module)

`test/` directory with CMake + Unity (Espressif's default). Per-module tests:

- `test_mel_features.c` — known input samples → known mel output. Would catch mel-bank formula errors, Hann window off-by-one, filterbank boundary bugs. Pure maths, no hardware.
- `test_detector.c` — synthetic conf stream → expected state transitions. Catches hysteresis bugs, hold-time bugs, threshold-boundary bugs.
- `test_noise_floor.c` — distribution of RMS values → expected p50/p95 after N samples. Catches histogram bugs.
- `test_sd_logger_format.c` — fixed inputs → exact expected CSV row. Catches field-order regressions, format-truncation.

Run in CI on every PR. Wouldn't have caught any **specific** Stage 1 bug (those were all ESP-IDF-environment), but prevents a whole class of regressions going forward.

### 4.2 QEMU integration tests

ESP-IDF has a qemu-esp32 + (nascent) qemu-esp32s3 target. Run the app in qemu with:

- Mock Wi-Fi via tun/tap → real `/metrics` on host loopback
- Mock SD via a looped-back file
- Mock audio via a pre-canned PCM file played through the capture tap

Would have caught F10 (NULL mutex) before flashing. Qemu boots in 3 s vs flash+verify 30 s. Massive iteration speed-up.

Cost: 1–2 days to set up the qemu harness + CI runner. High leverage going forward.

### 4.3 On-device smoke tests

A tiny shell script that:

1. Flashes the current build.
2. Reads serial for 30 s via `stty -hupcl + dd` (per `monitor.md`).
3. Greps the log for expected markers: `boot complete`, `yamnet: arena used`, `wifi up`, `net: mDNS`, `hk:`, `main: boot complete`.
4. Hits `/healthz` and `/metrics` via curl.
5. Exits 0 if all green.

```bash
# tools/smoke-test.sh
set -e
idf.py -C . -B /tmp/build build flash
stty -f /dev/cu.usbmodem3101 -hupcl 2>/dev/null
dd if=/dev/cu.usbmodem3101 of=/tmp/smoke.log bs=1 count=40000 2>/dev/null &
DD=$!; sleep 30; kill $DD
grep -q "boot complete" /tmp/smoke.log || { echo "FAIL: no boot complete"; exit 1; }
grep -q "yamnet: arena used" /tmp/smoke.log || { echo "FAIL: yamnet didn't load"; exit 1; }
sleep 5
curl -sf http://192.168.1.100/healthz || { echo "FAIL: healthz"; exit 1; }
echo "PASS"
```

Run before every commit. Cheap once CI has a runner with a physical board attached.

### 4.4 Continuous soak test

Exactly what the **Monitor tool + tee'd log file** is doing right now. Runs for hours. Reports `REBOOT` / `WIFI-DROP` / `NEW-ALERT`. Keeps the durable file. Already in place.

Add: automatic screenshot / `/metrics` dump to SD on any crash (via the event recorder's pre-roll buffer expanded to include a JSON metrics snapshot).

### 4.5 Fault injection

Deliberately break things and observe recovery:

- Unplug Wi-Fi router for 5 min → does device recover cleanly when it returns?
- Yank SD mid-write → does the logger fail soft and resume?
- Flood `/audio.pcm` with 10 concurrent clients → does the cap hold?
- Burn cry_conf to 1.0 artificially via a debug endpoint → does the event pipeline fire + record + alert as expected?

Three of these (Wi-Fi drop, SD-yank, stream-flood) are easy to add to the smoke-test. Fourth needs a debug hook in detector.

---

## 5. Instrumentation — observability as a first-class feature

What made F14 hard to diagnose: no ongoing signal of task health. We fixed it with the 10 s serial heartbeat + 30 s SD snapshot. Extend:

1. **Task CPU percentage** per second per task. ESP-IDF provides via `vTaskGetRunTimeStats`. Expose in `/metrics`.
2. **Queue / StreamBuffer high-water marks.** `xStreamBufferBytesAvailable` max per interval. Catches "audio starved" before it's a problem.
3. **Interrupt latency histogram.** Catches ISR-hostile changes.
4. **Crash upload on reboot.** Save the core-dump to SD via `esp-idf-core-dump` to the `logs_fat` partition; web UI shows "last crash" with timestamp + backtrace. Enables post-mortem without physical access.

```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
```

Plus one more partition (`coredump`, 64 KB). Small cost; huge debug value.

---

## 6. The "how to test now" answer — prioritised

In order of value-per-time, given what we have today:

1. **Enable stack overflow canary + heap poisoning + TWDT** in sdkconfig. ~5 lines. Re-flash. Free F14-class protection.
2. **Host-side `mel_features` unit test.** Catches the largest class of "algorithm wrong" bugs (and that's the area most likely to regress during Stage 2 calibration work).
3. **Smoke-test script.** Runs in 30 s per commit. Catches all "doesn't boot" regressions before real testing.
4. **NULL-guard lint in CI** (the grep above). Catches F10/F13-class races.
5. **Coredump-to-SD.** Every unrecoverable crash leaves a forensic artefact on the SD card.
6. **QEMU** as the long-term iteration speedup — only if flash-test-iterate cycle becomes the bottleneck again.

---

## 7. Summary

| Practice | Cost | Bug class it catches |
|---|---|---|
| Stack overflow canary (Kconfig) | free | F14-type task overflows, instantly |
| Heap poisoning (Kconfig) | ~5 % heap | Use-after-free, double-free |
| TWDT enabled + per-task resets | tiny | Stuck tasks, infinite loops |
| NULL-guard convention + CI lint | half-day | F10 / F13-type init-order races |
| Module-init declarative table | 1 day | Reorder mistakes |
| Host unit tests (mel, detector, noise_floor) | 1 day per module | Algorithm regressions |
| Smoke-test script | 2 hours | Build + smoke regressions |
| QEMU harness | 1–2 days | Fast iteration on non-HW bugs |
| Coredump-to-SD | half-day | Forensics for unreproducible crashes |

Total "high-leverage" work: **~3–4 days.** Pays for itself the first time a Stage 2 bug surfaces without a host on the bench.
