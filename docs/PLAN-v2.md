# AC4 Black Flag co-op — plan v2 (2026-10-17)

Supersedes the ladder in `PLAN.md` and the assumptions in `COOP-PROJECT-PLAN`. Grounded in the full
decompilation (`C:\Users\Administrator\ac4-decomp`, 128,340 functions) and the verified live results in
`MODLOG.md` / `docs/COOP-JOURNAL.md`.

## Objective

Two players in one host-authoritative world: shared NPCs, shared combat (both can damage and kill the same
guard, with damage and death visible on both machines), shared ships and progress. Guest keeps its own
input.

## Verified ground truth

| thing | value | status |
|---|---|---|
| `CSrvNPCHealth` vtable | `0x02712F60` | verified live |
| Life / Max / flags | `+0x5C` / `+0x5E` / `+0x60` | verified live |
| dead encoding | `Life=-1 (0xFFFF)`, `flags |= 0x01000000` | verified live |
| death trigger | `CSrvNPCHealth::SetLife(hp, v)` (`0x19FB4C0`) — **any value; only `v < 0` fires the death event** | verified (live + decomp) |
| is-dead predicate (combat, authoritative) | `sub_133F550(entity)`: `(ai+0xB8 & 0xC)==4` **or** bit `0x1000` at `+4` of the service with class id `0x5EB03CEC` (found in `ai+0x5C`, count `ai+0x62`) | decomp + live |
| is-dead predicate (AI-side, different) | `sub_753330`: `*(BYTE**)(ai+0x98) && **… || *(BYTE*)(ai+0x8C)`; `ai+0x98 == health+0x63` | verified |
| entity → health | `ai = GetEntityAI(e)`; service in `ai->[+0x70]` (count `+0x76`) with vtable `0x02712F60` (a different list from `ai+0x5C`) | verified live |
| `GetEntityAI` | `0x6F5F70`: `*(e+0xE8) ? *(*(e+0xE8)+0x1C) : 0` — **goes blind if `+0xE8` is cleared** | decomp |
| stable identity | `HandleContainer::m_ObjectID` (u64); `ManagedObject::GetObjectID` = `0x9F0B50` | decomp |
| death service | vtable `0x02722528`, reached only virtually (one file references the vtable: its dtor) | decomp |
| ragdoll pipeline | `Anim::*ForRagdoll`, `PhysicForRagdollUpdate`, `ActionPostUpdateForRagdoll` in `5863D0.c` | decomp |
| input devices | 560-byte entries at `devMgr+1896`, counts `+1902/+1908`, XInput slot `+532`; `XInputGetState` called in `0xA065D0`, `0xA28A50`, `0xABF21A` | decomp |
| kill recipe (current) | `SetLife(-1)` + flags + **is-alive service `+4 |= 0x1000`** + death clip `1818493434` + `SetLayer(0x29)` (+ `ai+0x8C=1` as AI-side stop) | verified live: engine is-alive flips to 0, guard dies, falls, no collision |
| ship hull HP | **unknown** — not a health object, not in the ship AI service table | open |

## Architecture

Host is authoritative for the world; the peer mirrors. Two lanes:

1. **Snapshot lane (host → peer, ~10 Hz)** — per guard: `objId`, transform, action clip, `hp`, `dead`.
   Peer matches by `objId` (position only as a fallback/validation) and drives the mirror.
2. **Event lane (peer → host, on change)** — `{objId, newLife}` for damage, `newLife = -1` for death.
   Host applies with `SetLife(hp, value)`.

Identity is **host-assigned**: `objId` comes from the host's own object table, the peer echoes it, the
host resolves it against the table it built during capture. No proximity guessing in the normal path.

## Phases (each must pass its oracle before moving on)

### P0 — Correct identity + reliable death detection
- Add `objId` (u64) and `hp` (u16) to `EntityState`; bump `NET_PROTO_VER`.
- Host: capture `objId` from `HandleContainer::m_ObjectID`; maintain `objId → {entity, health*}` for the
  current capture; death read from the **cached health** (`flags & 0x01000000`) rather than re-deriving
  `GetEntityAI` after death.
- Peer: remember `objId → local mirror`; detect damage/death on the cached health pointer; report by `objId`.
- Host: resolve events by `objId` first, position second.
- **Oracle:** in self-test, kill a mirror → the snapshot reports `dead=1` for *that* `objId`
  (log line), and the mirror is not driven afterwards. Two-machine: guest kill produces the host guard's
  death with the correct guard.

### P1 — Per-hit damage sync
- Peer sends `{objId, newLife}` whenever a mirror's Life changes (throttled ~10 Hz).
- Host: `SetLife(hp, newLife)` when positive; `SetLife(hp, -1)` when `<= 0`.
- Host snapshot carries `hp`; peer applies it to closed-loop the mirror.
- **Oracle:** guest hits a guard 3 times → host's guard loses exactly the same health; both HUDs agree.

### P2 — Engine-true corpse
- Find the real death path: locate the `Human+0x4C` motion swap on a genuine death (candidates: the AI
  death state machine, the ragdoll component ctor, the `*ForRagdoll*` update stages).
- Replace the death-clip + `SetLayer` approximation with the engine path.
- **Oracle:** a peer-reported kill produces a corpse identical in kind to a locally-killed guard
  (ragdoll drop, no clip hack, no collision).

### P3 — Input relay (true co-op combat)
- Hook `XInputGetState` (three call sites) or inject a 560-byte device entry for a synthetic pad.
- Peer streams its pad state into the host as a second pad; the host's own combat resolves damage.
- **Oracle:** guest attacks by its own input; the host sees the guard take damage from "player 2".

### P4 — Ship hull HP
- Find the real hull field with the decomp (ship class methods; the earlier entity diff was inconclusive
  only because the ship was moving). Use an anchored-ship typed diff as the oracle.

### P5 — Quest / progress
- `SaveGame` / `MissionHistory` are readable now; mirror mission state host→peer.

## Current status

- Kill recipe: **done**, verified live.
- Relay: host half verified solo; guest detection verified solo; **two-machine datagram untested**;
  host→guest death unproven (prime suspect: `GetEntityAI` blind after death).
- Mirrors: transform/clip drive works; self-test jitter fixed by skipping the redundant transform write.
- Ship hull HP: unknown.

## Testing reality

One machine only; Steam blocks a same-PC second instance. Every phase must be verifiable solo first via a
self-test harness, then confirmed on two machines by the tester. Both machines must run the same `.asi`
and the same `NET_PROTO_VER`.
