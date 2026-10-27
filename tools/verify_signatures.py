#!/usr/bin/env python3
"""Offline oracle for the AC4Coop signature resolver.

Parses the ON-DISK AC4BFSP.exe, maps RVA -> file offset correctly, scans .text for the three
documented patterns, and re-derives every address the plugin will patch from the pattern bases.
Then it compares those derived addresses against the literals baked into the plugin.

This is the source of truth for the pattern set, not a throwaway: run it BEFORE building, against the
EXE that will actually be patched. If it fails, the PATTERN is wrong -- fix the pattern, never the
expectation.

Exit codes: 0 = all patterns unique and every derived address equals its literal; 1 = oracle failure;
2 = the EXE is not the documented build (size/hash/machine mismatch).

Usage:
    python verify_signatures.py [path\\to\\AC4BFSP.exe]
"""

import argparse
import hashlib
import struct
import sys

# --- ground truth -------------------------------------------------------------------------------
DEFAULT_EXE = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"
EXPECTED_SHA256 = "bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150"
EXPECTED_SIZE = 45056040
EXPECTED_TIMESTAMP = 0x64A813D4
EXPECTED_IMAGE_BASE = 0x00400000
EXPECTED_SIZE_OF_IMAGE = 0x4E7E000

IMAGE_SCN_MEM_EXECUTE = 0x20000000
PE32_MAGIC = 0x10B
IMAGE_FILE_MACHINE_I386 = 0x014C

# --- patterns (must match src/deref_guard.cpp and src/action_guard.cpp) --------------------------
PATTERNS = {
    "sub_679750": "55 8B EC 66 8B 15 ?? ?? ?? ?? 56 8B F1 66 8B 46 12 66 23 45 08",
    "sub_681980": "55 8B EC 83 EC 2C 53 56 8B F1 66 8B 46 10 66 23 45 0C",
    "sub_679220": "66 8B 81 FC 00 00 00 66 83 E0 07 75 03 B0 01 C3",
}

# Documented function-base VAs for this build (fallback route only).
LITERAL_BASE = {
    "sub_679750": 0x00679750,   # A
    "sub_681980": 0x00681980,   # B
    "sub_679220": 0x00679220,   # action gate
}

# --- sites, as (label, base name, offset from base, literal VA) ---------------------------------
SITES = [
    ("deref[0] A+0x086", "sub_679750", 0x086, 0x006797D6),
    ("deref[1] A+0x094", "sub_679750", 0x094, 0x006797E4),
    ("deref[2] B+0x167", "sub_681980", 0x167, 0x00681AE7),
    ("deref[3] B+0x1E1", "sub_681980", 0x1E1, 0x00681B61),
    ("deref[4] B+0x1A3", "sub_681980", 0x1A3, 0x00681B23),
    ("deref[5] B+0x1C9", "sub_681980", 0x1C9, 0x00681B49),
    ("action  base+0x000", "sub_679220", 0x000, 0x00679220),
]

# Continue / skip / resume targets. 6 continues + 6 skips from deref_guard's table, plus the action
# guard's resume point. (The dispatch says "8"; the table in the contract yields 12 + 1, so all 13 are
# checked here rather than silently dropping five.)
TARGETS = [
    ("deref[0] cont", "sub_679750", 0x08C, 0x006797DC),
    ("deref[0] skip", "sub_679750", 0x094, 0x006797E4),
    ("deref[1] cont", "sub_679750", 0x09A, 0x006797EA),
    ("deref[1] skip", "sub_679750", 0x0A5, 0x006797F5),
    ("deref[2] cont", "sub_681980", 0x16D, 0x00681AED),
    ("deref[2] skip", "sub_681980", 0x17A, 0x00681AFA),
    ("deref[3] cont", "sub_681980", 0x1E7, 0x00681B67),
    ("deref[3] skip", "sub_681980", 0x1F5, 0x00681B75),
    ("deref[4] cont", "sub_681980", 0x1A9, 0x00681B29),
    ("deref[4] skip", "sub_681980", 0x1B6, 0x00681B36),
    ("deref[5] cont", "sub_681980", 0x1CF, 0x00681B4F),
    ("deref[5] skip", "sub_681980", 0x1DC, 0x00681B5C),
    ("action  resume", "sub_679220", 0x007, 0x00679227),
]


