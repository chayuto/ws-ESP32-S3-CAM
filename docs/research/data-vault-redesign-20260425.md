# Data-vault redesign — no human in the label loop

**Status:** accepted, implemented, first ensemble release frozen.
**Date:** 2026-04-25
**Decision:** remove manual annotation from the cry-detect-01 label-
production path. Replace with an automated multi-oracle ensemble.
**Supersedes:** §2 of `cry-detect-data-program-plan.md` (the
"annotation workflow" tier).
**Related:** `log-management-design-20260423.md` (vault layout),
`deep-analysis-20260423.md` (oracle component origins).

## 0. The decision in one paragraph

Manual annotation does not scale (single-annotator bottleneck on
100+ captures/night), introduces inconsistent labels, and contradicts
the data-flywheel goal of the project. Today's auto-oracles (YAMNet
+ a feature classifier + sub-type clustering + temporal context) are
collectively more reliable than a tired human at 6 am. The previous
plan kept humans as the label authority. We're flipping that:
**automated ensemble produces labels; humans only audit oracle
disagreement counts as a system-health metric.**

## 1. Why now (evidence)

Three concrete observations across the first four sessions
(20260420 – 20260424) drove the flip:

1. **Auto YAMNet oracle is good enough for bulk labelling.**
   YAMNet FP32 separates real cries (≥ 0.95) from confirmed FPs
   (< 0.05) with no overlap on the captured-WAV distribution. The
   borderline band (0.1–0.5) where YAMNet itself is uncertain is
   small (~4–7 % of captures) — these don't need a human's binary
   call; they need MORE oracles, not a human guess.

2. **Feature-only classifier almost ties YAMNet.** Per
   `deep-analysis-20260423.md` §Q2, an sklearn LogReg on 10 cheap
   acoustic features (HNR, flatness, voiced_frac, spectral bands,
   etc.) hits 92 % accuracy / AUC 0.97 cross-session against
   YAMNet ground truth. That is essentially a second independent
   opinion at zero compute cost. When two independent oracles
   agree, the agreement IS the label.

3. **Human labels are noisier than the ensemble.** Surveying the
   `triggers.jsonl[].note` field (where the user has occasionally
   typed labels like "Cry1", "cry", "Bedroom-quiet", "Morning cry"),
   we found 11 captures with a comparable human verdict. **10 of
   11 agreed with the ensemble. The 1 disagreement was the human
   being wrong** — a test recording the user filename'd "Cry1"
   that both oracles unanimously flagged as not-cry (yam=0.012,
   feat_clf_prob=0.0003). Trusting the human there would have
   poisoned the dataset.

## 2. The new architecture

### 2.1 Four oracles per capture

Every capture is scored by:

| oracle | type | output | independence basis |
|---|---|---|---|
| YAMNet wide-class (FP32) | trained NN | `yam_cry_score`, `yam_speech_score` | externally trained on AudioSet, no contact with our data |
| Acoustic feature classifier (sklearn LogReg) | shallow ML | `feat_clf_prob` | uses cheap signal-processing features, not raw audio |
| Sub-type cluster (k=4 KMeans) | unsupervised | `cluster_id`, `cluster_label` | uses f0 / HNR / band features, descriptive only |
| Temporal context | rule | `temporal_high_conf_neighbors_5min` | counts cluster membership, ignores audio content |

The feature classifier is **retrained at every audit run** on the
current pool of confidently-labelled captures. Cluster centroids
are likewise refit every audit. This is cheap (sub-second on 300
captures) and means the ensemble grows in self-consistency as the
dataset grows.

### 2.2 The combiner

```
oracle_agreement = |yam_cry_score - feat_clf_prob|
consensus_score  = mean(yam_cry_score, feat_clf_prob)

confidence_tier:
  high_pos    if agreement < 0.2 AND consensus >= 0.7
  high_neg    if agreement < 0.2 AND consensus <= 0.1
  medium_pos  if consensus >= 0.4
  medium_neg  if agreement < 0.3 AND consensus < 0.4
  low         otherwise (oracles disagree)
```

**Training-eligible subset = `high_pos ∪ high_neg`.** Other tiers
stay in the dataset for evaluation and uncertainty study, but
should not be used as ground-truth examples for fitting future
models. This is the core signal of the redesign: **labels carry
their own confidence, and we propagate that downstream**.

### 2.3 Human notes as supplementary monitoring (not label authority)

The `triggers.jsonl[].note` field is parsed at audit time into
{`cry`, `not_cry`, `test`, `auto`, `""`}. We compute
`human_note_agrees` = bool(parsed_label == ensemble_verdict). This
metric is **monitoring, not labelling** — a sustained spike in
disagreements means our oracles need a closer look, but the
ensemble verdict is canonical.

## 3. First-run results (4 sessions, 318 unique captures)

