"""PE helpers: correct RVA -> file offset mapping, shared by the relocation tools.

The first versions of these tools indexed the file by (VA - ImageBase), which is only valid when a
section's file offset equals its RVA. That is not guaranteed, so every signature was being read
from the wrong bytes. This module fixes that.
"""
import struct

IMAGE_BASE_DEFAULT = 0x400000


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not a PE")
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("no PE header")
        coff = e_lfanew + 4
        nsec = struct.unpack_from("<H", d, coff + 2)[0]
        opt_size = struct.unpack_from("<H", d, coff + 16)[0]
        opt = coff + 20
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        sec = opt + opt_size
        self.sections = []
        for i in range(nsec):
            o = sec + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin-1", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append({
                "name": name, "vaddr": vaddr, "vsize": vsize,
                "rawptr": rawptr, "rawsize": rawsize, "chars": chars,
            })

    def rva_to_off(self, rva):
        for s in self.sections:
            span = max(s["vsize"], s["rawsize"])
            if s["vaddr"] <= rva < s["vaddr"] + span:
                delta = rva - s["vaddr"]
                if delta < s["rawsize"]:
                    return s["rawptr"] + delta
                return None
        return None

    def va_to_off(self, va):
        return self.rva_to_off(va - self.image_base)

    def off_to_va(self, off):
        for s in self.sections:
            if s["rawptr"] <= off < s["rawptr"] + s["rawsize"]:
                return self.image_base + s["vaddr"] + (off - s["rawptr"])
        return None

    def bytes_at_va(self, va, n):
        off = self.va_to_off(va)
        if off is None:
            return b""
        return self.data[off:off + n]

    def is_code_va(self, va):
        off = self.va_to_off(va)
        if off is None:
            return False
        for s in self.sections:
            if s["rawptr"] <= off < s["rawptr"] + s["rawsize"]:
                return bool(s["chars"] & 0x20000000)   # IMAGE_SCN_MEM_EXECUTE
        return False
