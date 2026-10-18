#pragma once
#include <cstdint>
#include "HookMath.h"

static const int BLEND_MAX = 46;   // blend-factor slots on the wire (ground move-cycle tree = SimpleAnimBlender<46>)

// Per-context drive blocks (see phase1.cpp Capture/Apply). Ground locomotion rides the common fields
// below; contexts whose state lives elsewhere carry it here, selected by PlayerState::contextId.
#pragma pack(push, 1)

// Swim (contextId 10): the swim locomotion input read/written on HumanSwimScope (see HumanSwimFSM.h).
// Plain floats (not HookMath::Vector4) to avoid 16-byte alignment inside the packed wire struct.
struct SwimBlock {
    float    destDir[4];         // HumanSwimScope::m_DestDirection (x,y,z,w)
    float    destSpeedRatio;     // HumanSwimScope::m_DestSpeedRatio (0 = idle, up to 1)
    float    curSpeedProportion; // HumanSwimScope::m_CurSpeedProportion (eased current speed, drives stroke blend)
    float    sprint;             // HumanSwimScope::m_Sprint (shift-held, 0 or 1)
    float    aiSwim[8];          // HumanSwimScope::m_AISwim (Wait/Turn/MoveVerySlow/Slow/Fast/CatchBreath/UnderWater/DivingBell)
    // CharacterController (biped) motion vectors — the actual velocity the FSM/anim graph read (0 on a
    // teleported puppet). Syncing these lets the puppet's swim state see "real" movement.
    float    ccVelocity[4];      // CharacterController::m_Velocity
    float    ccForce[4];         // CharacterController::m_Force
    float    ccImpulse[4];       // CharacterController::m_Impulse
};

union ContextBlock {
    SwimBlock swim;
    uint8_t   _reserve[128]; // stable footprint as future context blocks are added
};

// The full transferable player state (Front A proved these reproduce movement).
struct PlayerState {
    uint32_t magic;                 // NET_MAGIC
    uint32_t protoVer;              // NET_PROTO_VER — struct/wire-layout version (both peers MUST match)
    uint32_t seq;                   // sender frame counter
    uint64_t worldId;               // ManagedObject::GetObjectID(world) — must match to show a puppet
    uint32_t sceneKey;              // packed world-cell of the player (0 = unknown) — interior/exterior gate
    uint32_t sceneCellId;           // engine's own last-load cell id (diagnosis only)
    HookMath::Transform    matrix;  // global matrix (rows 0-2 rotation, row 3 translation)
    float    curSpeed;              // HumanGroundData.m_CurSpeedRatio
    float    destSpeed;             // HumanGroundData.m_DestSpeedRatio
    HookMath::Vector4    heading;   // HumanGroundData.m_DestHeading
    bool  isSet;                    // HumanGroundData.m_IsSet
    bool  sprint;                   // HumanGroundData.m_Sprint
    bool  breakHardOnEdge;          // HumanGroundData.m_BreakHardOnEdge
    bool  breakHard;                // HumanGroundData.m_BreakHard
    uint8_t  curMvt;                // HumanGroundData.m_CurMvtType
    uint8_t  destMvt;               // HumanGroundData.m_DestMvtType
    int32_t  contextId;             // AIActor::GetCurrentContextID(human, 1): active Human context on the main
                                    // AI track. 2=ground(default), 5=Ladder, 6=InAir, 7=Ledge, 8=Walling,
                                    // 10=Swim, 30=Climbing, 31=Hanging (shared engine enum, identical PS3<->PC).
    int32_t  subState;
    int32_t  humanGroundStage;

    bool     weaponTotallyOut;
    bool     isInTree;
    bool     isOnShip;
    bool     isOnOcean;
    bool     doJumpToVoid;
    int32_t  stalkingZoneCount;
    int32_t  stalkingZoneBlendedCount;
    int32_t  stalkingClipCount;
    int32_t  stalkingClipBlendedCount;
    int32_t  stalkingClipAllowFastStalkingCount;
    int32_t  disturbedZoneCount;

