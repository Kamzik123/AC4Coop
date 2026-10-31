# DOC seat — contradiction resolution + generated STATUS (2026-10-29)

Seat: DOC. Task: `ops/bus/inbox/doc/20261029T1500Z_orch_002.json` (Phase-0 / 0.5–0.7).
Scope: documentation only. No source file, no `ref/AC4Coop/`, no `ops/STATE.json`, no
`audit/tools/` was modified.

Commands run (from `bf-coop/`):

```
C:\Users\Administrator\AppData\Local\Python\bin\python.exe audit\tools\run_audit.py
C:\Users\Administrator\AppData\Local\Python\bin\python.exe ops\tools\gen_status.py
C:\Users\Administrator\AppData\Local\Python\bin\python.exe ops\tools\gen_status.py --check
```

## 1. Contradiction actions

| id | action taken | file:line |
|---|---|---|
| X-01 | Roadmap item 8 relabelled **per-system authority**; the old host-authoritative label is marked SUPERSEDED 2026-10-29 with a pointer to `ops/DECISIONS.md` D-002 and the master plan. | `COOP-ROADMAP.md:62` |
| X-02 | The "type + position is reliable" sentence is replaced by a SUPERSEDED 2026-10-29 note pointing to D-002 and `COOP-MASTER-PLAN.md:197`; the link-matching step now says "through the plan's identity layer, not by nearest position". | `COOP-ROADMAP.md:86`, `COOP-ROADMAP.md:93` |
| X-03 | The wrong hull-HP expression is deleted; one line now says the field is not hull HP, regenerates +6/tick, and points to the disproof (`MODLOG.md:2702`, this report §8/X-03). | `SHIP-SYNC-HANDOFF.md:19` |
| X-04 | The 2026-10-08 kill-recipe line is amended in place (history kept) with SUPERSEDED 2026-10-29 and a pointer to the controlled negative test at `MODLOG.md:2845`. | `MODLOG.md:1118` |
| X-05 | "loop closed" is replaced: host side proven solo 2026-10-08 (`MODLOG.md:1118`); the two-machine datagram was unproven until G2 (`COOP-PLAN-REVIEW-2026-10-17.md`). | `COOP-REVIEW.md:33` |
| X-06 | Corrected the wrong document: shipped code calls `SO_REUSEADDR` (`ref/AC4Coop/src/net.cpp:333`); `COOP-REVIEW.md` now says `SO_REUSEADDR`. `CODE-REVIEW.md` was already correct and is unchanged. | `COOP-REVIEW.md:31` |
| X-07 | `STATUS.md` was regenerated and no longer contains the old "Runtime-verified: NO" sentence. The deployment fact now names the deployed ASI `f474422f` (263,680 B, mtime 2026-10-28) and records that no two-machine run exists (`two-machine logs found = 0`). Side A (`ref/AC4Coop/docs/HANDOFF-STATE.md:9`) is another seat's file — see §4. | `STATUS.md:47`, `STATUS.md:70` |
| X-08 | Both sentences now name the binary: prose names `f474422f` vs `64aa4256`; `REVIEW-PACKET` names `f474422f` and the 2026-10-18 `64aa4256` build. | `STATUS.md:6`, `REVIEW-PACKET.md:55` |
| X-09 | Counts split by metric: 135,109 **function starts** (linear sweep), 134,020 **decompiled functions** (corpus), 128,612 in the IDB function table (audit claim C-01). | `CODE-MAP.md:45`, `CODE-MAP.md:63`, `CODE-MAP.md:72` |
| X-10 | Roadmap item 9 is marked SUPERSEDED 2026-10-18 (route dropped, `COOP-MASTER-PLAN.md:123`); combat step 5 and the recommended-order entry no longer schedule it. | `COOP-ROADMAP.md:65`, `COOP-ROADMAP.md:97`, `COOP-ROADMAP.md:128` |

Two additional MODLOG headlines named in the task, amended in place (no history deleted):

| headline | action | file:line |
|---|---|---|
| 2026-10-09 night-#5 "ghost runs/jumps/climbs WITH animation" | Amended with SUPERSEDED 2026-10-09 and a pointer to the night-#5b correction; the original sentence is preserved. | `MODLOG.md:2020` (correction at `MODLOG.md:2029`) |
| "CLOSED (with certainty) — Runtime entity creation" | Narrowed to keyed creation from an NPC key; notes that a second *Edward* body is possible through the cached `EntityBuilder`; keeps "do not retry the keyed path". | `MODLOG.md:689` (evidence at `MODLOG.md:1722`) |

