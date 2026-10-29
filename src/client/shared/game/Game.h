#pragma once
// Minimal SDK for gta_sa.exe 1.0 US (HOODLUM, 14 383 616 bytes).
//
// Addresses are cross-checked against three independent sources:
//  * gta_sa.pdb symbols shipped with the game (re/gta_sa_symbols.txt),
//  * gta-reversed (github.com/gta-reversed/gta-reversed),
//  * plugin-sdk VALIDATE_OFFSET tables (re/plugin_sdk_offsets.txt).
// Classes below are *views* over game memory: they are never constructed by us,
// only accessed through pointers obtained from the game. Field accessors use explicit offsets.
#include <cstdint>

#include "shared/game/Types.h"

namespace game {

template <typename T>
inline T& Ref(uintptr_t addr) {
    return *reinterpret_cast<T*>(addr);
}
template <typename T>
inline T& Field(const void* base, int off) {
    return *reinterpret_cast<T*>((uint8_t*)base + off);
}

// ---- calling helpers ----
template <uintptr_t A, typename R = void, typename... Args>
inline R Call(Args... args) {
    return reinterpret_cast<R(__cdecl*)(Args...)>(A)(args...);
}
template <uintptr_t A, typename R = void, typename C, typename... Args>
inline R CallMethod(C* self, Args... args) {
    return reinterpret_cast<R(__thiscall*)(C*, Args...)>(A)(self, args...);
}
template <uintptr_t A, typename R = void, typename... Args>
inline R CallStd(Args... args) {
    return reinterpret_cast<R(__stdcall*)(Args...)>(A)(args...);
}
template <int Index, typename R = void, typename C, typename... Args>
inline R CallVirtual(C* self, Args... args) {
    void** vt = *reinterpret_cast<void***>(self);
    return reinterpret_cast<R(__thiscall*)(C*, Args...)>(vt[Index])(self, args...);
}

// =====================================================================================
//  Enumerations
// =====================================================================================
enum eEntityType : uint8_t {
    ENTITY_TYPE_NOTHING = 0,
    ENTITY_TYPE_BUILDING = 1,
    ENTITY_TYPE_VEHICLE = 2,
    ENTITY_TYPE_PED = 3,
    ENTITY_TYPE_OBJECT = 4,
    ENTITY_TYPE_DUMMY = 5,
};

enum ePedType : int32_t {
    PED_TYPE_PLAYER1 = 0,
    PED_TYPE_PLAYER2 = 1,
    PED_TYPE_PLAYER_NETWORK = 2,
    PED_TYPE_PLAYER_UNUSED = 3,
    PED_TYPE_CIVMALE = 4,
    PED_TYPE_CIVFEMALE = 5,
    PED_TYPE_COP = 6,
    PED_TYPE_GANG1 = 7,  // Ballas
    PED_TYPE_GANG2 = 8,  // Grove
    PED_TYPE_GANG3 = 9,  // Vagos
    PED_TYPE_GANG4 = 10,
    PED_TYPE_GANG5 = 11,
    PED_TYPE_GANG6 = 12,
    PED_TYPE_GANG7 = 13,
    PED_TYPE_GANG8 = 14,
    PED_TYPE_GANG9 = 15,
    PED_TYPE_GANG10 = 16,
    PED_TYPE_DEALER = 17,
    PED_TYPE_MEDIC = 18,
    PED_TYPE_FIREMAN = 19,
    PED_TYPE_CRIMINAL = 20,
    PED_TYPE_BUM = 21,
    PED_TYPE_PROSTITUTE = 22,
    PED_TYPE_SPECIAL = 23,
    PED_TYPE_MISSION1 = 24,
};

enum ePedCreatedBy : uint8_t {
    PED_UNKNOWN = 0,
    PED_GAME = 1,
    PED_MISSION = 2,
    PED_GAME_MISSION = 3,
};

enum eMoveState : int32_t {
    PEDMOVE_NONE = 0,
    PEDMOVE_STILL = 1,
    PEDMOVE_TURN_L = 2,
    PEDMOVE_TURN_R = 3,
    PEDMOVE_WALK = 4,
    PEDMOVE_JOG = 5,
    PEDMOVE_RUN = 6,
    PEDMOVE_SPRINT = 7,
};

enum eGameState : int32_t {
    GAME_STATE_INITIAL = 0,
    GAME_STATE_LOGO = 1,
    GAME_STATE_PLAYING_LOGO = 2,
    GAME_STATE_TITLE = 3,
    GAME_STATE_PLAYING_INTRO = 4,
    GAME_STATE_FRONTEND_LOADING = 5,
    GAME_STATE_FRONTEND_LOADED = 6,
    GAME_STATE_FRONTEND_IDLE = 7,
    GAME_STATE_LOADING_STARTED = 8,
    GAME_STATE_IDLE = 9,  // in game
};

enum eVehicleAppearance : int32_t {
    VEHICLE_APPEARANCE_AUTOMOBILE = 1,
    VEHICLE_APPEARANCE_BIKE = 2,
    VEHICLE_APPEARANCE_HELI = 3,
    VEHICLE_APPEARANCE_BOAT = 4,
    VEHICLE_APPEARANCE_PLANE = 5,
};

// CVehicle::m_nVehicleSubClass (0x594)
enum eVehicleType : int32_t {
    VEHICLE_TYPE_AUTOMOBILE = 0,
    VEHICLE_TYPE_MTRUCK = 1,
    VEHICLE_TYPE_QUAD = 2,
    VEHICLE_TYPE_HELI = 3,
    VEHICLE_TYPE_PLANE = 4,
    VEHICLE_TYPE_BOAT = 5,
    VEHICLE_TYPE_TRAIN = 6,
    VEHICLE_TYPE_FHELI = 7,
    VEHICLE_TYPE_FPLANE = 8,
    VEHICLE_TYPE_BIKE = 9,
    VEHICLE_TYPE_BMX = 10,
    VEHICLE_TYPE_TRAILER = 11,
};

constexpr int MODEL_PLAYER = 0;
constexpr int NUM_WEAPON_SLOTS = 13;

// =====================================================================================
//  Engine structures (views)
// =====================================================================================
struct CControllerState {
    int16_t LeftStickX;      // 0x00 move left/right
    int16_t LeftStickY;      // 0x02 move forward/back
    int16_t RightStickX;     // 0x04
    int16_t RightStickY;     // 0x06
    int16_t LeftShoulder1;   // 0x08 (look behind / action)
    int16_t LeftShoulder2;   // 0x0A (prev weapon / look left)
    int16_t RightShoulder1;  // 0x0C aim / handbrake
    int16_t RightShoulder2;  // 0x0E (next weapon / look right)
    int16_t DPadUp;          // 0x10
    int16_t DPadDown;        // 0x12
    int16_t DPadLeft;        // 0x14
    int16_t DPadRight;       // 0x16
    int16_t Start;           // 0x18
    int16_t Select;          // 0x1A
    int16_t ButtonSquare;    // 0x1C jump / brake
    int16_t ButtonTriangle;  // 0x1E enter/exit vehicle
    int16_t ButtonCross;     // 0x20 sprint / accelerate
    int16_t ButtonCircle;    // 0x22 fire
    int16_t ShockButtonL;    // 0x24 crouch / horn
    int16_t ShockButtonR;    // 0x26 look behind
    int16_t m_bChatIndicated;      // 0x28
    int16_t m_bPedWalk;            // 0x2A
    int16_t m_bVehicleMouseLook;   // 0x2C
    int16_t m_bRadioTrackSkip;     // 0x2E
};
static_assert(sizeof(CControllerState) == 0x30);

struct CPad {
    CControllerState NewState;  // 0x00
    CControllerState OldState;  // 0x30
    uint8_t _pad60[0x10E - 0x60];
    uint16_t DisablePlayerControls;  // 0x10E
    uint8_t _pad110[0x118 - 0x110];
    uint8_t bApplyBrakes;               // 0x118
    uint8_t bDisablePlayerEnterCar;     // 0x119
    uint8_t bDisablePlayerDuck;         // 0x11A
    uint8_t bDisablePlayerFireWeapon;   // 0x11B
    uint8_t bDisablePlayerFireWeaponWithL1;  // 0x11C
    uint8_t bDisablePlayerCycleWeapon;  // 0x11D
    uint8_t bDisablePlayerJump;         // 0x11E
    uint8_t bDisablePlayerDisplayVitalStats;  // 0x11F
    uint8_t _pad120[0x134 - 0x120];
};
static_assert(sizeof(CPad) == 0x134);

struct CWeapon {
    int32_t type;          // 0x00 eWeaponType
    int32_t state;         // 0x04
    int32_t ammoInClip;    // 0x08
    int32_t ammoTotal;     // 0x0C
    uint32_t timeForNextShot;  // 0x10
    uint8_t f14, f15, f16, f17;
    void* fxSystem;        // 0x18
};
static_assert(sizeof(CWeapon) == 0x1C);

struct CPedClothesDesc {
    uint32_t modelKeys[10];    // 0x00
    uint32_t textureKeys[18];  // 0x28
    float fatStat;             // 0x70
    float muscleStat;          // 0x74
};
static_assert(sizeof(CPedClothesDesc) == 0x78);

// The clothes builder takes the fat from a description but the muscle from this game's stats (0x5A42B0): while
// another player's body is built, the stat holds that player's muscle (CStats::StatTypesFloat[STAT_MUSCLE])
struct MuscleFrom {
    float saved;
    explicit MuscleFrom(const CPedClothesDesc* d) : saved(Ref<float>(0xB793DC)) {
        if (d) Ref<float>(0xB793DC) = d->muscleStat;
    }
    ~MuscleFrom() { Ref<float>(0xB793DC) = saved; }
};

struct CEntity {
    // CPlaceable
    void* vtable;                       // 0x00
    CSimpleTransform& Placement() { return Field<CSimpleTransform>(this, 0x04); }
    CMatrix*& Matrix() { return Field<CMatrix*>(this, 0x14); }
    // CEntity
    void*& RwObject() { return Field<void*>(this, 0x18); }
    uint32_t& Flags() { return Field<uint32_t>(this, 0x1C); }
    uint16_t& ModelIndex() { return Field<uint16_t>(this, 0x22); }
    int8_t& AreaCode() { return Field<int8_t>(this, 0x2F); }  // interior id
    uint8_t Type() { return Field<uint8_t>(this, 0x36) & 7; }
    uint8_t Status() { return Field<uint8_t>(this, 0x36) >> 3; }
    void SetStatus(uint8_t s) { Field<uint8_t>(this, 0x36) = (uint8_t)((Field<uint8_t>(this, 0x36) & 7) | (s << 3)); }

