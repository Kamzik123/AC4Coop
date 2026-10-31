# PLAN — enemy sync and the path to a playable co-op loop
**2026-10-30, supersedes the entity-lane sections of PROGRAM-PLAN.md**

---

## 1. Where we actually are (proven, not assumed)

| subsystem | state | evidence |
|---|---|---|
| transport | **works** | `rx>0` both ways over Radmin, ~200 ms RTT; synthetic peer loopback also clean |
| player puppet | **works, now cheap** | render-only matrix writes, collision disabled, `avgD≈0.06 m`, 33 kB/s |
| enemy census | **broken** | `enemies=0` out of ~5000 candidate-sees while standing among soldiers |
| enemy identity | **fragile** | matching = host `objId` → engine resolver → sticky table → nearest position within 1.5 m; ~40 % unresolved before the scope filter |
| death lane host→guest | **works** | `[KT] death push` → guest `[KF] SetLife(-1) rc=0 36→65535` |
| death lane guest→host | **built, unverified** | `GuestDeathScan` + `[GKT]`; the old lane never existed on a guest |
| death crash | **fixed** | `0x67B2EA` = `mov edx,[ecx]`, `this->[0x114]` null, same family as `0x6797DC` |
| corpse state | **fixed, unverified** | 8-tick latch after the tick was re-enabling behaviour |
| ships | **fixed, unverified** | real `objId`; the guest's Jackdaw is now unaddressable |

**The single root problem is identity.** Every remaining symptom — "not there", "wrong guard", "idling instead of dead", "nobod=1314" — is one question: *which local body is the host's entity?* Position matching is a guess, and guesses do not compose into stealth, combat or missions.

---

## 2. Architecture decision

> **The host owns every enemy. The guest owns a display copy bound to a host-assigned slot id.**

Four rules, and nothing else is allowed to touch enemy state:

1. **Host** enumerates enemies each snapshot, assigns each a stable `slot` (u16) that persists for the entity's lifetime, and sends `slot, archetype, transform, action, blend, dead, hp`.
2. **Guest** keeps `slot → local body`, bound **once** (nearest match at first sight, archetype-checked), then sticky forever, evicted only when the body dies or the slot ends.
3. If a slot has **no local body**, the guest **spawns** one from the host's archetype (Phase B). No proxying of a random nearby civilian — that is what makes it look wrong today.
4. **Guest never simulates a bound enemy**: no AI, no local transform authority, no local death decisions. Host state in, render out.

Why this and not "match better": matching has a ceiling — two independently seeded worlds will always have a different cast in view. Spawning removes the ceiling.

---

## 3. Gates (each has a machine-checkable oracle; no human opinion)

| gate | question | oracle | pass |
|---|---|---|---|
| **G6** | does the host know which NPCs are enemies? | `[Sync] scope` + discriminator dump | ≥ 1 enemy found whenever the player is within 30 m of a soldier, and 0 for pure-civilian crowds over a 60 s sample in both places |
| **G7** | does the guest have the same enemies? | new `[Sync] bind` report per window: `sent=N bound=B spawned=S unmatched=U meanErr=E` | `B+S == N` for 60 consecutive windows, `meanErr < 0.5 m` |
| **G8** | do them look identical? | side-by-side screenshots at a fixed pose + `[Smooth] maxD` | same pose within one frame, `maxD < 0.35 m` at 20 Hz, no flicker in a 30 s capture |
| **G9** | does a kill land deterministically? | scripted 10-kill run, alternating killer | 10/10 on both screens, guest log `applied=1`, no `0x67B2EA`, corpse settles (no revive) |
| **G10** | does stealth respect the other player? | guard detection count with the peer hidden vs exposed | the host's guards detect the *guest's* body when it is in line of sight (this is the gate that decides whether co-op stealth is possible at all) |
| **G11** | naval | sail, board, broadside, sink | no flicker, hull HP matches, no drowning, crew on the right deck |
| **G12** | progression | start mission on A, join on B | both enter the same mission objective set; no desync of objectives |

---

## 4. Phases

### Phase A — identity foundation (this session)
- **A1** find the enemy discriminator. **RESULT (2026-10-30, run 51A70717F838, player standing among guards):**
  - `[Sync] ai n=4897 vt=026D3CD8 x4897 distinct=1` — **every** human NPC instantiates the same
    AI class. The AI vtable is not a discriminator.
  - `weapon_A8=0 icon_D0=0` across ~4,900 candidate-sees in three separate locations, while soldiers
    were visibly present. Both vtables are genuine (`off_27106A8` set by sub_19E16E0,
    `off_269E9D0` set by sub_11AD500) but neither is attached to the NPCs we enumerate.
  - The component histogram contains obvious garbage ids (`C7FFFF7F`, `3F3F3F3F`, `2C880575`,
    `00000003`), so the component walk (entity+0x60, count at +0x64/+0x66) is over-reading the real
    array. Any predicate built on that walk is unreliable — which is also why the two "proven"
    ids never fire.
  **Conclusion: there is no cheap class discriminator available to us.** Class-based
  "enemies only" cannot be delivered without a dedicated RE pass on the entity component model.
