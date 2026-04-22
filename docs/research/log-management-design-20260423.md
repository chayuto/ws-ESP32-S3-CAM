# Log-management design: end-to-end for cry-detect-01 and beyond

**Status:** draft design, no code changes
**Written:** 2026-04-23
**Motivated by:** pain points accumulated across 3 overnight sessions
  (20260420, 20260421, 20260422), see §1
**Supersedes (on adoption):** current extract_session.sh + ad-hoc session dirs

## 0. Goals and non-goals

### Goals

1. **Session-scoped data.** Each night's data is self-contained — tomorrow's
   analysis script should NOT need to re-filter by timestamp just to find
   "tonight's WAVs". One session dir = one night's capture set plus its
   context.
2. **No duplication.** Host should pull each byte of device-side data at most
   once. Extracts should be fast (≤ 2 min) after the first night, not 40 min
   of pulling the same 280 MB each time.
3. **Self-describing on disk.** Future-us opening a random file should know
   what it is, what version, what columns, what build produced it.
   Reverse-engineering from firmware source is a smell.
4. **Build provenance on every event.** Every capture carries the
   `build_sha` that produced it. Analysis scripts can then correctly join
   cross-build data.
5. **Reset diagnostics.** Every boot carries a reset reason. Tonight's
   two mystery reboots should have been labeled with cause.
6. **Retention without loss.** On device: old logs purge predictably so
   SD doesn't fill over months. On host: WAVs never purge (they're training
   data) but heavy logs archive out of hot paths.
7. **Multi-device ready.** Design works without change when we add a second
   device (same household, different room) or share an architecture with
   a different baby-monitor project.
8. **Dataset publishability.** The archive must be slicable into named,
   frozen dataset releases that a future training run can reference
   immutably.

### Non-goals (this design doesn't solve)

- A unified observability stack (we're NOT moving to Prometheus / OTel /
  some time-series DB). Files + JSONL + CSV are sufficient for our scale
  and much easier to replay offline.
- A custom database. Plain files on SD + host disk stay the canonical
  store. Indexing happens at analysis time from CSV / JSONL.
- Real-time streaming to cloud. The device stays offline-capable, logs
  locally, host pulls periodically. This is not changing.
- Hard-real-time alert delivery. This design is about data collection
  and training. Alerting is a separate track.

## 1. Pain points (evidence base)

Observed in sessions 20260420..20260422:

1. **Extract duplication.** Tonight's extract re-downloaded
   `infer-20260418.jsonl` (53.8 MB) for the 4th time — same bytes, same
   content. Cumulative: 3 sessions × ~280 MB device logs = 840 MB of
   identical duplicates across session dirs. Every session the figure grows.
2. **Slow extract.** With ~280 MB pulled over Wi-Fi at ~220 KB/s, extracts
   take 20–45 min, dominated by log re-downloads. This conflicts with the
   "fast morning audit" goal.
3. **No session boundary in events/.** Tonight's ~22 WAVs sit alongside
   04-18..04-21 WAVs (179 total) in a single `/sdcard/events/` dir. Every
   analysis script filters by timestamp to extract "tonight". The dir is
   a growing grab-bag.
4. **No build_sha per capture.** `triggers.jsonl` records timestamp, note,
   rms, cry_conf — but not build_sha. Tonight's captures are under
   build `4af50c57`; prior are under `a4870a21` / `a895bfdc`. The
   `cry_conf` field means different things for each (pre-fix double-sigmoid
   vs post-fix probability). Joining across builds currently requires
   cross-referencing CRY-0000.LOG by timestamp — brittle.
5. **Reset reason not exposed.** `/metrics` returns `reset_reason: None`.
   CRY-0000.LOG has boot records but no cause. Tonight's 19:30 / 19:56
   reboots are "suspected physical disturbance" with no way to confirm.
6. **No log schema in files.** `cry-YYYYMMDD.log` has 35 CSV columns
   and no header row — schema lives in `main/sd_logger.c`. When we
   audit old logs, we reverse-engineer column positions from source. If
   the firmware adds/removes a column, every old analysis silently misreads.
