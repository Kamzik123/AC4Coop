# Claims ledger

Every technical claim that a decision rests on. A claim is only **VERIFIED** when an
independent check exists — not because an agent asserted it.

Status values:
- `VERIFIED-LIVE` — seen in a game log on a real run
- `VERIFIED-BINARY` — read from the disassembly/decompilation, address cited
- `VERIFIED-SOURCE` — read in our own source, file:line cited
- `UNVERIFIED` — asserted, not yet checked independently
- `DISPROVEN` — checked and false

| # | claim | asserted by | evidence | status | independent check |
|---|---|---|---|---|---|
| C1 | Entity component **count** is at `+0x66`; the count is authoritative, `+0x64` is `uint16 cap|flags` (cap = v & 0x3FFF, **bit15 = buffer not heap-owned**) | RE component array | `GetComponentByType @0x5200C0`, `AddComponent @0x504F60`, container template `sub_A09AB0` (`& 0xFFFF3FFF` proves u16+flags), `ComponentCache @0x4375D0`, 4 more consumers | **VERIFIED-BINARY (2 independent)** | V1 `ses_e74290e3…` confirmed; note: a 32-bit write to +0x64 would destroy the count |
| C16 | The component array base at `+0x60` is **reallocated** on grow (old block freed) | V1 | `sub_A09AB0 @0xa09af6/0xa09b1a` | VERIFIED-BINARY | — |
| C17 | `AddComponent` appends at the tail **without re-sorting**, while readers `lower_bound` on (desc+0x2C, desc+0x2E) | V1 | `0x504fff` vs `0x52003d` | VERIFIED-BINARY | — |
| C18 | entity+0xF0 is an 8-bit generation stamp for component index caches; wraps at 256 (ABA) | V1 | writer `0x527000/0x52700a`, consumer `0x4375e2` | VERIFIED-BINARY | — |
| C19 | The `+0x64 & 0xFFFF` over-read can walk up to 0xBFFF entries when bit15 is set | V1 | mask semantics above | VERIFIED-BINARY | — |
| C2 | There is no archetype-id entity factory; creation is `RowSelector`-driven | RE factory | `0x5FAC00` → `0x5FD520` → `0x79BC50` | VERIFIED-BINARY | pending V2 |
| C3 | `EncodeEntityPacket` can overflow `g_txBuf` (header-only bounds check) | net audit | net.cpp:178-191 | VERIFIED-SOURCE | pending V2 |
| C4 | `g_haveEntities` is never cleared → stale snapshot re-applied | net audit | net.cpp:23, 352, 487-495 | VERIFIED-SOURCE | pending V2 |
| C5 | A guest has no death detection (`CaptureGuards` is host-only) | death-lane patch | phase1.cpp:3415 host branch | VERIFIED-SOURCE | pending V3 |
| C6 | One-shot death packet was overwritten by the next snapshot in `RecvThread` | id-table patch | single last-writer-wins slot | VERIFIED-SOURCE | pending V3 |
| C7 | Guest kill on an already-dead body crashed at `0x67B2EA` (`this+0x114` null) | crash RE | Windows log offset + `sub_67B2A0` disasm | VERIFIED-BINARY + VERIFIED-LIVE | (accepted) |
| C8 | Every human NPC shares one AI vtable (`0x026D3CD8`) | histogram build | `[Sync] ai … distinct=1` on a live run | VERIFIED-LIVE | (accepted) |
| C9 | `0x027106A8` / `0x0269E9D0` are real vtables but never attached to enumerated NPCs | histogram build | live log, 3 locations, `weapon_A8=0 icon_D0=0` | VERIFIED-LIVE | (accepted) |
| C10 | The puppet was physics-warped 20×/s | product decision | `SetGlobalMatrixInternal(&m, true)` at :1204 | VERIFIED-SOURCE | (accepted) |
| C11 | Puppet position-mirror mode is smoother/cheaper | implementation agent | `[Pup] mode=position`, `avgD=0.06`, 33 kB/s | VERIFIED-LIVE | (accepted, solo peer) |
| C12 | The guest objId table saturated at 256 and silently refused new ids | id-table patch | `GuestObjAdd` early return | VERIFIED-SOURCE | pending V3 |
| C13 | Ship mirror matched by naval-manager index (not portable) | ship patch | `fillShipState(..., i)` / `GetShipEntity(mgr, es.key)` | VERIFIED-SOURCE | (accepted) |
| C14 | Slot identity + proximity scope works | slot patch | `[Sync] bind sent/bound/unmatched/meanErr` | **UNVERIFIED** — never run in game | first run |
| C15 | Enemy-only classification is impossible without a dedicated RE pass | A1 result | C8 + C9 + C1 (walk was broken, so re-test needed now) | **UNVERIFIED after C1** — the walk was wrong when it was measured | re-run after C1 fix |

