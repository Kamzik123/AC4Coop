// Host-IP peer-to-peer UDP link (Winsock).
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <share.h>
#include "net.h"
#include "paths.h"

#pragma comment(lib, "ws2_32.lib")

namespace {
    bool               g_enabled = false;
    bool               g_secondInstance = false;   // LocalPort taken -> this is the same-PC 2nd instance
    SOCKET             g_sock = INVALID_SOCKET;
    sockaddr_in        g_peer{};
    CRITICAL_SECTION   g_lock;
    PlayerState        g_remote{};
    bool               g_haveRemote = false;
    EntityPacket       g_remoteEntities{};
    bool               g_haveEntities = false;
    bool               g_strictPeer = true;   // reject datagrams from any source but the configured peer
    uint64_t           g_lastRecvMs = 0;      // GetTickCount64() of the last ACCEPTED packet (0 = none yet)

    // ---- received DEATH ring (B-4: a one-shot death packet must not be lost to the snapshot slot) --
    // g_remoteEntities is last-writer-wins: `g_remoteEntities = decoded` below overwrites whatever the
    // previous datagram carried. The snapshot stream runs at SnapshotHz, so a one-entry KIND_KILL packet
    // that arrives just before a snapshot was overwritten BEFORE the main thread ever called
    // Net::GetEntities - the host had logged "[KT] death push", the guest logged nothing at all. Death
    // entries (kind==3) are therefore copied into this small ring as they are decoded, and the frame
    // drains it through GetKillEntities(), so the death is seen at least once regardless of how many
    // snapshots follow. Same idea as the player-packet ring above (FIX 4), sized far smaller because a
    // death is rare. A kill-only packet is also kept OUT of g_remoteEntities so it cannot displace a
    // snapshot (and cannot be processed twice).
    #define NET_KILLQ 32
    const uint8_t      NET_KIND_KILL = 3;     // phase1.cpp:2379 KIND_KILL (wire value, unchanged)
    EntityState        g_killRing[NET_KILLQ];
    int                g_killHead = 0;        // next write slot
    int                g_killCount = 0;       // filled entries; full -> the OLDEST is overwritten

    // ---- received player-packet ring (FIX 4: drain every queued packet per frame) ---------------
    // g_remote is last-wins: when two player packets land between two frames the older one is lost,
    // which starves the interpolation buffer exactly when the frame rate dips. This ring keeps them
    // all, in arrival order. g_remote/g_haveRemote are still maintained so other callers see no change.
    #define NET_RXQ 16
    PlayerState        g_rxq[NET_RXQ];
    int                g_rxqHead = 0;    // next write slot
    int                g_rxqCount = 0;   // filled entries (<= NET_RXQ); full -> the OLDEST is overwritten

    void dbg(const char* s) {
        printf("%s", s);
        OutputDebugStringA(s);
        static FILE* f = nullptr;
        if (!f) f = _fsopen(Paths::LogPath("net_log.txt"), "a", _SH_DENYWR);   // next to the ASI, not the CWD
        if (f) { fputs(s, f); fflush(f); }
    }

    // ---- same-PC instance slot marker (D-018) ----------------------------------------------------
    // A named mutex is an OWNED marker: whoever creates it first is slot 1, a second process that
    // finds it already existing is slot 2. Unlike a bind failure this cannot be defeated by
    // SO_REUSEADDR, which (deliberately, see the bind comment below) lets any number of sockets
    // bind the same port, so bind() never fails and the old "LocalPort taken" signal was dead.
    HANDLE             g_slotMutex = NULL;   // kept open for the process lifetime; closing it releases the marker

    bool DetectSecondInstance() {
        static bool s_done   = false;
        static bool s_second = false;
        if (!s_done) {
            s_done = true;
            SetLastError(0);
            HANDLE h = CreateMutexW(NULL, FALSE, L"AC4Coop_InstanceSlot_v1");
            if (!h) {
                char m[96]; wsprintfA(m, "[Net] instance marker failed err=%lu (assuming slot 1)\n", GetLastError()); dbg(m);
                s_second = false;
            } else {
                s_second = (GetLastError() == ERROR_ALREADY_EXISTS);
                g_slotMutex = h;   // never closed, not even on shutdown: it IS the marker
            }
        }
        return s_second;
    }

