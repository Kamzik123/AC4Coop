#!/usr/bin/env python3
"""Create a hive seat with an explicit model and the correct working directory.

    py ops/tools/mkseat.py "NET: protocol lane" [directory]

Why this exists: sessions created with a bare `POST /api/session` inherit the
default model, which on this machine resolves to a FREE provider and times out.
The create payload requires {"model": {"id": ..., "providerID": ...}} — a
provider-qualified string or a {providerID, modelID} object both return 400.

Prints the new session id.
"""
from __future__ import annotations

import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

CFG = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
PW = json.load(open(os.path.join(CFG, "service.json")))["password"]
AUTH = "Basic " + base64.b64encode(f"opencode:{PW}".encode()).decode()
BASE = os.environ.get("OPENCODE_API", "http://127.0.0.1:28894")
MODEL = {"id": "deepseek-flash", "providerID": "deepseek"}


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Authorization": AUTH,
                                        "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(r, timeout=30) as x:
            return x.status, json.loads(x.read())
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")[:300]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    title = argv[1]
    directory = argv[2] if len(argv) > 2 else os.getcwd()
    q = urllib.parse.quote(directory, safe="")

    attempts = [
        (f"/api/session?directory={q}", {"title": title, "model": MODEL}),
        ("/api/session", {"title": title, "model": MODEL,
                          "location": {"directory": directory}}),
        ("/api/session", {"title": title, "model": MODEL, "directory": directory}),
        ("/api/session", {"title": title, "model": MODEL}),
    ]
    for path, body in attempts:
        code, data = call("POST", path, body)
        if code < 300 and isinstance(data, dict):
            sid = (data.get("data") or data).get("id")
            if sid:
                c2, s = call("GET", "/api/session/" + sid)
                s = s.get("data", s)
                where = (s.get("location") or {}).get("directory")
                print(sid)
                sys.stderr.write(f"model={s.get('model')} dir={where}\n")
                return 0
    print(f"create failed: {code} {data}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
