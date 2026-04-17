# Retraining ROI analysis — should we retrain YAMNet?

*2026-04-17. Stage-2 research note. Answers: "will retraining with more crying-baby recordings make the model meaningfully better, or is the gain marginal?"*

**TL;DR:** It depends on **which kind** of retraining. There are four, ordered by effort × reward:

1. **Recalibrate INT8** (no weight changes) — hours — big accuracy lift, cheap.
2. **Binary head on YAMNet's 1024-d embedding** — days — big, purpose-built, keeps multi-class head.
3. **Personalise to one baby** — hours, needs family-specific data — huge for one household.
4. **Fine-tune full backbone** — week — marginal at best, breaks the "free" 520 other classes.

---

## 1. What "retraining" means technically

YAMNet's inference graph:

```
waveform → [STFT + mel] → 96×64 log-mel patch → MobileNetV1 backbone → [1024-d embedding] → Dense(521) → sigmoid → class probabilities
```

Our on-device model stops at mel input (we computed it in firmware) and runs MobileNetV1 + Dense(521). We read `output[20]` = `Baby cry, infant cry`.

Three places you can intervene:

| Intervention | What changes | Existing code impact |
|---|---|---|
| **Re-quantize (INT8 PTQ recalibration)** | activation scale/zero-point per layer — **weights unchanged** | re-run `tools/convert_yamnet.py --audio-dir …`; flash new `.tflite` |
| **Retrain last dense layer** (transfer learning on embedding) | swap the Dense(521) for Dense(2) or Dense(N) — **backbone weights frozen** | export a different `.tflite`; on-device code still reads an INT8 probability |
| **Full fine-tune** | all MobileNetV1 + Dense weights updated | same `.tflite` shape; on-device unchanged |

Data needs scale dramatically:

| Intervention | Minimum useful data | Typical production data |
|---|---|---|
| PTQ recalibration | 100 mel patches (≈ 2 min of audio) | 500 patches |
| Transfer-learn head | 500 labelled clips (cry / not-cry) | 2,000–5,000 |
| Full fine-tune | 5,000 labelled clips | 20,000+ |

---

## 2. What actually limits our current model

Measured behaviour over the deployment so far:

- `cry_conf` idles at **0.57–0.62 in a quiet room**. Ground truth: no baby is crying.
- Threshold 0.85 barely masks the bias. Real cry (not tested yet) needs to push conf decisively above 0.85.
- RMS dependence weak — the model registers "cry-ish" on broadband noise regardless of level.

Root causes, ordered by likely contribution:

1. **Synthetic INT8 calibration** (large effect). The `convert_yamnet.py` script used Gaussian random patches for PTQ; real log-mel distributions are different, causing per-layer scale drift that biases the softmax.
2. **AudioSet class-20 is a noisy label.** "Baby cry, infant cry" in AudioSet includes 2 s clips where the actual cry is a fraction of the window; the 521-class classifier is trained at clip-level weak-label supervision, so its class-20 decision boundary is inherently fuzzy.
3. **Domain mismatch.** AudioSet is YouTube clips recorded on varied mics in varied rooms. Our mic is a specific PDM MEMS, fixed gain 36 dB, in one bedroom. Even a perfectly-calibrated model sees a different input distribution than it was trained on.

Intervention #1 (recalibration) fixes cause #1 at minimal cost. Causes #2 and #3 are mitigated by intervention #2 (trained head on embedding).

---

## 3. ROI table — each approach in detail

### 3.1 Recalibrate INT8 PTQ with real audio — **recommended first**

What: re-run `tools/convert_yamnet.py --audio-dir <wavs>` with 300–500 real log-mel patches drawn from the deployment room + ESC-50 crying samples. No weight changes, only activation statistics change.

Expected outcome:
- Quiet-room `cry_conf` distribution: from ~(0.57, 0.62) to ~(0.08, 0.22) [estimate from INT8 PTQ literature, not measured].
- Real-cry `cry_conf`: from unknown to expected > 0.8 on clear clips.
- Detection threshold can drop from 0.85 to ~0.5.
- **Side benefit:** all 521 classes get better calibration simultaneously — helps the Stage-2 multi-class plan (§yamnet-class-exploitation.md) even more than it helps cry alone.

Data needed: ~500 WAVs, 16 kHz mono, 1 s each. Collection via the Stage-2 `/rec/trigger` endpoint (file-api plan) or a simple `arecord` harness.

