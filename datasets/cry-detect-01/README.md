# cry-detect-01 dataset root

This is the dataset archive for the cry-detect-01 project. It is the
**canonical store** for captured audio, labels, and frozen dataset
releases — intended to outlive the current firmware/tooling.

Layout follows `docs/research/log-management-design-20260423.md` §5.

```
datasets/cry-detect-01/
  sessions/          per-session captures + raw logs + derived audits
  labels/            cross-session master label ledger + history
  releases/          frozen snapshots for reproducible training runs
```

This tree is NOT the live capture path — the device writes to its
own `/sdcard/` and the host pulls nightly into `projects/cry-detect-01/logs/`.
The migration script (`tools/migrate_session_to_dataset.py`) lifts
closed sessions from the live tree into this dataset tree, applying
the target schema.

## Status (2026-04-23)

- **Phase 1 migration:** in progress. 3 sessions (20260420, 20260421,
  20260422) in migrate queue. No sessions migrated yet — scaffold only.
- **Phase 2+:** not started (firmware endpoints `/session/{begin,end}`
  TBD, see design doc §9).
- **First dataset release:** planned as `cry-v0.0-exploratory.json`
  after migration completes.

## Important properties (see design doc §2 for rationale)

- **Sessions are immutable once `_raw_ok` marker lands.** Do not edit
  raw files in a closed session — treat as append-only label history.
- **Raw vs derived:** `events/`, `triggers.jsonl`, `heartbeat.jsonl`,
  `infer.jsonl`, `boots.jsonl`, `meta.json` are raw. Anything under
  `derived/audit-vN/` is regeneratable from raw + tooling version.
- **Labels live separately from data.** Adding / revising labels never
  mutates raw.
- **Dataset releases pin exact WAV list + label state** at a moment
  in time — reproducible training.

## Not yet built (tracked in log-management design §9)

- `tools/session_lifecycle.sh` — wrapper for /session/begin + end
  HTTP calls (requires firmware endpoints from Phase 2).
- `tools/migrate_session_to_dataset.py` — lift a `logs/night-*/` into
  `datasets/.../sessions/` with schema transforms.
- `tools/freeze_release.py` — snapshot current WAV + label state to
  `releases/cry-vX.Y.json`.
- `tools/weekly_purge.sh` — device-side purge after `_raw_ok`.

## Related docs

- `docs/research/log-management-design-20260423.md` — full design
- `docs/research/cry-detect-data-program-plan.md` — why this project
  exists and what's downstream
- `docs/research/night-session-2026042{0,1,2}.md` — per-session
  reports (pre-migration)
