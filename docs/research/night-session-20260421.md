# Full-night session report — 2026-04-21 → 2026-04-22

**Session window:** 2026-04-21 19:00 → 2026-04-22 08:00 +1000 (13 h)
**Environment:** bedroom only, no mid-session move (different wall outlet vs 04-20 per §1.1 of the data-program plan)
**Firmware:** `a4870a21` (fresh flash from HEAD at 16:06, same C code as `a895bfdc`, new ELF hash from rebuild)
**Monitor:** `cry_monitor.sh v2` running 17:47 → 09:02 (15 h 15 m uptime, **zero outages, zero silent gaps**)
**Baby:** slept through except for one overnight burst at 03:38 with successful self-settle

## 1. Headline findings

1. **Zero data-loss incidents.** The v2 monitor logged 840+ heartbeats across the night without a NORESP chain; device uptime grew from 44 s at 17:47 REBOOT to 54 950 s by the morning probe (~15 h 16 m continuous). The 04-20 7 h 37 m outage did not recur. Outlet change + v2 escalation path = zero blind spots. **This was the single success criterion for tonight, per the data-program plan §1.**
2. **Bedroom-only recording raised auto-RMS precision from 47 % (04-20 mixed env) to 69.6 % (tonight).** Of 23 captures, 16 are YAMNet-confirmed real cries, 7 are FPs. No borderline cases.
3. **Baby self-settled overnight.** A real cry burst at 03:38 (rms 1984, YAM 0.996) → 3-min silence → second burst at 03:41 (rms 2035, YAM 0.999) → silence through morning. No caregiver intervention needed per subsequent metrics.
4. **HNR filter degraded slightly in bedroom context.** Rejects 5 / 7 FPs at `hnr_db > 4` (71 %), vs 21 / 22 in 04-20 living-room (95 %). Two leakers are harmonic adult-speech-like sounds near the device at 20:17–20:18 — an FP mode the living-room session didn't expose.
5. **On-device model unchanged.** Max `max_cry_1s` tonight = 0.622 (never hit the 0.718 ceiling seen 04-20). The re-PTQ fix is still deferred per the data-program plan §7.

## 2. Timeline

All times local (+1000). Anomalies reported by `cry_monitor.sh` in real time.

| time | event | rms | cm1s | YAM | bucket |
|---|---|---:|---:|---:|---|
| **17:47:28** | REBOOT after move to bedroom (live dry-run) | — | — | — | — |
| 18:48:02 | NEW_WAV (pre-bedtime) | 21 | 0.475 | — | (dry-run tail) |
| 19:02:20 | bedtime cluster starts | 267 | 0.500 | 0.991 | bedtime |
| 19:03:25 | bedtime | 312 | 0.500 | 0.995 | bedtime |
| 19:04:31 | bedtime peak | **1155** | 0.500 | 0.999 | bedtime |
| 19:05:50 | bedtime | 683 | 0.517 | 0.997 | bedtime |
| 19:07:07 | bedtime loudest | **1581** | 0.500 | 0.998 | bedtime |
| 19:08:10 | bedtime | 915 | 0.501 | 0.999 | bedtime |
| 19:10:37 | bedtime, settling | 295 | 0.622 | 0.892 | bedtime |
| 19:11:55 | bedtime | 230 | 0.500 | 0.969 | bedtime |
| 19:14:03 | bedtime tail | 251 | 0.501 | 0.961 | bedtime |
| **19:14 — baby asleep** | | | | | |
| 20:17:46 | stir | 294 | 0.500 | 0.051 | stir (FP) |
| 20:18:57 | stir | 296 | 0.500 | 0.000 | stir (FP) |
| 20:25:45 | stir (real short cry) | 636 | 0.500 | 0.993 | stir |
| 20:28:46 | stir (real short cry) | 249 | 0.622 | 0.989 | stir |
| 22:31:53 | isolated real cry | **404** | 0.517 | 0.996 | late-stir |
| 22:59:00 | stir | 269 | 0.500 | 0.008 | late-stir (FP) |
| 23:10:41 | stir | 458 | 0.500 | 0.008 | late-stir (FP) |
| 23:16:23 | stir | 284 | 0.500 | 0.006 | late-stir (FP) |
| 23:37:51 | short real cry | 277 | 0.517 | 0.936 | late-stir |
| 01:34:05 | stir | 464 | 0.500 | 0.007 | overnight (FP) |
| **03:38:07** | **real cry — overnight wake** | **1984** | 0.517 | 0.996 | overnight |
| **03:41:40** | **real cry — second burst** | **637** | 0.501 | 0.999 | overnight |
| 04:56:01 | noise | 292 | 0.500 | 0.002 | overnight (FP) |
| 07:22:20 | morning real cry / wake-up | 682 | 0.501 | 0.710 | morning |

