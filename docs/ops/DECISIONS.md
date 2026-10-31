# Decision log

Append-only. A decision is `proposed` until ORCH marks it `accepted`. Anything that
changes a gate, a protocol or a public claim needs a row here **before** the code.

---

## D-001 — Replicate outcomes, not worlds — **accepted**

*Context:* the engine gives each machine its own world; there is no shared
simulation. Four years of modding this title have not produced one.
*Decision:* both machines simulate; only identity, state and outcomes cross the
wire.
*Consequences:* no shared physics, no second simulated entity stream for ambient
NPCs, no lockstep. Every capability must be expressible as an event or a pose.

## D-002 — Per-system authority, not host-authoritative — **accepted**

*Context:* `COOP-ROADMAP.md` says host-authoritative; `COOP-MASTER-PLAN.md` says
the opposite, with a reason (a host with a divergent world is wrong).
*Decision:* avatar/ship pose = owner; damage/death = causer; alert = observer;
objectives = grow-only union.
*Consequences:* no single point of failure, but conflict rules must be explicit
per system. `COOP-ROADMAP.md`'s line is superseded by this row.

## D-003 — Objectives are a grow-only set — **accepted**

*Context:* an objective can only go false→true within a session; both players can
complete it in the same second.
*Decision:* `(missionId, objectiveId)` union, idempotent, no delete.
*Consequences:* no rollback of progress; missions that can *un*-complete something
need an explicit second event kind.

## D-004 — Ships can only sink if hull HP is found in 3 sessions — **open**

*Context:* `[[ship+0x5C]+0x84]` regenerates and is not health; `ShipHealthData`
exists as a class.
*Decision needed:* after E4.1, either implement sinking or ship boarding-only.
*Owner:* NAVAL → ORCH.

## D-005 — Fix the crash class, not the crash site — **accepted**

*Context:* the six-site guard left 464 sites of the same shape unguarded and a
seventh site crashed within a day.
*Decision:* one guard at the dereference helper / a null-safe wrapper for
`sub_733FB0`; no new per-site patches without a `DECIDE`.
*Consequences:* the patch table shrinks; a mis-designed wrapper can hide real
faults, so it must log every null it substitutes.

## D-006 — Save files are never transferred — **accepted**

*Context:* saves are account-bound and encrypted.
*Decision:* local saves only; progress syncs as events.
*Consequences:* divergence is possible and must be detected at join and refused
with a clear message.

## D-007 — Kit config: `ReloadBuilder` and `IdResolver` — **open**

*Context:* the 2 PC kit sets `ReloadBuilder=0`, `IdResolver=0`, while the code
defaults to 1 and the code comment says the builder is released after spawn and
must be reloaded.
*Decision needed:* run both settings once on one machine, keep the one that
spawns the puppet reliably, and document the reason.
*Owner:* NET → ORCH.

## D-008 — C3 kill switch — **open**

*Context:* perception is the only genuinely uncertain route in the plan.
*Decision needed:* confirm that two failed perception experiments end C3 and the
product ships C0–C2 + naval. Written here so it cannot be re-litigated later.
*Owner:* human + ORCH.

## D-009 — Combat fallback if no apply path exists — **open**

*Context:* `DamageEvent` may have no callable apply path; the health-diff route
(`SetLife` at `0x19FB4C0`, storage `+0x5C`) is callable but coarse.
*Decision needed:* at the end of stage 2A, choose record/replay vs health-diff,
and delete the other from the docs.
*Owner:* COMBAT → ORCH.

## D-011 — Same-PC two-instance as gate evidence — **open (revised)**

*Context:* a second PC/copy is not available. The shipped code already implements a
same-PC second instance (`net.cpp:335-344`: bind `LocalPort`, else bind `PeerPort`
and set `g_secondInstance`), and the only single-instance guard is one
`CreateMutexW` in `_WinMain@16` (`0x406D25`), which the plugin already hooks. Neither
path has ever been exercised in a log.
*Decision needed:* if `experiments/E0-A-two-instance.md` passes, accept two
same-PC instances as sufficient evidence for M1–M5 **functional** gates, requiring
two distinct pids, two different bound ports, both `rx>0`, both naming the same
build hash. The "non-loopback peer" check becomes a separate transport test, not a
co-op gate.
*Owner:* human + ORCH. Run E0-A first; this decision should be made from its logs.


## D-012 — Hand the plugin tree to the NET seat — **accepted**

*Context:* two agents writing `ref/AC4Coop/src/` will lose work.
*Decision:* NET is the only writer. The existing session is told to stop writing
`src/`, reply with its branch, and from then on receives `TASK`/`REVIEW` messages
like any other seat.
*Consequences:* the existing session becomes a reviewer/helper until ORCH assigns
it a seat.

## D-013 — Inter-session transport is the server API, not a plugin — **accepted**

