// AC4 Co-op — Phase 0 instrumentation DLL  (MSVC / x86)
// Goal: prove we can locate the local player each frame and read its world position,
//       using only the engine's own functions (no code patching / hooks).
//
// Call chain (resolved from the live module base at runtime, so ASLR-safe):
//   scimitar::CinematicEntityProperty::GetWorld()      RVA 0x14D4D0  -> World*
//   scimitar::World::GetPlayer(World*)                 RVA 0x30E810  -> Entity* (player Human)
//   scimitar::BaseEntity::GetGlobalPosition(this,out)  RVA 0x13460   -> fills ubiVector4 {x,y,z,w}
//
// Read-only against game state, on a low-rate poll thread. Every engine call is wrapped
// in SEH so a transient bad pointer (loading screens, entity teardown) can never crash
// the game. If this proves stable we upgrade to a per-frame hook in Phase 1.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include "windowed.h"
#include "phase1.h"
#include "net.h"
#include "action_guard.h"

// ---- Target function RVAs (file VA - 0x400000) ---------------------------------
static const uintptr_t RVA_GetWorld          = 0x14D4D0;
static const uintptr_t RVA_GetPlayer         = 0x30E810;
static const uintptr_t RVA_GetGlobalPosition = 0x13460;

// ---- Engine calling conventions (32-bit) ---------------------------------------
typedef void* (__cdecl    *GetWorld_t)();
typedef void* (__thiscall *GetPlayer_t)(void* worldThis);
typedef void* (__thiscall *GetGlobalPos_t)(void* entityThis, void* outVec4);

static GetWorld_t     pGetWorld     = nullptr;
static GetPlayer_t    pGetPlayer    = nullptr;
static GetGlobalPos_t pGetGlobalPos = nullptr;

static volatile bool  g_run = true;
static FILE*          g_log = nullptr;

struct __declspec(align(16)) Vec4 { float x, y, z, w; };

// ---- Safe memory probe ---------------------------------------------------------
static bool readable(const void* p, size_t n)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + n <= end;
}

static void logline(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    OutputDebugStringA(buf);
}

// One sample of world -> player -> position. SEH-guarded: never propagates a fault.
static bool sampleOnce(Vec4& pos, void*& outWorld, void*& outPlayer)
{
    __try {
        void* world = pGetWorld();
        if (!readable(world, 0x800)) return false;
        void* player = pGetPlayer(world);
        if (!readable(player, 0x50)) return false;
        pGetGlobalPos(player, &pos);
        outWorld = world;
        outPlayer = player;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static DWORD WINAPI PollThread(LPVOID)
{
    AllocConsole();
    FILE* dummy; freopen_s(&dummy, "CONOUT$", "w", stdout);
    char logPath[MAX_PATH];
    Phase1::ModuleRelativePath(logPath, sizeof(logPath), "phase0_log.txt");
    fopen_s(&g_log, logPath, "w");

    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    pGetWorld     = (GetWorld_t)    (base + RVA_GetWorld);
    pGetPlayer    = (GetPlayer_t)   (base + RVA_GetPlayer);
    pGetGlobalPos = (GetGlobalPos_t)(base + RVA_GetGlobalPosition);

    logline("[AC4Coop] Phase 0 online. module base = %p\n", (void*)base);
    Phase1::InstallVirtualPadHook();   // Phase A: must run before the engine enumerates pads
    logline("[AC4Coop] GetWorld=%p  GetPlayer=%p  GetGlobalPosition=%p\n",
            (void*)pGetWorld, (void*)pGetPlayer, (void*)pGetGlobalPos);

    void*    lastPlayer = nullptr;
    uint64_t ticks = 0;

    while (g_run)
    {
        Phase1::InstallVirtualPadHook();   // retry until xinput1_3.dll is loaded
        Sleep(50); // ~20 Hz
        ++ticks;

        // F6 (edge-triggered): request a spawn; the actual work runs on the main thread (frame hook).
        static bool f6prev = false;
        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6 && !f6prev) { logline("[AC4Coop] F6 -> spawn requested\n"); Phase1::RequestSpawn(); }
        f6prev = f6;

        // F7 (edge-triggered): dump the player Human* + swim chain (for Cheat Engine verification).
        static bool f7prev = false;
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !f7prev) { logline("[AC4Coop] F7 -> dump requested\n"); Phase1::RequestDump(); }
        f7prev = f7;

        // F8 (edge-triggered): crowd (non-player) body probe — GO/NO-GO for the ambient-NPC-sync plan.
        static bool f8prev = false;
        bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8 && !f8prev) { logline("[AC4Coop] F8 -> crowd probe requested\n"); Phase1::RequestCrowdProbe(); }
        f8prev = f8;

        // F9 (edge-triggered): scene/interior probe — observation only, logs the [Scene] block.
        static bool f9prev = false;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9prev) { logline("[AC4Coop] F9 -> scene probe requested\n"); Phase1::RequestSceneProbe(); }
        f9prev = f9;

        // F10 (edge-triggered): ship/boarding probe -- observation only, logs the [Ship] block.
        static bool f10prev = false;
        bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10 && !f10prev) { logline("[AC4Coop] F10 -> ship probe requested\n"); Phase1::RequestShipProbe(); }
        f10prev = f10;

        // F11 (edge-triggered): REMOVE the ghost ship (safe hide) — press before switching worlds.
        static bool f11prev = false;
        bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !f11prev) { logline("[AC4Coop] F11 -> remove ghost ship\n"); Phase1::RequestGhostDespawn(); }
        f11prev = f11;

        // F12 (edge-triggered): construct-ship experiment -- build a SHIP entity at the ctor VA and add it
        // to the world so the USER can see whether the engine streams its model (hull/masts) on its own.
        static bool f12prev = false;
        bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12 && !f12prev) { logline("[AC4Coop] F12 -> construct-ship experiment\n"); Phase1::RequestShipCtorProbe(); }
        f12prev = f12;

        // F13 (edge-triggered): ship-drive test -- write +50 X onto the LOCAL ship's transform, then let the
        // USER watch whether the ship jumps and STAYS (transform writable -> we can drive it) or snaps back
        // (physics owns the transform). Observation only; no wire/co-op state touched.
        static bool f13prev = false;
        bool f13 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f13 && !f13prev) { logline("[AC4Coop] F5 -> ship-drive test\n"); Phase1::RequestShipDriveProbe(); }
        f13prev = f13;

        // F1 (edge-triggered): guard -> CSrvNPCHealth mapping probe — test which owner object holds
        // the guard's health service.
        static bool f1prev = false;
        bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1 && !f1prev) { logline("[AC4Coop] F1 -> guard health map\n"); Phase1::RequestGuardHealthMap(); }
        f1prev = f1;

        // F2 (edge-triggered): health heap-scan probe — find the live CSrvHealth objects by vtable.
        static bool f2prev = false;
        bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (f2 && !f2prev) { logline("[AC4Coop] F2 -> health heap probe\n"); Phase1::RequestHealthProbe(); }
        f2prev = f2;

        // F3 (edge-triggered): input-state probe — dump the input manager + device input state to pin
        // the per-pad buttons + stick layout for the input relay. Read-only.
        static bool f3prev = false;
        bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !f3prev) { logline("[AC4Coop] F3 -> input-state probe\n"); Phase1::RequestInputProbe(); }
        f3prev = f3;

        // V (edge-triggered): toggle the pad-1 test stick and compare player vs puppet movement.
        static bool vprev = false;
        bool v = (GetAsyncKeyState('V') & 0x8000) != 0;
        if (v && !vprev) { logline("[AC4Coop] V -> toggle virtual pad stick\n"); Phase1::ToggleVirtualPad(); }
        vprev = v;

        // F4 (edge-triggered): entity-enumeration probe — dump the world's entity lists (ship list,
        // character list, object list, entity manager) to verify the offsets live. Observation only.
        static bool f14prev = false;
        bool f14 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f14 && !f14prev) { logline("[AC4Coop] F4 -> entity-enumeration probe\n"); Phase1::RequestEntityEnum(); }
        f14prev = f14;

        // (Phase 0 position logging removed — Phase 1 uses the main-thread frame hook.)
        (void)lastPlayer;
    }

    logline("[AC4Coop] poll thread exiting.\n");
    if (g_log) fclose(g_log);
    return 0;
}

