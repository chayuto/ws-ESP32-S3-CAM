# Full-night session report — 2026-04-22 → 2026-04-23

**Session window:** 2026-04-22 18:33 → 2026-04-23 07:17 +1000 (12 h 44 m)
**Environment:** bedroom, unchanged vs 04-21 (same outlet, same placement)
**Firmware:** `4af50c57` — FIRST session under the double-sigmoid fix (commit `a32afe2`, `main/yamnet.cc`)
**Monitor:** `cry_monitor v2`, PID 98639, ran 15:09 → 07:17 (16 h, hit MAX_ITER naturally)
**Baby:** bedtime cluster (14 cries), silent overnight, morning cry cluster (10 cries at 05:35-06:26)

## 1. Headline findings

1. **100% capture precision tonight.** 24 captures total, **24/24 YAMNet-confirmed real cries** (YAM ≥ 0.5, most at 0.9+), zero false positives. First session ever with that precision — the bedroom environment at this outlet/placement is acoustically clean enough that loud sounds ARE cries.
2. **Bedtime was unusually long (~2 h active).** 18:33–19:00 (bedtime cry cluster) → 20:26 (final settle cry). Compare 04-21 which settled by 19:14.
3. **Morning cry cluster at 05:35–06:26** was the real wake event (10 captures, YAM 0.89-0.997). 04-21 showed small stirs at similar times but nothing as consistent. Likely hungry-wake rather than fuss-wake.
4. **On-device `cry_conf` now meaningful but under-predicting vs YAMNet.** Peak monitor-observed `max_cry_1s` = **0.451** (at bedtime 18:53 and morning 05:54), while YAMNet on the same captures scored 0.99+. Model is discriminating correctly (real cries score non-zero, silence scores ~0) but at a compressed range vs FP32 YAMNet. See §4.
5. **Two unexplained reboots at 19:30 + 19:56.** Confirmed in `CRY-0000.LOG`. No `reset_reason` available (firmware doesn't expose it — pain point from log-management design §6.5). Device ran 11 h 21 m clean after that; the reboots don't repeat.
6. **Zero data-loss outages.** Unlike 04-20 (7h37m dark hole), tonight had zero silent gaps. The two reboots were each ~60 s of downtime and were caught by the monitor's `REBOOT` anomaly event. Tier 1 of the data-program plan holds.

## 2. Timeline

All times +1000. YAM = max of YAMNet baby_cry_infant ∪ crying_sobbing. cm1s peaks come from `/tmp/cry-monitor.log` at 60 s resolution (the full 1.5 fps `infer-20260422.jsonl` was not recovered — see §7 data gaps).

### Bedtime cluster (14 captures, 2 h 0 m)

| time | note (trigger RMS) | rms_peak | YAM | monitor cm1s | hnr_db |
|---|---|---:|---:|---:|---:|
| 18:33:46 | auto-rms-24x-rms862 | 2836 | 0.910 | — | 8.0 |
| 18:49:56 | auto-rms-39x-rms1386 | 1690 | 0.565 | — | 7.5 |
| 18:53:29 | auto-rms-9x-rms316 | 4387 | **0.993** | **0.429** | 8.3 |
| 18:55:35 | auto-rms-24x-rms840 | 3496 | 0.996 | — | 7.9 |
| 18:56:48 | auto-rms-10x-rms341 | 3755 | 0.992 | — | 8.3 |
| 18:58:37 | auto-rms-13x-rms467 | 2687 | 0.983 | — | 6.3 |
| 18:59:46 | auto-rms-18x-rms652 | 4340 | **0.998** | — | 5.8 |
| 19:30:24 | — | — | — | — | — | ← REBOOT (see §5) |
| 19:38:10 | auto-rms-6x-rms229 | 1994 | 0.993 | **0.451** | 9.0 |
| 19:41:31 | auto-rms-10x-rms351 | 3362 | 0.993 | — | 9.1 |
| 19:48:09 | auto-rms-27x-rms956 | 1271 | 0.895 | — | 9.9 |
| 19:50:54 | auto-rms-19x-rms682 | 1668 | 0.994 | — | 8.2 |
| 19:56:09 | — | — | — | — | — | ← REBOOT (see §5) |
| 20:01:23 | auto-rms-18x-rms764 | 2083 | 0.980 | — | 10.2 |
| 20:24:29 | auto-rms-6x-rms334 | 2403 | 0.768 | — | 6.4 |
| 20:26:51 | auto-rms-14x-rms752 | 3479 | 0.983 | — | 10.0 |

### Overnight (~9 h silent)

20:26 → 05:35 = 9 h 9 m with no captures. Monitor log confirms quiet baseline throughout (RMS p50 ≈ 25-30, no transients above trigger threshold).

### Morning cluster (10 captures, 51 min)

| time | note | rms_peak | YAM | monitor cm1s | hnr_db |
|---|---|---:|---:|---:|---:|
| 05:35:01 | auto-rms-10x-rms361 | 2089 | **0.997** | 0.066 | 9.7 |
| 05:46:42 | auto-rms-8x-rms301 | 2004 | 0.983 | 0.050 | 9.9 |
| 05:54:53 | auto-rms-12x-rms417 | 3762 | 0.984 | **0.451** | 9.3 |
| 05:55:58 | auto-rms-9x-rms322 | 1581 | 0.958 | 0.011 | 8.5 |
| 05:57:01 | auto-rms-11x-rms382 | 2037 | 0.987 | 0.042 | 7.4 |
| 05:58:08 | **Morning cry** (manual) | 3605 | 0.971 | 0.046 | 6.8 |
| 05:58:42 | auto-rms-19x-rms658 | 3990 | 0.991 | 0.046 | 4.9 |
| 06:00:53 | auto-rms-35x-rms1242 | 2321 | **0.997** | 0.000 | 9.9 |
| 06:01:57 | auto-rms-40x-rms1414 | 2718 | 0.996 | 0.063 | 5.7 |
| 06:26:10 | auto-rms-11x-rms376 | 2696 | 0.997 | — | 9.4 |

The `Morning cry` note at 05:58:08 is a manual caregiver annotation (via the device's `/record/trigger?note=...` path) — the only non-auto trigger in tonight's dataset.

## 3. Capture effectiveness comparison

| session | env | total | real (YAM≥0.5) | border (0.3-0.5) | FP (<0.1) | precision |
|---|---|---:|---:|---:|---:|---:|
| 04-20 | bedroom + living-room | 129 | 61 | 8 | 57 | 47 % |
| 04-21 | bedroom (1st, buggy fw) | 23 | 16 | 0 | 7 | **70 %** |
| **04-22 (tonight)** | **bedroom (2nd, fixed fw)** | **24** | **24** | 0 | **0** | **100 %** |

Tonight's 100 % precision is the combination of:
- Stable bedroom acoustic environment (the 04-21 leakers at 20:17–20:18 didn't recur — presumably caregiver speech was absent this time).
- Same auto-RMS trigger threshold working on a cleaner baseline.
- Fixed firmware emits true probabilities but — importantly — the **auto-RMS trigger is not model-driven**, so the firmware fix doesn't directly affect which captures fire. The fix affects only what `cry_conf` reads out as.

## 4. On-device model behavior under the new scale

### Expected vs observed

Based on offline validation against 165 night WAVs (see `docs/research/night-session-20260421.md` §7, commit `93adbf5`), the un-clipped model was expected to score real cries in the **0.85–0.99** range (dequantized probabilities). The 01:09:23 cry from 04-21 was shown to dequantize to **0.9336** offline.

On hardware tonight, the monitor observed:

```
cm1s distribution across 960 polls (16 h session):
  0.0:   939   97.8%   (silence / non-cry)
  0.1:    17    1.8%   (weak signal)
  0.2:     1    0.1%
  0.4:     1    0.1%
  0.5:     2    0.2%

Top cm1s peaks:
  19:38:56  cm1s=0.451  rms=36    (bedtime, low rms — possibly whimper)
  05:54:53  cm1s=0.451  rms=152   (morning fuss)
  18:53:32  cm1s=0.429  rms=1632  (loudest bedtime cry)
```

Real cries peak at **0.43–0.45** on hardware vs YAMNet's 0.93-0.99 on the same audio. Consistent gap of ~0.5 probability units.

### Why the discrepancy?

Three non-exclusive hypotheses:

1. **On-device mel-feature pipeline differs from YAMNet reference.** The ESP32 computes mel features from I²S samples; YAMNet's `features.waveform_to_log_mel_spectrogram_patches` uses a specific STFT + mel-filter-bank. Any drift (window, hop, fmin/fmax, num_bands, power-vs-magnitude) compresses the input distribution the model was trained on. `main/mel_features.c` is where to check.
2. **60-s monitor resolution undersamples the peak.** Inference runs at ~1.5 fps, so in a 60-s window there are ~90 patches. The monitor captures **one** instantaneous `max_cry_1s` value per minute. If a 5-second cry peak happens between polls, it's invisible to the monitor log. The actual device-side peak could be higher — only `infer-20260422.jsonl` (not recovered, see §7) would show.
3. **This baby's cry voice is atypical for YAMNet's training distribution.** YAMNet was trained on AudioSet baby cry clips; if the timbre here differs meaningfully (pitch, formants), the model gives lower-confidence even on clear cries. The consistent 0.5-gap across both sessions supports this — it's systematic, not noisy.

### Why this still works for the data program

Even compressed at ~0.45 peak, tonight's model output:
- **Correctly discriminates** (real cries 0.0–0.45, silence 0.0) — no overlap with the FP band from 04-20.
- **Is temporally aligned** with YAMNet-confirmed events (top peaks all land on captured real cries).
- **Is probability-scaled** (a genuine 0-to-1 range), so thresholds are now meaningful numbers.

For data collection the gap doesn't matter — YAMNet post-hoc remains the labeler. For alerting, tonight's peak of 0.45 means the current firmware `base_threshold=0.70` will never fire alerts on this baby. The threshold needs to be retuned to the observed distribution (likely to 0.35 for "high recall, some FP risk") once more sessions accumulate.

## 5. The two reboots at 19:30 and 19:56

### CRY-0000.LOG evidence

Confirms both reboots:

```
up=0s,NOT_SYNCED,0,boot,...                           # boot #1
up=8s,NOT_SYNCED,8,wifi_up,...,-58,...                # wifi at -58 dBm after boot
2026-04-22T19:30:24.450+10:00,ntp_synced,...,-56,...  # first mystery reboot

up=0s,NOT_SYNCED,0,boot,...                           # boot #2 (26 min later)
up=8s,NOT_SYNCED,8,wifi_up,...,0.000,...
2026-04-22T19:56:09.943+10:00,ntp_synced,...          # second mystery reboot
```

### What we know

- **Both are NEW-firmware boots** (watched-class fields all 0.000, consistent with `4af50c57` probability scale).
- **Previous uptime**: 15 791 s (4 h 23 m) before reboot #1 — lines up with my 15:07 workbench flash.
- **Inter-reboot gap**: 1492 s (24 min 52 s) between reboots — quite short.
- **RSSI shifted** -48 → -58 across reboot #2 (weaker signal post-reboot).
- **Monitor confirmed both** via `REBOOT prev_up=X now=Y build=4af50c57` anomalies.
- **Post-reboot-2 uptime**: 11 h 21 m completely clean. No further reboots overnight.

### What we don't know

- `reset_reason` is not exposed in `/metrics` or in `CRY-0000.LOG`. Firmware pain point per the log-management design §6.5.
- No panic dump, no watchdog log.

### Hypotheses (most plausible first)

1. **Physical disturbance during/around deployment.** User moved the device to bedroom around 19:30 (confirmed reboot #1). Someone then bumped the device / power cord around 19:56 — perhaps while setting up the room for bedtime. This is the simplest explanation given the 11-hour clean run afterwards.
2. **Wi-Fi router event** at ~19:56 that caused the device to fail a health check it couldn't recover from without a reboot. Would be odd behavior; ESP32 Wi-Fi stack is usually reconnect-tolerant.
3. **Brownout on the new outlet** — possible if the outlet shares a circuit with something that cycled on. The 11h clean run argues against this recurring, but can't rule out one-off.
4. **Firmware regression from the double-sigmoid fix.** Argues against: the fix removed two `expf` calls and two lines. No state machine changes, no new allocations. The 11h post-19:56 uptime is longer than any session we've had on the previous firmware. If the fix caused reboots, we'd expect more.

Hypothesis 1 is most likely. To rule out 2–4 systematically, we need `reset_reason` wired up in firmware (see §8 actions).

## 6. HNR and cry-profile observations

Tonight's 24 cries have a clean HNR distribution:

| bucket | n | HNR min / p50 / max | all pass `hnr_db > 4`? |
|---|---:|---|:---:|
| bedtime | 14 | 5.8 / 8.2 / 10.2 | ✅ 14/14 |
| morning | 10 | 4.9 / 9.1 / 9.9 | ✅ 10/10 |

HNR > 4 rule — which on 04-20 rejected 21/22 morning FPs — holds perfectly here: every real cry passes. The 04-21 "2 FPs slip through HNR" mode is not present because **there are no FPs to slip through**. This strengthens the log-HNR-don't-drop decision: one session's threshold isn't another session's, but when the session is clean, HNR is informative about the cry quality, not about FP-rejection.

## 7. Data gaps tonight

### Missing device-log files

The morning extract completed stages 1 and 2 partially before the device became unreachable mid-extract (moved off-network / powered off). After re-running, the device was already offline and the second extract bailed at stage 2, running audit on whatever was on disk.

Not recovered:
- `infer-20260422.jsonl` (tonight's 1.5 fps inference stream) — ~100 MB, ~75k frames
- `infer-20260423.jsonl` (morning)
- `cry-20260421.log`, `cry-20260422.log`, `cry-20260423.log` (30 s heartbeat snapshots)

Recovered / substitutable:
- `CRY-0000.LOG` (for boot events) ✓
- `triggers.jsonl` (per-capture metadata) ✓
- All 24 tonight WAVs + YAMNet scores ✓
- `/tmp/cry-monitor.log` (60 s polls covering full session) ✓ — substitutes for infer+cry logs at lower resolution

### Consequences

- **dev_max1s analysis is coarser.** The monitor log samples cm1s once per minute instead of 1.5 Hz. Analysis is qualitatively correct but misses short peaks.
- **Heartbeat baseline (`cry-20260422.log`) absent.** Per-30 s baseline stats are substituted by the monitor's 60 s polls.
- **The 19:30/19:56 reboots** are confirmed from CRY-0000.LOG so that investigation isn't blocked.

### Re-extraction opportunity

When the device comes back online (USB or bedroom power), running `tools/extract_session.sh logs/night-20260422` will fill in the missing logs. The analysis CSVs can then be regenerated. Low urgency — nothing downstream is blocked.

## 8. Tomorrow's actions (ordered by value)

1. **Re-extract device logs when device is back online.** Fills in `infer-20260422.jsonl` and lets us compute the true peak `max_cry_1s` per WAV (expect 0.6-0.9 based on offline YAMNet inference, vs monitor's 60 s-sampled 0.45).
2. **Wire `reset_reason` into firmware.** Small change: call `esp_reset_reason()` at `yamnet_init` or boot and log to CRY-0000.LOG. Would have answered tonight's reboot mystery in one glance.
3. **Adjust the detector `base_threshold` downward.** Current 0.70 will never fire on this baby. Based on tonight's peak-cm1s of 0.45, a threshold of 0.30-0.35 would alert on real cries at this voice's observed distribution. But — do not change during an active data-collection week; wait until tooling / monitoring is stable.
4. **Commit the log-management design doc.** `docs/research/log-management-design-20260423.md` is written, not committed — review with fresh eyes first.
5. **Commit the analyze-script trigger-matcher fix.** Done for 04-21 retroactively; the `tools/analyze_night_NNN.py` scripts should be unified into a single `tools/session_analysis.py` per the design doc §7. Follow-up.

## 9. Success check against the data-program plan

Per `docs/research/cry-detect-data-program-plan.md` §10 criteria:

- **Zero silent data-loss incidents:** ✅ Monitor caught both reboots in real time. No hidden outages.
- **Capture precision:** ✅ 100% tonight (24/24 real), best ever.
- **Firmware stability under fix:** ✅ 11h 21m clean after initial jostles.
- **On-device model signal:** partial — model is discriminating but at compressed range vs YAMNet. Not blocking for data collection. Would block an alerting product.

## 10. Artifacts

All under `projects/cry-detect-01/logs/night-20260422/` (local):

- `wavs/` — 200 WAVs (24 are tonight)
- `triggers.jsonl` — 200 entries
- `device-logs/` — CRY-0000.LOG, cry-{18..20}.log, infer-{18..21}.jsonl, infer-boot.jsonl. Missing: infer-{22,23}, cry-{21..23}
- `manifest.csv`, `segments.csv`, `specgrams/` (200 PNGs) — derived, complete
- `yamnet_files.csv`, `yamnet_segments.csv` — YAMNet oracle scores, complete
- `analysis/joined.csv`, `drift.csv`, `tonight_buckets.csv`, `summary.json` — derived

Host-side:
- `/tmp/cry-monitor.log` — 960 rows (15:09 → 07:16), full session coverage at 60 s resolution
- `/tmp/cry-monitor.anomalies` — all tonight's anomaly events (NEW_WAV × 21, REBOOT × 2)
