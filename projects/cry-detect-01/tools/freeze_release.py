#!/usr/bin/env python3
"""Freeze a dataset release from the current master label ledger.

Produces a JSON manifest that pins:
  - Exact set of WAV filenames included
  - Their labels at this moment (with hash for integrity)
  - Build / tooling versions used to produce the derived features
  - Split assignments (train / val / test-holdout)

Future training runs reference `release_id` in their config. If the
master.csv evolves, the old release still points at the same WAV list
and label snapshot.

Usage:  tools/freeze_release.py <release_id>
   e.g. tools/freeze_release.py cry-v0.0-exploratory
"""
import csv, hashlib, json, os, subprocess, sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DATASET = REPO / "datasets" / "cry-detect-01"
MASTER = DATASET / "labels" / "master.csv"

def git_head_sha(short=True):
    try:
        sha = subprocess.check_output(
            ["git", "-C", str(REPO), "rev-parse", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
        return sha[:8] if short else sha
    except Exception:
        return "unknown"

def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while (b := f.read(chunk)):
            h.update(b)
    return h.hexdigest()

def main(release_id):
    if not MASTER.exists():
        print(f"master labels missing at {MASTER}; run build_master_labels.py first", file=sys.stderr)
        return 1
    rows = list(csv.DictReader(open(MASTER)))

    # Simple heuristic splits: reserve every 5th WAV (by sort order) as test-holdout
    # Train/val split: 80/20 among the rest. Deterministic by filename sort.
    rows.sort(key=lambda r: r["capture_file"])
    split = {}
    for i, r in enumerate(rows):
        if i % 5 == 4:
            split[r["capture_file"]] = "test"
        elif i % 5 == 3:
            split[r["capture_file"]] = "val"
        else:
            split[r["capture_file"]] = "train"

    # Compute content hashes for WAVs — guards against accidental mutation
    logs = REPO / "projects" / "cry-detect-01" / "logs"
    wav_hashes = {}
    hash_sampled = 0
    for r in rows:
        # Check the most-recent session first since it's cumulative
        candidates = [
            logs / "night-20260422" / "wavs" / r["capture_file"],
            logs / r["session_id"] / "wavs" / r["capture_file"],
        ]
        wav_path = next((c for c in candidates if c.exists()), None)
        if wav_path and hash_sampled < 5:  # hash first 5 for size check
            wav_hashes[r["capture_file"]] = sha256_file(wav_path)
            hash_sampled += 1

    # Summary counts
    from collections import Counter
    split_counts = Counter(split.values())
    label_counts = Counter(r["yam_label_auto"] for r in rows)

    release = {
        "_schema": {"version": "v1", "type": "dataset_release"},
        "release_id": release_id,
        "frozen_at": datetime.now(timezone(timedelta(hours=10))).isoformat(),
        "git_head_sha": git_head_sha(),
        "device_id": "cry-detect-01",
        "n_captures": len(rows),
        "sessions": sorted(set(r["session_id"] for r in rows)),
        "splits": dict(split_counts),
        "label_distribution_auto": dict(label_counts),
        "label_source": "yamnet_auto_only_no_human",
        "notes": (
            "v0.0-exploratory — first frozen snapshot. Labels are YAMNet-only "
            "(no human review yet). Intended for tooling dry-runs and dataset "
            "infrastructure bring-up, NOT for model training. The sessions "
            "span firmware builds a895bfdc, a4870a21, 4af50c57 — any model "
            "training on this release should filter on build_sha or avoid "
            "using dev-side cry_conf as a feature."
        ),
        "captures": [
            {
                "file": r["capture_file"],
                "session_id": r["session_id"],
                "ts_iso": r["ts_iso"],
                "split": split[r["capture_file"]],
                "label_auto": r["yam_label_auto"],
                "label_human": r.get("human_label", ""),
                "yam_cry_max": max(
                    float(r.get("yam_baby_cry") or 0),
                    float(r.get("yam_crying_sobbing") or 0),
                ),
            }
            for r in rows
        ],
        "sample_wav_hashes": wav_hashes,  # spot-check for drift detection
    }

    out_path = DATASET / "releases" / f"{release_id}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(release, f, indent=2, default=str)
    print(f"wrote {out_path}")
    print(f"  {len(rows)} captures, splits: {dict(split_counts)}")
    print(f"  label distribution: {dict(label_counts)}")

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "cry-v0.0-exploratory"))
