#!/usr/bin/env python3
"""Offline time + spectral analysis of captured event WAVs.

Reads a directory of 40 s, 16 kHz mono int16 WAVs plus a
`triggers.jsonl` label ledger, and emits BOTH a file-level manifest
AND a segment-level manifest (0.96 s / 0.48 s hop, YAMNet-native).

No decisions are made here — this is the catalogue step.

Usage:
    audit_wavs.py --wav-dir DIR --triggers triggers.jsonl \
                  --out-csv manifest.csv \
                  --out-jsonl manifest.jsonl \
                  --out-segments-csv segments.csv \
                  --out-segments-jsonl segments.jsonl
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal as sps

# ----------------------------------------------------------------------
# Analysis grid constants — pick hops that divide evenly into a 0.96 s
# segment so file-level precompute maps cleanly to segment slices.
# ----------------------------------------------------------------------

SEG_LEN_S = 0.96
SEG_HOP_S = 0.48
ENV_WIN_MS = 50.0
ENV_HOP_MS = 25.0        # 40 frames/s → 38 frames per 0.96 s segment
STFT_NPERSEG = 1024      # 64 ms at 16 kHz
STFT_NOVERLAP = 512      # 32 ms hop → 31.25 frames/s
F0_WIN_MS = 40.0
F0_HOP_MS = 20.0         # 50 frames/s → 48 frames per 0.96 s segment


# ----------------------------------------------------------------------
# WAV loader
# ----------------------------------------------------------------------


def load_wav_mono16(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        sr = w.getframerate()
        n_ch = w.getnchannels()
        n_fr = w.getnframes()
        sw = w.getsampwidth()
        assert sw == 2, f"expected 16-bit, got {sw*8}-bit in {path.name}"
        raw = w.readframes(n_fr)
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if n_ch > 1:
        x = x.reshape(-1, n_ch).mean(axis=1)
    return x, sr


# ----------------------------------------------------------------------
# Full-file pre-computes — one pass each, segments slice into them.
# ----------------------------------------------------------------------


def rms_envelope(x: np.ndarray, sr: int, win_ms: float = ENV_WIN_MS, hop_ms: float = ENV_HOP_MS) -> np.ndarray:
    win = max(1, int(sr * win_ms / 1000.0))
    hop = max(1, int(sr * hop_ms / 1000.0))
    if len(x) < win:
        return np.array([float(np.sqrt(np.mean(x * x) + 1e-12))], dtype=np.float32)
    n_frames = 1 + (len(x) - win) // hop
    out = np.empty(n_frames, dtype=np.float32)
    for i in range(n_frames):
        s = i * hop
        seg = x[s : s + win]
        out[i] = float(np.sqrt(np.mean(seg * seg) + 1e-12))
    return out


def stft_mag(x: np.ndarray, sr: int) -> tuple[np.ndarray, np.ndarray]:
    f, _t, Z = sps.stft(
        x,
        fs=sr,
        nperseg=STFT_NPERSEG,
        noverlap=STFT_NOVERLAP,
        boundary=None,
        padded=False,
    )
    return f, np.abs(Z).astype(np.float32)


def f0_track(
    x: np.ndarray,
    sr: int,
    f_lo: float = 150.0,
    f_hi: float = 1200.0,
    energy_floor: float = 300.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Frame-wise F0 Hz and voicing confidence (0..1)."""
    win = int(sr * F0_WIN_MS / 1000.0)
    hop = int(sr * F0_HOP_MS / 1000.0)
    if len(x) < win:
        return np.array([], dtype=np.float32), np.array([], dtype=np.float32)
    lag_lo = max(1, int(sr / f_hi))
    lag_hi = min(win - 1, int(sr / f_lo))
    n_frames = 1 + (len(x) - win) // hop
    f0 = np.zeros(n_frames, dtype=np.float32)
    conf = np.zeros(n_frames, dtype=np.float32)
    for i in range(n_frames):
        s = i * hop
        seg = x[s : s + win].astype(np.float64)
        seg = seg - seg.mean()
        e = float(np.sqrt(np.mean(seg * seg) + 1e-12))
        if e < energy_floor:
            continue
        ac = np.correlate(seg, seg, mode="full")[win - 1 :]
        if ac[0] <= 0:
            continue
        ac = ac / ac[0]
        region = ac[lag_lo : lag_hi + 1]
        if len(region) == 0:
            continue
        peak = int(np.argmax(region))
        lag = lag_lo + peak
        f0[i] = sr / lag
        conf[i] = float(region[peak])
    return f0, conf


# ----------------------------------------------------------------------
# Scalar helpers
# ----------------------------------------------------------------------


def zcr(x: np.ndarray) -> float:
    if len(x) < 2:
        return 0.0
    s = np.sign(x)
    s[s == 0] = 1
    return float(np.mean(np.abs(np.diff(s)) > 0))


