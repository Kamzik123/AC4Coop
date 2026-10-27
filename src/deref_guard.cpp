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
#include "build_gate.h"
#include "pattern_scan.h"

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <share.h>
#include <cstdint>
#include <cstdarg>

namespace {

const uintptr_t VA_IMAGE_BASE = 0x00400000;   // module is not ASLR-relocated (see Hook.h)
const size_t    STEAL_LEN     = 6;            // mov ecx,[esi+disp32]

// Stub layout, 72 bytes. Every branch is a rel32 conditional (0F 8x) rather than rel8, so the extra
// checks cannot push a short jump out of range, and the recorder always sits at a fixed offset:
//
//   00  50                  push eax                    ; preserve the caller's EAX/EDX
//   01  52                  push edx
//   02  8B 8E <disp32>      mov  ecx,[esi+OFF]          ; the stolen instruction, re-executed verbatim
//   08  85 C9               test ecx,ecx
//   0A  0F 84 <rel32>       jz   <reject>               ; null
//   10  81 F9 00 00 01 00   cmp  ecx,0x00010000         ; cheap wild-pointer bound, low
//   16  0F 82 <rel32>       jb   <reject>               ; below the bound
//   1C  81 F9 00 00 00 7F   cmp  ecx,0x7F000000         ; cheap wild-pointer bound, high
//   22  0F 83 <rel32>       jae  <reject>               ; at/above the bound
//   28  8B 01               mov  eax,[ecx]              ; the object's vtable pointer
//   2A  2D <imm32>          sub  eax,IMAGE_BASE         ; vtable - module base, as an unsigned delta
//   2F  3D <imm32>          cmp  eax,IMAGE_SIZE
//   34  0F 83 <rel32>       jae  <reject>               ; outside the module image (either side)
//   3A  5A                  pop  edx                    ; accept -> original code, registers restored
//   3B  58                  pop  eax
//   3C  E9 <rel32>          jmp  SITE+6
//   41  5A                  pop  edx                    ; reject -> recorder, registers restored
//   42  58                  pop  eax
//   43  E9 <rel32>          jmp  <record>
//
// EAX and EDX are saved at entry and restored on BOTH exits, so the engine observes the same register
// state at the continue and the skip target as it did before this guard existed; the stub's own use of
// EAX (vtable load, range delta) cannot leak into the caller. ECX is never rewritten after the stolen
// load, so both exits inherit exactly the value the original `mov ecx,[esi+OFF]` would have produced,
// and the stack is balanced on every path (2 pushes, 2 pops).
//
// The vtable check accepts anything inside the loaded module image [ImageBase, ImageBase+SizeOfImage),
// read from the in-memory PE rather than a baked-in section range: a legitimate engine object whose
// vtable lives outside .rdata (e.g. in .data) must never be skipped -- that would be a silent missing
// update, not a crash. Null, 0xFFFFFFFF and wild/heap pointers are still rejected, so the crash case
// is fully covered; every reject lands on the call-free recorder, which counts the hit and jumps to
// the site's documented skip target.
const size_t    STUB_SIZE     = 72;
const size_t    SHIM_SIZE     = 18;           // 7 + 6 + 5
const size_t    STUB_REGION   = 448;          // 6 * 72 = 432, rounded so the shims stay 16-byte aligned
const size_t    CAVE_SIZE     = 1024;         // 6*72 + 6*18 = 540 used; the rest is headroom
const size_t    SITE_COUNT    = 6;

// Wild-pointer bound for an object pointer. Anything outside [64 KiB, 0x7F000000) is not a heap
// object on this build; in particular 0xFFFFFFFF (the observed sentinel) fails the high test.
const uint32_t  MIN_TARGET_PTR = 0x00010000;
const uint32_t  MAX_TARGET_PTR = 0x7F000000;

// Part B: the unchecked [node+0x114] loader at the head of the action-lane call chain that crashed
// with EIP=0x0073448F, EDX=0xFFFFFFFF. It is NOT one of the six sites above, so the null guards do
// not cover it: 0x6795FD must get its own patch, its own cave stub and its own recorder.
const uintptr_t LITERAL_STALE_SITE = 0x006795FD;   // mov ecx,[esi+0x114]
const uintptr_t LITERAL_STALE_CONT = 0x00679603;   // test ecx,ecx ; je 0x679611 ; call 0x734460
const uintptr_t LITERAL_STALE_SKIP = 0x00679611;   // mov byte ptr [ebp+8],0  (engine's "no target")
const size_t    STALE_CAVE_SIZE    = 256;          // stub 72 + recorder 18, room to spare

// A rel32 patch reaches +-2 GB. Require a comfortable margin so the boundary itself is never used.
const uintptr_t MAX_CAVE_DELTA = 0x7FFF0000;

// Hint addresses tried, in order, if the first VirtualAlloc lands outside +-2 GB of the module.
const uintptr_t CAVE_HINTS[] = { 0x20000000, 0x24000000, 0x30000000, 0x50000000, 0x10000000 };

const char* INI_PATH = ".\\AC4Coop.ini";

// `mov ecx,[esi+0x114]` / `mov ecx,[esi+0x118]`. Matched before patching so a different build is
// never half-hooked; a mismatch is logged and that site is left untouched.
const BYTE EXP_114[STEAL_LEN] = { 0x8B, 0x8E, 0x14, 0x01, 0x00, 0x00 };
const BYTE EXP_118[STEAL_LEN] = { 0x8B, 0x8E, 0x18, 0x01, 0x00, 0x00 };

// The six sites. `cont` is the instruction after the stolen load; `skip` is the instruction a null
// pointer must resume at. The VAs quoted below are the documented values for this build (kept for the
// offline oracle and for the comments); the table itself is expressed as offsets from the function
// base, so no literal code address reaches the patcher:
//
//   0x006797E4  the +0x118 load that follows the +0x114 call
//   0x006797F5  epilogue (`or word[esi+0xE],di`) of sub_679750
//   0x00681AFA  `push edi; push ebx; jmp 0x00681B61` inside sub_681980 — a null +0x114 must land
//               here, not at 0x00681B61, because that code pushes the argument pair the +0x118 call
//               expects; entering at 0x00681B61 directly would run the call with a stale pair
//   0x00681B36  post-call cmp/je 0x00681B75 / jmp 0x00681B5C for case 3
//   0x00681B5C  shared tail after case 4's call
//   0x00681B75  phase-2 epilogue
enum FuncId { FUNC_A = 0, FUNC_B = 1, FUNC_COUNT = 2 };

// The two function bases. These literals are the ONLY literal code addresses in this unit, and they
// are reached only as the fallback route (zero pattern matches on a KNOWN build). RVAs are
// `VA - 0x00400000`.
const char* const PATTERN_FUNC_A = "55 8B EC 66 8B 15 ?? ?? ?? ?? 56 8B F1 66 8B 46 12 66 23 45 08";
const char* const PATTERN_FUNC_B = "55 8B EC 83 EC 2C 53 56 8B F1 66 8B 46 10 66 23 45 0C";
const uintptr_t   LITERAL_FUNC_A = 0x00679750;   // sub_679750
const uintptr_t   LITERAL_FUNC_B = 0x00681980;   // sub_681980

// Site table, as offsets from the function base -- deliberately no absolute VAs, so a resolved base
// moves every site with it.
//
//   func  off    contOff  skipOff   what it is
//   A     0x86   0x8C     0x94      the [node+0x114] load in sub_679750
//   A     0x94   0x9A     0xA5      the [node+0x118] load that follows the +0x114 call
//   B     0x167  0x16D    0x17A     the [node+0x114] load in sub_681980
//   B     0x1E1  0x1E7    0x1F5     the [node+0x118] load; phase-2 epilogue is the skip target
//   B     0x1A3  0x1A9    0x1B6     case 3's [node+0x114] load; skip = post-call cmp/je, not the call
//   B     0x1C9  0x1CF    0x1DC     case 4's [node+0x114] load; skip = shared tail after case 4
//
// Install order is the table order and must not change: the stubs are laid out contiguously in the
// cave by index.
struct SiteDesc {
    FuncId       func;
    DWORD        off;        // site offset from the function base
    const BYTE*  expected;   // per-site prologue, matched before anything is patched
    DWORD        fieldOff;   // node field the stolen load reads (+0x114 / +0x118)
    DWORD        contOff;    // continue offset from the function base
    DWORD        skipOff;    // skip offset from the function base
};

const SiteDesc kSites[SITE_COUNT] = {
    { FUNC_A, 0x86,  EXP_114, 0x114, 0x8C,  0x94  },
    { FUNC_A, 0x94,  EXP_118, 0x118, 0x9A,  0xA5  },
    { FUNC_B, 0x167, EXP_114, 0x114, 0x16D, 0x17A },
    { FUNC_B, 0x1E1, EXP_118, 0x118, 0x1E7, 0x1F5 },
    { FUNC_B, 0x1A3, EXP_114, 0x114, 0x1A9, 0x1B6 },
    { FUNC_B, 0x1C9, EXP_114, 0x114, 0x1CF, 0x1DC },
};

// Documented literal VA -> address in this process. The module is not ASLR-relocated, so this is a
// no-op today; it exists so the fallback would still be correct for a relocated image.
uintptr_t VaLiteral(uintptr_t moduleBase, uintptr_t va)
{
    return moduleBase + (va - VA_IMAGE_BASE);
}

// SizeOfImage straight out of the MAPPED PE headers, so the guard's acceptance window is the image the
// loader actually mapped rather than a baked-in .rdata literal. Fails closed: Init() installs nothing
// when this cannot be read, because a zero/garbage window would reject every target (a silent missing
// update -- exactly the failure mode this window exists to rule out).
bool ReadSizeOfImage(uintptr_t moduleBase, uint32_t* sizeOfImage)
{
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)moduleBase;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;