7. **No retention policy.** `CRY_REC_KEEP_FILES=5000` caps recording WAVs
   at ~8 GB but `infer-YYYYMMDD.jsonl` and `cry-YYYYMMDD.log` grow
   indefinitely. At ~100 MB/day, 180 nights → 18 GB of logs before we
   notice.
8. **Analysis scripts re-invent boundaries.** Every session's analyze
   script opens with "filter to tonight's window [START, END]". Should
   be read-from-dir-structure, not re-derive per script.
9. **Cross-session scale shifts.** `cry_conf` in build ≤ a4870a21 is
   sigmoid(dequant) ∈ [0.500, 0.7303]. In build 4af50c57 it's dequant
   directly ∈ [0, 0.996]. Same field name, different semantics. No
   versioning discipline caught this.
10. **trigger_note matcher bug.** Analyze script keyed WAV ts to
    triggers.jsonl ts at ms precision — WAV filenames are second-resolution.
    Every row had empty trigger_note until manually fixed.

## 2. Design principles

1. **One session = one directory.** The session is the unit of data. Moving,
   archiving, deleting, sharing, labeling all happen at session granularity.
2. **Session dir is immutable once closed.** No appending to yesterday's
   session dir. This makes archiving, checksums, and dataset releases safe.
3. **Append-only logs rotate at session boundary,** not calendar day.
   A session spans a continuous collection window (typically one night).
   Multiple sessions in the same calendar day are distinct dirs.
4. **Self-describing files.** Every JSONL has a `_schema` header as its
   first line. Every CSV has a commented header block. Versions bump on
   schema change; old readers warn, new readers auto-detect.
5. **Idempotent extract.** Running extract twice = same disk state as
   running it once. Incremental (byte-offset resume) for append-only files,
   checksum-compare for immutable files.
6. **Build provenance on everything.** Every record that represents a
   captured event or heartbeat carries `build_sha`. Makes cross-build
   joins trivial.
7. **Host archive is canonical.** Device SD is ephemeral working storage
   (can be wiped safely). Anything the host hasn't ingested is at risk.
   Host archive is under git-LFS or separate rsync'd backup.
8. **Separate raw data from derived.** WAVs + jsonl = raw, never regenerate.
   manifest.csv + specgrams = derived, cheap to regenerate from raw, go in
   a separate subdir with a version stamp.
9. **Dataset releases are named snapshots.** "Model training run v1 used
   dataset cry-v0.3, which is WAVs X..Y labeled on 2026-05-01." The release
   file pins exact WAV list + label state at that moment.

## 3. Conceptual model

```
Device (1)
  ↓ emits
Session (N per device, one per collection window)
  ↓ contains
Capture (N per session, one per WAV recorded)
  + Heartbeat stream (1 per session, continuous)
  + Inference stream (1 per session, per-frame)
  + Boot record (1 per device-reboot within session; if reboot happened, 2+)
  ↓ joined and audited via
YAMNet ground-truth scores (per-capture, per-segment)
  ↓ human-reviewed into
Labels ledger (all captures ever, evolving annotations)
  ↓ sliced into
Dataset release (frozen snapshot for training)
```

Four data objects, clean ownership.

### Session definition

A session is:
- Started by a **session-begin event** — a well-defined moment when the
  host declares "I'm collecting from this device now".
- Ended by a **session-end event** — host declares "I'm done".
- Device-reboots within a session stay in the same session (not start a
  new one). The session dir spans all uptime windows between begin and end.

Session IDs are `<device_id>-<iso_local_start>` for human readability,
e.g. `cry-detect-01-2026-04-22T19-30`. The colon is replaced by dash so
the directory name is safe on all filesystems.

### Session boundaries in practice

- **Typical case:** host calls `/session/begin` at ~18:00, device creates
  `/sdcard/sessions/<session_id>/` and starts writing all event/log
  files there. At ~08:00 morning, host calls `/session/end`, device
  finalizes the session dir (writes `meta.json` with end_ts, flushes all
  streams). Host then runs extract, pulling the whole closed session dir
  in one shot.
