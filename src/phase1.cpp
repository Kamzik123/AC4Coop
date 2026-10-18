// Phase 1 — spawn a second Human from the player's cached EntityBuilder + RowSelector, bind to pad 1.
#define _CRT_SECURE_NO_WARNINGS
#include "phase1.h"
#include "net.h"
#include "Hook.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <share.h>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <malloc.h>
#include "Scimitar/World.h"
#include "Scimitar/EntityBuilder.h"
#include "Scimitar/RowSelector.h"
#include "Scimitar/DuplicationManager.h"
#include "Scimitar/PlayerPadManager.h"
#include "Scimitar/LoadingManager.h"
#include "Scimitar/LoadingThread.h"
#include "Scimitar/WhiteRoomManager.h"
#include "Scimitar/PlayerProgression.h"
#include "Scimitar/HandleManager.h"
#include "Scimitar/AI/AIEntityHelper.h"
#include "Scimitar/AI/Human.h"
#include "Scimitar/AI/HumanSwimFSM.h"
#include "Scimitar/Components/CharacterController.h"

namespace {

// sub_4054D0(char active): focus activate/deactivate. Inline-hook target (see InstallNoPauseHook), so it
// keeps its raw RVA — the hook patches this code rather than calling it through a class wrapper.
const uintptr_t RVA_Deactivate      = 0x54D0;

// Human activity-context replication (parkour/climb/swim/etc). A Human owns per-activity context
// objects; exactly one is ACTIVE on the "main AI track" (track 1) and drives its animation statechart.
// Ground is the implicit default (GetCurrentContextID returns 2 when no explicit context is active).
// Human* (from GetHuman) IS the AIActor base, so AIActor::TransitToContext/ExitCurrentContext thiscall
// directly on the Human pointer.
const int       MAIN_AI_TRACK       = 1;        // locomotion/parkour contexts live on track 1
const int       CTX_GROUND          = 2;        // default context id (no explicit context active)
const int       CTX_SWIM            = 10;       // Swim context: positions the character via its physics capsule
const int       ACTLAYER_PRIMARY    = 0;        // main body-animation layer (action replication)
const int       ACTPERSIST_ALWAYS   = 4;        // ACTPersistType: keep a blend-factor slot across action/transition
                                                // resets (default is transient) — makes our driven blend stick

// Per-frame MAIN-THREAD hook site (from AC4Hook): the Human update, which runs once per frame
// for the player. base+0x12FAACD steals 6 bytes:  8B 8E 08 01 00 00  =  mov ecx,[esi+0x108]
// (World::LinkUpdateAction was wrong: it only runs from EngineContextImp::RelinkWorldTasks on
//  graph changes, not per frame.)
const uintptr_t RVA_HumanUpdate = 0x12FAACD;
const size_t    HU_STEAL_LEN    = 6;

Entity*  g_puppet = nullptr;   // last-spawned puppet entity
void*  g_blockReg = nullptr; // entity whose RegisterPlayer we suppress (the puppet)
int    g_spawnTries = 0;     // net-mode spawn-retry budget (reset on despawn so respawn can retry)
int    g_spawnCd = 0;        // net-mode spawn-retry cooldown (frames)
uint64_t g_puppetWorldId = 0;// worldId the puppet was spawned in (informational)
void*  g_puppetVtbl = nullptr;// vtable ptr captured at spawn; if it changes/unmaps the entity was freed
                             // (world change) -> drop the dangling ref. A pause leaves it intact.
int    g_driveLoco = 0;      // [Coop] DriveLocomotion: replicate HumanGroundData player->puppet
int    g_driveContext = 0;   // [Coop] DriveContext: replicate the active Human context (parkour/climb/swim)
int    g_lastRemoteCtx = -1; // last remote contextId we drove onto the puppet (edge-trigger; -1 = fresh puppet)
int      g_driveActions = 1;             // [Coop] DriveActions: replay the peer's current action (anim clip)
uint32_t g_lastActionAlias = 0xFFFFFFFF; // last action alias driven onto the puppet (edge-trigger; -1 = fresh)
// Pending blend-in override for the action we just targeted on the puppet: applied once the puppet's current
// evaluator is that action (SetTargetAction may not create it synchronously). Frames left to wait; 0 = none.
uint32_t g_blendFixAlias = 0;
float    g_blendFixLen   = 0.0f;
float    g_blendFixStartFromEnd = 0.0f;
float    g_blendFixTargetStart  = 0.0f;
uint16_t g_blendFixFlags = 0;
int      g_blendFixWait  = 0;
int      g_disablePuppetAI = 1;          // [Coop] DisablePuppetAI: disable the puppet's behavior component so
                                         // its own statechart stops issuing actions/physics that fight our sync
bool     g_casActivated = false;         // whether the current puppet's CharacterActionSet has been activated (once)
// [Coop] HybridLoco: per-context action ownership. The crash is two threads playing an action at once, so we
// guarantee EXACTLY ONE action-writer at all times: on the GROUND context the puppet's OWN FSM owns actions
// (behavior ENABLED, we only feed HumanGroundData, we never SetTargetAction) — this reproduces the MVP's
// correct locomotion gait; on every OTHER context WE own actions (behavior DISABLED + action replication) for
// parkour/climb/etc. HybridLoco=0 falls back to the old global-flag behavior for A/B.
int      g_hybridLoco = 1;
int      g_seekOnSwitch = 1;   // [Coop] SeekOnSwitch: seek a newly targeted action to the peer's phase
float    g_minBlendIn = 0.1f;   // [Coop] MinBlendInMs: blend used when the peer's switch had none (0 = off)
int      g_bhvState   = -1;              // current puppet behavior-enable state: -1 unknown, 0 disabled, 1 enabled
int      g_switchCd   = 0;               // frames to suppress OUR action writes after a behavior toggle, so the
                                         // game's worker-thread action update can quiesce and never overlap ours
// [Coop] DriveContextMask: bitmask of which context ids we ALLOW the puppet to enter (bit N = context N).
// The target-less parkour contexts (Ladder/Ledge/Walling/NarrowObject/Climbing/Hanging/Boat) crash the
// engine's later Human::Update because it derefs climb-target data (wall normal / ledge point / edge query)
// a real climb computed but our externally-teleported puppet never did. InAir(6)/Swim(10) are self-contained
// (gravity/water) and safe. Default = (1<<6)|(1<<10) = 0x440. Ground(2) is always allowed (handled via
// ExitCurrentContext, no target needed). A disallowed id pops the puppet back to ground instead of crashing.
unsigned g_ctxMask = (1u << 6) | (1u << 10);
HookMath::Vector4  g_offset = { 2.0f, 0.0f, 0.0f, 0.0f }; // puppet stands +2m on X from the player

// [Coop] ShipPassenger: deck offset, in the LOCAL ship's local space (metres), where the peer's puppet is
// placed when BOTH peers are aboard their own ships. Axes are a first guess for this build (X right,
// Y up, Z fore/aft) — tune this ONE constant; the rotation is applied from the ship's global matrix below.
static const float kShipPassengerDeckOffset[3] = { 0.0f, 2.0f, -3.0f };

// [Coop] DisableSpawns: suppress ambient entity spawning so both clients present the same empty world
// (ambient NPCs/guards/ships are seeded independently per client and won't sync). Two layered hooks,
// both gated on this flag:
//   (1) SpawningRequest::Spawn (RVA 0x39B8D0) -> return 0 (declined), but ONLY once the player is live
//       in the world (playerInWorld). This is the universal spawn entry every strategy funnels through
//       (crowd, blob NPCs, guards, sphere, naval). Blocking it unconditionally hung boot because the
//       player + essential entities spawn through here too, so we allow spawns while the player is
//       absent (boot / world load / fast-travel / death) and only decline during live gameplay.
//   (2) CrowdHerder::UpdateSpawningNeeds (RVA 0x1578B50) -> no-op. Stops the herd crowd at the source
//       so it never even builds spawn requests (crowd is never essential, so this stays unconditional).
// The retail PC build ignores the -crowdmaxnpc dev knob, so memory hooks are the only way.
int    g_disableSpawns = 0;

// [Net] interpolation: buffer remote snapshots and render the puppet a fixed delay behind local
// time, lerping between the two snapshots bracketing render time. Hides jitter and packet loss.
int    g_interpEnabled = 1;  // [Net] Interpolation
int    g_interpDelayMs = 100;// [Net] InterpDelayMs (render this far behind now; >= ~2 packet intervals)

// HumanGroundData drive fields, offsets from the bundle pointer *(GetHuman(entity)+0x160).
// All PC-verified by the mapped Human.h.
//const uintptr_t OFF_BUNDLE              = 0x160; // *(Human+0x160) = HumanDataBundle*
//const uintptr_t OFF_G_HEADING           = 0x30;  // ubiVector4 m_DestHeading
//const uintptr_t OFF_G_CURSPD            = 0xA4;  // float m_CurSpeedRatio
//const uintptr_t OFF_G_DESTSPD           = 0xA8;  // float m_DestSpeedRatio
//const uintptr_t OFF_G_ISSET             = 0xFC;  // bool  m_IsSet
//const uintptr_t OFF_G_SPRINT            = 0xFD;  // bool  m_Sprint
//const uintptr_t OFF_G_BREAKHARDONEDGE   = 0xFE;  // bool  m_BreakHardOnEdge
//const uintptr_t OFF_G_BREAKHARD         = 0x101;  // bool  m_BreakHard
//const uintptr_t OFF_G_CURMVT            = 0x134; // u8    m_CurMvtType
//const uintptr_t OFF_G_DESTMVT           = 0x135; // u8    m_DestMvtType

uintptr_t base() { return (uintptr_t)GetModuleHandleW(nullptr); }

void L(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s", buf);
    OutputDebugStringA(buf);
    // Also append to a per-process file, flushed each line, so the trail survives a crash.
    static FILE* f = nullptr;
    if (!f) {
        char name[64], path[MAX_PATH];
        wsprintfA(name, "phase1_log_%u.txt", GetCurrentProcessId());
        Phase1::ModuleRelativePath(path, sizeof(path), name);
        // _SH_DENYWR: deny other writers but allow readers, so the log is readable live (no quit needed).
        f = _fsopen(path, "w", _SH_DENYWR);
    }
    if (f) { fputs(buf, f); fflush(f); }
}

} // namespace

void Phase1::ModuleRelativePath(char* out, size_t cap, const char* fileName) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&Phase1::ModuleRelativePath, &self);
    char dir[MAX_PATH] = {};
    DWORD n = self ? GetModuleFileNameA(self, dir, MAX_PATH) : 0;
    char* slash = (n > 0 && n < MAX_PATH) ? strrchr(dir, '\\') : nullptr;
    if (slash) slash[1] = 0;
    else       dir[0] = 0;   // fall back to the current directory
    _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, fileName);
}

namespace {

// SEH read-probe: actually attempt the read rather than trusting VirtualQuery.
// (VirtualQuery-based checks were wrongly rejecting valid large-address-aware pointers >2GB.)
bool readable(const void* p, size_t n) {
    if (!p) return false;
    __try {
        volatile char first = ((const volatile char*)p)[0];
        volatile char last  = ((const volatile char*)p)[n - 1];
        (void)first; (void)last;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Compact blend-array log: only the non-zero slots as "idx:val" (46 slots, most are 0 at any time).
static void FormatBlendSlots(const float* bf, int n, char* out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 16 < cap; ++i) {
        if (bf[i] > -0.005f && bf[i] < 0.005f) continue;
        len += _snprintf_s(out + len, cap - len, _TRUNCATE, "%s%d:%.2f", len ? " " : "", i, bf[i]);
    }
}

// Vec4 <-> plain-float[4] copies (the wire uses float[4] to avoid 16-byte alignment in the packed packet).
inline void vecToArr(float* d, const HookMath::Vector4& v) { d[0]=v.x; d[1]=v.y; d[2]=v.z; d[3]=v.w; }
inline void arrToVec(HookMath::Vector4& v, const float* s) { v.x=s[0]; v.y=s[1]; v.z=s[2]; v.w=s[3]; }

// Resolve a Scimitar handle container -> object pointer:  obj = HC[0] & (HC[2] >> 31)
void* handleDeref(void* hc) {
    if (!readable(hc, 12)) return nullptr;
    int32_t obj  = ((int32_t*)hc)[0];
    int32_t vbit = ((int32_t*)hc)[2] >> 31;   // all-ones if valid, else 0
    return (void*)(obj & vbit);
}

// Resolve a Scimitar Handle -> object pointer. Handle is a 4-byte wrapper holding a HandleContainer*;
// the container itself is what handleDeref() consumes ([0]=object, [2]>>31 = valid bit). The engine's
// "no entity" sentinel 0x04DD5F8C (in the container pointer or in the resolved object) means "none".
void* handleResolve(const Handle& h) {
    HandleContainer* hc = h.m_HandleContainer;
    if ((uintptr_t)hc == 0x04DD5F8C) return nullptr;
    if (!readable(hc, 12)) return nullptr;
    int32_t obj  = ((int32_t*)hc)[0];
    int32_t vbit = ((int32_t*)hc)[2] >> 31;   // all-ones if valid, else 0
    void*   o    = (void*)(obj & vbit);
    return ((uintptr_t)o == 0x04DD5F8C) ? nullptr : o;
}

// True when the given world is the white-room (loadout/test world), which we must not spawn into.
// Callers run inside an SEH __try, so the raw derefs here are covered; the readable() probes keep the
// common "manager not up yet" case from tripping the handler.
bool inWhiteRoom(void* world) {
    WhiteRoomManager* wrm = WhiteRoomManager::GetInstance();
    if (!readable(wrm, 0x20)) return false;
    void* hc = wrm->GetWorldHandle();        // handle container to the white-room world
    void* wrWorld = handleDeref(hc);
    return wrWorld != nullptr && wrWorld == world;
}

// Reload an EntityBuilder by resource ID (mirrors LoadingThread::LoadFile) and wait for the object.
void* reloadBuilder(uint64_t id) {
    HandleManager* hm = HandleManager::GetInstance();
    if (!hm) { L("[Phase1] HandleManager unavailable\n"); return nullptr; }

    HandleContainer* handle = hm->CreateReference(id, nullptr);
    if (!readable(handle, 4)) { L("[Phase1] CreateReference failed\n"); return nullptr; }
    int stream = LoadingThread::GetFastLoadDataStream();
    char ok = LoadingThread::AddLoadTask_Callback(0, stream, 0xFFFFFFFF, id, (int)&handle, 0, 0, 0, 0, 0);
    LoadingThread::GetWaitTasks();
    L("[Phase1] reload AddLoadTask ok=%d handle=%p stream=%d\n", (int)ok, handle, stream);

    // object populates at handle->m_Object once the load completes
    for (int i = 0; i < 200; ++i) {
        if (readable(handle, 4)) {
            void* obj = handle->m_Object;
            if (readable(obj, 8)) return obj;
        }
        Sleep(10);
    }
    return nullptr;
}

// Walk Human -> swim context -> FSM -> HumanSwimScope (see HumanSwimFSM.h). Each hop is readable()-guarded
// so a chain that isn't up yet (not actually swimming) just yields null instead of faulting. Optionally
// reports the intermediate pointers for offset-verification logging.
HumanSwimScope* swimScope(Human* h, void** outCtx = nullptr, void** outFsm = nullptr) {
    void** pSwim = (void**)&h->m_HumanContextBundle.m_Swim;  // address only (no deref of the value yet)
    if (!readable(pSwim, 4)) return nullptr;                 // guards a wrong bundle offset in Human.h
    HumanSwim* swimCtx = (HumanSwim*)*pSwim;
    if (outCtx) *outCtx = swimCtx;
    if (!readable(swimCtx, 0x34)) return nullptr;
    HumanSwimFSM* fsm = swimCtx->m_FSM;
    if (outFsm) *outFsm = fsm;
    if (!readable(fsm, 0x10)) return nullptr;
    HumanSwimScope* sc = fsm->m_HumanSwimScope;
    if (!readable(sc, 0x1114)) return nullptr;   // need through m_DestSpeedRatio @ 0x1110
    return sc;
}

} // namespace

// ---- Phase A: virtual pad (XInputGetState IAT hook) ---------------------------------------------
// The engine polls one XInputGetState per 560-byte device record, so a second player needs a second
// record. The cheapest way to get one is to make the engine *discover* a pad at index 1 during its own
// enumeration: report it connected from the very start. This hook is installed at ASI load time, before
// the game initialises devices.
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD dwUserIndex, void* pState);
static XInputGetStateFn s_realXInputGetState = nullptr;
static volatile long s_vpadEnabled = 0;
static volatile long s_vpadInstalled = 0;
static int s_vpadServeLog = 0;
static int s_vpadAskLog = 0;
static int s_vpadTest = 0;          // [Coop] VPadTest=1 -> hold the stick forward on pad 1

static DWORD WINAPI VPadGetState(DWORD idx, void* st) {
    if (idx == 1 && s_vpadEnabled) {
        if (st) {
            memset(st, 0, 20);                                   // dwPacketNumber + XINPUT_GAMEPAD
            unsigned char* b = (unsigned char*)st;
            *(DWORD*)(b) = GetTickCount();                        // packet number
            if (s_vpadTest) {
                *(short*)(b + 8)  = 30000;                        // sThumbLY = forward
                *(short*)(b + 12) = 0;                            // sThumbRY
            }
        }
        if (s_vpadServeLog < 6) { ++s_vpadServeLog; L("[VPad] served synthetic pad 1 (test=%d)\n", s_vpadTest); }
        return 0;                                  // ERROR_SUCCESS
    }
    if (s_vpadAskLog < 12) { ++s_vpadAskLog; L("[VPad] engine polled pad %u\n", (unsigned)idx); }
    return s_realXInputGetState ? s_realXInputGetState(idx, st) : 1167;   // ERROR_DEVICE_NOT_CONNECTED
}

static void PatchXInputIAT(void* target, void* replacement) {
    HMODULE mod = GetModuleHandleA(nullptr);
    if (!mod) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)mod + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)((char*)mod + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)((char*)mod + imp->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            if ((void*)thunk->u1.Function != target) continue;
            DWORD old = 0;
            if (VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
                thunk->u1.Function = (ULONG_PTR)replacement;
                VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), old, &old);
                L("[VPad] IAT patched (XInputGetState)\n");
            }
        }
    }
}

void Phase1::InstallVirtualPadHook() {
    if (s_vpadInstalled) return;
    // Test scaffolding only: advertising a phantom pad at index 1 from process start is not something a
    // normal build should do. Opt in with [Coop] VPadEnable=1.
    {
        char ini[MAX_PATH];
        Phase1::ModuleRelativePath(ini, sizeof(ini), "AC4Coop.ini");
        if (GetPrivateProfileIntA("Coop", "VPadEnable", 0, ini) == 0) return;
    }
    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) xi = GetModuleHandleA("xinput1_4.dll");
    if (!xi) xi = GetModuleHandleA("xinput9_1_0.dll");
    if (!xi) return;                               // not loaded yet; caller retries
    void* p = (void*)GetProcAddress(xi, "XInputGetState");
    if (!p) return;
    s_realXInputGetState = (XInputGetStateFn)p;
    {
        // Read the ini by absolute path: at ASI-load time the CWD is not the game folder yet.
        char ini[MAX_PATH];
        Phase1::ModuleRelativePath(ini, sizeof(ini), "AC4Coop.ini");
        s_vpadTest = GetPrivateProfileIntA("Coop", "VPadTest", 0, ini);
        L("[VPad] ini=%s VPadTest=%d\n", ini, s_vpadTest);
    }
    PatchXInputIAT(p, (void*)VPadGetState);
    InterlockedExchange(&s_vpadEnabled, 1);
    InterlockedExchange(&s_vpadInstalled, 1);
    L("[VPad] installed; reporting a connected pad at index 1\n");
}

// ---- PLAN-v3 gate: does single-player accept a second local player on pad 1? -------------------
static int  g_padGateFrames = 0;
static void* g_padGatePuppet = nullptr;
static void ProbePadGate();

// Runtime toggle for the pad-1 test stick, with a position comparison so the result doesn't depend on
// someone watching: sample player + puppet, wait ~2 s, sample again, log both deltas.
static int s_posProbeFrames = 0;
static int s_driveTest = 0;          // Route B test: drive the puppet from OUR vector, no virtual pad
static int s_contSamples = 0;
static void* s_contEnt = nullptr;
static float s_p0Start[3], s_ppStart[3];

void Phase1::ToggleVirtualPad() {
    s_driveTest ^= 1;
    L("[Drive] puppet drive test -> %d  (virtual pad stays neutral: s_vpadTest=%d)\n", s_driveTest, s_vpadTest);
    L("[Drive] g_puppet=%p g_padGatePuppet=%p\n", g_puppet, g_padGatePuppet);
    s_contEnt = g_padGatePuppet;
    s_contSamples = 10;                 // 10 samples x 60 frames = ~10 s
    s_posProbeFrames = 120;
    World* world = World::GetCurrentWorld();
    Entity* player = world ? world->GetPlayer() : nullptr;
    if (player && readable(player, 4)) {
        s_p0Start[0] = player->m_GlobalMatrix.M41;
        s_p0Start[1] = player->m_GlobalMatrix.M42;
        s_p0Start[2] = player->m_GlobalMatrix.M43;
        L("[VPad] player start (%.2f,%.2f,%.2f)\n", s_p0Start[0], s_p0Start[1], s_p0Start[2]);
    }
    if (g_padGatePuppet && readable(g_padGatePuppet, 4)) {
        Entity* pe = (Entity*)g_padGatePuppet;
        s_ppStart[0] = pe->m_GlobalMatrix.M41;
        s_ppStart[1] = pe->m_GlobalMatrix.M42;
        s_ppStart[2] = pe->m_GlobalMatrix.M43;
        L("[VPad] puppet start (%.2f,%.2f,%.2f)\n", s_ppStart[0], s_ppStart[1], s_ppStart[2]);
    } else {
        L("[VPad] no puppet to compare\n");
    }
    s_posProbeFrames = 120;
}

// Route B: force the puppet to move from the pad-1 test state using its OWN ground data — the same seam
// the network puppet uses (m_IsSet + m_DestSpeedRatio + m_DestHeading). If this walks, the real co-op
// input path is "peer pad -> our drive -> puppet", independent of the engine granting a second player.
static void DrivePuppetFromPad() {
    if (!s_driveTest || !g_padGatePuppet) return;
    Entity* pe = (Entity*)g_padGatePuppet;
    if (!readable(pe, 4)) return;
    Human* h = AIEntityHelper::GetHuman(pe);
    if (!h || !readable(h, 4)) return;
    HumanDataBundle* b = h->m_Data.m_HumanDataBundle;
    if (!b || !readable(b, 0x400)) return;
    HookMath::Vector4 fwd;
    fwd.x = pe->m_GlobalMatrix.M11;
    fwd.y = pe->m_GlobalMatrix.M12;
    fwd.z = pe->m_GlobalMatrix.M13;
    fwd.w = 0.0f;
    b->m_GroundData.m_DestHeading = fwd;
    b->m_GroundData.m_IsSet = true;
    b->m_GroundData.m_DestSpeedRatio = 1.0f;
}

static void FinishPadProbe() {    World* world = World::GetCurrentWorld();
    Entity* player = world ? world->GetPlayer() : nullptr;
    if (player && readable(player, 4)) {
        float dx = player->m_GlobalMatrix.M41 - s_p0Start[0];
        float dz = player->m_GlobalMatrix.M43 - s_p0Start[2];
        L("[VPad] PLAYER moved2=%.2f (dx=%.2f dz=%.2f)\n", dx*dx + dz*dz, dx, dz);
    }
    if (g_padGatePuppet && readable(g_padGatePuppet, 4)) {
        Entity* pe = (Entity*)g_padGatePuppet;
        float dx = pe->m_GlobalMatrix.M41 - s_ppStart[0];
        float dz = pe->m_GlobalMatrix.M43 - s_ppStart[2];
        L("[VPad] PUPPET moved2=%.2f (dx=%.2f dz=%.2f)\n", dx*dx + dz*dz, dx, dz);
    }
    L("[VPad] verdict: whichever moved owns the pad-1 input\n");
}

void* Phase1::TrySpawnPuppet() {
    Entity* newEntity = nullptr;
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return nullptr;

        Entity* player = world->GetPlayer();
        if (!player)
            return nullptr;

        // 2) player's cached builder handle + selector
        RowSelector*     selector  = PlayerProgression::GetCachedSelector();
        HandleContainer* builderHC = PlayerProgression::GetCachedBuilderHandle();
        void* builder = handleDeref(builderHC);

        // container diagnostics + extract the builder's resource ID (HandleContainer::m_ObjectID)
        uint64_t builderID = 0;
        if (readable(builderHC, 20)) {
            uint32_t* hc = (uint32_t*)builderHC;
            builderID = builderHC->m_ObjectID;
            L("[Phase1] HC: [0]=%08X [1]=%08X [2]=%08X  builderID=%08X`%08X\n",
              hc[0], hc[1], hc[2], (uint32_t)(builderID >> 32), (uint32_t)builderID);
        } else {
            L("[Phase1] builderHC=%p not readable\n", builderHC);
        }
        L("[Phase1] selector=%p builderHC=%p builder(masked)=%p\n", selector, builderHC, builder);

        // builder object was released after the player spawned -> reload it by ID
        if (!readable(builder, 8) && builderID) {
            bool doReload = GetPrivateProfileIntA("Coop", "ReloadBuilder", 1, ".\\AC4Coop.ini") != 0;
            if (doReload) {
                L("[Phase1] builder released; reloading by ID...\n");
                builder = reloadBuilder(builderID);
                L("[Phase1] reloaded builder=%p\n", builder);
            }
        }
        if (!readable(builder, 8) || !readable(selector, 8)) {
            L("[Phase1] builder/selector not ready\n"); return nullptr;
        }
        L("[Phase1] builder.vtbl=%p selector.vtbl=%p\n", *(void**)builder, *(void**)selector);

        // 3) duplicate the selector (BuildEntityWithSelector consumes/frees the one it gets).
        //    RowSelector::Duplicate needs an active DuplicationManager scope (Begin/End), exactly
        //    as the game wraps it in LoadEntityBuilderAndSelectorsForPlayer.
        DuplicationManager::Begin(1, 0);
        RowSelector* dupSel = nullptr;
        // If Duplicate faults (e.g. a stale selector right after world travel), End MUST still run or the
        // DuplicationManager stays unbalanced and every later respawn attempt corrupts. __finally ensures it.
        __try {
            dupSel = selector->Duplicate(0, 0);
        } __finally {
            DuplicationManager::End();
        }
        L("[Phase1] dupSelector=%p\n", dupSel);
        if (!readable(dupSel, 8)) { L("[Phase1] selector duplicate failed\n"); return nullptr; }

        // 4) BUILD the entity
        int rc = ((EntityBuilder*)builder)->BuildEntityWithSelector(&newEntity, dupSel);
        L("[Phase1] BuildEntityWithSelector rc=%d entity=%p\n", rc, newEntity);
        if (!readable(newEntity, 0x50)) { L("[Phase1] build produced no entity\n"); return nullptr; }
        L("[Phase1] entity.vtbl=%p\n", *(void**)newEntity);
        // hijack probe: did building change who the game considers "the player"?
        L("[Phase1] GetPlayer AFTER build = %p (orig=%p new=%p)\n",
          world->GetPlayer(), player, newEntity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Phase1] EXCEPTION during resolve/build (code=0x%08X)\n", GetExceptionCode());
        return nullptr;
    }

    // Steps below mutate world state — gate behind ini so we can test build-only first.
    bool doIntegrate = Net::Enabled() ||
                       GetPrivateProfileIntA("Coop", "IntegratePuppet", 0, ".\\AC4Coop.ini") != 0;
    if (!doIntegrate) {
        // Not in world -> do NOT register it for driving (SetGlobalMatrixInternal on a
        // non-integrated entity is unsafe). Just report the successful build.
        L("[Phase1] built entity only (Coop.IntegratePuppet=0). Not adding to world.\n");
        return newEntity;
    }

    __try {
        World* world = World::GetCurrentWorld();
        Entity* player = world->GetPlayer();

        // 5) mark the puppet so BhvAssassin::AddToWorldInternal skips player/gamer/pad enrollment,
        //    then add to world (deferred engine command; processes next frame)
        g_blockReg = newEntity;
        world->AddEntity(newEntity);
        L("[Phase1] AddEntity issued; GetPlayer now = %p (orig=%p new=%p)\n",
          world->GetPlayer(), player, newEntity);

        // 6) position at player + offset (identity orientation, no physics for initial placement)
        HookMath::Vector4 ppos = player->GetPosition();
        newEntity->SetPosition(ppos + g_offset);

        // 7) OPTIONAL: bind to pad 1 (local only; a networked puppet is a remote proxy, never a pad player)
        if (!Net::Enabled() && GetPrivateProfileIntA("Coop", "RegisterPad1", 0, ".\\AC4Coop.ini") != 0) {
            PlayerPadManager* pm = PlayerPadManager::FromWorld(world);
            L("[Phase1] padManager=%p\n", pm);
            if (readable(pm, 0x20)) {
                pm->RegisterPlayer(newEntity, 1);
                L("[Phase1] RegisterPlayer(pad=1) done\n");
                // PLAN-v3 gate: check in ~45 frames whether the registration persisted and the world is
                // still healthy (G1/G2). G3 (does the engine read pad-1 input) needs the virtual pad.
                g_padGatePuppet = newEntity;
                g_padGateFrames = 45;
                L("[Phase1] pad gate armed (G1/G2 in ~45 frames)\n");
            }
        }
        g_puppet = newEntity;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Phase1] EXCEPTION during integrate (code=0x%08X)\n", GetExceptionCode());
    }
    return newEntity;
}

