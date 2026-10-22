#include "entities/player/Players.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Players& Players::Instance() {
    static Players p;
    return p;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Players::Instance()); }
} g_reg;

constexpr int kRemoteSlot = 1;  // CWorld::Players index used for the remote player while it is processed
constexpr int kLastWeapon = 46;  // eWeaponType: the parachute
constexpr int kCellphone = 330;  // MODEL_CELLPHONE
constexpr float kTeleportDist = 5.0f;

// ------------------------------------------------------------------ context switching
// CStats::StatTypesInt (0xB79000, 223 ints) followed by CStats::StatTypesFloat (0xB79380, 82 floats): the remote
// player's skills, stamina, fat... while it is processed. Its running, swimming and shooting advance the progress
// counters after them (m_SprintStaminaCounter.. 0xB794D8, UpdateFatAndMuscleStats' state 0xB79534): those stay ours,
// and so do the stat update boxes (CStats::bShowUpdateStats, 0x8CDE56: "Stamina +", "Respect - Upgraded" texts).
constexpr uintptr_t kStatsBlock = 0xB79000;
constexpr size_t kStatsBlockSize = 0x4C8, kStatsKeptSize = 0x538;
constexpr uintptr_t kShowUpdateStats = 0x8CDE56;

// The remote player is a PLAYER1 ped as well, and what its processing does to the player's aim stays its own:
// CCamera::m_PlayerWeaponMode (cleared when it does not aim), m_vecAimingTargetCoors, and the lock-on crosshair
// CWeaponEffects gCrossHair[0] (the engine picks it by the ped type)
constexpr uintptr_t kCamWeaponMode = addr::TheCamera + 0x830, kCamAimingCoors = addr::TheCamera + 0x854;
constexpr uintptr_t kCrossHair = 0xC8A838;
// Goggles it puts on switch the screen's night or thermal vision (CPed::AddGogglesModel points the ped's
// m_pGogglesState, +0x500, at CPostEffects::m_bNightVision / m_bInfraredVision): the screen is this game's player's
constexpr uintptr_t kVision = 0xC402B8;
// its sniper shots shake the camera (CamShakeNoPos: TheCamera +0x12C force, +0x5C start) and every gun the pads
constexpr uintptr_t kCamShake = addr::TheCamera + 0x12C, kCamShakeStart = addr::TheCamera + 0x5C;

struct LocalContext {
    uint8_t stats[kStatsKeptSize];
    uint8_t showUpdateStats;
    CPad pads[2];  // whole: buttons, controls, shake
    int8_t focus;
    CPlayerPed* slotPed;
    uint16_t camMode;
    uint8_t weaponMode[12], aimingCoors[12], crossHair[0x2C], vision[2];
    float shake;
    uint32_t shakeStart;
    float myHealth;  // an ambulance it gets into heals "the player" (CVehicle::SetDriver's freebies: Players[0])
    float fov, orientation, alpha, beta;
    CVector front, source, up;
    uint8_t mouse3rd;
    float chairX, chairY;
};

void SaveLocal(LocalContext& c) {
    memcpy(c.stats, (void*)kStatsBlock, kStatsKeptSize);
    c.showUpdateStats = Ref<uint8_t>(kShowUpdateStats);
    memcpy(c.pads, GetPad(0), sizeof c.pads);
    c.focus = PlayerInFocus();
    c.slotPed = PlayerInfo(kRemoteSlot)->ped;
    c.camMode = cam::Mode();
    memcpy(c.weaponMode, (void*)kCamWeaponMode, sizeof c.weaponMode);
    memcpy(c.aimingCoors, (void*)kCamAimingCoors, sizeof c.aimingCoors);
    memcpy(c.crossHair, (void*)kCrossHair, sizeof c.crossHair);
    memcpy(c.vision, (void*)kVision, sizeof c.vision);
    c.shake = Ref<float>(kCamShake);
    c.shakeStart = Ref<uint32_t>(kCamShakeStart);
    c.myHealth = PlayerInfo(0)->ped ? PlayerInfo(0)->ped->Health() : 0.0f;
    c.fov = cam::Fov();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.front = cam::Front();
    c.source = cam::Source();
    c.up = cam::Up();
    c.mouse3rd = cam::UseMouse3rdPerson();
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();
}

void RestoreLocal(const LocalContext& c) {
    memcpy((void*)kStatsBlock, c.stats, kStatsKeptSize);
    Ref<uint8_t>(kShowUpdateStats) = c.showUpdateStats;
    memcpy(GetPad(0), c.pads, sizeof c.pads);
    PlayerInFocus() = c.focus;
    PlayerInfo(kRemoteSlot)->ped = c.slotPed;
    cam::Mode() = c.camMode;
    memcpy((void*)kCamWeaponMode, c.weaponMode, sizeof c.weaponMode);
    memcpy((void*)kCamAimingCoors, c.aimingCoors, sizeof c.aimingCoors);
    memcpy((void*)kCrossHair, c.crossHair, sizeof c.crossHair);
    memcpy((void*)kVision, c.vision, sizeof c.vision);
    Ref<float>(kCamShake) = c.shake;
    Ref<uint32_t>(kCamShakeStart) = c.shakeStart;
    if (PlayerInfo(0)->ped) PlayerInfo(0)->ped->Health() = c.myHealth;
    cam::Fov() = c.fov;
    cam::Orientation() = c.orientation;
    cam::Alpha() = c.alpha;
    cam::Beta() = c.beta;
    cam::Front() = c.front;
    cam::Source() = c.source;
    cam::Up() = c.up;
    cam::UseMouse3rdPerson() = c.mouse3rd;
    cam::CrosshairMultX() = c.chairX;
    cam::CrosshairMultY() = c.chairY;
}

void ApplyRemote(RemotePlayer& rp) {
    if (rp.stats.size() == kStatsBlockSize) memcpy((void*)kStatsBlock, rp.stats.data(), kStatsBlockSize);  // its skills, stamina...
    Ref<uint8_t>(kShowUpdateStats) = 0;
    CPad* p0 = GetPad(0);
    CPad* p1 = GetPad(1);
    p0->NewState = p1->NewState = rp.padNew;
    p0->OldState = p1->OldState = rp.padOld;
    p0->DisablePlayerControls = 0;
    memset(&p0->bApplyBrakes, 0, 8);
    PlayerInFocus() = (int8_t)kRemoteSlot;
    PlayerInfo(kRemoteSlot)->ped = rp.ped;
    const CameraState& c = rp.state.cam;
    cam::Mode() = c.mode;
    cam::WeaponMode() = c.weaponMode;
    cam::Fov() = c.fov;
    cam::Orientation() = c.orientation;
    cam::Alpha() = c.alpha;
    cam::Beta() = c.beta;
    cam::Front() = c.front;
    cam::Source() = rp.ped->GetPosition() + c.sourceOffset;
    cam::Up() = c.up;
    cam::UseMouse3rdPerson() = c.useMouse3rd ? 1 : 0;
    cam::CrosshairMultX() = c.chairX;
    cam::CrosshairMultY() = c.chairY;
    if (rp.ped->PlayerData()) rp.ped->LookPitch() = c.lookPitch;
}

// A driver's drive-by (CVehicle::DoDriveByShootings: looking aside and firing out of the window) runs only for a car of
// STATUS_PLAYER, and the remote player's car is a mirror here: it runs for it under its pad and camera. The shots only
// show: the shooter's game reports its hits.
void DriveBy(CPlayerPed* ped) {
    CVehicle* v = ped->InVehicle() ? ped->Vehicle() : nullptr;
    if (!v || v->Driver() != ped) return;
    uint8_t& typeStatus = Field<uint8_t>(v, 0x36);
    uint8_t saved = typeStatus;
    typeStatus &= 0x07;  // m_nStatus (bits 3..7): STATUS_PLAYER
    // with the usual car camera the side comes from the camera's looking left/right, and the camera is ours: the
    // remote player's look buttons set it for the call
    auto* camLook = (uint8_t*)(cam::Active() + 0x08);
    uint8_t look[2] = {camLook[0], camLook[1]};
    const CControllerState& pad = GetPad(0)->NewState;
    camLook[0] = pad.LeftShoulder2 && !pad.RightShoulder2;
    camLook[1] = pad.RightShoulder2 && !pad.LeftShoulder2;
    int clip = ped->ActiveWeapon().ammoInClip;
    CallMethod<0x741FD0>(v);
    camLook[0] = look[0], camLook[1] = look[1];
    typeStatus = saved;
    if (ped->ActiveWeapon().ammoInClip != clip) LOGD("Remote player drive-by: a shot");
}

// A passenger's drive-by is the game's split-screen mode's (CTaskSimpleCarDrive::ProcessPed 0x6449AB: a player in a
// passenger seat leans out of the window, CTaskSimpleGangDriveBy). In co-op it starts for a passenger holding the right
// mouse button (R1 on a pad) with any gun in hand, not only an SMG (the task takes any instant-hit weapon), and ends
// when the button is let go. The checks of the SMG slot and of the split-screen camera after the call are dropped.
// The player processed is the one in focus: the other player's copy is processed under its own pad, so its drive-by
// shows here too (its game folds the mouse button into R1, FillCommon).
std::map<CPed*, bool> g_ourDriveBy;  // passengers whose drive-by this started (a mission's is left alone)
bool AimFromSeat(const CPad* pad) { return pad->NewState.RightShoulder1 || pad->NewState.m_bVehicleMouseLook; }

