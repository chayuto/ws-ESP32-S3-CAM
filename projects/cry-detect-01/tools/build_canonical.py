#!/usr/bin/env python3
"""Merge logs/night-*/ snapshots into a single canonical local mirror.

For each filename that appears across multiple session snapshots, picks the
largest copy — append-only formats (infer-*.jsonl, cry-*.log, triggers.jsonl)
mean bigger == more complete. WAVs are immutable once closed; the copy in
any session is byte-identical, so we just pick one.

Output:
  logs/canonical/
  ├── manifest.json                ← {filename: {size, sha256, source_session}}
  ├── infer-YYYYMMDD.jsonl         ← largest copy across sessions (hardlinked)
  ├── cry-YYYYMMDD.log             ← same
  ├── triggers.jsonl               ← largest
  ├── infer-boot.jsonl             ← latest snapshot's copy
  ├── CRY-NNNN.LOG                 ← latest
  └── wavs/
      └── *.wav                    ← union (hardlinks)

Idempotent — safe to rerun. Non-destructive — never modifies sources.
Uses hardlinks where the filesystem supports them, falls back to copy.

Usage:
  tools/build_canonical.py                  # rebuild canonical/
  tools/build_canonical.py --check          # re-hash sources, verify manifest
  tools/build_canonical.py --logs-dir DIR   # alt logs dir (default: <repo>/projects/cry-detect-01/logs)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DEFAULT_LOGS = REPO / "projects" / "cry-detect-01" / "logs"

# Files we expect inside each session's device-logs/. Ordered by category.
ROOT_LOG_PATTERNS = [
    re.compile(r"^infer-\d{8}\.jsonl$"),
    re.compile(r"^cry-\d{8}\.log$"),
    re.compile(r"^infer-boot\.jsonl$"),
    re.compile(r"^CRY-\d{4}\.LOG$"),
]

def is_root_log(name: str) -> bool:
    return any(p.match(name) for p in ROOT_LOG_PATTERNS)


def sha256_of(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def link_or_copy(src: Path, dst: Path) -> str:
    """Hardlink src -> dst if same filesystem; otherwise copy. Returns 'link' or 'copy'."""
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    try:
        os.link(src, dst)
        return "link"
    except OSError:
        shutil.copy2(src, dst)
        return "copy"


def collect_sources(logs_dir: Path):
    """Walk all night-*/ session dirs and gather candidates per filename.

    Returns:
      root_logs: dict[name, list[(session_name, Path, size)]]   sorted by size desc
      wavs:      dict[name, list[(session_name, Path, size)]]
      triggers:  list[(session_name, Path, size)]               sorted by size desc
    """
    root_logs: dict[str, list] = {}
    wavs: dict[str, list] = {}
    triggers: list = []

    sessions = sorted([p for p in logs_dir.iterdir()
                       if p.is_dir() and p.name.startswith("night-")])

    for sess in sessions:
        # device-logs
        dl = sess / "device-logs"
        if dl.is_dir():
            for f in dl.iterdir():
                if not f.is_file():
                    continue
                if not is_root_log(f.name):
                    continue
                root_logs.setdefault(f.name, []).append((sess.name, f, f.stat().st_size))
        # wavs
        wd = sess / "wavs"
        if wd.is_dir():
            for f in wd.iterdir():
                if f.is_file() and f.name.endswith(".wav"):
                    wavs.setdefault(f.name, []).append((sess.name, f, f.stat().st_size))
        # triggers.jsonl
        t = sess / "triggers.jsonl"
        if t.is_file():
            triggers.append((sess.name, t, t.stat().st_size))

    # Sort each filename's candidates by size desc (largest first).
    for d in (root_logs, wavs):
        for n in d:
            d[n].sort(key=lambda x: x[2], reverse=True)
    triggers.sort(key=lambda x: x[2], reverse=True)

    return sessions, root_logs, wavs, triggers


def cmd_build(args):
    logs_dir = Path(args.logs_dir).resolve()
    canon = logs_dir / "canonical"
    canon_wavs = canon / "wavs"
    canon.mkdir(parents=True, exist_ok=True)
    canon_wavs.mkdir(parents=True, exist_ok=True)

    sessions, root_logs, wavs, triggers = collect_sources(logs_dir)
    if not sessions:
        sys.stderr.write(f"no night-*/ sessions in {logs_dir}\n")
        return 2

    manifest = {
        "schema_version": 1,
        "logs_dir": str(logs_dir),
        "sessions_seen": [s.name for s in sessions],
        "files": {},  # filename -> {size, sha256, source_session, kind, link_mode}
    }

    n_link = n_copy = n_skipped = 0

    def place(name: str, candidates: list, kind: str, dst_parent: Path):
        nonlocal n_link, n_copy, n_skipped
        if not candidates:
            return
        sess, src, size = candidates[0]
        dst = dst_parent / name

        # Skip if dst already correct
        if dst.exists() and dst.stat().st_size == size:
            try:
                if dst.samefile(src):
                    n_skipped += 1
                    digest = sha256_of(dst)
                    manifest["files"][name] = {
                        "kind": kind, "size": size, "sha256": digest,
                        "source_session": sess, "link_mode": "preexisting",
                    }
                    return
            except FileNotFoundError:
                pass

        mode = link_or_copy(src, dst)
        if mode == "link":
            n_link += 1
        else:
            n_copy += 1

        digest = sha256_of(dst)
        manifest["files"][name] = {
            "kind": kind, "size": size, "sha256": digest,
            "source_session": sess, "link_mode": mode,
        }

    # Triggers
    if triggers:
        sess, src, size = triggers[0]
        dst = canon / "triggers.jsonl"
        if dst.exists() and dst.stat().st_size == size and dst.samefile(src):
            n_skipped += 1
        else:
            mode = link_or_copy(src, dst)
            n_link += (mode == "link"); n_copy += (mode == "copy")
        digest = sha256_of(dst)
        manifest["files"]["triggers.jsonl"] = {
            "kind": "triggers", "size": size, "sha256": digest,
            "source_session": sess,
        }

    # Root logs
    for name, cands in sorted(root_logs.items()):
        place(name, cands, kind="root_log", dst_parent=canon)

    # WAVs
    for name, cands in sorted(wavs.items()):
        place(name, cands, kind="wav", dst_parent=canon_wavs)

    # Write manifest
    (canon / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True))

    print(f"canonical:           {canon}")
    print(f"sessions seen:       {len(sessions)} ({', '.join(s.name for s in sessions)})")
    print(f"root logs:           {len(root_logs)}")
    print(f"wavs:                {len(wavs)}")
    print(f"triggers.jsonl:      {'yes' if triggers else 'no'}")
    print(f"linked / copied:     {n_link} / {n_copy}  (skipped {n_skipped})")
    total_size = sum(v["size"] for v in manifest["files"].values())
    print(f"total canonical:     {total_size/1e9:.2f} GB across {len(manifest['files'])} files")
    return 0


def cmd_check(args):
    logs_dir = Path(args.logs_dir).resolve()
    canon = logs_dir / "canonical"
    mfn = canon / "manifest.json"
    if not mfn.exists():
        sys.stderr.write(f"no manifest at {mfn}; run without --check first\n")
        return 2
    manifest = json.loads(mfn.read_text())
    bad = 0
    checked = 0
    for name, info in sorted(manifest["files"].items()):
        if info["kind"] == "wav":
            p = canon / "wavs" / name
        else:
            p = canon / name
        if not p.exists():
            print(f"MISSING  {name}")
            bad += 1
            continue
        if p.stat().st_size != info["size"]:
            print(f"SIZE     {name}  manifest={info['size']}  on_disk={p.stat().st_size}")
            bad += 1
            continue
        h = sha256_of(p)
        if h != info["sha256"]:
            print(f"HASH     {name}  manifest={info['sha256'][:12]}  on_disk={h[:12]}")
            bad += 1
        checked += 1
    print(f"checked {checked} files, {bad} bad")
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--logs-dir", default=str(DEFAULT_LOGS),
                    help=f"sessions parent dir (default: {DEFAULT_LOGS})")
    ap.add_argument("--check", action="store_true",
                    help="re-hash canonical files against manifest.json")
    args = ap.parse_args()
    if args.check:
        return cmd_check(args)
    return cmd_build(args)


if __name__ == "__main__":
    raise SystemExit(main())
