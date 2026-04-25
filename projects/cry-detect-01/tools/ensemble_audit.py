#!/usr/bin/env python3
"""Ensemble auditor: produce per-capture label vectors without human review.

Removes manual annotation from the cry-detect-01 label-production path.
Each capture gets scored by four independent oracles whose agreement
forms a confidence tier:

  1. YAMNet wide-class (FP32 oracle):
       cry_pos = max across baby_cry, crying_sobbing, whimper, wail_moan,
                 screaming
       cry_neg = max across speech, child_speech, babbling, baby_laughter
       yam_cry_score = clip(cry_pos - 0.5 * cry_neg, 0, 1)
     Already computed in <session>/yamnet_files.csv.

  2. Acoustic-feature classifier (sklearn LogReg):
       Features: HNR, flatness, voiced_frac, b_250_500, b_1k_2k, centroid,
                 rolloff85, active_frac, onsets, zcr.
       Trained at audit time on pooled (cry, FP) samples from all
       sessions where YAMNet gives a confident verdict (yam>=0.5 → cry,
       yam<0.1 → FP). Validated cross-session in
       deep-analysis-20260423.md §Q2 at 92% accuracy / AUC 0.975.

  3. Sub-type cluster (k=4 KMeans):
       Categorical {fuss, full_cry, distress, screech} based on
       F0-contour + envelope + HNR features. Assignment is descriptive
       (not predictive) but reveals novel modes per
       deep-analysis-20260423.md §Q3.

  4. Temporal context:
       n high-confidence (yam_cry_score >= 0.5) captures within ±5 min,
       excluding self. Real cries come in clusters; isolated high-conf
       captures are slightly less reliable.

Combiner:
  oracle_agreement  = |yam_cry_score - feat_clf_prob|
  consensus_score   = mean(yam_cry_score, feat_clf_prob)
  confidence_tier:
    high_pos    : agreement < 0.2 AND consensus_score >= 0.7
    high_neg    : agreement < 0.2 AND consensus_score <= 0.1
    medium_pos  : consensus_score >= 0.4
    medium_neg  : consensus_score <  0.4 AND agreement < 0.3
    low         : oracles disagree (agreement >= 0.3)

Only `high_*` captures are recommended for v0.x model training. medium/low
stay in the dataset as evaluation/uncertainty samples.

Output: datasets/cry-detect-01/labels/master.csv with the full schema
documented at docs/research/data-vault-redesign-20260425.md.

Usage:
  tools/ensemble_audit.py                    # default sessions + output
  tools/ensemble_audit.py --dry-run          # don't write master.csv
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import sys
from collections import defaultdict
from pathlib import Path
from typing import Optional

import numpy as np
from sklearn.cluster import KMeans
from sklearn.linear_model import LogisticRegression
from sklearn.preprocessing import StandardScaler

REPO = Path(__file__).resolve().parents[3]
PROJECT = REPO / "projects" / "cry-detect-01"
DATASET = REPO / "datasets" / "cry-detect-01"
MASTER  = DATASET / "labels" / "master.csv"

ENSEMBLE_VERSION = "v0.1"
YAMNET_ORACLE_VERSION = "google/yamnet/1"

# YAMNet AudioSet class indices (matches main/metrics.c watched list)
YAM_POS_IDX = {"baby_cry_infant": 20, "crying_sobbing": 19,
               "whimper": 21, "wail_moan": 22, "screaming": 11}
YAM_NEG_IDX = {"speech": 0, "child_speech": 1,
               "babbling": 13, "baby_laughter": 14, "giggle": 15}

FEAT_CLF_FEATURES = [
    "hnr_db", "flatness_mean", "f0_voiced_frac",
    "b_250_500", "b_1k_2k",
    "centroid_mean_hz", "rolloff85_mean_hz",
    "active_frac", "onsets", "zcr",
]

CLUSTER_FEATURES = [
    "f0_voiced_frac", "f0_mean_hz", "f0_std_hz",
    "f0_p50_hz", "f0_p95_hz",
    "hnr_db", "b_500_1k", "b_1k_2k", "b_2k_4k",
    "centroid_mean_hz", "active_frac",
]

CLUSTER_LABELS = {
    # Mapping is established post-fit by inspecting centroid f0_mean.
    # See assign_cluster_labels() below — labels follow per-fit ordering.
}


def _f(x, d=None):
    if x is None or x == "":
        return d
    try:
        return float(x)
    except Exception:
        return d


def parse_wav_ts(name: str) -> Optional[dt.datetime]:
    """Filename → tz-aware datetime."""
    stem = name.removeprefix("cry-").removesuffix(".wav")
    try:
        return dt.datetime.strptime(stem, "%Y%m%dT%H%M%S%z")
    except Exception:
        return None


def load_all_sessions():
    """Walk logs/night-*/ and return {file: row_dict} with manifest +
    yamnet + session attribution."""
    rows = {}  # filename -> row
    sessions = sorted((PROJECT / "logs").glob("night-*"))
    print(f"[load] {len(sessions)} session dirs: {[s.name for s in sessions]}")
    for sess in sessions:
        man = sess / "manifest.csv"
        yam = sess / "yamnet_files.csv"
        if not (man.exists() and yam.exists()):
            print(f"  {sess.name}: missing manifest or yamnet, skip")
            continue
        manifest = {r["file"]: r for r in csv.DictReader(open(man))}
        yamscores = {r["file"]: r for r in csv.DictReader(open(yam))}
        for fn, m in manifest.items():
            if fn in rows:
                continue  # first session owns it (extracts are cumulative)
            ys = yamscores.get(fn, {})
            tdt = parse_wav_ts(fn)
            rows[fn] = {
                "session_id": sess.name,
                "capture_file": fn,
                "ts_iso": tdt.isoformat() if tdt else "",
                "ts_dt": tdt,
                "manifest": m,
                "yam": ys,
            }
    print(f"[load] {len(rows)} unique captures")
    return rows


def yam_score_from_row(r):
    """Extract yam_cry_score (max over cry-positive classes) plus the
    SEPARATE yam_speech_score for downstream mixed-audio filtering.

    Earlier draft used yam_cry_pos - 0.5*yam_speech as the score, but
    that conflated 'mixed audio with caregiver speech' (still a cry,
    valid training signal) with 'not a cry'. We now keep them separate."""
    ys = r["yam"]
    pos = max([_f(ys.get(f"max_yam_{k}"), 0) for k in YAM_POS_IDX] + [0])
    neg = max([_f(ys.get(f"max_yam_{k}"), 0) for k in YAM_NEG_IDX] + [0])
    yam_cry_score = max(0.0, min(1.0, pos))
    yam_speech_score = max(0.0, min(1.0, neg))
    return {
        "yam_baby_cry": _f(ys.get("max_yam_baby_cry_infant"), 0.0),
        "yam_crying_sobbing": _f(ys.get("max_yam_crying_sobbing"), 0.0),
        "yam_whimper": 0.0,  # not in our oracle CSV — placeholder
        "yam_speech": _f(ys.get("max_yam_speech"), 0.0),
        "yam_child_speech": _f(ys.get("max_yam_child_speech"), 0.0),
        "yam_screaming": _f(ys.get("max_yam_screaming"), 0.0),
        "yam_cry_pos_max": pos,
        "yam_cry_neg_max": neg,
        "yam_cry_score": yam_cry_score,
        "yam_speech_score": yam_speech_score,
        # purity = cry_score minus speech_score; <0 means speech-dominated.
        # Useful for downstream filtering of "mixed" captures.
        "yam_cry_purity": pos - neg,
    }


def feat_vec(r, features):
    m = r["manifest"]
    vals = []
    for k in features:
        v = _f(m.get(k))
        if v is None:
            return None
        vals.append(v)
    return np.array(vals, dtype=np.float32)


def train_feat_classifier(rows: dict):
    """Train logistic regression on pooled high-confidence captures
    (yam_cry_score >= 0.5 → cry, < 0.1 → FP). Returns (scaler, model,
    train_n)."""
    X, y = [], []
    for r in rows.values():
        ys = yam_score_from_row(r)
        if 0.1 <= ys["yam_cry_score"] < 0.5:
            continue
        v = feat_vec(r, FEAT_CLF_FEATURES)
        if v is None:
            continue
        X.append(v)
        y.append(1 if ys["yam_cry_score"] >= 0.5 else 0)
    X = np.stack(X); y = np.array(y)
    scaler = StandardScaler().fit(X)
    clf = LogisticRegression(max_iter=1000, class_weight="balanced", random_state=0)
    clf.fit(scaler.transform(X), y)
    train_acc = clf.score(scaler.transform(X), y)
    print(f"[feat_clf] trained on n={len(y)} ({y.sum()} cry / {len(y)-y.sum()} FP), train_acc={train_acc:.3f}")
    return scaler, clf, len(y)


def fit_subtype_clusters(rows: dict, k: int = 4):
    """Fit KMeans on the cry-confident subset; return (scaler, kmeans,
    label_map). label_map gives semantic names {0:'fuss', 1:'distress', ...}
    based on centroid f0_mean."""
    X, files = [], []
    for fn, r in rows.items():
        ys = yam_score_from_row(r)
        if ys["yam_cry_score"] < 0.5:
            continue
        v = feat_vec(r, CLUSTER_FEATURES)
        if v is None:
            continue
        X.append(v); files.append(fn)
    if len(X) < k * 2:
        print(f"[cluster] only {len(X)} cry samples, can't fit k={k}; skipping")
        return None, None, {}
    X = np.stack(X)
    scaler = StandardScaler().fit(X)
    km = KMeans(n_clusters=k, n_init=50, random_state=0).fit(scaler.transform(X))
    # Inverse-transform centroids to original feature units to label them
    inv = scaler.inverse_transform(km.cluster_centers_)
    f0_idx = CLUSTER_FEATURES.index("f0_mean_hz")
    voiced_idx = CLUSTER_FEATURES.index("f0_voiced_frac")
    f0s = inv[:, f0_idx]
    voiced = inv[:, voiced_idx]
    # Heuristic labels: lowest f0 with high voicing → fuss; highest f0 →
    # distress; very high f0 + high jitter → screech; moderate → full_cry.
    # Approximation: sort by f0 ascending → fuss, full_cry, distress, screech.
    order = np.argsort(f0s)
    label_map = {}
    if k == 4:
        names = ["fuss", "full_cry", "distress", "screech"]
        for rank, c in enumerate(order):
            label_map[int(c)] = names[rank]
    else:
        for c in range(k):
            label_map[int(c)] = f"cluster_{c}"
    print(f"[cluster] k={k} fit on {len(X)} cries; centroids f0_mean = {[f'{f:.0f}' for f in f0s[order]]}")
    print(f"[cluster] label_map = {label_map}")
    return scaler, km, label_map


def temporal_context(rows: dict, window_s: int = 300):
    """Per capture: count of OTHER captures within ±window_s, and how
    many of those are yam_cry_score >= 0.5."""
    timeline = sorted(
        [(r["ts_dt"], fn, yam_score_from_row(r)["yam_cry_score"])
         for fn, r in rows.items() if r["ts_dt"] is not None],
        key=lambda x: x[0],
    )
    out = {}
    for i, (t, fn, _) in enumerate(timeline):
        n_total = 0
        n_high = 0
        # left scan
        j = i - 1
        while j >= 0 and (t - timeline[j][0]).total_seconds() <= window_s:
            n_total += 1
            if timeline[j][2] >= 0.5: n_high += 1
            j -= 1
        # right scan
        j = i + 1
        while j < len(timeline) and (timeline[j][0] - t).total_seconds() <= window_s:
            n_total += 1
            if timeline[j][2] >= 0.5: n_high += 1
            j += 1
        out[fn] = (n_total, n_high)
    return out


import re
import urllib.parse


def parse_human_note(note: str) -> tuple[str, str]:
    """Parse a trigger_note into (human_label, raw_decoded_text).

    Returns:
      ("cry", text)      if note semantically asserts a cry
      ("not_cry", text)  if note semantically asserts not-cry / quiet
      ("test", text)     if note is a bench/test marker
      ("auto", note)     if note is the auto-rms-* label (no human input)
      ("", "")           if note is empty / unrecognized

    Notes are SUPPLEMENTARY. The ensemble label is authoritative; this
    parser exists to compute disagreement metrics, not to override.
    """
    if not note: return ("", "")
    if note.startswith("auto-rms-"): return ("auto", note)
    text = urllib.parse.unquote(note).strip()
    low = text.lower()
    # Cry-positive patterns
    if low == "cry" or re.match(r"^cry\d+$", low) or "cry " in low or low.endswith(" cry") or "cry%20" in low.lower() or low.startswith("cry "):
        return ("cry", text)
    # Cry-negative / quiet-state
    if "quiet" in low: return ("not_cry", text)
    # Bench / test triggers
    if low in {"capacity-test", "verify-round-2", "test-get-alias"}: return ("test", text)
    # Pure environment annotations don't claim cry/not-cry
    if low in {"living-room", "bedroom"}: return ("", text)
    return ("", text)


def confidence_tier(yam: float, feat: float) -> str:
    agreement = abs(yam - feat)
    consensus = 0.5 * (yam + feat)
    if agreement < 0.2 and consensus >= 0.7: return "high_pos"
    if agreement < 0.2 and consensus <= 0.1: return "high_neg"
    if consensus >= 0.4: return "medium_pos"
    if agreement < 0.3 and consensus < 0.4: return "medium_neg"
    return "low"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    rows = load_all_sessions()
    feat_scaler, feat_clf, feat_clf_n = train_feat_classifier(rows)
    cl_scaler, kmeans, cluster_label_map = fit_subtype_clusters(rows, k=4)
    temporal = temporal_context(rows, window_s=300)

    # Triggers index for capture-time on-device readings
    trigs_by_session = {}
    for sess in sorted((PROJECT / "logs").glob("night-*")):
        path = sess / "triggers.jsonl"
        if not path.exists(): continue
        idx = {}
        for line in open(path):
            line = line.strip()
            if not line: continue
            try:
                d = json.loads(line)
                idx[d["ts"][:19]] = d
            except Exception: continue
        trigs_by_session[sess.name] = idx

    out_rows = []
    audited_at = dt.datetime.now(dt.timezone(dt.timedelta(hours=10))).isoformat()
    for fn, r in rows.items():
        ys = yam_score_from_row(r)

        # Feature classifier
        v = feat_vec(r, FEAT_CLF_FEATURES)
        if v is not None:
            feat_clf_prob = float(feat_clf.predict_proba(feat_scaler.transform(v[None]))[0, 1])
        else:
            feat_clf_prob = None

        # Cluster (only meaningful for cry-confident captures)
        cluster_id = None; cluster_label = None
        if ys["yam_cry_score"] >= 0.5 and kmeans is not None:
            cv = feat_vec(r, CLUSTER_FEATURES)
            if cv is not None:
                cluster_id = int(kmeans.predict(cl_scaler.transform(cv[None]))[0])
                cluster_label = cluster_label_map.get(cluster_id)

        # Temporal
        n_total, n_high = temporal.get(fn, (0, 0))

        # Combiner
        if feat_clf_prob is None:
            tier = "low"  # missing oracle = low confidence by default
            agreement = None; consensus = None
        else:
            agreement = abs(ys["yam_cry_score"] - feat_clf_prob)
            consensus = 0.5 * (ys["yam_cry_score"] + feat_clf_prob)
            tier = confidence_tier(ys["yam_cry_score"], feat_clf_prob)

        # Trigger reading
        trig = trigs_by_session.get(r["session_id"], {}).get((r["ts_iso"] or "")[:19])
        m = r["manifest"]

        # Human-note (supplementary). Parser maps free-text notes into
        # cry/not_cry/test/auto/empty. We compare to the ensemble verdict
        # but DO NOT override — humans here are noisy, often misaligned.
        raw_note = (trig or {}).get("note", "") or ""
        human_label, human_text = parse_human_note(raw_note)
        ensemble_says_cry = (tier in ("high_pos", "medium_pos")) if feat_clf_prob is not None else None
        if human_label in ("cry", "not_cry") and ensemble_says_cry is not None:
            human_says_cry = human_label == "cry"
            human_note_agrees = (human_says_cry == ensemble_says_cry)
        else:
            human_note_agrees = None  # no claim or no ensemble verdict

        out_rows.append({
            "session_id": r["session_id"],
            "capture_file": fn,
            "ts_iso": r["ts_iso"],

            # YAMNet primary
            "yam_baby_cry": f"{ys['yam_baby_cry']:.4f}",
            "yam_crying_sobbing": f"{ys['yam_crying_sobbing']:.4f}",
            "yam_speech": f"{ys['yam_speech']:.4f}",
            "yam_child_speech": f"{ys['yam_child_speech']:.4f}",
            "yam_screaming": f"{ys['yam_screaming']:.4f}",
            "yam_cry_pos_max": f"{ys['yam_cry_pos_max']:.4f}",
            "yam_cry_neg_max": f"{ys['yam_cry_neg_max']:.4f}",
            "yam_cry_score": f"{ys['yam_cry_score']:.4f}",
            "yam_speech_score": f"{ys['yam_speech_score']:.4f}",
            "yam_cry_purity": f"{ys['yam_cry_purity']:.4f}",

            # Feature classifier
            "feat_clf_prob": f"{feat_clf_prob:.4f}" if feat_clf_prob is not None else "",

            # Cluster
            "cluster_id": cluster_id if cluster_id is not None else "",
            "cluster_label": cluster_label or "",

            # Temporal
            "temporal_neighbors_5min": n_total,
            "temporal_high_conf_neighbors_5min": n_high,

            # Combiner
            "oracle_agreement": f"{agreement:.4f}" if agreement is not None else "",
            "consensus_score": f"{consensus:.4f}" if consensus is not None else "",
            "confidence_tier": tier,

            # Raw features for debugging / future retraining
            "hnr_db": m.get("hnr_db", ""),
            "f0_mean_hz": m.get("f0_mean_hz", ""),
            "f0_voiced_frac": m.get("f0_voiced_frac", ""),
            "rms_peak": m.get("rms_peak", ""),
            "duration_s": m.get("dur_s", ""),
            "centroid_mean_hz": m.get("centroid_mean_hz", ""),
            "flatness_mean": m.get("flatness_mean", ""),

            # Capture-time on-device reading
            "trigger_note": raw_note,
            "trigger_rms": (trig or {}).get("rms", ""),
            "dev_cry_conf_at_capture": (trig or {}).get("cry_conf", ""),

            # Human-note (supplementary): parsed semantic + raw text +
            # whether it agrees with the ensemble verdict (for monitoring,
            # NOT for overriding labels).
            "human_note_label": human_label,
            "human_note_text": human_text,
            "human_note_agrees": "" if human_note_agrees is None else str(human_note_agrees).lower(),

            # Provenance
            "yamnet_oracle_version": YAMNET_ORACLE_VERSION,
            "ensemble_version": ENSEMBLE_VERSION,
            "audited_at": audited_at,
        })

    # Tier histogram
    tiers = defaultdict(int)
    for r in out_rows:
        tiers[r["confidence_tier"]] += 1
    print(f"\n[ensemble] confidence-tier distribution:")
    for t, n in sorted(tiers.items()):
        print(f"  {t:<12} {n}")

    # Cluster histogram (cry-only)
    clusters = defaultdict(int)
    for r in out_rows:
        if r["cluster_label"]:
            clusters[r["cluster_label"]] += 1
    print(f"\n[ensemble] cluster distribution (yam_cry_score>=0.5 only):")
    for c, n in sorted(clusters.items()):
        print(f"  {c:<12} {n}")

    # Human-note vs ensemble agreement
    h_label = defaultdict(int); h_agree = defaultdict(int)
    for r in out_rows:
        if r["human_note_label"]:
            h_label[r["human_note_label"]] += 1
        h_agree[r["human_note_agrees"]] += 1
    print(f"\n[ensemble] human-note (supplementary, n={sum(h_label.values())} non-empty):")
    for k, n in sorted(h_label.items()):
        print(f"  {k:<10} {n}")
    print(f"  agreement w/ ensemble: {dict(h_agree)}")

    if args.dry_run:
        print("\n[dry-run] not writing master.csv")
        return 0

    MASTER.parent.mkdir(parents=True, exist_ok=True)
    fields = list(out_rows[0].keys())
    with open(MASTER, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(out_rows)
    print(f"\nwrote {MASTER} ({len(out_rows)} rows, {len(fields)} columns)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
