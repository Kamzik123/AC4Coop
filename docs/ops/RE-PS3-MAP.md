# RE-PS3-MAP — PS3 (scimitar_final.elf) → PC (AC4BFSP.exe) symbol map

Seat: RE. Date: 2026-10-30. Task: T22.

Every PS3 address here is from **`D:\Games\acbf_ps3_gold\scimitar_final.map`** (the linker map:
`Address Size Align Out In File Symbol`, second line of each symbol is the demangled name).
Every PS3 function was disassembled with capstone 5.0.7 (`CS_ARCH_PPC`, `CS_MODE_32|CS_MODE_BIG_ENDIAN`)
at that address and the listed instructions are the actual bytes.
PC addresses were confirmed by Hex-Rays on `AC4BFSP.exe.i64`.

## 0. Address-space caveat — read this first

**`logs\ps3_pubnames.txt` and `ops\evidence\ps3_p1_map.tsv` addresses are NOT usable for disassembly.**
They do not resolve in `scimitar_final.elf`:

| VA from pubnames/p1_map | what the ELF actually holds there |
|---|---|
| `0x001a7cf3` `Entity::GetComponentByType` | nothing that decodes (0 instructions) |
| `0x001b7c91` `EntityBuilder::BuildEntityWithSelector` | garbage: `lhz r2,0x80(0); lbz r24,0x3080(0); xori r14,r8,0x9048` |
| `0x00191d2c` `CharacterFactionChangedEvent::…` | mid-function junk (`sth r11,0xd4(r1); stvx …`) |

The `.map` address for the same symbols does disassemble, and the control experiment is exact:
`AbstractEntityAI::GetComponentContainer` at `.map` `0x0039877c`, size `0xc`, decodes to

```
0039877c  addi  r3, r3, 0x90
00398780  clrldi r3, r3, 0x20
00398784  blr
```

which is a 3-instruction `return this + 0x90;` — a 12-byte getter, i.e. exactly the size in the map.
Use the `.map`. Use `ps3_p1_map.tsv` only as a *name list*.

## 1. Confirmed map

| PS3 symbol | PS3 addr | PS3 size | PC addr | PC size | how confirmed | conf |
|---|---|---|---|---|---|---|
| `scimitar::Faction::Faction()` | `0x01d1af14` | `0x60` | `0x0019123B0` | ctor | Both are the ctor and both initialise **two inline containers at +0xC and +0x14**: PS3 `addi r3,r3,0xc` … `addi r29,r29,0x14`; PC `sub_A062A0(a1+12); sub_A062A0(a1+20);`. PS3 also stores vtables `0x22EB18`/`0x23CDD8`, PC stores `off_27043E0`. Identical member offsets across two builds. | **high** |
| `scimitar::Entity::GetComponentByType(ClassDescriptor const*)` | `0x006a3e68` | `0x54` | `0x005200C0` | 51 B | Same shape: PS3 = `addi r29,r29,0x60` → `bl <iter version>` → `lhz r0,4(r29)` → `lwz r9,0(r29)` → `slwi r0,r0,2` → compare + return 0. PC = `sub_520020(...)` then `a2 == *(this+96) + 4*(u16)*(this+102)` → return 0 else `*a2`. Same thin-wrapper over the iterator version. **See §2 for the +0x64/+0x66 difference.** | **high** (structure) |
| `scimitar::Entity::GetComponentByType(ClassDescriptor const*, Component* const*&)` | `0x006a2bd0` | `0xf0` | `sub_520020` | — | PS3 calls `std::_Lower_bound<Component**, ClassDescriptor const*, int, ObjectInheritanceIsAFinder>` @`0x6a2a80`; the PC wrapper above calls `sub_520020` as its only callee. Same role; not decompiled side-by-side. | medium |
| `scimitar::ComponentList::AddComponent(Component*, bool)` | `0x0069803c` | `0xb0` | *unconfirmed* | — | PS3 body: `addi r28,r3,0xc`, then `bl 0x4e95a0` = `BigArray<…>::Add(…)`. This is the append+grow path. CLAIMS C1 cites PC `AddComponent` @`0x00504F60` (464 B) but I did not decompile it against this PS3 body. | low |
| `scimitar::EntityBuilder::BuildEntityWithSelector(Entity*&, RowSelector&)` | `0x006aa328` | `0xd8` | `0x005FAC00` | 94 B | PC body: `*a2 = (*(vtbl+76))(this,a3); (*(vtbl+64))(this,a2,a3);` — an out-param `Entity*&` filled by a virtual "get/select" then a virtual "apply", with `RowSelector&` threaded through. PS3 does the same two-phase build with `RowSelector` helper calls (`0x699c30`, `0x69de48`) and array frees. Signature match is exact. | medium-high |
| `scimitar::Entity::SetGlobalMatrixInternal(Gear::Matrix44<float> const&, bool)` | `0x006a5248` | `0x200` | `0x00528200` | 139 B | PS3 copies a `Matrix44<float>` with AltiVec `lvx/stvx` in `+0x10` strides (`addi r9,r3,0x10`, `lvx v0,0,r4`, `stvx v0,0,r28`). PC `0x528200` is the Entity override located independently by the existing `SgmDetour` hook (`RE-AC4/address-unit-audit.md:125`, `hook-gating-and-identity-audit.md:47`). Same 4×4 matrix copy-out. | **high** |
| `scimitar::AbstractEntityAI::GetComponentContainer()` | `0x0039877c` | `0xc` | *unconfirmed* | — | Body is `return this + 0x90`. **This corrects T20**: PC `sub_6F5F70` (`*(*(entity+0xE8)+0x1C)`) is *not* this function. | high (PS3) |

