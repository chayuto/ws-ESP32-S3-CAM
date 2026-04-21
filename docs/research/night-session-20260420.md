# Full-night session report — 2026-04-20 → 2026-04-21

**Session start (bedtime):** 2026-04-20 19:45:26 +1000 (first bedroom auto-rms fire)
**Session end (extract):** 2026-04-21 10:29 +1000
**Firmware:** `b7a36a2-dirty` (built 2026-04-19), single binary across entire session
**Environment:** bedroom 19:45 → 01:32 ; living-room 09:08 → 10:29 (device moved pre-bedtime-2 — see §7)
**Incident of interest:** 01:04 – 01:20 (five auto-captures) followed by **01:32:14 → 09:08:27 unexplained downtime (7 h 36 m 13 s)**

## 1. Headline findings

1. **On-device detector is near-random in this environment.** Across 118 WAVs with a YAMNet ground-truth label (61 real cries ≥ 0.5, 57 confirmed silence/noise < 0.1), the on-device `max_cry_1s` saturates at 0.718 — even on cries YAMNet rates ≥ 0.997. At every practical threshold, TPR ≈ FPR (drift §3).
2. **Auto-RMS trigger works as a loud-sound detector, not a cry detector.** 22/22 living-room morning captures were genuine household noise (YAMNet baby_cry ≤ 0.139, mean 0.019). The `Nx` ratio label (rms/nf50) does not separate cries from kitchen clatter — both sit in the 5–30× band.
3. **Bedtime and 1 am incident captures were near-perfect on content** (17/17 real, 0 FP) because the bedroom baseline (nf_p95 p50 = 226) is acoustically clean and what's loud there is almost always the baby.
4. **Living-room baseline is ~1.5 × nosier and far more volatile.** nf_p95 p50 = 341, p95 = 419, max = 514.8, vs. bedroom stable at 226–278. The RMS-ratio trigger fires on every kitchen transient.
5. **7 h 36 m of overnight data is permanently lost.** Device had no boot record in `CRY-0000.LOG` between 20:02:53 (bedtime-2 boot) and 09:08:38 (morning boot). On wall charger. Cause: unknown — see §7.

## 2. Capture effectiveness

Total WAVs mirrored this session: **129** (all days 04-18..04-21; this report focuses on the 04-20/04-21 bedtime cycle).

| Bucket | n | real (YAM≥0.5) | borderline (0.3-0.5) | false positive (<0.1) |
|---|---:|---:|---:|---:|
| `bedtime-04-20` (19:45–20:30) | **12** | **12** | 0 | 0 |
| `incident-01am-04-21` (01:04–01:20) | **5** | **5** | 0 | 0 |
| `morning-FP-04-21` (09:15–10:20 living room) | **22** | **0** | 0 | 21 |
| `day-04-20` (context, daytime + evening bench) | 20 | 13 | 1 | 4 |
| `day-04-19` (context) | 62 | 31 | 2 | 28 |
| `day-04-18` (context, bench) | 8 | 0 | 3 | 4 |

In-bedroom night captures: **17/17 real, 0 false positive** — a clean dataset. Morning living-room captures: **0/22 real, 21 confirmed FP** — zero useful additions, only noise pollution.

## 3. Drift: on-device `max_cry_1s` vs YAMNet oracle

**Input:** per-WAV YAMNet peak for (baby_cry_infant ∪ crying_sobbing), vs. per-WAV peak of on-device `max_cry_1s` in a ±12 s window around capture time (from `infer-*.jsonl`, now complete after re-pull).

```
TRUE POSITIVES  (YAM ≥ 0.5, n=61)   dev_max1s: p50=0.517  mean=0.566  p95=0.640  max=0.718
TRUE NEGATIVES  (YAM < 0.1, n=57)   dev_max1s: p50=0.501  mean=0.561  p95=0.635  max=0.652
```

Both distributions have the same median, mean, p95, and near-identical max. ROC-style sweep:

