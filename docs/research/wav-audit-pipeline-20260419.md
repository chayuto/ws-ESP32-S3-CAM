# WAV audit pipeline — first run

**Date:** 2026-04-19
**Input:** `logs/night-20260418/wavs/` — 22 event WAVs (40 s × 16 kHz mono int16) from the overnight auto-trigger + phone-label session, + `triggers.jsonl` label ledger
**Output:** see `/audit-wavs` slash command

## Why this exists

Claude has no native audio-listening capability; raw WAVs are opaque. Opening 22 × 40 s = ~15 minutes of audio for human audition is also impractical after a disrupted night. A reusable offline pipeline replaces the listening step with three machine-readable artifacts: numeric features, spectrogram PNGs, and a FP32 YAMNet oracle label. Any future overnight capture can be processed in ~1 minute with `/audit-wavs <dir>`.

## Pipeline design

Single orchestrator `tools/audit_pipeline.sh <dir>` runs three stages:

### 1. Numeric features (`tools/audit_wavs.py`)

Two outputs from one pass over each WAV:

- `manifest.csv` / `.jsonl` — file level (32 feature columns)
- `segments.csv` / `.jsonl` — segment level, 0.96 s windows at 0.48 s hop (YAMNet-native grid, 82 segments per 40 s file)

Feature set per row:

| Domain | Features |
|---|---|
| Time | `rms_peak`, `rms_mean`, `rms_p50`, `rms_p95`, `noise_floor_p10`, `active_frac`, `onsets`, `zcr`, `clip_frac` |
| Spectral | `centroid_mean_hz` / `_std_hz`, `rolloff85_mean_hz`, `rolloff95_mean_hz`, `flatness_mean`, 6 band energy ratios (0–250, 250–500, 500–1k, 1k–2k, 2k–4k, 4k–8k Hz), `peak_hz_300_800`, `peak_hz_800_1600` |
| Pitch | `f0_voiced_frac`, `f0_mean_hz`, `f0_std_hz`, `f0_p50_hz`, `f0_p95_hz`, `hnr_db` |
| Context | `file`, `seg_idx`, `start_s`, `end_s`, `file_dur_s`, `file_noise_floor`, `file_active_floor`, `trigger_*` (joined from `triggers.jsonl`) |

Pitch uses frame-wise autocorrelation over 40 ms / 20 ms frames, F0 search 150–1200 Hz (infant cry fundamental ~300–700 Hz sits well inside this). HNR is computed from the per-frame peak autocorrelation of voiced frames.

### 2. Spectrograms (`tools/render_specgrams.py`)

One PNG per file — log-mel spectrogram (125 Hz – 7.5 kHz log-y) + RMS envelope + 0.48 s segment grid as vertical lines. `matplotlib` `Agg` backend, no display needed. These are readable via the `Read` tool, which means Claude can "look" at audio by looking at the PNG. Cry bursts show as stacked harmonics at ~400 Hz F0 with clear bands through 4 kHz; silence / non-voiced noise does not produce those stacks.

### 3. YAMNet FP32 oracle (`tools/score_yamnet.py`)

Loads Google's FP32 YAMNet from TF-Hub (`https://tfhub.dev/google/yamnet/1`) at 17 MB, runs each 40 s WAV through it, and writes:

- `yamnet_files.csv` — per-file max and mean on 8 cry-adjacent classes
- `yamnet_segments.csv` — per 0.96 s frame scores + top-3 activated classes

Cry-relevant class ids tracked: 14 (baby laughter), 19 (crying/sobbing), 20 (baby cry/infant), plus 0 speech, 1 child speech, 4 babbling, 11 screaming, 13 laughter as siblings / distractors.

This is the authoritative label for the retraining set. YAMNet FP32 is the model our on-device INT8 was distilled from, so whatever FP32 YAMNet says about a clip is by definition the label we want the INT8 to reproduce. Using it as the oracle is the core idea behind Stage 2.1 real-audio PTQ recalibration.

### Grid alignment

`segments.csv` uses start_s = seg_idx × 0.48, end_s = start_s + 0.96.
`yamnet_segments.csv` uses the same grid (YAMNet's internal patch_hop_seconds = 0.48, patch_frames = 96 frames at 10 ms). Rows are 1:1 joinable on (file, seg_idx), so heuristic + oracle columns end up in the same row with a trivial left-join.

## First-run findings (22 WAVs from 2026-04-18 overnight)

### File-level split — clean

| Group | max(baby_cry_infant) | max(crying_sobbing) |
|---|---|---|
| Incident (14 files, 04:54–05:12) | **0.88 – 0.98** | 0.98 – 0.999 |
| Bench / daytime tests (8 files) | 0.003 – 0.34 | 0.009 – 0.42 |

Bench files are noise floor, empty-room checks, or test transients. Incident files are unambiguous cry. No file straddles the boundary.

### Heuristic vs oracle agreement

| Threshold | Incident rows tagged | Bench rows tagged |
|---|---|---|
| YAMNet `yam_baby_cry_infant > 0.5` | ~35 % of segments | ~0 % |
| Heuristic `hnr_db > 6 dB` | 44 % | 4 % |
| Heuristic `voiced_frac > 0.5` | 35 % | 8 % |

The heuristic `hnr_db > 6 AND voiced_frac > 0.1` catches nearly the same segments as YAMNet's `baby_cry_infant > 0.5` — validates the heuristic as a fallback. YAMNet is more precise (fewer false positives on the bench side).

### Spectrogram spot-check

`logs/night-20260418/specgrams/cry-20260419T045502+1000.png` — visual confirmation of textbook infant cry: clean harmonic stacks (~400 Hz F0 + harmonics at 800/1200/1600/2000 Hz) during voiced bursts (0–7 s, 10–15 s, 19–22 s), near-silence between. RMS envelope rides the same intervals.

## Grid math sanity

- 40 s file / 0.48 s hop = 83.3 → **82 full-coverage segments** (our `audit_wavs.py` ends at the last complete 0.96 s window)
- YAMNet reports **83 frames** — one extra because its patch_frames = 96 frames of 10 ms mel = 0.96 s, and it doesn't require the right edge to sit inside the waveform (zero-pads the last patch). The per-frame `(start_s, end_s)` from both pipelines match to within one frame; a `seg_idx`-level join is safe.

## Reproduction

```zsh
# One-time env setup:
python3.13 -m venv .venv-analysis
.venv-analysis/bin/pip install 'setuptools<81' numpy scipy matplotlib tensorflow tensorflow-hub

# Each capture batch:
projects/cry-detect-01/tools/audit_pipeline.sh logs/night-YYYYMMDD
```

Run time on M-series Mac for 22 WAVs: ~25 s (numeric) + ~10 s (specgrams) + ~15 s (YAMNet warm) = under a minute.

## Next decisions (out of scope for this pipeline)

Left intentionally undecided — the pipeline is pure catalogue:

- Which segments to keep for the Stage 2.1 PTQ calibration set (threshold on `yam_baby_cry_infant` / `yam_crying_sobbing`)
- Whether to extract those segments as individual WAV files or keep them as start_s/end_s references into the parent 40 s file
- Whether to collapse per-segment labels into a per-file "is this file predominantly cry" categorical tag before retraining

Those decisions sit with whoever opens `segments.csv` and `yamnet_segments.csv` in the next session.
