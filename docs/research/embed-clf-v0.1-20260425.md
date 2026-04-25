# embed-clf v0.1 — third ensemble oracle for cry-detect-01

**Date:** 2026-04-25
**Experiment ID:** `2026-04-25-embed-clf-v0.1` (lab notebook in
`projects/cry-detect-01/ml-experiments/`, gitignored)
**Status:** SHIPPED — integrated into `ensemble_audit.py` v0.2,
released as `cry-v0.2-ensemble.json`.
**Outcome:** hypothesis confirmed + exceeded.

## Hypothesis

A small classifier trained on YAMNet's pre-classifier 1024-d embeddings
(aggregated per WAV via concat(mean, max) → 2048-d) will achieve **LOSO
CV accuracy ≥ 95 %, AUC ≥ 0.97**, beating the existing acoustic-feature
classifier (92 % / 0.975 per `deep-analysis-20260423.md` §Q2). Worth
adding as a 3rd ensemble oracle.

## Result (LOSO CV across 4 sessions, n=250)

| model | features | LOSO acc | LOSO AUC | TPR @ FPR=5% | confusion (TN/FP/FN/TP) |
|---|---|---:|---:|---:|---|
| **embed_clf v0.1** | YAMNet meanmax (2048-d) | **0.988** | **0.998** | **0.992** | 125 / **0** / 3 / 122 |
| embed_clf MLP | YAMNet meanmax (2048-d) | 0.928 | 0.970 | 0.888 | 115 / 10 / 8 / 117 |
| ablation: mean only | YAMNet mean (1024-d) | 0.972 | 0.994 | 0.984 | 123 / 2 / 5 / 120 |
| ablation: max only | YAMNet max (1024-d) | 0.988 | 0.998 | 0.992 | 125 / 0 / 3 / 122 |
| ablation: random frame | YAMNet single frame (1024-d) | 0.744 | 0.828 | 0.456 | 91 / 34 / 30 / 95 |
| baseline: feat_clf §Q2 | 10 hand-features | 0.92 | 0.975 | — | — |

**Conclusion:** LogReg on YAMNet embeddings beats the feat_clf baseline
by 7 percentage points of accuracy and 2 of AUC; **zero LOSO false
positives** across 250 captures.

The MLP underperforms LogReg — at n=250 the more flexible model overfits.
Stick with LogReg.

## Ablation findings

- **max-only is identical to meanmax** (0.988 / 0.998 in both). The cry
  signal lives in the strongest single frame's embedding; mean
  aggregation dilutes it. We keep meanmax as the production feature
  for robustness across other sound types where mean might matter.
- **mean-only is slightly worse** (0.972 / 0.994), confirming max
  carries the cry-discriminative info.
- **Random single frame** drops to 0.744 / 0.828 — confirms that *some*
  aggregation across the 40 s WAV is essential.

## False-negative analysis

All 3 LOSO false negatives are from `night-20260424`, all with
`yam_speech_score > 0.96`:

| capture | yam_cry | yam_speech | feat_clf | embed_clf | cluster |
|---|---:|---:|---:|---:|---|
| `cry-20260423T185005+1000.wav` | 0.84 | 0.97 | 0.996 | 0.06 | distress |
| `cry-20260423T185923+1000.wav` | 0.97 | 0.96 | 0.94 | 0.003 | fuss |
| `cry-20260423T193404+1000.wav` | 0.94 | 0.9999 | 0.97 | 0.13 | full_cry |

These are **cry-with-speech mixed audio** — caregiver speaking near a
crying baby. The other 3 training sessions have fewer of these mixes;
when 04-24 is held out, embed_clf hasn't seen the mode. **Zero false
positives** indicates the model isn't generally over-cautious — only
this specific mixed mode is under-represented.

This is the data gap that adding a 3rd oracle is *supposed* to surface.

## Production integration

`tools/ensemble_audit.py` bumped `ENSEMBLE_VERSION` from `v0.1` to
`v0.2`. Now runs three oracles:

```
yam_cry_score   (YAMNet wide-class FP32, primary)
feat_clf_prob   (sklearn LogReg on 10 acoustic features, retrained at audit)
embed_clf_prob  (sklearn LogReg on YAMNet meanmax embeddings — NEW)
```

