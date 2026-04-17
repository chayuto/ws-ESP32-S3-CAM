# Deployment-01 log analysis — 8 h overnight monitor (2026-04-17 → 2026-04-18)

*Analysis of `logs/monitor-20260417.log`, 476 one-minute samples spanning 2026-04-17 23:12 through 2026-04-18 07:22. Parent reported multiple real crying events during the period; the detector fired zero alerts. This doc quantifies why.*

## Parent ground truth

> "how many cries detected? there were a lot of crying"

Device reported: **0 alerts.**

## Log parse summary

| Metric | Value | Interpretation |
|---|---|---|
| Rows parsed | 476 | 1/min, ~7.9 h |
| `alerts` total | **0** | Detector never fired |
| RMS median | 55 | Quiet-room baseline |
| RMS p95 | 170 | Elevated-but-common ambient |
| RMS max | **444** | Major acoustic event at 23:27:23 |
| `cry_conf` min | 0.563 | Noise floor of the model in silence |
| `cry_conf` max | **0.637** | Max signal the model produced all night |
| `cry_conf` range | **0.074** | **The entire operating range** of the model |
| Rows with RMS > 100 | 68 (14.3 %) | Consistent with an active room |
| Sustained RMS-elevated windows (≥ 2 min) | 14 | Strong candidates for real crying |

## Timeline of suspected crying events

Windows where RMS stayed > 100 for ≥ 2 consecutive minutes. Max RMS during the window and the model's best `cry_conf` reported in the same interval.

| Start (UTC+0) | Duration | Peak RMS | Mean `cry_conf` | Max `cry_conf` | Detected? |
|---|---|---|---|---|---|
| 00:15 | 2 min | 160 | 0.619 | 0.622 | ❌ |
| 00:20 | 3 min | 161 | 0.593 | 0.597 | ❌ |
| 00:25 | 3 min | 235 | 0.598 | 0.598 | ❌ |
| **00:40** | **9 min** | **210** | **0.608** | **0.622** | ❌ |
| 00:55 | 5 min | 209 | 0.603 | 0.610 | ❌ |
| **02:22** | **7 min** | **292** | **0.591** | **0.619** | ❌ |
| **02:37** | **3 min** | **367** | **0.606** | **0.610** | ❌ |
| 03:22 | 2 min | 152 | 0.585 | 0.603 | ❌ |
| **03:33** | **7 min** | **198** | **0.603** | **0.613** | ❌ |
| 03:45 | 3 min | 123 | 0.586 | 0.595 | ❌ |
| 04:38 | 2 min | 170 | 0.605 | 0.606 | ❌ |
| 06:28 | 3 min | 196 | 0.565 | 0.566 | ❌ |
| 06:39 | 2 min | 165 | 0.591 | 0.614 | ❌ |
| 06:45 | 2 min | 156 | 0.573 | 0.583 | ❌ |

Plus an outlier single-minute **peak at 23:27:23 with RMS 444 and cry_conf 0.637** — the max of the entire deployment.

Aggregate: **at least 51 minutes of elevated sound** across the night, no alerts.

## Top-20 RMS peaks (sorted)

| Time | Uptime (s) | RMS | `cry_conf` |
|---|---|---|---|
| 23:27:23 | 881 | **444** | 0.637 |
| 02:37:55 | 12312 | 367 | 0.598 |
| 02:22:53 | 11411 | 292 | 0.619 |
| 02:39:55 | 12432 | 238 | 0.610 |
| 00:27:37 | 4494 | 235 | 0.598 |
| 02:38:55 | 12372 | 232 | 0.610 |
| 02:52:57 | 13214 | 216 | 0.625 |
| 02:24:53 | 11531 | 215 | 0.597 |
| 00:45:39 | 5577 | 210 | 0.598 |
| 02:18:52 | 11170 | 210 | 0.595 |
| 00:57:41 | 6299 | 209 | 0.610 |
| 02:27:54 | 11711 | 203 | 0.597 |
| 04:45:11 | 19949 | 199 | 0.610 |
| 00:42:39 | 5396 | 198 | 0.610 |
| 03:38:03 | 15920 | 198 | 0.610 |
| 06:30:26 | 26264 | 196 | 0.563 |
| 06:28:26 | 26144 | 184 | 0.566 |
| 00:47:39 | 5697 | 183 | 0.606 |
| 01:00:41 | 6479 | 182 | 0.606 |
| 05:37:19 | 23076 | 181 | 0.582 |

