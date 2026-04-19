# Audit captured event WAVs

Run the full offline audit pipeline on a directory of cry-detect-01 event
WAVs. Produces numeric manifests, per-file spectrogram PNGs, and
YAMNet FP32 oracle labels — no human listening required.

Usage: `/audit-wavs <dir>`

`<dir>` is a directory laid out as:

```
<dir>/
├── wavs/                      ← 40 s, 16 kHz mono int16 WAVs
├── triggers.jsonl             ← optional, ledger from /record/trigger
└── (outputs written here after run)
```

Typical example: `/audit-wavs logs/night-20260418`

---

## What the pipeline does

Three stages, orchestrated by `projects/cry-detect-01/tools/audit_pipeline.sh`:

1. **Numeric features** — `tools/audit_wavs.py`
   - `manifest.csv` / `manifest.jsonl` — one row per WAV (32 columns)
   - `segments.csv` / `segments.jsonl` — one row per 0.96 s / 0.48 s hop
     segment (~82 per 40 s file). Time-domain RMS envelope, ZCR, clipping;
     spectral centroid, rolloff, flatness; 6 energy bands; F0 + HNR.
2. **Spectrograms** — `tools/render_specgrams.py`
   - `specgrams/<stem>.png` — mel spectrogram (125 Hz – 7.5 kHz, log axis)
     + RMS envelope + 0.48 s segment grid. Useful when Claude is doing
     the audit: PNGs are readable via the `Read` tool.
3. **YAMNet FP32 oracle** — `tools/score_yamnet.py`
   - `yamnet_files.csv` — per-file max/mean of cry-relevant classes
   - `yamnet_segments.csv` — per 0.96 s frame scores for:
     `baby_cry_infant`, `crying_sobbing`, `baby_laughter`, `speech`,
     `child_speech`, `babbling`, `screaming`, `laughter`
     plus the top-3 activated class ids / names.

Segment grids align: `segments.csv` and `yamnet_segments.csv` share
(`file`, `seg_idx`) identity, so heuristic + oracle columns can be
joined 1:1.

## Environment

Uses `<repo>/.venv-analysis` — Python 3.13 with numpy, scipy,
matplotlib, tensorflow, tensorflow-hub. Create it once with:

```zsh
python3.13 -m venv .venv-analysis
.venv-analysis/bin/pip install 'setuptools<81' numpy scipy matplotlib tensorflow tensorflow-hub
```

First YAMNet load downloads ~17 MB of weights to `~/.tensorflow-hub/`.
Subsequent runs are cache-local and take ~30 s for 22 files on an M-series Mac.

## Running it

```zsh
projects/cry-detect-01/tools/audit_pipeline.sh logs/night-20260418
```

Or from the slash command handler: invoke the script with the provided
directory. Do **not** re-run stages individually unless debugging — the
orchestrator keeps paths consistent.

## Downstream use

After the run, decision logic is applied *against the artifacts*, not
the raw WAVs:

- `yamnet_segments.csv` column `yam_baby_cry_infant > 0.5` is a strong
  positive label.
- `segments.csv` columns `hnr_db > 6` AND `f0_voiced_frac > 0.1` is a
  good heuristic fallback.
- Mismatch (YAMNet says cry but heuristic doesn't, or vice versa) is a
  review flag.
- Any 0.96 s segment is directly usable as a calibration sample for
  `tools/convert_yamnet.py --audio-dir <extracted-segments>`.

See `docs/research/wav-audit-pipeline-20260419.md` for the process
notes and first-run findings.