*Context:* `opencode-tell-sessions` fails to load (missing `./v1` export on its
`@opencode-ai/plugin` peer). `@dataforxyz/agent-intercom-opencode` installs but the
loader rejects it: "Plugin must export a default definition" — the package's default
is a V1 factory while this host wants a V2 object and resolves a `/server`
entrypoint first. Three restarts produced no tools.
*Decision:* `ops/tools/dm.py` talks to `POST /api/session/<id>/prompt` on the local
server (Basic auth from `service.json`). Verified: a send returns 200 and becomes a
real user turn. No restart, no plugin, auditable.
*Consequences:* the DM is a prompt, so the receiving session actually works;
the bus file remains the durable record. Revisit only if a future host loads
intercom natively.

## D-014 — An unrun build is never deployed — **accepted**

*Context:* `78e128c9` contains the first real fix for the `0x681817` crash and has
never executed.
*Decision:* a build goes to the game folder only after one run in which it does not
fault; until then `bin/` may be ahead of the game folder and the audit will say so
(A-04 red is the correct state).
*Consequences:* provenance is temporarily "HEAD is ahead of deployed", which the
audit already reports rather than hides.

## D-015 — 0.3 is an observer + a scanner, not a smaller table — **accepted**

*Context:* 0.3 was written as "collapse the six-site guard to one root-cause guard
so the patch table shrinks". NET correctly refused to guess: neither a logging
detour (a) nor wrapping every `E8 sub_733FB0` call site (b) shrinks the table.
*Measurement (ORCH, `ops/tools/null_join_scan.py`, 2026-10-29):* the anti-pattern
`test reg,reg / jz L / L dereferences reg` occurs **146 times** in 27,494,912 bytes
of `.text`. The known crash `0x681801` is in the list; `0x6797DC` is not (it is a
different class — a stale node pointer, not a null return). 146 is small enough to
triage with data and far too large to patch by hand.
*Decision:* 0.3 delivers (a) — a **read-only observer on `sub_733FB0`** that logs
every NULL return with the caller's return address (`[esp]` at the callee's `ret`),
capped, no behaviour change — plus the scanner as a permanent tool. The six site
guards and the `0x6817FA` caller wrap stay. No seventh site, no wrapping of every
call site.
*The invariant replaces the old acceptance:* **every NULL return that reaches a
deref must be either observed with a return address or patched.** After one live
run, the log histogram selects which of the 146 candidates are real, and only those
get patched. Shrinking the table was a means, not the goal; it is withdrawn as a
criterion.
*Consequences:* the table does not shrink this week. In exchange, the next crash of
this class is classified in minutes instead of after a session of RE.

## D-016 — `SetLife(hp, -1)` is the kill; the forged-state write is retired — **accepted**

*Context:* `COMBAT` designed three backends (`B_FILTER`/`B_REACT`/`B_SETLIFE`) and treated
`B_SETLIFE` as the fallback. A live run on 2026-10-29 (`pid 12372`, build `5e0f7110`)
showed the opposite: going through the engine setter produces a **real** death.

```
[KF] before: alive=1  health life=36/36 flags=00000004
[KF] SetLife(-1) rc=0  life 36 -> 65535
[KF] is-alive service=… +4 00000000 -> 00001000
[KF] after : alive=0 health life=65535 flags=01000004
[KF] requested death clip alias=1818493434
[KF] collision layer -> Disabled(0x29)
```

Six kills, all `rc=0`, and the operator reports the deaths look correct on screen —
versus `CombatTest`'s direct `Life=0xFFFF + flags` write, which the same operator
reported as "not a single guard died". `sub_19FB4C0` with a negative argument stores
−1 and takes its own death branch; that branch is the transition the forged write skips.
*Decision:* `B_SETLIFE` is the primary apply path for C2. The forged-state write is
kept only as an A/B control in `CombatTest` and must not be used to produce a death.
`B_FILTER`/`B_REACT` remain research, not blockers: the game can already kill.
*Consequences:* stage 2A no longer waits on finding a callable apply path. What remains
is identity (see D-017) and two-machine delivery.

## D-017 — Identity is empty because the entity lane was off — **accepted**

