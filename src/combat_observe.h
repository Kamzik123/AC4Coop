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
