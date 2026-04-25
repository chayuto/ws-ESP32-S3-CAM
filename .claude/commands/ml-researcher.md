# ML researcher mode for cry-detect-01

Activate the scientist's mindset for any ML / data-science work in this
repo: form a hypothesis, run a controlled comparison, log everything for
reproducibility, and surface only the *durable conclusions* in the
research-doc tree.

Usage: `/ml-researcher <topic>` — explicit invocation, OR
       activate this mode whenever the work is ML/data-science (training
       a model, tuning a threshold, running an ablation, calibrating
       quantization, etc.).

Examples of when to use this mode:

- "Train a per-baby cry head on YAMNet embeddings."
- "Calibrate the firmware detector threshold from session data."
- "Test whether a third oracle (PANNs) improves disagreement rate."
- "Compare INT16 vs INT8 PTQ on the cry head."

---

## The principle

Two failed re-PTQ attempts (commit `35cf4be` 2026-04-25) cost a few hours
each and only produced durable knowledge BECAUSE we documented the
negative result. Without that discipline they'd be silent failures
re-tried later.

**Every ML experiment generates two output streams:**

1. **Internal log** (gitignored, like raw data): hypothesis statement,
   exact config, every numeric result, plots, intermediate models.
   This is the *lab notebook*. It's noisy, large, often boring.
2. **Durable conclusion** (committed to `docs/research/`): a research
   note that records what we tried, what happened, and what to do next.
   Short, scannable, future-self-friendly.

We must produce BOTH. Skipping the lab notebook means we can't verify
the conclusion. Skipping the conclusion means future-us re-treads the
same path.

---

## Workflow

### Step 1 — Pre-register the hypothesis

Before touching any tool, write down (in a fresh notebook entry):

```
Hypothesis: <1-2 sentences. What we EXPECT to happen and why.>
Falsifier:  <what result would tell us we're wrong>
Method:     <baseline + intervention + ablation, named explicitly>
Data slice: <e.g. "all high_pos + high_neg from
            cry-v0.1-ensemble.json", or specific session list>
Predicted outcome: <pre-commit a number or direction>

Model / version stamp (REQUIRED — see §Step 1.5)
```

This is non-negotiable. If you don't know what you expect, you're not
running an experiment — you're rummaging.

### Step 1.5 — Stamp every model version in `config.json`

Every experiment must carry a complete model/version manifest at start.
This isn't paperwork — we have already shipped 4 firmware builds and 2
ensemble versions. A result that holds under one model often regresses
under another; without version stamps, future-us can't reproduce or
diagnose drift.

Required fields in `<experiment_dir>/config.json`:

```json
{
  "experiment_id": "YYYY-MM-DD-<slug>",
  "git_head_sha": "<short hash at experiment start>",
  "data_slice": "<release id or session list>",
  "models_used": {
    "yamnet_oracle":        "google/yamnet/1 (FP32 from TF Hub)",
    "yamnet_int8_tflite":   "spiffs/yamnet.tflite (sha256 …, calib synthetic|real-data)",
    "ensemble_version":     "v0.1 | v0.2 | …",
    "feat_clf":             "sklearn LogReg seed=N (retrained at <iso>)",
    "subtype_kmeans":       "k=4 seed=N (fit at <iso>)",
    "firmware_build_sha":   "<8-char ELF hash from /metrics, e.g. 9a786780>",
    "yamnet_class_map":     "hf/yamnet_class_map.csv"
  },
  "host_python": "<X.Y>",
  "host_tf":     "<X.Y.Z>",
  "seed":        0
}
```

Helper: `tools/build_inventory.py` prints the currently-active model
versions at the top of `INVENTORY.md` so the values can be copied
verbatim into `config.json`.

### Step 2 — Allocate an experiment dir

```
projects/cry-detect-01/ml-experiments/YYYY-MM-DD-<topic-slug>/
├── README.md                 ← the lab notebook (start with the
│                                pre-registration above)
├── config.json               ← exact config / seeds / data slice
├── artifacts/                ← models, intermediate CSVs, plots
└── results.json              ← machine-readable summary
```

This directory is **gitignored** (matches the data-vault discipline:
raw and intermediate stays out of git).

### Step 3 — Run the experiment

Iterate freely. Save EVERY intermediate result to `artifacts/`. Update
the lab notebook (`README.md`) live as you go — short bullet entries
timestamped, no need to be polished. Capture what didn't work as well
as what did.

When you change anything significant (different seed, different data
slice, different baseline), update `config.json` and note the change
in the lab notebook with the timestamp.

### Step 4 — Conclude

Decide one of:

