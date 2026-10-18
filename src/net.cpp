// Host-IP peer-to-peer UDP link (Winsock).
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include "net.h"

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

    void dbg(const char* s) { printf("%s", s); OutputDebugStringA(s); }

    DWORD WINAPI RecvThread(LPVOID) {
        // Read into a buffer sized for the LARGEST packet type (EntityPacket), then dispatch on the
        // leading magic. Both PlayerState and EntityPacket share the one UDP socket.
        char buf[sizeof(EntityPacket)];
        for (;;) {
            sockaddr_in from{}; int flen = sizeof(from);
            int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
            if (n >= 4) {
                uint32_t magic = *(uint32_t*)buf;
                bool playerPkt = (magic == NET_MAGIC        && n == (int)sizeof(PlayerState));
                bool entityPkt = (magic == NET_ENTITY_MAGIC && n == (int)sizeof(EntityPacket));
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
                        EnterCriticalSection(&g_lock);
                        if (playerPkt) { g_remote = *(PlayerState*)buf; g_haveRemote = true; }
                        else           { g_remoteEntities = *(EntityPacket*)buf; g_haveEntities = true; }
                        g_lastRecvMs = GetTickCount64();   // heartbeat for the presence check
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
    if (GetPrivateProfileIntA("Net", "Enabled", 0, ".\\AC4Coop.ini") == 0) return false;

    int localPort = GetPrivateProfileIntA("Net", "LocalPort", 47801, ".\\AC4Coop.ini");
    int peerPort  = GetPrivateProfileIntA("Net", "PeerPort",  47801, ".\\AC4Coop.ini");
    char peerIp[64] = {0};
    GetPrivateProfileStringA("Net", "PeerIP", "127.0.0.1", peerIp, sizeof(peerIp), ".\\AC4Coop.ini");

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { dbg("[Net] WSAStartup failed\n"); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { dbg("[Net] socket failed\n"); return false; }

    // Bind even if another socket already holds the port (a leftover/transient process from a
    // previous run or the launch chain). With the link started only on the first in-world frame,
    // the LIVE game is always the last to bind - and on Windows the last binder wins the incoming
    // datagrams - so the live game always receives, whatever else is holding the port.
    { BOOL reuse = TRUE; setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)); }

    // Bind LocalPort; if it's already taken (2nd instance on the same PC sharing this ini), bind
    // PeerPort instead and send to LocalPort. Two-PC setups use LocalPort==PeerPort and just bind.
    int bindPort = localPort, sendPort = peerPort;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons((u_short)bindPort);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        bindPort = peerPort; sendPort = localPort;
        g_secondInstance = true;   // LocalPort taken -> this is the same-PC 2nd instance (guest)
        local.sin_port = htons((u_short)bindPort);
        if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
            char m[96]; wsprintfA(m, "[Net] bind(%d/%d) failed err=%d\n", localPort, peerPort, WSAGetLastError()); dbg(m);
            closesocket(g_sock); g_sock = INVALID_SOCKET; return false;
        }
    }

    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons((u_short)sendPort);
    inet_pton(AF_INET, peerIp, &g_peer.sin_addr);

    InitializeCriticalSection(&g_lock);
    CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);

    g_enabled = true;
    g_lastRecvMs = 0;   // no packet accepted yet
    char m[128]; wsprintfA(m, "[Net] up: bound:%d -> %s:%d\n", bindPort, peerIp, sendPort); dbg(m);
    return true;
}

bool Enabled() { return g_enabled; }

bool IsSecondInstance() { return g_secondInstance; }

void Send(const PlayerState& s) {
    if (!g_enabled) return;
    sendto(g_sock, (const char*)&s, sizeof(s), 0, (sockaddr*)&g_peer, sizeof(g_peer));
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

void SendEntities(const EntityPacket& p) {
    if (!g_enabled) return;
    sendto(g_sock, (const char*)&p, sizeof(p), 0, (sockaddr*)&g_peer, sizeof(g_peer));
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

} // namespace Net