## 2. Audit gate line, before and after

Before (audit.json generated `2026-10-29T13:20:41`, read at the start of this task; the file has
since been overwritten by the required re-run):

```
G0=PASS, G1=FAIL, G2=FAIL, G3=FAIL, G4=FAIL, G5=FAIL
claims 16/32 (50.0%) — 4 critical, 7 high, 5 medium failures
live contradictions: 10
```

After (fresh `run_audit.py`, audit.json generated `2026-10-29T13:24:43`):

```
G0=FAIL, G1=FAIL, G2=FAIL, G3=FAIL, G4=FAIL, G5=FAIL
claims 17/32 (53.1%) — 3 critical, 8 high, 4 medium failures
live contradictions: 0 of 10
```

The document gate moved inside the red gate:

| gate | before | after | reason |
|---|---|---|---|
| G4 | 0/3 PASS (0.0%) | 2/3 PASS (66.7%), still FAIL | E-02 and E-03 now pass. E-01 still fails: `COOP-MASTER-PLAN.md` header date is 2026-10-25 while the body contains 2026-10-27. That file is ORCH-owned and was not edited. |

Three other claim movements are **not** documentation effects, and are recorded so the gate line
is not misread:

- `B-04` (critical) FAIL → PASS: the working tree shows `M src/deref_guard.cpp` (see A-05 detail),
  which is a code change in the tree during this window. I did not touch `src/`; the doc edits
  cannot move a binary check.
- `A-05` (high) PASS → FAIL: same dirty tree (`M AC4Coop.ini`, `M src/deref_guard.cpp`).
- `A-07` (high) PASS → FAIL: a plain `run_audit.py` does not rebuild, so `rebuild.text_diff_bytes`
  is not captured. The previous PASS came from an audit.json produced with `--rebuild`.

G0's flip is therefore a run-mode + working-tree artifact, not a doc change.

## 3. Entries I do not consider factual contradictions

- **X-05 — scope, not fact.** `MODLOG.md:1118` records a one-machine apply test on 2026-10-08;
  `COOP-PLAN-REVIEW-2026-10-17.md:146` says the two-machine relay was not end-to-end. Both
  statements can be true simultaneously. The defect was that neither sentence named its scope, so
  the fix names the scope instead of deleting either side.
- **X-08 — two binaries, not two facts about one binary.** The deployment artifacts in
  `audit/report/audit.json` show the deployed ASI `f474422f` (mtime 2026-10-28) and the 2026-10-18
  kit `64aa4256`. The old STATUS sentence described the 2026-10-18 binary; the REVIEW-PACKET
  sentence was about the packet's own build. The fix names the binary in both places.
- **X-04 — an over-generalised observation, not a false observation.** The 2026-10-08 guard died
  after a real damage event had already run (`MODLOG.md:1118`); the controlled write with no
  preceding engine death produced no visible change (`MODLOG.md:2845`). The contradiction is the
  generalisation "no engine function needed", which is now superseded in place.

## 4. Left for the file's owner

- **X-07 side A.** `ref/AC4Coop/docs/HANDOFF-STATE.md:9` still says "Deployed 2026-10-18". The
  deployed ASI is `f474422f`, mtime 2026-10-28 (audit artifacts), so that line is stale. The file
  was committed by the other agent during this task and is outside DOC's ownership; it was not
  touched. Recommended one-line fix: name `f474422f` and 2026-10-28.
- **E-01.** `COOP-MASTER-PLAN.md` needs its header date bumped (2026-10-25 → at least 2026-10-27)
  before G4 can be green. ORCH owns that file; I did not edit it.

## 5. Verification

- `run_audit.py` recomputed the ledger: `live 0 of 10`. The report's own line reads
  `**Live contradictions: 0** (both sides of the quote still present).`
  (`audit/report/AUDIT-REPORT-2026-10-29.md:172`).
- `gen_status.py --check` → `STATUS.md is current`.
- A literal-quote scan confirms both sides are no longer simultaneously present: X-01…X-06,
  X-08…X-10 have one side `MISSING` (or both for X-08/X-09); X-07 has `a_state=present`,
  `b_state=MISSING` and is recorded in §4.
- Generated-half scan of `STATUS.md`: no `verified`, `proven`, or `working` words (the only match
  is the literal archive directory name `AC4Coop-kamzik-WORKING-FDF433D3` in an artifact path).
- The pre-regeneration STATUS is preserved at
  `ops/evidence/DOC-2026-10-29/STATUS.pre-regeneration.md`; nothing was deleted from `MODLOG.md`.
