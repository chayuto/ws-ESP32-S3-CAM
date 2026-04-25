# cry-detect-01 — ML experiment lab notebooks

This directory holds **gitignored** experiment notebooks and
intermediate artifacts for ML research on cry-detect-01. Only the
conventions document (this README) and a `.gitkeep` are in git.

Created and used by the `/ml-researcher` Claude Code skill (see
`.claude/commands/ml-researcher.md`).

## Purpose

Mirrors the data-vault discipline: **raw + intermediate is gitignored**;
only durable conclusions land in git (under `docs/research/`).

A single experiment lives at:

```
ml-experiments/YYYY-MM-DD-<topic-slug>/
├── README.md            ← lab notebook (timestamped entries)
├── config.json          ← exact config + seeds + data slice + MODEL VERSIONS
├── artifacts/           ← models, intermediate CSVs, plots
└── results.json         ← machine-readable summary
```

## Required: log every model version active during the experiment

Per the conventions in `.claude/commands/ml-researcher.md`, every
experiment's `config.json` must record:

```json
{
  "experiment_id": "2026-04-25-embed-clf-v0.1",
  "git_head_sha": "<short hash at experiment start>",
  "data_slice": "cry-v0.1-ensemble.json (high_pos+high_neg)",
  "models_used": {
    "yamnet_oracle":        "google/yamnet/1 (FP32 from TF Hub)",
    "yamnet_int8_tflite":   "spiffs/yamnet.tflite (sha256 ABCD…, calib synthetic)",
    "ensemble_version":     "v0.1",
    "feat_clf":             "sklearn LogReg seed=0 trained at <iso>",
    "subtype_kmeans":       "k=4 seed=0 fit at <iso>",
    "firmware_build_sha":   "9a786780 (mel-fix)",
    "yamnet_class_map":     "hf/yamnet_class_map.csv (sha256 …)"
  },
  "host_python": "3.13",
  "host_tf":     "2.21.0",
  "seed":        0
}
```

**Why model versions in every log:**

We have already shipped 4 firmware builds (broken, sigmoid-fix,
mel-fix, mel-fix-rev2) and at least 2 ensemble versions. A result
that holds under one model often regresses under another; without
the version stamp, future-us can't reproduce or compare.

The `tools/build_inventory.py` regen surfaces the currently-active
model versions in `INVENTORY.md` for at-a-glance comparison with
any experiment's `config.json`.

## Conventions in brief

- Pre-register hypothesis BEFORE running anything (in `README.md`).
- Save every intermediate artifact under `artifacts/`.
- Update `README.md` live with timestamped entries — short bullets
  are fine, polish later.
- On conclusion: write a research note to `docs/research/<topic>.md`
  ONLY if the result is significant or negative-and-instructive.
  Null/unsurprising results stay in the lab notebook.
- Always set + log seeds (`numpy.random.default_rng(seed)`,
  `random.seed(seed)`, sklearn `random_state`).
- Hold out by **session**, not by row, for cry-detect-01.
- Always include a baseline; ablate when a positive result lands.

## Why gitignored

- Notebooks evolve fast; commit churn is noise.
- Intermediate models can be ~MB each; don't bloat git.
- Conclusions are what future-us reads, and those go in
  `docs/research/`. The notebook is the audit trail, not the output.

## Subdirectory listing

This README lives in git. Subdirectories (`YYYY-MM-DD-<topic>/`) and
all their contents are local-only. Re-running an experiment from
scratch should be reproducible from `config.json` + the data slice
in the master ledger — that's the contract.
