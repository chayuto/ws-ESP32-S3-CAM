# Deployment move — SD mount failure postmortem

**Date:** 2026-04-18 evening
**Duration of issue:** ongoing at time of writing
**Status:** physical cause confirmed; software-side clean

## What happened

1. Device was running cleanly on the bench for a full day — `sd_mounted: true`, 4 WAVs captured + JSONL intact, auto-trigger primed, noise floor stable.
2. Firmware was re-flashed with the "LED code removed" build (previous iteration had stripped `led_alert.c/h`, Kconfig entry, web endpoint, UI slider — after the P6 LED toggle test showed no visible response on-board).
3. Board was moved from bench to deployment environment.
4. After move, `/metrics` reported `sd_mounted: false`, `/record/status` fell back to `/logs/events` (SPIFFS, which has no room for 40 s WAVs).
5. Hypothesis raised: LED-removal patch broke SD init.

## What we tested

Reverted the LED-removal patch in full:
- Restored `led_alert.c` and `led_alert.h` from `HEAD` (git checkout)
- Restored `CMakeLists.txt` sources list
- Restored `main.c` includes + `led_alert_init()` + all `led_alert_set()` calls
- Restored `web_ui.c` `handler_led_brightness` + route registration
- Restored `Kconfig.projbuild` `CRY_DETECT_LED_EXPANDER_PIN`
- Restored `sdkconfig` entry `CONFIG_CRY_DETECT_LED_EXPANDER_PIN=6`
- Restored UI LED slider block in `index.html` and `app.js` fetch/set functions
- Rebuilt, flashed, reverified

**Result after revert:** `sd_mounted: false` — unchanged.

## Conclusion

SD mount failure is not caused by the LED code removal. The identical firmware mounted SD cleanly before the move, and the fully-restored firmware still fails after the move.

Cause is one of:
- SD card dislodged during transport
- Contact oxidation / skin oil on card fingers
- SD socket damaged / solder joint cracked by movement stress
- SDMMC controller wedged (clears only with cold power cycle)

## State at commit

Running firmware is functionally identical to the pre-removal iteration plus all Stage 2.6+ work (auto-trigger, P1 fixes, manual-trigger UI card, P50-based threshold math, GET alias for /record/trigger). LED code is back to committed-baseline behavior (default brightness 100; NVS override of 0 persists across reboots).

## What the revert preserves

- `auto_trigger.c/h` + Kconfig block — RMS-threshold auto-trigger for overnight data collection
- `web_ui.c` P0 #1 deferred-queue pattern for on_net_state
- `event_recorder.c` manual trigger + `triggers.jsonl` + retention loop rewrite (5000-file ceiling)
- `metrics_logger.c` fopen exponential backoff
- `audio_capture.c` overrun metrics + throttled log + tap-size guard
- `metrics.c` fanout_locked subscriber snapshot
- `index.html` / `app.js` CAPTURE TRAINING SAMPLE card with preset-label buttons

All of the above are unchanged by the revert — only the LED file deletion was reversed.

## Action items

1. **Physical reseat** — try firmer push until the slot clicks. Most likely root cause.
2. **Try a different SD card** — isolates dead card from dead slot.
3. **Cold power cycle** — unplug USB/battery for 5 s, replug. Clears any SDMMC controller wedge.
4. **Last resort:** reflow SD socket solder joints under magnification.

## Lessons

- Never correlate "the feature stopped working" with "the last change" without a counterfactual. SD mount is binary-independent here — the firmware binary was the same before and after the "buggy" change.
- Board relocations are a source of physical faults disguised as code faults. Always eliminate the physical path first.
- LED_ALERT code touched no SD / SDMMC / FATFS symbols — the blast radius was always zero. A code-level read of the diff would have shown this, but schedule pressure favored the empirical revert.