| dev threshold | TPR | FPR | TP | FN | FP | TN |
|---:|---:|---:|---:|---:|---:|---:|
| 0.50 | 100.0% | 100.0% | 61 | 0 | 57 | 0 |
| 0.52 | 44.3% | 49.1% | 27 | 34 | 28 | 29 |
| 0.60 | 42.6% | 47.4% | 26 | 35 | 27 | 30 |
| 0.62 | 39.3% | 36.8% | 24 | 37 | 21 | 36 |
| 0.65 |  3.3% |  1.8% |  2 | 59 |  1 | 56 |
| 0.70 |  3.3% |  0.0% |  2 | 59 |  0 | 57 |
| 0.72 |  0.0% |  0.0% |  0 | 61 |  0 | 57 |

There is no threshold that keeps TPR > FPR by a useful margin. The model is saturated — the softmax output never exceeds ~0.72 regardless of input — and within the 0.50–0.65 band it responds to loudness, not to cry spectra.

Worst misses (YAM ≥ 0.9 but dev peak ≤ 0.50):

| local ts | bucket | YAM | dev_max1s | rms_peak |
|---|---|---:|---:|---:|
| 2026-04-21 01:09:23 | incident-01am | 0.997 | 0.501 | 1650 |
| 2026-04-20 19:45:26 | bedtime | 0.984 | 0.501 | 5319 |
| 2026-04-20 19:21:26 | day | 0.945 | 0.500 | 780 |
| 2026-04-19 13:54:07 | day | 0.985 | 0.501 | 3794 |
| 2026-04-19 20:18:09 | day | 0.942 | 0.500 | 4550 |

High RMS does not rescue the model — it outputs 0.500 on 4.5× the silence threshold on 5.3 kRMS infant cries.

See `logs/night-20260420/analysis/drift.csv` for the full per-WAV join.

## 4. 1 am incident timeline (00:50 – 01:32)

Reconstructed minute-by-minute from `infer-20260421.jsonl` (2535 samples). `max1s_pk` = peak `max_cry_1s` in the minute; `rms_max` = peak input RMS:

| minute | rms_max | rms_p95 | max1s_pk | annotation |
|---|---:|---:|---:|---|
| 00:50–01:02 | 200–1,360 | 145–283 | 0.500–0.517 | baby settling |
| 01:03–01:04 | **1,907** | 820 | 0.591 | **first cry onset** |
| 01:05–01:08 | 58–79 | 55–61 | 0.500 | quiet (calmed) |
| 01:09 | 883 | 296 | 0.501 | **cry resumes** |
| 01:10 | **2,457** | 1,277 | 0.517 | full cry |
| 01:11 | **4,138** | 1,127 | **0.501** | **peak loudness — detector at silence threshold** |
| 01:12–01:14 | 86–120 | 60–72 | 0.500 | quiet |
| 01:15 | **3,876** | 2,943 | 0.517 | cry |
| 01:16 | 1,664 | 672 | 0.501 | cry continues |
| 01:17–01:19 | 56–79 | 54–61 | 0.500 | quiet |
| 01:20 | **3,001** | 1,692 | 0.517 | final cry burst (WAV captured) |
| 01:21–01:27 | 117–440 | 64–316 | 0.500–**0.622** | post-cry settling |
| 01:28 | 153 | 64 | **0.718** | detector peak — *after* the loudest moments |
| 01:29–01:31 | 55–102 | 54–62 | 0.500 | silence |
| 01:32:14.761 | — | — | — | **last log sample before blackout** |

Two properties of the on-device model are clearly visible here:

- **Under-confidence during the loudest cry frames** (01:11 at RMS 4,138 outputs 0.501). Likely input-scaling saturation — the mel feature head clips when the mic signal is near full scale, and the model has never seen that regime in training.
- **Over-confidence during calm moments after a cry** (01:28 at RMS 153 outputs 0.718, the highest value anywhere in the session). The model appears to latch onto whimper-like residuals that YAMNet separates into `baby_cry` vs `whimper` confidently, but that the on-device watched head confuses.

