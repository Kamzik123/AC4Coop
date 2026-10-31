# AC4 Black Flag co-op — review bundle

Everything the programme has produced: the plugin source, the plans, the evidence, the claims
ledger, the journals, and the honest list of what is wrong. Written for an outside reviewer.

Start with **§1 (status)** and **§7 (what I would look at first)**. `docs/ops/CLAIMS.md` is the
ledger of every technical claim and how it was checked.

---

## 1. What this is

A native C++ plugin (`AC4Coop.asi`, loaded through a `dinput8.dll` ASI loader) that adds two-player
co-op to **Assassin's Creed IV Black Flag (2013, AnvilNext, PC 1.05)** — roam, combat, kills,
eventually stealth and naval.

Target binary: `AC4BFSP.exe`, 45,056,040 bytes, sha256 `bc11ad79…ab5150`.
Everything is offsets and detours against that binary. There is no engine source.

Two players, two machines (Steam copy + Ubisoft Connect copy), UDP over a VPN at ~200 ms RTT.

## 2. Status, honestly

| subsystem | state |
|---|---|
| transport (UDP, sockets, protocol) | **works** — verified live both directions |
| player puppet (the other player's body) | **works** — render-only position mirror, collision off, `avgD≈0.06 m`, ~33 kB/s |
| entity census (which NPCs to sync) | **partly** — role/kind table extracted from symbols, filter is a heuristic |
| NPC identity (which local body is the host's NPC) | **fragile** — slot ids built, never run in game |
| death lane host→guest | **works** — verified live |
| death lane guest→host | **built, unverified** |
| ships | **built, unverified** |
| stealth (do guards see the other player?) | **probably broken by construction** — the ghost has no perception registration |
| naval / progression | design only |

The single biggest unverified assumption: **no two-machine run has happened since the last five
commits.** Everything after `5BC501A06101` is compile-and-reason only.

## 3. Architecture

- **Player puppet:** each machine spawns a copy of its own player as the other player's body,
  then drives its transform from the peer's snapshots. Interpolated (`InterpDelayMs`), render-only
  matrix writes (`SetGlobalMatrixInternal(..., false)`), collision layer disabled.
- **NPC sync:** host-authoritative. Host sends `EntityPacket` (ships then characters, ≤128 per
  packet, 20 Hz). Guest binds each host entity to a local body, then drives transform/action/dead.
- **Kills:** host detects a guard's life hitting `0xFFFF`; pushes a `KIND_KILL` entry through a
  32-slot ring (a one-shot packet used to be overwritten by the next snapshot); guest applies it.
  Guest detects its own kills with `GuestDeathScan` and pushes back.
- **Config:** `AC4Coop.ini` next to the ASI. Every path is module-relative (not CWD).

## 4. What the PS3 debug build changed

The project had a **PS3 debug ELF with full DWARF** (`scimitar_final.elf`, 0.52 GB) and did not use
it for most of the work. Extracting from it corrected several things that had been inferred:

```
EntityDescriptorNPCSubType: 1 Templar, 2 Peasant, 3 Thief, 5 Mercenary, 6 Target,
   15 Assassin, 16 Harasser, 17 Pirate, 19 Animal, 23 NavalNPC, 25 Criminal, 28 Vigilantes, …

Layer: Disabled = 40 (0x28), FloatingObject = 41 (0x29)
   → the plugin was setting 0x29 and calling it "Disabled"; it was FloatingObject

Faction_IDEnum: FactionName=0, Allies=1, Enemies=2
   → hostility is a relationship between faction objects, not a role number
```

**PowerPC caveat:** names, member order and enum values transfer; byte offsets do not. Every PC
offset is re-confirmed against the PC binary. Two builds even differ in the component struct
(PS3 reads the component count at `+0x64`, PC at `+0x66` — the PC reader and writer agree, so `+0x66`
is correct for us).

## 5. Rules this programme runs on

1. **No guessing.** Every claim carries a `file:line` or a binary address, or is labelled *unproven*.
2. **Claims ledger** (`docs/ops/CLAIMS.md`) — C1…C24, each with evidence and its independent check.
3. **One writer per file.** Two agents editing `phase1.cpp` at once already cost a build.
4. **One build from a clean tree.** Seats commit their own file; the orchestrator builds and deploys.
5. **Reports are files** (`docs/ops/tools/bus.py`) — a polling watcher silently dropped three
   finished reports, so capture no longer depends on timing.
6. **Reproducible builds** — `/Brepro` and no PDB in Release, so the ASI hash is a real identity.

## 6. Layout

```
src/                      the plugin (phase1.cpp is the bulk: puppet, NPC sync, deaths, probes)
   build_gate.cpp         verifies the running EXE against known_builds.txt
   deref_guard.cpp        null guards for loader sites that crash
   action_guard.cpp       null action-target guard
   net.cpp / net.h        UDP transport, entity packets, kill ring
   paths.cpp              module-relative ini/log resolution
docs/ops/                 plans, decisions, claims, journals, run sheets, tools
docs/evidence/ps3/        extracted enums, class index, PS3 name→address map, structs
docs/logs/                real two-machine logs used as evidence
```

## 7. What I would look at first, as a reviewer

1. **`docs/ops/CLAIMS.md`** — the claims most likely to be wrong are the ones marked
   VERIFIED-SOURCE rather than VERIFIED-LIVE, and C21/C24 (the enemy classifier) which is a
   heuristic pretending less than it is.
2. **`docs/ops/PLAN-PS3.md`** and `docs/ops/PLAN-2026-10-30-enemy-sync.md` — do the gates actually
   decide anything? G6/G7/G9 have machine-readable oracles (`coop_report.py`), G10 does not.
3. **`src/phase1.cpp`, the comment blocks** — they carry the reasoning and the dead ends. Several
   "proven" comments were later disproven and are marked as such; some may still be stale.
4. **The death lane** — it works in one direction only, verified; the other direction is built on
   the same primitives but unproven. `GuestDeathScan` and the tombstone logic are the risky part.
5. **`docs/ops/JOURNAL-2026-10-30.md` §6** — the list of failures and the rule each one produced.

## 8. Known-wrong or unfinished, stated plainly

- **Enemy classification is a heuristic.** `SyncEnemiesOnly` syncs `kind==3` with a role set. The
  game's own hostility test is faction-based, and the faction attach point has no writer we could
  find. A run-time `[Sync] krreject` line exists so a real session can correct the set.
- **The ghost probably cannot be seen by guards.** It has no perception registration, and a
  "make it targetable" write (`+0x660`) turned out to be nonsense — `+0x660` is not a flags word at
  all, and the write was removed.
- **Spawning a missing enemy has no implementation.** RE found no archetype factory; the viable
  route is cloning a live guard, which is not built.
- **Two instances on one PC** is currently being attempted with a second copy of the game. The
  repack's Uplay emulator lacks `UPLAY_USER_IsOwned`; we swapped in the real loader and the known
  exe, which is untested at the time of writing.
- **The IDB was damaged once** (function list 128,610 → 964) and recovered from symbol manifests.
  ~95k unnamed function boundaries remain unrestored; they can be re-added on demand.

## 9. Build and deploy

```
MSBuild AC4Coop.sln /nologo /v:m /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143
```
Output `bin\AC4Coop.asi`. Deploy with `AC4Coop.ini`, `dinput8.dll`, `known_builds.txt` next to
`AC4BFSP.exe`. Current release: `33f8fdb08190`, 303,616 B, commit `ad1aea9` (reproducible).

Logs land next to the ASI: `phase0_log_<pid>.txt`, `phase1_log_<pid>.txt`.
`docs/ops/tools/coop_report.py <log>` prints a pass/fail table for the gates.