## Headline finding — the model is insensitive to signal level

At 02:37:55, the acoustic energy in the room rose **~7×** (RMS 55 → 367). The model's class-20 `Baby cry, infant cry` output moved **+0.02** (from ~0.58 baseline → 0.598). That's within ordinary tick-to-tick noise of the model's output; it is not a response to the sound.

The 0.074 total operating range of the model across 8 h of widely varying ambient sound demonstrates that **`output[20]` is essentially a constant of ~0.58–0.64**, almost uncorrelated with what's actually in the mic. Threshold-based detection cannot work on this signal regardless of where we set the threshold.

## Cause

Documented in `docs/research/retraining-roi-analysis.md` §4:

1. **Synthetic INT8 PTQ calibration** using Gaussian random mel patches, not real audio. Activation ranges per layer are mis-scaled against real-distribution log-mel statistics, compressing the output distribution.
2. **Weak AudioSet labels** for class 20 → decision boundary is fuzzy even in the float baseline.
3. **Domain mismatch** — this room, this mic, this gain, ~nothing in the AudioSet training set matches.

All three compound. The model we shipped is effectively blind to crying in this deployment.

## Threshold calculus — why tonight's flash (0.65) still won't catch most cries

Across the entire 8 h deployment:

- Rows with `cry_conf` ≥ 0.65: **0 of 476**.
- Rows with `cry_conf` ≥ 0.63: **1** (the RMS=444 peak, single minute).
- Rows with `cry_conf` ≥ 0.62: **5** (0.9 % of samples, scattered).
- Rows with `cry_conf` ≥ 0.60: **~100** (21 % of samples, mostly quiet-room).

With the new 0.65 threshold + 6-frame smoothing requirement, the detector would still **fire zero alerts** over this exact deployment. Dropping further to 0.60 would fire constant false alarms during quiet periods where cry hovers at 0.59.

**There is no threshold on the existing model that catches crying without firing on silence.** The cry and silence distributions overlap almost completely in `cry_conf` space.

## What actually has to happen next

Stage 2.1 (real-audio INT8 recalibration) is no longer a cleanup — it is the **only blocker** preventing the bedroom-monitor use case from working.

Concretely, tomorrow's bench session:

1. Harvest SD card. `/sdcard/cry-20260418.log` contains the same data at 30-s granularity; crossreference against this monitor analysis to confirm correlation.
2. Flash tomorrow's code batch (threshold 0.65, stack canary, multi-class monitor, 20-class SD columns). **Even if it detects nothing more**, Stage 2.6's multi-class logging will reveal whether the cry-spectrum union (classes 19+20+21+22) shows more separation than class-20 alone. If it does — maybe we squeak by. If it doesn't — 2.1 is unavoidable.
3. Collect real-audio training data via `POST /rec/trigger` (forthcoming Stage 2.7 file-API). Need at least ~300 WAV clips of this actual bedroom (mix of silence and ambient), plus 50+ real cry clips.
4. Re-run `convert_yamnet.py --audio-dir <collected>` on the host. Expected outcome: `cry_conf` distribution opens up from 0.07 range to something like 0.6–0.9 range on silence-vs-cry. Threshold 0.75 becomes usable with a meaningful margin.

Until 2.1 lands, **this monitor is not a functional baby-cry detector.** The infrastructure works — audio capture, mel extraction, tens of thousands of successful inferences, web UI, SD log, mDNS, LED state machine, Wi-Fi resilience, NTP sync — but the ML pipeline at the end is effectively flatlined.

This is a useful lesson: our 6-hour end-to-end green-light yesterday was a **system-integration green light, not a detection green light**. The two are completely separate, and we had no way to tell them apart without real-world ground truth. That's exactly what this deployment produced. The cost of not catching this earlier was also zero — the device stayed quiet, no false alarms, just a log we can now act on. Worst case for the infrastructure-first approach.

## Artefact

- Monitor log: `logs/monitor-20260417.log` (committed via gitignore — kept locally only, can exfiltrate rows to the committed `/docs/research/` path if a reviewer wants).
- SD log (on the device): `/sdcard/cry-20260417.log` and `/sdcard/cry-20260418.log`. Higher-resolution version of the same data (30 s vs 60 s). Pull with tomorrow's bench session.
- Event recordings: **none — the event_recorder is triggered by the detector, which didn't fire.** Another argument for the Stage 2.7 `/rec/trigger` endpoint so we can force-record during suspected cry windows.