Cost: 1 h host compute (no GPU). 1 flash cycle.

**Verdict: do first. The highest accuracy return per hour of effort we can buy.**

### 3.2 Transfer-learned binary head on embedding — **recommended after 3.1**

What: re-export YAMNet so the TFLite graph outputs the 1024-d embedding (before Dense(521)); add a new small MLP head trained specifically for baby-cry-or-not; keep the original 521-class head alongside.

The on-device graph becomes:

```
mel patch → MobileNetV1 (frozen) → 1024-d embedding
                                      │
                        ┌─────────────┴─────────────┐
                        ▼                           ▼
               Dense(521) → 521 probs     Dense(2) → binary cry-or-not
                    ↑                         ↑
               (unchanged, for             (trained on our data
                smoke / siren etc.)          — just cry vs not-cry)
```

Why this wins:
- **Frozen backbone** → no risk of breaking the other 520 classes.
- **Purpose-built head** → decision boundary fit to exactly the cry-vs-not-cry task using our room/mic/gain statistics.
- **Cheap to train**: 1024 → (hidden 64) → 2. ~70 K parameters. Trains in minutes on a laptop CPU.
- **Tiny footprint**: +5–20 KB in the `.tflite`.
- **No new ops**: Dense + Softmax already in the op resolver.

Expected outcome: on our deployment, binary-head cry probability should discriminate much more sharply than class-20. Published AudioSet-embedding transfer benchmarks typically get 10–30× F1 improvement over using the class-level output directly on narrow tasks.

Data needed:
- **Positive class (cry):** 500–2000 clips. Sources: ESC-50 `crying_baby` (~40), Donate-a-Cry corpus (~1000), AudioSet labelled "Baby cry" segments via pseudo-labels (~10000 available but noisy), parent-recorded clips via `/rec/trigger` (priceless — room/baby-specific).
- **Negative class:** 1000–5000 clips. Sources: ESC-50 non-cry, UrbanSound8K, AudioSet broad negatives, **quiet-room recordings of this specific bedroom** (the most important — directly teaches the model what "my silence" looks like).
- Augmentation: time-shift, pitch-shift ±10 %, gain variation, add bedroom-noise masks. 3–5× the raw data.

Cost:
- Data collection: 1–2 days passive + a few intentional cry-clip recordings.
- Training: hours on CPU or ~15 min on a free Colab T4.
- Pipeline wiring: 1 day (modify `convert_yamnet.py` to dual-head export, extend `yamnet.cc` to read both outputs, extend detector to use binary head as primary + multi-class head as context).

**Verdict: this is where "meaningful" retraining pays off. Do after 3.1. Combined effort ~3 days end-to-end.**

### 3.3 Personalise to YOUR baby — **optional Stage-3 polish**

Same pipeline as 3.2, but trained on **~100 clips of your baby** + negative class = your bedroom ambient. Dramatically over-fits to one family's acoustic environment.

Expected outcome: near-perfect detection of *your* baby's cry. Would probably miss other babies. Doesn't matter — only yours lives in the bedroom.

Implementation: identical to 3.2 with a smaller, more focused dataset. Could be live-updated by the device itself (on-device learning is heavy but the Dense(2) head is small enough — Stage-3 stretch).

Cost: collection is the cost. 2 weeks of passive recording via `/rec/trigger` on every cry_start event, then a 15-min training session.

**Verdict: huge gain if the user commits to it. Right project to build this on — baby monitors are the textbook case for personalisation.**

### 3.4 Full fine-tune of YAMNet — **not recommended**

Unfreezes the MobileNetV1 backbone and retrains end-to-end on cry-heavy data.

Why it looks tempting: more parameters moving = bigger decision boundary shift = presumably more accurate.

Why it's actually bad for this project:

1. **Breaks the other 520 classes.** Our Stage 2 multi-class monitor (smoke, siren, doorbell, laughter context) depends on the 521-class head holding its accuracy. Fine-tuning shifts the backbone representation → every other class degrades.
2. **Needs 10× more data** than 3.2 to avoid overfitting, OR complex regularisation.
3. **Marginal gain over 3.2** for cry-only tasks; published transfer-learning benchmarks show fine-tuning beats frozen-backbone + trained head by < 5 % F1 on narrow classes.
4. **Re-quantization pain**: after training in float32, need to redo INT8 PTQ; accuracy can drop unpredictably.