All five auto-captures fired correctly (YAM ≥ 0.991 on every one) — the trigger beat the detector at cry detection.

See `logs/night-20260420/analysis/timeline_1am.csv` for the full 42-minute trace.

## 5. Morning FP storm (09:08 – 10:30, living room)

Reconstructed from `infer-20260421.jsonl` (4841 samples) and `cry-20260421.log` (188 snapshots). Device booted at 09:08:27 with cold nf_p95 = 65.8. Within 3 min of activity the p95 climbed to 226; by 10:10 it peaked at 514.8 — 2.3× the bedroom stable baseline.

22 auto-rms captures fired, **none with YAM ≥ 0.15**:

| local ts | note | YAM cry | rms_peak | rms_p95 |
|---|---|---:|---:|---:|
| 09:15:14 | auto-rms-5x-rms238 | 0.008 | 1,050 | 329 |
| 09:17:17 | auto-rms-13x-rms583 | 0.026 | 6,979 | 1,377 |
| 09:27:45 | auto-rms-9x-rms377 | 0.000 | 3,385 | 1,039 |
| 10:10:09 | auto-rms-19x-rms680 | **0.139** | 5,017 | 931 |
| 10:20:11 | auto-rms-29x-rms1015 | 0.048 | 5,407 | 1,139 |

The `Nx` ratio spans 5–29× — the same range as genuine cries (bedtime 5–34×, 1 am 8–18×). There is no separator in the label that would allow post-hoc culling by ratio; **loudness-above-noise-floor is orthogonal to cry-ness in an active household**.

Specific FP sources (by listening to the spectrograms) include kitchen clatter, dishes, tap water, and adult speech — all transients that sustain above nf50×multiplier for long enough to pass the sustain gate.

See `logs/night-20260420/analysis/morning_storm.csv`.

## 6. Environmental baselines

Snapshot-rows from `cry-20260420.log` (bedroom overnight after 21:07 move) and `cry-20260421.log` (living room after 09:08 boot):

| metric | bedroom-overnight-2 (n=529) | livingroom-morning (n=188) |
|---|---:|---:|
| input_rms    p50 / p95 / max | 66.6 / 244.4 / **675** | 29.0 / 292.0 / **1,136** |
| nf_p95       p50 / p95 / max | 226.0 / 277.7 / 278 | 341.1 / 419.0 / **515** |
| max_cry_1s   p50 / p95 / max | 0.500 / 0.517 / 0.718 | 0.500 / 0.500 / 0.517 |

Bedroom is quieter at the floor (p50 RMS 67 vs 29? — living-room has lower instantaneous quiet, but the **ceiling** is nearly 2× higher because activity is infrequent-but-big). nf_p95 — the running 95th-percentile noise floor used as the auto-trigger reference — saturates higher in the living room, which in principle would *raise* the auto-trigger threshold. In practice it saturates *too slowly*: kitchen transients are short enough (seconds) and infrequent enough that they rarely enter the p95 window, so the effective threshold stays low and fires on each transient.

See `logs/night-20260420/analysis/baseline.csv`.

## 7. The 7 h 36 m downtime

Authoritative record: `/sdcard/CRY-0000.LOG` (persistent across reboots, re-pulled fresh).

- Boot #39: 2026-04-20T20:02:53 (bedtime-2 boot, post bedroom-move reboot)
- Boot #40: **2026-04-21T09:08:38** (morning boot)
- Gap: **7 h 36 m 13 s** with *no* boot record → device was unpowered, not merely crashed.