## 2. RESOLVED — the count offset differs between builds; the PC side is self-consistent

The PS3 build and the PC build do **not** lay out the component list the same way. Both are internally
consistent, so nothing is broken — but offsets must never be ported across.

**PC is proven both directions** (Hex-Rays, this session):

`Entity`-component add, `0x00504F60`:
```
v6 = *(_DWORD *)(this + 100) & 0x3FFF;                     // +0x64 dword, low 14 bits = CAPACITY
if ( (u16)*(this + 102) + 1 > v6 )                         // +0x66 u16 = COUNT
    sub_A09AB0(this + 96, v7, 4, ...);                     // +0x60 = array base, grow reallocs it
v8 = *(_DWORD *)(this + 96);
v9 = (u16)*(this + 102) + 1;
*(_WORD *)(this + 102) = v9;                               // COUNT incremented at +0x66
```
`Entity::GetComponentByType`, `0x005200C0`:
```
if ( a2 == *(_DWORD *)(this + 96) + 4 * *(u16 *)(this + 102) ) return 0;
```
Writer and reader agree: **array `+0x60`, capacity `+0x64` (dword `& 0x3FFF`), count `+0x66` (u16)**.
So **C1 stands for the PC build, and the mod's `+0x66` is correct — do not change it.**

**PS3 reads a different offset** (capstone, two independent functions):
* `0x006a3e68`: `addi r29,r29,0x60` / `lhz r0,4(r29)` / `lwz r9,0(r29)` / `slwi r0,r0,2`
  → end = `base + 4 * (u16 at entity+0x64)`.
* `0x006a2bd0`: `lwzu r0,0x60(r31)` … `lhz r4,4(r31)` … `slwi r4,r4,2; add r4,r4,r0` — same.

**Rule for this project: any offset read out of the PS3 ELF is a hypothesis for the PC build until it is
re-confirmed on the PC side.** The two builds differ at least here. (PS3 gold vs PC 1.05 are different
revisions; this is the first proven instance, not necessarily the only one.)

## 3. Unconfirmed — PS3 side known, PC side not found

