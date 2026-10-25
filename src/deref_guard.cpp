// Per-site null-deref guards for the co-op action-lane crash — see deref_guard.h for the rationale.
//
// Verified crash (dump 2026-10-25 17:53): EIP=0x006797DC, ECX=0 [node+0x114]==NULL, from sub_7342B0,
// node ESI=0x070F8A60. sub_679750 does `mov edx,[ecx]` with ECX loaded straight out of [esi+0x114].
//
// For each site we replace the 6-byte load with `E9 rel32` + `90` and reproduce it in a cave stub:
//
//   8B 8E <disp32>     mov  ecx,[esi+OFF]     ; the stolen instruction, re-executed verbatim
//   85 C9              test ecx,ecx
//   0F 84 <rel32>      jz   <record>          ; null -> record the hit, then the site's skip target
//   E9 <rel32>         jmp  SITE+6            ; non-null -> continue the original code
//
//   <record> (8 bytes away in the same cave, still call-free):
//   F0 FF 05 <imm32>   lock inc dword ptr [hits[i]]
//   89 35 <imm32>      mov  dword ptr [lastNode[i]], esi
//   E9 <rel32>         jmp  SKIP              ; the documented skip target for that site
//
// The jz lands on the recorder, which is data-only and ends with a jump to exactly the skip address
// from the table, so the observable control flow is the documented one; the recorder exists only so
// that "a guard fired" is observable at all without a call in the hot path (no SEH, no reentrancy,
// no stack assumptions). Counters are drained by a 5 s reporter thread.

#include "deref_guard.h"
#include "Hook.h"

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <share.h>
#include <cstdint>
#include <cstdarg>

namespace {

const uintptr_t VA_IMAGE_BASE = 0x00400000;   // module is not ASLR-relocated (see Hook.h)
const size_t    STEAL_LEN     = 6;            // mov ecx,[esi+disp32]
const size_t    STUB_SIZE     = 19;           // 6 + 2 + 6 + 5
const size_t    SHIM_SIZE     = 18;           // 7 + 6 + 5
const size_t    STUB_REGION   = 128;          // 6 * 19 = 114, rounded so shims start aligned
const size_t    CAVE_SIZE     = 512;
const size_t    SITE_COUNT    = 6;

// A rel32 patch reaches +-2 GB. Require a comfortable margin so the boundary itself is never used.
const uintptr_t MAX_CAVE_DELTA = 0x7FFF0000;

// Hint addresses tried, in order, if the first VirtualAlloc lands outside +-2 GB of the module.
const uintptr_t CAVE_HINTS[] = { 0x20000000, 0x24000000, 0x30000000, 0x50000000, 0x10000000 };

const char* INI_PATH = ".\\AC4Coop.ini";

// `mov ecx,[esi+0x114]` / `mov ecx,[esi+0x118]`. Matched before patching so a different build is
// never half-hooked; a mismatch is logged and that site is left untouched.
const BYTE EXP_114[STEAL_LEN] = { 0x8B, 0x8E, 0x14, 0x01, 0x00, 0x00 };
const BYTE EXP_118[STEAL_LEN] = { 0x8B, 0x8E, 0x18, 0x01, 0x00, 0x00 };

// The six sites. `cont` is SITE+6 (the untouched instruction after the stolen load); `skip` is the
// instruction a null pointer must resume at. Values are absolute VAs, verified from disassembly:
//
//   0x006797E4  the +0x118 load that follows the +0x114 call
//   0x006797F5  epilogue (`or word[esi+0xE],di`) of sub_679750
//   0x00681AFA  `push edi; push ebx; jmp 0x00681B61` inside sub_681980 — a null +0x114 must land
//               here, not at 0x00681B61, because that code pushes the argument pair the +0x118 call
//               expects; entering at 0x00681B61 directly would run the call with a stale pair
//   0x00681B36  post-call cmp/je 0x00681B75 / jmp 0x00681B5C for case 3
//   0x00681B5C  shared tail after case 4's call
//   0x00681B75  phase-2 epilogue
struct SiteDesc {
    uintptr_t    va;
    const BYTE*  expected;
    DWORD        off;
    uintptr_t    cont;
    uintptr_t    skip;
};

const SiteDesc kSites[SITE_COUNT] = {
    { 0x006797D6, EXP_114, 0x114, 0x006797DC, 0x006797E4 },
    { 0x006797E4, EXP_118, 0x118, 0x006797EA, 0x006797F5 },
    { 0x00681AE7, EXP_114, 0x114, 0x00681AED, 0x00681AFA },
    { 0x00681B61, EXP_118, 0x118, 0x00681B67, 0x00681B75 },
    { 0x00681B23, EXP_114, 0x114, 0x00681B29, 0x00681B36 },
    { 0x00681B49, EXP_114, 0x114, 0x00681B4F, 0x00681B5C },
};

bool     g_installed = false;
BYTE*    g_cave      = nullptr;
FILE*    g_log       = nullptr;

// Fire counters written by the cave (absolute imm32 data stores; must outlive the cave, so they live
// in this translation unit's data rather than in the executable cave).
volatile LONG g_hits[SITE_COUNT];
volatile LONG g_lastNode[SITE_COUNT];
LONG          g_logged[SITE_COUNT];
volatile LONG g_totalLogged = 0;

// Debugger + live-readable file, same shape as the other units' loggers. Called from the game thread
// (install time) and from the reporter thread only.
void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

// Emits the per-fire lines the cave counted: the first 20 hits per site, then every 500th.
void DrainCounters()
{
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!g_hits[i]) continue;