    volatile LONG g_txPackets = 0, g_txBytes = 0, g_txEnts = 0, g_rxPackets = 0, g_rxBytes = 0;

    // ---- Phase 2: compact entity wire ---------------------------------------------------------
    // A record carries a stable 64-bit identity. `key` is the entity-manager INDEX and is NOT stable
    // (the list shifts as NPCs stream in/out), so keying a delta shadow on it both kills the savings
    // and can apply a delta to the wrong entity. objId is stable; where there is none (ships) we
    // synthesise an id with bit 63 set, which can never collide with a real objId.
    struct ShadowSlot { uint64_t id; EntityState st; };
    static const int SHADOW_SLOTS = 4096;
    ShadowSlot g_txShadow[SHADOW_SLOTS];
    ShadowSlot g_rxShadow[SHADOW_SLOTS];
    uint8_t    g_txBuf[16384];
    uint32_t   g_txEpoch = 1;
    uint32_t   g_txSinceReset = 0;
    uint32_t   g_rxEpoch = 1;
    static const uint32_t EPOCH_PACKETS = 64;    // both sides drop their shadow this often; also the
                                                 // self-heal bound after a lost delta packet (~3 s)
    uint32_t   g_wireFullRecs = 0;               // instrumentation: records that carried every field
    uint32_t   g_wireRecs = 0;                   // instrumentation: records in the last packet

    inline uint64_t ShadowId(uint8_t kind, uint32_t key, uint64_t objId) {
        if (objId) return objId;
        return 0x8000000000000000ull | ((uint64_t)(kind & 3) << 32) | (uint64_t)key;
    }
    inline uint32_t ShadowHash(uint64_t id) {
        uint64_t x = id * 0x9E3779B97F4A7C15ull;
        return (uint32_t)(x >> 52) & (SHADOW_SLOTS - 1);
    }
    // Open addressing, bounded probe. Returns nullptr only if the table is saturated (impossible at
    // <=1792 ids per epoch); the caller then sends a full record, which is always correct.
    ShadowSlot* ShadowFind(ShadowSlot* table, uint64_t id, bool create) {
        const uint32_t h = ShadowHash(id);
        for (uint32_t i = 0; i < 64; ++i) {
            ShadowSlot& s = table[(h + i) & (SHADOW_SLOTS - 1)];
            if (s.id == id) return &s;
            if (s.id == 0) {
                if (!create) return nullptr;
                s.id = id;
                std::memset(&s.st, 0, sizeof(s.st));
                return &s;
            }
        }
        return nullptr;
    }

