# PS3 debug ELF — the symbol source we already had

```
D:\Games\acbf_ps3_gold\scimitar_final.elf     0.52 GB   debug build, DWARF
bf-coop\logs\ps3_pubnames.txt                 38.1 MB   demangled names
bf-coop\logs\dwarf\                           ~40 MB    extracted types + functions
bf-coop\tools\ps3_dwarf_dump.py                         the original extractor
ops\tools\ps3_query.py                                  targeted queries (enums / struct)
ops\evidence\ps3_enums_focus.txt                        2,324 candidate enums
```

The file was on disk the whole time. It was **not** in the orchestrator's context, which is why the
whole day was spent inferring from bit masks what this file states outright.

## What it already corrects

### 1. "role" is `EntityDescriptorNPCSubType`, not Guard/Civilian

31 values, extracted from the ELF:

```
0 All   1 Templar   2 Peasant   3 Thief   4 Courtesan   5 Mercenary   6 Target
7 CityEventBourgeois   8 CityEventPickpocketeer   9 CityEventOfficial   10 Orator
11 Horse   12 Nurse   13 TemplarAchievement   14 Male_Present_Hardsole   15 Assassin
16 Harasser   17 Pirate   18 Lite   19 Animal   20 Haytham   21 HomesteadNPC
22 Drunk   23 NavalNPC   24 CartHorse   25 Criminal   26 Dealer
27 NewspaperBoy   28 Vigilantes   29 VaneMarooned   30 ElTiburon
```

So `(kind 3, role 1)` = **Templar**, and role 2 = **Peasant**. RE's earlier table
("1=Guard, 2=Civilian") was a label error on our side, not a wrong value.

**Multiple enemy types confirmed and enumerated:** Templar, Thief, Mercenary, Harasser, Pirate,
Criminal, Vigilantes, NavalNPC (plus Assassin and Target as special cases). Hostile-ness is not one
of these numbers — it comes from faction and from the AI's own `IsEnemyOfThePlayer` handler.

### 2. `Layer` enum — the puppet's collision layer is wrong

```
LayerNone=0 … Character=5 … Disabled=40 … FloatingObject=41 … LayerSize=47
```

The plugin sets the puppet's filter to **`0x29` = 41 = `FloatingObject`**, believing it to be
"Disabled". `Disabled` is **40 (0x28)**. That is a concrete, testable bug — and it is one of the two
suspects AI identified for why guards do not perceive the ghost.

### 3. `ObjType`

```
OT_BHVGenericNPC=1   OT_BhvHorse=2   OT_BhvAssassin=3   OT_BhvAnimal=4   OT_AbstractObject=5
```

The behaviour-class discriminator we were inferring from vtables has a name.

### 4. Faction

The class exists (`scimitar::Faction`, 22 symbols) with
`CharacterFactionChangedEvent` / `FactionStatusChangedEvent` consumed by `EagleVisionComponent`,
and `FactionCondition`, `FactionDeathCondition`, `FactionCouplingCondition` as graph conditions.
The hostile test is exposed as
`CLConflict::CanDoConflict_PlayerEnemyHandler_IsEnemyOfThePlayer_PlayerPerceptionMonitor`.

## Next extraction steps

1. `ps3_query.py struct Entity` → exact member names/offsets (settles +0x9C, +0xE8, +0x660, +0x64/+0x66).
2. Enumerate `scimitar::Faction` layout and the faction enumeration values.
3. Map PS3 names onto the PC binary (Diaphora/BinDiff style, structure-based, then confirm each
   name against the PC code before trusting it).
4. Replace the `kind/role` heuristic in `SyncEnemiesOnly` with whatever the symbols prove hostile.

**Caveat that stays true:** PS3 is PowerPC — names and member *order* transfer, byte offsets do not.
Every offset still has to be confirmed on the PC build.

## 5. Faction is data, not a bitfield

`Faction_IDEnum` (DWARF, from the ELF) gives the Faction object's own members:

```
PINDX_FactionName = 0     PINDX_Allies = 1     PINDX_Enemies = 2
```

So a faction carries a **name**, an **allies** list and an **enemies** list. Hostility is a
relationship between two faction objects — which is why the damage gate `sub_133F630` only reads
`kind`, and why role was never the right axis. Related types found in the same pass:

- `Faction_ClassIDEnum`  ID=1287238156
- `CharacterFactionChangedEvent` (ID=1787754780), `FactionStatusChangedEvent` (1664693941)
- `AlertLevelTag`: Idle=0, TransitionSlowToIdle=1, Swim=2, Follow=3, TransitionFastToSlow=4,
  Avoid=5, Escape=6
- `PerceptionValues_IDEnum`: Range, ZCutoffUp/Down, HorizontalFOVAngle, VerticalFOVAngle,
  RangeSqr, Horizontal/VerticalFOVHalfAngleCos — the perception test is a cone with a Z cutoff
- `DetectionComponent`, `DetectionAabb`, `DetectionShape`, `ProximityPerceptionEvent`

**Consequence for the co-op mod:** "is this an enemy" must become "is this body's faction in the
enemies list of the player's faction" once `Faction` is mapped onto the PC build. The `kind/role`
test is a placeholder, not the answer.

## 6. What the ELF can and cannot settle

| question | ELF answers it? |
|---|---|
| enum values (NPC subtype, Layer, AlertLevel, …) | **yes**, directly |
| class member **names and order** | **yes** (DWARF) |
| class member **PC byte offsets** | no — PowerPC layout, must be re-confirmed on PC |
| PC function addresses | no — needs structure-based matching (RE T22) |
| behaviour/state-machine logic | only from decompiling the PS3 code |
