"""Fuzzy function matching with correct PE section mapping.

Normalises the first N instructions of a known-build function to (mnemonic + operand kinds),
then scores that stream against every plausible function start in the target build.
Reports the best candidates with a score so a human can confirm by decompiling.

    python fuzzy_match.py --target <exe> --sites "0x70e810,0x528200"
"""
import os
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

KNOWN = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"
N_INSNS = 16
PROLOGUES = (b"\x55\x8b\xec", b"\x8b\xff\x55\x8b\xec", b"\x53", b"\x56", b"\x57",
             b"\x83\xec", b"\x81\xec", b"\x6a\xff\x68", b"\x8b\xc1", b"\x8b\x44\x24")


def tokens(md, pe, va, n=N_INSNS):
    code = pe.bytes_at_va(va, 128)
    if len(code) < 8:
        return []
    out = []
    for ins in md.disasm(code, va):
        ops = []
        for op in ins.operands:
            ops.append({1: "r", 2: "i", 3: "m"}.get(op.type, "?"))
        out.append(ins.mnemonic + ":" + ",".join(ops))
        if len(out) >= n:
            break
    return out


def score(a, b):
    if not a or not b:
        return 0.0
    common = min(len(a), len(b))
    same = sum(1 for i in range(common) if a[i] == b[i])
    return same / max(len(a), len(b))


def main(argv):
    target = None
    sites = []
    for i, a in enumerate(argv):
        if a == "--target" and i + 1 < len(argv):
            target = argv[i + 1]
        if a == "--sites" and i + 1 < len(argv):
            sites = [int(x, 16) for x in argv[i + 1].split(",") if x]
    if not target or not sites:
        print(__doc__)
        return 2

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    kpe, tpe = PE(KNOWN), PE(target)
    print(f"known image base {kpe.image_base:#x}  target {tpe.image_base:#x}")
    print("target sections:", ", ".join(f"{s['name']}({s['vaddr']:#x}/{s['rawptr']:#x})"
                                        for s in tpe.sections if s["chars"] & 0x20000000))

    # candidate function starts: prologue patterns inside the target's code sections
    cands = set()
    for s in tpe.sections:
        if not (s["chars"] & 0x20000000):
            continue
        blob = tpe.data[s["rawptr"]: s["rawptr"] + s["rawsize"]]
        for pat in PROLOGUES:
            start = 0
            while True:
                i = blob.find(pat, start)
                if i < 0:
                    break
                cands.add(tpe.image_base + s["vaddr"] + i)
                start = i + 1
    print(f"candidate function starts in target: {len(cands)}")

    for va in sites:
        tmpl = tokens(md, kpe, va)
        if not tmpl:
            print(f"\n{va:#x}: no template (not code in the known build?)")
            continue
        best = []
        for c in cands:
            sc = score(tmpl, tokens(md, tpe, c))
            if sc >= 0.5:
                best.append((sc, c))
        best.sort(reverse=True)
        print(f"\n{va:#x}  template={tmpl[:6]}â€¦")
        for sc, c in best[:6]:
            print(f"   score={sc:.2f}  {c:#x}")
        if not best:
            print("   nothing above 0.50")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