- **Reboot during session:** on reboot, device resumes writing to the
  same session dir (session_id persisted in NVS or derived from
  `/sdcard/sessions/_current_session`). A new boot record is appended;
  data streams get a new rotation suffix (`infer-2.jsonl`, `infer-3.jsonl`)
  but stay in same dir.
- **No host contact:** device auto-closes the previous session on its
  first boot of a new calendar day if host never called `/session/end`.
  Prevents orphaned "open" sessions after a host failure.

## 4. Target on-device file layout

```
/sdcard/
  sessions/
    <session_id>/                          # e.g. cry-detect-01-2026-04-22T19-30
      meta.json                            # session-level metadata (see §6.1)
      events/
        cry-20260422T183423+1000.wav       # same filename convention as today
        cry-20260422T190156+1000.wav
      triggers.jsonl                       # per-capture trigger metadata
      heartbeat-1.jsonl                    # 30s snapshots, rotation 1 (pre-first-reboot)
      heartbeat-2.jsonl                    # rotation 2 after a reboot
      infer-1.jsonl                        # per-frame inference, rotation 1
      infer-2.jsonl                        # rotation 2
      boots.jsonl                          # every boot: ts, uptime, build_sha, reset_reason
    _current_session -> cry-detect-01-...  # symlink to active session dir

  archive/                                  # (optional) compressed closed sessions
    cry-detect-01-2026-04-21T...tar.zst     # device-side space recovery

  lost_and_found/                           # orphaned files (e.g. WAV written
    events/                                 # before any session began)

  system.log                                # rare, important: bootloader faults,
                                            # SD errors. Rotating, tiny.
```

### Key changes vs current

| aspect | current | target |
|---|---|---|
| events dir | `/sdcard/events/` shared across all time | `/sdcard/sessions/<id>/events/` per session |
| inference log | `/sdcard/infer-YYYYMMDD.jsonl` (calendar-day) | `/sdcard/sessions/<id>/infer-<rot>.jsonl` (session-day) |
| heartbeat log | `/sdcard/cry-YYYYMMDD.log` | `/sdcard/sessions/<id>/heartbeat-<rot>.jsonl` (JSONL, self-describing) |
| boot log | `/sdcard/CRY-0000.LOG` (text, ad-hoc format) | `/sdcard/sessions/<id>/boots.jsonl` (JSONL, carries reset_reason) |
| cross-boot identity | calendar day | session_id + rotation index |

### Why JSONL everywhere?

The current `cry-YYYYMMDD.log` is CSV without a header. Column drift is a
silent failure mode. JSONL:
- Self-describing per-line
- New fields are additive (old readers ignore unknown keys)
- Easy to parse in any language
- Schema embedded in `_schema` opener line
- Cost: ~2× bytes vs CSV. Acceptable at our volumes (we have 16 GB SD free).

## 5. Target host archive layout

```
ws-ESP32-S3-CAM/
  datasets/                                # NEW — top-level, separate from code
    cry-detect-01/                         # per-device
      sessions/
        cry-detect-01-2026-04-22T19-30/    # per-session, same id as on device
          meta.json                        # copied from device
          events/                          # WAVs
          triggers.jsonl
          heartbeat.jsonl                  # concatenated from heartbeat-*.jsonl
          infer.jsonl                      # concatenated from infer-*.jsonl
          boots.jsonl
          _raw_ok                          # marker: raw data extracted + verified
          derived/                         # generated artifacts, never canonical
            audit-v1/                      # named audit pass, versioned
              manifest.csv
              segments.csv
              yamnet_files.csv
              yamnet_segments.csv
              specgrams/
            audit-v2/                      # re-audit with improved pipeline
              ...
          labels/                          # per-session human/auto labels
            auto-yamnet.csv                # from audit
            human.csv                      # from annotation tool
            merged.csv                     # combined view
          README.md                        # human notes (env, incidents, etc.)

      archive/                             # old sessions, compressed
        cry-detect-01-2026-01-15T19-30.tar.zst

      labels/                              # cross-session
        master.csv                         # one row per capture, ever
        label-history.jsonl                # append-only audit trail of label changes

      releases/                            # dataset releases (frozen snapshots)
        cry-v0.1.json                      # pointer to WAVs + label state for training
        cry-v0.2.json
      ...
```