- **A1-RESULT (final): the discriminator exists and is cheap.** `entity+0x9C` is a packed u32:
  `kind = v & 7`, `role = (v >> 3) & 0x3F`; **IsSoldier = kind==3 && role==1**, **IsCivilian = (2,2)**.
  Proven against the game's own predicates (`sub_73B4B0` guard, `sub_54EC00` civilian, role name
  table `sub_1406C70`: 1=Guard, 2=Civilian, 3=Thief, 5=Mercenary, 19=Animal, 23=Naval).
  One dword load — no component walk, so it is immune to the `+0x64/+0x66` bug that invalidated every
  earlier classification. The earlier "class-based filtering is impossible" conclusion was correct
  only for component-based filtering; it is _not_ true for the entity kind/role word.
  Status: **VERIFIED-BINARY (C21)**, live confirmation queued as QA T14, implementation as IMPL T13.
- **A1-DECISION (superseded):** proximity scope stays as the fallback; class filtering returns via
  `kind/role` now that a proven discriminator exists.
- **A2** replace the host `objId` on the wire with a **stable slot id** (host-side table, 256 entries, assigned on first sight, released on despawn).
- **A3** guest binding: bind once, sticky, archetype-checked, evict on death/despawn. Emit the `[Sync] bind` report.
- **Exit:** G6 and G7 green with `SyncEnemiesOnly=1` (no spawning yet — `unmatched` may be > 0, but `meanErr` for bound bodies must be < 0.5 m).

### Phase B — guest-side spawning (the 100 % step)
- **B1** RE the entity factory: how `SpawnPuppet` builds the player, and what selects an NPC archetype. Reuse the proven `Human` chain; no new offsets without a live readback.
- **B2** spawn a display body per unmatched slot from the host's archetype id, then bind it.
- **B3** cap the spawn budget (e.g. 24 bodies within 60 m of the peer), despawning on despawn/out-of-range.
- **Exit:** G7 with `spawned > 0` and `unmatched == 0`.

### Phase C — death lane (finish what is built)
- **C1** verify G9 both directions with the current `[GKT]` lane.
- **C2** make death idempotent by slot, not by entity pointer.
- **C3** corpse latch verification; if the corpse still revives, tune `CORPSE_LATCH_TICKS`.
- **Exit:** G9 green.

### Phase D — stealth and combat parity
- **D1** G10 experiment: do host guards detect the guest's body? If they only ever see the guest as a non-entity, co-op stealth needs the host to *place* a proxy the AI can see.
- **D2** player-vs-player combat: decide whether the ghost is targetable (probably not initially).
- **Exit:** G10 answer recorded as a decision, green or explicitly cut.

### Phase E — naval
- **E1** verify the new ship identity end-to-end (sail, ram, broadside).
- **E2** hull HP and sinking via the outcome lane.
- **Exit:** G11.

### Phase F — progression
- **F1** objectives as grow-only sets (already the design); mission start/complete on the host propagates.
- **Exit:** G12.

---

## 5. Immediate sequence (next 60 minutes)

1. **A1 in flight** — `51A70717F838` deployed; one run among guards gives the AI-vtable clusters.
2. Classify: pick the AI vtable(s) that appear *only* around armed NPCs; confirm on a second location.
3. Swap the predicate, rebuild, verify `enemies>0` and `sent>0` in the same spot.
4. Then A2/A3 as one patch (slot ids + bind report), because they are useless apart.

## 6. Risks and their mitigation

| risk | mitigation |
|---|---|
| AI vtable is also shared by civilians | fall back to a two-signal test (AI vtable **and** a component only soldiers have), learned from the same dump |
| spawning NPCs crashes the engine | Phase B is gated behind `SpawnEnemies=0` default; each spawn is one entity, verified with readback before the next |
| 20 Hz + 200 ms makes parry timing impossible | animation-only replication on the guest; combat resolution stays host-side, so timing never crosses the wire |
| scope creep into "sync everything" | the rule in §2: enemies only, everything else is local |

## 7. Explicitly out of scope

- civilian/ambient synchronisation (product decision — they stay local and may differ)
- player-vs-player damage
- anything requiring a second instance on one PC (retired, D-019)
