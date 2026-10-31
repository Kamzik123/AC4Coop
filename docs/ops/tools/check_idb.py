"""Check the AC4 BF IDB function list after a restore. Read-only."""
import sys

DB = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"
PROBES = [0x54EC00, 0x73B4B0, 0x5FAC00, 0x67B2A0, 0x5200C0, 0x504F60, 0x133F630]

import idapro
import ida_funcs
import ida_name

rc = idapro.open_database(DB, False)
print("open rc", rc)
if rc != 0:
    sys.exit(1)
try:
    for a in PROBES:
        f = ida_funcs.get_func(a)
        nm = ida_name.get_name(a) or ""
        print(f"{a:#010x}  func={'yes' if f else 'NO ':<3}  end={f.end_ea:#x}  name={nm}" if f
              else f"{a:#010x}  func=NO   name={nm}")
    # rough total
    n = 0
    ea = ida_funcs.get_next_func(0)
    while ea is not None and ea != ida_funcs.BADADDR and n < 300000:
        n += 1
        ea = ida_funcs.get_next_func(ea)
    print("total functions:", n)
finally:
    idapro.close_database(False)
