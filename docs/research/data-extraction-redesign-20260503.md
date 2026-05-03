# Data extraction redesign — Wi-Fi-only, resumable, SD-managed-on-device

**Date:** 2026-05-03
**Context:** Phase B firmware planning. Pairs with Phase A (distilled student
side-by-side) but is independent — can ship before, after, or alongside.

## Problem

Pulling a session off the device today is "manual chore" tier:

- ~1 GB / 10 days of inference JSONL + ~250 MB of WAVs over Wi-Fi at ~2 MB/s
  per session.
- Day-bucketed JSONL files are mutable until midnight, so `extract_session.sh`
  blows them away and re-pulls them every sync — even the parts already
  mirrored.
- 200+ small WAV files = 200+ HTTP round trips.
- Idempotency lives in the *script*, not the protocol: re-running into a fresh
  session dir bypasses it (already burned us once on 2026-05-03).
- No partial-transfer resume — a Wi-Fi blip mid-pull means restart from zero.
- Retention is silent: device has been quietly deleting files older than the
  last sync because the host fell behind.

## Hard constraints (this redesign)

1. **No physical SD removal, ever.** All transfer over Wi-Fi.
2. **Resumable.** Network blip, host laptop sleep, device reboot — next sync
   picks up where the last one stopped, no manual fixup.
3. **Idempotent at the protocol level**, not the script level. Re-running sync
   into any state must converge.
4. **SD lifecycle is the device's job.** It manages free space, prefers
   deleting already-synced data, signals the host when retention pressure
   threatens un-synced data.
5. **Bandwidth-efficient at steady state.** A "weekly catch-up" should be
   megabytes, not gigabytes.

## Current state (what we have)

`projects/cry-detect-01/main/file_api.c`:
- `GET /files/ls?path=...` — directory listing (name, type, size).
- `GET /files/get?path=...` — full-file download, no Range support.
- `GET /files/stat`, `head`, `tail`, `rm`, `df`, `coredump/*`.

`projects/cry-detect-01/main/metrics_logger.c:109`:
- Day-bucketed `infer-YYYYMMDD.jsonl` filenames, written via
  `snprintf("%s/infer-%s.jsonl", mount, day)`.

`projects/cry-detect-01/main/event_recorder.c`:
- WAVs in `/sdcard/events/cry-<ISO>.wav`, immutable once closed.
- Retention: `CRY_REC_KEEP_FILES` (default 500), oldest-first deletion.

`projects/cry-detect-01/main/log_retention.c`:
- Day-buckets older than `CRY_LOG_RETENTION_DAYS` (default 14) deleted hourly.

`projects/cry-detect-01/tools/extract_session.sh`:
- Lists `/sdcard/events`, pulls each WAV (skip if local file exists), refreshes
  `triggers.jsonl`, lists `/sdcard` root, re-pulls every JSONL/log/LOG.

The retention rule + day-bucket model is the root of most pain. Every sync,
the entire current and prior days re-stream over Wi-Fi.

## Phase B sync subsystem

Seven small additions, all gated behind Kconfig (default off until validated),
all additive — existing `/files/*` endpoints stay.

### 1. Hourly-bucketed, gzipped inference logs

- Replace `infer-YYYYMMDD.jsonl` with `infer-YYYYMMDDTHH.jsonl`.
- On hour rollover, close the current bucket and gzip it in place →
  `infer-YYYYMMDDTHH.jsonl.gz`.
- Closed `.gz` buckets are immutable — host syncs them once, never re-pulls.
- Only the *current* hour's file is mutable.
- gzip on text JSONL = ~10–15× smaller. 100 MB/day day-bucket → ~7 MB/day of
  closed `.gz` buckets, plus a ~300 KB tail-file at any moment.

WAVs stay PCM; on-device Opus encode is a heavier lift, deferred.

### 2. Manifest endpoint

`GET /manifest.json?since=<unix_seconds>` returns:

```json
{
  "generation_id": "20260510T120000Z",
  "device_uptime_s": 12345,
  "files": [
    {
      "path": "/sdcard/events/cry-20260510T143022+1000.wav",
      "size": 1310572,
      "mtime": 1748456422,
      "sha256": "ab12...",
      "sync_state": "pending",
      "category": "wav"
    },
    {
      "path": "/sdcard/infer-20260510T13.jsonl.gz",
      "size": 312411,
      "mtime": 1748452800,
      "sha256": "cd34...",
      "sync_state": "synced",
      "category": "infer_log"
    }
  ],
  "truncated": false
}
```

