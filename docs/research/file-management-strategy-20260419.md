# File management strategy — cry-detect-01

**Date:** 2026-04-19
**Motivation:** After one overnight deployment we have 22 WAVs, 95 MB of device-side inference logs, 700+ monitor samples, and a 1,804-row feature manifest across multiple files. Multiplied by one capture session per night plus model retraining cycles, file sprawl becomes the fastest-growing risk to the project. This doc sets the rules — what lives where, who owns cleanup, and which device code needs to catch up.

## 1. Growth projections (single board, current cadence)

### Device (SD card, 15.8 GB free)

| Artifact | Rate | 30-day | 365-day |
|---|---|---:|---:|
| `cry-YYYYMMDD.log` (esp_log text) | ~500 KB/day | 15 MB | 183 MB |
| `infer-YYYYMMDD.jsonl` (1 Hz metrics) | ~54 MB/day | 1.6 GB | 20 GB |
| `events/cry-*.wav` | capped at 5,000 files | — | — (retention loop) |
| `events/triggers.jsonl` | ~95 B/trigger | ~200 KB | ~2 MB |
| **Total at day 365** (no retention) | | | **~20.2 GB** |

Without retention, `infer-*.jsonl` runs the card out of space at roughly day **~280**. WAVs are bounded (5,000-file ceiling = 6.4 GB max). Text log is negligible. **The inference JSONL is the sole growth driver**, and it has no retention today.

### Local (repo `logs/`)

| Artifact | Per-night size |
|---|---:|
| WAV mirrors (22–50 files) | 28–64 MB |
| `device-logs/` (daily infer JSONL + text logs) | 50–100 MB |
| `manifest.csv` / `segments.csv` / `yamnet_*.csv` / `manifest.jsonl` / `segments.jsonl` | 3 MB |
| `specgrams/` (22 PNGs @ 120 dpi) | 4 MB |
| `monitor.jsonl` | 120 KB |
| **Total per session** | **~100 MB** |

One session per night → **~36 GB/year** of raw captures on local disk. Not catastrophic — SSDs are big — but structured archival is worth setting up before we have five of them interleaved with datasets.

## 2. Directory taxonomy (canonical)

```
ws-ESP32-S3-CAM/
├── logs/                              (gitignored)
│   ├── night-YYYYMMDD/                ← one per capture session, dated by
│   │   │                                start-of-night local date
│   │   ├── README.md                  ← human-readable index (this is critical)
│   │   ├── wavs/                      ← mirror of /sdcard/events/*.wav
│   │   ├── triggers.jsonl             ← mirror of /sdcard/events/triggers.jsonl
│   │   ├── monitor.jsonl              ← LAN-side 1-min polling
│   │   ├── monitor.sh, events.log,    ← monitor runtime artifacts
│   │   │   monitor.stdout, wavs_seen.txt
│   │   ├── device-logs/               ← mirror of /sdcard/ (logs only)
│   │   │   ├── infer-YYYYMMDD.jsonl
│   │   │   ├── cry-YYYYMMDD.log
│   │   │   └── CRY-0000.LOG / infer-boot.jsonl (if present)
│   │   ├── manifest.csv / .jsonl      ← file-level numeric features
│   │   ├── segments.csv / .jsonl      ← 0.96 s YAMNet-grid features
│   │   ├── yamnet_files.csv           ← FP32 oracle, file level
│   │   ├── yamnet_segments.csv        ← FP32 oracle, segment level
│   │   └── specgrams/*.png            ← log-mel PNGs per file
│   │
│   └── archive/                       ← sessions older than N days,
│       └── night-YYYYMMDD.tar.zst         compressed + moved here
│
├── datasets/                          (gitignored — not yet created)
│   ├── README.md                      ← versioning discipline
│   └── cry-vN/                        ← frozen curation, monotonic version
│       ├── manifest.jsonl             ← {file, start_s, end_s, label, source_session}
│       ├── positives/*.wav            ← 0.96 s clips extracted for training
│       └── negatives/*.wav
│
├── projects/cry-detect-01/
│   ├── main/*.c *.h                   ← firmware (git-tracked)
│   ├── tools/                         ← analysis + extract scripts (git-tracked)
│   └── hf/yamnet_class_map.csv        ← YAMNet label vocabulary (git-tracked)
│
└── docs/research/                     ← postmortems + strategy (git-tracked)
```

