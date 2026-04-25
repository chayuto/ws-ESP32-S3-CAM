# Host-side auto-ensemble for binary audio-event labels

A reproducible, no-human-in-the-loop method for producing high-confidence
training labels from a stream of short ambient-audio captures, using
small classifiers stacked on top of a pretrained audio backbone.

This is the *method* document. The cry-detect-01 project uses it to
label baby-cry captures, but the recipe is sound-event-agnostic:
substitute the YAMNet class indices and feature definitions for any
binary audio-event task where you have a pretrained audio classifier
and tens-to-hundreds of unlabeled captures per night.

The companion `data-vault-redesign-20260425.md` covers *why* we replaced
human annotation with an ensemble. This doc covers *how* the ensemble
is built and why each piece is in it.

---

## 1. Pipeline at a glance

```
   one ~40 s WAV capture
            │
            ▼
 ┌──────────────────────────────────────────────────────────┐
 │ Oracle 1: pretrained backbone (YAMNet FP32, off TF Hub) │
 │   per-frame logits over 521 AudioSet classes            │
 │   → max over (target classes) → yam_pos_score           │
 │   → max over (confounder classes) → yam_neg_score       │
 │   → 1024-d embedding per frame, before classifier head  │
 └──────────────────────────────────────────────────────────┘
            │
            ├─→ frame embeddings → concat(mean, max) over frames → 2048-d
            │
            ▼
 ┌──────────────────────────────────────────────────────────┐
 │ Oracle 2: feat_clf — sklearn LogReg on 10 hand-features  │
 │   (HNR, voiced fraction, F0 mean/var, spectral centroid, │
 │    flatness, RMS, duration, etc.)                        │
 │ Oracle 3: embed_clf — sklearn LogReg on the 2048-d        │
 │   meanmax embedding from Oracle 1                         │
 │ Oracle 4: sub-type cluster — KMeans on Oracle 1's         │
 │   embeddings (or a cheaper feature subset). Cluster ID    │
 │   acts as a soft prior in the combiner.                   │
 │ (Optional) Oracle 5: temporal context — count of high-    │
 │   conf neighbors within ±N minutes. Cheap but powerful    │
 │   when captures are bursty.                               │
 └──────────────────────────────────────────────────────────┘
            │
            ▼
   confidence-tier combiner (spread + consensus)
            │
            ▼
   tier ∈ {high_pos, high_neg, medium_pos, medium_neg, low}
            │
            ▼
   training-eligible = high_pos ∪ high_neg
   research bucket  = low (oracles disagree — listen to these)
```

---

## 2. Oracle construction

### 2.1 The pretrained backbone (Oracle 1)

Use the **same** pretrained model you've quantized for on-device
inference, but at FP32 host-side. For audio events, YAMNet
(`google/yamnet/1` on TF Hub) is the obvious choice: 521-class AudioSet
output and a 1024-d embedding before the classifier head. INT8 PTQ for
the device is a separate concern; the host oracle stays FP32.

Two derived scores per capture:

- **`pos_score`** — `max` (or `mean`, see §6) over per-frame logits,
  restricted to your target's positive AudioSet classes
  (e.g. for cry: 20 *Baby cry, infant cry* + 21 *Crying, sobbing*).
- **`neg_score`** — same operation over your confounder classes
  (e.g. *Speech*, *Child speech*, *Music*). Tracking the confounder
  separately lets later layers reason about mixed audio rather than
  forcing one number to summarize both signals.

### 2.2 The feature classifier (Oracle 2)

A LogReg over ~10 hand-picked acoustic features chosen for the target
event. For cry-like signals these are interpretable:

| feature | rationale |
|---|---|
| HNR (harmonic-to-noise ratio) | tonal cries score high; broadband FPs score low |
| F0 mean / variance | baby cries cluster in 300–1000 Hz |
| voiced fraction | distinguishes voicing from impulsive noise |
| RMS peak / mean | gates very-quiet captures |
| spectral centroid | crude timbre proxy |
| spectral flatness | distinguishes tonal vs noise-like |
| capture duration | long durations → more context, more confident |

Keep this small (~10 features) and intentional. The point isn't to
match the backbone — it's to be *independent enough* that disagreement
is informative.

### 2.3 The embedding classifier (Oracle 3)

A LogReg over the backbone's pre-classifier embeddings, aggregated
across frames. **Aggregation matters**: see §6 — for cry-like events,
`max`-only (1024-d) was indistinguishable from `concat(mean, max)`
(2048-d), because the discriminative signal lives in the strongest
single frame and `mean` dilutes it.

We ship `concat(mean, max)` (2048-d) as the production feature for
robustness across other event types where mean might carry information.

### 2.4 The sub-type cluster (Oracle 4)

KMeans (small k — for cry we use k=4) over a fast feature subset.
For cry the four clusters are interpretable as fuss / full_cry /
distress / screech — not a hard label, but a useful soft prior in
the combiner. Captures whose cluster centroid disagrees with the
LogReg classifiers are a flag worth surfacing.

### 2.5 Temporal context (Oracle 5, optional)

Audio events of interest tend to come in bursts. A neighbor density
score — e.g. *count of captures with `pos_score ≥ 0.7` in ±5 min* —
is essentially free to compute and meaningfully reduces FPs on
isolated suspicious-but-ambiguous captures.

---

## 3. The combiner

