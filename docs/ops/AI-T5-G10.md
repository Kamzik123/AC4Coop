# AI / T5 — G10: do the HOST's guards perceive the GUEST's body?

Status: experiment **designed, not run**. Everything below cites code; nothing is claimed as
observed in game. Unproven items are labelled **[unproven]**.

Gate text: `ops/PLAN-2026-10-30-enemy-sync.md:48` —
"the host's guards detect the *guest's* body when it is in line of sight (this is the gate that
decides whether co-op stealth is possible at all)".

---

## 1. What the host actually spawns — entity class and components

`Phase1::TrySpawnPuppet()` `ref/AC4Coop/src/phase1.cpp:465`.

It does **not** spawn from an archetype, template hash or class string (all absent). It **builds a
copy of the host player**:

| step | code |
|---|---|
| take the local player's cached selector + builder | `477-478` `GetCachedSelector()` / `GetCachedBuilderHandle()` |
| duplicate the selector | `515` `selector->Duplicate(0,0)` inside `DuplicationManager::Begin(1,0)` (`510-517`) |
| build | `523` `BuildEntityWithSelector(&newEntity, dupSel)` |
| add to world | `553` `world->AddEntity(newEntity)` |
| place | `559` `SetPosition(player->GetPosition() + g_offset)` (`g_offset = {2,0,0,0}`, `:101`) |
| suppress player enrollment | `552` `g_blockReg = newEntity` |

So the puppet **is a second copy of the player character**: `Human` + `BhvAssassin`
(vtable `0x026FA898`, asserted at `:1917`) — **not** `BhvGenericNPC` (`0x026E34D8`, the guard class,
used as an exclusion filter at `:3083`, `:3224`, `:4077`).

Components/state it provably carries:

- **`Human` component** — the code asks `AIEntityHelper::GetHuman(e)` for arbitrary entities
  (`:2958`) and drives the puppet's `m_GroundData` fields directly (`:1293-1314`).
- **Behaviour component (`BhvAssassin`)** — present, and **enabled on the ground**:
  `:1239-1244` keeps the puppet's FSM running on the GROUND context (hybrid mode); it is only
  `SetEnabled(false)` off-ground (`:1251-1259`).
- **A pad slot** — `BhvAssassin::AddToWorldInternal` hands the puppet an *empty* pad via the
  `GetUnusedPadID` detour (`:8238-8241`). It is a pad-owning entity, but not the player
  (`g_blockReg`, `:550-552`).
- **Valid transform + graphics/skeleton** — `SetGlobalMatrixInternal(&m, phys)` `:1224`.
- **Collision filter forced to `Disabled(0x29)`** — `:1279` `0x636480(&cc->m_FilterInfo, 0x29)`,
  one-shot at `:1175`.
- **Character controller told not to drive** — `cc->m_DriveEntity = false` `:1273`.
- Human context FSM (`TransitToContext`, `:1316-1341`), action aliases (`:1413+`), CAS copy (`:1396`).

**Absent on the puppet (verified absent, not assumed):** any perception/awareness/sight/suspicion
registration, any faction/team/relationship setter, any invulnerability or invisibility flag, any
health/death lane (`es.hp`/death handling touches guards only, `:2929-2946`), any possession or
player-controller grant.

`FocusFlags` probe at `:1926` (`*(uint32_t*)(p+0x660) |= 0x51`) is the one "make me targetable"
attempt — but `ops/PROGRAM-PLAN.md:316` records that route as **disproved**: `+0x660` is a pointer,
not a flag field. The write is therefore suspect **[unproven whether it corrupts something]**.

---

## 2. Which component would a guard's awareness need — and is it on the puppet?

Honest answer: **the component is not located.** `ops/PROGRAM-PLAN.md:294` says exactly that
("a per-entity detection component not yet located"), and `:321-328` makes locating the
**perception candidate list** experiment E3.2 (2-4 sessions, kill criterion at 4). What is known:

- Guard alert predicates exist but read a **global** detection node, not a per-guard field:
  `PROGRAM-PLAN.md:297-301` — `arg1+0x0C` / bitset `+0x40` / u16 `+0x146`, `arg1` is the global
  node `dword_4E1982C`; the entity-side storage is unmapped.
- `entity+0x5C` → World, `World+0x8E0` = **the player's** alert actor only
  (`PROGRAM-PLAN.md:284-288`). Useless for "did a guard see the puppet".
