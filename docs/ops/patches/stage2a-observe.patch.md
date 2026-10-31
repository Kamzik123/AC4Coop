# stage2a-observe — patch proposal (COMBAT → NET)

**Status:** proposal, 2026-10-29. Applies `bf-coop/ops/experiments/E2-combat.md` §1.2 as one
new translation unit. Nothing in `bf-coop/ref/AC4Coop/src/` was modified by this seat.

**This patch is observation-only.** It adds no byte patch, calls `VirtualProtect` on nothing,
writes no game memory, and calls no game function. The only writes it performs are to
thread contexts (the debug registers, which is the only way to arm a hardware breakpoint) and
to its own log file. It therefore **cannot alter combat behaviour**; the worst it can do is cost
time (see `## risks` R7) or crash the process it is observing (R1, mitigated in code).

Idiom: hand-rolled like `deref_guard.cpp` / `action_guard.cpp` — `BuildGate::Trusted()`,
`PatternScan::Find`, the same `Log()` helper, the same `_fsopen` PID-suffixed log, the same
`__try`/`__except` read probes, `_beginthreadex` for the background thread. No MinHook, no code
caves, no trampolines. The VEH shape is copied from `phase1.cpp`'s `DrTrap`
(`AddVectoredExceptionHandler(1, …)`, `CONTEXT_DEBUG_REGISTERS`, `SetThreadContext`), extended
from one write breakpoint to four execution breakpoints with a step-over/re-arm cycle.

---

## 1. The four sites, as bytes on the pinned build

Resolved from the binary itself, not from a listing. `bf-coop/audit/config/project.json` pins
`AC4BFSP.exe` sha256 `bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150`,
size 45 056 040. The file at
`D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe` hashes to exactly
that and is 45 056 040 bytes, so the patterns below are the pinned build's bytes.

`.text` is `VA 0x00401000 .. 0x01E39A00` (27 494 912 bytes). Every count below is an
overlapping search of the whole section.

| slot | VA | pattern | len | matches in `.text` | decoded prologue |
|---|---|---|---|---|---|
| DR0 | `0x017106C0` | `55 8B EC 8B 45 08 8B 80 9C 00 00 00 8B D0 80 E2` | 16 | **1** | `push ebp; mov ebp,esp; mov eax,[ebp+8]; mov eax,[eax+0x9C]; mov edx,eax; and dl,7` |
| DR1 | `0x0173E190` | `55 8B EC 83 EC 24 53 56 57 8B 7D 08 8B D9 8B CF` | 16 | **1** | `push ebp; mov ebp,esp; sub esp,0x24; push ebx; push esi; push edi; mov edi,[ebp+8]; mov ebx,ecx` |
| DR2 | `0x019FB4C0` | `55 8B EC 56 8B F1 E8 55 40 20 00 85 C0 8B 45 08` | 16 | **1** | `push ebp; mov ebp,esp; push esi; mov esi,ecx; call …; test eax,eax; mov eax,[ebp+8]` |
| DR3 | `0x0133F550` | `55 8B EC 56 8B 75 08 85 F6 75 05 32 C0 5E 5D C3 56 E8 8A 30 3C FF 83 C4` | 24 | **1** | `push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]; test esi,esi; jnz +5; xor al,al; pop esi; pop ebp; ret; push esi; call …` |

Uniqueness notes, stated because the shorter prefixes are **not** unique:

* `0x017106C0`: 8 bytes → 54 matches, 12 bytes → 12, 16 → 1. The 16-byte form is the shortest
  that is unique, which is why the table uses 14+2.
* `0x0173E190`: 8 → 72, 12 → 3, 16 → 1.
* `0x019FB4C0`: 8 → 176, 9 → 1 (**the 9-byte form `55 8B EC 56 8B F1 E8 55 40` is already
  unique**, but 9 bytes is thin for a build-identity check; the table uses 16, which is also 1).
* `0x0133F550`: 8 → 333, 12 → 7, 16 → **7**, 24 → 1. This is the one site where a 16-byte
  pattern is *not* unique — `push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]; test esi,esi` is
  a very common predicate shape. The 24-byte pattern is required.

