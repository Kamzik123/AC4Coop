# Evidence folders — what a gate needs on disk

`gate.py` will not turn a gate green unless its evidence folder exists, contains
the required files (globs + minimum counts), and **at least one file names the
build under test** (the first 12 characters of the ASI sha256). That last rule is
what stops a green gate from surviving a rebuild.

Naming: `ops/evidence/<gate>-<YYYY-MM-DD>/`, e.g. `ops/evidence/M1-2026-11-03`.
One folder per gate per run. Never reuse a folder after the build changes.

---

## M0 — Truth

Required: `AUDIT-REPORT-*.md` (a copy of the audit report).

`gate.py check M0` additionally reads `audit/report/audit.json` and requires the
audit rig's own gates **AG0, AG2, AG4, AG5** to pass. Those are `AG`, not `M` —
do not confuse them: `AG` is the audit rig, `M` is the programme milestone.

If you are asked why a specific AG gate is red, read §2 of the audit report; it
names the claim id and the measured value.

## M1 — shared presence / shared movement

Required:

* `phase1_log_*.txt` × 2 (one per machine, or one per process in a two-process run)
* `AC4Coop_build_*.log` × 2 (the build sidecars — they carry the exe hash)
* `SIGNOFF.md` — must contain `asi=<hash>` and `tree=clean|dirty`

`SIGNOFF.md` is one file with one section per player:

```markdown
# M1 sign-off — 2026-11-03
asi=f474422f88c6  tree=dirty  commit=958b435
host  (player: ____): gait matches [ ]  facing correct [ ]  no visible pop [ ]
guest (player: ____): gait matches [ ]  facing correct [ ]  no visible pop [ ]
notes: ...
```

A sign-off with the boxes unticked is not a sign-off.

## M2 — shared combat

Required: `kills.tsv` (columns `tick, victimObjId, killerSide, aDeadMs, bDeadMs,
corpseA, corpseB`), plus both logs.

## M3 — shared stealth

Required: `trials.tsv` (columns `trial, guardObjId, acquired, alertMs,
targetSide, peerAgrees`), plus both logs.

## M4 — shared naval

Required: `hull.tsv` (columns `hit, shooterSide, targetShipId, hullA, hullB,
appliedMs`), plus both logs, plus the boarding screenshot.

## M5 — shared progression

Required: `objectives.tsv` (columns `missionId, objectiveId, side, completions,
tick`), plus both logs, plus the two save **fingerprints** (hash only — never the
save itself).

---

## Running a gate, in six lines

1. `bf-coop\audit\tools\run_audit.ps1 --rebuild` — confirm the audit is green
   enough for M0, and note the ASI hash.
2. Deploy that exact ASI to both instances; confirm both logs will carry it.
3. Run the scene to the duration in `GATES.md` (M1 10 min, M2 15 min, M3 15 min,
   M4 one transit + boarding, M5 one full mission).
4. Copy the logs and the table into `ops/evidence/<gate>-<date>/`.
5. `py -3 ops\tools\gate.py check <M#> --evidence ops/evidence/<gate>-<date>`
6. Only if it prints PASS: `… gate.py set <M#> --evidence …`.

## What makes a log *not* evidence

* its first lines do not name the ASI hash (task 0.2 exists to fix this);
* `peer=127.0.0.1` and no process id/role in the peer line (see
  `experiments/E0-second-instance.md`);
* it was produced by a build whose hash differs from `STATE.json`;
* it ends mid-line with no clean-exit marker and no crash dump was collected.
