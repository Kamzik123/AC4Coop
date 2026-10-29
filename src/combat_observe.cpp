// Stage-2A combat observation -- E2-combat.md 1.2, installed as a byte-patch hook.
//
// WHAT THIS IS
// ------------
// Four prologue detours, one per site, built with the project's own idiom
// (`Hook::DetourFunc` + a trampoline in private RWX memory). Each hit writes one log line:
//
//   [CO] site=%d eip=%08X this=%08X a2=%08X a3=%08X
//
// The stub (one per site, 64 bytes of PAGE_EXECUTE_READWRITE) is:
//
//   stub:
//       pushfd
//       pushad                       ; 32 bytes; ESP now points at the saved-register block
//       push esp                     ; pointer to that block        (cdecl arg #2)
//       push <siteId>                ;                              (cdecl arg #1)
//       call CombatObserve_LogHit    ; cdecl(int siteId, const void* saved)
//       add  esp, 8
//       popad
//       popfd
//       <stolen bytes, verbatim>
//       jmp  dword ptr [slot]        ; FF 25 <addr of a data slot holding site+steal>
//
// The jump back is a real indirect jump through a data slot inside the stub page, NOT
// `FF 25 <resume>` (which would read a dword of engine code as the target -- the bug in
// action_guard.cpp's BuildTrampoline). `FF 25 disp32` clobbers no register and `pushfd`;
// `pushad`/`popad`/`popfd` restore the full machine state before the stolen instructions run,
// so the site resumes with exactly the state the original prologue would have left.
//
// WHY BYTE PATCHING AND NOT DEBUG REGISTERS
// -----------------------------------------
// DR0-DR3 are per-thread state: observing every thread means arming every thread, which means
// cross-thread context edits under suspend (the crash class this build removes) or a VEH
// single-step dance. A prologue detour is thread-agnostic -- one write to the
// site and every thread that calls it is observed -- and it is the same mechanism deref_guard
// and action_guard already use.
//
// OBSERVATION ONLY: the only bytes written are (a) each site's own prologue (the detour itself,
// and the exact original bytes are restored on detach) and (b) our own stub pages. No engine
// state is read or written, no engine function is called, and the arguments are logged as
// VALUES -- never dereferenced. It cannot change combat behaviour.
//
// THE FOUR SITES (AC4BFSP.exe, sha256 bc11ad79..., image base 0x00400000, no ASLR, no .reloc)
// ---------------------------------------------------------------------------------------
//   0  0x017106C0  __thiscall(this, a2, a3)   the damage filter under test (E2.1)
//        55 8B EC 8B 45 08 | 8B 80 9C 00 00 00 ...
//        push ebp; mov ebp,esp; mov eax,[ebp+8]            steal=6
//   1  0x0173E190  __thiscall(this, event)    the reaction handler (E2.2)
//        55 8B EC 83 EC 24 | 53 ...
//        push ebp; mov ebp,esp; sub esp,0x24                steal=6
//   2  0x019FB4C0  __thiscall(int16_t* hp, int value)   ground truth: the life write
//        55 8B EC 56 8B F1 | E8 55 40 20 00 ...
//        push ebp; mov ebp,esp; push esi; mov esi,ecx       steal=6
//   3  0x0133F550  plain call: entity on the stack    the aliveness predicate
//        55 8B EC 56 8B 75 08 | 85 F6 ...
//        push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]   steal=7
//        (the `jne +5` at +9 is OUTSIDE the stolen range)
//
// Every stolen range was decoded (capstone) and contains only complete instructions and no
// relative branch, so copying it to the stub cannot invalidate a displacement. The call at
// site 2 +6 is deliberately NOT stolen, so its rel32 stays where the linker put it.
//
// ARGUMENT LOCATIONS
// ------------------
// PUSHAD order on x86 is EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI, so the block from ESP upward is
// EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX. That puts saved ECX at +24 (NOT +16 -- +16 is EBX).
// With pushfd below the block:
//   +0..31  pushad block      +32 EFLAGS   +36 return address   +40 a2   +44 a3
// At function entry [esp] is the return address and [esp+4]/[esp+8] are a2/a3, which is why the
// +36/+40/+44 offsets are stable regardless of the prologue. Site 3's entity is a2; its ECX is
// not an object and is logged raw.
//
// Uniqueness in .text, overlapping search, pinned build: 0->1 match at 16 bytes (8B:54, 12B:12);
// 1->1 at 16 (8B:72, 12B:3); 2->1 at 16 (8B:176, 9B:1); 3->1 at 24 (8B:333, 12B:7, 16B:7 --
// 16 bytes is NOT unique there, 24 is required).