    size_t EncodeEntityPacket(const EntityPacket& p, uint8_t* out, size_t cap) {
        if (cap < sizeof(EntityWireHeader)) return 0;
        // Symmetric epoch reset: the receiver drops its base when it sees a new epoch, so the two
        // tables can never drift apart.
        if (++g_txSinceReset > EPOCH_PACKETS) {
            std::memset(g_txShadow, 0, sizeof(g_txShadow));
            g_txSinceReset = 0;
            ++g_txEpoch;
        }
        EntityWireHeader h{};
        h.magic = p.magic; h.protoVer = p.protoVer; h.seq = p.seq; h.epoch = g_txEpoch;
        h.count = 0;                         // patched to the WRITTEN count after the loop
        std::memcpy(out, &h, sizeof(h));
        size_t o = sizeof(h);
        uint32_t fullRecs = 0;
        uint32_t written = 0;
        for (uint32_t i = 0; i < p.count && i < (uint32_t)MAX_SYNC_ENTITIES; ++i) {
            const EntityState& e = p.ents[i];
            const uint64_t id = ShadowId(e.kind, e.key, e.objId);
            ShadowSlot* sh = ShadowFind(g_txShadow, id, true);
            uint16_t mask = 0;
            if (!sh) {
                mask = (uint16_t)(EF_ALL & ~EF_OBJID);   // table saturated: send everything
            } else {
                // Quantised delta: position/quaternion only go on the wire when they moved enough to be
                // visible. The shadow keeps the SENT value, so small movements accumulate rather than
                // being lost. 5 cm / ~0.3 deg.
                const float dx = e.pos[0] - sh->st.pos[0];
                const float dy = e.pos[1] - sh->st.pos[1];
                const float dz = e.pos[2] - sh->st.pos[2];
                if (dx * dx + dy * dy + dz * dz > 0.0025f) mask |= EF_POS;
                float dq = 0.0f;
                for (int q = 0; q < 4; ++q) { float d = e.quat[q] - sh->st.quat[q]; dq += (d < 0.0f ? -d : d); }
                if (dq > 0.005f) mask |= EF_QUAT;
                if (e.actionAlias != sh->st.actionAlias) mask |= EF_ACTION;
                if (e.hullHP      != sh->st.hullHP)      mask |= EF_HULLHP;
                if (e.hp          != sh->st.hp)          mask |= EF_HP;
                if (e.arch        != sh->st.arch)        mask |= EF_ARCH;
                if (e.dead        != sh->st.dead)        mask |= EF_DEAD;
                if (e.stage       != sh->st.stage)       mask |= EF_STAGE;
                // objId is carried by the record identity, never by a field bit.
            }
            // Nothing changed since the last packet: omit the record entirely (0 bytes instead of a
            // 16-byte stub). Absence is safe - ApplyGuards/ApplyShips only drive what is present.
            if (mask == 0) continue;
            const uint8_t idKind = (e.objId ? 0 : 1);   // 0 = real objId, 1 = synthesised from kind+key
            const uint64_t wireId = e.objId ? e.objId
                                            : (((uint64_t)(e.kind & 3) << 32) | (uint64_t)e.key);
            if (o + 16 > cap) return 0;
            std::memcpy(out + o, &e.key, 4); o += 4;
            out[o++] = e.kind;
            out[o++] = idKind;
            std::memcpy(out + o, &mask, 2); o += 2;
            std::memcpy(out + o, &wireId, 8); o += 8;
            if (mask & EF_POS)    { std::memcpy(out + o, e.pos, 12); o += 12; }
            if (mask & EF_QUAT)   { std::memcpy(out + o, e.quat, 16); o += 16; }
            if (mask & EF_ACTION) { std::memcpy(out + o, &e.actionAlias, 4); o += 4; }
            if (mask & EF_HULLHP) { std::memcpy(out + o, &e.hullHP, 4); o += 4; }
            if (mask & EF_HP)     { std::memcpy(out + o, &e.hp, 2); o += 2; }
            if (mask & EF_ARCH)   { std::memcpy(out + o, &e.arch, 2); o += 2; }
            if (mask & EF_DEAD)   { out[o++] = e.dead; }
            if (mask & EF_STAGE)  { out[o++] = e.stage; }
            if (sh) {
                sh->st.key = e.key; sh->st.kind = e.kind; sh->st.objId = e.objId;
                if (mask & EF_POS)    std::memcpy(sh->st.pos,  e.pos,  sizeof(e.pos));
                if (mask & EF_QUAT)   std::memcpy(sh->st.quat, e.quat, sizeof(e.quat));
                if (mask & EF_ACTION) sh->st.actionAlias = e.actionAlias;
                if (mask & EF_HULLHP) sh->st.hullHP = e.hullHP;
                if (mask & EF_HP)     sh->st.hp = e.hp;
                if (mask & EF_ARCH)   sh->st.arch = e.arch;
                if (mask & EF_DEAD)   sh->st.dead = e.dead;
                if (mask & EF_STAGE)  sh->st.stage = e.stage;
            }
            if (mask == (uint16_t)(EF_ALL & ~EF_OBJID)) ++fullRecs;
            ++written;
        }
        h.count = written;                   // the receiver writes exactly this many records
        std::memcpy(out, &h, sizeof(h));
        g_wireRecs = written;
        g_wireFullRecs = fullRecs;
        return o;
    }

    // Oracle for the Phase 2 codec, run at startup on both peers:
    bool DecodeEntityPacket(const uint8_t* in, size_t len, EntityPacket& out);   // fwd decl

