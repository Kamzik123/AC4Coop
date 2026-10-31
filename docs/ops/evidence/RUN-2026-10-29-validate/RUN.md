# RUN 2026-10-29 — validation run (build 5e0f7110, commit f063ea1)

| fact | value |
|---|---|
| build line | `[Build] asi=5e0f7110… commit=f063ea1 tree=clean` — 0.2 proven |
| crash | none |
| forged kills applied | 5 / 5 `RESULT stayed_dead=1 resurrected=0 lost=0` |
| visible deaths | **0** (operator report; matches the log's `after_alive=0` + no corpse) |
| `[NR]` null returns from sub_733FB0 | **0** |
| `[DG] null target` substitutions | 3 (the guard class is active) |
| net | `rx==tx` 304/304, `0 ent` (single instance, self-peer — expected) |
| smoothing | frames=165 drop=0 wipe=0 clamp=0 maxD=0.0498 |

## What this proves

* 0.2: a log can now be tied to a build. Proven.
* 0.1: five forged-state corruptions (the 2026-10-28 crash scenario) produced no fault. **Not yet proof** — see below.
* 0.3: the observer installs (`[NO] observer installed @ 0x00733FB0 steal=9`) but recorded zero nulls, so it has no positive control in this run.

## What it does not prove

* That the crash is fixed. With zero `[NR]` observations we cannot say the path was exercised. The A/B run (`FixNullReturn=0`) is what turns "no crash" into "the crash is fixed".
* That the observer logs anything at all. A positive control (a forced null) is needed.

## Confirmed again: the forged kill is not a kill

`[CT] before alive=1 hp=36.0` -> `applied write=1 after_alive=0 hp=65535.0` -> the predicate says dead, the guard keeps standing, no corpse, marker stays. This is the 2026-10-28 result repeating: writing the death *state* does not run the death *transition*. It is why stage 2A is an event replay, not a state forge.
