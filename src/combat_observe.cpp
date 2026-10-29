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
// debug registers of THE CALLING THREAD (SetThreadContext(GetCurrentThread()) or the context record
// the CPU already handed to our VEH) and to this unit's log file. It cannot alter combat behaviour.
//
// WHICH THREADS GET DRs, AND WHY THAT IS THE ONLY SAFE SET
// -------------------------------------------------------
// Debug registers are per-thread state, so arming a thread this module did not create would mean
// suspending it and rewriting its context from outside (OpenThread + SuspendThread +
// SetThreadContext on a foreign thread). That is the crash class this project has hit before: the
// foreign thread can be holding the loader lock, a heap lock, or a half-finished kernel transition.
// This unit therefore never edits a foreign thread's context:
//   - Init() arms the thread it is called on (the thread inside DllMain), by self-edit;
//   - DLL_THREAD_ATTACH arms each thread from INSIDE that thread, also by self-edit;
//   - a thread that existed before this module was loaded is never armed. That is a known coverage
//     gap, recorded in the run report; it is NOT papered over with unsafe context edits.
// Disarm follows the same rule: a thread clears its own DRs — the detach thread in Shutdown(), the
// reporter on its way out, or any armed thread from its own single-step handler once quiesced.
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
//
// Every one of those writes is applied to the context record the CPU already handed to our VEH,
// i.e. to the context of the thread that trapped, or (when arming) to GetCurrentThread(). The
// handler never reaches for another thread's context.

#include "combat_observe.h"
#include "Hook.h"
#include "build_gate.h"
#include "pattern_scan.h"

#include <windows.h>
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

// Threads are never tracked in a table any more: no code path can name another thread, so there is
// nothing to enumerate at shutdown and nothing to race with a new thread's attach.

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