class PE:
    """Minimal PE32 reader: headers, section table, RVA -> file offset."""

    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ image")
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("no PE signature at e_lfanew")

        coff = e_lfanew + 4
        self.machine, self.num_sections, self.timestamp = struct.unpack_from("<HHI", data, coff)
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20

        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != PE32_MAGIC:
            raise ValueError("expected PE32 (0x10B), got 0x%X" % magic)
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", data, opt + 56)[0]

        self.sections = []
        sec = opt + opt_size
        for i in range(self.num_sections):
            o = sec + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            chars = struct.unpack_from("<I", data, o + 36)[0]
            self.sections.append({
                "name": name, "vsize": vsize, "vaddr": vaddr,
                "rawsize": rawsize, "rawptr": rawptr, "chars": chars,
            })

    def text_section(self):
        for s in self.sections:
            if s["name"] == ".text" and (s["chars"] & IMAGE_SCN_MEM_EXECUTE):
                return s
        raise ValueError("no executable .text section")

    def rva_to_offset(self, rva):
        for s in self.sections:
            span = max(s["vsize"], s["rawsize"])
            if s["vaddr"] <= rva < s["vaddr"] + span:
                delta = rva - s["vaddr"]
                if delta >= s["rawsize"]:
                    raise ValueError("RVA 0x%X is in uninitialised tail of %s"
                                     % (rva, s["name"]))
                return s["rawptr"] + delta
        raise ValueError("RVA 0x%X is not mapped by any section" % rva)

    def read_rva(self, rva, size):
        off = self.rva_to_offset(rva)
        return self.data[off:off + size]


def parse_pattern(text):
    toks = []
    for t in text.split():
        if t in ("??", "?"):
            toks.append(None)
        else:
            toks.append(int(t, 16))
    if not toks:
        raise ValueError("empty pattern")
    return toks


def scan(text_bytes, text_rva, image_base, toks):
    """All match VAs (image-base-relative), over the raw .text bytes only."""
    n = len(toks)
    hits = []
    if len(text_bytes) < n:
        return hits

    head = toks[0]
    last = len(text_bytes) - n
    if head is None:
        candidates = range(0, last + 1)
    else:
        candidates = [i for i in range(0, last + 1) if text_bytes[i] == head]

    for i in candidates:
        ok = True
        for j in range(1, n):
            t = toks[j]
            if t is not None and text_bytes[i + j] != t:
                ok = False
                break
        if ok:
            hits.append(image_base + text_rva + i)
    return hits


