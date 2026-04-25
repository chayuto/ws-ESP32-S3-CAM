#!/usr/bin/env python3
"""Freeze a dataset release from the current ensemble label ledger.

Schema v2 (2026-04-25): replaces single categorical label with the
full ensemble vector. Releases pin every score so a future training
run can reproduce a slice without depending on the current state of
master.csv.

What's in a release:
  - Exact set of WAV filenames + per-capture label vector at this moment
  - Splits (train / val / test, deterministic by filename sort)
  - Provenance: git_head_sha, ensemble_version, yamnet_oracle_version,
    feat_clf retrain timestamp (== audited_at)
  - Aggregate stats (tier histogram, cluster histogram, human-note
    agreement counts) for sanity check

Usage:  tools/freeze_release.py <release_id>
   e.g. tools/freeze_release.py cry-v0.1-ensemble
"""
import csv, hashlib, json, os, subprocess, sys
from collections import Counter, defaultdict
from datetime import datetime, timezone, timedelta
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DATASET = REPO / "datasets" / "cry-detect-01"
MASTER = DATASET / "labels" / "master.csv"

RELEASE_SCHEMA_VERSION = "v2"


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


def split_for(i):
    """Deterministic train/val/test by row index."""
    if i % 5 == 4: return "test"
    if i % 5 == 3: return "val"
    return "train"


def f(s):
    try: return float(s) if s else None
    except Exception: return None


def main(release_id):
    if not MASTER.exists():
        print(f"master labels missing at {MASTER}; run ensemble_audit.py first", file=sys.stderr)
        return 1
    rows = list(csv.DictReader(open(MASTER)))
    rows.sort(key=lambda r: r["capture_file"])

    # Sample a few WAV hashes for drift detection
    logs = REPO / "projects" / "cry-detect-01" / "logs"
    wav_hashes = {}
    for r in rows[:5]:
        for sess in (r["session_id"], "night-20260424", "night-20260422", "night-20260421", "night-20260420"):
            wp = logs / sess / "wavs" / r["capture_file"]
            if wp.exists():
                wav_hashes[r["capture_file"]] = sha256_file(wp)
                break

    # Aggregates for the release manifest header
    splits = Counter()
    tiers = Counter(); clusters = Counter()
    human_label_counts = Counter(); human_agree_counts = Counter()
    for i, r in enumerate(rows):
        splits[split_for(i)] += 1
        tiers[r["confidence_tier"]] += 1
        if r.get("cluster_label"): clusters[r["cluster_label"]] += 1
        if r.get("human_note_label"): human_label_counts[r["human_note_label"]] += 1
        if r.get("human_note_agrees"): human_agree_counts[r["human_note_agrees"]] += 1

    # Per-capture record (slim) — pins the score vector and split
    captures = []
    for i, r in enumerate(rows):
        captures.append({
            "file": r["capture_file"],
            "session_id": r["session_id"],
            "ts_iso": r["ts_iso"],
            "split": split_for(i),
            # Ensemble label vector (authoritative)
            "yam_cry_score": f(r["yam_cry_score"]),
            "yam_speech_score": f(r["yam_speech_score"]),
            "yam_cry_purity": f(r["yam_cry_purity"]),
            "feat_clf_prob": f(r["feat_clf_prob"]),
            "consensus_score": f(r["consensus_score"]),
            "oracle_agreement": f(r["oracle_agreement"]),
            "confidence_tier": r["confidence_tier"],
            "cluster_label": r["cluster_label"] or None,
            "temporal_high_conf_neighbors_5min": int(r["temporal_high_conf_neighbors_5min"] or 0),
            # Human note (supplementary)
            "human_note_label": r["human_note_label"] or None,
            "human_note_agrees": r["human_note_agrees"] or None,
        })

    # Recommended training subset: high_pos OR high_neg only (oracles agree
    # strongly). Rest stay in dataset for evaluation/inspection.
    train_eligible = sum(1 for c in captures if c["confidence_tier"] in ("high_pos", "high_neg"))

    audited_at = rows[0].get("audited_at") if rows else ""
    ensemble_version = rows[0].get("ensemble_version") if rows else ""
    yamnet_oracle_version = rows[0].get("yamnet_oracle_version") if rows else ""

    release = {
        "_schema": {"version": RELEASE_SCHEMA_VERSION, "type": "dataset_release"},
        "release_id": release_id,
        "frozen_at": datetime.now(timezone(timedelta(hours=10))).isoformat(),
        "git_head_sha": git_head_sha(),
        "device_id": "cry-detect-01",
        "n_captures": len(rows),
        "sessions": sorted(set(r["session_id"] for r in rows)),
        "splits": dict(splits),
        "confidence_tier_distribution": dict(tiers),
        "cluster_distribution": dict(clusters),
        "human_note_label_distribution": dict(human_label_counts),
        "human_note_agreement_distribution": dict(human_agree_counts),
        "train_eligible_n": train_eligible,
        "label_source": "auto_ensemble_no_human_in_label_loop",
        "ensemble": {
            "version": ensemble_version,
            "yamnet_oracle_version": yamnet_oracle_version,
            "feat_clf": "sklearn LogisticRegression on 10 acoustic features, retrained at audit time",
            "subtype_cluster": "k=4 KMeans on F0/HNR/band features, fit on yam_cry_score>=0.5",
            "temporal_window_s": 300,
            "audited_at": audited_at,
        },
        "training_recommendation": (
            "Use captures with confidence_tier in {high_pos, high_neg} for "
            "model training (oracles agree). medium_* and low captures stay "
            "in the dataset for evaluation, uncertainty study, and oracle-"
            "disagreement investigation. human_note fields are SUPPLEMENTARY "
            "monitoring data — never override the ensemble verdict."
        ),
        "captures": captures,
        "sample_wav_hashes": wav_hashes,
    }

    out_path = DATASET / "releases" / f"{release_id}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f_:
        json.dump(release, f_, indent=2, default=str)
    print(f"wrote {out_path}")
    print(f"  {len(rows)} captures   splits={dict(splits)}")
    print(f"  tiers={dict(tiers)}")
    print(f"  train_eligible (high_pos+high_neg) = {train_eligible}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "cry-v0.1-ensemble"))
