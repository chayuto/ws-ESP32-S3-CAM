# Classification logging — recording all model outputs for ground-truth correlation

*2026-04-17. Follow-on to `yamnet-class-exploitation.md`. Answers the question: "can we log what the model sees continuously, so we can later correlate back to real-world events by timestamp?"*

**Short answer: yes, cheaply.** Design and rough implementation cost follow.

---

## 1. The research need

Right now `/sdcard/cry-YYYYMMDD.log` captures:

- Discrete events (`boot`, `cry_start`, `wifi_up`).
- Periodic `snapshot` rows every 30 s with *one* derived number: `last_cry_conf` = sigmoid(`output[20]`).

That's enough to know when the model decided to alert. **It is not enough to know why, or to calibrate thresholds after the fact.** If the baby coughs at 23:15:07 and class 20 briefly hits 0.72, we can't see that unless the detector itself fires. And we can't see which *other* classes lit up — maybe it was actually class 42 (Cough) that dominated.

The fix: log the **full classifier output** at inference cadence, durably, with timestamps.

---

## 2. Data volume arithmetic

At ~1.4 inferences/sec, what does logging cost?

| Option | Bytes/inference | KB/hour | MB/day |
|---|---|---|---|
| All 521 classes as `idx:conf` (5 decimals) | ~6000 | ~30,000 | ~700 MB — **too much for SD** |
| All 521 as binary int8 (raw output) | 521 | ~2,600 | ~62 MB — cheap but needs a decoder tool |
| Top-5 as `idx:conf` | ~60 | ~300 | ~7 MB — **readable and cheap** |
| Top-10 as `idx:conf` | ~120 | ~600 | ~14 MB | 
| Watched-20 indices only (from yamnet-class-exploitation §Multi-class) | ~240 | ~1,200 | ~28 MB | 
| Watched-20 + top-5 union (dedup) | ~300 | ~1,500 | ~35 MB |

A typical SD card is 8–32 GB; a month of the richest scheme fits easily. **Recommended: Watched-20 + top-5 union**, written to a separate rolling file `classes-YYYYMMDD.log` so the primary event log (`cry-*.log`) stays human-readable.

---

## 3. Proposed schema — `classes-YYYYMMDD.log`

Each row is one inference. Schema:

```
wallclock,uptime_s,inference_n,top5:[idx:conf;...],watched:[idx:conf;...]
```

Example rows:

```
2026-04-17T23:15:07.123Z,1234,5678,"0:0.912;1:0.431;65:0.229;20:0.195;13:0.102","20:0.195;19:0.044;21:0.012;42:0.611;44:0.081;0:0.912;1:0.431;13:0.102;14:0.005;15:0.003;70:0.018;78:0.004;371:0.002;406:0.089;11:0.002;389:0.001;390:0.001;393:0.000;394:0.000;349:0.003"
```

Fields:
- `wallclock`: ISO-8601 UTC if NTP synced, else `NOT_SYNCED`. Same format as the event log.
- `uptime_s`: monotonic anchor; use to cross-reference with pre-NTP rows or the main event log.
- `inference_n`: the rolling `inference_count`. Lets us detect dropped rows, stride-check SD writes.
- `top5`: literal top-5 indices by confidence, any class. Captures unexpected things (e.g. class 389 Alarm Clock if the alarm goes off).
- `watched`: fixed order of the 20 curated classes from `yamnet-class-exploitation.md`. Stable columns → post-hoc analysis is a simple CSV `read_csv`.

Expected row size: 250–350 bytes. At 1.4 fps = ~30 KB/min ≈ 40 MB/day.

## 4. Implementation sketch (code, ready for next flash)

Stage 2 milestone **2.6a**. Depends on 2.6 "multi-class monitor" (reading >1 output index).

### 4.1 `yamnet.h` — expose full confidences

```c
int yamnet_num_classes(void);                          /* 521 */
void yamnet_get_confidences(float *out_521);           /* dequant + sigmoid, caller allocates */
```

### 4.2 Internal top-K helper in `main.c` (no new module needed)

```c
static void top_k(const float *confs, int n, int k, int *out_idx, float *out_conf)
{
    /* partial selection; simple O(n*k), n=521, k=5 -> ~2.5K ops, negligible */
    for (int i = 0; i < k; i++) { out_idx[i] = -1; out_conf[i] = -1.0f; }
    for (int i = 0; i < n; i++) {
        float c = confs[i];
        if (c <= out_conf[k - 1]) continue;
        int j = k - 1;
        while (j > 0 && out_conf[j - 1] < c) {
            out_conf[j] = out_conf[j - 1];
            out_idx[j]  = out_idx[j - 1];
            j--;
        }
        out_conf[j] = c;
        out_idx[j]  = i;
    }
}
```

### 4.3 `sd_logger` — new entry point

