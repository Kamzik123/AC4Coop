# Morning test — 2026-10-30

Build under test: **`BA88854C8731`** (293,888 B). Kit zip rebuilt with the same file.
Both machines must run this hash — the guest report must show
`[Build] asi=ba88854c8731…` as line 1.

## What changed overnight (and why)

| # | bug | evidence | fix |
|---|---|---|---|
| 1 | scene-change invalidation fired ~40–120×/s | 3467 `[Net] scene change` lines in 40 s, alternating `peerKey=000F6C26`/`FFFFFFFF` | compare only when a packet actually arrived; validity flags instead of the `0xFFFFFFFF` sentinel |
| 2 | guest had **no death detection at all** | `CaptureGuards` is only called under `if (g_entHost)` (phase1.cpp:3415) | new `GuestDeathScan` on the guest tick: `[GKT] guest death detected/push` |
| 3 | one-shot death packet overwritten before the frame thread read it | host logged `death push`, guest log had no `[Kill] peer DEATH` | kill entries copied to a 32-slot ring in `net.cpp` (`g_killRing`), drained each frame; snapshot `es.dead` is now the primary route |
| 4 | guest objId table saturated at 256 and then silently refused | `nobod=645 steal=211` per 5 s window on the guest | evict stale/LRU entries, rebind via the engine id resolver on a miss |
| 5 | vanish-lane deaths carried position `(0,0,0)` | receiver's 4 m fallback searched the world origin | `PushHostDeath` now sends the victim's last known position |
| 6 | guest killed an already-dead body → crash `0x0067B2EA` | `[KF] SetLife(-1) rc=-1 life 65535 -> 65535` | skip if `life==0xFFFF \|\| deadFlag`; re-enable behaviour before the kill |
| 7 | corpse stood back up (death clip, still alive) | mirror tick re-enabled behaviour on the next snapshot for the same body | 8-tick corpse latch: re-disable behaviour, clear drive, stop driving locally-dead bodies |
| 8 | ship mirror matched by naval-manager **index**, unrelated across machines | `fillShipState(..., i)` → `GetShipEntity(mgr, es.key)` | ships carry a real `objId`; the local Jackdaw can never be a target; `ShipSync=0` kill switch |
| 9 | native civilians flooded the mirror | `ent=4700` per window | `[Coop] SyncEnemiesOnly` (default 1): only entities with `WeaponComponent 0x027106A8` or `MapIconBehaviorGuard 0x0269E9D0` are captured |

## The crash, RE'd (not guessed)

Windows Application log: `AC4BFSP.exe`, `0xC0000005`, offset `0x0027B2EA`, imagebase `0x00400000`
→ `0x0067B2EA`, inside `sub_67B2A0`:

```
0x67B2E1  mov ecx, [esi+114h]
0x67B2EA  mov edx, [ecx]      ; AV — this->[0x114] is NULL
0x67B2F1  call edx
```

Same signature as the old `0x6797DC` crash. Trigger: killing an already-dead mirror body.

## How to run it

1. **Both**: replace `AC4Coop.asi` with `BA88854C8731`, keep the ini.
2. Host ini: `EntityHost=1`, `EntitySync=1`, `SyncEnemiesOnly=1`, `ShipSync=1`, `GuestKillMirror=1`.
   Guest ini: `EntityHost=0`, everything else identical.
3. Launch, both in-world, **focused** (alt-tab pauses the world and freezes what you send).
4. Walk together through a street with guards. Do **not** touch ships yet.

## What to read, in this order

**Host log (`phase1_log_<pid>.txt`):**

```
[Sync] scope candidates=N enemies=M sent=K (SyncEnemiesOnly=1)
```
`M` should be a small number (guards), not thousands. `K` is what actually goes on the wire.
If `sent` is still in the thousands, the filter is not working — stop and report.

```
[Net] stats tx=… ent=K
```
`ent=` should drop from ~4700 to roughly the enemy count + ships. This is the bandwidth win.

**Guest log:**

```
[Build] asi=ba88854c8731…            ← must match the host
[Net] stats … mirror ok=… nobod=… steal=…
```
`nobod` should collapse now that only enemies are sent. `steal` should fall too.

## The three tests

| # | do | pass signal |
|---|---|---|
| 1 | host kills a guard the guest can see | guest: `[Mirror] snapshot death objId=… applied=1` (or `[Kill] peer DEATH` → `[KT] death recv … applied=1`); guard settles as a corpse, does not stand back up |
| 2 | guest kills a guard the host can see | guest: `[GKT] guest death detected` + `[GKT] guest death push`; host: `[Kill] peer DEATH` → `[KT] death recv … applied=1` |
| 3 | both walk past the same guards | guards are in the same place, same pose, on both screens; no flicker, no teleporting |

If a death does not land, the log now names the gate:
- `[KT] death push SKIPPED … reason=…` — host side
- `[GKT] push SKIPPED … reason=…` — guest side
- `[Mirror] dead … skipped reason=already-dead|no-behaviour-component` — guest applied nothing
- `[KH] unresolved` — the guest could not resolve the id

## Escape hatches (no rebuild needed)

| ini key | effect |
|---|---|
| `GuestKillMirror=0` | disables the guest-side kill entirely (use if it crashes again) |
| `SyncEnemiesOnly=0` | restores the old "every NPC" scope |
| `ShipSync=0` | disables the ship lane |
| `KillTrace=0` | silences `[KT]` diagnostics |

## Not done yet

- no spawn-based mirroring: an enemy the host has and the guest does not is still dropped (`nobod`)
- no civilian sync at all (deliberate, per decision)
- nothing verified in-game overnight — every claim above is from the build, the source, or the two logs already collected