        LONG hits;
        LONG node = g_lastNode[i];
        __try {
            hits = g_hits[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        bool emit = false;
        if (g_logged[i] < 20 && hits > g_logged[i]) {
            g_logged[i] = hits;
            emit = true;
        } else if (hits >= g_logged[i] + 500) {
            g_logged[i] = hits;
            emit = true;
        }

        if (emit)
            Log("[DG] null target at site 0x%08X node=%p\n", (unsigned)kSites[i].va, (void*)node);
    }
}

unsigned __stdcall ReporterThread(void*)
{
    for (;;) {
        Sleep(5000);
        if (!g_installed) continue;
        __try {
            DrainCounters();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Never let the reporter take the process down.
        }
    }
    return 0;
}

int32_t Rel32(const void* fromNext, const void* target)
{
    return (int32_t)((intptr_t)target - (intptr_t)fromNext);
}

// A cave is usable only if every patched site can reach it with an E9 rel32.
bool CaveInRange(const BYTE* cave, const uintptr_t* siteAddrs, const bool* valid)
{
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!valid[i]) continue;
        uintptr_t d = (uintptr_t)cave > siteAddrs[i] ? (uintptr_t)cave - siteAddrs[i]
                                                     : siteAddrs[i] - (uintptr_t)cave;
        if (d >= MAX_CAVE_DELTA) return false;
    }
    return true;
}

