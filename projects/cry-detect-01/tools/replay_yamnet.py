#!/usr/bin/env python3
"""Replay the 22 overnight WAVs through the INT8 on-device YAMNet and
compare to the FP32 oracle in yamnet_segments.csv.

Why this exists: the on-device detector's cry_conf was pinned in
[0.563, 0.651] all night and never triggered once. This script runs
the same INT8 model (`spiffs/yamnet-recalib.tflite` by default) against
the same audio the firmware would have seen, so we can pick
CRY_DETECT_THRESHOLD numerically rather than by guessing.

Three output interpretations are recorded per patch so we can see which
one matches the FP32 oracle:

  - `dequant_float`     : output_scale * (raw - output_zero_point). If
                          the model's final activation was folded into
                          quantization, this is already a probability.
  - `sigmoid_of_dequant`: sigmoid(dequant_float). What firmware
                          currently reports as cry_conf. Correct only if
                          `dequant_float` is a logit.
  - `raw_int8`          : the pre-dequant value. What firmware would
                          compare to CRY_DETECT_THRESHOLD if we fixed
                          main.c to honour the Kconfig int.

Usage:
    .venv-analysis/bin/python \
        projects/cry-detect-01/tools/replay_yamnet.py \
        --wav-dir logs/night-20260418/wavs/ \
        --model projects/cry-detect-01/spiffs/yamnet-recalib.tflite \
        --oracle-csv logs/night-20260418/yamnet_segments.csv \
        --out-csv /tmp/replay-recalib.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
import wave
from pathlib import Path
from statistics import mean, median

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")

import numpy as np
import tensorflow as tf

# Use the vendored Google YAMNet features/params (same code convert_yamnet.py
# calibrates against — guarantees replay log-mels match calibration log-mels).
SCRIPT_DIR = Path(__file__).resolve().parent
YAMNET_WORK = SCRIPT_DIR / ".yamnet_work"


def load_wav_float(path: Path) -> tuple[np.ndarray, int]:
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


def patches_from_wav(path: Path, features_mod, params_mod) -> np.ndarray:
    """Return log-mel patches shaped [n_patches, 96, 64]."""
    x, sr = load_wav_float(path)
    assert sr == 16000, f"{path}: sr={sr}"
    p = params_mod.Params(sample_rate=16000, patch_hop_seconds=0.48)
    _spec, patches = features_mod.waveform_to_log_mel_spectrogram_patches(
        tf.constant(x, dtype=tf.float32), p)
    return patches.numpy()


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav-dir", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--oracle-csv", required=True)
    ap.add_argument("--out-csv", required=True)
    ap.add_argument("--baby-cry-index", type=int, default=20)
    args = ap.parse_args()

    # Vendored Google YAMNet features/params
    if str(YAMNET_WORK) not in sys.path:
        sys.path.insert(0, str(YAMNET_WORK))
    import features as features_mod
    import params as params_mod

    # Load TFLite model
    interp = tf.lite.Interpreter(model_path=args.model)
    interp.allocate_tensors()
    in_det = interp.get_input_details()[0]
    out_det = interp.get_output_details()[0]
    in_scale, in_zp = in_det["quantization"]
    out_scale, out_zp = out_det["quantization"]
    print(f"model: {args.model}")
    print(f"  input : {in_det['dtype'].__name__} shape={in_det['shape']} scale={in_scale:.6f} zp={in_zp}")
    print(f"  output: {out_det['dtype'].__name__} shape={out_det['shape']} scale={out_scale:.6f} zp={out_zp}")

    # Oracle lookup {(file, seg_idx): fp32_cry}
    oracle = {}
    with open(args.oracle_csv) as f:
        r = csv.DictReader(f)
        for row in r:
            key = (row["file"], int(row.get("seg_idx", row.get("frame_idx", 0))))
            try:
                oracle[key] = float(row["yam_baby_cry_infant"])
            except (KeyError, ValueError):
                pass
    print(f"oracle rows: {len(oracle)}")

    wav_paths = sorted(Path(args.wav_dir).glob("*.wav"))
    print(f"WAVs: {len(wav_paths)}")

    out_rows = []
    per_file_stats = []
    for wp in wav_paths:
        patches = patches_from_wav(wp, features_mod, params_mod)
        n = patches.shape[0]
        for seg_idx in range(n):
            patch = patches[seg_idx].astype(np.float32)
            q = np.round(patch / in_scale + in_zp).astype(np.int32)
            q = np.clip(q, -128, 127).astype(np.int8)
            interp.set_tensor(in_det["index"], q[np.newaxis, ...])
            interp.invoke()
            out = interp.get_tensor(out_det["index"])[0]
            raw = int(out[args.baby_cry_index])
            dequant = float(out_scale) * (raw - int(out_zp))
            sig = sigmoid(dequant)
            fp32 = oracle.get((wp.name, seg_idx))
            out_rows.append({
                "file": wp.name,
                "seg_idx": seg_idx,
                "raw_int8": raw,
                "dequant_float": round(dequant, 5),
                "sigmoid_of_dequant": round(float(sig), 5),
                "fp32_oracle": round(fp32, 5) if fp32 is not None else "",
            })
        # per-file aggregate (use dequant_float as candidate probability)
        file_rows = out_rows[-n:]
        dq = [r["dequant_float"] for r in file_rows]
        sg = [r["sigmoid_of_dequant"] for r in file_rows]
        rr = [r["raw_int8"] for r in file_rows]
        oraclev = [r["fp32_oracle"] for r in file_rows if r["fp32_oracle"] != ""]
        per_file_stats.append({
            "file": wp.name, "n": n,
            "dequant_max": max(dq), "dequant_mean": mean(dq),
            "sig_max": max(sg), "sig_mean": mean(sg),
            "raw_max": max(rr), "raw_mean": mean(rr),
            "fp32_max": max(oraclev) if oraclev else None,
        })
        print(f"  {wp.name:40s} n={n:>3} raw_max={max(rr):+4} dq_max={max(dq):+.3f} sig_max={max(sg):.3f} fp32_max={max(oraclev) if oraclev else 'n/a'}")

    # Write per-segment CSV
    with open(args.out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()))
        w.writeheader()
        for r in out_rows:
            w.writerow(r)
    print(f"\nwrote {len(out_rows)} segments -> {args.out_csv}")

    # Grouping: incident vs bench (filename prefix rule from night-20260418)
    def grp(name): return "incident" if name.startswith("cry-20260419T0") else "bench"
    inc = [s for s in per_file_stats if grp(s["file"]) == "incident"]
    ben = [s for s in per_file_stats if grp(s["file"]) == "bench"]

    print("\n== SEPARATION (per-file max across all metrics) ==")
    print(f"INCIDENT (n={len(inc)}):")
    print(f"  raw_max:  min={min(s['raw_max'] for s in inc):+4} med={median([s['raw_max'] for s in inc]):+4} max={max(s['raw_max'] for s in inc):+4}")
    print(f"  dq_max:   min={min(s['dequant_max'] for s in inc):+.3f} med={median([s['dequant_max'] for s in inc]):+.3f} max={max(s['dequant_max'] for s in inc):+.3f}")
    print(f"  sig_max:  min={min(s['sig_max'] for s in inc):.3f} med={median([s['sig_max'] for s in inc]):.3f} max={max(s['sig_max'] for s in inc):.3f}")
    print(f"BENCH (n={len(ben)}):")
    print(f"  raw_max:  min={min(s['raw_max'] for s in ben):+4} med={median([s['raw_max'] for s in ben]):+4} max={max(s['raw_max'] for s in ben):+4}")
    print(f"  dq_max:   min={min(s['dequant_max'] for s in ben):+.3f} med={median([s['dequant_max'] for s in ben]):+.3f} max={max(s['dequant_max'] for s in ben):+.3f}")
    print(f"  sig_max:  min={min(s['sig_max'] for s in ben):.3f} med={median([s['sig_max'] for s in ben]):.3f} max={max(s['sig_max'] for s in ben):.3f}")

    # Which interpretation agrees with FP32 oracle?
    # Compute correlation of each column with fp32_oracle, patch by patch.
    paired = [r for r in out_rows if r["fp32_oracle"] != ""]
    if paired:
        def corr(xs, ys):
            xs, ys = np.array(xs, dtype=np.float64), np.array(ys, dtype=np.float64)
            if xs.std() == 0 or ys.std() == 0: return float("nan")
            return float(np.corrcoef(xs, ys)[0, 1])
        fp32 = [r["fp32_oracle"] for r in paired]
        print(f"\n== CORRELATION WITH FP32 ORACLE (n={len(paired)}) ==")
        print(f"  raw_int8           : r = {corr([r['raw_int8'] for r in paired], fp32):+.4f}")
        print(f"  dequant_float      : r = {corr([r['dequant_float'] for r in paired], fp32):+.4f}")
        print(f"  sigmoid_of_dequant : r = {corr([r['sigmoid_of_dequant'] for r in paired], fp32):+.4f}")

    # Threshold recommendation (assumes dequant_float is the probability,
    # verified by correlation above)
    dq_inc_max = [s["dequant_max"] for s in inc]
    dq_ben_max = [s["dequant_max"] for s in ben]
    if dq_inc_max and dq_ben_max:
        # Halfway between bench-max and incident-min (most conservative pick)
        ben_max = max(dq_ben_max)
        inc_min = min(dq_inc_max)
        thr = (ben_max + inc_min) / 2
        print(f"\n== THRESHOLD PICK (dequant_float) ==")
        print(f"  bench max (worst FP): {ben_max:+.3f}")
        print(f"  incident min (worst TP miss): {inc_min:+.3f}")
        print(f"  midpoint threshold  : {thr:+.3f}  ← propose as detector threshold")
        if ben_max < inc_min:
            print(f"  separation margin   : {inc_min - ben_max:+.3f}  (safe)")
        else:
            print(f"  OVERLAP: bench_max >= incident_min — no clean pick possible")

    return 0


if __name__ == "__main__":
    sys.exit(main())
