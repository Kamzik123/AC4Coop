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
#include <string.h>
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

// Disarm hotkey. The patch proposed VK_F2; F1-F15 are all bound by phase1 (F1 health map .. F15
// CombatTest) and V / Ctrl+Alt+J / Ctrl+Alt+K / Ctrl+Alt+L are bound too, so F2 would have been a
// silent collision with phase1's health scan. Ctrl+Alt+M is free in this file (the bound letters
// are only V, J, K, L) and needs both modifiers, so a plain M in gameplay never fires.
const int       HOTKEY_VK     = 'M';          // with Ctrl+Alt held
const bool      HOTKEY_MODS   = true;

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
volatile LONG g_stop  = 0;          // 1 = disarm requested (cap or hotkey)
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

// "55 8B EC ..." from a memory range, for the mismatch record. Bounded, no allocation.
void HexDump(const void* p, size_t n, char* out, size_t cap)
{
    static const char* kHex = "0123456789ABCDEF";
    size_t o = 0;
    const BYTE* b = (const BYTE*)p;
    for (size_t i = 0; i < n && o + 4 < cap; ++i) {
        if (i) out[o++] = ' ';
        out[o++] = kHex[b[i] >> 4];
        out[o++] = kHex[b[i] & 0x0F];
    }
    out[o] = 0;
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

// Ctrl+Alt+M, edge-triggered so one press is one request.
bool HotkeyDown()
{
    if (!(GetAsyncKeyState(HOTKEY_VK) & 0x8000)) return false;
    if (!HOTKEY_MODS) return true;
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
           (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
}

unsigned __stdcall ReporterThread(void*)
{
    DWORD lastSweep = GetTickCount();
    bool  wasDown   = false;
    for (;;) {
        Sleep(100);
        if (g_state == ST_OFF) break;

        if (g_stop) { BeginDisarm("hit cap or hotkey"); continue; }

        const bool down = HotkeyDown();
        if (down && !wasDown) {
            InterlockedExchange(&g_stop, 1);
            BeginDisarm("hotkey");
            wasDown = down;
            continue;
        }
        wasDown = down;

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
        char seen[3 * 24 + 1] = "?";
        char expect[3 * 24 + 1] = "?";
        bool matched = false;
        if (n != 0 && Readable((const void*)va, n)) {
            HexDump((const void*)va, n < 24 ? n : 24, seen, sizeof(seen));
            HexDump(want, n < 24 ? n : 24, expect, sizeof(expect));
            matched = memcmp((const void*)va, want, n) == 0;
        }

        if (!matched) {
            // Loud, and with the bytes: this is the build-drift record (R9). The site is skipped,
            // never armed on a guessed address.
            Log("[CO] %s: bytes at 0x%08X do not match the pattern -> not observed\n"
                "[CO]   expected: %s\n[CO]   actual  : %s\n",
                kSites[i].name, (unsigned)va, expect, seen);
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

    unsigned reporterTid = 0;
    g_reporter = (HANDLE)_beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, &reporterTid);
    g_reporterTid = reporterTid;
    if (!g_reporter) Log("[CO] reporter thread failed; arming without the sweep\n");

    ArmAllThreads();

    Log("[CO] armed; hit cap %u, disarm with Ctrl+Alt+%c\n",
        (unsigned)MAX_HITS, (char)HOTKEY_VK);
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
    InterlockedExchange(&g_stop, 1);
    const LONG prev = InterlockedExchange(&g_state, ST_STOPPING);
    // ST_STOPPING too: the reporter may have entered BeginDisarm() already; disarming twice is a
    // no-op per thread, skipping it would leave DRs armed with the VEH about to be removed.
    if (prev == ST_ARMED || prev == ST_STOPPING) DisarmAllThreads();
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    InterlockedExchange(&g_state, ST_OFF);

    // Bounded join before fclose: the reporter logs on its own tick, so closing the file under it
    // is a use-after-free inside a stream. At detach the reporter exits within one 100 ms tick; if
    // the wait times out (process teardown) the file and handle are deliberately left open — the
    // OS reclaims them, and a leaked handle is better than a torn FILE*.
    if (g_reporter) {
        if (WaitForSingleObject(g_reporter, 1000) == WAIT_OBJECT_0) {
            CloseHandle(g_reporter);
            g_reporter = nullptr;
        } else {
            OutputDebugStringA("[CO] reporter did not stop in 1 s; log left open\n");
        }
    }
    if (!g_reporter && g_log) { fclose(g_log); g_log = nullptr; }
}

bool CombatObserve::Armed()
{
    return g_state == ST_ARMED;
}
