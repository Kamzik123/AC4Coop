#pragma once
// Stage-2A combat observation -- E2-combat.md 1.2. See combat_observe.cpp for the sites, the byte
// patterns and the log format.
//
// OBSERVATION ONLY, by byte-patch hook: each of the four sites has its whole-instruction prologue
// prefix replaced with a 5-byte JMP into a private stub (Hook::DetourFunc + a trampoline), and the
// exact original bytes are restored on detach. No engine state is read or written, no engine
// function is called, and the arguments are logged as values -- never dereferenced. It cannot
// change combat behaviour.
//
// Thread-agnostic by construction: one write to the site covers every thread. There is no per-thread
// state, hence no reason for DLL_THREAD_ATTACH (DisableThreadLibraryCalls is back on).
//
// Default OFF via [Coop] CombatObserve=0. When off, Init() reads one INI value and returns: no log
// file, no allocation, no VirtualProtect, no detour, no thread.
class CombatObserve
{
public:
    static void Init();       // DLL_PROCESS_ATTACH, next to DerefGuard::Init()
    static void Shutdown();   // DLL_PROCESS_DETACH -- restores the original prologues
    static bool Armed();      // true while at least one site is patched
};