def clip_ratio(x: np.ndarray, thresh: float = 32000.0) -> float:
    if len(x) == 0:
        return 0.0
    return float(np.mean(np.abs(x) >= thresh))


def onset_count(env: np.ndarray, floor: float, hop_ms: float, refractory_ms: float = 150.0) -> int:
    if len(env) < 2:
        return 0
    above = env > floor
    rising = np.where((~above[:-1]) & above[1:])[0]
    if len(rising) == 0:
        return 0
    refractory_frames = max(1, int(refractory_ms / hop_ms))
    kept = [int(rising[0])]
    for idx in rising[1:]:
        if idx - kept[-1] >= refractory_frames:
            kept.append(int(idx))
    return len(kept)


CRY_BANDS = [
    ("b_0_250", 0, 250),
    ("b_250_500", 250, 500),
    ("b_500_1k", 500, 1000),
    ("b_1k_2k", 1000, 2000),
    ("b_2k_4k", 2000, 4000),
    ("b_4k_8k", 4000, 8000),
]


def spectral_aggs(f: np.ndarray, S: np.ndarray) -> dict:
    """Aggregate spectral features over whatever frames S contains.

    S has shape (F_bins, n_frames); if n_frames == 0 we return zeros
    so callers still get a stable schema."""
    if S.size == 0 or S.shape[1] == 0:
        out = {
            "centroid_mean_hz": 0.0,
            "centroid_std_hz": 0.0,
            "rolloff85_mean_hz": 0.0,
            "rolloff95_mean_hz": 0.0,
            "flatness_mean": 0.0,
            "peak_hz_300_800": 0.0,
            "peak_hz_800_1600": 0.0,
        }
        for name, _, _ in CRY_BANDS:
            out[name] = 0.0
        return out

    eps = 1e-12
    denom = S.sum(axis=0) + eps
    centroid = (f[:, None] * S).sum(axis=0) / denom

    cum = np.cumsum(S, axis=0)
    total = cum[-1, :] + eps
    idx85 = np.argmax(cum >= 0.85 * total[None, :], axis=0)
    idx95 = np.argmax(cum >= 0.95 * total[None, :], axis=0)
    rolloff85 = f[idx85]
    rolloff95 = f[idx95]

    geo = np.exp(np.mean(np.log(S + eps), axis=0))
    arith = np.mean(S, axis=0) + eps
    flatness = geo / arith

    grand_total = float((S ** 2).sum()) + eps
    be = {}
    for name, lo, hi in CRY_BANDS:
        m = (f >= lo) & (f < hi)
        be[name] = float((S[m, :] ** 2).sum() / grand_total) if np.any(m) else 0.0

    def _peak_in(lo: float, hi: float) -> float:
        m = (f >= lo) & (f < hi)
        if not np.any(m):
            return 0.0
        band_S = S[m, :]
        band_f = f[m]
        mean_t = band_S.mean(axis=1)
        return float(band_f[int(np.argmax(mean_t))])

    out = {
        "centroid_mean_hz": float(centroid.mean()),
        "centroid_std_hz": float(centroid.std()),
        "rolloff85_mean_hz": float(rolloff85.mean()),
        "rolloff95_mean_hz": float(rolloff95.mean()),
        "flatness_mean": float(flatness.mean()),
        **be,
        "peak_hz_300_800": _peak_in(300.0, 800.0),
        "peak_hz_800_1600": _peak_in(800.0, 1600.0),
    }
    return out


def env_aggs(env: np.ndarray, floor: float) -> dict:
    if len(env) == 0:
        return {
            "rms_peak": 0.0,
            "rms_mean": 0.0,
            "rms_p50": 0.0,
            "rms_p95": 0.0,
            "noise_floor_p10": 0.0,
            "active_frac": 0.0,
            "onsets": 0,
        }
    return {
        "rms_peak": float(env.max()),
        "rms_mean": float(env.mean()),
        "rms_p50": float(np.percentile(env, 50)),
        "rms_p95": float(np.percentile(env, 95)),
        "noise_floor_p10": float(np.percentile(env, 10)),
        "active_frac": float(np.mean(env > floor)),
        "onsets": onset_count(env, floor, hop_ms=ENV_HOP_MS),
    }