    bool IsVisible() { return (Flags() >> 7) & 1; }
    void SetStreamingDontDelete(bool v) {
        if (v) Flags() |= (1u << 10);
        else Flags() &= ~(1u << 10);
    }

    CVector GetPosition() {
        CMatrix* m = Matrix();
        return m ? m->pos : Placement().pos;
    }
    float GetHeading() {
        CMatrix* m = Matrix();
        if (m) return std::atan2(-m->forward.x, m->forward.y);
        return Placement().heading;
    }
    void SetPositionRaw(const CVector& p) {
        if (CMatrix* m = Matrix()) m->pos = p;
        else Placement().pos = p;
    }
    bool IsPed() { return Type() == ENTITY_TYPE_PED; }
    bool IsVehicle() { return Type() == ENTITY_TYPE_VEHICLE; }
    bool IsObject() { return Type() == ENTITY_TYPE_OBJECT; }
};

struct CPhysical : CEntity {
    uint32_t& PhysicalFlags() { return Field<uint32_t>(this, 0x40); }
    CVector& MoveSpeed() { return Field<CVector>(this, 0x44); }
    CVector& TurnSpeed() { return Field<CVector>(this, 0x50); }
    float& Mass() { return Field<float>(this, 0x8C); }
    CPhysical*& AttachedTo() { return Field<CPhysical*>(this, 0xFC); }