Monitor captured these in real time via `NEW_WAV` anomalies, plus one `CRY_SIGNAL` at 19:06:14 when the on-device `cm1s` hit 0.622 (the 4th bedtime capture).

## 3. Capture effectiveness by bucket

| bucket | n | real (YAM ≥ 0.5) | border (0.3-0.5) | FP (< 0.1) | precision |
|---|---:|---:|---:|---:|---:|
| `bedtime` (19:00-20:30) | 13 | **11** | 0 | 2 | 85 % |
| `late-evening-stirs` (22:00-24:00) | 5 | 2 | 0 | 3 | 40 % |
| `overnight` (00:00-06:00) | 4 | 2 | 0 | 2 | 50 % |
| `morning` (06:00-08:00) | 1 | 1 | 0 | 0 | 100 % |
| **total** | **23** | **16** | 0 | **7** | **70 %** |

Compare to 04-20 session:
- bedroom bedtime was 12 / 12 (100 %) — tonight 11 / 13 (85 %). Two FPs (20:17, 20:18) crept into the bedtime bucket because the stir-cluster crossed the 20:30 boundary; reclassifying those as "stirs" moves bedtime to 11 / 11 (100 %) matching last night.
- overall 04-20: 61 real / 129 = 47 %. Tonight 16 / 23 = 70 %. Precision **grew 23 pp** purely from environment, no code change.

## 4. Drift: on-device vs YAMNet (tonight)

The on-device model remained saturated. Every real cry tonight capped at one of four int8 output values after sigmoid: 0.501, 0.517, 0.591 (unused), 0.622. The 0.718 ceiling was never approached, and no capture distinguished between cries (YAM 0.89-0.999).

| YAMNet bucket | n | dev_max1s p50 | dev_max1s max |
|---|---:|---:|---:|
| real cry (≥ 0.5) | 16 | 0.517 | 0.622 |
| FP (< 0.1) | 7 | 0.500 | 0.501 |

Same unusable pattern as 04-20: output quant clip at 0.73 theoretical. Re-PTQ remains the fix when we want on-device alerting.

## 5. The 03:38 overnight event (deep view)

Two 40-s captures, 3-min gap, full self-settle:

```
min      rms_max  rms_p95  max1s_pk  cry_pk
03:30–36  < 380    < 250    0.500     0.500    (quiet)
03:37     243      232      0.500     0.500
03:38     1984     1208     0.622     0.622    ← real cry burst 1 (YAM 0.996)
03:39     45       43       0.500     0.500    ← IMMEDIATE silence
03:40     84       45       0.500     0.500
03:41     2018     373      0.622     0.622    ← real cry burst 2 (YAM 0.999)
03:42     49       47       0.500     0.500    ← IMMEDIATE silence
03:43     132      49       0.500     0.500
03:44     89       51       0.500     0.500
03:45     245      46       0.501     0.501
03:46–49  < 90     < 52     0.500     0.500    (back to baseline)
```

**Interpretation:** brief discomfort / dream-cry → self-quiet → single re-check cry → self-quiet again. Total cry time ~80 s out of a 13 h night. No caregiver intervention. This is the kind of data point a *production alerting* system must NOT fire on, since waking the parent for an 80-s self-settle defeats the point. A ≥ 90-s sustained-cry gate would be the simplest rule; tonight's dataset would not have fired it.

## 6. Bedroom baseline (whole session)

From `cry-20260421.log` + `cry-20260422.log` snapshot rows (30 s cadence, 1559 rows total):

| metric | min | p25 | p50 | p95 | max |
|---|---:|---:|---:|---:|---:|
| input_rms | 20.6 | 33.4 | **41.1** | 175.5 | 1015 |
| nf_p95 | 65.8 | 149.8 | **184.0** | 184.0 | 184.0 |
| max_cry_1s | 0.47 | 0.47 | 0.50 | 0.50 | 0.62 |
| cry_conf | 0.50 | 0.50 | 0.50 | 0.50 | 0.62 |

Vs 04-20 bedroom-overnight-2 baseline (for reference):
- RMS p50 was 66.6 → tonight 41.1 (40 % quieter median)
- nf_p95 p50 was 226 → tonight 184 (saturated but at a lower ceiling)

