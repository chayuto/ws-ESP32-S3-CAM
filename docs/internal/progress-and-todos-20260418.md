# cry-detect-01 — Progress snapshot & pending TODOs

*Written 2026-04-18 (Sydney AEST) at end-of-session. Device is running cleanly; commit `5942960` on `main` is the latest deploy.*

## Where we are

### Shipped in the last two sessions

**Session of 2026-04-17 → 2026-04-18 (today)**

| Commit | What |
|---|---|
| `5127379` | Stage 2.7 remote file API (`/files/ls`, `/get`, `/tail`, `/stat`, `/rm`, `/df`), Sydney AEST RFC-3339 timestamps, TZ env at boot, repo-level README |
| `5942960` | Stage 2.6a verbose 1 Hz JSONL classification logger (top-10 over all 521 AudioSet classes, watched 20, die-temp, per-task stack HWM), P0 hygiene fixes, coredump-to-flash partition, sdkconfig template updated |
| (staged in `docs/research/`) | `project-hygiene-audit-20260418.md` — 35-item punch list from full project audit |
| (staged in `.claude/commands/`) | `safe-dev.md` — pre-flash checklist + learned-once crash table |

### Verified-live on device (commit `5942960`)
- HTTP at `192.168.1.100` / `cry-detect-01.local`
- NTP synced, local time stamps on SD log
- `/sdcard/infer-20260418.jsonl` filling at ~1 KB/s with rich rows (top-10/521, 20 watched, die-temp, all task HWMs)
- `/sdcard/cry-20260418.log` filling on the 2 KB fsync cadence
- `/files/get`, `/files/tail`, `/files/df` all returning live data — no SD removal needed
- No crashes across 200+ seconds of monitored runtime after P0 fixes
- Core-dump-to-flash enabled — next panic will write an ELF to the new `coredump` partition, fetchable via `/files/get?path=...`

### Key empirical finding (still the critical blocker)
**INT8 PTQ from synthetic waveforms compressed every output of YAMNet into the 0.55–0.68 band.** Verified across two overnight runs + today's kitchen-ambience test. `cry_conf` correlates with presence of cry-like sound, but **absolute threshold is not separable from silence**. Rank-based interpretation still carries signal (`cry_baby` sat at the bottom of the watched list in a child-active kitchen). Stage 2.1 (real-audio recalibration) is the critical path for detection. Everything else is scaffolding.

## Pending TODOs — picked up in priority order

### P0 — unlikely to fire but should land before the next overnight

- [ ] **Audit P0 #5** (`event_recorder.c:169-199`): partial-write failure during recording leaves a stale `FILE *` → reset `s_recording = false` on any `fwrite` short-return before `continue`
- [ ] **Audit P0 #6** (`event_recorder.c:83`): pre-NTP filename `cry-boot%04u.wav` collides every 65 k boots → NVS-persisted boot counter (also useful as breadcrumb key per P2 #23)
- [ ] **Audit P0 #8** (`audio_stream.c:52,64,78`): unsynchronized `s_listener_count` → protect with a mutex or `atomic_fetch_add`

### P1 — one focused sweep, biggest reliability win

- [ ] **Audit P1 #10, #13, #14** — **fwrite / fopen error-swallowing across the project.** Every `fwrite`, `fopen`, `fclose` in `sd_logger.c`, `metrics_logger.c`, `event_recorder.c` should: (a) check return value, (b) increment a `metrics.sd_write_errors` counter on failure, (c) surface the counter in `/metrics` JSON so a silently-failing SD is visible on the web UI. One PR. Probably 60-90 min.
- [ ] **Audit P1 #11** — `RING_LINE_BYTES=448` in `sd_logger.c` may truncate v3 schema rows silently (15 fixed cols + 20 watched floats ≈ 300 chars, tight). Widen to 512 + assert on snprintf truncation.
- [ ] **Audit P1 #17** — `hk` task at 6 KB now drains the deferred-log queue + metrics + noise_floor. Measure actual HWM from the JSONL rows we're now collecting; bump to 8 KB if headroom < 1 KB.
- [ ] **Audit P1 #22** — bump `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` from the default 2304 → 4096. The JSONL already caught `sys_evt=608` HWM; prevents the whole class of esp_event crashes we hit today.

