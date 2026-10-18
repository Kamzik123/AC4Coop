# AC4Coop

Custom peer-to-peer co-op mod for Assassin's Creed IV: Black Flag (PC, `AC4BFSP.exe`, x86).
Built as an **ASI plugin** (`bin\AC4Coop.asi`), so it needs an ASI loader such as Ultimate ASI Loader in the
game folder.

Two game instances connect over UDP. Each one spawns a **puppet** `Human` that mirrors the other player's
position, orientation and animation. On top of that, the host's world is **mirrored** to the peer: the
host is authoritative, and the peer's guards + ships become playback dummies driven from the host's
snapshot (see **World mirror** below).

## Status

Working:

- **Networking.** Host-IP UDP link (`[Net]`), with a snapshot buffer and interpolation (`InterpDelayMs`).
  Works with two instances on one PC (auto port-swap) or across two PCs.
- **Puppet lifecycle.** The puppet spawns automatically when the peer is in the same world. It despawns
  when the peer leaves or disconnects, and is dropped and respawned across world travel.
- **Host-authoritative world mirror (`EntitySync=1`).** The host owns its world; the peer's guards and
  ships are **playback dummies** driven every frame from the host's snapshot — the same seam as the
  player puppet. Per matched guard: disable `BhvGenericNPC`, set `CharacterController::m_DriveEntity =
  false` (this is the piece that used to crash/freeze — the controller was fighting the drive), then
  `SetGlobalMatrixInternal` (transform) + `SetTargetAction` (action clip). Nearest-position matching keys
  each host guard to its peer mirror (spawns are deterministic). The host's dying guard sends its **die
  clip** + a dead flag, so the peer's mirror falls. Verified in a single-instance self-test; a
  two-machine pass should confirm it over the wire.
- **Ship sync.** The peer's ship mirrors position + heading via the render-override seam (a ship ghost).
  Hull HP is a plain value (`*(ship+0x5C) + 0x84`) — readable and writable — so ship damage can be
  replicated without the guard health-statechart problem.
- **Position and orientation.** The puppet is placed every frame with `SetGlobalMatrixInternal`, and
  `CharacterController::m_DriveEntity = false` stops its physics from fighting the placement. This is
  jitter-free in every context.
- **Animation by action replication.** The puppet's behavior component is disabled, so it is a playback
  dummy. The peer's current action is replayed with `SetTargetAction` using the peer's real alias,
  channel and layer, which plays the same clip regardless of context. The local player's
  `CharacterActionSet` is handed to the puppet so Edward's clip overrides resolve.
- **Locomotion gait (walk/jog/run).** The ground move cycle is a 46-slot blend tree (`SimpleAnimBlender<46>`
  → `SetTargetBlendFactors(arr, 0, PRIMARY)`). The whole channel-0 blend array is synced, with
  `ACTPERSIST_ALWAYS` persistence. Syncing only 4 slots was why the puppet always walked.
- **Smooth action switches.**
  - **Blend-in:** the peer's real blend-in descriptor (length, start times, `m_Flags`/`m_BlendType`) is
    copied onto the puppet's new evaluator. A transition-less `SetTargetAction` otherwise gets the
    engine's default blend `{0.2, flags 0x600}`, which is `ACTBLENDTYPE_NONE`, a hard cut. That covers
    state-machine-generated transitions such as crouch→stand.
  - **Phase seek:** the new clip is seeked to the peer's playback ratio (`SeekOnSwitch`).
  - **Minimum blend:** when the peer switched with no blend at all, the puppet uses a short blend
    (`MinBlendInMs`) to hide network timing, and never replaces a blend the puppet found on its own.

The animation side is "good enough for now". See **Known issues / next** below.

## Modes (`AC4Coop.ini` `[Coop]`)