    // physical flag bits
    enum : uint32_t {
        PF_BULLET_PROOF = 1u << 18,
        PF_FIRE_PROOF = 1u << 19,
        PF_COLLISION_PROOF = 1u << 20,
        PF_MELEE_PROOF = 1u << 21,
        PF_INVULNERABLE = 1u << 22,
        PF_EXPLOSION_PROOF = 1u << 23,
    };
    void SetProofs(bool bullet, bool fire, bool explosion, bool collision, bool melee) {
        uint32_t& f = PhysicalFlags();
        auto set = [&](uint32_t bit, bool on) { f = on ? (f | bit) : (f & ~bit); };
        set(PF_BULLET_PROOF, bullet);
        set(PF_FIRE_PROOF, fire);
        set(PF_EXPLOSION_PROOF, explosion);
        set(PF_COLLISION_PROOF, collision);
        set(PF_MELEE_PROOF, melee);
    }
};

struct CVehicle;

struct CPed : CPhysical {
    uint32_t& PedFlags() { return Field<uint32_t>(this, 0x46C); }
    uint32_t& PedFlags2() { return Field<uint32_t>(this, 0x470); }
    uint32_t& PedFlags3() { return Field<uint32_t>(this, 0x474); }
    uint32_t& PedFlags4() { return Field<uint32_t>(this, 0x478); }
    void*& Intelligence() { return Field<void*>(this, 0x47C); }
    void*& PlayerData() { return Field<void*>(this, 0x480); }  // CPlayerPedData*, only for CPlayerPed
    uint8_t& CreatedBy() { return Field<uint8_t>(this, 0x484); }
    int32_t& PedState() { return Field<int32_t>(this, 0x530); }
    int32_t& MoveState() { return Field<int32_t>(this, 0x534); }
    float& Health() { return Field<float>(this, 0x540); }
    float& MaxHealth() { return Field<float>(this, 0x544); }
    int16_t& Money() { return Field<int16_t>(this, 0x756); }
    float& Armour() { return Field<float>(this, 0x548); }
    float& CurrentRotation() { return Field<float>(this, 0x558); }
    float& AimingRotation() { return Field<float>(this, 0x55C); }
    CVehicle*& Vehicle() { return Field<CVehicle*>(this, 0x58C); }
    int32_t& PedType() { return Field<int32_t>(this, 0x598); }
    CWeapon* Weapons() { return &Field<CWeapon>(this, 0x5A0); }
    uint8_t& ActiveWeaponSlot() { return Field<uint8_t>(this, 0x718); }
    uint8_t& WeaponAccuracy() { return Field<uint8_t>(this, 0x71A); }
    CEntity*& TargetedObject() { return Field<CEntity*>(this, 0x71C); }
    uint8_t& WeaponSkill() { return Field<uint8_t>(this, 0x72C); }
    uint8_t& FightingStyle() { return Field<uint8_t>(this, 0x72D); }

