# HIVE — operating model

Five persistent seats. The orchestrator (this session) is the only one that decides,
deploys and speaks to the user. Seats do work and report.

## Seats

| seat | owns | may write | may not |
|---|---|---|---|
| **NET** | `src/net.cpp`, `src/net.h`, `ops/tools/peer9.py` | those files | `phase1.cpp`, offsets |
| **RE** | the binary (IDA), `ops/RE-*.md` | its own notes | any `src/` file |
| **IMPL** | `src/phase1.cpp` — **the only writer** | `phase1.cpp` | `net.cpp`, ships/death lanes outside its task |
| **QA** | verification, oracles, `ops/QA-*.md`, `ops/tools/coop_report.py` | its own notes + tools | any `src/` file |
| **AI** | stealth/combat parity experiments, `ops/AI-*.md` | its own notes | any `src/` file |

Two seats never write the same file. That rule exists because two agents editing
`phase1.cpp` at once has already cost a build in this project.

## The rules

1. **Read `ops/TASKBOARD.md` before starting.** Your task is the row with your seat name.
2. **Evidence or it didn't happen.** Every claim carries `file:line` or a binary address.
   Label anything you did not prove: *unproven*.
3. **`ops/CLAIMS.md` is the ledger.** If your work verifies or disproves a claim, say its number.
4. **Never rebuild over another seat's build.** If you must build, say the hash you produced.
5. **Report format** (this exact shape, nothing else):
   ```
   SEAT <name> | status: DONE|BLOCKED|NEED-MORE
   did: <1-3 lines>
   evidence: <file:line | address | log line>
   build: <hash + bytes, or "none">
   next: <what you would do next, one line>
   blocked-by: <seat name or "nothing">
   ```
6. **Do not ask the orchestrator questions you can answer yourself.** If genuinely blocked,
   report `BLOCKED` with the reason.
7. **No prose essays.** The report format exists so five seats can be read in one screen.

## Blackboard

- `ops/PLAN-2026-10-30-enemy-sync.md` — the plan and gates
- `ops/CLAIMS.md` — what is proven, by whom, and how
- `ops/TASKBOARD.md` — who is doing what, live
- `ops/AGENT-LOG.md` — session ids
- `ops/evidence/` — logs and captures

## Escalation

`IMPL` and `NET` report build hashes to the orchestrator; the orchestrator deploys and
tells the user. No seat talks to the user and no seat touches the game folder.

## Pitfall: creating seats through the API

A bare `POST /api/session` with only a title inherits the **default model**, which on this
machine resolves to a free provider model that times out. The create payload also rejects
`{"model": "provider/id"}` and `{"model": {"providerID": ..., "modelID": ...}}`.

The working shape is:

```json
{"title": "NET: ...", "model": {"id": "deepseek-flash", "providerID": "deepseek"}}
```

`directory` is ignored by the endpoint — seats are created with `C:\Users\Administrator` as
their working directory, so every seat instruction must begin with
`cd "<repo>"` (see the header sent to each seat).

`ops/tools/mkseat.py` now sends the correct payload.

## Rule 8 — every seat owns its artefact path

`ref/AC4Coop/bin/AC4Coop.asi` is a **shared** output. Two seats building at the same time
overwrite each other: on 2026-10-30 NET reported `2D8E2E4DC499` (301,568 B) and eleven minutes
later the file on disk was `0EF897576388` (302,080 B) from IMPL, i.e. NET's tested binary was
destroyed before it could be deployed or verified.

So, when you build:

```powershell
Copy-Item bin\AC4Coop.asi "dist\builds\<SEAT>-<first 12 of sha256>.asi"
```

and report **that path plus the hash**. The orchestrator only ever deploys from `dist\builds\`.

## Rule 9 — one build at a time

Rule 8 gives every seat its own artefact, but a shared **object tree** still races: NET's rebuild
linked IMPL's in-progress `phase1.obj`, so the artefact was not "NET's change only", and it
overwrote `bin\AC4Coop.asi` while IMPL held it.

Before building, take the lock:

```powershell
$lock = "ops\bus\build.lock"
if (Test-Path $lock) { "BUSY: " + (Get-Content $lock); exit 1 }
"$env:SEAT $(Get-Date -Format o)" | Set-Content $lock
# ... msbuild ...
Copy-Item bin\AC4Coop.asi "dist\builds\<SEAT>-<hash12>.asi"
Remove-Item $lock
```

If the lock is older than 20 minutes it is stale — delete it and proceed, and say so in the report.

## Rule 10 — commit, then the orchestrator builds once

Rules 8 and 9 stop two seats *overwriting* each other, but neither stops a seat building on top of
another seat's **half-finished source**. That is what happened at 11:28: NET's artefact contained
IMPL's in-progress `phase1.cpp`, and IMPL's artifact disappeared in the same motion.

So the build protocol changes:

1. A seat finishes its own file and **commits only that file**:
   ```powershell
   cd "<repo>\ref\AC4Coop"
   git add src/phase1.cpp            # ONLY your own file(s)
   git -c user.name="<SEAT>" -c user.email="seat@local" commit -q -m "<SEAT>: <what changed>"
   git log --oneline -1
   ```
2. The seat reports `SEAT <NAME> | status: DONE` with the commit hash and **no build**.
3. The orchestrator builds **once** from a clean tree, copies to `dist\builds\REL-<hash12>.asi`,
   verifies the hash, and deploys.

No seat builds a release artefact any more. Seat-local builds are allowed only to check that your
own file compiles, and must use the Rule 9 lock and be thrown away.

## The report channel — `ops/tools/bus.py` (replaces the watcher)

The polling watcher is retired. It required a seat to go *quiet* before firing, seats stay
`busy` after their last message, so finished reports sat unseen - NET's T2, IMPL's two reports and
RE's +0x660 answer were all lost that way.

Reports are now **files**, written by the seat, read by the orchestrator:

```powershell
py ops\tools\bus.py report <SEAT> <DONE|BLOCKED|NEED-MORE> "<your report text>"
```

That is the contract. A report that is not written to the bus does not exist. Keep it under ~1500
characters; the orchestrator can read your session for detail.

Orchestrator side:

```powershell
py ops\tools\bus.py inbox          # unacked reports, newest last
py ops\tools\bus.py inbox --ack    # show and mark read
py ops\tools\bus.py status         # per-seat last report + session busy age
py ops\tools\bus.py send <SEAT> "<text>"
```

`status` is also the freeze detector: a seat busy for >10 minutes with no new report is hung and
gets interrupted and re-chartered.

## Rule 11 - never call the question tool in a seat

A seat has no human attached. The `question` tool blocks the turn forever: the session stays
`busy` with no progress (GFX froze 38 minutes this way, and it may explain earlier seat freezes).
Read what you need from the machine, decide, and record the uncertainty in the report as *unproven*.