**Verdict: don't. The multi-class-head-preserved design (3.2) is strictly better engineering.**

---

## 4. Marginal or meaningful — honest numeric guess

For cry detection in this specific deployment:

| Intervention | Current | Expected after | Delta |
|---|---|---|---|
| Baseline (synthetic-calib class-20) | FP rate probably 10–30 % given thresholds | — | — |
| **3.1 Real-audio recalibration** | — | FP rate ≤ 2 %, TP rate > 90 % on clear clips | **large** |
| **3.2 Trained binary head** (on top of 3.1) | — | FP rate ≤ 0.5 %, TP rate > 95 % | **meaningful** |
| **3.3 Personalised head** | — | FP rate ≈ 0 %, TP rate > 98 % | **huge for one household** |
| **3.4 Full fine-tune** | — | FP rate similar to 3.2; *other 520 classes degrade* | **not worth it** |

(Absolute numbers are calibrated guesses from transfer-learning literature — not measured on this device. Collect real data from the deployed logs before trusting any of these.)

**So "marginal" is the wrong word for 3.1 and 3.2.** Both are meaningful. Marginal is the right word for 3.4.

---

## 5. Data sources ranked

For the cry positive class, in order of usefulness per clip:

1. **Recordings of your own baby in this bedroom via `/rec/trigger`.** Highest signal: matches deployment exactly. Need ~100 for personalisation, ~200 for mixed dataset.
2. **Donate-a-Cry corpus** ([github.com/gveres/donateacry-corpus](https://github.com/gveres/donateacry-corpus)). ~1000 infant cry clips, crowd-sourced, varied mics. Public domain-ish (verify licence before redistributing).
3. **ESC-50 `crying_baby`** — 40 clips. Low volume but high quality, commonly used benchmark. Apache-2.0.
4. **AudioSet "Baby cry" segments** — weak-labelled, noisy; use only for augmentation.
5. **Baby Chillanto DB1** — academic, high-quality, multi-cause labelled; access-restricted.

For the negative class:

1. **Your bedroom quiet recordings** via `/rec/trigger` with no trigger. Teaches the model what "silence here" looks like. Highest impact.
2. **ESC-50 non-baby** (1960 clips across 49 classes). Diverse negatives.
3. **UrbanSound8K** (8732 clips, 10 classes). Real-world urban negatives — fans, HVAC, traffic.
4. **AudioSet broad negatives** — huge; overkill for a bedroom monitor.

---

## 6. Proposed Stage-2 integration

| When | Action | Doc reference |
|---|---|---|
| Stage 2.1 (already queued) | Real-audio recalibration (intervention 3.1) | `cry-detect-starter-plan.md`, `stage2-plan.md` |
| Stage 2.6a | Classification logging → collects the real-audio data stream | `classification-logging-plan.md` |
| Stage 2.7 | File API → pull collected audio from device | `file-api-plan.md` |
| **Stage 2.8 (new)** | Transfer-learned binary head (intervention 3.2) | *this doc* |
| Stage 3.1 (optional) | Personalised head (intervention 3.3) | Stretch |

Rationale: 2.6a + 2.7 generate the data. 2.8 consumes it. Without the logging + file API first, data collection is painful. **Don't start 2.8 until the infrastructure to feed it exists.**

---

## 7. What would change the calculus

Triggers that would push us into 3.3 (personalisation) or 3.4 (fine-tune):

- **If the user wants to publish a production baby monitor** with better-than-OEM accuracy — 3.4 might be worth it to squeeze the last 2 % of FP rate out.
- **If the baby has an unusual cry pattern** not well-represented in public datasets (e.g. medical condition) — 3.3 is the only path.
- **If Stage 2.1 and 2.8 together still leave > 2 % FP rate** after tuning — consider 3.4 as a last resort.

Otherwise: **3.1 + 3.2 is the plan. 3.3 is the polish.**

---

## 8. Summary

- **Don't retrain in the sense of "touch the weights" yet.** Recalibrate (3.1) first — it's the biggest and cheapest win.
- **Then add a transfer-learned binary head (3.2).** Purpose-built for our task; keeps the multi-class Y Amnet head alive for smoke/siren/etc.
- **Personalise (3.3) as an optional Stage-3 feature.** Massive gain for one household, nice "premium" baby-monitor story.
- **Skip full fine-tune (3.4).** Not worth it.

The word "marginal" applies only to 3.4. Everything else is meaningful.
