# Co-op plan review — with the full AC4 decompilation (2026-10-17)

Source of truth: `C:\Users\Administrator\ac4-decomp\` — **128,340 decompiled functions**, one `.c` per
function named by address (`19FB4C0.c` = `sub_19FB4C0`), 128 MB total. Indexed with `gamedb`
(`gamedb search|read|graph|sql -r C:\Users\Administrator\ac4-decomp`). `findstr /s /m /c:"…"` works too
and is fast.

This is the first time we can answer structural questions instead of probing memory. Several earlier
"walls" were artefacts of not knowing the code.

---

## 1. What the decompilation changes

### 1.1 Damage sync is nearly free — `CSrvNPCHealth::SetLife` takes an arbitrary value

`19FB4C0.c` (verified earlier live, re-read here):

```c
int __thiscall sub_19FB4C0(__int16 *this, int a2)   // health, newLife
{
  v3 = sub_1BFF520(this) < 0;          // currently dead?
  if ( !v3 || a2 >= 0 ) {
    *(this + 46) = a2;                 // +0x5C = Life  (word index 46)
    if ( a2 < 0 )
      return sub_A014A0(dword_4E1CC40, 0);   // fires the death event
  }
}
```

So the engine setter accepts **any** life value, and only a negative value is the death path.
That means we no longer need "kill only": the peer can send a hit and the host can call
`SetLife(hp, newLife)` with the exact value. **Full damage replication, not just deaths**, with no
health-write hacks and no invented offsets.

- `newLife > 0` → pure damage, no death event.
- `newLife <= 0` → call `SetLife(hp, -1)` to make the engine run the death (event + state machine).

This replaces the "only deaths are relayed" limitation in the current design.

### 1.2 There is a real ragdoll pipeline, and it is named

The world update-graph builder (`5863D0.c`) registers staged nodes, including:

```
Anim::UpdateActionBonesForRagdoll
Anim::SkeletonUpdateForRagdoll
PhysicForRagdollUpdate        (registered twice, pre/post)
ActionPostUpdateForRagdoll
```

So the corpse fall is a first-class engine path (skeleton-under-ragdoll + physics), not a cosmetic clip.
Our current approximation (`SetTargetAction(death clip)` + collision-layer disable) can eventually be
replaced by the real thing. The likely seam is the `Human` motion object that a real death swaps at
`HUM+0x4C` — the ragdoll motion — which is what we failed to fake with a hardcoded pointer.

### 1.3 There is a stable identity: `HandleContainer::m_ObjectID`

- `HandleContainer` (SDK `BaseTypes.h`) is `{ ManagedObject* m_Object; int m_SoftRefCount; int m_HardRefCount; uint64 m_ObjectID; }`.
- `ManagedObject::GetObjectID()` → engine `0x9F0B50`.

The relay's current identity is "nearest soldier within 4 m of a position" — ambiguous and drift-prone.
Correct design: **host-assigned identity**. The host captures each guard with its `m_ObjectID` into the
snapshot; the peer echoes that ID back on a hit; the host resolves ID → entity through the map it built.
Position becomes a sanity check, not the key. This removes the biggest correctness hole in the relay.

### 1.4 The input layer is not a black box after all

`A065D0.c` shows the device registry: entries are **560 bytes**, array at `devMgr + 1896`,
`devMgr + 1902/1908` = counts, entry `+532` = XInput user index, entry `+4` = device state/type.
`XInputGetState` is called directly in exactly **three** functions: `0xA065D0`, `0xA28A50`, `0xABF21A`.

So the old "input is device-based, guest has no device" wall is surmountable: hook `XInputGetState`
(or the engine wrapper) and answer for a synthetic slot. That is the remaining route to true guest input.

### 1.5 Confirmed: `GetEntityAI` can go blind

`6F5F70.c`: `return *(e+0xE8) ? *(*(e+0xE8)+0x1C) : 0;`

Every health/death lookup we do starts here, and `CaptureGuards` uses it for `es.dead`. If `+0xE8` (or
its `+0x1C`) is cleared when an NPC dies, `GetEntityAI` returns 0 and the death can never be captured —
which matches the observed "kill happened, but `es.dead` never became 1". **Unverified** (needs a live
check or a call-site read), but it is now the prime suspect and cheap to confirm.

### 1.6 The death service is only ever reached virtually

`off_2722528` (`CSrvNPCDeath` vtable) is referenced by **one** file: its own destructor (`1B98A90.c`).
So there is no direct call site to copy — the death entry has to be found through the object's vtable
slots or through the code that invokes them, not by searching for a direct call.

---

## 2. Re-assessment of the plan, workstream by workstream

| # | Workstream | Before | Now |
|---|---|---|---|
| 1 | **Relay correctness** (identity + ordering) | position guessing | **host-assigned `m_ObjectID`**, position as sanity check |
| 2 | **Damage sync** | deaths only | **per-hit via `SetLife`**, exact values |
| 3 | **Corpse / ragdoll** | clip + collision hack | real ragdoll path exists and is named; find the `HUM+0x4C` motion swap |
| 4 | **Input relay** | blocked ("device wall") | hook `XInputGetState` / inject a 560-byte device entry |
| 5 | **Ship hull HP** | unknown field | still unknown, but now cheap: the ship class is fully readable |
| 6 | **Quest / progress** | deep RE | fully readable (`SaveGame` / `MissionHistory`) |
| 7 | **Local combat on the peer** | needs skeleton-pose seam | ragdoll/anim pipeline is visible; scope it properly now |

---

## 3. Revised priority order

**P0 — make the current kill feature actually correct** (small, all local):
1. Identity: add `objId` to the snapshot; echo it on hits; host resolves by ID.
2. Unblock `es.dead`: stop depending on `GetEntityAI` after death. Cache `entity -> health` (and
   `entity -> ai`) at capture time, and read the cached health flags (`+0x60 & 0x01000000`) for the dead
   state. That is immune to `+0xE8` being cleared.
3. Do not drive a mirror we killed locally (already implemented).

**P1 — damage sync** (now cheap): send `{objId, newLife}` on damage; host calls `SetLife(hp, newLife)`,
or `SetLife(hp, -1)` when it should die. This turns "both players can kill" into "both players fight the
same guard".

**P2 — real corpse**: locate the `HUM+0x4C` motion swap on a genuine death (read the AI death state
machine / the ragdoll component ctor) and drive it instead of the clip hack. Use the named
`*ForRagdoll*` stages as the map.

**P3 — input relay**: hook `XInputGetState` (3 call sites) or synthesize a device entry, so the peer's
own input drives the host's combat. This is the "true co-op" step and is now unblocked in principle.

**P4 — ships**: find hull HP with the decomp (read the ship class; the earlier entity byte-diff was
inconclusive only because the ship was moving).

**P5 — quest/progress**: now readable; low priority until combat is done.

---

## 4. Concrete lookups to run next (with the index)

- `gamedb graph -r … 19FB4C0 --direction callers` — every damage/death caller of `SetLife`.
- `gamedb read -r … 753330` — the is-dead predicate, its callers, to find who *sets* dead.
- Search for the function that writes `Human+0x4C` on death (motion swap) — the ragdoll seam.
- `gamedb search -r … Hull` / ship class methods → hull HP field.
- The three `XInputGetState` call sites → where device state is consumed, to design the hook.

---

## 5. Honest status

- The relay is **not** end-to-end yet; the two-machine datagram is untested, and host→guest death
  propagation is still unproven (prime suspect identified above).
- The kill recipe works and is verified live (real death, animation, no collision).
- Ship hull HP remains unsolved — but no longer expensive to find.
- Everything here is either verified against the binary/live game, or explicitly labelled as a
  hypothesis to test.
