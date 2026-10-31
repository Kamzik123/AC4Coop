# Roster

Nine seats. A seat is a session, not a model. Seats are created at the phase
boundary where they are needed — an idle agent with fresh context costs more than
it saves.

Every seat brief is: **scope · owns · forbidden · deliverable · report format**.
The report format is mandatory, because the orchestrator reads nine of them a day.

---

## ORCH — orchestrator (this session)

* **scope:** the plan, the gates, the merge, the arbitration, the decisions.
* **owns:** `ops/`, `audit/`, `CODE-MAP.md`, `COOP-MASTER-PLAN.md` (as history).
* **forbidden:** writing mod source (that is NET/COMBAT/etc.).
* **deliverable:** a green or red gate with an evidence folder, and a decision log
  that a newcomer can follow.
* **report:** `STATE.json` diff + the message that explains it.

## BIN — binary truth

* **scope:** the IDB. Names, structs, xrefs, and the answers to "what does this
  function actually do". Owns `audit/ida/ida_findings.json`.
* **owns:** `audit/ida/`, `RE-AC4/`, and the naming inside the `.i64`.
* **forbidden:** touching plugin source, netcode, or any `.md` that describes
  behaviour ("verified" is a claim, and claims are QA's row to write).
* **deliverable:** for each question, a decompiled structure (conditions, offsets,
  constants, caller count) written as flat facts — never prose alone.
* **report:** `EVIDENCE` with `evidence.claims` listing new ledger row ids.

## NET — transport, hooks, crash-freedom

* **scope:** `net.cpp`, `netstate.h`, `deref_guard.*`, `action_guard.*`,
  `build_gate.*`, `dllmain.cpp`, and the new `repl/` module.
* **owns:** `ref/AC4Coop/src/` except the combat/stealth/naval files listed below.
* **forbidden:** changing a capture hook's semantics without a `DECIDE`; adding a
  patch site without a byte pattern and an alignment check.
* **deliverable:** a build that passes M0 and M1, with the patch table regenerated.
* **report:** `EVIDENCE` with the diff, the new patch sites, and the log.

## COMBAT — damage seam and combat events

* **scope:** stage 2A and 2B. `combat/*.cpp`, the damage/health hooks.
* **owns:** `ref/AC4Coop/src/combat*`, `ops/evidence/E2.*`.
* **forbidden:** writing `Life`/flags as the *primary* mechanism (that route is
  disproven); shipping without an apply path or an explicit fallback decision.
* **deliverable:** `Damage{...}` records on the wire and an apply path proven by
  20/20 matched kills.
* **report:** the kill table, not a narrative.

## AI — perception and stealth

* **scope:** C3. Alert-state read/write (already verified: `entity+0x8E0` →
  `[+0x1C]` → `+0x40` bitset / `+0x146` u16), and the perception candidate list.
* **owns:** `ref/AC4Coop/src/stealth*`, `ops/evidence/E3.*`.
* **forbidden:** re-opening `FocusFlags` (disproved), and faking guard behaviour
  without labelling it a stimulus injection.
* **deliverable:** M3 numbers, or a written kill of 3B with the experiments that
  killed it.
* **report:** per-trial table (guard, trial, acquired y/n, alert ms).

## NAVAL — ship ghost and naval combat

* **scope:** C4. Ghost ship (render-only), hull HP discovery, cannon events,
  boarding handoff.
* **owns:** `ref/AC4Coop/src/ship*`, `ops/evidence/E4.*`.
* **forbidden:** giving the ghost a physics body; writing the disproven
  `[[ship+0x5C]+0x84]`.
* **deliverable:** M4 numbers, or "ships cannot sink, boarding only" with evidence.
* **report:** hull-delta table + crash log.

## MISSION — progression

* **scope:** C5. Objective-state RE, the grow-only journal, cue barriers.
* **owns:** `ref/AC4Coop/src/mission*`, `ops/evidence/E5.*`.
* **forbidden:** touching save files; guessing an objective flag.
* **deliverable:** M5 numbers or a written kill after 5 sessions.
* **report:** objective table + divergence test result.

## QA — oracle, scenes, gates

* **scope:** the audit rig, the scene scripts, the evidence folders, the gate
  runner. QA is the only seat that can say "this gate is green".
* **owns:** `audit/`, `ops/tools/`, `ops/evidence/`, `ops/STATE.json` gates.
* **forbidden:** editing the thing under test. QA may not fix a bug to make a gate
  pass; it files it.
* **deliverable:** a green/red gate with the exact command that produced it.
* **report:** gate verdict + the numbers + artifact paths.

## DOC — truth in prose

* **scope:** generated `STATUS.md`, the contradiction ledger, the handoffs, release
  notes. Keeps the repo readable by a fresh agent.
* **owns:** `STATUS.md`, `ops/DECISIONS.md`, `README-*`, `HANDOFF*`.
* **forbidden:** any sentence with *verified/proven/working* that is not backed by
  a ledger row id.
* **deliverable:** zero live contradictions, one generated status file.
* **report:** the contradiction count before/after.

---

## Activation schedule

| seat | created at | why then |
|---|---|---|
| QA | now | the oracle must exist before the first gate |
| BIN | now | the IDB debt blocks every other seat |
| NET | now | Phase 0 fixes are on the critical path |
| DOC | now | contradictions are actively misleading agents |
| COMBAT | end of Phase 1 | needs a working two-machine session to test against |
| AI | after E2.1 | needs the event model to publish alerts into |
| NAVAL | after M2 | needs a working damage event to reuse |
| MISSION | after M2 | RE is long; start when the plan is stable |

## Session registry

Filled by ORCH when a seat is created. `sessionID` is the OpenCode session id;
`bus` is the inbox path. A seat with no session id is a brief, not an agent.

| seat | sessionID | bus inbox | created |
|---|---|---|---|
| ORCH | `ses_e78f161e7ffd99U6I3sT7cXOz7` | `ops/bus/inbox/orch/` | 2026-10-29 |
| NET | `ses_e988712e4ffdYTcGC8j4bxedc3` | `ops/bus/inbox/net/` | 2026-10-29 |
| BIN | `ses_e78cdbfecffe5E4DsuPWOlgLbP` | `ops/bus/inbox/bin/` | 2026-10-29 |
| COMBAT | `ses_e78cdbfe5ffexSCiRR4lW5Xkkn` | `ops/bus/inbox/combat/` | 2026-10-29 |
| QA | `ses_e78cdbfeaffeL3EV6VlJHlz0Tg` | `ops/bus/inbox/qa/` | 2026-10-29 |
| DOC | `ses_e78d2b1b3ffewc85tbpGONwFAl` | `ops/bus/inbox/doc/` | 2026-10-29 |
| AI | _pending_ | `ops/bus/inbox/ai/` | — |
| NAVAL | _pending_ | `ops/bus/inbox/naval/` | — |
| MISSION | _pending_ | `ops/bus/inbox/mission/` | — |

Messaging: `py ops\tools\dm.py send <seat> "…"` (see `PROTOCOL.md` §1b). A seat is
resumed by DM or by passing its `sessionID` to `subagent`. The plugin route was
abandoned — it does not load on this host; `dm.py` talks to the server API directly.