// ---- crowd (non-player) body probe [F8] ----------------------------------------------
// GO/NO-GO probe for the ambient-NPC-sync plan (bf-coop/AMBIENT-NPC-SYNC-PLAN.md §4): can the same
// build machinery produce a NON-PLAYER (crowd) body, and does that body render?
//
// Static REC (byte-verified against AC4BFSP.exe, tools/_dump_eb_vtable.py + _probe_crowd_builder.py):
//   * PlayerProgression caches ONLY the player's RowSelector (0x04DEEF90) + EntityBuilder handle
//     container (0x04DEEF94); written only by PlayerProgressionManager::LoadEntityBuilderAndSelectors-
//     ForPlayer (0x12F9xxx). There is NO crowd analogue of those globals.
//   * The crowd/free-roam spawn RVA 0x1FD730 is EntityBuilder vtable SLOT 39 (+0x9C). Builder base
//     vtable = 0x1E57790 (slot 17 +0x44 = BuildEntityWithSelector 0x5FAC00; slot 39 +0x9C = 0x5FD730);
//     derived variant = 0x1E577E0 (also slot 39). Slot 39 takes a crowd template HASH, reads the
//     manager global VA 0x2AC1E68, and in its slow path builds the body as: map entry -> template
//     object -> [[vtbl]+0xC](0,0) clone -> FUN_00526590 (activate flags/spatial) -> FUN_00A2E820
//     (job flush). It uses NO RowSelector — the crowd path selects by template hash, not by a selector.
//   * Candidate builder sources: world+0xA48 (fallback +0xA44) per SPAWN-RETHINK.md §11, and
//     world+0x924 (the world's spawner/template manager read by the world-side caller).
//
// So the probe acquires a builder object, validates its vtable is the builder family, then calls SLOT 39
// with a crowd template hash (0x49BB47AC first, then the live per-area hashes 0x47CD5ECC / 0x479DB35C).
// Fallback: if slot 39 yields nothing, run the documented recipe (Duplicate -> BuildEntityWithSelector,
// SLOT 17 / 0x5FAC00) on the same builder with the player's cached RowSelector.
//
// One-shot observation: adds the body to the world a few metres from the player and logs ONE verdict
// line. Adopts nothing, drives nothing, never touches the puppet path (g_puppet / TrySpawnPuppet).
// Fully SEH-guarded; every dereference is readable()-checked.
void* Phase1::TryCrowdProbe() {
    // area crowd template hashes — engine-captured live (MODLOG 2026-10-07: 0x49BB47AC x96; RE-NOTES §tooling)
    const uint32_t CROWD_HASHES[3] = { 0x49BB47ACu, 0x47CD5ECCu, 0x479DB35Cu };
    const int      N_HASHES = 3;
    const int      SLOT_CROWD_SPAWN = 39;   // +0x9C  FUN_005FD730  (crowd / free-roam spawn by template hash)

    void*     builder   = nullptr;
    void*     builderVt = nullptr;
    void*     ent       = nullptr;
    const char* route   = "none";
    uint32_t  usedHash  = 0;
    bool      added     = false;

    __try {
        World* world = World::GetCurrentWorld();
        if (!readable(world, 0x1000)) { L("[Probe] F8: no world (not in-world)\n"); return nullptr; }
        Entity* player = world->GetPlayer();
        if (!readable(player, 0x50)) { L("[Probe] F8: no player (not in-world)\n"); return nullptr; }

        L("[Probe] F8: world=%p player=%p\n", world, player);

        // ---- 1) acquire a builder object ----
        // The world keeps its current EntityBuilder behind a SmartPtr/handle: the object pointer lives
        // at +0 and a status word at +8 whose sign bit says "valid" (Ref::get == handleDeref, i.e.
        // obj & (status >> 31)). world+0xA48 (primary) / +0xA44 (fallback) hold that SmartPtr -- NOT a
        // direct object pointer, so dereferencing the field value as an object is wrong (returns the
        // status word as a "vtable"). world+0x924 is the Ai::SpawningManager, not a builder.
        auto inFamily = [](void* vt) {
            return vt == (void*)0x1E57790 || vt == (void*)0x1E577E0;
        };

        // a. proven path: the player's cached builder handle container -> builder object
        HandleContainer* cachedHC  = PlayerProgression::GetCachedBuilderHandle();
        void*            cachedObj = handleDeref(cachedHC);
        void*            cachedVt  = readable(cachedObj, 8) ? *(void**)cachedObj : nullptr;
        L("[Probe]   cachedBuilder = %p vt = %p builderFamily = %d\n",
          cachedObj, cachedVt, (int)inFamily(cachedVt));

        // b. candidate offsets: log BOTH the raw field AND the handle/SmartPtr-resolved object
        const uintptr_t CAND_OFFS[3] = { 0xA48, 0xA44, 0x924 };
        for (int i = 0; i < 3; ++i) {
            void* raw   = readable((char*)world + CAND_OFFS[i], 4) ? *(void**)((char*)world + CAND_OFFS[i]) : nullptr;
            void* rawVt = readable(raw, 8) ? *(void**)raw : nullptr;
            void* res   = handleDeref(raw);   // Ref::get: obj @ +0, valid while sign of status @ +8 is set
            void* resVt = readable(res, 8) ? *(void**)res : nullptr;
            L("[Probe]   off 0x%X raw=%p rawVt=%p resolved=%p resVt=%p\n",
              (unsigned)CAND_OFFS[i], raw, rawVt, res, resVt);
        }

        // c. sweep the world object for the builder vtable family (exact match on the two known builder
        //    vtables). First FAMILY hit wins; hard cap on lines logged so the trail cannot explode.
        int sweeps = 0;
        for (uintptr_t off = 0x000; off <= 0x2000; off += 4) {
            if (!readable((char*)world + off, 4)) continue;
            void* p  = *(void**)((char*)world + off);
            void* vt = readable(p, 8) ? *(void**)p : nullptr;
            if (!inFamily(vt)) continue;
            if (sweeps < 32) { L("[Probe] SWEEP +0x%X = %p vt = %p FAMILY\n", (unsigned)off, p, vt); ++sweeps; }
            if (!builder) { builder = p; builderVt = vt; }
        }

        // d. choose: first FAMILY sweep hit, else the cached builder if its vtable is in the family
        if (!builder && inFamily(cachedVt)) { builder = cachedObj; builderVt = cachedVt; }
        if (!builder) {
            L("[Probe] NO-GO: no crowd builder (cachedBuilder=%p vt=%p; no FAMILY vt 0x1E57790/0x1E577E0 at +0xA48/+0xA44/+0x924 or in world+0x0..0x2000)\n",
              cachedObj, cachedVt);
            return nullptr;
        }
        L("[Probe] builder=%p vt=%p -> slot39 (crowd spawn), %d hashes\n", builder, builderVt, N_HASHES);

        // ---- 2) primary route: vtable slot 39 = crowd/free-roam spawn by template hash ----
        // RE-NOTES (FUN_005fd730(group,hash)) + MODLOG 2026-10-08 (30.2): FUN_005FD730 is __thiscall and
        // ECX = the spawn MANAGER (world+0x924), NOT the builder. The builder (ECX=builder) call AVs
        // (code=0xC0000005) on all three hashes. So: resolve the manager + its vtable, then sweep the
        // plausible conventions in order, each in its own SEH guard, stopping at the first readable
        // entity. The old this=builder call is kept LAST so the trail shows whether it still AVs.
        typedef void* (__thiscall *Slot39_t)(void*, unsigned int);                // thiscall, 1 stack arg
        typedef void* (__thiscall *Slot39b_t)(void*, unsigned int, unsigned int); // thiscall, 2 stack args

        void* spawnMgr = readable((char*)world + 0x924, 4) ? *(void**)((char*)world + 0x924) : nullptr;
        void* spawnVt  = readable(spawnMgr, 8) ? *(void**)spawnMgr : nullptr;
        L("[Probe]   spawnMgr=%p vt=%p\n", spawnMgr, spawnVt);

        void* spawnSlot39 = readable(spawnVt, 0xA0) ? ((void**)spawnVt)[SLOT_CROWD_SPAWN] : nullptr;
        void* buildSlot39 = readable(builderVt, 0xA0) ? ((void**)builderVt)[SLOT_CROWD_SPAWN] : nullptr;

        int tries = 0;   // running call counter shared by both the live-key and legacy paths

        // Live-key path (default ON): enumerate the spawn manager's LIVE template table and pass real
        // pointers. Legacy hardcoded-hash sweep (CrowdProbeLiveKeys=0) is the instant revert.
        int useLiveKeys = GetPrivateProfileIntA("Coop", "CrowdProbeLiveKeys", 1, ".\\AC4Coop.ini");
        if (useLiveKeys != 0) {
            // ---- 2a) live template-key path ----
            // The spawn manager holds a live template table. The observed layout is an MSVC std::vector<T>
            // (begin @ +0x94, end @ +0x98, capacity @ +0x9C, 8-byte elements), while the legacy u16 count
            // probe read +0x9A which lands *inside* the end pointer. Derive the count from the vector's
            // begin/end delta and keep the old u16 probe as a secondary candidate.
            // Entries are 8 bytes {e0, e4}; each candidate's key identity is the u32 at +0x1C.
            unsigned hdr[9];
            const unsigned hdrOff[9] = { 0x88, 0x8C, 0x90, 0x94, 0x98, 0x9C, 0xA0, 0xA4, 0xA8 };
            for (int h = 0; h < 9; ++h) {
                hdr[h] = readable((char*)spawnMgr + hdrOff[h], 4)
                         ? *(unsigned*)((char*)spawnMgr + hdrOff[h]) : 0u;
            }
            L("[Probe]   mgrHdr +0x88..+0xA8 = %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
              hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7], hdr[8]);

            void*    tbl  = readable((char*)spawnMgr + 0x94, 4) ? *(void**)((char*)spawnMgr + 0x94) : nullptr;
            void*    last = readable((char*)spawnMgr + 0x98, 4) ? *(void**)((char*)spawnMgr + 0x98) : nullptr;
            unsigned cntVec = (last && tbl && (char*)last >= (char*)tbl)
                              ? (unsigned)(((char*)last - (char*)tbl) / 8) : 0u;
            unsigned cntU16 = readable((char*)spawnMgr + 0x9A, 2)
                              ? (unsigned)*(unsigned short*)((char*)spawnMgr + 0x9A) : 0u;
            const unsigned CNT_MAX = 256;   // clamp any candidate so the per-entry loop stays bounded
            if (cntVec > CNT_MAX) cntVec = CNT_MAX;
            if (cntU16 > CNT_MAX) cntU16 = CNT_MAX;
            unsigned cnt = (cntVec > 0 && cntVec <= CNT_MAX) ? cntVec
                         : ((cntU16 > 0 && cntU16 <= CNT_MAX) ? cntU16 : 0u);
            L("[Probe]   spawnTbl=%p last=%p cntVec=%u cntU16=%u -> using %u\n", tbl, last, cntVec, cntU16, cnt);

            if (cnt == 0) {
                // Diagnostic only: probe an alternate candidate table pointer; never swept in this build.
                void* alt = readable((char*)spawnMgr + 0x90, 4) ? *(void**)((char*)spawnMgr + 0x90) : nullptr;
                if (readable(alt, 16)) {
                    L("[Probe]   altTbl(+0x90)=%p w0=%08X w1=%08X w2=%08X w3=%08X\n",
                      alt, *(unsigned*)((char*)alt + 0),
                      *(unsigned*)((char*)alt + 4),
                      *(unsigned*)((char*)alt + 8),
                      *(unsigned*)((char*)alt + 12));
                }
            }

            void* cands[32];
            int   nc = 0;
            if (readable(tbl, 1)) {
                for (unsigned i = 0; i < cnt; ++i) {
                    char* entry = (char*)tbl + 8 * i;
                    if (!readable(entry, 8)) { L("[Probe]   entry[%u] unreadable\n", i); continue; }
                    void* e0 = *(void**)(entry + 0);
                    void* e4 = *(void**)(entry + 4);
                    unsigned k0 = (readable(e0, 0x20) && readable((char*)e0 + 0x1C, 4))
                                  ? *(unsigned*)((char*)e0 + 0x1C) : 0u;
                    unsigned k4 = (readable(e4, 0x20) && readable((char*)e4 + 0x1C, 4))
                                  ? *(unsigned*)((char*)e4 + 0x1C) : 0u;
                    L("[Probe]   entry[%u] e0=%p e4=%p e0+1C=%08X e4+1C=%08X\n", i, e0, e4, k0, k4);
                    if (readable(e0, 0x20) && readable((char*)e0 + 0x1C, 4) && *(unsigned*)((char*)e0 + 0x1C) != 0) {
                        if (nc < 32) cands[nc++] = e0;
                    }
                    if (readable(e4, 0x20) && readable((char*)e4 + 0x1C, 4) && *(unsigned*)((char*)e4 + 0x1C) != 0) {
                        if (nc < 32) cands[nc++] = e4;
                    }
                }
            } else {
                L("[Probe]   spawnTbl unreadable -> no live keys\n");
            }

            struct LiveTry { void* thisPtr; void* fn; const char* routeName; };
            const int CALL_CAP = 16;   // hard cap on TOTAL spawn calls in the live path

            for (int c = 0; c < nc && !ent; ++c) {
                void* cand = cands[c];
                if (!readable((char*)cand + 0x1C, 4)) continue;
                unsigned kid = *(unsigned*)((char*)cand + 0x1C);   // key id used for the verdict line
                const LiveTry LV[3] = {
                    { spawnMgr, spawnSlot39, "slot39(spawnMgr,0x6BCD60,liveKey)" },
                    { spawnMgr, buildSlot39, "slot39(spawnMgr,0x5FD730,liveKey)" },
                    { builder,  buildSlot39, "slot39(builder,0x5FD730,liveKey)"  },  // kept last for the trail
                };
                for (int v = 0; v < 3 && !ent; ++v) {
                    if (!LV[v].thisPtr || !readable(LV[v].fn, 4)) {
                        L("[Probe]   live skip: this=%p fn=%p unreadable\n", LV[v].thisPtr, LV[v].fn);
                        continue;
                    }
                    if (tries >= CALL_CAP) {
                        L("[Probe]   live call cap (%d) reached -> truncating\n", CALL_CAP);
                        break;
                    }
                    ++tries;
                    unsigned exc = 0;
                    void* e = nullptr;
                    __try { e = ((Slot39_t)LV[v].fn)(LV[v].thisPtr, kid); }
                    __except (exc = (unsigned)GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { e = nullptr; }
                    L("[Probe]   try#%d cand[%d]=%p key=%08X this=%p fn=%p -> %p exc=0x%08X\n",
                      tries, c, cand, kid, LV[v].thisPtr, LV[v].fn, e, exc);
                    if (readable(e, 0x50)) { ent = e; route = LV[v].routeName; usedHash = kid; }
                }
                if (tries >= CALL_CAP) break;
            }
        } else {
            // ---- 2b) legacy hardcoded-hash sweep (exact revert: CrowdProbeLiveKeys=0) ----
            struct Slot39Try {
                void*       thisPtr;    // ECX
                void*       fn;         // slot 39 target
                int         nargs;      // 1 or 2 stack args
                unsigned    fixed0;     // nargs==2 only: the leading "group" value (hash is last)
                const char* routeName;  // verdict-line route tag
            };
            const Slot39Try TRIES[4] = {
                { spawnMgr, spawnSlot39, 1, 0, "slot39(spawnMgr,1arg)"   },
                { spawnMgr, spawnSlot39, 2, 0, "slot39(spawnMgr,2arg,0)" },
                { spawnMgr, spawnSlot39, 2, 1, "slot39(spawnMgr,2arg,1)" },
                { builder,  buildSlot39, 1, 0, "slot39(builder,1arg)"    },  // the old call, kept last
            };

            for (int t = 0; t < 4 && !ent; ++t) {
                if (!TRIES[t].thisPtr || !readable(TRIES[t].fn, 4)) {
                    L("[Probe]   try skip: this=%p fn=%p unreadable\n", TRIES[t].thisPtr, TRIES[t].fn);
                    continue;
                }
                for (int i = 0; i < N_HASHES && !ent && tries < 12; ++i) {
                    ++tries;
                    unsigned a0 = (TRIES[t].nargs == 1) ? CROWD_HASHES[i] : TRIES[t].fixed0;
                    unsigned a1 = (TRIES[t].nargs == 1) ? 0u : CROWD_HASHES[i];
                    unsigned exc = 0;
                    void* e = nullptr;
                    __try {
                        if (TRIES[t].nargs == 1) e = ((Slot39_t)TRIES[t].fn)(TRIES[t].thisPtr, CROWD_HASHES[i]);
                        else                     e = ((Slot39b_t)TRIES[t].fn)(TRIES[t].thisPtr, a0, a1);
                    }
                    __except (exc = (unsigned)GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { e = nullptr; }
                    L("[Probe]   try#%d this=%p fn=%p args=(%u,%u) -> %p exc=0x%08X\n",
                      tries, TRIES[t].thisPtr, TRIES[t].fn, a0, a1, e, exc);
                    if (readable(e, 0x50)) { ent = e; route = TRIES[t].routeName; usedHash = CROWD_HASHES[i]; }
                }
            }
        }

        // ---- 3) fallback: the documented recipe (Duplicate -> BuildEntityWithSelector) ----
        if (!ent) {
            RowSelector* selector = PlayerProgression::GetCachedSelector();
            if (readable(selector, 8)) {
                DuplicationManager::Begin(1, 0);
                RowSelector* dupSel = nullptr;
                __try { dupSel = selector->Duplicate(0, 0); } __finally { DuplicationManager::End(); }
                if (readable(dupSel, 8)) {
                    Entity* e = nullptr;
                    __try { ((EntityBuilder*)builder)->BuildEntityWithSelector(&e, dupSel); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                        L("[Probe]   BuildEntityWithSelector EXCEPTION code=0x%08X\n", GetExceptionCode());
                    }
                    L("[Probe]   BuildEntityWithSelector -> %p\n", e);
                    if (readable(e, 0x50)) { ent = e; route = "slot17(BuildEntityWithSelector)"; }
                } else {
                    L("[Probe]   selector duplicate failed\n");
                }
            } else {
                L("[Probe]   no cached selector -> fallback skipped\n");
            }
        }

        if (!readable(ent, 0x50)) {
            L("[Probe] no entity produced (builder=%p vt=%p) -> NO-GO\n", builder, builderVt);
            return nullptr;
        }

        // ---- 4) integrate (observation only): add to world unless already present, then place ----
        if (GetPrivateProfileIntA("Coop", "CrowdProbeIntegrate", 1, ".\\AC4Coop.ini") != 0) {
            bool already = false;                                   // bounded membership scan
            int  n = world->m_CharacterList.GetSize();
            if (n < 0 || n > 4096) n = 0;
            for (int i = 0; i < n && !already; ++i) {
                Entity** slot = world->m_CharacterList.GetElement(i);
                if (slot && readable(slot, 4) && *slot == (Entity*)ent) already = true;
            }
            if (already) {
                L("[Probe] entity already in world -> AddEntity skipped\n");
                added = true;
            } else {
                __try {
                    world->AddEntity((Entity*)ent);
                    ((Entity*)ent)->SetPosition(player->GetPosition() + g_offset);
                    added = true;
                    L("[Probe] AddEntity issued + placed at player+g_offset\n");
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    L("[Probe] AddEntity/place EXCEPTION code=0x%08X\n", GetExceptionCode());
                }
            }
        } else {
            L("[Probe] built entity only (CrowdProbeIntegrate=0); not added to world\n");
        }

        // ---- 5) verdict line (the GO/NO-GO) ----
        uint32_t evt      = readable(ent, 8) ? (uint32_t)(uintptr_t)(*(void**)ent) : 0;
        unsigned children = readable((char*)ent + 0x66, 2) ? *(unsigned short*)((char*)ent + 0x66) : 0;
        uint32_t f5C      = readable((char*)ent + 0x5C, 4) ? (uint32_t)(uintptr_t)(*(void**)((char*)ent + 0x5C)) : 0;
        uint32_t fB0      = readable((char*)ent + 0xB0, 4) ? (uint32_t)(uintptr_t)(*(void**)((char*)ent + 0xB0)) : 0;
        unsigned anim8D0  = readable((char*)ent + 0x8D0, 4) ? *(unsigned*)((char*)ent + 0x8D0) : 0;
        unsigned av0 = 0, av1 = 0, av2 = 0, av3 = 0;
        if (readable((void*)(uintptr_t)anim8D0, 0x10)) {
            av0 = *(unsigned*)((char*)(uintptr_t)anim8D0 + 0);
            av1 = *(unsigned*)((char*)(uintptr_t)anim8D0 + 4);
            av2 = *(unsigned*)((char*)(uintptr_t)anim8D0 + 8);
            av3 = *(unsigned*)((char*)(uintptr_t)anim8D0 + 12);
        }
        HookMath::Vector4 pos = ((Entity*)ent)->GetPosition();

        uint64_t bid = (uint64_t)(uintptr_t)builder;             // default: the builder pointer itself
        HandleContainer* bhc = PlayerProgression::GetCachedBuilderHandle();
        if (readable(bhc, 0x14) && handleDeref(bhc) == builder) bid = bhc->m_ObjectID;

        RowSelector* sel = PlayerProgression::GetCachedSelector();
        uint32_t selVt = readable(sel, 8) ? (uint32_t)(uintptr_t)(*(void**)sel) : 0;

        L("[Probe] entity=0x%08X vt=0x%08X children=%u f5C=0x%08X fB0=0x%08X pos=(%.1f,%.1f,%.1f) builderID=0x%08X%08X selVt=0x%08X route=%s hash=%08X added=%d anim8D0=0x%08X anim8D0v={%08X %08X %08X %08X}\n",
          (uint32_t)(uintptr_t)ent, evt, children, f5C, fB0, pos.x, pos.y, pos.z,
          (uint32_t)(bid >> 32), (uint32_t)bid, selVt, route, usedHash, (int)added,
          anim8D0, av0, av1, av2, av3);
        return ent;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Probe] EXCEPTION during crowd probe (code=0x%08X)\n", GetExceptionCode());
        return nullptr;
    }
}

// ---- scene / interior awareness (Feature #1) ---------------------------------------
// A coarse, deterministic "which world cell is the player standing in" key. Interior spaces in this
// game are separate world cells, so two peers in different cells are (almost always) in different
// scenes and must not see or drive each other's puppet. Returns 0 == "unknown" so an unreadable cell
// size can never wedge the link (the caller treats 0 on either side as "allow").
static int floorDivIdx(float v, float cell) {
    float q = v / cell;
    int   i = (int)q;
    if ((float)i > q) --i;                 // floor() semantics for negative coordinates
    return i;
}

