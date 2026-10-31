"""Dump the structures the co-op work depends on, with member offsets, from the PS3 DWARF.

Writes one file per target so results survive partial scans:
    ops/evidence/ps3_structs/<Name>.txt

    python ps3_structs.py            # all targets
    python ps3_structs.py Entity     # one
"""
import os
import re
import sys

from elftools.elf.elffile import ELFFile

ELF = r"D:\Games\acbf_ps3_gold\scimitar_final.elf"
OUT = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ops\evidence\ps3_structs"

TARGETS = [
    "Entity", "Component", "Human", "CharacterController", "Actor", "Faction",
    "BhvGenericNPC", "BhvAssassin", "AbstractEntityAI", "HumanDataBundle",
    "CSrvNPCHealth", "EntityManager", "World", "VisualTargetingComponent",
    "PerceptionComponent", "PlayerPerceptionMonitor", "WeaponComponent",
]


def safe(name):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:120]


def main(argv):
    want = argv[1:] if len(argv) > 1 else None
    os.makedirs(OUT, exist_ok=True)
    f = open(ELF, "rb")
    elf = ELFFile(f)
    dwarf = elf.get_dwarf_info()

    hits = {}
    for cu in dwarf.iter_CUs():
        for d in cu.iter_DIEs():
            if d.tag not in ("DW_TAG_structure_type", "DW_TAG_class_type"):
                continue
            n = d.attributes.get("DW_AT_name")
            if not n:
                continue
            name = n.value.decode("utf-8", "replace")
            short = name.split("::")[-1]
            if want and not any(w.lower() == short.lower() or w.lower() == name.lower() for w in want):
                continue
            if not want and short not in TARGETS:
                continue
            members = []
            for c in d.iter_children():
                if c.tag != "DW_TAG_member":
                    continue
                mn = c.attributes.get("DW_AT_name")
                mn = mn.value.decode("utf-8", "replace") if mn else "?"
                off = c.attributes.get("DW_AT_data_member_location")
                o = off.value if off is not None else "?"
                if isinstance(o, int):
                    o = hex(o)
                elif isinstance(o, (list, tuple)):
                    o = "loc[" + ",".join(str(x) for x in o)[:40] + "]"
                else:
                    o = str(o)[:40]
                members.append((o, mn))
            if not members:
                continue
            size = d.attributes.get("DW_AT_byte_size")
            size = size.value if size else "?"
            hits.setdefault(name, (size, members))

    for name, (size, members) in hits.items():
        p = os.path.join(OUT, safe(name) + ".txt")
        with open(p, "w", encoding="utf-8") as w:
            w.write(f"{name}  size={size}  members={len(members)}\n")
            for o, mn in members:
                w.write(f"  +{o:<10} {mn}\n")
    print(f"structs written: {len(hits)} -> {OUT}")
    for name, (size, members) in sorted(hits.items(), key=lambda kv: -len(kv[1][1]))[:15]:
        print(f"  {name:45} size={size:<8} members={len(members)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
