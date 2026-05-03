# Phase B implementation plan — concrete

**Date:** 2026-05-03
**Bundle:** B.1 + B.2 + B.4 in one flash. Defers B.3 (retention rewrite) and
B.5 (gzip-on-close) until B.1+B.2 have soaked.
**Pairs with:** `data-extraction-redesign-20260503.md` (the design rationale).

This is the file-by-file delivery plan: what gets added or changed, the
function signatures and on-disk schemas, and the validation steps that must
pass before the device is considered cut over to the new sync flow.

## Scope summary

| Phase | Component | Lines | Lands in this push |
|---|---|---:|---|
| B.1 | Range support in `file_api_get` | ~80 | Yes |
| B.1 | Hourly buckets in `metrics_logger.c` (raw JSONL, no gzip) | ~80 | Yes |
| B.1 | Session markers on boot in `main.c` | ~40 | Yes |
| B.2 | New `sync_ledger.c/.h` (.sync-ledger.jsonl) | ~250 | Yes |
| B.2 | New `sync_api.c/.h` (`/manifest.json`, `/sync/ack`) | ~300 | Yes |
| B.2 | `metrics.sync.*` fields | ~50 | Yes |
| B.2 | Hooks in `metrics_logger.c`, `event_recorder.c` | ~30 | Yes |
| B.4 | `tools/sync.py` host client | ~450 | Yes |
| B.4 | `tools/wipe_sd.sh` recursive-rm helper | ~50 | Yes |
| B.3 | Free-space-target retention | ~200 | **No, follow-up** |
| B.5 | gzip-on-close for closed buckets | ~150 | **No, follow-up** |

Total firmware delta this flash: ~830 LOC C + 30 LOC CMake. Host: ~500 LOC
shell+Python. All Kconfig-gated; defaults preserve current behavior.

## Files added

```
projects/cry-detect-01/main/sync_ledger.h      (NEW)
projects/cry-detect-01/main/sync_ledger.c      (NEW)
projects/cry-detect-01/main/sync_api.h         (NEW)
projects/cry-detect-01/main/sync_api.c         (NEW)
projects/cry-detect-01/tools/sync.py           (NEW)
projects/cry-detect-01/tools/wipe_sd.sh        (NEW)
```

## Files modified

```
projects/cry-detect-01/main/file_api.c         — Range support in file_api_get
projects/cry-detect-01/main/metrics_logger.c   — hourly buckets, ledger hook
projects/cry-detect-01/main/event_recorder.c   — ledger hook on WAV close
projects/cry-detect-01/main/main.c             — session marker, sync_api init
projects/cry-detect-01/main/metrics.h          — sync.* fields in cry_metrics_t
projects/cry-detect-01/main/metrics.c          — sync.* in metrics_to_json
projects/cry-detect-01/main/web_ui.c           — register sync_api URIs
projects/cry-detect-01/main/CMakeLists.txt     — add new sources
projects/cry-detect-01/main/Kconfig.projbuild  — new Phase B menu
```

## Kconfig surface (proposed)

New submenu under "Cry-detect-01" → "Sync subsystem (Phase B)":

```kconfig
menu "Sync subsystem (Phase B)"

    config CRY_SYNC_HOURLY_BUCKETS
        bool "Hourly-bucketed inference logs (infer-YYYYMMDDTHH.jsonl)"
        default n
        help
            Replaces day-bucketed JSONL with hourly. Closed hour-buckets are
            immutable and registered with the sync ledger; the host's sync.py
            pulls each closed bucket exactly once. Only the current hour's
            file is mutable. No gzip yet — that lands as B.5 once core-0
            CPU cost has been measured.

    config CRY_SYNC_RANGE_GET
        bool "Support Range: bytes=N- on /files/get (HTTP 206)"
        default y
        help
            Single-range only. Cheap to support, no protocol cost when
            client doesn't send Range. Defaults to y because it's safe and
            unblocks the new sync.py resume path.

    config CRY_SYNC_LEDGER_ENABLED
        bool "Enable sync ledger (.sync-ledger.jsonl tracks per-file state)"
        default n
        help
            Append-only JSONL on /sdcard/.sync-ledger.jsonl. Written when a
            file is closed (WAV / hour-bucket) and on /sync/ack. Required
            for /manifest.json to report sha256 + sync_state.

    config CRY_SYNC_API_ENABLED
        bool "Expose /manifest.json + /sync/ack endpoints"
        default n
        depends on CRY_SYNC_LEDGER_ENABLED
        help
            Manifest is paginated (since=<unix_seconds>) and chunk-streamed.
            Ack flips per-file sync_state from pending → synced. Old
            /files/* endpoints stay live regardless.

    config CRY_SYNC_SESSION_MARKER
        bool "Write /sdcard/.session-started-<ISO> on first boot"
        default n
        help
            One marker per boot, idempotent (skipped if a marker for the
            current boot_count already exists). Enables host to identify
            session boundaries without a server-side database.

endmenu
```

