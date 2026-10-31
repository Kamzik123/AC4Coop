# QA — T14(2): what build 280D652169D0 can and cannot prove about C21

## Build under test — confirmed on disk

```
280D652169D0  ref\AC4Coop\bin\AC4Coop.asi                  303,104 B  2026-10-30 12:15:24
280D652169D0  dist\builds\REL-280d652169d0.asi             303,104 B  2026-10-30 12:15:24
```
Matches the hash the orchestrator named. **But no log from this build is in the repo**: a
recursive grep for `kindrole` over every `*.txt` / `*.log` returns **0 hits**, and the newest
`phase1_log_*.txt` on disk is still `RUN-2026-10-29-*`. The live half of C21 therefore has
**no captured evidence yet** — the claim is currently "the build should emit it", not "it did".

## What the deployed line actually is

`phase1.cpp:7798-7799`
```cpp
L("[Sync] kindrole n=%u soldier=%u civilian=%u other=%u\n",
  s_krN, s_krSoldier, s_krCivilian, s_krOther);
```
counted at `phase1.cpp:3232-3236`, i.e. **before** the `SyncEnemiesOnly` filter at `:3237`, so
`n` is the whole enumerated human population:
```cpp
3232: const KindRole kr = EntityKindRole(e);
3233: ++s_krN;
3234: if (IsSoldierBody(kr))       ++s_krSoldier;
3235: else if (IsCivilianBody(kr)) ++s_krCivilian;
3236: else                         ++s_krOther;
```
with `EntityKindRole` at `:2673-2681` (`raw = *(u32*)(e+0x9C)`, `kind = raw & 7`,
`role = (raw >> 3) & 0x3F`) and the predicates at `:2682-2683`
(`soldier = kind==3 && role==1`, `civilian = kind==2 && role==2`).

## Verdict: the aggregate proves liveness and non-degeneracy — not C21

| C21 clause | proven by `[Sync] kindrole`? |
|---|---|
| `+0x9C` is readable at runtime for every enumerated human | **yes** — `n > 0` |
| the population splits into soldier / civilian / other | **yes** — three non-empty classes |
| the counters are self-consistent | **yes** — `soldier+civilian+other == n` |
| the tuples are `(3,1)` and `(2,2)` | **NO** — only sums are printed; no kind, no role, no `raw` |
| the packing `kind=v&7, role=(v>>3)&0x3F` | **NO** — `raw` is never printed |
| "every attacker is (3,1)", "every fleer is (2,2)" | **NO** — no field records attacking or fleeing |

The sharpest objection: `soldier` and `civilian` are counted **by the very predicate under
test** (`:3234-3235`). The line is the predicate counting itself, so it cannot disagree with
itself. A wrong offset that returned plausible garbage would still print
`n>0 soldier>0 civilian>0 other>0`. What would make it evidence is an **independent** signal
on the same bodies — the component walk already in scope (`skel` vtable `0x01E41E58`,
`BhvGenericNPC` `0x026E34D8`, `:3082-3087`, and the `weapon_A8` / `icon_D0` counters behind C8/C9).

So: aggregate = **C21 liveness, PARTIAL**. Semantic C21 stays unproven.

## The emit, as shipped in REL-80f9a9a7e588 (T18) — and the gate that turns it off

`phase1.cpp:3253-3258`, inside the pre-filter loop, no new engine calls:
```cpp
if (KrLogDue()) {
    ++s_krLines; ++s_krWinLines;
    L("[KR] ent=%08X objId=%llu raw=%08X kind=%d role=%d bhv=%08X skel=%u comps=%u\n",
      (unsigned)e, (unsigned long long)hc->GetObjectID(),
      kr.raw, kr.kind, kr.role,
      (bhvC && readable(bhvC, 4)) ? *(uint32_t*)bhvC : 0u, skel ? 1u : 0u, (unsigned)n);
}
```
Differences from the spec I handed over, all absorbed by the parser:
* `comps=` is added (IMPL's call — it is the component count already in `n`); the parser
  reads key/value pairs, so an added or removed field cannot stop a row matching;
* there is **no `stage=` and no `d2=`**, so "every attacker is (3,1)" is *not* evaluable
  per row from this emit — the `kindrole attackers` row reports that as INFO;
* `kind=%d` / `role=%d` are **signed**: a body whose `+0x9C` read fails prints
  `raw=00000000 kind=-1 role=-1`, which is now its own `kindrole read` FAIL instead of
  being silently dropped by a `\d+` regex.

**Blocker for the run:** `KrLog` defaults to **0** and **no `.ini` on disk sets it**
(recursive grep for `KrLog` over every `*.ini` → 0 hits). A tester who deploys this build
and runs will produce **zero `[KR]` rows**, and the oracle will correctly say
`field absent`. Add under `[Coop]` before the run:

```ini
KrLog=1
```
Caps are 96 rows / 5 s and 600 / process, so rows repeat for the same body across windows;
`kindrole split` therefore reports rows and distinct `ent=` counts separately.

## Machine-decided PASS, as implemented in `ops/tools/coop_report.py --kindrole`

| row | rule |
|---|---|
| `kindrole live` | `n>0` **and** `soldier+civilian+other == n` (FAIL otherwise) |
| `kindrole coverage` | INFO when a scene contains only one class |
| `kindrole read` | FAIL if any row has `kind<0` or `role<0` (the `+0x9C` read failed) |
| `kindrole packing` | every `[KR]` row: `kind==raw&7`, `role==(raw>>3)&0x3F`, no bits above bit 8 |
| `kindrole vs components` | every `skel=1 && bhv=0x026E34D8` body has `kind∈{2,3}` — **the independent check** |
| `kindrole attackers` | INFO: the shipped row has no `stage=`, so a fighting body cannot be identified |
| `kindrole fleers` | INFO, always: not measurable, no flee signal exists in the log |

## Numbered run procedure

1. Deploy `80F9A9A7E588`; set `[Coop]` `KrLog=1`, `EntitySelfTest=0`, `KillTrace=1`.
2. Scene A — civilian street (no patrol in view): idle 20 s.
3. Scene B — guard post / fort: idle 20 s, let the patrol walk past.
4. Scene C — mixed market: idle 20 s, then start one fight and one flee.
5. Copy the single `phase1_log_<pid>.txt` to `ops/evidence/RUN-<date>-c21/` unedited.
6. `py ops\tools\coop_report.py ops\evidence\RUN-<date>-c21 --kindrole --expect-hash 80F9A9A7E588`
7. PASS = no FAIL rows among `kindrole live`, `kindrole read`, `kindrole packing`,
   `kindrole vs components`. `attackers` / `fleers` stay INFO by construction — that is a
   property of the log, not a run failure.

Until step 6 has run on a real log, C21's live half is **open**: the build on disk is
`80F9A9A7E588` (303,616 B, 2026-10-30 12:18:33) but **no `[KR]` row exists in any log in the
repo** (`grep '^\[KR\] '` over every `*.txt`/`*.log` → 0 hits), and `KrLog` is unset.