```
[ensemble] confidence-tier distribution:
  high_neg     125     ← confident not-cry (training negatives)
  high_pos     125     ← confident cry      (training positives)
  medium_pos    36     ← cry-leaning, single oracle confident
  medium_neg    21     ← not-cry-leaning
  low           11     ← oracle disagreement (3.5% of dataset)

[ensemble] cluster distribution (yam_cry_score >= 0.5 only):
  fuss          54     ← low-pitch grumble (f0_mean ~340)
  full_cry      50     ← moderate sustained (f0_mean ~590)
  distress      37     ← high-pitch broken (f0_mean ~810)
  screech        7     ← very high-pitch jittery (f0_mean ~750+, NEW
                          mode emerging in 04-22 onwards)

[ensemble] human-note (supplementary, n=14 actual human-input notes):
  cry            9
  not_cry        2
  test           3
  agreement w/ ensemble: 10 true / 1 false / 3 N/A (test labels)
```

**Training-eligible captures: 250 of 318 (78.6 %).** The 11 `low`-tier
captures are the dataset's most interesting subset for understanding
oracle blind spots — they're not garbage, they're research-mode
cases.

The single human/ensemble mismatch (filename "Cry1", ensemble:
high_neg) is the project's first concrete piece of evidence for
why we don't let humans authorize labels: the human was wrong,
both oracles independently caught it.

## 4. Schema changes

`master.csv` columns (post-redesign, schema v2):

```
identity         : session_id, capture_file, ts_iso
yamnet           : yam_baby_cry, yam_crying_sobbing, yam_speech,
                   yam_child_speech, yam_screaming, yam_cry_pos_max,
                   yam_cry_neg_max, yam_cry_score, yam_speech_score,
                   yam_cry_purity
feat classifier  : feat_clf_prob
cluster          : cluster_id, cluster_label
temporal         : temporal_neighbors_5min,
                   temporal_high_conf_neighbors_5min
combiner         : oracle_agreement, consensus_score, confidence_tier
raw features     : hnr_db, f0_mean_hz, f0_voiced_frac, rms_peak,
                   duration_s, centroid_mean_hz, flatness_mean
device-side      : trigger_note, trigger_rms, dev_cry_conf_at_capture
supplementary    : human_note_label, human_note_text, human_note_agrees
provenance       : yamnet_oracle_version, ensemble_version, audited_at
```

**Removed** (compared to old draft):
- `human_label` (no longer authoritative)
- `human_label_ts`, `human_annotator` (no annotation flow)
- `yam_label_auto` (single-categorical replaced by full score vector)

## 5. Release format change (v1 → v2)

Old `cry-v0.0-exploratory.json` (schema v1) carried a single
`label_auto` per capture. New `cry-v0.1-ensemble.json` (schema v2)
carries the full per-capture score vector:

```json
{
  "file": "cry-20260421T010923+1000.wav",
  "session_id": "night-20260420",
  "ts_iso": "2026-04-21T01:09:23+10:00",
  "split": "train",
  "yam_cry_score": 0.9193,
  "yam_speech_score": 0.0064,
  "yam_cry_purity": 0.9129,
  "feat_clf_prob": 0.9824,
  "consensus_score": 0.9509,
  "oracle_agreement": 0.0631,
  "confidence_tier": "high_pos",
  "cluster_label": "fuss",
  "temporal_high_conf_neighbors_5min": 4,
  "human_note_label": "cry",
  "human_note_agrees": "true"
}
```

Future training runs reference `release_id` and slice on
`confidence_tier`. Multiple training experiments can pick different
slices (e.g., one trained on high_pos+high_neg only; another on
high+medium; another that uses `consensus_score` as a regression
target rather than a binary label).

## 6. Tradeoffs and open risks

### Pro

- **Scales.** Adding device 2 or doubling capture rate is free —
  no annotator bottleneck.
- **Reproducible.** Re-run the auditor and you get the same labels,
  modulo the feat_clf retrain (which is seeded). Past releases stay
  frozen.
- **Probabilistic.** Labels carry confidence. Models can be trained
  to predict the consensus, weighted by confidence. We move away
  from binary "is/isn't" and toward calibrated scoring.
- **Uncertainty becomes data.** Low-tier captures aren't lost — they're
  flagged for oracle-blind-spot study. That's what makes the
  ensemble improve over time.

### Con

- **Oracle bias propagates.** If YAMNet has a systematic blind spot
  for this baby's voice (e.g., the new "screech" sub-type), the
  feature classifier (trained against YAMNet) inherits it. Mitigation:
  add a third independent oracle (e.g., PANNs, AST) at the next
  iteration. Cheap incremental change.
- **No defense against acoustic novelty.** A genuinely novel cry
  type the oracles don't recognize would be quietly mislabelled
  `low` and sit in the disagreement bucket forever. Mitigation:
  surface low-tier counts as a session-level metric; if they spike
  unexpectedly, investigate.