All gated knobs default `n` so a Phase B build with no `sdkconfig` overrides
behaves identically to the current firmware — the new code paths are dead
code until you flip the knobs in `sdkconfig.defaults` (or via menuconfig).

For the upcoming flash, the build's `sdkconfig.defaults` will set:

```
CONFIG_CRY_SYNC_HOURLY_BUCKETS=y
CONFIG_CRY_SYNC_RANGE_GET=y
CONFIG_CRY_SYNC_LEDGER_ENABLED=y
CONFIG_CRY_SYNC_API_ENABLED=y
CONFIG_CRY_SYNC_SESSION_MARKER=y
```

## On-disk schemas

### `/sdcard/.sync-ledger.jsonl` (append-only, hourly compaction)

One row per event. Latest row per `path` wins. Compaction reads tail-to-head,
dedups by `path`, atomic-renames the result back over the original.

```json
{"ts":1748456422,"op":"register","path":"/sdcard/events/cry-...wav","size":1280044,"mtime":1748456400,"sha256":"abc...","category":"wav"}
{"ts":1748460000,"op":"close","path":"/sdcard/infer-20260510T13.jsonl","size":312411,"mtime":1748460000,"sha256":"cde...","category":"infer_log"}
{"ts":1748462100,"op":"ack","path":"/sdcard/events/cry-...wav","sha256":"abc...","sync_state":"synced"}
{"ts":1748466000,"op":"compact","entries":2031}
```

`op` values:
- `register` — file just appeared (initial state = `pending`)
- `close` — alias for `register`, used when an active file finishes (the close
  side has the final size+sha)
- `ack` — host has verified its mirror; flips state to `synced`
- `compact` — bookkeeping marker, not a state change
- `purge` — file deleted on device (retention or rm); host can drop from local
  index later

In-memory state (after replay) per path: `{size, mtime, sha256, sync_state,
category}`. Loaded into a fixed-size hash table at boot (`SYNC_LEDGER_MAX_FILES
= 4096`, ~256 KB PSRAM). Overflow → log + force compaction.

### `/sdcard/.session-started-<ISO8601>.json`

```json
{"firmware_git_sha":"abc1234","boot_count":17,"generation_id":"20260510T120000Z","mounted_at":"2026-05-10T12:00:00+10:00"}
```

`generation_id` is stored in NVS (`sync.gen_id` key). Bumped manually via
admin endpoint or `nvs_namespace_erase`. Default value at first boot is the
current ISO8601 datetime.

## Manifest endpoint

`GET /manifest.json?since=<unix_seconds>&limit=<N>`

- `since` filters to files with `mtime > since` (default 0 = full list).
- `limit` caps row count (default 500, max 2000).
- Response paginated via `truncated: true` + `next_since: <mtime_of_last>`.
- Mutable files (current hour bucket): `sha256` field is `null`, `mutable: true`.
- Streams via `httpd_resp_send_chunk` to keep stack/heap bounded.

```json
{
  "generation_id": "20260510T120000Z",
  "device_uptime_s": 12345,
  "now": 1748466122,
  "files": [
    {
      "path": "/sdcard/events/cry-20260510T143022+1000.wav",
      "size": 1280044,
      "mtime": 1748456422,
      "sha256": "abc...",
      "sync_state": "pending",
      "category": "wav",
      "mutable": false
    }
  ],
  "truncated": false,
  "next_since": null
}
```

## Ack endpoint

`POST /sync/ack` — body is JSON:

```json
{
  "files": [
    {"path": "/sdcard/events/cry-...wav", "sha256": "abc..."},
    {"path": "/sdcard/infer-20260510T13.jsonl", "sha256": "cde..."}
  ]
}
```

- For each entry: ledger lookup by `path`, sha256 match required, flip
  `sync_state` to `synced`.
- Returns `{"acked": N, "rejected": M, "rejected_paths": [...]}`.
- Bulk acks are normal (host sends after a batch fetch).

## Range protocol

Client sends `Range: bytes=N-`. Server:
- If header absent or malformed → existing path (200 OK, full file).
- If valid → seek to N, send `206 Partial Content`,
  `Content-Range: bytes N-(M-1)/M`, `Content-Length: M-N`. Stream the rest
  via `httpd_resp_send_chunk`.
- Single-range only. `Range: bytes=N-M` (closed) accepted; multi-range
  rejected with `416 Range Not Satisfiable`.

