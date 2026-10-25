// Null-action-target guard — see action_guard.h for the crash this prevents.
//
// Hook site (AC4BFSP.exe, absolute VA):
//
//   0x00679220  66 8B 81 FC 00 00 00   mov  ax, word ptr [ecx+0FCh]   <- 7 bytes, stolen whole
//   0x00679227  ...                     and  ax, 7                   <- resume here
//
// __thiscall sub_679220(node) (ECX = node) is a boolean gate:
//   if ((word[node+0xFC] & 7) == 0) return 1;
//   if ((word[node+0xFC] & 7) == 5 && (byte[node+0x15] & 4) == 0) return 0;
//   if (byte[node+0x15] & 2) return 1;
//   return (f32[node+0] > f32[node+4]) || (fabs(f32[node+0] - f32[node+4]) <= 0.0005f);
//
// We do not reimplement it. The detour only answers the one question the gate cannot: does this node
// have a bound target? If not, return 1 so the caller takes its known-good "at rest" path instead of
// the null dereference in sub_681980 / sub_679750. Otherwise we run the stolen instruction in a
// trampoline and continue at 0x00679227, i.e. the original function decides.

#include "action_guard.h"
#include "Hook.h"

#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <cstdint>
#include <cstdarg>

namespace {

// The module is not ASLR-relocated (see Hook.h), so these are absolute VAs; RVAs below are VA-0x400000.
const uintptr_t VA_GATE   = 0x00679220;   // sub_679220
const uintptr_t VA_RESUME = 0x00679227;   // `and ax, 7`, first instruction after the stolen 7 bytes

const uintptr_t STEAL_LEN = 7;
const size_t    NODE_BOUND_TARGET_A = 0x114;   // bound target #1 (null by construction when unbound)
const size_t    NODE_BOUND_TARGET_B = 0x118;   // bound target #2 (ditto)
const size_t    NODE_STATE_WORD     = 0xFC;    // word the gate switches on
const size_t    NODE_OUTER_DELTA    = 0xE0;    // node is an embedded sub-object at outer+0xE0

// 66 8B 81 FC 00 00 00 — mov ax, word ptr [ecx+0FCh]. Matched before patching so a different build
// is never half-hooked.
const BYTE kPrologue[STEAL_LEN] = { 0x66, 0x8B, 0x81, 0xFC, 0x00, 0x00, 0x00 };

const char* INI_PATH = ".\\AC4Coop.ini";

// Address of the trampoline; read by GuardDetour's inline asm (assigned from C++, so it is not folded).
DWORD g_trampoline = 0;
bool  g_installed  = false;

FILE*         g_log          = nullptr;
volatile LONG g_hits         = 0;   // interceptions where the guard fired
volatile LONG g_reported     = 0;   // detailed lines written so far
volatile LONG g_badReads     = 0;   // interceptions where the node itself was unreadable
DWORD         g_lastSummary  = 0;   // GetTickCount() of the last throttled total line

// Same shape as the other units' loggers: debugger + a live-readable file, never a blocking console
// write from a hook. Called from the game thread, so it stays bounded and cheap.
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

// SEH read probe, matching phase1.cpp (an actual attempted read rather than a VirtualQuery guess, so
// high (>2 GB) pointers are not wrongly rejected).
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

// Returns 1 when the node has no bound target (guard fires, caller goes "at rest"), else 0 (run the
// original gate). Never faults: a bad node pointer falls through to the original body.
int __cdecl OnIntercept(void* node)
{
    void*          a114 = nullptr;
    void*          a118 = nullptr;
    unsigned short fc   = 0;

    // Probe the node through the last field read (0x114/0x118 are the tail). The __try below is kept
    // as belt-and-braces for a region that is mapped yet still faults (teardown racing a guard page).
    if (!Readable(node, NODE_BOUND_TARGET_B + sizeof(void*))) {
        LONG bad = InterlockedIncrement(&g_badReads);
        if (bad <= 5) Log("[AG] unreadable node=%p (fallthrough)\n", node);
        return 0;
    }

    __try {
        // All three loads inside the guard: a garbage ECX faults here and is swallowed below.
        a114 = *(void**)((BYTE*)node + NODE_BOUND_TARGET_A);
        a118 = *(void**)((BYTE*)node + NODE_BOUND_TARGET_B);
        fc   = *(unsigned short*)((BYTE*)node + NODE_STATE_WORD);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Unreadable node: behave exactly as before the hook. A fault inside a hook callback would set
        // s_faulted and permanently disable the hook, so it must never escape this function.
        LONG bad = InterlockedIncrement(&g_badReads);
        if (bad <= 5) Log("[AG] unreadable node=%p (fallthrough)\n", node);
        return 0;
    }

    if (a114 != nullptr && a118 != nullptr)
        return 0;   // target bound -> the original gate's own logic decides

    LONG hits = InterlockedIncrement(&g_hits);
    if (hits <= 20 || (hits % 500) == 0) {
        InterlockedIncrement(&g_reported);
        Log("[AG] null target node=%p outer=%p a114=%p a118=%p fc=%04X\n",
            node, (BYTE*)node - NODE_OUTER_DELTA, a114, a118, (unsigned)fc);
    }

    DWORD now = GetTickCount();
    if (now - g_lastSummary >= 5000) {
        g_lastSummary = now;
        Log("[AG] hits=%u (throttled)\n", (unsigned)hits);
    }

    return 1;
}

// Entry stub: ECX = node (__thiscall). Preserve the volatile registers the caller may still hold,
// answer the gate, then either return 1 or run the stolen instruction in the trampoline.
//
//   push edx / push ecx        ; save EDX; pass node as the __cdecl argument
//   call OnIntercept
//   add  esp,4 / pop edx / pop ecx
//   test eax,eax
//   jne  guarded -> mov eax,1 ; ret
//   else -> jmp dword ptr [g_trampoline]   (mov ax,[ecx+0FCh]; jmp 0x00679227)
__declspec(naked) void GuardDetour()
{
    __asm {
        push edx
        push ecx                        // node -> first argument of OnIntercept
        call OnIntercept
        add  esp, 4
        pop  edx
        pop  ecx                        // restore node for the original/trampoline path
        test eax, eax
        jne  guard_fired
        jmp  dword ptr [g_trampoline]
    guard_fired:
        mov  eax, 1
        ret                             // return to the gate's caller, prologue not executed
    }
}

// Trampoline: re-execute the whole stolen instruction, then jump back with an absolute indirect jump
// (FF 25 rel32-free), so it is valid wherever VirtualAlloc happens to place it.
BYTE* BuildTrampoline(uintptr_t resume)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;