    static bool SameEntity(const EntityState& a, const EntityState& b) {
        return a.key == b.key && a.kind == b.kind && a.dead == b.dead && a.stage == b.stage
            && a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] && a.pos[2] == b.pos[2]
            && a.quat[0] == b.quat[0] && a.quat[1] == b.quat[1]
            && a.quat[2] == b.quat[2] && a.quat[3] == b.quat[3]
            && a.actionAlias == b.actionAlias && a.hullHP == b.hullHP
            && a.objId == b.objId && a.hp == b.hp && a.arch == b.arch;
    }

    bool SelfTestWire() {
        EntityPacket a{};
        a.magic = NET_ENTITY_MAGIC; a.protoVer = NET_PROTO_VER; a.seq = 1; a.count = 3;
        for (int i = 0; i < 3; ++i) {
            EntityState& e = a.ents[i];
            e.key = (uint32_t)i; e.kind = 2; e.dead = (uint8_t)(i == 1); e.stage = (uint8_t)(i + 1);
            e.pos[0] = 10.5f + i; e.pos[1] = -3.25f * i; e.pos[2] = 7.0f;
            e.quat[0] = 0.1f; e.quat[1] = 0.2f; e.quat[2] = 0.3f; e.quat[3] = 0.9f;
            e.actionAlias = 0x11223344u + i; e.hullHP = 100 + i;
            e.objId = 0xAABBCCDD00000000ull + i; e.hp = (uint16_t)(50 + i); e.arch = (uint16_t)(0x35F7 + i);
        }
        std::memset(g_txShadow, 0, sizeof(g_txShadow));
        std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
        g_txEpoch = g_rxEpoch = 1; g_txSinceReset = 0;
        uint8_t buf[16384];
        const size_t n1 = EncodeEntityPacket(a, buf, sizeof(buf));
        EntityPacket b{};
        bool ok = n1 > 0 && DecodeEntityPacket(buf, n1, b) && b.count == 3;
        if (ok) for (int i = 0; i < 3; ++i) ok = ok && SameEntity(a.ents[i], b.ents[i]);
        // Second pass: nothing changed, so every entity must be omitted (header only).
        const size_t n2 = EncodeEntityPacket(a, buf, sizeof(buf));
        EntityPacket c{};
        bool ok2 = (n2 == sizeof(EntityWireHeader)) && DecodeEntityPacket(buf, n2, c) && c.count == 0;
        // Both peers must start from an empty shadow.
        std::memset(g_txShadow, 0, sizeof(g_txShadow));
        std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
        g_txEpoch = g_rxEpoch = 1; g_txSinceReset = 0;
        char m[160];
        wsprintfA(m, "[Net] wire codec selftest: roundtrip=%s nochange=%s (full=%u B, unchanged=%u B for 3 ents)\n",
                  ok ? "PASS" : "FAIL", ok2 ? "PASS" : "FAIL", (unsigned)n1, (unsigned)n2);
        dbg(m);
        return ok && ok2;
    }

    bool DecodeEntityPacket(const uint8_t* in, size_t len, EntityPacket& out) {
        if (len < sizeof(EntityWireHeader)) return false;
        EntityWireHeader h{};
        std::memcpy(&h, in, sizeof(h));
        if (h.magic != NET_ENTITY_MAGIC || h.protoVer != NET_PROTO_VER) return false;
        if (h.count > (uint32_t)MAX_SYNC_ENTITIES) return false;
        // Same epoch rule as the sender: a new epoch means the base is gone on both sides.
        if (h.epoch != g_rxEpoch) {
            std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
            g_rxEpoch = h.epoch;
        }
        out.magic = h.magic; out.protoVer = h.protoVer; out.seq = h.seq; out.count = 0;
        size_t o = sizeof(h);
        for (uint32_t i = 0; i < h.count; ++i) {
            if (o + 16 > len) return false;
            uint32_t key = 0; uint8_t kind = 0; uint8_t idKind = 0; uint16_t mask = 0; uint64_t wireId = 0;
            std::memcpy(&key, in + o, 4); o += 4;
            kind = in[o++];
            idKind = in[o++];
            std::memcpy(&mask, in + o, 2); o += 2;
            std::memcpy(&wireId, in + o, 8); o += 8;
            if (mask & ~(uint16_t)(EF_ALL & ~EF_OBJID)) return false;
            EntityState e{};
            e.key = key; e.kind = kind; e._pad = 0;
            e.objId = (idKind == 0) ? wireId : 0;
            ShadowSlot* sh = ShadowFind(g_rxShadow, ShadowId(kind, key, e.objId), true);
            if (sh) e = sh->st, e.key = key, e.kind = kind, e._pad = 0, e.objId = (idKind == 0) ? wireId : 0;
            if (mask & EF_POS)    { if (o + 12 > len) return false; std::memcpy(e.pos, in + o, 12); o += 12; }
            if (mask & EF_QUAT)   { if (o + 16 > len) return false; std::memcpy(e.quat, in + o, 16); o += 16; }
            if (mask & EF_ACTION) { if (o + 4 > len) return false; std::memcpy(&e.actionAlias, in + o, 4); o += 4; }
            if (mask & EF_HULLHP) { if (o + 4 > len) return false; std::memcpy(&e.hullHP, in + o, 4); o += 4; }
            if (mask & EF_HP)     { if (o + 2 > len) return false; std::memcpy(&e.hp, in + o, 2); o += 2; }
            if (mask & EF_ARCH)   { if (o + 2 > len) return false; std::memcpy(&e.arch, in + o, 2); o += 2; }
            if (mask & EF_DEAD)   { if (o + 1 > len) return false; e.dead = in[o++]; }
            if (mask & EF_STAGE)  { if (o + 1 > len) return false; e.stage = in[o++]; }
            if (sh) sh->st = e;
            out.ents[out.count++] = e;
        }
        return true;
    }


    DWORD WINAPI RecvThread(LPVOID) {
        // Read into a buffer sized for the LARGEST packet type (EntityPacket), then dispatch on the
        // leading magic. Both PlayerState and EntityPacket share the one UDP socket.
        char buf[16384];
        for (;;) {
            sockaddr_in from{}; int flen = sizeof(from);
            int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
            if (n >= 4) {
                uint32_t magic = *(uint32_t*)buf;
                bool playerPkt = (magic == NET_MAGIC        && n == (int)sizeof(PlayerState));
                bool entityPkt = (magic == NET_ENTITY_MAGIC && n >= ENTITY_WIRE_HEADER_SIZE);
                if (playerPkt || entityPkt) {
                    // Strict peer (Feature #3): accept only datagrams from the configured PeerIP.
                    if (g_strictPeer && from.sin_addr.s_addr != g_peer.sin_addr.s_addr) {
                        static bool warned = false;
                        if (!warned) {
                            warned = true;
                            char got[32] = {0}, exp[32] = {0};
                            inet_ntop(AF_INET, &from.sin_addr, got, sizeof(got));
                            inet_ntop(AF_INET, &g_peer.sin_addr, exp, sizeof(exp));
                            char m[160];
                            wsprintfA(m, "[Net] dropped packet from unexpected source %s (expected %s)\n", got, exp);
                            dbg(m);
                        }
                    } else {
                        EntityPacket decoded;
                        const bool entityOk = !entityPkt ||
                            DecodeEntityPacket((const uint8_t*)buf, (size_t)n, decoded);
                        EnterCriticalSection(&g_lock);
                        if (playerPkt) {
                            g_remote = *(PlayerState*)buf; g_haveRemote = true;
                            // Queue it too (overwrites the OLDEST when the ring is full), so a frame
                            // that drains several packets feeds the interpolator all of them.
                            g_rxq[g_rxqHead] = g_remote;
                            g_rxqHead = (g_rxqHead + 1) % NET_RXQ;
                            if (g_rxqCount < NET_RXQ) ++g_rxqCount;
                        }
                        else if (entityOk) {
                            // B-4: pull the death entries out into their own ring first. A packet made
                            // only of them (the dedicated one-entry death push) does not touch the
                            // snapshot slot at all, so it can neither be overwritten before the frame
                            // reads it nor clobber a snapshot.
                            bool hadNonKill = false;
                            for (uint32_t ki = 0; ki < decoded.count; ++ki) {
                                const EntityState& ks = decoded.ents[ki];
                                if (ks.kind != NET_KIND_KILL) { hadNonKill = true; continue; }
                                g_killRing[g_killHead] = ks;
                                g_killHead = (g_killHead + 1) % NET_KILLQ;
                                if (g_killCount < NET_KILLQ) ++g_killCount;
                            }
                            if (hadNonKill || decoded.count == 0) {
                                g_remoteEntities = decoded; g_haveEntities = true;
                            }
                        }
                        g_lastRecvMs = GetTickCount64();   // heartbeat for the presence check
                        InterlockedIncrement(&g_rxPackets);
                        InterlockedAdd(&g_rxBytes, (LONG)n);
                        LeaveCriticalSection(&g_lock);
                    }
                }
            } else if (n == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEINTR || e == WSAENOTSOCK) break;   // socket closed
                Sleep(1);
            }
        }
        return 0;
    }
}

