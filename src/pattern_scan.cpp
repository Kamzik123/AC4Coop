// Byte-pattern scanner -- see pattern_scan.h.
//
// The pattern text is "55 8B EC ?? ?? 56": hex bytes separated by spaces, `??` (or a lone `?`) for a
// wildcard. Parsing is strict -- a truncated or non-hex token rejects the whole pattern rather than
// scanning for a shorter prefix, because a silently-shortened signature is exactly the kind of thing
// that resolves to the wrong address.
//
// The module is read where Windows mapped it, so section VirtualAddress values are already RVAs from
// the module base; no RVA->file-offset mapping is needed here. That mapping only matters for the
// on-disk oracle (tools/verify_signatures.py).

#include "pattern_scan.h"

#include <windows.h>
#include <cstring>

namespace {

const int MAX_TOKENS = 512;

struct Token {
    BYTE value;
    bool wildcard;
};

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns the token count, or -1 on a malformed pattern.
int ParsePattern(const char* text, Token* out, int cap)
{
    int n = 0;
    const char* s = text;
    while (*s) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        if (n >= cap) return -1;

        if (s[0] == '?' && s[1] == '?') {
            out[n].wildcard = true;
            out[n].value = 0;
            s += 2;
        } else if (s[0] == '?' && (s[1] == ' ' || s[1] == '\t' || s[1] == 0)) {
            out[n].wildcard = true;
            out[n].value = 0;
            s += 1;
        } else {
            const int hi = HexNibble(s[0]);
            const int lo = HexNibble(s[1]);
            if (hi < 0 || lo < 0) return -1;
            out[n].wildcard = false;
            out[n].value = (BYTE)((hi << 4) | lo);
            s += 2;
        }
        ++n;
    }
    return n;
}

// The single executable section named ".text", or nullptr.
const IMAGE_SECTION_HEADER* FindTextSection(const IMAGE_NT_HEADERS* nt)
{
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

        char name[9];
        memset(name, 0, sizeof(name));
        memcpy(name, sec->Name, sizeof(sec->Name));
        if (strcmp(name, ".text") != 0) continue;

        return sec;
    }
    return nullptr;
}

} // namespace

uintptr_t PatternScan::Find(const HMODULE module, const char* pattern, int* matches)
{
    if (matches) *matches = 0;
    if (!module || !pattern || !*pattern) return 0;

    Token tokens[MAX_TOKENS];
    const int count = ParsePattern(pattern, tokens, MAX_TOKENS);
    if (count <= 0) return 0;

    const BYTE* base = (const BYTE*)module;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_SECTION_HEADER* text = FindTextSection(nt);
    if (!text) return 0;

    const BYTE*   start = base + text->VirtualAddress;
    const size_t  bytes = text->Misc.VirtualSize ? text->Misc.VirtualSize : text->SizeOfRawData;
    if (bytes < (size_t)count) return 0;

    const size_t last = bytes - (size_t)count;
    uintptr_t first = 0;
    int found = 0;

    for (size_t i = 0; i <= last; ++i) {
        // Cheap reject on the first token, which is the common case for a prologue signature.
        if (!tokens[0].wildcard && start[i] != tokens[0].value) continue;

        size_t k = 1;
        for (; k < (size_t)count; ++k) {
            if (tokens[k].wildcard) continue;
            if (start[i + k] != tokens[k].value) break;
        }
        if (k != (size_t)count) continue;

        if (found == 0) first = (uintptr_t)(start + i);
        ++found;
    }

    if (matches) *matches = found;
    return first;
}