        const IMAGE_NT_HEADERS32* nt =
            (const IMAGE_NT_HEADERS32*)((const BYTE*)moduleBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
        if (nt->OptionalHeader.SizeOfImage == 0) return false;

        *sizeOfImage = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


bool     g_entry     = false;   // Init() runs at most once, even when a config gate installs nothing
bool     g_installed = false;
BYTE*    g_cave      = nullptr;
FILE*    g_log       = nullptr;

// Module image window for vtable validation, read from the in-memory PE in Init().
uintptr_t g_imageBase = 0;
uint32_t  g_imageSize = 0;

// Fire counters written by the cave (absolute imm32 data stores; must outlive the cave, so they live
// in this translation unit's data rather than in the executable cave).
volatile LONG g_hits[SITE_COUNT];
volatile LONG g_lastNode[SITE_COUNT];
LONG          g_logged[SITE_COUNT];
volatile LONG g_totalLogged = 0;

// Part B's own counter pair, same contract as the per-site ones.
volatile LONG g_staleHits = 0;
volatile LONG g_staleNode = 0;
LONG          g_staleLogged = 0;

// Site VAs as resolved in Init(), for the reporter's log lines (the site table itself holds offsets).
uintptr_t     g_siteVa[SITE_COUNT];
uintptr_t     g_staleVa = 0;

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
            Log("[DG] null target at site 0x%08X node=%p\n", (unsigned)g_siteVa[i], (void*)node);
    }

