#pragma once
#include <cstddef>
#include "netstate.h"
// Phase 1 — spawn a second Human from the player's EntityBuilder+selector and bind it to pad 1.
// Engine entity construction is only safe on a thread the engine initialized (per-thread pool
// allocators + scheduler registration), so the spawn/drive run on the game's MAIN thread via a
// per-frame hook on World::LinkUpdateAction. The hotkey just requests; the frame hook executes.

namespace Phase1 {
    void InstallFrameHook();   // call once at DLL attach; inline-hooks World::LinkUpdateAction
    void RequestSpawn();       // hotkey handler (any thread): queue a spawn for the next frame
    void RequestDump();        // hotkey handler (any thread): queue a player/swim-chain dump next frame
    void RequestCrowdProbe();  // hotkey handler (F8, any thread): queue a crowd (non-player) body probe
    void RequestSceneProbe();  // hotkey handler (F9, any thread): queue a scene/interior probe next frame
    void RequestShipProbe();   // hotkey handler (F10, any thread): queue a ship/boarding probe next frame
    void RequestShipSpawn();   // hotkey handler (F11, any thread): queue a ship-dude spawn request next frame
    void RequestGhostDespawn(); // hotkey handler (F11, any thread): queue a ghost-ship removal next frame
    void RequestShipCtorProbe(); // hotkey handler (F12, any thread): queue a construct-ship-entity probe next frame
    void RequestShipDriveProbe(); // hotkey handler (F13, any thread): queue a local-ship transform-write probe next frame
    void RequestEntityEnum();     // hotkey handler (F14, any thread): queue an entity-enumeration probe next frame
    void RequestInputProbe();     // hotkey handler (F3, any thread): queue an input-state probe next frame
    void RequestHealthProbe();    // hotkey handler (F2, any thread): queue a health heap-scan probe next frame
    void RequestGuardHealthMap(); // hotkey handler (F1, any thread): queue a guard->health mapping probe
    // hotkey handler (Ctrl+Alt+K, any thread): [Coop] CombatTest=1 -> kill the nearest live guard within 25 m
    // and watch that object for 10 s. Returns false (and does nothing) when CombatTest=0.
    bool RequestCombatTest();
    // hotkey handlers (Ctrl+Alt+J / Ctrl+Alt+L, any thread): the dead-bit diagnostics. Both return false
    // (and do nothing) when [Coop] WatchTarget / WatchDeadBit is 0, which is the default.
    bool RequestWatchTarget();   // log the nearest soldier's ent/health/svc/svcFlags + life/flags
    bool RequestWatchDeadBit();  // arm the PAGE_GUARD trap that reports whoever writes svc+4
    void DumpPlayerInfo();     // main-thread: print player Human* + the swim context->FSM->scope chain

    // Executed on the main thread by the frame hook:
    void* TrySpawnPuppet();    // build -> add to world -> position -> RegisterPlayer(pad 1)
    void InstallVirtualPadHook();  // Phase A: report a connected pad at index 1 (retry-safe)
    void ToggleVirtualPad();       // runtime toggle for the pad-1 test stick + position comparison
    void* TryCrowdProbe();     // acquire a CROWD builder -> build a non-player body -> add+place -> log verdict
    void  TrySceneProbe();     // log ONE guarded [Scene] diagnostic block (F9) — observation only
    void  TryShipProbe();      // log ONE guarded [Ship] diagnostic block (F10) — observation only
    void  TryShipSpawn();      // F11: request a ship-dude spawn via FUN_01082810 — observation only
    void  TryShipCtorProbe();  // F12: construct a SHIP entity (VA 0x00503330) + AddEntity -> does the engine stream its model? observation only
    void  TryShipDupProbe();   // F12: DUPLICATE the ship via Entity::Duplicate -> does a 2nd ship spawn without a crash? observation only
    void  TryShipDriveProbe(); // F13: WRITE +50 X to the local ship's m_GlobalMatrix -> does it STAY or snap back? observation only
    void  DespawnPuppet();     // remove the puppet from the world (peer left / disconnected)
    void  DespawnGhostShip();  // remove the net-path ghost ship when the peer disembarks

    // Networking (Front B): capture local player state / apply a remote peer's state to the puppet.
    bool  CaptureLocalState(PlayerState& out);
    void  ApplyRemoteState(const PlayerState& in);

    // Builds "<folder containing AC4Coop.asi>\<fileName>" (the game folder), so log files never depend on a
    // machine-specific path.
    void  ModuleRelativePath(char* out, size_t cap, const char* fileName);
}
