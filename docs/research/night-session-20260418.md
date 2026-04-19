# Full-night session report — 2026-04-18 → 2026-04-19

**Session start (monitor first sample):** 2026-04-18 21:12:58 +1000
**Session end (monitor last sample):** 2026-04-19 09:26:23 +1000
**Elapsed:** 12 h 13 m (730 × 1-min samples, 100 % reachable)
**Firmware:** `build_sha=5e9d5a7e`, `fw_ver=42a7e55-dirty`, single build across entire session
**Incident of interest:** 2026-04-19 04:54 – 05:12 (18 min)

## 1. Device health

| Metric | All-night value |
|---|---|
| Reachable | **730 / 730** (100 %) |
| SD mounted | true throughout |
| NTP synced | true throughout |
| Build SHA | `5e9d5a7e` throughout — no reflashes/reboots |
| Alert count transitions | **0 → 0** — INT8 model saturated, never fired |
| Uptime at wake | 47,130 s (~13 h 5 m), clean |
| Free heap / PSRAM at wake | 2.31 MB / 2.27 MB — stable, no growth leak |
| Inference FPS | 1.39 steady |
| Inference ms p95 | 509 ms steady |
| Wi-Fi RSSI | –57 dBm at wake (good) |

No wobble, no drops, no auto-reboots. The RMS-only auto-trigger did all the work while the model was silent.

## 2. Capture effectiveness

Total WAVs written to SD: **22** (22 files × 1.28 MB = 28.2 MB, 0.18 % of 15.8 GB free). Capacity ceiling is 5000 files (`CRY_REC_KEEP_FILES`) so we're at 0.44 % of the retention budget.

Pre-incident (bench / daytime tests): 8 WAVs spanning 16:57 → 19:20 local.
Incident window: 14 WAVs over 18 minutes (04:54 → 05:12).
Post-incident (05:12 → 09:26): **0 new WAVs** — 4 hours of quiet morning silenced the auto-trigger.

Cooldown behavior matches design: 60 s between fires, 40 s recording each, so the in-window density caps at ~6 fires per 10 min. We hit 14 fires in 18 min = 0.78/min, consistent with the cry pattern leaving short breathing gaps.

## 3. Monitor log — hourly rollup (input_rms distribution)

| Hour (local) | n | min | med | p95 | **max** | nf50 end | wav_count |
|---|---:|---:|---:|---:|---:|---:|---:|
| 21:00 (Apr 18) | 47 | 69 | 94 | 116 | 119 | 99 | 8 |
| 22:00 | 60 | 55 | 70 | 87 | 92 | 81 | 8 |
| 23:00 | 59 | 57 | 68 | 120 | **233** | 81 | 8 |
| 00:00 (Apr 19) | 60 | 49 | 70 | 280 | **308** | 81 | 8 |
| 01:00 | 60 | 35 | 65 | 180 | **215** | 66 | 8 |
| 02:00 | 59 | 52 | 63 | 223 | **283** | 66 | 8 |
| 03:00 | 60 | 49 | 62 | 133 | **210** | 66 | 8 |
| 04:00 | 60 | 57 | 62 | 249 | **968** | 66 | 10 |
| 05:00 | 59 | 59 | 64 | 788 | **1266** | 66 | 22 |
| 06:00 | 60 | 56 | 65 | 78 | 93 | 66 | 22 |
| 07:00 | 60 | 51 | 67 | 76 | 90 | 66 | 22 |
| 08:00 | 59 | 53 | 68 | 77 | 79 | 66 | 22 |
| 09:00 | 27 | 64 | 72 | 92 | 156 | 66 | 22 |

**Three observations from the table:**

1. **Noise floor converged cleanly**: nf50 99 → 81 → 66 during the first ~3 h of deployment as the histogram filled. After 01:00 it pinned at 66.
2. **Pre-incident "near-misses" at 23:00, 00:00, 02:00, 03:00** — peak RMS hit 210–308 during hours with 0 WAVs captured. Those events were above the file-level peak but didn't sustain 1500 ms above the 5× nf50 threshold (= ~330 at 66 nf50), so the auto-trigger correctly did *not* fire. Interpretation: brief shuffles / settling noises, not crying. Worth revisiting if we later retune thresholds.
3. **Post-incident floor is flat** — the 06:00–09:00 window peaks at 79–156, well under the trigger. No crying, no false positives. Sleep window ended cleanly.

