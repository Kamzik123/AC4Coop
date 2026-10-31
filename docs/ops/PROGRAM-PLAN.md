# Program plan — true co-op for AC4 Black Flag

Status: v1, 2026-10-29. Owner: ORCH.
Supersedes: the milestone tables in `COOP-MASTER-PLAN.md` and `COOP-ROADMAP.md`
(those stay as history and as the RE record; this is the execution plan).

---

## 0. Evidence tags used in this document

| tag | meaning |
|---|---|
| **[V]** | verified this session against `AC4BFSP.exe` (sha256 `bc11ad79…`) — address given |
| **[D]** | documented in the repo, not re-verified today |
| **[H]** | hypothesis. Has an experiment in §8. Never build on it without that experiment |

`AC4BFSP.exe` has **no DYNAMICBASE and no `.reloc` section** **[V, PE header,
DllCharacteristics=0x8100]**, so it loads at `0x400000` and absolute VAs are safe
*on this exact build*. That is why the build gate exists and why it is not optional.

---

## 1. What "true co-op" means, and what it cannot mean

The engine gives each machine its own world. There is no shared simulation to join
and no supported way to add a second player to one. That is not a temporary
limitation; it is the shape of the asset.

So "true co-op" is defined as:

> **Two players, two machines, one campaign.** Each machine simulates its own
> world. Everything that matters is replicated as **identity + state + outcomes**,
> with an explicit owner for each system, so that both players experience the same
> events, the same enemies and the same progress.

What that buys you: roaming, fighting, stealth, naval combat and mission progress
all *feel* shared, because the outcomes are identical and the AI reacts on both
machines.

What it cannot buy you: a guard that physically walks between both worlds, a
cannonball that exists in one shared physics space, or a mission script that runs
once. Those are re-expressed as events, not simulated twice.

### Capability ladder (product-facing)

| id | capability | the player-visible test |
|---|---|---|
| **C0** | Shared presence | Both players see each other, correctly placed, for 10 minutes without a crash. |
| **C1** | Shared movement | Walking, sprinting, parkour, swimming and climbing look the same on both screens. |
| **C2** | Shared combat | A kill on A is dead on B within 200 ms, 20/20 times, with a corpse and no false deaths. |
| **C3** | Shared stealth | The same guard reacts to the same player on both machines, and both players see the same alert state. |
| **C4** | Shared naval | Ship-vs-ship damage lands on the right ship on both machines; boarding transfers cleanly. |
| **C5** | Shared progression | Mission objectives complete once, on both sides, and survive save/load. |

C0–C1 = "co-op free roam". C2 = "it feels real". C3 = "it is a game". C4 = "the
fantasy". C5 = "a campaign".

---

## 2. The architectural thesis (read this before writing code)

Every feature request in this project — roam, fight, stealth, ship, missions — is
the same three primitives wearing different hats:

1. **Who** — a stable handle for an entity across two machines.
2. **What changed** — an ordered, idempotent event.
3. **Where** — a pose/state stream for things that move.

The project's history is a series of bespoke hooks, one per feature: a transform
writer, an action-alias lane, a ship matrix override, a health heap scan, a guard
mirror. Each worked alone, each needed its own crash guard, and none composed.
That is why every new feature re-opens the same questions.

**This plan builds the three primitives once, as a module (`repl/`), and then
expresses each capability as a schema on top of it.** A capability is done when its
schema is registered — not when another hook is written.

### 2.1 `repl/` — the four interfaces

```cpp
// 1. Identity: who is this, across machines?
struct EntityRef {
    uint32_t epoch;     // incremented on world/mission load; invalidates stale refs
    uint64_t objectId;  // HandleContainer::m_ObjectID  [D] 0x9F0B50 -> sub_A200D0
    uint16_t classTag;  // crc32(class name) & 0xFFFF, or a small enum
};
bool  ResolveRef(EntityRef, void** outEntity, uint32_t* outGeneration);

// 2. Events: what changed, decided once, applied everywhere.
struct Event {
    uint8_t  kind;       // Damage, Death, Alert, Objective, Boarding, ...
    uint32_t seq;        // per-sender monotonic
    uint32_t tick;       // sender's frame counter for ordering
    EntityRef actor, target;
    uint32_t payloadHash; // kind-specific blob
    uint8_t  body[64];
};
void  ReplicateEvent(const Event&);     // send side: authoritative local change
bool  ApplyRemoteEvent(const Event&);   // receive side: idempotent, tombstoned

// 3. Pose: where things are, at ~20 Hz, smoothed.
void  PublishPose(EntityRef, const Pose&);
Pose  SamplePose(EntityRef, double nowMs);

// 4. Ownership: who may write this system.
enum class Owner { LocalAvatar, PeerAvatar, Causal, GrowOnlySet, LocalOnly };
Owner AuthorityFor(uint8_t eventKind);
```