    uint32_t actionAlias;           // ActionComponent current action alias on ACTLAYER_PRIMARY (the anim clip
                                    // the peer is playing) — replayed on the puppet via SetTargetAction so it
                                    // animates like the peer regardless of context. 0 = none.
    uint32_t actionChannel;         // the action's BodyPartChannel id (*(u16*)(*(Action+0x0C)+8)). Passed to
                                    // SetTargetAction as the channel instead of 0xFFFF: 0xFFFF makes the
                                    // CharacterActionSetManager re-derive it via a kit lookup that bails (no
                                    // CAS override -> Connor's base clip); the real channel takes that path.
    int32_t  actionLayer;           // the action's ACTLayer (Action::m_Layer @ 0x10); passed as the
                                    // SetTargetAction layer instead of a hardcoded ACTLAYER_PRIMARY.
    uint8_t  blendCount;            // valid slots in blendFactors (the primary controller's array size, clamped)
    float    blendFactors[BLEND_MAX];      // ch0/PRIMARY controller blend factors — the FULL array. The ground move
                                    // cycle is a 46-slot blend tree (SimpleAnimBlender<46>); the walk/jog/run
                                    // gait lives past slot 3, which is why syncing only 4 slots stayed walking.
    float    actionRatio;           // current action's normalized playback position [0,1] (GetCurrentActionRatio
                                    // ch0/PRIMARY). On an action switch the puppet seeks to it so the new clip
                                    // starts in phase instead of at frame 0 (the "snappy" swaps).
    float    blendInLen;            // peer's current evaluator blend-in length (m_BlendInEvaluator.m_ActionBlend.
                                    // m_Length). FSM-generated transitions (crouch->stand etc.) carry a custom
                                    // ActionTransition our transition-less SetTargetAction can't find (-> default
                                    // 0.2s = snappy); we copy this onto the puppet's new evaluator. <0 = unknown.
    float    blendInStartFromEnd;   // ...m_ActionBlend.m_StartTimeFromEnd
    float    blendInTargetStart;    // ...m_ActionBlend.m_TargetStartTime
    uint16_t blendInFlags;          // ...m_ActionBlend.m_Flags (low 3 bits = m_BlendType). The engine's default for
                                    // a transition-less SetTargetAction is flags 0x600 = ACTBLENDTYPE_NONE -> hard cut,
                                    // so the TYPE must be copied too, not just the length.
    uint8_t  isTransitionAction;    // peer's current evaluator is a transition clip (diagnostic)
    float    timeScale;           // ActionComponent primary-channel playback time scale (clip speed). The
                                    // loop's foot-speed rides this; without it the puppet plays the right clip
                                    // slow. Default 1.0 (NEVER 0 — that freezes the anim). GetTimeScale/SetTimeScale.

    // Context-tagged payload: the active context's own drive data, selected by `contextId`. Each context
    // whose motion/animation isn't covered by the common ground fields gets a block here; the reserve
    // keeps the packet size stable as blocks are added so both ends stay wire-compatible. Only the block
    // matching `contextId` is meaningful on any given packet.
    ContextBlock ctx;

    // ---- ship sync (filled when the player is aboard their ship) ---------------------
    bool     shipValid;    // player is aboard a ship -> the fields below are meaningful
    float    shipPos[3];   // ship world position (x,y,z) = m_GlobalMatrix.M41/42/43
    float    shipQuat[4];  // ship rotation quaternion (x,y,z,w), from the 3x3 at +0x10
    int32_t  shipHP;       // ship hull HP = *(int*)(*(void**)(ship+0x5C)+0x84)

    uint8_t  valid;                 // 1 = a live player was captured
};
#pragma pack(pop)

static const uint32_t NET_MAGIC = 0x43344341; // 'AC4C'
// Wire/struct-layout version. PlayerState is #pragma pack(1): adding a field shifts every following
// offset, so both peers MUST run the same build. Bump this whenever the layout changes.
static const uint32_t NET_PROTO_VER = 8;   // 8: EntityState carries the NPC archetype id (was _pad2)

// ---- entity sync (host-authoritative "synced worlds") -------------------------------
// One replicated entity's transform. `key` is the index in the host's enumeration; `kind`
// selects which list that index is into (0 = World::m_CharacterList, 1 = ships filtered
// from m_EntityManager.m_Entities by vtable 0x01E4A128). pos = m_GlobalMatrix.M41/M42/M43;
// quat = the 3x3 rotation (HookMath::FromMatrix).
static const uint32_t NET_ENTITY_MAGIC = 0x45344341; // 'AC4E'
static const int      MAX_SYNC_ENTITIES = 128;

#pragma pack(push, 1)
struct EntityState {
    uint32_t key;          // index in the source list
    uint8_t  kind;         // 2 = guard (soldier), 1 = ship
    uint8_t  dead;         // death state: 1 = dead (*(*(e+0xE8)+0x1C)+0xB8 & 0xC == 4)
    uint8_t  stage;        // HumanGroundStage (Fight=4 = aggro, Navigation=1 = calm)
    uint8_t  _pad;
    float    pos[3];       // world position (x,y,z)
    float    quat[4];      // world rotation (x,y,z,w)
    uint32_t actionAlias;  // current action clip (file alias); 0 = none (guards only)
    int      hullHP;       // ship hull HP (ships only; 0 for guards) - host-authoritative damage
    uint64_t objId;        // host-assigned stable identity (HandleContainer::m_ObjectID); 0 = unknown
    uint16_t hp;           // host-authoritative Life (guards); 0xFFFF = dead
    uint16_t arch;         // archetype id at behaviour+0x14: lets the peer match class-to-class
};

struct EntityPacket {
    uint32_t magic;        // NET_ENTITY_MAGIC
    uint32_t protoVer;     // NET_PROTO_VER (both peers MUST match)
    uint32_t seq;          // sender frame counter
    uint32_t count;        // valid entries in ents[]
    EntityState ents[MAX_SYNC_ENTITIES];
};
#pragma pack(pop)
