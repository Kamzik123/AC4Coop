#pragma once

// Null-action-target guard for the co-op puppet crash.
//
// The engine's action/animation node (embedded sub-object at outer+0xE0) may legally have no bound
// target: its +0x114 / +0x118 are null by construction. The state gate sub_679220 (VA 0x00679220)
// returns 0 for such a node, after which sub_734240 / sub_7342B0 call sub_681980 / sub_679750, whose
// `switch (word[node+0xFC] & 7)` case 1/6 dereferences [node+0x114] with no null check and crashes.
//
// ActionGuard keeps the gate returning 1 ("at rest") while either field is null; all 15 callers test
// AL and abandon their target path on 1.
namespace ActionGuard {
    // Installs a 7-byte detour at VA 0x00679220. Call once at DLL attach, next to the other hook
    // installs. Config-gated by [Coop] GuardNullActionTarget (default 1; 0 = do not install).
    // Safe to call more than once; a bad prologue or a failed allocation leaves the game untouched.
    void Init();
}
