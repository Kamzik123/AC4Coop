# AC4 Black Flag co-op — STATUS (single source of truth)

_Last verified: 2026-10-27 (current state below; older entries are history)._

## 2026-10-27 — evidence build deployed; crash root-caused; runtime fleet answers

**Solo oracle PASS — 2026-10-27 17:07:31–17:10:37, pid 13860, build `F565E804` (251,392 B).**
Gate `verdict=KNOWN` (size 45,056,040, sha256 `bc11ad79…`, base 0x400000) · `[SIG]` **3/3** (`sub_679750`,
`sub_681980`, `sub_679220`) · `[DG] 6 null-deref sites guarded, cave=0x078E0000` · heartbeat ticks
200/400/600/800 at 10 s spacing · in-world 3 min 6 s, `rx == tx` (350 pkt, 189,633 B), 141 `[Net] apply`,
82 `[Net] action ->`, no crash, no dump, clean exit. Smoothing: `drop=0 wipe=0 clamp=0 ext=0`, avgD
0.15→0.02; `maxD` 5.02 peak in 2 of 24 windows — not extrapolation (`ext=0`), candidate is the H1
transform fight or a legitimate jump/fall.

Full record, including what is implemented vs only proposed: **`SESSION-2026-10-27.md`**.

- **RE: object id — the paged-table populator is found, and the verdict is MIXED with a range discriminator.**
  `byte_4DD5FA0` is `::Engine::HandleManager::HandleContainer`: `+0x98` descriptor array (12 B: base, count
  `0x4000`, **free-list head**), `+0x9E` page count, page = `0x50000` B (`sub_A08810`). The **only** writer of
  `entry+0xC/+0x10` is `sub_A0BBF0` (`0xA0BD40`/`a0bd43`, `a0bd65`/`a0bd68`) via `sub_A0BDE0`/`sub_A0BE50`; it
  stores the caller's `u64` verbatim, and mints from `0x4DD982C` only when the caller passes 0. Authored path
  **proven**: `0x406C40`–`0x406C4E` inserts a `u64` read out of a parsed data file. Registration `sub_A1F2F0`
  never touches the table, and `sub_A1DCF0`'s CAS `obj+4 = (id_hi<<18)|(obj+4 & 0xFF03FFFF)` **preserves the
  18-bit handle**, so handle extraction stands. Counter init is `0x1E323C1 mov dword_4DD982C, 0F0000000h`
  ⇒ **minted ids live in `[0xF0000000, 0xF8000000)`, everything below came from data**. Remaining: the
  load-side re-insert, `sub_A1E8D0` record semantics, and the two-client read now needing to log
  `id >= 0xF0000000` rather than just `branch`.

- **Deployed:** 251,904 B, sha256 `AB829A9A10EA5F6FBB833BFEF06D1A31F66FBB7365C925DDA853F15B6286FBDE`,
  with `known_builds.txt` `A8D913F4E9EF517793E6082BF4665BBDE35D2531EFC41F557C57530D30347AE7`.
  **Runtime-verified: NO.** The last builds that actually ran were 249,344 B `51026EF3` (the negative and
  positive gate runs). The evidence build's heartbeat/provenance have never executed.
- **Action-lane crash, corrected by dump:** `EIP=0x0073448F`, `EDX=0xFFFFFFFF`, i.e. `[node+0x114]` pointing
  at a **null-interface target whose vtable slot holds the sentinel**. `0xFFFFFFFF` at `[obj+0]` is a
  **sentinel, not destruction** (2,456 stores enumerated, 0 inside a deleting dtor). `+0x114` is cleared only
  at `0x68571E`, in the **tail of `0x685360`, after the detach vcalls** — a window. `+0x118` is the
  **owner back-pointer** (alias `+0x1F8`), *not* a second target, and is never cleared.
  **Proposed, not implemented:** sentinel-vtable guard (null **or** first dword outside `.rdata` → skip) and
  a detach-in-progress flag.
- **T3: the tick path is now known, and it is not the registration call.** `Universe+0x18` is a **strong**
  handle table (bump `+4`); the per-frame tick reads the **weak observer list at `Universe+0x08`**
  (bump `+8`), and **registration alone ticks nothing**. The five phases (`0x657BE0/10/40/70/CA0`) are
  task **#2** (`Engine::BeginFrame`), each a nullary fan-out calling `[obj->vtable + 0x3C/40/44/48/4C]`;
  the only insertion path into `+0x08` is `0x0066F630` ← `0x006DED70` ← script binding `0x006E6C64`.
  No component is required (phases touch only `[obj]`→vptr); the sole field gate is `[obj+0x880] ∈ {0,1}`.
  **0 of 5 phases is an AI or perception update** — so this explains ticking, not guard awareness.
  A3d oracle: `a3d_verify.py` ALL CHECKS PASS.