namespace Net {

bool Init() {
    if (GetPrivateProfileIntA("Net", "Enabled", 0, Paths::Ini()) == 0) return false;

    int localPort = GetPrivateProfileIntA("Net", "LocalPort", 47801, Paths::Ini());
    int peerPort  = GetPrivateProfileIntA("Net", "PeerPort",  47801, Paths::Ini());
    char peerIp[64] = {0};
    GetPrivateProfileStringA("Net", "PeerIP", "127.0.0.1", peerIp, sizeof(peerIp), Paths::Ini());

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { dbg("[Net] WSAStartup failed\n"); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { dbg("[Net] socket failed\n"); return false; }

    // Bind even if another socket already holds the port (a leftover/transient process from a
    // previous run or the launch chain). With the link started only on the first in-world frame,
    // the LIVE game is always the last to bind - and on Windows the last binder wins the incoming
    // datagrams - so the live game always receives, whatever else is holding the port.
    { BOOL reuse = TRUE; setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)); }

    // Instance slot comes from the owned mutex marker FIRST (D-018). With SO_REUSEADDR below the
    // bind can never fail, so a bind failure alone can no longer tell the 2nd instance apart.
    g_secondInstance = DetectSecondInstance();

    // Bind LocalPort; if it's already taken (2nd instance on the same PC sharing this ini), bind
    // PeerPort instead and send to LocalPort. Two-PC setups use LocalPort==PeerPort and just bind.
    // Slot 1 keeps exactly this behaviour; slot 2 is decided by the marker above, and a bind failure
    // is kept only as a SECONDARY signal (it can still fire for other reasons).
    int bindPort = g_secondInstance ? peerPort  : localPort;
    int sendPort = g_secondInstance ? localPort : peerPort;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons((u_short)bindPort);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        bindPort = g_secondInstance ? localPort : peerPort;
        sendPort = g_secondInstance ? peerPort : localPort;
        g_secondInstance = true;   // bind failure (secondary signal) -> this is the same-PC 2nd instance (guest)
        local.sin_port = htons((u_short)bindPort);
        if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
            char m[96]; wsprintfA(m, "[Net] bind(%d/%d) failed err=%d\n", localPort, peerPort, WSAGetLastError()); dbg(m);
            closesocket(g_sock); g_sock = INVALID_SOCKET; return false;
        }
    }
    { char m[96]; wsprintfA(m, "[Net] instance slot=%d bind=%d peer=%d\n", g_secondInstance ? 2 : 1, bindPort, sendPort); dbg(m); }

    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons((u_short)sendPort);
    inet_pton(AF_INET, peerIp, &g_peer.sin_addr);

    InitializeCriticalSection(&g_lock);
    CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);

    g_enabled = true;
    g_lastRecvMs = 0;   // no packet accepted yet
    SelfTestWire();     // [Phase 2] prove the compact codec round-trips before any traffic
    char m[128]; wsprintfA(m, "[Net] up: bound:%d -> %s:%d\n", bindPort, peerIp, sendPort); dbg(m);
    return true;
}