Argument reads at the breakpoint address (all four are read at function entry, before the
prologue runs, so `ESP` is the caller's `ESP` minus 4 for the return address):

| slot | ABI | `this` | `a2` | `a3` |
|---|---|---|---|---|
| 0 | `__thiscall(this, a2, a3)` | `ECX` | `[esp+4]` (= `[ebp+8]` after the prologue) | `[esp+8]` |
| 1 | `__thiscall(this, event)` | `ECX` | `[esp+4]` (= `[ebp+8]`) | `[esp+8]` (not an argument; logged raw) |
| 2 | `__thiscall(int16_t* hp, int value)` | `ECX` = the health object | `[esp+4]` = the value about to be written to `+0x5C` | `[esp+8]` |
| 3 | plain call, entity on the stack | `ECX` is **not** an object here | `[esp+4]` = entity | `[esp+8]` |

The `[ebp+8]` in the decoded column is the evidence for the `[esp+4]` claim: the prologue loads
the first stack argument immediately, so the count of stack arguments and their positions are
read out of the code rather than assumed.

---

## 2. `src/combat_observe.h` (new file, full contents)

```cpp
#pragma once
// Stage-2A combat observation — E2-combat.md §1.2. See combat_observe.cpp for the sites, the byte
// patterns and the log format.
//
// OBSERVATION ONLY. Four hardware execution breakpoints (DR0-DR3) plus a VEH; no byte patching, no
// writes to game memory, no calls into the engine. It cannot change combat behaviour.
//
// Default OFF via [Coop] CombatObserve=0. When off, Init() logs one line and returns BEFORE the VEH
// is registered, so no exception in the process can ever reach this unit.
class CombatObserve
{
public:
    static void Init();            // DLL_PROCESS_ATTACH, next to DerefGuard::Init()
    static void Shutdown();        // DLL_PROCESS_DETACH — clears every DR and removes the VEH
    static void OnThreadAttach();  // DLL_THREAD_ATTACH — DRs are per-thread; call from the new thread
    static bool Armed();           // true between a successful arm and a completed disarm
};
```

---

## 3. `src/combat_observe.cpp` (new file, full contents)

```cpp
// Stage-2A combat observation — E2-combat.md §1.2, made installable for NET to wire up.
//
// WHAT THIS IS
// ------------
// Four hardware EXECUTION breakpoints, one per site, armed in DR0..DR3, serviced by a first-chance
// vectored exception handler. One log line per hit:
//
//   [CO] site=<n> eip=<addr> ret=<addr> this=<ptr> a2=<ptr> a3=<ptr> life_before=<n> alive=<0|1>
//
// OBSERVATION ONLY: no bytes are patched, `VirtualProtect` is never called on the module, no game
// function is called from the handler, and no game memory is written. The only writes are to the
// debug registers (via SetThreadContext) and to this unit's log file. It cannot alter combat
// behaviour.
//
// WHY HARDWARE BREAKPOINTS AND NOT AN E9 PATCH (unlike deref_guard/action_guard):
// an execution breakpoint fires on the first byte of the function, so no prologue length has to be
// known and no bytes have to be copied or restored. The observational phase of E2 must leave .text
// byte-for-byte intact, so the interface the rest of the programme is measuring cannot move.
//
// THE FOUR SITES (AC4BFSP.exe, sha256 bc11ad79…, image base 0x00400000, no ASLR, no .reloc)
// ----------------------------------------------------------------------------------------
//   0  0x017106C0  __thiscall(this, a2, a3)   the damage filter under test (E2.1)
//        55 8B EC 8B 45 08 8B 80 9C 00 00 00 8B D0 80 E2
//        push ebp; mov ebp,esp; mov eax,[ebp+8]; mov eax,[eax+0x9C]; mov edx,eax; and dl,7
//        a2=[esp+4] (the [ebp+8] load above), a3=[esp+8].
//   1  0x0173E190  __thiscall(this, event)    the reaction handler (E2.2)
//        55 8B EC 83 EC 24 53 56 57 8B 7D 08 8B D9 8B CF
//        push ebp; mov ebp,esp; sub esp,0x24; push ebx; push esi; push edi;
//        mov edi,[ebp+8]; mov ebx,ecx        -> event=[esp+4], this=ECX.
//   2  0x019FB4C0  __thiscall(int16_t* hp, int value)   ground truth: the life write
//        55 8B EC 56 8B F1 E8 55 40 20 00 85 C0 8B 45 08
//        push ebp; mov ebp,esp; push esi; mov esi,ecx; call …; test eax,eax; mov eax,[ebp+8]
//        this=ECX=the health object (the `mov esi,ecx` proves it), value=[esp+4], Life at +0x5C.
//   3  0x0133F550  plain call: entity on the stack    the aliveness predicate
//        55 8B EC 56 8B 75 08 85 F6 75 05 32 C0 5E 5D C3 56 E8 8A 30 3C FF 83 C4
//        push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]; test esi,esi; jnz +5; xor al,al;
//        pop esi; pop ebp; ret; push esi; call …
//        entity=[esp+4]. ECX is NOT an object at this site; it is logged raw and is not a `this`.
//
// Uniqueness in .text, overlapping search, pinned build: 0->1 match at 16 bytes (8B:54, 12B:12);
// 1->1 at 16 (8B:72, 12B:3); 2->1 at 16 (8B:176, 9B:1); 3->1 at 24 (8B:333, 12B:7, 16B:7 —
// 16 bytes is NOT unique here, 24 is required).
//
// WHAT life_before / alive MEAN, AND WHY SOME SITES CANNOT FILL THEM
// -----------------------------------------------------------------
// The engine offers exactly two read-only ways to learn a life value without calling anything:
// site 2 hands us the health object in ECX, and site 0 exposes a candidate at
// *(*(this+4)+28) — the value E2 §1.3 calls svc_arg. So:
//
//   site 2: life_before = *(int16_t*)(this+0x5C), read before the instruction executes. Exact.
//   site 0: the candidate chain is probed (SEH) and read only if both dereferences are readable;
//           it is a HYPOTHESIS field (E2 §1.3 U3), not a fact, which is precisely why the
//           experiment wants it logged next to site 2's `this`.
//   site 1, 3: life_before = -1 (unknown). We deliberately do NOT call FN_GET_SERVICE(0x10EF8C0)
//           from the handler: a call into the engine from inside a VEH is a reentrancy and
//           allocation hazard on a thread we have just stopped mid-instruction, and it would add a
//           fifth address to the patch that E2 never byte-verified. -1 is honest; a fabricated 0
//           would read as "dead" in the analysis and corrupt the C1/C3 verdicts.
//
// alive is derived, never asserted: 0 only when we can PROVE death from the two facts we hold
// (life_before <= 0, or the service flags word at +0x04 has 0x1000 — the E2 §1 formula), and 1
// otherwise, including when the life is unknown. A "1" therefore means "not observed dead", not
// "the predicate returned true". The predicate itself is site 3; calling it from inside a
// breakpoint set on its own entry would re-enter this handler.
//
// DEBUG REGISTERS
// ---------------
// Execution breakpoint = Ln enabled, RWn=00, LENn=00. Four slots:
//   DR7 = 0x55  (L0|L1|L2|L3 = bits 0,2,4,6; every RW/LEN field 00, LE/GE clear)
// E2 §1.2 sketches 0x00000555; that also sets bit 8 (LE, the legacy "exact vs inexact" selector)
// and bit 10 (reserved). Neither is needed for four exact execution breakpoints, so this unit uses
// 0x55 and leaves the reserved bit alone.
//
// Single-step cycle, which is the part that crashes if it is wrong:
//   - an execution breakpoint reports BEFORE the instruction runs, with EIP == the breakpoint, so
//     continuing without touching DR7 re-fires it forever;
//   - on a hit we clear only that slot's Ln bit and set EFlags.TF, then continue;
//   - the single-step trap arrives with DR6.BS set and EFlags.TF still set — that pair is what
//     identifies it as OURS (this unit is the only thing in the process that writes TF), so no
//     per-thread table is needed and two threads hitting the same slot cannot confuse each other;
//   - on the step we re-arm all slots, clear TF and clear DR6.

#include "combat_observe.h"
#include "Hook.h"
#include "build_gate.h"
#include "pattern_scan.h"

#include <windows.h>
#include <tlhelp32.h>
#include <process.h>
#include <stdio.h>
#include <share.h>
#include <cstdint>
#include <cstdarg>

namespace {

const uintptr_t VA_IMAGE_BASE = 0x00400000;   // the module is not ASLR-relocated (see Hook.h)
const char*     INI_PATH      = ".\\AC4Coop.ini";

const size_t    SITE_COUNT    = 4;            // one DR each: 4 slots, 4 sites, no headroom
const size_t    PATTERN_MAX   = 32;
const LONG      MAX_HITS      = 5000;         // hard cap; the log stops at exactly this many lines
const size_t    MAX_SEEN_TIDS = 256;
const DWORD     REARM_MS      = 2000;         // new-thread sweep cadence
const int       HOTKEY_VK     = VK_F2;        // disarm key; see the integration note

const DWORD     DR7_EXEC4     = 0x00000055;   // L0..L3 enabled, RWn=00 LENn=00 for all four
const DWORD     DR6_BS        = 0x00004000;   // single-step trap bit
const DWORD     EFLAGS_TF     = 0x00000100;

enum State { ST_OFF = 0, ST_ARMED = 1, ST_STOPPING = 2 };

// `va` is the documented VA on the pinned build. It is the FALLBACK route only: each site is
// normally found by pattern, and the literal is used only when the pattern finds zero matches on a
// build BuildGate already trusts. `pattern` is the build-identity check as well as the locator.
struct SiteDesc {
    const char* name;
    uintptr_t   va;
    const char* pattern;
};

const SiteDesc kSites[SITE_COUNT] = {
    { "dmg-filter  0x17106C0", 0x017106C0u,
      "55 8B EC 8B 45 08 8B 80 9C 00 00 00 8B D0 80 E2" },
    { "dmg-react   0x173E190", 0x0173E190u,
      "55 8B EC 83 EC 24 53 56 57 8B 7D 08 8B D9 8B CF" },
    { "set-life    0x19FB4C0", 0x019FB4C0u,
      "55 8B EC 56 8B F1 E8 55 40 20 00 85 C0 8B 45 08" },
    { "alive       0x133F550", 0x0133F550u,
      "55 8B EC 56 8B 75 08 85 F6 75 05 32 C0 5E 5D C3 56 E8 8A 30 3C FF 83 C4" },
};

// ---- state ---------------------------------------------------------------------------------
bool     g_entry     = false;
FILE*    g_log       = nullptr;
PVOID    g_veh       = nullptr;
HANDLE   g_reporter  = nullptr;
DWORD    g_reporterTid = 0;

uintptr_t g_va[SITE_COUNT];         // resolved address per slot
bool      g_siteOk[SITE_COUNT];     // pattern verified at the resolved address

volatile LONG g_state = ST_OFF;     // State
volatile LONG g_stop  = 0;          // 1 = disarmequested (cap or hotkey)
volatile LONG g_hits  = 0;          // total hits logged
volatile LONG g_inLog = 0;          // logger reentrancy guard

// Threads this unit has armed. A fixed table rather than a Toolhelp walk at shutdown, because
// Shutdown() runs from DLL_PROCESS_DETACH under the loader lock and must not allocate.
DWORD         g_seenTid[MAX_SEEN_TIDS];
volatile LONG g_seenCount = 0;

// Same shape as the other units' loggers: debugger + a live-readable file, never a blocking
// console write. Called from the game thread inside a VEH, so it is bounded and reentrancy-guarded.
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
// PatternScan's matcher always agree; a wildcard would make it return 0 and the site is skipped.
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

// ---- debug-register helpers ----------------------------------------------------------------
void BuildArmedContext(CONTEXT* c)
{
    DWORD dr[4] = { 0, 0, 0, 0 };
    DWORD dr7   = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!g_siteOk[i]) continue;
        dr[i] = (DWORD)g_va[i];
        dr7  |= (1u << (i * 2));          // Ln
    }
    c->Dr0 = dr[0];
    c->Dr1 = dr[1];
    c->Dr2 = dr[2];
    c->Dr3 = dr[3];
    c->Dr6 = 0;
    c->Dr7 = dr7;                          // RWn/LENn all 00 = execute, exact
}

void BuildDisarmedContext(CONTEXT* c)
{
    c->Dr0 = c->Dr1 = c->Dr2 = c->Dr3 = 0;
    c->Dr6 = 0;
    c->Dr7 = 0;
}

bool SeenTid(DWORD tid)
{
    const LONG n = g_seenCount;
    for (LONG i = 0; i < n && i < (LONG)MAX_SEEN_TIDS; ++i)
        if (g_seenTid[i] == tid) return true;
    return false;
}

void RememberTid(DWORD tid)
{
    const LONG slot = InterlockedIncrement(&g_seenCount) - 1;
    if (slot >= 0 && slot < (LONG)MAX_SEEN_TIDS) g_seenTid[slot] = tid;
    else InterlockedDecrement(&g_seenCount);   // table full: stop growing, keep the first 256
}

// Applies one context edit to another thread. The thread is suspended only for the duration of
// Get/SetThreadContext (microseconds); nothing here takes the loader lock or allocates, so a
// thread suspended while holding either is not a deadlock for us.
bool EditThreadContext(DWORD tid, bool arm)
{
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                           FALSE, tid);
    if (!th) return false;

    const bool self = (tid == GetCurrentThreadId());
    DWORD prev = 0;
    if (!self) {
        prev = SuspendThread(th);
        if (prev == (DWORD)-1) { CloseHandle(th); return false; }
    }

    bool ok = false;
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    // Arming writes only the DR block. Disarming also asks for CONTEXT_CONTROL so TF can be
    // cleared: a thread suspended in the middle of a step-over keeps TF in its own context, and
    // once the VEH is removed that pending single-step would be delivered to the engine's SEH.
    c.ContextFlags = arm ? CONTEXT_DEBUG_REGISTERS
                         : (CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL);
    if (GetThreadContext(th, &c)) {
        if (arm) {
            BuildArmedContext(&c);
        } else {
            BuildDisarmedContext(&c);
            c.EFlags &= ~EFLAGS_TF;
        }
        ok = SetThreadContext(th, &c) != 0;
    }

    if (!self) ResumeThread(th);
    CloseHandle(th);
    return ok;
}

void ArmAllThreads()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log("[CO] thread snapshot failed (%lu); only the calling thread is armed\n",
            (unsigned long)GetLastError());
        if (EditThreadContext(GetCurrentThreadId(), true)) RememberTid(GetCurrentThreadId());
        return;
    }

    const DWORD self = GetCurrentThreadId();
    const DWORD pid  = GetCurrentProcessId();
    size_t armed = 0, failed = 0;

    THREADENTRY32 te;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self || te.th32ThreadID == g_reporterTid) continue;
            if (SeenTid(te.th32ThreadID)) continue;
            if (EditThreadContext(te.th32ThreadID, true)) { RememberTid(te.th32ThreadID); ++armed; }
            else ++failed;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    if (EditThreadContext(self, true)) { RememberTid(self); ++armed; }

    // The report says `armed`, and a session whose simulation thread is missing is INVALID (E2
    // §1.4). A failed arm is a missing measurement, never a wrong one.
    Log("[CO] armed %u thread(s), %u refused\n", (unsigned)armed, (unsigned)failed);
}

// Only threads that appeared after the initial arm, so an already-armed thread (possibly mid
// single-step) is never rewritten.
void ArmUnseenThreads()
{
    if (g_state != ST_ARMED) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    const DWORD self = GetCurrentThreadId();
    const DWORD pid  = GetCurrentProcessId();
    size_t armed = 0;

    THREADENTRY32 te;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self || te.th32ThreadID == g_reporterTid) continue;
            if (SeenTid(te.th32ThreadID)) continue;
            if (EditThreadContext(te.th32ThreadID, true)) { RememberTid(te.th32ThreadID); ++armed; }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    if (armed) Log("[CO] re-armed %u new thread(s)\n", (unsigned)armed);
}

// Reads every thread this unit armed back to "no debug registers". Used by the reporter (cap,
// hotkey) and by Shutdown(), both of which must not allocate, hence the cached table.
void DisarmAllThreads()
{
    const LONG n = g_seenCount;
    size_t done = 0;
    for (LONG i = 0; i < n && i < (LONG)MAX_SEEN_TIDS; ++i)
        if (EditThreadContext(g_seenTid[i], false)) ++done;
    Log("[CO] disarmed %u/%u thread(s)\n", (unsigned)done, (unsigned)(n > 0 ? n : 0));
}

// ---- life / alive, read-only ----------------------------------------------------------------
// `hp` is the object site 2 is about to write. Exact: we break before the instruction executes.
bool ServiceState(const void* hp, int* life, int* alive)
{
    if (!Readable(hp, 0x60)) return false;          // covers +0x04 and +0x5C
    __try {
        const int16_t  v     = *(const int16_t*)((const BYTE*)hp + 0x5C);
        const uint32_t flags = *(const uint32_t*)((const BYTE*)hp + 0x04);
        *life  = (int)v;
        *alive = (v > 0 && (flags & 0x1000) == 0) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Site 0's candidate service: *(*(this+4)+28), the value E2 §1.3 logs as svc_arg. Two SEH-probed
// dereferences, no call. It is a hypothesis, so a failure just leaves life_before at -1.
bool FilterCandidate(const void* thisPtr, const void** svcOut)
{
    if (!Readable(thisPtr, 8)) return false;
    const void* p = nullptr;
    __try {
        p = *(const void* const*)((const BYTE*)thisPtr + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!p || !Readable(p, 32)) return false;
    const void* svc = nullptr;
    __try {
        svc = *(const void* const*)((const BYTE*)p + 28);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *svcOut = svc;
    return svc != nullptr;
}

// ---- the handler ----------------------------------------------------------------------------
void LogHit(size_t site, const CONTEXT* c)
{
    const LONG n = InterlockedIncrement(&g_hits);
    if (n > MAX_HITS) {
        if (InterlockedExchange(&g_stop, 1) == 0)
            Log("[CO] hit cap %u reached; disarming\n", (unsigned)MAX_HITS);
        return;                                     // the file stops at exactly MAX_HITS lines
    }

    // At entry ESP points at the return address; the prologue has not run yet.
    uint32_t ret = 0, a2 = 0, a3 = 0;
    __try {
        const uint32_t* sp = (const uint32_t*)(uintptr_t)c->Esp;
        ret = sp[0];
        a2  = sp[1];
        a3  = sp[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ret = a2 = a3 = 0;
    }

    int life = -1, alive = 1;                       // 1 = "not observed dead", see the header
    if (site == 2) {
        if (!ServiceState((const void*)(uintptr_t)c->Ecx, &life, &alive)) { life = -1; alive = 1; }
    } else if (site == 0) {
        const void* svc = nullptr;
        if (FilterCandidate((const void*)(uintptr_t)c->Ecx, &svc))
            if (!ServiceState(svc, &life, &alive)) { life = -1; alive = 1; }
    }

    Log("[CO] site=%u eip=%08X ret=%08X this=%08X a2=%08X a3=%08X life_before=%d alive=%d\n",
        (unsigned)site, (unsigned)g_va[site], ret, (unsigned)c->Ecx, a2, a3, life, alive);
}

// Called inside CombatVeh's __try. Returns a disposition; never returns normally on a fault.
LONG HandleSingleStep(PEXCEPTION_POINTERS ep)
{
    CONTEXT* c = ep->ContextRecord;
    const DWORD dr6 = (DWORD)c->Dr6;

    // (a) our own step-over: TF still set + DR6.BS. This unit is the only writer of TF.
    if (dr6 & DR6_BS) {
        if ((c->EFlags & EFLAGS_TF) == 0) return EXCEPTION_CONTINUE_SEARCH;   // not ours
        c->EFlags &= ~EFLAGS_TF;
        c->Dr6 = 0;
        if (g_state == ST_ARMED) BuildArmedContext(c);   // re-arm; while stopping, stay disarmed
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // (b) a breakpoint hit: DR6 bits 0..3 name the slot.
    const DWORD hit = dr6 & 0x0Fu;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;

    DWORD ours = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!(hit & (1u << i))) continue;
        if (!g_siteOk[i] || (uintptr_t)c->Eip != g_va[i]) continue;   // another DR user's slot
        if (g_state == ST_ARMED && g_stop == 0) LogHit(i, c);
        ours |= (1u << i);
    }
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;

    // Stop the immediate re-trigger at the same EIP, step the instruction, re-arm on the step.
    for (size_t i = 0; i < SITE_COUNT; ++i)
        if (ours & (1u << i)) c->Dr7 &= ~(1u << (i * 2));
    c->Dr6 = 0;
    c->EFlags |= EFLAGS_TF;
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG CALLBACK CombatVeh(PEXCEPTION_POINTERS ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (g_state == ST_OFF) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;

    __try {
        return HandleSingleStep(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A fault inside the handler must never take the process down: put the thread back into a
        // runnable state (no TF, no DRs of ours) and let the engine's own instruction execute.
        CONTEXT* c = ep->ContextRecord;
        c->EFlags &= ~EFLAGS_TF;
        c->Dr6 = 0;
        c->Dr7 = 0;
        OutputDebugStringA("[CO] handler fault; debug registers cleared for this thread\n");
        InterlockedExchange(&g_stop, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

// ---- background thread ------------------------------------------------------------------
void BeginDisarm(const char* why)
{
    if (InterlockedCompareExchange(&g_state, ST_STOPPING, ST_ARMED) != ST_ARMED) return;
    Log("[CO] disarming (%s) after %u hit(s)\n", why, (unsigned)g_hits);
    DisarmAllThreads();
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    InterlockedExchange(&g_state, ST_OFF);
    Log("[CO] disarmed; no debug registers remain\n");
}

unsigned __stdcall ReporterThread(void*)
{
    DWORD lastSweep = GetTickCount();
    for (;;) {
        Sleep(100);
        if (g_state == ST_OFF) break;

        if (g_stop) { BeginDisarm("hit cap or hotkey"); continue; }

        if ((GetAsyncKeyState(HOTKEY_VK) & 0x8000) != 0) {
            InterlockedExchange(&g_stop, 1);
            BeginDisarm("hotkey");
            continue;
        }

        const DWORD now = GetTickCount();
        if (now - lastSweep >= REARM_MS) { lastSweep = now; ArmUnseenThreads(); }
    }
    return 0;
}

// ---- resolution -------------------------------------------------------------------------
void ResolveSites(HMODULE game, uintptr_t moduleBase)
{
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        g_va[i]     = 0;
        g_siteOk[i] = false;

        int hits = 0;
        const uintptr_t found = PatternScan::Find(game, kSites[i].pattern, &hits);

        uintptr_t va = 0;
        const char* how = nullptr;
        if (hits == 1) {
            va = found;
            how = "pattern, 1 match";
        } else if (hits == 0 && BuildGate::Trusted()) {
            // BuildGate::Trusted() was required by Init(), so the literal route is the known-build
            // route; the byte verification below still runs before the site is ever armed.
            va = moduleBase + (kSites[i].va - VA_IMAGE_BASE);
            how = "literal (0 matches, known build)";
        } else {
            Log("[CO] %s: pattern=%d matches -> not observed\n", kSites[i].name, hits);
            continue;
        }

        BYTE want[PATTERN_MAX];
        const size_t n = ParsePattern(kSites[i].pattern, want, sizeof(want));
        if (n == 0 || !Readable((const void*)va, n) ||
            memcmp((const void*)va, want, n) != 0) {
            Log("[CO] %s: bytes at 0x%08X do not match the pattern -> not observed\n",
                kSites[i].name, (unsigned)va);
            continue;
        }

        g_va[i]     = va;
        g_siteOk[i] = true;
        Log("[CO] %s resolved 0x%08X (%u-byte pattern, %s)\n",
            kSites[i].name, (unsigned)va, (unsigned)n, how);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------------------------

void CombatObserve::Init()
{
    if (g_entry) return;
    g_entry = true;

    // Fail closed on arming, not on loading: an unverified build keeps this module resident and
    // silent, and the process still reaches the menu (same rule as the two guards).
    if (!BuildGate::Trusted()) {
        OutputDebugStringA("[CO] skipped: build not trusted\n");
        return;
    }

    // ---- config gate FIRST ---------------------------------------------------------------
    // Nothing below this line runs when the switch is off: no VEH is registered, so no exception
    // in the process can ever be delivered to this unit, and no thread context is touched.
    if (GetPrivateProfileIntA("Coop", "CombatObserve", 0, INI_PATH) == 0) {
        OutputDebugStringA("[CO] disabled by [Coop] CombatObserve=0\n");
        return;
    }

    // Several processes in AC4's launch chain load this ASI; only the real game has the module.
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) {
        OutputDebugStringA("[CO] install skipped: ac4bfsp.exe not loaded\n");
        return;
    }

    // Game-folder log, live-readable (shared write denied). PID-suffixed so a stale process cannot
    // hold the file and swallow a later run's evidence.
    if (!g_log) {
        char logPath[64];
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE,
                    ".\\AC4Coop_combat_observe_%u.log", (unsigned)GetCurrentProcessId());
        g_log = _fsopen(logPath, "a", _SH_DENYWR);
    }

    Log("[CO] stage 2A observer starting (observation only: no byte patch, no game memory writes)\n");

    ResolveSites(game, (uintptr_t)game);

    size_t ok = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) if (g_siteOk[i]) ++ok;
    if (ok == 0) {
        Log("[CO] install FAILED: no site matched -> nothing armed\n");
        return;
    }
    if (ok < SITE_COUNT) {
        // Still worth running: E2 §1.4 needs slot 3 as the session-validity witness, so a missing
        // slot is recorded rather than silently absent.
        Log("[CO] WARNING: %u of %u sites unresolved; the log will not cover them\n",
            (unsigned)(SITE_COUNT - ok), (unsigned)SITE_COUNT);
    }

    g_veh = AddVectoredExceptionHandler(1, CombatVeh);   // first-chance, same call as phase1 DrTrap
    if (!g_veh) {
        Log("[CO] install FAILED: AddVectoredExceptionHandler\n");
        return;
    }

    // State before arming: the very first trap must find ST_ARMED, otherwise it is passed on to
    // the engine's SEH and the process dies on an unhandled single-step.
    InterlockedExchange(&g_state, ST_ARMED);

    g_reporter = (HANDLE)_beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, (unsigned*)&g_reporterTid);
    if (!g_reporter) Log("[CO] reporter thread failed; arming without the sweep\n");

    ArmAllThreads();

    Log("[CO] armed; hit cap %u, disarm with VK 0x%02X\n",
        (unsigned)MAX_HITS, (unsigned)HOTKEY_VK);
}

void CombatObserve::OnThreadAttach()
{
    // DRs are per-thread. This is the only correct place to arm a thread that did not exist when
    // Init() ran; the reporter's 2 s sweep is the fallback for a call that never arrives.
    if (g_state != ST_ARMED) return;
    const DWORD self = GetCurrentThreadId();
    if (self == g_reporterTid || SeenTid(self)) return;
    if (EditThreadContext(self, true)) RememberTid(self);
}

void CombatObserve::Shutdown()
{
    if (!g_entry) return;

    // Order matters. The VEH must keep servicing single-steps while the DRs are being cleared, so
    // the state machine goes STOPPING (handle traps, never re-arm, never log) before the sweep,
    // and only becomes OFF after the VEH is gone.
    const LONG prev = InterlockedExchange(&g_state, ST_STOPPING);
    if (prev == ST_ARMED) DisarmAllThreads();
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    InterlockedExchange(&g_state, ST_OFF);

    if (g_log) { fclose(g_log); g_log = nullptr; }
    g_reporter = nullptr;   // the reporter exits on its next tick (g_state == ST_OFF); no join here
}
```

---

## 4. Project + INI integration

### 4.1 `.vcxproj` — exactly two lines

The project lists its units as individual `ClCompile` entries with flat object names (the audit
rebuild tree carries `action_guard.obj`, `deref_guard.obj`, `dllmain.obj`, …, one per file), so
two lines are added, following the existing entries' `Include` spelling verbatim:

```xml
  <ItemGroup>
    <ClCompile Include="src\action_guard.cpp" />
    <ClCompile Include="src\combat_observe.cpp" />   <!-- ADD: stage 2A observer -->
  </ItemGroup>

  <ItemGroup>
    <ClInclude Include="src\action_guard.h" />
    <ClInclude Include="src\combat_observe.h" />     <!-- ADD -->
  </ItemGroup>
```

No new include path, no new library: `tlhelp32.h` and `Thread32First`/`OpenThread`/
`SetThreadContext` are SDK + `kernel32.lib`, both already in use by the two guard units.

### 4.2 `dllmain.cpp` — the three call sites (NET's file, not edited here)

Without these the unit compiles and does nothing, which is the safe failure:

```cpp
case DLL_PROCESS_ATTACH:
    ...
    DerefGuard::Init();
    ActionGuard::Init();
    CombatObserve::Init();          // ADD — reads [Coop] CombatObserve, default 0

case DLL_THREAD_ATTACH:
    CombatObserve::OnThreadAttach(); // ADD — DRs are per-thread

case DLL_PROCESS_DETACH:
    CombatObserve::Shutdown();       // ADD — before the CRT teardown of this module
    ...
```

`Shutdown()` allocates nothing and takes no loader lock (its thread list is the fixed
`g_seenTid[]` table, not a Toolhelp walk), so it is safe at detach; it calls `fclose`, which the
project already does from detach paths.

### 4.3 INI block

Append to the existing `[Coop]` section of `AC4Coop.ini`:

```ini
[Coop]
; ---- Stage 2A combat observer (E2-combat.md §1.2) -------------------------------------------
; 0 = off (default). 1 = install four hardware execution breakpoints (DR0-DR3) on
; 0x17106C0 / 0x173E190 / 0x19FB4C0 / 0x133F550 and write one "[CO] site=..." line per hit.
; OBSERVATION ONLY: no byte patches, no calls into the engine, no game-memory writes, so it
; cannot change combat behaviour. Capped at 5000 hits. Press F2 in game to disarm early.
; Leaves .text byte-for-byte untouched for the whole session.
CombatObserve=0
```

---

## 5. Verification checklist (no game required)

### V1 — compiles with the same toolset

Build the ASI the way the project already builds it (same platform, **x86/Win32** — the pinned
image base `0x00400000` and every `CONTEXT` field used here are 32-bit).

Already done for the two files as printed above, by extracting the two ```cpp blocks verbatim from
this document and compiling them against the real `ref/AC4Coop/src` headers:

```
call "<VS2022>\VC\Auxiliary\Build\vcvarsall.bat" x86 && cl /nologo /c /W3 /EHsc ^
     /D_CRT_SECURE_NO_WARNINGS /I"<...>\ref\AC4Coop\src" /Fo:co.obj combat_observe.cpp
```

Result: **exit 0, zero warnings, `co.obj` 17 494 bytes.** (One fix was needed to reach that state
and is already in the source above: `#include <process.h>` for `_beginthreadex`.) The headers
consumed are `combat_observe.h` plus the project's existing `Hook.h` (`GameModuleBase`),
`build_gate.h` (`BuildGate::Trusted`) and `pattern_scan.h` (`PatternScan::Find`) — the same four
dependencies `deref_guard.cpp` and `action_guard.cpp` already have, so no include path or library
changes are needed.

Offline wiring check, which proves the unit cannot *silently* do nothing after integration:

```
dumpbin /symbols AC4Coop.asi | findstr /i "CombatObserve CombatVeh ReporterThread"
```

expect `CombatObserve::Init`, `CombatObserve::Shutdown`, `CombatObserve::OnThreadAttach`,
`CombatObserve::Armed` and the internal `CombatVeh`. The linker strips unreferenced members, so
this line is also the check that the `dllmain.cpp` call sites were actually added.

### V2 — the pattern scan finds each site exactly once on the pinned exe

Run this against the pinned binary (`sha256 bc11ad79…`, 45 056 040 bytes). It parses the PE head
itself, maps VA→file offset through the section table, and counts overlapping matches of each
pattern across all of `.text`:

```python
# ops/tools/e2_site_patterns.py   <path to AC4BFSP.exe>
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
for va, p in PAT.items():
    b = bytes(int(x, 16) for x in p.split())
    pos, c = blob.find(b), 0
    first = pos
    while pos >= 0:
        c += 1
        pos = blob.find(b, pos + 1)
    ok = (c == 1 and first == va - base - tva)
    print('%08X  len=%2d  matches=%d  at_va=%s  %s' %
          (va, len(b), c, 'yes' if first == va - base - tva else 'NO', 'OK' if ok else 'FAIL'))
```

Recorded output for the pinned build (run 2026-10-29, this seat, `py ops/tools/e2_site_patterns.py "<exe>"`):

```
sha256 bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150
text   0x00401000..0x01E39A00 (27494912 bytes)
0133F550  len=24  matches=1  at_va=yes  OK
017106C0  len=16  matches=1  at_va=yes  OK
0173E190  len=16  matches=1  at_va=yes  OK
019FB4C0  len=16  matches=1  at_va=yes  OK
```

(The script prints in ascending VA order; the table above is in slot order.)

The same script doubles as the offline oracle for `ResolveSites()`: `hits == 1` for all four is
exactly what the unit logs as `(16-byte pattern, 1 match)`.

**If any count is not 1, do not turn the switch on.** A count of 0 on this build means the file is
not the pinned one; a count > 1 means the pattern must be lengthened (see §1) before the patch is
usable. `ResolveSites()` refuses a site whose 0-match literal does not byte-verify, so the failure
mode is "site not observed", not "breakpoint on the wrong address".

### V3 — the arg reads match the code they are attached to

Already evidenced by the decoded prologues in §1: each site's prologue loads `[ebp+8]` into the
register later used as the event/entity/value, and site 2 copies `ECX` into `ESI` before writing
`+0x5C`. This is what licenses `a2 = [esp+4]` at function entry; `[esp]` is the return address by
definition. No disassembler is needed to re-derive it.

### V4 — the VEH cannot fire when the config is off

Three independent facts, all checkable without the game:

1. `GetPrivateProfileIntA(... "CombatObserve", 0, ...)` sits **above** both
   `AddVectoredExceptionHandler` and every `EditThreadContext` call in `Init()`, and its `return`
   is unconditional. With the key absent, the default `0` takes the same path.
2. `g_state` is only ever set away from `ST_OFF` at the one `InterlockedExchange(&g_state,
   ST_ARMED)` after the VEH exists; `CombatVeh` returns `EXCEPTION_CONTINUE_SEARCH` on
   `g_state == ST_OFF` before touching anything, so even a hypothetical handler entry is inert.
3. No `DRn` is written: `BuildArmedContext` is reachable only from `EditThreadContext(..., true)`,
   which is reachable only from `ArmAllThreads`, `ArmUnseenThreads` and `OnThreadAttach`, all of
   which require `g_state == ST_ARMED`.

Runtime confirmation on a run with `CombatObserve=0` (after the offline gate): **no log file is
created at all** — the `_fsopen` call is below the gate, so `AC4Coop_combat_observe_<pid>.log`
never appears. The only trace is the single debugger line
`[CO] disabled by [Coop] CombatObserve=0` (DebugView / `OutputDebugString`), and zero `[CO] site=`
lines anywhere. With `CombatObserve=1` and zero combat, expect `site=3` lines at most (the
aliveness predicate can be polled) and **no** `site=0/1/2` lines — that is E2's C2 check, which
this instrument makes measurable.

### V5 — it cannot write game memory or change behaviour

Grep the file for `VirtualProtect(`, `WriteProcessMemory(`, `memcpy(`, `FlushInstructionCache(`,
`WriteFile(` and it returns nothing — the words `VirtualProtect` and `memcpy` appear only inside
the comments that say they are not used. The only in-unit `memset` calls are the three that zero a
stack `CONTEXT` / `THREADENTRY32` (`memset(&c, 0, sizeof(c))`, `memset(&te, 0, sizeof(te))` ×2),
i.e. locals, never a game address. The only writes to anything outside the module are
`SetThreadContext` (the debug registers of a thread we hold a handle to) and `fputs`. There is
no hook, no patch, no cave, and no call into the engine — `FN_GET_SERVICE` is deliberately not
called (see the header comment), so even the read chain at site 0 is a probe of two pointers.

Grep the built ASI's `.text` for the four site VAs after integration:
`findstr` on a dump is not required; the log lines `[CO] … resolved 0x017106C0` are the check.

### V6 — DR7 value

`0x55`, not `0x555`. A breakpoint armed with L0..L3 and every RW/LEN field `00` traps *before* the
instruction and reports `EIP == the breakpoint address`; the handler's slot match
(`(uintptr_t)c->Eip != g_va[i]`) is what proves the value is right the first time a trap arrives.
If it were wrong, the first `[CO] site=` line would simply never appear while the game kept
running.

---

## 6. Fixes applied to the E2 sketch (worth knowing before reading the old code)

| E2 §1.2 | this patch | why |
|---|---|---|
| `c.Dr7 = 0x00000555` | `0x55` | `0x555` additionally sets LE (bit 8) and the reserved bit 10; four exact execution breakpoints need only L0..L3. |
| `EXCEPTION_BREAKPOINT` accepted in the handler | only `EXCEPTION_SINGLE_STEP` | nothing here emits `0xCC`; accepting `STATUS_BREAKPOINT` would steal another tool's `int3`. |
| slot chosen by `SlotFor(ip)` scan | `DR6 & 0x0F` then `EIP == g_va[slot]` | `DR6` names the slot the CPU actually hit; the EIP compare rejects a slot another instrument owns. |
| TF re-arm "on the single-step exception" (unqualified) | `DR6.BS && (EFlags & TF)` | TF is the only thing that identifies the step as ours; a global/tid table can be confused by two threads in the same slot. |
| arming every thread at install only | + `OnThreadAttach()` + 2 s unseen-tid sweep | DRs are per-thread; a thread spawned after install is otherwise silently unobserved (E2 R6). |
| `life_after` from a re-pointed DR0 at `ret` | not implemented | burning a slot on the return address halves the sites; `life_after` remains a second-session question. |
| reporter does `fputs` after `Sleep(5000)` | 100 ms control tick, logging inline in the handler | a 5 s drain cannot stop a 5000-hit flood in time; the cap is enforced at the hit. |

---

## 7. Risks

| id | what could crash / mislead | how this code prevents it |
|---|---|---|
| **R1** | **A fault inside the VEH.** The handler reads `[esp]`, `[esp+4]`, `[esp+8]`, `ECX` as an object, and dereferences `this+4 → +28`. A garbage `ECX` at site 0 would fault *inside* an exception callback, and a second fault there is fatal. | The whole handler body is inside `__try/__except`. On any fault: TF cleared, `DR6`/`DR7` zeroed for that thread, a fixed string via `OutputDebugStringA` only (no CRT formatting on the fault path), stop requested. Every pointer read is also fronted by `Readable()`, which is an actual attempted read under its own `__except` — the same probe `action_guard.cpp` uses. |
| **R2** | **Unhandled single-step reaching the engine's SEH.** If a slot is cleared and TF set but our handler does not claim the step, the game's exception machinery sees a `STATUS_SINGLE_STEP` it never asked for. The dangerous window is teardown: a thread suspended mid step-over keeps TF in its context, and clearing the DRs does not clear TF. | The step is claimed by `DR6.BS && EFlags.TF` — a pair only this unit creates. `g_state` is set to `ST_ARMED` *before* the first `SetThreadContext`, so the first trap cannot be missed. Disarm asks for `CONTEXT_CONTROL` and clears TF in the same write that clears the DRs, so no orphan single-step survives past `RemoveVectoredExceptionHandler`. On a handler fault the thread is returned with TF and DRs clear, i.e. runnable. |
| **R3** | **Another instrument owns a debug register.** A profiler, an anti-cheat, another seat's `DrTrap`. | Four slots, four sites, zero headroom: a site whose EIP does not match is passed on untouched (`EXCEPTION_CONTINUE_SEARCH`), and the arming code only writes DR values for sites whose pattern byte-verified. **Do not run this together with another DR instrument**; if `armed N thread(s)` reports fewer threads than the session needs, the session is INVALID (E2 §1.4). |
| **R4** | **A thread spawned after install is never observed** — silent missing data, which is worse than a crash because it looks like "the filter did not fire". | `OnThreadAttach()` plus the reporter's 2 s unseen-tid sweep; `Disarmed`/`re-armed %u new thread(s)` lines make the gap visible, and `armed %u thread(s)` is the number the session script records. |
| **R5** | **Suspend/SetThreadContext deadlock or corruption** — suspending a thread mid-instruction and writing its context. | The suspend window contains only `GetThreadContext`/`SetThreadContext`; no allocation, no loader lock, no CRT, and the current thread is never suspended. A thread already mid-single-step is never rewritten, because only *unseen* tids are armed by the sweep. A refused `OpenThread` is counted, not retried in a loop. |
| **R6** | **Logging from inside an exception callback on the game thread** — `fputs`/`fflush` allocate and take a lock; a nested exception while holding it hangs the process. | Fixed 512-byte stack buffer, `_vsnprintf_s(_TRUNCATE)`, an `InterlockedExchange` reentrancy guard that makes a nested call a no-op, and a hard 5000-hit cap that ends the flood at the hit rather than at the next 100 ms tick. |
| **R7** | **The instrument changes the thing it measures.** 5000 traps × (trap + one single-step) cost real microseconds, and DR traps defeat some micro-architectural fast paths. | Cap + F2 + "press F2 when the scene ends". Declare the session INVALID if frame time moves >10 % versus the same scene with `CombatObserve=0`; the unit is off by default precisely so that A/B is one INI edit. |
| **R8** | **`life_before` misread as ground truth.** Site 0's value comes from a two-dereference chain that is a hypothesis (E2 §1.3 U3), and sites 1/3 log `-1`. | `-1` is documented as "not determinable at this site" and the analysis is told to discard it; site 2's value is exact and is the only one the C1/C3 verdicts may use. `alive` is likewise documented as "not observed dead" rather than "the predicate returned true" — which is why site 3 exists as its own breakpoint. |
| **R9** | **Build drift.** A different `AC4BFSP.exe` (Steam update) has these VAs, or the same bytes at different addresses. | `BuildGate::Trusted()` gates everything; each site is located by pattern and then re-verified byte-for-byte with `memcmp` before arming; a 0-match pattern only falls back to the literal when the build is already trusted, and the literal is verified the same way. A mismatch logs `not observed` and arms nothing for that slot — it never arms a guessed address. |
| **R10** | **`DllMain` hazards.** `Init()` runs under the loader lock (opens a log, creates a thread); `Shutdown()` runs at detach. | Same operations `deref_guard.cpp` already performs at attach (PID log + `_beginthreadex`), so no new hazard class; `Shutdown()` allocates nothing and iterates a fixed table instead of walking Toolhelp, and it removes the VEH before the module can be unloaded. If a future BfLLR-style unload path drops the module without `DLL_PROCESS_DETACH`, the DRs die with the process, not with the module. |
| **R11** | **`F2` collides with a bound game key** (F1 is already the phase1 debug key). | The hotkey only *disarms* — the failure mode of a collision is "the observer stops early", never "the game does something". If NET prefers another key, change `HOTKEY_VK` in one place. |

**Programme-level caveat, restated:** this unit produces evidence, not capability. Nothing here can
be promoted to the stage-2B applier without E2's C1–C5 verdicts, and because it neither patches
nor writes, no result from it is contaminated by its own installation.
