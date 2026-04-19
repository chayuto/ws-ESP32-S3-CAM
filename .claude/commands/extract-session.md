# Close out a capture session

One-shot mirror of the deployed device + audit run for an overnight
capture. Idempotent — safe to rerun partway through the night, or to
refresh a session where the WAVs are already downloaded but device
logs keep growing.

Usage: `/extract-session <session_dir> [host]`

- `<session_dir>` — e.g. `logs/night-20260419` (will be created if
  missing).
- `[host]` — defaults to `http://192.168.1.100`.

---

## What it does

Executes `projects/cry-detect-01/tools/extract_session.sh`, which:

1. **Kills the running bash monitor** if one was started from the same
   session dir. (Looks up `<session_dir>/monitor.sh` in `pgrep`.)
2. **Mirrors `/sdcard/events/`** → `<session_dir>/wavs/*.wav` +
   `<session_dir>/triggers.jsonl`. Existing files are skipped.
3. **Mirrors `/sdcard/` root logs** (`infer-*.jsonl`, `cry-*.log`,
   `infer-boot.jsonl`, `CRY-0000.LOG`) → `<session_dir>/device-logs/`.
   These are re-downloaded every run because the device keeps appending
   while online.
4. **Runs the audit pipeline** via `tools/audit_pipeline.sh` —
   produces `manifest.csv` / `segments.csv` / `yamnet_files.csv` /
   `yamnet_segments.csv` / `specgrams/*.png`.
5. **Writes a `README.md` stub** if none exists yet. Fill it in
   before archival (see the taxonomy rules in
   `docs/research/file-management-strategy-20260419.md`).

## Conventions

- Session directory name: `logs/night-YYYYMMDD`, dated by the
  **start** of the night (the evening the deployment began).
- Device is assumed reachable at `http://192.168.1.100`; override if
  running on a different LAN.
- Never edits files already present. Won't `rm` WAVs.
- If the device is offline at the time the command runs, the mirror
  step short-circuits gracefully and the audit runs on whatever is
  already on disk.

## When to invoke

- End of every overnight capture, first thing in the morning.
- After any accidental monitor death (data since last download will
  still come through because infer-*.jsonl is day-bucketed and
  append-only on the device).

## Related

- `/audit-wavs <dir>` — just the analysis stage (if WAVs are already
  local and you only want to re-run audit).
- `docs/research/file-management-strategy-20260419.md` — why the
  directory layout and retention rules exist.
