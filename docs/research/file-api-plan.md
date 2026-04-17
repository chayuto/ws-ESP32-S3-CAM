# File API — agentic-first remote access to SD / flash

*2026-04-17. Stage-2 Milestone 2.7. Answers: "how do I pull the SD log and the event recordings without physically removing the SD card?"*

Current state: Stage 1 exposes only `GET /recordings/<file>.wav` + `GET /events/list`. Fine for a parent clicking "▶ play". Not enough for **agentic inspection** — when an LLM (or a CI job, or me at 3 am) needs to walk the filesystem, read the daily log, download the last cry recording, or clean up old files, we need a **general, safe, well-scoped file API**.

Design rule: **every operation that today requires physical SD removal must have an HTTP equivalent.**

---

## 1. Scope & safety

### In scope

- Read-only listing of directories on SD and the fallback FAT partition.
- Read any file within the whitelisted roots.
- Stat (size, mtime, optional SHA-256).
- Append-only-safe tail read (no race with the logger's `fwrite`).
- **Delete** a file within the whitelisted roots (space management).
- **Recording trigger**: manual or multi-class trigger, writing WAV via existing `event_recorder`.

### Out of scope (defer unless needed)

- **Write** (upload) arbitrary files — risky; not needed for Stage-2.
- **Move / rename** — achievable via delete + re-log; skip.
- **Symlinks / special files** — none on FATFS/SPIFFS anyway.
- **TLS / auth** — Stage 3 lift. This is a LAN device; trust boundary = Wi-Fi network.

### Whitelisted roots

Exactly these, hard-coded in `web_ui.c`:

| Path prefix | Read | Delete | Notes |
|---|---|---|---|
| `/sdcard` | ✅ | ✅ | SD card when present. |
| `/logs` | ✅ | ✅ | `logs_fat` 1 MB fallback partition. |
| `/yamnet` | ✅ | ❌ | SPIFFS, read-only (would brick model). |

Any path that:
- Does not start with one of the above.
- Contains `..` or a `%2e%2e` URL-encoded traversal.
- Contains `\\0` or other control bytes.

…returns `400 Bad Request`. No exceptions.

---

## 2. Endpoints

### 2.1 `GET /files/ls?path=/sdcard`

Directory listing. JSON response:

```json
{
  "path": "/sdcard",
  "entries": [
    {"name":"cry-20260417.log",        "type":"file", "size":152304, "mtime":1766250123},
    {"name":"classes-20260417.log",    "type":"file", "size":8234112, "mtime":1766250125},
    {"name":"events",                  "type":"dir",  "size":0,       "mtime":1766245000}
  ]
}
```

- `type`: `"file"` or `"dir"`.
- `size`: bytes (0 for directories).
- `mtime`: Unix epoch seconds.
- No pagination yet (FAT32 dirs are small; add if > 500 entries).

### 2.2 `GET /files/get?path=/sdcard/cry-20260417.log[&range=start-end]`

Stream a file's contents. Content-Type inferred from extension (`.log` → `text/plain`, `.wav` → `audio/wav`, `.csv` → `text/csv`, default `application/octet-stream`).

Optional HTTP-style byte range for partial downloads: `?range=0-65535` → first 64 KB. Implemented as chunked HTTP, no seek required for short ranges.

### 2.3 `GET /files/head?path=...&bytes=4096`
### 2.4 `GET /files/tail?path=...&bytes=4096`

First / last N bytes. `tail` is **race-safe** against the logger: opens file, `fseek(SEEK_END)`, reads backward. Aligns to whole lines if `?align=lines` is set. Default `bytes=4096`, max 1 MB.

### 2.5 `GET /files/stat?path=...`

```json
{"path":"/sdcard/cry-20260417.log","size":152304,"mtime":1766250123,"sha256":"optional-hash"}
```

SHA-256 is optional (`?hash=1`), adds ~500 ms for a 10 MB file on this CPU. Useful for cross-checking against a local copy.

### 2.6 `DELETE /files/rm?path=...`

Deletes a single file. Returns `200 OK` with `{"deleted":"/path"}` or `404`/`400`. No directory rm (prevents accidents).

### 2.7 `GET /files/df`

Disk-free per mounted filesystem. Lets an agent decide when to rotate.

```json
{
  "sdcard":   {"total_bytes": 32000000000, "free_bytes": 31500000000},
  "logs":     {"total_bytes": 1048576,     "free_bytes":   1040000},
  "yamnet":   {"total_bytes": 5242880,     "free_bytes":   1190208}
}
```

### 2.8 `POST /rec/trigger[?class=20]`

Trigger an event recording manually, optionally for a *specific* class (re-using the event-recorder pre-roll + post-roll). Returns the filename.

### 2.9 `POST /rec/trigger-on-class`

Config endpoint: tell the detector to also record when classes `[393, 394, 390]` (smoke/fire/siren) fire. Currently detector only triggers on class 20.

---

## 3. Schema for "record everything non-cry too"

Extend the existing `event_recorder` module with a **trigger source** per recording:

| Existing | Extension |
|---|---|
| Files `cry-YYYYMMDDTHHMMSSZ.wav` | Add source-tag: `cry-…`, `smoke-…`, `siren-…`, `manual-…`, `all-…` |
| `event_recorder_trigger(float conf)` | `event_recorder_trigger_for(const char *source, int class_idx, float conf)` |
| `/recordings/` list | Extend `/files/ls?path=/sdcard/events` — one entry per WAV, filename self-describes |

Lets the Telegram / SSE layer include *which* class caused the recording in its message payload: "Smoke alarm detected at 23:47, listen: /recordings/smoke-20260417T234710Z.wav".

---

## 4. Implementation sketch

### 4.1 New module `file_api.c/h`

```c
/* Path validation */
bool file_api_path_is_safe(const char *path);

/* Directory listing, streaming JSON chunks */
esp_err_t file_api_list_handler(httpd_req_t *req);

/* File stream download with extension sniffing */
esp_err_t file_api_get_handler(httpd_req_t *req);

/* Head, tail, stat, delete, df */
esp_err_t file_api_head_handler(httpd_req_t *req);
esp_err_t file_api_tail_handler(httpd_req_t *req);
esp_err_t file_api_stat_handler(httpd_req_t *req);
esp_err_t file_api_rm_handler(httpd_req_t *req);
esp_err_t file_api_df_handler(httpd_req_t *req);
```

### 4.2 Path validation (security-critical, 20 lines)

```c
static bool path_is_safe(const char *p) {
    static const char *ALLOWED_ROOTS[] = { "/sdcard/", "/logs/", "/yamnet/", NULL };
    if (!p || p[0] != '/') return false;
    if (strstr(p, "..") || strchr(p, '\\')) return false;
    for (int i = 0; ALLOWED_ROOTS[i]; i++) {
        if (strncmp(p, ALLOWED_ROOTS[i], strlen(ALLOWED_ROOTS[i])) == 0) return true;
        /* Allow exact prefix without trailing slash too — for ls of root */
        if (strcmp(p, ALLOWED_ROOTS[i] - 1) == 0) return true;  /* wrong indexing; illustrative */
    }
    return false;
}
```

### 4.3 Delete whitelist (extra safety)

Even within the whitelisted roots, forbid deleting the *current* open log file (which `sd_logger` is writing to). Check against `sd_logger_current_path()` (new getter).

### 4.4 Stack pressure

All handlers avoid the F15-style "8 KB on stack" bug: allocate big buffers with `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. Lesson carried forward from `web_ui.c:handler_log_tail`.

### 4.5 Concurrency

`httpd_config_t` has `max_open_sockets=7` by default. At most 2 of those are SSE + 1 audio stream. A heavy file download does not starve the SSE clients because esp_http_server uses per-connection scheduling. Verify with a parallel curl of `/files/get?path=/sdcard/classes-*.log` while `/events` is open.

Cap file-download concurrency to 2 via a semaphore — keeps heap predictable and prevents one streamer from blocking metrics.

---

## 5. Agentic usage — the point of this

A future session (or another tool) can run:

```bash
# Enumerate what's on the card
curl -s http://cry-detect-01.local/files/df
curl -s http://cry-detect-01.local/files/ls?path=/sdcard

# Grab today's classifications for analysis
curl -s http://cry-detect-01.local/files/get?path=/sdcard/classes-20260417.log \
  > /tmp/classes-20260417.log

# Spot-check the last 200 lines of the event log
curl -s "http://cry-detect-01.local/files/tail?path=/sdcard/cry-20260417.log&bytes=16384"

# Delete yesterday's logs to free space
curl -X DELETE "http://cry-detect-01.local/files/rm?path=/sdcard/cry-20260416.log"
curl -X DELETE "http://cry-detect-01.local/files/rm?path=/sdcard/classes-20260416.log"

# Trigger a manual recording to capture what the room sounds like right now
curl -X POST "http://cry-detect-01.local/rec/trigger"
# → {"file":"manual-20260417T235012Z.wav"}

# Pull it
curl -s http://cry-detect-01.local/files/get?path=/sdcard/events/manual-20260417T235012Z.wav \
  > /tmp/sample.wav
```

That's every fieldwork operation an agent or human would want. **No SD removal, no physical access.**

---

## 6. UX bolt-on (minor)

Dashboard gets a single new card:

- **Files** panel: file tree of `/sdcard`, `/logs`, `/yamnet`. Click a file → download or tail. Click a cry-*.wav → embedded `<audio controls>`. Delete button with confirm.

Under 100 lines added to `app.js` + `index.html`. Optional; the HTTP API is the primary interface.

---

## 7. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Path traversal (attacker-crafted `?path=/../../sdkconfig`) | Strict whitelist + reject `..` |
| Accidentally deleting live log | Forbid deletion of `sd_logger_current_path()` |
| Fill SD with WAVs and trigger recorder DoS | Existing `CRY_REC_KEEP_FILES` pruning, plus add `CRY_REC_MIN_FREE_KB` check |
| Heavy download starving SSE | Semaphore cap on `/files/get` concurrency |
| Binary data corrupting HTTP chunked transfer | `Content-Type: application/octet-stream` + correct `Content-Length` when known |
| Delete race with writer (`rm /sdcard/cry-20260417.log` while `sd_logger` has it open) | Forbid rm of currently-open file; writer should also handle `ENOENT` and reopen |

---

## 8. Stage-2 position and cost

**Milestone 2.7** — file API. **Landed after** Milestones 2.6 (multi-class monitor) + 2.6a (classification logging) because those generate the data that the API is designed to retrieve.

- Core API (ls/get/stat/rm): **1 day**.
- Record-on-class extension: **½ day**.
- Dashboard file panel: **½ day** (optional).

Total: **~1–2 days**. Zero new hardware dependencies.

After this lands, we genuinely don't need to touch the SD card physically until it dies.

---

## 9. Interaction with other milestones

- Enables the **host-side classification analysis kit** from `classification-logging-plan.md` §6 — those scripts `curl` the log files via this API rather than needing local copies.
- Enables **remote calibration**: collect real-audio WAVs via `/rec/trigger`, pull via `/files/get`, rebuild `yamnet.tflite` on host, upload new model via a separate *upload* endpoint (explicitly out of scope here; deferred to Stage 3).
- Pairs with `DELETE /files/rm` to let the agent keep SD space bounded without physical intervention.

---

## 10. Summary

**~1–2 days of code turns the device into a self-serving data source.** Every artefact it produces — logs, recordings, model files, event WAVs — becomes reachable via a well-scoped, safety-checked HTTP API. The "agentic-first" development loop unlocks: a later session can spelunk the deployed device's state without needing physical access.

Queue as **Stage 2 Milestone 2.7**, immediately after classification logging (2.6a) lands and is producing data.