| Setting | Meaning |
|---|---|
| `DisablePuppetAI=1`, `DriveActions=1` | Action replication: the working mode. Keep `DriveContext=0`. |
| `DisablePuppetAI=1`, `DriveActions=0` | Braindead baseline: position only, and the ActionComponent update is killed. Used to isolate position bugs. |
| `DisablePuppetAI=0`, `DriveLocomotion=1` | Legacy: feeds `HumanGroundData` to the puppet's own state machine (MVP path). |
| `HybridLoco=1` | Per-context ownership: on ground the puppet's own state machine animates; everywhere else we replicate actions. With this on, the action/blend sync above runs **only off-ground**. Set `0` to use action replication everywhere. |
| `SeekOnSwitch` | Seek a newly targeted action to the peer's phase (default 1). |
| `MinBlendInMs` | Blend used when the peer's switch had none (default 100, 0 = copy the cut exactly). |
| `DisableSpawns` | Suppress ambient spawning (legacy, default 1). Superseded by the world mirror when `EntitySync=1`. |
| `EntitySync=1` | Host-authoritative **world mirror**: relay the host's guards + ships to the peer. Set `EntityHost=1` on the authoritative machine and `0` on the mirror. |
| `EntitySelfTest=1` | Single-instance self-test: capture the host's own guards + ships and drive them at `EntitySelfTestOffset` units, to verify the mirror pipeline without a peer. |

`[Net]`: `Enabled`, `PeerIP`, `LocalPort`/`PeerPort`, `Interpolation`, `InterpDelayMs`.
`[Display]`: `Fullscreen`, `Borderless`, `ForceWindowedMs` (windowed mode is recommended for two-instance
testing).

## Wire format

`src/netstate.h` holds two packet types: `PlayerState` (the puppet) and `EntityPacket` / `EntityState`
(the world mirror — guards + ships). The receiver drops any packet whose size differs, so **both
instances must run the same build**. `PlayerState` main fields:

- the global matrix
- legacy ground data
- `contextId`
- action alias, channel and layer
- `blendCount` + `blendFactors[46]`
- `actionRatio`
- blend-in descriptor (`blendInLen`, `blendInStartFromEnd`, `blendInTargetStart`, `blendInFlags`)
- `timeScale`
- a 128-byte context union (swim, now mostly legacy)

`EntityPacket` (the world mirror) carries `count` + `EntityState[]`: `key, kind (1=ship, 2=guard),
dead, stage, pos[3], quat[4], actionAlias`.

## Build

The user builds it with VS2022/MSBuild (`AC4Coop.sln`, Release|Win32, toolset **v145**). Output is
`bin\AC4Coop.asi`. If v145 isn't installed, pass `/p:PlatformToolset=v143`; it compiles the same.

## Deploy / test

Copy `bin\AC4Coop.asi` and `AC4Coop.ini` next to `AC4BFSP.exe` (with an ASI loader). For a same-PC test,
run two instances with `PeerIP=127.0.0.1` and the default ports.

- **Hotkeys:**
  - **F4** dumps the entity enumeration + the guard/health probe to the log.
  - **F6** requests a manual puppet spawn (used when `[Net] Enabled=0`).
  - **F7** dumps the player `Human*` and the swim chain to the log.
- **Logs:** written next to `AC4Coop.asi` (the game folder): `phase0_log.txt` and
  `phase1_log_<pid>.txt`. Useful lines:
  - `[Net] cap ...` / `cap SWITCH ...`: the sender's action, blend array and blend-in.
  - `[Net] action -> ...` / `blendIn fix ...`: what the puppet was told to do.
  - `[Net] PUP ...`: read-back of the puppet's actual action, blend array and time scale.

## Engine hooks (raw RVAs, base 0x400000)