```c
void sd_logger_classifications(uint32_t inference_n,
                               const int *top_idx, const float *top_conf, int top_n,
                               const int *watched_idx, const float *watched_conf, int watched_n);
```

Writes to a secondary file `classes-YYYYMMDD.log` opened on demand. Schema as in §3. Re-uses `format_timestamp()` so wallclock conventions match.

### 4.4 Wire into `inference_task`

```c
static const int WATCHED[] = {
    20,19,21,22,                   /* cry spectrum */
    0,1,13,14,15,                  /* context */
    65,70,78,42,44,                /* FP sources */
    371,406,                       /* appliance noise */
    11,389,390,393,394,349,        /* urgent */
};
#define WATCHED_N (sizeof(WATCHED)/sizeof(WATCHED[0]))
static float all_confs[521];
static int top_idx[5]; static float top_conf[5];
static int watched_conf_idx[WATCHED_N]; static float watched_conf_buf[WATCHED_N];
...
yamnet_get_confidences(all_confs);
top_k(all_confs, 521, 5, top_idx, top_conf);
for (int i = 0; i < WATCHED_N; i++) {
    watched_conf_idx[i] = WATCHED[i];
    watched_conf_buf[i] = all_confs[WATCHED[i]];
}
sd_logger_classifications(m.inference_count,
                          top_idx, top_conf, 5,
                          watched_conf_idx, watched_conf_buf, WATCHED_N);
```

### 4.5 Write cadence

Options, by SD-write / accuracy trade-off:

- Every inference (~1.4 fps) — highest fidelity, 40 MB/day, 2.4 KB/s.
- Every 2nd inference — half the data, keeps ~0.7 s resolution.
- Only when any `watched` class crosses a **delta threshold** (changed by >0.05 since last logged row) — event-triggered. ~10× less data if the room is quiet.

Recommend **every inference** for the first week of research deployment (rich data); switch to delta-triggered once we know what's normal.

---

## 5. Correlating back to real-world events

Parent process:

1. Note the wall-clock time of a real-world event ("baby sneezed at 23:18").
2. `grep "^2026-04-17T23:18" classes-20260417.log`.
3. Read the `top5` column — should see class 44 (Sneeze) prominent.
4. Read the `watched` column — can check whether class 20 (Baby cry) spuriously spiked.

Enables:
- **Threshold tuning**: "every time class 20 was > 0.7, was it actually crying? ROC curve."
- **FP inventory**: "what classes fire during a vacuum run? During TV audio?"
- **Diurnal patterns**: p95 of each class over 24 h → HVAC on/off, traffic, neighbours.
- **Training data mining**: audio captured around high-confidence rows becomes a self-labelling mechanism for Stage 2.1 real-audio calibration.

---

## 6. Host-side analysis kit (stage 2.6b)

Once the data is flowing, we want simple tooling:

- `tools/classes_plot.py clay-YYYYMMDD.log --class 20` → time-series plot of a class conf.
- `tools/classes_cooccur.py` → heatmap of which classes fire together.
- `tools/classes_roc.py --ground-truth labels.csv` → ROC per class given hand-labelled events.

Stub as Python scripts in the repo; host-side only; no device dependency.

---

## 7. Stage-2 position

This work is **Milestone 2.6a**, immediately after 2.6 (multi-class monitor) and before 2.1 (calibration). Reason: calibration accuracy depends on knowing the real distribution of classes in the deployment — **you need the logging on first, to collect data, to inform calibration later**. Build the measurement tool before tuning.

Estimated cost: **~1 day** (mostly wiring + schema testing). Zero hardware dependency, no network dependency.

---

## 8. Risks / things to watch

1. **SD-write amplification** at 1.4 fps. Each `fwrite` ≈ 300 B + FATFS overhead. Should fit, but profile `log_bytes_written` over an hour.
2. **Stack pressure** — 521-float buffer is 2 KB on the inference task's stack, which is already 8 KB. Safe; but confirm.
3. **Sigmoid per class per inference** — 521 × (subtract + scale + sigmoid ≈ 3 ops) ≈ 1.5K ops. ≈ 0.003 ms at 240 MHz. Trivial.
4. **File rotation** — need a separate counter for classes-log size; reuse `CRY_DETECT_LOG_ROTATE_KB`. At ~40 MB/day even 2 GB SD lasts >1 month before rotation, but set up rotation anyway so old data moves to `classes-YYYYMMDD.log.1`, etc.

---

## 9. Summary

**~1 day of code ships a research-grade SD trace of every classification, indexed by wall-clock and uptime.** Pairs naturally with the existing `monitor-YYYYMMDD.log` on the host for cross-referencing. Single biggest leverage for understanding model behaviour in the field, improving calibration, and quantifying FP / TP rates.

**Queue as Stage-2 Milestone 2.6a, ahead of 2.1 calibration.**
