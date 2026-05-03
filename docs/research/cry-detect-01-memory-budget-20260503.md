# cry-detect-01 memory budget

**Date:** 2026-05-03
**Firmware state:** Phase A — distilled student loaded in parallel with YAMNet teacher (CONFIG_CRY_STUDENT_ENABLED=y, v0.2.0).
**Build commit:** `5d6b9be` (`fetch_model.sh: also pull distilled student from HF v0.2.0`).

A snapshot of where flash and PSRAM go on the Waveshare ESP32-S3-CAM-GC2145 after the Phase A side-by-side student integration. Useful as a baseline before adding new features (caregiver-speech suppression, camera motion detection, longer audio buffers, etc.).

## Hardware

| | |
|---|---|
| Chip | ESP32-S3 rev v0.2 (Xtensa LX7 dual-core, 240 MHz) |
| Flash | 16 MB QIO @ 80 MHz |
| PSRAM | 8 MB OPI (~7.5 MB free to heap on a clean boot) |
| Internal SRAM | ~512 KB |

## Flash (16 MB total) — partition layout

`projects/cry-detect-01/partitions.csv`:

| Partition | Type | Size | Offset (auto) |
|---|---|---:|---|
| `nvs` | data | 24 KB | 0x9000 |
| `factory` | app | 5 MB | 0x10000 |
| `yamnet` | SPIFFS | 5 MB | auto |
| `logs_fat` | FAT | 1 MB | auto |
| `coredump` | coredump | 64 KB | auto |

Subtotal **~11.1 MB**. The remaining ~4.9 MB sits unallocated — available if/when we want to grow `factory` for OTA, add a second SPIFFS for a sub-classifier model, or carve out a recovery partition.

### Current usage (Phase A, build `5d6b9be`)

| Partition | Used | Free | Usage |
|---|---:|---:|---:|
| `factory` (app code) | 1,264,160 B / 1.21 MB | **3.79 MB** | 24 % |
| `yamnet` (SPIFFS) | 4,175,551 B / 3.98 MB | **1.02 MB** | 80 % |

SPIFFS contents:

| File | Size | Source |
|---|---:|---|
| `yamnet.tflite` | 4,061,160 B | `chayuto/yamnet-mel-int8-tflm` (teacher) |
| `student.tflite` | 112,848 B | `chayuto/yamnet-cry-distill-int8` v0.2.0 |
| `student.config.json` | 1,543 B | HF model card metadata (recommended threshold, class indices) |

## PSRAM (8 MB) — runtime allocations

All `heap_caps_malloc(... MALLOC_CAP_SPIRAM)` call sites in `main/`:

| Allocation | Size | Source | Notes |
|---|---:|---|---|
| YAMNet teacher model bytes | ~4.0 MB | `yamnet.cc:63` | size of yamnet.tflite |
| YAMNet teacher tensor arena | 1,536 KB | `yamnet.cc:114` | `CONFIG_CRY_DETECT_TENSOR_ARENA_KB` |
| **Student model bytes** | **~110 KB** | `student.cc:76` | size of student.tflite |
| **Student tensor arena** | **256 KB** | `student.cc:132` | `CONFIG_CRY_STUDENT_TENSOR_ARENA_KB` (was 96 KB; bumped after host estimate showed 170 KB naive sum + alignment headroom) |
| Event-recorder pre-roll ring | ~320 KB | `event_recorder.c:260` | 10 s × 16 kHz × 2 B (`CONFIG_CRY_REC_PREROLL_S=10`) |
| Mel-features Hann + FFT work + ring + accum | ~70 KB | `mel_features.c:106–110` | static at init |
| `metrics_logger` 521-class snapshot | 2 KB | `metrics_logger.c:399` | one float per class |
| Inference-task pcm + patch + all_confs | ~25 KB | `main.c:151–153` | static at task start |
| Audio-stream listener buffers | ~32 KB peak | `audio_stream.c:67` | only when stream client connected |
| File-API request buffers | ~50 KB peak | `file_api.c:136 / 170 / 313` | transient on HTTP request |
| **PSRAM in use (estimated)** | **~6.3 MB** | | |
| **PSRAM free at runtime** | **~1.7 MB** | | reported live in `/metrics.free_psram` |

### Phase A delta (vs teacher-only build)