A pairwise-agreement rule (any two oracles agree → decided) was our
first design and it doesn't survive a third oracle: it gives no useful
signal when 2 vs 1 versus 3 vs 0. Use **spread + consensus** instead:

```
scores      = [oracle_1, oracle_2, oracle_3, ...]   # all in [0,1]
spread      = max(scores) - min(scores)
consensus   = mean(scores)

high_pos    if spread < 0.2 AND consensus >= 0.7
high_neg    if spread < 0.2 AND consensus <= 0.1
medium_pos  if spread < 0.4 AND consensus >= 0.4
medium_neg  if spread < 0.4 AND consensus <  0.4
low         otherwise   (spread >= 0.4 — at least one oracle dissents)
```

The `low` tier is the *research bucket*: captures the oracles disagree
about. Listening to `low` is where you find genuine model gaps; you
should NOT use them for training.

`high_pos ∪ high_neg` is the training-eligible set. Tune the spread
and consensus thresholds for your event type — for rare events
(e.g. screams) tighten `consensus` so you don't accept weakly-positive
captures as positives.

---

## 4. Cross-validation: leave-one-session-out, never row-level

If your captures come from a small number of "sessions" (one
microphone, one room, contiguous time), they are heavily correlated:
mic position, ambient noise floor, even individual subjects
(one baby) repeat across rows. **Row-level CV will leak**: the model
memorizes the session, not the event.

Use **leave-one-session-out (LOSO) CV** — hold out an entire session,
train on the others. Report per-session metrics in addition to the
pooled OOF average. Numbers will be lower than row-level CV. They
will also be more honest.

A non-negotiable corollary: never select hyperparameters or compare
seeds on row-level CV scores when your data has session structure.

---

## 5. The /ml-researcher discipline

Every host-side training run in this project follows the
`/ml-researcher` slash command (see `.claude/commands/ml-researcher.md`
for the canonical version). The four rules:

1. **Pre-register.** Before any training, write down: hypothesis,
   falsifier, predicted outcome (a number or band), and method.
   "I expect AUC 0.97–0.99" is a measurable claim; "let's see what
   happens" is rummaging.
2. **Stamp model versions.** Every experiment carries a complete
   `config.json` with backbone version + checkpoint hash, deployed-
   tflite SHA256, ensemble version, feature-classifier seed, firmware
   build SHA, host Python/TF versions, and seed.
3. **Lab notebook in, conclusion out.** Lab notebooks (timestamped
   markdown + intermediate CSVs + plots) stay local — gitignored
   like raw data. Only the *durable conclusion* is committed.
4. **Negative results count as conclusions.** Failed experiments
   prevent re-treading. Document them as full research notes, not
   silent reverts. Two PTQ recalibration attempts cost a few hours
   each; without the writeup, future-us re-tries them.

---

## 6. Findings worth carrying forward

Empirical results from cry-detect-01 that informed the recipe above.
Numbers are LOSO across 4 sessions, n in the low hundreds, with the
underlying captures kept private — but the directions reproduce
across event types in our experience.

- **`max`-only ≈ `concat(mean, max)` for sparse events.** When the
  event of interest is concentrated in a small fraction of frames
  (a cry burst inside a 40 s capture), the strongest single frame's
  embedding carries the discriminative signal. Mean dilutes it.
  If your event is sustained (e.g. running engine), `mean` will
  matter more and `concat(mean, max)` is the right default.
- **A small LogReg beats a 2-layer MLP at n ≈ 250.** The MLP
  overfit the within-session structure even with early stopping
  and a 15 % validation split. Stick with LogReg until you have
  >>1k captures.
- **The third oracle's main job is to surface disagreement.** Adding
  `embed_clf` on top of `yam_score` + `feat_clf` shrank the high-tier
  set by ~3 % and *grew* the `low` tier 4×. That is the desired
  outcome — a third independent oracle should make you less
  confident about the captures it disagrees with, not more confident
  about the consensus ones.
- **Speech-overlapping events are the hardest mode** in our data.
  Three of three LOSO false negatives were captures with strong
  speech score AND strong cry score. Mitigation isn't a new oracle;
  it's more representation of mixed-audio events in the training
  pool. Track this explicitly via the `neg_score` channel from §2.1.
- **PTQ recalibration is a sharp tool.** Re-quantizing with
  real-data calibration *narrowed* the on-device output distribution
  in our setup, which raised the FP rate compared to synthetic-noise
  PTQ. Always evaluate PTQ end-to-end against held-out captures, not
  just by inspecting the calibration set's input distribution.

---

## 7. What you need to reproduce this

- A pretrained audio backbone (YAMNet from TF Hub costs nothing).
- ~150–300 captures of your target event domain, stored as 16 kHz
  mono WAVs of consistent length.
- ~30 lines of sklearn for each of `feat_clf`, `embed_clf`, and the
  cluster oracle.
- Discipline to (a) split by session not row, (b) pre-register every
  experiment, (c) commit the conclusion not the lab notebook.

The code in `projects/cry-detect-01/tools/` (`ensemble_audit.py`,
`freeze_release.py`, `build_inventory.py`, `audit_pipeline.sh`,
`score_yamnet.py`) is reusable as-is for any binary audio-event task;
the YAMNet class indices and feature set are the only points where
you'll edit. Specific captures, labels, and trained classifier weights
are kept local — see `CLAUDE.md` "Publish boundary".
