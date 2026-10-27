#pragma once

// Per-site null guards for the loader's nullable object pointers (node+0x114 / node+0x118).
//
// Why this exists (and why ActionGuard is not the right fix):
//   The action node at outer+0xE0 has two optional object pointers, +0x114 / +0x118, which the node
//   constructor leaves NULL. Seven sites in sub_679750 (0x006797xx) and sub_681980 (0x00681Axx)
//   dereference them with no null check, e.g.
//
//       0x006797D6  8B 8E 14 01 00 00   mov ecx,[esi+0x114]
//       0x006797DC  8B 01               mov eax,[ecx]        <- EIP on the crash dump
//
//   The fix has to be per site: the state predicate sub_679220 answers a different question for 15
//   callers (one of them, sub_736410, uses the answer only to skip an optional clamp before an
//   *unconditional* virtual call, so forcing it to 1 crashes there). So each deref site gets its own
//   guard: re-execute the stolen load, test the pointer, and if it is null skip the call that would
//   dereference it. Non-null execution is byte-for-byte the original path.
namespace DerefGuard {
    // Installs six 6-byte jumps into stubs in a single VirtualAlloc'd cave within +-2 GB of the
    // module. The two function bases (sub_679750 / sub_681980) are resolved by pattern first; every
    // site -- and its continue/skip targets -- is then an offset from that base. The documented
    // literals (0x006797D6 / 0x006797E4 / 0x00681AE7 / 0x00681B61 / 0x00681B23 / 0x00681B49) are
    // used only as the fallback route when a pattern finds nothing on a KNOWN build.
    // Config-gated by [Coop] GuardNullTarget (default 1; 0 = install nothing), and by
    // BuildGate::Trusted(): an untrusted build installs nothing.
    // Every site is byte-verified before it is touched; a site whose bytes differ is skipped and
    // logged, never half-patched. Safe to call more than once.
    void Init();
}