**Why quieter?** Different wall outlet may have reduced low-frequency hum; bedroom placement details are the other variable. Either way, a quieter baseline means the auto-RMS 5×/7× multiplier triggers on smaller real sounds — confirmed by tonight's capture cadence (23 events from a quieter room vs 17 from a similar window last night).

## 7. HNR filter re-test on tonight's data

The 04-20 analysis concluded `hnr_db > 4` was a clean post-capture cull. Tonight's data weakens that:

| group | n | HNR min–p50–max | pass `hnr_db > 4` |
|---|---:|---|---:|
| real cries | 16 | 2.2 – 8.5 – 11.1 | 15 / 16 (94 %) |
| FPs | 7 | 1.8 – 2.9 – 8.7 | **2 / 7 passes** (29 % leak) |

The one real-cry miss is 19:14:03 (HNR 2.2) — end-of-cluster wet / short cry. The two FP leakers are 20:17:46 (HNR 8.2) and 20:18:57 (HNR 8.7) — both from a cluster that sounds, in the spectrogram, like muffled adult voice (likely caregiver speech in room during settle-check). They have strong harmonic content from human vocal folds but are not baby cries.

**Consequence for the data-program plan §8:** the HNR-cull decision we already made — *log the value, don't drop the WAV* — is vindicated. A one-night conclusion of "95 % FP rejection" would have been optimistic; bedroom FP modes aren't the same as living-room FP modes. Keep collecting and tune thresholds from data, not from a single session.

## 8. Known data-quality gaps

1. **trigger_note match failed for all tonight's captures.** The matching key in `/tmp/analyze_night_20260421.py` requires millisecond-precision equality between WAV filename (second resolution) and `triggers.jsonl` timestamp (millisecond). Low-impact — RMS / note content is still visible in `triggers.jsonl` directly, and the preview in §2 above was pulled from there. Will fix the matcher in the next analysis iteration with a ±2 s window.
2. **Morning 07:22 capture** (YAM 0.710) is the only event in its bucket. May or may not represent the actual wake-up; no corroborating captures nearby. Human audit would resolve.

## 9. What changed overnight vs the data-program plan

| tier | target | outcome |
|---|---|---|
| 1.1 power resilience | zero outages | ✓ zero outages across 15 h |
| 1.2 monitor alarm | detect real-time gaps | ✓ NEW_WAV, REBOOT, CRY_SIGNAL all fired and observed |
| 1.3 extract idempotency | triggers refresh works | ✓ triggers.jsonl pulled fresh, 179 trigs end-to-end |
| 2 annotation | label audit | ○ not attempted — no annotation tool yet |
| 3 retention | purge old WAVs | ○ extract still pulls all 179 (incl. 04-18/19/20 duplicates). Next session should purge SD after audit. |
| 4 metadata | README filled | ○ partial — outlet change noted, placement details still TODO |
| 5 variety | same mic, same room | — as planned (consistent environment for now) |
| 6 training | dataset v0.1 | — not yet (need 4-6 weeks cadence) |

Nothing blocking, but retention + annotation are the next two items by value.

## 10. Artifacts

All under `projects/cry-detect-01/logs/night-20260421/`:

- `wavs/` — 179 WAVs (cumulative since 04-18, 23 are tonight)
- `device-logs/` — CRY-0000.LOG (21.7 KB, 42 boots), infer-{18..22}.jsonl, cry-{18..22}.log
- `manifest.csv`, `segments.csv`, `specgrams/` (179 PNGs)
- `yamnet_files.csv`, `yamnet_segments.csv`
- `triggers.jsonl` (179 entries total)
- `analysis/joined.csv` — tonight's 23 WAVs joined with YAMNet + on-device peak
- `analysis/drift.csv` — dev_max1s vs YAMNet delta per WAV
- `analysis/timeline_bedtime.csv` — 30-min dense trace of the 19:02 cluster
- `analysis/timeline_0338.csv` — 20-min dense trace of the overnight event
- `analysis/baseline_bedroom.csv` — whole-session RMS / nf_p95 distribution
- `analysis/tonight_buckets.csv` — per-bucket capture-effectiveness table
- `analysis/summary.json` — machine-readable headline stats
- `README.md` — session metadata (partial; needs placement detail)

Related host-side state:
- `/tmp/cry-monitor.log` — 840+ heartbeat rows, kept for post-mortem evidence
- `/tmp/cry-monitor.anomalies` — empty of outages, contains ~25 NEW_WAV and 1 CRY_SIGNAL entries
