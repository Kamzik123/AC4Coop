import glob
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
GAME = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag"
PY = sys.executable
POS = (43.5, -73.4, 3.6)          # from the peer's own decode
ROW = re.compile(r"\[Id\] idx=(\d+) objId=(\d+) ent=([0-9A-Fa-f]+).*?pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")

log = max(glob.glob(os.path.join(GAME, "phase1_log_*.txt")), key=os.path.getmtime)
rows = []
for line in open(log, encoding="utf-8", errors="replace"):
    m = ROW.search(line)
    if m:
        rows.append((m.group(2), (float(m.group(4)), float(m.group(5)), float(m.group(6)))))

if not rows:
    print("no [Id] rows")
    sys.exit(1)

rows.sort(key=lambda r: sum((a - b) ** 2 for a, b in zip(r[1], POS)))
print("nearest candidates:")
for oid, p in rows[:5]:
    d = sum((a - b) ** 2 for a, b in zip(p, POS)) ** 0.5
    print(f"  objId={oid:>12}  pos={p}  {d:.1f} m")

target = rows[0][0]
print(f"\ntarget objId={target}")
cmd = [PY, "ops/tools/peer9.py", "--walk", "--echo-pos", "--duration", "40", "--kill", target]
print("run:", " ".join(cmd))
r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
out = (r.stdout or "") + (r.stderr or "")
print("\n".join(out.splitlines()[-6:]))
