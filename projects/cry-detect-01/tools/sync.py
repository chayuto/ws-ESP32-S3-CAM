#!/usr/bin/env python3
"""Phase B sync client. Manifest-driven, resumable, idempotent.

Replaces extract_session.sh. Pulls files the device hasn't seen acked yet,
verifies sha256, and posts a batched ack so the device can mark them
synced. Re-running is a no-op when nothing has changed.

Mirror layout (all under --mirror):

    .sync/
        state.json          # {last_manifest_mtime, generation_id, host_id}
        pending_acks.jsonl  # files fetched but not yet acked (recovery)
        checksums.idx       # local sha256 cache (path -> sha)
    wavs/cry-*.wav
    infer/infer-YYYYMMDDTHH.jsonl
    markers/.session-started-*.json
    misc/                   # any unrecognized category

Subcommands:
    init    create the mirror dir + .sync/ scaffolding
    once    run a single sync pass and exit
    loop    daemon mode (sync pass every --interval seconds)
    verify  re-hash every local file and compare against device manifest
    dry-run fetch manifest, print plan, exit without pulling

Stdlib only (urllib, hashlib, json). No external deps.

Examples:
    sync.py --host http://192.168.1.100 --mirror ~/cry-mirror init
    sync.py --host http://192.168.1.100 --mirror ~/cry-mirror once
    sync.py --host http://192.168.1.100 --mirror ~/cry-mirror loop --interval 900
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Optional

# ----------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------

DEFAULT_HOST = "http://192.168.1.100"
MANIFEST_PAGE_LIMIT = 500           # max entries per /manifest.json call
ACK_BATCH_SIZE = 100                # max paths per /sync/ack POST
HTTP_TIMEOUT_SHORT = 10
HTTP_TIMEOUT_LONG = 300             # for /files/get
CATEGORY_TO_SUBDIR = {
    "wav": "wavs",
    "infer_log": "infer",
    "session_marker": "markers",
}
DEFAULT_SUBDIR = "misc"


# ----------------------------------------------------------------------
# Logging — stdout, prefixed, no external deps
# ----------------------------------------------------------------------

def log(level: str, msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {level:5s} {msg}", flush=True)


def info(msg: str)  -> None: log("INFO",  msg)
def warn(msg: str)  -> None: log("WARN",  msg)
def err(msg: str)   -> None: log("ERROR", msg)


# ----------------------------------------------------------------------
# HTTP helpers
# ----------------------------------------------------------------------

def http_get_json(url: str, timeout: int = HTTP_TIMEOUT_SHORT) -> dict:
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def http_post_json(url: str, body: dict, timeout: int = HTTP_TIMEOUT_SHORT) -> dict:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def http_get_range(url: str, start: int, dest_path: Path,
                   timeout: int = HTTP_TIMEOUT_LONG) -> int:
    """Fetch from `start` byte to EOF, appending to dest_path. Returns bytes
    written. Uses Range: bytes=N- and expects 206 Partial Content (or 200
    if the device doesn't support Range — falls back gracefully)."""
    headers = {"Range": f"bytes={start}-"} if start > 0 else {}
    req = urllib.request.Request(url, headers=headers, method="GET")
    written = 0
    with urllib.request.urlopen(req, timeout=timeout) as r:
        # If start > 0 but device responded 200 (no Range support), reset.
        if start > 0 and r.status == 200:
            warn(f"  device returned 200 for Range request; restarting from 0")
            dest_path.unlink(missing_ok=True)
            start = 0
        mode = "ab" if start > 0 else "wb"
        with dest_path.open(mode) as f:
            while True:
                chunk = r.read(64 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                written += len(chunk)
    return written


# ----------------------------------------------------------------------
# Mirror layout helpers
# ----------------------------------------------------------------------

def device_path_to_local(device_path: str, category: str, mirror: Path) -> Path:
    """Map a device path like /sdcard/events/cry-XXX.wav to the local mirror."""
    name = Path(device_path).name
    subdir = CATEGORY_TO_SUBDIR.get(category, DEFAULT_SUBDIR)
    return mirror / subdir / name


def sha256_file(path: Path, buf_size: int = 1 << 16) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            data = f.read(buf_size)
            if not data:
                break
            h.update(data)
    return h.hexdigest()


# ----------------------------------------------------------------------
# State
# ----------------------------------------------------------------------

class State:
    """Persisted sync state. Survives host laptop reboot."""

    def __init__(self, mirror: Path):
        self.mirror = mirror
        self.sync_dir = mirror / ".sync"
        self.state_path = self.sync_dir / "state.json"
        self.acks_path  = self.sync_dir / "pending_acks.jsonl"
        self.cksum_path = self.sync_dir / "checksums.idx"
        self.last_manifest_mtime: int = 0
        self.generation_id: str = ""
        self.host_id: str = ""

    def load(self) -> None:
        if not self.state_path.exists():
            return
        d = json.loads(self.state_path.read_text())
        self.last_manifest_mtime = int(d.get("last_manifest_mtime", 0))
        self.generation_id       = d.get("generation_id", "")
        self.host_id             = d.get("host_id", "")

    def save(self) -> None:
        self.sync_dir.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps({
            "last_manifest_mtime": self.last_manifest_mtime,
            "generation_id":       self.generation_id,
            "host_id":             self.host_id,
        }, indent=2))

    def record_pending_ack(self, path: str, sha: str) -> None:
        self.sync_dir.mkdir(parents=True, exist_ok=True)
        with self.acks_path.open("a") as f:
            f.write(json.dumps({"path": path, "sha256": sha}) + "\n")

    def drain_pending_acks(self) -> list[dict]:
        if not self.acks_path.exists():
            return []
        rows = []
        with self.acks_path.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError:
                    continue
        return rows

    def clear_pending_acks(self, acked_paths: set[str]) -> None:
        """Remove acked paths from the pending file. Keep failed/rejected ones
        so the next run retries."""
        if not self.acks_path.exists():
            return
        keep = []
        with self.acks_path.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if row.get("path") not in acked_paths:
                    keep.append(line)
        if keep:
            self.acks_path.write_text("\n".join(keep) + "\n")
        else:
            self.acks_path.unlink(missing_ok=True)


# ----------------------------------------------------------------------
# Manifest pagination
# ----------------------------------------------------------------------

def fetch_manifest_pages(host: str, since: int) -> tuple[list[dict], dict]:
    """Walk all manifest pages starting from `since`. Returns (entries, head)
    where head has generation_id + device_uptime_s + now."""
    entries: list[dict] = []
    cur_since = since
    head: dict = {}
    while True:
        url = f"{host}/manifest.json?since={cur_since}&limit={MANIFEST_PAGE_LIMIT}"
        info(f"  GET {url}")
        page = http_get_json(url)
        if not head:
            head = {
                "generation_id":    page.get("generation_id", ""),
                "device_uptime_s":  page.get("device_uptime_s", 0),
                "now":              page.get("now", 0),
            }
        files = page.get("files", []) or []
        entries.extend(files)
        if not page.get("truncated"):
            break
        nxt = page.get("next_since")
        if nxt is None or nxt <= cur_since:
            break
        cur_since = nxt
    return entries, head


# ----------------------------------------------------------------------
# Sync pass
# ----------------------------------------------------------------------

def plan_actions(entries: list[dict], state: State,
                 force_verify: bool = False) -> list[dict]:
    """For each manifest entry, decide what action to take. Returns a list of
    action dicts: {"action": "skip"|"fetch"|"refetch", "entry": ..., "reason": ...}.
    """
    actions = []
    for e in entries:
        device_path = e.get("path", "")
        category    = e.get("category", "file")
        sha_remote  = (e.get("sha256") or "")
        is_mutable  = bool(e.get("mutable"))
        size        = int(e.get("size", 0))
        local       = device_path_to_local(device_path, category, state.mirror)
        if not local.exists():
            actions.append({"entry": e, "local": local, "action": "fetch", "reason": "missing"})
            continue
        if is_mutable:
            # Mutable files (current hour bucket) — always refetch.
            actions.append({"entry": e, "local": local, "action": "refetch", "reason": "mutable"})
            continue
        if not sha_remote:
            actions.append({"entry": e, "local": local, "action": "refetch", "reason": "no remote sha"})
            continue
        if local.stat().st_size != size:
            actions.append({"entry": e, "local": local, "action": "refetch", "reason": "size mismatch"})
            continue
        if force_verify:
            sha_local = sha256_file(local)
            if sha_local != sha_remote:
                actions.append({"entry": e, "local": local, "action": "refetch", "reason": "verify sha mismatch"})
            else:
                actions.append({"entry": e, "local": local, "action": "skip", "reason": "verified"})
            continue
        # Trust local. We'll verify on fetch path; here we skip.
        actions.append({"entry": e, "local": local, "action": "skip", "reason": "present"})
    return actions


def fetch_and_verify(host: str, entry: dict, local: Path) -> tuple[bool, str]:
    """Fetch via Range-resumable GET into <local>.partial, verify sha256 if
    the manifest provided one, atomic rename. Returns (ok, sha_local)."""
    device_path = entry["path"]
    sha_remote  = (entry.get("sha256") or "")
    is_mutable  = bool(entry.get("mutable"))
    size        = int(entry.get("size", 0))
    local.parent.mkdir(parents=True, exist_ok=True)

    partial = local.with_suffix(local.suffix + ".partial")
    start = partial.stat().st_size if partial.exists() else 0
    if start > size:
        # Stale partial larger than remote — wipe.
        partial.unlink(missing_ok=True)
        start = 0

    url = f"{host}/files/get?path={urllib.parse.quote(device_path)}"
    try:
        wrote = http_get_range(url, start, partial)
        info(f"  fetched {device_path}: +{wrote} bytes (total {partial.stat().st_size})")
    except urllib.error.URLError as e:
        warn(f"  fetch failed {device_path}: {e}")
        return False, ""

    sha_local = sha256_file(partial)
    if not is_mutable and sha_remote and sha_local != sha_remote:
        warn(f"  sha mismatch on {device_path} (got {sha_local[:8]}, expected {sha_remote[:8]}); discarding partial")
        partial.unlink(missing_ok=True)
        return False, ""
    # Atomic rename.
    partial.rename(local)
    return True, sha_local


def post_acks(host: str, paths_with_sha: list[tuple[str, str]]) -> set[str]:
    """POST /sync/ack in batches. Returns the set of acked paths."""
    acked: set[str] = set()
    for i in range(0, len(paths_with_sha), ACK_BATCH_SIZE):
        batch = paths_with_sha[i:i + ACK_BATCH_SIZE]
        body = {"files": [{"path": p, "sha256": s} for (p, s) in batch]}
        try:
            resp = http_post_json(f"{host}/sync/ack", body)
            n_ok = int(resp.get("acked", 0))
            n_rej = int(resp.get("rejected", 0))
            info(f"  POST /sync/ack -> acked={n_ok} rejected={n_rej}")
            rej_paths = set(resp.get("rejected_paths", []) or [])
            for (p, _) in batch:
                if p not in rej_paths:
                    acked.add(p)
        except urllib.error.URLError as e:
            warn(f"  /sync/ack failed: {e}")
    return acked


# ----------------------------------------------------------------------
# Subcommands
# ----------------------------------------------------------------------

def cmd_init(args) -> int:
    mirror = Path(args.mirror).expanduser().resolve()
    if mirror.exists() and any(mirror.iterdir()):
        warn(f"{mirror} is not empty — re-using existing mirror dir")
    mirror.mkdir(parents=True, exist_ok=True)
    for sub in ("wavs", "infer", "markers", "misc", ".sync"):
        (mirror / sub).mkdir(exist_ok=True)
    state = State(mirror)
    state.load()
    state.host_id = state.host_id or os.uname().nodename
    state.save()
    info(f"initialized mirror at {mirror}")
    return 0


def cmd_once(args, force_verify: bool = False) -> int:
    return _run_pass(args, force_verify=force_verify)


def cmd_loop(args) -> int:
    interval = int(args.interval)
    info(f"loop mode: interval={interval}s")
    while True:
        try:
            _run_pass(args)
        except Exception as e:
            err(f"pass failed: {e}")
        time.sleep(interval)


def cmd_verify(args) -> int:
    return _run_pass(args, force_verify=True, dry_run=True)


def cmd_dry_run(args) -> int:
    return _run_pass(args, dry_run=True)


def _run_pass(args, force_verify: bool = False, dry_run: bool = False) -> int:
    mirror = Path(args.mirror).expanduser().resolve()
    if not mirror.exists():
        err(f"mirror dir {mirror} does not exist; run `init` first")
        return 2
    state = State(mirror)
    state.load()

    info(f"=== sync pass ({'dry-run' if dry_run else 'live'}) ===")
    info(f"  mirror={mirror}")
    info(f"  host={args.host}")
    info(f"  since={state.last_manifest_mtime}  gen_id={state.generation_id or '(none)'}")

    # --- 1. Drain any pending acks from a previous pass that died mid-ack ---
    pending = state.drain_pending_acks()
    if pending and not dry_run:
        info(f"draining {len(pending)} pending acks from previous pass...")
        acked = post_acks(args.host, [(r["path"], r["sha256"]) for r in pending])
        state.clear_pending_acks(acked)

    # --- 2. Fetch manifest ---
    try:
        entries, head = fetch_manifest_pages(args.host, state.last_manifest_mtime)
    except urllib.error.URLError as e:
        err(f"manifest fetch failed: {e}")
        return 1

    info(f"manifest: {len(entries)} entries since={state.last_manifest_mtime}, generation_id={head.get('generation_id', '')}")

    # --- 3. Generation_id check ---
    remote_gen = head.get("generation_id", "")
    if state.generation_id and remote_gen and state.generation_id != remote_gen:
        warn(f"generation_id changed: local={state.generation_id} remote={remote_gen}")
        warn("forcing full re-verify of local files")
        force_verify = True
        # Fetch a full manifest for verification.
        if state.last_manifest_mtime > 0:
            entries, head = fetch_manifest_pages(args.host, 0)
            info(f"  full manifest after gen change: {len(entries)} entries")

    # --- 4. Plan actions ---
    actions = plan_actions(entries, state, force_verify=force_verify)
    n_skip   = sum(1 for a in actions if a["action"] == "skip")
    n_fetch  = sum(1 for a in actions if a["action"] == "fetch")
    n_refet  = sum(1 for a in actions if a["action"] == "refetch")
    info(f"plan: skip={n_skip} fetch={n_fetch} refetch={n_refet}")

    if dry_run:
        for a in actions:
            if a["action"] != "skip":
                e = a["entry"]
                info(f"  {a['action']:8s} {e['path']:55s} ({a['reason']})  -> {a['local']}")
        return 0

    # --- 5. Execute ---
    fetched_paths: list[tuple[str, str]] = []  # (device_path, sha)
    fail = 0
    for a in actions:
        if a["action"] == "skip":
            continue
        e = a["entry"]
        info(f"  {a['action']:8s} {e['path']}")
        ok, sha = fetch_and_verify(args.host, e, a["local"])
        if ok:
            fetched_paths.append((e["path"], sha))
            state.record_pending_ack(e["path"], sha)
        else:
            fail += 1

    info(f"fetched: {len(fetched_paths)} (failures: {fail})")

    # --- 6. Ack ---
    if fetched_paths:
        acked = post_acks(args.host, fetched_paths)
        info(f"acked: {len(acked)}/{len(fetched_paths)}")
        state.clear_pending_acks(acked)

    # --- 7. Update state ---
    if entries:
        max_mtime = max(int(e.get("mtime", 0)) for e in entries)
        if max_mtime > state.last_manifest_mtime:
            state.last_manifest_mtime = max_mtime
    if remote_gen and not state.generation_id:
        state.generation_id = remote_gen
    elif remote_gen and state.generation_id != remote_gen:
        state.generation_id = remote_gen  # we just re-verified
    state.save()
    info("=== pass complete ===")
    return 0 if fail == 0 else 1


# ----------------------------------------------------------------------
# CLI entry
# ----------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description="Phase B sync client")
    p.add_argument("--host", default=DEFAULT_HOST,
                   help=f"device base URL (default {DEFAULT_HOST})")
    p.add_argument("--mirror", required=True,
                   help="local mirror directory (required)")
    sp = p.add_subparsers(dest="cmd", required=True)

    sp.add_parser("init",     help="bootstrap the mirror dir + .sync/ scaffolding")
    sp.add_parser("once",     help="single sync pass; exit on completion")
    sp.add_parser("verify",   help="re-hash all local files and compare against device")
    sp.add_parser("dry-run",  help="show plan without fetching")

    p_loop = sp.add_parser("loop",
                           help="continuous mode (sync every --interval seconds)")
    p_loop.add_argument("--interval", type=int, default=900,
                        help="seconds between passes (default 900)")

    args = p.parse_args()

    handlers = {
        "init":     cmd_init,
        "once":     cmd_once,
        "loop":     cmd_loop,
        "verify":   cmd_verify,
        "dry-run":  cmd_dry_run,
    }
    return handlers[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main())