BYTE* AllocateCave(const uintptr_t* siteAddrs, const bool* valid)
{
    BYTE* cave = (BYTE*)VirtualAlloc(nullptr, CAVE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (cave && CaveInRange(cave, siteAddrs, valid)) {
        Log("[DG] cave (unhinted) = %p\n", cave);
        return cave;
    }
    if (cave) {
        Log("[DG] cave %p out of rel32 range, retrying with hints\n", cave);
        VirtualFree(cave, 0, MEM_RELEASE);
    }

    for (size_t h = 0; h < sizeof(CAVE_HINTS) / sizeof(CAVE_HINTS[0]); ++h) {
        cave = (BYTE*)VirtualAlloc((LPVOID)CAVE_HINTS[h], CAVE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
        if (!cave) continue;
        if (CaveInRange(cave, siteAddrs, valid)) {
            Log("[DG] cave (hint 0x%08X) = %p\n", (unsigned)CAVE_HINTS[h], cave);
            return cave;
        }
        Log("[DG] cave %p out of rel32 range, next hint\n", cave);
        VirtualFree(cave, 0, MEM_RELEASE);
    }
    return nullptr;
}

// Writes one stub + its recorder. Both are position-independent except for the E9 targets, which are
// computed once the cave address is final.
void BuildStub(BYTE* stub, BYTE* shim, size_t i, uintptr_t cont, uintptr_t skip)
{
    // 8B 8E <disp32>  mov ecx,[esi+OFF]
    stub[0] = 0x8B;
    stub[1] = 0x8E;
    *reinterpret_cast<uint32_t*>(stub + 2) = kSites[i].off;

    // 85 C9           test ecx,ecx
    stub[6] = 0x85;
    stub[7] = 0xC9;

    // 0F 84 <rel32>   jz <record>
    stub[8]  = 0x0F;
    stub[9]  = 0x84;
    *reinterpret_cast<int32_t*>(stub + 10) = Rel32(stub + 14, shim);

    // E9 <rel32>      jmp SITE+6
    stub[14] = 0xE9;
    *reinterpret_cast<int32_t*>(stub + 15) = Rel32(stub + 19, (const void*)cont);

    // F0 FF 05 <imm32>  lock inc dword ptr [hits[i]]
    shim[0] = 0xF0;
    shim[1] = 0xFF;
    shim[2] = 0x05;
    *reinterpret_cast<uint32_t*>(shim + 3) = (uint32_t)(uintptr_t)&g_hits[i];

    // 89 35 <imm32>     mov dword ptr [lastNode[i]], esi
    shim[7]  = 0x89;
    shim[8]  = 0x35;
    *reinterpret_cast<uint32_t*>(shim + 9) = (uint32_t)(uintptr_t)&g_lastNode[i];

    // E9 <rel32>        jmp SKIP
    shim[13] = 0xE9;
    *reinterpret_cast<int32_t*>(shim + 14) = Rel32(shim + 18, (const void*)skip);
}

// Data-only 6-byte overwrite: `E9 rel32` then 0x90. The E9 opcode is written last so a thread that is
// already executing the original `mov ecx,[esi+OFF]` is never left with a half-rewritten instruction
// (offset 0 stays 0x8B until the jump is fully in place and the bytes behind it are NOPs).
bool PatchSite(BYTE* at, const void* cont)
{
    BYTE patch[STEAL_LEN];
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) = Rel32(at + 5, cont);
    patch[5] = 0x90;

    DWORD oldProtect = 0;
    if (!VirtualProtect(at, STEAL_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("[DG] site 0x%08X: VirtualProtect failed (%lu) (skipped)\n",
            (unsigned)(uintptr_t)at, (unsigned long)GetLastError());
        return false;
    }

    volatile BYTE* dst = at;
    for (size_t k = 1; k < STEAL_LEN; ++k) dst[k] = patch[k];   // tail first
    dst[0] = patch[0];                                         // then the jump opcode

    FlushInstructionCache(GetCurrentProcess(), at, STEAL_LEN);
    DWORD ignored = 0;
    VirtualProtect(at, STEAL_LEN, oldProtect, &ignored);
    return true;
}

} // namespace

void DerefGuard::Init()
{
    if (g_installed) return;

    // Config gate: [Coop] GuardNullTarget=0 leaves the game completely unpatched (A/B without a
    // rebuild). Default 1 — the crash is fatal, not cosmetic.
    if (GetPrivateProfileIntA("Coop", "GuardNullTarget", 1, INI_PATH) == 0) {
        OutputDebugStringA("[DG] disabled by [Coop] GuardNullTarget=0\n");
        return;
    }

    // Several processes in AC4's launch chain load this ASI; only the real game has the module.
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) {
        OutputDebugStringA("[DG] install skipped: ac4bfsp.exe not loaded\n");
        return;
    }

    if (!g_log) g_log = _fsopen(".\\AC4Coop_deref_guard.log", "a", _SH_DENYWR);

    const int32_t delta = (int32_t)((uintptr_t)game - VA_IMAGE_BASE);

    uintptr_t siteAddrs[SITE_COUNT];
    for (size_t i = 0; i < SITE_COUNT; ++i)
        siteAddrs[i] = (uintptr_t)((int32_t)kSites[i].va + delta);

    // Verify every site before touching any of them: a mismatching site is skipped, never patched,
    // and the rest still install (the crash sites are independent).
    bool valid[SITE_COUNT];
    size_t validCount = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        const BYTE* p = (const BYTE*)siteAddrs[i];
        if (memcmp(p, kSites[i].expected, STEAL_LEN) == 0) {
            valid[i] = true;
            ++validCount;
        } else {
            valid[i] = false;
            Log("[DG] site 0x%08X: unexpected bytes %02X %02X %02X %02X %02X %02X (skipped)\n",
                (unsigned)kSites[i].va,
                p[0], p[1], p[2], p[3], p[4], p[5]);
        }
    }

    if (validCount == 0) {
        Log("[DG] install FAILED: no site matched the expected layout\n");
        return;
    }

    BYTE* cave = AllocateCave(siteAddrs, valid);
    if (!cave) {
        Log("[DG] install FAILED: no cave within rel32 range\n");
        return;
    }
    g_cave = cave;

    // Stubs first, recorders in a second block (keeps every stub contiguous and 16-byte friendly).
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!valid[i]) continue;
        BuildStub(cave + i * STUB_SIZE,
                  cave + STUB_REGION + i * SHIM_SIZE,
                  i,
                  siteAddrs[i] + STEAL_LEN,
                  (uintptr_t)((int32_t)kSites[i].skip + delta));
    }
    FlushInstructionCache(GetCurrentProcess(), cave, STUB_REGION + SITE_COUNT * SHIM_SIZE);

    size_t patched = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!valid[i]) continue;
        // Patch the site to jump to ITS cave stub (which re-executes the stolen load, tests the
        // pointer and then either continues at SITE+6 or skips). Jumping to SITE+6 directly would
        // skip the load and continue with a stale ECX - i.e. reproduce the crash by design.
        if (PatchSite((BYTE*)siteAddrs[i], (const void*)(cave + i * STUB_SIZE)))
            ++patched;
    }

    Log("[DG] %u null-deref sites guarded, cave=%p\n", (unsigned)patched, cave);

    g_installed = true;
    // _beginthreadex, not CreateThread: the reporter calls CRT functions (fputs/_vsnprintf_s) and
    // CreateThread does not initialise the CRT's per-thread state - that crashed the game at the
    // first 5 s drain (ntdll, bad per-thread pointer).
    _beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, nullptr);
}