- **Wire — two silent bugs, neither a crash.** (a) capture reads `Entity+0x40` (matrix) while the live
  transform is reported at `CC+0x1A0/+0x1C0/+0x5A0` — needs the live two-read comparison before switching;
  (b) the `+0x53D` dirty gate is unusable at a task boundary (producer and consumer are both task #8);
  latch at the setters `0x7C9CAC/0x7C809C/0x7C85C5` instead. 6 fields (~185 B of 545) can leave the wire.
- **The flicker, explained (H1).** `Entity+0x50` bit `0x20000` is a **transform-dirty flag**, not a tick
  gate. `SetGlobalMatrixInternal` writes the matrix and **sets** it; phase 2 (`0x51D2F0`, slot 0x40)
  rebuilds from the CharacterController and walks children, phase 3 (`0x51D340`, slot 0x44) rebuilds again
  and clears it. Each 10 Hz warp invites an overwrite — that is the flicker. Tick condition: the puppet is
  ticked **iff its parent is in `Universe+0x08` AND that parent's `+0x66 > 0`**. **Do not register it
  (`0x66F630`) and do not parent it to a live entity.** Invariant: `puppet+0x50 & 0x20000` re-setting
  between our writes means the engine is fighting us. Proposed **L9** (untested): write `CC+0x1A0`
  instead of the matrix, so the engine's own rebuild produces our value.
- **Rules adopted:** a vtable length is settled only by the installing dtor/ctor (zero-dword stop scored
  0/6 and produced every wrong value); never sample `+0x53D` at a task boundary; slot indices need
  installing-dtor proof per class before any call.
- **Next gate:** reboot (a PID survived `Stop-Process -Force` and held a log file), then one launch:
  heartbeat ≥2 beats inside the run window, `[SIG]` ×3, `[DG]` 6, 2.5 min in-world, no dump.
- **Puppet collision: static map done, no safe seam yet (2026-10-27).** The puppet inherits the player's
  `RigidBodyComponent` (ClassInfo `0x27A6D10`, body embedded at `comp+0xA0`, no heap pointer). Reached only via
  the owner's component map: `sub_4B7C70(owner)` → `sub_5200C0(map, &ClassInfo)` — and **player and puppet
  resolve through the same call**, so any write must key off the owner. Three candidate seams, none verified
  safe: (1) detach via `sub_43CC90` + commit `sub_5BE170`/`sub_5BB6C0`; (2) collision mode `comp+0x104` via
  `sub_636480` (modes 1/3/8, `40` = unchanged, semantics UNRESOLVED); (3) attach `CollisionPreventionModifier`,
  which is declared (`0x27B0850`) but **never instantiated anywhere** and has no factory. Refuted: `vtable+0x90`
  is a forwarder, `vtable+0x340` returns 0, `vtable+0x154` is `nullsub_1`. **Nothing shipped; a blind write on
  any of these risks a per-frame crash.** Stopgap for the weekend: increase the puppet spawn offset so it does
  not overlap the player.

## 2026-10-18 — deployed build, and the receive path is proven

- **Deployed:** `D:\...\Assassin's Creed IV Black Flag\AC4Coop.asi`, 231,936 B, sha256
  `85C3F2176B1D8342C58C6A18D5DAD131C410561811F04030312C02DA33EABCB9` (= `dist\AC4Coop-diag-2026-10-18.zip`).
  Ini: `EntitySync=1`, `EntityHost=1`, `PeerIP=26.214.206.117`, ports 47801.

> **Build-state correction (2026-10-23).** The hash above is stale. What was actually at `dist\AC4Coop.asi`
> is an ORPHAN Oct-17 staging binary: 205,312 B, sha256 `7311EAFC443051B1...`, present in no zip and in no
> log entry — do not ship it from `dist\`. The binary that was live in the game folder and in
> `ref\AC4Coop\bin\` is 234,496 B, sha256
> `5B87CCEE663D5750A55BC7D9D7DF56D8B834A662F5C02D99870D62436465BC9B` (2026-10-18 20:14:14, built from the
> then-uncommitted tree). That source state is now preserved as commit `99b331e` on
> `fix/live-game-udp-bind` (safety branch `pre-crashfix-ab7125d` = the old HEAD). Deployed `.asi` and
> deployed `.ini` are from different generations.

- **The 15:03-15:09 logs were a solo run, not a broken build.** Every stats line in all five logs read
  `rx=0`, and no puppet ever spawned; the peer machine simply was not running.
- **Receive path proven on one machine** (PeerIP=127.0.0.1 loopback): `rx` == `tx` (357 pkt), puppet spawned
  (`g_puppet=44D4F720`), per-packet `[Net] apply`, action replication, `[Relay]` and `[Cap]` both firing.
  So transport, decode, presence gate, spawn and apply all work; only a real second PC remains unproven.
- **Three diagnostics added and verified live** so a dead session explains itself in `phase1_log_<pid>.txt`:
  `[Net] cfg ...` (startup config), a one-shot `[Net] WARNING: 10 s of tx with rx=0 ...`, and
  `[Net] apply ... dst=<destSpeed> ctx=<context>`.
- **Next:** the two-machine run with this build. Pass criteria are the 5 in `README-TESTER.txt`; if it is
  silent, the new warning now names the cause instead of leaving `rx=0` unexplained.
- **Action-lane crash: root-caused and fixed in source, awaiting one in-game run.** Verified in IDA
  (2026-10-23): the 0x681AED / 0x6797DC faults are unchecked `[node+0x114]` / `[node+0x118]` derefs on a
  legal "no bound target" state. `src/action_guard.cpp` detours the state gate 0x679220 and returns
  "at rest" while either field is null (`[Coop] GuardNullActionTarget`, default 1). Build
  `417D56D5...`, 236,032 B — not deployed yet. PASS = survives >= 420 s with `[Net] action ->` > 0 and
  `[AG] null target` > 0 under the A1/A2 config.

- **2026-10-25 — id resolver + action guard, build `BF9DEEC7` (237,568 B), not deployed.** Mirrored entities
  now resolve through the engine registry (`0x00A1F160`, registry `0x004DD9824`) with the returned object
  verified against its own 64-bit id (`GetObjectID` `0x9F0B50`, EDX:EAX — the earlier "returns 32 bits" note
  was wrong), falling back to the legacy scan + 1.5 m match. `[Coop] IdResolver` (default 1). The
  `GetUnusedPadID` detour is live and required by puppet spawn (not a dead feature, not gated by a
  non-existent `SpawnBody` key); a default-1 `[Coop] LegacyPadIdHook` switch was added for diagnosis only.

_Superseded build references below (160,768 B `6F6B9F36...`, `FDF433D3...`) are 2026-10-12/13 history._

## 2026-10-13 — ship RE mapped; passenger mode is the next build

- **Ship RE is mapped** (source of truth: `SHIP-RE.md` §1-7): ship entity vtable fingerprint
  **`0x01E4A128`**; transform @ `+0x10` (rotation 3×3) / `+0x40` (position xyz, w `+0x4C` = 1.0); the
  ship is resolved from the Human via `h->m_IsOnShipEntity`.
- **Passenger mode = next build.** Concrete spec in `SHIP-COMBAT-PLAN.md` §3 **Phase S2** (no new wire
  fields — `isOnShip` already ships). Flag `ShipPassenger`, default 0.
- **One open RE item:** ship **HP** is not in the entity header — it lives in a sub-object; the
  `FUN_00fb3220` **`__thiscall`** convention fix is pending.

## 2026-10-12 — ambient-NPC-sync effort CLOSED (DROPPED by user decision)

- **CLOSED, not paused.** The ambient-NPC-sync effort is dropped (user decision, 2026-10-12). There are
  **no active NPC-sync tasks and no actionable next steps by decision** — the two ACTIVE probe sections
  below are **superseded history**; do not resume them.
- **Co-op is the shipped deliverable and is unaffected:** two players, two machines, over Radmin VPN —
  each sees the other's engine-spawned puppet animated by action replication; both keep their own ambient
  NPCs (`DisableSpawns=0`).
- **Deployed build (co-op):** `D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4Coop.asi`,
  SHA-256 `6F6B9F362E16628D216F27E8A5B1E3D2DB619A2D960DAA69E8381C0B65344899` (160,768 B). Co-op behaviour is
  unchanged by every probe build — all probe code lives on the F8 path only.
- **F8 neutralised:** `CrowdProbeIntegrate=0` in the live `AC4Coop.ini` (backup
  `bf-coop\dist\archive\AC4Coop.ini.pre-npcdrop`); one-line revert.
- **Dead artifact (never deploy):** `bf-coop\ref\AC4Coop\bin\AC4Coop.asi` SHA-256 `E2496B4F…` (spawn-table
  count diagnostic).
- **BLOCKED / OPEN (only if ever resumed):** enumerate the spawn manager's **live template table**
  properly (`[mgr+0x94]` pointer; the `u16` count at `[mgr+0x9A]` misreads 0 — expect an MSVC
  `std::vector` begin/end/capacity at `+0x94`/`+0x98`/`+0x9C`). Nothing else is open. Full detail:
  `MODLOG.md` 2026-10-12.
- **OPTIONAL follow-ups — explicitly NOT done:** (a) strip the F8 probe code for a pristine release;
  (b) a two-machine co-op re-verify after the probe builds; (c) a knowledge-base field note on the
  crowd-spawn dead ends.

## 2026-10-12 — crowd-builder probe (ACTIVE) [SUPERSEDED — see the CLOSED entry above]

- **ACTIVE:** the body-skinning / crowd-builder GO/NO-GO probe. Step-1 **typing bug fixed and deployed**:
  build SHA-256 `31BB7309A9E425656984A93A7648CD05527A39C9AE0AD92CC61BA61AD7EE1E39` (157,696 B) at
  `D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4Coop.asi`
  (**F8 → `Phase1::TryCrowdProbe`**).
- **BLOCKED on:** the re-probe — the user must **relaunch `AC4BFSP.exe`** (the deployed `.asi` was locked
  by the running game, pid 13104, so it had to be stopped to swap the file; not a code defect) and
  **press F8 in-world**; then read `D:\…\phase1_log_<pid>.txt` **live** (logger now uses
  `_fsopen(..., "w", _SH_DENYWR)`, no quit needed).
- **GATE (GO/NO-GO):** decider = the `cachedBuilder = … builderFamily = ?` line **plus** any
  `SWEEP … FAMILY` line. Family found → **GO** → start `AMBIENT-NPC-SYNC-PLAN.md` **Phase 1** (host
  enumeration + relevance window). Sweep empty **and** cached builder not in family → **NO-GO** → fall
  back to the (now dropped) hijack route; artifacts are archived at
  `bf-coop\archive\hijack-ghost-v1` and that route is not an option (dropped 2026-10-18).
- **The earlier NO-GO was a false negative** — step 1 dereferenced a SmartPtr/handle as a raw pointer
  (`world+0xA48`/`+0xA44`), yielding the status word as a bogus "vtable" (`vt = 00000001`). Full detail:
  `MODLOG.md` 2026-10-12 and `AMBIENT-NPC-SYNC-PLAN.md` §4.

## 2026-10-12 (re-run) — probe **GO on body creation**; crowd route still open (supersedes the gate above)

- **ACTIVE:** retarget of the crowd spawn in `Phase1::TryCrowdProbe` **step 2** — next build, **not yet
  deployed** (the user is playing; deployment requires the game closed). Its SHA-256 is **not yet reported**.
- **Probe result (build SHA-256 `31BB7309…`, pid 22784): GO on body creation.** The step-1 fix acquired a
  real EntityBuilder (`cachedBuilder = 37D6FB30`, `vt = 0x1E57790`, `builderFamily = 1`); the
  **`BuildEntityWithSelector` (vtable slot 17)** route built entity `0x48CBDFD0` (vt `0x1E4CE90`,
  children=32), issued `AddEntity`, placed it at player+g_offset (`added=1`), no crash — **the user
  visually confirmed an Edward clone/puppet appeared.** The Phase 1 gate ("can we create an engine body?")
  is **answered YES**.
- **SUPERSEDED GATE:** the old NO-GO/GO decider (cached-builder family sweep) is moot — the builder was
  acquired and a body was built. Also re-confirmed: `world+0xA48`/`+0xA44` are SmartPtr/handle values, not
  pointers; `world+0x924` = `Ai::SpawningManager` (`vt = 0x1E61278`).
- **OPEN ITEM (the blocker):** the crowd hash-spawn route. `FUN_005FD730` AVs **3/3** (`0xC0000005`) when
  called with `this` = builder; per RE its ECX is the **spawn manager** (`*(world+0x924)`), signature
  effectively `FUN_005fd730(group, hash)`. Next change: `this = spawnMgr`, try the 1-arg and the 2-arg
  `(group, hash)` forms.
- **Do NOT overstate:** the produced body is a **player (Edward) clone** (it used the player's cached
  selector), **not** a crowd NPC. Genuine crowd bodies (sailor/guard) are **not yet achieved**.
- Full detail: `MODLOG.md` 2026-10-12 (re-run); plan §4 in `AMBIENT-NPC-SYNC-PLAN.md`.

## ✅ IT WORKS — end-to-end, two machines.

**Two players, two machines, over Radmin VPN: both see each other's second player** — a genuine
**engine-spawned** body (built from the local player's own cached `EntityBuilder`) that is **animated
via action replication** — moving and synced. The partner is a real second player, not a marker, not a
raw-pose puppet.

- **The implementation that shipped is Kamzik123's AC4Coop** (`bf-coop\ref\AC4Coop`, GitHub
  `Kamzik123/AC4Coop`) — **not our own plugin.** Our plugin (`ac-rogue\AC.PatchFix`) is the historical
  line; it is **not** what produced the working result.
- **The one real bug we found and fixed** (it made the co-op silently not work):
  AC4Coop started its UDP link in `DllMain`. AC4's launch chain (Steam/Ubisoft → DRM → real game)
  starts **several processes that load the ASI**; a transient bound UDP `47801` and exited, leaving a
  **corpse socket**, so the **live game's bind failed with `10048`** → **no net at all** (the puppet
  spawned but never moved). `netstat` showed `47801` held by a PID that was **not** the live
  `AC4BFSP.exe`.
- **The fix** (in `bf-coop\ref\AC4Coop\src\`; a PR to Kamzik's repo carries it):
  1. `dllmain.cpp` — don't `Net::Init()` at attach;
  2. `phase1.cpp` — start the link on the **first in-world frame** (only the live game reaches that
     frame; the transient processes loading the ASI never do);
  3. `net.cpp` — add **`SO_REUSEADDR`** so the live game binds even if a leftover socket holds the
     port, and (being last to bind) receives the datagrams.
  **`SO_REUSEADDR` alone regresses** — the transient also binds and its corpse eats the packets — so it
  must stay **paired with the deferred init**.
- **Working build:** `AC4Coop.asi`, SHA-256
  `FDF433D3AD6DD094E1CA47556AE1E741A16F6CEF446FE048EFD98296F8F5E2C5`.
- **Kit:** `bf-coop\dist\AC4Coop-kamzik.zip` (game-root install: `AC4Coop.asi` + `AC4Coop.ini` +
  `dinput8.dll`). Two-PC net: `PeerIP` = the **other** machine, `LocalPort=PeerPort=47801`.
- **Frozen working snapshot:** `bf-coop\dist\archive\AC4Coop-kamzik-WORKING-FDF433D3\`.
- **Also proven along the way (for the record):** the Radmin link carries UDP (a direct UDP test
  delivered packets); the Windows Firewall rule for `AC4BFSP.exe` allows all profiles; ping works.

## (a) What this project is
A **two-player co-op mod for AC4 Black Flag single-player** (`AC4BFSP.exe`, x86, image base
`0x400000`, no ASLR), for a user who owns the game. Two implementations exist:

- **Kamzik123's AC4Coop** — the **reference and the one that works**. It spawns a puppet from the
  player's own cached `EntityBuilder` and **animates it by action replication** (`SetTargetAction` +
  the locomotion blend tree + the local player's `CharacterActionSet`), which is the correct approach.
- **Ours** — `ac-rogue\AC.PatchFix`, an ASI plugin. It spawns / clones / hijacks a second body and
  drives it with raw bone-matrix writes. **Not deployed; not what shipped.**

## (b) Deployed state (verified 2026-10-12)
- **Game world:** runs **mod16** — `D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\DataPC_extra_chr.forge`,
  **1,530,189,906 B = mod16 byte-exact** — plus the modP12 Havana build.
- **Active plugin:** **Kamzik's `AC4Coop.asi`** (153,600 B) in the game **ROOT**, with `AC4Coop.ini`:
  - `[Net] Enabled=1, PeerIP=<the other machine>, LocalPort=47801, PeerPort=47801`
    (the shipped kit ini reads `PeerIP=26.113.208.88`; set it to the other machine's Radmin IP)
  - `[Coop] DriveActions=1, DisablePuppetAI=1, HybridLoco=1, DisableSpawns=1`
  - `[Display] Fullscreen=0`
- **Our plugin is NOT deployed.** The game's `plugins\` folder holds only `.bak` / `.disabled`
  backups (and `.dmp` dumps) — **no active `.asi`**.

## (c) The three body approaches — historical, from *our* plugin (superseded)
The working partner does **not** use any of these; Kamzik's action replication is the route that
shipped. Kept so the dead ends are never re-chased.

| # | Approach | Source | Status | Log signature |
|---|---|---|---|---|
| 1 | **Engine spawn** | `spawn_body.cpp` (`[Coop] SpawnBody`) | Spawns a real body, but **glides** (no animation) | `SpawnBody: ... added=1 ... adopted as the driven body` then `[PoseMirror] mirrored=0 rwrites=0` |
| 2 | **Clone** | `ghost_body.cpp` `clone_body_make` (`[Coop] CloneBody`) | Source resolver broken (RVA off by `0x2000000`) and still did not resolve live | clone entry produced nothing / `via=none` |
| 3 | **Hijack** | `ghost_body.cpp` crowd picker (`[Coop] CrowdBody`) | Drives a real crowd body; with **mod16** it rendered as Edward and the 233-bone mirror wrote — the "perfect test" (2026-10-10). **DROPPED 2026-10-18** | `CrowdBody=true` + mod16 → `[PoseMirror] mirrored=<N> rwrites=<N>` (non-zero) |

**Why #1 glides (the decisive detail):** the spawned body has **no walk-child skeleton** —
`c14+0x134 == 0` — so the pose mirror's gate `if (ghost == 0 || in_skel != ghost) return;`
(`pose_mirror.cpp`) never fires. Position moves; the pose is never written.

**Related experiment — `ApiMove`:** switching the drive from a raw matrix write to the engine's
`set_world_matrix_helper` made the mirror start writing (`mirrored=6`) but made the engine re-place
the spawned body ("snaps away") → **reverted to `false`**.

## (d) Blockers — all resolved or moot
1. **Spawn glides (our plugin)** — the engine-spawned body has a null walk-child skeleton, so raw-pose
   mirroring cannot animate it. **Moot for the result:** Kamzik sidesteps this with action replication,
   and that is what shipped.
2. **Clone source resolver (our plugin)** — the player-object-array RVAs were wrong by `0x2000000`
   (corrected: array ptr `0x26C16A0`, count `0x26C16A6`); even corrected it did not resolve live.
   **Closed — our clone route.**
3. **Net/port (`47801`) — RESOLVED.** Was: Kamzik's co-op UDP `47801` socket leaked to a transient PID
   on every launch → the live game's bind failed with `10048`, no net. **Root cause:** the link was
   started in `DllMain` in a launch-chain process that then died. **Fix (shipped):** don't init at
   attach; start the link on the **first in-world frame**; add **`SO_REUSEADDR`** (paired with the
   deferred init). Robust for any PID.

## (e) Artifact map
`bf-coop\dist\` holds the kits.
- **Current / working = `dist\AC4Coop-kamzik.zip`** (Kamzik's `.asi` — SHA `FDF433D3…` — + an `.ini` +
  `dinput8.dll` + `README-INSTALL.txt`). Live folder: `dist\AC4Coop-kamzik\`.
- **Frozen working snapshot:** `dist\archive\AC4Coop-kamzik-WORKING-FDF433D3\`.
- **Historical (superseded):** `spawnbody-v1\` (engine-spawn evidence), `spawnfix-v1\`,
  `archive\hijack-ghost-v1\`, `AC4BF-Coop-v0.x\`.

## (f) Next
See **`bf-coop\PLAN-v2.md`** — **Phase 1 is DONE** (Kamzik's build works end-to-end). Phases 2–3
(port action replication into our plugin; explicitly retire the dead ends) are **optional future
work, not needed for the working result**.
- **Invisible NPCs (2026-10-27) — cause found and fixed.** [Coop] DisableSpawns=1 (inherited from the
  closed ambient-NPC work) made SpawningRequest::Spawn return "declined" and blocked
  CrowdHerder::UpdateSpawningNeeds, so soldier/gunner/captain bodies never spawned while map markers
  remained. DisableSpawns=0 fixes it (user-confirmed). Side effect: the entity lane then carries
  **4,693 entity updates / 60 kB/s** on loopback — use EntitySync=0 for the first two-machine run.
- **CORRECTION (a previously closed belief was partly wrong): SP does link a network transport — it just has no replication vocabulary.** Method: full-file raw byte scan of `AC4BFSP.exe` (45,056,040 B, sha256 `bc11ad79…ab5150`), Latin-1 1:1 decode, count of all occurrences — not a strings tool, so the zeros below are trustworthy.
  - `S2C_` = 0, `C2S_` = 0, `M2R_` = 0, `netproxy` = 0, `CreateReplica` = 0, `Memento` = 0, `AcquireMasterRole` = 0, `MigrateHost` = 0, `NetGroup` = 0 → **no replication RPC vocabulary in SP** (that stays verified).
  - `prudp` = **47** real hits, incl. a Ubisoft source path `F:/p4/assassin/...` and `/StationURL.cpp`, plus `prudp:/` / `prudps:/` URL schemes at `0x0222A97C` and `0x0222ACF8`.
  - `RDV` = **17** real hits: `rdv::Facade` (`0x02197C44`), `rdv::ClientSettings`, `rdv::TrackingExtensionProtocol` (`0x02197D3C`).
  - So SP **links** Ubisoft's PRUDP transport + RDV client, but not the replication layer. `AC4BFMP.exe` (30,269,344 B) has the replication vocabulary (`S2C_` ×23, `M2R_` ×27) and 92 `prudp` hits, incl. the full `prudp:/address=;port=;stream=;sid=;type=` connection string at `0x013618EC`.
  - **Unverified hypothesis:** the most likely reason SP carries PRUDP/RDV is the online-services SDK (uPlay / Kenway's Fleet / social features), not co-op replication. Confirming or killing it is cheap (xref the `rdv::` and `prudp` strings in the IDB), and it does **not** reopen R2 as a host. This records presence only — the SP transport is not claimed to be usable.
- **Entity identity, static pass (2026-10-27) — layout PINNED, verdict still a prediction.** The id container
  behind `0xA076A0` is a **two-level paged table** off global `0x4DD5FA0` (`dir = *(0x4DD5FA0+0x98)`, 12-byte
  page descriptors, `0x4000` slots/page, 16 pages): **entry stride 20 (0x14)** `a076cb lea edx,[eax+eax*4]`,
  **u64 id at entry+0xC/+0x10** `a076ce`/`a076d2`, handle in `obj+4 & 0x3FFFF` (`a076a9`). No count field —
  enumerate 16 pages, skip null descriptors. Audit doc's 20-byte/`+12` claim **holds**.
- **The mint is a counter, not a hash:** `0xA20173 lock xadd [edx],esi` on `[0x4DD9824+8]` is the only place a
  new id is born (no multiply/hash on any path). 4 of 5 writer sites classified: 1 counter, 3 copy, 0 hash.
  Writer #5 (paged `entry+0xC`) is **CLOSED** — see the newer bullet at the top of this file; the verdict is
  MIXED with the `0xF0000000` range discriminator.
- **Correction:** `0x9F0B50` **does** return a u64 (`edx:eax`); only its `int` prototype truncates — declare it
  `unsigned __int64 __fastcall(void* entity)` and it is a usable id source, no container walk needed.
- **Correction:** the `0xA076A0` gate is `obj+4 & 0x3FFFF` **and** `& 0x10000000`, and `0x10000000` is set by
  the registry itself (`0xa1f376`), so it means "id registered", **not** "authored". The doc's authored-data
  reading is unsupported.
- **Prediction (NOT verified): mixed, not uniformly "unequal".** Handle-bearing pre-registered entities read
  the paged table (may be equal across clients); handle-less lazily-minted crowd/AI get counter ids (unequal).
  Mapping layer stays the safe design, but per-branch rather than blanket.
- **Pending:** the two-client live read — log `(h, id, branch, mission, frame, save_slot)` where
  `h = *(u32*)(entity+4)`, `id = GetObjectID64(entity)`, `branch = (h&0x3FFFF && h&0x10000000) ? 2 : 4`.
  Equal `h` + unequal `id` would kill the authored hypothesis outright.
- Full write-up with every instruction cited: `RE-AC4/40-entity/objectid-anatomy.md` → `## Static pass — 2026-10-27`.
  IDB comments added at `0x9F0B50 / 0xA200D0 / 0xA076A0 / 0xA1DCF0 / 0xA1F2F0`, `idb_save` ok.
- **RE: the object id survives a property round-trip verbatim, except in one band (2026-10-27).** `0xA51910`
  is a **bidirectional** Ptr property serialiser (`[stream+6]` selects read/write; same vtable slots for both),
  not a bespoke save record. Ids round-trip unchanged and are only *resolved* on load (`a51e80 call sub_A1F160`,
  a 3-tier lookup) — never re-registered through the paged table. **But** `a51ca7`/`a51cb8` null
  `0xFFFFFFFF` (the null-id sentinel) and every id in `[0xF0000000, 0xF7FFFFFF]`. Wire ids must be
  `< 0xF0000000` or host-assigned. Whether the `.sav` stream is one of `0xA51910`'s ten callers is **unproven**.
  Corrected claim: earlier wording said "the save path serialises the id" — it is a generic serialiser, and
  save-path use is plausible but undemonstrated.
- **TransformProbe built and staged, NOT deployed (2026-10-27).** `[Coop] TransformProbe` (default `0`) logs the
  wire source and all three candidate transforms (`Entity m_GlobalMatrix`, `CC+0x1A0`, `CC+0x1C0`,
  `[[CC+0x510]]+0x00..0x3F`) every 30 frames for the local player and the puppet, with per-read bounds checks
  and `__try/__except`; nothing runs when the key is 0. Build **254,464 B**, sha256
  `8a4c191bd39a1a1d9db137ba25f63bcd791efa05e306adf89d76d9c8a7ae4bf5`, commit `2b6d91c`, oracle PASS,
  `netstate.h` unmodified, deployed `.asi` untouched at 252,928 B / `8D3B86F1…A87699`. Note: the probe reaches
  the CharacterController via the existing `Human::GetCharacterComponent()` (`0x005FC130`), so it makes one pure
  getter call per entity per 30 frames; and `CC+0x1A0/+0x1C0/+0x510` appear **nowhere else in the plugin
  source** — this probe is their first use, read-only.
- **Capture source: hypothesis FALSIFIED (2026-10-27, pid 4196).** With `TransformProbe=1` over 6,600 frames,
  `Entity`'s global matrix and `CC+0x1A0` agree to **0.0971 m mean / 0.4253 m max** (0 samples > 0.5 m) through
  walking and sailing — the wire source is **live**, not stale. `cc1A0`/`cc1C0` are adjacent vec4s of one
  structure. The `node` lane (`[[CC+0x510]]` as a matrix pointer) is **invalid** (NaN, 4/220 changes) and is
  dropped. The capture-source change is cancelled; B2's "no per-frame writer" reading does not describe what the
  capture path actually reads. **The 4.83 m `maxD` and the pauses remain unexplained** — next candidates are the
  puppet's own transform-dirty fight (H1), the 150 ms interpolation delay, and the newly reported physical
  collision between puppet and local player.
- **Damage/death traced by four parallel lanes (2026-10-28).** Death is **event-driven**, not a function: a
  `DamageEvent` goes to a per-class handler whose vtable slot 0 computes and slot 4 commits `(killed, health)`;
  `sub_133F5A0(entity)` then plays the transition. **No callable `ApplyDamage` or `Kill` exists.** Assassination
  is the *same* path with `event+0x20 == 8` (string-verified "Assassinate Low/High Profile"); firearms candidate
  `0x18` is unverified; there is **no headshot logic** anywhere. Darts are AI behaviour conditions —
  `sub_14A08B0(entity, character, "CLBerserk"|"CLSleep", 0, 1)` applies one, idempotently; sleep/berserk are
  non-lethal and reversible, poison is lethal over time through the damage path. Health `off_2712F60` has no
  TakeDamage slot. The dead bit (`0x1000`) is written by no literal immediate; candidate `0x169EB60` is
  **refuted** (`return 0`), so the writer is **UNRESOLVED** — next step is a dynamic write watchpoint on
  `health+0x5C`, not more static reading. Our old recipe is disproven; it faked the state and skipped the
  transition, which is exactly the looping-animation/no-corpse symptom, and the half-dead entity produced the
  `0x681817` crash (a seventh null-deref site). Recommended design: **record and replay the DamageEvent**.
- **Alive predicate is an OR, not an AND (2026-10-28, [V]).** `sub_133F550` returns DEAD when **any** of:
  the entity pointer is null; `(sub_7025F0(e)[+0xB8] & 0x0C) == 4`; or `(sub_10EF8C0(e)[+4] & 0x1000)`.
  Our notes had AND, which is why writing `0x1000` made the game report dead without a death — we satisfied one
  branch and the engine's real death almost certainly uses the other. Corrected in `journal/28-damage-death.md`.
- **Page-guard writer trap run (pid 5844, [M]).** Armed on `svc+4`; caught only **reads** (`val=00000000`), by
  `0x0133F586` (`sub_133F550`, bit `0x1000`) and `0x01ACE125` (`sub_1ACDFE0`, bit `0x10`); the target was never
  killed while armed. 2000 guard-page exceptions produced 43 real hits — the page is too hot, so the trap is the
  wrong instrument. **The writer of `0x1000` is still unobserved.** Next instrument: hardware breakpoint on the
  exact address across **all** threads (the earlier per-thread DR attempt never fired because the engine writes
  from job threads). Also in that run: **1397** `[Kill] event … unresolved (no soldier within 4m)` — the existing
  kill relay matches by position and essentially never resolves.