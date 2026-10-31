import base64
import json
import os
import sys
import time
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
cfg = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
pw = json.load(open(os.path.join(cfg, "service.json")))["password"]
auth = "Basic " + base64.b64encode(f"opencode:{pw}".encode()).decode()

SID = "ses_e787ad2e0ffeSuMwdt0QV9Nmgn"
mark = 1793275163553
now_ms = int(time.time() * 1000)


def get(p):
    r = urllib.request.Request("http://127.0.0.1:28894" + p, headers={"Authorization": auth})
    with urllib.request.urlopen(r, timeout=20) as resp:
        return json.loads(resp.read())


msgs = get(f"/api/session/{SID}/message?limit=10").get("data", [])
print(f"now={now_ms} mark={mark} (mark is {round((now_ms-mark)/1000)}s ago)")
for m in msgs[-10:]:
    t = m.get("time") or {}
    created = t.get("created")
    parts = m.get("content") or m.get("parts") or []
    txt = "".join(p.get("text", "") for p in parts if isinstance(p, dict) and p.get("type") == "text")
    age = round((now_ms - created) / 1000) if isinstance(created, (int, float)) else "?"
    print(f"  {str(m.get('type')):10} {str(m.get('id'))[:26]:26} created={created} age={age}s textlen={len(txt)}")
    if txt:
        print(f"      {txt[:100]!r}")
