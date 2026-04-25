# Repo boundary — `ws-ESP32-S3-CAM` vs `yamnet-cry-distill-int8`

The cry-detect work splits cleanly into two concerns: a **device side**
(audio harvest, on-device inference, log/event tracking, runtime model
slot) and a **training side** (teacher/student knowledge distillation,
public-data evaluation, INT8 export, model card). Each side has a
different audience (ESP32/embedded vs audio-ML) and a different
publish boundary.

This document records which files live in which repo and what crosses
the boundary. The training repo does not exist yet at the time of
writing; this doc precedes its creation so the boundary is settled
before the migration.

The on-device YAMNet INT8 build (with the documented mel-magnitude
correction and double-sigmoid removal) is already published as
[`chayuto/yamnet-mel-int8-tflm`](https://huggingface.co/chayuto/yamnet-mel-int8-tflm).
The new training repo's eventual deliverable is a *distilled student
model* derived from that teacher — a different artifact, not a
replacement.

---

## `ws-ESP32-S3-CAM` (device-side workspace) keeps

| Layer | Files |
|---|---|
| **Firmware (runtime)** | `projects/cry-detect-01/main/*` — audio_capture, mel_features, yamnet, detector, event_recorder, auto_trigger, noise_floor, sd_logger, metrics, file_api, web_ui, network, log_retention, breadcrumb, led_alert |
| **Web UI** | `projects/cry-detect-01/www/` |
| **Runtime model slot** | `projects/cry-detect-01/spiffs/yamnet.tflite` (gitignored; populated by `fetch_model.sh` from HF) |
| **Harvest + extract** | `tools/extract_session.sh`, `tools/fetch_model.sh`, `tools/cry_monitor.sh` |
| **Host-side label production (auto-ensemble)** | `tools/ensemble_audit.py`, `freeze_release.py`, `build_inventory.py`, `audit_pipeline.sh`, `score_yamnet.py` |
| **Datasets** | `datasets/cry-detect-01/{captures,releases,master.csv,INVENTORY.md}` (all gitignored) |
| **Public docs** | `docs/research/*` (method, prior-art, plans), `CLAUDE.md`, `README.md`, project README, blog post |
| **Board slash commands** | `/build /flash /monitor /restore-factory /hardware-specs /peripherals` |

The auto-ensemble tooling stays here because its output (labels,
release JSONs, `INVENTORY.md`) is local-only data and the harvest →
label → audit pipeline is one coherent flow. Splitting it across
repos would only shuffle ownership without simplifying anything.

## `yamnet-cry-distill-int8` (new training repo) gets

| Layer | Files |
|---|---|
| **Teacher** | `src/teacher.py` — YAMNet FP32 from TF Hub, per-frame soft-target generation |
| **Student** | `src/student/{crnn,dscnn}.py` — small architectures (target ≤500 KB INT8) |
| **Data loaders** | `src/data/audioset.py` (yt-dlp + ID lists), `esc50.py`, `urbansound.py`, `home_captures.py` (consumes ws-ESP32-S3-CAM frozen-release JSONs via configurable path) |
| **Training** | `src/train.py` — distillation loop (KL on teacher logits, optional CE on in-domain ensemble labels) |
| **Eval** | `src/eval.py` — headline: held-out AudioSet split. Side: LOSO on home captures |
| **Quantization** | `src/quantize.py` — TFLite INT8 with representative-set selection |
| **PTQ harness (migrated)** | `src/quantize/repTQ.py` — moves from `ws-ESP32-S3-CAM/projects/cry-detect-01/tools/repTQ_yamnet.py` |
| **Artifact + card** | `models/`, `MODEL_CARD.md`, `README.md` (public pitch + reproducibility) |
| **Lab discipline** | `.claude/commands/ml-researcher.md`, `ml-experiments/<dated-dir>/` (gitignored) |
| **Repo slash commands** | `/train /eval /quantize /upload-hf /verify-on-device` |

## Contract between the repos

Three artifacts cross the boundary:

1. **Frozen release JSON** at
   `ws-ESP32-S3-CAM/datasets/cry-detect-01/releases/cry-vX.Y-ensemble.json`
   — schema documented in `host-side-auto-ensemble-method.md`. The
   training repo reads this via a configurable path env var
   (e.g. `CAPTURES_RELEASE_PATH`). Never copied across the boundary.
2. **Capture WAVs** at `ws-ESP32-S3-CAM/datasets/cry-detect-01/captures/`.
   Read by the training repo via filesystem path. Stays gitignored on
   both sides; never copied.
3. **Published HF model**. Training repo produces `.tflite` → uploads
   to HF → `ws-ESP32-S3-CAM/tools/fetch_model.sh` pulls it into
   `spiffs/`. `fetch_model.sh` becomes pluggable: YAMNet teacher
   (current, `chayuto/yamnet-mel-int8-tflm`) or distilled student
   (future).

## Privacy invariants

- Raw audio captures never leave `ws-ESP32-S3-CAM/datasets/`.
- Captures **may** be used as in-domain distillation data when training
  the student; the *resulting weights* are publishable as long as the
  story stands on public-data eval (held-out AudioSet) on its own
  merits. See repo-root `CLAUDE.md` "Publish boundary".
- Per-capture intermediate artifacts (label CSVs, release JSONs,
  ensemble pickles) stay local on both sides.
- Public eval headlined on AudioSet held-out; LOSO-on-captures kept
  as a side metric, never the lead.

## Migration list (one pass)

When the new repo is created, move:

- `projects/cry-detect-01/tools/repTQ_yamnet.py` → new repo
  `src/quantize/repTQ.py`. Negative-result writeup stays as a research
  note in `ws-ESP32-S3-CAM/docs/research/`.
- `.claude/commands/ml-researcher.md` → new repo
  `.claude/commands/ml-researcher.md`. Single source of truth lives in
  the training repo; ESP32-side `CLAUDE.md` cross-references it for
  the (smaller) sklearn fits that happen here for the auto-ensemble.

The rest of the training pipeline (teacher loader, student architectures,
data loaders, training loop, eval, quantization) is *new code* that
starts life in the new repo — there's nothing else to migrate.

## Two pieces deliberately left in `ws-ESP32-S3-CAM`

- **On-device verifier endpoint** (a `/verify` route in firmware that
  ingests a WAV via the file-API and returns inference output) — lives
  in firmware. The host-side driver script that pushes a known WAV and
  compares to an FP32 reference belongs in the training repo (it's an
  evaluation tool).
- **The auto-ensemble tooling** (`ensemble_audit.py`, `freeze_release.py`,
  `build_inventory.py`, `audit_pipeline.sh`, `score_yamnet.py`) — these
  produce labels, not models, and are tightly coupled to
  `datasets/cry-detect-01/`. Method documented publicly in
  `host-side-auto-ensemble-method.md`; the *tool* is reusable for any
  binary audio-event task, but spinning it off requires a second
  consumer that doesn't exist yet.
