# ops/ — running the co-op program

This folder is the programme, not the project. The code lives in `ref/AC4Coop/`,
the reverse-engineering record in `RE-AC4/` and `MODLOG.md`, the oracle in
`audit/`. This folder answers: *what are we doing this week, who is doing it, and
how do we know it worked.*

Read in this order:

1. `PROGRAM-PLAN.md` — architecture, phases, gates, experiments, kill criteria.
2. `GATES.md` — the machine-decided acceptance tests.
3. `PROTOCOL.md` — how seats talk, and the file bus.
4. `ROSTER.md` — the nine seats and their scopes.
5. `STATE.json` — the only mutable index (build, gates, experiments, next).
6. `DECISIONS.md` — accepted and open decisions.
7. `RISK-REGISTER.md` — what is most likely to waste the next month.

---

## The first ten actions, in order

This is the answer to "what do we do now". Each is small enough to finish.

| # | action | seat | evidence produced |
|---|---|---|---|
| 1 | Fix `0x681817` (patch the join, or null-safe `sub_733FB0`) | NET | 10-min run, no fault |
| 2 | Log the ASI hash at load | NET | `[Build] asi=…` on line ≤ 3 of every log |
| 3 | Run **E1.1** — is `m_ObjectID` stable across respawn? | BIN | id before/after table |
| 4 | Replace the six-site guard with one root-cause guard | NET | patch table shrinks, audit `hooks.all_aligned` still PASS |
| 5 | Commit `HANDOFF-STATE.md`, then delete the superseded handoffs | DOC | clean tree, 0 live contradictions |
| 6 | IDB pass: 50 names + 6 structs | BIN | `audit/ida/ida_findings.json` |
| 7 | Reconcile the kit ini (D-007) and regenerate `SHA256SUMS.txt` | QA | config diff explained |
| 8 | Generate `STATUS.md` from `audit.json` | DOC | no hand-written status |
| 9 | **Book the second machine** and run scene 1.1 | QA + human | two logs with a real peer IP |
| 10 | Gate M0 review | QA | `STATE.json` M0 green |

Actions 1, 2 and 4 are on the critical path; 3 is the cheapest load-bearing
experiment in the whole plan; 9 is the one nobody can do for you.

---

## Working with the existing sessions

There is already an agent session doing day-to-day work on `ref/AC4Coop`. Two
writers on one tree is how work gets lost, so:

* **NET is the only seat allowed to write `ref/AC4Coop/src/`.**
* The existing session is treated as an extra pair of hands, not a seat: it gets
  `TASK` messages on the bus, works on a branch, and its output is reviewed by QA
  like anyone else's.
* Until it is told otherwise, assume it may be mid-edit. `git status` before any
  merge; never `git checkout --` someone else's work.

The bus message to send it first:

```
TASK  re: OP-001
Read ops/PROGRAM-PLAN.md §2 and §4-Phase-0.
Do not start new probe builds. The next three changes are 0.1 (fix 0x681817),
0.2 (log the ASI hash) and 0.3 (one root-cause guard instead of six sites).
Reply with the branch name you will use and STOP until ORCH confirms.
```

---

## The bus, in one screen

```
ops/bus/inbox/<seat>/<UTC>_<from>_<n>.json     write-then-rename
ops/bus/outbox/<seat>/                          consumed
ops/bus/archive/<date>/                         cold storage
ops/bus/STATE.json                              message index
```

Types: `TASK EVIDENCE BLOCK HANDOFF DECIDE REVIEW PING`. The schema is in
`PROTOCOL.md` §2. If the `opencode-tell-sessions` plugin is loaded, the same
message can be DM'd with `session_send`; the bus file is still written, because
DMs are not auditable and the bus is.

---

## Keeping the orchestrator awake

The orchestrator cannot subscribe to other sessions, so a watcher polls the seat
sessions and injects a short prompt into the orchestrator's own session when a seat
replies **and has been quiet for 45 s** (a seat is finished only when it stops
talking; per-tool-step `idle` markers are not turn boundaries).

```powershell
# start (detached; survives this turn) — do NOT re-seed on restart
Start-Process -FilePath "$env:LOCALAPPDATA\Python\bin\python.exe" `
  -ArgumentList 'ops\tools\watch_seats.py' `
  -WorkingDirectory '<bf-coop>' -WindowStyle Hidden

# one-time only, on the very first run: everything that exists is old news
py ops\tools\watch_seats.py --seed

# health check
py ops\tools\watch_seats.py --once --dry-run
Stop-Process -Id (Get-Content ops\bus\watch.pid)
```

**The reseed trap:** running `--seed` on every restart sets the watermark to "now"
and silently swallows any reply that arrived while the watcher was down. That
happened once and cost a 40-minute delay. Seed only at the very beginning.

## Synthetic peer (E0-B)

```powershell
py ops\tools\peer9.py --selftest                     # asserts sizeof(PlayerState)==560
py ops\tools\pick_target.py                          # candidate objIds from the game log
py ops\tools\e0b_run.py --timeout 3600 --duration 40 # arms and waits for the next launch
```

`e0b_run.py` learns the player position from the game's own packets, picks the
nearest guard, then runs the peer with `--echo-pos --kill`. It wakes the
orchestrator with a summary when it finishes.

## Definition of "working"

A capability is working when **its gate is green in `STATE.json`**, the evidence
folder exists, and the ASI hash in the evidence matches the deployed build. No
other sentence, in any document, may use the word.

---

## Housekeeping

* `ops/evidence/<gate>-<date>/` is committed. Logs are evidence; do not `.gitignore`
  them.
* `ops/tools/` holds `gate.py` (gate runner) and `scene_*.py` (scripted scenes).
  They are written as each gate is approached, not up front.
* `ops/STATE.json` is written by `gate.py` and by ORCH only.