**Authority rules (the part that must not be fudged):**

| system | owner | conflict rule |
|---|---|---|
| local avatar pose | that machine | its own word is final |
| peer avatar pose | the peer | interpolate; never correct backwards |
| NPC damage | the **causer** (who dealt it) | LWW by `(tick, seq)` |
| NPC death | causer | **sticky tombstone** — first death wins, never resurrects |
| NPC alert state | the machine that observed the stimulus | LWW by tick, with a 300 ms quiet window to stop flapping |
| ship damage | the shooter (for the target's ship) | LWW; target machine applies to its own hull |
| mission objective | the machine that completed it | **grow-only set** — union, idempotent |
| save files | local only | never transferred |

**Why grow-only sets for objectives:** a mission objective is a boolean that can
only go false→true in a session. A union of sets is idempotent under duplication
and reordering, so it survives packet loss, resend and both players completing the
same objective in the same second. Nothing else in this project needs a CRDT; this
one does.

---

## 3. Reality check: what is already true

| fact | status |
|---|---|
| Deployed ASI == built ASI == kit ASI == HEAD (+4 bytes in `.text`) | [V] `f474422f…`, rebuilt today |
| 16 patch sites instruction-aligned, no overlaps | [V] capstone |
| Puppet spawns, is driven, and applies packets — **on one machine over loopback** | [D]/E2 |
| No two-machine run exists on disk | [V] all logs `peer=127.0.0.1`, `rx==tx` |
| Null-deref crash at `0x681817` (`sub_733FB0` result dereferenced after a null check) | [V] disasm + decompile |
| `sub_733FB0` returns null by design (3 paths) | [V] decompile |
| Deref guard covers 6 of 470 `[reg+0x114]` sites | [V] byte scan |
| Alive predicate = NOT(null ∨ AI-band ∨ service-bit) | [V] `sub_133F550` |
| `SetLife` stores at `+0x5C`, refuses to raise a negative life | [V] `sub_19FB4C0` |
| Action gate logic matches the documented rule | [V] `sub_679220` |
| Alert **predicates** are readable (`sub_1013BA0` conflict, `sub_1002E20` alerted, `sub_1013D20` investigating) | [V] — but the route to a **guard's own** `ctx` is UNVERIFIED: `entity+0x5C` is the shared `World`, so `world+0x8E0` is the player's slot, not a guard's | |
| Damage reaction handler candidate `sub_173E190` (0x5E9, `__thiscall(this,event)`) | [V] exists; semantics [H] |
| Damage-kind filter candidate `sub_17106C0` (reads `event+0x9C & 7`, kind table) | [V] exists; semantics [H] |
| IDB has 0.28% named functions and 0 engine structs | [V] `survey_binary` |

The last row matters for scheduling: **every offset named in this plan currently
lives in a markdown file**, not in the database. Phase 0 fixes that, because
otherwise five agents will each re-derive the same offsets and disagree.

---

## 4. Phase plan

Every phase has: entry criteria, the work, the gate, and the rollback. Gates are
machine-decidable and live in `GATES.md`. Nothing starts a phase before the
previous gate is green, except the explicitly parallel tracks marked ∥.

### Phase 0 — Truth and hygiene (3–5 days)

**Goal:** make it possible to trust a log and to run the same experiment twice.

| # | task | owner | done when |
|---|---|---|---|
| 0.1 | Fix `0x681817`: patch the join (`test edi,edi` path) or make `sub_733FB0`'s callers null-safe | BIN + NET | a 10-min run with the same scenario no longer faults; site added to `HOOK-TABLE.md` |
| 0.2 | Log the ASI's own sha256 + build time at load | NET | every `phase1_log_<pid>.txt` starts with `[Build] asi=<hash>` |
| 0.3 | Kill the site-enumeration guard: replace with one guard at the deref helper, or a `sub_733FB0` null-safe wrapper | NET | guard table shrinks to ≤ 2 entries and covers the class |
| 0.4 | Kit config: reconcile the 15 drift keys, delete `EntitySelfTestHP`, set `ReloadBuilder`/`IdResolver` deliberately, regenerate `SHA256SUMS.txt` | QA | `audit/config/project.json` config diff is explained line by line in `ops/DECISIONS.md` |
| 0.5 | IDB naming pass: the ~50 functions in `HOOK-TABLE.md`/`CODE-MAP.md`, and 6 structs (`Human`, `Entity`, `AIActor`, `EntityGroup`, `CharacterController`, `CSrvNPCHealth`) | BIN | `audit/ida/ida_findings.json` records ≥ 50 names and ≥ 6 engine structs |
| 0.6 | `STATUS.md` becomes generated: `ops/tools/gen_status.py` renders it from `audit.json` + `ops/STATE.json` | DOC | no hand-written status file remains |
| 0.7 | Freeze the contradictions: resolve or delete all 10 in `audit/ledger/contradictions.jsonl` | DOC | audit `AG4` green |

**Gate M0:** `run_audit.ps1 --rebuild` reports **AG0, AG2, AG4 and AG5** green (the audit rig's gates — `AG` prefix; the programme milestones below are `M0`–`M5`).

**Rollback:** none needed; this phase only removes ambiguity.

---

### Phase 1 — C0/C1: shared presence and movement (1–2 weeks)

**Entry:** M0 green, **and** either a second machine *or* E0-A (two instances from
one install) passing. E0-A is the cheap path and is described in
`experiments/E0-A-two-instance.md`; it has never been run, and it either works or
it names a 20-line transport bug.

| # | task | owner | detail |
|---|---|---|---|
| 1.1 | Two-machine run #1 (log-only, no new code) | QA + H | 60 s walk, both logs kept, `peer=<real IP>` |
| 1.2 | Join flow: `HOST.ini` / `GUEST.ini` become the only supported path; one key derives role, peer address, ports | NET | no manual IP edits in a README |
| 1.3 | Session lifecycle: hello carries save fingerprint + build hash; mismatch is a loud refusal, not silence | NET | `NET_PROTO_VER=10` |
| 1.4 | Puppet collision: pick a seam from the verified collision map, or keep the spawn offset until a seam is proven | NET | documented decision, no blind writes |
| 1.5 | Motion fidelity: `maxD` histogram over 10 min; fix the 4.83 m spikes by measurement, not guess | NET + QA | p99 ≤ 1.0 m, max ≤ 2.0 m |
| 1.6 | Name tag + marker + "peer is in another cell" UX | UI | visible on both screens |
| 1.7 | Interpolation delay derived from data: `p95 one-way + one interval`, not 150 ms | NET | logged each session |

**Gate M1 (C0+C1):**
10-minute free roam on two machines, both logs show `rx>0`, `[Net] apply` per
packet, `maxD ≤ 2.0 m`, `drop=0 wipe=0 clamp=0`, no crash, both players report
the gait matches. **Human sign-off recorded with build hash + timestamp.**

Procedure, config and failure modes: `TWO-MACHINE-RUNBOOK.md`.
What a synthetic peer can prove first (one machine): `experiments/E0-B-synthetic-peer.md`.

**Rollback:** if `maxD` cannot be brought under 2.0 m in 5 sessions, ship C0 with
a teleport-style pose correction and put C1 behind a config flag.

---

### Phase 2 — C2: shared combat (3–5 weeks)

**Entry:** M1 green. Combat is the first real test of the event model.

#### Stage 2A — the damage seam (1 week, RE spike, parallel ∥)

The project already knows the *symptoms*. What it never pinned is a **callable
apply path**. Two candidates, both verified to exist, neither verified to be the
right thing:

* `sub_173E190` — 0x5E9-byte `__thiscall(this, event)`, reads a large event
  object, calls several vtable slots, and writes state **[V: exists]**;
* `sub_17106C0` — 0x68-byte `__thiscall(this, a2, a3)`, tests
  `*(a2+0x9C) & 7 == 2`, `& 0x1F8 == 152`, then an accepted-kind table
  (`3,6,14,23,5,25,26`) **[V: exists]**. This *looks* like a filter, and a filter
  is exactly the seam we want: it sees every damage event before it is applied.

**Experiment E2.1 (falsification-first):** hook `sub_17106C0` read-only, punch a
guard once, and log `(event kind, `*(event+0x9C)`, source id, target id, amount)`.
Kill criterion: if the function is not called on a melee hit, it is not the damage
filter and stage 2A restarts from the health service (`sub_10EF8C0` → `class
0x5EB03CEC`, `sub_19FB4C0` SetLife) **[V both]**.

**Experiment E2.2:** log callers of `sub_173E190` and whether the event object
carries a victim id. Kill criterion: no victim identity ⇒ record/replay cannot use
it and we fall back to health-diff events (§2 alternative below).

#### Stage 2B — event-sourced damage (2 weeks)

Once 2A names the seam:

1. **Record** — on every accepted damage event, emit
   `Damage{causerRef, targetRef, amount, kind, tick, seq}`.
2. **Replay** — on the peer, resolve `targetRef` (by `objectId` + `epoch`), and
   apply through the same engine path. Never write `Life`/flags directly: that is
   the disproven forged-state route which produced looping animations and a
   missing corpse.
3. **Death** — applied by the same path; additionally publish a sticky
   `Death{targetRef, tick}` tombstone so a late packet cannot resurrect.
4. **Corpse/marker** — if they do not appear via the engine path, that is a
   finding, not something to fake.

**Fallback if no apply path exists:** health-diff events. Poll the target's
`SetLife` storage (`+0x5C`) at 10 Hz, emit `Delta{victim, -N}` when it drops, and
replay by calling `sub_19FB4C0(hp, newLife)` **[V signature]**. This is *inferior*
(no damage kinds, no hit direction) but it is callable today and it is honest.
Decide between the two at the end of 2A; record the decision.

**Gate M2 (C2):**
20/20 kills matched on two machines, 0 false deaths, corpse present on both,
`≤ 200 ms` from the killing blow to the death on the peer, and a 15-minute session
with combat that does not crash.

**Rollback / kill:** if 20/20 cannot be reached in 3 weeks of 2B, ship
"host-authoritative highlight": only the *outcome* of a fight (the guard is dead,
the corpse exists) is replicated, without per-hit damage. That is C2-lite and it
is shippable.

---

### Phase 3 — C3: shared stealth (4–8 weeks, highest risk)

**Entry:** M2 green.

Stealth has two halves, and they have very different risk.

#### 3A — alert-state replication — **blocked: the read route is not found**

The predicates are verified **[V]** — `sub_1013BA0` (In Conflict) and `sub_1013D20`
(Investigating) read a bitset at `ctx+0x40`; `sub_1002E20` (Alerted) reads
`u16 ctx+0x146`. **What is not verified is how to obtain a *guard's* `ctx`.**

The obvious route is wrong: `*(entity+0x5C)` is `m_AssociatedWorld`, the shared
`World` (`0x54D4D0` returns the one current world), and `+0x8E0` is a single fixed
slot on it (`0x139891F mov esi,[eax+8E0h]`, no index). Every entity's `+0x5C`
therefore points at the same World, and that chain returns **the player's** alert
actor for every guard — plausible garbage, not a guard's state.

Candidate routes, none proven:

1. the AI object `GetEntityAI(guard)` = `*(*(guard+0xE8)+0x1C)`, `m_AIState@+0xB8`
   (the same object whose `(&0xC) == 4` band the alive predicate reads);
2. a per-entity detection component not yet located.

Work: find the per-entity route first (one RE session, `ops/experiments/E3-stealth.md`).
**Status 2026-10-29:** the per-entity `AIAlertState` exists — `arg1+0x0C`, bitset `+0x40`,
u16 `+0x146` — but `arg1` is a **global detection state-machine node**
(`dword_4E1982C`, built from one root via `sub_9FDF50`) whose `+0x0C` is populated per
tick with whichever entity is being processed. The entity-side storage field is still
unmapped; the next read is the **writer of `[node+0x0C]`**.

If that writer is not found in one more session, build C3 on a **coarse** level derived
from what the entity lane already captures (`es.actionAlias`, `es.stage` — HumanGroundStage
Fight=4) and label it coarse, never as the engine's alert state.

Then publish `Alert{guardRef, level, tick}` at 10 Hz on change and write it on the peer
under LWW with a 300 ms quiet window. Acceptance is unchanged: both machines report the
same level for the same guard within 300 ms over a scripted 5-guard scenario.

**Unknown:** the route, and then whether writing `+0x146` is stable or recomputed by the
perception tick (E3.1). If it is recomputed, inject the *stimulus* instead (3B).

#### 3B — the puppet must be perceivable (high risk, the coin flip)

The old route (`FocusFlags` at `VisualTargetingComponent+0x660`) is disproved
**[D]** — the field is a pointer. So this is open.

Two candidate mechanisms, in order of preference:

1. **Register the puppet where the AI builds its candidate list.** The A3
   investigation established that `Universe+0x08` is a weak-observer list and
   `0x006608A0` ticks nothing **[D]**. The missing piece is the perception
   candidate list itself. Experiment E3.2: find the function that fills the
   guard's "things I can see" container by watching a guard acquire the *player*
   (hardware breakpoint on the player entity's reference count, or on the
   detection band `sub_13DD550`'s feeder). Cost: 2–4 sessions. Kill criterion:
   if no candidate list is findable in 4 sessions, go to (2).
2. **Stimulus injection (client-side illusion, labelled [H]).** Compute locally
   whether the peer avatar is inside a guard's cone and range; if so, inject the
   same transition the engine would (alert level / investigate point) and let the
   AI's own state machine proceed. This is consistent with the architecture:
   we replicate a *stimulus*, not a behaviour.

**Gate M3 (C3):**
On B, with the puppet placed in a guard's cone and no other target, the guard
acquires and attacks the puppet in ≥ 8 of 10 trials; A sees the same guard state
within 300 ms; 0 crashes over 15 minutes.

**Rollback / kill:** if both 3B routes fail, C3 is deferred and the product is
"C0–C2 + naval". Do not let C3 block C4/C5 (they do not depend on it).

---

### Phase 4 — C4: shared naval combat (3–6 weeks)

**Entry:** M2 green. C3 not required.

The ship is a *second avatar*, not a world object. That is the whole trick.

| # | task | detail |
|---|---|---|
| 4.1 | Ghost ship = render clone (`SetGlobalMatrixInternal` override) **[D]**; already proven, keep it render-only | never give the ghost a physics body |
| 4.2 | Ship pose stream: hull + mast skeleton at 10 Hz, smoothed | reuse `repl/` pose |
| 4.3 | **Hull HP must be found, not guessed.** The `[[ship+0x5C]+0x84]` value regenerates +6/tick and is not health **[D]**; `ShipHealthData` exists as a class **[D]**. Spike: enumerate services on a live ship, diff a damaged vs undamaged ship, find the setter. Kill criterion: 3 sessions, else ships are unkillable and C4 is boarding-only |
| 4.4 | Cannon events: `Shot{shooterShip, targetShip, damage, kind, tick}`; the target's owner applies to its own hull | one physics world per machine |
| 4.5 | Boarding handoff: freeze the boarded ship as a prop on the attacker's machine, transfer player control, release on completion | needs a state machine, tested by script |
| 4.6 | World travel safety: no ghost ship across a load; tombstone and respawn | the current build crashes on world travel **[D]** |

**Gate M4 (C4):**
Two ships exchange 10 cannon hits; each machine's hull value drops by the same
amount within 200 ms; a ship sinks on both; boarding completes and both players
see the same deck state; one world-travel transition with no crash.

---

### Phase 5 — C5: shared progression (3–8 weeks, research-heavy)

**Entry:** M2 green (C3/C4 optional). This is the phase that needs the most RE.

**What is known:** saves are per-account and encrypted **[D]**; mission logic is
Domino (condition/operator graph) **[D]**; nothing about the mission-state
container is known **[D]**.

| # | task | detail |
|---|---|---|
| 5.1 | **RE spike: mission state.** Find the object that answers "is objective X complete". Start from `DominoSpawningSpecification`/`Mission*` classes named in `CODE-MAP.md`, and from what changes in memory when an objective ticks. Kill criterion: 5 sessions without a storage location ⇒ C5 is deferred |
| 5.2 | Objective journal: grow-only set of `(missionId, objectiveId)`; each side publishes completions; union on receipt | idempotent by construction |
| 5.3 | Mission *events* that cannot be boolean (a scripted sequence, a cutscene, a checkpoint) are replicated as `Cue{missionId, cueId, tick}` with a "wait for peer" barrier | avoid double-triggering cutscenes |
| 5.4 | Save policy: local saves only; on join, refuse if mission progress diverges beyond the journal | never ship a save file |
| 5.5 | Anti-double-trigger: the objective set is checked before the local completion handler runs | one completion, two machines |

**Gate M5 (C5):**
Complete one full mission with ≥ 3 objectives on two machines; each objective
completes exactly once on each side; no cutscene plays twice; save/load on both
machines restores the same journal; a deliberate packet-loss run (20 %) does not
duplicate or lose an objective.

---

### Phase 6 — productisation (2–3 weeks, after C2)

Join UX, version handshake with loud failure, NAT/relay decision (a plain UDP
relay is the boring correct answer), telemetry that is actually a log you can
read, licence/permission for the upstream base, and a tester kit that cannot be
misconfigured.

---

## 5. Parallel tracks

| track | runs | owner |
|---|---|---|
| **IDB truth** — names + structs, kept current | all phases | BIN |
| **Oracle** — `audit/` grows one row per claim; runs in CI | all phases | QA |
| **Test harness** — scripted scenes (fight arena, 5 guards, ship duel) | from Phase 1 | QA |
| **Docs** — generated status, one source of truth | all phases | DOC |

---

## 6. The experimental backlog (do these before promising a phase)

| id | question | cost | kills |
|---|---|---|---|
| E2.1 | Is `sub_17106C0` the damage filter? | 1 session | stage 2A route |
| E2.2 | Does `sub_173E190`'s event carry a victim id? | 1 session | record/replay of that event |
| E3.1 | Is `ctx+0x146` writable and stable for 5 s? | 1 session | 3A write path |
| E3.2 | Where is the AI's perception candidate list? | 2–4 sessions | 3B route 1 |
| E4.1 | Where is ship hull HP? | 3 sessions | ships that can sink |
| E5.1 | What object holds objective completion? | 5 sessions | C5 |
| E1.1 | Does `m_ObjectID` survive respawn? | 1 session | identity model (blocker for C2+) |

E1.1 is the cheapest and the most load-bearing: if ids are not stable across
respawn/streaming, the whole event model needs an alias layer. **Run it first.**

**Static answer (2026-10-29, `E1-identity.md`):** the id is never rewritten for a
live object (`sub_A1F2F0` early-returns when already bound), the mint counter is
monotonic from `0xF0000000`, and the serialiser **zeroes minted ids** at save/load
(`0xA51CCC`) — so authored ids persist and minted ids are session-scoped. The wire
must therefore carry `epoch`, and a minted id must never be written to a save.
The live run is still required for the cross-machine case.

---

## 7. Organisation

Seats, ownership and the communication protocol are in `ROSTER.md` and
`PROTOCOL.md`. The short version:

* One writer per file. Branch per workstream. ORCH merges.
* Every claim in a report is either a ledger row (machine-checked) or tagged
  `[H]`. Reports without evidence are rejected, not debated.
* A seat may not edit `audit/`, `ops/STATE.json`, or another seat's branch.

---

## 8. Timeline (estimates, with the assumptions that make them true)

| phase | estimate | assumption |
|---|---|---|
| 0 | 3–5 days | IDA pass is mechanical |
| 1 | 1–2 weeks | a second machine is available **this week** |
| 2 | 3–5 weeks | E2.1 finds a real apply path |
| 3 | 4–8 weeks | E3.2 finds the candidate list (coin flip) |
| 4 | 3–6 weeks | E4.1 finds hull HP |
| 5 | 3–8 weeks | E5.1 finds objective state |
| 6 | 2–3 weeks | — |

**Total to C5: 4–7 months of focused work.** C0–C2 (the shippable mod) is
**5–7 weeks**. The plan is designed so that C2 ships even if C3, C4 and C5 all
fail.

If the second machine is not available weekly, Phase 1 cannot start and nothing
downstream is real. That is the schedule's only hard dependency.

---

## 9. Kill criteria (decide now, not in month four)

* **C3:** two failed perception experiments ⇒ stop, ship C0–C2.
* **C4:** no hull HP after 3 sessions ⇒ boarding-only, no sinking.
* **C5:** no objective state after 5 sessions ⇒ "co-op free roam + combat" is the
  product; document it, do not promise a campaign.
* **Any phase:** three crashes traced to a guard you added ⇒ remove the guard and
  fix the root cause. Guards are not a strategy.

---

## 10. What this plan deliberately does not do

* No shared physics, no second simulated world, no lockstep.
* No engine-native second player (the pad-merge route is closed).
* No ambient NPC sync (closed by decision, and the event model makes it
  unnecessary for the outcomes that matter).
* No save-file transfer, ever.
* No server infrastructure owned by Ubisoft.
* No feature work before the experiment that could kill it.