- **(A) Significant outcome — durable.** Write a research note
  summarizing hypothesis, method, result, conclusion, next steps.
  Include numerical evidence inline. Cross-reference the experiment
  dir in case future-us wants to re-run.
  - **Where to write it:** `docs/internal/<topic>-YYYYMMDD.md` if it
    references real captures, capture timestamps, or numbers derived
    from our private data (this is the usual case — `docs/internal/`
    is gitignored). Use `docs/research/<topic>-YYYYMMDD.md` only when
    the note is purely about method/tooling and would be meaningful
    without our data. If unsure, default to `docs/internal/` — see
    `CLAUDE.md` "Publish boundary".
  - Notes in `docs/internal/` are kept local. Notes in `docs/research/`
    are committed.

- **(B) Null / unsurprising — ephemeral.** Update the lab notebook
  with the conclusion, leave the experiment dir on disk, do NOT write
  a separate research note. The notebook is enough — it's there if a
  future experiment needs to verify what we already saw.

- **(C) Negative result — durable.** Treat as (A). Negative results
  prevent re-treading. Same private/public split as (A) — most land
  in `docs/internal/` because they reference our data.

The boundary is "would future-us benefit from learning this in 3 months
without reading the lab notebook?" If yes → (A) or (C). If no → (B).

### Step 5 — Update inventory if needed

If the experiment changed `master.csv` (e.g. added an oracle), re-run
`tools/build_inventory.py` so `INVENTORY.md` reflects current state.
Both `master.csv` and `INVENTORY.md` are gitignored (under `datasets/`)
— regenerating refreshes the local snapshot only; nothing to commit.

---

## Conventions

- **Notebook format:** `README.md` with timestamped entries. No need
  for Jupyter — markdown + numpy + matplotlib + sklearn covers
  everything we do. Persist plots as PNGs in `artifacts/`.
- **Seeds:** always set + log `numpy.random.default_rng(seed)`,
  `random.seed(seed)`, and any sklearn `random_state`.
- **Comparisons:** always include a baseline. Single-treatment
  results without a baseline are observations, not experiments.
- **Held-out:** for cry-detect-01 specifically, hold out by
  SESSION (e.g. leave-one-session-out CV), not by row. The data
  has heavy within-session correlation; row-level splits leak.
- **Ablations:** when a positive result lands, ablate at least one
  component before believing it.
- **Naming:** `YYYY-MM-DD-<topic-slug>` for experiment dirs.
  Example: `2026-04-25-embed-clf-v0.1`.

---

## Standard tooling

These already exist in `projects/cry-detect-01/tools/` and most ML
experiments will compose them:

| tool | purpose |
|---|---|
| `ensemble_audit.py` | regenerate `master.csv` ensemble labels |
| `freeze_release.py <id>` | snapshot current state into a release |
| `build_inventory.py` | refresh `INVENTORY.md` for quick check |
| `audit_pipeline.sh` | numeric features + YAMNet oracle on a WAV dir |
| `score_yamnet.py` | YAMNet FP32 inference on raw WAVs |

Reusable starting code:

| script | location |
|---|---|
| Train sklearn LogReg on features | embedded in `ensemble_audit.py` |
| YAMNet TF-Hub loading | `/tmp/diag_mel_drift.py` (port if needed) |
| INT8 PTQ harness | `tools/repTQ_yamnet.py` |
| Per-WAV cry contour features | `/tmp/cry_subtypes.py` (port if needed) |

---

## When to commit

- Tools (training script, evaluator, etc.) → commit if they're
  reusable. One-off scratch under `ml-experiments/<dir>/` stays
  gitignored.
- Modified `ensemble_audit.py` → commit (it's production tooling).
- New oracle adapter → commit if it's a real component.
- Updated `master.csv` after running the auditor → commit.
- New release `.json` → commit.
- Research note → commit.
- Lab notebook + experiment dir → never commit (gitignored).

---

## Anti-patterns to avoid

- **Trying multiple seeds and reporting the best.** Set a seed,
  state the seed, run multiple seeds and report distribution if
  the result is unstable.
- **Cherry-picking favorable splits.** Use leave-one-session-out
  CV consistently.
- **No pre-registered hypothesis.** Without it you can rationalize
  any outcome as success.
- **Skipping the negative-result writeup.** It's the only way to
  prevent re-treading. Future-us will not remember why we already
  ruled something out.
- **Inflating tooling.** The 4-oracle ensemble is the right scale
  for our data size. A neural-net ensemble would be over-engineered
  here. Match complexity to data quantity.

---

## Cross-references

- `docs/research/data-vault-redesign-20260425.md` — why we have an
  ensemble in the first place.
- `docs/research/data-reassessment-20260425.md` — what data is
  usable for what experiment.
- `docs/research/cry-detect-data-program-plan.md` — long-term roadmap
  + threshold for first real training run (1000 captures).
- `datasets/cry-detect-01/INVENTORY.md` — quick-reference current state.
- `projects/cry-detect-01/ml-experiments/README.md` — directory
  conventions.