| | Phase A OFF (baseline) | Phase A ON (today) | Δ |
|---|---:|---:|---:|
| Flash app binary | 1,261,648 B | 1,264,160 B | +2,512 B |
| Flash SPIFFS | 4,062,703 B | 4,175,551 B | +114,383 B |
| PSRAM (model bytes) | ~4.0 MB | ~4.11 MB | +110 KB |
| PSRAM (tensor arenas) | 1,536 KB | 1,792 KB | +256 KB |
| **Total PSRAM impact** | | | **+~370 KB** |

Phase A is cheap — under 5 % of free PSRAM consumed for a parallel model + arena.

## Internal SRAM (~512 KB)

Internal SRAM holds task stacks, static globals, IRAM-resident hot code paths, and the small heap. We don't size-budget this directly; the live `free_heap` field in `/metrics` is the canonical observation. Phase A doesn't materially affect internal SRAM — student code is small (~2.5 KB in flash) and the working set lives in PSRAM.

Per-task stacks (referenced from `main.c`):

| Task | Stack | Pinned |
|---|---:|---|
| `infer` | 16 KB | core 1 |
| `hk` (housekeeping) | 8 KB | core 0 |
| `metrics_logger` | (default) | (no pin) |

`metrics_logger` JSONL row buffer is 2 KB on the stack; we monitored it after the Phase A schema growth and kept the cap unchanged (current row at full schema is ~1 KB).

## Kconfig settings that affect memory

These knobs appear in `idf.py menuconfig` under "Cry-detect-01":

| Knob | Default | Memory effect |
|---|---:|---|
| `CRY_DETECT_TENSOR_ARENA_KB` | 1024 (we use 1536 in this build) | YAMNet arena, PSRAM |
| `CRY_STUDENT_ENABLED` | `n` | toggles entire student path |
| `CRY_STUDENT_TENSOR_ARENA_KB` | 256 | student arena, PSRAM (only if enabled) |
| `CRY_REC_PREROLL_S` | 10 | preroll ring, PSRAM (× 32 KB/s) |
| `CRY_REC_KEEP_FILES` | 500 | flash (SD card, not on-board) |
| `CRY_STREAM_RING_KB` | 32 | per-listener PSRAM (only when streaming) |

## Headroom check (what fits next)

Plenty of room across all three pools for the enrichment paths discussed in the integration plan:

- **App partition: 3.79 MB free.** Code growth from caregiver-speech logic (~hundreds of bytes), camera motion detection (~few KB), longer audio buffers (negligible code), or a sub-classifier wrapper (~3 KB like student.cc) is all far below the limit. We could grow the binary 3× before resizing the partition.
- **SPIFFS: 1.02 MB free.** Could hold:
  - 9 more 110 KB cry-head / sub-type / soundscape student models, OR
  - A second YAMNet variant for A/B comparison, OR
  - The Hann window LUT / FFT twiddle factors if we ever want to read them from flash instead of computing at boot.
- **PSRAM: ~1.7 MB free at idle.** Could hold:
  - A 4-second audio context buffer (4 × 16 kHz × 2 B = 128 KB) for windowed classification — easy fit
  - 3-5 additional 256 KB tensor arenas for parallel sub-classifier students
  - Camera frame buffers from the GC2145 if we wire that path (one 320×240 RGB frame = 230 KB)

## Next-step considerations

1. **Verify on-device after first flash.** Boot log will print `student: arena used: <n> / 262144 bytes`. Trim CRY_STUDENT_TENSOR_ARENA_KB once we know the real number; the host estimate of 170 KB is conservative. If real usage is ~120 KB, we can drop the Kconfig default back to 192 KB and reclaim ~64 KB for other features.
2. **Audit `event_recorder` preroll.** 320 KB of PSRAM held continuously is a substantial fraction of total budget. If we add the audio-context buffer and sub-classifier arenas, this is the first place to look for reclaim — could drop preroll to 5 s (160 KB) without losing much capture coverage.
3. **The 4.9 MB unallocated flash** is enough for an OTA partition pair (would need 2× factory size = 10 MB). If we want OTA we'd need to drop SPIFFS to 4 MB or carve from logs_fat. Defer until OTA is actually a goal.

## Cross-references

- Phase A integration plan: [`student-integration-plan-20260503.md`](student-integration-plan-20260503.md)
- Sibling repo (model source): [chayuto/yamnet-cry-distill-int8](https://github.com/chayuto/yamnet-cry-distill-int8) v0.2.0
- Hardware specs (full board reference): see `CLAUDE.md` "Board Facts" section