| RVA | Purpose |
|---|---|
| `0x12FAACD` | Per-frame main-thread hook in the Human update (6 stolen bytes). All engine calls happen here. |
| `0x54D0` | Focus deactivate: no-pause-on-focus-loss, for running two instances. |
| `0x39B8D0` | `SpawningRequest::Spawn`: declines ambient spawns while the player is in the world. |
| `0x1578B50` | `CrowdHerder::UpdateSpawningNeeds`: no-op. |
| `0x344CF0` | `PlayerPadManager::GetUnusedPadID`: gives the puppet its own empty pad slot, so it registers as a new gamer instead of reusing the player's. |

Every other engine call goes through the Scimitar class wrappers in `src/Scimitar/`. These hardcode
absolute addresses, assuming base 0x400000.

### Key ActionComponent addresses (PC)

| Address | Function |
|---|---|
| `0x6625D0` | `SetTargetAction(alias, flags, ActionTransition*, channel, layer)` |
| `0x666C80` | `GetCurrentAction` |
| `0x666BB0` | channel-controller lookup (controller `+0x28` = current evaluator, `+0x2C/+0x32` = blend factors data/count) |
| `0x652B10` / `0x667590` | `SetBlendFactor` / `GetBlendFactor` |
| `0x652D60` | `SetBlendFactorPersistence` |
| `0x667170` / `0x652640` | `GetCurrentActionRatio` / `SetCurrentActionRatio(ratio, ch, processDisplacement, processEvents, layer)` |
| `0x667540` / `0x6528B0` / `0x652A40` | time scale get / set / persistence |
| `0x651840` | `ActivateCharacterActionSet(Reference<CAS>*)` |
| `0x4C6980` | `ActionController::SetTargetAction` |

Evaluator layout (same on PC and PS3):

| Offset | Field |
|---|---|
| `+0xD4` | type (4 = ActionEvaluator) |
| `+0xE0` / `+0xE4` | blend-in runtime `m_Time` / `m_Length` |
| `+0x1C8` | `m_ActionBlend` (length, startFromEnd, targetStart; `m_Flags` u16 at `+0x1DC`) |
| `+0x258` | `m_IsTransitionAction` |
| `+0x2A8` | `m_Action` |

## Source layout

- `src/dllmain.cpp`: ASI entry, windowed mode, hotkey poll thread, log.
- `src/phase1.{h,cpp}`: frame hook, spawn/despawn, `CaptureLocalState` / `ApplyRemoteState`,
  interpolation, `CaptureGuards`/`ApplyGuards` + `CaptureShips`/`ApplyShips` (the world mirror).
- `src/net.{h,cpp}`: UDP transport.
- `src/netstate.h`: wire struct.
- `src/windowed.{h,cpp}`, `src/scimitarCommandLineOptions.h`: windowed mode.
- `src/Scimitar/`: reversed engine classes (`Entity`, `World`, `Human`, `AIActor`, `ActionComponent`,
  `CharacterController`, `HumanSwimFSM`, …).
- `HANDOFF.md`: long-form research and decision log (the older sections describe the superseded
  context-forcing approach).

## Known issues / next

- **Combat interactivity (the next big step).** The peer's guards are playback dummies — they mirror the
  host but don't react to the peer. To let the peer *fight* the same guards, relay the peer's **pad
  inputs** (buttons + stick) to the host and let the host's own combat system resolve the damage (this
  sidesteps the guard health statechart entirely — a real second player's input drives the damage). The
  input system is mapped: `PlayerPadManager` at `World+0x7F8 → *(holder+4)`, the `FirePadButtonPressed`
  / `Released` / `StickChanged` events (`sub_800F60` → `sub_7FE9A0`), the pad readers
  (`PadButtonReader`/`PadAxisReader`), and the device layer (DirectInput + XInput). The per-pad **state
  struct** (buttons bitmask + stick floats) still needs a live dump.
- **Guard death over the wire.** The die clip + dead flag are replicated; a live kill on the host should
  make the peer's mirror fall. The ragdoll (physics) isn't replicated — the clip alone provides the fall.
