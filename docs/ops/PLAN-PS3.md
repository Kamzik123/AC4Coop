# PLAN — finish co-op using the PS3 debug build

Supersedes the guessing half of `PLAN-2026-10-30-enemy-sync.md`. The rule now: **read the symbol,
then confirm on PC.** No more inferring a field from a bit mask.

Source of truth: `D:\Games\acbf_ps3_gold\scimitar_final.elf` (0.52 GB, DWARF),
`logs\ps3_pubnames.txt` (38 MB demangled names), `logs\dwarf\` (pre-extracted).
Tools: `ops/tools/ps3_query.py`, `ps3_structs.py`, `ps3_pubnames.py`.

---

## P0 — Symbol extraction (mostly done)

| item | state |
|---|---|
| `EntityDescriptorNPCSubType` (31 values) | **extracted** |
| `Layer` (Disabled=40, FloatingObject=41, …) | **extracted** |
| `ObjType` (BhvAssassin=3, BHVGenericNPC=1) | **extracted** |
| `Faction_IDEnum` (Name/Allies/Enemies) | **extracted** |
| `PerceptionValues`, `AlertLevelTag`, detection shapes | **extracted** |
| full class index (2,950 types) | **extracted** → `ops/evidence/ps3_class_index.txt` |
| dense game-class layouts (Bhv*, Human, Entity) | **thin in DWARF** — only engine/template types are dense; game classes must come from pubnames + decompile |

**Exit:** every enum the mod touches is extracted. Done for now.

## P1 — PS3 → PC name map (RE T22, in flight)

Targets, in priority order:

1. `Entity::GetComponentByType`, the component container grow path
2. `EntityKindRole` equivalent — the setter/getter of the `+0x9C` word
3. `Faction` object: get faction, allies/enemies lists, `CharacterFactionChangedEvent`
4. `CLConflict::…IsEnemyOfThePlayer…`, `PlayerPerceptionMonitor`
5. `SetGlobalMatrixInternal` and the `CharacterController` filter setter
6. entity factory: `BuildEntityWithSelector` and its vtable slots
7. `CSrvNPCHealth::SetLife`

Method per name: PS3 address from pubnames → decompile PS3 → record shape (call pattern, string
refs, immediates) → find the PC candidate by shape → **decompile the PC side to confirm** →
confidence tag. Unconfirmed entries stay unconfirmed; they never become code.

**Exit:** ≥ 12 entries confirmed, each with both decompiles cited.

## P2 — Replace heuristics with symbol-proven facts

| change | replaces | proof needed |
|---|---|---|
| `SyncEnemiesOnly` = faction enemies-list test | `kind==3 && role==1` | P1 item 3+4 |
| puppet collision layer `0x29 → 0x28` | mislabelled "Disabled" | **done** (IMPL T21) |
| puppet perception registration | "no perception" assumption | P1 item 4 |
| spawn missing enemy via factory | "clone a live guard" workaround | P1 item 6 |
| component walk via `+0x66` | `+0x64` capacity | **done** |

**Exit:** no `kind/role` heuristic left in a shipped build; every classification cites a symbol.

## P3 — Verify in game, one change at a time

Each P2 change is A/B'd separately (ini flag or separate commit) so a failure is attributable.
Oracle: `ops/tools/coop_report.py --kindrole` plus a new `--faction` check.

**Exit:** G6 (enemy census), G7 (identity), G9 (kills) green on a two-machine run.

## P4 — Same treatment for the rest

Naval (faction on ships, `ShipSuperThreeArchetype::GetEnemySidesToTarget`), progression
(objective/desync reason ids — the ELF has the reason enum), and the puppet spawn path.

---

## Execution order, right now

1. IMPL T21 lands (layer fix + fighter set) → build → deploy.
2. RE T22 delivers the first 5 confirmed names.
3. I extract PS3 addresses for all P1 targets from pubnames so RE starts from a list, not a search.
4. Then P2 items are scheduled one at a time, each with an A/B switch.