    // Part B: the stale-target guard at 0x006795FD, same reporting shape as the per-site counters.
    if (g_staleHits) {
        const LONG hits = g_staleHits;
        const LONG node = g_staleNode;

        bool emit = false;
        if (g_staleLogged < 20 && hits > g_staleLogged) {
            g_staleLogged = hits;
            emit = true;
        } else if (hits >= g_staleLogged + 500) {
            g_staleLogged = hits;
            emit = true;
        }

        if (emit)
            Log("[DG] stale target at site 0x%08X node=%p\n", (unsigned)g_staleVa, (void*)node);
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

// Writes one stub + its recorder. Both are position-independent except for the E9/0F 8x targets,
// which are computed once the cave address is final. `hits` / `lastNode` are the recorder's storage
// (per-site for Part A, a single pair for Part B).
//
// Register contract: the stub touches EAX/EDX/ECX/flags internally but preserves the caller's EAX and
// EDX across BOTH exits (2 pushes at entry, 2 pops before each jump), and never rewrites ECX after the
// stolen load. ESI, EDI and EBX are untouched. So every continue/skip target sees exactly the register
// state the pre-guard build produced; the internal EAX/EDX use is invisible to the engine.
void BuildStub(BYTE* stub, BYTE* shim, DWORD fieldOff, uintptr_t cont, uintptr_t skip,
               uintptr_t imageBase, uint32_t imageSize,
               volatile LONG* hits, volatile LONG* lastNode)
{
    // The reject trampoline sits after the accept jump: every conditional reject hops there, pops the
    // saved pair and then jumps to the recorder, so the stack is balanced on every path.
    BYTE* reject = stub + 65;   // 41

    // 00  50              push eax       -- preserve the caller's EAX
    stub[0] = 0x50;

    // 01  52              push edx       -- preserve the caller's EDX
    stub[1] = 0x52;

    // 02  8B 8E <disp32>  mov ecx,[esi+OFF]
    stub[2] = 0x8B;
    stub[3] = 0x8E;
    *reinterpret_cast<uint32_t*>(stub + 4) = fieldOff;

    // 08  85 C9           test ecx,ecx
    stub[8] = 0x85;
    stub[9] = 0xC9;

    // 0A  0F 84 <rel32>   jz <reject>
    stub[10] = 0x0F;
    stub[11] = 0x84;
    *reinterpret_cast<int32_t*>(stub + 12) = Rel32(stub + 16, reject);

    // 10  81 F9 <imm32>   cmp ecx,MIN_TARGET_PTR
    stub[16] = 0x81;
    stub[17] = 0xF9;
    *reinterpret_cast<uint32_t*>(stub + 18) = MIN_TARGET_PTR;

    // 16  0F 82 <rel32>   jb <reject>      (ecx < MIN_TARGET_PTR)
    stub[22] = 0x0F;
    stub[23] = 0x82;
    *reinterpret_cast<int32_t*>(stub + 24) = Rel32(stub + 28, reject);

    // 1C  81 F9 <imm32>   cmp ecx,MAX_TARGET_PTR
    stub[28] = 0x81;
    stub[29] = 0xF9;
    *reinterpret_cast<uint32_t*>(stub + 30) = MAX_TARGET_PTR;

    // 22  0F 83 <rel32>   jae <reject>     (ecx >= MAX_TARGET_PTR, unsigned)
    stub[34] = 0x0F;
    stub[35] = 0x83;
    *reinterpret_cast<int32_t*>(stub + 36) = Rel32(stub + 40, reject);

    // 28  8B 01           mov eax,[ecx]    -- the vtable pointer; ecx is a readable object by now
    stub[40] = 0x8B;
    stub[41] = 0x01;

    // 2A  2D <imm32>      sub eax,IMAGE_BASE
    // EAX becomes the unsigned distance from the module base: a vtable below the image wraps to a huge
    // value and is rejected by the compare below, so one test covers both sides of the range.
    stub[42] = 0x2D;
    *reinterpret_cast<uint32_t*>(stub + 43) = (uint32_t)imageBase;

    // 2F  3D <imm32>      cmp eax,IMAGE_SIZE
    stub[47] = 0x3D;
    *reinterpret_cast<uint32_t*>(stub + 48) = imageSize;

    // 34  0F 83 <rel32>   jae <reject>     (vtable outside the module image, either side)
    stub[52] = 0x0F;
    stub[53] = 0x83;
    *reinterpret_cast<int32_t*>(stub + 54) = Rel32(stub + 58, reject);

    // 3A  5A              pop edx         -- accept: restore the caller's EDX
    stub[58] = 0x5A;

    // 3B  58              pop eax         -- ... and EAX
    stub[59] = 0x58;

    // 3C  E9 <rel32>      jmp SITE+6
    stub[60] = 0xE9;
    *reinterpret_cast<int32_t*>(stub + 61) = Rel32(stub + 65, (const void*)cont);

    // 41  5A              pop edx         -- reject: restore the caller's EDX
    stub[65] = 0x5A;

    // 42  58              pop eax         -- ... and EAX
    stub[66] = 0x58;

    // 43  E9 <rel32>      jmp <record>
    stub[67] = 0xE9;
    *reinterpret_cast<int32_t*>(stub + 68) = Rel32(stub + 72, shim);

    // F0 FF 05 <imm32>  lock inc dword ptr [hits]
    shim[0] = 0xF0;
    shim[1] = 0xFF;
    shim[2] = 0x05;
    *reinterpret_cast<uint32_t*>(shim + 3) = (uint32_t)(uintptr_t)hits;

    // 89 35 <imm32>     mov dword ptr [lastNode], esi
    shim[7]  = 0x89;
    shim[8]  = 0x35;
    *reinterpret_cast<uint32_t*>(shim + 9) = (uint32_t)(uintptr_t)lastNode;

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

// Part A: the six per-site null/stale deref guards. `game` / `moduleBase` are resolved by Init(),
// which also owns the config gates, the log file and the reporter thread.
static bool InstallNullGuards(HMODULE game, uintptr_t moduleBase)
{


    // ---- resolve the two function bases (pattern first, literal only on a KNOWN build) ----------
    const char* const kFuncName[FUNC_COUNT]    = { "sub_679750", "sub_681980" };
    const char* const kFuncPattern[FUNC_COUNT] = { PATTERN_FUNC_A, PATTERN_FUNC_B };
    const uintptr_t   kFuncLiteral[FUNC_COUNT] = { LITERAL_FUNC_A, LITERAL_FUNC_B };

    uintptr_t funcBase[FUNC_COUNT] = { 0, 0 };

    for (int f = 0; f < FUNC_COUNT; ++f) {
        int hits = 0;
        const uintptr_t found = PatternScan::Find(game, kFuncPattern[f], &hits);

        if (hits == 1) {
            funcBase[f] = found;
            Log("[SIG] %s resolved 0x%08X (pattern, 1 match)\n",
                kFuncName[f], (unsigned)found);
        } else if (hits == 0 && BuildGate::Trusted()) {
            funcBase[f] = VaLiteral(moduleBase, kFuncLiteral[f]);
            Log("[SIG] %s pattern=0 -> literal 0x%08X\n",
                kFuncName[f], (unsigned)funcBase[f]);
        } else if (hits == 0) {
            // Unreachable while the Trusted() gate above holds, but kept explicit: a zero-match
            // pattern on an unknown build must abort, never fall through to a literal.
            Log("[SIG] %s pattern=0, build unknown -> hook aborted\n", kFuncName[f]);
        } else {
            // More than one match: ambiguous regardless of build. Its two sites are aborted.
            Log("[SIG] %s pattern=%d matches -> hook aborted\n", kFuncName[f], hits);
        }
    }

    // ---- derive the six sites from those bases --------------------------------------------------
    uintptr_t siteAddrs[SITE_COUNT];
    uintptr_t contAddrs[SITE_COUNT];
    uintptr_t skipAddrs[SITE_COUNT];

    // Verify every site before touching any of them: a mismatching site is skipped, never patched,
    // and the rest still install (the crash sites are independent).
    bool valid[SITE_COUNT];
    size_t validCount = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        const uintptr_t fb = funcBase[kSites[i].func];
        siteAddrs[i] = contAddrs[i] = skipAddrs[i] = 0;
        valid[i] = false;

        if (!fb) continue;   // base unresolved; the [SIG] line above already says why

        siteAddrs[i] = fb + kSites[i].off;
        contAddrs[i] = fb + kSites[i].contOff;
        skipAddrs[i] = fb + kSites[i].skipOff;
        g_siteVa[i]  = siteAddrs[i];

        const BYTE* p = (const BYTE*)siteAddrs[i];
        if (memcmp(p, kSites[i].expected, STEAL_LEN) == 0) {
            valid[i] = true;
            ++validCount;
        } else {
            Log("[DG] site 0x%08X (base+0x%X): unexpected bytes "
                "%02X %02X %02X %02X %02X %02X (skipped)\n",
                (unsigned)siteAddrs[i], (unsigned)kSites[i].off,
                p[0], p[1], p[2], p[3], p[4], p[5]);
        }
    }

    if (validCount == 0) {
        Log("[DG] install FAILED: no site matched the expected layout\n");
        return false;
    }

    BYTE* cave = AllocateCave(siteAddrs, valid);
    if (!cave) {
        Log("[DG] install FAILED: no cave within rel32 range\n");
        return false;
    }
    g_cave = cave;

    // Stubs first, recorders in a second block (keeps every stub contiguous and 16-byte friendly).
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!valid[i]) continue;
        BuildStub(cave + i * STUB_SIZE,
                  cave + STUB_REGION + i * SHIM_SIZE,
                  kSites[i].fieldOff,
                  contAddrs[i],
                  skipAddrs[i],
                  g_imageBase,
                  g_imageSize,
                  &g_hits[i],
                  &g_lastNode[i]);
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

    return patched > 0;
}

// ---------------------------------------------------------------------------------------------
// Part B: the stale-target guard at 0x006795FD.
// ---------------------------------------------------------------------------------------------

// A cave for Part B, which must install independently of the six-site cave (and of GuardNullTarget).
static bool CaveReachable(const BYTE* cave, uintptr_t site)
{
    const uintptr_t d = (uintptr_t)cave > site ? (uintptr_t)cave - site : site - (uintptr_t)cave;
    return d < MAX_CAVE_DELTA;
}

static BYTE* AllocCaveNear(uintptr_t site, size_t size)
{
    BYTE* cave = (BYTE*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (cave && CaveReachable(cave, site)) {
        Log("[DG] stale cave (unhinted) = %p\n", cave);
        return cave;
    }
    if (cave) {
        Log("[DG] stale cave %p out of rel32 range, retrying with hints\n", cave);
        VirtualFree(cave, 0, MEM_RELEASE);
    }

    for (size_t h = 0; h < sizeof(CAVE_HINTS) / sizeof(CAVE_HINTS[0]); ++h) {
        cave = (BYTE*)VirtualAlloc((LPVOID)CAVE_HINTS[h], size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
        if (!cave) continue;
        if (CaveReachable(cave, site)) {
            Log("[DG] stale cave (hint 0x%08X) = %p\n", (unsigned)CAVE_HINTS[h], cave);
            return cave;
        }
        Log("[DG] stale cave %p out of rel32 range, next hint\n", cave);
        VirtualFree(cave, 0, MEM_RELEASE);
    }
    return nullptr;
}

// The loader at the head of the action-lane call chain that crashed with EIP=0x0073448F,
// EDX=0xFFFFFFFF (the object's first dword -- its vtable -- held the sentinel):
//
//   0x006795FD  8B 8E 14 01 00 00   mov  ecx,[esi+0x114]
//   0x00679603  85 C9               test ecx,ecx
//   0x00679605  0F 84 ..            je   0x00679611          engine's own "no target" path
//   0x0067960B  ..                  call 0x00734460         (unchecked virtual dispatch)
//   0x00679611  C6 45 08 00         mov  byte ptr [ebp+8],0
//
// The 6-byte load is replaced with `E9 rel32` + 0x90 into a private cave stub that re-executes it and
// applies the same checks as Part A, including the EAX/EDX save/restore on both exits. Every reject
// goes through the call-free recorder (count the hit, latch ESI) and then the engine's own 0x00679611;
// a pass jumps to 0x00679603, so the original test/je/call still run in their original order. The
// reject path inherits ECX from the re-executed load, which is exactly the value the original
// `mov ecx,[esi+0x114]` produced before the engine's own test at 0x00679603.
static bool InstallStaleTarget(uintptr_t moduleBase)
{
    const uintptr_t site = VaLiteral(moduleBase, LITERAL_STALE_SITE);
    const uintptr_t cont = VaLiteral(moduleBase, LITERAL_STALE_CONT);
    const uintptr_t skip = VaLiteral(moduleBase, LITERAL_STALE_SKIP);

    static const BYTE expSite[STEAL_LEN] = { 0x8B, 0x8E, 0x14, 0x01, 0x00, 0x00 };
    static const BYTE expCont[2]         = { 0x85, 0xC9 };
    static const BYTE expSkip[4]         = { 0xC6, 0x45, 0x08, 0x00 };

    const BYTE* p = (const BYTE*)site;
    const BYTE* c = (const BYTE*)cont;
    const BYTE* s = (const BYTE*)skip;

    // Byte-verify the site AND both places the stub jumps to before touching a single byte.
    if (memcmp(p, expSite, sizeof(expSite)) != 0 ||
        memcmp(c, expCont, sizeof(expCont)) != 0 ||
        memcmp(s, expSkip, sizeof(expSkip)) != 0) {
        Log("[DG] stale-target guard FAILED @ 0x%08X (site %02X %02X %02X %02X %02X %02X, "
            "cont %02X %02X, skip %02X %02X %02X %02X)\n",
            (unsigned)site, p[0], p[1], p[2], p[3], p[4], p[5],
            c[0], c[1], s[0], s[1], s[2], s[3]);
        return false;
    }

    g_staleVa = site;

    BYTE* cave = AllocCaveNear(site, STALE_CAVE_SIZE);
    if (!cave) {
        Log("[DG] stale-target guard FAILED @ 0x%08X (no cave within rel32 range)\n", (unsigned)site);
        return false;
    }

    // Recorder immediately after the 72-byte stub: both live in this cave, nothing is shared with
    // the six-site cave.
    BuildStub(cave, cave + STUB_SIZE, 0x114, cont, skip,
              g_imageBase, g_imageSize, &g_staleHits, &g_staleNode);
    FlushInstructionCache(GetCurrentProcess(), cave, STUB_SIZE + SHIM_SIZE);

    if (!PatchSite((BYTE*)site, cave)) {
        Log("[DG] stale-target guard FAILED @ 0x%08X (patch failed)\n", (unsigned)site);
        return false;
    }

    Log("[DG] stale-target guard @ 0x%08X (vtable range)\n", (unsigned)site);
    return true;
}

void DerefGuard::Init()
{
    if (g_entry) return;
    g_entry = true;

    // Fail closed on patching, not on loading: an unverified build keeps this module resident and
    // silent, and the process still reaches the menu.
    if (!BuildGate::Trusted()) {
        OutputDebugStringA("[DG] skipped: build not trusted\n");
        return;
    }

    // Two independent gates. GuardNullTarget covers the six Part A sites; GuardStaleTarget covers the
    // separate loader at 0x006795FD. `GuardNullTarget=1 GuardStaleTarget=0` reproduces the previous
    // build exactly; `0/1` A/Bs the stale guard on its own.
    const bool nullGuard  = GetPrivateProfileIntA("Coop", "GuardNullTarget", 1, INI_PATH) != 0;
    const bool staleGuard = GetPrivateProfileIntA("Coop", "GuardStaleTarget", 1, INI_PATH) != 0;

    if (!nullGuard)
        OutputDebugStringA("[DG] null-target guards disabled by [Coop] GuardNullTarget=0\n");
    if (!staleGuard)
        OutputDebugStringA("[DG] stale-target guard disabled by [Coop] GuardStaleTarget=0\n");
    if (!nullGuard && !staleGuard) {
        OutputDebugStringA("[DG] disabled: both [Coop] guards are 0\n");
        return;
    }

    // Several processes in AC4's launch chain load this ASI; only the real game has the module.
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) {
        OutputDebugStringA("[DG] install skipped: ac4bfsp.exe not loaded\n");
        return;
    }

    // Game-folder log, live-readable (shared write denied) like phase1's. PID-suffixed so a stale
    // process cannot hold it open and swallow a later run's evidence.
    if (!g_log) {
        char logPath[64];
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE,
                    ".\\AC4Coop_deref_guard_%u.log", (unsigned)GetCurrentProcessId());
        g_log = _fsopen(logPath, "a", _SH_DENYWR);
    }

    const uintptr_t moduleBase = (uintptr_t)game;

    // Image window for vtable validation, read from the in-memory PE rather than baked in, so a
    // relocated or re-laid-out image still validates against its own [ImageBase, ImageBase+Size).
    uint32_t sizeOfImage = 0;
    if (!ReadSizeOfImage(moduleBase, &sizeOfImage)) {
        Log("[DG] install FAILED: cannot read SizeOfImage from the module PE headers\n");
        return;
    }
    g_imageBase = moduleBase;
    g_imageSize = sizeOfImage;
    Log("[DG] vtable range 0x%08X..0x%08X (module image)\n",
        (unsigned)g_imageBase, (unsigned)(g_imageBase + g_imageSize));

    bool installedAny = false;
    if (nullGuard)  installedAny |= InstallNullGuards(game, moduleBase);
    if (staleGuard) installedAny |= InstallStaleTarget(moduleBase);

    g_installed = installedAny;
    if (!installedAny) return;

    // _beginthreadex, not CreateThread: the reporter calls CRT functions (fputs/_vsnprintf_s) and
    // CreateThread does not initialise the CRT's per-thread state - that crashed the game at the
    // first 5 s drain (ntdll, bad per-thread pointer).
    _beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, nullptr);
}
