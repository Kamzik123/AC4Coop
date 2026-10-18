# DECOMP AUDIT — every claim we built on, re-checked against the binary (2026-10-17)

Sources: `C:\Users\Administrator\ac4-decomp` (128,340 functions, one `.c` per function by address),
`findstr /s` for whole-tree search, `gamedb` for indexing. "verified" = decomp + live, "decomp" = read
from the decompilation only, "inference" = reasoned but not yet proven.

This document corrects the journal and `PLAN-v2` where the decomp disagrees with what we had concluded.

---

## 1. The kill feature — one real correction

### 1.1 There are TWO different "dead" predicates, and we only satisfy one

`sub_133F550` is the engine's **combat is-alive** test (called by the damage handler `sub_173E190` on the
damage target):

```c
bool sub_133F550(int a1)                       // a1 = ENTITY (not health!)
{
  v2 = sub_7025F0(a1);                         // entity+0xE8 -> +0x1C  = the AI
  if (!v2 || (*(_BYTE *)(v2 + 184) & 0xC) != 4) {   // ai+0xB8 & 0xC == 4  -> DEAD
    v3 = sub_10EF8C0(a1);                      // ai+0x5C service array -> class 1588608236 (0x5EB03CEC)
    if (!v3 || (v3[1] & 0x1000) == 0) return true;  // that service's +4 bit 0x1000 -> DEAD
  }
  return false;
}
```

`sub_753330` is a **different** predicate, and it is the one we used:

```c
BOOL sub_753330(int this) { v1 = *(BYTE**)(this+0x98); return v1 && *v1 || *(BYTE*)(this+0x8C); }
```

`this+0x98` is a cached pointer to `health+0x63` (verified live), `this+0x8C` is a byte on the AI.

**Consequence:** writing `ai+0x8C = 1` satisfies `sub_753330` (the AI stops — which is why the guard
froze) but does **not** satisfy `sub_133F550` (combat still sees it alive). Our real kill works because
`SetLife(hp, -1)` makes the engine run its own death path, which is what actually sets the +0xB8 /
service-flag state. The extra `ai+0x8C = 1` is at best redundant and at worst desynchronises the two
predicates.

**Correction to `PLAN-v2` and the journal:**
- Authoritative dead test to *read*: `sub_133F550(entity)` — one engine call, both terms handled.
- Authoritative way to *cause* death: `SetLife(hp, -1)` only. Do not hand-write `+0x8C`.
- If a synthetic flag write is ever needed, the ones that matter are `ai+0xB8 |= 4` and/or bit `0x1000` at
  `service+4` for class `0x5EB03CEC`, **not** `+0x8C`.

### 1.2 The AI has TWO service arrays

`sub_1370E20` (the is-alive companion lookup) walks `ai+0x5C` with count at `ai+0x62`; the health getter
`sub_1371140` walks the container's `+0x70` array with count at `+0x76`. Both are real and they are
different lists. Our health mapping (verified live: 20 entries, health + death service) used **`+0x70`** —
correct. The is-alive service lives in **`+0x5C`**.

### 1.3 Dead NPCs leave the snapshot enumeration — inference, now strongly supported

Our in-game test showed: kill happens, but `es.dead` never reaches the apply side, and the `[Cap]`
diagnostic never fires (so the entity is not even enumerated as `dead=0`). Combined with the engine being
unable to answer `sub_133F550` for an entity whose `+0xE8` is gone, the consistent explanation is that the
dead NPC is removed from `m_EntityManager` (or its AI torn down) the same frame. **Still worth one direct
probe**, but the architecture already handles it: deaths are pushed as events, not read from snapshots.

---

## 2. New capabilities the decomp hands us (already used or ready)

| capability | evidence | status |
|---|---|---|
| `SetLife(hp, v)` takes **any** life value; only `v<0` fires death | `19FB4C0.c` | **used** (P1 damage lane, verified live) |
| Host-assigned identity via `HandleContainer::m_ObjectID` | `BaseTypes.h`, `0x9F0B50` | **used** (P0, verified by objId resolution) |
| Is-alive as one engine call | `133F550.c` | **not yet used** — switch detection to it |
| Ragdoll pipeline exists and is named | `5863D0.c` stages `Anim::*ForRagdoll`, `PhysicForRagdollUpdate` | open (P2) |
| Input device registry: 560-byte entries at `devMgr+1896`, counts `+1902/+1908`, XInput slot `+532`; `XInputGetState` only in `0xA065D0`, `0xA28A50`, `0xABF21A` | `A065D0.c` | open (P3) — the old "device wall" is not a wall |
| Full call graph for anything | `gamedb graph` | available (index building) |

---

## 3. Claims from earlier sessions, re-graded

