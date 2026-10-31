# Inter-session protocol

Two transports, one message format. The file bus is canonical; session DMs are for
latency and for nudging a human-run session.

## 1. Transports

### 1a. File bus (always available, auditable)

```
ops/bus/
  inbox/<seat>/            # messages addressed to a seat
  outbox/<seat>/           # messages a seat has finished with
  archive/                 # consumed messages, by date
  STATE.json               # the only mutable index
```

A message is one JSON file: `ops/bus/inbox/<seat>/<UTCstamp>_<from>_<n>.json`.
Write-then-rename (`.tmp` → `.json`) so a reader never sees a half file.
Messages are immutable once written. Consuming a message means moving it to
`outbox/<seat>/` and recording it in `STATE.json`.

### 1b. Session DM (`ops/tools/dm.py`, server API)

```powershell
py ops\tools\dm.py list                     # id + title of every live session
py ops\tools\dm.py send <id|title-substr> "message"
py ops\tools\dm.py read <id|title-substr> [n]
```

It talks to the local OpenCode server (`http://127.0.0.1:<port>/api`, Basic auth
with `opencode:<password from ~/.config/opencode/service.json>`), which is the same
surface the UI uses. `POST /api/session/<id>/prompt` with `{"text": ...}`; a 200
means the message became a real user turn in that session.

Why not a plugin: `opencode-tell-sessions` fails to load on this host (its
`@opencode-ai/plugin` peer has no `./v1` export), and `agent-intercom-opencode`
installs but the loader rejects it ("Plugin must export a default definition" — the
package exports a V1 factory, this host wants a V2 object and resolves a `/server`
entrypoint first). `dm.py` needs no restart and no plugin. If a future host loads
intercom natively, use `intercom_send`; the bus file is still written either way.

Rules for DMs:

* **Keep a DM under ~1800 characters.** The prompt path truncates longer bodies
  mid-sentence (observed twice on 2026-10-29: "…65 host" and "…session Full"). If it
  needs to be longer, write a bus file and DM only its path plus a 3-line summary.
* Prefix every DM with the message `id`, so the reply can be matched.
* One question per DM. If it needs three answers, write three messages or one
  bus file.
* **Never** use DMs to change a decision. Decisions go in `ops/DECISIONS.md` and
  are announced, not negotiated in chat.

## 2. Message format

```json
{
  "id": "MSG-20261029T1402Z-orch-001",
  "from": "orch",
  "to": "bin",
  "type": "TASK | EVIDENCE | BLOCK | HANDOFF | DECIDE | REVIEW | PING",
  "re": "E2.1" ,
  "created": "2026-10-29T14:02:11Z",
  "deadline": "2026-10-30T12:00:00Z",
  "body": "…",
  "evidence": {
    "artifacts": ["ops/evidence/E2.1/damage-filter-17106c0.log"],
    "claims": ["B-31"],
    "verdict": "PASS | FAIL | INCONCLUSIVE"
  },
  "reply_to": null
}
```

Type meanings:

| type | meaning | required fields |
|---|---|---|
| `TASK` | do this, by then | `re`, `deadline`, `body` |
| `EVIDENCE` | here is a result | `evidence.artifacts`, `evidence.verdict` |
| `BLOCK` | I cannot proceed because X | `body` names the exact blocker and who can unblock |
| `HANDOFF` | my seat is free; here is the state | `body` lists open tasks + file paths |
| `DECIDE` | a choice is needed (ORCH or human) | `body` has options and a recommendation |
| `REVIEW` | check my artifact | `evidence.artifacts` |

## 3. Rules

1. **One writer per file.** If two seats need to change one file, the second opens
   a `BLOCK`. Cross-cutting files (this one, `PROGRAM-PLAN.md`, `STATE.json`,
   `audit/`) are ORCH-only.
2. **Evidence or `[H]`.** Every `EVIDENCE` message carries either artifact paths
   or the literal tag `[H]` and the experiment that would settle it. A message
   with neither is returned unread.
3. **Answer the question asked. Do not restate earlier findings.** A seat that
   re-summarises work the orchestrator already has costs a full turn per reply.
   New question → new facts only; reference the earlier answer by its commit or
   section instead of repeating it.
4. **No silent scope.** A seat that starts work outside its brief writes a
   `DECIDE` first.
5. **No DM decisions.** See above.
6. **Fresh context reads this file first**, then `PROGRAM-PLAN.md`, then
   `ops/STATE.json`, then its seat brief.
7. **Timebox.** Every `TASK` has a deadline. Missing it is not failure; going
   quiet is.

## 4. `STATE.json` — the only mutable index

```json
{
  "updated": "2026-10-29T14:02:11Z",
  "build": {"asi": "f474422f…", "exe": "bc11ad79…", "commit": "958b435"},
  "gates": {"M0": "unknown|green|red", "M1": "…", "M2": "…", "M3": "…", "M4": "…", "M5": "…"},
  "seats": {"orch": {"state": "active", "since": "..."}, "bin": {"state": "blocked", "blocker": "IDA busy"}},
  "experiments": {"E1.1": "todo|running|done", "E2.1": "…"},
  "decisions_open": ["D-004"],
  "next": ["0.1", "0.2", "E1.1"]
}
```

A gate is `unknown` whenever `build.asi` changes. This is deliberate: a green gate
that was earned by a different binary is a lie.

## 5. Daily rhythm

| when | what | who |
|---|---|---|
| start of session | read `STATE.json`, claim one experiment | every seat |
| every 2 h | `PING` with a one-line progress + the artifact path | working seats |
| end of session | `HANDOFF` with open tasks and exact file paths | every seat |
| gate reached | `REVIEW` to QA with the evidence folder | the seat that ran the scene |
| whenever a decision is made | append to `DECISIONS.md` | ORCH |

## 6. What the orchestrator does that seats may not

* flip a gate,
* change the plan,
* merge a branch,
* accept a `DECIDE`,
* delete or supersede a contradiction,
* declare a capability "working".

Everything else is delegated.
