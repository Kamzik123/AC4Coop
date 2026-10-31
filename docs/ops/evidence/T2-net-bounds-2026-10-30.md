# T2 evidence — SEAT NET (2026-10-30)

Build: `2D8E2E4DC4991F42525C619B9A572A22348561E9D2D25EEFD2E500000B39CF71`, 301568 B
`ref/AC4Coop/bin/AC4Coop.asi` (MSBuild Release|x86, PlatformToolset=v143, 0 warnings/errors).
Tree built: `src/net.cpp` 965F9D0E..., `src/net.h` C6297F83..., plus IMPL's uncommitted
`src/phase1.cpp` CE567159... (commit aa8637c, tree=dirty).
Pre-edit `net.cpp` was CACD7F5F... (backup held in %TEMP% only, not in the repo).

## Item 1 — EncodeEntityPacket bounds (net.cpp:179-193)

Static parity oracle: every field reserved by the new `need +=` lines equals the bytes the
record body actually writes, for every mask; the fixed part reserves 16 B and writes 16 B.

```
reserved per field: {'EF_POS': 12, 'EF_QUAT': 16, 'EF_ACTION': 4, 'EF_HULLHP': 4, 'EF_HP': 2, 'EF_ARCH': 2, 'EF_DEAD': 1, 'EF_STAGE': 1}
written  per field: {'EF_POS': 12, 'EF_QUAT': 16, 'EF_ACTION': 4, 'EF_HULLHP': 4, 'EF_HP': 2, 'EF_ARCH': 2, 'EF_DEAD': 1, 'EF_STAGE': 1}
worst-case record = 16 + 42 = 58 B
old check reserved 16 B -> up to 42 B written past cap
worst snapshot at MAX_SYNC_ENTITIES=128: 20 + 128*58 = 7444 B (g_txBuf is 16384 B)
PARITY: PASS
```
Note the honest scope: with `g_txBuf` = 16384 B and `MAX_SYNC_ENTITIES` = 128 the old check was
**latent** (max possible `o` was 7444 B), so this was never reachable through `Net::SendEntities`.
It is a real bug for any caller with a smaller `cap` (and for `SelfTestWire`), and the old
`return 0` also left `g_txShadow` advanced for records that were never put on the wire.

Reproduce: script body is at the bottom of this file.

## Items 2-4 — snapshot TTL, kill-ring drop log, heartbeat liveness

- net.cpp:24-25 `g_entitiesMs` + `NET_ENTITIES_TTL_MS` (3000)
- net.cpp:371-374 snapshot-slot write stamps the tick
- net.cpp:521-542 `GetEntities` expires the slot after 3 s, clears `g_haveEntities`, logs once
- net.cpp:366 counts a ring overwrite, net.cpp:383-392 logs the first 8 (`NET_KILL_DROP_LOGS`)
- net.cpp:376-379 a header-only entity packet (`count==0`) no longer refreshes `g_lastRecvMs`

Binary contains the new log strings (proves the build carries the patch):
`Select-String bin\AC4Coop.asi -Pattern 'kill ring FULL','entity snapshot expired'` -> both found.

## Not proven

No in-game run. The Phase-2 codec selftest (`[Net] wire codec selftest: roundtrip=...`) only runs
at plugin startup, inside the game; that is QA/T7's oracle, not this seat's.

## Checker script (verbatim)

```python
"""Static oracle for T2 item 1 (EncodeEntityPacket bounds).

Reads ref/AC4Coop/src/net.cpp and, for every field bit, compares
  (a) the bytes RESERVED by the new `need +=` lines, against
  (b) the bytes actually WRITTEN by the `o +=` lines in the record body.
They must be identical for every mask, and the fixed part must be 16.
Also checks the worst case against the buffer actually used by SendEntities.
"""
import re
import sys

SRC = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ref\AC4Coop\src\net.cpp"
text = open(SRC, encoding="utf-8", errors="replace").read()
lines = text.splitlines()

# the record body: from the `need = 16;` line to the end of the EF_STAGE write
start = next(i for i, l in enumerate(lines) if "size_t need = 16;" in l)
end = next(i for i, l in enumerate(lines) if i > start and "if (mask & EF_STAGE)  { out[o++]" in l)
body = lines[start:end + 1]

res = {}
wrote = {}
for l in body:
    m = re.search(r"if \(mask & (EF_\w+)\)\s+need \+= (\d+);", l)
    if m:
        res[m.group(1)] = int(m.group(2))
    m = re.search(r"if \(mask & (EF_\w+)\)\s+\{ std::memcpy\(out \+ o,.*?(\d+)\); o \+= (\d+); \}", l)
    if m:
        wrote[m.group(1)] = int(m.group(3))
    m = re.search(r"if \(mask & (EF_\w+)\)\s+\{ out\[o\+\+\]", l)
    if m:
        wrote[m.group(1)] = 1

fixed_res = int(re.search(r"size_t need = (\d+);", body[0]).group(1))
# fixed part written: key(4)+kind(1)+idKind(1)+mask(2)+wireId(8)
fixed_written = 4 + 1 + 1 + 2 + 8

print("reserved per field:", res)
print("written  per field:", wrote)
ok = True
for bit in sorted(set(res) | set(wrote)):
    if res.get(bit) != wrote.get(bit):
        ok = False
        print(f"MISMATCH {bit}: reserve {res.get(bit)} write {wrote.get(bit)}")
if fixed_res != fixed_written:
    ok = False
    print(f"MISMATCH fixed part: reserve {fixed_res} write {fixed_written}")
elif fixed_res != 16:
    ok = False
    print(f"MISMATCH fixed part is {fixed_res}, expected 16")

# the old check reserved the fixed part only; show what that left uncovered
coverable = sum(res.values())
print(f"worst-case record = {fixed_res} + {coverable} = {fixed_res + coverable} B")
print(f"old check reserved {fixed_written} B -> up to {coverable} B written past cap")
maxrec = fixed_res + coverable
print(f"worst snapshot at MAX_SYNC_ENTITIES=128: 20 + 128*{maxrec} = {20 + 128 * maxrec} B "
      f"(g_txBuf is 16384 B)")
print("PARITY:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
```