- **Climbing IK:** hand and foot targets aren't replicated, so climb poses are approximate.
- **Phase drift:** the phase is synced only on action switch. Long loops can drift; no continuous
  correction yet.
- **Blend-tree transitions:** while the puppet is mid-transition, `SetTargetBlendFactors` writes to the
  incoming evaluator's `m_InitialBlendFactors` (`+0x2BC`), which isn't mirrored yet.
- **Character overrides:** the CAS is borrowed from the local player, which assumes both players are the
  same character.
- **Leftover code:** the swim context block and the ground-data drive are legacy under action replication
  and can be removed.
- **Pause menu:** the puppet disappears in the pause menu.

## Combat sync — real kills on both sides (2026-10-17)

The earlier plan (relay raw pad input so the host resolves the damage) is **superseded**. All of the
following is verified against the live game:

- **A mirrored guard takes real damage.** The damage system is independent of the behaviour we disable
  for the mirror: hitting a playback dummy moves its health (`36/36 -> 35/36 -> dead`).
- **Entity -> health mapping** (no heap scan needed): `GetEntityAI(guard)` -> the AI's service table at
  `ai+0x70` (count at `ai+0x76`); the entry whose vtable is `0x02712F60` is that guard's `CSrvNPCHealth`
  (`Life` at `+0x5C`, `Max` at `+0x5E`, flags at `+0x60`). Live soldiers show 20 services, including
  `CSrvNPCDeath` (vtable `0x02722528`).
- **Death is engine-native.** `CSrvNPCHealth::SetLife(hp, -1)` (`sub_19FB4C0`) is the engine's own death
  write: a negative value lands `Life` on `0xFFFF` and posts the health event. Writing `0xFFFF` directly
  does **not** work — the setter (and its event) is the trigger.
- **The is-dead predicate** is `sub_753330`: `*(BYTE**)(ai+0x98) && **... || *(BYTE*)(ai+0x8C)`, and
  `ai+0x98` is just a cached pointer to `health+0x63`.

Kill recipe (verified live: guard dies, plays the death animation, lies down, no collision):

```
ai = AIEntityHelper::GetEntityAI(guard);
hp = service in ai->[+0x70] (count +0x76) whose vtable == 0x02712F60;
cc = ((Human*)AIEntityHelper::GetHuman(guard))->GetCharacterComponent();

((int (__thiscall*)(void*, int))0x19FB4C0)(hp, -1);   // SetLife(-1): engine death + event
hp->[+0x60] |= 0x01000000;                            // dead flag
ai->[+0x8C] = 1;                                      // engine is-dead predicate
SetTargetAction(1818493434) on the ActionComponent;   // death clip (captured from a natural kill)
((char (__thiscall*)(void*, int))0x636480)(&cc->m_FilterInfo, 0x29);  // SetLayer -> Disabled
```

`SetLayer(..., 0x29)` is what removes the corpse's collision body (`CollisionFilterInfo::SetLayer`
propagates the filter to the physics object). Do **not** set `m_DriveEntity = false` on a corpse — the
controller then stops applying the animation's root motion and the body stands back up (A/B verified).

**Guest -> host relay:** the guest watches each mirror's `Life` in `ApplyGuards`; on the `0xFFFF`
transition it sends an `EntityPacket` entry with `kind = 3` carrying the host-space position. The host
resolves the nearest soldier within 4 m and runs the recipe. Host-replicated deaths are not echoed back
(`es.dead` check).

**Debug hotkeys:** `F1` simulate a peer kill (goes through the position path, not a pointer);
`F2` guest-side detection harness (press twice: record alive, then kill+report); `F3` ship field diff.

**Known limits:** self-test skips the transform write (the capture source and the mirror are the same
world, so it was redundant snapping and showed as jitter); real two-machine co-op will need interpolation
of guard transforms between snapshots. Ship hull HP is still unknown — it is not a `CSrvHealth` object
and not in the ship AI's service table.