## Standing rules

1. No claim enters the plan as fact while `UNVERIFIED`.
2. A claim made by an implementation agent is checked by a *different* agent that did not write it.
3. `VERIFIED-SOURCE` is weaker than `VERIFIED-LIVE` — say which one.
4. A hypothesis is labelled, never smuggled in as a conclusion.

| C20 | `+0x660` is NOT a flags word; the engine never bit-tests it; the puppet write corrupts adjacent data | RE T12 | VisualTargetingComponent record @0x27E3F58 size 0xA0; +0x660 is 0x5C0 past it; zero RMW/test forms on [reg+660h] in the image | **VERIFIED-BINARY** | RE (one pass, exhaustive byte scan) |

| C21 | Soldier/civilian is `entity+0x9C`: packed u32, kind=(v&7), role=((v>>3)&0x3F); IsSoldier = kind==3 && role==1, IsCivilian = (2,2) | RE T3 | `sub_73B4B0` guard, `sub_54EC00` civilian, name table `sub_1406C70` (1=Guard,2=Civilian,3=Thief,5=Mercenary,19=Animal,23=Naval) | **VERIFIED-BINARY** | needs one live dump (QA T14) |
| C3' | net packet overflow was **latent, not live**: worst case 20+128*58 = 7444 B vs 16384 B buffer; break-even 282 entities | QA T4 | `net.cpp:184-193`, `:99` static array | **DISPROVEN as stated** | owner agrees |
| C4' | `g_haveEntities` IS cleared — 3 s TTL (`net.cpp:25,528-531`); residual: returns before the TTL branch when `!g_enabled` | QA T4 | as cited | **DISPROVEN as stated** | residual noted |
| C5' | `CaptureGuards` is host-only, but the guest has `GuestDeathScan` (`:4054`, called `:4328`) | QA T4 | as cited | **DISPROVEN as stated** | |
| C6' | kill-only packets are kept out of the snapshot slot and go through the 32-slot ring; residual: overflow logged only to OutputDebugString, so invisible to the log oracle | QA T4 | `net.cpp:37,359-398` | **PARTLY DISPROVEN** | |

| C22 | `[Sync] kindrole soldier/civilian/other` proves C21 | — | — | **REFUTED by QA T14** | the split is counted by the predicate under test, so it cannot disagree with itself; it proves liveness + non-degeneracy only. Needs a per-body `[KR]` line (IMPL T17) |
| C23 | `coop_report.py` death checks are lane-correct | QA T14(1) | `kt_push_ids`/`gkt_push_ids`/`kt_recv_ids` joined per lane; 18/18 selftest incl. paired regressions | **VERIFIED** (self-test; no live log yet) |

| C23 | `coop_report.py` scores the [KR] rows correctly | QA T14/T18 | 20/20 selftest; synthetic log: packing PASS (3 rows), vs-components PASS (2 humans), kind=-1 its own FAIL, comps= optional | **VERIFIED** (synthetic only; no live log yet) | |

| C24 | 'enemy' = kind==3 && role==1 | IMPL T13 | shipped filter | **INCOMPLETE** - covers Guards only; Thief(3)/Mercenary(5)/variant bits 9-14/faction not modelled. RE T20 open |
