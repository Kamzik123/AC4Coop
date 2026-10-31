"""Targeted DWARF queries against the PS3 debug ELF.

Answers three questions directly instead of inferring them from bit masks:
  1. every enum whose name mentions Role / Kind / Faction / Enemy / Hostile  (name -> value)
  2. the struct that owns the word at +0x9C (kind/role) and +0x660
  3. scimitar::Faction layout

    python ps3_query.py enums
    python ps3_query.py struct <substring>
"""
import sys

from elftools.elf.elffile import ELFFile

ELF = r"D:\Games\acbf_ps3_gold\scimitar_final.elf"
KEY = ("role", "kind", "faction", "enemy", "hostile", "archetype", "variation")


def die_name(d):
    a = d.attributes.get("DW_AT_name")
    return a.value.decode("utf-8", "replace") if a else None


def main(argv):
    mode = argv[1] if len(argv) > 1 else "enums"
    f = open(ELF, "rb")
    elf = ELFFile(f)
    dwarf = elf.get_dwarf_info()
    print(f"# CU count {sum(1 for _ in dwarf.iter_CUs())}", flush=True)

    if mode == "enums":
        seen = 0
        for cu in dwarf.iter_CUs():
            for d in cu.iter_DIEs():
                if d.tag != "DW_TAG_enumeration_type":
                    continue
                n = (die_name(d) or "").lower()
                if not any(k in n for k in KEY):
                    continue
                items = []
                for c in d.iter_children():
                    if c.tag != "DW_TAG_enumerator":
                        continue
                    cn = die_name(c) or "?"
                    cv = c.attributes.get("DW_AT_const_value")
                    items.append(f"{cn}={cv.value if cv else '?'}")
                if not items:
                    continue
                seen += 1
                print(f"\nENUM {die_name(d)}  ({len(items)} values)")
                print("  " + ", ".join(items))
                if seen >= 60:
                    print("\n# stopped at 60 enums")
                    return 0
        return 0

    if mode == "struct":
        sub = (argv[2] if len(argv) > 2 else "Entity").lower()
        for cu in dwarf.iter_CUs():
            for d in cu.iter_DIEs():
                if d.tag not in ("DW_TAG_structure_type", "DW_TAG_class_type"):
                    continue
                n = die_name(d)
                if not n or sub not in n.lower():
                    continue
                size = d.attributes.get("DW_AT_byte_size")
                members = []
                for c in d.iter_children():
                    if c.tag != "DW_TAG_member":
                        continue
                    mn = die_name(c) or "?"
                    off = c.attributes.get("DW_AT_data_member_location")
                    mo = off.value if off else "?"
                    members.append(f"  +{mo if isinstance(mo,str) else hex(mo)}  {mn}")
                if members:
                    print(f"\nSTRUCT {n}  size={size.value if size else '?'}")
                    print("\n".join(members[:400]))
        return 0
    print("usage: enums | struct <substring>")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
