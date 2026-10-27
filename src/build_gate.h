#pragma once

#include <windows.h>

// Build gate: decide, once per process, whether the running AC4BFSP.exe is a build whose code layout
// we have verified. Everything that patches engine code asks Trusted() first.
//
// The gate is deliberately dumb and non-fatal:
//   - it hashes the EXE that is actually running (GetModuleFileNameW(NULL)), never a hardcoded path;
//   - it looks the digest up in `known_builds.txt`, which lives NEXT TO THIS ASI (so a new build can
//     be trusted by editing a text file, without a rebuild);
//   - it never deletes, unloads or uninstalls anything. An unknown build leaves the module resident
//     and inert: the process must still reach the menu, just with no patches installed.
//
// `[Coop] ForceUnknownBuild=1` (AC4Coop.ini, default 0) trusts an unknown build for the session after
// logging a loud warning. It exists for A/B work on a patched/repacked EXE, not for shipping.
namespace BuildGate {
    // Hash + known-build lookup + logging; sets the state Trusted() reports.
    // Returns Trusted(). Must be called before any patch installer runs.
    bool Verify(HMODULE self);

    // True if the running build is listed in known_builds.txt, or ForceUnknownBuild=1 overrode it.
    bool Trusted();
}