| PS3 symbol | PS3 addr | size | PS3 shape (actual disasm) | PC status |
|---|---|---|---|---|
| `CRLDetect::InDetect_DetectionProcessRoot_PlayerPerceptionMonitor::OnEnter()` | `0x016044cc` | `0x30` | `lis r9,0x29b; lwz r3,0xc(r3); lwz r4,-0x23d4(r9); li r5,0; bl 0xc0a8dc` — i.e. `activate(*(this+0xC), &global_0x29ADC2C, 0)` | **no PC address.** No PC string contains `PlayerPerceptionMonitor`; the class is a statechart node with no distinguishing constant, so shape-matching alone is unsafe. |
| `CLConflict::CanDoConflict_PlayerEnemyHandler_IsEnemyOfThePlayer::OnEnter()` | `0x0167b224` | `0xb0` | `lwz r28,0xc(r3)`; `lwz r31,0x60(r28)`; `lwz r0,0x5c(r31)`; `lwz r30,0xad0(r3)` — walks `this+0xC → +0x60 → +0x5C` and touches `+0xAD0` | **no PC address.** Only two PC functions read a `+0xAD0` displacement: `0x16CE1C0` (299 B) and `0x19AD670` (569 B). Neither decompiles as this logic. Candidates, not matches. |
| `scimitar::Faction::IsIn(Faction*, Array<Handle<Faction>,…>&)` | `0x01d28f44` | `0x5c` | `bl 0xbcfa00` (handle deref) then `SmallArrayData::Exist(...)`, returns bool | **no PC address.** |
| `scimitar::Faction::AddToAllies(Handle<Faction>)` | `0x01d28e18` | `0x88` | not disassembled | **no PC address.** |
| faction getter chain step 1 — PC `sub_6F5F70` (`*(*(entity+0xE8)+0x1C)`) | **unknown** | — | byte pattern `80 63 00 E8 80 63 00 1C 4E 80 00 20` (`lwz r3,0xE8(r3); lwz r3,0x1C(r3); blr`) does **not** occur anywhere in the ELF, so the PC body has null checks the PS3 one does not, or the sequence differs | **no PS3 name yet** |
| faction getter chain step 2 — PC `sub_133A4C0` (`*(char+0xA4)`, name at `faction+8`) | **unknown** | — | — | **no PS3 name yet** |

## 4. Next probes (mechanical, cheap)

1. **Faction chain naming.** Scan the `.map` for functions of size `≤ 0x40`, disassemble, and keep those whose
   body is `lwz rX,0xE8(rY)` followed by `lwz rX,0x1C(rX)` (allowing intervening `cmpwi/beq`). That directly
   names PC `sub_6F5F70`. Then find the PS3 counterpart of `sub_133A4C0` by scanning for
   `lwz rX,0xA4(rY)` inside a function that also calls the handle-deref `0xbcfa00`.
2. **Statechart nodes.** `PlayerPerceptionMonitor::OnEnter` / `IsEnemyOfThePlayer::OnEnter` are found through
   the statechart's class registration, not by string. Resolve the PS3 vtable that holds `0x016044cc`
   (search `.data` for the dword `0x016044cc`), take the sibling slots (`OnExit`, `OnEmbbdedActivated`), and
   match that *vtable shape* to the PC side — this is the only reliable matcher for these two.
3. **C1 re-check.** Dump one PC entity `+0x60..+0x68` (dword at +0x60, u16 at +0x64, u16 at +0x66) next to
   the live component count. If `+0x64` equals the count, the mod's `+0x66` read is the bug that has been
   chasing this project.

## 5. Tooling notes for the next seat

* `ops\tools\ps3_query.py` cannot dump `Entity`/`ComponentList`: line 74 does `hex()` on a
  `ListContainer` and raises `TypeError: 'ListContainer' object cannot be interpreted as an integer`.
  `struct ComponentList` also prints nothing. Fix or replace before trusting it.
* DWARF parsing of the 553 MB ELF (`cu count 348`) does not finish in 15 min for a whole-file walk; scope any
  future DWARF query to a single CU or pre-index it once to a TSV.
* `pyelftools` and `capstone` are on **system `py`**, not on the IDA venv python.
