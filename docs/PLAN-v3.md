# Co-op plan v3 — thin client, with a hard gate (2026-10-17)

Supersedes `PLAN-v2.md`. v2's two-lane relay (snapshots + health events) is **kept as the fallback and as
what the tester build ships**, but it is no longer the target architecture.

## The decision

The end-state should be **one simulation** — the host's — with the peer as a thin client:

- The peer streams its **input**; it does not simulate NPCs or combat.
- The host runs the peer as a **real second player** (pad 1), so combat, damage, death, ragdoll, stealth
  and interaction are all engine-native for both players.
- The host streams **world state** to the peer: living NPCs' transforms/anim, **plus a short tombstone
  list** of recently dead entities (objId, position, death time) so deaths can be played and bodies
  removed.
- No health reconciliation, no kill relay, no mirror AI hacks.

Why: every feature under the current model needs its own bespoke lane and its own reconciliation, and the
peer's NPCs are inert dummies. That is why the corpse needed clip hacks and why damage needs an event lane
at all.

## The gate (must pass before the rewrite)

**G: does the single-player build accept a second local player on pad 1?**

Evidence in favour (decomp + existing mod code):
- `PlayerPadManager::RegisterPlayer` is literally `padMgr[pad] = entity` (`0x734500`), an 8-slot array.
- The mod already spawns a puppet and calls `RegisterPlayer(puppet, 1)` while `Net` is disabled.
- `World+0x7F8 -> holder`, `PlayerPadManager = *(holder+4)`.
- Devices are plain 560-byte entries (`devMgr+1896`, count `+1902/+1908`, XInput slot `+532`);
  `XInputGetState` is called in only three functions (`0xA065D0`, `0xA28A50`, `0xABF21A`).

Sub-gates, in order:
- **G1** `RegisterPlayer(pad=1)` persists (slot 1 keeps the puppet across frames).
- **G2** No crash; the local player (pad 0) still works; world stays stable.
- **G3** The engine actually *reads input* for pad 1. Needs a synthetic pad (next step), so G3 is tested
  right after the hook exists, not by wishful thinking.

If **G1+G2** pass, build the virtual pad and test **G3**. If G3 passes, migrate. If any gate fails, the
v2 relay is the ceiling for this engine and we keep hardening it.

## Migration if the gate passes

- **Phase A — virtual pad.** Hook `XInputGetState` (three call sites) or extend the device array with a
  synthetic 560-byte entry for a remote pad. Feed it the peer's buttons/sticks.
- **Phase B — peer as player 2.** Host keeps a real player entity on pad 1; the peer's input drives it.
  The peer renders that entity (interpolation first; prediction/reconciliation later).
- **Phase C — world streaming.** Snapshots: living NPCs (transform/anim/hp) + tombstones. The peer applies
  and renders; it stops simulating NPCs, damage and deaths entirely.
- **Phase D — retire the relay** (keep it behind a flag as a fallback/diagnostic).

## What stays from v2 regardless

- `objId` identity (`HandleContainer::m_ObjectID`) — needed for tombstones too.
- The verified kill recipe (`SetLife(-1)` + health flags + is-alive service `0x1000` + `ai+0x8C` AI-stop +
  death clip + `SetLayer(0x29)`) — still the correct way to force a death when needed (tests, host-side
  admin, fallback path).
- The entity→health cache and the event lane — fallback path only.
- Everything in `DECOMP-AUDIT.md`, including the corrected is-alive semantics.

## Status

- v2 relay: implemented, verified solo (objId both ways, damage via `SetLife`), two-machine untested.
- Kill: verified live, engine-confirmed dead (is-alive `1 -> 0`).
- **Gate result (2026-10-17): G1/G2 PASS, G3 FAIL.**
  - G1: `RegisterPlayer(pad=1)` persists; `pad[1] == puppet`; puppet has a live AI + Human.
  - G2: world stable (103 chars / 3575 entities), pad 0 fully playable.
  - Message of the gate: the engine *does* enumerate and poll pads 0..3 (`[VPad] engine polled pad 1..3`),
    and it even auto-assigned the puppet to pad 2 — so the pad plumbing exists.
- **G3 FAIL (confirmed twice, the second time without confounds).** First run was invalid — the stick
  was held from launch, before the puppet existed. Retry: pad neutral at launch, puppet spawned first,
  then the stick toggled at runtime and positions sampled programmatically:
  ```
  [VPad] test stick -> 1
  [VPad] player start (100.56,-73.82,2.40)   puppet start (102.56,-73.82,2.24)
  [VPad] PLAYER moved2=0.81   (dx=0.89 dz=-0.08)
  [VPad] PUPPET moved2=0.00   (dx=0.00 dz=0.00)
  ```
  So SP merges pad input onto the single local player; the `pad -> entity` map is real but the SP input
  path does not consult it. **Route closed.**
- Consequence: the "engine-native second player for free" route is **closed**. The v2 model (mirror +
  events) is the ceiling for this engine, exactly as the gate was designed to decide.
- Practical path forward: keep v2, and if guest input is wanted, feed it to **our own** puppet drive
  (HumanGroundData / action replication — already implemented) instead of trying to make the engine treat
  it as player 2. Combat for the peer then stays with the event lane (P1), not native engine combat.

## Update (after the Route B test)

**Route B WORKS.** With pad-1 state applied to the puppet's own `HumanGroundData` (`m_IsSet`,
`m_DestSpeedRatio`, `m_DestHeading`), the puppet **travelled across the ground, played a walk animation
and interacted with its surroundings** (user-observed).

- The 2-second position probe read `PUPPET moved2=0.00` while the user saw it walk � the probe is
  measuring the wrong entity/window and must be fixed before it is trusted again. Observation > probe here.
- Native second-player routing remains closed (sub_18317F0's controller binding), but it is no longer
  needed: input relay can go peer pad -> our drive -> puppet.
- Next: map the pad axes properly (stick magnitude -> speed ratio, direction -> heading) instead of the
  constant full-forward test vector, then this is real input relay.
