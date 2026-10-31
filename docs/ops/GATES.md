# Gates

A gate is a **machine-decided** pass. A human may add a note, never remove a
failure. Every gate lists: entry, the exact commands, the numbers, the artifacts
that must exist afterwards.

The runner is `audit/tools/run_audit.ps1` for anything static, and
`ops/tools/gate.py` for anything that needs the logs. `run_audit.ps1` writes
`audit/report/audit.json`; `gate.py` reads it (for M0) and is the only thing that
writes the gates in `ops/STATE.json`. A gate is only green if the numbers below
appear in a file that still exists **and** that file names the build under test.

---

## M0 — Truth

Entry: none.

```powershell
bf-coop\audit\tools\run_audit.ps1 --rebuild
```

| check | pass |
|---|---|
| `audit` gates | M0, M2, M4, M5 all PASS |
| contradictions | 0 live in `audit/ledger/contradictions.jsonl` |
| hook table | `0x681817` present in `HOOK-TABLE.md`, and the site is guarded |
| build log | every new `phase1_log_*.txt` has `[Build] asi=<sha256>` on line ≤ 3 |
| IDB | `idb.named_functions ≥ 2000` **or** the ≥50 load-bearing functions named + 6 structs |
| kit | `ops/DECISIONS.md` explains every drift key |

Artifacts: `audit/report/AUDIT-REPORT-<date>.md`, `audit/report/audit.json`.

---

## M1 — C0 + C1 (shared presence, shared movement)

Entry: M0 green; two machines; both builds identical.

Scene script (`ops/tools/scene_roam.py`): spawn, walk 60 s, sprint 30 s, climb one
wall, swim 20 s, jump from a roof, stand still 10 s. Total ~3 min, then 7 min of
free roam.

| # | check | pass |
|---|---|---|
| 1 | both logs show `peer=<not 127.0.0.1>` | both |
| 2 | `rx > 0` and `tx > 0` on both | both |
| 3 | `[Net] apply` count ≈ `rx` | ≥ 90 % |
| 4 | `[Smooth] drop=0 wipe=0 clamp=0` | both |
| 5 | `maxD` | p99 ≤ 1.0 m, max ≤ 2.0 m |
| 6 | crashes | 0 in 10 min |
| 7 | human gait sign-off | both, in `SIGNOFF.md`, containing `asi=<hash>` and `tree=clean\|dirty` |

Artifacts (`ops/evidence/M1-<date>/`): two `phase1_log_<pid>.txt`, two
`AC4Coop_build_<pid>.log`, `SIGNOFF.md` (one per player, stamped).

---

## M2 - C2 (shared combat)

Entry: M1 green.

Scene (`scene_fight.py`): 20 scripted kills — 10 on A, 10 on B — each on a guard
that the other machine can also see. Plus 5 deliberate "both hit the same guard"
races and 5 "kill a guard that is already dead remotely" cases.

| # | check | pass |
|---|---|---|
| 1 | matched kills | 20/20 |
| 2 | false deaths (a death on B with no cause on A) | 0 |
| 3 | corpse present on both | 20/20 |
| 4 | latency blow → death on peer | ≤ 200 ms, p95 |
| 5 | double-kill race | exactly one death, no double corpse |
| 6 | late packet (drop 20 %) | no resurrection, no duplicate corpse |
| 7 | crash | 0 in 15 min |

Artifacts: per-kill table `ops/evidence/M2-<date>/kills.tsv` with columns
`tick, victimObjId, killerSide, aDeadMs, bDeadMs, corpseA, corpseB`, plus both
logs.

---

## M3 — C3 (shared stealth)

Entry: M2 green.

Scene (`scene_stealth.py`): 5 guards, puppet placed in one cone at a time, 10
trials, no other target. Plus one run where the *player* (not the puppet) is
detected on both machines.

| # | check | pass |
|---|---|---|
| 1 | guard acquires + attacks the puppet | ≥ 8/10 |
| 2 | alert level agreement within 300 ms | ≥ 45/50 samples |
| 3 | same guard targeted on both machines | ≥ 9/10 |
| 4 | player-detection parity (both machines agree) | 10/10 |
| 5 | crash | 0 in 15 min |

Artifacts: `ops/evidence/M3-<date>/trials.tsv` with columns
`trial, guardObjId, acquired, alertMs, targetSide, peerAgrees`, plus both logs.

---

## M4 — C4 (shared naval)

Entry: M2 green (C3 not required).

Scene (`scene_ship.py`): two ships, 10 cannon hits each direction, one sink, one
boarding, one world travel.

| # | check | pass |
|---|---|---|
| 1 | hull delta equal on both machines | ≤ 1 unit error, 10/10 |
| 2 | sink observed on both | yes |
| 3 | boarding deck state identical | yes (screenshot + log) |
| 4 | world travel | no crash, ghost tombstoned |
| 5 | crash | 0 |

Artifacts: `ops/evidence/M4-<date>/hull.tsv` with columns
`hit, shooterSide, targetShipId, hullA, hullB, appliedMs`, plus both logs and the
boarding screenshot.

---

## M5 — C5 (shared progression)

Entry: M2 green.

Scene: one full mission, ≥ 3 objectives, one cutscene, one checkpoint, then
save/load on both machines, then a 20 % packet-loss replay.

| # | check | pass |
|---|---|---|
| 1 | each objective completes exactly once per machine | yes |
| 2 | cutscene plays exactly once per machine | yes |
| 3 | journal identical after save/load | yes |
| 4 | 20 % loss run | no duplicate, no loss |
| 5 | divergence refusal | join with mismatched progress is refused with a clear message |

Artifacts: `ops/evidence/M5-<date>/objectives.tsv` with columns
`missionId, objectiveId, side, completions, tick`, plus both logs, both save files'
fingerprints (hash only — never the save), and the divergence-refusal screenshot.

---

## Gate hygiene

* A gate may not be marked green by editing `STATE.json` by hand. `gate.py` writes
  it, and the commit that flips a gate must include the evidence folder.
* If the build changes **or the ASI disappears**, every gate goes back to `unknown`
  until the scene is re-run. `gate.py status` does this by hashing the ASI.
* Every evidence folder must name the build it belongs to (the 12-character ASI
  hash must appear in at least one file). `gate.py` refuses the gate otherwise.
* A green gate with a human sign-off note is the only thing that can be called
  "working" in any document.