New combiner uses 3-oracle spread + consensus:

```
spread     = max(scores) - min(scores)
consensus  = mean(scores)
high_pos   if spread < 0.2 AND consensus >= 0.7
high_neg   if spread < 0.2 AND consensus <= 0.1
medium_pos if spread < 0.4 AND consensus >= 0.4
medium_neg if spread < 0.4 AND consensus <  0.4
low        otherwise (≥ 0.4 spread = at least one oracle dissents)
```

## Confidence-tier shift (v0.1 → v0.2)

| tier | v0.1 (2 oracles) | v0.2 (3 oracles) | Δ |
|---|---:|---:|---:|
| high_pos | 125 | 118 | −7 |
| high_neg | 125 | 128 | +3 |
| medium_pos | 36 | 13 | −23 |
| medium_neg | 21 | 12 | −9 |
| low | 11 | **47** | **+36** |
| **train-eligible** | 250 | 246 | −4 |

The 36 newly-low-tier captures are exactly the desired output of a
3rd independent oracle — captures the 2-oracle ensemble treated as
settled but where embed_clf provides a contrarian view. Most of those
36 are mid-confidence captures (medium_pos in v0.1) where embed_clf
is more decisive in either direction; the rest are speech-overlapping
cries that embed_clf currently calls not-cry.

Net training-eligible loss is small (4 captures, 1.6 %). Quality of
the high-tier set is HIGHER because it now requires 3-oracle agreement
instead of 2.

## Files shipped

| path | what |
|---|---|
| `projects/cry-detect-01/hf/embed_clf_v0.1.pkl` | trained scaler + LogReg, 65 KB |
| `projects/cry-detect-01/tools/ensemble_audit.py` | v0.2 with 3rd oracle |
| `datasets/cry-detect-01/labels/master.csv` | regenerated, 38 columns |
| `datasets/cry-detect-01/releases/cry-v0.2-ensemble.json` | frozen release |
| `datasets/cry-detect-01/INVENTORY.md` | regenerated |

## Reproducibility

The lab notebook (gitignored) at
`projects/cry-detect-01/ml-experiments/2026-04-25-embed-clf-v0.1/`
contains:

- `extract_embeddings.py` — runs YAMNet on all 250 train-eligible
  captures, caches embeddings to `artifacts/embeddings.npz`.
- `train_eval.py` — LOSO CV + ablations + comparison.
- `save_final_model.py` — trains on all 250, saves the deployed pickle.
- `config.json` — full model-version stamp at experiment start.
- `results.json` — machine-readable summary.

Re-running these scripts reproduces today's pickle deterministically
(seed=0, sklearn 1.x, sklearn pickle is version-coupled — note the
host_python and sklearn versions in config.json before re-running).

## Limitations + open follow-ups

1. **Mixed-audio cry-with-speech is under-represented.** 3 LOSO false
   negatives all share this mode. Mitigation: as more sessions
   accumulate, retrain. Or upweight mixed-audio examples explicitly.
2. **sklearn pickle is version-coupled.** Future-us upgrading sklearn
   may need to retrain. For long-term portability, export model
   coefficients to JSON or convert to ONNX.
3. **embed_clf is host-side only.** Can't run on the ESP32 (the YAMNet
   trunk is needed to produce embeddings). When data scale supports it,
   we'd train a from-scratch on-device cry head and replace YAMNet's
   final layer with our 4-class head.
4. **The 47 low-tier captures are a research bucket** — listening to
   them and characterizing what makes them hard is the next experiment.

## Lessons (for future ml-researcher invocations)

- **Pre-registering predicted bands worked.** I predicted 0.97-0.99 AUC;
  actual 0.998. Knowing the upper bound was 0.99 made me sanity-check
  that 0.998 wasn't a leakage bug (it's not — LOSO CV holds out whole
  sessions, no row leakage possible).
- **Always run an ablation.** "max-only is as good as meanmax" is a
  surprise finding that I'd have missed without ablation. Useful for
  thinking about future on-device approximations.
- **Negative-result discipline kept the day moving.** This is the
  positive sequel to `data-reassessment-20260425.md` §A's negative
  re-PTQ result. Both stayed in research notes; future-us can read
  both and not re-tread.
