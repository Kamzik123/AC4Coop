#pragma once

#include <windows.h>
#include <cstdint>

// Byte-pattern scanner over an already-mapped PE module.
//
// Why this exists: the plugin's patch sites used to be absolute VAs. A VA is only correct for the one
// build it was read from; a signature is correct for any build that kept the code. Callers resolve a
// site by pattern first and fall back to the literal VA only when the build is known.
//
// Scope is deliberately narrow:
//   - the in-memory PE is walked properly (IMAGE_DOS_HEADER -> IMAGE_NT_HEADERS -> section table);
//   - only the section named ".text" with IMAGE_SCN_MEM_EXECUTE is scanned, nothing else;
//   - the whole section is always scanned, because the caller needs the match COUNT (a pattern that
//     matches twice is ambiguous and must be rejected, not silently taken first).
namespace PatternScan {
    // Wildcards are "??". Scans ONLY the module's .text section. Returns the first match VA or 0;
    // `matches` receives the total number of matches found (all of them, not just the first).
    // Returns 0 and sets *matches = 0 for a malformed pattern or a module that is not a PE32 image.
    uintptr_t Find(const HMODULE module, const char* pattern, int* matches);
}