- The AI object exists per entity (`GetEntityAI(e) = *(*(e+0xE8)+0x1C)`, `+0xB8`), `PROGRAM-PLAN.md:292`.

Requirement for G10 to pass, in component terms: the guard must be able to (a) find the puppet as a
**candidate**, (b) pass its **visibility/LOS** test, (c) pass its **faction/hostility** test.
The puppet satisfies (c) trivially only if guards treat `BhvAssassin` as an enemy — and the player
class *is* what guards aggro on, so that is the one part that is plausible **[unproven]**.
(a) is unproven. (b) is the concrete risk:

> **Prime suspect:** the collision filter is forced to `Disabled(0x29)` on the puppet (`:1279`).
> If the guard's sight test is a physics/collision query, a body in group `0x29` may be skipped.
> **[unproven — highest-value single-variable test]**

Second suspect: with `DisablePuppetAI=1` (ini default, `:8378`) the behaviour component is disabled
whenever the puppet is off the GROUND context (`:1251-1259`); an AI that only considers entities with
a live behaviour component would lose it there. **[unproven]**

**Expectation before the test [unproven, do not quote as a result]:** the puppet is a genuine
`Human` entity in the host world with a live `BhvAssassin` FSM and a pad, so proximity awareness is
plausible; nothing in the code registers it for perception, and `0x29` is a plausible silent
exclusion.

---

## 3. G10 experiment — 3 minutes, 7 steps

Oracle: **`[Fight]` snapshot on F4** (hotkey `VK_F4` → `Phase1::RequestEntityEnum`,
`dllmain.cpp:402` → `ProbeEntityEnum`, `phase1.cpp:4337`), which prints every entity whose
`HumanGroundStage >= 4` (`:4485-4487`, `:4504`). Stage enum is real:
`ref/AC4Coop/src/Scimitar/AI/Human.h:294-330` — `1=Navigation`, `2=Hurt`, `4=Fight`,
`0xC=RangeCombat`, `0x11=FleeFight`. **A guard that only *notices* (investigate) may stay at
stage 1 — that is a known false-negative of this oracle and is why step 6 is a mandatory negative
control.**

Setup (host only, one machine):
`[Net] Enabled=1 LocalPort=47801 PeerPort=47802 PeerIP=127.0.0.1`;
`[Coop] EntitySync=1 EntityHost=1 SyncEnemiesOnly=1 PuppetParkX=0 PuppetPhysics=0 DisablePuppetAI=1`.
Build under test per `ops/STATE.json` (deployed `51A70717F838`).

1. **t=0** Launch, load a save, get in-world and **stand still 3 m from an idle soldier**, weapon
   sheathed, no other target in view. Press **F4**. Expect `[Fight] fighters=0`.
   (Baseline: if this is not 0, the scene is already in combat — reload.)
2. **t=20 s** Start the synthetic peer so the guest body appears and circles the player:
   `python ops/tools/peer9.py --walk --echo-pos --port 47802 --to 127.0.0.1:47801`
   (`ops/tools/peer9.py:415-431, 546-558`; world/scene are learned from the game's own packets,
   `:485-486`).
