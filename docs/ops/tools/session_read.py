import base64
import json
import os
import sys
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

cfg = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
pw = json.load(open(os.path.join(cfg, "service.json")))["password"]
auth = "Basic " + base64.b64encode(f"opencode:{pw}".encode()).decode()

SID = sys.argv[1] if len(sys.argv) > 1 else "ses_e988712e4ffdYTcGC8j4bxedc3"


def get(path):
    req = urllib.request.Request("http://127.0.0.1:28894" + path,
                                 headers={"Authorization": auth})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read())


data = get(f"/api/session/{SID}/message?limit=10")
msgs = data.get("data", data)
for m in msgs[-10:]:
    role = m.get("type") or m.get("role")
    parts = m.get("content") or m.get("parts") or []
    texts = [p.get("text", "") for p in parts
             if isinstance(p, dict) and p.get("type") == "text"]
    body = "\n".join(texts).strip()
    if body:
        print(f"--- {role} ---")
        print(body[:1800])
        print()
