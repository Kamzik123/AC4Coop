# E3 — shared stealth: experiment cards

> **Verdict (2026-10-29, BIN) — this chain reads the LOCAL PLAYER, not a guard.**
>
> `*(entity + 0x5C)` is `m_AssociatedWorld` — the shared `World` object
> (`m_EntityFlags@+0x50`, `m_DataOwner@+0x58`, `m_AssociatedWorld@+0x5C`,
> `m_Components@+0x60`; the component array is binary-verified at `+0x60` by
> `0x4375D0`). `World::GetCurrentWorld` (`0x54D4D0`) returns that one world from the
> global manager. `+0x8E0` is a **single fixed slot** on it
> (`0x139891F mov esi,[eax+8E0h]`, no index).
>
> So every entity's `+0x5C` points at the same World, and
> `*(*(entity+0x5C)+0x8E0)+0x1C` returns **the same alert actor for every guard** —
> the player's, or whatever single entity the world tracks. It cannot read an arbitrary
> guard's alert level. **Do not build 3A on it.**
>
> What *is* verified: the three predicates and the fields they read
> (`sub_1013BA0` In Conflict → bitset `ctx+0x40`; `sub_1002E20` Alerted → `u16 ctx+0x146`;
> `sub_1013D20` Investigating → bitset `ctx+0x40`), and that `sub_13DD550` is the
> string helper.
>
> What is **UNVERIFIED**: how to reach a *guard's own* `ctx`. Candidate routes, none proven:
>
> 1. the AI object `GetEntityAI(guard)` = `*(*(guard+0xE8)+0x1C)`, whose `m_AIState` is at
>    `+0xB8` — the same object the alive predicate reads as `(&0xC) == 4`;
> 2. a per-entity detection component not yet located.
>
> **E3.1 therefore starts with finding that route**, not with writing `+0x146`.
>
> **Update (same day): the per-entity object exists.** The conflict-bit *writer*
> `sub_17AE740` (registered in the table at `0x29eda08`) reads a **per-entity** argument:
>
> ```
> 0x17AE74D  mov eax,[ebp+8]        ; arg1 = the per-entity object
> 0x17AE750  mov eax,[eax+0Ch]      ; -> AIAlertState   (note: +0x0C, not +0x1C)
> 0x17AE754  mov esi,[eax+40h]      ; bitset
> 0x17AE76E  cmp [eax+146h],0       ; alerted u16
> ```
>
> So there are **two** `AIAlertState` holders: the predicate's is at `World+0x8E0 → +0x1C`
> (the player's), the writer's is at `arg1 → +0x0C` (per entity). Offsets:
> conflict index `u16@(off_29EDE38+0x42)`, alerted `u16@+0x146`, investigating
> `off_29EEB9C` (no writer found — set by a computed index).
>
> **Still UNVERIFIED: `Entity* → arg1`.** `sub_17AE740` is an indirect callback; the
> dispatcher that calls it is what settles arg1's type. That is the one call site left.
>
> **Second update: `arg1` is a global state-machine node, not a per-NPC object.**
> `sub_17AE740/790` are wired by `sub_1786BE0`, a singleton init
> (`if (!dword_4E1982C) dword_4E1982C = sub_9FDD50(0)`, no args, no callers of its own).
> The node graph is built from one global root via `sub_9FDF50(0,parent)`, which sets
> `[obj+0]=vtable(off_25A508C)`, `[obj+0x24]=parent`, `[obj+0x34]=[parent+0x34]+1` — and
> **never writes `+0x0C`**. So `[arg1+0x0C]` is populated at runtime with *whichever
> entity is currently being processed*.
>
> Consequence: **the alert state is per-entity but the storage field on the entity is
> still unmapped.** The machine borrows it one NPC at a time.
>
> **Next address to read:** the writer of `[node+0xC]` — the detection-tick populate step
> that stores the current entity's AIAlertState into the global node. Reachable from
> xrefs to the `off_25A508C` methods (`sub_9FDD50`/`sub_9FDF50` siblings), or from the
> `dword_4E1982C` machine's `+0x0C` during a guard update.
>
> **Fallback if that writer is not found:** build C3 on a *coarse* level derived from data
> the entity lane already captures — `es.actionAlias` and `es.stage` (HumanGroundStage;
> Fight=4) — instead of the exact alert object. Weaker (no true Unaware/Investigating/
> Alerted/Conflict), shippable, and enough to make guards react visibly. Say so in the
> plan if chosen; do not pass it off as the real alert state.

Phase 3 (`PROGRAM-PLAN.md:270-320`). Two experiments decide whether C3 is possible
at all: E3.1 tests the **write** path (3A), E3.2 tests the **perception** path (3B).

## Verified starting facts

Quoted from `PROGRAM-PLAN.md:278-281`:

```
# ctx here is whatever object the predicate is called on; reaching a GUARD's ctx
# is the open question. The world+0x8E0 route gives the player's, not the guard's.
ctx     = *(void**)(state + 0x1C)
alerted = *(uint16_t*)(ctx + 0x146) != 0                       // sub_1002E20
conflict= bitset_test(ctx + 0x40, idx(off_29EDE38))            // sub_1013BA0
invest. = bitset_test(ctx + 0x40, idx(off_29EEB9C))            // sub_1013D20
```

* the alert predicates and their fields are readable (`sub_1013BA0` In Conflict,
  `sub_1013D20` Investigating, `sub_1002E20` Alerted) — but **only when you already
  hold a guard's own `ctx`**, and the route to that object is UNVERIFIED (see above);
* `sub_13DD550` is the string-returning helper;
* both are `[V]` (verified this session against `AC4BFSP.exe` sha256
  `bc11ad79…`, `PROGRAM-PLAN.md:13`).

Everything else on this page marked **UNVERIFIED** is what the card settles.

---

## E3.1 — Is `ctx+0x146` writable and stable?

**Plan text** (`PROGRAM-PLAN.md:289-291`): *"whether writing `+0x146` is stable or
recomputed by the perception tick. Experiment E3.1: write it, watch 5 s. If it is
recomputed, we must inject the stimulus instead (3B)."*

| | |
|---|---|
| question | if we write `u16 ctx+0x146` (and the `+0x40` bitset) on a live guard, does the value hold, and for how long? |
| cost | 1 session |
| kills | the 3A write path |

**Procedure (one session).**

1. Spawn in Havana free roam; pick one guard and resolve `ctx` with the settled chain —
   `ctx = *(*(*(guard + 0x5C) + 0x8E0) + 0x1C)`. **Do not** route through
   `GetEntityAI`: it returns a different object (`AbstractEntityAI`, `m_AIState@+0xB8`).
   Log `guardObjId`, `ctx` VA.
2. Provoke detection until `+0x146 != 0` and the In Conflict bit is set; confirm
   with `sub_13DD550` that the guard's alert string matches.
3. Write `u16 0` to `ctx+0x146`; if the `+0x40` bit for `off_29EDE38` is set, clear
   that bit too. Note the tick of the write.
4. Sample `+0x146` and the `+0x40` bitset at 10 Hz for 5 s (`PROGRAM-PLAN.md:284`
   already assumes a 10 Hz publish rate).
5. Repeat 3–4 three times on three different guards, then once on a guard that has
   never been alerted.

**What to log.** One line per sample: `tick, guardObjId, ctx, w146, bitConflict,
bitInvestigate, writeTick, writeValue`. Plus a one-line header with the ASI hash
(same rule as any evidence log — see `ops/evidence/README.md`).

**PASS.** The written value survives the whole 5 s window on at least 3 of 3
guards, and a write on a never-alerted guard does not make the guard attack
(sanity: we did not create a fake alert). Record per-guard survival in seconds.

**FAIL.** The value returns to its pre-write state within 5 s ⇒ the perception tick
recomputes it ⇒ 3A cannot write level directly; escalate to 3B (stimulus
injection, `PROGRAM-PLAN.md:293-312`).

**Kill criterion.** 1 session. If the answer is FAIL, the 3A write path is dead —
do not retry it with a different offset before E3.2 is run.

**UNVERIFIED:** both the write stability *and* whether the `+0x40` bits are
recomputed on the same tick as `+0x146`. Both are settled by the 5 s sample above.

---

## E3.2 — Where is the AI perception candidate list?

**Plan text** (`PROGRAM-PLAN.md:300-307`): *"the missing piece is the perception
candidate list itself. Experiment E3.2: find the function that fills the guard's
'things I can see' container by watching a guard acquire the player (hardware
breakpoint on the player entity's reference count, or on the detection band
`sub_13DD550`'s feeder)."*

Disproved already **[D]**: `FocusFlags` at `VisualTargetingComponent+0x660` — *"the
field is a pointer"* (`PROGRAM-PLAN.md:295-296`); `Universe+0x08` is a
weak-observer list and `0x006608A0` ticks nothing (`PROGRAM-PLAN.md:301-302`). Do
not re-run those.

| | |
|---|---|
| question | which container holds a guard's candidate targets, and which function adds the *player* to it? |
| cost | 2–4 sessions |
| kills | 3B route 1 (fallback: stimulus injection) |

**Procedure (first session).**

1. Stand the player alone in front of one guard, nothing else in the cone; confirm
   acquisition by reading `+0x146 != 0` (E3.1's reader) and the `sub_13DD550`
   string.
2. Set a hardware write breakpoint on the player entity's reference count; step
   the cone entry/exit until the writer is a function that also walks a container.
3. Alternatively break on the feeder of `sub_13DD550` (the detection band) and
   walk back to whoever produced the candidate.
4. Record container base + count offsets and the element stride; dump the first
   entries and identify them by object id.
5. Re-enter the cone 3 times and confirm the same slot churns.

**What to log.** `tick, guardObjId, writerVA, containerBase, count, element[0..3]`
per breakpoint hit, plus the acquisition transition
(`alertBefore, alertAfter, ticksToAcquire`). Header with the ASI hash.

**PASS.** A container base + count is named, and the player appears in it at the
moment of acquisition. Stronger PASS (the one M3 actually needs): after inserting
the puppet object id into that container, the guard acquires and attacks the puppet
in ≥ 8 of 10 trials — the M3 acceptance number (`GATES.md:91`,
`PROGRAM-PLAN.md:316`).

**FAIL.** No container whose contents correlate with acquisition, after the
breakpoints fire and are attributed. **UNVERIFIED** until then.

**Kill criterion.** 4 sessions with no candidate list findable ⇒ go to route 2,
stimulus injection (`PROGRAM-PLAN.md:306-312`). Both routes failing defers C3 to
"C0–C2 + naval" (`PROGRAM-PLAN.md:319-320`); C4 and C5 do not depend on it, so do
not let this block them.

**Cost.** 2–4 sessions (`PROGRAM-PLAN.md:306`, `:397`).
