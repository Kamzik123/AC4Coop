import json

p = "ops/STATE.json"
st = json.load(open(p, encoding="utf-8"))
st["build"] = {
    "asi_head": "96bbcc6034be2932",
    "asi_deployed": "96bbcc6034be2932",
    "exe": "bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150",
    "commit": "920ee69",
    "branch": "fix/live-game-udp-bind",
    "dirty": [],
    "note": "docs-only change since 8a6efd3. 22 audit-tracked sites aligned.",
}
st["audit"] = {"claims": "31/35",
               "gates": {"G0": "PASS", "G1": "PASS", "G2": "PASS", "G3": "FAIL", "G4": "PASS", "G5": "PASS"}}
st["facts"]["padid_contract"] = ("__thiscall(ecx=this, BYTE* out, retn 4): returns 1 on success with the first "
                                 "null slot index in *out; returns 0 when no slot is free and leaves *out "
                                 "unwritten. The sole caller pre-zeroes *out and ignores the return, so "
                                 "'no slot' silently means slot 0 = the real player's pad.")
st["next"] = [
    "operator launch: E0-B kill lane + [AD] capture (ops/NEXT-RUN.md)",
    "after that: D-02/D-03/D-04 close from the same logs",
    "D-01 stays open until a second machine exists",
    "BIN: settle entity+0x0C (in flight)",
]
json.dump(st, open(p, "w", encoding="utf-8", newline="\n"), indent=2)
print("STATE updated")