bool __cdecl PassengerDriveBy() {
    const CPad* pad = GetPad(0);
    CPlayerPed* ped = FindPlayerPed(-1);
    if (!Net().Online() || !ped || pad->DisablePlayerControls || !AimFromSeat(pad) || cam::Mode() == 0x37) return false;  // MODE_AIMWEAPON_FROMCAR
    const CWeapon& w = ped->ActiveWeapon();
    void* info = w.type ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
    if (!info || Field<int32_t>(info, 0) != 1 || w.ammoTotal <= 0) return false;  // m_nWeaponFire: FIRE_INSTANT_HIT
    g_ourDriveBy[ped] = true;
    return true;
}

using GangDriveByFn = bool(__fastcall*)(void*, void*, CPed*);
GangDriveByFn g_origGangDriveBy = nullptr;
bool __fastcall HookGangDriveByProcess(void* task, void* edx, CPed* ped) {
    auto ours = g_ourDriveBy.find(ped);
    // let go: the task's own way out, command 7 (m_nNextCommand; a player's pad sets only 1 and 2 below 4). The rest of
    // the lean-out animation brings the passenger back in, and the task finishes when it is over. Finishing it at once
    // (m_bIsFinished) left the passenger leaning out of the window with the gun until two clicks.
    if (ours != g_ourDriveBy.end() && !AimFromSeat(GetPad(0))) Field<int8_t>(task, 0x0F) = 7;
    bool done = g_origGangDriveBy(task, edx, ped);
    if (done && ours != g_ourDriveBy.end()) g_ourDriveBy.erase(ours);
    return done;
}

// Two players dead or busted at about the same time come back at the same hospital or police station door, one inside
// the other: the one whose game was done first stood stuck in the other, still in its respawn view, until that ended.
// Ours comes back 1.5 m to the right when the other player's copy is at that door already (both at once: KeepApart).
using RestoreFn = void(__cdecl*)(CPlayerPed*, CVector, float);
RestoreFn g_origRestore = nullptr;
void __cdecl HookRestorePlayer(CPlayerPed* player, CVector pos, float heading) {
    if (Net().Online() && player == FindPlayerPed(0))
        for (auto& [id, rp] : Players::Instance().All()) {
            if (!rp.ped) continue;
            CVector d = rp.ped->GetPosition() - pos;
            if (d.Length2D() > 1.5f || std::fabs(d.z) > 3.0f) continue;
            float h = heading * 0.0174533f;  // degrees: 0 faces north, the right is (cos, sin)
            pos = pos + CVector{std::cos(h), std::sin(h), 0.0f} * 1.5f;
            LOGI("Respawn 1.5 m aside: player %d stands at the door", id);
            break;
        }
    g_origRestore(player, pos, heading);
}

// DELETE_CHAR deletes whoever it is given (CTheScripts::RemoveThisPed). A mission that clears "the player's car" for
// the people it brings deletes its passengers: in "Sweet's Girl" the host rode with the partner driving, the host's own
// CJ was deleted, and the host's game crashed on its next look at him (the partner's screen stayed dark). A player, ours
// or the other one's copy, only gets out of the car; vanilla scripts never delete a player.
using RemovePedFn = void(__cdecl*)(CPed*);
RemovePedFn g_origRemoveThisPed = nullptr;
void __cdecl HookDeleteChar(CPed* ped) {
    if (ped && ped->IsPlayer()) {
        if (ped->InVehicle()) {
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        LOGW("A script deleted a player (%s): out of its car instead", ped == FindPlayerPed(0) ? "ours" : "a copy");
        return;
    }
    g_origRemoveThisPed(ped);
}

using ProcessControlFn = void(__fastcall*)(CPlayerPed*, void*);
ProcessControlFn g_origProcessControl = nullptr;
bool g_inRemoteContext = false;

void __fastcall HookPlayerProcessControl(CPlayerPed* self, void* edx) {
    RemotePlayer* rp = g_inRemoteContext ? nullptr : Players::Instance().FindByPed(self);
    if (!rp) {
        g_origProcessControl(self, edx);
        return;
    }
    LocalContext saved;
    SaveLocal(saved);
    ApplyRemote(*rp);
    g_inRemoteContext = true;
    g_origProcessControl(self, edx);
    DriveBy(self);  // still under its pad and camera
    g_inRemoteContext = false;
    RestoreLocal(saved);
    Field<bool*>(self, 0x500) = nullptr;  // CPed::m_pGogglesState: taking its goggles off must not switch ours off
}

// The remote player's gang work (CPlayerPed::ProcessGroupBehaviour, called from its ProcessControl): recruiting this
// game's Grove members with its buttons, and the Grove gang's hate for cops from its wanted level here, which is not
// its real one. Its own game runs its gang.
using GroupBehaviourFn = void(__fastcall*)(CPlayerPed*, void*, CPad*);
GroupBehaviourFn g_origGroupBehaviour = nullptr;
void __fastcall HookGroupBehaviour(CPlayerPed* self, void* edx, CPad* pad) {
    if (!g_inRemoteContext) g_origGroupBehaviour(self, edx, pad);
}

// Its photos are its game's (a snapshot it gets there comes over as a pickup): here CWeapon::TakePhotograph flashed
// this screen, saved it to this player's gallery and paid this player for a snapshot in this view
using TakePhotoFn = bool(__fastcall*)(void*, void*, CEntity*, CVector*);
TakePhotoFn g_origTakePhoto = nullptr;
bool __fastcall HookTakePhotograph(void* weapon, void* edx, CEntity* owner, CVector* point) {
    return !g_inRemoteContext && g_origTakePhoto(weapon, edx, owner, point);
}

using GetInfoFn = CPlayerInfo*(__fastcall*)(CPlayerPed*, void*);
GetInfoFn g_origGetPlayerInfo = nullptr;
CPlayerInfo* __fastcall HookGetPlayerInfoForThisPlayerPed(CPlayerPed* self, void* edx) {
    if (Players::Instance().FindByPed(self)) return PlayerInfo(kRemoteSlot);
    return g_origGetPlayerInfo(self, edx);
}

// Every PLAYER1 ped gets real-time shadow number 0, and the remote player is one too (enemies attack it). The two
// players took that shadow from each other, and the one left holding it without owning it crashed the game when it was
// deleted: CPhysical::~CPhysical returns the shadow to its owner (a world load, the game's exit). The remote player
// takes a free shadow like any other ped.
using GetShadowFn = void*(__fastcall*)(void*, void*, CPed*);
GetShadowFn g_origGetShadow = nullptr;
void* __fastcall HookGetRealTimeShadow(void* manager, void* edx, CPed* e) {  // CRealTimeShadowManager::GetRealTimeShadow
    bool remote = e->IsPed() && e->PedType() == PED_TYPE_PLAYER1 && e != FindPlayerPed(0);
    if (remote) e->PedType() = PED_TYPE_PLAYER2;
    void* shadow = g_origGetShadow(manager, edx, e);
    if (remote) e->PedType() = PED_TYPE_PLAYER1;
    return shadow;
}

// Every vehicle's sound (CAEVehicleAudioEntity::Service) takes a PLAYER1 driver for the player: the remote player's
// car turned this game's radio on and took over the player's engine sounds, and with both players driving the two
// cars fought over one radio. For its sound the remote driver is not a player.
using VehicleAudioFn = void(__fastcall*)(void*, void*);
VehicleAudioFn g_origVehicleAudio = nullptr;
void __fastcall HookVehicleAudioService(void* audio, void* edx) {
    auto* veh = Field<CVehicle*>(audio, 0x4);  // CAEAudioEntity::m_pEntity
    CPed* driver = veh ? veh->Driver() : nullptr;
    bool remote = driver && driver->PedType() == PED_TYPE_PLAYER1 && driver != FindPlayerPed(0);
    if (remote) driver->PedType() = PED_TYPE_PLAYER_NETWORK;
    g_origVehicleAudio(audio, edx);
    if (remote) driver->PedType() = PED_TYPE_PLAYER1;
}

// The other players' weapon icon goes next to the health bar over their heads. The HUD's sprite code draws it right
// after the HUD: the icons are textures of the weapon models' dictionaries ("<model>ICON"), the fist is a HUD sprite.
void DrawWeaponIcon(int weapon, float x, float y, float w, float h, uint8_t alpha) {
    if (weapon < 0 || weapon > kLastWeapon) return;  // from the network
    struct { void* texture; } sprite{};  // CSprite2d
    void* info = Call<0x743C60, void*>(weapon, (uint8_t)1);  // CWeaponInfo::GetWeaponInfo, standard skill
    int model = info ? Field<int32_t>(info, 0x0C) : -1;     // m_nModelId1
    if (model <= 0) {
        sprite.texture = Ref<void*>(0xBAB1FC);  // CHud::Sprites[SPRITE_FIST]
    } else {
        void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)model * 4);
        auto* txds = Ref<CPoolHeader*>(0xC8800C);  // CTxdStore::ms_pTxdPool
        int slot = mi ? Field<int16_t>(mi, 0x0A) : -1;  // m_nTxdIndex
        if (!txds || slot < 0 || slot >= txds->size || txds->byteMap[slot] < 0) return;
        void* dict = Ref<void*>((uintptr_t)txds->objects + (uintptr_t)slot * 0xC);  // TxdDef::m_pRwDictionary
        uint32_t key = Call<0x53CF70, uint32_t>(Field<uint32_t>(mi, 0x04), "ICON");  // CKeyGen::AppendStringToKey
        if (dict) sprite.texture = Call<0x734E50, void*>(dict, key);               // RwTexDictionaryFindHashNamedTexture
    }
    if (!sprite.texture) return;
    float rect[4] = {x, y + h, x + w, y};  // CRect: left, bottom, right, top
    CRGBA color{255, 255, 255, alpha};
    CallMethod<0x728350>(&sprite, rect, &color);  // CSprite2d::Draw(const CRect&, const CRGBA&)
}

