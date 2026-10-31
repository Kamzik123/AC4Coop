# E0-B runbook â€” synthetic peer (`peer9.py`)

A Python process that speaks the plugin's UDP wire protocol (`NET_PROTO_VER = 9`),
so we can exercise the co-op network path with **one** game instance.  It is a real
second endpoint for everything except rendering a world.

- Peer script: `bf-coop/ops/tools/peer9.py` (stdlib only).
- Wire authority: `ref/AC4Coop/src/netstate.h` (v9); validation: `ref/AC4Coop/src/net.cpp`.
- Sizes (derived by hand, verified against the orchestrator's independent derivation):
  `sizeof(PlayerState) = 545`, `sizeof(EntityState) = 56`, `sizeof(EntityPacket) = 7184`
  (the decoded canonical struct â€” it is *never* sent raw; on the wire it is a 20-byte
  `EntityWireHeader` + compact per-record fields, `netstate.h:151-181`).

Run `py peer9.py --selftest` (this machine uses the `py` launcher; `python`/`python3`
work where the alias is intact).  Expected:

```
SELFTEST PASS: sizeof(PlayerState)=545 (expected 545), sizeof(EntityState)=56, sizeof(EntityPacket)=7184; player round-trip + entity-wire kill round-trip OK
```

---

## 1. Game-side config â€” `AC4Coop.ini` (next to the exe)

```ini
[Net]
Enabled=1
LocalPort=47801
PeerPort=47802
PeerIP=127.0.0.1
```

`net.cpp Init()` reads exactly these keys (`net.cpp:341-346`).  With one game running it
binds `LocalPort` (47801) and sends to `PeerIP:PeerPort` (127.0.0.1:47802) â€” slot 1, not
the second-instance path (`net.cpp:368-369`).

## 2. Reading a guard's objId from the game's `[Id]` lines

The plugin writes per-entity identity lines tagged `[Id]` (also to `net_log.txt` in the
game folder, see `net.cpp:39`).  To target a specific guard:

1. Launch the game once, load a save, stand in a guard camp.
2. Open the game console log / `net_log.txt` and find the `[Id]` line for the guard you
   are looking at (the line carries the stable object id the plugin assigns).
3. Note the **decimal** objId.  It must be the *same* id the game associates with that
   guard, because the kill is matched by `objId` (the record's wire identity), not by
   list index.

> The exact `[Id]` line format was not present in the files read for this task; confirm
> the field position against the live log before trusting an automated grep.  If the
> objId is emitted in hex, prefix is irrelevant â€” `--kill` takes a decimal integer.

## 3. Commands for acceptance checks 1â€“6

All commands run from `bf-coop/ops/tools/` while the game is loaded and in-world.

| # | check | command | pass |
|---|-------|---------|------|
| 1 | game shows `[Net] up: bound:47801 -> 127.0.0.1:47802` | `py peer9.py --still --duration 5` | game log contains the `[Net] up` line (`net.cpp:396`) |
| 2 | peer receives â‰¥ 15 packets/s and decodes magic + protoVer | `py peer9.py --still --duration 10` | peer prints `RX player magic=AC4C ver=9` lines; count â‰¥ 150 over 10 s |
| 3 | game shows `[Net] apply` > 0 and the puppet spawns | `py peer9.py --walk --duration 30` | game log shows `[Net] apply`; a puppet appears and circles (radius ~30 m) |
| 4 | `KIND_KILL` for a guard's objId kills it on screen | `py peer9.py --kill <objId> --duration 10` | the guard dies; no crash |
| 5 | `--loss 0.2` â†’ no double-death, no invented death | `py peer9.py --kill <objId> --loss 0.2 --jitter 50 --duration 20` | the guard dies exactly once; no other NPC dies |
| 6 | game never rejects a peer packet (`proto` / `mask`) | any of the above runs | game log has no rejection lines |

Options: `--port` (bind, default 47802), `--to` (game addr, default `127.0.0.1:47801`),
`--walk` (scripted circle) / `--still` (default), `--loss` (0..1 drop fraction),
`--jitter` (extra ms per packet), `--duration` (seconds, 0 = until Ctrl+C),
`--kill <objId>` (emit a `KIND_KILL`, kind=3, record), `--hp` (default 0xFFFF),
`--dead` (accepted; a kill already carries `dead=1`).

The peer sends `PlayerState` at 20 Hz; with `--kill` it also sends one `EntityPacket`
per tick carrying a `KIND_KILL` record (`kind=3, idKind=0, wireId=<objId>, mask=EF_HP|EF_DEAD`).
On each received packet it prints one line with magic, protoVer, seq, size; entity records
add `kind/objId/hp/dead`.  Exit prints `sent / received / decode_failures / max_seq_gap`.

## 4. What each failure means

- **Check 1 fails (no `[Net] up` line).** The plugin did not start the link.  Most likely
  `Enabled` â‰  1 in the ini, or the ini is not next to the exe, or a stale second-instance
  mutex (`AC4Coop_InstanceSlot_v1`) made the game think it is slot 2.
- **Check 2 fails (peer sees nothing).** The game is not sending to 47802, or the peer's
  `--port`/`--to` do not match the ini (`PeerPort`/`LocalPort`).  Also: the plugin only
  starts on the first in-world frame (`net.cpp:354-357`) â€” stay in-world.  Strict-peer
  (`net.cpp:297`) drops datagrams whose source IP â‰  `PeerIP`; keep `127.0.0.1`.
- **Check 3 fails (peer decodes but no puppet).** The wire is right but the state is wrong.
  The puppet spawn requires `worldId` to match (`netstate.h:36`).  The peer echoes the
  game's `worldId`/`sceneKey` from the first received packet; if the peer never got a
  packet, it sends `worldId=0`.  Run check 2 first.  Position is a circle at y=0 around
  the origin â€” if the world's origin is underground/far, the puppet may be out of view.
- **Check 4 fails (guard does not die).** Either the objId is wrong (re-read step 2), or
  the kill lane is not wired for `kind=3`.  `kind=3` is the E0-B card/dispatch convention;
  it is **not** enumerated in `netstate.h:129` (which only documents 1=ship, 2=guard), so
  confirm the plugin's `ProcessKillEvents` actually keys on `kind==3` before changing the
  wire.  No `proto`/`mask` rejection in the log means the packet was *accepted* â€” the
  failure is then semantic, not transport.
- **Check 5 fails (double-death or invented death).** Transport is fine (loss/jitter are
  ours); this is a game-side dedup/identity bug in how kill events are applied.  Report
  with both logs.
- **Check 6 fails (`proto` / `mask` rejection).** The peer produced a datagram the plugin
  rejected.  Likely causes: wrong `sizeof(PlayerState)` (must be exactly 545 â€”
  `net.cpp:293` dispatches on exact length and silently drops anything else), a mask with
  the `EF_OBJID` bit (1<<8) set (`net.cpp:263` rejects it), or `protoVer` â‰  9.  The
  self-test guards the first two.

## 5. Known interpretive choices (not guesses on layout)

- `kind=3` = `KIND_KILL` is per the card/dispatch; `netstate.h`'s `EntityState` comment
  enumerates only 1/2.  Flagged here so a mismatch against `ProcessKillEvents` is caught
  by check 4, not hidden.
- The peer sends the kill record **every tick** (20 Hz).  Check 5's "no double-death" is
  therefore a real dedup test of the game; if the game re-kills every packet, that is the
  bug to find.
- The peer emits a **full** entity record for the kill only as `EF_HP|EF_DEAD` (minimal),
  matching the card's `hp=0xFFFF`.  `objId` is carried by the record identity (`wireId`),
  never as a mask bit, per `net.cpp:153`.
- `--jitter N` adds a uniform `[0, N]` ms delay per packet; `--loss N` drops a random `N`
  fraction of *sent* packets.

---

## Corrections after the first live run (2026-10-29, ORCH)

Three things in the original runbook were wrong. The peer itself is right; the
numbers and the trigger were not.

### 1. `sizeof(PlayerState)` is 560, not 545

`HookMath::Transform` and `HookMath::Vector4` are `__declspec(align(16))`
(`HookMath.h:34,58`). `#pragma pack(1)` does **not** override an explicit
alignment, so the compiler inserts padding that a hand count misses:

| field | hand-derived | actual |
|---|---|---|
| `matrix` | 28 | **32** (+4 pad after `sceneCellId`) |
| `heading` | 104 | **112** (+8 pad after `destSpeed`) |
| `valid` | 544 | **556** |
| `sizeof` | 545 | **560** (557 rounded up to the struct's align-16) |

Verified two ways: `cl.exe` printing `offsetof` for every field, and decoding the
game's own packets live (`size=560`, 0 decode failures). `peer9.py --selftest`
now asserts 560.

### 2. The trigger line is `[Net] init=`, not `[Net] up:`

`[Net] up: bound:…` goes to `OutputDebugString` and never reaches the log file. A
watcher keyed on it waits forever.

### 3. The kill lane is gated off by `EntitySelfTest=1`

`EntitySyncTick` returns at the self-test branch **before** the host branch, and
the host branch contains the only call to `ProcessKillEvents(in)`. With the
self-test on (needed for the F2 relay harness) the game ignores every incoming
entity/kill packet. For E0-B set:

```ini
[Coop]
EntitySelfTest=0   ; read once at first use — needs a relaunch
EntitySync=1
EntityHost=1
```

Reported to NET as a latent bug: the kill poll should run above the self-test
branch.

### Result of the first run (player lane only)

```
peer sent 986, received 1524, decode_failures 0
game log: [Net] apply seq=418/448/478 pos=(-46.7,107.1,7.4) … — the peer's circle, applied
game log: [Kill] 0   ([Kill] peer DEATH 0)
```

So a foreign process **can** drive the puppet. The kill lane is the part still
unproven, and it is blocked only by the config above.
