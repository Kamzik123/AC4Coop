"""Disassembly-normalised signature matching between two builds of AC4BFSP.exe.

Raw byte matching fails on a recompiled build because relative call/jmp displacements and
absolute operands differ. So each site is disassembled and turned into a pattern where those
operands are wildcards, everything else (opcodes, register operands, small immediates) must match.

    python sig_match.py --target <exe>

Outputs ops/evidence/sig_map_v2.json and (optionally) a C++ header with the wildcard patterns.
"""
import json
import os
import re
import sys
import glob

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

KNOWN = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"
SRC = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ref\AC4Coop\src"
IMAGE_BASE = 0x400000
MAX_INSN = 14


def load(p):
    with open(p, "rb") as f:
        return f.read()


def collect_addrs():
    pats = [re.compile(r'base\(\)\s*\+\s*0x([0-9A-Fa-f]+)'),
            re.compile(r'\b0x([0-9A-Fa-f]{6,8})\b')]
    found = {}
    for path in glob.glob(os.path.join(SRC, "*.cpp")) + glob.glob(os.path.join(SRC, "*.h")):
        text = open(path, encoding="utf-8", errors="replace").read()
        for p in pats:
            for m in p.finditer(text):
                v = int(m.group(1), 16)
                if v < IMAGE_BASE:
                    v += IMAGE_BASE
                if IMAGE_BASE <= v < IMAGE_BASE + 0x3000000:
                    found.setdefault(v, os.path.basename(path))
    return found


def pattern_for(code, va, md):
    """Return (wildcard_mask_list) for the first MAX_INSN instructions at va."""
    out = []
    for insn in md.disasm(code[va - IMAGE_BASE: va - IMAGE_BASE + 64], va):
        raw = list(insn.bytes)
        wild = [False] * len(raw)
        # wildcard anything that encodes an address: call/jmp rel32, and imm32 that lands in-image
        if insn.mnemonic in ("call", "jmp", "jcc", "je", "jne", "ja", "jb", "jae", "jbe",
                             "jg", "jl", "jge", "jle", "js", "jns", "jo", "jno", "jp", "jnp"):
            for i in range(len(raw)):
                wild[i] = True          # rel8/rel32 differ between builds
        for i in range(len(raw) - 3):
            val = int.from_bytes(bytes(raw[i:i + 4]), "little")
            if IMAGE_BASE <= val < IMAGE_BASE + 0x2000000:
                for k in range(i, i + 4):
                    wild[k] = True
        out.append((raw, wild))
        if len(out) >= MAX_INSN:
            break
    return out


def build_matcher(pat):
    """Compile a wildcard instruction list into a callable: bytes,offset -> bool."""
    chunks = []
    for raw, wild in pat:
        lit = bytes(0 if w else b for b, w in zip(raw, wild))
        mask = bytes(0 if w else 0xFF for w in wild)
        chunks.append((lit, mask))
    return chunks


def scan(chunks, data):
    hits = []
    first_lit, first_mask = chunks[0]
    n = len(first_lit)
    start = 0
    while True:
        i = data.find(first_lit, start)
        if i < 0:
            break
        start = i + 1
        ok = True
        pos = i
        for lit, mask in chunks:
            seg = data[pos:pos + len(lit)]
            if len(seg) < len(lit):
                ok = False
                break
            for a, b, m in zip(seg, lit, mask):
                if (a & m) != (b & m):
                    ok = False
                    break
            if not ok:
                break
            pos += len(lit)
        if ok:
            hits.append(i)
            if len(hits) > 6:
                break
    return hits


def main(argv):
    target = None
    for i, a in enumerate(argv):
        if a == "--target" and i + 1 < len(argv):
            target = argv[i + 1]
    if not target:
        print(__doc__)
        return 2

    known, tgt = load(KNOWN), load(target)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False

    rows = []
    for va in sorted(collect_addrs()):
        if va - IMAGE_BASE + 80 >= len(known):
            continue
        pat = pattern_for(known, va, md)
        if not pat:
            rows.append({"known": va, "target": None, "n": 0, "insns": 0})
            continue
        hits = scan(build_matcher(pat), tgt)
        rows.append({"known": va, "target": (hits[0] + IMAGE_BASE) if hits else None,
                     "n": len(hits), "insns": len(pat)})

    ok = [r for r in rows if r["target"]]
    uniq = [r for r in rows if r["target"] and r["n"] == 1]
    print(f"located {len(ok)}/{len(rows)}   unique {len(uniq)}")
    for r in rows:
        t = f"{r['target']:#x}" if r["target"] else "NOT FOUND"
        print(f"  {r['known']:#9x} -> {t:>10}  hits={r['n']} insns={r['insns']}")

    out = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ops\evidence\sig_map_v2.json"
    json.dump({"known": KNOWN, "target": target, "rows": rows}, open(out, "w"), indent=1)
    print("map ->", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
