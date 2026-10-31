#!/usr/bin/env python3
"""Wake the orchestrator when a seat replies.

Polls the local OpenCode API for new assistant *text* in the seat sessions and
injects a short prompt into the orchestrator's own session, which wakes it as a
normal turn.

Dedupe is by message timestamp (not id): an assistant turn may contain tool-only
messages, and an id watermark stalls when the newest message has no text.

    python ops/tools/watch_seats.py --once --dry-run
    python ops/tools/watch_seats.py --seed
    python ops/tools/watch_seats.py
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

OPS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG_DIR = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
PASSWORD = json.load(open(os.path.join(CFG_DIR, "service.json")))["password"]
AUTH = "Basic " + base64.b64encode(f"opencode:{PASSWORD}".encode()).decode()

BASE = os.environ.get("OPENCODE_API", "http://127.0.0.1:28894")
ORCH = os.environ.get("OPENCODE_SESSION_ID", "ses_e78f161e7ffd99U6I3sT7cXOz7")
STATE = os.path.join(OPS, "bus", "watch_state.json")
LOG = os.path.join(OPS, "bus", "watch.log")
SEATS = os.path.join(OPS, "bus", "seats.json")

SEAT_PREFIX = re.compile(r"^(NET|BIN|QA|COMBAT|DOC|AI|NAVAL|MISSION)\b", re.I)
MARKER = "[SEAT "
REPORT = re.compile(r"SEAT\s+[A-Z]+\s*\|\s*status\s*:", re.I)
WAKE_CHARS = 1700          # keep the injected prompt under the ~2000-char cut
QUIET_SECONDS = 45         # a seat is "done" only after this much silence


def log(msg: str) -> None:
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {msg}"
    print(line)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def api(method: str, path: str, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Authorization": AUTH,
                                          "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except Exception:
            return e.code, raw.decode("utf-8", "replace")[:300]


def load_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def save_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def seat_sessions():
    explicit = load_json(SEATS, {})
    code, data = api("GET", "/api/session")
    if code != 200 or not data:
        log(f"sessions: HTTP {code}")
        return {}
    rows = data.get("data", data) if isinstance(data, dict) else data
    out = {}
    for s in rows:
        sid = s.get("id")
        title = (s.get("title") or "").strip()
        if not sid or sid == ORCH:
            continue
        if sid in explicit:
            out[sid] = explicit[sid]
        elif SEAT_PREFIX.match(title):
            out[sid] = title
    return out


def message_text(m):
    parts = m.get("content") or m.get("parts") or []
    texts = [p.get("text", "") for p in parts
             if isinstance(p, dict) and p.get("type") == "text"]
    return "\n".join(t for t in texts if t).strip()


def created_ms(m):
    t = m.get("time") or {}
    if isinstance(t, dict):
        for k in ("created", "completed", "streamed"):
            if isinstance(t.get(k), (int, float)):
                return int(t[k])
    if isinstance(m.get("created"), (int, float)):
        return int(m["created"])
    return 0


def seed(state):
    """Everything that exists now is old news; only later messages wake us."""
    now = int(time.time() * 1000)
    marks = state.setdefault("last_ts", {})
    for sid in seat_sessions():
        marks[sid] = now
    save_json(STATE, state)
    log(f"seeded {len(marks)} seat(s) at now; only later messages wake the orchestrator")


def poll(state, dry_run=False):
    seats = seat_sessions()
    muted = set(state.get("muted", []))
    marks = state.setdefault("last_ts", {})
    woke = 0
    for sid, title in seats.items():
        if sid in muted:
            continue
        try:
            code, data = api("GET", f"/api/session/{sid}/message?limit=14")
        except Exception as exc:
            log(f"{title}: read error {type(exc).__name__}")
            continue
        if code != 200 or not data:
            continue
        msgs = data.get("data", data) if isinstance(data, dict) else data
        mark = marks.get(sid, 0)
        # A turn is finished when the seat goes QUIET: tool-step boundaries also emit
        # an "idle" marker, so idle alone is not a turn boundary.
        newest = max([created_ms(m) for m in msgs] or [0])
        if newest <= mark:
            continue
        if (time.time() * 1000 - newest) < QUIET_SECONDS * 1000:
            continue                       # still working; leave the mark alone
        fresh = []
        for m in msgs:
            if m.get("type") != "assistant":
                continue
            body = message_text(m)
            ts = created_ms(m)
            if body and MARKER not in body and ts > mark:
                fresh.append((ts, body))
        marks[sid] = newest
        if not fresh:
            continue
        fresh.sort()
        # Only a finished REPORT wakes the orchestrator. Seats narrate their progress
        # ("Let me read ...", "Now let me ...") at the end of tool-only turns; waking on
        # those burns orchestrator context for no information. The contract in
        # ops/HIVE.md is that a report starts with "SEAT <name> | status:".
        report = [t for _, t in fresh if REPORT.search(t[:600])]
        if not report:
            continue
        body = "\n\n".join(report[-3:])[:WAKE_CHARS]
        text = (f"{MARKER}{title} | {sid}]\n{body}\n\n"
                f"(truncated at {WAKE_CHARS} chars - read the full reply with: "
                f"py ops/tools/session_read.py {sid})\n\n"
                f"Act on this reply: update STATE, answer the seat if it asked "
                f"something, and continue the programme. Do not ask the user.")
        if dry_run:
            log(f"DRY {title}: {body[:100]!r}")
        else:
            code, _ = api("POST", f"/api/session/{ORCH}/prompt", {"text": text})
            log(f"woke orch for {title} (HTTP {code})")
        woke += 1
    save_json(STATE, state)
    return woke


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--seed", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--interval", type=float, default=15.0)
    args = ap.parse_args(argv)

    state = load_json(STATE, {})
    if "last" in state and "last_ts" not in state:
        state = {"last_ts": {}}          # migrate away from id watermarking
    if args.seed:
        seed(state)
        return 0
    if args.once:
        log(f"once: {poll(state, dry_run=args.dry_run)} wake(s)")
        return 0

    log(f"watching {len(seat_sessions())} seat(s)")
    while True:
        try:
            poll(state)
        except Exception as exc:
            log(f"poll error: {type(exc).__name__}: {exc}")
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