### Critical property: raw vs derived

Under each session dir, `events/ + *.jsonl + meta.json + boots.jsonl`
is **raw**, read-only after `_raw_ok` marker lands, never regenerated.
Everything under `derived/` is **cheap to recompute** from raw +
tooling — re-running audit_pipeline on raw produces `audit-v2/` without
touching raw data.

This matters because:
- Audit pipeline bugs don't corrupt ground truth
- We can run N different audit passes for comparison
- Raw can be checksummed + archived safely
- Dataset releases can reference specific audit versions

### Labels are separate from data

`derived/audit-v1/yamnet_files.csv` is produced by a tool.
`labels/human.csv` is produced by humans.
`labels/master.csv` joins across sessions.

When we re-label something, we append to `label-history.jsonl` rather than
mutating. Reversible and auditable.

## 6. Schema specifications

### 6.1 `meta.json` (per session)

```json
{
  "_schema": {"version": "v1", "type": "session_meta"},
  "session_id": "cry-detect-01-2026-04-22T19-30",
  "device_id": "cry-detect-01",
  "device_hw_rev": "ESP32-S3-CAM-GC2145",
  "start_ts": "2026-04-22T19:30:51+1000",
  "end_ts": "2026-04-23T07:17:35+1000",
  "environment": {
    "room": "bedroom",
    "placement": "~1m from cot, facing cot",
    "outlet_id": "bedroom-east-wall",
    "ambient": {"fan": "off", "ac": "off", "window": "closed"}
  },
  "subject": {
    "baby_age_months": 8,
    "caregiver": "redacted"
  },
  "build": {
    "build_sha": "4af50c57",
    "git_commit": "a32afe2",
    "firmware_notes": "double-sigmoid fix; cry_conf in [0, 0.996]"
  },
  "boot_count_in_session": 3,
  "extract_ok": true,
  "extract_ts": "2026-04-23T08:10:00+1000"
}
```

Written by the device at session-begin (partial), updated at session-end
(completed). Pulled by host unchanged.

### 6.2 `triggers.jsonl`

Current:
```json
{"ts":"2026-04-22T18:53:32.050","note":"auto-rms-33x-rms1632","rms":1632.0,"cry_conf":0.429,"state":3}
```

Target (schema v2):
```json
{"_schema":{"version":"v2","type":"triggers","fields":{
  "ts":"iso8601-local","note":"string","rms":"float",
  "cry_conf":"float-in-0-to-1","cry_conf_scale":"enum",
  "state":"int","build_sha":"hex8","session_id":"string"}}}
{"ts":"2026-04-22T18:53:32.050+1000","note":"auto-rms-33x-rms1632",
 "rms":1632.0,"cry_conf":0.429,"cry_conf_scale":"probability",
 "state":3,"build_sha":"4af50c57","session_id":"cry-detect-01-2026-04-22T19-30"}
```

Added fields:
- `cry_conf_scale`: `"probability"` (new firmware) or `"sigmoid_squashed"`
  (old firmware). Analysis can then automatically un-transform old data
  for comparability. This is the single smallest fix that addresses pain
  point #9.
- `build_sha`: pain point #4.
- `session_id`: redundant with file path but explicit for safety in case
  files get moved.
- Full timezone in `ts` (currently missing).

### 6.3 `heartbeat.jsonl` (replaces `cry-YYYYMMDD.log`)

