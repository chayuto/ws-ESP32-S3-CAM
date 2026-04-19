#!/usr/bin/env python3
"""Render one PNG per WAV: log-mel spectrogram + RMS envelope + 0.96 s
segment grid.

Outputs go to <out-dir>/<stem>.png. Intended for visual audition when
the analyst cannot listen to audio directly.

Usage:
    render_specgrams.py --wav-dir DIR --out-dir DIR \
                        [--seg-len 0.96] [--seg-hop 0.48]
"""
from __future__ import annotations

import argparse
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal as sps

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        sr = w.getframerate()
        n_ch = w.getnchannels()
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if n_ch > 1:
        x = x.reshape(-1, n_ch).mean(axis=1)
    return x, sr


def mel_filterbank(sr: int, n_fft: int, n_mels: int = 64, f_min: float = 125.0, f_max: float = 7500.0) -> np.ndarray:
    """Simple triangular mel filterbank; no librosa dep."""
    def hz_to_mel(h): return 2595.0 * np.log10(1.0 + h / 700.0)
    def mel_to_hz(m): return 700.0 * (10 ** (m / 2595.0) - 1.0)
    mel_min, mel_max = hz_to_mel(f_min), hz_to_mel(f_max)
    mel_pts = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_pts = mel_to_hz(mel_pts)
    bin_pts = np.floor((n_fft + 1) * hz_pts / sr).astype(int)
    fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for m in range(1, n_mels + 1):
        l, c, r = bin_pts[m - 1], bin_pts[m], bin_pts[m + 1]
        if c == l:
            c = l + 1
        if r == c:
            r = c + 1
        for k in range(l, c):
            fb[m - 1, k] = (k - l) / max(1, c - l)
        for k in range(c, r):
            fb[m - 1, k] = (r - k) / max(1, r - c)
    return fb


def log_mel(x: np.ndarray, sr: int, n_fft: int = 1024, hop: int = 160, n_mels: int = 64) -> tuple[np.ndarray, np.ndarray]:
    f, t, Z = sps.stft(x, fs=sr, nperseg=n_fft, noverlap=n_fft - hop, boundary=None, padded=False)
    S = np.abs(Z).astype(np.float32) ** 2
    fb = mel_filterbank(sr, n_fft, n_mels=n_mels)
    M = fb @ S
    return t, 10.0 * np.log10(M + 1e-6)


def rms_env(x: np.ndarray, sr: int, win_ms: float = 50.0, hop_ms: float = 25.0) -> tuple[np.ndarray, np.ndarray]:
    win = int(sr * win_ms / 1000.0)
    hop = int(sr * hop_ms / 1000.0)
    if len(x) < win:
        return np.array([0.0]), np.array([np.sqrt(np.mean(x * x) + 1e-12)], dtype=np.float32)
    n_frames = 1 + (len(x) - win) // hop
    out = np.empty(n_frames, dtype=np.float32)
    for i in range(n_frames):
        s = i * hop
        seg = x[s : s + win]
        out[i] = float(np.sqrt(np.mean(seg * seg) + 1e-12))
    t = (np.arange(n_frames) * hop + win / 2) / sr
    return t, out


def render(path: Path, out_path: Path, seg_len: float, seg_hop: float) -> None:
    x, sr = load_wav(path)
    dur = len(x) / sr

    t_spec, LM = log_mel(x, sr, n_fft=1024, hop=160, n_mels=64)
    t_env, env = rms_env(x, sr)

    fig, (ax_env, ax_spec) = plt.subplots(
        2, 1, figsize=(14, 5), sharex=True, gridspec_kw={"height_ratios": [1, 3]}
    )

    ax_env.plot(t_env, env, color="#1f77b4", linewidth=0.9)
    ax_env.set_ylabel("RMS")
    ax_env.set_title(f"{path.name}   dur={dur:.2f}s  sr={sr}Hz  peak={env.max():.0f}")
    ax_env.grid(alpha=0.3)

    # Segment grid — only lines, no labels, light grey
    t = 0.0
    while t < dur:
        ax_env.axvline(t, color="#aaaaaa", linewidth=0.3, alpha=0.5)
        ax_spec.axvline(t, color="#ffffff", linewidth=0.3, alpha=0.25)
        t += seg_hop

    im = ax_spec.imshow(
        LM,
        aspect="auto",
        origin="lower",
        cmap="magma",
        extent=[t_spec[0] if len(t_spec) else 0, t_spec[-1] if len(t_spec) else dur, 125, 7500],
        vmin=float(np.percentile(LM, 5)),
        vmax=float(np.percentile(LM, 99)),
    )
    ax_spec.set_yscale("log")
    ax_spec.set_yticks([125, 250, 500, 1000, 2000, 4000, 7500])
    ax_spec.set_yticklabels(["125", "250", "500", "1k", "2k", "4k", "7.5k"])
    ax_spec.set_ylabel("Hz (log)")
    ax_spec.set_xlabel("time (s)")
    ax_spec.set_xlim(0, dur)

    # Colorbar sharing spec axis
    cbar = fig.colorbar(im, ax=ax_spec, pad=0.01, fraction=0.025)
    cbar.set_label("log mel (dB)")

    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav-dir", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--seg-len", type=float, default=0.96)
    ap.add_argument("--seg-hop", type=float, default=0.48)
    args = ap.parse_args()

    wavs = sorted(args.wav_dir.glob("*.wav"))
    if not wavs:
        print(f"no wavs in {args.wav_dir}", file=sys.stderr)
        return 1

    for i, p in enumerate(wavs, 1):
        out = args.out_dir / (p.stem + ".png")
        try:
            render(p, out, args.seg_len, args.seg_hop)
            print(f"[{i:3}/{len(wavs)}] {p.name} → {out.name}", file=sys.stderr)
        except Exception as e:
            print(f"[{i:3}/{len(wavs)}] FAIL {p.name}: {e}", file=sys.stderr)

    print(f"\nwrote {len(wavs)} PNGs → {args.out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