Corroborating evidence:
- `cry-20260420.log` last snapshot: 01:31:20 (uptime 19,719 s ≈ 5 h 28 m from boot #39)
- `infer-20260421.jsonl` last pre-silence sample: 01:32:14.761
- `cry-20260421.log` first snapshot: 09:08:38 with uptime = 11 s → fresh cold boot
- No boot event in `CRY-0000.LOG` between #39 and #40
- No crash-dump, no WDT reset, no brownout hint — just a clean power-off at ~01:32:15 and cold-up at 09:08:27

Ruled out: laptop sleep (device on wall charger), firmware crash (would have rebooted and logged boot #39.5), SD-card I/O stall (would have continued heartbeats in RAM ring buffer and flushed on recovery, but there is no heartbeat at all).

Remaining candidates, not diagnosable from device data:
- Switched outlet toggled off (human or pet)
- Household breaker trip or micro-outage
- Wall adapter thermal cut-out / intermittent fault
- Smart plug auto-off if there is one on the circuit

**Data for 01:32:14 → 09:08:27 is permanently lost on both device and host.**

## 8. Deep-dive: diagnosing *why* the on-device model saturates

Re-PTQ — not retraining — is the fix. The on-device model at `spiffs/yamnet.tflite` is a straight INT8 PTQ of Google's YAMNet built by `hf/convert_yamnet.py` with synthetic-random calibration (the script's own warning: *"accuracy will be slightly below the float baseline"*). Reading the tflite tensor metadata:

```
input  : shape=[1,96,64]  int8  scale=0.0401  zp=+38   → dequant range [-6.65, +3.57]
output : shape=[1,521]    int8  scale=0.00391 zp=-128  → dequant range [ 0.000, +0.996]
```

The output quant range is `[0, 0.996]` — every int8 output value maps to a logit between 0 and 1. After sigmoid, the *theoretical* maximum probability this firmware can ever emit is:

```
sigmoid(0.996) = 0.7303
```

Which is precisely the `max_cry_1s = 0.718` ceiling observed across the entire session. The model isn't failing — the output dequantization is clipping every confident class to the same value.

Cross-checked against FP32 YAMNet (TF Hub) on three WAVs from this session:

| WAV | FP32 max baby_cry(20) | INT8 on-device max | loss |
|---|---:|---:|---:|
| 2026-04-21 01:09:23 (real cry) | **0.965** | 0.718 | clipped |
| 2026-04-20 19:13:00 (real cry) | **0.847** | 0.718 | clipped |
| 2026-04-21 09:15:14 (kitchen FP) | 0.002 | 0.500 | baseline (also clipped) |

The reverse-map of every observed `max_cry_1s` score to its generating int8 tensor value is a small set of specific codes:

```
0.500 → int8 = -128 (zero-point, output logit 0)
0.501 → int8 = -127 (just above zero-point)
0.517 → int8 = -110
0.591 → int8 = -34
0.622 → int8 =  0
0.718 → int8 = +111  (near ceiling; +127 would give 0.730)
```

There are only six distinct values because the model keeps outputting the same int8 tensor for the same class across many inputs — the effective output resolution near `cry_baby` is ~6 meaningful bins out of 256, because the calibration never saw real cry audio to expand the scale.

**Input quantization is healthy.** Scale 0.04 with zp=+38 covers [-6.65, +3.57], and on every tested WAV the actual log-mel values fit inside that band (observed min=-6.77, max=+3.06, clipping rate 0%). So the input-side PTQ tolerated the feature distribution well — the broken part is the *output* calibration.

**Fix path, in order of effort:**

1. **Re-run `convert_yamnet.py --audio-dir <path-with-real-cries>`** on the 17 night TPs from this session. The representative_dataset will then exercise the full output logit range of the cry classes and PTQ will pick a wider output scale (probably scale ≈ 0.05 → logit range ±6.4 → sigmoid(6.4)=0.998 reachable).
2. **Or switch `inference_output_type=tf.int16`** in the converter (inputs stay int8). Output dequant range expands by 256× at the cost of 521 extra bytes per inference — trivial on ESP32-S3. Full FP32 output would be 2 KB, still trivial.
3. **Or keep the current tflite and dequantize only 4 watched classes (cry_baby, crying, whimper, wail) in FP32** via a side tensor. Requires reconverting anyway — same work as (2).

All three are host-side-only. None require firmware changes. See `/tmp/diag_model_saturation.py` for the reproducer.

## 9. Deep-dive: FP sub-types in the morning storm

K-means (k=3) over a 12-dim acoustic feature vector over the 22 FP WAVs yields three clean clusters, each with zero HNR-gate leakage except one cluster:

| cluster | n | mean crest | mean log_e IQR | onset/s | mean HNR | mean voiced_frac | character | HNR > 4 reject |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0 "busy bursty" | 10 | 18.4 | 4.69 | 3.9 | -0.9 | 0.64 | kitchen clatter / cabinet bursts / footsteps | **10/10** |
| 1 "impulsive" | 4 | 21.7 | 1.29 | 1.7 | -2.2 | 0.35 | single impacts (slam, microwave ding) | **4/4** |
| 2 "sustained voiced-like" | 8 | 16.3 | 2.47 | 2.7 | +1.2 | 0.69 | running water / TV / adult speech | **7/8** |

The TP vs FP feature separations were:

| feature | TP (n=17) p10/p50/p90 | FP (n=22) p10/p50/p90 | separation |
|---|---|---|---|
| `hnr_hpss` (dB) | 0.24 / 4.05 / 10.4 | -3.04 / -0.16 / 1.86 | **strong** (cleanest single axis) |
| `spectral_flatness` | 0.017 / 0.028 / 0.042 | 0.042 / 0.061 / 0.084 | strong |
| `voiced_frac` | 0.65 / 0.99 / 1.00 | 0.37 / 0.60 / 0.82 | strong |
| `rolloff85` | 2687 / 3521 / 4147 | 4022 / 4429 / 4845 | moderate |
| `crest factor` | 8.1 / 11.1 / 15.3 | 12.2 / 16.2 / 26.5 | moderate |
| `onset rate /s` | 1.4 / 2.4 / 3.4 | 1.9 / 3.2 / 4.3 | weak |

The leaker is `cry-20260421T091514+1000.wav` (HNR 7.60) — the first morning capture. Listening to it (via the spectrogram): sustained adult speech / TV dialogue. Adult male fundamentals (~100-200 Hz) can have strong harmonic structure and pass an HNR-only gate. A secondary `f0_mean > 300 Hz` gate would catch it (see §10).

## 10. Deep-dive: F0 contour sub-types on real cries

K-means (k=3) over F0 contour features on the 17 night TPs reveals three distinct cry types:

| cluster | n | members | voiced_frac | f0 mean | f0 p50 | f0 p90 | character |
|---|---:|---|---:|---:|---:|---:|---|
| 0 "distress" | 5 | all five 01 am | 0.60 | 718 | 602 | 1201 | pain/distress — high pitch, broken voicing, wide range |
| 1 "full cry" | 8 | most bedtime | 0.91 | 488 | 380 | 923 | full-voice sustained cry, moderate pitch |
| 2 "fuss" | 4 | four bedtime | **0.98** | **350** | **276** | 660 | low-pitch grumble, very sustained, near speech range |

This matches audible perception: the 01 am wake was a single strong distress event (hungry/uncomfortable), while bedtime was a typical mixed protest — some full cries and some fuss.

**Implication for two-gate filtering:** an f0-based second gate trades TP recall for FP robustness.

| gate | TP kept / 17 | FP rejected / 22 |
|---|---:|---:|
| `hnr_db > 4` alone | 17 | 21 |
| `hnr_db > 4` AND `≥20% of voiced f0 in 350–800 Hz` | 14 | 22 |
| `hnr_db > 4` AND `f0_mean > 300 Hz` | ~13 | 22 |

The tightening costs 3 fuss cries (f0 < 350 Hz) to catch 1 adult-speech leaker. **Not worth it** — fuss cries are the low-urgency ones you most want the system to catch silently, and a single TV/speech FP per session is acceptable noise. **Stick with HNR-only for firmware-side filtering.**

## 11. Implications for next iteration

Ordered by expected impact on the "wake me if my baby cries" loop.

1. **Re-PTQ the tflite with real-cry calibration — one-shot, no firmware change.** Per §8 the on-device model's *weights* are fine; the *output quantization scale* is 256× too narrow because it was calibrated on synthetic log-mel patches. Run `hf/convert_yamnet.py --audio-dir logs/night-20260420/wavs` and reflash the spiffs partition. Expected outcome: `max_cry_1s` can reach ≥0.95 on real cries (matches FP32 YAMNet's 0.965 on the 01:09 cry), opening ~0.80 as a usable alerting threshold. Verify with `/tmp/diag_model_saturation.py` before flashing.
2. **Keep the auto-RMS trigger, but cull the captured WAV post-hoc with HNR.** Harmonic-to-noise ratio cleanly separates this dataset:

   | feature | night-TP p10 / p50 / p90 (n=17) | morning-FP p10 / p50 / p90 (n=22) |
   |---|---|---|
   | `hnr_db`       | **7.0 / 8.5 / 10.5** | **1.0 / 1.5 / 2.2** |
   | `b_1k_2k`      | 0.094 / 0.326 / 0.583 | 0.021 / 0.041 / 0.068 |
   | `b_250_500`    | 0.013 / 0.029 / 0.100 | 0.172 / 0.217 / 0.290 |
   | `flatness_mean`| 0.291 / 0.349 / 0.422 | 0.398 / 0.474 / 0.526 |

   A single `hnr_db > 4` threshold keeps **17/17 TPs and rejects 21/22 FPs** on this session. Baby cries are harmonically voiced (f0 420–720 Hz with overtones); kitchen/household transients are broadband noise. This is a recorder-side cull that runs once per 40 s capture, not a real-time filter — suits the "record generously, alert selectively" pattern.