```json
{"_schema":{"version":"v1","type":"heartbeat"}}
{"ts":"2026-04-22T19:30:51+1000","uptime_s":37,"event":"boot",
 "build_sha":"4af50c57","reset_reason":"POWERON"}
{"ts":"2026-04-22T19:31:21+1000","uptime_s":67,"event":"snapshot",
 "cry_conf":0.000,"max_cry_1s":0.000,"rms":40.3,"nf_p95":53.5,
 "nf_warm":true,"latency_ms":648,"inference_count":150,"inference_fps":1.48,
 "free_heap":2284152,"free_psram":2268824,"rssi":-55,"state":"idle",
 "watched":{"cry_baby":0.0,"cry_adult":0.0,"whimper":0.001, ... },
 "build_sha":"4af50c57"}
```

vs current compressed CSV (~35 columns, no header, column meanings lost).
JSONL at 30 s cadence is ~400 B/row, 2880 rows/night = 1.2 MB — still
small. Human-readable on disk; `jq` processable.

`watched` becomes a dict instead of 20 positional float columns — adding
a new class is a non-breaking change.

### 6.4 `infer.jsonl`

Already JSONL today. Target v2 adds build_sha + session_id per row
(redundant within a single rotation, but de-duplicable when files merge
across rotations). Schema line on first line.

### 6.5 `boots.jsonl` (replaces `CRY-0000.LOG`)

```json
{"_schema":{"version":"v1","type":"boots"}}
{"seq":42,"boot_ts":"2026-04-22T19:30:51+1000","build_sha":"4af50c57",
 "reset_reason":"POWERON","prev_uptime_s":15791,"free_heap_at_boot":2284152,
 "wifi_up_ts":"2026-04-22T19:30:54+1000","ntp_sync_ts":"2026-04-22T19:30:56+1000"}
```

Fields that the current CRY-0000.LOG lacks:
- `reset_reason`: wired from `esp_reset_reason()`. Enum:
  `POWERON|SW|PANIC|INT_WDT|TASK_WDT|BROWNOUT|SDIO|USB|EXT|DEEPSLEEP|UNKNOWN`.
- `prev_uptime_s`: how long the previous boot lasted. Diagnostic for
  "did we crash quickly?" vs "stable for hours then reboot".
- `free_heap_at_boot`: early-boot memory sanity check.
- Timings for wifi_up / ntp_sync so we can notice regressions in
  connect-time.

### 6.6 `labels/master.csv` (per-device, cross-session)

```csv
session_id,capture_file,ts_local,env,yam_cry,yam_speech,yam_scream,
  human_label,human_notes,human_annotator,audit_version,audit_ts,
  build_sha_capture,build_sha_audit,hnr_db,f0_mean_hz,...
cry-detect-01-2026-04-22T19-30,cry-20260422T185332+1000.wav,2026-04-22 18:53:32,bedroom,
  0.429,0.010,0.002,cry,"bedtime peak cry",chayut,audit-v1,2026-04-23T08:20,
  4af50c57,audit-v1,9.4,521,...
```

One row per capture ever. Columns evolve (additive). Dataset releases
pin which columns and which rows by session_id filter.

### 6.7 Schema evolution policy

- **Breaking change** (rename, remove, type-change) → bump major version,
  keep old readers working via explicit version check. Log migration notes
  in `docs/research/`.
- **Additive change** (new field) → no version bump needed. Old readers
  ignore.
- Every schema doc lives at `docs/schemas/<type>-v<N>.md` — the canonical
  reference, linked from firmware + host tooling.

## 7. Data flow

### Nightly ingest (target)

```
19:30  host: POST /session/begin device={cry-detect-01}
         device: mkdir sessions/<id>/, write meta.json {start_ts, build},
                 set _current_session symlink
19:30→07:17  device: writes to sessions/<id>/ continuously
           : reboots land in same dir, new rotations
07:17  host: POST /session/end
         device: finalizes meta.json {end_ts, boot_count}, fsync all
07:20  host: extract_session.sh <id>
         → pull sessions/<id>/ wholesale (one GET per file, no dup)
         → mirror to datasets/cry-detect-01/sessions/<id>/
         → write _raw_ok marker when checksums verify
07:25  host: audit_pipeline.sh <id>
         → writes to sessions/<id>/derived/audit-v1/
07:30  host: update-master-labels.py
         → joins auto-yamnet into master.csv
         → no human labels yet — that's for later
```