    bool InVehicle() { return (PedFlags() >> 8) & 1; }
    bool IsDucking() { return (PedFlags() >> 26) & 1; }
    bool IsInTheAir() { return (PedFlags() >> 9) & 1; }
    CVehicle* VehicleIfIn() { return InVehicle() ? Vehicle() : nullptr; }
    CWeapon& ActiveWeapon() { return Weapons()[ActiveWeaponSlot()]; }
    bool IsPlayer() { return PedType() == PED_TYPE_PLAYER1 || PedType() == PED_TYPE_PLAYER2; }
};

struct CPlayerPed : CPed {
    CPedClothesDesc* ClothesDesc() {
        void* pd = PlayerData();
        return pd ? Field<CPedClothesDesc*>(pd, 0x04) : nullptr;
    }
    float& LookPitch() { return Field<float>(PlayerData(), 0x54); }
    float& MoveBlendRatio() { return Field<float>(PlayerData(), 0x14); }
    void*& Wanted() { return Field<void*>(PlayerData(), 0x00); }
};

struct CVehicle : CPhysical {
    uint32_t& VehicleFlagsLo() { return Field<uint32_t>(this, 0x428); }
    uint32_t& VehicleFlagsHi() { return Field<uint32_t>(this, 0x42C); }
    uint8_t* Colors() { return &Field<uint8_t>(this, 0x434); }  // primary, secondary, tertiary, quaternary
    uint16_t* Upgrades() { return &Field<uint16_t>(this, 0x43A); }  // 15 slots
    CPed*& Driver() { return Field<CPed*>(this, 0x460); }
    CPed** Passengers() { return &Field<CPed*>(this, 0x464); }  // 8
    uint8_t& NumPassengers() { return Field<uint8_t>(this, 0x484); }
    uint8_t& MaxPassengers() { return Field<uint8_t>(this, 0x488); }
    float& SteerAngle() { return Field<float>(this, 0x494); }
    float& GasPedal() { return Field<float>(this, 0x49C); }
    float& BrakePedal() { return Field<float>(this, 0x4A0); }
    uint8_t& CreatedBy() { return Field<uint8_t>(this, 0x4A4); }
    float& Health() { return Field<float>(this, 0x4C0); }
    CVehicle*& Trailer() { return Field<CVehicle*>(this, 0x4C8); }
    int32_t& DoorLock() { return Field<int32_t>(this, 0x4F8); }
    int32_t& VehicleClass() { return Field<int32_t>(this, 0x590); }  // the base eVehicleType (0x594 is the exact one)
    int32_t& VehicleSubClass() { return Field<int32_t>(this, 0x594); }  // eVehicleType
    int16_t& RemapTxd() { return Field<int16_t>(this, 0x59A); }