    memcpy(t, kPrologue, STEAL_LEN);            // 66 8B 81 FC 00 00 00  mov ax,[ecx+0FCh]
    t[7]  = 0xFF;                               // jmp dword ptr [imm32]
    t[8]  = 0x25;
    *reinterpret_cast<DWORD*>(t + 9) = (DWORD)resume;
    FlushInstructionCache(GetCurrentProcess(), t, 13);
    return t;
}

} // namespace

void ActionGuard::Init()
{
    if (g_installed) return;

    // Config gate: [Coop] GuardNullActionTarget=0 leaves the game completely unpatched (A/B without a
    // rebuild). Read the same way as the surrounding options.
    if (GetPrivateProfileIntA("Coop", "GuardNullActionTarget", 1, INI_PATH) == 0) {
        OutputDebugStringA("[AG] disabled by [Coop] GuardNullActionTarget=0\n");
        return;
    }

    // Several processes in AC4's launch chain load this ASI; only the real game has the module.
    HMODULE game = (HMODULE)GameModuleBase;
    if (!game) game = GetModuleHandleA("ac4bfsp.exe");
    if (!game) {
        OutputDebugStringA("[AG] install skipped: ac4bfsp.exe not loaded\n");
        return;
    }

    // Game-folder log, live-readable (shared write denied) like phase1's.
    if (!g_log) g_log = _fsopen(".\\AC4Coop_action_guard.log", "a", _SH_DENYWR);

    uintptr_t base   = (uintptr_t)game;
    uintptr_t gate   = base + (VA_GATE   - 0x400000);
    uintptr_t resume = base + (VA_RESUME - 0x400000);

    // Refuse to patch anything that is not the expected prologue (wrong build / already hooked).
    if (memcmp((const void*)gate, kPrologue, STEAL_LEN) != 0) {
        Log("[AG] install FAILED @ 0x00679220\n");
        return;
    }

    BYTE* tramp = BuildTrampoline(resume);
    if (!tramp) {
        Log("[AG] install FAILED @ 0x00679220\n");
        return;
    }
    g_trampoline = (DWORD)(uintptr_t)tramp;

    // E9 rel32 + 2x 0x90 (7 bytes) via the project's detour helper, which NOP-pads the remainder.
    if (!Hook::DetourFunc((BYTE*)gate, (DWORD)(uintptr_t)&GuardDetour, (DWORD)STEAL_LEN)) {
        Log("[AG] install FAILED @ 0x00679220\n");
        return;
    }

    g_lastSummary = GetTickCount();
    g_installed   = true;
    Log("[AG] guard installed @ 0x%08X (steal=%u, resume=0x%08X)\n",
        (unsigned)gate, (unsigned)STEAL_LEN, (unsigned)resume);
}
