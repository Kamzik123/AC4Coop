# Risk register

Scored as **L** (likelihood) × **I** (impact), 1–5. Reviewed every gate.
The mitigation column names a *test*, not an intention.

| id | risk | L | I | score | mitigation / early warning |
|---|---|---|---|---|---|
| R1 | No second machine available weekly | 4 | 5 | **20** | **E0-A first**: two instances from one install (`experiments/E0-A-two-instance.md`). The code already supports it (`net.cpp:335`), the mutex guard is a single `CreateMutexW` in `_WinMain@16`, and the plugin hooks it. If E0-A passes, M1–M5 functional gates run on one PC. If it fails on `SO_REUSEADDR`, that is a 20-line fix, not a purchase. |
| R2 | `m_ObjectID` is not stable across respawn/streaming | 3 | 5 | **15** | E1.1, one session, before any event code. If unstable, add an alias layer (`(epoch, spawnIndex, classTag)`) — this changes the wire format, so learn it now. |
| R3 | No callable damage apply path | 3 | 4 | **12** | E2.1/E2.2. Fallback D-009 (health diff). Decide at end of stage 2A, not in month three. |
| R4 | Puppet is not perceivable by the AI | 4 | 4 | **16** | E3.2, 4-session budget, then stimulus injection, then D-008 kill switch. |
| R5 | Ship hull HP not found | 3 | 3 | **9** | E4.1, 3 sessions, else boarding-only (D-004). |
| R6 | Mission objective state not findable | 3 | 4 | **12** | E5.1, 5 sessions, else the product is free-roam + combat (documented, not promised). |
| R7 | Guard-by-enumeration pattern returns | 3 | 4 | **12** | D-005. Any new patch site needs bytes + alignment + a `DECIDE`. The audit rig fails a misaligned steal. |
| R8 | Documentation drifts again | 4 | 3 | **12** | `STATUS.md` generated; contradictions ledger re-verified every run; DOC seat's job is this and only this. |
| R9 | A green gate earned on an old binary | 3 | 5 | **15** | `gate.py` resets gates to `unknown` when the ASI hash changes. No hand-edits to `STATE.json`. |
| R10 | Two seats write one file and lose work | 3 | 3 | **9** | One-writer rule + branch per seat; ORCH merges. Bus logs who touched what. |
| R11 | Agent hallucinates an offset into the IDB | 3 | 4 | **12** | BIN seat writes flat facts only; claims become ledger rows; QA refuses prose. |
| R12 | Upstream licence blocks release | 4 | 2 | **8** | Ask before Phase 6. "No licence" = all rights reserved. |
| R13 | Interpolation latency feels bad on a real VPN | 2 | 3 | **6** | Measure p95 one-way per session; derive `InterpDelayMs` from data (task 1.7). |
| R14 | Scope creep into "shared world" again | 3 | 5 | **15** | D-001/D-002 are accepted decisions. A `DECIDE` is required to revisit; the plan's §10 lists what is out. |
| R15 | The user's existing dev session and the program collide on `ref/AC4Coop` | 3 | 3 | **9** | NET is the only writer of `src/`; the external session must be told to hand off or work on a branch. See `ops/README.md` §"Existing sessions". |

## Early-warning signals

* Two consecutive sessions with no new `EVIDENCE` artifact → stop and re-plan.
* A `BLOCK` older than 48 h that ORCH has not answered → escalate to the human.
* The audit score drops below the previous run → the build regressed; do not
  proceed to a gate.
* A seat asks for an exception to D-001/D-002 → that is scope creep, not a bug.
