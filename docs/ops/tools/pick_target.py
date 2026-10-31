#!/usr/bin/env python3
"""Pick a kill target for the E0-B synthetic-peer test from the game's own log.

The game logs its entity list with the id it will put on the wire:

    [Id] idx=3 objId=4026532617 ent=3409EA40 evt=01E4CE90 ... pos=(0.00,0.27,-0.83)

That objId is what the peer must echo back. This prints the candidates and the
exact command to run.

    python ops/tools/pick_target.py            # newest log
    python ops/tools/pick_target.py <log>      # a specific log
"""
from __future__ import annotations

import glob
import os
import re
import sys

GAME = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag"
ROW = re.compile(
    r"\[Id\] idx=(\d+) objId=(\d+) ent=([0-9A-Fa-f]+) evt=([0-9A-Fa-f]+)"
    r".*?pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")


def newest_log():
    logs = glob.glob(os.path.join(GAME, "phase1_log_*.txt"))
    if not logs:
        sys.exit("no phase1_log_*.txt in the game folder")
    return max(logs, key=os.path.getmtime)


def main(argv):
    path = argv[1] if len(argv) > 1 else newest_log()
    if not os.path.isfile(path):
        sys.exit(f"no such log: {path}")
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = ROW.search(line)
        if m:
            rows.append({
                "idx": int(m.group(1)),
                "objId": m.group(2),
                "ent": "0x" + m.group(3).upper(),
                "pos": (float(m.group(5)), float(m.group(6)), float(m.group(7))),
            })
    print(f"log: {os.path.basename(path)}")
    print(f"candidates: {len(rows)}")
    if not rows:
        sys.exit("no [Id] rows — the log predates the identity scan, or the run never reached the world")
    for r in rows:
        x, y, z = r["pos"]
        print(f"  idx={r['idx']:>2}  objId={r['objId']:>12}  ent={r['ent']}  "
              f"pos=({x:8.2f},{y:8.2f},{z:8.2f})")
    print("\nnote: objId 402653xxxx are minted in this session; a guard that streams in later")
    print("      may not appear here. If the kill misses, re-run after standing near the guard.")
    print("\ncommand:")
    print(f"  py ops/tools/peer9.py --kill {rows[0]['objId']} --duration 30")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