// ---- arming: the calling thread, and only the calling thread --------------------------------
// Applies one context edit to the thread that calls this function. The handle is ALWAYS
// GetCurrentThread(): there is no thread id parameter, so no call site can name another thread.
// Nothing here allocates or takes the loader lock, so it is safe from DllMain and from
// DLL_THREAD_ATTACH.
bool EditCallingThreadContext(bool arm)
{
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    // Arming writes only the DR block. Disarming also asks for CONTEXT_CONTROL so TF can be
    // cleared: a thread quiesced in the middle of a step-over still has TF in its own context, and
    // with the handler gone that pending single-step would be delivered to the engine's SEH.
    c.ContextFlags = arm ? CONTEXT_DEBUG_REGISTERS
                         : (CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL);

    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) return false;
    if (arm) {
        BuildArmedContext(&c);
    } else {
        BuildDisarmedContext(&c);
        c.EFlags &= ~EFLAGS_TF;
    }
    return SetThreadContext(th, &c) != 0;
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
    const bool live = (g_state == ST_ARMED) && (g_stop == 0);

    // (a) our own step-over: TF still set + DR6.BS. This unit is the only writer of TF.
    if (dr6 & DR6_BS) {
        if ((c->EFlags & EFLAGS_TF) == 0) return EXCEPTION_CONTINUE_SEARCH;   // not ours
        c->EFlags &= ~EFLAGS_TF;
        c->Dr6 = 0;
        if (g_state == ST_ARMED) {
            BuildArmedContext(c);   // re-arm
        } else {
            // Quiesced while this thread was mid step-over: it drops every DR of ours, for good.
            // This is the thread's OWN context record, so it is not a foreign-thread edit.
            BuildDisarmedContext(c);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // (b) a breakpoint hit: DR6 bits 0..3 name the slot.
    const DWORD hit = dr6 & 0x0Fu;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;

    DWORD ours = 0;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        if (!(hit & (1u << i))) continue;
        if (!g_siteOk[i] || (uintptr_t)c->Eip != g_va[i]) continue;   // another DR user's slot
        if (live) LogHit(i, c);
        ours |= (1u << i);
    }
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;

    if (!live) {
        // Quiesced (hit cap, Ctrl+Alt+M, or a disarm in progress): this thread clears its own DRs
        // from its own context record and the instruction at EIP runs untouched. Clearing Ln is
        // what stops it from firing again; no other thread is involved, and no thread is suspended.
        BuildDisarmedContext(c);
        c->EFlags &= ~EFLAGS_TF;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

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

    // Only a thread that runs this function can be disarmed here, and the only thread that does is
    // ours: the reporter (cap/hotkey). The edit is therefore a self-edit.
    EditCallingThreadContext(false);

    // The VEH deliberately STAYS registered for the rest of the process. Threads armed by
    // DLL_THREAD_ATTACH still hold DR0-DR3, and removing the handler would deliver their next #DB
    // to the engine's SEH as an unhandled single-step (the process dies). Keeping it lets each of
    // those threads neutralise itself on its first trap (HandleSingleStep, quiesced path), which is
    // a thread editing its own context record — the safe direction. Shutdown() removes the handler
    // when the process is on its way out, and the DRs that are still set expire with the process.
    Log("[CO] disarmed this thread; other armed threads clear their own DRs on their next trap\n");
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
    bool wasDown = false;
    for (;;) {
        Sleep(100);
        if (g_state != ST_ARMED) break;              // ST_OFF: never installed; ST_STOPPING: quiesced

        if (g_stop) { BeginDisarm("hit cap or hotkey"); break; }

        const bool down = HotkeyDown();
        if (down && !wasDown) {
            InterlockedExchange(&g_stop, 1);
            BeginDisarm("hotkey");
            break;
        }
        wasDown = down;
    }

    // This is the module's own thread, so clearing its DRs is a self-edit and cannot trap: it never
    // executes one of the four sites. Harmless when it was never armed.
    EditCallingThreadContext(false);
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

    // The reporter thread is created while the state is still ST_OFF, so its own DLL_THREAD_ATTACH
    // arrives before anything is armed and it never carries DRs of ours. OnThreadAttach also skips
    // it by id, in case that notification lands late.
    unsigned reporterTid = 0;
    g_reporter = (HANDLE)_beginthreadex(nullptr, 0, ReporterThread, nullptr, 0, &reporterTid);
    g_reporterTid = reporterTid;
    if (!g_reporter) Log("[CO] reporter thread (cap/hotkey) failed; the observer runs without it\n");

    g_veh = AddVectoredExceptionHandler(1, CombatVeh);   // first-chance, same call as phase1 DrTrap
    if (!g_veh) {
        Log("[CO] install FAILED: AddVectoredExceptionHandler\n");
        return;
    }

    // State before arming: the very first trap must find ST_ARMED, otherwise it is passed on to
    // the engine's SEH and the process dies on an unhandled single-step.
    InterlockedExchange(&g_state, ST_ARMED);

    // The FIRST armed thread, and the only one armed from here: the thread DllMain is running on.
    // That is a self-edit (GetCurrentThread()), so it is safe inside the loader lock. Every other
    // thread arms itself from its own DLL_THREAD_ATTACH.
    const DWORD installTid = GetCurrentThreadId();
    if (EditCallingThreadContext(true)) {
        Log("[CO] armed the install thread tid=%u (DllMain's thread; other threads arm themselves "
            "on DLL_THREAD_ATTACH)\n", (unsigned)installTid);
    } else {
        Log("[CO] WARNING: could not arm the install thread tid=%u (error %lu); coverage is now "
            "DLL_THREAD_ATTACH only\n",
            (unsigned)installTid, (unsigned long)GetLastError());
    }

    Log("[CO] armed; hit cap %u, disarm with Ctrl+Alt+%c\n",
        (unsigned)MAX_HITS, (char)HOTKEY_VK);
}

void CombatObserve::OnThreadAttach()
{
    // Runs ON the new thread, under the loader lock, for every thread the process creates. It must
    // not allocate, log, open a file or call into the loader: only an int test and, when live, one
    // Get/SetThreadContext pair on GetCurrentThread(). The decision itself is made in DllMain
    // (Armed()), so the disabled cost is a single int test there and nothing here.
    if (g_state != ST_ARMED) return;

    const DWORD self = GetCurrentThreadId();
    if (self == g_reporterTid) return;    // our own reporter: never armed (see Init)

    // Fail silently: a thread that cannot be armed is a missing measurement on that thread, never a
    // wrong one, and there is no safe way to log from under the loader lock.
    EditCallingThreadContext(true);
}

void CombatObserve::Shutdown()
{
    if (!g_entry) return;

    // Order matters. The VEH must keep servicing single-steps while the DRs are being cleared, so
    // the state machine goes STOPPING (handle traps, never re-arm, never log) before the disarm,
    // and only becomes OFF after the VEH is gone.
    InterlockedExchange(&g_stop, 1);
    const LONG prev = InterlockedExchange(&g_state, ST_STOPPING);
    // Only if this unit ever armed anything: with the observer disabled the state never left
    // ST_OFF, so a disabled install touches no thread context even on the way out.
    if (prev == ST_ARMED || prev == ST_STOPPING) {
        // THIS thread only — the detach thread is the one running Shutdown, so this is a self-edit.
        //
        // DELIBERATE: DR0-DR3 on every other thread are left set and expire with the process. The
        // only way to clear them from here would be OpenThread + SuspendThread + SetThreadContext
        // on a foreign thread, which is the crash class this build exists to remove (the target may
        // hold the loader lock or a heap lock, and we would resume it into a context we guessed).
        // A stale DR costs at most one extra trap on that thread before it exits, and the process is
        // already winding down.
        EditCallingThreadContext(false);
    }
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