## 4. YAMNet FP32 ground truth — 22 WAVs

Separation between the daytime-bench set and the incident set is **total** on the cry class. Every bench WAV scores `baby_cry_infant < 0.35` and every incident WAV scores `> 0.88`.

### Bench-day WAVs (8 files — mostly speech)

| File | baby_cry | crying_sobbing | speech | screaming | note |
|---|---:|---:|---:|---:|---|
| 16:57 capacity-test | 0.20 | 0.39 | **1.00** | 0.00 | voice |
| 17:01 verify-round-2 | 0.04 | 0.08 | **1.00** | 0.01 | voice |
| 18:12 test-get-alias | 0.34 | 0.37 | **1.00** | 0.09 | voice |
| 18:20 living-room | 0.05 | 0.08 | **0.96** | 0.00 | voice |
| 19:02 auto-rms-8x-rms525 | 0.00 | 0.01 | **1.00** | 0.00 | voice test |
| 19:04 auto-rms-5x-rms337 | 0.01 | 0.03 | **1.00** | 0.00 | voice test |
| 19:19 auto-rms-11x-rms576 | 0.21 | 0.42 | **1.00** | 0.00 | voice test |
| 19:20 auto-rms-29x-rms1528 | 0.13 | 0.24 | **1.00** | 0.01 | voice test |

These were labelling and auto-trigger verification runs, not real crying. Labels reflect that: **all 8 are dominantly `speech`**, not cry. They are **not** useful as cry training data. They *are* useful as **negative examples** (speech / near-silence) for calibration.

### Incident WAVs (14 files — true cry)

| File | baby_cry | crying_sobbing | speech | screaming | trigger | note |
|---|---:|---:|---:|---:|---|---|
| 04:54 | 0.94 | 0.99 | 0.63 | 0.06 | auto-rms-6x-rms397 | first fire, breathing-room pre-peak |
| 04:55 | 0.92 | 0.99 | 0.42 | 0.42 | `cry` (phone) | phone-labeled, highest-quality |
| 05:00 | 0.97 | **1.00** | 0.21 | 0.02 | auto-rms-12x-rms765 | peak cry |
| 05:02 | 0.97 | **1.00** | 0.74 | 0.11 | auto-rms-9x-rms560 | mother + cry |
| 05:03 | 0.98 | **1.00** | 0.39 | 0.37 | auto-rms-19x-rms1247 | intense |
| 05:04 | 0.88 | 0.98 | 0.51 | 0.49 | `Cry2` (phone) | phone-labeled |
| 05:04 | 0.92 | 0.99 | 0.31 | 0.12 | auto-rms-14x-rms951 | |
| 05:06 | 0.93 | 0.99 | 0.16 | 0.02 | `Cry3` (phone) | trigger at rms 65 — tail of previous cry |
| 05:06 | 0.93 | 0.99 | 0.29 | 0.04 | auto-rms-14x-rms933 | |
| 05:07 | 0.92 | 0.99 | 0.19 | 0.00 | `Cry4` (phone) | trigger at rms 65 |
| 05:08 | 0.95 | **1.00** | 0.97 | 0.07 | auto-rms-22x-rms1435 | speech+cry overlay |
| 05:09 | 0.95 | **1.00** | 0.99 | 0.44 | `Cry5` (phone) | speech+cry, rms 106 |
| 05:10 | 0.95 | 0.99 | 0.09 | 0.00 | auto-rms-6x-rms377 | |
| 05:11 | 0.92 | 0.99 | 0.58 | 0.45 | auto-rms-23x-rms1538 | final burst |

**All 14 are dominantly cry.** 5 are phone-supervised ("cry", Cry2–5), 9 are auto-triggered. Auto and phone labels agree without exception.