// and its wanted level left of the health bar: the HUD's own stars (CHud::DrawWanted: "]" of FONT_GOTHIC in gold),
// smaller. CFont::DrawFonts prints them right after the HUD.
void DrawWantedStars(int level, float right, float midY, uint8_t alpha) {
    if (level <= 0) return;
    float scaleX = ScreenWidth() / 640.0f * 0.605f * 0.6f, scaleY = ScreenHeight() / 448.0f * 1.21f * 0.6f;
    CRGBA gold = Ref<CRGBA>(0xBAB22C + 6 * 4);  // CHudColours HudColour: HUD_COLOUR_GOLD
    gold.a = alpha;
    Call<0x7195C0>(false, false);          // CFont::SetBackground
    Call<0x719380>(scaleX, scaleY);        // CFont::SetScale
    Call<0x719610>(2);                     // CFont::SetAlignment: right
    Call<0x7195B0>(true);                  // CFont::SetProportional
    Call<0x719490>(0);                     // CFont::SetFontStyle: FONT_GOTHIC
    Call<0x719590>(1);                     // CFont::SetOutlinePosition
    Call<0x719510>(CRGBA{0, 0, 0, alpha});  // CFont::SetDropColor
    Call<0x719430>(gold);                  // CFont::SetColor
    char text[8] = {}, gxt[8] = {};
    memset(text, ']', (size_t)std::min(level, 6));
    Call<0x718600>((const char*)text, gxt);              // AsciiToGxtChar
    Call<0x71A700>(right, midY - 8.0f * scaleY, (const char*)gxt);  // CFont::PrintString
}

using HudDrawFn = void(__cdecl*)();
HudDrawFn g_origHudDraw = nullptr;
void __cdecl HookHudDraw() {
    g_origHudDraw();
    if (Net().Online()) Players::Instance().DrawWeaponIcons();
}

// A save keeps every PLAYER1 ped on foot (CPools::SavePedPool), and the remote player is one: the host's saves and the
// worlds it sent carried a copy of the partner. Loading made every such ped a CPlayerPed of player 0 sharing its data,
// so a second CJ walked by this game's pad, took the player's clothes and cleared its crosshair. Saves leave the remote
// players out, and a save that has copies already loads only the story's CJ ($PLAYER_ACTOR, main.scm's $12).
using BoolFn = bool(__cdecl*)();
BoolFn g_origSavePedPool = nullptr, g_origLoadPedPool = nullptr;
bool __cdecl HookSavePedPool() {
    std::vector<CPed*> hidden;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && IsPedValid(rp.ped) && rp.ped->PedType() == PED_TYPE_PLAYER1) {
            rp.ped->PedType() = PED_TYPE_PLAYER_NETWORK;
            hidden.push_back(rp.ped);
        }
    bool ok = g_origSavePedPool();
    for (CPed* p : hidden) p->PedType() = PED_TYPE_PLAYER1;
    return ok;
}

int g_loadCount = 0, g_loadSeen = 0;  // CPools::LoadPedPool: its player peds (it loads no other ones)
CPlayerPed* g_loadKept = nullptr;
std::vector<CPlayerPed*> g_loadDropped;

using LoadDataFn = void(__cdecl*)(void*, int);
LoadDataFn g_origLoadPedCount = nullptr;
void __cdecl HookLoadPedCount(void* dst, int size) {  // CGenericGameStorage::LoadDataFromWorkBuffer: the count
    g_origLoadPedCount(dst, size);
    g_loadCount = size == 4 ? *(int32_t*)dst : 0;
    g_loadSeen = 0;
}

using PlayerCtorFn = CPlayerPed*(__fastcall*)(CPlayerPed*, void*, int, bool);
PlayerCtorFn g_origLoadedPlayerCtor = nullptr;
CPlayerPed* __fastcall HookLoadedPlayerCtor(CPlayerPed* self, void* edx, int playerId, bool groupCreated) {
    ++g_loadSeen;
    // the one $PLAYER_ACTOR names, or the last as the game would have it
    bool keep = g_loadCount <= 1 ||
                (!g_loadKept && (GetPedRef(self) == Ref<int32_t>(addr::ScriptSpace + 12) || g_loadSeen >= g_loadCount));
    if (keep) {
        g_loadKept = self;
        return g_origLoadedPlayerCtor(self, edx, playerId, groupCreated);
    }
    g_loadDropped.push_back(self);  // a ped of player 1 with no group: nothing of player 0's is touched, it goes after
    return g_origLoadedPlayerCtor(self, edx, kRemoteSlot, true);
}

bool __cdecl HookLoadPedPool() {
    g_loadCount = g_loadSeen = 0;
    g_loadKept = nullptr;
    g_loadDropped.clear();
    bool ok = g_origLoadPedPool();
    if (g_loadKept && !g_loadDropped.empty()) {
        PlayerInfo(0)->ped = g_loadKept;  // every loaded player ped was made player 0 in turn
        for (CPlayerPed* p : g_loadDropped)
            if (IsPedValid(p)) {
                WorldRemove(p);
                CallVirtual<0, void>(p, 1u);  // scalar deleting destructor
            }
        LOGI("The save had %d player characters: the story's CJ stays, the copies are removed", g_loadCount);
    }
    g_loadDropped.clear();
    g_loadKept = nullptr;
    g_loadCount = 0;
    return ok;
}

// a player's jetpack is the simplest active task (CPedIntelligence::GetTaskJetPack)
bool HasJetpack(CPed* ped) { return ped->Intelligence() && CallMethod<0x601110, void*>(ped->Intelligence()); }

int WeaponModel(int weaponType) {
    int model = -1;
    Cmd(0x0781).I(weaponType).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
    return model;
}

uint8_t ClampByte(float v) { return (uint8_t)std::clamp(v, 0.0f, 255.0f); }

constexpr int TASK_ENTER_CAR_AS_PASSENGER = 700;
constexpr int TASK_ENTER_CAR_AS_DRIVER = 701;
constexpr int TASK_LEAVE_CAR = 704;
// what the script commands (TASK_ENTER_CAR_AS_PASSENGER / _DRIVER, the G key's too) give: the plain one under it
constexpr int TASK_ENTER_CAR_AS_PASSENGER_TIMED = 712, TASK_ENTER_CAR_AS_DRIVER_TIMED = 713;

bool EnteringCar(CPed* ped) {
    int t = ActivePrimaryTaskType(ped);
    return t == TASK_ENTER_CAR_AS_PASSENGER || t == TASK_ENTER_CAR_AS_DRIVER || t == TASK_ENTER_CAR_AS_PASSENGER_TIMED ||
           t == TASK_ENTER_CAR_AS_DRIVER_TIMED;
}
constexpr uint64_t kEnterGraceMs = 4000;  // let the enter animation finish before warping

uint8_t WantedLevel(CPlayerPed* me) {
    void* wanted = me->PlayerData() ? me->Wanted() : nullptr;
    return wanted ? (uint8_t)std::clamp(Field<int32_t>(wanted, 0x2C), 0, 6) : 0;  // CWanted::m_nWantedLevel
}

void FillCommon(OnFootState& st, CPlayerPed* me) {
    st.health = ClampByte(me->Health());
    st.wanted = WantedLevel(me);
    st.armour = ClampByte(me->Armour());
    CWeapon& w = me->ActiveWeapon();
    st.weapon = (uint8_t)w.type;
    st.ammo = (uint16_t)std::clamp(w.ammoTotal, 0, 65535);
    st.area = me->AreaCode();
    CPad* pad = GetPad(0);
    if (pad->DisablePlayerControls == 0 && !game::MenuActive()) st.pad = PadState::From(pad->NewState);
    // a passenger aims out of the window with the mouse's look button, which the pad state has not: R1 (PassengerDriveBy)
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() != me && pad->NewState.m_bVehicleMouseLook)
        st.pad.buttons |= PadState::B_RS1;
    CameraState& c = st.cam;
    c.mode = cam::Mode();
    c.weaponMode = cam::WeaponMode();
    c.fov = cam::Fov();
    c.front = cam::Front();
    c.sourceOffset = cam::Source() - me->GetPosition();
    c.up = cam::Up();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.lookPitch = me->PlayerData() ? me->LookPitch() : 0.0f;
    c.useMouse3rd = cam::UseMouse3rdPerson() != 0;
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();
}

void WriteCommon(Writer& w, const OnFootState& st) {
    st.pad.Write(w);
    st.cam.Write(w);
    w.U8(st.health);
    w.U8(st.armour);
    w.U8(st.weapon);
    w.U16(st.ammo);
    w.I8(st.area);
    w.U8(st.wanted);
}

void ReadCommon(Reader& r, OnFootState& st) {
    st.pad.Read(r);
    st.cam.Read(r);
    st.health = r.U8();
    st.armour = r.U8();
    st.weapon = r.U8();
    st.ammo = r.U16();
    st.area = r.I8();
    st.wanted = r.U8();
}
}  // namespace

int ActivePrimaryTaskType(CPed* ped, void** taskOut) {
    if (taskOut) *taskOut = nullptr;
    void* intel = ped ? ped->Intelligence() : nullptr;
    if (!intel) return -1;
    for (int i = 0; i < 5; ++i) {
        void* task = Field<void*>(intel, 0x04 + i * 4);  // CTaskManager::m_aPrimaryTasks
        if (!task) continue;
        if (taskOut) *taskOut = task;
        return CallVirtual<4, int>(task);  // CTask::GetTaskType
    }
    return -1;
}