### Incremental extract property

Because the session dir is a self-contained unit and only the "current"
session is mutable, extract for a CLOSED session is:
- `curl /files/ls?path=/sdcard/sessions/<id>/ --recurse`
- For each file not already at host: GET it. Skip if size matches and
  sha matches. Small WAV/triggers/boot files verify via SHA-256 in
  meta.json.
- Total transfer: just tonight's ~25 MB (WAVs + small logs). No re-pull
  of historical days.

### Extract for the CURRENT (open) session

- Same as above, EXCEPT append-only files (`infer-*.jsonl`,
  `heartbeat-*.jsonl`) support **byte-offset range GET**.
- Host remembers `last_extracted_bytes` per file; next extract fetches
  only the new tail.
- Supports mid-session check-ins without re-pulling gigs.

### Why session-begin / session-end instead of auto?

- Host controls precisely when data is "our study night" vs "ambient".
  Walking the device past the cot to charge at 15:00 isn't a session.
- Session dir can't be half-written if host never calls end — there's an
  explicit "close" step, so `_raw_ok` is meaningful.
- Fallback: if host never calls end, device auto-closes on its first
  boot-after-noon-next-day. Prevents indefinitely-open sessions.

## 8. Retention

### On device

- **Current active session:** never auto-deleted. Anything could be
  important.
- **Closed sessions (not yet extracted):** retained indefinitely until
  disk hits 80% full. Then oldest-closed-first ring delete.
- **Closed sessions (_raw_ok confirmed by host):** device can tar+zstd
  to `/sdcard/archive/` (compresses ~5×) and eventually delete. Host
  confirms via `/session/<id>/archive-ok` HTTP call.
- **`/sdcard/lost_and_found/`:** wiped weekly by firmware. Not canonical
  data.

Target: device never holds more than ~1 GB. Assuming 50 MB/session, that's
20 closed sessions — a full month if one session/night.

### On host

- **Raw session dirs:** kept forever. Training data.
- **Derived dirs (audit-vN/):** kept for current + previous version;
  older auto-deleted (always reproducible from raw).
- **Labels:** never deleted; append-only history.
- **Dataset release files:** never deleted (reproducibility anchor).
- **Archive dir:** closed sessions older than 90 days compress to `.tar.zst`
  in `archive/`, WAVs stay accessible as-needed. Saves ~70% of disk.

## 9. Migration from current state

Principle: **no data loss during migration**. Current disk state is
precious (3 nights of labeled-via-YAMNet WAVs).

### Phase 0 — design adoption (today's task)

Commit this doc. Decisions below are open questions to close before
Phase 1.

### Phase 1 — backwards-compatible host layout

1. Create `datasets/cry-detect-01/sessions/` structure. Populate with
   existing `projects/cry-detect-01/logs/night-*` contents via a one-shot
   migration script:
   - Each `night-YYYYMMDD/` → `cry-detect-01-YYYY-MM-DDTXX-XX/`
     (best-guess start-time from earliest WAV ts in session window)
   - Separate WAVs into `events/` dir (currently flat in `wavs/`)
   - Populate `meta.json` from existing README.md field parsing +
     defaults
   - Move derived artifacts (manifest, specgrams, yamnet_*.csv) under
     `derived/audit-v1/`
   - Concatenate `device-logs/cry-*.log` → `heartbeat.jsonl` (convert
     CSV → JSONL on the fly using documented column positions)
   - Write `_raw_ok` marker
2. Symlink current `logs/night-YYYYMMDD` to new location for transition
   period; remove symlink after analysis tools updated.
3. Update `analyze_night_*.py` to read from new layout. Old session
   dirs work because the host-side converter reproduced all fields.

### Phase 2 — session boundaries