3. **t=20-35 s** Confirm the body exists and where it is — this is the geometry proof, not a guess:
   `[Net] peer in world (id=…) -> spawning puppet (try 1)` `phase1.cpp:7959`
   `[Net] spawn succeeded, g_puppet=0x…` `:7970`
   `[Net] apply seq=N pos=(x,y,z) spd=.. dst=.. ctx=.. bundle=..` `:1561` (printed every 30th
   apply, position read back from the puppet's own matrix).
4. **t=35 s** Press **F4** while the guard is looking at the ghost.
   **DETECTION:** `[Fight] fighters>=1` and a row `[Fight] [0]=0x… stage=4 nComp=…` (or stage 0xC/0x11)
   for a guard within ~5 m of the `pos=` in step 3.
   **NO DETECTION:** `[Fight] fighters=0` and no row with `stage>=4`, while `[Net] apply` shows the
   ghost standing in the open in front of the guard.
5. **t=50 s Attribution** Read the last `pos=(x,y,z)` from `[Net] apply`, Ctrl+C the peer, relaunch
   `peer9.py --pos x,y,z` (same numbers) so the ghost freezes there, then walk the host player
   **30 m away behind cover**. If `fighters>=1` persists with the ghost and not the player, the
   trigger is the ghost. (No world/scene args needed — `:485-486` echoes them.)
6. **t=90 s Negative control** Ctrl+C the peer and wait for `[Net] puppet despawned (0x…)`
   `:1588` (or `[Net] puppet entity gone -> drop ref` `:7926`). Same player position, same guards,
   press **F4** → `[Fight] fighters=0`. If step 4 was positive and this is not 0, the "detection"
   was another fight in the scene and the run is void.
7. **t=150 s** Save both logs to `ops/evidence/G10-<date>/` with the verdict written as the exact
   lines from steps 3-6. Gate passes on 3 consecutive exposed runs with a clean control.

If **step 4 is negative but the ghost visibly draws guards** (they turn, walk over, investigate,
never reach `stage>=4`), the oracle is the limit, not the game: record it as "perceived, not yet
quantified" and ask IMPL for **one line** — sample `es.stage` per guard at 2 Hz
(`es.stage` is already captured at `:2961`, just unlogged both on the wire log and the `[Arch]` line,
which de-duplicates per process at `:3093-3103`).

---

## 4. Fallback if guards cannot see the ghost

Ordered by cost, cheapest first. Each is a hypothesis, not a plan of record.

1. **Collision filter variant** (`PuppetCollision=1`, i.e. skip the `0x29` write at `:1279`). One
   ini key + one `if`, one build, one 3-minute run. Kills the "physics sight test skips group 0x29"
   hypothesis first because it is the only place the mod actively removes the body from the world's
   physical model.
2. **`DisablePuppetAI=0`** — keep the behaviour component enabled everywhere so the puppet is never
   a body with a dead FSM. No code change, ini only. Tests "perception requires a live behaviour
   component".
3. **Engine spawn path instead of `BuildEntityWithSelector`** — spawn the ghost through
   `SpawningRequest::Spawn` (the path the `MySpawnRequest` hook already intercepts at `:8305-8308`,
   RVA `0x39B8D0`) so it enters the world the way the engine registers spawned characters, rather
   than by duplicate-then-`AddEntity` (`:523`, `:553`). **[unproven: whether `AddEntity` vs the spawn
   request differ in any perception registration]**
4. **E3.2 — find the perception candidate list** and register the puppet in it
   (`ops/PROGRAM-PLAN.md:321-328`). 2-4 sessions; kill criterion at 4 sessions.
5. **Stimulus injection** (`PROGRAM-PLAN.md:329-333`, labelled `[H]`): compute cone/range/LOS on the
   host and drive the guard's own transition. Consistent with "replicate a stimulus, not a
   behaviour", but it is a client-side illusion, needs a per-guard `ctx` route that is itself
   unverified (`PROGRAM-PLAN.md:282`), and `+0x146` may be recomputed every perception tick
   (`:311`).
6. **Cut G10** — per `ops/DECISIONS.md:66-71` (D-008): two failed perception experiments end C3 and
   the product ships C0-C2 + naval. G10 failing twice is that trigger.

---

## 5. Claim to record after the run

Proposed ledger entry (orchestrator to add to `ops/CLAIMS.md`):

> **C16** — The host's guards do/don't perceive the guest puppet; the puppet is a copy of the host
> player (`Human`/`BhvAssassin`), added to the world with a pad slot, collision filter
> `Disabled(0x29)`, and no perception registration — `phase1.cpp:465-580, 1279, 8238-8241`.
> Evidence: `ops/evidence/G10-<date>/`. Status after run: VERIFIED-LIVE or DISPROVED.

Also touches **C15** (`ops/CLAIMS.md:33`): an enemy discriminator is still unavailable, so G10 must
be judged on guard *behaviour*, not on a class filter.

---

## 6. Run sheet — G10 (follow it literally; no interpretation needed)

You need: the game, one PowerShell window, this repo. Total 3 minutes after the world loads.
Every "PASS"/"FAIL" below is a decision you can make by looking at the output of one command.

### 6.0 Before the clock — edit the ini (2 min, once)

File: `<game folder>\AC4Coop.ini`. Edit **only** these keys. The plugin reads them at load, so edit
before launching the game.

```ini
[Net]
Enabled=1
LocalPort=47801
PeerPort=47802
PeerIP=127.0.0.1

[Coop]
EntitySync=1
EntityHost=1
SyncEnemiesOnly=1
PuppetParkX=0
PuppetPhysics=0
DisablePuppetAI=1
```

Then launch the game, load a save, get in-world. **Leave the game focused** while running steps 3-6
(alt-tab pauses the world). Do not draw a weapon, do not attack anyone, do not touch the ship.

### 6.1 In PowerShell — one window, run once

```powershell
cd 'C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop'
$game = '<game folder>'          # e.g. 'C:\Program Files (x86)\Steam\steamapps\common\Assassin''s Creed IV Black Flag'
$log  = (Get-ChildItem "$game\phase1_log_*.txt" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
$log
```
PASS: `$log` prints a path with today's date. FAIL: no file → the plugin did not load; stop here.

### 6.2 The eleven checks, in order

**Step 1 — baseline (t = 0:00)**
1. In game: walk to an **idle soldier** and stand still **3 m** from him, nothing else around.
2. Press **F4** once.
3. Run:
   ```powershell
   Select-String -Path $log -Pattern '\[Fight\] fighters=' | Select-Object -Last 1
   ```
   PASS = `[Fight] fighters=0`. FAIL = `fighters=1` or more → you are already in a fight; reload and
   redo. **A FAIL here voids the whole run** (there is no clean baseline).

**Step 2 — socket up (t = 0:20)**
1. In the PowerShell window, start the synthetic peer and leave it running:
   ```powershell
   python ops/tools/peer9.py --walk --echo-pos --port 47802 --to 127.0.0.1:47801
   ```
2. It prints `[peer9] bound 0.0.0.0:47802 -> 127.0.0.1:47801  proto=9 ...` within 1 s.
3. Run:
   ```powershell
   Select-String -Path $log -Pattern '\[Net\] cfg|\[Net\] stats' | Select-Object -Last 2
   ```
   PASS = the `cfg` line contains `peer=127.0.0.1` and the `stats` line contains a non-zero `rx=`.
   FAIL = `peer=` is anything else, or `rx=0` → the packet is not arriving (wrong port / firewall);
   fix and restart from step 1.

**Step 3 — the body exists and is where you think (t = 0:20-0:35)**
1. Look at the screen: a second character ("ghost") should be walking a small (~3 m) circle around
   **you**, not around the guard. If you see no ghost, go to FAIL below.
2. Run:
   ```powershell
   Select-String -Path $log -Pattern '\[Net\] peer in world|\[Net\] spawn succeeded|\[Net\] apply seq=' | Select-Object -Last 6
   ```
   PASS = you see `spawn succeeded` and **at least two** `apply seq=` lines whose `pos=(...)` values
   differ from each other. (Two different positions = the body is really moving in the world.)
   FAIL = no `apply seq=` lines → the puppet never received a state; the peer is not reaching the
   apply gate (world/scene mismatch or wrong port). Record the lines and stop.

**Step 4 — the actual G10 question (t = 0:35)**
1. Move **yourself** (the player) so the ghost's circle passes **within ~3 m in front of the idle
   soldier** and you are standing 3-5 m behind/next to him. Still no weapon, no attacks by you.
2. Wait 10 s while the ghost circles through his view.
3. Press **F4** once.
4. Run:
   ```powershell
   Select-String -Path $log -Pattern '\[Fight\]' | Select-Object -Last 6
   ```
   **PASS (detection) = a line `[Fight] fighters=1` (or more) plus a row
   `[Fight] [0]=0x........ stage=4 nComp=...`** — i.e. a guard reached `stage=4` (`Fight`) or
   `stage=12` (`RangeCombat`) / `17` (`FleeFight`) while the ghost was in front of him and you were
   not attacking.
   **FAIL (no detection) = `[Fight] fighters=0`** and no `stage>=4` row.
   `fighters>=1` caused by a fight on the other side of the street = **INVALID**; move to a quieter
   spot and repeat step 4 (do not count it).

**Step 5 — attribution: is it the ghost or you? (t = 0:50)**
1. Copy the **last** `pos=(x,y,z)` numbers from the `apply seq=` line you already have.
2. In the peer window press **Ctrl+C**, then **within 3 s** start the frozen peer with those exact
   numbers (same window is fine):
   ```powershell
   python ops/tools/peer9.py --pos <x>,<y>,<z> --port 47802 --to 127.0.0.1:47801
   ```
   (No `--world`/`--scene` needed; the peer copies them from the game's own packets.)
3. Walk **your player 30 m away**, behind a wall. Do not despawn the ghost.
4. Run:
   ```powershell
   Select-String -Path $log -Pattern '\[Net\] apply seq=' | Select-Object -Last 1
   Select-String -Path $log -Pattern '\[Fight\] fighters=' | Select-Object -Last 1
   ```
   PASS = the `apply seq=` `pos=` equals the numbers you passed, and the last `fighters=` is still
   `>=1` with the guard at the ghost. That makes the ghost the trigger, not you.
   FAIL = `fighters=0` after you walked away → the earlier aggro was yours. Step 4 then answers
   nothing; re-run with `DisablePuppetAI=0` before concluding.
   If the ghost despawned instead (`[Net] puppet despawned`), the peer was started >3 s late; wait
   for `spawn succeeded`, then redo step 5.

**Step 6 — negative control (t = 1:30)**
1. In the peer window press **Ctrl+C**.
2. Run until it prints, then stop the game input (stand still):
   ```powershell
   Select-String -Path $log -Pattern '\[Net\] puppet despawned|\[Net\] puppet entity gone' | Select-Object -Last 1
   ```
   PASS = one of those two lines appears within ~5 s.
3. Press **F4** once, then:
   ```powershell
   Select-String -Path $log -Pattern '\[Fight\] fighters=' | Select-Object -Last 1
   ```
   PASS = `fighters=0`. FAIL = `>=1` → a fight was already running in the scene; step 4's PASS was
   not caused by the ghost and the run is void (reload, quieter location, redo from step 1).

**Step 7 — file the result (t = 1:50)**
```powershell
New-Item -ItemType Directory -Force "ops\evidence\G10-$(Get-Date -f yyyy-MM-dd)" | Out-Null
Copy-Item $log "ops\evidence\G10-$(Get-Date -f yyyy-MM-dd)\"
Select-String -Path $log -Pattern '\[Fight\]|\[Net\] (apply seq|spawn succeeded|puppet despawned|peer in world)' |
  ForEach-Object { $_.Line } | Set-Content "ops\evidence\G10-$(Get-Date -f yyyy-MM-dd)\verdict-lines.txt"
```
PASS = the folder holds the full log and `verdict-lines.txt`.

### 6.3 Decision (no judgement needed)

| result of the run | verdict | next |
|---|---|---|
| step 1 `0`, step 4 `fighters>=1`, step 6 `0`, step 5 PASS | **G10 GREEN** | record C16 as VERIFIED-LIVE; stealth is possible |
| step 4 `fighters=0` on **3 clean runs** (steps 1, 3, 6 all PASS) | **G10 RED** | fallback rung 1: collision filter, then rung 2 (`DisablePuppetAI=0`) — §4 |
| step 4 shows guards turning / walking over / investigating but `fighters=0` | **G10 AMBER, "perceived, not quantified"** | ask IMPL for the one-line per-guard `es.stage` 2 Hz sample (`es.stage` already captured, `phase1.cpp:2961`) |
| step 1 not `0`, or step 6 not `0`, or step 5 FAIL | **VOID** | move to a street with a single idle soldier and rerun; do not count it |

---

## 7. T5b — instrumentation request for IMPL (diff-spec, no code)

One log line per guard, showing `es.stage` before it is thrown away. Today the value is read at
`phase1.cpp:2961` (`es.stage = …m_HumanGroundStage`) and only ever leaves the process inside the
wire packet (`netstate.h` `EntityState::stage`); nothing on the host reads it back, so G10's
"the guard switched to combat" is invisible in the log.

**Wanted:** a `[G10]` line, host only, that makes G10 decidable from the log with no human judgement.

### 7.1 Exact line

```
[G10] key=%u objId=%llu arch=%04X stage=%d was=%d aggro=%d alias=%08X dPeer=%6.1f dPlayer=%6.1f pos=(%.1f,%.1f,%.1f)\n
```

| field | source |
|---|---|
| `key` | `es.key` (`:2915`), the slot index — lets you follow one guard across lines |
| `objId` | `es.objId` (`:2916`) — the only identity that survives respawn-free |
| `arch` | `es.arch` (`:2919`, from `bhv+0x14`) |
| `stage` | `es.stage` as filled at `:2961` |
| `was` | the previous `stage` for this `key` (static table, §7.3) |
| `aggro` | `1` if `stage == 4 (Fight) || stage == 0xC (RangeCombat) || stage == 0x11 (FleeFight)`, else `0` |
| `alias` | `es.actionAlias` (`:2967`) — second term, proves an attack clip is playing |
| `dPeer` | **3-D Euclidean** distance from the guard's `es.pos[3]` to the ghost: `g_peerPos` (`:1192-1196`) with `g_puppetParkX` (`:102`) added on X; `-1.0` if `g_peerPosValid == 0` |
| `dPlayer` | 3-D Euclidean distance from the guard to `world->GetPlayer()->GetPosition()`, read **once per `CaptureGuards` call**, not per guard |
| `pos` | `es.pos[3]` |

Axis note: use full 3-D distance, **not** the `sqrt(dx*dx+dz*dz)+|dy|` form of `NoteAppliedPos`
(`:6673`) — the up-axis is ambiguous between that code and `peer9.py:537`, and 3-D removes the
question at 5 m scale.

### 7.2 Where it is emitted

- **Site:** end of `EmitGuardEntity` (`:2904`), immediately after the stage/alias fill block ends
  (`:2968`), before the function returns. `es` is fully populated there and `gh`/`gpB` have already
  been range-checked.
- **Caller path:** `CaptureGuards` (`:3040`) ← `EntitySyncTick` (`:4206`) ← `:7811`, i.e. every
  player frame while `Net::Enabled()`. **Host only** — `CaptureGuards` is not reached on the guest
  (C5, `ops/CLAIMS.md:23`), so there is no double logging.
- **No new hook, no new detour, no threading.** Statics + `L()` only, same as every other probe line.
- **Must not touch:** `es`/`pkt`, the wire format, `NET_PROTO_VER`, the `[Arch]` dedup statics
  (`:3093-3103`), or anything on the apply/mirror path.
- `L()` is already used inside `__try` on this path; keep the line inside the same guard as `:3123`
  so a bad `pos` read cannot escape.

### 7.3 Rate cap

- Per guard, keyed by `es.key` (table of `MAX_SYNC_ENTITIES == 128`, `netstate.h:176`; entries
  `{ lastStage, lastAlias, lastMs }`): emit when **`stage != lastStage`** (immediate), **or**
  `alias` changed while `aggro == 1`, **or** heartbeat `lastMs + 1000 <= nowMs`.
- Global cap: **10 lines/s** across all guards; when the cap is hit, keep the on-change lines and
  drop heartbeats (i.e. test the global budget only on the heartbeat branch).
- `nowMs`: one `GetTickCount()` per `CaptureGuards` call, passed down.
- Expected volume in G10: 1-3 guards near the ghost → 2-4 lines/s, so 3 minutes ≈ 500 lines.

### 7.4 Ini gate

`[Coop] StageLog=0` (default **0**, production unchanged), read once where the other `[Coop]` keys
are read (`:8378-8382` block). The G10 run sheet sets `StageLog=1`.

### 7.5 What proves what

- **Detection (the gate's question):** a line with `aggro=1` and `dPeer < 5.0`.
- **The ghost, not the player, caused it:** the same line with `dPlayer > 25.0`. No manual 30 m walk
  needed — the line carries both distances.
- **Calm:** `stage=1 aggro=0` for every guard with `dPeer < 5.0` while the ghost stands in front of
  him → non-detection.
- **Not proven by this line, ever:** *noticing* (a guard that investigates without engaging stays at
  `stage=1`; the alert level lives in the unmapped per-guard AI state, `PROGRAM-PLAN.md:282-301`).
  A run where all guards stay `stage=1` but visibly converge is **AMBER**, not RED — the line cannot
  see suspicion, only commitment.
- `stage=2 (Hurt)` / `3 (Unbalance)` mean the *guard* is being hit, not that he sees anyone — never
  count those as detection.

### 7.6 Acceptance (IMPL self-check before handing over)

| # | condition | expected |
|---|---|---|
| 1 | `StageLog=0` | zero `[G10]` lines, `[Sync]`/`[Arch]` output byte-identical to before |
| 2 | ghost absent, `StageLog=1` | `[G10]` lines with `dPeer=-1.0`, `aggro=0` |
| 3 | guard fighting the **player** at 30 m from the ghost | `aggro=1 dPlayer<3 dPeer>25` |
| 4 | guard fighting the **ghost**, player 30 m away | `aggro=1 dPeer<5 dPlayer>25` |
| 5 | 3 min run | ≤ 10 lines/s, no crash, log < 2 MB |

Grep for the run sheet: `Select-String -Path $log -Pattern '\[G10\]'`. G10 verdict from the log alone:
`any line with aggro=1 and dPeer<5 and dPlayer>25` → GREEN.