void StopEntering(CVehicle* v) {
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        void* task = nullptr;
        int type = ActivePrimaryTaskType(ped, &task);
        bool entering = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_DRIVER ||
                        type == TASK_ENTER_CAR_AS_PASSENGER_TIMED || type == TASK_ENTER_CAR_AS_DRIVER_TIMED;
        if (entering && Field<CVehicle*>(task, 0x0C) == v) CallMethod<0x601640>(ped->Intelligence(), true);  // FlushImmediately
    }
}

int PassengerSeatOf(CVehicle* v, CPed* ped) {
    if (!v || !ped) return -2;
    if (v->Driver() == ped) return -1;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] == ped) return i;
    return -2;
}

int SeatOfTargetDoor(int door) {
    switch (door) {
        case 11: return 1;  // rear left
        case 9: return 2;   // rear right
        default: return 0;  // front right (8) and anything else
    }
}

int SeatOrAny(CVehicle* v, int seat) { return v && seat >= 0 && seat < v->MaxPassengers() ? seat : -1; }

float RemoteDistance(const CVector& p, int8_t area, uint8_t* who) {
    float best = 1e9f;
    for (auto& [id, rp] : Players::Instance().All())
        if (float d = (rp.state.pos - p).Length(); rp.hasState && rp.state.area == area && d < best) {
            best = d;
            if (who) *who = id;
        }
    return best;
}

// ------------------------------------------------------------------ module
void Players::OnInstallHooks() {
    mem::HookFn(0x60EA90, &HookPlayerProcessControl, &g_origProcessControl, "CPlayerPed::ProcessControl");
    void* coopGame = nullptr;  // CGameLogic::IsCoopGameGoingOn, always false (Patches)
    mem::RedirectCall(0x644A1A, (const void*)&PassengerDriveBy, &coopGame);
    mem::Nop(0x644A27, 28);  // the SMG slot holds a weapon with ammo (PassengerDriveBy checks the gun in hand)
    mem::Nop(0x644A69, 6);   // jne: the camera is MODE_TWOPLAYER_IN_CAR_AND_SHOOTING
    mem::HookFn(0x62D3B0, &HookGangDriveByProcess, &g_origGangDriveBy, "CTaskSimpleGangDriveBy::ProcessPed");
    mem::HookFn(0x442060, &HookRestorePlayer, &g_origRestore, "CGameLogic::RestorePlayerStuffDuringResurrection");
    mem::RedirectCallFn(0x4677A5, &HookDeleteChar, &g_origRemoveThisPed);  // DELETE_CHAR -> CTheScripts::RemoveThisPed
    mem::HookFn(0x609FF0, &HookGetPlayerInfoForThisPlayerPed, &g_origGetPlayerInfo,
                "CPlayerPed::GetPlayerInfoForThisPlayerPed");
    mem::HookFn(0x706970, &HookGetRealTimeShadow, &g_origGetShadow, "CRealTimeShadowManager::GetRealTimeShadow");
    mem::HookFn(0x502280, &HookVehicleAudioService, &g_origVehicleAudio, "CAEVehicleAudioEntity::Service");
    mem::HookFn(0x58FAE0, &HookHudDraw, &g_origHudDraw, "CHud::Draw");
    mem::RedirectCallFn(0x60F595, &HookGroupBehaviour, &g_origGroupBehaviour);  // CPlayerPed::ProcessControl
    mem::RedirectCallFn(0x74278B, &HookTakePhotograph, &g_origTakePhoto);      // CWeapon::Fire, the camera
    mem::RedirectCallFn(0x5D0880, &HookSavePedPool, &g_origSavePedPool);  // CPools::Save
    mem::RedirectCallFn(0x5D0890, &HookLoadPedPool, &g_origLoadPedPool);  // CPools::Load
    mem::RedirectCallFn(0x5D2D95, &HookLoadPedCount, &g_origLoadPedCount);  // in CPools::LoadPedPool
    mem::RedirectCallFn(0x5D2E07, &HookLoadedPlayerCtor, &g_origLoadedPlayerCtor);
    Net().SetHandler(proto::G_PLAYER_ONFOOT, [](uint8_t from, Reader& r) { Instance().HandleOnFoot(from, r); });
    Net().SetHandler(proto::G_TELEPORT, [](uint8_t from, Reader& r) { Instance().HandleTeleport(from, r); });
    Net().SetHandler(proto::G_PLAYER_DRIVER, [](uint8_t from, Reader& r) { Instance().HandleDriver(from, r); });
    Net().SetHandler(proto::G_PLAYER_PASSENGER, [](uint8_t from, Reader& r) { Instance().HandlePassenger(from, r); });
    Net().SetHandler(proto::G_PLAYER_ENTER_CAR, [](uint8_t from, Reader& r) { Instance().HandleEnterCar(from, r); });
    Net().SetHandler(proto::G_PLAYER_EXIT_CAR, [](uint8_t from, Reader& r) { Instance().HandleExitCar(from, r); });
}

RemotePlayer* Players::FindByPed(CPed* ped) {
    if (!ped) return nullptr;
    for (auto& [id, rp] : m_remote)
        if (rp.ped == ped) return &rp;
    return nullptr;
}

RemotePlayer* Players::Get(uint8_t id) {
    auto it = m_remote.find(id);
    return it == m_remote.end() ? nullptr : &it->second;
}

// as SET_PLAYER_MODEL (0x47A820) changes it: a new clump, the walk set kept. A special character (290-299) must be
// in memory already: it is loaded by name (Peds::SpecialModelFor). For the other players' copies only.
bool Players::SetModel(CPlayerPed* ped, int model) {
    if (ped->ModelIndex() == model) return true;
    bool special = model >= 290 && model <= 299;
    if (model != 0 && (!IsPedModel(model) || (special ? !IsModelLoaded(model) : !LoadModelNow(model)))) return false;
    int32_t walk = Field<int32_t>(ped, 0x4D4);  // CPed::m_nAnimGroup
    // The copy's tasks hold animations of the old clump (a car door's, a fight's, the look-at's bones) and touched them
    // freed later: SET_PLAYER_MODEL is for an idle player. Flushed first, out of a car first (the flush takes the drive
    // task, the next state seats it again).
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    CallMethod<0x601640>(ped->Intelligence(), true);  // CPedIntelligence::FlushImmediately
    CallVirtual<8>(ped);                        // CEntity::DeleteRwObject
    Field<uint16_t>(ped, 0x22) = 0xFFFF;        // CEntity::m_nModelIndex: SetModelIndex rebuilds only a new one
    CallVirtual<5>(ped, (uint32_t)model);       // CPed::SetModelIndex
    Field<int32_t>(ped, 0x4D4) = walk;
    return true;
}

void Players::OnSessionStart() { m_lastSend = 0; }

void Players::OnSessionEnd() {
    for (auto& [id, rp] : m_remote) DestroyRemote(rp);
    m_remote.clear();
    g_ourDriveBy.clear();
}

void Players::OnPeerLeave(uint8_t id) {
    auto it = m_remote.find(id);
    if (it == m_remote.end()) return;
    DestroyRemote(it->second);
    m_remote.erase(it);
}

void Players::OnGameStarted() {
    Cmd(0x0501).I(0).I(1).Run();  // SET_PLAYER_CAN_DO_DRIVE_BY: passengers may shoot from the window
    g_ourDriveBy.clear();
    m_enterReported = nullptr;
    m_lastCar = -1;  // another world
    m_exitReported = false;
    // world was (re)created: our peds are gone, they will be respawned from the next states
    for (auto& [id, rp] : m_remote) {
        rp.ped = nullptr;
        rp.pedHandle = -1;
        rp.blip = 0;
    }
    // a reload of the host's world after its progress: back where the partner stood, as it was
    if (flow::SavedSpot s; Net().Online() && !Net().IsHost() && flow::TakeSavedSpot(s)) {
        CPlayerPed* me = FindPlayerPed(0);
        int h = GetPedRef(me);
        Cmd(0x04BB).I(s.area).Run();                          // SET_AREA_VISIBLE
        Cmd(0x0860).I(h).I(s.area).Run();                     // SET_CHAR_AREA_VISIBLE
        Cmd(0x03CB).F(s.x).F(s.y).F(s.z).Run();               // LOAD_SCENE: its ground first
        Cmd(0x00A1).I(h).F(s.x).F(s.y).F(s.z - 1.0f).Run();   // SET_CHAR_COORDINATES
        LightArea(s.area, {s.x, s.y, s.z});  // the host's world came with the light of where the host stood
        me->CurrentRotation() = me->AimingRotation() = s.heading;
        me->Health() = s.health;
        me->Armour() = s.armour;
        Cmd(0x048F).I(h).Run();  // REMOVE_ALL_CHAR_WEAPONS: the host's ones came with its world
        for (int i = 0; i < 13; ++i)
            if (s.weapons[i] > 0) {
                int model = WeaponModel(s.weapons[i]);
                if (model > 0 && !IsModelLoaded(model)) LoadModelNow(model);
                Cmd(0x01B2).I(h).I(s.weapons[i]).I(std::max(s.ammo[i], 1)).Run();  // GIVE_WEAPON_TO_CHAR
            }
        Cmd(0x01B9).I(h).I(s.activeWeapon).Run();  // SET_CURRENT_CHAR_WEAPON
        if (s.carModel > 0 && IsVehicleModel(s.carModel) && HasRoom(VehiclePool(), kKeepVehicles, "vehicle") &&
            LoadModelNow(s.carModel)) {  // the car left next to it
            int car = -1;
            Cmd(0x00A5).I(s.carModel).F(s.carX).F(s.carY).F(s.carZ).Out(&car).Run();  // CREATE_CAR
            if (CVehicle* v = GetVehicle(car)) {
                Cmd(0x0175).I(car).F(s.carHeading * 57.29578f).Run();  // SET_CAR_HEADING
                memcpy(v->Colors(), s.carColors, 4);
                v->Health() = s.carHealth;
                Cmd(0x01C3).I(car).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary car, as before
                m_lastCar = car;
                LOGI("The car left next to the partner (model %d) is back", s.carModel);
            }
        }
        LOGI("Back at the spot before the reload of the host's world");
        return;
    }
    // the host's world puts the partner where the host stands: a step aside, where nothing is in the way
    if (Net().Online() && !Net().IsHost()) {
        CPlayerPed* me = FindPlayerPed(0);
        CVector p = me->GetPosition();
        float h = me->CurrentRotation();
        for (float side : {1.2f, -1.2f}) {
            CVector q = p + CVector{std::cos(h) * side, std::sin(h) * side, 0};
            if (!Call<0x56A490, bool>(&p, &q, true, true, false, true, false, true, false)) continue;  // GetIsLineOfSightClear
            Cmd(0x00A1).I(GetPedRef(me)).F(q.x).F(q.y).F(q.z - 1.0f).Run();  // SET_CHAR_COORDINATES
            break;
        }
    }
}

