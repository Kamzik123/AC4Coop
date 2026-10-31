#!/usr/bin/env python3
"""bus_watch.py - wake the orchestrator when a NEW report file appears on the bus.

Unlike the old seat watcher this has no "quiet" heuristic: it reacts to a file
appearing in ops/bus/outbox/, which the seat writes atomically. If this process is
dead, nothing is lost - the report stays on the bus and the orchestrator sees it
on the next `bus.py inbox`.

  py ops/tools/bus_watch.py            # loop, 5 s poll
  py ops/tools/bus_watch.py --once     # single pass, for testing

Delivery is deduped by the report id in ops/bus/notified.json.
"""
from __future__ import annotations

import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

OPS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUS = os.path.join(OPS, "bus")
OUT = os.path.join(BUS, "outbox")
NOTIFIED = os.path.join(BUS, "notified.json")
LOG = os.path.join(BUS, "bus_watch.log")
CFG = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
BASE = os.environ.get("OPENCODE_API", "http://127.0.0.1:28894")
ORCH = os.environ.get("OPENCODE_SESSION_ID", "ses_e78f161e7ffd99U6I3sT7cXOz7")


def log(msg):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {msg}\n")


def auth():
    pw = json.load(open(os.path.join(CFG, "service.json")))["password"]
    return "Basic " + base64.b64encode(("opencode:" + pw).encode()).decode()


def post(path, body):
    r = urllib.request.Request(BASE + path, data=json.dumps(body).encode(), method="POST",
                               headers={"Authorization": auth(), "Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=25) as x:
        return x.status


def load(p, d):
    try:
        return json.load(open(p, encoding="utf-8"))
    except Exception:
        return d


def save(p, o):
    tmp = p + ".tmp"
    json.dump(o, open(tmp, "w", encoding="utf-8"))
    os.replace(tmp, p)


def scan():
    seen = load(NOTIFIED, {"ids": []})
    ids = set(seen.get("ids", []))
    new = []
    if os.path.isdir(OUT):
        for name in sorted(os.listdir(OUT)):
            if not name.endswith(".json"):
                continue
            rid = name[:-5]
            if rid in ids:
                continue
            try:
                rep = json.load(open(os.path.join(OUT, name), encoding="utf-8"))
            except Exception:
                continue
            new.append(rep)
            ids.add(rid)
    if new:
        save(NOTIFIED, {"ids": sorted(ids)})
    return new


def wake(reports):
    head = "\n".join(
        f"- {r['seat']} {r['status']}: {(r.get('text') or '')[:300]}"
        for r in reports)
    text = (f"[BUS] {len(reports)} new seat report(s)\n{head}\n\n"
            f"Read them all with: py ops\\tools\\bus.py inbox --ack\n"
            f"Act on them: update ops/CLAIMS.md, ops/TASKBOARD.md, ops/STATE.json, and continue the plan.")
    return post(f"/api/session/{ORCH}/prompt", {"text": text})


def main(argv):
    once = "--once" in argv
    log("bus_watch started")
    while True:
        try:
            new = scan()
            if new:
                try:
                    code = wake(new)
                    log(f"woke orch for {len(new)} report(s) HTTP {code}")
                except Exception as e:
                    # put the ids back so nothing is silently marked as notified
                    s = load(NOTIFIED, {"ids": []})
                    s["ids"] = [i for i in s.get("ids", []) if i not in [r["id"] for r in new]]
                    save(NOTIFIED, s)
                    log(f"wake failed: {type(e).__name__}: {e}")
        except Exception as e:
            log(f"scan error: {type(e).__name__}: {e}")
        if once:
            return 0
        time.sleep(5)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
