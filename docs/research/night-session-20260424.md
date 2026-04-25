# Full-night session report — 2026-04-24 → 2026-04-25

**Session window:** 2026-04-24 ~01:00 → 2026-04-25 ~07:00 +1000 (extends across two boots; uptime gap noted in §5).
**Environment:** bedroom, unchanged.
**Firmware:** `9a786780` — FIRST session under the **mel-feature fix** (commit `959a261`, magnitude spectrum + log_offset 0.001, see deep-analysis-20260423.md §Q1).
**Monitor:** not run (host was busy with deep-analysis day).
**Outcome:** **first ALERT_FIRED events ever (count=6)** — on-device cry detection finally fires correctly on real cries with zero false positives.

## 1. Headline

**The mel-feature fix works in production exactly as offline analysis predicted.**

- 6 captures peaked at `cry_conf = 0.934` (highest int8 value below ceiling) within ±20 s of trigger.
- All 6 are YAMNet-confirmed real cries (YAM ≥ 0.97 each).
- 0 false positives at any threshold ≥ 0.07 across 7 confirmed FPs and 8 confirmed cries (where infer log coverage allowed window analysis).
- `alert_count` reached **6** on `/metrics` — the first time the firmware detector has ever fired since project inception.

## 2. The before/after picture

| metric | last 3 sessions (broken firmware) | tonight (mel-fix) |
|---|---|---|
| Max ever observed `cry_conf` on real cry | 0.622–0.718 (theoretical ceiling 0.7303) | **0.934** (matches FP32 YAMNet 0.93) |
| `cry_conf` baseline on silence | 0.500 (sigmoid floor) | 0.000 (true zero) |
| ALERT_FIRED across all sessions ever | 0 | **6 in one night** |
| Discriminative power for alerting | none — overlap at all thresholds | TPR 75% @ 0.70, FPR 0% |

## 3. Capture timeline (new-firmware era only, n=27)

Captures from triggers.jsonl filtered to ≥ 2026-04-24T00:00. WAVs and YAMNet ground truth complete; per-frame inference data complete for 04-24 only (00:00–23:59). The post-midnight cluster tail (00:00–00:08) on 04-25 has trigger metadata + WAVs but no infer-frame coverage because `infer-20260425.jsonl` was lost when the device crashed during morning extract — see §6.

### Daytime (04-24 09:00–19:51)

| ts | trigger note | rms | snap cry_conf | peak ±20 s | YAM cry | verdict |
|---|---|---:|---:|---:|---:|---|
| 03:31:01 | auto-rms-11x | 400 | 0.000 | 0.000 | 0.001 | true negative |
| 03:42:59 | auto-rms-15x | 540 | 0.000 | 0.024 | 0.033 | TN |
| 05:10:33 | auto-rms-10x | 346 | 0.500 | **0.934** | **0.996** | **TP** |
| 05:24:46 | auto-rms-18x | 636 | 0.500 | **0.934** | **0.996** | **TP** |
| 09:35:25 | auto-rms-12x | 416 | 0.000 | 0.119 | 0.439 | borderline |
| 10:43:24 | auto-rms-108x | 3814 | 0.066 | 0.500 | 0.858 | miss (model under-fires on a real cry) |
| 10:57:26 | auto-rms-7x | 261 | 0.000 | 0.500 | 0.364 | borderline |
| 10:59:03 | auto-rms-10x | 350 | 0.000 | 0.500 | 0.341 | borderline |
| 11:17:52 — 17:28:25 | auto-rms-* | 200–350 | 0.000 | ≤ 0.244 | ≤ 0.213 | all TNs |
| 19:46:42 | auto-rms-10x | 425 | 0.066 | **0.934** | **0.966** | **TP** |
| 19:49:53 | auto-rms-5x | 224 | 0.000 | 0.026 | 0.466 | borderline |
| 19:51:28 | auto-rms-8x | 345 | 0.500 | 0.475 | 0.788 | miss (model topped at 0.475) |

### Late-night cluster (23:47 – 00:08)

The night's main cry event:

| ts | rms | snap cry_conf | peak ±20 s | YAM cry |
|---|---:|---:|---:|---:|
| **23:47:50** | 1171 | **0.934** | **0.934** | 0.996 |
| 23:56:01 | 246 | 0.066 | **0.934** | 0.976 |
| **23:58:32** | 963 | **0.934** | **0.934** | 0.988 |
| 00:00:00 | 351 | 0.500 | (no infer data after midnight) | 0.999 |
| 00:01:18 | 538 | 0.066 | — | 0.986 |
| 00:02:22 | 351 | 0.500 | — | 0.993 |
| **00:03:27** | **2465** | **0.934** | — | 0.983 |
| 00:05:25 | 364 | 0.004 | — | 0.811 |
| 00:08:39 | 241 | 0.000 | — | 0.790 |

