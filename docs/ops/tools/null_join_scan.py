#!/usr/bin/env python3
"""Find the class of bug that crashed at 0x681817, across the whole .text.

The anti-pattern is:

    test reg, reg          ; 85 /r with mod=11
    jz   L                 ; 74 rel8  or  0F 84 rel32
    ...
L:  <first instruction dereferences reg>      <-- the null path lands here

0x681817 is one instance (`test edi,edi; jz 0x681817; movzx ecx,[edi+0x2B6]`).
This scanner enumerates every instance so the guard strategy is chosen from a
count, not from the last crash.

    python ops/tools/null_join_scan.py [--all] [--json out.json]

A hit is a *candidate*: the register can be reloaded before the target in some
cases, and a linear sweep can start decoding mid-instruction. Every hit needs one
of two things before it is treated as real: a disassembly read, or an observer log
line showing a NULL actually arriving there.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "audit", "tools"))
from ac4audit.pe import PE  # noqa: E402

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    raise SystemExit("capstone is required: pip install capstone")

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def s8(b):
    return b - 256 if b >= 128 else b


def text_of(pe):
    seg = [s for s in pe.segments if s["name"] == ".text"][0]
    return seg["va"], pe.data[seg["rawptr"]:seg["rawptr"] + seg["rawsize"]]


def derefs(ins, reg):
    """True if this instruction reads/writes [reg+disp] (not [reg] alone as a call)."""
    if ins.mnemonic in ("call", "jmp"):
        return False
    op = ins.op_str.replace(" ", "")
    pat = f"[{reg}+" if reg not in ("esp",) else f"[{reg}+"
    if pat in op:
        return True
    if f"[{reg}-" in op:
        return True
    if f"[{reg}]" in op and ins.mnemonic in ("mov", "movzx", "movsx", "cmp", "add", "sub",
                                             "or", "and", "movss", "fld", "fstp", "inc", "dec"):
        return True
    return False


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="print every hit, not the first 40")
    ap.add_argument("--json", default="")
    ap.add_argument("--config", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "audit", "config", "project.json"))
    args = ap.parse_args(argv)

    cfg = json.load(open(args.config, encoding="utf-8"))
    pe = PE(os.path.join(cfg["game_dir"], cfg["exe_name"]))
    base, blob = text_of(pe)
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    hits = []
    i = 0
    while i < len(blob) - 8:
        if blob[i] != 0x85 or (blob[i + 1] & 0xC0) != 0xC0:
            i += 1
            continue
        modrm = blob[i + 1]
        reg = REG32[(modrm >> 3) & 7]
        j = i + 2
        tgt = None
        if blob[j] == 0x74:
            tgt = base + j + 2 + s8(blob[j + 1])
        elif blob[j] == 0x0F and blob[j + 1] == 0x84:
            tgt = base + j + 6 + struct.unpack_from("<i", blob, j + 2)[0]
        if tgt is None or not (base <= tgt < base + len(blob)):
            i += 1
            continue
        off = tgt - base
        for ins in md.disasm(blob[off:off + 20], tgt):
            if derefs(ins, reg):
                hits.append({
                    "site": base + i,
                    "reg": reg,
                    "target": tgt,
                    "first_deref": f"{ins.address:08X} {ins.bytes.hex()} {ins.mnemonic} {ins.op_str}",
                    "disp": ins.op_str,
                })
            break  # only the first instruction at the target matters
        i += 1

    # keep one hit per (site) and drop hits whose site is inside another hit's target block
    uniq = {h["site"]: h for h in hits}
    hits = sorted(uniq.values(), key=lambda h: h["site"])

    print(f"scanned .text 0x{base:08X}..0x{base + len(blob):08X} ({len(blob):,} bytes)")
    print(f"test/jz-into-deref candidates: {len(hits)}")
    shown = hits if args.all else hits[:40]
    for h in shown:
        print(f"  site 0x{h['site']:08X}  test {h['reg']},{h['reg']}  "
              f"jz 0x{h['target']:08X}  ->  {h['first_deref']}")
    if len(hits) > len(shown):
        print(f"  ... {len(hits) - len(shown)} more (--all)")

    # how many of the known guarded/fixed sites are in the list?
    known = {0x681801: "0x681817 crash (fixed by caller wrap)",
             0x6797DC: "deref guard A (0x6797DC crash)"}
    found = {h["site"] for h in hits}
    print("\nknown-site cross-check:")
    for site, why in known.items():
        print(f"  {'IN LIST ' if site in found else 'not hit '} 0x{site:08X}  {why}")

    if args.json:
        json.dump({"text_base": f"0x{base:08X}", "count": len(hits), "hits": hits},
                  open(args.json, "w", encoding="utf-8"), indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
