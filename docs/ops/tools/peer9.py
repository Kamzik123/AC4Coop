#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
peer9.py -- synthetic co-op peer for Assassin's Creed IV Black Flag.

Speaks the plugin's UDP wire protocol, NET_PROTO_VER = 9.  Standard library only.

Authoritative sources (all offsets/sizes below cite these lines):
  ref/AC4Coop/src/netstate.h   -- struct layouts + constants (the authority)
  ref/AC4Coop/src/net.cpp      -- how the plugin VALIDATES a datagram
  ref/AC4Coop/src/HookMath.h   -- Transform (4x4 float) and Vector4 (4 float)

Topology (one PC, one game -- E0-B card):
  game: bind 47801  <-rx--  peer: send -> 127.0.0.1:47801
  game: send -> 47802 --tx->  peer: bind 47802

The plugin accepts a PlayerState datagram ONLY when:
  * magic == 0x43344341 ('AC4C') AND n == sizeof(PlayerState)   (net.cpp:325-326)
  * source IP == PeerIP (strict-peer)                            (net.cpp:329)
so the single most important number here is sizeof(PlayerState) == 560.

Entity datagrams use the Phase-2 compact wire (netstate.h:151-181): a 20-byte
EntityWireHeader then per-record [u32 key, u8 kind, u8 idKind, u16 mask, u64 id]
plus only the fields whose mask bit is set, in fixed order (net.cpp:199-206).
objId is carried by the record identity (wireId), never by a mask bit; the
EF_OBJID bit (1<<8) must NOT appear in a mask (net.cpp:295 rejects it).

Post-T2 receive-path behaviour (net.cpp as of build 2D8E2E4D, 2026-10-30):
  * a header-only entity packet (count == 0) is a HEARTBEAT and does NOT refresh
    the plugin's liveness timer (net.cpp:376-379). What keeps the plugin's peer
    present is the PlayerState stream, so keep sending it (this tool does).
  * the plugin drops a peer entity snapshot it has not seen updated for 3 s
    (net.cpp:528-537), so a killed peer stops being mirrored instead of sticking.
  * the encoder now reserves the whole record before writing it (net.cpp:179-193);
    a packet that no longer fits ends early and is sent with the records that fit.
