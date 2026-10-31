# IDB hardening — names + structs (BIN seat, shift 2)

Date: 2026-10-29. Binary sha256 `bc11ad79…` (base 0x400000, no DYNAMICBASE).
IDB: `D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe.i64`.
Session `bin-heavy-shift2`. All offsets below were read from decompiles THIS shift, not from markdown.

## PHASE 1 — naming table

| address | old name | new name | evidence (decompiled line) | conf |
|---|---|---|---|---|
| 0x9F0B50 | sub_9F0B50 | `HandleContainer_GetObjectID` | `return sub_A200D0(this, 0, 1);` (3-arg thunk; E1 confirms push1=allowAuthored, push0=allowWrite) | high |
| 0xA200D0 | sub_A200D0 | `HandleContainer_ResolveObjectID` | 4-arg core: `v4=sub_A1E8D0(a2,0,0)` authored table → `v7=*(a2+4); (v7&0x3FFFF)&&(v7&0x10000000)` paged → `sub_A1EC40(a2)` map → `_InterlockedIncrement(this+2)` mint | high |
| 0xA1F160 | sub_A1F160 | `HandleContainer_GetObjectByID` | id→obj: `sub_A1E8D0(0,a1,a2)` → `sub_A076F0(a1,a2)` → `sub_A1DC00(a1,a2)`; returns object ptr (`v3=*(v2+8)`) | high |
| 0xA1C9A0 | sub_A1C9A0 | `HandleContainer_MintID` | `return (unsigned int)_InterlockedIncrement(this + 2);` | high |
| 0x19FB4C0 | sub_19FB4C0 | `CSrvNPCHealth_SetLife` | `*(this + 46) = a2;` with `__int16 *this` ⇒ offset 0x5C; death path `sub_A014A0(dword_4E1CC40,0)` | high |
| 0x1BFF520 | sub_1BFF520 | `CSrvNPCHealth_GetLife` | `return *(this + 46);` (same 0x5C) | high |
| 0x133F550 | sub_133F550 | `Entity_IsAlive` | `v2=sub_7025F0(a1); if(!v2 || (*(BYTE*)(v2+184)&0xC)!=4) { v3=sub_10EF8C0(a1); if(!v3||(*(v3+4)&0x1000)==0) return true; } return false;` | high |
| 0x10EF8C0 | sub_10EF8C0 | `Entity_GetComponentByClassTag` | `while (sub_A1D0D0(*v2) != 1588608236)` over component list from `sub_6F5F70` (0x5EB03CEC class tag) | medium |
| 0x733FB0 | sub_733FB0 | `EntityHandle_GetEntity` | `v2=*(this+212); v2==4→this; v2==5→sub_E10E50(*(this+560)) or sub_4956E0; else 0` | medium |
| 0x13DD550 | sub_13DD550 | `AI_Detection_GetStateName` | returns `sub_986600("DETECTION","In Conflict"/"Alerted"/"Investigating"/"Unaware")`; reads actor via `*(a2+2272)` (0x8E0) | high |
| 0x1013BA0 | sub_1013BA0 | `AI_AlertState_IsInConflict` | bit test `*(*(*(this+7)+64)+4*(idx>>5)) & (1<<(idx&0x1F))` (off_29EDE38) — labeled "In Conflict" by 0x13DD550 | high |
| 0x1002E20 | sub_1002E20 | `AI_AlertState_IsAlerted` | `return *(_WORD *)(*(this + 7) + 326) != 0;` — labeled "Alerted" | high |
| 0x1013D20 | sub_1013D20 | `AI_AlertState_IsInvestigating` | same bit-test shape, off_29EEB9C — labeled "Investigating" | high |
| 0x679220 | sub_679220 | `Filter_IsSatisfied` | `if((*(WORD*)this+126 &7)==0)return 1; if(==5 && !(*(BYTE*)this+21 &4))return 0; if(*(BYTE*)this+21&2)return 1; return *(float*)this > f1 || fabs(diff)<=0.0005` | medium |

### Left as `sub_*` (reason)