### Rules

1. **`logs/` is always write-once, read-many** after the capture session closes. No in-place edits, no "clean up the WAV names" — if you need curated clips, copy them to `datasets/cry-vN/`.
2. **Canonical filenames across all sessions:** `manifest.csv`, `segments.csv`, `yamnet_files.csv`, `yamnet_segments.csv`, `monitor.jsonl`, `triggers.jsonl`, `device-logs/`. Any session directory is machine-parseable without peeking inside first.
3. **Every `night-YYYYMMDD/` has a `README.md`** written at the close of the session with the dataset split, anomalies, and pointer to any related postmortem. Zero exceptions — future self cannot audit a pile of WAVs without it.
4. **`datasets/` are frozen.** Once `cry-vN` exists, don't modify it. If the curation needs to change, create `cry-v(N+1)`. This keeps retraining reproducible.
5. **Never commit WAVs, JSONL, or PNGs.** Privacy (home audio) and size. `.gitignore` already excludes `logs/` and venvs; add `datasets/`.

## 3. Device-side retention policy

The SD is the authoritative capture medium; local `logs/` is a mirror. Device-side retention has to catch up in the next flash.

### Existing retention
- **WAVs:** `event_recorder.c` retention loop, 5,000-file ceiling (`CRY_REC_KEEP_FILES`). Works.
- **`triggers.jsonl`:** grow-only, but tiny. Not an issue.

### Missing retention
- **`infer-YYYYMMDD.jsonl`:** no age cap. Grows forever until SD fills. **Needs retention.**
- **`cry-YYYYMMDD.log`:** same (rotates by day via `sd_logger.c:make_path_locked`, no age cap).
- **`infer-boot.jsonl`, `cry-NNNN.log`:** pre-NTP fallback files that accumulate across reboots. Minor but messy.

### Proposed additions (next flash — see §6)

A new `log_retention.c` module, 100 lines or so:

- Task loop running once per hour (cheap).
- Scans `/sdcard/` for `infer-*.jsonl` and `cry-*.log` matching the day-bucket pattern.
- Deletes files whose *filename-encoded date* is older than `CRY_LOG_RETENTION_DAYS` (default **14**).
- Deletes `infer-boot.jsonl` and `cry-NNNN.log` older than `CRY_LOG_PRENTP_KEEP` reboots' worth (default: keep the 3 most recent, delete the rest).
- Emits to `events.log` + `/metrics` any time it deletes a file, so we have audit trail.
- **Never touches** `/sdcard/events/` — WAV retention belongs to `event_recorder`.

Kconfig block:

```kconfig
menu "Log retention"
    config CRY_LOG_RETENTION_ENABLED
        bool "Enable device-side log retention task"
        default y
    config CRY_LOG_RETENTION_DAYS
        int "Days to keep infer-*.jsonl and cry-*.log"
        default 14
        range 1 365
    config CRY_LOG_PRENTP_KEEP
        int "Pre-NTP rotated log files to keep"
        default 3
        range 0 50
    config CRY_LOG_RETENTION_PERIOD_S
        int "Seconds between retention scans"
        default 3600
        range 60 86400
endmenu
```

### Disk-pressure breadcrumb

Separate, small addition: if `sd_free_bytes < CRY_SD_LOW_SPACE_BYTES` (default 256 MB), breadcrumb it via `/metrics.sd_low_space=true` and esp_log. Lets the bash monitor + dashboards see pressure before it becomes a write failure.

## 4. Local-side workflow

### Capture session start

Before deploying, on the host:

```zsh
mkdir -p logs/night-$(date +%Y%m%d)
cp <monitor.sh-template> logs/night-$(date +%Y%m%d)/monitor.sh
nohup bash logs/night-$(date +%Y%m%d)/monitor.sh &
```

