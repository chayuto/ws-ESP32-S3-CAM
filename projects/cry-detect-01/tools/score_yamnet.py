#!/usr/bin/env python3
"""Oracle labelling of event WAVs using Google's FP32 YAMNet model.

Loads YAMNet via tensorflow-hub, scores every 40 s WAV, and emits:

  - yamnet_segments.csv   one row per 0.48 s hop (YAMNet-native frame)
                          with class scores for cry-relevant classes plus
                          top-3 most-activated class ids / names.
  - yamnet_files.csv      file-level aggregate (max / mean of cry classes).

YAMNet frames are 0.96 s windows at 0.48 s hop — identical grid to our
segments.csv, so the two can be row-aligned after the fact on
(file, seg_idx).

First run downloads ~17 MB of weights into ~/.tensorflow-hub/.

Usage:
    score_yamnet.py --wav-dir DIR \
                    --class-map hf/yamnet_class_map.csv \
                    --out-segments-csv yamnet_segments.csv \
                    --out-files-csv yamnet_files.csv
"""
from __future__ import annotations

import argparse
import csv
import sys
import wave
from pathlib import Path

import numpy as np

# Silence the TF banner. Set BEFORE importing tensorflow.
import os
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")

import tensorflow as tf  # noqa: E402
import tensorflow_hub as hub  # noqa: E402


YAMNET_HUB = "https://tfhub.dev/google/yamnet/1"

# Cry-relevant class indices (per yamnet_class_map.csv)
CRY_CLASSES = {
    14: "baby_laughter",
    19: "crying_sobbing",
    20: "baby_cry_infant",
    # extras — context / siblings
    0: "speech",
    1: "child_speech",
    4: "babbling",
    11: "screaming",
    13: "laughter",
}


def load_class_map(path: Path) -> list[str]:
    names: list[str] = []
    with path.open() as f:
        r = csv.DictReader(f)
        for row in r:
            names.append(row["display_name"])
    return names


def load_wav_float(path: Path) -> tuple[np.ndarray, int]:
    """Return mono float32 waveform in [-1, 1] and sample rate."""
    with wave.open(str(path), "rb") as w:
        sr = w.getframerate()
        n_ch = w.getnchannels()
        sw = w.getsampwidth()
        assert sw == 2, f"expected 16-bit, got {sw*8}-bit in {path.name}"
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if n_ch > 1:
        x = x.reshape(-1, n_ch).mean(axis=1)
    return x, sr


def score_file(model, x: np.ndarray, sr: int) -> np.ndarray:
    """Return scores array shape (N_frames, 521). YAMNet expects 16 kHz."""
    assert sr == 16000, f"YAMNet wants 16 kHz, got {sr}"
    scores, _embeddings, _spec = model(x)
    return scores.numpy()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav-dir", required=True, type=Path)
    ap.add_argument("--class-map", required=True, type=Path)
    ap.add_argument("--out-segments-csv", required=True, type=Path)
    ap.add_argument("--out-files-csv", required=True, type=Path)
    args = ap.parse_args()

    wavs = sorted(args.wav_dir.glob("*.wav"))
    if not wavs:
        print(f"no wavs in {args.wav_dir}", file=sys.stderr)
        return 1

    class_names = load_class_map(args.class_map)
    assert len(class_names) == 521, f"expected 521 classes, got {len(class_names)}"

    print(f"loading YAMNet from {YAMNET_HUB} ...", file=sys.stderr)
    model = hub.load(YAMNET_HUB)
    print("  loaded.", file=sys.stderr)

    seg_cols = [
        "file",
        "seg_idx",
        "start_s",
        "end_s",
    ] + [f"yam_{name}" for name in CRY_CLASSES.values()] + [
        "yam_top1_idx",
        "yam_top1_name",
        "yam_top1_score",
        "yam_top2_idx",
        "yam_top2_name",
        "yam_top2_score",
        "yam_top3_idx",
        "yam_top3_name",
        "yam_top3_score",
    ]
    file_cols = ["file", "n_frames"] + [
        f"{stat}_yam_{name}" for stat in ("max", "mean") for name in CRY_CLASSES.values()
    ]

    args.out_segments_csv.parent.mkdir(parents=True, exist_ok=True)
    args.out_files_csv.parent.mkdir(parents=True, exist_ok=True)

    with args.out_segments_csv.open("w", newline="") as fs, args.out_files_csv.open("w", newline="") as ff:
        ws = csv.writer(fs)
        ws.writerow(seg_cols)
        wf = csv.writer(ff)
        wf.writerow(file_cols)

        for i, p in enumerate(wavs, 1):
            try:
                x, sr = load_wav_float(p)
                scores = score_file(model, x, sr)  # (N, 521)
            except Exception as e:
                print(f"[{i:3}/{len(wavs)}] FAIL {p.name}: {e}", file=sys.stderr)
                continue

            n_frames = scores.shape[0]
            hop_s = 0.48
            win_s = 0.96

            # File-level aggregate
            file_row = [p.name, n_frames]
            for stat in ("max", "mean"):
                fn = np.max if stat == "max" else np.mean
                for idx in CRY_CLASSES:
                    file_row.append(float(fn(scores[:, idx])))
            wf.writerow(file_row)

            # Segment-level rows
            for seg_idx in range(n_frames):
                start_s = seg_idx * hop_s
                end_s = start_s + win_s
                row = [p.name, seg_idx, round(start_s, 3), round(end_s, 3)]
                for idx in CRY_CLASSES:
                    row.append(round(float(scores[seg_idx, idx]), 6))
                # top-3
                top3 = np.argsort(scores[seg_idx])[::-1][:3]
                for t in top3:
                    row.extend([int(t), class_names[int(t)], round(float(scores[seg_idx, t]), 6)])
                ws.writerow(row)

            top_cry = float(np.max(scores[:, 20]))
            top_crysob = float(np.max(scores[:, 19]))
            print(
                f"[{i:3}/{len(wavs)}] {p.name}  frames={n_frames}  "
                f"max(baby_cry)={top_cry:.3f}  max(crying_sobbing)={top_crysob:.3f}",
                file=sys.stderr,
            )

    print(
        f"\nwrote segments → {args.out_segments_csv}\nwrote files → {args.out_files_csv}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