| address | why not renamed |
|---|---|
| 0x679750 | state-machine apply/update; masks + virtual dispatch; domain unclear |
| 0x681980 | heavy update; entity refs + virtual calls; role not self-evident |
| 0x6817C0 | reset/init of a filter/transition node (E1 crash path `0x681801`); no unique verb |
| 0x744CF0 | scans a presence array for a **zero** slot and writes the index — a free-slot allocator, but of *which* resource is unclear (existing note: "does not return active pad id") |
| 0x133F5A0 | event/notification dispatch on death; too many indirections to name safely |
| 0x173E190 | huge interaction/attack dispatcher (class-tag match 1743972456/-1686089828, cases 13/14/19/20) |
| 0x17106C0 | predicate full of magic constants (`a2+0x9C&7==2`, `&0x1F8==0x98`, a3+0x10∈{23,5,14,26}) |
| 0x528200 | "for each child call vfunc+188" broadcast; callback semantics unknown |
| 0x79B8D0 | sets flags, calls vfuncs; unclear activate/enable |
| 0x1978B50 | list-processing loop; unclear |
| 0x16FA7D0 | transform/matrix update (SIMD at +0x150/+0xE00); too broad to name |
| 0x636480 | bit-twiddling flag/state setter (bits 26/27/28 + low 6) |

### Cheap-list: 0x1A3A20C was a typo for 0x1E3A20C

**0x1A3A20C is NOT an IAT slot** — byte-level proof: the byte at 0x1A3A20C is `0x52` = `push edx`, mid-instruction inside `sub_1A3A0C0` (0x1A3A0C0, `.text`, normal prologue `push ebp / mov ebp,esp`). `.text` spans 0x401000..0x1e3a000, so 0x1A3A20C is code, not `.idata`.

The dispatch's "(IAT slot)" hint was right in spirit but the address had a one-character typo: **`0x1E3A20C`** (E, not A) IS the IAT slot, and it names the import **`CreateMutexW`** (KERNEL32). Verified via `entity_query(imports)` over 0x1E3A200..0x1E3A220 → `0x1e3a20c = CreateMutexW`.

## PHASE 2 — structs (declared in local type library)

| name | size | fields proven (offset : type : evidence) |
|---|---|---|
| `CSrvNPCHealth` | 0x5E | `m_Life` 0x5C `__int16` — SetLife `*(this+46)` / GetLife `*(this+46)` |
| `Entity` | 0x8E4 | `m_vtable` 0x00 (class tag via `*(*(vt+0x14)+0x14)`); `m_ObjectIDBits` 0x04 `u32` (bits 0..17 handle idx, bit28 registered, bits 18..31 id_hi — 0xA200D0); `m_Components` 0xE8 `ptr` (sub_7025F0/sub_6F5F70 read `*(a1+232)`); `m_AIActor` 0x8E0 `ptr` (0x13DD550 reads `*(a2+2272)`) |
| `AIActor` | 0xB9 | `m_AlertState` 0x1C `ptr` (`*(this+7)` in 0x1013BA0/0x1002E20/0x1013D20); `m_AIState` 0xB8 `u8` (bits 0x0C, value 4 == dead — 0x133F550 `*(v2+184)&0xC!=4`) |
| `AIAlertState` | 0x148 | `m_DetectionBits` 0x40 `u32` (bitmask array base, 0x1013BA0/0x1013D20); `m_Alerted` 0x146 `__int16` (0x1002E20 `*(WORD*)(this+326)!=0`) |

Not provable this shift → **not declared**: `Human` (m_WeaponTotallyOut/m_IsInTree/m_IsOnShip/m_IsOnOcean/m_DoJumpToVoid/m_Stalking*/m_DisturbedZoneCount — no access site decompiled), `EntityGroup` (0x1E4A128 family — no reference seen), `CharacterController` (m_DriveEntity/m_FilterInfo/+0x146/+0x1C0 TransformProbe fields — the +0x146 seen here is the AIAlertState "alerted" word, a coincidental offset).

## PHASE 3 — survey_binary statistics (after this shift)

