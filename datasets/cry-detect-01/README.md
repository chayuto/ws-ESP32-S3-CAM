# cry-detect-01 dataset root

Canonical store for captured audio, labels, and frozen dataset releases —
intended to outlive the current firmware/tooling.

Layout follows `docs/research/log-management-design-20260423.md` §5,
with the **2026-04-25 redesign** that removed humans from the label
loop (`docs/research/data-vault-redesign-20260425.md`).

```
datasets/cry-detect-01/
  sessions/          per-session captures + raw logs + derived audits
  labels/master.csv  cross-session ensemble label ledger
  releases/          frozen snapshots for reproducible training runs
```

This tree is NOT the live capture path — the device writes to its
own `/sdcard/` and the host pulls nightly into
`projects/cry-detect-01/logs/`. Phase 1 migration (per the log-mgmt
design §9.1) will lift closed sessions into `sessions/` here.

## Labels are produced by an automated ensemble (no human in the loop)

`tools/ensemble_audit.py` walks all `logs/night-*/` and produces
`labels/master.csv`. Each capture gets a per-capture score vector
from four oracles:

1. **YAMNet wide-class** (FP32 oracle, primary)
2. **Acoustic-feature classifier** (sklearn LogReg, retrained at audit time)
3. **Sub-type cluster** (k=4 KMeans on f0/HNR/band features)
4. **Temporal context** (cluster membership in ±5 min)

A combiner produces `confidence_tier ∈ {high_pos, high_neg, medium_pos,
medium_neg, low}`. Training-eligible captures are `high_pos ∪ high_neg`
(oracles agree strongly).

**Human notes** (`triggers.jsonl[].note` parsed) are kept as a
SUPPLEMENTARY signal in `human_note_label` and `human_note_agrees`.
They never override the ensemble verdict — see the redesign note for
the misalignment evidence.

## Status (2026-04-25, post-redesign)

- **Ensemble auditor:** built and running. `master.csv` has 318 unique
  captures across 4 sessions.
- **First ensemble release:** `cry-v0.1-ensemble.json` (250 train-
  eligible captures, splits 192 / 63 / 63 train/val/test).
- **Older release:** `cry-v0.0-exploratory.json` retained for diff
  reference.
- **Phase 1 migration** (lift live sessions into `sessions/` per
  the log-mgmt design): not yet started — current ensemble pipeline
  reads directly from `logs/night-*/`.
- **Phase 2** (firmware `/session/begin` / `/session/end` endpoints):
  not yet started.

## Important properties

- **Sessions are immutable once written.** Re-running the auditor
  produces a new `master.csv` but never mutates raw WAVs / triggers /
  device-logs.
- **Raw vs derived:** WAVs + `triggers.jsonl` + `device-logs/*` are raw.
  `manifest.csv`, `yamnet_files.csv`, `master.csv`, releases are
  derived — regeneratable from raw + tooling version.
- **Labels live separately from data.** Re-auditing never changes raw.
- **Dataset releases pin per-capture score vectors** — a frozen release
  survives any future `master.csv` schema change.

## Tooling

Built and in `projects/cry-detect-01/tools/`:

- `ensemble_audit.py` — produces `master.csv` from all sessions.
  Idempotent. Re-run after each new session or oracle upgrade.
- `freeze_release.py <release_id>` — snapshots current `master.csv`
  into `releases/<release_id>.json`.

Not yet built (tracked in log-management design §9):

- `tools/session_lifecycle.sh` — wrapper for `/session/begin + end`
  HTTP calls (requires firmware endpoints).
- `tools/migrate_session_to_dataset.py` — lift `logs/night-*/` into
  `datasets/.../sessions/`.
- `tools/weekly_purge.sh` — device-side purge after `_raw_ok`.

## Related docs

- `docs/research/data-vault-redesign-20260425.md` — the no-human
  redesign (this document's defining decision).
- `docs/research/log-management-design-20260423.md` — full
  end-to-end design.
- `docs/research/cry-detect-data-program-plan.md` — project goals
  and tier roadmap.
- `docs/research/deep-analysis-20260423.md` — feature classifier and
  sub-type cluster origin work.
- `docs/research/night-session-2026042{0,1,2,4}.md` — per-session
  reports.
