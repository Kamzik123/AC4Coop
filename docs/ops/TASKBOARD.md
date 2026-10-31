# TASKBOARD

Owner column = the seat that does the work. Only one seat owns a file at a time.

| # | task | owner | state | build / evidence | note |
|---|---|---|---|---|---|
| T1 | fix `+0x66` component-count bug everywhere it is read | IMPL | **CLOSED — not a bug** | C1 re-proven | RE re-verified both directions on PC 2026-10-30: writer `0x504F60` increments u16 `+0x66`, reads capacity `*(u32*)(+0x64)&0x3FFF`; reader `0x5200C0` uses `+0x66`. `+0x66` is correct, no change |
| T2 | net audit findings: heap overflow, stale `g_haveEntities`, kill-ring overflow, drain order | NET | **done (code)** | `2D8E2E4DC499` | drain-order one-liner handed to IMPL (phase1.cpp:4280); see ops/evidence/T2-net-bounds-2026-10-30.md |
| T3 | real enemy discriminator using the corrected component walk | RE | **superseded by T13/C21** | C21 | `+0x9C` kind/role, no component walk needed |
| T4 | independent verification of C3/C4 (net) and C5/C6 (death lane) | QA | **queued** | — | adversarial: assume the author is wrong |
| T5 | G10 stealth experiment design: do host guards see the guest body? | AI | **done** | `ops/AI-T5-G10.md` | puppet is a `BhvAssassin` copy of the host player, no perception registration | needed |
| T6 | log oracle: `ops/tools/coop_report.py` → G6/G7/G9 pass-fail table from a phase1 log | QA | **queued** | — | parser only, no game needed |
| T7 | deploy + two-machine run of the slot-identity build | IMPL → orchestrator | blocked | `25A601E5AB8A` | needs the tester online |

## Done this session

| # | task | owner | build |
|---|---|---|---|
| D1 | puppet pure-position mirror | IMPL | `5BC501A06101` |
| D2 | corpse latch | IMPL | `236822D0A860` |
| D3 | slot identity + proximity scope | IMPL | `25A601E5AB8A` |
| D4 | component-model RE (+0x66) | RE | — |
| D5 | entity factory RE (no archetype factory) | RE | — |
| D6 | net receive-path audit | NET | – |
| D7 | net recv hardening: encoder bounds, 3 s snapshot TTL, kill-drop log, heartbeat liveness | NET | `2D8E2E4DC499` |

| T8 | run the G10 experiment on the host | AI + orchestrator | queued | ops/AI-T5-G10.md run sheet | needs the game running |
| T9 | guard the +0x660 FocusFlags write (pointer corruption risk) | IMPL | **moot** | C20 | the store was removed in REL-9db4f744ae80; `+0x660` is never bit-tested by the engine |

| T10 | [G10] stage instrumentation (StageLog flag) | IMPL (from AI spec) | queued | ops/AI-T5-G10.md sect.7 | makes guard detection a single grep |

| T11 | release build from clean tree | orchestrator | **done** | REL-200d214bd3c4 / 7614bb7 | deployed + kit; verifies 4 changes + strings |
| T12 | decide +0x660 pointer-vs-flags | RE | **done** | C20 | neither: no bit op on `[reg+660h]` anywhere; float/opaque dword where it exists |

| T6 | log oracle coop_report.py | QA | **delivered, validated** | ops/tools/coop_report.py 33.8 KB | runs on a real log; FAILs are correct for the old build |

| T13 | soldier discriminator from entity+0x9C | IMPL | in flight | RE C21 | unblocks enemies-only |
| T14 | fix oracle death-lane join + verify C21 live | QA | in flight | ops/QA-T4-verification.md | |
| T15 | release build 12905e6 (+0x660 store removed) | orchestrator | **done** | REL-9db4f744ae80 | 303,104 B |

| T13 | soldier discriminator (entity+0x9C) | IMPL | **done** | commit b221394 | SyncEnemiesOnly now real |
| T15 | build+deploy REL-280d652169d0 | orchestrator | **done** | 303,104 B @ b221394 | kit rebuilt, ini restored to two-machine |
| T16 | two-machine run | both humans | **UNBLOCKED — second copy installing** | kit `dist\2pc-kit-2026-10-31` = **REL-fbd38635fc81** (commit `3c80c85`, clean) | run sheet `ops/RUN-2026-10-31-two-machine.md`; run in order A→B→C→D→E, C decides whether co-op stealth exists |

| T17 | per-body [KR] line so C21 is checkable | IMPL | in flight | ops/QA-T14-C21-probe.md | aggregate is self-referential |
| T18 | score a live kindrole/KR log | QA | blocked | needs the two-machine run | |

| T17 | per-body [KR] probe | IMPL | **done** | commit ed332a8 / REL-80f9a9a7e588 | KrLog=1 set in the game ini |
| T19 | build+deploy REL-80f9a9a7e588 | orchestrator | **done** | 303,616 B @ ed332a8 | kit rebuilt |

| T20 | full hostile set (roles) + faction field | RE | **done** | bus `20261030T140936Z_RE_12608` | hostility = kind + faction, never role. Ship a faction test |

| T21 | component-count offset across builds | RE | **done** | `ops/RE-PS3-MAP.md` §2 | PC self-consistent at `+0x66`; PS3 reads `+0x64`. Not a bug — a build difference. **Never port PS3 offsets to PC** |
| T22 | PS3→PC symbol map for co-op functions | RE | **done** | `ops/RE-PS3-MAP.md` | 7 rows resolved (Faction ctor, GetComponentByType, BuildEntityWithSelector, SetGlobalMatrixInternal high); 5 unconfirmed — see §3 |
| T23 | ship the faction test (replace `kind==3&&role==1`) | IMPL | **queued** | C21 + T20 | read `Faction*` via `sub_6F5F70` -> `sub_133A4C0` -> name at `+8`; gate on kind==3 first |
| T24 | name the PS3 twin of `sub_6F5F70` / `sub_133A4C0` | RE | **queued** | RE-PS3-MAP §4 | `lwz rX,0xE8(r3)` fingerprint found **zero** hits — PC body has null checks PS3 lacks; scan `.map` size<=0x40 instead |
