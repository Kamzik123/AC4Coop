"""Restore the AC4 BF IDB function list from the symbol manifests.

Background: the shared DB lost its function records (128,610 -> 964) when it was
opened from a second process without auto-analysis. Instruction bytes, names and
comments are intact; only the function boundaries are gone.

This re-adds every address known from RE-AC4/symbols/functions*.jsonl, then saves.
It must be the ONLY process touching the DB - check idb_list first.

    python restore_funcs.py            # dry run, prints what it would do
    python restore_funcs.py --apply
"""
import json
import os
import sys

SYM = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\RE-AC4\symbols"
DB = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"


def collect():
    seen = set()
    for name in ("functions.jsonl", "functions_v2.jsonl"):
        p = os.path.join(SYM, name)
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                a = rec.get("addr")
                if isinstance(a, str) and a.startswith("0x"):
                    seen.add(int(a, 16))
    return sorted(seen)


def main(argv):
    apply_ = "--apply" in argv
    addrs = collect()
    print(f"addresses from manifests: {len(addrs)}  range {addrs[0]:#x}..{addrs[-1]:#x}")
    if not apply_:
        print("dry run - pass --apply to write")
        return 0

    import idapro
    import ida_funcs
    import ida_auto
    import ida_idaapi

    rc = idapro.open_database(DB, False)
    print("open_database rc =", rc)
    if rc != 0:
        print("FAILED to open; abort (do not save)")
        return 1
    try:
        added = present = failed = 0
        for a in addrs:
            if ida_funcs.get_func(a):
                present += 1
                continue
            if ida_funcs.add_func(a):
                added += 1
            else:
                failed += 1
        print(f"already present={present} added={added} failed={failed}")
        # let analysis settle the new boundaries
        ida_auto.auto_wait()
        print("auto_wait done")
    finally:
        idapro.close_database(True)
        print("database saved")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
