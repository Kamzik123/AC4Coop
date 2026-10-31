#!/usr/bin/env python3
"""bus.py - durable agent-to-orchestrator mailbox.

Why this exists: the polling watcher dropped reports whenever a seat stayed
"busy", and the orchestrator never learned a report had been written. Capture
must be lossless even when waking is not. So reports are FILES, written by the
seat itself, and reading them is a pull the orchestrator performs.

Commands
--------
  report <SEAT> <STATUS> <text>   seat-side: write a durable report file
  inbox [--all] [--ack]           orchestrator: show unacked reports
  ack <id>                        mark one report read
  status                          per-seat last report + session busy age
  send <SEAT> <text>              orchestrator -> seat (records + delivers)
  tail [n]                        last n reports regardless of ack

Files
-----
  ops/bus/outbox/<utc>_<SEAT>_<id>.json    one report per file, immutable
  ops/bus/archive/...                       acked reports moved here
  ops/bus/sent/<utc>_<SEAT>.json            outbound messages, for audit
  ops/bus/acked.json                        {id: acked_at}

Exit codes: 0 ok, 1 nothing found, 2 usage.
"""
from __future__ import annotations

import base64
import datetime
import json
import os
import re
import shutil
import sys
import urllib.error
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

OPS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUS = os.path.join(OPS, "bus")
OUT = os.path.join(BUS, "outbox")
ARC = os.path.join(BUS, "archive")
SENT = os.path.join(BUS, "sent")
ACKED = os.path.join(BUS, "acked.json")

SEATS = {
    "NET": "ses_e741f0becffevKuDnylqDe7rph",
    "IMPL": "ses_e741f0b73ffeeVFuPV4Qu0iltx",
    "RE": "ses_e741f0b23ffemD1twBuCaV2rgl",
    "QA": "ses_e741f0ad5ffeoIwLMuj5xt0Tun",
    "AI": "ses_e741f0a85ffehWi7MKTgSQJzJM",
    "GFX": "ses_e73dfe2f1fferk6GOWns9DtwWz",
}

CFG = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
BASE = os.environ.get("OPENCODE_API", "http://127.0.0.1:28894")


def now():
    return datetime.datetime.now(datetime.UTC)


def stamp(t=None):
    return (t or now()).strftime("%Y%m%dT%H%M%SZ")


def ensure():
    for d in (OUT, ARC, SENT, BUS):
        os.makedirs(d, exist_ok=True)


def load_json(p, default):
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def save_json(p, obj):
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
    os.replace(tmp, p)


def password():
    return json.load(open(os.path.join(CFG, "service.json")))["password"]


def api(method, path, body=None):
    auth = "Basic " + base64.b64encode(("opencode:" + password()).encode()).decode()
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Authorization": auth, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(r, timeout=25) as x:
            return x.status, json.loads(x.read())
    except urllib.error.HTTPError as e:
        return e.code, None
    except Exception as e:
        return 0, str(e)


# ---------------------------------------------------------------- seat side

def cmd_report(argv):
    if len(argv) < 3:
        print("usage: bus.py report <SEAT> <STATUS> <text>")
        return 2
    seat, status, text = argv[0].upper(), argv[1].upper(), " ".join(argv[2:])
    ensure()
    rid = f"{stamp()}_{seat}_{os.getpid()}"
    body = {"id": rid, "seat": seat, "status": status, "ts": now().isoformat(),
            "text": text, "session": SEATS.get(seat, "")}
    save_json(os.path.join(OUT, rid + ".json"), body)
    print(f"report written: {rid}")
    return 0


def _reports(root):
    rows = []
    for name in sorted(os.listdir(root)):
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(root, name), encoding="utf-8") as f:
                rows.append(json.load(f))
        except Exception:
            continue
    rows.sort(key=lambda r: r.get("ts", ""))
    return rows


def cmd_inbox(argv):
    ensure()
    show_all = "--all" in argv
    do_ack = "--ack" in argv
    acked = load_json(ACKED, {})
    rows = _reports(OUT)
    pending = [r for r in rows if r["id"] not in acked]
    shown = rows if show_all else pending
    if not shown:
        print(f"inbox empty ({len(rows)} archived, {len(pending)} pending)")
        return 1
    for r in shown:
        mark = "NEW " if r["id"] not in acked else "    "
        print(f"{mark}{r['ts']}  {r['seat']:5} {r['status']:9} {r['id']}")
        print("    " + (r.get("text") or "").replace("\n", "\n    ")[:4000])
        print()
    if do_ack and pending:
        for r in pending:
            acked[r["id"]] = now().isoformat()
        save_json(ACKED, acked)
        print(f"acked {len(pending)}")
    return 0


def cmd_ack(argv):
    if not argv:
        print("usage: bus.py ack <id>|--all")
        return 2
    ensure()
    acked = load_json(ACKED, {})
    if argv[0] == "--all":
        for r in _reports(OUT):
            acked[r["id"]] = now().isoformat()
    else:
        acked[argv[0]] = now().isoformat()
    save_json(ACKED, acked)
    print("acked")
    return 0


def cmd_tail(argv):
    n = int(argv[0]) if argv else 5
    rows = _reports(OUT)
    for r in rows[-n:]:
        print(f"{r['ts']}  {r['seat']:5} {r['status']:9} {r['id']}")
        print("   " + (r.get("text") or "").replace("\n", "\n   ")[:1200])
    return 0


# ------------------------------------------------------------ orchestrator side

def cmd_send(argv):
    if len(argv) < 2:
        print("usage: bus.py send <SEAT> <text>")
        return 2
    seat = argv[0].upper()
    text = " ".join(argv[1:])
    sid = SEATS.get(seat)
    if not sid:
        print(f"unknown seat {seat}")
        return 2
    ensure()
    rec = {"seat": seat, "session": sid, "ts": now().isoformat(), "text": text}
    save_json(os.path.join(SENT, f"{stamp()}_{seat}.json"), rec)
    code, _ = api("POST", f"/api/session/{sid}/prompt", {"text": text})
    print(f"sent to {seat} ({sid}) -> HTTP {code}")
    return 0 if code < 300 else 1


def cmd_status(argv):
    print(f"{'seat':6}{'last report':22}{'status':10}{'session busy age':20}msgs")
    for seat, sid in SEATS.items():
        last = "never"
        status = "-"
        for r in _reports(OUT):
            if r["seat"] == seat:
                last = r["ts"][11:19]
                status = r["status"]
        code, data = api("GET", f"/api/session/{sid}")
        age, nmsg = "-", "?"
        if code == 200 and isinstance(data, dict):
            s = data.get("data", data)
            t = s.get("time") or {}
            upd = t.get("updated") or 0
            if upd:
                mins = (now().timestamp() * 1000 - upd) / 60000
                age = f"{mins:.0f}m ({'busy' if t.get('busy') else 'idle'})"
            code2, d2 = api("GET", f"/api/session/{sid}/message?limit=1")
            nmsg = str(len(d2.get("data", d2))) if code2 == 200 and isinstance(d2, dict) else "?"
        print(f"{seat:6}{last:22}{status:10}{age:20}{nmsg}")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    rest = argv[2:]
    if cmd == "report":
        return cmd_report(rest)
    if cmd == "inbox":
        return cmd_inbox(rest)
    if cmd == "ack":
        return cmd_ack(rest)
    if cmd == "tail":
        return cmd_tail(rest)
    if cmd == "send":
        return cmd_send(rest)
    if cmd == "status":
        return cmd_status(rest)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