- `named_functions`: **379** (recorded baseline 363; +14 are this shift's renames, +2 drift not caused by this shift)
- `total_functions`: **128612**
- `unnamed_functions`: 127317
- engine structs defined this shift: **4** (3 of the six + AIAlertState discovered during analysis)

## Contradictions vs existing notes

1. `0x1A3A20C` is not an IAT slot — it is `push edx` inside `sub_1A3A0C0`; the intended IAT slot was `0x1E3A20C` = import `CreateMutexW` (KERNEL32).
2. `padid.behaviour` says the function "returns 0" — actually `sub_744CF0` returns **1** when it finds a free slot and 0 when none; the note's core claim (it does not return the active pad id) is correct.
3. `alive.decompile` / `setlife.decompile` / `gate.logic` in ida_findings.json all match the decompiles read this shift — no contradiction.

## PHASE 4 — Human / EntityGroup / CharacterController (shift 3)

### Human access chain — THE wire answer

GetEntityAI = `0x6F5F70`; GetHuman = `0x13905A0`. Full chain with the proving instruction:

```
Entity* e
  e+0xE8                -> AIComponent* (a COMPONENT SLOT, direct pointer) [0x6F5F86 mov eax,[a1+0E8h]]
  AIComponent+0x1C      -> AbstractEntityAI* (per-entity AI statechart)      [0x6F5F88 mov eax,[v2+1Ch]]
  AbstractEntityAI+0x20 -> p3                                                [0x7026EE]
  p3+0x84               -> p4                                                [0x702708]
  p4+0x24               -> p5                                                [0x70270A]
  p5->vtbl[18](p5,0)    -> &Human->IHuman  (= Human + 0x10C)                 [0x13905CD]
  Human* = that - 0x10C                                                      [0x13905CF v3-268]
```

**Human is reached from a COMPONENT SLOT, not from `Entity+0xE8` being the AI pointer.**
`entity+0xE8` is the AI component; the AI (AbstractEntityAI) is at `AIComponent+0x1C`. The plugin's
`GetEntityAI` (0x6F5F70) correctly implements the two-hop `*(*(e+0xE8)+0x1C)` — its assumption holds.
The `-0x10C` in GetHuman proves `offsetof(Human, IHuman) == sizeof(AIActor) == 0x10C` (PutHoodDown also
receives `&this->IHuman` and reads `*(this-1)` = `m_AIActorDataPtr` at 0x108, so `this` = 0x10C).

### Contradiction vs prior shift's Entity struct

- Prior `Entity.m_Components @ 0xE8` is **WRONG**. 0xE8 is a direct `AIComponent*` (single pointer, not an
  array). The component ARRAY (`BaseSmallArray<Component*>`) is at **+0x60** (data ptr) / **+0x66** (u16
  count), with a components-version byte at **+0xF0** — proven by the component getters 0x4375D0 / 0x5F96D0
  (`[a2+60h]`, `[a2+66h]`, `[a2+0F0h]`).
- Prior `Entity.m_AIActor @ 0x8E0` is **UNVERIFIED**: the +0x8E0 read in 0x13DD550 is relative to
  `*(a3+0x5C)` (see sub_15DEF90), whose base object is not confirmed to be an Entity. The canonical
  entity→AI path is `+0xE8 -> +0x1C`.

### Proven fields (declared in local types this shift)

| struct | proven to | fields (offset : evidence) |
|---|---|---|
| `Human` | 0x110 | vtable 0x00; m_Flags 0x04; m_ActorTrackList[5] 0x08 stride 0x30 (track.m_CurrentContext @ +0x14 — 0x52C2E0/0x52DFF0); m_ActorMainTrack 0x100 (0x52DFF0 `[this+256]`); m_ActorSecondaryTrack 0x101; m_IsHuman 0x102; m_DataGateway 0x104; m_AIActorDataPtr 0x108 (0x169F260 `[this-1]`); IHuman 0x10C. Also `AIActorData+4 = m_ActorEntity (Entity*)` via 0x5FC130/0x5FC110 `[this+1]`. |
| `CharacterController` | 0x0C | vtable 0x00 (0x1E6EE10, 100 slots, descriptor slot5=0x7AA8E0); m_ObjectFlags 0x04 (enabled bit 0x4000000, 0x7130C0 `InterlockedCompareExchange(this+1)`); m_Owner 0x08 Entity* (0x7AF890 `[this+8]`). |
| `EntityGroup` | 0x149 | vtable 0x00 (0x1E4A128, 83 slots, Duplicate=slot3=0x503600); +0x100 4×16-byte block (0x503600 OWORD copies); field_0x140 array ptr; field_0x146 u16 count; field_0x148 byte. |

### NOT proven this shift (correct outcome: left `unknown`)

- **Human** locomotion fields `m_WeaponTotallyOut / m_IsInTree / m_IsOnShip / m_IsOnOcean / m_DoJumpToVoid /
  m_Stalking*Count / m_DisturbedZoneCount` — no engine accessor decompiled; the plugin-header offsets for
  these are CLAIMS, unverified.
- **CharacterController** `m_DriveEntity / m_KamikaziPosition / m_ConcatenateToAnim / m_UseVelocityPosition /
  m_UseKamikazePosition / m_Velocity / m_Force / m_Impulse / m_FilterInfo` — vtable recovered, but no
  field-access instruction was decompiled; offsets remain plugin claims.
- **EntityGroup** `+0x5C` (m_AssociatedWorld) and `+0x78` (rigging) — plugin claims only (`+0xE8` is
  corroborated by the independent Entity+0xE8 = AI-component finding).

## PHASE 5 — six more load-bearing names + the 0x8E0 chain (shift 4)

### New names (named_loadbearing 14 -> 20)

| address | name | evidence (decompiled line) | conf |
|---|---|---|---|
| 0x528200 | `Entity_SetGlobalMatrixInternal` | `*(this+20) \|= 0x20000` dirty flag; `(*(vtable+128))(this,a3)` matrix/skeleton push; if a3 (updatePhysics) iterate children `(*(vtable+188))(*child, this+4)`. **Plugin phase1.cpp:6994 confirms VA 0x00528200 = SetGlobalMatrixInternal, __thiscall ECX=this.** | high |
| 0x744CF0 | `PlayerPadManager_GetUnusedPadID` | scans `this[padid]` bytes from an ordered id list (sub_A0A790), returns first zero slot: `while(*(this+(u8)*v5)){if(++v5==v4)return 0;} *a2=*v5; return 1;`. **Plugin phase1.cpp:6796 confirms `base()+0x344CF0 = PlayerPadManager::GetUnusedPadID`.** | high |
| 0x679750 | `AnimNode_Update` | 2-arg: `v3 = a2 & *(u16*)(this+18)`; switch `*(this+252)&7` (cases 1-6); drives `*(this+276)`->vtable+40 and `*(this+280)`->vtable+152; `*(u16*)(this+14) \|= v3` | medium |
| 0x681980 | `AnimNode_Dispatch` | 4-arg: resolves entities (sub_69D440/sub_69AD50), switch `*(this+252)&7`, applies to `*(this+276)`->vtable+28 and `*(this+280)`->vtable+140 | medium |
| 0x6817C0 | `AnimNode_Reset` | 1-arg: zeroes `*this`,`*(this+3)`,`*(this+20)`; `*(this+8)=*(this+58)`; re-fetches entities via EntityHandle_GetEntity(`*(this+69)`,`*(this+70)`); sub_67DD50 reset | medium |
| 0x17106C0 | `Combat_SeamPredicate` | `*(a2+156)&7==2 && (*(a2+156)&0x1F8)==0x98` then `*(a3+16)∈{23,5,14,26}` xor `*(a3+32)∈{3,6,14}` -> call sub_1A90920 | medium |

**AnimNode class**: `this` is the POD node sub-object at `outer+0xE0` of the anim-dispatcher class
(vtable **0x1E47678**; see ground-truth-register rows `sub_734240`/`sub_734120`). Same node family as
`Filter_IsSatisfied` (0x679220). No vtable of its own — `*(this)` is a data dword (Reset zeroes it), and
its child/target objects live at `this+0x114`/`this+0x118` (entities) and `this+276`/`this+280` (polymorphic).

**Refused** (left `sub_*`): `0x173E190` — huge interaction/attack dispatcher (class-tag 1743972456/-1686089828,
cases 13/14/19/20); role clear but no single verb fits, so not named.

### The 0x8E0 chain — VERDICT: the plan's base is wrong

`sub_13DD550` (AI_Detection_GetStateName) has exactly one caller: `sub_15DEF90`. Decompiled:
`a2 = *(a3 + 0x5C)`, then inside `v3 = *(a2 + 0x8E0)` -> `AI_AlertState_IsInConflict(v3)` (`v3+0x1C` -> +0x40/+0x146).

- `sub_15DEF90` is a **registered debug-overlay renderer** — registered in `sub_1622D70` (0x16235EE:
  `v176 = sub_15DEF90; sub_463CB0(...)`) alongside `"FRIENDS"`/`"SAVE_GAME"`/`"MISSION_RATING"`. Its `a3` is
  the overlay context, **not an Entity**.
- Cross-check `sub_1398900` (a caller of IsInConflict) reaches the same object as
  `*( *(a2+0xC) + 0x5C ) + 0x8E0`. So the `+0x8E0` object is always reached through a `*(X+0x5C)` hop,
  never directly off an Entity.
- The object at `+0x8E0` (has `m_AlertState @ +0x1C`) is a **different type** from the `AbstractEntityAI`
  that GetEntityAI returns (which has `m_AIState @ +0xB8`, services @ +0x5C/+0x70, alive @ +0x8C — a
  Statechart whose +0x1C is a `BaseSmallArray`, not a detection pointer).

**Corrected chain for alert state is NOT `entity+0x8E0`.** The verified entity->AI path remains
`entity+0xE8 -> AIComponent -> +0x1C -> AbstractEntityAI`; a verified Entity->alert-actor path was NOT
established. The call site that would settle the identity of `a3` (and therefore `*(a3+0x5C)`) is
`sub_1622D70` at `0x16235EE` (the debug-screen registration that binds `sub_15DEF90`), whose overlay
context type is still unknown.
