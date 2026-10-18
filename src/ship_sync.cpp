// ship_sync.cpp — REFERENCE IMPLEMENTATION (not compiled into the ASI).
// The ship-sync data layer: capture the local ship, relay it, and drive a ghost ship's render.
// Wire into phase1.cpp + netstate.h/net.cpp when the ghost-spawn (SHIP-SYNC-HANDOFF.md §2) lands.
//
// PROVEN parts (already in the ASI):
//   - SetGlobalMatrixInternal hook (VA 0x00528200) = the render-override seam (works).
//   - ship = handleResolve(h->m_IsOnShipEntity); transform at +0x10 (pos +0x40); HP = *(int*)(*(void**)(ship+0x5C)+0x84).
//
// This file is the exact code to add once a second ship entity ("ghost") exists in each world.

#include "phase1.h"
#include "Scimitar/Entity.h"
#include "Scimitar/Components/PhysicComponent.h"
#include "HookMath.h"

// ---- wire struct (append to PlayerState in netstate.h; bump NET_PROTO_VER) ----
#pragma pack(push, 1)
struct ShipState {
    bool     valid;      // player is aboard a ship
    float    pos[3];     // ship world position (x,y,z)   = m_GlobalMatrix.M41/M42/M43
    float    quat[4];    // ship rotation (x,y,z,w)        = from m_GlobalMatrix 3x3
    int32_t  hullHP;     // hull hit points                = *(int*)(*(void**)(ship+0x5C)+0x84)
};
#pragma pack(pop)

// ---- capture (run on the sender each frame, on the main thread) ----
bool Phase1::CaptureShipState(ShipState& out) {
    out.valid = false;
    World* world = World::GetCurrentWorld();
    Entity* player = world ? world->GetPlayer() : nullptr;
    Human* h = player ? AIEntityHelper::GetHuman(player) : nullptr;
    if (!h) return false;
    void* ship = handleResolve(h->m_IsOnShipEntity);
    if (!ship) ship = handleResolve(h->m_Data.m_Vehicle);
    if (!ship) return false;

    HookMath::Transform* m = &((BaseEntity*)ship)->m_GlobalMatrix;
    if (!readable(m, sizeof(HookMath::Transform))) return false;
    out.valid = true;
    out.pos[0] = m->M41; out.pos[1] = m->M42; out.pos[2] = m->M43;

    // rotation 3x3 -> quaternion (row-major M11..M33 at +0x10..+0x3C)
    HookMath::Quaternion q;
    HookMath::FromMatrix(m, &q);
    out.quat[0] = q.x; out.quat[1] = q.y; out.quat[2] = q.z; out.quat[3] = q.w;

    void* stat = readable((char*)ship + 0x5C, 4) ? *(void**)((char*)ship + 0x5C) : nullptr;
    out.hullHP = (stat && readable((char*)stat + 0x84, 4)) ? *(int*)((char*)stat + 0x84) : 0;
    return true;
}

// ---- apply (run on the receiver each frame): drive a GHOST ship's render to the peer's state ----
// g_ghostShipEnt is the spawned second ship entity (from SHIP-SYNC-HANDOFF.md §2). Its world matrix is
// overridden in LogSetGlobalMatrix (the existing hook) by replacing the F5 g_holdPos with the peer pos,
// and leaving Z free so the ghost still bobs on the local waves.
void Phase1::ApplyShipState(const ShipState& in, void* ghostShipEnt) {
    if (!in.valid || !ghostShipEnt) return;
    // store as the render-override target consumed by LogSetGlobalMatrix
    g_shipSyncTarget[0] = in.pos[0];
    g_shipSyncTarget[1] = in.pos[1];
    g_shipSyncTarget[2] = in.pos[2];
    g_shipSyncEnt = ghostShipEnt;
    // hull HP: optionally write to the ghost's status component (same offset as the real ship).
    void* stat = readable((char*)ghostShipEnt + 0x5C, 4) ? *(void**)((char*)ghostShipEnt + 0x5C) : nullptr;
    if (stat && readable((char*)stat + 0x84, 4)) *(int*)((char*)stat + 0x84) = in.hullHP;
}

// ---- relay (net.cpp) ----
// Send:  ShipState ss; Phase1::CaptureShipState(ss); include ss in the UDP packet alongside PlayerState.
// Recv:  read ss from the packet; Phase1::ApplyShipState(ss, g_ghostShipEnt).
//
// NOTE: the render override currently pins the LOCAL ship (g_holdShipEnt, F5 test). For the sync it must
// pin the GHOST ship (g_shipSyncEnt) instead, and the ghost must be a full second ship entity in the world
// — which is the remaining spawn work. See SHIP-SYNC-HANDOFF.md for the exact plan.
