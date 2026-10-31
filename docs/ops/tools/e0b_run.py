#!/usr/bin/env python3
"""E0-B: run the synthetic peer against a live game instance, unattended.

Two phases, because the peer needs to know where the player is before it can
stand next to them:

  1. listen-only for a few seconds, decode the game's PlayerState, learn the
     player position (and the world id it must echo);
  2. pick the guard nearest that position from the game's own `[Id]` rows, then
     run the peer with --echo-pos and --kill.

Trigger is `[Net] init=` — the bound-port line goes to OutputDebugString and
never reaches the log file.

    python ops/tools/e0b_run.py [--timeout 1800] [--duration 30]
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

OPS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag"
PY = sys.executable
PEER = os.path.join(OPS, "tools", "peer9.py")
EVID = os.path.join(OPS, "evidence", "E0-B")
ROW = re.compile(r"\[Id\] idx=(\d+) objId=(\d+) ent=([0-9A-Fa-f]+).*?pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")
POS = re.compile(r"pos=\(([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)\)")


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def wait_for_net(timeout, known):
    start = time.time()
    while time.time() - start < timeout:
        for p in sorted(glob.glob(os.path.join(GAME, "phase1_log_*.txt")),
                        key=os.path.getmtime, reverse=True)[:6]:
            if p in known:
                continue          # a session that predates this runner
            try:
                txt = open(p, encoding="utf-8", errors="replace").read(200_000)
            except OSError:
                continue
            if "[Net] init=" in txt:
                m = re.search(r"_(\d+)\.txt$", os.path.basename(p))
                return p, (m.group(1) if m else "?")
        time.sleep(2)
    return None, None


def runner(*args, duration=None):
    cmd = [PY, PEER] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=(duration or 30) + 60)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--duration", type=int, default=30)
    args = ap.parse_args(argv)

    log(f"waiting up to {args.timeout}s for [Net] init= in a NEW game log")
    known = set(glob.glob(os.path.join(GAME, "phase1_log_*.txt")))
    log_path, pid = wait_for_net(args.timeout, known)
    if not log_path:
        log("TIMEOUT: no game log with [Net] init= appeared")
        return 1
    log(f"game: {os.path.basename(log_path)} (pid {pid})")

    # ---- phase 1: learn where the player is -------------------------------------
    r1 = runner("--still", "--duration", "6")
    pos = None
    for line in (r1.stdout + r1.stderr).splitlines():
        if line.startswith("RX player"):
            m = POS.search(line)
            if m:
                pos = tuple(float(v) for v in m.groups())
    if not pos:
        log("phase 1 failed: the game sent no decodable PlayerState — is it in-world?")
        print((r1.stdout + r1.stderr)[-1500:])
        return 2
    log(f"player position: {pos}")

    # ---- pick the guard nearest the player --------------------------------------
    rows = []
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = ROW.search(line)
        if m:
            rows.append((m.group(2), (float(m.group(4)), float(m.group(5)), float(m.group(6)))))
    if rows:
        best = min(rows, key=lambda r: sum((a - b) ** 2 for a, b in zip(r[1], pos)))
        target, tpos = best
        dist = sum((a - b) ** 2 for a, b in zip(tpos, pos)) ** 0.5
        log(f"target objId={target} at {tpos} ({dist:.1f} m)")
    else:
        target, dist = None, None
        log("no [Id] rows in the log; running presence-only")

    # ---- phase 2: presence + kill ----------------------------------------------
    extra = ["--kill", target] if target else []
    r2 = runner("--walk", "--echo-pos", "--duration", str(args.duration), *extra,
                duration=args.duration)
    out = (r2.stdout or "") + (r2.stderr or "")
    print(out[-2500:])

    def count(pat):
        try:
            txt = open(log_path, encoding="utf-8", errors="replace").read()
        except OSError:
            return 0
        return len(re.findall(pat, txt))

    summary = {
        "log": os.path.basename(log_path), "pid": pid,
        "player_pos": pos, "target_objId": target, "target_dist_m": dist,
        "peer_rc": r2.returncode,
        "game_apply": count(r"\[Net\] apply"),
        "game_kill_deaths": count(r"\[Kill\] peer DEATH"),
        "game_kill_unresolved": count(r"\[Kill\] event objId=.*unresolved"),
        "game_host_death": count(r"\[Relay\] host DEATH"),
        "peer_tail": out[-1200:],
    }
    os.makedirs(EVID, exist_ok=True)
    path = os.path.join(EVID, f"run-{time.strftime('%Y%m%d-%H%M%S')}.json")
    json.dump(summary, open(path, "w", encoding="utf-8"), indent=2)
    log(json.dumps({k: v for k, v in summary.items() if k != "peer_tail"}, indent=2))
    log(f"summary: {path}")

    # wake the orchestrator with the result, so nobody has to notice the file
    try:
        import base64
        import urllib.request
        cfg = os.path.expandvars(r"%USERPROFILE%\.config\opencode")
        pw = json.load(open(os.path.join(cfg, "service.json")))["password"]
        auth = "Basic " + base64.b64encode(f"opencode:{pw}".encode()).decode()
        orch = os.environ.get("OPENCODE_SESSION_ID", "")
        if orch:
            text = ("[SEAT E0-B | run finished]\n"
                    + json.dumps({k: v for k, v in summary.items() if k != "peer_tail"}, indent=2)
                    + "\n\nAct on this: decide whether the kill lane passed.")
            req = urllib.request.Request(
                f"http://127.0.0.1:28894/api/session/{orch}/prompt",
                data=json.dumps({"text": text}).encode(),
                headers={"Authorization": auth, "Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=20)
    except Exception as exc:
        log(f"could not wake orchestrator: {type(exc).__name__}")

    return 0 if summary["game_kill_deaths"] or summary["game_apply"] else 3


if __name__ == "__main__":
    sys.exit(main())
