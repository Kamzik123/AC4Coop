# ops/tools/e2_site_patterns.py   <path to AC4BFSP.exe>
# Offline oracle for stage2a-observe: proves each E2 site pattern is unique in .text and sits at
# the documented VA on the pinned build. No IDA, no game.
import hashlib, struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:2] == b'MZ'
e = struct.unpack_from('<I', d, 0x3C)[0]
n = struct.unpack_from('<H', d, e + 6)[0]
o = struct.unpack_from('<H', d, e + 20)[0]
opt = e + 24
base = struct.unpack_from('<I', d, opt + 28)[0]
secs = []
for i in range(n):
    p = opt + o + i * 40
    secs.append((struct.unpack_from('<I', d, p + 12)[0],   # VirtualAddress
                 struct.unpack_from('<I', d, p + 16)[0],   # SizeOfRawData
                 struct.unpack_from('<I', d, p + 20)[0]))  # PointerToRawData
tva, tsz, tptr = secs[0]                                   # .text is section 0 on this build
blob = d[tptr:tptr + tsz]
print('sha256', hashlib.sha256(d).hexdigest())
print('text   0x%08X..0x%08X (%d bytes)' % (base + tva, base + tva + tsz, tsz))

PAT = {
 0x017106C0: "55 8B EC 8B 45 08 8B 80 9C 00 00 00 8B D0 80 E2",
 0x0173E190: "55 8B EC 83 EC 24 53 56 57 8B 7D 08 8B D9 8B CF",
 0x019FB4C0: "55 8B EC 56 8B F1 E8 55 40 20 00 85 C0 8B 45 08",
 0x0133F550: "55 8B EC 56 8B 75 08 85 F6 75 05 32 C0 5E 5D C3 56 E8 8A 30 3C FF 83 C4",
}
for va, p in sorted(PAT.items()):
    b = bytes(int(x, 16) for x in p.split())
    pos = blob.find(b)
    first = pos
    c = 0
    while pos >= 0:
        c += 1
        pos = blob.find(b, pos + 1)
    ok = (c == 1 and first == va - base - tva)
    print('%08X  len=%2d  matches=%d  at_va=%s  %s' %
          (va, len(b), c, 'yes' if first == va - base - tva else 'NO', 'OK' if ok else 'FAIL'))