| claim | grade now | note |
|---|---|---|
| "health objects are CSrvNPCHealth (vt 0x02712F60)" | **verified** | heap scan + service table + CRC32 class id |
| "Life/Max/flags at +0x5C/+0x5E/+0x60" | **verified** | live reads + `SetLife` decomp |
| "dead encoding is Life=0xFFFF, flags\|=0x01000000" | **verified** | observed on a natural death; also = `SetLife(-1)` |
| "write Life=0xFFFF by hand to kill" | **WRONG** | the setter's *event* is the trigger; direct writes did nothing |
| "`ai+0xB8` is the death flag" (early) | **partly right** | it is one of two terms in `sub_133F550`; it does not change on every death |
| "`ai+0x8C` is the dead flag" (mid) | **partly right** | true for `sub_753330` (AI-side), not for combat is-alive |
| "`ai+0x98` points at a death record" | **WRONG** | it is `health+0x63` (verified live) |
| "GetEntityAI = `*(*(e+0xE8)+0x1C)`" | **verified** | `6F5F70.c` |
| "ship hull HP is `*(*(ship+0x5C)+0x84)`" | **no support** | not found by any decomp search; that field is the regenerating value we already rejected |
| "hull HP is a CSrvHealth object" | **disproven live** | 0 of 21 health objects changed under real hull damage |
| "ship AI has a service table" | **disproven live** | `ai+0x70` NULL, count 0 for the Jackdaw |
| "input relay is blocked by the device layer" | **superseded** | device registry is fully described; hook `XInputGetState` |
| "keep-AI + forced transform crashes" | **verified empirical** | still true; decomp explains it (AI writes the transform after us in the same frame) |
| "local combat on the guest needs a skeleton-pose seam" | **still true** | ragdoll/anim pipeline is the place to look, but not a drop-in |
| "quest state is SaveGame/MissionHistory" | **decomp-supported** | no CryEngine-style fact tokens |
| "corpse must not set `m_DriveEntity=false`" | **verified live** | it stops root-motion application and leaves the body standing |
| "`SetLayer(0x29)` removes corpse collision" | **verified live** | `636480.c` propagates the filter to the physics object |

---

## 4. What this changes in the plan

- **P0/P1 (done)** stay, with one amendment: read death via `sub_133F550(entity)`; stop writing `ai+0x8C`.
- **P2 (corpse)** gets a concrete map instead of guesswork: the named `*ForRagdoll*` update stages, plus
  the `HUM+0x4C` motion swap observed on a real death.
- **P3 (input)** changes from "blocked" to "bounded": three `XInputGetState` call sites and a fully
  described device array.
- **P4 (ships)** changes method: not a health object and not in the AI service table; must be found by
  reading the ship class's own update/damage code, not by diffing a moving ship.
- **P5 (quest)** unchanged, but now cheap to read.

---

## 5. Open questions + the exact next probe for each

1. **Is a dead NPC removed from `m_EntityManager`, or just skipped?**
   Probe: after a kill, scan `m_Entities` for the killed pointer and log found/not-found. (Decides whether
   the guest can ever be told by snapshot instead of event.)
2. **What sets the `0x1000` bit for service class `0x5EB03CEC`?**
   Lookup: find writers of `+4 |= 0x1000` near the NPC death path; that is the real death flag setter.
3. **Ragdoll entry point**: find who swaps `Human+0x4C` on a genuine death (the motion object). Lookup:
   callers of the `*ForRagdoll*` stage functions in `5863D0.c`.
4. **Ship hull HP**: read the ship class damage code (vtable `0x01E4A128` users) rather than diffing.
5. **Local combat**: scope the skeleton-pose seam using the ragdoll bone-update stage as the template.

---

## 6. Addendum — verified live: `SetLife` alone does NOT make the guard combat-dead

Test sequence on a live soldier:

```
[KF] before: alive=1  health life=36/36 flags=00000004
[KF] SetLife(-1) rc=0  life 36 -> 65535
[KF] after : alive=1  health life=65535 flags=01000004      <-- still alive to combat!
```

Then, setting the flag the decomp pointed at:

```
[KF] is-alive service=FCAC40F0 +4 00000000 -> 00001000
[KF] after : alive=0  health life=65535 flags=01000004
```

**Corrected kill recipe (verified):**
1. `SetLife(hp, -1)` — engine death event; Life/flags.
2. `hp->[+0x60] |= 0x01000000`.
3. `svc = sub_10EF8C0(entity)`; `svc[1] |= 0x1000` — **this** is the combat-dead term.
4. `ai->[0x8C] = 1` — AI-side stop only (sub_753330); it is *not* the combat flag.
5. death clip + `SetLayer(0x29)`.

The earlier belief "SetLife(-1) is the whole death" was wrong; it produces the health state and the event,
but combat is-alive is a separate service flag. This also explains why the guard previously needed the
`+0x8C` write to stop behaving: the AI-side predicate was the only one being satisfied.

## 7. Rule this audit enforces

Nothing in the plan may cite an offset or a flag as fact unless it is (a) read out of the decompilation
*and* (b) confirmed live, or explicitly marked **inference**. Three of our earlier "facts" were half-right
in exactly the way that cost days: `+0xB8`, `+0x8C`, and `+0x98`. All three are now resolved precisely.