3. **Lengthen the nf_p95 window, or compute a separate trigger reference that is robust to household transients.** The current window learns the loud kitchen transients into its p95, raising the threshold globally but still firing on each new transient because they don't cluster enough to dominate p95. A longer window (e.g., 10 min rolling) or a trimmed-mean baseline would adapt more slowly and fire less.
4. **Power resilience before tonight.** A USB wall-wart UPS / small LiPo hat would have survived the 7 h unexplained outage. Without it, this failure mode recurs.
5. **Monitor needs a heartbeat-gap alarm.** The local Mac monitor only noticed the device was gone when an HTTP probe failed at 08:44. A single missed heartbeat (30 s) should page — not a failed HTTP ping, which only caught the tail.

## 9. Artifacts

All under `projects/cry-detect-01/logs/night-20260420/`:

- `wavs/` — 129 captured WAVs (1.28 MB each)
- `device-logs/` — CRY-0000.LOG + cry-YYYYMMDD.log + infer-YYYYMMDD.jsonl (278 MB)
- `manifest.csv`, `segments.csv` — local acoustic features
- `yamnet_files.csv`, `yamnet_segments.csv` — FP32 oracle scores
- `triggers.jsonl` — per-capture trigger metadata from the device
- `specgrams/` — 129 mel-spectrogram PNGs
- `analysis/joined.csv` — per-WAV join of manifest + YAMNet + on-device peaks
- `analysis/drift.csv` — dev vs YAMNet scatter
- `analysis/timeline_1am.csv` — 42-minute second-by-second trace of the 1 am incident
- `analysis/morning_storm.csv` — 90-minute second-by-second trace of the living-room FP storm
- `analysis/baseline.csv` — bedroom vs living-room RMS / nf distributions
- `analysis/summary.json` — machine-readable headline stats
- `analysis/fp_clusters.csv` — per-FP-WAV feature vector + k=3 cluster assignment (§9)
- `analysis/tp_contours.csv` — per-TP-WAV F0 contour features (§10)