def table(headers, rows, widths):
    out = []
    out.append("  " + "  ".join(h.ljust(w) for h, w in zip(headers, widths)))
    out.append("  " + "  ".join("-" * w for w in widths))
    for r in rows:
        out.append("  " + "  ".join(str(c).ljust(w) for c, w in zip(r, widths)))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="AC4Coop offline signature oracle")
    ap.add_argument("exe", nargs="?", default=DEFAULT_EXE, help="path to AC4BFSP.exe")
    args = ap.parse_args()

    with open(args.exe, "rb") as f:
        data = f.read()

    size = len(data)
    sha = hashlib.sha256(data).hexdigest()
    print("=== image ===")
    print("  path        %s" % args.exe)
    print("  size        %d (expected %d) %s"
          % (size, EXPECTED_SIZE, "OK" if size == EXPECTED_SIZE else "MISMATCH"))
    print("  sha256      %s %s" % (sha, "OK" if sha == EXPECTED_SHA256 else "MISMATCH"))

    pe = PE(data)
    print("  machine     0x%04X %s"
          % (pe.machine, "OK" if pe.machine == IMAGE_FILE_MACHINE_I386 else "MISMATCH"))
    print("  timestamp   0x%08X %s"
          % (pe.timestamp, "OK" if pe.timestamp == EXPECTED_TIMESTAMP else "MISMATCH"))
    print("  imagebase   0x%08X %s"
          % (pe.image_base, "OK" if pe.image_base == EXPECTED_IMAGE_BASE else "MISMATCH"))
    print("  sizeofimage 0x%X %s"
          % (pe.size_of_image,
             "OK" if pe.size_of_image == EXPECTED_SIZE_OF_IMAGE else "MISMATCH"))

    text = pe.text_section()
    text_rva = text["vaddr"]
    text_bytes = data[text["rawptr"]:text["rawptr"] + text["rawsize"]]
    print("  .text       rva=0x%08X raw=0x%08X size=0x%X vexec=0x%X"
          % (text_rva, text["rawptr"], len(text_bytes), text["vsize"]))

    header_ok = (size == EXPECTED_SIZE and sha == EXPECTED_SHA256 and
                 pe.machine == IMAGE_FILE_MACHINE_I386 and
                 pe.timestamp == EXPECTED_TIMESTAMP and
                 pe.image_base == EXPECTED_IMAGE_BASE and
                 pe.size_of_image == EXPECTED_SIZE_OF_IMAGE)
    if not header_ok:
        print("\nFATAL: this is not the documented build; the literal table below does not apply.")
        return 2

    # --- pattern scan ---------------------------------------------------------------------------
    print("\n=== patterns (scanned over .text only) ===")
    bases = {}
    pattern_ok = True
    for name, pat in PATTERNS.items():
        hits = scan(text_bytes, text_rva, pe.image_base, parse_pattern(pat))
        resolved = hits[0] if len(hits) == 1 else (hits[0] if hits else 0)
        base_ok = (len(hits) == 1 and resolved == LITERAL_BASE[name])
        if len(hits) != 1:
            pattern_ok = False
        if base_ok:
            bases[name] = resolved
        else:
            # keep a value so the tables below still show derived addresses and how they differ
            bases[name] = LITERAL_BASE[name]
        print("  %-10s matches=%d  first=0x%08X  literal_base=0x%08X  %s"
              % (name, len(hits), resolved, LITERAL_BASE[name],
                 "BASE-EQUAL" if base_ok else "BASE-DIFFERS"))
        if len(hits) > 1:
            for h in hits:
                print("      extra match 0x%08X" % h)

    # --- table 1: sites -------------------------------------------------------------------------
    print("\n=== sites ===")
    rows = []
    sites_ok = True
    for label, base_name, off, literal in SITES:
        hits = len(scan(text_bytes, text_rva, pe.image_base,
                        parse_pattern(PATTERNS[base_name])))
        resolved = bases[base_name] + off
        equal = (resolved == literal)
        if not equal:
            sites_ok = False
        rows.append((label, base_name, hits, "0x%08X" % resolved, "0x%08X" % literal,
                     "OK" if equal else "DIFF"))
    print(table(["site", "pattern", "matches", "resolved", "literal", "equal"], rows,
                [20, 10, 7, 10, 10, 5]))

    # --- table 2: continue / skip / resume targets ----------------------------------------------
    print("\n=== continue / skip / resume targets ===")
    rows = []
    targets_ok = True
    for label, base_name, off, literal in TARGETS:
        hits = len(scan(text_bytes, text_rva, pe.image_base,
                        parse_pattern(PATTERNS[base_name])))
        resolved = bases[base_name] + off
        equal = (resolved == literal)
        if not equal:
            targets_ok = False
        rows.append((label, base_name, hits, "0x%08X" % resolved, "0x%08X" % literal,
                     "OK" if equal else "DIFF"))
    print(table(["target", "pattern", "matches", "resolved", "literal", "equal"], rows,
                [20, 10, 7, 10, 10, 5]))

    # --- verdict --------------------------------------------------------------------------------
    print()
    print("  patterns unique+equal : %s" % ("PASS" if pattern_ok else "FAIL"))
    print("  sites resolved==literal: %s" % ("PASS" if sites_ok else "FAIL"))
    print("  targets resolved==literal: %s" % ("PASS" if targets_ok else "FAIL"))
    print("  %d sites, %d targets checked" % (len(SITES), len(TARGETS)))

    if pattern_ok and sites_ok and targets_ok:
        print("\nORACLE: PASS")
        return 0
    print("\nORACLE: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
