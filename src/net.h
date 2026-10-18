#pragma once
#include "netstate.h"

// Host-IP peer-to-peer UDP link. Both sides send their local PlayerState and receive the peer's.
// Config from AC4Coop.ini [Net]: Enabled, LocalPort, PeerIP, PeerPort.
namespace Net {
    bool Init();                        // read ini, open socket, start recv thread; returns Enabled()
    bool Enabled();
    bool IsSecondInstance();            // true if LocalPort was taken (this is the same-PC 2nd instance)
    void Send(const PlayerState& s);    // sendto peer (no-op if disabled)
    bool GetRemote(PlayerState& out);   // copy latest received state; false if none yet

    // Entity sync (host-authoritative "synced worlds"): a separate packet type on the same socket.
    void SendEntities(const EntityPacket& p);   // sendto peer (no-op if disabled)
    bool GetEntities(EntityPacket& out);        // copy latest received entity list; false if none yet

    // Session-lifecycle hardening (Feature #3, ini [Coop] NetStrictPeer):
    void     SetStrictPeer(bool strict); // accept datagrams only from the configured PeerIP (default true)
    uint64_t LastRecvMs();               // GetTickCount64() of the last ACCEPTED packet (0 = none yet)
    uint32_t MillisSinceRecv();          // ms since the last accepted packet; 0xFFFFFFFF if none yet
}