    bool EngineOn() { return (VehicleFlagsLo() >> 4) & 1; }
    void SetEngineOn(bool v) { v ? VehicleFlagsLo() |= (1u << 4) : VehicleFlagsLo() &= ~(1u << 4); }
    bool LightsOn() { return (VehicleFlagsLo() >> 6) & 1; }
    bool SirenOn() { return (VehicleFlagsHi() >> 15) & 1; }  // bit 47 overall
    void SetSiren(bool v) { v ? VehicleFlagsHi() |= (1u << 15) : VehicleFlagsHi() &= ~(1u << 15); }
    bool IsBike() { return VehicleSubClass() == VEHICLE_TYPE_BIKE || VehicleSubClass() == VEHICLE_TYPE_BMX; }
    bool IsTrain() { return VehicleSubClass() == VEHICLE_TYPE_TRAIN; }
};

struct CObject : CPhysical {};

struct CPlayerInfo {
    CPlayerPed* ped;  // 0x00
    uint8_t _pad04[0xB8 - 0x04];
    int32_t money;         // 0xB8
    int32_t displayMoney;  // 0xBC
    uint8_t _padC0[0xDC - 0xC0];
    uint8_t playerState;   // 0xDC
    uint8_t _padDD[0x14F - 0xDD];
    uint8_t maxHealth;     // 0x14F
    uint8_t maxArmour;     // 0x150
    uint8_t _pad151[0x190 - 0x151];
};
static_assert(sizeof(CPlayerInfo) == 0x190);

// CPool<T> header (0x14 bytes)
struct CPoolHeader {
    uint8_t* objects;
    int8_t* byteMap;  // bit 7 set = slot free; low 7 bits = reference counter
    int32_t size;
    int32_t firstFree;
    bool ownsAllocations;
    bool locked;
};

// =====================================================================================
//  Globals
// =====================================================================================
namespace addr {
constexpr uintptr_t Players = 0xB7CD98;          // CWorld::Players[2]
constexpr uintptr_t PlayerInFocus = 0xB7CD74;    // CWorld::PlayerInFocus (int8)
constexpr uintptr_t TimeInMs = 0xB7CB84;         // CTimer::m_snTimeInMilliseconds
constexpr uintptr_t TimeStep = 0xB7CB5C;         // CTimer::ms_fTimeStep
constexpr uintptr_t FrameCounter = 0xB7CB4C;     // CTimer::m_FrameCounter
constexpr uintptr_t UserPause = 0xB7CB49;        // CTimer::m_UserPause
constexpr uintptr_t CodePause = 0xB7CB48;        // CTimer::m_CodePause
constexpr uintptr_t TheCamera = 0xB6F028;
constexpr uintptr_t GameState = 0xC8D4C0;        // gGameState
constexpr uintptr_t MenuManager = 0xBA6748;      // FrontEndMenuManager
constexpr uintptr_t RsGlobal = 0xC17040;
constexpr uintptr_t D3DDevice = 0xC97C28;        // _RwD3DDevice (IDirect3DDevice9*)
constexpr uintptr_t Pads = 0xB73458;             // CPad::Pads[2]
constexpr uintptr_t NewKeyState = 0xB73190;      // CPad::NewKeyState
constexpr uintptr_t OldKeyState = 0xB72F20;
constexpr uintptr_t PedPool = 0xB74490;
constexpr uintptr_t VehiclePool = 0xB74494;
constexpr uintptr_t ObjectPool = 0xB7449C;
constexpr uintptr_t ModelInfoPtrs = 0xA9B0C8;    // CModelInfo::ms_modelInfoPtrs
constexpr uintptr_t StreamingInfo = 0x8E4CC0;    // CStreaming::ms_aInfoForModel (0x14 bytes each)
constexpr uintptr_t ForegroundApp = 0x8D621C;
constexpr uintptr_t ScriptSpace = 0xA49960;      // CTheScripts::ScriptSpace
}  // namespace addr

inline CPlayerInfo* PlayerInfos() { return reinterpret_cast<CPlayerInfo*>(addr::Players); }
inline int8_t& PlayerInFocus() { return Ref<int8_t>(addr::PlayerInFocus); }
inline uint32_t TimeMs() { return Ref<uint32_t>(addr::TimeInMs); }
inline float TimeStep() { return Ref<float>(addr::TimeStep); }
inline int32_t GameState() { return Ref<int32_t>(addr::GameState); }
inline bool MenuActive() { return Ref<uint8_t>(addr::MenuManager + 0x5C) != 0; }
inline void* D3DDevice() { return Ref<void*>(addr::D3DDevice); }
inline CPad* GetPad(int i) { return reinterpret_cast<CPad*>(addr::Pads) + i; }
inline bool IsForeground() { return Ref<uint8_t>(addr::ForegroundApp) != 0; }

// RsGlobal.ps->window (HWND). RsGlobal: appName(0) maximumWidth(4) maximumHeight(8) frameLimit(C)
// quit(10) ps(14). PsGlobalType: window(0).
inline void* GameWindow() {
    void* ps = Ref<void*>(addr::RsGlobal + 0x14);
    return ps ? Ref<void*>((uintptr_t)ps) : nullptr;
}
inline int ScreenWidth() { return Ref<int32_t>(0xC17044); }   // RsGlobal.maximumWidth
inline int ScreenHeight() { return Ref<int32_t>(0xC17048); }  // RsGlobal.maximumHeight

// TheCamera (CCamera @0xB6F028) and its active CCam (0x238 bytes each, array at +0x174)
namespace cam {
inline uint8_t& ActiveCamIndex() { return Ref<uint8_t>(addr::TheCamera + 0x59); }
inline uintptr_t Active() { return addr::TheCamera + 0x174 + (uintptr_t)ActiveCamIndex() * 0x238; }
inline uint16_t& Mode() { return Ref<uint16_t>(Active() + 0x0C); }
inline float& Fov() { return Ref<float>(Active() + 0xB4); }
inline float& Alpha() { return Ref<float>(Active() + 0xAC); }  // m_fVerticalAngle
inline float& Beta() { return Ref<float>(Active() + 0xBC); }   // m_fHorizontalAngle
inline CVector& Front() { return Ref<CVector>(Active() + 0x190); }
inline CVector& Source() { return Ref<CVector>(Active() + 0x19C); }
inline CVector& Up() { return Ref<CVector>(Active() + 0x1B4); }
inline float& Orientation() { return Ref<float>(addr::TheCamera + 0x150); }   // CCamera::m_fOrientation
inline uint16_t& WeaponMode() { return Ref<uint16_t>(addr::TheCamera + 0x830); }  // m_PlayerWeaponMode.m_nMode
inline uint8_t& UseMouse3rdPerson() { return Ref<uint8_t>(0xB6EC2E); }
inline float& CrosshairMultX() { return Ref<float>(0xB6EC14); }
inline float& CrosshairMultY() { return Ref<float>(0xB6EC10); }
inline bool WideScreenOn() { return Ref<uint8_t>(addr::TheCamera + 0x3D) != 0; }
}  // namespace cam

// =====================================================================================
//  Functions
// =====================================================================================
inline CPlayerPed* FindPlayerPed(int id = -1) { return Call<0x56E210, CPlayerPed*>(id); }
inline CVehicle* FindPlayerVehicle(int id = -1, bool includeRemote = false) {
    return Call<0x56E0D0, CVehicle*>(id, includeRemote);
}

CPoolHeader* PedPool();
CPoolHeader* VehiclePool();
CPoolHeader* ObjectPool();
constexpr int kPedPoolElemSize = 0x7C4;
constexpr int kVehiclePoolElemSize = 0xA18;
constexpr int kObjectPoolElemSize = 0x19C;

int GetPedRef(CPed* ped);
CPed* GetPed(int handle);
int GetVehicleRef(CVehicle* v);
CVehicle* GetVehicle(int handle);
int GetObjectRef(CObject* o);
CObject* GetObject(int handle);
// Checks that a pointer still refers to a live entity in the corresponding pool.
bool IsPedValid(CPed* ped);
bool IsVehicleValid(CVehicle* v);
bool IsObjectValid(CObject* o);
// The slots the copies leave to the game. A pool's new gives null when it is full and the script commands that create
// people, cars and objects crash on it (CREATE_CHAR at 0x467654), a mission's too; the game itself frees its own random
// people and cars below 8 free slots, never the copies (they are script entities).
constexpr int kKeepPeds = 12, kKeepVehicles = 10, kKeepObjects = 20;
bool HasRoom(CPoolHeader* pool, int keep, const char* what);  // more than keep slots free, or a warning now and then

void RequestModel(int model, int flags = 2 /* STREAMING_GAME_REQUIRED: MarkModelNotNeeded clears it */);
void LoadAllRequestedModels(bool onlyPriority = false);
bool IsModelLoaded(int model);
int ModelLoadState(int model);  // 0 not asked for, 1 in memory, 2 asked for, 3 being read, 4 finishing; -1 no such model
bool LoadModelNow(int model);  // request + blocking load
void MarkModelNotNeeded(int model);
bool IsValidModel(int model);
bool IsVehicleModel(int model);
bool IsPedModel(int model);
bool IsObjectModel(int model);  // known here and neither a ped nor a vehicle: what CREATE_OBJECT and pickups can use

void WorldAdd(CEntity* e);
void WorldRemove(CEntity* e);

// Moves an entity without physics (matrix + RenderWare frame).
void SetEntityPosition(CEntity* e, const CVector& pos);
// The light of an interior, as going through its door sets it (CEntryExit::TransitionFinished: the timecycle's extra
// colour of the interior's own entrance nearest to pos), or none outside. For players moved there by our code.
void LightArea(int area, const CVector& pos);
// The street door into interior `area` whose inside door is in the same building as `inside` (one area holds many: all
// the Bincos), the one nearest `hint` (where the player came from): a CEntryExit, or null. Going through it sets it on
// the ped (CPed::m_pEnex: the radar centres on it, a death inside comes back by the hospital nearest it, the interior
// manager starts the shop's scripts).
void* StreetDoorOf(int area, const CVector& inside, const CVector& hint);
CVector DoorSpot(void* enex);  // the middle of its entrance
// World -> screen (game screen space = back buffer pixels). Returns false when behind the camera.
bool WorldToScreen(const CVector& world, float& sx, float& sy, float& depth);
// CPlayerInfo of a player ped (handles our remote player outside CWorld::Players).
inline CPlayerInfo* PlayerInfo(int slot) { return PlayerInfos() + slot; }

}  // namespace game