### P2 — debuggability, once P0/P1 settle

- [ ] **Coredump fetch endpoint** — build on what we already wired: new `/files/coredump` GET that reads `esp_core_dump_image_*` APIs, returns the ELF. Post-crash debugging becomes one curl + `addr2line` instead of DTR-trap serial dance. Probably 30 min + a research note on the coredump partition layout.
- [ ] **Audit P2 #22** — `/log/level?tag=X&level=debug` endpoint calling `esp_log_level_set` at runtime. 15-line handler.
- [ ] **Audit P2 #23** — NVS breadcrumb: on every major state transition write `{"stage": "...", "uptime_s": N}` to NVS. After a panic/reboot, one log row reports the last known stage. Catches silent-reboot causes that `coredump` misses (e.g. brown-out).

### P3 — cleanups to do when next in those files

- Tight task stacks (`led=644`, `led_task` never retains handle for shutdown)
- `sd_logger_tail` ring read/write race on the same slot (unlikely but present)
- `detector_submit` / `detector_get_state` lack synchronisation
- `qsort` per inference to compute p95 — replace with streaming percentile
- All the unchecked `snprintf` truncations listed in the audit

## Stage roadmap (above task-level, from `docs/internal/stage2-plan.md`)

| Stage | State | Notes |
|---|---|---|
| 1 — pretrained YAMNet INT8 + SD log + web UI | ✅ shipped | deployed 2026-04-17 overnight |
| 2.1 — **real-audio recalibration of INT8** | ⏳ **critical-path blocker** | needs ear-truth dataset from this apartment |
| 2.6a — per-inference classification logging | ✅ shipped today | JSONL files are the ear-truth input for 2.1 |
| 2.7 — remote file API | ✅ shipped | unblocks data export without SD removal |
| 2.8 — binary head on YAMNet embedding (transfer learning) | ⏳ after 2.1 | retraining-ROI analysis already done |
| 3 — discreet bedroom monitor UX refinements | ⏳ later | LED night mode done, more as needed |

## How to pick up next session (quickstart)

1. Verify device is still up: `curl -s http://192.168.1.100/metrics | python3 -m json.tool | head -20`
2. Pull the day's JSONL for analysis:
   ```bash
   TS=$(date -u +%Y%m%dT%H%M%SZ)
   DIR=/Users/chayut/repos/ws-ESP32-S3-CAM/logs/cry-detect-01-export-$TS
   mkdir -p $DIR
   curl -s "http://192.168.1.100/files/get?path=/sdcard/infer-$(date +%Y%m%d).jsonl" \
     -o $DIR/infer.jsonl
   wc -l $DIR/infer.jsonl
   ```
3. Read `docs/research/project-hygiene-audit-20260418.md` for the full 35-item list and pick a P0/P1/P2 batch.
4. Follow `.claude/commands/safe-dev.md` on any new subsystem.

## Open questions to think about between sessions

- **Stage 2.1 ear-truth dataset:** minimum viable approach is to leave the verbose JSONL running for a few days and manually label windows where a child was actually crying. The `top10/521` field in every row should give us enough signal to later synthesize calibration audio. Do we need a separate audio-recording channel, or is the already-captured WAVs from `event_recorder` enough once we lower its trigger?
- **Noise-floor adaptation window:** current 300 s warmup, 10% margin. JSONL will tell us whether p50/p95 are stable after warmup in a real room.
- **Log format v4 idea** (JSONL header per day, mel-on-trigger side-car) — parked; the verbose 1 Hz JSONL may be sufficient to skip v4 entirely.