def f0_aggs(f0: np.ndarray, conf: np.ndarray, conf_thresh: float = 0.3) -> dict:
    if len(conf) == 0:
        return {
            "f0_voiced_frac": 0.0,
            "f0_mean_hz": 0.0,
            "f0_std_hz": 0.0,
            "f0_p50_hz": 0.0,
            "f0_p95_hz": 0.0,
            "hnr_db": 0.0,
        }
    voiced = conf > conf_thresh
    voiced_frac = float(np.mean(voiced))
    fv = f0[voiced]
    if len(fv):
        f0_mean = float(fv.mean())
        f0_std = float(fv.std())
        f0_p50 = float(np.percentile(fv, 50))
        f0_p95 = float(np.percentile(fv, 95))
    else:
        f0_mean = f0_std = f0_p50 = f0_p95 = 0.0
    # HNR from voiced confidences
    if voiced.any():
        r = np.clip(conf[voiced], 0.001, 0.999)
        hnr = float(10.0 * np.log10(np.mean(r / (1 - r))))
    else:
        hnr = 0.0
    return {
        "f0_voiced_frac": voiced_frac,
        "f0_mean_hz": f0_mean,
        "f0_std_hz": f0_std,
        "f0_p50_hz": f0_p50,
        "f0_p95_hz": f0_p95,
        "hnr_db": hnr,
    }


# ----------------------------------------------------------------------
# File-level pre-compute bundle, reused across all segments
# ----------------------------------------------------------------------


class FileAnalysis:
    def __init__(self, path: Path):
        self.path = path
        self.x, self.sr = load_wav_mono16(path)
        self.n = len(self.x)
        self.dur = self.n / self.sr if self.sr else 0.0

        self.env = rms_envelope(self.x, self.sr)
        self.env_per_sec = 1000.0 / ENV_HOP_MS  # frames/s

        self.f_bins, self.S = stft_mag(self.x, self.sr)
        stft_hop = STFT_NPERSEG - STFT_NOVERLAP
        self.stft_per_sec = self.sr / stft_hop

        self.f0, self.conf = f0_track(self.x, self.sr)
        self.f0_per_sec = 1000.0 / F0_HOP_MS

        # File-wide noise floor for a stable "active" threshold across segments
        self.noise_floor = float(np.percentile(self.env, 10)) if len(self.env) else 0.0
        self.active_floor = max(3.0 * self.noise_floor, 150.0)

    # ------------------ slicing helpers ------------------

    def _slice_env(self, start_s: float, end_s: float) -> np.ndarray:
        a = int(round(start_s * self.env_per_sec))
        b = int(round(end_s * self.env_per_sec))
        return self.env[a:b]

    def _slice_stft(self, start_s: float, end_s: float) -> np.ndarray:
        a = int(round(start_s * self.stft_per_sec))
        b = int(round(end_s * self.stft_per_sec))
        return self.S[:, a:b]

    def _slice_f0(self, start_s: float, end_s: float) -> tuple[np.ndarray, np.ndarray]:
        a = int(round(start_s * self.f0_per_sec))
        b = int(round(end_s * self.f0_per_sec))
        return self.f0[a:b], self.conf[a:b]

    def _slice_time(self, start_s: float, end_s: float) -> np.ndarray:
        a = int(round(start_s * self.sr))
        b = int(round(end_s * self.sr))
        return self.x[a:b]

    # ------------------ feature extractors ------------------

    def features_for_window(self, start_s: float, end_s: float) -> dict:
        env = self._slice_env(start_s, end_s)
        x = self._slice_time(start_s, end_s)
        S = self._slice_stft(start_s, end_s)
        f0, conf = self._slice_f0(start_s, end_s)

        row = {
            "start_s": round(start_s, 3),
            "end_s": round(end_s, 3),
            "dur_s": round(end_s - start_s, 3),
            **env_aggs(env, self.active_floor),
            "active_floor": round(self.active_floor, 2),
            "zcr": round(zcr(x), 4),
            "clip_frac": round(clip_ratio(x), 6),
            **spectral_aggs(self.f_bins, S),
            **f0_aggs(f0, conf),
        }
        # Round spectral fields + f0 consistently
        for k in (
            "centroid_mean_hz",
            "centroid_std_hz",
            "rolloff85_mean_hz",
            "rolloff95_mean_hz",
            "peak_hz_300_800",
            "peak_hz_800_1600",
            "f0_mean_hz",
            "f0_std_hz",
            "f0_p50_hz",
            "f0_p95_hz",
            "hnr_db",
        ):
            row[k] = round(row[k], 1 if "hz" in k or k == "hnr_db" else 4)
        for k in ("flatness_mean", "f0_voiced_frac", "rms_peak", "rms_mean", "rms_p50", "rms_p95", "noise_floor_p10", "active_frac"):
            row[k] = round(row[k], 4)
        return row

    def file_row(self) -> dict:
        row = self.features_for_window(0.0, self.dur)
        row = {
            "file": self.path.name,
            "sr": self.sr,
            "samples": self.n,
            **row,
        }
        return row

    def segment_rows(self) -> list[dict]:
        rows = []
        t = 0.0
        idx = 0
        while t + SEG_LEN_S <= self.dur + 1e-6:
            seg = self.features_for_window(t, t + SEG_LEN_S)
            rows.append({
                "file": self.path.name,
                "seg_idx": idx,
                **seg,
            })
            t += SEG_HOP_S
            idx += 1
        return rows