static uint32_t computeSceneKey(World* world, Entity* player) {
    __try {
        if (!readable(world, 8)) return 0;
        uint32_t cell = world->m_FloorHeightData.m_CellSize;   // World.h:116 — terrain cell size (metres)
        if (cell == 0) return 0;                               // unreadable/absent -> "unknown"
        if (!readable(player, 8)) return 0;
        float x = player->m_GlobalMatrix.M41;
        float y = player->m_GlobalMatrix.M42;
        float z = player->m_GlobalMatrix.M43;
        uint32_t cx = (uint32_t)floorDivIdx(x, (float)cell) & 0x3FFu;
        uint32_t cy = (uint32_t)floorDivIdx(y, (float)cell) & 0x3FFu;
        uint32_t cz = (uint32_t)floorDivIdx(z, (float)cell) & 0x3FFu;
        return cx | (cy << 10) | (cz << 20);                   // 10 bits per axis
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The engine's own last-load cell id (diagnosis only): World::m_LoadingAdvisor is a LoadingAdvisor*.
static uint32_t worldLastLoadCellId(World* world) {
    __try {
        if (!readable(world, 8)) return 0;
        LoadingAdvisor* adv = world->m_LoadingAdvisor;
        if (!readable(adv, 0x100)) return 0;
        return (uint32_t)adv->m_LastLoadPositionCellID;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- networking capture/apply (Front B) --------------------------------------------
// Ghost ship sync state: the spawned second ship entity + the peer's ship position to drive it to
// (via the SetGlobalMatrixInternal render override). g_ghostShip is set when m_Duplicate succeeds.
static void*   g_ghostShip = nullptr;        // the spawned ghost ship entity
static float   g_ghostSyncPos[3] = {0,0,0};  // peer's ship world position (x,y,z)
static float   g_ghostSyncQuat[4] = {0,0,0,1}; // peer's ship rotation quaternion (x,y,z,w)
static volatile long g_ghostHasSync = 0;     // 1 = we have a peer ship position to drive to
static bool    g_ghostAuto = false;          // 1 = spawned by the net path (despawn on disembark); 0 = manual F12
static float   g_localShipZ = 0.0f;          // the LOCAL ship's current Z (water height, bobs with waves)

// Read a player entity's full state (transform + locomotion) into a PlayerState.
bool Phase1::CaptureLocalState(PlayerState& s) {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return false;

        if (inWhiteRoom(world))
            return false;

        s.worldId = world->GetObjectID();
        s.protoVer = NET_PROTO_VER;

        Entity* player = world->GetPlayer();
        if (!player)
            return false;

        s.sceneKey    = computeSceneKey(world, player);
        s.sceneCellId = worldLastLoadCellId(world);

        s.matrix = player->m_GlobalMatrix;
        s.curSpeed = s.destSpeed = 0;
        s.sprint = s.curMvt = s.destMvt = 0;
        s.contextId = CTX_GROUND;

        // DIAGNOSTIC: real world-space speed from position delta (m_CurSpeedRatio reads 0 always on this build,
        // so it can't tell us if the player is moving). This is the ground truth of "am I actually running?".
        static float s_pvx = 0, s_pvy = 0, s_pvz = 0; static bool s_pvHave = false;
        float velMag = 0.0f;
        {
            float x = player->m_GlobalMatrix.M41, y = player->m_GlobalMatrix.M42, z = player->m_GlobalMatrix.M43;
            if (s_pvHave) { float dx=x-s_pvx, dy=y-s_pvy, dz=z-s_pvz; velMag = sqrtf(dx*dx+dy*dy+dz*dz); }
            s_pvx=x; s_pvy=y; s_pvz=z; s_pvHave=true;
        }

        Human* h = AIEntityHelper::GetHuman(player);
        HumanDataBundle* pB = h->m_Data.m_HumanDataBundle;
        s.contextId = h->GetCurrentContextID(MAIN_AI_TRACK);

        s.curSpeed = pB->m_GroundData.m_CurSpeedRatio;
        s.destSpeed = pB->m_GroundData.m_DestSpeedRatio;
        s.heading = pB->m_GroundData.m_DestHeading;
        s.isSet = pB->m_GroundData.m_IsSet;
        s.sprint = pB->m_GroundData.m_Sprint;
        s.breakHardOnEdge = pB->m_GroundData.m_BreakHardOnEdge;
        s.breakHard = pB->m_GroundData.m_BreakHard;
        s.curMvt = pB->m_GroundData.m_CurMvtType;
        s.destMvt = pB->m_GroundData.m_DestMvtType;
        s.humanGroundStage = (int32_t)pB->m_GroundData.m_HumanGroundStage;
        s.subState = (int32_t)pB->m_GroundData.m_SubState;

        s.weaponTotallyOut = h->m_WeaponTotallyOut;
        s.isInTree = h->m_IsInTree;
        s.isOnShip = h->m_IsOnShip;
        s.isOnOcean = h->m_IsOnOcean;
        s.doJumpToVoid = h->m_DoJumpToVoid;
        s.stalkingZoneCount = h->m_StalkingZoneCount;
        s.stalkingZoneBlendedCount = h->m_StalkingZoneBlendedCount;
        s.stalkingClipCount = h->m_StalkingClipCount;
        s.stalkingClipBlendedCount = h->m_StalkingClipBlendedCount;
        s.stalkingClipAllowFastStalkingCount = h->m_StalkingClipAllowFastStalkingCount;
        s.disturbedZoneCount = h->m_DisturbedZoneCount;

        // Current primary action (anim clip) the player is playing — the file-defined alias at Action+0x08.
        // Replicated so the puppet plays the same clip regardless of context (universal animation sync).
        s.actionAlias = 0;
        s.actionChannel = 0xFFFF;
        s.actionLayer = ACTLAYER_PRIMARY;
        s.blendCount = 0;
        for (int i = 0; i < BLEND_MAX; ++i) s.blendFactors[i] = 0.0f;
        s.timeScale = 1.0f;   // default = normal playback; NEVER send 0 (would freeze the puppet's anim)
        s.actionRatio = 0.0f;
        s.blendInLen = -1.0f;
        s.blendInStartFromEnd = 0.0f;
        s.blendInTargetStart = 0.0f;
        s.blendInFlags = 0;
        s.isTransitionAction = 0;
        {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x40)) {
                s.timeScale = ac->GetTimeScale(0, ACTLAYER_PRIMARY);   // primary-channel clip speed (foot-speed warp)
                s.actionRatio = ac->GetCurrentActionRatio(0, ACTLAYER_PRIMARY);   // playback phase (seek on switch)
                // The blend-in the peer's engine actually used for its current action (incl. FSM-generated
                // custom transitions the puppet can't look up itself).
                char* ev = (char*)ac->GetCurrentEvaluator(0, ACTLAYER_PRIMARY);
                if (readable(ev, 0x2B0) && *(uint8_t*)(ev + 0xD4) == 4) {
                    s.blendInLen          = *(float*)(ev + 0x1C8);
                    s.blendInStartFromEnd = *(float*)(ev + 0x1CC);
                    s.blendInTargetStart  = *(float*)(ev + 0x1D0);
                    s.blendInFlags        = *(uint16_t*)(ev + 0x1DC);
                    s.isTransitionAction = *(uint8_t*)(ev + 0x258);
                }
                unsigned char mirrored = 0;
                void* act = ac->GetCurrentAction(&mirrored, 0, ACTLAYER_PRIMARY);
                if (readable(act, 0x14)) {
                    s.actionAlias = *(uint32_t*)((char*)act + 8);
                    void* bpc = *(void**)((char*)act + 0x0C);          // Action::m_BodyPartChannel
                    if (readable(bpc, 0x0A)) s.actionChannel = *(uint16_t*)((char*)bpc + 8);
                    s.actionLayer = *(int32_t*)((char*)act + 0x10);    // Action::m_Layer (ACTLayer)

                    // The walk/jog/run speed sub-blend within the current locomotion LOOP action. Blend factors
                    // are indexed on the CONTROLLER channel (channel 0 = the primary channel controller, the
                    // same channelID we pass to GetCurrentAction above), NOT the action's body-part channel
                    // (s.actionChannel, which is only for SetTargetAction/CAS routing). The engine's own
                    // parkour code sets these as SetBlendFactor(i, v, 0, ACTLAYER_PRIMARY). The discrete START
                    // clips are picked by the alias, so only the LOOP needs this blend to vary with speed.
                    // Capture the WHOLE array: the ground move cycle evaluates a 46-slot blend tree
                    // (UpdateGroundBlendTree -> SetTargetBlendFactors(46 floats, 0, PRIMARY)) and the gait
                    // weights live past slot 3 — syncing only 0..3 left the puppet on the default (walk).
                    int n = ac->GetBlendFactorCount(0, ACTLAYER_PRIMARY);
                    if (n > BLEND_MAX) n = BLEND_MAX;
                    if (n < 0) n = 0;
                    s.blendCount = (uint8_t)n;
                    for (int i = 0; i < n; ++i)
                        s.blendFactors[i] = ac->GetBlendFactor((unsigned char)i, 0, ACTLAYER_PRIMARY);
                }
            }
            static uint32_t lastCapAlias = 0;
            if (s.actionAlias != lastCapAlias) {
                L("[Net] cap SWITCH %u -> %u blendIn=%.2f type=%u flags=%04X tgt=%.2f trans=%u ratio=%.2f\n",
                  lastCapAlias, s.actionAlias, s.blendInLen, (unsigned)(s.blendInFlags & 7), (unsigned)s.blendInFlags,
                  s.blendInTargetStart, (unsigned)s.isTransitionAction, s.actionRatio);
                lastCapAlias = s.actionAlias;
            }
            static int bc = 0;
            if ((++bc % 30) == 0) {
                char nz[512];
                FormatBlendSlots(s.blendFactors, s.blendCount, nz, sizeof(nz));
                L("[Net] cap action=%u ch=%u ly=%d blend(n=%u)={%s} ts=%.2f VEL=%.3f\n",
                  s.actionAlias, s.actionChannel, s.actionLayer, (unsigned)s.blendCount, nz, s.timeScale, velMag);
            }
        }

        // Context-tagged payload. Swim: read the swim locomotion input from the FSM scope so the peer's
        // puppet can play the moving-swim anim and travel (ground rides the common fields above).
        if (s.contextId == CTX_SWIM) {
            HumanSwimScope* sc = swimScope(h);
            if (sc) {
                s.ctx.swim.destDir[0] = sc->m_DestDirection.x;
                s.ctx.swim.destDir[1] = sc->m_DestDirection.y;
                s.ctx.swim.destDir[2] = sc->m_DestDirection.z;
                s.ctx.swim.destDir[3] = sc->m_DestDirection.w;
                s.ctx.swim.destSpeedRatio     = sc->m_DestSpeedRatio;
                s.ctx.swim.curSpeedProportion = sc->m_CurSpeedProportion;   // eased current speed -> anim blend
                s.ctx.swim.sprint             = sc->m_Sprint;
                for (int i = 0; i < 8; ++i) s.ctx.swim.aiSwim[i] = sc->m_AISwim[i];  // swim anim-request floats
            } else {
                s.ctx.swim.destSpeedRatio = 0.0f;
                s.ctx.swim.curSpeedProportion = 0.0f;
                s.ctx.swim.sprint = 0.0f;
                for (int i = 0; i < 8; ++i) s.ctx.swim.aiSwim[i] = 0.0f;
            }
            // Character-controller motion vectors (the actual velocity/force/impulse the FSM & anim graph
            // read). GetCharacterComponent returns the biped controller (a CharacterController).
            CharacterController* pcc = (CharacterController*)h->GetCharacterComponent();
            if (readable(pcc, 0x600)) {
                vecToArr(s.ctx.swim.ccVelocity, pcc->m_Velocity);
                vecToArr(s.ctx.swim.ccForce,    pcc->m_Force);
                vecToArr(s.ctx.swim.ccImpulse,  pcc->m_Impulse);
            } else {
                for (int i = 0; i < 4; ++i) { s.ctx.swim.ccVelocity[i]=0; s.ctx.swim.ccForce[i]=0; s.ctx.swim.ccImpulse[i]=0; }
            }
            static int sl = 0;
            if ((++sl % 30) == 0)
                L("[Net] swim cap scope=%p spd=%.3f cur=%.3f vel=(%.2f,%.2f,%.2f)\n",
                  sc, s.ctx.swim.destSpeedRatio, s.ctx.swim.curSpeedProportion,
                  s.ctx.swim.ccVelocity[0], s.ctx.swim.ccVelocity[1], s.ctx.swim.ccVelocity[2]);
        }

        // ---- ship sync: capture the local ship's position/rotation/HP when aboard ----------
        s.shipValid = false;
        s.shipPos[0] = s.shipPos[1] = s.shipPos[2] = 0.0f;
        s.shipQuat[0] = s.shipQuat[1] = s.shipQuat[2] = 0.0f; s.shipQuat[3] = 1.0f;
        s.shipHP = 0;
        void* ship = handleResolve(h->m_IsOnShipEntity);
        if (!ship) ship = handleResolve(h->m_Data.m_Vehicle);
        if (ship && readable(ship, sizeof(HookMath::Transform))) {
            HookMath::Transform* sm = &((BaseEntity*)ship)->m_GlobalMatrix;
            s.shipPos[0] = sm->M41; s.shipPos[1] = sm->M42; s.shipPos[2] = sm->M43;
            g_localShipZ = sm->M43;   // local wave height, for the ghost's Z (bobs with the local waves)
            HookMath::Quaternion q;
            HookMath::FromMatrix(sm, &q);
            s.shipQuat[0] = q.x; s.shipQuat[1] = q.y; s.shipQuat[2] = q.z; s.shipQuat[3] = q.w;
            void* stat = readable((char*)ship + 0x5C, 4) ? *(void**)((char*)ship + 0x5C) : nullptr;
            s.shipHP = (stat && readable((char*)stat + 0x84, 4)) ? *(int*)((char*)stat + 0x84) : 0;
            s.shipValid = true;
        }

        s.magic = NET_MAGIC;
        s.valid = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Apply a remote peer's state to the puppet (absolute position — shared world coords, no offset).
// Never nulls g_puppet on a transient fault (that would trigger endless respawns).
void Phase1::ApplyRemoteState(const PlayerState& s) {
    // Ship sync (independent of the puppet): drive the spawned ghost ship to the peer's ship position.
    if (s.valid && s.shipValid) {
        g_ghostSyncPos[0] = s.shipPos[0];
        g_ghostSyncPos[1] = s.shipPos[1];
        g_ghostSyncPos[2] = s.shipPos[2];
        g_ghostSyncQuat[0] = s.shipQuat[0];
        g_ghostSyncQuat[1] = s.shipQuat[1];
        g_ghostSyncQuat[2] = s.shipQuat[2];
        g_ghostSyncQuat[3] = s.shipQuat[3];
        g_ghostHasSync = 1;
    } else {
        g_ghostHasSync = 0;
    }
    if (!g_puppet || !s.valid) return;
    __try {
        // Position + orientation of the entity: SetGlobalMatrixInternal(updatePhysics=true) sets the entity
        // matrix, updates the graphic node/skeleton and pushes into the physic components — this drives
        // rendering and rotation for every context. It does NOT move the character controller, though, which
        // is what FSM/physics-driven contexts (swim) actually position from — that's handled by the kamikaze
        // controller warp just below. s.matrix is pack(1)/const, so copy to an aligned local for the call.
        HookMath::Transform m = s.matrix;
        g_puppet->SetGlobalMatrixInternal(&m, true);
        Human* h = AIEntityHelper::GetHuman(g_puppet);
        HumanDataBundle* qB = h->m_Data.m_HumanDataBundle;
        bool bok = readable((void*)qB, 0x150);

        // ---- per-frame effective mode (per-context action ownership) -------------------------------------
        // Exactly ONE action-writer at all times so two threads never play an action at once (the crash):
        //   GROUND context  -> puppet's own FSM owns actions: behavior ENABLED, feed HumanGroundData, we do
        //                       NOT SetTargetAction (reproduces the MVP's correct locomotion gait).
        //   other contexts  -> WE own actions: behavior DISABLED + action replication (parkour/climb/etc.).
        // HybridLoco=0 restores the old global-flag behavior for A/B.
        bool hybrid     = (g_hybridLoco != 0);
        bool peerGround = (s.contextId == CTX_GROUND);
        bool aiOff   = hybrid ? !peerGround : (g_disablePuppetAI != 0);   // disable the puppet's behavior?
        bool actNow  = hybrid ? !peerGround : (g_driveActions   != 0);    // run our action replication?
        bool locoNow = hybrid ?  peerGround : (g_driveLoco       != 0);   // feed HumanGroundData (FSM anim)?
        bool braindead = aiOff && !actNow;                               // position-only (never true in hybrid)

        // Behavior ownership toggle: ENABLE when the FSM should own the puppet (ground), DISABLE when WE do
        // (off-ground). A toggle starts a short cooldown during which we suppress our own action writes, so the
        // game's worker-thread action update can quiesce before/after we take over -> the two never overlap.
        // Retries every frame until the deferred AddToWorld has actually set the behavior component up.
        {
            int wantBhv = aiOff ? 0 : 1;
            if (g_bhvState != wantBhv) {
                void* bhv = g_puppet->GetBehaviorComponent();
                if (readable(bhv, 8)) {
                    ((Component*)bhv)->SetEnabled(wantBhv != 0);
                    g_bhvState = wantBhv;
                    g_switchCd = 4;                            // ~4 frames of no-action-writes around the toggle
                    if (wantBhv == 0) g_casActivated = false;  // re-activate our CAS when we next own actions
                    L("[Net] puppet behavior %s (ctx=%d)\n", wantBhv ? "ENABLED" : "disabled", s.contextId);
                }
            }
        }
        bool writeActions = actNow && g_switchCd == 0;   // our action writes are gated on the settle cooldown
        if (g_switchCd > 0) --g_switchCd;

        // We are ALWAYS the position authority (SetGlobalMatrixInternal above): stop the character controller
        // from driving the entity so its physics can never fight the forced matrix — in both modes. In ground
        // mode the (enabled) FSM still animates the correct gait: UpdateMoveCycleNew derives the walk/jog/run
        // from the HumanGroundData intention we feed below, not from the puppet actually moving through space.
        {
            CharacterController* cc = (CharacterController*)h->GetCharacterComponent();
            if (readable(cc, 0x600)) cc->m_DriveEntity = false;
        }
        // Braindead only (no action replication AND no FSM): kill the ActionComponent update (flag @ +0x159)
        // so nothing poses the skeleton. Never reached in hybrid (there's always an animator).
        if (braindead) {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x160)) *((unsigned char*)ac + 0x159) = 1;
        }

        if (locoNow) {   // feed HumanGroundData so the puppet's (enabled) FSM locomotes at the peer's gait
        qB->m_GroundData.m_CurSpeedRatio = s.curSpeed;
        qB->m_GroundData.m_DestSpeedRatio = s.destSpeed;
        qB->m_GroundData.m_DestHeading = s.heading;
        qB->m_GroundData.m_IsSet = s.isSet;
        qB->m_GroundData.m_Sprint = s.sprint;
        qB->m_GroundData.m_BreakHardOnEdge = s.breakHardOnEdge;
        qB->m_GroundData.m_BreakHard = s.breakHard;
        qB->m_GroundData.m_CurMvtType = s.curMvt;
        qB->m_GroundData.m_DestMvtType = s.destMvt;
        qB->m_GroundData.m_HumanGroundStage = (HumanGroundData::HumanGroundStage)s.humanGroundStage;
        qB->m_GroundData.m_SubState = (HumanGroundData::HumanGroundSubState)s.subState;
        h->m_WeaponTotallyOut = s.weaponTotallyOut;
        h->m_IsInTree = s.isInTree;
        h->m_IsOnShip = s.isOnShip;
        h->m_IsOnOcean = s.isOnOcean;
        h->m_DoJumpToVoid = s.doJumpToVoid;
        h->m_StalkingZoneCount = s.stalkingZoneCount;
        h->m_StalkingZoneBlendedCount = s.stalkingZoneBlendedCount;
        h->m_StalkingClipCount = s.stalkingClipCount;
        h->m_StalkingClipBlendedCount = s.stalkingClipBlendedCount;
        h->m_StalkingClipAllowFastStalkingCount = s.stalkingClipAllowFastStalkingCount;
        h->m_DisturbedZoneCount = s.disturbedZoneCount;
        } // locoNow
        // Context replication: make the puppet ENTER the peer's active context so its statechart plays
        // the matching parkour/climb/swim animation (transform is already synced). Edge-triggered on the
        // remote id so we mirror each context change once (re-issuing every frame would restart the anim).
        // TransitToContext exits the current context and enters the new one on the puppet's active track;
        // returning to ground (id 2) has no explicit context, so we pop via ExitCurrentContext instead.
        if (g_driveContext && readable(h, 0x50)) {
            int want = s.contextId;
            if (want != g_lastRemoteCtx) {
                int cur = h->GetCurrentContextID(MAIN_AI_TRACK);
                // A context is drivable if it's ground, or its bit is set in the allow-mask. The masked
                // (target-needing) contexts would crash a later engine update, so instead of entering them
                // we force the puppet back to ground — never leave it stranded in a stale parkour pose.
                bool allowed = (want == CTX_GROUND) ||
                               (want >= 0 && want < 32 && (g_ctxMask & (1u << want)));
                if (allowed) {
                    if (want != cur) {
                        if (want == CTX_GROUND) h->ExitCurrentContext();
                        else                    h->TransitToContext(want);
                        L("[Net] context %d -> %d (puppet)\n", cur, want);
                    }
                } else if (cur != CTX_GROUND) {
                    h->ExitCurrentContext();
                    L("[Net] context %d -> ground (puppet; %d masked)\n", cur, want);
                }
                g_lastRemoteCtx = want;
            }
        }
        // Swim locomotion drive: once the puppet is actually in the Swim context (transitioned above), the
        // FSM + scope exist, so push the peer's swim input onto them — this is what makes the puppet leave
        // the idle swim pose, play the moving-swim anim and travel. Position is already corrected via the
        // SetGlobalMatrixInternal capsule push at the top. Gated exactly like the swim position path.
        if (g_driveContext && (g_ctxMask & (1u << CTX_SWIM)) && s.contextId == CTX_SWIM) {
            // Authoritative position: the swim FSM positions the character through its CharacterController,
            // which the entity-matrix push doesn't move — so force the controller to the peer's position via
            // the kamikaze target + flag (field-write form of CharacterController::SetGlobalPositionKamikaze).
            // Swim-gated only: on ground the FSM is really moving the character and this fights it -> jitter.
            CharacterController* cc = (CharacterController*)h->GetCharacterComponent();
            if (readable(cc, 0x600)) {
                cc->m_KamikaziPosition.x  = s.matrix.M41;
                cc->m_KamikaziPosition.y  = s.matrix.M42;
                cc->m_KamikaziPosition.z  = s.matrix.M43;
                cc->m_KamikaziPosition.w  = 1.0f;
                cc->m_ConcatenateToAnim   = false;
                cc->m_UseVelocityPosition = false;
                cc->m_UseKamikazePosition = true;
                // Give the puppet the peer's real motion so the FSM/anim graph see movement (the kamikaze
                // position is authoritative; these feed the state/anim, not the final position).
                arrToVec(cc->m_Velocity, s.ctx.swim.ccVelocity);
                arrToVec(cc->m_Force,    s.ctx.swim.ccForce);
                arrToVec(cc->m_Impulse,  s.ctx.swim.ccImpulse);
            }
            HumanSwimScope* sc = swimScope(h);
            if (sc) {
                sc->m_DestDirection.x   = s.ctx.swim.destDir[0];
                sc->m_DestDirection.y   = s.ctx.swim.destDir[1];
                sc->m_DestDirection.z   = s.ctx.swim.destDir[2];
                sc->m_DestDirection.w   = s.ctx.swim.destDir[3];
                sc->m_DestSpeedRatio    = s.ctx.swim.destSpeedRatio;
                // Also drive the eased CURRENT state — the FSM won't advance these itself on a teleported
                // puppet (no real velocity), so without them the facing goes stale and the stroke anim
                // never blends in. m_CurSpeedProportion is the stroke blend; m_Sprint the fast-swim flag;
                // m_AISwim[] are the per-gait swim anim-request floats that select which stroke plays.
                sc->m_CurSpeedProportion = s.ctx.swim.curSpeedProportion;
                sc->m_Sprint             = s.ctx.swim.sprint;
                for (int i = 0; i < 8; ++i) sc->m_AISwim[i] = s.ctx.swim.aiSwim[i];
            }
            static int sw = 0;
            if ((++sw % 30) == 0)
                L("[Net] swim drv scope=%p destSpd=%.3f curSpd=%.3f sprint=%.0f ai=[%.2f %.2f %.2f %.2f %.2f]\n",
                  sc, s.ctx.swim.destSpeedRatio, s.ctx.swim.curSpeedProportion, s.ctx.swim.sprint,
                  s.ctx.swim.aiSwim[0], s.ctx.swim.aiSwim[2], s.ctx.swim.aiSwim[3], s.ctx.swim.aiSwim[4], s.ctx.swim.aiSwim[6]);
        }

        // Give the puppet the local player's CharacterActionSet ONCE so its actions resolve to the right
        // character's clip overrides (both are Edward on this machine) instead of the generic base. The
        // gameplay path that normally sets it is disabled on the puppet. One-shot: re-setting it every frame
        // resets the action to idle. The CAS reference lives at ActionComponent+0x18.
        if (writeActions && !g_casActivated) {
            ActionComponent* pac = h->GetActionComponent();
            World* casWorld = World::GetCurrentWorld();
            Entity* casLp = casWorld ? casWorld->GetPlayer() : nullptr;
            Human*  casLph = casLp ? AIEntityHelper::GetHuman(casLp) : nullptr;
            ActionComponent* lac = casLph ? casLph->GetActionComponent() : nullptr;
            if (readable(pac, 0x40) && readable(lac, 0x40)) {
                pac->ActivateCharacterActionSet((char*)lac + 0x18);
                g_casActivated = true;
                L("[Net] CAS set from local player (pac=%p lac=%p)\n", pac, lac);
            }
        }

        // Universal animation sync: play the peer's current action (anim clip) on the puppet via
        // SetTargetAction. Only issue when the peer's alias changed AND the puppet isn't ALREADY playing it:
        // re-forcing a target action that's already current re-runs its transition and intermittently CRASHES
        // (our tracked g_lastActionAlias resets on spawn/despawn, which is when a redundant re-force happened).
        // Checking the puppet's real current action is the reliable guard.
        if (writeActions && s.actionAlias != 0 && s.actionAlias != g_lastActionAlias) {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x40)) {
                unsigned char mir = 0;
                void* cur = ac->GetCurrentAction(&mir, 0, s.actionLayer);
                uint32_t curAlias = readable(cur, 0x0C) ? *(uint32_t*)((char*)cur + 8) : 0;
                if (curAlias != s.actionAlias) {
                    // Pass the peer's real channel + layer (not 0xFFFF / a hardcoded layer) so the
                    // CharacterActionSetManager doesn't take the kit-lookup path that bails and drops the CAS
                    // override (-> base/Connor clip).
                    ac->SetTargetAction((int)s.actionAlias, 0, 0, (int)s.actionChannel, (int)s.actionLayer);
                    // Start the new clip at the peer's phase instead of frame 0 — the game does the same right
                    // after its own SetTargetAction calls. Without it every swap pops. processDisplacement=0
                    // (we own position), processEvents=0 (don't fire the skipped span's anim events).
                    if (g_seekOnSwitch && s.actionRatio > 0.0f && s.actionRatio < 1.0f)
                        ac->SetCurrentActionRatio(s.actionRatio, 0, false, false, ACTLAYER_PRIMARY);
                    if (s.blendInLen >= 0.0f) {
                        g_blendFixAlias = s.actionAlias;
                        g_blendFixLen   = s.blendInLen;
                        g_blendFixStartFromEnd = s.blendInStartFromEnd;
                        g_blendFixTargetStart  = s.blendInTargetStart;
                        g_blendFixFlags = s.blendInFlags;
                        g_blendFixWait  = 10;
                    }
                    L("[Net] action -> %u ch=%u ly=%d ratio=%.2f blendIn=%.2f (puppet, cur was %u)\n", s.actionAlias, s.actionChannel, s.actionLayer, s.actionRatio, s.blendInLen, curAlias);
                }
                g_lastActionAlias = s.actionAlias;
            }
        }

        // Blend-in override: once the puppet's current evaluator IS the action we just targeted, give its
        // blend-in the peer's real length (stored ActionBlend, used if Start() hasn't run yet, AND the runtime
        // length, used if it has). Only while the blend is still in progress, so we never rewind a finished one.
        if (writeActions && g_blendFixWait > 0) {
            ActionComponent* ac = h->GetActionComponent();
            char* ev = readable(ac, 0x40) ? (char*)ac->GetCurrentEvaluator(0, ACTLAYER_PRIMARY) : nullptr;
            // 0x2C0 (not 0x2B0): the descriptor we write reaches +0x1DC, and the engine mirrors it into
            // m_InitialBlendFactors at +0x2BC - validate the whole span we can touch.
            void* evAct = (readable(ev, 0x2C0) && *(uint8_t*)(ev + 0xD4) == 4) ? *(void**)(ev + 0x2A8) : nullptr;
            // +0x258 == m_IsTransitionAction: never rewrite the blend of a mid-transition evaluator - its
            // descriptor is being handed off to its target (README names this as the corruption source).
            if (readable(evAct, 0x0C) && *(uint32_t*)((char*)evAct + 8) == g_blendFixAlias
                && *(uint8_t*)(ev + 0x258) == 0) {
                // Blend-in descriptor = m_BlendInEvaluator (+0xE0).m_ActionBlend (+0x1C8): m_Length, +0x1CC
                // m_StartTimeFromEnd, +0x1D0 m_TargetStartTime, +0x1D4 ratio keys (array - NOT copied), +0x1DC
                // u16 m_Flags (bits 0-2 m_BlendType). Copy the peer's whole descriptor: without a found transition
                // the engine uses flags 0x600 = ACTBLENDTYPE_NONE, a hard cut whatever the length says.
                // ACTBLENDTYPE_AROLLBROLL_WITHRATIO (6) needs the ratio keys we don't copy -> plain AROLLBROLL (1).
                float    t       = *(float*)(ev + 0xE0);
                float    was     = *(float*)(ev + 0xE4);
                uint16_t wasFl   = *(uint16_t*)(ev + 0x1DC);
                uint16_t fl      = g_blendFixFlags;
                float    len     = g_blendFixLen;
                if ((fl & 7) == 6) fl = (uint16_t)((fl & ~7) | 1);
                bool write = true;
                if ((fl & 7) == 0) {
                    // Peer cut with NO blend — typically a transition clip handing off to its target, seamless on
                    // the peer because the clip ends on the target's start pose. Our switch lands whenever the
                    // packet does, so that match isn't guaranteed: never downgrade a blend the puppet found itself,
                    // and otherwise use a short AROLLBROLL (MinBlendIn) to hide the timing seam.
                    if ((wasFl & 7) != 0 || g_minBlendIn <= 0.0f) write = false;
                    else { fl = (uint16_t)((fl & ~7) | 1); len = g_minBlendIn; }
                }
                // Re-validate immediately before the store: a rapid action-switch burst can replace the
                // controller's cached evaluator between the fetch above and here, and writing a freed or
                // recycled evaluator is what crashes AC4BFSP (0xc0000005). Require it is still the SAME
                // live, non-transition evaluator; otherwise skip this fix (the blend just stays as-is).
                if (write && t < len) {
                    char* evNow = readable(ac, 0x40) ? (char*)ac->GetCurrentEvaluator(0, ACTLAYER_PRIMARY) : nullptr;
                    if (evNow != ev || !readable(ev, 0x2C0) || *(uint8_t*)(ev + 0x258) != 0) {
                        write = false;
                    } else {
                        *(float*)(ev + 0x1C8)    = len;
                        *(float*)(ev + 0x1CC)    = g_blendFixStartFromEnd;
                        *(float*)(ev + 0x1D0)    = g_blendFixTargetStart;
                        *(uint16_t*)(ev + 0x1DC) = fl;
                        *(float*)(ev + 0xE4)     = len;   // runtime length, in case Start() already ran
                    }
                } else {
                    write = false;
                }
                L("[Net] blendIn fix alias=%u len %.2f -> %.2f type %u -> %u (t=%.2f)%s\n",
                  g_blendFixAlias, was, len, (unsigned)(wasFl & 7), (unsigned)(fl & 7), t, write ? "" : " [kept]");
                g_blendFixWait = 0;
            } else {
                --g_blendFixWait;
            }
        }

        // Blend factors (the walk/jog/run speed sub-blend within the current LOOP action) — set EVERY frame
        // since they vary continuously with the peer's speed. Indexed on the primary CONTROLLER channel 0 +
        // ACTLAYER_PRIMARY (same as the capture / the engine's own SetBlendFactor calls), NOT s.actionChannel
        // (the body-part channel used for SetTargetAction routing).
        // ROOT CAUSE of "always slowest walk": a plain SetBlendFactor is TRANSIENT — the puppet's action update
        // resets it to the slot default before sampling the clip, so our value never took. Confirmed via log:
        // the LOCAL capture reads real, varying blend factors (e.g. [0,0.04,0.95,0]) — the capture was fine;
        // the write just didn't stick. Fix (mirrors the engine's own locomotion): mark each slot
        // ACTPERSIST_ALWAYS(=4) so it survives the per-frame reset, THEN write the peer's factor. Done every
        // frame because the controller/evaluator is recreated on each SetTargetAction (fresh default persistence).
        if (writeActions && s.actionAlias != 0) {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x40)) {
                // Full array, bounded by BOTH the peer's slot count and the puppet's own controller size
                // (sized per action; the engine drops writes past it anyway).
                int n = s.blendCount;
                int pn = ac->GetBlendFactorCount(0, ACTLAYER_PRIMARY);
                if (pn < n) n = pn;
                for (int i = 0; i < n; ++i) {
                    ac->SetBlendFactorPersistence((unsigned char)i, ACTPERSIST_ALWAYS, 0, ACTLAYER_PRIMARY);
                    ac->SetBlendFactor((unsigned char)i, s.blendFactors[i], 0, ACTLAYER_PRIMARY);
                }
                // Playback time scale (clip speed) — the missing gait lever: read-back proved the action AND
                // blend factors already match the peer, so the only remaining difference is how fast the clip
                // plays. Persist it (like the blend) so the puppet's action update doesn't snap it back to 1.0.
                // Guard against a bogus/zero wire value that would freeze the anim.
                if (s.timeScale > 0.01f) {
                    ac->SetTimeScalePersistence(ACTPERSIST_ALWAYS, 0, ACTLAYER_PRIMARY);
                    ac->SetTimeScale(s.timeScale, 0, ACTLAYER_PRIMARY, false);
                }
                // DIAGNOSTIC: read back the puppet's OWN state right after we wrote it — (1) is it playing the
                // action we sent (alias match), (2) did the blend stick (readback == sent), (3) did the time
                // scale stick. One line tells us where the chain breaks.
                static int rc = 0;
                if ((++rc % 30) == 0) {
                    unsigned char mir = 0;
                    void* pcur = ac->GetCurrentAction(&mir, 0, ACTLAYER_PRIMARY);
                    uint32_t pAlias = readable(pcur, 0x0C) ? *(uint32_t*)((char*)pcur + 8) : 0;
                    float rb[BLEND_MAX] = {};
                    int pn = ac->GetBlendFactorCount(0, ACTLAYER_PRIMARY);
                    if (pn > BLEND_MAX) pn = BLEND_MAX;
                    for (int i = 0; i < pn; ++i) rb[i] = ac->GetBlendFactor((unsigned char)i, 0, ACTLAYER_PRIMARY);
                    float pupTS = ac->GetTimeScale(0, ACTLAYER_PRIMARY);
                    char rxs[512], pus[512];
                    FormatBlendSlots(s.blendFactors, s.blendCount, rxs, sizeof(rxs));
                    FormatBlendSlots(rb, pn, pus, sizeof(pus));
                    L("[Net] PUP act=%u (rx=%u) rxBlend(n=%u)={%s} pupBlend(n=%d)={%s} rxTS=%.2f pupTS=%.2f\n",
                      pAlias, s.actionAlias, (unsigned)s.blendCount, rxs, pn, pus, s.timeScale, pupTS);
                }
            }
        }
        static int lc = 0;
        if ((++lc % 30) == 0)
            L("[Net] apply seq=%u pos=(%.1f,%.1f,%.1f) spd=%.2f bundle=%d\n",
              s.seq, g_puppet->m_GlobalMatrix.M41, g_puppet->m_GlobalMatrix.M42, g_puppet->m_GlobalMatrix.M43, s.curSpeed, bok ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int ec = 0; if ((++ec % 30) == 0) L("[Net] ApplyRemoteState EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

// Remove the puppet from the world when the peer leaves our world or disconnects. World::RemoveEntity
// is the deferred counterpart to World::AddEntity (processes next frame). Clears g_puppet immediately
// so nothing drives the entity after we've asked for its removal. Main-thread only (like spawn).
void Phase1::DespawnPuppet() {
    if (!g_puppet) return;
    Entity* dead = g_puppet;
    g_puppet = nullptr;      // stop driving it right away, before the deferred removal runs
    g_blockReg = nullptr;    // clear the pad-suppression marker
    g_lastRemoteCtx = -1;    // next puppet starts fresh (re-apply the first remote context)
    g_lastActionAlias = 0xFFFFFFFF;  // re-apply the peer's action onto the next puppet
    g_bhvState = -1;                 // re-establish the next puppet's behavior state (toggle from unknown)
    g_switchCd = 0;
    g_casActivated = false;          // re-activate its CharacterActionSet
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return;

        world->RemoveEntity(dead);

        L("[Net] puppet despawned (%p)\n", dead);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Net] DespawnPuppet EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

// ---- ship-entity CONSTRUCTOR experiment [F12] ----------------------------------------
// Decisive test for "real ship sync": construct a SHIP entity from the engine's own ship constructor
// (file VA 0x00503330, RVA 0x103330; __thiscall -> ECX = this, NO stack args) on a zeroed 0x800-byte block,
// then hand it to the world EXACTLY like the puppet path does: World::AddEntity + Entity::SetPosition.
// Observation only: the USER looks at the game and reports whether the engine STREAMS the ship's model
// (hull/masts) on its own, or whether the entity is invisible/empty. No co-op path is touched
// (never touch g_puppet / g_blockReg). Whole body is SEH-guarded; every pointer read is readable()-checked.
typedef void (__thiscall *ShipCtorFn)(void* thisPtr);   // ship entity ctor: this in ECX, no stack args

void Phase1::TryShipCtorProbe() {
    __try {
        // (a) same world/player acquisition as TrySpawnPuppet.
        World* world = World::GetCurrentWorld();
        if (!world) { L("[ShipCtor] no world (not in-world)\n"); return; }

        Entity* player = world->GetPlayer();
        if (!player) { L("[ShipCtor] no player (not in-world)\n"); return; }

        // (b) generous, ZEROED block for the ship entity (ship entity is >0x400 bytes; 0x800 is a safe margin).
        //     VirtualAlloc returns zero-filled pages, which the ctor expects for a fresh object.
        void* buf = VirtualAlloc(nullptr, 0x800, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) { L("[ShipCtor] VirtualAlloc(0x800) failed (err=%u)\n", GetLastError()); return; }
        L("[ShipCtor] entity buf=%p (0x800 zeroed)\n", buf);

        // (c) run the ship entity CONSTRUCTOR (__thiscall, ECX=this, no stack args).
        __try {
            ((ShipCtorFn)(base() + 0x00103330))(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            L("[ShipCtor] ctor EXCEPTION code=0x%08X (entity half-built; NOT adding)\n", GetExceptionCode());
            return;
        }

        // (d) verify the constructed entity's vtable (expect 0x01E4A128).
        uint32_t vt = readable(buf, 4) ? *(uint32_t*)buf : 0;
        L("[ShipCtor] entity=%p vt=%08X (expect 0x01E4A128)\n", buf, vt);

        // (e) add to world + place near the player, copying TrySpawnPuppet's exact AddEntity/SetPosition pair.
        __try {
            world->AddEntity((Entity*)buf);
            HookMath::Vector4 ppos = player->GetPosition();
            ((Entity*)buf)->SetPosition(ppos + g_offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            L("[ShipCtor] AddEntity/SetPosition EXCEPTION code=0x%08X\n", GetExceptionCode());
            return;
        }

        // (f) verdict prompt for the human observer.
        L("[ShipCtor] AddEntity issued at player+offset. Observe the game - did a ship (hull/masts) appear, or is it invisible/empty?\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[ShipCtor] outer EXCEPTION code=0x%08X\n", GetExceptionCode());
    }
}

// Route A (ship physics): construct a fresh keyframed hkpRigidBody that shares the source ship's
// collision shape. The body is heap-allocated (0x220) and intentionally LEAKED on despawn — the ghost
// is hidden (never removed), so a Havok-memory-pool mismatch on free is avoided.

// Havok memory-pool allocator (via the engine's TLS + hkMemorySystem) — the physics update runs the
// memory system on bodies, so a plain malloc'd body can hang. Falls back to aligned malloc.
static void* HavokAlloc(unsigned int size) {
    __try {
        void* tls = TlsGetValue(*(DWORD*)(base() + 0x049DA4C8));   // DAT_04dda4c8 (TLS index)
        if (!tls) return nullptr;
        void* memSys = *(void**)((char*)tls + 0x2C);               // hkMemorySystem*
        if (!memSys || !readable(memSys, 4)) return nullptr;
        void* vt = *(void**)((char*)memSys + 0);                   // memory system vtable
        if (!vt || !readable(vt, 8)) return nullptr;
        typedef void* (__thiscall *AllocFn)(void*, unsigned int);
        AllocFn alloc = *(AllocFn*)((char*)vt + 4);                // vtable slot 1 = alloc
        return alloc(memSys, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void* ConstructGhostBody(void* sourceBody, const HookMath::Transform& tm) {
    __declspec(align(16)) unsigned char cinfo[0xD0];   // hkpRigidBodyCinfo read via aligned SSE
    memset(cinfo, 0, sizeof(cinfo));
    L("[ShipPhys] sourceBody=%p readable=%d\n", sourceBody, sourceBody ? (readable(sourceBody, 0x20) ? 1 : 0) : 0);
    if (sourceBody && readable(sourceBody, 0x20)) {
        void* shape = *(void**)((char*)sourceBody + 0x10);
        L("[ShipPhys]   sourceBody vt=%08X shape(+0x10)=%p\n", *(uint32_t*)sourceBody, shape);
        *(void**)(cinfo + 0x04) = shape;   // shape (shared, refcounted by base ctor)
        void* world = *(void**)((char*)sourceBody + 0x08);
        L("[ShipPhys]   world(+0x08)=%p\n", world);
        if (world && readable(world, 4)) {
            uint32_t wvt = *(uint32_t*)world;
            L("[ShipPhys]   world vtable=%08X\n", wvt);
            for (int i = 0; i < 40; ++i) {
                if (!readable((void*)(wvt + i * 4), 4)) break;
                L("[ShipPhys]   world vt[%d]=%08X\n", i, *(uint32_t*)(wvt + i * 4));
            }
        }
    }
    *(float*)(cinfo + 0x10) = tm.M41;
    *(float*)(cinfo + 0x14) = tm.M42;
    *(float*)(cinfo + 0x18) = tm.M43;
    *(float*)(cinfo + 0x1C) = 1.0f;
    *(float*)(cinfo + 0x20) = 0.0f;
    *(float*)(cinfo + 0x24) = 0.0f;
    *(float*)(cinfo + 0x28) = 0.0f;
    *(float*)(cinfo + 0x2C) = 1.0f;
    *(unsigned char*)(cinfo + 0xB4) = 5;          // motionType = keyframed (hkpFixedRigidMotion)
    *(float*)(cinfo + 0xB8) = 1200000.0f;          // mass
    void* body = HavokAlloc(0x220);
    if (!body) body = _aligned_malloc(0x220, 16);
    if (!body) return nullptr;
    memset(body, 0, 0x220);
    *(unsigned short*)((char*)body + 0x04) = 0x220;  // memSizeAndFlags
    __try {
        Hook::CallClassMethod<void, void*, void*>(0x00C4F2A0, body, cinfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[ShipPhys] ConstructGhostBody EXCEPTION 0x%08X\n", GetExceptionCode());
        // leak the half-constructed body (we can't tell Havok-pool vs aligned alloc; the ghost leaks anyway)
        return nullptr;
    }
    return body;
}

// Spawn a ghost ship = a FULL clone of the LOCAL ship (EntityGroup vtable[3] deep-copy + the 3 direct
// component fields + physics disabled). Returns the ghost EntityGroup, or nullptr on failure.
// Runs on the main thread (the frame hook). This is the ship-spawn primitive proven via F12.
// Deep-copy a ship entity into a visual-only ghost: EntityGroup::Duplicate (vtable[3]) + component
// deep-copy (+0x5C World shared, +0x78 rigging + +0xE8 AIComponent cloned) + disable physics/AI, then
// AddEntity at the source ship's position. The render override drives it thereafter. Verified: works for
// the Jackdaw (and any vt 0x01E4A128 ship, since they share the EntityGroup class + component offsets).
static void* DuplicateShip(void* shipEnt) {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world || !shipEnt || !readable(shipEnt, 4) || *(uint32_t*)shipEnt != 0x01E4A128u) return nullptr;

        // 1) Clone the EntityGroup via its own Duplicate (vtable[3] = FUN_00503600): full deep copy incl.
        //    the sub-entity array (hull/masts/sails). arg2=0 (CreateObject), arg3=1 (dup flag).
        void* dup = nullptr;
        DuplicationManager::Begin(1, 0);
        __try {
            typedef int (__thiscall *EGroupDupFn)(void*, void*, void*);
            dup = (void*)(*(EGroupDupFn*)(*(uintptr_t*)shipEnt + 0x0C))(shipEnt, nullptr, (void*)1);
        } __finally {
            DuplicationManager::End();
        }
        if (!dup || !readable(dup, 4)) return nullptr;

        // 2) FUN_00503600 does NOT copy the direct component fields. +0x5C = the game World (m_AssociatedWorld)
        //    -> SHARE the pointer (a cloned World has a null player -> the damage path null-derefs GetPlayer).
        //    +0x78 rigging + +0xE8 AIComponent -> deep-copy via their vtable[3].
        DuplicationManager::Begin(1, 0);
        __try {
            void* worldPtr = readable((char*)shipEnt + 0x5C, 4) ? *(void**)((char*)shipEnt + 0x5C) : nullptr;
            if (worldPtr) *(void**)((char*)dup + 0x5C) = worldPtr;
            static const int COMP_OFFS[2] = { 0x78, 0xE8 };
            for (int k = 0; k < 2; ++k) {
                void* comp = readable((char*)shipEnt + COMP_OFFS[k], 4) ? *(void**)((char*)shipEnt + COMP_OFFS[k]) : nullptr;
                if (!comp || !readable(comp, 4)) continue;
                typedef void* (__thiscall *CompCloneFn)(void*, void*, void*);
                void* cclone = (*(CompCloneFn*)(*(uintptr_t*)comp + 0x0C))(comp, nullptr, (void*)1);
                if (cclone && readable(cclone, 4)) *(void**)((char*)dup + COMP_OFFS[k]) = cclone;
            }
        } __finally {
            DuplicationManager::End();
        }

        // 3) Ghost physics (Route A): give the clone a FRESH keyframed hkpRigidBody (shares the source
        //    shape) so updatePhysics=true writes its motion state and the masts/sails skeleton follows.
        //    The RigidBodyComponent clone shallow-copied m_HKRigidBody; replace it with the new body.
        //    Keep the AIComponent (ship-ctrl) DISABLED — its damage path null-derefs on the broken world.
        Component* rbComp = ((Entity*)dup)->GetComponentByType(RigidBodyComponent::GetStaticClassDescriptor());
        if (rbComp && readable(rbComp, 4)) {
            RigidBodyComponent* rbc = (RigidBodyComponent*)rbComp;
            void* srcBody = rbc->m_RigidBody.m_HKRigidBody;   // shallow-copied source body (pre-null)
            void* newBody = ConstructGhostBody(srcBody, ((BaseEntity*)shipEnt)->m_GlobalMatrix);
            rbc->m_RigidBody.m_HKRigidBody = newBody;          // may be null on failure
            rbc->m_DriveEntity = false;                        // keyframed: physics never drives the entity
            if (newBody) {
                *(void**)((char*)newBody + 0x0C) = rbc;        // back-pointer (m_userData)
                L("[ShipPhys] ghost body constructed %p (src %p) vt=%08X\n", newBody, srcBody, *(uint32_t*)newBody);
                // keep ENABLED (m_DriveEntity=false already) — testing whether the Havok-pool alloc fixed the freeze
            } else {
                rbc->SetEnabled(false);                        // fallback: no physics
                L("[ShipPhys] ghost body construction FAILED\n");
            }
        }
        void* aiComp = readable((char*)dup + 0xE8, 4) ? *(void**)((char*)dup + 0xE8) : nullptr;
        if (aiComp && readable(aiComp, 4)) {
            ((Component*)aiComp)->SetEnabled(false);
        }

        // 4) Add to the world at the SOURCE ship's position (water level); the override drives it from there.
        __try {
            world->AddEntity((Entity*)dup);
            HookMath::Transform* sm = &((BaseEntity*)shipEnt)->m_GlobalMatrix;
            HookMath::Vector4 gpos;
            gpos.x = sm->M41; gpos.y = sm->M42; gpos.z = sm->M43; gpos.w = 0.0f;
            ((Entity*)dup)->SetPosition(gpos);
            return dup;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Spawn the peer's ship ghost from the LOCAL player's ship (the Jackdaw).
static void* SpawnGhostShip() {
    __try {
        World* world = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;
        Human* h = player ? AIEntityHelper::GetHuman(player) : nullptr;
        if (!h) return nullptr;
        void* shipEnt = handleResolve(h->m_IsOnShipEntity);
        if (!shipEnt) shipEnt = handleResolve(h->m_Data.m_Vehicle);
        return DuplicateShip(shipEnt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void Phase1::TryShipDupProbe() {
    if (g_ghostShip) { L("[ShipDup] ghost already spawned (%p) — press F11 to remove\n", g_ghostShip); return; }
    // Route A investigation: dump the SOURCE ship's Havok rigid body (vtable + first 0x100 bytes) to
    // locate hkpRigidBody::clone + hkpWorld. Runs on F12 (before the spawn). Observation only.
    __try {
        World* w = World::GetCurrentWorld();
        Entity* pl = w ? w->GetPlayer() : nullptr;
        L("[Target] w=%p pl=%p\n", w, pl);
        // TargetProbe: the player has no VisualTargetingComponent; the NPCs (guards) do. Enumerate the
        // entity manager and dump each entity's VTC to find the FocusFlags field + bit values.
        ClassDescriptor* vtcDesc = *(ClassDescriptor**)(base() + 0x23E3674);  // VisualTargetingComponent descriptor (via GetStaticClassDescriptor)
        if (pl) {
            Component* c = pl->GetComponentByType(vtcDesc);
            L("[Target] player=%p VTC=%p\n", pl, c);
        }
        if (w) {
            int shown = 0;
            BaseBigArray<Reference>& ents = w->m_EntityManager.m_Entities;
            int nobj = ents.GetSize();
            L("[Target] entities=%d\n", nobj);
            // Dump each entity's component VTABLE pointers (m_Components data @ +0x60, size @ +0x64).
            // Map the vtable addrs back to class names via vt_class_map to find the targeting component.
            for (int i = 0; i < nobj && shown < 16; ++i) {
                Reference* ref = ents.GetElement(i);
                if (!ref || !readable(ref, 4)) break;
                HandleContainer* hc = ref->m_HandleContainer;
                if (!hc || !readable(hc, 4)) continue;
                Entity* e = (Entity*)hc->m_Object;
                if (!e || !readable(e, 4)) continue;
                Component** data = *(Component***)((char*)e + 0x60);
                unsigned short szLo = *(unsigned short*)((char*)e + 0x64);
                unsigned short szHi = *(unsigned short*)((char*)e + 0x66);
                int count = (szLo & 0xFFFF);
                if (count <= 0 || count > 96) count = (szHi & 0xFFFF);
                if (!data || !readable(data, 4) || count <= 0 || count > 96) continue;
                // Character signature: SkeletonComponent / CharacterController (skip props which only have RigidBody)
                bool isChar = false;
                for (int k = 0; k < count; ++k) {
                    Component* c = data[k];
                    uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                    if (vt == 0x01E41E58 || vt == 0x01E6EE10) { isChar = true; break; }
                }
                if (!isChar) continue;
                L("[Target] ent[%d]=%p evt=%08X nComp=%d:", i, e, *(uint32_t*)e, count);
                for (int k = 0; k < count && k < 32; ++k) {
                    Component* c = data[k];
                    uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                    L(" %08X", vt);
                }
                L("\n");
                ++shown;
            }
            // Dump the first crowd NPC's behavior + eagle-vision component memory (FocusFlags holder).
            for (int i = 0; i < nobj; ++i) {
                Reference* ref = ents.GetElement(i);
                if (!ref || !readable(ref, 4)) break;
                HandleContainer* hc = ref->m_HandleContainer;
                if (!hc || !readable(hc, 4)) continue;
                Entity* e = (Entity*)hc->m_Object;
                if (!e || !readable(e, 4)) continue;
                Component** data = *(Component***)((char*)e + 0x60);
                int count = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
                if (count <= 0 || count > 96) count = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
                if (!data || !readable(data, 4) || count <= 0 || count > 96) continue;
                Component* bhv = 0, * eagle = 0;
                for (int k = 0; k < count; ++k) {
                    Component* c = data[k];
                    uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                    if (vt == 0x026E34D8) bhv = c;
                    else if (vt == 0x026EE050) eagle = c;
                }
                if (!bhv && !eagle) continue;
                L("[Crowd] ent[%d]=%p bhv=%p eagle=%p\n", i, e, bhv, eagle);
                if (bhv && readable(bhv, 0x200)) {
                    unsigned char* p = (unsigned char*)bhv;
                    for (int off = 0; off < 0x200; off += 16) {
                        L("[bhv+%03X] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                          off, p[off],p[off+1],p[off+2],p[off+3],p[off+4],p[off+5],p[off+6],p[off+7],
                          p[off+8],p[off+9],p[off+10],p[off+11],p[off+12],p[off+13],p[off+14],p[off+15]);
                    }
                }
                if (eagle && readable(eagle, 0x100)) {
                    unsigned char* p = (unsigned char*)eagle;
                    for (int off = 0; off < 0x100; off += 16) {
                        L("[eagle+%03X] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                          off, p[off],p[off+1],p[off+2],p[off+3],p[off+4],p[off+5],p[off+6],p[off+7],
                          p[off+8],p[off+9],p[off+10],p[off+11],p[off+12],p[off+13],p[off+14],p[off+15]);
                    }
                }
                break;
            }
            // FocusFlags probe: dump the puppet's components + behavior memory (F12).
            // FocusFlags (VERIFIED reflection): IsTargetable=0x1, IsFightable=0x10, IsMeleeTargetable=0x40.
            if (g_puppet) {
                Entity* pe = g_puppet;
                Component** pdata = *(Component***)((char*)pe + 0x60);
                int pcount = (*(unsigned short*)((char*)pe + 0x64) & 0xFFFF);
                if (pcount <= 0 || pcount > 96) pcount = (*(unsigned short*)((char*)pe + 0x66) & 0xFFFF);
                L("[FF] puppet=%p nComp=%d:", pe, pcount);
                Component* bhv = 0;
                if (pdata && readable(pdata, 4) && pcount > 0 && pcount <= 96) {
                    for (int k = 0; k < pcount; ++k) {
                        Component* c = pdata[k];
                        uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                        L(" %08X", vt);
                        if (vt == 0x026FA898 || vt == 0x026E34D8 || vt == 0x01E46778) bhv = c;  // BhvAssassin/BhvGenericNPC/CharacterBehavior
                    }
                }
                L("\n");
                if (bhv) {
                    unsigned char* p = (unsigned char*)bhv;
                    L("[FF] puppet bhv=%p +660=%08X +30=%08X +80=%08X +C0=%08X +1D0=%08X +1E0=%08X\n", bhv,
                      *(uint32_t*)(p+0x660), *(uint32_t*)(p+0x30), *(uint32_t*)(p+0x80),
                      *(uint32_t*)(p+0xC0), *(uint32_t*)(p+0x1D0), *(uint32_t*)(p+0x1E0));
                    *(uint32_t*)(p+0x660) |= 0x51;   // IsTargetable|IsFightable|IsMeleeTargetable
                    L("[FF] set puppet +660=%08X\n", *(uint32_t*)(p+0x660));
                } else {
                    L("[FF] no known behavior on puppet\n");
                }
                // Compare with the LOCAL player (Edward) — the guards DO attack him.
                if (pl) {
                    Component** pd2 = *(Component***)((char*)pl + 0x60);
                    int pc2 = (*(unsigned short*)((char*)pl + 0x64) & 0xFFFF);
                    if (pc2 <= 0 || pc2 > 96) pc2 = (*(unsigned short*)((char*)pl + 0x66) & 0xFFFF);
                    Component* plBhv = 0;
                    if (pd2 && readable(pd2, 4) && pc2 > 0 && pc2 <= 96) {
                        for (int k = 0; k < pc2; ++k) {
                            Component* c = pd2[k];
                            uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                            if (vt == 0x026FA898) { plBhv = c; break; }
                        }
                    }
                    if (plBhv) {
                        unsigned char* p2 = (unsigned char*)plBhv;
                        L("[FF] player bhv=%p +660=%08X +30=%08X +80=%08X +C0=%08X +1D0=%08X +1E0=%08X\n", plBhv,
                          *(uint32_t*)(p2+0x660), *(uint32_t*)(p2+0x30), *(uint32_t*)(p2+0x80),
                          *(uint32_t*)(p2+0xC0), *(uint32_t*)(p2+0x1D0), *(uint32_t*)(p2+0x1E0));
                    } else {
                        L("[FF] no BhvAssassin on player\n");
                    }
                }
            } else {
                L("[FF] no puppet spawned\n");
            }
        }
        Human* hh = pl ? AIEntityHelper::GetHuman(pl) : nullptr;
        if (hh) {
            void* s = handleResolve(hh->m_IsOnShipEntity);
            if (!s) s = handleResolve(hh->m_Data.m_Vehicle);
            if (s) {
                Entity* se = (Entity*)s;
                Component* rb = se->GetComponentByType(RigidBodyComponent::GetStaticClassDescriptor());
                L("[Phys] ship=%p rbComp=%p\n", s, rb);
                if (rb && readable(rb, 4)) {
                    RigidBodyComponent* rbc = (RigidBodyComponent*)rb;
                    void* hk = rbc->m_RigidBody.m_HKRigidBody;
                    L("[Phys] hkBody=%p driveMode=%d mass=%.1f linDamp=%.1f\n", hk,
                      (int)rbc->m_RigidBody.m_DrivingMode, rbc->m_RigidBody.m_Mass,
                      rbc->m_RigidBody.m_LinearDamping);
                    if (hk && readable(hk, 0x220)) {
                        L("[Phys] hkBody vtable=%08X\n", *(uint32_t*)hk);
                        unsigned char* p = (unsigned char*)hk;
                        for (int off = 0; off < 0x220; off += 16) {
                            L("[Phys] +%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                              off, p[off],p[off+1],p[off+2],p[off+3],p[off+4],p[off+5],p[off+6],p[off+7],
                              p[off+8],p[off+9],p[off+10],p[off+11],p[off+12],p[off+13],p[off+14],p[off+15]);
                        }
                    }
                    // Also dump the RigidBody struct (shape reference + mass + body ptr) — the engine's
                    // own copy of the shape, so we can share it in the cinfo.
                    {
                        unsigned char* rb = (unsigned char*)&rbc->m_RigidBody;
                        L("[Phys] RigidBody struct @ %p (shape ref @ +0x44):\n", rb);
                        for (int off = 0; off < 0x70; off += 16) {
                            L("[Phys] RB+%02X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                              off, rb[off],rb[off+1],rb[off+2],rb[off+3],rb[off+4],rb[off+5],rb[off+6],rb[off+7],
                              rb[off+8],rb[off+9],rb[off+10],rb[off+11],rb[off+12],rb[off+13],rb[off+14],rb[off+15]);
                        }
                    }
                    // ShipPhysicComponent structure probe: physics state (Vector4 groups) + hull-body
                    // arrays (SHIP-RE C8: +0x270/+0x278/+0x2e0/+0x2e8). Observation only.
                    {
                        unsigned char* c = (unsigned char*)rbc;
                        for (int off = 0x160; off < 0x320; off += 16) {
                            L("[ShipPhysC] +%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                              off, c[off],c[off+1],c[off+2],c[off+3],c[off+4],c[off+5],c[off+6],c[off+7],
                              c[off+8],c[off+9],c[off+10],c[off+11],c[off+12],c[off+13],c[off+14],c[off+15]);
                        }
                    }
                    // Hull-body array: +0x2E0 = array (stride 0x60), count @ +0x2E6. Dump each hull body.
                    {
                        void* arr = *(void**)((char*)rbc + 0x2E0);
                        unsigned short cnt = *(unsigned short*)((char*)rbc + 0x2E6);
                        L("[ShipPhysC] hullArray=%p count=%u\n", arr, cnt);
                        if (arr && cnt && readable(arr, cnt * 0x60)) {
                            for (int i = 0; i < (int)cnt && i < 8; ++i) {
                                unsigned char* e = (unsigned char*)arr + i * 0x60;
                                L("[ShipPhysC] hull[%d] @ %p:\n", i, e);
                                for (int off = 0; off < 0x60; off += 16) {
                                    L("[ShipPhysC]   +%02X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                      off, e[off],e[off+1],e[off+2],e[off+3],e[off+4],e[off+5],e[off+6],e[off+7],
                                      e[off+8],e[off+9],e[off+10],e[off+11],e[off+12],e[off+13],e[off+14],e[off+15]);
                                }
                            }
                        }
                    }
                }
                // Sail state probe: ship-ctrl (AIComponent @ +0xE8) + its sub-objects (helm/sail/throttle
                // @ +0x0C/+0x1C/+0x24 per SHIP-RE C9) to locate the sail unfurl field.
                {
                    void* ai = readable((char*)s + 0xE8, 4) ? *(void**)((char*)s + 0xE8) : nullptr;
                    L("[Sail] ship=%p aiComp(+0xE8)=%p\n", s, ai);
                    if (ai && readable(ai, 0x30)) {
                        L("[Sail] aiComp vt=%08X\n", *(uint32_t*)ai);
                        unsigned char* a = (unsigned char*)ai;
                        for (int off = 0; off < 0x30; off += 16) {
                            L("[Sail] ai+%02X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                              off, a[off],a[off+1],a[off+2],a[off+3],a[off+4],a[off+5],a[off+6],a[off+7],
                              a[off+8],a[off+9],a[off+10],a[off+11],a[off+12],a[off+13],a[off+14],a[off+15]);
                        }
                        static const int subOffs[3] = { 0x0C, 0x1C, 0x24 };
                        for (int k = 0; k < 3; ++k) {
                            void* sub = *(void**)((char*)ai + subOffs[k]);
                            L("[Sail] sub@+%02X=%p\n", subOffs[k], sub);
                            if (sub && readable(sub, 0x40)) {
                                unsigned char* b = (unsigned char*)sub;
                                for (int off = 0; off < 0x40; off += 16) {
                                    L("[Sail] sub+%02X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                      off, b[off],b[off+1],b[off+2],b[off+3],b[off+4],b[off+5],b[off+6],b[off+7],
                                      b[off+8],b[off+9],b[off+10],b[off+11],b[off+12],b[off+13],b[off+14],b[off+15]);
                                }
                            }
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Phys] EXCEPTION 0x%08X\n", GetExceptionCode());
    }
    void* dup = SpawnGhostShip();
    if (dup) {
        g_ghostShip = dup;
        g_ghostAuto = false;   // manual F12 spawn: persists until F11 removes it (no auto-despawn)
        L("[ShipDup] spawned ghost ship %p vt=%08X (manual, will track peer)\n", dup, *(uint32_t*)dup);
    } else {
        L("[ShipDup] spawn failed (not aboard?)\n");
    }
}

// Hide the net-path ghost ship when the peer disembarks. We do NOT World::RemoveEntity the clone: its
// removal walks the cloned rigging/AI components' SHARED refcounted sub-arrays -> FUN_005559f0 erase
// crash (verified: AC4BFSP.exe+0x155A22 on disembark; SHIP-RE C11). Move it far away instead and clear
// g_ghostShip so nothing drives it; the engine's streamer reclaims it once the player leaves the area.
void Phase1::DespawnGhostShip() {
    if (!g_ghostShip) return;
    Entity* dead = (Entity*)g_ghostShip;
    g_ghostShip = nullptr;
    g_ghostAuto = false;
    __try {
        HookMath::Transform hideTransform;   // identity rotation, position far away
        hideTransform.M41 = 0.0f; hideTransform.M42 = -100000.0f; hideTransform.M43 = 0.0f;
        dead->SetGlobalMatrixInternal(&hideTransform, false);
        L("[ShipSync] ghost ship hidden (%p) [no RemoveEntity]\n", dead);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[ShipSync] DespawnGhostShip EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

// ---- NPC ship sync (host-authoritative): deep-copy each nearby NPC ship into a visual ghost ----
// Host enumerates the NavalManager ship list [mgr+0x3E8] (handle containers, stride 0xD0, count u16
// @ +0x3EE) and broadcasts each ship's transform. The peer deep-copies its OWN matching ship into a
// ghost (DuplicateShip: clone + disable physics/AI + AddEntity) and drives the ghost's RENDER via the
// SetGlobalMatrixInternal hook — the proven ship-ghost seam (verified on the Jackdaw; NPC ships are the
// same vt 0x01E4A128 EntityGroup class). Character sync is a separate, still-unRE'd effort.
static int   g_entSync = -1;   // ini [Coop] EntitySync (read once)
static int   g_entHost = -1;   // ini [Coop] EntityHost (read once): 1 = authoritative sender
static uint32_t g_entSeq = 0;  // entity-packet sequence

// Render-override targets: ghost entity + the transform to render it at. Filled each tick, consumed by
// the SetGlobalMatrixInternal hook — drives the RENDER (the last write, so no fight with physics).
struct SyncTarget {
    uint32_t ent;
    float    pos[3];
    float    quat[4];
};
static SyncTarget g_syncTargets[MAX_SYNC_ENTITIES];
static int        g_syncCount = 0;

// NavalManager = GetWorldComponentByType(*(void**)0x028b168c). VERIFIED live (descriptor classID AC23FDEE).
static void* GetNavalManager() {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world) return nullptr;
        ClassDescriptor* navalDesc = *(ClassDescriptor**)(base() + 0x24b168c);
        return world->GetWorldComponentByType(navalDesc);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The i-th ship ENTITY from [mgr+0x3E8] (entry = handle container -> +0x00 = entity).
static Entity* GetShipEntity(void* mgr, int i) {
    void* list = *(void**)((char*)mgr + 0x3E8);
    if (!list || !readable((char*)list + i * 0xD0, 4)) return nullptr;
    void* hc = *(void**)((char*)list + i * 0xD0);
    if (!hc || !readable(hc, 4)) return nullptr;
    Entity* ent = (Entity*)*(void**)hc;   // HandleContainer->m_Object
    if (!ent || !readable(ent, 4)) return nullptr;
    return ent;
}

// The LOCAL player's ship (the Jackdaw), resolved the same way SpawnGhostShip does. Skipped from the
// NPC-ship sync because the Jackdaw is already synced via g_ghostShip.
static Entity* GetLocalShip() {
    __try {
        World* world = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;
        Human* h = player ? AIEntityHelper::GetHuman(player) : nullptr;
        if (!h) return nullptr;
        void* shipEnt = handleResolve(h->m_IsOnShipEntity);
        if (!shipEnt) shipEnt = handleResolve(h->m_Data.m_Vehicle);
        return (Entity*)shipEnt;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void fillShipState(EntityState& es, Entity* ent, uint32_t key) {
    es.key = key; es.kind = 1;
    HookMath::Transform* m = &ent->m_GlobalMatrix;
    es.pos[0] = m->M41; es.pos[1] = m->M42; es.pos[2] = m->M43;
    HookMath::Quaternion q; HookMath::FromMatrix(m, &q);
    es.quat[0] = q.x; es.quat[1] = q.y; es.quat[2] = q.z; es.quat[3] = q.w;
    // Hull HP (host-authoritative ship damage): *(ship+0x5C) -> +0x84.
    es.hullHP = 0;
    void* stat = readable((char*)ent + 0x5C, 4) ? *(void**)((char*)ent + 0x5C) : nullptr;
    if (stat && readable((char*)stat + 0x84, 4)) es.hullHP = *(int*)((char*)stat + 0x84);
}

static void CaptureShips(EntityPacket& pkt) {
    pkt.magic = NET_ENTITY_MAGIC; pkt.protoVer = NET_PROTO_VER; pkt.count = 0;
    void* mgr = GetNavalManager();
    if (!mgr || !readable((char*)mgr + 0x3E8, 8)) return;
    unsigned short n = *(unsigned short*)((char*)mgr + 0x3EE);
    Entity* localShip = GetLocalShip();
    uint32_t count = 0;
    __try {
        for (int i = 0; i < (int)n && count < (uint32_t)MAX_SYNC_ENTITIES; ++i) {
            Entity* ent = GetShipEntity(mgr, i);
            if (!ent || *(uint32_t*)ent != 0x01E4A128u) continue;
            if (ent == localShip) continue;   // skip the Jackdaw (synced via g_ghostShip)
            fillShipState(pkt.ents[count], ent, (uint32_t)i);
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; }
    pkt.count = count;
    pkt.seq = ++g_entSeq;
    static bool logged = false;
    if (!logged) { logged = true; L("[ShipSync] ship list count=%u captured=%u\n", (unsigned)n, count); }
}

// Option 3: mirror the ORIGINAL ship's render (no deep-copy). For each received ship, drive the local
// matching ship's RENDER matrix to the host's transform via the SetGlobalMatrixInternal hook. The ship's
// own physics/AI keeps running (its logic position stays local), but the visual follows the host.
static void ApplyShips(const EntityPacket& pkt) {
    void* mgr = GetNavalManager();
    if (!mgr || !readable((char*)mgr + 0x3E8, 8)) return;
    Entity* player = World::GetCurrentWorld() ? World::GetCurrentWorld()->GetPlayer() : nullptr;
    int count = 0;
    __try {
        for (uint32_t i = 0; i < pkt.count && i < (uint32_t)MAX_SYNC_ENTITIES; ++i) {
            const EntityState& es = pkt.ents[i];
            if (es.kind != 1) continue;
            Entity* ent = GetShipEntity(mgr, (int)es.key);   // the ORIGINAL ship (no deep-copy)
            if (!ent || !readable(ent, 4)) continue;
            if (ent == player || ent == (Entity*)g_ghostShip) continue;
            SyncTarget& t = g_syncTargets[count];
            t.ent = (uint32_t)ent;
            t.pos[0] = es.pos[0]; t.pos[1] = es.pos[1]; t.pos[2] = es.pos[2];
            t.quat[0] = es.quat[0]; t.quat[1] = es.quat[1]; t.quat[2] = es.quat[2]; t.quat[3] = es.quat[3];
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_syncCount = count;
}

// ---- guard/character sync (G1) -------------------------------------------------
// Capture the nearby characters (Skeleton + BhvGenericNPC/BhvAssassin) as kind=2, appended after the
// ships. The peer matches each by NEAREST POSITION (deterministic spawns) and drives the render.
// ---- P0: stable identity + health cache ---------------------------------------------------------
// GetEntityAI (0x6F5F70) is `*(e+0xE8) ? *(*(e+0xE8)+0x1C) : 0`. Once +0xE8 is cleared (a dead NPC) the
// AI — and therefore the health — becomes unreachable, which is why es.dead never propagated. Cache
// entity -> health while it is alive and keep using the cached service afterwards.
static unsigned int FindHealthInAI(unsigned int ai);          // defined later

// Engine's authoritative combat is-alive (sub_133F550). Returns false when the entity is dead by either
// term: ai+0xB8 & 0xC == 4, or bit 0x1000 on the ai+0x5C service with class id 0x5EB03CEC.
static bool EngineIsAlive(unsigned int ent) {
    if (!ent) return true;
    return ((bool (__cdecl*)(int))0x133F550)((int)ent);
}

struct EntHealth { unsigned int ent; unsigned int hp; };
static EntHealth s_entHealth[256];
static int s_entHealthN = 0;

static unsigned int CachedHealthForEntity(unsigned int ent) {
    for (int i = 0; i < s_entHealthN; ++i) if (s_entHealth[i].ent == ent) return s_entHealth[i].hp;
    return 0;
}
static void RememberEntityHealth(unsigned int ent, unsigned int hp) {
    for (int i = 0; i < s_entHealthN; ++i)
        if (s_entHealth[i].ent == ent) { s_entHealth[i].hp = hp; return; }
    if (s_entHealthN < 256) {
        s_entHealth[s_entHealthN].ent = ent;
        s_entHealth[s_entHealthN].hp  = hp;
        ++s_entHealthN;
    }
}

// Host-side object table for the current capture: objId -> entity + health service. The peer echoes the
// objId back, so the host never has to guess which guard a hit belongs to.
struct ObjEntry { uint64_t objId; unsigned int ent; unsigned int hp; };
static ObjEntry s_objMap[256];
static int s_objMapN = 0;

static void ObjMapReset() { s_objMapN = 0; }
static void ObjMapAdd(uint64_t objId, unsigned int ent, unsigned int hp) {
    if (!objId || s_objMapN >= 256) return;
    for (int i = 0; i < s_objMapN; ++i)
        if (s_objMap[i].objId == objId) { s_objMap[i].ent = ent; s_objMap[i].hp = hp; return; }
    s_objMap[s_objMapN].objId = objId;
    s_objMap[s_objMapN].ent   = ent;
    s_objMap[s_objMapN].hp    = hp;
    ++s_objMapN;
}
static bool ObjMapFind(uint64_t objId, unsigned int* ent, unsigned int* hp) {
    for (int i = 0; i < s_objMapN; ++i)
        if (s_objMap[i].objId == objId) {
            if (ent) *ent = s_objMap[i].ent;
            if (hp)  *hp  = s_objMap[i].hp;
            return true;
        }
    return false;
}

// ---- host -> guest death lane -------------------------------------------------------------------
// A killed NPC drops out of m_EntityManager.m_Entities, so a dead=1 snapshot entry is usually never
// produced and the peer's mirror stays alive. The host therefore watches each soldier's Life and
// reports the 0xFFFF transition through the same kind=3 lane the guest already uses (ApplyPeerHealth
// handles life==0xFFFF on either side).
static const uint8_t KIND_KILL = 3;

struct HostLifeWatch { uint64_t objId; unsigned short life; unsigned int hpSvc; unsigned int gen; bool vanishedLogged; };
static HostLifeWatch s_hostLife[256];
static int  s_hostLifeN = 0;
static unsigned int s_hostLifeGen = 0;

static HostLifeWatch* HostLifeFor(uint64_t objId) {
    for (int i = 0; i < s_hostLifeN; ++i) if (s_hostLife[i].objId == objId) return &s_hostLife[i];
    if (s_hostLifeN >= 256) return nullptr;
    HostLifeWatch* w = &s_hostLife[s_hostLifeN++];
    w->objId = objId; w->life = 0; w->hpSvc = 0; w->gen = 0; w->vanishedLogged = false;
    return w;
}

static void EmitHostDeath(EntityPacket& pkt, uint32_t& count, uint64_t objId, const float* pos) {
    if (!objId || count >= (uint32_t)MAX_SYNC_ENTITIES) return;
    EntityState& es = pkt.ents[count];
    memset(&es, 0, sizeof(es));
    es.kind = KIND_KILL;
    es.objId = objId;
    es.hp = 0xFFFF;
    if (pos) { es.pos[0] = pos[0]; es.pos[1] = pos[1]; es.pos[2] = pos[2]; }
    ++count;
    L("[Relay] host DEATH objId=%llu -> peer\n", (unsigned long long)objId);
}


// Guest-side map: host-assigned objId -> local mirror entity, filled from the snapshots.
static ObjEntry s_guestObj[256];
static int s_guestObjN = 0;
static void GuestObjAdd(uint64_t objId, unsigned int ent) {
    if (!objId || s_guestObjN >= 256) return;
    for (int i = 0; i < s_guestObjN; ++i)
        if (s_guestObj[i].objId == objId) { s_guestObj[i].ent = ent; return; }
    s_guestObj[s_guestObjN].objId = objId;
    s_guestObj[s_guestObjN].ent   = ent;
    s_guestObj[s_guestObjN].hp    = 0;
    ++s_guestObjN;
}
static unsigned int GuestObjFind(uint64_t objId) {
    for (int i = 0; i < s_guestObjN; ++i) if (s_guestObj[i].objId == objId) return s_guestObj[i].ent;
    return 0;
}

// Entities whose health came FROM the host. Never report those back (prevents an event ping-pong).
static unsigned int s_peerApplied[64];
static int s_peerAppliedN = 0;
static bool PeerApplied(unsigned int e) {
    for (int i = 0; i < s_peerAppliedN; ++i) if (s_peerApplied[i] == e) return true;
    return false;
}
static void MarkPeerApplied(unsigned int e) {
    if (PeerApplied(e)) return;
    if (s_peerAppliedN < 64) s_peerApplied[s_peerAppliedN++] = e;
}

// Soldier archetypes, measured on the host (bhv+0x14 & 0xFFFF, all on behaviour vtable 0x026E34D8).
// 0x35F7 was the only one accepted before; the other three are the gunners/agiles/heavies/captains the
// tester reported as not synced. Values are ids, not offsets - 0x0000/0x0400/0x0404 also showed up but
// look like pointer low words rather than archetypes, so they stay out until proven otherwise.
static bool IsSoldierArchetype(uint32_t id) {
    switch (id) {
    case 0x35F7:   // normal soldier
    case 0x31EC:
    case 0x318C:
    case 0x3594:
        return true;
    default:
        return false;
    }
}

static void CaptureGuards(EntityPacket& pkt) {
    ObjMapReset();                       // rebuild the objId table for this capture
    World* world = World::GetCurrentWorld();
    if (!world) return;
    Entity* player = world->GetPlayer();
    float px=0,py=0,pz=0;
    if (player) { px=player->m_GlobalMatrix.M41; py=player->m_GlobalMatrix.M42; pz=player->m_GlobalMatrix.M43; }
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    uint32_t count = pkt.count;   // append after the ships (kind=1)
    __try {
        for (int i = 0; i < nobj && count < (uint32_t)MAX_SYNC_ENTITIES; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4)) continue;
            if (e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel=false; Component* bhvC=0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c,4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;   // BhvGenericNPC only (excludes BhvAssassin player/puppet)
            }
            if (!skel || !bhvC) continue;
            // Archetype probe: every distinct (behaviour vtable, +0x14 id) seen, so the filter below can be
            // widened from data instead of guesswork. Logs once per combination per session.
            {
                static uint32_t s_archVt[16]; static uint32_t s_archId[16]; static int s_archN = 0;
                uint32_t avt = *(uint32_t*)bhvC;
                uint32_t aid = *(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF;
                bool seen = false;
                for (int a = 0; a < s_archN; ++a) if (s_archVt[a] == avt && s_archId[a] == aid) { seen = true; break; }
                if (!seen && s_archN < 16) {
                    s_archVt[s_archN] = avt; s_archId[s_archN] = aid; ++s_archN;
                    unsigned int ahp = FindHealthInAI((unsigned int)AIEntityHelper::GetEntityAI(e));
                    unsigned int alife = (ahp && readable((void*)ahp, 0x70)) ? *(unsigned short*)(ahp + 0x5C) : 0;
                    L("[Arch] bhvVt=%08X id14=%04X life=%u accepted=%d\n",
                      avt, aid, alife, IsSoldierArchetype(aid) ? 1 : 0);
                }
            }
            if (!IsSoldierArchetype(*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF)) continue;
            float x=e->m_GlobalMatrix.M41, y=e->m_GlobalMatrix.M42, z=e->m_GlobalMatrix.M43;
            float dx=x-px, dy=y-py, dz=z-pz;
            if (dx*dx+dy*dy+dz*dz > 80.0f*80.0f) continue;   // only nearby characters
            EntityState& es = pkt.ents[count];
            es.key = (uint32_t)i;
            es.kind = 2;    // character (guard)
            es.pos[0]=x; es.pos[1]=y; es.pos[2]=z;
            HookMath::Quaternion q; HookMath::FromMatrix(&e->m_GlobalMatrix, &q);
            es.quat[0]=q.x; es.quat[1]=q.y; es.quat[2]=q.z; es.quat[3]=q.w;
            // Death state. Verified: the engine's own is-dead predicate is sub_753330 ->
            // *(BYTE*)(ai+0x8C), and a real death flips exactly that byte 0 -> 1 (field diff: AI+08C).
            // The old +0xB8 test is wrong — a genuine death never touches +0xB8, so es.dead stayed 0
            // and host kills were never replicated to the peer.
            AbstractEntityAI* ai = AIEntityHelper::GetEntityAI(e);
            // P0: identity + health that survive the AI being torn down after death.
            unsigned int hpSvc = FindHealthInAI((unsigned int)ai);
            if (hpSvc) RememberEntityHealth((unsigned int)e, hpSvc);
            else       hpSvc = CachedHealthForEntity((unsigned int)e);
            es.objId = hc->GetObjectID();
            if (hpSvc && readable((void*)hpSvc, 0x70)) {
                es.hp = *(unsigned short*)(hpSvc + 0x5C);
                // Dead = the engine's own answer. Primary: the health dead flag (set by the engine on a
                // real death); secondary: combat is-alive. Do NOT use ai+0x8C here — that is a different
                // (AI-side) predicate, not the combat one.
                bool flagsDead = (*(unsigned int*)(hpSvc + 0x60) & 0x01000000u) != 0;
                es.dead = (flagsDead || !EngineIsAlive((unsigned int)e)) ? 1 : 0;
            } else {
                es.hp = 0;
                es.dead = EngineIsAlive((unsigned int)e) ? 0 : 1;
            }
            ObjMapAdd(es.objId, (unsigned int)e, hpSvc);
            // Host -> guest death lane: remember Life and flag the 0xFFFF transition.
            uint64_t deathId = 0;
            if (es.objId) {
                HostLifeWatch* w = HostLifeFor(es.objId);
                if (w) {
                    w->gen = s_hostLifeGen;
                    if (!w->hpSvc && hpSvc) w->hpSvc = hpSvc;
                    if (w->life != 0xFFFFu && w->life != 0 && es.hp == 0xFFFFu) { deathId = es.objId; w->life = 0xFFFFu; }
                    else if (w->life != 0xFFFFu) w->life = es.hp;
                }
            }
            if (es.dead) {
                static void* s_capDead[64];
                static int   s_capDeadN = 0;
                bool seen = false;
                for (int q = 0; q < s_capDeadN; ++q) if (s_capDead[q] == e) { seen = true; break; }
                if (!seen && s_capDeadN < 64) {
                    s_capDead[s_capDeadN++] = e;
                    L("[Cap] ent=%08X dead=1 ai=%08X (captured into the snapshot)\n", e, ai);
                }
            }
            // Alertness: HumanGroundStage (Fight=4=aggro, Navigation=1=calm).
            es.stage = 0;
            es.actionAlias = 0;
            Human* gh = AIEntityHelper::GetHuman(e);
            if (gh && readable(gh, 4)) {
                HumanDataBundle* gpB = gh->m_Data.m_HumanDataBundle;
                if (gpB && readable(gpB, 4)) es.stage = (uint8_t)(int32_t)gpB->m_GroundData.m_HumanGroundStage;
                // Current action clip (file alias) — for the mirror drive on the peer.
                ActionComponent* ac = gh->GetActionComponent();
                if (ac && readable(ac, 0x40)) {
                    unsigned char mirrored = 0;
                    void* act = ac->GetCurrentAction(&mirrored, 0, ACTLAYER_PRIMARY);
                    if (act && readable(act, 0x14)) es.actionAlias = *(uint32_t*)((char*)act + 8);
                }
            }
            ++count;
            if (deathId) EmitHostDeath(pkt, count, deathId, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Anything we tracked last capture but did not see now either walked out of range or died. Only the
    // cached health's dead flag decides; a guard that merely left has it clear and is left alone.
    for (int i = 0; i < s_hostLifeN; ++i) {
        HostLifeWatch& w = s_hostLife[i];
        if (w.gen == s_hostLifeGen || w.life == 0xFFFFu || !w.hpSvc) continue;
        bool dead = false;
        unsigned short life = w.life;
        __try {
            dead = (*(unsigned int*)(w.hpSvc + 0x60) & 0x01000000u) != 0;
            life = *(unsigned short*)(w.hpSvc + 0x5C);      // the service can outlive the entity list entry
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { dead = false; }
        if (!w.vanishedLogged) {
            w.vanishedLogged = true;
            L("[Relay] objId=%llu vanished dead=%d life=%u\n",
              (unsigned long long)w.objId, dead ? 1 : 0, (unsigned)life);
        }
        if (life == 0xFFFFu || dead) {
            w.life = 0xFFFFu;
            EmitHostDeath(pkt, count, w.objId, nullptr);
        }
    }
    ++s_hostLifeGen;

    pkt.count = count;
}

// ---- Kill relay (guest -> host) ----------------------------------------------------------------
// The peer's mirrors die natively (verified live), so the guest only has to notice and report it.
// Kills travel as EntityPacket entries with kind == 3; the host resolves them by position (the same
// 4 m tolerance ApplyGuards already uses) and runs the verified kill recipe on the matching soldier.
// KIND_KILL is declared above, next to the host-side death watch that also emits it.

// Defined below with the mirror bookkeeping; needed here for the local-kill marking.
static void MarkMirrorKilledLocally(void* e);
static bool MirrorKilledLocally(void* e);

static unsigned int FindHealthInAI(unsigned int ai);     // defined further below
// P0/P1: peer -> host health event. objId is the host-assigned identity; position is a fallback only.
static void ApplyPeerHealth(uint64_t objId, unsigned short life, float x, float y, float z);  // below
static bool SelfTestModeActive();                        // defined below
static void KillGuardEntity(Entity* e);                  // defined below

// Re-entrancy guard shared by both event lanes (self-test loopback must not ping-pong).
static bool s_inEventLoop = false;

static void ProcessKillEvents(const EntityPacket& pkt) {
    for (uint32_t i = 0; i < pkt.count && i < (uint32_t)MAX_SYNC_ENTITIES; ++i) {
        if (pkt.ents[i].kind != KIND_KILL) continue;
        ApplyPeerHealth(pkt.ents[i].objId, pkt.ents[i].hp,
                        pkt.ents[i].pos[0], pkt.ents[i].pos[1], pkt.ents[i].pos[2]);
    }
}

// Guest side: remember each mirror's last Life and report the 0xFFFF transition exactly once.
struct MirrorLifeWatch { unsigned int ent; unsigned short life; };
static MirrorLifeWatch s_mirrLife[64];
static int s_mirrLifeN = 0;

static void TrackMirrorDeathForRelay(Entity* e, const EntityState& es) {
    if (!e || es.dead || PeerApplied((unsigned int)e)) return;   // never echo host-originated health
    // Use the cached health: after a kill GetEntityAI can go blind (entity+0xE8 cleared), and then a
    // fresh FindHealthInAI would return 0 and the event would be lost.
    unsigned int ai = (unsigned int)AIEntityHelper::GetEntityAI(e);
    unsigned int hp = FindHealthInAI(ai);
    if (hp) RememberEntityHealth((unsigned int)e, hp);
    else    hp = CachedHealthForEntity((unsigned int)e);
    if (!hp || !readable((void*)hp, 0x70)) return;
    unsigned short life = *(unsigned short*)(hp + 0x5C);

    int slot = -1;
    for (int i = 0; i < s_mirrLifeN; ++i) if (s_mirrLife[i].ent == (unsigned int)e) { slot = i; break; }
    if (slot < 0) {
        if (s_mirrLifeN >= 64) return;
        slot = s_mirrLifeN++;
        s_mirrLife[slot].ent = (unsigned int)e;
        s_mirrLife[slot].life = life;
        return;                          // baseline only; no event for the initial state
    }

    unsigned short prev = s_mirrLife[slot].life;
    if (life == prev) return;
    s_mirrLife[slot].life = life;
    if (life == 0xFFFFu) MarkMirrorKilledLocally(e);   // stop driving the corpse at once

    // P0/P1: report ANY life change (damage as well as death) keyed by the host's objId.
    EntityPacket out;
    memset(&out, 0, sizeof(out));
    out.magic = NET_ENTITY_MAGIC;
    out.protoVer = NET_PROTO_VER;
    out.seq = GetTickCount();
    out.count = 1;
    out.ents[0].kind = KIND_KILL;             // "health update" lane
    out.ents[0].objId = es.objId;
    out.ents[0].hp = life;
    out.ents[0].pos[0] = es.pos[0];
    out.ents[0].pos[1] = es.pos[1];
    out.ents[0].pos[2] = es.pos[2];
    L("[Relay] mirror ent=%08X objId=%llu life %u -> %u -> host\n",
      e, (unsigned long long)es.objId, (unsigned)prev, (unsigned)life);
    Net::SendEntities(out);

    // Self-test: no peer exists, so deliver it to the local apply path (what the host would do).
    if (SelfTestModeActive() && !s_inEventLoop) {
        s_inEventLoop = true;
        L("[Relay] SELF-TEST loopback: delivering to the host path\n");
        ProcessKillEvents(out);
        s_inEventLoop = false;
    }
}

// True when EntitySelfTest is on. In self-test the capture source and the mirror are the same world, so
// writing the transform back is redundant — and snapping a stationary guard ~10x/s is exactly what reads
// as visible jitter. Self-test therefore keeps the AI disable but skips the transform write.
static bool SelfTestModeActive() {
    static int s = -1;
    if (s < 0) s = GetPrivateProfileIntA("Coop", "EntitySelfTest", 0, ".\\AC4Coop.ini");
    return s != 0;
}

// Mirror bookkeeping: one-time setup per entity, and clip-change detection so we don't re-issue the
// same action every frame (that reads as flicker).
struct DriveEnt { Entity* e; float pos[3]; float quat[4]; unsigned int alias; int dead; };
static DriveEnt s_drive[MAX_SYNC_ENTITIES];
static int      s_driveN = 0;
static void* s_mirroredEnt[96];
static int   s_mirroredN = 0;
struct MirrorAction { unsigned int ent; unsigned int alias; };
static MirrorAction s_mirrorAct[96];
static int s_mirrorActN = 0;

static bool MirrorFirstSight(void* e) {
    for (int i = 0; i < s_mirroredN; ++i) if (s_mirroredEnt[i] == e) return false;
    if (s_mirroredN < 96) s_mirroredEnt[s_mirroredN++] = e;
    return true;
}

static bool MirrorAliasChanged(void* e, unsigned int alias) {
    for (int i = 0; i < s_mirrorActN; ++i)
        if (s_mirrorAct[i].ent == (unsigned int)e) {
            if (s_mirrorAct[i].alias == alias) return false;
            s_mirrorAct[i].alias = alias;
            return true;
        }
    if (s_mirrorActN < 96) {
        s_mirrorAct[s_mirrorActN].ent = (unsigned int)e;
        s_mirrorAct[s_mirrorActN].alias = alias;
        ++s_mirrorActN;
    }
    return true;
}

// Mirrors THIS peer killed. The host is still reporting them alive (its snapshot predates the kill or
// the kill hasn't arrived), so es.dead alone is not enough: without this we would keep driving the
// corpse's transform/clip from the host snapshot and it would stand back up or slide around.
static void* s_mirrorDeadLocal[96];
static int   s_mirrorDeadLocalN = 0;

static bool MirrorKilledLocally(void* e) {
    for (int i = 0; i < s_mirrorDeadLocalN; ++i) if (s_mirrorDeadLocal[i] == e) return true;
    return false;
}
static void MarkMirrorKilledLocally(void* e) {
    if (MirrorKilledLocally(e)) return;
    if (s_mirrorDeadLocalN < 96) s_mirrorDeadLocal[s_mirrorDeadLocalN++] = e;
}

static void ApplyGuards(const EntityPacket& pkt) {
    World* world = World::GetCurrentWorld();
    if (!world) return;
    static bool s_selfTestMode = false;   // set by EntitySyncTick; see below
    s_selfTestMode = SelfTestModeActive();
    Entity* player = world->GetPlayer();
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    s_driveN = 0;                       // rebuild the per-frame drive cache
    __try {
        for (uint32_t i = 0; i < pkt.count; ++i) {
            const EntityState& es = pkt.ents[i];
            if (es.kind != 2) continue;
            Entity* best = 0; Component* bestBhv = 0; float bestD = 4.0f * 4.0f;   // 4m match tolerance
            for (int j = 0; j < nobj; ++j) {
                Reference* ref = ents.GetElement(j);
                if (!ref || !readable(ref, 4)) break;
                HandleContainer* hc = ref->m_HandleContainer;
                if (!hc || !readable(hc, 4)) continue;
                Entity* e = (Entity*)hc->m_Object;
                if (!e || !readable(e, 4) || e == player) continue;
                Component** data = *(Component***)((char*)e + 0x60);
                int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
                if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
                if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
                bool skel=false; Component* bhvC=0;
                for (int k = 0; k < n; ++k) {
                    Component* c = data[k];
                    uint32_t vt = (c && readable(c,4)) ? *(uint32_t*)c : 0;
                    if (vt == 0x01E41E58) skel = true;
                    if (vt == 0x026E34D8) bhvC = c;   // BhvGenericNPC only
                }
                if (!skel || !bhvC) continue;
                if (!IsSoldierArchetype(*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF)) continue;
                float dx = e->m_GlobalMatrix.M41 - es.pos[0];
                float dy = e->m_GlobalMatrix.M42 - es.pos[1];
                float dz = e->m_GlobalMatrix.M43 - es.pos[2];
                float d = dx*dx + dy*dy + dz*dz;
                if (d < bestD) { bestD = d; best = e; bestBhv = bhvC; }
            }
            if (!best) continue;
            // Guest relay: if OUR attack is what killed this mirror, report it to the host (which owns
            // the real guard). Skipped when the death came from the host snapshot, so it can't echo.
            GuestObjAdd(es.objId, (unsigned int)best);   // objId -> local mirror, for inbound events
            TrackMirrorDeathForRelay(best, es);
            // Visibility: log each guard the first time it becomes a playback dummy, so a live test can
            // state unambiguously whether the guard being hit was mirrored or a normal AI.
            if (MirrorFirstSight(best))
                L("[Mirror] ent=%08X -> playback dummy pos=(%.1f,%.1f,%.1f) dead=%u\n",
                  best, es.pos[0], es.pos[1], es.pos[2], (unsigned)es.dead);
            // Re-assert EVERY tick. The engine re-enables the behaviour/controller on its own; once it is
            // back the guard fights the drive — which shows up as glitching/popping and a framerate
            // collapse. One-time disabling is NOT sufficient (verified live).
            if (bestBhv && readable(bestBhv, 0x40))
                ((Component*)bestBhv)->SetEnabled(false);
            Human* bh = AIEntityHelper::GetHuman(best);
            if (bh && readable(bh, 4)) {
                CharacterController* cc = (CharacterController*)bh->GetCharacterComponent();
                if (readable(cc, 0x600)) cc->m_DriveEntity = false;
            }
            bool locallyDead = MirrorKilledLocally(best);
            if (es.dead) {
                static void* s_deadSeen[64];
                static int   s_deadSeenN = 0;
                bool seen = false;
                for (int i = 0; i < s_deadSeenN; ++i) if (s_deadSeen[i] == best) { seen = true; break; }
                if (!seen && s_deadSeenN < 64) {
                    s_deadSeen[s_deadSeenN++] = best;
                    L("[Mirror] ent=%08X host reports DEAD (es.dead=1, from ai+0x8C)\n", best);
                }
            }
            if (!es.dead && !locallyDead) {
                HookMath::Quaternion q; q.x=es.quat[0]; q.y=es.quat[1]; q.z=es.quat[2]; q.w=es.quat[3];
                HookMath::Transform m;
                HookMath::ToMatrix(&q, &m);
                m.M41 = es.pos[0]; m.M42 = es.pos[1]; m.M43 = es.pos[2];
                if (!s_selfTestMode) ((Entity*)best)->SetGlobalMatrixInternal(&m, true);
                // Only re-issue the clip when the alias changed.
                if (es.actionAlias && MirrorAliasChanged(best, es.actionAlias)) {
                    if (bh && readable(bh, 4)) {
                        ActionComponent* ac = bh->GetActionComponent();
                        if (ac && readable(ac, 0x40))
                            ac->SetTargetAction((int)es.actionAlias, 0, 0, 0xFFFF, ACTLAYER_PRIMARY);
                    }
                }
            }
            // (4) Death: reproduce the REAL death locally instead of hand-writing AI flags. Once per
            //     entity. The old `ai+0xB8 |= 4` write is removed — it is only one of the two terms of
            //     the engine's is-alive test and left the mirror in a half-dead state.
            if (es.dead && !MirrorKilledLocally(best)) {
                MarkMirrorKilledLocally(best);
                L("[Mirror] ent=%08X host says dead -> applying the real death locally\n", best);
                KillGuardEntity(best);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Per-frame drive, de-duplicated: SetGlobalMatrixInternal(...,true) is a warp (expensive + invalidates
// the render transform), so it must NOT run every frame. Re-apply only when the target actually changed,
// which keeps a static mirror static (no self-inflicted jitter, no cost).
struct AppliedState { unsigned int ent; float pos[3]; float quat[4]; };
static AppliedState s_applied[128];
static int s_appliedN = 0;

static void DriveCachedMirrors() {
    for (int i = 0; i < s_driveN; ++i) {
        DriveEnt& d = s_drive[i];
        if (!d.e || d.dead || !readable(d.e, 4)) continue;      // never puppeteer a corpse
        int k = -1;
        for (int j = 0; j < s_appliedN; ++j) if (s_applied[j].ent == (unsigned int)d.e) { k = j; break; }
        if (k < 0) {
            if (s_appliedN >= 128) continue;
            k = s_appliedN++;
            s_applied[k].ent = (unsigned int)d.e;
            s_applied[k].pos[0] = 1e30f;                         // sentinel: first pass always applies
        }
        AppliedState& a = s_applied[k];
        bool same = a.pos[0] == d.pos[0] && a.pos[1] == d.pos[1] && a.pos[2] == d.pos[2] &&
                    a.quat[0] == d.quat[0] && a.quat[1] == d.quat[1] &&
                    a.quat[2] == d.quat[2] && a.quat[3] == d.quat[3];
        if (same) continue;
        HookMath::Quaternion q; q.x=d.quat[0]; q.y=d.quat[1]; q.z=d.quat[2]; q.w=d.quat[3];
        HookMath::Transform m;
        HookMath::ToMatrix(&q, &m);
        m.M41 = d.pos[0]; m.M42 = d.pos[1]; m.M43 = d.pos[2];
        ((Entity*)d.e)->SetGlobalMatrixInternal(&m, true);
        a.pos[0]=d.pos[0]; a.pos[1]=d.pos[1]; a.pos[2]=d.pos[2];
        a.quat[0]=d.quat[0]; a.quat[1]=d.quat[1]; a.quat[2]=d.quat[2]; a.quat[3]=d.quat[3];
    }
}

// Runs each frame inside OnFrame's net path (main thread). Host captures+broadcasts; peer applies.
static void EntitySyncTick() {
    if (g_entSync < 0) g_entSync = GetPrivateProfileIntA("Coop", "EntitySync", 0, ".\\AC4Coop.ini");
    if (g_entHost < 0) {
        g_entHost = GetPrivateProfileIntA("Coop", "EntityHost", 0, ".\\AC4Coop.ini");
        // Same-PC 2nd instance (LocalPort was taken) = the mirror/guest, regardless of the ini.
        if (Net::IsSecondInstance()) g_entHost = 0;
    }
    if (!g_entSync) return;

    // Gate on a real loaded world: the title/menu screen has m_CharacterList == 1 (just the player).
    World* gateWorld = World::GetCurrentWorld();
    if (!gateWorld || gateWorld->m_CharacterList.GetSize() < 2) return;

    static uint32_t tick = 0;
    if (++tick < 6) return;      // ~10 Hz throttle (60fps / 6)
    tick = 0;

    // Self-test (single machine): clone the local NPC ships into ghosts and drive them at +offset X,
    // so the user sees the duplicate ships (proves the deep-copy + render override with no peer).
    static int s_selfTest = -1;
    if (s_selfTest < 0) s_selfTest = GetPrivateProfileIntA("Coop", "EntitySelfTest", 0, ".\\AC4Coop.ini");
    if (s_selfTest) {
        EntityPacket pkt;
        CaptureShips(pkt);
        CaptureGuards(pkt);   // guards too: verify the mirror pipeline (capture -> drive) in one instance
        static int s_selfMax = -1;
        if (s_selfMax < 0) s_selfMax = GetPrivateProfileIntA("Coop", "EntitySelfTestMax", 1, ".\\AC4Coop.ini");
        if ((int)pkt.count > s_selfMax) pkt.count = (uint32_t)s_selfMax;
        static float s_off = -1e9f;
        if (s_off == -1e9f) s_off = (float)GetPrivateProfileIntA("Coop", "EntitySelfTestOffset", 0, ".\\AC4Coop.ini");
        for (uint32_t i = 0; i < pkt.count; ++i) pkt.ents[i].pos[0] += s_off;
        ApplyShips(pkt);
        ApplyGuards(pkt);   // guards: disable AI + drive to the offset position
        return;
    }

    if (g_entHost) {
        EntityPacket pkt;
        CaptureShips(pkt);
        CaptureGuards(pkt);   // append the characters after the ships
        if (pkt.count) Net::SendEntities(pkt);
        // Peer-reported kills (guest -> host): resolve by position and kill the real guard.
        EntityPacket in;
        if (Net::GetEntities(in) && in.protoVer == NET_PROTO_VER) ProcessKillEvents(in);
    } else {
        EntityPacket pkt;
        if (Net::GetEntities(pkt) && pkt.protoVer == NET_PROTO_VER) {
            ApplyShips(pkt);
            ApplyGuards(pkt);   // apply the characters
            ProcessKillEvents(pkt);   // symmetric; host->guest packets carry no kill entries
        }
    }
}


// ---- entity-enumeration probe: empirically verify the world's entity lists ---------
// Dumps (all readable()-guarded + SEH): NavalManager via GetWorldComponentByType, the ship list
// [mgr+0x3E8], the player ship [mgr+0x1674], and the World header's m_CharacterList / m_ObjectList /
// m_EntityManager.m_Entities counts+vtables. The goal is to CONFIRM the offsets + classification
// against the live game before any sync code is written (no guessing).
static void ProbeEntityEnum() {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world) { L("[Enum] no world\n"); return; }
        Entity* player = world->GetPlayer();
        L("[Enum] world=%p player=%p\n", world, player);

        // 1) Ship manager via the NavalManager class descriptor. Verified: NavalManager vtable 0x02692740
        //    slot[5] GetStaticClassDescriptor = `mov eax,[0x028b168c]` -> descriptor 0x028b6b48 (classID
        //    AC23FDEE, name "NavalManager"). Pass the DESCRIPTOR (not the vtable).
        void* mgr = nullptr;
        __try {
            ClassDescriptor* navalDesc = *(ClassDescriptor**)(base() + 0x24b168c);  // *(void**)0x028b168c
            mgr = world->GetWorldComponentByType(navalDesc);
        } __except (EXCEPTION_EXECUTE_HANDLER) { mgr = nullptr; }
        L("[Enum] NavalManager=%p (via descriptor *(void**)0x028b168c)\n", mgr);

        // 2) Ship list [mgr+0x3E8] (stride 0xD0, count u16 @+0x3EE).
        if (mgr && readable((char*)mgr + 0x3E8, 8)) {
            void* list = *(void**)((char*)mgr + 0x3E8);
            unsigned short count = *(unsigned short*)((char*)mgr + 0x3EE);
            L("[Enum] shipList=%p count=%u\n", list, count);
            int shown = 0;
            for (int i = 0; i < (int)count && shown < 16; ++i) {
                void* slot = (char*)list + i * 0xD0;
                if (!readable(slot, 4)) break;
                void* hc = *(void**)slot;                      // the entry = a HandleContainer*
                if (!hc || !readable(hc, 4)) continue;
                Entity* shipEnt = (Entity*)*(void**)hc;        // HandleContainer->m_Object = the ship entity
                if (!shipEnt || !readable(shipEnt, 4)) continue;
                uint32_t vt = *(uint32_t*)shipEnt;
                float x=0,y=0,z=0;
                if (readable((char*)shipEnt + 0x40, 12)) { x=*(float*)((char*)shipEnt+0x40); y=*(float*)((char*)shipEnt+0x44); z=*(float*)((char*)shipEnt+0x48); }
                L("[Enum]   ship[%d]=%p vt=%08X pos=(%.0f,%.0f,%.0f)\n", i, shipEnt, vt, x, y, z);
                ++shown;
            }
        } else {
            L("[Enum] ship list unreachable (mgr=%p)\n", mgr);
        }

        // 3) Player ship [mgr+0x1674] (Handle; sentinel 0x04DD5F8C = none).
        if (mgr && readable((char*)mgr + 0x1674, 4)) {
            void* hc = *(void**)((char*)mgr + 0x1674);
            L("[Enum] playerShipHandle=%p (sentinel 0x04DD5F8C = none)\n", hc);
        }

        // 4) World header lists (offsets from the reconstructed World.h — verify empirically).
        if (readable(world, 8)) {
            int nc = world->m_CharacterList.GetSize();
            L("[Enum] m_CharacterList size=%d\n", nc);
            int shown = 0;
            for (int i = 0; i < nc && shown < 12; ++i) {
                Entity** slot = world->m_CharacterList.GetElement(i);
                if (!slot || !readable(slot, 4)) break;
                Entity* e = *slot;
                if (!e || !readable(e, 4)) continue;
                uint32_t vt = *(uint32_t*)e;
                float x=0,y=0,z=0;
                if (readable((char*)e + 0x40, 12)) { x=*(float*)((char*)e+0x40); y=*(float*)((char*)e+0x44); z=*(float*)((char*)e+0x48); }
                L("[Enum]   char[%d]=%p vt=%08X pos=(%.0f,%.0f,%.0f)\n", i, e, vt, x, y, z);
                ++shown;
            }
            int no = world->m_ObjectList.GetSize();
            L("[Enum] m_ObjectList size=%d\n", no);
            BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
            int nobj = ents.GetSize();
            L("[Enum] m_EntityManager.m_Entities size=%d\n", nobj);
            int shownE = 0;
            for (int i = 0; i < nobj && shownE < 32; ++i) {
                Reference* ref = ents.GetElement(i);
                if (!ref || !readable(ref, 4)) break;
                HandleContainer* hc = ref->m_HandleContainer;
                if (!hc || !readable(hc, 4)) continue;
                Entity* e = (Entity*)hc->m_Object;
                if (!e || !readable(e, 4)) continue;
                uint32_t vt = *(uint32_t*)e;
                float x=0,y=0,z=0;
                if (readable((char*)e + 0x40, 12)) { x=*(float*)((char*)e+0x40); y=*(float*)((char*)e+0x44); z=*(float*)((char*)e+0x48); }
                L("[Enum]   ent[%d]=%p vt=%08X pos=(%.0f,%.0f,%.0f)\n", i, e, vt, x, y, z);
                ++shownE;
            }
            // Guard enumeration: characters (Skeleton) flagged by WeaponComponent (0x027106A8) or
            // MapIconBehaviorGuard (0x0269E9D0) — the soldiers, vs the unarmed crowd. Logs position so the
            // two machines can be diffed (spawn determinism).
            {
                Entity* pl = world->GetPlayer();
                float px=0,py=0,pz=0;
                if (pl) { px=pl->m_GlobalMatrix.M41; py=pl->m_GlobalMatrix.M42; pz=pl->m_GlobalMatrix.M43; }
                int chars=0, guards=0;
                for (int i = 0; i < nobj && chars < 90; ++i) {
                    Reference* ref = ents.GetElement(i);
                    if (!ref || !readable(ref, 4)) break;
                    HandleContainer* hc = ref->m_HandleContainer;
                    if (!hc || !readable(hc, 4)) continue;
                    Entity* e = (Entity*)hc->m_Object;
                    if (!e || !readable(e, 4)) continue;
                    Component** data = *(Component***)((char*)e + 0x60);
                    int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
                    if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
                    if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
                    bool skel=false, weapon=false, guardIcon=false, cloth=false; uint32_t bhv=0; Component* bhvC=0;
                    for (int k = 0; k < n; ++k) {
                        Component* c = data[k];
                        uint32_t vt = (c && readable(c,4)) ? *(uint32_t*)c : 0;
                        if (vt == 0x01E41E58) skel = true;
                        if (vt == 0x027106A8) weapon = true;
                        if (vt == 0x0269E9D0) guardIcon = true;
                        if (vt == 0x01E61938) cloth = true;
                        if (vt == 0x026E34D8 || vt == 0x026FA898) { bhv = vt; bhvC = c; }
                    }
                    if (!skel || !bhv) continue;
                    uint32_t arch = (bhvC && readable(bhvC, 0x18)) ? *(uint32_t*)((char*)bhvC + 0x14) : 0;
                    float x=e->m_GlobalMatrix.M41, y=e->m_GlobalMatrix.M42, z=e->m_GlobalMatrix.M43;
                    float dx=x-px, dy=y-py, dz=z-pz; float d=sqrtf(dx*dx+dy*dy+dz*dz);
                    if (d > 80.0f) continue;
                    L("[Guard] [%d]=%p bhv=%08X cloth=%d arch=%08X pos=(%.0f,%.0f,%.0f) d=%.0f\n",
                      i, e, bhv, cloth?1:0, arch, x, y, z, d);
                    ++chars; if (weapon || guardIcon) ++guards;
                }
                L("[Guard] nearbyChars=%d guards=%d\n", chars, guards);
            }
            // Fighter dump: characters in a COMBAT ground stage (the aggro'd soldiers). Dump their
            // components + behavior memory to find the real guard discriminator (verified live).
            {
                int fighters = 0;
                for (int i = 0; i < nobj && fighters < 4; ++i) {
                    Reference* ref = ents.GetElement(i);
                    if (!ref || !readable(ref, 4)) break;
                    HandleContainer* hc = ref->m_HandleContainer;
                    if (!hc || !readable(hc, 4)) continue;
                    Entity* e = (Entity*)hc->m_Object;
                    if (!e || !readable(e, 4)) continue;
                    Component** data = *(Component***)((char*)e + 0x60);
                    int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
                    if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
                    if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
                    bool skel=false; Component* bhvC=0;
                    for (int k = 0; k < n; ++k) {
                        Component* c = data[k];
                        uint32_t vt = (c && readable(c,4)) ? *(uint32_t*)c : 0;
                        if (vt == 0x01E41E58) skel = true;
                        if (vt == 0x026E34D8) bhvC = c;   // BhvGenericNPC
                    }
                    if (!skel || !bhvC) continue;
                    Human* h = AIEntityHelper::GetHuman(e);
                    if (!h || !readable(h, 4)) continue;
                    HumanDataBundle* pB = h->m_Data.m_HumanDataBundle;
                    if (!pB || !readable(pB, 4)) continue;
                    int stage = (int32_t)pB->m_GroundData.m_HumanGroundStage;
                    if (stage < 4) continue;   // skip idle/navigation/hurt; combat stages are >= Fight(4)
                    L("[Fight] [%d]=%p stage=%d nComp=%d:", i, e, stage, n);
                    for (int k = 0; k < n && k < 24; ++k) {
                        Component* c = data[k];
                        uint32_t vt = (c && readable(c,4)) ? *(uint32_t*)c : 0;
                        L(" %08X", vt);
                    }
                    L("\n");
                    if (readable(bhvC, 0x80)) {
                        unsigned char* p = (unsigned char*)bhvC;
                        for (int off = 0; off < 0x80; off += 16) {
                            L("[Fight] bhv+%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                              off, p[off],p[off+1],p[off+2],p[off+3],p[off+4],p[off+5],p[off+6],p[off+7],
                              p[off+8],p[off+9],p[off+10],p[off+11],p[off+12],p[off+13],p[off+14],p[off+15]);
                        }
                    }
                    ++fighters;
                }
                L("[Fight] fighters=%d\n", fighters);
            }
            // Health probe: find a soldier + dump its health service (Life@+0x5C) via GetService<CSrvNPCHealth>
            // (FUN_01371140, thiscall with this = the Human). Confirms the health access for the kill relay (G2).
            {
                int shownH = 0;
                for (int i = 0; i < nobj && shownH < 2; ++i) {
                    Reference* ref = ents.GetElement(i);
                    if (!ref || !readable(ref, 4)) break;
                    HandleContainer* hc = ref->m_HandleContainer;
                    if (!hc || !readable(hc, 4)) continue;
                    Entity* e = (Entity*)hc->m_Object;
                    if (!e || !readable(e, 4)) continue;
                    Component** data = *(Component***)((char*)e + 0x60);
                    int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
                    if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
                    if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
                    bool skel=false; Component* bhvC=0;
                    for (int k = 0; k < n; ++k) {
                        Component* c = data[k];
                        uint32_t vt = (c && readable(c,4)) ? *(uint32_t*)c : 0;
                        if (vt == 0x01E41E58) skel = true;
                        if (vt == 0x026E34D8) bhvC = c;
                    }
                    if (!skel || !bhvC) continue;
                    if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
                    Human* h = AIEntityHelper::GetHuman(e);
                    unsigned int ec0 = *(unsigned int*)((char*)e + 0x0C);
                    unsigned int e58 = *(unsigned int*)((char*)e + 0x58);
                    unsigned int e5c = *(unsigned int*)((char*)e + 0x5C);
                    unsigned int e60 = *(unsigned int*)((char*)e + 0x60);
                    unsigned int h5c = h ? *(unsigned int*)((char*)h + 0x5C) : 0;
                    unsigned int h60 = h ? *(unsigned int*)((char*)h + 0x60) : 0;
                    unsigned int h64 = h ? *(unsigned int*)((char*)h + 0x64) : 0;
                    L("[Health] guard=%p human=%p e+0C=%08X e+58=%08X e+5C=%08X e+60=%08X | h+5C=%08X h+60=%08X h+64=%08X\n", e, h, ec0, e58, e5c, e60, h5c, h60, h64);
                    // health service via the sub_7025F0 chain: *(*(entity+0xE8) + 0x1C)
                    unsigned int eE8 = *(unsigned int*)((char*)e + 0xE8);
                    unsigned int svc = (eE8 && readable((void*)eE8, 0x24)) ? *(unsigned int*)((char*)eE8 + 0x1C) : 0;
                    if (svc && readable((void*)svc, 0xC0)) {
                        unsigned int s0c = *(unsigned int*)((char*)svc + 0x0C);
                        unsigned int s58 = *(unsigned int*)((char*)svc + 0x58);
                        L("[Health]   svc=%08X vt=%08X svc+0C=%08X svc+58=%08X\n", svc, *(unsigned int*)svc, s0c, s58);
                        if (s0c && readable((void*)s0c, 0x78)) {
                            void* arr = *(void**)((char*)s0c + 0x70);
                            unsigned short cnt = *(unsigned short*)((char*)s0c + 0x76);
                            L("[Health]   svc container=%08X arr=%p cnt=%u\n", s0c, arr, cnt);
                            for (int m = 0; m < (int)cnt && m < 10; ++m) {
                                void* s = arr && readable((char*)arr + m*4, 4) ? *(void**)((char*)arr + m*4) : 0;
                                uint32_t vt = s && readable(s, 4) ? *(uint32_t*)s : 0;
                                L("[Health]     svc[%d]=%p vt=%08X\n", m, s, vt);
                            }
                        }
                    } else {
                        L("[Health]   e+E8=%08X svc=%08X (unreadable)\n", eE8, svc);
                    }
                    if (h5c && readable((void*)h5c, 0x64))
                        L("[Health]   *(h+5C) vt=%08X life@5C=%d max@5E=%d\n", *(unsigned int*)h5c, *(unsigned short*)((char*)h5c+0x5C), *(unsigned short*)((char*)h5c+0x5E));
                    if (e5c && readable((void*)e5c, 0x64))
                        L("[Health]   *(e+5C) -> life=%d max=%d\n", *(unsigned short*)((char*)e5c+0x5C), *(unsigned short*)((char*)e5c+0x5E));
                    if (ec0 && readable((void*)ec0, 0x78)) {
                        void* arr = *(void**)((char*)ec0 + 0x70);
                        unsigned short cnt = *(unsigned short*)((char*)ec0 + 0x76);
                        L("[Health]   e+0C container=%08X arr=%p cnt=%u\n", ec0, arr, cnt);
                        for (int m = 0; m < (int)cnt && m < 8; ++m) {
                            void* svc = arr && readable((char*)arr + m*4, 4) ? *(void**)((char*)arr + m*4) : 0;
                            uint32_t vt = svc && readable(svc, 4) ? *(uint32_t*)svc : 0;
                            L("[Health]     svc[%d]=%p vt=%08X\n", m, svc, vt);
                        }
                    }
                    // scan the entity's first 0x200 bytes for the AI component (vt 0x01E69D88) = the likely service owner
                    for (int off = 0; off < 0x200; off += 4) {
                        unsigned int p = *(unsigned int*)((char*)e + off);
                        if (p && readable((void*)p, 4) && *(unsigned int*)p == 0x01E69D88u) {
                            L("[Health] AI @ e+%02X = %08X; AI+0C=%08X AI+58=%08X\n", off, p,
                              *(unsigned int*)((char*)p + 0x0C), *(unsigned int*)((char*)p + 0x58));
                            unsigned int c0 = *(unsigned int*)((char*)p + 0x0C);
                            if (c0 && readable((void*)c0, 0x78)) {
                                void* arr = *(void**)((char*)c0 + 0x70);
                                unsigned short cnt = *(unsigned short*)((char*)c0 + 0x76);
                                L("[Health]   AI container=%08X arr=%p cnt=%u\n", c0, arr, cnt);
                                for (int m = 0; m < (int)cnt && m < 8; ++m) {
                                    void* svc = arr && readable((char*)arr + m*4, 4) ? *(void**)((char*)arr + m*4) : 0;
                                    uint32_t vt = svc && readable(svc, 4) ? *(uint32_t*)svc : 0;
                                    L("[Health]     svc[%d]=%p vt=%08X\n", m, svc, vt);
                                }
                            }
                            break;
                        }
                    }
                    ++shownH;
                }
            }
        }
        // 5) Template table: the spawn manager (*(world+0x924)) + its template arrays. FUN_005fac60
        //    walks [mgr+0x94] (count u16 @ +0x9a, 8-byte entries {obj,key}); FUN_00602ae0 iterates
        //    [mgr+0x84] (count u16 @ +0x8a) of template handles. Dump both to enumerate the catalog.
        {
            void* smgr = readable((char*)world + 0x924, 4) ? *(void**)((char*)world + 0x924) : nullptr;
            L("[Tpl] spawnMgr=*(world+0x924)=%p\n", smgr);
            if (smgr && readable((char*)smgr, 0x100)) {
                unsigned short c84 = *(unsigned short*)((char*)smgr + 0x8a);
                void* a84 = *(void**)((char*)smgr + 0x84);
                unsigned short c94 = *(unsigned short*)((char*)smgr + 0x9a);
                void* a94 = *(void**)((char*)smgr + 0x94);
                L("[Tpl] arr84=%p cnt=%u | arr94=%p cnt=%u\n", a84, c84, a94, c94);
                int t = 0;
                for (int i = 0; i < (int)c94 && t < 20; ++i) {
                    char* e = (char*)a94 + i * 8;
                    if (!readable(e, 8)) break;
                    uint32_t obj = *(uint32_t*)(e + 0), key = *(uint32_t*)(e + 4);
                    uint32_t objvt = (obj && readable((void*)obj, 4)) ? *(uint32_t*)obj : 0;
                    L("[Tpl]   slot[%d] obj=%08X vt=%08X key=%08X\n", i, obj, objvt, key);
                    ++t;
                }
            }
            // Population manager (vt 0x1E57830): scan world+0x000..0x2000 for a pointer to it, then
            // dump its +8 array (count u16 @ +0xE) of template ids.
            int popHits = 0;
            for (int off = 0; off < 0x2000 && popHits < 4; off += 4) {
                void* p = *(void**)((char*)world + off);
                if (!p || !readable(p, 4) || *(uint32_t*)p != 0x1E57830u) continue;
                L("[Tpl] popMgr @ world+0x%03X = %p\n", off, p);
                void* arr8 = *(void**)((char*)p + 8);
                unsigned short cnt8 = *(unsigned short*)((char*)p + 0xe);
                L("[Tpl]   arr@+8=%p cnt=%u\n", arr8, cnt8);
                int tt = 0;
                for (int i = 0; i < (int)cnt8 && tt < 16; ++i) {
                    uint32_t id = (arr8 && readable((char*)arr8 + i * 4, 4)) ? *(uint32_t*)((char*)arr8 + i * 4) : 0;
                    L("[Tpl]     [%d]=%08X\n", i, id);
                    ++tt;
                }
                ++popHits;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Enum] EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

// ---- main-thread execution via a per-frame inline hook -----------------------------
static volatile long g_spawnReq = 0;     // set by hotkey (any thread), consumed on the main thread
static volatile long g_dumpReq  = 0;     // set by hotkey (F7): dump player Human* + swim chain next frame
static volatile long g_crowdReq = 0;     // set by hotkey (F8): build a crowd (non-player) body next frame
static volatile long g_sceneReq = 0;     // set by hotkey (F9): log a [Scene] diagnostic block next frame
static volatile long g_shipReq  = 0;     // set by hotkey (F10): log a [Ship] diagnostic block next frame
static volatile long g_shipSpawnReq = 0; // set by hotkey (F11): request a ship-dude spawn next frame
static volatile long g_shipCtorReq = 0;  // set by hotkey (F12): construct a SHIP entity + AddEntity next frame
static volatile long g_shipDriveReq = 0; // set by hotkey (F13): write +50 X to the local ship transform next frame
static volatile long g_ghostDespawnReq = 0; // set by hotkey (F11): remove the ghost ship next frame
static volatile long g_enumReq = 0;      // set by hotkey (F14): entity-enumeration probe next frame
static volatile long g_inputReq = 0;     // set by hotkey (F3): input-state probe next frame
static volatile long g_healthReq = 0;    // set by hotkey (F2): health heap-scan probe next frame
static volatile long g_mapReq = 0;       // set by hotkey (F1): guard->CSrvNPCHealth mapping probe next frame
static volatile long g_holdShip = 0;      // F5 toggle: pin the ship's position EVERY frame (drive test)
static void*  g_holdShipEnt = nullptr;    // the ship entity being held
static void*  g_holdShipRB = nullptr;     // the ship's RigidBodyComponent, driving mode flipped to Fixed
static float  g_holdPos[3] = { 0.0f, 0.0f, 0.0f };  // captured held position
static uint32_t      g_frameJmpBack = 0;  // resume addr after the stolen instruction (base+RVA+6)

void Phase1::RequestSceneProbe() { g_sceneReq = 1; }   // any thread: queue the probe for the main thread
void Phase1::RequestShipProbe()  { g_shipReq  = 1; }   // any thread: queue the F10 ship probe for the main thread
void Phase1::RequestShipSpawn()  { g_shipSpawnReq = 1; }   // any thread: queue the F11 ship-spawn request for the main thread
void Phase1::RequestGhostDespawn() { g_ghostDespawnReq = 1; } // any thread: queue the F11 ghost-removal for the main thread
void Phase1::RequestShipCtorProbe() { g_shipCtorReq = 1; } // any thread: queue the F12 construct-ship experiment for the main thread
void Phase1::RequestShipDriveProbe() { g_shipDriveReq = 1; } // any thread: queue the F13 ship-transform-write probe for the main thread
void Phase1::RequestEntityEnum()  { g_enumReq = 1; }         // any thread: queue the F14 entity-enumeration probe for the main thread
void Phase1::RequestInputProbe()  { g_inputReq = 1; }        // any thread: queue the F3 input-state probe for the main thread
void Phase1::RequestHealthProbe() { g_healthReq = 1; }       // any thread: queue the F2 health heap-scan probe for the main thread
void Phase1::RequestGuardHealthMap() { g_mapReq = 1; }      // any thread: queue the F1 guard->health mapping probe

// ---- Guard -> CSrvNPCHealth mapping probe (F1) ------------------------------------------------
// sub_1371140(owner) resolves CSrvNPCHealth out of owner->[+0x0C]'s service array at [+0x70]
// (count at +0x76), keyed by class id 0x57EEEEC7 — which is CRC32("CSrvNPCHealth"), verified offline.
// The unknown is *which* object is the owner, so test every candidate derivable from a live guard
// (Entity, its AI, its Human) and report the one whose service array actually holds a
// CSrvNPCHealth. Pure memory reads — no engine calls, no writes.
static void DumpSvcArray(const char* tag, int idx, void* cand) {
    unsigned int c = (unsigned int)cand;
    if (!c || !readable((void*)c, 0x10)) return;
    unsigned int ct = *(unsigned int*)c;
    unsigned int cont = *(unsigned int*)(c + 0x0C);
    if (!cont || !readable((void*)cont, 0x80)) return;          // quiet: only real containers
    unsigned int arr = *(unsigned int*)(cont + 0x70);
    unsigned int n   = *(unsigned short*)(cont + 0x76);
    if (!arr || !readable((void*)arr, 4) || n == 0 || n > 64) return;
    L("[Map]   %s[%d]=%08X vt=%08X container=%08X n=%u\n", tag, idx, c, ct, cont, n);
    for (unsigned int i = 0; i < n; ++i) {
        unsigned int svc = *(unsigned int*)(arr + i * 4);
        if (!svc || !readable((void*)svc, 4)) continue;
        if (*(unsigned int*)svc == 0x02712F60u)
            L("[Map]     *** HEALTH svc=%08X life=%u/%u flags=%08X  <== OWNER %s[%d]=%08X\n",
              svc, *(unsigned short*)(svc + 0x5C), *(unsigned short*)(svc + 0x5E), *(unsigned int*)(svc + 0x60),
              tag, idx, c);
    }
}

static void ProbeGuardHealthMap() {
    World* world = World::GetCurrentWorld();
    if (!world) { L("[Map] no world\n"); return; }
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    int shown = 0;
    __try {
        for (int i = 0; i < nobj && shown < 4; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4) || e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel = false; Component* bhvC = 0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;
            }
            if (!skel || !bhvC) continue;
            if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;   // soldiers only
            float x = e->m_GlobalMatrix.M41, y = e->m_GlobalMatrix.M42, z = e->m_GlobalMatrix.M43;
            float dx = x - px, dy = y - py, dz = z - pz;
            if (dx*dx + dy*dy + dz*dz > 80.0f*80.0f) continue;
            AbstractEntityAI* ai = AIEntityHelper::GetEntityAI(e);
            Human* human = AIEntityHelper::GetHuman(e);
            L("[Map] GUARD ent=%08X pos=(%.1f,%.1f,%.1f) ai=%08X human=%08X comps=%d\n", e, x, y, z, ai, human, n);
            ++shown;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Map] EXCEPTION code=0x%08X\n", GetExceptionCode());
    }
    L("[Map] guards shown=%d (nobj=%d)\n", shown, nobj);
}

// ---- Health snapshot / diff probe (F2) ---------------------------------------------------------
// Every live CSrvNPCHealth object (vtable 0x02712F60) with Life@+0x5C, Max@+0x5E, flags@+0x60.
// F2 press #1 records a baseline; every later press diffs against it. This answers the co-op wall
// question objectively: after hitting a guard, does its NPC-health Life actually fall — including
// when that guard is a driven mirror (BhvGenericNPC disabled, m_DriveEntity=false)? Read-only.
struct HealthEnt { unsigned int addr; unsigned short life; unsigned short hpMax; unsigned int flags; };
static HealthEnt g_hPrev[128];
static int       g_hPrevN = 0;
static int       g_hDumps = 0;

// Scan committed RW regions for CSrvNPCHealth objects. Returns the number found (<= cap).
static int ScanNPCHealth(HealthEnt* out, int cap) {
    int n = 0;
    __try {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
        unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
        while (p < end && n < cap) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
                unsigned int* q = (unsigned int*)mbi.BaseAddress;
                unsigned int* qe = (unsigned int*)((char*)mbi.BaseAddress + mbi.RegionSize);
                for (; q + 1 < qe && n < cap; ++q) {
                    if (*q != 0x02712F60u) continue;
                    unsigned char* o = (unsigned char*)q;
                    out[n].addr  = (unsigned int)o;
                    out[n].life  = *(unsigned short*)(o + 0x5C);
                    out[n].hpMax = *(unsigned short*)(o + 0x5E);
                    out[n].flags = *(unsigned int*)(o + 0x60);
                    ++n;
                }
            }
            p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[HealthDiff] EXCEPTION code=0x%08X\n", GetExceptionCode());
    }
    return n;
}

// ---- Is the health-data object (health+0x04 -> +0x10, vt 0x026AA778) itself the service container
// that sub_1371140 walks? If so its +0x70 is the array and +0x76 the count, and that array must contain
// the very health object we started from. Decisive, read-only. ----------------------------------
static void ProbeHealthDataWalk() {
    static HealthEnt hs[160];
    int hn = ScanNPCHealth(hs, 160);
    L("[Data] health objects=%d\n", hn);
    int shown = 0;
    for (int i = 0; i < hn && shown < 3; ++i) {
        unsigned int h = hs[i].addr;
        if (!readable((void*)h, 0x70)) continue;
        unsigned int p04 = *(unsigned int*)(h + 0x04);
        if (!p04 || !readable((void*)p04, 0x30)) continue;
        unsigned int pd = *(unsigned int*)(p04 + 0x10);
        if (!pd || !readable((void*)pd, 0x80)) continue;
        unsigned int arr = *(unsigned int*)(pd + 0x70);
        unsigned int cnt = *(unsigned short*)(pd + 0x76);
        L("[Data] health=%08X life=%u/%u | pd=%08X vt=%08X arr(+70)=%08X cnt(+76)=%u cache(+58)=%08X\n",
          h, *(unsigned short*)(h + 0x5C), *(unsigned short*)(h + 0x5E), pd, *(unsigned int*)pd,
          arr, cnt, *(unsigned int*)(pd + 0x58));
        if (arr && readable((void*)arr, 4) && cnt > 0 && cnt <= 64) {
            for (unsigned int k = 0; k < cnt && k < 8; ++k) {
                unsigned int svc = *(unsigned int*)(arr + k * 4);
                if (!svc || !readable((void*)svc, 4)) { L("[Data]   [%u]=%08X (bad)\n", k, svc); continue; }
                L("[Data]   [%u]=%08X vt=%08X%s\n", k, svc, *(unsigned int*)svc,
                  (svc == h ? "   <== THIS HEALTH" : ""));
            }
        }
        ++shown;
    }
    L("[Data] done\n");
}

// ---- Container/owner finder (F1): positive test, no heuristics --------------------------------
// sub_1371140 (verified): owner+0x58 cache; container = owner+0x0C; arr = *(container+0x70);
// n = *(u16*)(container+0x76); the array must contain a CSrvNPCHealth (class id 0x57EEEEC7).
// So a container is any object whose +0x70 array actually holds a live health object. Scan every
// dword that looks like a vtable (= an object start), test that predicate, then find the owner as the
// object whose +0x0C equals the container. Fully read-only, and only prints positive hits.
static unsigned int g_rwList[16];
static int          g_rwN = 0;

static void ProbeReverseWalk() {
    static HealthEnt hs[160];
    int hn = ScanNPCHealth(hs, 160);
    if (!hn) { L("[Rev] no health objects\n"); return; }
    unsigned int want[160];
    for (int i = 0; i < hn; ++i) want[i] = hs[i].addr;
    for (int i = 1; i < hn; ++i) { unsigned int v = want[i]; int j = i - 1; while (j >= 0 && want[j] > v) { want[j+1] = want[j]; --j; } want[j+1] = v; }

    g_rwN = 0;
    L("[Rev] health objects=%d — searching for the service container...\n", hn);
    int found = 0;
    __try {
        SYSTEM_INFO si; GetSystemInfo(&si);
        unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
        unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
        while (p < end && found < 6) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
                unsigned int* q = (unsigned int*)mbi.BaseAddress;
                unsigned int* qe = (unsigned int*)((char*)mbi.BaseAddress + mbi.RegionSize) - 0x20;
                for (; q < qe && found < 6; ++q) {
                    unsigned int vt = *q;
                    if (vt < 0x01E00000u || vt > 0x02800000u) continue;      // looks like a vtable
                    unsigned int C = (unsigned int)q;
                    if (!readable((void*)C, 0x78)) continue;
                    unsigned int arr = *(unsigned int*)(C + 0x70);
                    unsigned int n   = *(unsigned short*)(C + 0x76);
                    if (!arr || n == 0 || n > 64 || !readable((void*)arr, 4)) continue;
                    for (unsigned int k = 0; k < n; ++k) {
                        unsigned int svc = *(unsigned int*)(arr + k * 4);
                        if (svc < 0x01000000u || svc > 0x7FFFFFFFu) continue;
                        int lo = 0, hi = hn - 1, hit = 0;
                        while (lo <= hi) { int mid = (lo + hi) >> 1; if (want[mid] == svc) { hit = 1; break; } if (want[mid] < svc) lo = mid + 1; else hi = mid - 1; }
                        if (!hit) continue;
                        L("[Rev] CONTAINER C=%08X vt=%08X n=%u svc[%u]=%08X health life=%u/%u flags=%08X cache(+58)=%08X\n",
                          C, vt, n, k, svc,
                          *(unsigned short*)(svc + 0x5C), *(unsigned short*)(svc + 0x5E), *(unsigned int*)(svc + 0x60),
                          *(unsigned int*)(C + 0x58));
                        if (g_rwN < 16) g_rwList[g_rwN++] = C;
                        ++found;
                        break;
                    }
                }
            }
            p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[Rev] EXC container 0x%08X\n", GetExceptionCode()); }
    L("[Rev] containers found=%d\n", found);
    if (!found) return;

    // owner: the object whose +0x0C equals a container
    int owners = 0;
    __try {
        SYSTEM_INFO si; GetSystemInfo(&si);
        unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
        unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
        while (p < end && owners < 12) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
                unsigned int* q = (unsigned int*)mbi.BaseAddress;
                unsigned int* qe = (unsigned int*)((char*)mbi.BaseAddress + mbi.RegionSize);
                for (; q < qe && owners < 12; ++q) {
                    unsigned int v = *q;
                    bool match = false;
                    for (int i = 0; i < g_rwN; ++i) if (g_rwList[i] == v) { match = true; break; }
                    if (!match) continue;
                    unsigned int owner = (unsigned int)q - 0x0C;
                    if (!readable((void*)owner, 0x60)) continue;
                    unsigned int vt = *(unsigned int*)owner;
                    if (vt < 0x01E00000u || vt > 0x02800000u) continue;
                    L("[Rev] OWNER=%08X vt=%08X  (container=%08X at +0x0C, cache+58=%08X)\n",
                      owner, vt, v, *(unsigned int*)(owner + 0x58));
                    ++owners;
                }
            }
            p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[Rev] EXC owner 0x%08X\n", GetExceptionCode()); }
    L("[Rev] owners found=%d\n", owners);
}

// ---- Kill nearest soldier through its own service table (F1, WRITES) ---------------------------
// Verified path: a guard's AbstractEntityAI (vt 0x026D3CD8) holds a service array at +0x70
// (count at +0x76), and one entry is its CSrvNPCHealth (vt 0x02712F60). The engine's own dead
// encoding — observed live on a guard that died — is Life(+0x5C)=0xFFFF and Flags(+0x60)|=0x01000000.
// This writes exactly that, so the engine treats it as a real death rather than a faked flag.
static void KillNearestGuard() {
    World* world = World::GetCurrentWorld();
    if (!world) { L("[Kill] no world\n"); return; }
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    Entity* best = 0; AbstractEntityAI* bestAi = 0; float bestD = 80.0f * 80.0f;
    __try {
        for (int i = 0; i < nobj; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4) || e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel = false; Component* bhvC = 0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;
            }
            if (!skel || !bhvC) continue;
            if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
            float dx = e->m_GlobalMatrix.M41 - px, dy = e->m_GlobalMatrix.M42 - py, dz = e->m_GlobalMatrix.M43 - pz;
            float d = dx*dx + dy*dy + dz*dz;
            if (d < bestD) { bestD = d; best = e; bestAi = AIEntityHelper::GetEntityAI(e); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[Kill] EXC scan 0x%08X\n", GetExceptionCode()); }
    if (!best || !bestAi) { L("[Kill] no soldier found within 80m\n"); return; }

    float gx = best->m_GlobalMatrix.M41, gy = best->m_GlobalMatrix.M42, gz = best->m_GlobalMatrix.M43;
    L("[Kill] target ent=%08X ai=%08X dist2=%.0f pos=(%.1f,%.1f,%.1f)\n",
      best, bestAi, bestD, gx, gy, gz);
    if (!readable((void*)bestAi, 0x78)) { L("[Kill] ai unreadable\n"); return; }

    unsigned int arr = *(unsigned int*)((char*)bestAi + 0x70);
    unsigned int n   = *(unsigned short*)((char*)bestAi + 0x76);
    L("[Kill] service array=%08X n=%u\n", arr, n);
    if (!arr || !readable((void*)arr, 4) || n == 0 || n > 64) { L("[Kill] bad service array\n"); return; }

    unsigned int hp = 0;
    for (unsigned int k = 0; k < n; ++k) {
        unsigned int svc = *(unsigned int*)(arr + k * 4);
        if (!svc || !readable((void*)svc, 0x70)) continue;
        unsigned int vt = *(unsigned int*)svc;
        if (k < 16 || vt == 0x02712F60u)
            L("[Kill]   svc[%u]=%08X vt=%08X\n", k, svc, vt);
        if (vt == 0x02712F60u && !hp) hp = svc;
    }
    if (!hp) { L("[Kill] no CSrvNPCHealth in this AI's service table\n"); return; }

    unsigned short life  = *(unsigned short*)(hp + 0x5C);
    unsigned short maxv  = *(unsigned short*)(hp + 0x5E);
    unsigned int   flags = *(unsigned int*)(hp + 0x60);
    L("[Kill] health=%08X before life=%u/%u flags=%08X -> writing dead encoding\n", hp, life, maxv, flags);
    *(unsigned short*)(hp + 0x5C) = 0xFFFF;
    *(unsigned int*)(hp + 0x60) = flags | 0x01000000u;
    L("[Kill] health=%08X after  life=%u flags=%08X\n",
      hp, *(unsigned short*)(hp + 0x5C), *(unsigned int*)(hp + 0x60));
}

// ---- Real-death field diff (F1) ---------------------------------------------------------------
// The dead encoding can't be located by instruction search (the engine sets these flags through
// generic bitfield helpers with runtime offsets). So observe instead: snapshot a guard's AI, Human
// and health bytes, let the USER kill that guard normally, then snapshot again and diff. Whatever the
// engine changes on a genuine death is what a host-side kill has to reproduce.
static unsigned int g_ddEnt = 0, g_ddAi = 0, g_ddHuman = 0, g_ddHealth = 0;
static unsigned char g_ddAiBuf[0x100], g_ddHumBuf[0x100], g_ddHpBuf[0x80];
static int g_ddHave = 0;

static unsigned int FindHealthInAI(unsigned int ai) {
    if (!ai || !readable((void*)ai, 0x78)) return 0;
    unsigned int arr = *(unsigned int*)(ai + 0x70);
    unsigned int n   = *(unsigned short*)(ai + 0x76);
    if (!arr || !readable((void*)arr, 4) || n == 0 || n > 64) return 0;
    for (unsigned int k = 0; k < n; ++k) {
        unsigned int svc = *(unsigned int*)(arr + k * 4);
        if (!svc || !readable((void*)svc, 0x70)) continue;
        if (*(unsigned int*)svc == 0x02712F60u) return svc;
    }
    return 0;
}

static void ProbeDeathDiff() {
    World* world = World::GetCurrentWorld();
    if (!world) { L("[Death] no world\n"); return; }

    if (!g_ddHave) {
        Entity* player = world->GetPlayer();
        float px = 0, py = 0, pz = 0;
        if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
        BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
        int nobj = ents.GetSize();
        Entity* best = 0; float bestD = 40.0f * 40.0f;
        __try {
            for (int i = 0; i < nobj; ++i) {
                Reference* ref = ents.GetElement(i);
                if (!ref || !readable(ref, 4)) break;
                HandleContainer* hc = ref->m_HandleContainer;
                if (!hc || !readable(hc, 4)) continue;
                Entity* e = (Entity*)hc->m_Object;
                if (!e || !readable(e, 4) || e == player) continue;
                Component** data = *(Component***)((char*)e + 0x60);
                int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
                if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
                if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
                bool skel = false; Component* bhvC = 0;
                for (int k = 0; k < n; ++k) {
                    Component* c = data[k];
                    uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                    if (vt == 0x01E41E58) skel = true;
                    if (vt == 0x026E34D8) bhvC = c;
                }
                if (!skel || !bhvC) continue;
                if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
                float dx = e->m_GlobalMatrix.M41 - px, dy = e->m_GlobalMatrix.M42 - py, dz = e->m_GlobalMatrix.M43 - pz;
                float d = dx*dx + dy*dy + dz*dz;
                if (d < bestD) { bestD = d; best = e; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { L("[Death] EXC scan 0x%08X\n", GetExceptionCode()); }
        if (!best) { L("[Death] no soldier within 40m\n"); return; }

        AbstractEntityAI* ai = AIEntityHelper::GetEntityAI(best);
        Human* hu = AIEntityHelper::GetHuman(best);
        g_ddEnt = (unsigned int)best; g_ddAi = (unsigned int)ai; g_ddHuman = (unsigned int)hu;
        g_ddHealth = FindHealthInAI(g_ddAi);
        L("[Death] BASELINE ent=%08X ai=%08X human=%08X health=%08X life=%u/%u flags=%08X pos=(%.1f,%.1f,%.1f)\n",
          g_ddEnt, g_ddAi, g_ddHuman, g_ddHealth,
          g_ddHealth ? *(unsigned short*)(g_ddHealth + 0x5C) : 0,
          g_ddHealth ? *(unsigned short*)(g_ddHealth + 0x5E) : 0,
          g_ddHealth ? *(unsigned int*)(g_ddHealth + 0x60) : 0,
          best->m_GlobalMatrix.M41, best->m_GlobalMatrix.M42, best->m_GlobalMatrix.M43);
        if (g_ddAi && readable((void*)g_ddAi, 0x100)) memcpy(g_ddAiBuf, (void*)g_ddAi, 0x100);
        if (g_ddHuman && readable((void*)g_ddHuman, 0x100)) memcpy(g_ddHumBuf, (void*)g_ddHuman, 0x100);
        if (g_ddHealth && readable((void*)g_ddHealth, 0x80)) memcpy(g_ddHpBuf, (void*)g_ddHealth, 0x80);
        g_ddHave = 1;
        L("[Death] now kill that soldier normally, then press F1 again.\n");
        return;
    }

    // diff pass
    L("[Death] DIFF (ai=%08X human=%08X health=%08X)\n", g_ddAi, g_ddHuman, g_ddHealth);
    int changes = 0;
    if (g_ddAi && readable((void*)g_ddAi, 0x100)) {
        unsigned char* now = (unsigned char*)g_ddAi;
        for (int i = 0; i < 0x100 && changes < 80; ++i)
            if (now[i] != g_ddAiBuf[i]) {
                L("[Death]  AI+%03X %02X -> %02X\n", i, g_ddAiBuf[i], now[i]);
                ++changes;
            }
    } else L("[Death] AI no longer readable (freed?)\n");
    if (g_ddHuman && readable((void*)g_ddHuman, 0x100)) {
        unsigned char* now = (unsigned char*)g_ddHuman;
        for (int i = 0; i < 0x100 && changes < 120; ++i)
            if (now[i] != g_ddHumBuf[i]) {
                L("[Death]  HUM+%03X %02X -> %02X\n", i, g_ddHumBuf[i], now[i]);
                ++changes;
            }
    } else L("[Death] Human no longer readable\n");
    if (g_ddHealth && readable((void*)g_ddHealth, 0x80)) {
        unsigned char* now = (unsigned char*)g_ddHealth;
        for (int i = 0; i < 0x80 && changes < 140; ++i)
            if (now[i] != g_ddHpBuf[i]) {
                L("[Death]  HP+%03X %02X -> %02X\n", i, g_ddHpBuf[i], now[i]);
                ++changes;
            }
    } else L("[Death] health no longer readable\n");
    if (g_ddHealth && readable((void*)g_ddHealth, 0x70))
        L("[Death] health now life=%u/%u flags=%08X\n",
          *(unsigned short*)(g_ddHealth + 0x5C), *(unsigned short*)(g_ddHealth + 0x5E),
          *(unsigned int*)(g_ddHealth + 0x60));
    L("[Death] total byte changes=%d\n", changes);
    g_ddHave = 0;   // next press starts a fresh baseline
}

// ---- Hardware write-breakpoint on the health's Life field (F1) ---------------------------------
// Static search is exhausted: the engine writes these state bytes through generic bitfield helpers
// with runtime offsets, so no immediate/field-offset pattern exists. Instead let the CPU report the
// writer: arm DR0 as a 4-byte write breakpoint on health+0x5C (Life). When something writes there we
// log the faulting EIP and disarm immediately. Whatever EIP appears during a normal kill is the code
// that owns the real death.
static volatile LONG g_drArmed = 0;
static int g_drHits = 0;
static LONG WINAPI DrTrap(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!g_drArmed) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    unsigned int eip = (unsigned int)c->Eip;
    if (g_drHits < 12)
        L("[DR] Life was written by eip=%08X (write@%08X)\n", eip, (unsigned int)ep->ExceptionRecord->ExceptionInformation[1]);
    ++g_drHits;
    c->Dr0 = 0; c->Dr1 = 0; c->Dr2 = 0; c->Dr3 = 0; c->Dr6 = 0; c->Dr7 = 0;
    InterlockedExchange(&g_drArmed, 0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ArmLifeWriteTrap() {
    static volatile LONG s_veh = 0;
    if (!s_veh) { AddVectoredExceptionHandler(1, DrTrap); InterlockedExchange(&s_veh, 1); }

    World* world = World::GetCurrentWorld();
    if (!world) { L("[DR] no world\n"); return; }
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    Entity* best = 0; float bestD = 40.0f * 40.0f;
    __try {
        for (int i = 0; i < nobj; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4) || e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel = false; Component* bhvC = 0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;
            }
            if (!skel || !bhvC) continue;
            if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
            float dx = e->m_GlobalMatrix.M41 - px, dy = e->m_GlobalMatrix.M42 - py, dz = e->m_GlobalMatrix.M43 - pz;
            float d = dx*dx + dy*dy + dz*dz;
            if (d < bestD) { bestD = d; best = e; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[DR] EXC scan 0x%08X\n", GetExceptionCode()); }
    if (!best) { L("[DR] no soldier within 40m\n"); return; }

    unsigned int hp = FindHealthInAI((unsigned int)AIEntityHelper::GetEntityAI(best));
    if (!hp) { L("[DR] no health in AI service table\n"); return; }
    g_drHits = 0;
    InterlockedExchange(&g_drArmed, 1);

    HANDLE th = GetCurrentThread();
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) { L("[DR] GetThreadContext failed %u\n", GetLastError()); InterlockedExchange(&g_drArmed, 0); return; }
    c.Dr0 = hp + 0x5C;
    c.Dr1 = c.Dr2 = c.Dr3 = 0;
    c.Dr6 = 0;
    c.Dr7 = 0x000D0001;   // L0=1, RW0=01 (write), LEN0=11 (4 bytes)
    if (!SetThreadContext(th, &c)) { L("[DR] SetThreadContext failed %u\n", GetLastError()); InterlockedExchange(&g_drArmed, 0); return; }
    L("[DR] armed write trap on health=%08X field +0x5C (life=%u/%u). Kill that soldier normally.\n",
      hp, *(unsigned short*)(hp + 0x5C), *(unsigned short*)(hp + 0x5E));
}

// ---- Guard-page access trap on the guard's health (F1) -----------------------------------------
// The DR write breakpoint armed on the main thread never fired, yet the object did reach the dead
// encoding => the write happens on another thread (Scimitar job threads; DRs are per-thread).
// PAGE_GUARD is thread-agnostic: it raises STATUS_GUARD_PAGE_VIOLATION for any access. We filter on
// the faulting address (inside the health) and record the faulting EIP, so the real death writer shows
// up. Press F1 again to disarm.
static unsigned int g_gpHealth = 0, g_gpPage = 0;
static int g_gpArmed = 0, g_gpCount = 0;
static DWORD g_gpArmTick = 0;
static unsigned int g_gpEips[24];
static int g_gpEipN = 0;

static void DisarmGuardPage() {
    if (g_gpArmed && g_gpPage) {
        DWORD old = 0;
        VirtualProtect((void*)g_gpPage, 0x1000, PAGE_READWRITE, &old);
    }
    g_gpArmed = 0;
    L("[GP] disarmed, %d accesses, %d distinct EIP(s)\n", g_gpCount, g_gpEipN);
}

static LONG WINAPI GpTrap(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (!g_gpArmed) return EXCEPTION_CONTINUE_SEARCH;
    unsigned int addr = (unsigned int)(ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[1];
    if (g_gpHealth && addr >= g_gpHealth && addr < g_gpHealth + 0x80) {
        unsigned int eip = (unsigned int)(ULONG_PTR)ep->ContextRecord->Eip;
        bool seen = false;
        for (int i = 0; i < g_gpEipN; ++i) if (g_gpEips[i] == eip) { seen = true; break; }
        if (!seen && g_gpEipN < 24) {
            g_gpEips[g_gpEipN++] = eip;
            L("[GP] health+%03X accessed by eip=%08X\n", addr - g_gpHealth, eip);
        }
        ++g_gpCount;
        // the Life word itself (+0x5C..+0x5D) — this is the death write; report and auto-disarm
        if (addr >= g_gpHealth + 0x5C && addr < g_gpHealth + 0x5E) {
            L("[GP] *** LIFE WRITE by eip=%08X (addr health+%03X)\n", eip, addr - g_gpHealth);
            DisarmGuardPage();
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (g_gpCount > 600) DisarmGuardPage();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ArmGuardPage() {
    static volatile LONG s_veh = 0;
    if (!s_veh) { AddVectoredExceptionHandler(1, GpTrap); InterlockedExchange(&s_veh, 1); }
    // Our hotkey fires twice per press (key down + repeat). Ignore the duplicate so arming isn't
    // immediately undone; an explicit disarm only happens after a real delay.
    if (g_gpArmed) {
        if (GetTickCount() - g_gpArmTick < 2000) return;
        DisarmGuardPage();
        return;
    }

    World* world = World::GetCurrentWorld();
    if (!world) { L("[GP] no world\n"); return; }
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    Entity* best = 0; float bestD = 40.0f * 40.0f;
    __try {
        for (int i = 0; i < nobj; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4) || e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel = false; Component* bhvC = 0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;
            }
            if (!skel || !bhvC) continue;
            if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
            float dx = e->m_GlobalMatrix.M41 - px, dy = e->m_GlobalMatrix.M42 - py, dz = e->m_GlobalMatrix.M43 - pz;
            float d = dx*dx + dy*dy + dz*dz;
            if (d < bestD) { bestD = d; best = e; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[GP] EXC scan 0x%08X\n", GetExceptionCode()); }
    if (!best) { L("[GP] no soldier within 40m\n"); return; }

    unsigned int hp = FindHealthInAI((unsigned int)AIEntityHelper::GetEntityAI(best));
    if (!hp) { L("[GP] no health in AI service table\n"); return; }

    g_gpHealth = hp;
    g_gpPage = hp & ~0xFFFu;
    g_gpCount = 0;
    g_gpEipN = 0;
    DWORD old = 0;
    if (!VirtualProtect((void*)g_gpPage, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) {
        L("[GP] VirtualProtect failed %u\n", GetLastError());
        return;
    }
    g_gpArmed = 1;
    g_gpArmTick = GetTickCount();
    float tx = best->m_GlobalMatrix.M41, ty = best->m_GlobalMatrix.M42, tz = best->m_GlobalMatrix.M43;
    L("[GP] armed on health=%08X (life=%u/%u) page=%08X TARGET ent=%08X pos=(%.1f,%.1f,%.1f) — kill THAT guard\n",
      hp, *(unsigned short*)(hp + 0x5C), *(unsigned short*)(hp + 0x5E), g_gpPage, best, tx, ty, tz);
}

// ---- Kill via the engine's own dead flag (F1, WRITES) ------------------------------------------
// Evidence: the game's own is-dead predicate is sub_753330 -> *(BYTE*)(ai+0x8C), and a real death
// flips AI+0x8C 0 -> 1. A real death also sets health Life=0xFFFF and Flags|=0x01000000. Writing the
// health alone did nothing; this writes BOTH, i.e. the engine's actual dead state rather than a
// half-state. (Animation quality is a separate, cosmetic question.)
// Find the nearest soldier guard to a world position within `radius` metres (0 if none).
static Entity* FindNearestSoldierTo(float x, float y, float z, float radius) {
    World* world = World::GetCurrentWorld();
    if (!world) return 0;
    Entity* player = world->GetPlayer();
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    Entity* best = 0; float bestD = radius * radius;
    __try {
        for (int i = 0; i < nobj; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4) || e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel = false; Component* bhvC = 0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;
            }
            if (!skel || !bhvC) continue;
            if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
            float dx = e->m_GlobalMatrix.M41 - x, dy = e->m_GlobalMatrix.M42 - y, dz = e->m_GlobalMatrix.M43 - z;
            float d = dx*dx + dy*dy + dz*dz;
            if (d < bestD) { bestD = d; best = e; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[Kill] EXC scan 0x%08X\n", GetExceptionCode()); }
    return best;
}

// Host -> peer health event. Snapshots only carry LIVING guards (dead NPCs drop out of the entity
// enumeration), so a death or a damage tick has to be pushed explicitly on the event lane.
static uint64_t ObjIdForEntity(unsigned int ent) {
    for (int i = 0; i < s_objMapN; ++i) if (s_objMap[i].ent == ent) return s_objMap[i].objId;
    return 0;
}
static void SendHealthEvent(uint64_t objId, unsigned short life, float x, float y, float z) {
    if (!g_entHost || !objId) return;              // host lane only
    EntityPacket out;
    memset(&out, 0, sizeof(out));
    out.magic = NET_ENTITY_MAGIC;
    out.protoVer = NET_PROTO_VER;
    out.seq = GetTickCount();
    out.count = 1;
    out.ents[0].kind = KIND_KILL;
    out.ents[0].objId = objId;
    out.ents[0].hp = life;
    out.ents[0].pos[0] = x; out.ents[0].pos[1] = y; out.ents[0].pos[2] = z;
    L("[Kill] push health event objId=%llu life=%u -> peer\n", (unsigned long long)objId, (unsigned)life);
    Net::SendEntities(out);

    // Self-test: there is no peer, so deliver the packet to the local apply path — exactly what the guest
    // would do. This is what makes the host->guest lane testable on one machine.
    if (SelfTestModeActive() && !s_inEventLoop) {
        s_inEventLoop = true;
        L("[Kill] SELF-TEST loopback: delivering the event to the apply path\n");
        ProcessKillEvents(out);
        s_inEventLoop = false;
    }
}

// The verified kill recipe, applied to one specific guard entity.
static void KillGuardEntity(Entity* e) {
    if (!e) return;

    unsigned int ai = (unsigned int)AIEntityHelper::GetEntityAI(e);
    unsigned int hp = FindHealthInAI(ai);
    L("[KF] target ent=%08X ai=%08X health=%08X pos=(%.1f,%.1f,%.1f)\n",
      e, ai, hp, e->m_GlobalMatrix.M41, e->m_GlobalMatrix.M42, e->m_GlobalMatrix.M43);
    if (!ai || !readable((void*)ai, 0xC0)) { L("[KF] ai unreadable\n"); return; }

    L("[KF] before: alive=%d  health life=%u/%u flags=%08X\n", (int)EngineIsAlive((unsigned int)e),
      hp ? *(unsigned short*)(hp + 0x5C) : 0, hp ? *(unsigned short*)(hp + 0x5E) : 0,
      hp ? *(unsigned int*)(hp + 0x60) : 0);

    if (hp) {
        // Engine-native death: CSrvNPCHealth::SetLife(hp, -1) (sub_19FB4C0, vtable slot 16). A negative
        // value IS the engine's own death write (Life lands on 0xFFFF) AND it posts the health event
        // that drives the death state machine — which is what should trigger the ragdoll/animation.
        int prev = *(unsigned short*)(hp + 0x5C);
        int rc = ((int (__thiscall*)(void*, int))0x19FB4C0)((void*)hp, -1);
        L("[KF] SetLife(-1) rc=%d  life %d -> %u\n", rc, prev, *(unsigned short*)(hp + 0x5C));
        *(unsigned int*)(hp + 0x60) |= 0x01000000u;   // the engine sets this on death too
    }
    // Combat dead: the engine's is-alive (sub_133F550) checks bit 0x1000 at +4 of the service returned by
    // sub_10EF8C0(entity) (class 0x5EB03CEC, found in ai+0x5C). SetLife alone does NOT set it — verified
    // live (alive stayed 1). This is the flag that actually makes the guard dead to combat.
    {
        unsigned int svc = ((unsigned int (__cdecl*)(int))0x10EF8C0)((int)e);
        if (svc && readable((void*)svc, 0x20)) {
            unsigned int before = *(unsigned int*)(svc + 4);
            *(unsigned int*)(svc + 4) = before | 0x1000u;
            L("[KF] is-alive service=%08X +4 %08X -> %08X\n", svc, before, *(unsigned int*)(svc + 4));
        } else L("[KF] is-alive service not found\n");
    }
    // AI-side stop (sub_753330 reads ai+0x8C). NOT the combat flag — kept only so the behaviour stops
    // running and cannot re-pose the corpse. Clearly labelled to avoid the old confusion.
    *(unsigned char*)(ai + 0x8C) = 1;
    L("[KF] after : alive=%d health life=%u flags=%08X\n", (int)EngineIsAlive((unsigned int)e),
      hp ? *(unsigned short*)(hp + 0x5C) : 0, hp ? *(unsigned int*)(hp + 0x60) : 0);

    // Play the death clip recorded from a real kill (alias 1818493434, captured live when a soldier
    // died naturally). The dead flag was set first, so the behaviour stops issuing its own actions and
    // this clip is what the corpse plays.
    // Combine the two paths: SetLife(-1) above already made this a REAL death (engine de-registers him,
    // he leaves the map and can no longer be interacted with). Now also request the recorded death clip
    // so the corpse actually falls instead of standing there.
    static const bool kPlayDeathClip = true;
    Human* h = AIEntityHelper::GetHuman(e);
    if (h && readable(h, 4)) {
        ActionComponent* ac = h->GetActionComponent();
        if (ac && readable(ac, 0x40)) {
            ac->SetTargetAction((int)1818493434u, 0, 0, 0xFFFF, ACTLAYER_PRIMARY);
            L("[KF] requested death clip alias=1818493434\n");
        } else L("[KF] no ActionComponent\n");
    } else L("[KF] no Human\n");

    // The corpse's collision capsule: a dead guard's CharacterController otherwise stays solid, so the
    // body acts like an invisible wall and gets nudged/rotated when walked into. Turn it off.
    if (h && readable(h, 4)) {
        CharacterController* cc = (CharacterController*)h->GetCharacterComponent();
        if (cc && readable(cc, 0x600)) {
            // Only the collision filter is changed. m_DriveEntity=false is deliberately NOT set here:
            // it stopped the controller applying the animation's root motion, which left the corpse
            // standing. Keep the controller fully functional and make it non-colliding instead.
            ((char (__thiscall*)(void*, int))0x636480)((void*)&cc->m_FilterInfo, 0x29);
            L("[KF] collision layer -> Disabled(0x29) (controller left untouched)\n");
        } else L("[KF] no CharacterController\n");
    }

    // Tell the peer. Dead guards disappear from the snapshot enumeration, so this push is the only way
    // a host kill can ever reach the guest.
    SendHealthEvent(ObjIdForEntity((unsigned int)e), 0xFFFFu,
                    e->m_GlobalMatrix.M41, e->m_GlobalMatrix.M42, e->m_GlobalMatrix.M43);
}

// Host path: the peer reported a health change for a guard. Resolve by the host-assigned objId first;
// fall back to the reported position only if the id is unknown (e.g. after a re-capture).
static void ApplyPeerHealth(uint64_t objId, unsigned short life, float x, float y, float z) {
    unsigned int ent = 0, hp = 0;
    bool byId = ObjMapFind(objId, &ent, &hp);
    if (!byId) {
        // Guest side: objId -> local mirror, learned from the snapshots.
        unsigned int ge = GuestObjFind(objId);
        if (ge) { ent = ge; byId = true; hp = CachedHealthForEntity(ge); }
    }
    if (!byId) {
        Entity* e = FindNearestSoldierTo(x, y, z, 4.0f);
        if (!e) { L("[Kill] event objId=%llu unresolved (no soldier within 4m)\n", (unsigned long long)objId); return; }
        ent = (unsigned int)e;
        hp  = CachedHealthForEntity(ent);
        L("[Kill] objId=%llu unknown -> position fallback ent=%08X\n", (unsigned long long)objId, ent);
    }
    if (!hp && ent) {
        hp = FindHealthInAI((unsigned int)AIEntityHelper::GetEntityAI((Entity*)ent));
        if (hp) RememberEntityHealth(ent, hp);
    }

    if (life == 0xFFFFu) {
        L("[Kill] peer DEATH objId=%llu -> ent=%08X health=%08X\n", (unsigned long long)objId, ent, hp);
        if (ent) {
            MarkPeerApplied(ent);                 // don't echo it back
            MarkMirrorKilledLocally((void*)ent);  // stop driving the corpse
            KillGuardEntity((Entity*)ent);
        }
        return;
    }
    if (!hp) { L("[Kill] peer damage objId=%llu: no health\n", (unsigned long long)objId); return; }
    int rc = ((int (__thiscall*)(void*, int))0x19FB4C0)((void*)hp, (int)life);
    if (ent) MarkPeerApplied(ent);
    L("[Kill] peer damage objId=%llu hp=%u (SetLife rc=%d)\n", (unsigned long long)objId, (unsigned)life, rc);
}

// F2: solo harness for the GUEST side of the relay. The real path can't be exercised in self-test
// because the host snapshot already says dead (so echo-suppression correctly skips it). Instead:
//   press 1 -> pick the nearest soldier and record his Life while alive (what ApplyGuards does each tick)
//   press 2 -> kill him, then call the REAL TrackMirrorDeathForRelay with dead=0 (host unaware, exactly
//              the real scenario) and check that it reports the transition.
static unsigned int s_relayTestEnt = 0;
static int s_relayTestState = 0;

static void RelaySelfTest() {
    World* world = World::GetCurrentWorld();
    if (!world) { L("[RelayTest] no world\n"); return; }
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }

    if (s_relayTestState == 0) {
        Entity* e = FindNearestSoldierTo(px, py, pz, 40.0f);
        if (!e) { L("[RelayTest] no soldier within 40m\n"); return; }
        EntityState es; memset(&es, 0, sizeof(es));
        es.kind = 2; es.dead = 0;
        es.pos[0] = e->m_GlobalMatrix.M41; es.pos[1] = e->m_GlobalMatrix.M42; es.pos[2] = e->m_GlobalMatrix.M43;
        TrackMirrorDeathForRelay(e, es);          // records Life while alive
        unsigned int hp = FindHealthInAI((unsigned int)AIEntityHelper::GetEntityAI(e));
        s_relayTestEnt = (unsigned int)e;
        s_relayTestState = 1;
        L("[RelayTest] armed on ent=%08X life=%u — press F2 again to kill + report\n",
          e, hp ? *(unsigned short*)(hp + 0x5C) : 0);
        return;
    }

    Entity* e = (Entity*)s_relayTestEnt;
    s_relayTestState = 0;
    if (!e || !readable(e, 4)) { L("[RelayTest] target gone\n"); return; }
    EntityState es; memset(&es, 0, sizeof(es));
    es.kind = 2; es.dead = 0;                     // host is NOT aware — the real scenario
    es.pos[0] = e->m_GlobalMatrix.M41; es.pos[1] = e->m_GlobalMatrix.M42; es.pos[2] = e->m_GlobalMatrix.M43;
    KillGuardEntity(e);
    L("[RelayTest] guard killed; now exercising guest death detection (expect a [Relay] line)\n");
    TrackMirrorDeathForRelay(e, es);
    L("[RelayTest] done (if no [Relay] line above, detection failed)\n");
}
// F1: solo self-test of the host path. Picks the nearest soldier and then runs the SAME position-based
// path a peer kill uses (not a direct pointer), so relay resolution is exercised without a second PC.
static void SimulatePeerKill() {
    World* world = World::GetCurrentWorld();
    if (!world) { L("[Kill] no world\n"); return; }
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
    Entity* e = FindNearestSoldierTo(px, py, pz, 40.0f);
    if (!e) { L("[Kill] no soldier within 40m\n"); return; }
    float x = e->m_GlobalMatrix.M41, y = e->m_GlobalMatrix.M42, z = e->m_GlobalMatrix.M43;
    uint64_t id = 0;
    for (int i = 0; i < s_objMapN; ++i)
        if (s_objMap[i].ent == (unsigned int)e) { id = s_objMap[i].objId; break; }
    L("[Kill] SELF-TEST peer event objId=%llu ent=%08X pos=(%.1f,%.1f,%.1f)\n",
      (unsigned long long)id, e, x, y, z);
    ApplyPeerHealth(id, 0xFFFFu, x, y, z);
}

// ---- Die-clip watcher (always on, throttled) ---------------------------------------------------
// Functional death is solved (AI+0x8C=1 + health encoding), but the corpse animation comes from the
// death state machine. To reproduce it we need the death clip's action alias. This samples the nearest
// soldier's current action alias and, when its dead flag flips 0->1, dumps the alias window around the
// transition — so a natural kill tells us the real die clip. Read-only.
struct DieWatch {
    unsigned int ent; int dead; unsigned int lastAlias;
    unsigned int ai; unsigned int health;
    unsigned int lastPtr98, lastByte98, lastLife;
};
static DieWatch s_dw[8];
static int s_dwN = 0;
static uint32_t s_dwBeat = 0;

static unsigned int GuardActionAlias(Entity* e) {
    Human* h = AIEntityHelper::GetHuman(e);
    if (!h || !readable(h, 4)) return 0;
    ActionComponent* ac = h->GetActionComponent();
    if (!ac || !readable(ac, 0x40)) return 0;
    unsigned char mirrored = 0;
    void* act = ac->GetCurrentAction(&mirrored, 0, ACTLAYER_PRIMARY);
    if (act && readable(act, 0x14)) return *(uint32_t*)((char*)act + 8);
    return 0;
}

static void TickDieClipWatch() {
    static uint32_t s_tick = 0;
    if ((++s_tick % 10) != 0) return;          // ~6 Hz at 60fps
    World* world = World::GetCurrentWorld();
    if (!world) return;
    Entity* player = world->GetPlayer();
    float px = 0, py = 0, pz = 0;
    if (player) { px = player->m_GlobalMatrix.M41; py = player->m_GlobalMatrix.M42; pz = player->m_GlobalMatrix.M43; }
    BaseBigArray<Reference>& ents = world->m_EntityManager.m_Entities;
    int nobj = ents.GetSize();
    int seen[8];
    for (int i = 0; i < s_dwN; ++i) seen[i] = 0;
    __try {
        for (int i = 0; i < nobj; ++i) {
            Reference* ref = ents.GetElement(i);
            if (!ref || !readable(ref, 4)) break;
            HandleContainer* hc = ref->m_HandleContainer;
            if (!hc || !readable(hc, 4)) continue;
            Entity* e = (Entity*)hc->m_Object;
            if (!e || !readable(e, 4) || e == player) continue;
            Component** data = *(Component***)((char*)e + 0x60);
            int n = (*(unsigned short*)((char*)e + 0x64) & 0xFFFF);
            if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)e + 0x66) & 0xFFFF);
            if (!data || !readable(data, 4) || n <= 0 || n > 96) continue;
            bool skel = false; Component* bhvC = 0;
            for (int k = 0; k < n; ++k) {
                Component* c = data[k];
                uint32_t vt = (c && readable(c, 4)) ? *(uint32_t*)c : 0;
                if (vt == 0x01E41E58) skel = true;
                if (vt == 0x026E34D8) bhvC = c;
            }
            if (!skel || !bhvC) continue;
            if ((*(uint32_t*)((char*)bhvC + 0x14) & 0xFFFF) != 0x35F7) continue;
            float x = e->m_GlobalMatrix.M41, y = e->m_GlobalMatrix.M42, z = e->m_GlobalMatrix.M43;
            float dx = x - px, dy = y - py, dz = z - pz;
            if (dx*dx + dy*dy + dz*dz > 30.0f*30.0f) continue;

            int slot = -1;
            for (int k = 0; k < s_dwN; ++k) if (s_dw[k].ent == (unsigned int)e) { slot = k; break; }
            if (slot < 0) {
                if (s_dwN >= 8) continue;
                slot = s_dwN++;
                s_dw[slot].ent = (unsigned int)e;
                s_dw[slot].dead = 0;
                s_dw[slot].lastAlias = 0xFFFFFFFFu;
                s_dw[slot].ai = 0;
                s_dw[slot].health = 0;
                s_dw[slot].lastPtr98 = 0xFFFFFFFFu;
                s_dw[slot].lastByte98 = 0xFFFFFFFFu;
                s_dw[slot].lastLife = 0xFFFFFFFFu;
                L("[DieClip] tracking ent=%08X pos=(%.1f,%.1f,%.1f)\n", e, x, y, z);
            }
            seen[slot] = 1;
            unsigned int ai = (unsigned int)AIEntityHelper::GetEntityAI(e);
            s_dw[slot].ai = ai;
            if (!s_dw[slot].health) s_dw[slot].health = FindHealthInAI(ai);
            int dead = (ai && readable((void*)ai, 0xC0) && *(unsigned char*)(ai + 0x8C)) ? 1 : 0;
            unsigned int alias = GuardActionAlias(e);
            if (alias != s_dw[slot].lastAlias) {
                L("[DieClip] ent=%08X alias %u -> %u dead=%d\n", e,
                  s_dw[slot].lastAlias == 0xFFFFFFFFu ? 0u : s_dw[slot].lastAlias, alias, dead);
                s_dw[slot].lastAlias = alias;
            }
            if (dead && !s_dw[slot].dead) {
                unsigned int hp = FindHealthInAI(ai);
                L("[DieClip] *** DEATH ent=%08X ai=%08X health=%08X life=%u/%u flags=%08X alias=%u\n",
                  e, ai, hp, hp ? *(unsigned short*)(hp+0x5C) : 0, hp ? *(unsigned short*)(hp+0x5E) : 0,
                  hp ? *(unsigned int*)(hp+0x60) : 0, alias);
            }
            s_dw[slot].dead = dead;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (int k = 0; k < s_dwN; ) {
        if (!seen[k]) { s_dw[k] = s_dw[s_dwN-1]; seen[k] = seen[s_dwN-1]; --s_dwN; }
        else ++k;
    }
    if ((++s_dwBeat % 240) == 0) L("[DieClip] tracking %d soldier(s)\n", s_dwN);
}

// Every-frame poll of the tracked guards: reads AI+0x98 (the engine's other death indicator — the
// dereferenced byte, i.e. the death record's flag), AI+0x8C, and the health Life. Any change is logged
// so a death transition can't fall between the 6 Hz discovery samples.
static void PollTrackedGuards() {
    for (int i = 0; i < s_dwN; ++i) {
        DieWatch& w = s_dw[i];
        if (!w.ai || !readable((void*)w.ai, 0x9C)) continue;
        unsigned int ptr98 = *(unsigned int*)(w.ai + 0x98);
        unsigned int b8c   = *(unsigned char*)(w.ai + 0x8C);
        unsigned int life  = 0xFFFFFFFFu;
        unsigned int flags = 0;
        if (w.health && readable((void*)w.health, 0x70)) {
            life  = *(unsigned short*)(w.health + 0x5C);
            flags = *(unsigned int*)(w.health + 0x60);
        }
        unsigned int byte98 = 0xFFFFFFFFu;
        if (ptr98 && readable((void*)ptr98, 4)) byte98 = *(unsigned char*)ptr98;
        if (ptr98 != w.lastPtr98 || b8c != (unsigned)w.dead || life != w.lastLife || byte98 != w.lastByte98) {
            L("[P98] ent=%08X ai=%08X ptr98=%08X byte98=%08X ai8C=%u life=%u flags=%08X health=%08X alias=%u\n",
              w.ent, w.ai, ptr98, byte98, b8c, life, flags, w.health, GuardActionAlias((Entity*)w.ent));
            w.lastPtr98 = ptr98; w.lastByte98 = byte98; w.lastLife = life;
        }
        if (life == 0xFFFFu && b8c) {
            static int s_logged = 0;
            if (s_logged < 8) { ++s_logged; L("[P98] *** DEAD ent=%08X life=FFFF ai8C=1 ptr98=%08X byte98=%08X\n", w.ent, ptr98, byte98); }
        }
        w.dead = b8c ? 1 : 0;
    }
}

// ---- Ship hull HP probe (F3) ------------------------------------------------------------------
// The old guess (*(ship+0x5C)+0x84) is a regenerating physics/repair value, not hull HP. Ships carry a
// CSrvHealth (vtable 0x02727088) the way characters carry CSrvNPCHealth (0x02712F60). Baseline/diff
// every such object while the ship takes damage, so the real hull field identifies itself.
struct ShipHp { unsigned int addr; unsigned int vt; unsigned short life; unsigned short hpMax; unsigned int flags; };
static ShipHp s_shPrev[64];
static int     s_shPrevN = 0;
static int     s_shDumps = 0;

static int ScanShipHealth(ShipHp* out, int cap) {
    int n = 0;
    __try {
        SYSTEM_INFO si; GetSystemInfo(&si);
        unsigned char* p = (unsigned char*)si.lpMinimumApplicationAddress;
        unsigned char* end = (unsigned char*)si.lpMaximumApplicationAddress;
        while (p < end && n < cap) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
                unsigned int* q = (unsigned int*)mbi.BaseAddress;
                unsigned int* qe = (unsigned int*)((char*)mbi.BaseAddress + mbi.RegionSize);
                for (; q + 1 < qe && n < cap; ++q) {
                    unsigned int vt = *q;
                    if (vt != 0x02727088u && vt != 0x02712F60u) continue;
                    unsigned char* o = (unsigned char*)q;
                    unsigned int flags = *(unsigned int*)(o + 0x60);
                    unsigned short life = *(unsigned short*)(o + 0x5C);
                    unsigned short mx   = *(unsigned short*)(o + 0x5E);
                    // Reject the noise: there are regions made of repeated vtable dwords (private vtable
                    // copies / padding) where 'flags' reads back as the vtable itself, and the
                    // life/max pair is nonsense. A real health has sane values.
                    if (flags == vt) continue;
                    if (mx == 0 || mx > 20000) continue;
                    out[n].addr = (unsigned int)o; out[n].vt = vt;
                    out[n].life = life;
                    out[n].hpMax = mx;
                    out[n].flags = flags;
                    ++n;
                }
            }
            p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L("[ShipHP] EXC 0x%08X\n", GetExceptionCode()); }
    return n;
}

static void ShipHealthDiff() {
    static ShipHp cur[64];
    int n = ScanShipHealth(cur, 64);
    if (s_shDumps == 0) {
        L("[ShipHP] BASELINE %d health object(s)\n", n);
        for (int i = 0; i < n; ++i)
            L("[ShipHP]  [%2d] obj=%08X vt=%08X life=%u/%u flags=%08X\n",
              i, cur[i].addr, cur[i].vt, cur[i].life, cur[i].hpMax, cur[i].flags);
        L("[ShipHP] now damage the ship, then press F3 again.\n");
    } else {
        int ch = 0;
        for (int i = 0; i < n; ++i) {
            const ShipHp* pv = 0;
            for (int j = 0; j < s_shPrevN; ++j) if (s_shPrev[j].addr == cur[i].addr) { pv = &s_shPrev[j]; break; }
            if (!pv) {
                L("[ShipHP] NEW obj=%08X vt=%08X life=%u/%u flags=%08X\n",
                  cur[i].addr, cur[i].vt, cur[i].life, cur[i].hpMax, cur[i].flags);
                ++ch;
            } else if (pv->life != cur[i].life || pv->hpMax != cur[i].hpMax || pv->flags != cur[i].flags) {
                L("[ShipHP] CHG obj=%08X vt=%08X life %u/%u -> %u/%u flags %08X -> %08X\n",
                  cur[i].addr, cur[i].vt, pv->life, pv->hpMax, cur[i].life, cur[i].hpMax, pv->flags, cur[i].flags);
                ++ch;
            }
        }
        L("[ShipHP] done: %d object(s), %d change(s)\n", n, ch);
    }
    s_shPrevN = n;
    for (int i = 0; i < n && i < 64; ++i) s_shPrev[i] = cur[i];
    ++s_shDumps;
}

// ---- Ship entity field diff (F3) ---------------------------------------------------------------
// Ruled out: hull HP is not a CSrvHealth/CSrvNPCHealth object (damaging the ship changed none of the 21
// live health objects). So diff the ship entity itself: snapshot its header + every component, take
// damage, diff again. Whatever the hull field is, it will show up as a changed byte.
struct ShipSnap {
    unsigned int ent;
    unsigned int comps[16];
    int ncomps;
    unsigned char entBuf[0x400];
    unsigned char compBuf[16][0x200];
};
static ShipSnap s_ss;
static int s_ssState = 0;

static void ShipEntityDiff() {
    Entity* ship = GetLocalShip();
    if (!ship) { L("[ShipDiff] no local ship (not aboard?)\n"); return; }

    if (s_ssState == 0) {
        memset(&s_ss, 0, sizeof(s_ss));
        s_ss.ent = (unsigned int)ship;
        Component** data = *(Component***)((char*)ship + 0x60);
        int n = (*(unsigned short*)((char*)ship + 0x64) & 0xFFFF);
        if (n <= 0 || n > 96) n = (*(unsigned short*)((char*)ship + 0x66) & 0xFFFF);
        if (data && readable(data, 4) && n > 0 && n <= 96) {
            for (int k = 0; k < n && s_ss.ncomps < 16; ++k) {
                Component* c = data[k];
                if (!c || !readable(c, 0x200)) continue;
                s_ss.comps[s_ss.ncomps++] = (unsigned int)c;
            }
        }
        if (readable((void*)s_ss.ent, 0x400)) memcpy(s_ss.entBuf, (void*)s_ss.ent, 0x400);
        for (int i = 0; i < s_ss.ncomps; ++i) memcpy(s_ss.compBuf[i], (void*)s_ss.comps[i], 0x200);
        L("[ShipDiff] BASELINE ship=%08X comps=%d — damage the ship, then press F3 again\n",
          s_ss.ent, s_ss.ncomps);
        // Also try the guard-style route: does the ship's AI carry a health service in ai->[+0x70]?
        {
            unsigned int ai = (unsigned int)AIEntityHelper::GetEntityAI(ship);
            L("[ShipDiff] ship ai=%08X\n", ai);
            if (ai && readable((void*)ai, 0x78)) {
                unsigned int arr = *(unsigned int*)(ai + 0x70);
                unsigned int n2  = *(unsigned short*)(ai + 0x76);
                L("[ShipDiff] ai service table=%08X n=%u\n", arr, n2);
                if (arr && readable((void*)arr, 4) && n2 > 0 && n2 <= 64) {
                    for (unsigned int k = 0; k < n2; ++k) {
                        unsigned int svc = *(unsigned int*)(arr + k * 4);
                        if (!svc || !readable((void*)svc, 0x70)) continue;
                        L("[ShipDiff]   svc[%u]=%08X vt=%08X life=%u/%u flags=%08X\n",
                          k, svc, *(unsigned int*)svc,
                          *(unsigned short*)(svc + 0x5C), *(unsigned short*)(svc + 0x5E),
                          *(unsigned int*)(svc + 0x60));
                    }
                }
            }
        }
        s_ssState = 1;
        return;
    }

    L("[ShipDiff] DIFF ship=%08X\n", s_ss.ent);
    int ch = 0;
    if (readable((void*)s_ss.ent, 0x400)) {
        unsigned char* now = (unsigned char*)s_ss.ent;
        for (int i = 0; i < 0x400 && ch < 80; ++i)
            if (now[i] != s_ss.entBuf[i]) { L("[ShipDiff]  ENT+%03X %02X -> %02X\n", i, s_ss.entBuf[i], now[i]); ++ch; }
    } else L("[ShipDiff] ship no longer readable\n");
    for (int k = 0; k < s_ss.ncomps && ch < 160; ++k) {
        unsigned int c = s_ss.comps[k];
        if (!readable((void*)c, 0x200)) { L("[ShipDiff]  comp[%d]=%08X gone\n", k, c); continue; }
        unsigned int vt = *(unsigned int*)c;
        unsigned char* now = (unsigned char*)c;
        for (int i = 0; i < 0x200 && ch < 200; ++i)
            if (now[i] != s_ss.compBuf[k][i])
                { L("[ShipDiff]  C%d(vt=%08X)+%03X %02X -> %02X\n", k, vt, i, s_ss.compBuf[k][i], now[i]); ++ch; }
    }
    L("[ShipDiff] done: %d change(s)\n", ch);
    s_ssState = 0;
}

// PLAN-v3 gate probe: is pad 1 bound to the puppet, and is the world still sane afterwards?
static void ProbePadGate() {
    World* world = World::GetCurrentWorld();
    if (!world) { L("[Gate] no world\n"); return; }
    PlayerPadManager* pm = PlayerPadManager::FromWorld(world);
    L("[Gate] padManager=%p\n", pm);
    if (!readable(pm, 0x40)) { L("[Gate] pad manager unreadable\n"); return; }
    for (int i = 0; i < 4; ++i) {
        void* e = ((void**)pm)[i];
        L("[Gate] pad[%d] = %p%s\n", i, e, (e && e == g_padGatePuppet) ? "  (puppet)" : "");
    }
    L("[Gate] G1 pad[1]==puppet: %s\n", (((void**)pm)[1] == g_padGatePuppet) ? "PASS" : "FAIL");
    L("[Gate] world characterList=%d entities=%d\n",
      world->m_CharacterList.GetSize(), world->m_EntityManager.m_Entities.GetSize());
    if (g_padGatePuppet && readable(g_padGatePuppet, 4)) {
        Entity* pe = (Entity*)g_padGatePuppet;
        L("[Gate] puppet still valid: pos=(%.1f,%.1f,%.1f) ai=%08X human=%08X\n",
          pe->m_GlobalMatrix.M41, pe->m_GlobalMatrix.M42, pe->m_GlobalMatrix.M43,
          (unsigned)AIEntityHelper::GetEntityAI(pe), (unsigned)AIEntityHelper::GetHuman(pe));
    } else {
        L("[Gate] puppet no longer valid\n");
    }
    // Did the engine discover a second device? (560-byte records, XInput slot at +532)
    {
        unsigned int dm = *(unsigned int*)0x4DD5EDC;
        if (dm && readable((void*)dm, 0x780)) {
            unsigned int arr = *(unsigned int*)(dm + 1896);
            unsigned int n   = *(unsigned int*)(dm + 1908);
            L("[Gate] devMgr=%08X arr=%08X count(+1902)=%u count(+1908)=%u\n",
              dm, arr, *(unsigned short*)(dm + 1902), n);
            if (arr && readable((void*)arr, 4) && n > 0 && n <= 8) {
                for (unsigned int i = 0; i < n; ++i) {
                    unsigned int e = arr + 560 * i;
                    if (!readable((void*)e, 560)) break;
                    L("[Gate] dev[%u] type(+4)=%u xinputIdx(+532)=%u\n",
                      i, *(unsigned int*)(e + 4), *(unsigned int*)(e + 532));
                }
            }
        }
    }
    L("[Gate] G2: if the game is still running and pad 0 still controls the player -> PASS\n");
}

static void ProbeHealthHeap() {    static HealthEnt cur[128];
    int n = ScanNPCHealth(cur, 128);
    if (g_hDumps == 0) {
        L("[HealthDiff] BASELINE: %d NPC health object(s)\n", n);
        for (int i = 0; i < n; ++i)
            L("[HealthDiff]  [%2d] obj=%08X life=%u/%u flags=%08X\n",
              i, cur[i].addr, cur[i].life, cur[i].hpMax, cur[i].flags);
        L("[HealthDiff] now hit a guard, then press F2 again to diff.\n");
    } else {
        int changed = 0;
        for (int i = 0; i < n; ++i) {
            const HealthEnt* prev = nullptr;
            for (int j = 0; j < g_hPrevN; ++j)
                if (g_hPrev[j].addr == cur[i].addr) { prev = &g_hPrev[j]; break; }
            if (!prev) {
                L("[HealthDiff] NEW  obj=%08X life=%u/%u flags=%08X\n", cur[i].addr, cur[i].life, cur[i].hpMax, cur[i].flags);
                ++changed;
            } else if (prev->life != cur[i].life || prev->hpMax != cur[i].hpMax || prev->flags != cur[i].flags) {
                L("[HealthDiff] CHG  obj=%08X life %u/%u -> %u/%u  flags %08X -> %08X\n",
                  cur[i].addr, prev->life, prev->hpMax, cur[i].life, cur[i].hpMax, prev->flags, cur[i].flags);
                ++changed;
            }
        }
        for (int j = 0; j < g_hPrevN; ++j) {
            bool gone = true;
            for (int i = 0; i < n; ++i) if (cur[i].addr == g_hPrev[j].addr) { gone = false; break; }
            if (gone) { L("[HealthDiff] GONE obj=%08X\n", g_hPrev[j].addr); ++changed; }
        }
        L("[HealthDiff] done: %d object(s), %d change(s)\n", n, changed);
    }
    g_hPrevN = n;
    for (int i = 0; i < n; ++i) g_hPrev[i] = cur[i];
    ++g_hDumps;
}

// ---- Input-state probe (F3): dump the input manager + device manager + device input state, to pin
// the per-pad buttons + stick layout for the input relay. Read-only. ------------------------------
static void ProbeInputState() {
    __try {
        unsigned int im = *(unsigned int*)0x2ABE524;
        L("[Input] inputMgr(dword_2ABE524)=%08X\n", im);
        if (im && readable((void*)im, 0xA8)) {
            for (int off = 0; off < 0xA8; off += 16) {
                unsigned char* p = (unsigned char*)im + off;
                L("[Input] im+%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  off, p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
            }
        }
        unsigned int dm = *(unsigned int*)0x4DD5EDC;
        L("[Input] devMgr(dword_4DD5EDC)=%08X\n", dm);
        if (dm && readable((void*)dm, 0x40)) {
            unsigned int dev = *(unsigned int*)((char*)dm + 0x0C);
            L("[Input]   devMgr+0C(dev)=%08X\n", dev);
            if (dev && readable((void*)dev, 0x10)) {
                unsigned int st = *(unsigned int*)((char*)dev + 4);
                L("[Input]   dev+4(state)=%08X\n", st);
                if (st && readable((void*)st, 0x800)) {
                    L("[Input]   stick@0x764=%d\n", *(int*)((char*)st + 0x764));
                    for (int off = 0; off < 0x100; off += 16) {
                        unsigned char* p = (unsigned char*)st + off;
                        L("[Input]   st+%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                          off, p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
                    }
                    for (int off = 0x700; off < 0x800; off += 16) {
                        unsigned char* p = (unsigned char*)st + off;
                        L("[Input]   st+%03X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                          off, p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
                    }
                }
            }
        }
        World* world = World::GetCurrentWorld();
        if (world && readable((char*)world + 0x7F8, 4)) {
            unsigned int holder = *(unsigned int*)((char*)world + 0x7F8);
            L("[Input] world+7F8 holder=%08X\n", holder);
            if (holder && readable((void*)holder, 0x40)) {
                for (int off = 0; off < 0x40; off += 4)
                    L("[Input]   holder+%02X=%08X\n", off, *(unsigned int*)((char*)holder + off));
                unsigned int ppm = *(unsigned int*)((char*)holder + 4);
                if (ppm && readable((void*)ppm, 0x20)) {
                    for (int pad = 0; pad < 8; ++pad)
                        L("[Input]   pad[%d]=%08X\n", pad, *(unsigned int*)((char*)ppm + pad*4));
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Input] EXCEPTION code=0x%08X\n", GetExceptionCode());
    }
}

// The Human-update hook fires for EVERY human each frame; only act for the player's human so our
// logic runs exactly once per frame (spawn/send/apply must not run dozens of times per frame).
static bool isPlayerUpdate(uintptr_t b, Human* updatedHuman) {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return false;

        Entity* player = world->GetPlayer();
        if (!player)
            return false;

        return AIEntityHelper::GetHuman(player) == updatedHuman;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- snapshot interpolation (main-thread only; no locking needed) ------------------
// Buffer the last N received remote states with their local arrival time, then each frame render
// the puppet at (now - g_interpDelayMs), lerping between the two snapshots that bracket that time.
struct Snap { PlayerState st; uint64_t t; };
static const int SNAP_MAX = 32;
static Snap     g_snaps[SNAP_MAX];
static int      g_snapCount = 0;   // filled entries (<= SNAP_MAX)
static int      g_snapHead  = 0;   // ring index of the next write slot
static uint32_t g_lastSnapSeq = 0;
static bool     g_haveSnapSeq = false;

static void norm3(float* v) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Blend two states. Start from the newer (copies seq/worldId/contextId/flags/bytes/valid — these are
// discrete and must NOT be interpolated), then lerp the continuous fields: position, orientation
// (per-row matrix nlerp in the game's native basis), speeds and heading.
static void lerpState(const PlayerState& a, const PlayerState& b, float t, PlayerState& o) {
    o = b;
    if (a.worldId != b.worldId) return;   // spanning a world change -> just show the newer state

    // Position: straight lerp of the translation row.
    float px = lerpf(a.matrix.M41, b.matrix.M41, t);
    float py = lerpf(a.matrix.M42, b.matrix.M42, t);
    float pz = lerpf(a.matrix.M43, b.matrix.M43, t);

    // Orientation: interpolate the rotation basis (rows 0-2) DIRECTLY in the game's native matrix space —
    // per-component lerp each row then renormalize (nlerp). Do NOT round-trip through the engine quat<->
    // matrix ops (FromMatrix/ToMatrix): they use a different basis convention and left the puppet rotated
    // 90 deg (lying on the floor, then culled). Across one interp interval the rotation delta is tiny, so
    // nlerped rows stay effectively orthonormal, and this stays in the exact basis the working direct-copy
    // apply path uses. M14/M24/M34 keep b's values (0 for an affine transform, copied by o = b above).
    o.matrix.M11 = lerpf(a.matrix.M11, b.matrix.M11, t);
    o.matrix.M12 = lerpf(a.matrix.M12, b.matrix.M12, t);
    o.matrix.M13 = lerpf(a.matrix.M13, b.matrix.M13, t);
    norm3(&o.matrix.M11);
    o.matrix.M21 = lerpf(a.matrix.M21, b.matrix.M21, t);
    o.matrix.M22 = lerpf(a.matrix.M22, b.matrix.M22, t);
    o.matrix.M23 = lerpf(a.matrix.M23, b.matrix.M23, t);
    norm3(&o.matrix.M21);
    o.matrix.M31 = lerpf(a.matrix.M31, b.matrix.M31, t);
    o.matrix.M32 = lerpf(a.matrix.M32, b.matrix.M32, t);
    o.matrix.M33 = lerpf(a.matrix.M33, b.matrix.M33, t);
    norm3(&o.matrix.M31);
    o.matrix.M41 = px; o.matrix.M42 = py; o.matrix.M43 = pz; o.matrix.M44 = 1.0f;

    o.curSpeed  = lerpf(a.curSpeed,  b.curSpeed,  t);
    o.destSpeed = lerpf(a.destSpeed, b.destSpeed, t);

    // Heading is a direction: component-lerp xyz then renormalize (keeps .w from the newer snapshot).
    o.heading.x = lerpf(a.heading.x, b.heading.x, t);
    o.heading.y = lerpf(a.heading.y, b.heading.y, t);
    o.heading.z = lerpf(a.heading.z, b.heading.z, t);
    norm3(&o.heading.x);

    // Blend factors are continuous, but only comparable when both snapshots are on the SAME action+channel
    // (they index that action's clips). Same action -> lerp for a smooth speed ramp; different action ->
    // keep b's (the alias we're switching to, copied by o = b) so we don't cross-blend unrelated clips.
    if (a.actionAlias == b.actionAlias && a.actionChannel == b.actionChannel && a.blendCount == b.blendCount) {
        for (int i = 0; i < b.blendCount; ++i) o.blendFactors[i] = lerpf(a.blendFactors[i], b.blendFactors[i], t);
        o.timeScale = lerpf(a.timeScale, b.timeScale, t);   // clip speed is continuous too
        // Phase: lerp unless the clip looped between snapshots (b < a), then keep b's (o = b) — never
        // interpolate backwards across the wrap.
        if (b.actionRatio >= a.actionRatio) o.actionRatio = lerpf(a.actionRatio, b.actionRatio, t);
    }
}

// Append a freshly-received packet. Dedupes by seq; a backwards seq (peer reconnect) resets the buffer.
static void PushSnapshot(const PlayerState& s, uint64_t now) {
    if (g_haveSnapSeq) {
        if (s.seq == g_lastSnapSeq) return;            // duplicate of the newest — GetRemote holds it
        if (s.seq <  g_lastSnapSeq) { g_snapCount = 0; g_snapHead = 0; }  // reset/reconnect
    }
    g_snaps[g_snapHead].st = s;
    g_snaps[g_snapHead].t  = now;
    g_snapHead = (g_snapHead + 1) % SNAP_MAX;
    if (g_snapCount < SNAP_MAX) ++g_snapCount;
    g_lastSnapSeq = s.seq; g_haveSnapSeq = true;
}

// Produce the interpolated state for render time (now - delay). Holds the newest state when render
// time runs past it (packet loss/stall) and clamps to the oldest before the buffer fills.
static bool ComputeInterpolated(uint64_t now, PlayerState& out) {
    if (g_snapCount == 0) return false;
    int newestIdx = (g_snapHead - 1 + SNAP_MAX) % SNAP_MAX;
    int oldestIdx = (g_snapHead - g_snapCount + SNAP_MAX) % SNAP_MAX;
    uint64_t renderT = now > (uint64_t)g_interpDelayMs ? now - (uint64_t)g_interpDelayMs : 0;

    if (g_snapCount == 1 || renderT >= g_snaps[newestIdx].t) { out = g_snaps[newestIdx].st; return true; }
    if (renderT <= g_snaps[oldestIdx].t)                     { out = g_snaps[oldestIdx].st; return true; }
    for (int i = 0; i < g_snapCount - 1; ++i) {
        int ai = (oldestIdx + i)     % SNAP_MAX;
        int bi = (oldestIdx + i + 1) % SNAP_MAX;
        uint64_t ta = g_snaps[ai].t, tb = g_snaps[bi].t;
        if (renderT >= ta && renderT <= tb) {
            float t = (tb > ta) ? (float)(renderT - ta) / (float)(tb - ta) : 1.0f;
            lerpState(g_snaps[ai].st, g_snaps[bi].st, t, out);
            return true;
        }
    }
    out = g_snaps[newestIdx].st; return true;         // shouldn't reach; be safe
}

// True while the engine is streaming/loading a world. Guards puppet construction so we never build an
// entity mid-load (EntityBuilder during a load transition is unsafe). Static read of g_LoadingThread.
static bool engineLoading() {
    __try { return LoadingManager::IsLoading(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// F9 scene/interior probe (observation only): one guarded [Scene] block. Press F9 outdoors, then again
// indoors, and compare computedSceneKey / lastLoadCell / whiteRoom to see whether the scene key really
// distinguishes an interior. No behaviour change.
void Phase1::TrySceneProbe() {
    __try {
        World*  world  = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;

        uint64_t worldId    = world ? world->GetObjectID() : 0;
        uint32_t cellSize   = 0;
        int      lastLoadCell = -1;
        // GridPartition::m_CellsLoadStatusDataBlock is NOT reachable from World in these headers (it sits
        // behind World::m_Grid, a Reference we cannot resolve here) — left as -1 rather than guessing.
        int      cellsBlock   = -1;
        int      worldAreas   = -1;
        if (readable(world, 8)) {
            cellSize   = world->m_FloorHeightData.m_CellSize;
            worldAreas = world->m_WorldAreas.GetSize();
            if (readable(world->m_LoadingAdvisor, 0x100))
                lastLoadCell = world->m_LoadingAdvisor->m_LastLoadPositionCellID;
        }
        L("[Scene] world=%p worldId=%08X%08X cellSize=%u lastLoadCell=%d cellsBlock=%d\n",
          world, (unsigned)(worldId >> 32), (unsigned)worldId, (unsigned)cellSize, lastLoadCell, cellsBlock);

        float px = 0.0f, py = 0.0f, pz = 0.0f;
        int   fakeCell = -1;
        if (readable(player, 8)) {
            px = player->m_GlobalMatrix.M41;
            py = player->m_GlobalMatrix.M42;
            pz = player->m_GlobalMatrix.M43;
            fakeCell = player->m_FakeCellIndex;
        }
        uint32_t key = (world && player) ? computeSceneKey(world, player) : 0u;
        L("[Scene] player=%p pos=(%.1f,%.1f,%.1f) computedSceneKey=%08X\n", player, px, py, pz, key);
        L("[Scene] entFakeCellIdx=%d\n", fakeCell);
        L("[Scene] worldAreasCount=%d\n", worldAreas);
        L("[Scene] whiteRoom=%d loading=%d\n",
          world ? (inWhiteRoom(world) ? 1 : 0) : -1, engineLoading() ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Scene] EXCEPTION during scene probe (code=0x%08X)\n", GetExceptionCode());
    }
}

// F10 ship/boarding probe (observation only): one guarded [Ship] block. Resolves the player Human* exactly
// like F7/F9 do (World::GetCurrentWorld() -> World::GetPlayer() -> AIEntityHelper::GetHuman), then reads the
// already-mapped ship fields from Human.h (m_IsOnShip / m_IsOnShipEntity / m_IsOnOcean / m_Vehicle /
// m_DrivableShipWheel / m_ExitShipTimer). Press F10 on foot, then again aboard a ship before and after
// taking damage: comparing the ent header dump across presses spots the global matrix and a u16 HP field.
// Every pointer read is readable()-guarded and the whole body sits in an SEH __try. No behaviour change.
// F11 ship-spawn request (observation only). Asks the engine (FUN_01082810 @ VA 0x01082810) to spawn a
// "ship-dude", then best-effort sets the type field at req+0xC to 0x13 so the USER can look at the game
// and report what actually appears (the gate question for real ship sync). Does NOT touch the wire or any
// co-op state. Runs on the main thread (via the frame hook), like the other probes.
void Phase1::TryShipSpawn() {
    __try {
        L("[ShipSpawn] requesting ship-dude spawn (FUN_01082810)\n");

        typedef int (__cdecl *ShipSpawnReqFn)();
        ShipSpawnReqFn fn = (ShipSpawnReqFn)(base() + 0x00C82810);

        void*    req = nullptr;
        unsigned exc = 0;
        __try {
            req = (void*)(intptr_t)fn();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = (unsigned)GetExceptionCode();
        }

        L("[ShipSpawn] FUN_01082810 -> req=%p exc=0x%08X\n", req, exc);

        if (req && readable(req, 0x10)) {
            *(int*)((char*)req + 0xC) = 0x13;
            L("[ShipSpawn] set type=0x13 at req+0xC\n");
        }

        L("[ShipSpawn] observe the game now - did a ship/dude/boat appear?\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[ShipSpawn] EXCEPTION (code=0x%08X)\n", (unsigned)GetExceptionCode());
    }
}

// F13 ship-drive probe (observation only): resolve the LOCAL ship exactly like TryShipProbe
// (World::GetCurrentWorld() -> World::GetPlayer() -> AIEntityHelper::GetHuman -> handleResolve(m_IsOnShipEntity)),
// then WRITE +50 in world X onto its transform (BaseEntity::m_GlobalMatrix.M41). This is the writable-transform
// GO/NO-GO for shared-ship co-op: if the ship jumps +50 and STAYS, physics does not own the transform and we can
// drive it; if it SNAPS BACK (or the game crashes), the physics controller owns the transform. Every read/write
// is readable()-guarded and the whole body sits in an SEH __try. No wire/co-op state is touched.
void Phase1::TryShipDriveProbe() {
    __try {
        World*  world  = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;
        Human*  h      = player ? AIEntityHelper::GetHuman(player) : nullptr;

        if (!h) {
            L("[ShipDrive] no ship (not aboard?) human=%p world=%p player=%p\n", h, world, player);
            return;
        }

        void* shipEnt = handleResolve(h->m_IsOnShipEntity);
        if (!shipEnt) shipEnt = handleResolve(h->m_Data.m_Vehicle);   // fallback when the on-ship handle is empty
        if (!shipEnt) {
            L("[ShipHold] no ship (not aboard?)\n");
            return;
        }

        // F5 = HOLD: capture the ship's world position, then OVERRIDE every world-transform write
        // (in the SetGlobalMatrixInternal hook) with that position — proof we can drive the ship.
        if (g_holdShip) {
            g_holdShip = 0;
            g_holdShipEnt = nullptr;
            L("[ShipHold] RELEASED\n");
        } else {
            HookMath::Transform* gm = &((BaseEntity*)shipEnt)->m_GlobalMatrix;
            if (!readable(gm, sizeof(HookMath::Transform))) { L("[ShipHold] transform unreadable\n"); return; }
            g_holdShipEnt = shipEnt;
            g_holdPos[0] = gm->M41; g_holdPos[1] = gm->M42; g_holdPos[2] = gm->M43;
            g_holdShip = 1;
            L("[ShipHold] HOLD ship=%p at (%.1f,%.1f,%.1f)\n", shipEnt, g_holdPos[0], g_holdPos[1], g_holdPos[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[ShipHold] EXCEPTION (code=0x%08X)\n", (unsigned)GetExceptionCode());
    }
}

void Phase1::TryShipProbe() {
    __try {
        World*  world  = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;
        Human*  h      = player ? AIEntityHelper::GetHuman(player) : nullptr;

        if (!h) {
            L("[Ship] no ship resolved (not aboard?) human=%p world=%p player=%p\n", h, world, player);
            return;
        }

        bool  isOnShip  = h->m_IsOnShip;
        bool  isOnOcean = h->m_IsOnOcean;

        // Raw first word of m_ExitShipTimer (its Timer layout is not mapped in these headers; logged as a
        // raw value only, so we can watch whether it is counting while boarding/leaving a ship).
        uint32_t exitShipTimerRaw = readable((const void*)&h->m_ExitShipTimer, 4)
                                  ? *(uint32_t*)&h->m_ExitShipTimer : 0;

        void* vehicle = handleResolve(h->m_Data.m_Vehicle);
        void* wheel   = handleResolve(h->m_Data.m_DrivableShipWheel);
        void* shipEnt = handleResolve(h->m_IsOnShipEntity);
        if (!shipEnt) shipEnt = vehicle;   // fallback when the on-ship handle is empty
        uint32_t shipEntVt = readable(shipEnt, 8) ? *(uint32_t*)shipEnt : 0;

        L("[Ship] human=%p isOnShip=%d isOnOcean=%d shipEnt=%p shipEntVt=%08X vehicle=%p drivableWheel=%p\n",
          h, (int)isOnShip, (int)isOnOcean, shipEnt, shipEntVt, vehicle, wheel);
        L("[Ship] exitShipTimerRaw=%08X\n", exitShipTimerRaw);

        if (!shipEnt) {
            L("[Ship] no ship resolved (not aboard?)\n");
            return;
        }

        // Header dump ent+0x00..0x400, 16 u32 per line. readable-guarded: 0 where unreadable.
        for (int row = 0; row < 16; ++row) {
            uint32_t v[16];
            for (int i = 0; i < 16; ++i) {
                uintptr_t off = (uintptr_t)(row * 0x40 + i * 4);
                v[i] = readable((char*)shipEnt + off, 4) ? *(uint32_t*)((char*)shipEnt + off) : 0;
            }
            L("[Ship] ent+0x%02X: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
              (unsigned)(row * 0x40),
              v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
              v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
        }

        // Global position via the same matrix accessor the mod uses on entities (BaseEntity::m_GlobalMatrix).
        if (readable(shipEnt, 0x100)) {
            L("[Ship] pos=(%.1f,%.1f,%.1f)\n",
              ((BaseEntity*)shipEnt)->m_GlobalMatrix.M41,
              ((BaseEntity*)shipEnt)->m_GlobalMatrix.M42,
              ((BaseEntity*)shipEnt)->m_GlobalMatrix.M43);
        } else {
            L("[Ship] pos=unavailable\n");
        }

        // Ship gameplay sub-object: FUN_00fb3220(ship, 0). Try __thiscall first (flag on stack),
        // then __fastcall (flag in DL) — convention was ambiguous; log whichever succeeds.
        {
            typedef int (__thiscall *ShipSubFn_t)(void* ship, char flag);
            typedef int (__fastcall *ShipSubFn_f)(void* ship, char flag);
            int   sub = 0;
            DWORD exc = 0;
            __try { sub = ((ShipSubFn_t)(base() + 0x00FB3220))(shipEnt, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { exc = GetExceptionCode(); }
            if (exc || !readable((void*)(uintptr_t)sub, 8)) {
                L("[Ship] sub thiscall: exc=0x%08X sub=%p (falling back to fastcall)\n", exc, (void*)(uintptr_t)sub);
                exc = 0;
                __try { sub = ((ShipSubFn_f)(base() + 0x00FB3220))(shipEnt, 0); }
                __except (EXCEPTION_EXECUTE_HANDLER) { exc = GetExceptionCode(); }
            }
            void* subp = (void*)(uintptr_t)sub;
            if (exc || !readable(subp, 8)) {
                L("[Ship] sub=EXCEPTION/unreadable (exc=0x%08X)\n", exc);
            } else {
                uint32_t subVt = *(uint32_t*)subp;
                L("[Ship] sub=%p subVt=%08X\n", subp, subVt);
                for (int row = 0; row < 8; ++row) {   // sub+0x00..0x80, 4 u32 per line
                    uint32_t v[4];
                    for (int i = 0; i < 4; ++i) {
                        uintptr_t off = (uintptr_t)(row * 0x10 + i * 4);
                        v[i] = readable((char*)subp + off, 4) ? *(uint32_t*)((char*)subp + off) : 0;
                    }
                    L("[Ship] sub+0x%02X: %08X %08X %08X %08X\n",
                      (unsigned)(row * 0x10), v[0], v[1], v[2], v[3]);
                }
            }
        }

        // Walk the ship entity's first 0x200 bytes for pointers to objects with a .rdata vtable
        // (its components). The ship HP (ShipHealthData / GcLNavalHealth) is behind one of these.
        {
            int dumped = 0;
            for (uintptr_t off = 0; off < 0x400 && dumped < 12; off += 4) {
                void* p = readable((char*)shipEnt + off, 4) ? *(void**)((char*)shipEnt + off) : nullptr;
                if (!p || !readable(p, 4)) continue;
                uint32_t vt = *(uint32_t*)p;
                if (vt < 0x01E00000u || vt > 0x02C00000u) continue;   // .rdata vtable range
                L("[Ship] @+0x%X->%p vt=%08X\n", (unsigned)off, p, vt);
                for (int row = 0; row < 4; ++row) {   // component +0x00..0x100, 16 u32 per line
                    uint32_t v[16];
                    for (int i = 0; i < 16; ++i) {
                        uintptr_t o = (uintptr_t)(row * 0x40 + i * 4);
                        v[i] = readable((char*)p + o, 4) ? *(uint32_t*)((char*)p + o) : 0;
                    }
                    L("[Ship]   +0x%02X: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
                      (unsigned)(row * 0x40),
                      v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15]);
                }
                ++dumped;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Ship] EXCEPTION during ship probe (code=0x%08X)\n", GetExceptionCode());
    }
}

// Runs on the main thread once per frame (only for the player's Human update).
static void __cdecl OnFrame(Human* updatedHuman) {
    if (!isPlayerUpdate(base(), updatedHuman)) return;

    // Keep g_localShipZ fresh every frame (not just inside the net capture), so the ghost's render
    // override has a live wave height even when Net is off (single-player F12 test). The ghost's
    // physics is disabled (m_DriveEntity=false) so nothing else drives its Z -> must feed it here.
    {
        void* ship = handleResolve(updatedHuman->m_IsOnShipEntity);
        if (!ship) ship = handleResolve(updatedHuman->m_Data.m_Vehicle);
        if (ship && readable(ship, sizeof(HookMath::Transform)))
            g_localShipZ = ((BaseEntity*)ship)->m_GlobalMatrix.M43;
    }

    // One-shot entity-enumeration probe (verifies the world's entity-list offsets live).
    { static bool s_enumDone = false; if (!s_enumDone) { s_enumDone = true; ProbeEntityEnum(); } }

    // Start the UDP link on the first REAL in-world frame, not at DLL attach: the launch chain's
    // transient processes bind the port and die (the live game's bind then fails with 10048 and
    // it gets no socket). They never reach a player frame, so binding here means only the live
    // game ever opens the socket, and there is no corpse socket to steal its datagrams.
    static bool s_netStarted = false;
    static int  s_netStrictPeer = 1;   // ini [Coop] NetStrictPeer (Feature #3) — read once
    static int  s_sceneAware    = 1;   // ini [Coop] SceneAware    (Feature #1) — read once
    static int  s_shipPassenger = 0;   // ini [Coop] ShipPassenger — read once (default OFF)
    static int  s_shipGhost     = 0;   // ini [Coop] ShipGhost — ghost-ship clone (default OFF; fragile on world travel)
    static int  s_ghostSelfTest = 0;   // ini [Coop] ShipGhostSelfTest — drive the ghost to the LOCAL ship (solo verify)
    if (!s_netStarted) {
        s_netStarted    = true;
        s_netStrictPeer = GetPrivateProfileIntA("Coop", "NetStrictPeer", 1, ".\\AC4Coop.ini");
        s_sceneAware    = GetPrivateProfileIntA("Coop", "SceneAware",    1, ".\\AC4Coop.ini");
        s_shipPassenger = GetPrivateProfileIntA("Coop", "ShipPassenger", 0, ".\\AC4Coop.ini");
        s_shipGhost     = GetPrivateProfileIntA("Coop", "ShipGhost",     0, ".\\AC4Coop.ini");
        s_ghostSelfTest = GetPrivateProfileIntA("Coop", "ShipGhostSelfTest", 0, ".\\AC4Coop.ini");
        Net::SetStrictPeer(s_netStrictPeer != 0);   // before Init: the recv thread filters from the start
        L("[Net] init=%d  (bound/peer line goes to OutputDebugString)\n", Net::Init() ? 1 : 0);
    }

    if (g_dumpReq) { g_dumpReq = 0; Phase1::DumpPlayerInfo(); }   // F7: works in net + local modes
    if (g_crowdReq) { g_crowdReq = 0; Phase1::TryCrowdProbe(); }  // F8: crowd body probe (net + local)
    if (g_sceneReq) { g_sceneReq = 0; Phase1::TrySceneProbe(); }  // F9: scene/interior probe (net + local)
    if (g_shipReq)  { g_shipReq  = 0; Phase1::TryShipProbe();  }  // F10: ship/boarding probe (net + local)
    if (g_ghostDespawnReq) { g_ghostDespawnReq = 0; Phase1::DespawnGhostShip(); }  // F11: remove the ghost ship
    if (g_shipSpawnReq) { g_shipSpawnReq = 0; Phase1::TryShipSpawn(); }  // (unbound) ship-dude spawn request (FUN_01082810)
    if (g_shipCtorReq)  { g_shipCtorReq  = 0; Phase1::TryShipDupProbe(); }  // F12: DUPLICATE the ship (spawn a 2nd ship) experiment
    if (g_shipDriveReq) { g_shipDriveReq = 0; Phase1::TryShipDriveProbe(); } // F13: write +50 X to local ship transform (writable?)
    if (g_enumReq) { g_enumReq = 0; ProbeEntityEnum(); }   // F14: entity-enumeration probe (net + local)
    if (g_inputReq) { g_inputReq = 0; ShipEntityDiff(); }  // F3: ship entity field diff (hull HP hunt)
    if (g_healthReq) { g_healthReq = 0; RelaySelfTest(); }  // F2: solo harness for the guest-side relay detection
    if (g_mapReq) { g_mapReq = 0; SimulatePeerKill(); }     // F1: self-test the position-based peer-kill path
    if (g_padGateFrames > 0 && --g_padGateFrames == 0) ProbePadGate();   // PLAN-v3 gate G1/G2
    if (s_posProbeFrames > 0 && --s_posProbeFrames == 0) FinishPadProbe(); // pad-1 ownership comparison
    if (s_contSamples > 0) {
        static int s_ctr = 0;
        if (++s_ctr >= 60) {
            s_ctr = 0;
            --s_contSamples;
            World* w = World::GetCurrentWorld();
            Entity* pl = w ? w->GetPlayer() : nullptr;
            float px = pl ? pl->m_GlobalMatrix.M41 : 0, pz = pl ? pl->m_GlobalMatrix.M43 : 0;
            float qx = 0, qz = 0;
            if (s_contEnt && readable(s_contEnt, 4)) {
                Entity* pe = (Entity*)s_contEnt;
                qx = pe->m_GlobalMatrix.M41; qz = pe->m_GlobalMatrix.M43;
            }
            L("[Drive] t=%d  player=(%.2f,%.2f)  puppet=(%.2f,%.2f)\n",
              10 - s_contSamples, px, pz, qx, qz);
        }
    }
    DrivePuppetFromPad();                                                // Route B: pad-1 -> puppet ground data
    // NOTE: the die-clip watchers (TickDieClipWatch / PollTrackedGuards) are intentionally DISABLED.
    // They did their job (captured the death clip alias 1818493434 and the dead-flag semantics), but
    // running a full entity scan + engine calls every few frames caused intermittent crashes during
    // normal play. Re-enable only for diagnostics.
    // TickDieClipWatch();
    // PollTrackedGuards();
    // Re-assert the guard page periodically: the heap can re-protect its pages, which silently drops
    // PAGE_GUARD and would make the trap look like "nothing happened".
    if (g_gpArmed && g_gpPage) {
        static uint32_t s_gpTick = 0;
        if ((++s_gpTick & 0xF) == 0) {
            DWORD oldProt = 0;
            VirtualProtect((void*)g_gpPage, 0x1000, PAGE_READWRITE | PAGE_GUARD, &oldProt);
        }
    }
    if (g_holdShip && g_holdShipEnt) {   // F5 hold test: pin the ship's position every frame (drive test)
        HookMath::Transform* hm = &((BaseEntity*)g_holdShipEnt)->m_GlobalMatrix;
        if (readable(hm, sizeof(HookMath::Transform))) {
            hm->M41 = g_holdPos[0]; hm->M42 = g_holdPos[1]; hm->M43 = g_holdPos[2];
        }
    }
    if (Net::Enabled()) {
        EntitySyncTick();   // host broadcasts entity transforms; peer mirrors them (synced worlds)
        // Capture only succeeds in a real playable world (not title/whiteroom). Require it to be
        // stable for a bit before spawning, so we never spawn during loads/menus.
        static int      stable       = 0;
        static uint64_t lastFreshMs  = 0;   // last time a fresh same-world packet arrived
        static uint32_t lastRawSeq   = 0;   // last raw packet seq seen (to detect fresh packets)
        static int      despawnGrace = 0;   // frames the puppet has wanted removal
        PlayerState local;
        bool localOk = Phase1::CaptureLocalState(local);
        if (localOk) { static uint32_t seq = 0; local.seq = ++seq; Net::Send(local); if (stable < 100000) ++stable; }
        else stable = 0;

        uint64_t now = GetTickCount64();
        bool loading = engineLoading();   // engine streaming/tearing down a world

        // Pull the newest raw packet; track freshness (seq advanced) and whether it's in OUR world.
        PlayerState raw;
        bool gotRaw = Net::GetRemote(raw) && raw.valid;

        // ---- Feature #3: protocol-version gate (NetStrictPeer / NetStrictPeer=0 disables the timer arm) ----
        // A peer from a different source has a different packed PlayerState layout: its fields decode as
        // garbage. Refuse to spawn/drive it at all rather than puppet a corrupt state.
        bool protoOk = gotRaw && raw.protoVer == NET_PROTO_VER;
        if (gotRaw && !protoOk) {
            static bool s_protoWarned = false;
            if (!s_protoWarned) {
                s_protoWarned = true;
                L("[Net] PEER PROTO MISMATCH (peer=%u local=%u) — refusing to spawn/drive. Update BOTH machines to the same build.\n",
                  (unsigned)raw.protoVer, (unsigned)NET_PROTO_VER);
            }
        }

        // ---- Feature #1: scene/interior awareness (SceneAware) ----
        // A peer in a different world cell is treated as NOT here: no drive, and the despawnGrace path
        // removes any stale puppet. A 0 key on either side means "unknown" -> allow, so an unreadable
        // cell size can never wedge the co-op. SceneAware=0 restores the old world-id-only behaviour.
        bool sceneKnown = gotRaw && localOk && raw.sceneKey != 0 && local.sceneKey != 0;
        bool sameScene  = (s_sceneAware == 0) || !sceneKnown || (raw.sceneKey == local.sceneKey);

        bool rawSameWorld = gotRaw && localOk && raw.worldId == local.worldId && protoOk && sameScene;
        if (gotRaw && raw.seq != lastRawSeq) {
            lastRawSeq = raw.seq;
            if (rawSameWorld) lastFreshMs = now;        // a fresh same-world packet keeps the puppet alive
            if (g_interpEnabled && protoOk) PushSnapshot(raw, now);
        }
        // Peer is "present" only with a recent fresh same-world packet AND (Feature #3) a live receive
        // timer — covers world change, disconnect, silence and unexpected-source drops, where GetRemote
        // keeps returning a stale last packet forever. With NetStrictPeer=0 the timer arm is inert, so
        // the condition reduces exactly to today's.
        bool timerOk = (s_netStrictPeer == 0) || (Net::MillisSinceRecv() < 2000);
        bool peerPresent = rawSameWorld && (now - lastFreshMs) < 2000 && timerOk;

        // Observability: log each scene transition once so the gating is visible in the log.
        {
            static uint32_t lastLocalKey = 0xFFFFFFFFu, lastPeerKey = 0xFFFFFFFFu;
            uint32_t lk = localOk ? local.sceneKey : 0xFFFFFFFFu;
            uint32_t pk = gotRaw  ? raw.sceneKey   : 0xFFFFFFFFu;
            if (lk != lastLocalKey || pk != lastPeerKey) {
                if (lastLocalKey != 0xFFFFFFFFu || lastPeerKey != 0xFFFFFFFFu)
                    L("[Net] scene change: localKey=%08X peerKey=%08X -> puppet %s\n",
                      lk, pk, (lk == pk || s_sceneAware == 0) ? "shown" : "hidden");
                lastLocalKey = lk; lastPeerKey = pk;
            }
        }

        // ---- puppet lifecycle ----
        // Liveness by vtable: g_puppet still points at the entity we built only while its vtbl pointer
        // is unchanged. A world change FREES it (memory unmapped, or reused with a different vtbl) so we
        // drop the dangling ref; a pause/menu leaves it INTACT so we preserve it. This distinguishes
        // "gone" from "paused" without relying on whiteroom/world-id detection (the pause menu defeats
        // that — its active world reads as a different real world, which previously mis-dropped on pause).
        bool puppetAlive = false;
        if (g_puppet) {
            __try { puppetAlive = readable(g_puppet, 8) && (*(void**)g_puppet == g_puppetVtbl); }
            __except (EXCEPTION_EXECUTE_HANDLER) { puppetAlive = false; }
        }
        if (g_puppet) {
            if (!puppetAlive) {                          // entity was freed (world change / teardown)
                L("[Net] puppet entity gone -> drop ref\n");
                g_puppet = nullptr; g_blockReg = nullptr; g_puppetWorldId = 0; g_puppetVtbl = nullptr;
                g_lastRemoteCtx = -1;                    // fresh puppet next time -> re-apply first context
                g_lastActionAlias = 0xFFFFFFFF;
                g_bhvState = -1; g_switchCd = 0;
                g_casActivated = false;
                g_spawnTries = 0; g_spawnCd = 180;       // let the new world's builder settle before respawn
                g_snapCount = 0; g_snapHead = 0; g_haveSnapSeq = false;
                despawnGrace = 0;
            } else if (!localOk) {
                despawnGrace = 0;                        // pause/menu/load, puppet intact -> preserve
            } else if (peerPresent) {
                despawnGrace = 0;                        // healthy: our world, peer present
            } else if (++despawnGrace > 180) {          // ~3s peer absent while we're live -> remove
                Phase1::DespawnPuppet();                 // peer left our world / disconnected
                despawnGrace = 0; g_spawnTries = 0; g_puppetVtbl = nullptr;
                g_snapCount = 0; g_snapHead = 0; g_haveSnapSeq = false;   // reset interp buffer for next peer
            }
        }

        // ---- spawn + drive: only while stably in our world, peer present, not loading ----
        // (localOk is false during a pause/menu, so this is skipped then; skipping during a load also
        //  avoids touching the puppet while the world is tearing down / the builder is repopulating.)
        if (localOk && peerPresent && stable > 60 && !loading) {
            if (!g_puppet) {
                if (g_spawnCd > 0) --g_spawnCd;
                else {
                    // Keep retrying (throttled) rather than giving up after 5: after world travel the
                    // cached builder/selector globals may only become usable later. Each attempt is
                    // SEH-guarded + dup-balanced, so persistent retry is safe. If it never succeeds the
                    // globals are permanently stale (need the live-selector path).
                    ++g_spawnTries; g_spawnCd = 120;                 // ~2s between attempts
                    if (g_spawnTries <= 5 || (g_spawnTries % 15) == 0)
                        L("[Net] peer in world (id=%08X%08X) -> spawning puppet (try %d)\n",
                          (uint32_t)(local.worldId >> 32), (uint32_t)local.worldId, g_spawnTries);
                    Phase1::TrySpawnPuppet();
                    if (g_puppet) {                       // record identity for the liveness check
                        g_puppetWorldId = local.worldId;
                        g_lastRemoteCtx = -1;             // drive the peer's current context onto the new puppet
                        g_lastActionAlias = 0xFFFFFFFF;   // and the peer's current action
                        g_bhvState = -1; g_switchCd = 0;  // re-establish the new puppet's behavior state
                        g_casActivated = false;           // re-activate its CharacterActionSet
                        __try { g_puppetVtbl = *(void**)g_puppet; } __except (EXCEPTION_EXECUTE_HANDLER) { g_puppetVtbl = nullptr; }
                        L("[Net] spawn succeeded, g_puppet=%p\n", g_puppet);
                    }
                }
            }
            if (g_puppet) {
                // Render a delayed, interpolated state; with interp off, use the raw latest packet.
                PlayerState remote; bool haveRemote;
                if (g_interpEnabled) haveRemote = ComputeInterpolated(now, remote);
                else { remote = raw; haveRemote = gotRaw; }
                // Same world + same scene ONLY: never drive a puppet from a peer we've gated out
                // (proto mismatch, different world cell, or unknown-and-disallowed).
                bool remoteSceneOk = (s_sceneAware == 0) || remote.sceneKey == 0 || local.sceneKey == 0 ||
                                     remote.sceneKey == local.sceneKey;
                if (haveRemote && remote.valid && remote.worldId == local.worldId &&
                    remote.protoVer == NET_PROTO_VER && remoteSceneOk) {
                    // ---- [Coop] ShipPassenger (default OFF) ------------------------------------------
                    // When ON and the PEER reports they're on a ship (remote.isOnShip) while OUR player is
                    // also aboard their own ship (updatedHuman->m_IsOnShip), place the puppet on the LOCAL
                    // ship's deck instead of at the peer's raw world position: the two ships sit at different
                    // world coords, so the raw position would drop the puppet in the sea. On land (peer not
                    // aboard) we fall through to the untouched normal path. Flag OFF => this block is a no-op
                    // (placedOnDeck stays false) and the original single call runs verbatim.
                    bool placedOnDeck = false;
                    static bool s_passengerWasOn = false;   // edge-trigger so transitions log exactly once
                    if (s_shipPassenger && remote.isOnShip && updatedHuman &&
                        updatedHuman->m_IsOnShip) {
                        void* ship = handleResolve(updatedHuman->m_IsOnShipEntity);
                        if (ship && readable(ship, 0x100)) {
                            __try {
                                // Ship world transform (BaseEntity::m_GlobalMatrix). Row-vector convention:
                                // M41..M43 = translation row, rows 0-2 = rotation basis (matches lerpState).
                                // world = shipPos + offset * R.
                                HookMath::Transform sm = ((BaseEntity*)ship)->m_GlobalMatrix;
                                float ox = kShipPassengerDeckOffset[0];
                                float oy = kShipPassengerDeckOffset[1];
                                float oz = kShipPassengerDeckOffset[2];
                                float wx = sm.M41 + ox*sm.M11 + oy*sm.M21 + oz*sm.M31;
                                float wy = sm.M42 + ox*sm.M12 + oy*sm.M22 + oz*sm.M32;
                                float wz = sm.M43 + ox*sm.M13 + oy*sm.M23 + oz*sm.M33;

                                PlayerState pr = remote;   // copy: never mutate the shared interpolated state
                                pr.matrix.M41 = wx; pr.matrix.M42 = wy; pr.matrix.M43 = wz;
                                // Adopt the ship's heading so the puppet faces along the deck.
                                pr.matrix.M11 = sm.M11; pr.matrix.M12 = sm.M12; pr.matrix.M13 = sm.M13;
                                pr.matrix.M21 = sm.M21; pr.matrix.M22 = sm.M22; pr.matrix.M23 = sm.M23;
                                pr.matrix.M31 = sm.M31; pr.matrix.M32 = sm.M32; pr.matrix.M33 = sm.M33;
                                Phase1::ApplyRemoteState(pr);
                                placedOnDeck = true;
                                if (!s_passengerWasOn) {
                                    s_passengerWasOn = true;
                                    L("[Ship] passenger: peer aboard -> placing puppet on local deck "
                                      "(ship=%p deckPos=(%.1f,%.1f,%.1f))\n", ship, wx, wy, wz);
                                }
                            } __except (EXCEPTION_EXECUTE_HANDLER) {
                                L("[Ship] passenger: EXCEPTION placing on deck (code=0x%08X) -> normal apply\n",
                                  GetExceptionCode());
                            }
                        }
                    }
                    if (!placedOnDeck) {
                        if (s_passengerWasOn) {   // transitioned off deck placement -> back to normal
                            s_passengerWasOn = false;
                            L("[Ship] passenger: peer off ship -> normal placement\n");
                        }
                        Phase1::ApplyRemoteState(remote);
                    }
                }
            }
        }
        // Auto-spawn the ghost ship when the peer is aboard (and none exists yet). The render override
        // (LogSetGlobalMatrix) then drives it to the peer's position/rotation every frame.
        if (s_shipGhost && g_ghostHasSync && !g_ghostShip) {
            g_ghostShip = SpawnGhostShip();
            if (g_ghostShip) { g_ghostAuto = true; L("[ShipSync] auto-spawned ghost ship %p\n", g_ghostShip); }
        } else if (!g_ghostHasSync && g_ghostAuto && g_ghostShip) {
            Phase1::DespawnGhostShip();   // peer disembarked -> hide the auto ghost
        }
        // Direct drive (F12 manual ghost + auto ghost): the ghost's RigidBody/AI components are disabled,
        // so the engine never writes its transform and the SetGlobalMatrixInternal hook can't fire for it.
        // Drive it ourselves every frame to the peer's ship position + heading (render-only, no physics).
        if (g_ghostShip && g_ghostHasSync) {
            HookMath::Quaternion q;
            q.x = g_ghostSyncQuat[0]; q.y = g_ghostSyncQuat[1]; q.z = g_ghostSyncQuat[2]; q.w = g_ghostSyncQuat[3];
            HookMath::Transform m;
            HookMath::ToMatrix(&q, &m);
            m.M41 = g_ghostSyncPos[0]; m.M42 = g_ghostSyncPos[1]; m.M43 = g_localShipZ;
            __try {
                ((Entity*)g_ghostShip)->SetGlobalMatrixInternal(&m, true);
                { static int n = 0; if ((n++ & 0x3F) == 0)
                    L("[ShipSync] driving ghost to (%.1f, %.1f, %.1f)\n", m.M41, m.M42, m.M43); }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                L("[ShipSync] drive EXCEPTION code=0x%08X\n", GetExceptionCode());
                g_ghostShip = nullptr;
            }
        } else if (g_ghostShip && s_ghostSelfTest) {
            // Solo self-test: drive the ghost to the LOCAL ship's transform + an X offset, so the user
            // can see the masts/sails skeleton rotate + bob single-player (no peer needed).
            Entity* ls = GetLocalShip();
            if (ls && readable(ls, 0x50)) {
                HookMath::Transform lm = ((BaseEntity*)ls)->m_GlobalMatrix;
                lm.M41 += 50.0f;   // offset so the ghost is visible (not overlapping the real ship)
                __try {
                    ((Entity*)g_ghostShip)->SetGlobalMatrixInternal(&lm, true);
                    { static int n = 0; if ((n++ & 0x3F) == 0)
                        L("[ShipSync] selftest ghost -> (%.1f, %.1f, %.1f)\n", lm.M41, lm.M42, lm.M43); }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    L("[ShipSync] selftest EXCEPTION 0x%08X\n", GetExceptionCode());
                }
            }
        }
        return;
    }
    if (g_spawnReq) { g_spawnReq = 0; Phase1::TrySpawnPuppet(); }
}

// Inline-hook detour: do our per-frame work, then reproduce the stolen instruction and jump back.
// esi holds the Human* being updated; preserved across our work by pushad/popad.
static __declspec(naked) void FrameDetour() {
    __asm {
        pushad
        pushfd
        push esi                // updatedHuman (esi = Human* being updated)
        call OnFrame
        add  esp, 4
        popfd
        popad
        mov  ecx, [esi+0x108]   // reproduce stolen instruction
        jmp  g_frameJmpBack
    }
}

// --- SetGlobalMatrixInternal hook: log the CALLER (return addr) when the SHIP's transform is written.
//     This reveals the naval sim's per-frame transform write, which is the seam we drive for shared ship.
static volatile uint32_t g_sgmThis   = 0;  // Entity* (this) of the current SetGlobalMatrixInternal call
static volatile uint32_t g_sgmMatrix = 0;  // Transform* arg (the matrix being written)
static volatile uint32_t g_sgmRet    = 0;  // return address (the caller = the transform writer)
static volatile uint32_t g_sgmPhysPtr = 0; // address of the updatePhysics bool arg (so we can clear it)
static uint32_t g_sgmJmpBack = 0;          // 0x00528200 + 6 (next instruction after stolen bytes)

static void __cdecl LogSetGlobalMatrix(uint32_t thisEnt) {
    if (!thisEnt || !readable((void*)thisEnt, 4)) return;

    // Immediate probe: log the FIRST time a character (vt 0x01E4CE90) reaches SetGlobalMatrixInternal.
    // This is the GO/NO-GO for the guard render override — if this never logs, characters use the
    // skeleton path and the guard drive needs the direct-call approach instead.
    { static bool s_charSeen = false;
      if (!s_charSeen && *(uint32_t*)thisEnt == 0x01E4CE90u) {
          s_charSeen = true;
          L("[SGM] CHARACTER (vt 0x01E4CE90) reached SetGlobalMatrixInternal -> guard render override GO\n");
      }
    }

    // One-time probe: record the DISTINCT vtables that reach SetGlobalMatrixInternal. This verifies
    // whether CHARACTERS (vt 0x01E4CE90) actually use this render path — if they don't appear here,
    // the render override can't drive them (they'd use the skeleton path instead).
    { static uint32_t s_vt[40]; static int s_n = 0; static bool s_done = false;
      if (!s_done && s_n < 40) {
          uint32_t vt = *(uint32_t*)thisEnt;
          bool dup = false;
          for (int k = 0; k < s_n; ++k) if (s_vt[k] == vt) { dup = true; break; }
          if (!dup) s_vt[s_n++] = vt;
          if (s_n == 40) {
              s_done = true;
              L("[SGM] distinct vtables reaching SetGlobalMatrixInternal:\n");
              for (int k = 0; k < s_n; ++k)
                  L("[SGM]   vt=%08X%s\n", s_vt[k], s_vt[k] == 0x01E4CE90u ? "  <-- character/Entity" : "");
          }
      }
    }

    if (!g_sgmMatrix || !readable((void*)(g_sgmMatrix + 0x30), 12)) return;

    // Ghost ship sync: drive the spawned ghost's render to the peer's ship state (position + ROTATION
    // synced; Z = the LOCAL wave height, so it bobs up/down with the local water instead of sinking).
    if (g_ghostShip && g_ghostHasSync && thisEnt == (uint32_t)g_ghostShip) {
        float* m = (float*)g_sgmMatrix;   // full 4x4 (row-major: m[0..2],m[4..6],m[8..10]=rot; m[12..14]=trans)
        float x = g_ghostSyncQuat[0], y = g_ghostSyncQuat[1], z = g_ghostSyncQuat[2], w = g_ghostSyncQuat[3];
        m[0] = 1 - 2*(y*y + z*z);  m[1] = 2*(x*y - w*z);  m[2] = 2*(x*z + w*y);
        m[4] = 2*(x*y + w*z);      m[5] = 1 - 2*(x*x + z*z);  m[6] = 2*(y*z - w*x);
        m[8] = 2*(x*z - w*y);      m[9] = 2*(y*z + w*x);      m[10] = 1 - 2*(x*x + y*y);
        m[12] = g_ghostSyncPos[0]; m[13] = g_ghostSyncPos[1]; m[14] = g_localShipZ;
        { static int g_shipDriveLog = 0; if ((g_shipDriveLog++ & 0x3F) == 0)
            L("[ShipSync] driving ghost to (%.1f, %.1f, %.1f)\n", g_ghostSyncPos[0], g_ghostSyncPos[1], g_localShipZ); }
        return;
    }

    // Entity sync (NPCs + ships): override the RENDER for each synced entity, and clear `updatePhysics`
    // so the override does NOT feed back into the entity's own physics (verified: SetGlobalMatrixInternal
    // 0x00528200 always writes the matrix, then only updates physics components when updatePhysics!=0).
    for (int i = 0; i < g_syncCount; ++i) {
        if (g_syncTargets[i].ent != thisEnt) continue;
        if (g_sgmPhysPtr) *(uint32_t*)g_sgmPhysPtr = 0;   // render-only: skip the physics update
        float* m = (float*)g_sgmMatrix;
        float x = g_syncTargets[i].quat[0], y = g_syncTargets[i].quat[1], z = g_syncTargets[i].quat[2], w = g_syncTargets[i].quat[3];
        m[0] = 1 - 2*(y*y + z*z);  m[1] = 2*(x*y - w*z);  m[2] = 2*(x*z + w*y);
        m[4] = 2*(x*y + w*z);      m[5] = 1 - 2*(x*x + z*z);  m[6] = 2*(y*z - w*x);
        m[8] = 2*(x*z - w*y);      m[9] = 2*(y*z + w*x);      m[10] = 1 - 2*(x*x + y*y);
        m[12] = g_syncTargets[i].pos[0]; m[13] = g_syncTargets[i].pos[1]; m[14] = g_syncTargets[i].pos[2];
        return;
    }

    // F5 hold test: pin the MAIN ship's X/Y (horizontal), leave Z for the waves. Ship-only.
    if (*(uint32_t*)thisEnt != 0x01E4A128u) return;
    if (!g_holdShip || !g_holdShipEnt) return;
    if (thisEnt != (uint32_t)g_holdShipEnt) return;
    float* p = (float*)(g_sgmMatrix + 0x30);   // M41/M42/M43 = translation
    p[0] = g_holdPos[0]; p[1] = g_holdPos[1];
    static long n = 0;
    if (n < 10) { L("[ShipHold] OVERRIDE main ship=%p X/Y -> (%.1f,%.1f) [Z=wave]\n",
                    thisEnt, g_holdPos[0], g_holdPos[1]); ++n; }
}

static __declspec(naked) void SgmDetour() {
    __asm {
        pushad
        mov eax, [esp+0x20]      // return address (caller of SetGlobalMatrixInternal)
        mov g_sgmRet, eax
        mov eax, [esp+0x18]      // ECX = this (Entity*)
        mov g_sgmThis, eax
        mov eax, [esp+0x24]      // arg1 = matrix* (Transform*)
        mov g_sgmMatrix, eax
        lea eax, [esp+0x28]      // &arg2 = the updatePhysics bool
        mov g_sgmPhysPtr, eax
        mov eax, [esp+0x18]      // reload `this`
        push eax                 // thisEnt arg
        call LogSetGlobalMatrix
        add  esp, 4
        popad
        push ebp                 // reproduce stolen bytes: PUSH EBP
        mov  ebp, esp            //                       MOV EBP,ESP
        mov  eax, [ebp+0x8]      //                       MOV EAX,[EBP+8]
        jmp  g_sgmJmpBack
    }
}

// --- generic trampoline hook helpers ---
static void patchJmp(uintptr_t at, void* to, size_t len) {
    DWORD old = 0;
    VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)at = 0xE9;
    *(uint32_t*)(at + 1) = (uint32_t)((uintptr_t)to - at - 5);
    for (size_t i = 5; i < len; ++i) *(uint8_t*)(at + i) = 0x90;
    VirtualProtect((void*)at, len, old, &old);
}
// Copy stolenLen bytes of `target` into fresh RWX memory + jmp back; returns the callable original.
static void* buildTrampoline(uintptr_t target, size_t stolenLen) {
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, stolenLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    for (size_t i = 0; i < stolenLen; ++i) t[i] = *(uint8_t*)(target + i);
    t[stolenLen] = 0xE9;
    *(uint32_t*)(t + stolenLen + 1) = (uint32_t)((target + stolenLen) - (uintptr_t)(t + stolenLen) - 5);
    return t;
}

// BhvAssassin::AddToWorldInternal calls GetUnusedPadID, but on PC it hands back the ACTIVE pad
// (one "valid" pad, unused-search fails -> defaults to the active slot), so the puppet enrolls onto
// the real player's pad+gamer and hijacks it. Force a genuinely-empty slot for the puppet instead;
// since gamerID==padID, a distinct pad also makes GamerCollection ADD a new gamer rather than reuse.
typedef char (__fastcall *GetUnusedPad_t)(void* padMgr, void* edx, uint8_t* out);
static GetUnusedPad_t g_origGetUnusedPad = nullptr;

static char __fastcall MyGetUnusedPadID(void* padMgr, void* edx, uint8_t* out) {
    if (g_blockReg) {                                  // a puppet exists -> give it its own slot
        for (int i = 0; i < 8; ++i) {
            if (((void**)padMgr)[i] == nullptr) {
                *out = (uint8_t)i;
                char msg[64]; wsprintfA(msg, "[Phase1] GetUnusedPadID -> forced empty pad %d\n", i);
                OutputDebugStringA(msg);
                return 1;
            }
        }
    }
    return g_origGetUnusedPad(padMgr, edx, out);        // real player / fallback
}

static void InstallGetUnusedPadHook() {
    uintptr_t fn = base() + 0x344CF0;                  // PlayerPadManager::GetUnusedPadID
    g_origGetUnusedPad = (GetUnusedPad_t)buildTrampoline(fn, 6);
    patchJmp(fn, (void*)&MyGetUnusedPadID, 6);
    OutputDebugStringA("[Phase1] GetUnusedPadID hook installed\n");
}

// Keep the game running when it loses focus (so a backgrounded 2nd instance doesn't pause).
// sub_4054D0(active): active==0 is the focus-loss/deactivate path — swallow it.
typedef int (__cdecl *Deactivate_t)(char);
static Deactivate_t g_origDeactivate = nullptr;
static int __cdecl MyDeactivate(char active) {
    if (active == 0) return 0;           // ignore alt-tab deactivation
    return g_origDeactivate(active);
}
static void InstallNoPauseHook() {
    uintptr_t fn = base() + RVA_Deactivate;
    g_origDeactivate = (Deactivate_t)buildTrampoline(fn, 7);
    patchJmp(fn, (void*)&MyDeactivate, 7);
    OutputDebugStringA("[Phase1] no-pause-on-focus-loss hook installed\n");
}

// True once the local player entity is live in a real (non-whiteroom) world. Used to gate the spawn
// block: the player + essential/mission entities spawn while the player is still ABSENT (boot, world
// load, fast-travel, death respawn), so we must let those through or the load hangs. Ambient
// population (crowd/blob NPCs/guards/ships) spawns continuously AFTER the player is present, so we
// only decline spawns once the player exists.
static bool playerInWorld() {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return false;

        if (inWhiteRoom(world)) return false;

        Entity* player = world->GetPlayer();
        if (!player)
            return false;

        void* h = AIEntityHelper::GetHuman(player);
        return readable(h, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// (1) Universal spawn gate: decline SpawningRequest::Spawn (RVA 0x39B8D0) — the entry EVERY spawning
// strategy funnels through (crowd, blob NPCs, guards, sphere, naval) — but ONLY while the player is
// present (see playerInWorld). __thiscall(this), no stack args, returns char in al. Intercepted as
// __fastcall(this, edx) — the dummy edx absorbs the register thiscall leaves unused; `return 0` maps
// to al=0, the function's own "declined" path, which callers handle. Blocking unconditionally hung
// boot because the player/essential entities spawn through here too, hence the playerInWorld gate.
const uintptr_t RVA_SpawnRequestSpawn = 0x39B8D0;
typedef char (__fastcall *SpawnReq_t)(void* thisPtr, void* edx);
static SpawnReq_t g_origSpawnReq = nullptr;

static char __fastcall MySpawnRequest(void* thisPtr, void* edx) {
    if (g_disableSpawns && playerInWorld()) return 0;   // decline ambient spawns during live gameplay
    return g_origSpawnReq(thisPtr, edx);                // allow during boot/load/respawn (player absent)
}

static void InstallSpawnRequestHook() {
    uintptr_t fn = base() + RVA_SpawnRequestSpawn;
    // Steal 5 bytes: push esi; mov esi,ecx; mov eax,[esi] (56 8B F1 8B 06) — clean, no relative refs.
    g_origSpawnReq = (SpawnReq_t)buildTrampoline(fn, 5);
    patchJmp(fn, (void*)&MySpawnRequest, 5);
    OutputDebugStringA("[Phase1] spawn suppression hook installed (SpawningRequest::Spawn)\n");
}

// (2) Extra: no-op CrowdHerder::UpdateSpawningNeeds (RVA 0x1578B50), the type-4 crowd spawn issuer.
// __thiscall(this, fractionArray, needDelta) — 2 stack args, callee-cleaned (ret 8). Intercepted as
// __fastcall so ecx=this maps; the dummy edx absorbs the unused reg, so the two stack args land as
// a2/a3, and an early return compiles to `ret 8`. Stops the herd crowd before it builds requests.
const uintptr_t RVA_CrowdSpawnNeeds = 0x1578B50;
typedef void (__fastcall *CrowdSpawnNeeds_t)(void* thisPtr, void* edx, int fractionArray, float needDelta);
static CrowdSpawnNeeds_t g_origCrowdSpawnNeeds = nullptr;

static void __fastcall MyCrowdSpawnNeeds(void* thisPtr, void* edx, int fractionArray, float needDelta) {
    if (g_disableSpawns) return;   // block new crowd spawns entirely
    g_origCrowdSpawnNeeds(thisPtr, edx, fractionArray, needDelta);
}

static void InstallCrowdSpawnHook() {
    uintptr_t fn = base() + RVA_CrowdSpawnNeeds;
    // Steal 6 bytes: push ebp; mov ebp,esp; sub esp,4Ch (55 8B EC 83 EC 4C) — clean, no relative refs.
    g_origCrowdSpawnNeeds = (CrowdSpawnNeeds_t)buildTrampoline(fn, 6);
    patchJmp(fn, (void*)&MyCrowdSpawnNeeds, 6);
    OutputDebugStringA("[Phase1] crowd-spawn suppression hook installed (UpdateSpawningNeeds)\n");
}

void Phase1::RequestSpawn() { g_spawnReq = 1; }
void Phase1::RequestDump()  { g_dumpReq  = 1; }
void Phase1::RequestCrowdProbe() { g_crowdReq = 1; }

// F7 dump (main thread): print the local player's Human* and every hop of the swim chain so the layout
// can be walked/verified in Cheat Engine. Each deref is readable()-guarded and the whole thing is
// SEH-wrapped, so it prints as far as the chain is valid and never faults the game. Swim fields only read
// sane once the player is actually IN the water (contextId 10); on the ground the swim context/FSM/scope
// may be null/unallocated — that's expected.
void Phase1::DumpPlayerInfo() {
    __try {
        World*  world  = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;
        Human*  h      = player ? AIEntityHelper::GetHuman(player) : nullptr;
        L("[Dump] world=%p player=%p HUMAN=%p\n", world, player, h);
        if (!readable(h, 0x50)) { L("[Dump] human not readable\n"); return; }

        L("[Dump]   contextId=%d  &m_HumanContextBundle=%p  groundBundle=%p\n",
          h->GetCurrentContextID(MAIN_AI_TRACK), &h->m_HumanContextBundle, h->m_Data.m_HumanDataBundle);

        void* swimCtx = nullptr; void* fsm = nullptr;
        HumanSwimScope* sc = swimScope(h, &swimCtx, &fsm);   // guards the m_Swim read internally
        L("[Dump]   m_Swim(HumanSwim)=%p  +0x30 FSM=%p  +0x0C scope=%p\n", swimCtx, fsm, sc);
        if (sc)
            L("[Dump]   scope+0xFF0 dir=(%.3f,%.3f,%.3f,%.3f)  +0x1000 spd=%.3f\n",
              sc->m_DestDirection.x, sc->m_DestDirection.y, sc->m_DestDirection.z, sc->m_DestDirection.w,
              sc->m_DestSpeedRatio);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Dump] EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

void Phase1::InstallFrameHook() {
    g_driveLoco = GetPrivateProfileIntA("Coop", "DriveLocomotion", 0, ".\\AC4Coop.ini");
    g_driveContext = GetPrivateProfileIntA("Coop", "DriveContext", 0, ".\\AC4Coop.ini");
    g_driveActions = GetPrivateProfileIntA("Coop", "DriveActions", 1, ".\\AC4Coop.ini");
    g_disablePuppetAI = GetPrivateProfileIntA("Coop", "DisablePuppetAI", 1, ".\\AC4Coop.ini");
    g_hybridLoco = GetPrivateProfileIntA("Coop", "HybridLoco", 1, ".\\AC4Coop.ini");
    g_seekOnSwitch = GetPrivateProfileIntA("Coop", "SeekOnSwitch", 1, ".\\AC4Coop.ini");
    g_minBlendIn = GetPrivateProfileIntA("Coop", "MinBlendInMs", 100, ".\\AC4Coop.ini") / 1000.0f;
    // DriveContextMask: read as a string so hex ("0x440") or decimal both parse. Empty -> keep the safe
    // default (InAir+Swim). strtoul base 0 auto-detects the 0x prefix.
    {
        char maskStr[32] = {0};
        GetPrivateProfileStringA("Coop", "DriveContextMask", "", maskStr, sizeof(maskStr), ".\\AC4Coop.ini");
        if (maskStr[0]) g_ctxMask = (unsigned)strtoul(maskStr, nullptr, 0);
        L("[Phase1] DriveContext=%d ctxMask=0x%X\n", g_driveContext, g_ctxMask);
    }
    // DisableSpawns is the new key; fall back to the old DisableCrowd for back-compat.
    g_disableSpawns = GetPrivateProfileIntA("Coop", "DisableSpawns", 0, ".\\AC4Coop.ini")
                   || GetPrivateProfileIntA("Coop", "DisableCrowd", 0, ".\\AC4Coop.ini");
    g_interpEnabled = GetPrivateProfileIntA("Net", "Interpolation", 1, ".\\AC4Coop.ini");
    g_interpDelayMs = GetPrivateProfileIntA("Net", "InterpDelayMs", 100, ".\\AC4Coop.ini");
    InstallGetUnusedPadHook();
    InstallNoPauseHook();
    if (g_disableSpawns) { InstallSpawnRequestHook(); InstallCrowdSpawnHook(); }
    uintptr_t b = base();
    uint8_t* at = (uint8_t*)(b + RVA_HumanUpdate);
    g_frameJmpBack = (uint32_t)(b + RVA_HumanUpdate + HU_STEAL_LEN);
    DWORD old = 0;
    VirtualProtect(at, HU_STEAL_LEN, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;                                            // jmp rel32 -> FrameDetour
    *(uint32_t*)(at + 1) = (uint32_t)((uint8_t*)&FrameDetour - at - 5);
    at[5] = 0x90;                                            // nop the 6th stolen byte
    VirtualProtect(at, HU_STEAL_LEN, old, &old);
    OutputDebugStringA("[Phase1] frame hook installed on Human update (per-frame, main thread)\n");

    // SetGlobalMatrixInternal (VA 0x00528200, RVA 0x128200) __thiscall: ECX=this, [EBP+8]=matrix*, [EBP+0xC]=updatePhysics.
    // Steal 6 bytes (55 8B EC 8B 45 08); trace the caller when the ship's transform is written.
    {
        uint8_t* sgm = (uint8_t*)(b + 0x128200);
        g_sgmJmpBack = (uint32_t)(b + 0x128200 + 6);
        VirtualProtect(sgm, 6, PAGE_EXECUTE_READWRITE, &old);
        sgm[0] = 0xE9;
        *(uint32_t*)(sgm + 1) = (uint32_t)((uint8_t*)&SgmDetour - sgm - 5);
        sgm[5] = 0x90;
        VirtualProtect(sgm, 6, old, &old);
        OutputDebugStringA("[Phase1] SGM hook installed (ship transform-write tracer)\n");
    }
}