bool Enabled() { return g_enabled; }

bool IsSecondInstance() { return g_secondInstance; }

void Send(const PlayerState& s) {
    if (!g_enabled) return;
    sendto(g_sock, (const char*)&s, sizeof(s), 0, (sockaddr*)&g_peer, sizeof(g_peer));
    InterlockedIncrement(&g_txPackets);
    InterlockedAdd(&g_txBytes, (LONG)sizeof(s));
}

bool GetRemote(PlayerState& out) {
    if (!g_enabled) return false;
    bool have;
    EnterCriticalSection(&g_lock);
    have = g_haveRemote;
    if (have) out = g_remote;
    LeaveCriticalSection(&g_lock);
    return have;
}

// Pop up to `max` queued player packets, oldest first. Returns how many were written to `out`.
// GetRemote is unchanged: callers that only want the newest packet keep using it.
int DrainRemote(PlayerState* out, int max) {
    if (!g_enabled || !out || max <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&g_lock);
    while (n < max && g_rxqCount > 0) {
        int idx = (g_rxqHead - g_rxqCount + NET_RXQ) % NET_RXQ;   // oldest entry in the ring
        out[n++] = g_rxq[idx];
        --g_rxqCount;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

void SendEntities(const EntityPacket& p) {
    if (!g_enabled) return;
    const size_t n = EncodeEntityPacket(p, g_txBuf, sizeof(g_txBuf));
    if (n == 0) return;
    sendto(g_sock, (const char*)g_txBuf, (int)n, 0, (sockaddr*)&g_peer, sizeof(g_peer));
    InterlockedIncrement(&g_txPackets);
    InterlockedAdd(&g_txBytes, (LONG)n);          // actual encoded size, not sizeof(EntityPacket)
    InterlockedAdd(&g_txEnts, (LONG)g_wireRecs);
    static uint32_t s_logCounter = 0;
    if ((++s_logCounter % 60) == 0) {
        char m[160];
        wsprintfA(m, "[Net] entwire pkt=%u B sent=%u/%u full=%u/%u\n",
                  (unsigned)n, (unsigned)g_wireRecs, (unsigned)p.count,
                  (unsigned)g_wireFullRecs, (unsigned)g_wireRecs);
        dbg(m);
    }
}

bool GetEntities(EntityPacket& out) {
    if (!g_enabled) return false;
    bool have;
    EnterCriticalSection(&g_lock);
    have = g_haveEntities;
    if (have) out = g_remoteEntities;
    LeaveCriticalSection(&g_lock);
    return have;
}

// B-4: drain the received DEATH ring into `out` (n = number of entries copied, 0 = nothing pending).
// The packet is shaped like any other EntityPacket, so the caller hands it straight to
// ProcessKillEvents. This is what makes a one-entry KIND_KILL datagram survive a snapshot arriving in
// the same frame gap: GetEntities() only ever exposes the latest snapshot, this queue keeps the deaths.
uint32_t GetKillEntities(EntityPacket& out, uint32_t max) {
    if (!g_enabled) return 0;
    if (max == 0 || max > (uint32_t)MAX_SYNC_ENTITIES) max = (uint32_t)MAX_SYNC_ENTITIES;
    out.magic = NET_ENTITY_MAGIC;
    out.protoVer = NET_PROTO_VER;
    out.seq = 0;
    out.count = 0;
    uint32_t n = 0;
    EnterCriticalSection(&g_lock);
    while (n < max && g_killCount > 0) {
        const int idx = (g_killHead - g_killCount + NET_KILLQ) % NET_KILLQ;   // oldest entry
        out.ents[n++] = g_killRing[idx];
        --g_killCount;
    }
    out.count = n;
    LeaveCriticalSection(&g_lock);
    if (n) out.seq = GetTickCount();
    return n;
}

// ---- session-lifecycle hardening (Feature #3) --------------------------------------
void SetStrictPeer(bool strict) { g_strictPeer = strict; }

uint64_t LastRecvMs() {
    if (!g_enabled) return 0;
    EnterCriticalSection(&g_lock);
    uint64_t t = g_lastRecvMs;
    LeaveCriticalSection(&g_lock);
    return t;
}

uint32_t MillisSinceRecv() {
    if (!g_enabled) return 0xFFFFFFFFu;
    EnterCriticalSection(&g_lock);
    uint64_t t = g_lastRecvMs;
    LeaveCriticalSection(&g_lock);
    if (t == 0) return 0xFFFFFFFFu;               // nothing received yet
    return (uint32_t)(GetTickCount64() - t);       // 64-bit delta, narrowed after the subtraction
}

// ---- wire counters ------------------------------------------------------------------------------
void GetStats(Stats& out) {
    out.txPackets = (uint32_t)InterlockedCompareExchange(&g_txPackets, 0, 0);
    out.txBytes   = (uint32_t)InterlockedCompareExchange(&g_txBytes,   0, 0);
    out.txEnts    = (uint32_t)InterlockedCompareExchange(&g_txEnts,    0, 0);
    out.rxPackets = (uint32_t)InterlockedCompareExchange(&g_rxPackets, 0, 0);
    out.rxBytes   = (uint32_t)InterlockedCompareExchange(&g_rxBytes,   0, 0);
}

void ResetStats() {
    InterlockedExchange(&g_txPackets, 0);
    InterlockedExchange(&g_txBytes,   0);
    InterlockedExchange(&g_txEnts,    0);
    InterlockedExchange(&g_rxPackets, 0);
    InterlockedExchange(&g_rxBytes,   0);
}

} // namespace Net