*Context:* the same run logged `[Kill] objId=0 unknown -> position fallback` on every
kill, and `[Relay] mirror … objId=0`, with 2 of 6 events unresolved ("no soldier within
4 m"). `ObjIdForEntity()` reads `s_objMap`, which is only filled by `ObjMapAdd` when an
entity packet **arrives**; `EntitySyncTick()` returns immediately when `[Coop] EntitySync=0`,
so nothing was ever sent and the map stayed empty. The live ini had `EntitySync=0`.
*Decision:* the identity experiment is not a new feature — it is turning the entity lane
on and watching whether `objId` becomes non-zero and stable. `EntitySync=1` +
`EntityHost=1` is the next run.
*Consequences:* if ids appear and match on push and apply, the one-machine C2 loop is
complete and only the second machine is missing. If they appear but drift, WP4's alias
layer is justified. If they stay zero, the map's refresh policy is the bug.

## D-018 — Second instance is a marker, not a bind failure — **accepted**

*Context:* `net.cpp:342` infers "I am the second instance" from `bind(LocalPort)` failing.
Measured on this machine (`ops/tools/reuseaddr_test.py`): two UDP sockets both with
`SO_REUSEADDR` **both bind successfully**, and the **first** binder keeps receiving —
the second gets nothing. So the fallback can never fire, `g_secondInstance` is always
false, and two instances would both bind `LocalPort` and both send to a port nobody
owns.
*Decision:* our own marker decides the slot. On attach:
`CreateMutexW(NULL, FALSE, L"AC4Coop_InstanceSlot_v1")` and
`g_isSecondInstance = (GetLastError() == ERROR_ALREADY_EXISTS)`. `Net::Init` uses that
flag first (instance 2 binds `PeerPort`, sends to `LocalPort`), keeps the bind-failure
as a secondary signal, and logs `[Net] instance slot=<1|2> bind=<port> peer=<port>`.
`SO_EXCLUSIVEADDRUSE` is **not** adopted: `SO_REUSEADDR` is what lets the live game
survive a zombie holding the port.
*Consequences:* the two-instance test becomes deterministic and readable from one log
line. This also explains why no previous run ever produced two sockets.

## D-019 — A synthetic peer replaces the second instance — **accepted**

*Context:* E0-A assumed two game instances on one PC. Steam/Uplay will not launch the
same title twice (the project recorded this on 2026-10-17: "Steam blocks a same-PC
second instance"; the plugin's `CreateMutexW` hook only defeats the *game's* guard, not
the store's). So the two-instance route is not available without a second machine.
*Decision:* the second endpoint is a **script in Python**, not a second game. One game
instance binds `LocalPort` and sends to `PeerIP:PeerPort`; the synthetic peer binds
`PeerPort` and sends to `LocalPort`. That is a real two-process, two-socket session with
distinct ports, no Steam, no second copy.
*What it proves:* the wire format, the socket path in both directions, `ApplyPeerHealth`
and the kill lane from a foreign sender, the entity/id lane, and the instance-slot logic.
*What it cannot prove:* that a puppet looks right, that two humans enjoy it, or anything
requiring a second rendered world. Those stay gated on a second machine.
*Consequences:* E0-A is retired; E0-B (synthetic peer) is the transport gate. D-018 stays
— the marker is still the correct mechanism for a VM or a second PC later.

## D-010 — `py -3` is fine, `python` is not — **accepted**

*Context:* the audit rig initially assumed the `py` launcher was broken. It is not;
plain `python` is the Store stub.
*Decision:* advertise `audit/tools/run_audit.ps1` only. Any doc that says
`python …` is wrong.

---

## 2026-10-29 — live two-machine session: enemy-only scope, and the real root causes

**Product decision (user):** stop syncing ambient NPCs. Sync **enemies only**, and drive
them from the host so they are exact. Implemented as `[Coop] SyncEnemiesOnly` (default 1) using
the predicate the guard-enumeration probe already proved: `WeaponComponent 0x027106A8` or
`MapIconBehaviorGuard 0x0269E9D0`. Civilians are no longer captured or sent.

**Root causes proven from the two live logs (not inferred):**
1. The guest had no death detection at all — the death watch lives inside `CaptureGuards`,
   which is only called under `if (g_entHost)`.
2. The one-shot death packet was overwritten by the next snapshot in `RecvThread`'s single
   last-writer-wins slot, so the host logged a successful push and the guest never saw it.
3. The guest objId table (256, append-only) saturated and then silently refused every new id.
4. The vanish-lane death carried position (0,0,0), so the receiver's 4 m fallback searched the
   world origin.
5. Ship mirroring matched by naval-manager **index**, which is unrelated across machines; the
   guest's own Jackdaw was a legal target — that is what drowned his ship and crew.
6. Guest kill on an already-dead body crashed at `0x0067B2EA` (`this->[0x114]` null in
   `sub_67B2A0`) — the same signature as the retired `0x6797DC` crash.
7. After the crash fix, the mirror tick re-enabled the behaviour on the next snapshot, so the
   corpse played its death clip and then carried on living. Fixed with an 8-tick corpse latch.

**Standing rule restated:** an inference is not a finding. Three of today's seven causes were
found only by reading the peer's log after a failed hypothesis (paths, permissions, UDP loss)
had already cost hours. Keep collecting both logs before theorising.

## D-2026-10-30 — reports are files, builds are serialised, facts are re-checked

*Context:* four separate losses in one day — NET's T2, both IMPL reports, and RE's `+0x660` answer
were all delivered in a seat session and never reached the orchestrator; NET's tested binary was
destroyed by another seat's build; and three "proven" facts (component count, the class
discriminator, `+0x660`) did not survive contact with the binary.

*Decisions:*
1. **Reports are files** (`ops/tools/bus.py`). A report not written to the bus does not exist.
   Reading is a pull; waking is a best-effort trigger on a new file, never on "quiet".
2. **One build, from a clean tree** (Rule 10). Seats commit only their own file and report a commit
   hash; the orchestrator builds once and deploys from `dist/builds/`.
3. **Every inherited fact is re-checked before it is used.** `ops/CLAIMS.md` records the evidence
   and the independent check; `VERIFIED-SOURCE` is weaker than `VERIFIED-LIVE` and must say which.

*Consequences:* the hive is slower to start a task and much harder to fool. The next session can
pick up from `ops/STATE.json` + `ops/JOURNAL-2026-10-30.md` without replaying any of it.