# ----------------------------------------------------------------------
# Triggers join (file-level)
# ----------------------------------------------------------------------


def load_triggers(path: Path) -> list[dict]:
    if not path or not path.exists():
        return []
    rows = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def parse_filename_ts(name: str) -> str | None:
    stem = name[:-4] if name.endswith(".wav") else name
    if not stem.startswith("cry-"):
        return None
    body = stem[4:]
    if "+" in body:
        date_time, tz = body.split("+", 1)
        tz_sign = "+"
    elif body[8:].count("-") == 1:
        date_time, tz = body.rsplit("-", 1)
        tz_sign = "-"
    else:
        return None
    if len(date_time) != 15 or date_time[8] != "T":
        return None
    d, t = date_time[:8], date_time[9:]
    return f"{d[:4]}-{d[4:6]}-{d[6:8]}T{t[:2]}:{t[2:4]}:{t[4:6]}{tz_sign}{tz[:2]}:{tz[2:]}"


def match_trigger(filename: str, trigger_rows: list[dict]) -> dict:
    if not trigger_rows:
        return {}
    iso = parse_filename_ts(filename)
    if not iso:
        return {}
    from datetime import datetime
    try:
        target = datetime.fromisoformat(iso)
    except ValueError:
        return {}
    best, best_delta = None, 1e9
    for r in trigger_rows:
        ts = r.get("ts")
        if not ts:
            continue
        try:
            t = datetime.fromisoformat(ts)
        except ValueError:
            continue
        if t.tzinfo is None and target.tzinfo is not None:
            t = t.replace(tzinfo=target.tzinfo)
        d = abs((t - target).total_seconds())
        if d < best_delta:
            best_delta = d
            best = r
    if best is None or best_delta > 60:
        return {}
    return {
        "trigger_note": best.get("note", ""),
        "trigger_rms": best.get("rms"),
        "trigger_cry_conf": best.get("cry_conf"),
        "trigger_ts": best.get("ts"),
        "trigger_match_delta_s": round(best_delta, 3),
    }


# ----------------------------------------------------------------------
# IO helpers
# ----------------------------------------------------------------------


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    # Stable column order: union of keys in observation order
    keys: list[str] = []
    seen: set[str] = set()
    for r in rows:
        for k in r:
            if k not in seen:
                seen.add(k)
                keys.append(k)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in keys})


def write_jsonl(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav-dir", required=True, type=Path)
    ap.add_argument("--triggers", type=Path, default=None)
    ap.add_argument("--out-csv", required=True, type=Path, help="file-level manifest CSV")
    ap.add_argument("--out-jsonl", type=Path, default=None, help="file-level manifest JSONL")
    ap.add_argument("--out-segments-csv", type=Path, default=None)
    ap.add_argument("--out-segments-jsonl", type=Path, default=None)
    args = ap.parse_args()

    wavs = sorted(args.wav_dir.glob("*.wav"))
    if not wavs:
        print(f"no wavs in {args.wav_dir}", file=sys.stderr)
        return 1

    trigger_rows = load_triggers(args.triggers) if args.triggers else []
    file_rows: list[dict] = []
    seg_rows: list[dict] = []

    for i, p in enumerate(wavs, 1):
        try:
            fa = FileAnalysis(p)
        except Exception as e:
            print(f"[{i:3}/{len(wavs)}] FAIL {p.name}: {e}", file=sys.stderr)
            continue
        tr = match_trigger(p.name, trigger_rows)
        file_row = {**fa.file_row(), **tr}
        file_rows.append(file_row)

        segs = fa.segment_rows()
        # Propagate trigger + file context onto every segment row for standalone use
        for s in segs:
            s.update({
                "file_dur_s": round(fa.dur, 3),
                "file_noise_floor": round(fa.noise_floor, 2),
                "file_active_floor": round(fa.active_floor, 2),
                **tr,
            })
        seg_rows.extend(segs)

        print(
            f"[{i:3}/{len(wavs)}] {p.name}  "
            f"file: peak={file_row['rms_peak']} f0p50={file_row['f0_p50_hz']} "
            f"voiced={file_row['f0_voiced_frac']} hnr={file_row['hnr_db']}dB  "
            f"segments={len(segs)}",
            file=sys.stderr,
        )

    write_csv(args.out_csv, file_rows)
    if args.out_jsonl:
        write_jsonl(args.out_jsonl, file_rows)
    if args.out_segments_csv:
        write_csv(args.out_segments_csv, seg_rows)
    if args.out_segments_jsonl:
        write_jsonl(args.out_segments_jsonl, seg_rows)

    print(
        f"\nwrote files={len(file_rows)} → {args.out_csv}, "
        f"segments={len(seg_rows)} → {args.out_segments_csv}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