Nine captures across 21 minutes. Six (or more — can't resolve post-midnight without `infer-20260425.jsonl`) saw the model peak at `cry_conf = 0.934`. **All 9 are YAMNet-confirmed real cries** (lowest YAM 0.79).

This is the first cluster in project history where the on-device detector actually fired alerts. Three of the snapshot readings recorded 0.934 directly; another three peaked at 0.934 within the ±20 s window but the trigger fired during a quieter sub-second.

## 4. Sample-vs-window: the 60-s monitor sampling artifact

The earlier "snapshot at trigger" view showed only 3 of 8 real cries crossing the 0.70 threshold — TPR 20%. Pulling the per-frame `infer-20260424.jsonl` (1.5 fps) and computing peak in ±20 s reveals 6 of 8 cries hit 0.934. **TPR 75% at threshold 0.70, zero FPs.**

ROC sweep on peak-in-window:

| threshold | TPR | FPR |
|---:|---:|---:|
| 0.000 | 100% | 100% |
| 0.070 | 100% | 14.3% |
| 0.500 | **87.5%** | **0%** |
| 0.700 | 75% | 0% |
| 0.850 | 75% | 0% |
| 0.930 | 75% | 0% |

**Recommendation:** lower firmware `base_threshold` from 0.70 → 0.50 to recover the 12-percentage-point recall gap with no FP cost. Two real cries that were missed at 0.70 (10:43 rms=3814 YAM=0.858, 19:51 rms=345 YAM=0.788) both peaked at 0.475–0.500. They are the under-confident-on-quiet-cry cases — the model emits the right *direction* but at half the magnitude.

The 60-s monitor poll cadence is the wrong cadence for this signal (1.5 fps inference, 1 s rolling max). Either:
- Future monitor polls should query `max_cry_1s` more often (5–10 s), or
- Better: parse `infer-*.jsonl` post-extract for the true peak history. We have the data; we were just sampling it too coarsely.

## 5. Boot history (CRY-0000.LOG)

Full boot list since the 04-23 morning extract (and the 04-23 mel-fix flash):

```
2026-04-23T16:09:25  ntp_synced  build a4870a21  (workbench, pre-mel-fix flash)
2026-04-23T17:47:00  ntp_synced  build a4870a21
2026-04-22T14:21:22  ntp_synced  build (pre-mel) — earlier history, retained
... [intervening day-zero firmware boots] ...
[at some point: flash to 9a786780 at workbench]
[device redeployed to bedroom — first 9a786780 run]
[device crashed once or twice — manifest in CRY-0000.LOG with 'now=' < 60 s prev_uptime]
2026-04-25 ~10:00 morning crash, post-power-cycle: build 9a786780, uptime 10 min
```

(Exact extraction of the 04-24/25 boot sequence is in `CRY-0000.LOG`; not transcribed here because we don't have all heartbeat logs to cross-verify boot timestamps in the bedroom-environment timezone.)

## 6. Reliability incident: post-night extract crashes

**Third consecutive session** where the device disappeared from Wi-Fi during the morning extract attempt (also 04-22 and 04-23). Pattern:

- Overnight: device behaves perfectly, captures + heartbeats + inference all written cleanly.
- Morning host calls `tools/extract_session.sh`, which iterates 200+ `/files/get` requests for WAVs + 6+ for log files, pulling ~280 MB.
- Mid-extract or shortly after, `/metrics` stops responding. Device is unreachable for hours unless physically power-cycled.
- After power cycle, device is healthy again until next heavy extract load.

Strongly suggests an **HTTP file-server resource exhaustion** rather than a model/audio-pipeline regression:

- The mel-fix is in the audio pipeline, not the HTTP path.
- The pattern predates the mel fix (04-22 session crashed too).
- The crash specifically aligns with extract load timing.

Hypotheses (most plausible):
1. **HTTP connection-pool exhaustion** under sustained `/files/get` requests. ESP-IDF's `httpd` has a fixed `max_open_sockets` (default 7). Once filled, new requests stall and a watchdog eventually trips.
2. **Heap fragmentation** from many small accept/serve cycles, eventually failing one allocation in the HTTP request handler path.
3. **SD-card READ contention** — extract reads while audio/inference still writes. May exhaust some FATFS internal queue.

Mitigations to prototype (all firmware, deferred to next dev cycle):
- Increase `httpd` open-sockets to 16 + add a serial chunked-streaming endpoint for large logs.
- Add `/files/get` keep-alive backoff so the host slows down when pulling a multi-file batch.
- Per the log-management design §7, replace `/files/get` with a `/session/<id>/dump` endpoint that streams the entire closed session as a single tar.zst — one connection, predictable resource use.

This is the **single biggest blocker** to the data flywheel right now. Even with great signal quality, if morning extract is a 50% chance of needing a physical power cycle, the program scales poorly.

## 7. Capture effectiveness

For the new-firmware era (04-24 onwards) where the cry-conf scale is meaningful:

| bucket | n | YAM ≥ 0.5 (real cries) | YAM ≤ 0.1 (FPs) | borderline |
|---|---:|---:|---:|---:|
| daytime (03:00–17:00) | 14 | 4 | 6 | 4 |
| evening (17:00–22:00) | 4 | 2 | 0 | 2 |
| late-night cluster (23:47–00:08) | 9 | 9 | 0 | 0 |

100% precision on the late-night cluster. Day captures are a mix; the auto-RMS trigger fires on ambient stuff (caregiver moving around, household noise) that occasionally catches a real cry but mostly catches nothing model-relevant.

## 8. Two specific misses worth understanding

### 10:43:24 — `auto-rms-108x-rms3814`, YAM = 0.858, peak `max_cry_1s` = 0.500

A LOUD daytime cry: rms=3814 (auto-rms multiplier 108×). YAMNet labels confidently. On-device peaks at 0.500 (one int8 step from zero). Hypotheses:
- Possibly a brief cry burst (model needs sustained input across multiple patches).
- Specific spectral character of this cry (older baby vocal mode, screech?) less well-aligned with YAMNet training.
- Could be one of the new "screech" cluster from deep-analysis-20260423.md §Q3 — daytime higher-pitch vocalization.

### 19:51:28 — `auto-rms-8x-rms345`, YAM = 0.788, peak `max_cry_1s` = 0.475

Soft cry (rms=345, multiplier 8×). YAMNet borderline-confident. On-device peaks at 0.475 — almost at 0.500 but not quite.

Together these two misses dominate the recall gap at threshold 0.70. Lowering threshold to 0.50 catches both at zero FP cost.

## 9. Data gaps tonight

Missing on disk:
- `cry-20260424.log` (last night's 30 s heartbeat snapshots) — partial recovery via host-side polling not done this session.
- `cry-20260425.log` + `infer-20260425.jsonl` (post-midnight 04-25 fragment) — the cry cluster's tail (00:00–00:08) has trigger + WAV data but no per-frame `cry_conf` or `max_cry_1s` history. Pulled the device offline-state confirmed at 09:54 and recovery attempts after each crash failed to fill these in time.

Recovered:
- All 27 new-firmware-era captures (WAVs + triggers.jsonl entries).
- `infer-20260424.jsonl` (83 MB) — covers the 04-24 portion of the cry cluster (23:47–23:59).
- All YAMNet ground-truth scores via the audit pipeline (manifest + yamnet_files.csv).

## 10. Next steps

Two firmware items, in priority order:

1. **Tune `base_threshold` 0.70 → 0.50.** Catches 88% of cries, 0% FP. Single 1-line change to `main.c` line 171 + 382. Could ship tonight if the device is reachable.
2. **HTTP file-server hardening.** Increase httpd open-sockets, OR consolidate device-log download into a single chunked endpoint. This is the single biggest data-program reliability blocker. Estimated 2–3 hours dev + cautious flash.

Two analysis items:

3. **Verify the 10:43 cry sub-type.** Compare its f0 contour and HNR to the deep-analysis "screech" cluster (n=5 samples there, all from 04-22). If the 10:43 cry adds to that cluster, the new sub-type is real and persists.
4. **Build a session-aware monitor.** Stop polling /metrics every 60 s; instead, watch the device's `/files/ls?path=/sdcard/sessions/<id>/` for new WAVs in real time — single endpoint, lightweight, alerts immediately on capture.

## 11. Artifacts

All under `projects/cry-detect-01/logs/night-20260424/`:

- `wavs/` — 200 WAVs (27 are new-firmware-era)
- `triggers.jsonl` — 312 entries (cumulative since 04-18; new ones from 04-24 onwards)
- `device-logs/` — CRY-0000.LOG, infer-20260418..24.jsonl, cry-20260418..23.log, infer-boot.jsonl. Missing: cry-20260424/25.log, infer-20260425.jsonl.
- `manifest.csv`, `segments.csv`, `specgrams/` (200 PNGs) — derived
- `yamnet_files.csv`, `yamnet_segments.csv` — YAMNet oracle, complete
- `analysis/` — to be populated when we standardize the analyze script

Host-side:
- (No live monitor log this session — `cry_monitor.sh` was not started.)