## 5. Anomalies & flags

### 5.1 Audio capture overrun — pipeline is chronically losing bytes

| Metric | Final value |
|---|---|
| `audio_overrun_bytes` | 491,437,248 B (468 MB) |
| `audio_overrun_events` | 393,848 |
| Session elapsed | ~13 h |

That is **~30,300 overrun events/hour = ~8.4/s**, averaging ~1 KB dropped per event. The stream buffer backing the audio consumer cannot keep up with the producer; one consumer (SD writer? SSE fanout?) is blocking and the tap ring overflows. This has been running all night without triggering any visible functional failure — SD captures still happened, metrics look normal — but the data quality of WAVs may be affected by intermittent sample loss. **Root-cause this before the next overnight run.**

Likely suspects:
- `audio_capture.c` tap ring sized too small vs. producer rate when multiple taps exist
- SD card write latency spikes (FATFS cluster allocations) blocking the writer task
- SSE fanout lock holding the consumer loop past audio frame deadline

Action: add overrun-per-consumer breakdown next time, and compare WAV checksums against expected per-sample count.

### 5.2 Pre-incident "near-miss" spikes

23:00 → 03:00 window saw peak RMS 210–308 across 5 hours with zero auto-triggers. The 1500 ms sustain prevented fires on short transients — as designed. But these might still be worth capturing on the next run (e.g., lower `CRY_AUTO_TRIG_SUSTAIN_MS` to 800 or add a secondary "transient capture" 10 s mode) to fill in the background-noise negative class.

### 5.3 Bench WAVs are not cry examples

8 of 22 WAVs (36 % of the dataset) are speech/test recordings. If the dataset is fed to retraining naively these become noise. Filter by `yam_baby_cry_infant > 0.5` or exclude by filename timestamp (keep only `cry-20260419T*`).

## 6. Extractable data inventory

Everything needed for Stage 2.1 calibration is on disk locally:

- `logs/night-20260418/wavs/*.wav` — 22 files (28 MB)
- `logs/night-20260418/triggers.jsonl` — 22 trigger rows with phone/auto labels
- `logs/night-20260418/monitor.jsonl` — 730 device snapshots (1-min cadence)
- `logs/night-20260418/manifest.csv` + `.jsonl` — file-level numeric features
- `logs/night-20260418/segments.csv` + `.jsonl` — 1,804 × 0.96 s segment features
- `logs/night-20260418/yamnet_files.csv` — per-file FP32 YAMNet scores
- `logs/night-20260418/yamnet_segments.csv` — per-segment FP32 YAMNet scores
- `logs/night-20260418/specgrams/*.png` — 22 log-mel spectrograms

Device-side (still available for fetch until SD clears):
- 22 WAVs in `/sdcard/events/` (12,331-file ceiling, nowhere near)
- `triggers.jsonl` (identical to local copy)

## 7. Proposed next-flash punch list

(Device is not being reflashed until tomorrow's window. These items are already ranked; batch them.)

1. **SSE buffer fix** — already committed `ef60239`, not yet flashed. Restores live web UI.
2. **Audio overrun root cause** — instrument per-consumer drops so we know which tap is slow; likely requires a code-level look at the writer-task scheduling.
3. **NTP epoch + TZ in `/metrics`** — self-dating logs without host clock reliance.
4. **Nav/index page** — one HTML landing linking /metrics, /events/list, /record/status, /files/ls, /log/tail.
5. *(Optional)* TOCTOU guard on `event_recorder_trigger_manual` — double-tap protection.

## 8. Verdict

**Successful overnight collection.** Device was rock-steady for 13 hours, auto-trigger captured the full 18-minute incident with 14 segments + 5 phone-supervised labels, YAMNet FP32 gave unambiguous ground-truth on all 22 files. The Stage 2.1 calibration set now has a real-world seed of **~500 strong cry segments** (per earlier segment stats) plus 8 labelled negatives from the bench session.

The only functional concern is the audio overrun bookkeeping, which did not prevent capture but warrants investigation before the next run.