- `since` filters to files with `mtime > since`. Empty `since` = full list.
- `sha256` lets host detect mid-write corruption + skip already-verified
  matches (e.g. after a host-side reformat).
- `truncated: true` if the response had to be capped (memory bound) — host
  pages with a smaller `since` window.
- `generation_id` is bumped on factory-reset / SD-format / clock-sync-discovers-
  large-jump. If the host's stored `generation_id` differs, treat the local
  mirror state as untrusted and re-verify by sha.

### 3. Range-capable GET

Extend `file_api_get` to honor `Range: bytes=N-` headers (single-range only,
which is enough for resume). Response sets `Content-Range: bytes N-(M-1)/M`
and status `206 Partial Content`. No support for multi-range; clients that
need it can fall back to full GET.

### 4. Sync state + ack endpoint

Each file has a sync state: `pending` (default on creation) → `synced` (after
host fetched). State stored in a small ledger file
`/sdcard/.sync-ledger.jsonl` (append-only, compacted hourly). Periodic flush;
worst-case loss on crash is "host re-acks files it already had," which is
idempotent.

`POST /sync/ack` body:
```json
{ "files": [{"path": "...", "sha256": "..."}] }
```

Device verifies sha matches its current view, flips state to `synced`. Bulk
acks are normal (host sends after a batch).

### 5. Free-space-driven retention

Replace `CRY_REC_KEEP_FILES=500` and `CRY_LOG_RETENTION_DAYS=14` with a single
free-space target:

- `CRY_SD_TARGET_FREE_PCT=20` (keep at least 20% of card free).
- Retention task wakes every N seconds. If free% is below target:
  1. Delete oldest `synced` files first (WAVs and closed `.gz` buckets
     equally eligible, oldest-first across categories).
  2. If no `synced` files exist and free% is *still* below target, raise
     `sync.pending_pressure=true` in `/metrics`. **Do not delete pending
     files** unless free% drops below a hard floor (e.g. 5%).
  3. If forced to delete pending data (hard floor), increment
     `sync.unsynced_dropped_count` and log a warning.

Effect: a host that's online and acking keeps the device tidy automatically.
A host that's offline for weeks accumulates unsynced data, the device tells
us via `/metrics`, and we can intervene before data is lost.

### 6. Session boundary markers

On boot:
- If `/sdcard/.session-started-*` doesn't exist, write
  `/sdcard/.session-started-<ISO8601>` with a small JSON body
  `{"firmware_git_sha": "...", "boot_count": N, "generation_id": "..."}`.
- These files are part of the manifest. Host pulls them once and uses them
  as natural session-boundary markers in the local mirror.

On factory reset / SD-format:
- Bump `generation_id`, write a new `.session-started-*`.
- `.session-closed-*` is optional — written by an explicit
  `POST /admin/close-session` if we ever want one, otherwise a session is
  "closed" the moment the next `.session-started-*` appears.

### 7. /metrics surface for sync health

New fields under a `sync:` block:

```json
"sync": {
  "pending_count": 14,
  "synced_count": 1832,
  "bytes_pending": 8472013,
  "oldest_pending_age_s": 720,
  "pending_pressure": false,
  "unsynced_dropped_count": 0,
  "generation_id": "20260510T120000Z"
}
```

Host monitors these. `oldest_pending_age_s > N hours` or
`pending_pressure=true` are the actionable signals.

## Host-side workflow

Replace `extract_session.sh` with `tools/sync.py` (or `sync_loop.sh`):

```
mirror_dir/
├── .sync/
│   ├── state.json          # {last_manifest_mtime, generation_id, host_id}
│   ├── pending_acks.jsonl  # files pulled but not yet acked
│   └── checksums.idx       # local sha256 index for fast diff
├── wavs/cry-*.wav
├── infer/infer-*.jsonl.gz
└── triggers.jsonl
```

Sync loop:

1. `GET /manifest.json?since=<state.last_manifest_mtime>` — read delta.
2. If `generation_id` differs from local: re-verify all local files by sha256
   before treating as mirrored.
