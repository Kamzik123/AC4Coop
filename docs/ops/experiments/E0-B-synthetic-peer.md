# E0-B — synthetic peer: a real second endpoint without a second game

Status: ready to build. Owner: QA. Supersedes E0-A (D-019).

---

## 1. Why

Steam/Uplay will not launch the same title twice on one PC, so "two instances" is
not available without a second machine. But the network path does not care what is
on the other end of the socket. A Python process that speaks `NET_PROTO_VER = 9`
is a valid peer for everything except rendering a world.

That turns the transport question from "buy a PC" into "write 300 lines".

## 2. Topology (one PC, one game)

```
game instance               synthetic peer (python)
  bind 47801   <--rx--------  send -> 127.0.0.1:47801
  send -> 47802 --tx-------->  bind 47802
```

Both sockets are distinct, both directions are exercised, and nothing about the
game's own instance guard matters — only one game runs.

## 3. What it must do (protocol from `ref/AC4Coop/src/netstate.h`, v9)

1. **PlayerState** at 20 Hz: magic `0x43344341` `'AC4C'`, `protoVer=9`, sequence,
   then the packed fields. A scripted walk (a circle of radius ~30 m) is enough;
   the point is that `[Net] apply` fires and the puppet moves.
2. **EntityPacket** for the kill lane: magic `0x45344341` `'AC4E'`, `protoVer=9`,
   `count`, then `EntityWireHeader` + per-record mask/fields. Send a `KIND_KILL`
   record (`kind=3`) with a chosen `objId` and `hp=0xFFFF`.
3. **Receive and decode** what the game sends: log sequence numbers, sizes,
   `PlayerState` deltas, and any `EntityState` records (kind, objId, hp, dead).
4. **Loss/jitter mode**: `--loss 0.2 --jitter 50` so the same script can produce
   the M2 packet-loss acceptance run.

## 4. Acceptance

| # | check | pass |
|---|---|---|
| 1 | game log shows `[Net] up: bound:47801 -> 127.0.0.1:47802` | yes |
| 2 | peer receives ≥ 15 packets/s and decodes the magic + protoVer | yes |
| 3 | game log shows `[Net] apply` > 0 and the puppet spawns | yes |
| 4 | sending a valid `KIND_KILL` for a guard's objId kills it on screen | yes |
| 5 | with `--loss 0.2`, no double-death and no invented death | yes |
| 6 | the game log never shows a packet from the peer being rejected (`proto` / `mask`) | yes |

Check 4 is the important one: it is the first time a kill arrives from a
**different process**, which is the actual co-op path.

## 5. Sequence

1. QA writes `ops/tools/peer9.py` (+ `ops/evidence/E0-B/README.md`).
2. Human launches the game once (single instance) and stands still in a guard camp.
3. QA (or the orchestrator) runs `peer9.py --kill <objId>` where the objId comes from
   the game's own `[Id]` lines, so the target is unambiguous.
4. Both logs are kept.

## 6. Honest limits

* No second rendered world: the puppet's *appearance* is still only validated against
  the local spawn, not against a peer's view.
* A synthetic peer will never discover a bug that only appears when both sides run the
  same tick loop (e.g. two statecharts racing).
* It cannot test join/leave timing under real latency; only loss and delay we inject.
