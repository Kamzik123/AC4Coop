#!/usr/bin/env python3
"""Direct inter-session messaging through the local OpenCode server API.

The Agent Intercom plugin would be nicer, but this host's plugin loader rejects
both a file path and a factory default. The server API is the same surface the
UI uses, it is already authenticated on this machine, and it needs no restart.

    python ops/tools/dm.py list
    python ops/tools/dm.py send <session-id|title-substring> "message"
    python ops/tools/dm.py read <session-id|title-substring> [n]
"""
from __future__ import annotations

import base64
import json
import os
import sys
import urllib.error
import urllib.request

CFG = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
PASSWORD = json.load(open(os.path.join(CFG, "service.json")))["password"]
AUTH = "Basic " + base64.b64encode(f"opencode:{PASSWORD}".encode()).decode()

# the desktop app binds the API on a random loopback port; find it
CANDIDATES = [28894]


def _request(method, path, body=None, base=None):
    url = (base or f"http://127.0.0.1:{CANDIDATES[0]}") + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Authorization": AUTH,
                                          "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read()
            try:
                return r.status, json.loads(raw)
            except Exception:
                return r.status, raw.decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except Exception:
            return e.code, raw.decode("utf-8", "replace")[:400]


def sessions():
    code, data = _request("GET", "/api/session")
    if code != 200:
        raise SystemExit(f"GET /api/session -> {code}: {data}")
    rows = data.get("data", data) if isinstance(data, dict) else data
    out = []
    for s in rows:
        out.append({
            "id": s.get("id"),
            "title": (s.get("title") or "").strip(),
            "directory": (s.get("location") or {}).get("directory") if isinstance(s.get("location"), dict) else s.get("directory"),
            "updated": (s.get("time") or {}).get("updated") if isinstance(s.get("time"), dict) else s.get("updated"),
        })
    return out


def resolve(target):
    if target.startswith("ses"):
        return target
    t = target.lower()
    hits = [s for s in sessions() if t in (s["title"] or "").lower()]
    if len(hits) == 1:
        return hits[0]["id"]
    if not hits:
        raise SystemExit(f"no session title contains {target!r}")
    raise SystemExit("ambiguous: " + ", ".join(f"{h['id']}={h['title']}" for h in hits))


def send(target, text):
    sid = resolve(target)
    # /prompt takes {"text": ...}; /message takes {"parts":[{"type":"text",...}]}
    attempts = [
        (f"/api/session/{sid}/prompt", {"text": text}),
        (f"/api/session/{sid}/message", {"parts": [{"type": "text", "text": text}]}),
        (f"/api/session/{sid}/prompt_async", {"text": text}),
    ]
    last = None
    for path, body in attempts:
        code, data = _request("POST", path, body)
        if code < 300:
            print(f"sent to {sid} via {path} -> {code}")
            return 0
        last = (path, code, data)
        if code == 400 and isinstance(data, dict) and "text" in str(data).lower():
            continue
    print(f"send failed: {last}")
    return 1


def read(target, n=5):
    sid = resolve(target)
    code, data = _request("GET", f"/api/session/{sid}/message?limit={n}")
    if code != 200:
        print(f"read -> {code} {data}")
        return 1
    msgs = data.get("data", data) if isinstance(data, dict) else data
    for m in msgs[-n:]:
        txt = ""
        if isinstance(m, dict):
            parts = m.get("parts") or []
            txt = " ".join(p.get("text", "") for p in parts if isinstance(p, dict))
            if not txt:
                txt = json.dumps(m)[:300]
        print(f"- {str(txt)[:500]}")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "list":
        for s in sessions():
            print(f"{s['id']}  {s['title'][:80]}")
        return 0
    if cmd == "send" and len(argv) >= 4:
        return send(argv[2], argv[3])
    if cmd == "read" and len(argv) >= 3:
        return read(argv[2], int(argv[3]) if len(argv) > 3 else 5)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