void Players::OnGameShutdown() {
    // the remote players go before the world is torn down (a world loaded in the game: the prologue's end)
    for (auto& [id, rp] : m_remote) {
        DestroyRemote(rp);
        rp.blip = 0;
    }
    PlayerInfo(kRemoteSlot)->ped = nullptr;
}

void Players::SpawnRemote(RemotePlayer& rp) {
    if (rp.ped || !rp.hasState || !core::IsPlayerReady()) return;
    CPlayerInfo* slot = PlayerInfo(kRemoteSlot);
    if (slot->ped) return;  // slot busy (should not happen)
    if (!HasRoom(PedPool(), 2, "ped")) return;  // the other player above everything else, still not into the last slots
    const CVector p = rp.state.pos;
    int playerIndex = -1, handle = -1;
    // made high above its place: CREATE_PLAYER clears the space around itself of this game's random peds and cars
    // (the taxi the other player sits in), and it goes where it is right after
    Cmd(0x0053).I(kRemoteSlot).F(p.x).F(p.y).F(p.z + 300.0f).Out(&playerIndex).Run();  // CREATE_PLAYER
    Cmd(0x01F5).I(kRemoteSlot).Out(&handle).Run();                            // GET_PLAYER_CHAR
    Cmd(0x0501).I(kRemoteSlot).I(1).Run();  // SET_PLAYER_CAN_DO_DRIVE_BY: its drive-bys show here (CPlayerInfo +0x153)
    auto* ped = (CPlayerPed*)GetPed(handle);
    if (!ped) {
        LOGE("Failed to create the remote player ped");
        slot->ped = nullptr;
        return;
    }
    // registered in CWorld::Players[1] only while processed (see HookPlayerProcessControl)
    slot->ped = nullptr;
    Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES (feet)
    ped->PedType() = PED_TYPE_PLAYER1;  // enemies hostile to the player attack the partner too
    ped->SetProofs(true, true, true, true, true);  // damage is decided by its owner
    ped->SetStreamingDontDelete(true);
    ped->AreaCode() = rp.state.area;
    rp.ped = ped;
    // CEntity::RegisterReference: the engine nulls it when it deletes the copy itself (with a car a mission deletes:
    // CWorld::Process removes the people of a deleted car), before the network handlers of the next frame use it
    CallMethod<0x571B70>(ped, (CEntity**)&rp.ped);
    rp.pedHandle = handle;
    rp.padNew = rp.padOld = CControllerState{};
    // radar blip
    int blip = 0;
    Cmd(0x0187).I(handle).Out(&blip).Run();  // ADD_BLIP_FOR_CHAR
    if (blip) {
        Cmd(0x07E0).I(blip).I(1).Run();       // SET_BLIP_AS_FRIENDLY
        Cmd(0x0168).I(blip).I(2).Run();       // CHANGE_BLIP_SCALE
        Cmd(0x018B).I(blip).I(2).Run();       // CHANGE_BLIP_DISPLAY: radar only, no 3D arrow
        rp.blip = blip;
    }
    LOGI("Remote player %d spawned (ped %p, handle %d)", rp.id, (void*)ped, handle);
}