1. Firmware: add `/session/begin`, `/session/end`, `/session/status`
   HTTP handlers. Session ID generated device-side as
   `<device_id>-<iso_start>`. Store current session ID in NVS so it
   survives reboots.
2. Firmware: when a session is active, write all events/logs under
   `/sdcard/sessions/<id>/`. When no session, writes go to
   `/sdcard/lost_and_found/` (preserved, but out of the way).
3. Host: update `cry_monitor.sh` to call `/session/begin` + `/session/end`
   with the night-20260XXX directory name.
4. Rollback: if `/session/*` endpoints absent, firmware falls back to
   current flat-dir behavior. Host tooling detects and uses the old
   path.

### Phase 3 — schema upgrades

1. Firmware: add `build_sha`, `session_id`, `cry_conf_scale` fields to
   every record in `triggers.jsonl`, `infer.jsonl`, `heartbeat.jsonl`.
2. Firmware: switch `heartbeat.jsonl` from CSV (`cry-YYYYMMDD.log`) to
   JSONL with `_schema` header.
3. Firmware: add `boots.jsonl` with `reset_reason` wired from
   `esp_reset_reason()`. Keep CRY-0000.LOG as a compatibility stream
   until next major migration.
4. Host: analyze scripts read schema version, auto-upgrade-map old
   records to new canonical form.

### Phase 4 — retention automation

1. Host: `weekly-purge.sh` — after `_raw_ok` marker present, call
   `/files/rm` on device to delete the session dir. Skipped for sessions
   younger than 7 days or without `_raw_ok`.
2. Firmware: nightly self-purge of `/sdcard/lost_and_found/` older
   than 30 days.
3. Host: monthly `archive-old-sessions.sh` — tar+zstd sessions older
   than 90 days into `archive/`, move WAVs into flat retrievable cache
   if desired.

### Phase 5 — multi-device readiness (future)

When adding device 2:
- Add `device_id` to `cry_monitor.sh` arguments.
- Session IDs include device_id by construction — naturally separated.
- `datasets/<device_id>/` branches at top level — no schema changes.
- Master labels can union across devices trivially.

No firmware change. This design already anticipates it.

## 10. Future expansion hooks

These are **not** in scope now but the design reserves room:

1. **Video + audio fusion.** The board has a GC2145 camera. Add
   `events/<ts>.jpg` beside `.wav`; update `triggers.jsonl` schema with
   an `image_path` field. Session dir absorbs without change.
2. **Per-baby / per-household multi-tenant.** Add `household_id` +
   `subject_id` at top of session meta.json. Master labels filter by
   subject_id. Dataset releases can include only one subject.
3. **Edge ML inference logs (embeddings).** When we distill a per-baby
   model, we'll want to save the 1024-d YAMNet embeddings per frame for
   training. Add `embeddings.npz` beside `infer.jsonl`, same session
   boundary.
4. **Real-time streaming / webhooks.** Firmware `/events/subscribe` SSE
   endpoint could push triggers.jsonl entries live. No data layout
   change; just a push counterpart to the pull model.
5. **Replay tool.** `replay-session.py <id>` re-plays captured WAVs
   through a new firmware model version and writes derived/audit-vN/.
   Works because raw is read-only.
6. **Label propagation.** When human re-labels an incident, derived
   datasets auto-invalidate if they sourced that capture. Dataset
   release manifests must not allow mutating labels to retroactively
   "update" a release — frozen means frozen.
7. **Dataset publication.** `datasets/cry-detect-01/releases/cry-v1.json`
   can be packaged into a public dataset (WAVs + labels + YAMNet scores,
   stripped of PII like caregiver names in meta.json). The `public_ok`
   flag per session controls.

## 11. Open questions (decide before Phase 1)

1. **Location of `datasets/` in the repo.** Option A: top of
   `ws-ESP32-S3-CAM/` (sibling to `projects/`). Option B: inside
   `projects/cry-detect-01/` (project-local). Leaning A because
   datasets outlive projects.
