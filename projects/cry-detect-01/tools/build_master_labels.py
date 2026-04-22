#!/usr/bin/env python3
"""Build the master label ledger from per-session YAMNet oracle + manifest.

Walks all sessions under logs/night-*/, joins each WAV's YAMNet scores
with its manifest features and trigger metadata. Writes a single CSV
ledger at datasets/cry-detect-01/labels/master.csv.

Columns designed to survive future schema additions (everything auto-
labelled, no human labels yet — those live in labels/human.csv when
an annotation tool is built).

Usage:  tools/build_master_labels.py
"""
import csv, glob, os, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PROJECT = REPO / "projects" / "cry-detect-01"
DATASET = REPO / "datasets" / "cry-detect-01"
OUT = DATASET / "labels" / "master.csv"

def classify_yam(ys):
    """Auto-label based on YAMNet scores."""
    try:
        b = float(ys.get("max_yam_baby_cry_infant", 0) or 0)
        c = float(ys.get("max_yam_crying_sobbing", 0) or 0)
        sp = float(ys.get("max_yam_speech", 0) or 0)
    except Exception:
        return "unknown"
    y = max(b, c)
    if y >= 0.5: return "cry"
    if y >= 0.3: return "borderline_cry"
    if sp >= 0.5: return "speech"
    if y < 0.1 and sp < 0.1: return "other"
    return "mixed"

sessions = sorted((PROJECT / "logs").glob("night-*"))
print(f"Found {len(sessions)} session dirs: {[s.name for s in sessions]}")

rows = []
seen_files = {}
for sess in sessions:
    man = sess / "manifest.csv"
    yam = sess / "yamnet_files.csv"
    trig = sess / "triggers.jsonl"
    if not (man.exists() and yam.exists()):
        print(f"  {sess.name}: missing manifest or yamnet, skip")
        continue
    # Index yamnet + triggers by file
    yams = {r["file"]: r for r in csv.DictReader(open(man))}
    yamscores = {r["file"]: r for r in csv.DictReader(open(yam))}
    trig_idx = {}
    if trig.exists():
        import json
        for line in open(trig):
            line = line.strip()
            if not line: continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            trig_idx[d["ts"][:19]] = d  # index by seconds
    sess_name = sess.name
    for fn, m in yams.items():
        if fn in seen_files:
            continue  # first session owns it
        ys = yamscores.get(fn, {})
        # wav ts from filename: cry-YYYYMMDDTHHMMSS+TZ.wav
        stem = fn.removeprefix("cry-").removesuffix(".wav")
        # e.g. "20260422T185332+1000"
        ts_part = stem.replace("+", " +").replace("-", "-")
        try:
            # Convert to canonical ISO: 2026-04-22T18:53:32+10:00
            y, mo, d_, rest = stem[:4], stem[4:6], stem[6:8], stem[9:]
            h, mi, s = rest[:2], rest[2:4], rest[4:6]
            tz = rest[6:]  # +1000
            iso = f"{y}-{mo}-{d_}T{h}:{mi}:{s}{tz[:3]}:{tz[3:]}"
        except Exception:
            iso = ""
        # trigger lookup (tolerate ms mismatch)
        t = None
        iso_sec = iso[:19] if iso else ""
        for k, v in trig_idx.items():
            if k.startswith(iso_sec) or iso_sec.startswith(k[:19]):
                t = v; break
        row = {
            "session_id": sess_name,
            "capture_file": fn,
            "ts_iso": iso,
            "yam_baby_cry": ys.get("max_yam_baby_cry_infant", ""),
            "yam_crying_sobbing": ys.get("max_yam_crying_sobbing", ""),
            "yam_speech": ys.get("max_yam_speech", ""),
            "yam_child_speech": ys.get("max_yam_child_speech", ""),
            "yam_screaming": ys.get("max_yam_screaming", ""),
            "yam_label_auto": classify_yam(ys),
            "hnr_db": m.get("hnr_db", ""),
            "rms_peak": m.get("rms_peak", ""),
            "f0_mean_hz": m.get("f0_mean_hz", ""),
            "f0_voiced_frac": m.get("f0_voiced_frac", ""),
            "centroid_mean_hz": m.get("centroid_mean_hz", ""),
            "flatness_mean": m.get("flatness_mean", ""),
            "duration_s": m.get("dur_s", ""),
            "trigger_note": (t or {}).get("note", ""),
            "trigger_rms": (t or {}).get("rms", ""),
            "dev_cry_conf_at_capture": (t or {}).get("cry_conf", ""),
            # human label deliberately empty — to be filled by annotation tool later
            "human_label": "",
            "human_label_ts": "",
        }
        seen_files[fn] = sess_name
        rows.append(row)


OUT.parent.mkdir(parents=True, exist_ok=True)
FIELDS = list(rows[0].keys())
with open(OUT, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=FIELDS)
    w.writeheader()
    w.writerows(rows)

# Summary
from collections import Counter
labels = Counter(r["yam_label_auto"] for r in rows)
print(f"\nWrote {len(rows)} rows → {OUT}")
print(f"Auto-label distribution:")
for k, n in labels.most_common():
    print(f"  {k:<20} {n}")