void Players::DestroyRemote(RemotePlayer& rp) {
    if (rp.blip) {
        Cmd(0x0164).I(rp.blip).Run();  // REMOVE_BLIP
        rp.blip = 0;
    }
    if (rp.ped && IsPedValid(rp.ped)) {
        CPed* ped = rp.ped;
        if (ped->InVehicle()) {  // the car kept it in its seat count (CTheScripts::RemoveThisPed takes it out first too)
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        if (PlayerInfo(kRemoteSlot)->ped == ped) PlayerInfo(kRemoteSlot)->ped = nullptr;
        if (CPlayerPed* me = FindPlayerPed(0); me && Field<CEntity*>(me, 0x128) == ped) Field<CEntity*>(me, 0x128) = nullptr;  // KeepApart
        WorldRemove(ped);
        Call<0x565510>(ped);            // CWorld::RemoveReferencesToDeletedObject: who stood on it or touched it
        CallVirtual<0, void>(ped, 1u);  // scalar deleting destructor (returns the slot to the ped pool)
        LOGI("Remote player %d removed", rp.id);
    }
    rp.ped = nullptr;
    rp.pedHandle = -1;
}

void Players::HandleOnFoot(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    OnFootState st;
    if (!st.Read(r)) return;
    RemotePlayer& rp = Touch(from);
    // unreliable channel: ignore states older than the newest applied one
    if (rp.hasState && (int32_t)(st.time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = st.time;
    // enter/exit are replicated as events; with the target held the button is the fight's second attack (a kick,
    // CTaskSimplePlayerOnFoot: MeleeAttackJustDown 4), which only the pad replay shows
    if (!(st.pad.buttons & PadState::B_RS1)) st.pad.buttons &= ~PadState::B_TRIANGLE;
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::OnFoot;
}

RemotePlayer& Players::Touch(uint8_t from) {
    RemotePlayer& rp = m_remote[from];
    rp.id = from;
    return rp;
}

void Players::HandleDriver(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    uint16_t netId = r.U16();
    VehicleState vs;
    vs.Read(r);
    OnFootState st;
    ReadCommon(r, st);
    if (!r.Ok()) return;
    RemotePlayer& rp = Touch(from);
    if (rp.hasState && (int32_t)(vs.time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = vs.time;
    st.time = vs.time;
    st.pos = vs.pos;
    st.vel = vs.vel;
    st.pad.buttons &= ~PadState::B_TRIANGLE;
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::Driver;
    rp.vehNetId = netId;
    rp.seat = -1;
    Vehicles::Instance().ApplyDriverState(from, netId, vs);
}

void Players::HandlePassenger(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    uint32_t time = r.U32();
    Vec3 p = r.Vec();
    OnFootState st;
    ReadCommon(r, st);
    if (!r.Ok()) return;
    RemotePlayer& rp = Touch(from);
    if (rp.hasState && (int32_t)(time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = time;
    st.time = time;
    st.pos = {p.x, p.y, p.z};
    st.pad.buttons &= ~PadState::B_TRIANGLE;
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::Passenger;
    rp.vehNetId = netId;
    rp.seat = seat;
}

void Players::HandleEnterCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    NetVehicle* nv = Vehicles::Instance().Find(netId);
    // a car deleted in the last frame is still listed (Vehicles drops it in its OnPreProcess, after the messages)
    if (!rp || !rp->ped || !nv || !Vehicles::Alive(*nv)) return;
    rp->enterTaskMs = NowMs();
    if (seat < 0)
        Cmd(0x05CB).I(rp->pedHandle).I(nv->handle).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
    else
        Cmd(0x05CA).I(rp->pedHandle).I(nv->handle).I(10000).I(SeatOrAny(nv->veh, seat)).Run();  // TASK_ENTER_CAR_AS_PASSENGER
}

void Players::HandleExitCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    NetVehicle* nv = Vehicles::Instance().Find(netId);
    if (!rp || !rp->ped || !nv || !nv->veh) return;
    if (rp->ped->InVehicle() && rp->ped->Vehicle() == nv->veh) {
        rp->exitTaskMs = NowMs();
        Cmd(0x05CD).I(rp->pedHandle).I(nv->handle).Run();  // TASK_LEAVE_CAR
    }
}

void Players::HandleTeleport(uint8_t from, Reader& r) {
    uint8_t target = r.U8();
    CVector pos{r.F32(), r.F32(), r.F32()};
    int8_t area = r.I8();
    if (!r.Ok() || target != Net().MyId() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    int handle = GetPedRef(me);
    Cmd(0x04BB).I(area).Run();                 // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(area).Run();       // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(pos.x).F(pos.y).F(pos.z).Run();  // SET_CHAR_COORDINATES
    LightArea(area, pos);
    ui::TheChat().AddSystem(Format(TR("%s переместил вас к себе.", "%s brought you over."), Net().PeerName(from).c_str()));
}

void Players::ApplyState(RemotePlayer& rp) {
    CPlayerPed* ped = rp.ped;
    const OnFootState& st = rp.state;

    // pad: "old" is always the previous frame, "new" only changes when a packet arrived
    rp.padOld = rp.padNew;
    if (rp.padFresh) {
        st.pad.To(rp.padNew);
        rp.padFresh = false;
    }

    // death is decided by the owner's game (the local ped has all proofs): play it once; after the
    // respawn a fresh ped replaces the dead one, which cannot be revived
    if (st.flags & OnFootState::F_DEAD) {
        if (!rp.deadShown) {
            rp.deadShown = true;
            LOGI("Remote player %d died", rp.id);
            Cmd(0x05BE).I(rp.pedHandle).Run();  // TASK_DIE
        }
        return;
    }
    if (rp.deadShown) {
        rp.deadShown = false;
        DestroyRemote(rp);  // SpawnRemote runs next frame
        return;
    }

    ped->Health() = (float)st.health;
    ped->Armour() = (float)st.armour;
    if (!Ref<uint8_t>(0xB5F851)) ped->AreaCode() = st.area;  // CCutsceneMgr::ms_running: the scene sets it (Missions)

    // crouching toggles on the crouch button's press, which the copy replays from the owner's pad: a press lost on the
    // way (or a press and its release in one frame) left the copy standing while its owner crouched, and every next
    // press the other way round. A difference that lasts is the owner's state put right.
    bool duck = (st.flags & OnFootState::F_DUCKING) != 0;
    if (duck == ped->IsDucking() || rp.mode != RemotePlayer::Mode::OnFoot) {
        rp.duckOffMs = 0;
    } else if (!rp.duckOffMs) {
        rp.duckOffMs = NowMs();
    } else if (NowMs() - rp.duckOffMs > 400 && ped->Intelligence()) {
        if (duck) CallMethod<0x601230>(ped->Intelligence(), (uint16_t)0);  // CPedIntelligence::SetTaskDuckSecondary
        else CallMethod<0x601390>(ped->Intelligence());                    // CPedIntelligence::ClearTaskDuckSecondary
        rp.duckOffMs = 0;
        LOGD("Remote player %d: crouch %s to match its owner", rp.id, duck ? "on" : "off");
    }

    // a phone call (CTaskComplexUseMobilePhone): the phone is put in the hand as a weapon's model, and the weapon's
    // comes back after it (CPed__removeCellphone); the animations go with Anims
    bool phone = (st.flags & OnFootState::F_PHONE) != 0;
    if (phone != (Field<int32_t>(ped, 0x740) == kCellphone) && LoadModelNow(kCellphone)) {
        if (phone) {
            CallMethod<0x5E5ED0>(ped, kCellphone);  // CPed::AddWeaponModel
        } else {
            CallMethod<0x5E3990>(ped, kCellphone);                       // CPed::RemoveWeaponModel
            CallMethod<0x5E6280>(ped, (int)ped->ActiveWeapon().type);  // CPed::SetCurrentWeapon: its gun again
        }
        LOGI("Remote player %d: phone %s", rp.id, phone ? "out" : "away");
    }

    // the jetpack flies by the player's buttons, which this ped gets: it only needs one. Dropping it (the enter key)
    // happens here as there; whatever is left over goes with the flag
    bool jetpack = (st.flags & OnFootState::F_JETPACK) != 0;
    if (jetpack != HasJetpack(ped) && rp.mode == RemotePlayer::Mode::OnFoot && NowMs() - rp.jetpackMs > 1000) {
        rp.jetpackMs = NowMs();
        if (jetpack) {
            LoadModelNow(370);                   // the jetpack
            Cmd(0x07A7).I(rp.pedHandle).Run();   // TASK_JETPACK
        } else {
            Cmd(0x0792).I(rp.pedHandle).Run();   // CLEAR_CHAR_TASKS_IMMEDIATELY
        }
        LOGI("Remote player %d: jetpack %s", rp.id, jetpack ? "on" : "off");
    }

    // weapon in hand
    CWeapon& active = ped->ActiveWeapon();
    int weapon = st.weapon <= kLastWeapon ? st.weapon : 0;  // from the network
    if (active.type != weapon && NowMs() - rp.lastWeaponTry > 500) {
        rp.lastWeaponTry = NowMs();
        if (weapon != 0) {
            int model = WeaponModel(weapon);
            if (model > 0 && !IsModelLoaded(model)) LoadModelNow(model);
            Cmd(0x01B2).I(rp.pedHandle).I(weapon).I(std::max<int>(st.ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x01B9).I(rp.pedHandle).I(weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    } else if (weapon != 0) {
        active.ammoTotal = std::max<int>(st.ammo, 1);  // never run dry locally: the owner decides
        if (active.ammoInClip <= 0) active.ammoInClip = 1;
    }

    if (rp.mode != RemotePlayer::Mode::OnFoot) {
        ApplyVehicleSeat(rp);
        return;
    }
    if (st.flags & OnFootState::F_IN_TRAIN) {
        RideTrain(rp);
        return;
    }
    // on foot but still sitting in a vehicle here: the exit animation gets a few seconds, then warp out
    if (ped->InVehicle()) {
        if (NowMs() - rp.exitTaskMs > kEnterGraceMs) {
            Cmd(0x0362).I(rp.pedHandle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            rp.exitTaskMs = NowMs();
        }
        return;
    }

    if (Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running: Missions places the ped in the scene

    // position: predict, then correct smoothly (teleport on large errors)
    float age = std::min(0.25f, (float)(NowMs() - rp.stateRecvMs) / 1000.0f);
    CVector target = st.pos + st.vel * age;
    CVector cur = ped->GetPosition();
    CVector err = target - cur;
    float dist2d = err.Length2D();
    // Where this game has no ground under it (one of you indoors: the other's map is unloaded; or far away), its
    // physics would drop it through the map, and the pull would hold it there, under the teleport distance.
    CPlayerPed* me = FindPlayerPed(0);
    bool found = false;
    float ground = Call<0x5696C0, float>(target.x, target.y, target.z + 1.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
    bool groundless = st.area != me->AreaCode() || !found || (target - me->GetPosition()).Length2D() > 150.0f;
    bool sunk = found && cur.z < ground + 0.3f && target.z > ground + 0.5f;  // a standing ped is ~1 m above its ground
    if (dist2d > kTeleportDist || std::fabs(err.z) > kTeleportDist || sunk) {
        Cmd(0x00A1).I(rp.pedHandle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        ped->MoveSpeed() = st.vel * (1.0f / 50.0f);
    } else if (groundless) {
        SetEntityPosition(ped, target);
        ped->MoveSpeed() = CVector{};
    } else if (dist2d > 0.05f || std::fabs(err.z) > 1.0f) {
        float k = std::min(1.0f, TimeStep() * 0.25f);  // ~ 12 per second at 50 steps
        CVector corr{err.x * k, err.y * k, std::fabs(err.z) > 1.0f ? err.z * k : 0.0f};
        SetEntityPosition(ped, cur + corr);
        if (err.z > 1.0f && ped->MoveSpeed().z < 0.0f) ped->MoveSpeed().z = 0.0f;  // below its place: no falling on
    }
    // standing still: keep the facing in sync (movement sets it while active). Aiming, the owner's game turns its player
    // with its camera, and that camera exists only there: here the player kept facing where it began to aim and only
    // its gun went up and down. It faces as its owner, quickly.
    bool moving = std::abs(st.pad.lx) > 10 || std::abs(st.pad.ly) > 10;
    bool aiming = (st.pad.buttons & PadState::B_RS1) != 0;
    if (aiming || !moving) {
        float r = LerpAngle(ped->CurrentRotation(), st.rotation, aiming ? 0.6f : 0.2f);
        ped->CurrentRotation() = ped->AimingRotation() = r;
    }
}

// Trains are not networked (Trains.cpp shows the host's in the other games): the player riding one sits in the cab of
// the train here nearest to its place.
void Players::RideTrain(RemotePlayer& rp) {
    CVehicle* train = Call<0x6F7090, CVehicle*>(rp.state.pos, true);  // CTrain::FindNearestTrain(position, an engine)
    CPlayerPed* ped = rp.ped;
    if (!train || (train->GetPosition() - rp.state.pos).Length() > 25.0f || (ped->InVehicle() && ped->Vehicle() == train) ||
        NowMs() - rp.enterTaskMs < 1500)
        return;
    rp.enterTaskMs = NowMs();
    CVector p = ped->GetPosition();
    if (ped->InVehicle()) {
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        return;
    }
    if (CPed* d = train->Driver(); d && d->IsPlayer()) return;          // taken here
    else if (d) Cmd(0x009B).I(GetPedRef(d)).Run();                       // DELETE_CHAR: the copy's own driver
    Field<uint8_t>(train, 0x42A) |= 2;  // bHasBeenOwnedByPlayer: a player warped in reports a car theft (0x64BBBA) on ours
    Cmd(0x036A).I(rp.pedHandle).I(GetVehicleRef(train)).Run();          // WARP_CHAR_INTO_CAR
}

void Players::ApplyVehicleSeat(RemotePlayer& rp) {
    NetVehicle* nv = Vehicles::Instance().Find(rp.vehNetId);
    if (!nv || !Vehicles::Alive(*nv)) return;  // the vehicle announcement has not arrived yet
    CPlayerPed* ped = rp.ped;
    int seat = PassengerSeatOf(nv->veh, ped);
    bool seated = ped->InVehicle() && ped->Vehicle() == nv->veh && seat == rp.seat;
    if (seated) return;
    // give the enter animation time; otherwise put the player in place directly
    if (NowMs() - rp.enterTaskMs < kEnterGraceMs && EnteringCar(ped)) return;
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    // a player warped in as the driver of a car no player owned yet reports a car theft against FindPlayerPed, ours here
    // (CTaskSimpleCarSetPedInAsDriver, 0x64BBBA), and makes it a player's car: the local pad drove it until the next frame
    Field<uint8_t>(nv->veh, 0x42A) |= 2;  // CVehicle::bHasBeenOwnedByPlayer
    if (rp.seat < 0) {
        if (nv->veh->Driver() && nv->veh->Driver() != ped) return;  // seat taken locally
        Cmd(0x036A).I(rp.pedHandle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        if (nv->veh->Driver() == ped) nv->veh->SetStatus(3);  // STATUS_PHYSICS: its owner's state moves it
    } else {
        if (SeatOrAny(nv->veh, rp.seat) < 0 || nv->veh->Passengers()[rp.seat]) return;  // no such seat, or taken
        Cmd(0x0430).I(rp.pedHandle).I(nv->handle).I(rp.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
    }
    rp.enterTaskMs = NowMs() - kEnterGraceMs + 1500;  // retry no more often than every 1.5 s
}

void Players::TryEnterAsPassenger() {
    if (!core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (me->InVehicle() || EnteringCar(me)) return;
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    CVector mp = me->GetPosition();
    CVehicle* best = nullptr;
    int bestSeat = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        float d = (v->GetPosition() - mp).Length();
        if (d > 10.0f || v->Status() == 5 /* wrecked */ || v->IsTrain()) continue;  // a train's copy is locked (Trains)
        int freeSeat = -1;
        for (int s = 0; s < v->MaxPassengers() && s < 8; ++s)
            if (!v->Passengers()[s]) {
                freeSeat = s;
                break;
            }
        if (freeSeat < 0) continue;
        bool partnerDrives = v->Driver() && FindByPed(v->Driver());
        float score = d - (partnerDrives ? 100.0f : 0.0f);
        if (score < bestScore) {
            bestScore = score;
            best = v;
            bestSeat = freeSeat;
        }
    }
    if (!best) return;
    Cmd(0x05CA).I(GetPedRef(me)).I(GetVehicleRef(best)).I(10000).I(bestSeat).Run();  // TASK_ENTER_CAR_AS_PASSENGER
}

void Players::DetectCarTasks() {
    CPlayerPed* me = FindPlayerPed(0);
    void* task = nullptr;
    int type = ActivePrimaryTaskType(me, &task);
    bool passenger = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_PASSENGER_TIMED;
    if ((type == TASK_ENTER_CAR_AS_DRIVER || passenger) && task) {
        auto* target = Field<CVehicle*>(task, 0x0C);  // CTaskComplexEnterCar::m_Car, ...AsPassengerTimed::m_TargetCar
        if (target && target != m_enterReported && IsVehicleValid(target)) {
            m_enterReported = target;
            if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(target)) {
                int seat = -1;
                if (type == TASK_ENTER_CAR_AS_PASSENGER_TIMED)  // the door the command chose for its seat (0: any)
                    seat = SeatOfTargetDoor(Field<int32_t>(task, 0x14));
                else if (passenger)  // m_TargetDoor once at the door, else m_TargetSeat (a door too)
                    seat = SeatOfTargetDoor(Field<int32_t>(task, 0x14) ? Field<int32_t>(task, 0x14) : Field<int32_t>(task, 0x1C));
                Writer w(proto::G_PLAYER_ENTER_CAR);
                w.U16(nv->netId);
                w.I8((int8_t)seat);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
    } else if (!me->InVehicle()) {
        m_enterReported = nullptr;
    }
    if (type == TASK_LEAVE_CAR && me->InVehicle()) {
        if (!m_exitReported) {
            m_exitReported = true;
            if (NetVehicle* nv = Vehicles::Instance().Find(me->Vehicle())) {
                Writer w(proto::G_PLAYER_EXIT_CAR);
                w.U16(nv->netId);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
    } else if (!me->InVehicle()) {
        m_exitReported = false;
    }
}

void Players::SendVehicleState(CVehicle* v) {
    CPlayerPed* me = FindPlayerPed(0);
    NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v);
    if (!nv) return;
    OnFootState common;
    FillCommon(common, me);
    uint64_t now = NowMs();
    // a button goes at once; a stick that moves (a gamepad's never rests) sent every frame: 33 ms at most, as SA-MP's rates
    bool padChanged = common.pad.buttons != m_lastSentPad.buttons || (!(common.pad == m_lastSentPad) && now - m_lastSend >= 33);
    bool driver = v->Driver() == me;
    uint64_t interval = driver ? 33 : 100;
    if (!padChanged && now - m_lastSend < interval) return;
    m_lastSend = now;
    m_lastSentPad = common.pad;
    if (driver) {
        Vehicles::Instance().TakeOwnership(*nv);
        VehicleState vs = VehicleState::Capture(v);
        vs.time = (uint32_t)now;
        Writer w(proto::G_PLAYER_DRIVER);
        w.U16(nv->netId);
        vs.Write(w);
        WriteCommon(w, common);
        Net().Send(proto::CH_STATE, w);
    } else {
        int seat = PassengerSeatOf(v, me);
        Writer w(proto::G_PLAYER_PASSENGER);
        w.U16(nv->netId);
        w.I8((int8_t)std::max(seat, 0));
        w.U32((uint32_t)now);
        CVector p = v->GetPosition();
        w.Vec({p.x, p.y, p.z});
        WriteCommon(w, common);
        Net().Send(proto::CH_STATE, w);
    }
}

void Players::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    for (auto& [id, rp] : m_remote) {
        if ((rp.ped && !IsPedValid(rp.ped)) || (!rp.ped && rp.pedHandle >= 0)) {  // the engine deleted it
            LOGW("Remote player %d ped vanished, respawning", id);
            rp.ped = nullptr;
            rp.pedHandle = -1;
            rp.blip = 0;
        }
        // states older than 5 s mean the player is loading / in the menu; a cutscene here keeps its ped in the scene
        // (its game loads the same cutscene and can be silent that long)
        bool active = rp.hasState && (NowMs() - rp.stateRecvMs < 5000 || (rp.ped && Ref<uint8_t>(0xB5F851)));
        if (!active) {
            if (rp.ped) DestroyRemote(rp);
            continue;
        }
        if (!rp.ped) SpawnRemote(rp);
        if (rp.ped) ApplyState(rp);
        if (rp.ped) KeepApart(rp.ped);
    }
}

// A script that puts this game's player on a spot (a changing room, a mission's) puts it inside the other player's
// ped when that one stands there: the two peds got stuck in each other and the player's next scripted task never
// started (the shop waits for it with the controls taken). Two players inside each other ignore each other's collision
// until they part (CPhysical::m_pEntityIgnoredCollision, the game clears it once they stop touching).
void Players::KeepApart(CPed* other) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->InVehicle() || other->InVehicle()) return;
    CVector d = other->GetPosition() - me->GetPosition();
    if (d.x * d.x + d.y * d.y > 0.4f * 0.4f || std::fabs(d.z) > 1.5f) return;
    Field<CEntity*>(me, 0x128) = other;
    Field<CEntity*>(other, 0x128) = me;
}

void Players::SendLocalState() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    OnFootState st;
    st.time = (uint32_t)NowMs();
    st.pos = me->GetPosition();
    st.vel = me->MoveSpeed() * 50.0f;
    st.rotation = me->CurrentRotation();
    st.aimRotation = me->AimingRotation();
    st.health = ClampByte(me->Health());
    st.armour = ClampByte(me->Armour());
    CWeapon& w = me->ActiveWeapon();
    st.weapon = (uint8_t)w.type;
    st.ammo = (uint16_t)std::clamp(w.ammoTotal, 0, 65535);
    st.flags = (me->IsDucking() ? OnFootState::F_DUCKING : 0) | (me->IsInTheAir() ? OnFootState::F_IN_AIR : 0) |
               (HasJetpack(me) ? OnFootState::F_JETPACK : 0) |
               (me->Health() <= 0.0f ? OnFootState::F_DEAD : 0) |
               (me->InVehicle() && me->Vehicle() && me->Vehicle()->IsTrain() ? OnFootState::F_IN_TRAIN : 0) |
               (Field<int32_t>(me, 0x740) == kCellphone ? OnFootState::F_PHONE : 0);  // CPed::m_nWeaponModelId
    st.area = me->AreaCode();
    st.moveState = (uint8_t)me->MoveState();
    st.wanted = WantedLevel(me);
    CPad* pad = GetPad(0);
    if (pad->DisablePlayerControls == 0 && !game::MenuActive()) st.pad = PadState::From(pad->NewState);
    CameraState& c = st.cam;
    c.mode = cam::Mode();
    c.weaponMode = cam::WeaponMode();
    c.fov = cam::Fov();
    c.front = cam::Front();
    c.sourceOffset = cam::Source() - st.pos;
    c.up = cam::Up();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.lookPitch = me->PlayerData() ? me->LookPitch() : 0.0f;
    c.useMouse3rd = cam::UseMouse3rdPerson() != 0;
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();

    uint64_t now = NowMs();
    bool padChanged = st.pad.buttons != m_lastSentPad.buttons;  // a stick alone waits for the 30 Hz below (see SendVehicleState)
    if (!padChanged && now - m_lastSend < 33) return;  // ~30 Hz, immediately on a button
    m_lastSend = now;
    m_lastSentPad = st.pad;
    Writer wr(proto::G_PLAYER_ONFOOT);
    st.Write(wr);
    Net().Send(proto::CH_STATE, wr);
}

void Players::OnPostProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    DetectCarTasks();
    FollowHost();
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) m_lastCar = GetVehicleRef(me->Vehicle());
    if (me->InVehicle() && me->Vehicle() && !me->Vehicle()->IsTrain()) SendVehicleState(me->Vehicle());
    else SendLocalState();  // a train is not networked: the other game seats this player in its copy (RideTrain)
}

// Where the story moves the host by script, the partner keeps up: joining after a new game's prologue (in a new game of
// its own) it comes to the host, and when the host gets on a bike in a mission or just after one, the partner gets a
// bike too (the prologue's ride home, the bikes of "Big Smoke").
void Players::FollowHost() {
    RemotePlayer* host = Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    if (Net().IsHost() || Ref<uint8_t>(0xB5F851) || !host || !host->ped || !host->hasState || me->InVehicle() ||
        NowMs() - m_followAt < 1000)  // CCutsceneMgr::ms_running: the scene holds the partner meanwhile
        return;
    CPed* hp = host->ped;
    CVector hpos = hp->GetPosition(), mpos = me->GetPosition();
    if (flow::IsFollower() && (hpos - mpos).Length() > 25.0f) {
        float h = hp->CurrentRotation();
        CVector p = hpos + CVector{std::cos(h) * 1.5f, std::sin(h) * 1.5f, 0};  // at the host's right hand
        int handle = GetPedRef(me);
        Cmd(0x04BB).I(hp->AreaCode()).Run();                      // SET_AREA_VISIBLE
        Cmd(0x0860).I(handle).I(hp->AreaCode()).Run();            // SET_CHAR_AREA_VISIBLE
        Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        Cmd(0x0173).I(handle).F(h * 57.29578f).Run();             // SET_CHAR_HEADING
        LightArea(hp->AreaCode(), p);
        m_followAt = NowMs();
        LOGI("Joined after the prologue: next to the host");
        return;
    }
    CVehicle* bike = hp->InVehicle() ? hp->Vehicle() : nullptr;
    int model = bike ? bike->ModelIndex() : 0;
    if (!bike || bike == m_bikeFor || bike->Driver() != hp || (model != 481 && model != 509 && model != 510)) return;  // BMX, bikes
    if ((!flow::IsFollower() && !HostMissionRecent(20000)) || (hpos - mpos).Length() > 60.0f) return;
    m_bikeFor = bike;
    m_followAt = NowMs();
    if (!IsModelLoaded(model)) {
        RequestModel(model);
        LoadAllRequestedModels(false);
    }
    float h = me->CurrentRotation();
    CVector p = mpos + CVector{std::cos(h) * 1.3f, std::sin(h) * 1.3f, -0.5f};  // at the partner's right hand
    int car = -1;
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")) return;
    Cmd(0x00A5).I(model).F(p.x).F(p.y).F(p.z).Out(&car).Run();  // CREATE_CAR
    if (car < 0) return;
    Cmd(0x0175).I(car).F(h * 57.29578f).Run();  // SET_CAR_HEADING
    Cmd(0x01C3).I(car).Run();                   // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary vehicle from now on
    ui::TheChat().AddSystem(TR("Рядом с вами велосипед. Садитесь и езжайте за хостом.",
                               "There is a bike next to you. Get on and follow the host."));
    LOGI("The host got on a bike (model %d): one for the partner too", model);
}

// the point over rp's head the name tag hangs from (back buffer coordinates) and its fade with distance
bool Players::TagAnchor(RemotePlayer& rp, float& sx, float& sy, float& alpha) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!rp.ped || !me || rp.ped->AreaCode() != me->AreaCode()) return false;
    CVector head = rp.ped->GetPosition() + CVector{0, 0, 0.95f};
    float dist = (head - cam::Source()).Length();
    if (dist > 70.0f) return false;
    float depth;
    if (!WorldToScreen(head, sx, sy, depth)) return false;
    auto& g = ui::Gfx();
    // game screen space -> back buffer
    sx *= g.Width() / (float)std::max(1, ScreenWidth());
    sy *= g.Height() / (float)std::max(1, ScreenHeight());
    alpha = dist > 50.0f ? 1.0f - (dist - 50.0f) / 20.0f : 1.0f;
    return true;
}

void Players::DrawWeaponIcons() {
    auto& g = ui::Gfx();
    float kx = ScreenWidth() / std::max(1.0f, g.Width()), ky = ScreenHeight() / std::max(1.0f, g.Height());
    for (auto& [id, rp] : m_remote) {
        float sx, sy, alpha;
        if (!rp.hasState || !TagAnchor(rp, sx, sy, alpha)) continue;
        // right of the health bar (64 x 6 at sy - 6, see DrawNameTag), as tall as the bars
        float s = g.Scale(), h = 26 * s, w = h * 47.0f / 58.0f;
        float x = sx + 32 * s + 4 * s, y = sy - 3 * s - h / 2;
        DrawWeaponIcon(rp.state.weapon, x * kx, y * ky, w * kx, h * ky, (uint8_t)(alpha * 255));
        DrawWantedStars(rp.state.wanted, (sx - 32 * s - 4 * s) * kx, (sy - 3 * s) * ky, (uint8_t)(alpha * 255));
    }
}

void Players::DrawNameTag(RemotePlayer& rp) {
    float sx, sy, alpha;
    if (!TagAnchor(rp, sx, sy, alpha)) return;
    auto& g = ui::Gfx();
    float s = g.Scale();
    std::string name = Net().PeerName(rp.id);
    if (NowMs() - rp.heardMs < 300) name = "{FFDC50}((•)){FFFFFF} " + name;  // talking right now
    bool host = rp.id == Net().HostId();
    uint32_t nameColor = host ? 0xFF64B4FF : 0xFF78DC78;
    float lh = g.LineHeight(ui::FONT_SMALL);
    g.Text(ui::FONT_SMALL, sx, sy - lh - 8 * s, name, ui::WithAlpha(nameColor, alpha), ui::TEXT_CENTER | ui::TEXT_OUTLINE);
    float bw = 64 * s, bh = 6 * s;
    float bx = sx - bw / 2, by = sy - 6 * s;
    float hp = std::clamp(rp.ped->Health() / 100.0f, 0.0f, 1.0f);
    g.Rect(bx - 1, by - 1, bw + 2, bh + 2, ui::WithAlpha(0xFF000000, alpha));
    g.Rect(bx, by, bw, bh, ui::WithAlpha(0xFF501414, alpha));
    g.Rect(bx, by, bw * hp, bh, ui::WithAlpha(0xFFDC3232, alpha));
    if (rp.ped->Armour() > 0) {
        float ar = std::clamp(rp.ped->Armour() / 100.0f, 0.0f, 1.0f);
        by -= bh + 3 * s;
        g.Rect(bx - 1, by - 1, bw + 2, bh + 2, ui::WithAlpha(0xFF000000, alpha));
        g.Rect(bx, by, bw, bh, ui::WithAlpha(0xFF3C3C3C, alpha));
        g.Rect(bx, by, bw * ar, bh, ui::WithAlpha(0xFFE6E6E6, alpha));
    }
}

void Players::OnRender() {
    if (!Net().Online()) return;
    for (auto& [id, rp] : m_remote) DrawNameTag(rp);
}

bool Players::OnCommand(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "tp" && cmd != "tphere") return false;
    if (!core::IsPlayerReady()) return true;
    // whom: /tp <nickname or number>; without one the host (for a partner) or the only other player. It used to take
    // the last player of the list whatever was typed.
    RemotePlayer* target = nullptr;
    int inGame = 0;
    for (auto& [id, rp] : m_remote)
        if (rp.hasState) ++inGame, target = &rp;
    if (!args.empty()) {
        target = nullptr;
        std::string want = ToLower(args[0]);
        bool number = !want.empty() && want.size() <= 3 && std::all_of(want.begin(), want.end(), [](char c) { return c >= '0' && c <= '9'; });  // a nickname may be UTF-8
        RemotePlayer* prefix = nullptr;
        int prefixes = 0;
        for (auto& [id, rp] : m_remote) {
            std::string name = ToLower(Net().PeerName(id));
            if ((number && id == std::stoi(want)) || name == want) {
                target = &rp;
                break;
            }
            if (name.rfind(want, 0) == 0) ++prefixes, prefix = &rp;
        }
        if (!target && prefixes == 1) target = prefix;
        if (!target) {
            ui::TheChat().AddError(Format(TR("Нет игрока «%s». Список игроков: /players.", "No player \"%s\". The player list: /players."),
                                          args[0].c_str()));
            return true;
        }
        if (!target->hasState) {
            ui::TheChat().AddError(Format(TR("%s ещё не в игре.", "%s is not in the game yet."), Net().PeerName(target->id).c_str()));
            return true;
        }
    } else if (inGame > 1) {
        RemotePlayer* host = Get(Net().HostId());
        if (Net().IsHost() || !host || !host->hasState) {
            ui::TheChat().AddError(TR("Укажите игрока: /tp <ник или номер> (список: /players).",
                                      "Name the player: /tp <nickname or number> (the list: /players)."));
            return true;
        }
        target = host;
    }
    if (!target) {
        ui::TheChat().AddError(TR("Напарник ещё не в игре.", "Your partner is not in the game yet."));
        return true;
    }
    CPlayerPed* me = FindPlayerPed(0);
    if (cmd == "tp") {
        CVector p = target->state.pos + CVector{1.0f, 0, 0};
        int handle = GetPedRef(me);
        Cmd(0x04BB).I(target->state.area).Run();
        Cmd(0x0860).I(handle).I(target->state.area).Run();
        Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();
        LightArea(target->state.area, p);
        ui::TheChat().AddSystem(Format(TR("Вы переместились к %s.", "You went over to %s."), Net().PeerName(target->id).c_str()));
    } else {
        CVector p = me->GetPosition() + CVector{1.0f, 0, 0};
        Writer w(proto::G_TELEPORT);
        w.U8(target->id);
        w.F32(p.x);
        w.F32(p.y);
        w.F32(p.z - 1.0f);
        w.I8(me->AreaCode());
        Net().Send(proto::CH_RELIABLE, w);
        ui::TheChat().AddSystem(Format(TR("%s перемещён к вам.", "%s was brought over to you."), Net().PeerName(target->id).c_str()));
    }
    return true;
}

}  // namespace coop