2. **Git-LFS or separate rsync?** At 25 MB/session, a year = ~9 GB of
   WAVs. git-LFS makes sharing easy but locks us to a specific provider.
   rsync to external SSD + NAS is cheaper but unversioned. Hybrid: LFS
   for labels/metadata, rsync for raw WAVs.
3. **meta.json PII.** Caregiver names, baby age, household — sensitive.
   Proposal: `meta.json` has a `_public_fields` list; tooling redacts
   non-public fields when preparing a dataset release.
4. **SHA-256 for integrity.** Worth the compute on the ESP32? Probably
   not per-WAV (can't spare the cycles during capture). Host-side hash
   on ingest is free and sufficient.
5. **Session-begin authentication.** Currently device is LAN-open. If
   multi-host ever needs to collaborate, we'd want `/session/*` to be
   token-guarded. Out of scope now but the HTTP surface should leave
   room.
6. **Archive format.** `tar.zst` by default? Evaluate vs zip (portability)
   vs individual file compression (random access).
7. **Time zone in filenames.** Session ID `...2026-04-22T19-30` is
   timezone-less — implicit local. Should it be `T19-30+1000` or UTC?
   Leaning local+tz-suffix (`T19-30+1000`) — matches log contents and
   human mental model.
8. **Close-session on reboot-during-session.** If device reboots 3 times
   in a session, should that auto-close the session as unstable? Probably
   keep open (the data is still useful) but add a field
   `unhealthy_reboots` to meta.json for flagging in downstream analysis.
9. **Historical back-fill of build_sha.** For existing sessions
   (20260420..20260422), we can reconstruct per-capture build_sha by
   cross-referencing CRY-0000.LOG boots against capture timestamps.
   Worth doing once during Phase 1 migration.

## 12. Success criteria

Phase 1 complete when:
- All 3 existing sessions migrated to new layout under `datasets/`.
- Existing analyze scripts work on new layout.
- First new-layout session (night-20260424 or later) runs end-to-end
  without host-side per-script filtering.

Phase 2 complete when:
- Firmware supports `/session/{begin,end,status}`.
- Extract for a closed session under 2 min (vs current 40 min).
- Device SD usage capped at ~1 GB sustained.

Phase 3 complete when:
- `cry_conf_scale` field in every trigger record.
- Cross-session analysis scripts can auto-normalize old double-sigmoid
  records to new-scale for comparable plots.
- Reset reason logged for every boot; tonight-20260422 style mysteries
  have ground truth.

Phase 4 complete when:
- No human-in-the-loop for retention. Nightly housekeeping is automatic.

Phase 5 complete when:
- Adding device 2 to collection program requires zero firmware/tooling
  changes; just host config.

## 13. Cost estimate (rough)

| phase | firmware effort | host effort | risk |
|---|---|---|---|
| 1 migration | — | 1 day (one-shot script) | Low (reversible) |
| 2 session boundaries | 1–2 days (HTTP handlers + NVS) | 0.5 day | Med (firmware touches data path) |
| 3 schema upgrades | 1 day (JSONL output + build_sha wiring + reset_reason) | 0.5 day (schema-aware readers) | Med (old-data compatibility) |
| 4 retention automation | — | 1 day | Low |
| 5 multi-device | — | 0.5 day (ops only) | Low |

Total: ~3 dev days of firmware + 3.5 dev days of host tooling, plus the
one-off migration. Could be phased one phase per week across the data-
collection program.

## 14. Non-migration path (do nothing)

If we don't adopt this:
- Extract grows to ~60 min per session at 6 months (assuming ~60 sessions
  × ~280 MB = 17 GB of device logs to re-pull each time).
- Duplicate host storage hits ~100 GB if left unmanaged.
- Analysis scripts accumulate more band-aid filters.
- Adding a second device forces a larger refactor.
- Any future schema change (which is inevitable) creates silent-read
  bugs in old analysis.

The status-quo cost is real and increasing.

---

**Next action:** close the open questions in §11 with the user, then
kick off Phase 1 migration as a separate task.