3. For each entry:
   - If absent locally → fetch with Range resume into `<path>.partial`,
     verify sha, atomic rename.
   - If present locally + sha matches → no-op.
   - If present locally + sha mismatches → fetch fresh into `.partial` (the
     remote file has changed; only legitimate for the current mutable
     hour-bucket).
4. Batch all newly-verified paths, `POST /sync/ack`.
5. Update `state.json`. Sleep N minutes. Repeat.

Properties this gives us:

- **Resumable.** `.partial` files + Range header. Kill the host mid-pull,
  re-run, it picks up byte-exact.
- **Idempotent at the protocol level.** Manifest is source of truth; host
  computes diff. Running twice does the same thing as running once.
- **Continuous, not session-based.** No "session dir" ceremony — there's just
  one mirror that's continuously diffed against. Session boundaries are
  retrieved as marker files, used for analysis grouping but not for sync
  state.
- **SD never fills with synced data.** Device acks-driven retention removes
  what host has already mirrored.
- **Bandwidth-cheap at steady state.** Manifest is small (kilobytes per call
  even with thousands of files); pull volume = exactly the new bytes.

The tool can run as cron (every 15 min), as systemd timer, or as a
foreground command. No long-running foreground process needed.

## SD lifecycle without physical access

This is the constraint that drives most of the above. Walk through failure
modes:

- **Host laptop offline for a week.** Manifest grows (`pending_count` rises).
  Device retention deletes synced files first; SD doesn't fill from new
  pending data alone (250 MB/week WAVs + tens of MB compressed logs is well
  inside an 8 GB partition for >1 month). When host comes back, pulls the
  delta in one go.
- **Host laptop offline for a month+.** Approaching pending pressure;
  `/metrics` exposes it. We can intervene by raising the free-space floor
  setting via menuconfig + reflash, or accept lossy retention — at our data
  volume this is unlikely to bite for 2-3 months.
- **Wi-Fi unreachable mid-pull.** `.partial` file persists locally; next
  sync resumes via Range.
- **Device reboot mid-pull.** `.partial` still valid (file was immutable
  except for the active hour-bucket); resume via Range. If it was the active
  bucket, sha mismatch → re-fetch the whole bucket.
- **SD card corruption.** Caught by sha mismatch on next sync. Operationally
  this needs a soft-format admin endpoint anyway (see future work).
- **Clock skew on device.** Manifest uses absolute mtime; if device clock
  jumped, `since` filter could miss files. Mitigation: `generation_id`
  bumps on large clock jumps, host falls back to full sha-diff.

## Migration plan

This is the Phase B firmware push the user is preparing for.

1. **Manual fresh-start (operator-driven, pre-flash).** No automatic format.
   Before flashing Phase B, the user wipes the SD by hand using the existing
   file API — see "Fresh-start procedure" below. Boot of the new firmware
   then writes the first `.session-started-<ISO>` against an empty card.
   Rationale: auto-format-on-boot is silently destructive if the wrong
   firmware lands on the wrong device, or if the flag survives a copy-paste.
   Operator confirmation is the right safety gate for a one-shot wipe.
2. **Old workflow continues to work** during rollout — the existing
   `/files/*` endpoints aren't touched, `extract_session.sh` still functions
   if needed, just falls back to the slow path.
3. **New sync.py runs alongside** until validated. Host can pull via both;
   they're idempotent against each other.
4. **Sunset the old extract_session.sh** once sync.py has run cleanly for a
   week. Move it to `tools/legacy/` as a fallback.

### Fresh-start procedure (manual, pre-Phase-B-flash)