// --- allow multiple game instances ------------------------------------------------
// WinMain creates a single named mutex and exits if GetLastError()==ERROR_ALREADY_EXISTS.
// IAT-hook CreateMutexW (its only caller is WinMain) and make each process create a UNIQUELY
// named mutex (append the PID). It then never collides -> the single-instance check always passes.
typedef HANDLE (WINAPI *CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
static CreateMutexW_t g_realCreateMutexW = nullptr;

static HANDLE WINAPI MyCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{
    wchar_t buf[300];
    if (name) {
        wsprintfW(buf, L"%s_ac4coop_%u", name, GetCurrentProcessId());
        name = buf;
    }
    return g_realCreateMutexW(sa, owner, name);
}

static void InstallMultiInstance()
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    void** iat = (void**)(base + 0x1A3A20C);   // CreateMutexW IAT slot (fileVA 0x1E3A20C)
    if (*iat == (void*)&MyCreateMutexW) return;
    g_realCreateMutexW = (CreateMutexW_t)*iat;
    DWORD old = 0;
    VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &old);
    *iat = (void*)&MyCreateMutexW;
    VirtualProtect(iat, sizeof(void*), old, &old);
    OutputDebugStringA("[AC4Coop] multi-instance: CreateMutexW IAT hooked\n");
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);

        InstallMultiInstance();   // must run before WinMain's single-instance check

        // Windowed mode — must run now, before the engine reaches scimitar::CreateWindow.
        // Config from .\AC4Coop.ini  [Display] Fullscreen=0  Borderless=0   (defaults: windowed, bordered)
        bool fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", 0, ".\\AC4Coop.ini") != 0;
        bool borderless = GetPrivateProfileIntA("Display", "Borderless", 0, ".\\AC4Coop.ini") != 0;
        Windowed::Apply(fullscreen, borderless);

        // Phase 1: hook the per-frame Human update so spawns/drives run on the game's main thread.
        Phase1::InstallFrameHook();

        // Guard the action/animation state gate (0x00679220) so a node with no bound target never
        // reaches sub_681980 / sub_679750's unchecked [node+0x114] dereference. [Coop] GuardNullActionTarget
        ActionGuard::Init();

        // Front B (peer-to-peer UDP link) is started on the first in-world frame instead of at
        // DLL attach. The game's launch chain (Steam -> Ubisoft -> DRM -> real game) starts more
        // than one process that loads this ASI, and the transient one binds the UDP port and then
        // dies - so the LIVE game's bind fails and it never gets a socket. The transient processes
        // never reach a player frame, so starting the link here means only the live game binds.
        (void)0;

        CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_run = false;
        Sleep(120);
    }
    return TRUE;
}