(In practice, wrap this in `/monitor-deploy <label>` — not yet written. Current monitor.sh lives in the log dir.)

### Capture session end

One command pulls everything and runs the audit pipeline:

```zsh
tools/extract_session.sh logs/night-YYYYMMDD
```

The script (see `tools/extract_session.sh`):
1. Kills the running monitor (if still alive).
2. `curl`s all files in `/sdcard/events/` → `logs/night-YYYYMMDD/wavs/` + `triggers.jsonl`.
3. `curl`s `/sdcard/*.jsonl` and `/sdcard/*.log` → `logs/night-YYYYMMDD/device-logs/`.
4. Runs `audit_pipeline.sh logs/night-YYYYMMDD` to produce all analysis artifacts.
5. Prompts to write `README.md` (or generates a template).

### Archival (monthly cadence)

Any `logs/night-YYYYMMDD/` older than 30 days and not referenced by an active `datasets/cry-vN/` curation:

```zsh
tools/archive_session.sh logs/night-YYYYMMDD
# → logs/archive/night-YYYYMMDD.tar.zst (~15–20 MB)
# → remove the uncompressed dir
```

zstd -19 compresses 100 MB → ~15 MB for JSONL-heavy content. Tape-out quality.

### Cleanup of `.venv-analysis/`

Not gitignored by default — already fixed. Stays around because creating it is expensive (TF download). Delete when no longer needed for a while.

## 5. Dataset curation workflow (Stage 2.1 onwards)

Not implemented yet — described here so the shape is fixed up front.

### `datasets/cry-vN/manifest.jsonl`

One row per training sample:

```json
{"id": "night-20260418-050236-s07",
 "source_session": "night-20260418",
 "source_file": "cry-20260419T050236+1000.wav",
 "start_s": 3.36,
 "end_s": 4.32,
 "label": "cry",
 "yam_baby_cry_infant": 0.974,
 "yam_crying_sobbing": 0.999,
 "extraction_sha": "a3f2..." }
```

### Extraction

Given `segments.csv` + `yamnet_segments.csv`, pick rows where `yam_baby_cry_infant > 0.6` AND `hnr_db > 6`, slice 0.96 s clips from the parent WAV, write as `datasets/cry-vN/positives/night-YYYYMMDD-HHMMSS-sNN.wav`. Mirror workflow for negatives.

### Versioning

- Create `cry-v1` once, **freeze it**. Re-run retraining against `cry-v1` any number of times.
- When a new capture session adds useful data or the threshold changes, create `cry-v2`. Never mutate `cry-v1`.
- `datasets/README.md` lists every version + what was added + the retraining commit that first used it.

## 6. Concrete action items

Ranked:

1. **[next flash]** Add `log_retention.c` + Kconfig block (§3). Prevents SD fill.
2. **[next flash]** Disk-pressure breadcrumb in `metrics.c` / `sd_logger.c`.
3. **[now, local]** Write `tools/extract_session.sh` — ends the manual `curl` dance at the close of every session.
4. **[now, local]** Add `.claude/commands/extract-session.md` so `/extract-session night-YYYYMMDD` is one shot.
5. **[later]** Write `tools/archive_session.sh` — tar+zstd rollup. Not urgent until month-old sessions pile up.
6. **[later]** Write `tools/curate_dataset.py` — creates `datasets/cry-vN/` from one or more session dirs. Blocked on having more than one session worth retraining.
7. **[docs]** Add `logs/README.md` at the root of `logs/` pointing at this strategy doc. One place to find the rules.

## 7. Non-negotiables

- **Do not run device-side deletes manually** while a board is deployed and load-bearing. Let the retention task handle it. (Active rule — see the SD reseat postmortem for why.)
- **Do not rearrange historical `night-YYYYMMDD/` directories** after their README is written. Tools may need stable paths for retraining traceability.
- **Never put creds or personal identifiers in logs or manifests.** WAVs are home audio — treat as sensitive.