Steps (no firmware change required for this; uses today's API):

1. Stop any active sync / monitor on the host.
2. Confirm anything you wanted to keep is mirrored locally (or accept the
   loss explicitly).
3. With the device still running its current firmware, recursively delete
   contents under `/sdcard/events/` and `/sdcard/*.{jsonl,log,LOG}` via
   `POST /files/rm` (one path at a time today; a small
   `tools/wipe_sd.sh` helper that walks `/files/ls` + per-entry `rm` is
   ~30 lines and worth writing once we commit to this).
4. `GET /metrics` → confirm `sd_free_pct` jumped.
5. Flash Phase B. First boot writes `.session-started-<ISO>` against the
   empty card and the new sync workflow takes over.

The `/files/rm` endpoint already exists (`file_api.c:204`) — no new firmware
needed for the wipe step itself. The helper script just makes step 3 a
one-liner instead of an interactive curl loop.

## Kconfig knobs (proposed)

```
config CRY_SYNC_HOURLY_BUCKETS
    bool "Use hourly-bucketed inference logs (gzipped on close)"
    default n
    help
        Switches metrics_logger from day-bucketed to hourly-bucketed
        inference logs. Closed buckets are gzipped in place. Once this is
        validated for a release cycle, default flips to y.

config CRY_SYNC_MANIFEST_ENABLED
    bool "Expose /manifest.json + /sync/ack endpoints"
    default n
    depends on CRY_SYNC_HOURLY_BUCKETS

config CRY_SYNC_RANGE_GET
    bool "Support Range: bytes=N- on GET /files/get"
    default y
    help
        Cheap to support, no protocol cost when client doesn't ask. Safe to
        ship enabled by default.

config CRY_SYNC_FREE_SPACE_RETENTION
    bool "Use free-space-target retention (replaces count/day caps)"
    default n
    depends on CRY_SYNC_MANIFEST_ENABLED
    help
        Requires sync state tracking to know which files are 'synced' and
        therefore safe to delete first. Falls back to count/day caps if
        sync state is unavailable.

config CRY_SD_TARGET_FREE_PCT
    int "Target free space on SD (%)"
    default 20
    range 5 80
    depends on CRY_SYNC_FREE_SPACE_RETENTION

config CRY_SD_HARD_FREE_PCT
    int "Hard floor before deleting unsynced data (%)"
    default 5
    range 1 20
    depends on CRY_SYNC_FREE_SPACE_RETENTION
```

(No `CRY_SD_WIPE_ON_FIRST_BOOT` knob — fresh-start is operator-driven via
the existing `/files/rm` endpoint, not firmware-automated. See the
fresh-start procedure under "Migration plan".)

## Implementation cost (rough)

- Hourly buckets + gzip-on-close: ~150 lines in `metrics_logger.c`, plus
  miniz or zlib component (already in IDF).
- Manifest endpoint: ~250 lines new module `sync_api.c`. SHA-256 from
  mbedtls (already linked).
- Sync state ledger: ~200 lines, append-only JSONL with hourly compaction.
- Range support in `file_api_get`: ~80 lines (parse header, set
  `Content-Range`, `fseek` + `httpd_resp_send_chunk` from offset).
- Free-space retention: ~150 lines in `log_retention.c`, repurpose existing
  scan + add ledger lookup.
- Host `sync.py`: ~400 lines Python + standard libs only.

Total firmware delta: ~700 lines. Host: ~400 lines. Plenty of flash + PSRAM
headroom (memory budget doc 2026-05-03 confirms 3.79 MB free in app
partition).

## Out of scope (deferred)

- **On-device Opus / FLAC for WAVs.** Big lift; PCM + gzipped logs already
  cuts steady-state bytes by ~5×. Revisit if SD pressure becomes a real
  signal in production.
- **Push model (device POSTs to host).** Conceptually clean but needs a
  host-side service running 24/7. Pull model with cron is simpler and
  matches our usage pattern.
- **Multi-range / batched archive endpoint.** Tarball of N new WAVs in one
  request. Marginal gain over pipelined keep-alive GETs at our file count;
  defer.
- **Encryption at rest / in flight.** Local network only; not worth the
  battery / CPU cost yet.
- **Admin SD-format endpoint.** Bulk wipe is currently a manual operator
  step using existing `/files/rm` calls (see "Fresh-start procedure"). A
  proper `POST /admin/sd-format` with auth + double-confirm could replace
  the recursive-rm dance — defer until the manual flow proves annoying.

## Cross-references

- Memory budget post-Phase A: `cry-detect-01-memory-budget-20260503.md`
- Phase A integration plan:
  `student-integration-plan-20260503.md`
- File-api current handlers: `projects/cry-detect-01/main/file_api.c`
- Inference log path templating: `projects/cry-detect-01/main/metrics_logger.c:109`
- Existing extract script: `projects/cry-detect-01/tools/extract_session.sh`