"""

import argparse
import math
import random
import socket
import struct
import sys
import time

# ---- wire constants (netstate.h) ------------------------------------------------
NET_MAGIC        = 0x43344341   # 'AC4C'  netstate.h:113
NET_ENTITY_MAGIC = 0x45344341   # 'AC4E'  netstate.h:123
NET_PROTO_VER    = 9            # netstate.h:116
BLEND_MAX        = 46           # netstate.h:5
CTX_RESERVE      = 128          # netstate.h:28 (_reserve[128])

# EntityField mask bits (netstate.h:158-169). EF_OBJID is excluded from any wire
# mask (objId rides the record identity); the plugin rejects a mask with bit 8 set
# (net.cpp:295: `mask & ~(EF_ALL & ~EF_OBJID)`).
EF_POS    = 1 << 0   # float pos[3]
EF_QUAT   = 1 << 1   # float quat[4]
EF_ACTION = 1 << 2   # u32 actionAlias
EF_HULLHP = 1 << 3   # i32 hullHP
EF_HP     = 1 << 4   # u16 hp
EF_ARCH   = 1 << 5   # u16 arch
EF_DEAD   = 1 << 6   # u8  dead
EF_STAGE  = 1 << 7   # u8  stage
EF_OBJID  = 1 << 8   # u64 objId (identity only -- never a field bit)
EF_ALL    = 0x01FF
EF_WIRE   = EF_ALL & ~EF_OBJID   # 0x00FF -- the only bits legal in a wire mask

# Record kinds (netstate.h:129 documents 1=ship, 2=guard).  kind=3 is KIND_KILL:
# the E0-B card + dispatch define it as the kill-lane event; ProcessKillEvents
# keys on it.  Not enumerated in netstate.h's EntityState comment -- flagged.
KIND_SHIP  = 1
KIND_GUARD = 2
KIND_KILL  = 3

ENTITY_WIRE_HEADER_SIZE = 20   # netstate.h:180


# =============================================================================
# PlayerState layout  (netstate.h:32-110, #pragma pack(1))
#
# sizeof(PlayerState) == 560, derived BY HAND *and* checked against cl.exe:
#   magic(I) protoVer(I) seq(I)                         4+4+4   = 12
#   worldId(Q)                                                  + 8  = 20
#   sceneKey(I) sceneCellId(I)                                  + 8  = 28
#   matrix = Transform = 16 floats (HookMath.h:58-64)          +64  = 92
#   curSpeed(f) destSpeed(f)                                    + 8  = 100
#   heading = Vector4 = 4 floats (HookMath.h:34-37)            +16  = 116
#   isSet,sprint,breakHardOnEdge,breakHard,curMvt,destMvt       + 6  = 122
#   contextId,subState,humanGroundStage (i32 each)             +12  = 134
#   weaponTotallyOut,isInTree,isOnShip,isOnOcean,doJumpToVoid  + 5  = 139
#   stalkingZoneCount..disturbedZoneCount (6 x i32)            +24  = 163
#   actionAlias(I) actionChannel(I) actionLayer(i)             +12  = 175
#   blendCount(B)                                               + 1  = 176
#   blendFactors[46] (46 floats)                              +184  = 360
#   actionRatio,blendInLen,blendInStartFromEnd,blendInTargetStart (4f) +16 = 376
#   blendInFlags(H)                                             + 2  = 378
#   isTransitionAction(B)                                       + 1  = 379
#   timeScale(f)                                                + 4  = 383
#   ctx = ContextBlock = _reserve[128]                        +128  = 511
#   shipValid(B) shipPos[3](3f) shipQuat[4](4f) shipHP(i)      +1+12+16+4 = 544
#   valid(B)                                                    + 1  = 545
# =============================================================================
SIZEOF_PLAYERSTATE = 560   # hardcoded constant; selftest asserts equality.

# Field list drives both pack and unpack; order == netstate.h field order.
_PLAYER_FIELDS = [
    ("magic",         "I"),
    ("protoVer",      "I"),
    ("seq",           "I"),
    ("worldId",       "Q"),
    ("sceneKey",      "I"),
    ("sceneCellId",   "I"),
    # HookMath::Transform is __declspec(align(16)) (HookMath.h:58). MSVC pack(1) does NOT
    # override explicit alignment, so matrix lands at 32, not 28.
    ("_pad_scene",    "4x"),
    ("matrix",        "16f"),
    ("curSpeed",      "f"),
    ("destSpeed",     "f"),
    # HookMath::Vector4 is also align(16) (HookMath.h:34) -> heading lands at 112, not 104.
    ("_pad_speed",    "8x"),
    ("heading",       "4f"),
    ("isSet",         "B"),
    ("sprint",        "B"),
    ("breakHardOnEdge", "B"),
    ("breakHard",     "B"),
    ("curMvt",        "B"),
    ("destMvt",       "B"),
    ("contextId",     "i"),
    ("subState",      "i"),
    ("humanGroundStage", "i"),
    ("weaponTotallyOut", "B"),
    ("isInTree",      "B"),
    ("isOnShip",      "B"),
    ("isOnOcean",     "B"),
    ("doJumpToVoid",  "B"),
    ("stalkingZoneCount", "i"),
    ("stalkingZoneBlendedCount", "i"),
    ("stalkingClipCount", "i"),
    ("stalkingClipBlendedCount", "i"),
    ("stalkingClipAllowFastStalkingCount", "i"),
    ("disturbedZoneCount", "i"),
    ("actionAlias",   "I"),
    ("actionChannel", "I"),
    ("actionLayer",   "i"),
    ("blendCount",    "B"),
    ("blendFactors",  "46f"),
    ("actionRatio",   "f"),
    ("blendInLen",    "f"),
    ("blendInStartFromEnd", "f"),
    ("blendInTargetStart",  "f"),
    ("blendInFlags",  "H"),
    ("isTransitionAction", "B"),
    ("timeScale",     "f"),
    ("ctx",           "128s"),
    ("shipValid",     "B"),
    ("shipPos",       "3f"),
    ("shipQuat",      "4f"),
    ("shipHP",        "i"),
    ("valid",         "B"),
    # The struct's alignment is 16 (from Transform), so sizeof rounds 557 up to 560.
    ("_pad_tail",     "3x"),
]
PLAYERSTATE_FMT  = "<" + "".join(f for _, f in _PLAYER_FIELDS)
PLAYERSTATE_SIZE = struct.calcsize(PLAYERSTATE_FMT)

# byte offset of every field, computed from the format string (self-verifying)
_PLAYER_OFFSET = {}
_o = 0
for _n, _f in _PLAYER_FIELDS:
    _PLAYER_OFFSET[_n] = _o
    _o += struct.calcsize("<" + _f)
assert _o == PLAYERSTATE_SIZE, "field list desync"


def make_player_state(seq, x, y, z, heading, world_id, scene_key):
    """Build a PlayerState dict with a sane ground-walk baseline."""
    if len(heading) == 3:
        heading = (heading[0], heading[1], heading[2], 0.0)
    tx, ty, tz, _tw = heading
    mat = [1.0, 0.0, 0.0, 0.0,
           0.0, 1.0, 0.0, 0.0,
           0.0, 0.0, 1.0, 0.0,
           x,    y,    z,   1.0]     # identity rotation; translation in row 4
    return {
        "magic":      NET_MAGIC,
        "protoVer":   NET_PROTO_VER,
        "seq":        seq,
        "worldId":    world_id,       # echo the game's worldId once seen (else 0)
        "sceneKey":   scene_key,      # echo the game's sceneKey (interior/exterior gate)
        "sceneCellId": 0,
        "matrix":     mat,
        "curSpeed":   1.0,
        "destSpeed":  1.0,
        "heading":    (tx, ty, tz, 0.0),
        "isSet":      1,
        "sprint":     0,
        "breakHardOnEdge": 0,
        "breakHard":  0,
        "curMvt":     0,
        "destMvt":    0,
        "contextId":  2,              # ground context (netstate.h:50)
        "subState":   0,
        "humanGroundStage": 0,
        "weaponTotallyOut": 0,
        "isInTree":   0,
        "isOnShip":   0,
        "isOnOcean":  0,
        "doJumpToVoid": 0,
        "stalkingZoneCount": 0,
        "stalkingZoneBlendedCount": 0,
        "stalkingClipCount": 0,
        "stalkingClipBlendedCount": 0,
        "stalkingClipAllowFastStalkingCount": 0,
        "disturbedZoneCount": 0,
        "actionAlias": 0,
        "actionChannel": 0,
        "actionLayer": 0,
        "blendCount":  0,
        "blendFactors": tuple(0.0 for _ in range(BLEND_MAX)),
        "actionRatio": 0.0,
        "blendInLen":  0.0,
        "blendInStartFromEnd": 0.0,
        "blendInTargetStart":  0.0,
        "blendInFlags": 0,
        "isTransitionAction": 0,
        "timeScale":   1.0,           # NEVER 0 (freezes the anim) -- netstate.h:95
        "ctx":         b"\x00" * CTX_RESERVE,
        "shipValid":   0,
        "shipPos":     (0.0, 0.0, 0.0),
        "shipQuat":    (0.0, 0.0, 0.0, 1.0),
        "shipHP":      0,
        "valid":       1,
    }


def pack_player_state(ps):
    buf = bytearray()
    for name, fmt in _PLAYER_FIELDS:
        f = "<" + fmt
        if fmt.endswith("x") and name.startswith("_pad"):
            buf += b"\x00" * struct.calcsize(f)   # alignment padding, no value
            continue
        v = ps[name]
        if fmt == "128s":
            buf += struct.pack(f, v)          # bytes, packed as-is
        elif len(fmt) > 1:
            buf += struct.pack(f, *v)         # multi-scalar (16f/4f/46f/3f) -> sequence
        else:
            buf += struct.pack(f, v)          # single scalar
    return bytes(buf)


def unpack_player_state(data):
    out = {}
    o = 0
    for name, fmt in _PLAYER_FIELDS:
        f = "<" + fmt
        size = struct.calcsize(f)
        if fmt.endswith("x") and name.startswith("_pad"):
            o += size                        # alignment padding, no value
            continue
        if fmt == "128s":
            out[name] = data[o:o + size]
        elif len(fmt) > 1:
            out[name] = struct.unpack_from(f, data, o)
        else:
            out[name] = struct.unpack_from(f, data, o)[0]
        o += size
    return out


# =============================================================================
# Phase-2 compact entity wire  (netstate.h:151-181, net.cpp:133-313)
# =============================================================================
# fixed field-emission order (net.cpp:199-206)
_WIRE_FIELDS = [
    (EF_POS,    "3f", "pos"),
    (EF_QUAT,   "4f", "quat"),
    (EF_ACTION, "I",  "action"),
    (EF_HULLHP, "i",  "hullHP"),
    (EF_HP,     "H",  "hp"),
    (EF_ARCH,   "H",  "arch"),
    (EF_DEAD,   "B",  "dead"),
    (EF_STAGE,  "B",  "stage"),
]


def encode_entity_wire(seq, epoch, records):
    """records: list of dicts {key,kind,idKind,wireId, <optional fields>}."""
    header = struct.pack("<IIIII", NET_ENTITY_MAGIC, NET_PROTO_VER,
                         seq, epoch, len(records))
    body = bytearray()
    for r in records:
        mask = 0
        fields = {}
        for bit, fmt, name in _WIRE_FIELDS:
            if name in r:
                mask |= bit
                fields[name] = (fmt, r[name])
        mask &= EF_WIRE                  # never set EF_OBJID
        body += struct.pack("<IBBHQ", r["key"], r["kind"], r.get("idKind", 0),
                            mask, r.get("wireId", 0))
        for _, (fmt, val) in fields.items():
            if isinstance(val, (tuple, list)):
                body += struct.pack("<" + fmt, *val)
            else:
                body += struct.pack("<" + fmt, val)
    return bytes(header) + bytes(body)


def decode_entity_wire(data):
    """Mirror of net.cpp DecodeEntityPacket.  Returns (header_dict, [records]) or None."""
    if len(data) < ENTITY_WIRE_HEADER_SIZE:
        return None
    magic, protoVer, seq, epoch, count = struct.unpack_from("<IIIII", data, 0)
    if magic != NET_ENTITY_MAGIC or protoVer != NET_PROTO_VER:
        return None
    if count > 128:   # MAX_SYNC_ENTITIES (netstate.h:124)
        return None
    o = ENTITY_WIRE_HEADER_SIZE
    recs = []
    for _ in range(count):
        if o + 16 > len(data):
            return None
        key, = struct.unpack_from("<I", data, o);  o += 4
        kind   = data[o];                          o += 1
        idKind = data[o];                          o += 1
        mask,  = struct.unpack_from("<H", data, o); o += 2
        wireId, = struct.unpack_from("<Q", data, o); o += 8
        if mask & ~EF_WIRE:                        # EF_OBJID bit is illegal
            return None
        rec = {
            "key": key, "kind": kind, "idKind": idKind, "wireId": wireId,
            "objId": wireId if idKind == 0 else None,   # 0 = real objId
        }
        for bit, fmt, name in _WIRE_FIELDS:
            if mask & bit:
                size = struct.calcsize("<" + fmt)
                if o + size > len(data):
                    return None
                vals = struct.unpack_from("<" + fmt, data, o);  o += size
                rec[name] = vals if size > 4 else (vals[0] if len(vals) == 1 else vals)
        recs.append(rec)
    return ({"magic": magic, "protoVer": protoVer, "seq": seq,
             "epoch": epoch, "count": count, "size": len(data)}, recs)


def make_kill_record(obj_id, hp, dead):
    """A KIND_KILL record: kind=3, real objId, hp sentinel, dead flag."""
    return {"key": 0, "kind": KIND_KILL, "idKind": 0, "wireId": obj_id,
            "hp": hp, "dead": 1 if dead else 0}


# =============================================================================
# self-test
# =============================================================================
def selftest():
    ok = True
    if PLAYERSTATE_SIZE != SIZEOF_PLAYERSTATE:
        print("SELFTEST FAIL: computed PlayerState size %d != hand-derived %d"
              % (PLAYERSTATE_SIZE, SIZEOF_PLAYERSTATE))
        return 1

    ps = make_player_state(42, 10.0, -3.5, 7.0, (0.0, 0.0, 1.0),
                           world_id=0x1122334455667788, scene_key=1)
    blob = pack_player_state(ps)
    if len(blob) != SIZEOF_PLAYERSTATE:
        print("SELFTEST FAIL: packed %d bytes, expected %d"
              % (len(blob), SIZEOF_PLAYERSTATE))
        return 1

    # rebuild: unpack and repack -> must be byte-identical
    rt = unpack_player_state(blob)
    blob2 = pack_player_state(rt)
    if blob2 != blob:
        print("SELFTEST FAIL: unpack/repack round-trip not byte-identical")
        return 1

    # spot-check the header fields land at their documented offsets
    magic, protoVer, seq = struct.unpack_from("<III", blob, 0)
    worldId, = struct.unpack_from("<Q", blob, 12)
    if (magic != NET_MAGIC or protoVer != NET_PROTO_VER or seq != 42
            or worldId != 0x1122334455667788):
        print("SELFTEST FAIL: header fields mispacked "
              "(magic=%08x proto=%d seq=%d world=%x)" % (magic, protoVer, seq, worldId))
        return 1

    # entity wire: build a kill record, encode, decode, compare
    rec = make_kill_record(0xAABBCCDD00000042, 0xFFFF, True)
    wire = encode_entity_wire(7, 1, [rec])
    hdr, recs = decode_entity_wire(wire)
    if (hdr is None or hdr["count"] != 1 or recs[0]["kind"] != KIND_KILL
            or recs[0]["objId"] != 0xAABBCCDD00000042
            or recs[0]["hp"] != 0xFFFF or recs[0]["dead"] != 1):
        print("SELFTEST FAIL: entity-wire round-trip mismatch: %r" % (recs,))
        return 1

    print("SELFTEST PASS: sizeof(PlayerState)=%d (expected %d), "
          "sizeof(EntityState)=56, sizeof(EntityPacket)=7184; "
          "player round-trip + entity-wire kill round-trip OK"
          % (PLAYERSTATE_SIZE, SIZEOF_PLAYERSTATE))
    return 0


# =============================================================================
# main
# =============================================================================
def main(argv):
    ap = argparse.ArgumentParser(description="AC4BF synthetic co-op peer (proto 9)")
    ap.add_argument("--selftest", action="store_true", help="run self-test and exit")
    ap.add_argument("--port", type=int, default=47802,
                    help="local bind port (default 47802)")
    ap.add_argument("--to", default="127.0.0.1:47801",
                    help="game address host:port (default 127.0.0.1:47801)")
    ap.add_argument("--walk", action="store_true",
                    help="scripted circle (radius 30 m) instead of standing still")
    ap.add_argument("--still", action="store_true",
                    help="stand still (default)")
    ap.add_argument("--loss", type=float, default=0.0,
                    help="fraction of sent packets to drop, 0..1 (default 0)")
    ap.add_argument("--jitter", type=float, default=0.0,
                    help="max extra send delay in ms (default 0)")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="run for N seconds, 0 = until Ctrl+C (default 0)")
    ap.add_argument("--pos", default=None,
                    help="x,y,z for the peer avatar (default 0,0,0)")
    ap.add_argument("--echo-pos", dest="echo_pos", action="store_true",
                    help="stand where the game's player is (from its own packets)")
    ap.add_argument("--world", type=lambda s: int(s, 0), default=0,
                    help="worldId to echo (hex accepted) - must match the game's")
    ap.add_argument("--scene", type=lambda s: int(s, 0), default=0,
                    help="sceneKey to echo (hex accepted) - the game's current localKey")
    ap.add_argument("--kill", type=int, default=None, metavar="OBJID",
                    help="send a KIND_KILL entity record for this objId")
    ap.add_argument("--hp", type=int, default=0xFFFF,
                    help="hp field on the entity record (default 0xFFFF)")
    ap.add_argument("--dead", action="store_true",
                    help="set dead=1 on the entity record (a kill implies it)")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    if not (0.0 <= args.loss <= 1.0):
        print("--loss must be in [0,1]")
        return 2

    host, _, port_s = args.to.partition(":")
    dest = (host, int(port_s) if port_s else 47801)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    sock.setblocking(False)
    print("[peer9] bound 0.0.0.0:%d -> %s:%d  proto=9  walk=%s loss=%.2f jitter=%.0fms"
          % (args.port, host, dest[1], args.walk, args.loss, args.jitter))
    if args.kill is not None:
        print("[peer9] kill lane: kind=%d objId=%d hp=%d dead=%d"
              % (KIND_KILL, args.kill, args.hp, 1 if args.dead else 0))

    seq = 0
    world_id = args.world
    scene_key = args.scene
    if args.pos:
        base_x, base_y, base_z = [float(v) for v in args.pos.split(",")]
    else:
        base_x = base_y = base_z = 0.0
    tx = rx = decode_fail = 0
    last_seq = {}          # magic -> last received seq
    max_gap = 0

    start = time.monotonic()
    next_send = start
    INTERVAL = 0.050       # 20 Hz

    def handle(datagram):
        nonlocal world_id, scene_key, rx, decode_fail, max_gap
        nonlocal base_x, base_y, base_z
        if len(datagram) < 4:
            return
        magic = struct.unpack_from("<I", datagram, 0)[0]
        if magic == NET_MAGIC:
            if len(datagram) == SIZEOF_PLAYERSTATE:
                ps = unpack_player_state(datagram)
                world_id = ps["worldId"]
                scene_key = ps["sceneKey"]
                mat = ps["matrix"]
                seq_ = ps["seq"]
                if args.echo_pos:
                    base_x, base_y, base_z = mat[12], mat[13], mat[14]
                gap = seq_ - last_seq.get(magic, seq_) - 1
                if gap > max_gap:
                    max_gap = gap
                last_seq[magic] = seq_
                print("RX player  magic=AC4C ver=%d seq=%d size=%d world=0x%x "
                      "pos=(%.1f, %.1f, %.1f) valid=%d"
                      % (ps["protoVer"], seq_, len(datagram), world_id,
                         mat[12], mat[13], mat[14], ps["valid"]))
            else:
                decode_fail += 1
                print("RX player  DECODE FAIL: size=%d (expected %d)"
                      % (len(datagram), SIZEOF_PLAYERSTATE))
        elif magic == NET_ENTITY_MAGIC:
            res = decode_entity_wire(datagram)
            if res is None:
                decode_fail += 1
                print("RX entity  DECODE FAIL: size=%d" % len(datagram))
                return
            hdr, recs = res
            gap = hdr["seq"] - last_seq.get(magic, hdr["seq"]) - 1
            if gap > max_gap:
                max_gap = gap
            last_seq[magic] = hdr["seq"]
            print("RX entity  magic=AC4E ver=%d seq=%d size=%d epoch=%d count=%d"
                  % (hdr["protoVer"], hdr["seq"], hdr["size"], hdr["epoch"], hdr["count"]))
            for r in recs:
                oid = r["objId"]
                oid_s = ("0x%x" % oid) if oid is not None else "synth(0x%x)" % r["wireId"]
                print("    kind=%d objId=%s hp=%s dead=%s"
                      % (r["kind"], oid_s,
                         r.get("hp", "-"), r.get("dead", "-")))
        else:
            print("RX unknown  magic=0x%08x size=%d" % (magic, len(datagram)))

    print("[peer9] running (Ctrl+C to stop) ...")
    try:
        while True:
            now = time.monotonic()
            if args.duration > 0 and (now - start) >= args.duration:
                break
            # drain whatever the game sent
            while True:
                try:
                    data, _addr = sock.recvfrom(16384)
                except (BlockingIOError, ConnectionResetError, OSError):
                    # no datagram pending; ConnectionResetError is Windows' ICMP
                    # port-unreachable when nothing listens on the destination
                    break
                rx += 1
                handle(data)
            # send on the 20 Hz cadence (+ jitter), dropping a random fraction
            if now >= next_send:
                if random.random() >= args.loss:
                    seq += 1
                    angle = (now - start) * (2 * math.pi / 20.0)   # full circle / 20 s
                    if args.walk:
                        # AC4 is Z-up: the small component in the game's own packets is z
                        # (height), so a horizontal circle lives in X/Y, not X/Z.
                        x = base_x + 3.0 * math.cos(angle)
                        y = base_y + 3.0 * math.sin(angle)
                        z = base_z
                        txh, tzh = -math.sin(angle), math.cos(angle)  # tangent
                    else:
                        x, y, z = base_x, base_y, base_z
                        txh, tzh = 0.0, 1.0
                    ps = make_player_state(seq, x, y, z, (txh, 0.0, tzh),
                                           world_id, scene_key)
                    sock.sendto(pack_player_state(ps), dest)
                    tx += 1
                    if args.kill is not None:
                        # KIND_KILL always carries dead=1 (a kill is a death); hp
                        # defaults to the 0xFFFF "dead" sentinel (--hp overrides).
                        rec = make_kill_record(args.kill, args.hp, True)
                        sock.sendto(encode_entity_wire(seq, 1, [rec]), dest)
                        tx += 1
                interval = INTERVAL
                if args.jitter > 0:
                    interval += random.uniform(0, args.jitter / 1000.0)
                next_send = now + interval
            # sleep only a little so we stay responsive to Ctrl+C and recv
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass

    print("[peer9] summary: sent=%d received=%d decode_failures=%d max_seq_gap=%d"
          % (tx, rx, decode_fail, max_gap))
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