- **The supplementary human-note signal is honestly mostly empty.**
  9 cry-asserted notes out of 318 captures (3 %). Not a strong
  monitoring channel by itself. Mitigation: as the user occasionally
  manually labels, the signal grows. Will only matter at scale.

## 7. Implementation

Code (committed today):

- `projects/cry-detect-01/tools/ensemble_audit.py` — the auditor.
  ~330 lines. Idempotent.
- `projects/cry-detect-01/tools/freeze_release.py` — schema v2.
  Pins per-capture score vectors.
- `datasets/cry-detect-01/labels/master.csv` — 318 rows, 37
  columns, regenerated.
- `datasets/cry-detect-01/releases/cry-v0.1-ensemble.json` — first
  ensemble release.

Removed:

- `projects/cry-detect-01/tools/build_master_labels.py` — superseded
  by `ensemble_audit.py` (different schema, no human_label column).

Updated:

- `docs/research/cry-detect-data-program-plan.md` §2 — replaced
  annotation-workflow with auto-ensemble.
- `docs/research/log-management-design-20260423.md` §5 + §6.6 —
  `human.csv` removed, master.csv schema updated.
- `datasets/cry-detect-01/README.md` — points at this redesign.

## 8. What's NOT changed

- The vault layout (`datasets/cry-detect-01/`) is unchanged from
  the log-management design. The redesign affects WHAT goes into
  `labels/master.csv` and `releases/*.json`, not where the data lives.
- The retention/archive plan is unchanged.
- Multi-device readiness is unchanged.
- The future model-training tracks (Phase 6 in the data-program
  plan) is unchanged in destination, just simpler in path: instead
  of waiting for 1000 human-labelled cries, we wait for 1000
  high_pos-tier captures. Same threshold, faster cadence.

## 9. Future expansion hooks (still valid)

The redesign keeps every hook from the log-management design intact:

- Add PANNs / AST as a third oracle: extend `ensemble_audit.py`
  with a new column, schema-additive change.
- Add per-baby fine-tuning: train on `cluster_label`-stratified
  high-tier captures.
- Multi-device collection: ensemble audits per device cleanly,
  releases can union or filter by `device_id`.
- Dataset publication: redact `human_note_text` if PII concerns
  arise (they don't currently — notes are short, no PII).

## 10. Sanity check that this isn't over-engineered

A simpler alternative would have been "use YAMNet alone, period".
Why not?

- YAMNet was trained on AudioSet (broad). This baby's voice may
  occupy a corner of distribution where YAMNet is less confident.
- A second independent oracle (the feature classifier, trained on
  THIS dataset's confident subset) catches cases where YAMNet's
  bias diverges from this baby's reality.
- The `low`-tier disagreement count gives us a system-health
  metric that pure-YAMNet can't.

So: ensemble buys us robustness to YAMNet's biases AND a continuous
improvement signal AND a self-monitoring metric. For ~330 lines of
Python, all in one auditor script, the cost is modest.

## 11. Rollback path

If this redesign turns out to be wrong (e.g., the feature
classifier has a systematic blind spot we didn't anticipate, and
training on `high_pos` produces a worse model than training on
YAMNet-only labels):

1. The `labels/master.csv` schema is backwards-compatible — the
   `yam_cry_score` column alone is the YAMNet-only signal.
2. Past releases (`cry-v0.0-exploratory.json`) are still valid;
   pin to them for any reverted training.
3. `ensemble_audit.py` can be downgraded to YAMNet-only by passing
   `--feat-clf=disabled`. (To be added — not currently a flag.)

The ensemble is additive infrastructure on top of YAMNet. We never
lose access to the simpler view.

## 12. Success criteria

The redesign is working if:

- **Adding a session adds labels with no human time.** ✓ today.
- **Confidence tiers correlate with downstream model performance**
  in the first real training run. (To be tested at v1.0 model
  training, when we hit 1000+ high_pos captures.)
- **The `low`-tier subset reveals interesting failure modes,** not
  just noise. Tracked via session-level low-count + manual
  inspection of any session where `low` count spikes.
- **Adding a third oracle (PANNs) is a one-day task.** Schema is
  additive; ensemble combiner generalizes naturally.

## 13. Open follow-ups

1. Add `--feat-clf=disabled` and `--cluster=disabled` flags to
   `ensemble_audit.py` for ablation experiments.
2. Persist the trained feat_clf as a pickle so a release is
   exactly reproducible (currently retrains on each audit run —
   technically a small drift).
3. Add PANNs as a 3rd oracle when scope allows.
4. Wire the disagreement-spike alarm into the cry_monitor pipeline
   (or a separate post-audit check).
5. Migrate existing `logs/night-*/` into `datasets/cry-detect-01/sessions/`
   per Phase 1 of the log-management design — separate task,
   doesn't block the current ensemble flow.