## Host: tools/sync.py

```
sync.py [--host HOST] [--mirror DIR] {once,loop,verify,dry-run,init}
```

- `init <dir>` — bootstrap a mirror dir with `.sync/state.json`.
- `once` — single sync pass, exit when complete.
- `loop --interval 900` — daemon mode.
- `verify` — re-hash every local file, compare against device manifest.
- `dry-run` — fetch manifest, print plan, exit without pulling.

Mirror layout:

```
mirror/
├── .sync/
│   ├── state.json          # {last_manifest_mtime, generation_id, host_id}
│   ├── pending_acks.jsonl  # files pulled but not yet acked
│   └── checksums.idx       # local sha256 cache (path → sha)
├── wavs/cry-*.wav
├── infer/infer-YYYYMMDDTHH.jsonl
└── markers/.session-started-*.json
```

State machine per manifest entry:

```
              local missing → fetch (.partial → verify → rename) → ack queue
              local present + sha match → skip
              local present + sha mismatch + mutable → re-fetch (current hour)
              local present + sha mismatch + immutable → log corruption, re-fetch
              mutable + no sha → always re-fetch
```

`generation_id` change → re-verify all local files by sha before treating as
mirrored.

## Validation steps

### Pre-flash (sanity, against current firmware)

1. `tools/wipe_sd.sh --dry-run http://192.168.1.100` → lists what would be
   deleted, exits without acting.
2. (Optional) Real wipe; expect `/files/df` to show free space jump.

### Post-flash (with new firmware on empty SD)

1. **Boot up** — confirm `/sdcard/.session-started-<ISO>.json` appears.
2. **Range** — `curl -H "Range: bytes=10-29" /files/get?path=/sdcard/.session-started-...json` → 206 + 20 bytes.
3. **Hourly bucket** — wait for inference task to start, confirm
   `/sdcard/infer-YYYYMMDDTHH.jsonl` exists (not the day-bucket).
4. **Manifest** — `curl /manifest.json` → JSON with at least the session
   marker file. After ~1 hour: confirm hour rolled over, ledger shows
   prior hour as closed with sha + size, current hour as `mutable:true`.
5. **End-to-end host pull** — `tools/sync.py --host 192.168.1.100
   --mirror /tmp/mirror init && sync.py once` — pulls everything,
   verifies sha, sends ack. `/metrics.sync.synced_count` rises.
6. **Resume** — kill `sync.py once` mid-pull (curl returning bytes), check
   `.partial` file exists, re-run, completes correctly.
7. **Idempotence** — `sync.py once` immediately after step 5 → no fetches,
   no acks (everything already verified).
8. **Reboot device** — ledger survives, manifest after reboot still shows
   prior `synced` state.

## Risks + mitigations

| Risk | Mitigation |
|---|---|
| Ledger corruption from crash mid-write | Append-only writes, fsync after each row, hourly atomic-rename compaction. Boot-time recovery: if ledger missing or unparseable, scan SD and rebuild as all-pending |
| Manifest sha256 compute on every request | Cache shas in ledger; only compute fresh for closed-but-not-yet-registered files. Mutable files skip sha entirely |
| Manifest response too large | Pagination via `since` + `limit`; `httpd_resp_send_chunk` streams output (no buffer-the-whole-thing) |
| Hour rollover races inference logger | Single-writer pattern: `metrics_logger`'s task owns the file pointer, no other task writes hour-buckets |
| Clock skew makes manifest `since` unreliable | `generation_id` bump on large clock jump (>1 hour delta) forces host re-verify |
| Session marker proliferates | Boot writes one marker per boot; existing marker for current `boot_count` is skipped |
| `extract_session.sh` users surprised by hourly files | Old script's `*.jsonl` filter still matches `infer-*T*.jsonl` (strictly more permissive). No-op for them |

## What this push does NOT do

- **Does not change retention** — old `CRY_REC_KEEP_FILES=500` and
  `CRY_LOG_RETENTION_DAYS=14` stay in force. B.3 lands later.
- **Does not compress on device** — every JSONL is raw text; gzip waits for
  B.5 after CPU measurement.
- **Does not introduce auth** — local network only, status quo unchanged.
- **Does not delete data based on sync state** — even after acking, files
  stay until the count/day caps roll them off. This is the safety gate
  before B.3.

## Cross-references

- Design rationale: `data-extraction-redesign-20260503.md`
- Memory budget post-Phase A: `cry-detect-01-memory-budget-20260503.md`
- File-api today: `projects/cry-detect-01/main/file_api.c`
- Metrics logger today: `projects/cry-detect-01/main/metrics_logger.c:101`
- Web UI handler registration: `projects/cry-detect-01/main/web_ui.c:317`