#include "combat_observe.h"
#include "Hook.h"
#include "build_gate.h"
#include "pattern_scan.h"
#include "paths.h"

#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <cstdint>
#include <cstdarg>
#include <cstring>

namespace {

const uintptr_t VA_IMAGE_BASE = 0x00400000;   // the module is not ASLR-relocated (see Hook.h)
// ini path: Paths::Ini() (module-relative; see src/paths.h)

const size_t    SITE_COUNT         = 4;
const size_t    PATTERN_MAX        = 32;
const LONG      MAX_HITS_TOTAL     = 5000;    // cap on non-reserved lines
const LONG      MAX_HITS_PER_SITE  = 100;     // reserved first-100 per site
const size_t    STUB_SIZE          = 64;
const size_t    STUB_SLOT_OFF      = 48;      // resume pointer; well clear of the 28 code bytes

// `va` is the documented VA on the pinned build. It is the FALLBACK route only: each site is
// normally found by pattern, and the literal is used only when the pattern finds zero matches on
// a build BuildGate already trusts. `verify` is the length of the full pattern memcmp'd at
// install; `steal` is the whole-instruction prefix that moves into the stub.
struct SiteDesc {
    const char* name;
    uintptr_t   va;
    const char* pattern;
    size_t      steal;
    size_t      verify;
};

const SiteDesc kSites[SITE_COUNT] = {
    { "dmg-filter 0x17106C0", 0x017106C0u,
      "55 8B EC 8B 45 08 8B 80 9C 00 00 00 8B D0 80 E2", 6, 16 },
    { "dmg-react  0x173E190", 0x0173E190u,
      "55 8B EC 83 EC 24 53 56 57 8B 7D 08 8B D9 8B CF", 6, 16 },
    { "set-life   0x19FB4C0", 0x019FB4C0u,
      "55 8B EC 56 8B F1 E8 55 40 20 00 85 C0 8B 45 08", 6, 16 },
    { "alive      0x133F550", 0x0133F550u,
      "55 8B EC 56 8B 75 08 85 F6 75 05 32 C0 5E 5D C3 56 E8 8A 30 3C FF 83 C4", 7, 24 },
};

// ---- state ---------------------------------------------------------------------------------
bool     g_installed = false;
FILE*    g_log       = nullptr;

uintptr_t g_va[SITE_COUNT];        // resolved site address
BYTE      g_orig[SITE_COUNT][PATTERN_MAX];   // original prologue bytes, for RestoreFunction
BYTE*     g_stub[SITE_COUNT];      // our stub page (never freed: see Shutdown)
size_t    g_steal[SITE_COUNT];
bool      g_armed[SITE_COUNT];

volatile LONG g_armedCount  = 0;
volatile LONG g_totalLogged = 0;
volatile LONG g_siteHits[SITE_COUNT] = { 0, 0, 0, 0 };
volatile LONG g_inLog       = 0;

// The saved-register block the stub hands us. Field order is the PUSHAD memory order; see the
// header comment for why ECX sits at +24.
struct SavedRegs {
    uint32_t edi;        // +0
    uint32_t esi;        // +4
    uint32_t ebp;        // +8
    uint32_t esp;        // +12  (ESP before PUSHAD, i.e. after PUSHFD)
    uint32_t ebx;        // +16
    uint32_t edx;        // +20
    uint32_t ecx;        // +24  <- the `this` argument
    uint32_t eax;        // +28
    uint32_t eflags;     // +32
    uint32_t ret;        // +36  original return address
    uint32_t a2;         // +40
    uint32_t a3;         // +44
};

// Same shape as the other units' loggers: debugger + a live-readable file, never a blocking
// console write. Called from the game thread inside our prologue hook, so it is bounded and
// reentrancy-guarded.
void Log(const char* fmt, ...)
{
    if (InterlockedExchange(&g_inLog, 1)) return;   // a fault inside the logger must not recurse
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    InterlockedExchange(&g_inLog, 0);
}

// SEH read probe, matching phase1.cpp / action_guard.cpp: an actual attempted read rather than a
// VirtualQuery guess, so a mapped-but-faulting page is still rejected.
bool Readable(const void* p, size_t n)
{
    if (!p || !n) return false;
    __try {
        volatile char first = ((const volatile char*)p)[0];
        volatile char last  = ((const volatile char*)p)[n - 1];
        (void)first; (void)last;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "55 8B EC" -> bytes. None of the four patterns contains a wildcard, so this parser and
// PatternScan's matcher always agree.
size_t ParsePattern(const char* s, BYTE* out, size_t cap)
{
    size_t n = 0;
    while (s && *s && n < cap) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const int hi = HexNibble(*s++);
        const int lo = *s ? HexNibble(*s++) : -1;
        if (hi < 0 || lo < 0) return 0;
        out[n++] = (BYTE)((hi << 4) | lo);
    }
    return n;
}

// "55 8B EC" rendering of `n` bytes, for the mismatch line.
void FormatBytes(const BYTE* p, size_t n, char* out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; ++i)
        o += (size_t)_snprintf_s(out + o, cap - o, _TRUNCATE, "%02X ", p[i]);
    if (o > 0) out[o - 1] = '\0';   // trim the trailing space
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The hook callback. Declared at file scope with cdecl so the stub's `call` hits a plain,
// stack-passed function; its address is baked into the stub at install time.
// ---------------------------------------------------------------------------------------------
extern "C" void __cdecl CombatObserve_LogHit(int siteId, const void* saved)
{
    if (siteId < 0 || siteId >= (int)SITE_COUNT || !saved) return;

    const SavedRegs* r = (const SavedRegs*)saved;

    uint32_t ecx = 0, a2 = 0, a3 = 0;
    __try {
        ecx = r->ecx;
        a2  = r->a2;
        a3  = r->a3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;   // the block is our own stack; a fault means something is badly wrong
    }

    // Per-site reservation first: the first 100 lines of each site are always written, so one hot
    // site cannot starve the others. After that the shared 5000-line cap applies.
    const LONG siteN    = InterlockedIncrement(&g_siteHits[siteId]);
    const bool reserved = (siteN <= MAX_HITS_PER_SITE);
    const LONG line     = InterlockedIncrement(&g_totalLogged);
    if (!reserved && line > MAX_HITS_TOTAL) return;

    Log("[CO] site=%d eip=%08X this=%08X a2=%08X a3=%08X\n",
        siteId, (unsigned)g_va[siteId], (unsigned)ecx, (unsigned)a2, (unsigned)a3);
}

namespace {

// Builds one stub (see the file header for the layout). VirtualAlloc, never a code cave: the
// page is ours, writable-by-us only until the first hit, and DELIBERATELY never VirtualFree'd.
// A thread can still be executing inside a stub when the detour is undone (detach, or the
// Ctrl+Alt+M disarm); freeing the page under it would fault on the very next instruction. Its
// tail re-executes the stolen bytes and jumps back into the site, so an in-flight thread finishes
// cleanly through the restored code. Leaking four small pages per process is the correct trade.
BYTE* BuildStub(int siteId, const BYTE* stolen, size_t steal, uintptr_t resume)
{
    BYTE* s = (BYTE*)VirtualAlloc(nullptr, STUB_SIZE, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (!s) return nullptr;

    size_t o = 0;
    s[o++] = 0x9C;                                  // pushfd
    s[o++] = 0x60;                                  // pushad
    s[o++] = 0x54;                                  // push esp  (-> SavedRegs*)
    s[o++] = 0x6A;                                  // push imm8 (siteId; 0..3, positive)
    s[o++] = (BYTE)siteId;

    s[o++] = 0xE8;                                  // call CombatObserve_LogHit
    const DWORD callFrom = (DWORD)(uintptr_t)(s + o + 4);   // next instruction
    *(DWORD*)(s + o) = (DWORD)(uintptr_t)&CombatObserve_LogHit - callFrom;
    o += 4;

    s[o++] = 0x83; s[o++] = 0xC4; s[o++] = 0x08;    // add esp, 8
    s[o++] = 0x61;                                  // popad
    s[o++] = 0x9D;                                  // popfd

    memcpy(s + o, stolen, steal);                   // stolen bytes, verbatim
    o += steal;

    // Absolute indirect jump through a DATA SLOT. `FF 25 disp32` is `jmp dword ptr [disp32]`; the
    // slot holds the resume VA. Registers and flags untouched.
    *(DWORD*)(s + STUB_SLOT_OFF) = (DWORD)resume;
    s[o++] = 0xFF;
    s[o++] = 0x25;
    *(DWORD*)(s + o) = (DWORD)(uintptr_t)(s + STUB_SLOT_OFF);
    o += 4;

    FlushInstructionCache(GetCurrentProcess(), s, o);
    return s;
}

// Resolves the four site addresses: pattern first, documented literal only on a trusted build.
void ResolveSites(HMODULE game, uintptr_t moduleBase)
{
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        g_va[i]     = 0;
        g_armed[i]  = false;

        int hits = 0;
        const uintptr_t found = PatternScan::Find(game, kSites[i].pattern, &hits);

        uintptr_t va = 0;
        const char* how = nullptr;
        if (hits == 1) {
            va = found;
            how = "pattern, 1 match";
        } else if (hits == 0 && BuildGate::Trusted()) {
            va = moduleBase + (kSites[i].va - VA_IMAGE_BASE);
            how = "literal (0 matches, known build)";
        } else {
            Log("[CO] %s: pattern=%d matches -> skipped\n", kSites[i].name, hits);
            continue;
        }

        g_va[i] = va;
        Log("[CO] %s resolved 0x%08X (steal=%u, %s)\n",
            kSites[i].name, (unsigned)va, (unsigned)kSites[i].steal, how);
    }
}

// Verifies the full pattern, then installs the detour. Never patches blind.
bool InstallOne(size_t i)
{
    BYTE want[PATTERN_MAX];
    const size_t n = ParsePattern(kSites[i].pattern, want, sizeof(want));
    if (n == 0 || n != kSites[i].verify || n > PATTERN_MAX) {
        Log("[CO] site %u: bad pattern descriptor (%u bytes) -> skipped\n", (unsigned)i, (unsigned)n);
        return false;
    }

    const uintptr_t va = g_va[i];
    if (!va) return false;

    if (!Readable((const void*)va, n) || memcmp((const void*)va, want, n) != 0) {
        char exp[3 * PATTERN_MAX + 4], act[3 * PATTERN_MAX + 4];
        FormatBytes(want, n, exp, sizeof(exp));
        FormatBytes((const BYTE*)va, n, act, sizeof(act));
        Log("[CO] site %u @ 0x%08X: pattern mismatch -> skipped\n", (unsigned)i, (unsigned)va);
        Log("[CO] site %u expected: %s\n", (unsigned)i, exp);
        Log("[CO] site %u actual  : %s\n", (unsigned)i, act);
        return false;
    }

    memcpy(g_orig[i], (const void*)va, kSites[i].steal);
    BYTE* stub = BuildStub((int)i, g_orig[i], kSites[i].steal, va + kSites[i].steal);
    if (!stub) {
        Log("[CO] site %u @ 0x%08X: no stub memory -> skipped\n", (unsigned)i, (unsigned)va);
        return false;
    }

    if (!Hook::DetourFunc((BYTE*)va, (DWORD)(uintptr_t)stub, (DWORD)kSites[i].steal)) {
        Log("[CO] site %u @ 0x%08X: DetourFunc failed -> skipped\n", (unsigned)i, (unsigned)va);
        return false;
    }

    g_stub[i]  = stub;
    g_steal[i] = kSites[i].steal;
    g_armed[i] = true;
    InterlockedIncrement(&g_armedCount);

    Log("[CO] site %u armed va=0x%08X steal=%u stub=0x%08X\n",
        (unsigned)i, (unsigned)va, (unsigned)kSites[i].steal, (unsigned)(uintptr_t)stub);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------------------------

void CombatObserve::Init()
{
    if (g_installed) return;

    // ---- config gate FIRST ---------------------------------------------------------------
    // One GetPrivateProfileIntA and one int test. With the switch off there is no log file, no
    // allocation, no VirtualProtect, no detour and no thread: this unit has no other entry point.
    if (GetPrivateProfileIntA("Coop", "CombatObserve", 0, Paths::Ini()) == 0) {
        OutputDebugStringA("[CO] disabled by [Coop] CombatObserve=0\n");
        return;
    }

    // Fail closed on patching, not on loading: an unverified build keeps this module resident and
    // silent, and the process still reaches the menu (same rule as the two guards).
    if (!BuildGate::Trusted()) {
        OutputDebugStringA("[CO] skipped: build not trusted\n");
        return;
    }

    // Several processes in AC4's launch chain load this ASI; only the real game has the module.
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) {
        OutputDebugStringA("[CO] install skipped: ac4bfsp.exe not loaded\n");
        return;
    }

    g_installed = true;

    // Game-folder log, live-readable (shared write denied). PID-suffixed so a stale process cannot
    // hold the file and swallow a later run's evidence.
    if (!g_log) {
        char leaf[64];
        _snprintf_s(leaf, sizeof(leaf), _TRUNCATE,
                    "AC4Coop_combat_observe_%u.log", (unsigned)GetCurrentProcessId());
        g_log = _fsopen(Paths::LogPath(leaf), "a", _SH_DENYWR);
    }

    Log("[CO] stage 2A observer starting (byte-patch hook; observe only, args logged as values)\n");

    ResolveSites(game, (uintptr_t)game);

    size_t armed = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i)
        if (InstallOne(i)) ++armed;

    Log("[CO] armed %d/%d sites\n", (int)armed, (int)SITE_COUNT);
    if (armed == 0)
        Log("[CO] install FAILED: no site verified -> nothing patched\n");
}

// Restores the original prologue at every armed site and clears the armed flags. Returns the count
// actually restored. Shared by Shutdown() (detach) and Disarm() (Ctrl+Alt+M) so both undo a site
// through the exact same path: Hook::RestoreFunction with the bytes saved at install time.
//
// The stub pages are deliberately NOT VirtualFree'd, here or in Shutdown(). A game thread may still
// be executing inside one when this runs; freeing it would crash the process on the next instruction.
// The stub's tail is the stolen bytes plus the slot jump into the (now restored) site, so an in-flight
// thread exits through the original code either way. Leaking four small pages per process is the
// correct trade against a crash.
static int RestoreAllSites()
{
    int restored = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!g_armed[i]) continue;
        Hook::RestoreFunction(g_va[i], kSites[i].steal, g_orig[i]);
        g_armed[i] = false;
        ++restored;
    }
    if (restored > 0) InterlockedExchange(&g_armedCount, 0);
    return restored;
}

void CombatObserve::Shutdown()
{
    if (!g_installed) return;

    const int restored = RestoreAllSites();

    Log("[CO] disarmed %d sites\n", restored);

    if (g_log) { fclose(g_log); g_log = nullptr; }
    g_installed = false;
}

// Disarm, not Shutdown: the log file stays open and the unit stays installed, so the sites can be
// inspected for a "disarmed N sites" line at the moment of the press. A second call is a no-op --
// g_armedCount is already 0, so nothing is written and nothing is logged.
void CombatObserve::Disarm()
{
    if (!g_installed) return;       // observer never installed (flag off) or already shut down
    if (g_armedCount <= 0) return;  // idempotent: nothing armed, so nothing to restore

    const int restored = RestoreAllSites();
    Log("[CO] disarmed %d sites\n", restored);
}

bool CombatObserve::Armed()
{
    return g_armedCount > 0;
}
