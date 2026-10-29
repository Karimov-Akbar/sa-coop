#include "entities/player/Players.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
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

bool EnteringCar(CPed* ped);

void ApplyRemote(RemotePlayer& rp) {
    if (rp.stats.size() == kStatsBlockSize) memcpy((void*)kStatsBlock, rp.stats.data(), kStatsBlockSize);  // its skills, stamina...
    Ref<uint8_t>(kShowUpdateStats) = 0;
    CPad* p0 = GetPad(0);
    CPad* p1 = GetPad(1);
    p0->NewState = p1->NewState = rp.padNew;
    p0->OldState = p1->OldState = rp.padOld;
    // its owner's activity holds it (a fall, a parachute, a script's animation): there its pad moves nothing either. Nor in
    // a script person's place: the owner's buttons play the second player's pool there, not this ped
    if (rp.animHeld || rp.avatar >= 0) p0->NewState = p1->NewState = p0->OldState = p1->OldState = CControllerState{};
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
    // A task walks its owner (the walk-in through a shop's door, a script's go-to) while its pad is still: the copy slid
    // along standing and spun round. Its stick here is the one that walks it the same way: the game turns a player to
    // atan2(-x, -y) of the stick less the camera's orientation, a stick of 60 walks and of 120 runs (as fast as its owner
    // goes: its move state said run while the door's walk-in walked it). Not while the copy walks to a car door by its own
    // enter task (its owner's walks it there too): the stick broke that walk off, and the copy trod on the spot until the
    // warp a few seconds later (27.10).
    if ((rp.state.flags & OnFootState::F_TASK_WALK) && !rp.animHeld && rp.mode == RemotePlayer::Mode::OnFoot && !EnteringCar(rp.ped)) {
        float speed = rp.state.vel.Length2D();
        float stick = speed > 2.6f ? 120.0f : speed > 0.3f ? 60.0f : 0.0f;
        float a = rp.state.rotation + c.orientation;
        p0->NewState.LeftStickX = p1->NewState.LeftStickX = (int16_t)(-std::sin(a) * stick);
        p0->NewState.LeftStickY = p1->NewState.LeftStickY = (int16_t)(-std::cos(a) * stick);
    }
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
    // The side comes from the car camera's looking left/right, and the camera is ours: its owner's camera sets it for the
    // call. Taken from its look buttons, the copy fired out of the window where its owner only looked aside (another
    // camera there, or drive-bys a mission's rule forbade, #30).
    const RemotePlayer* rp = Players::Instance().FindByPed(ped);
    if (!rp || rp->state.cam.driveByOff) {
        typeStatus = saved;
        return;
    }
    auto* camLook = (uint8_t*)(cam::Active() + 0x08);
    uint8_t look[2] = {camLook[0], camLook[1]};
    camLook[0] = rp->state.cam.lookLeft;
    camLook[1] = rp->state.cam.lookRight;
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
// passengers whose drive-by this started (a mission's is left alone), and their CPlayerInfo::m_bCanDoDriveBy (+0x153) before
// it: the task clears it at its start (0x621AEF), and only its abort sets it again (0x62D336), not the let go below. Left
// at 0 it kept the SMG out of the player's hands at the next car entry (CPed::RemoveWeaponWhenEnteringVehicle, #42).
std::map<CPed*, uint8_t> g_ourDriveBy;
bool AimFromSeat(const CPad* pad) { return pad->NewState.RightShoulder1 || pad->NewState.m_bVehicleMouseLook; }

// our player leans out of a passenger's window (the right mouse button or R1 held)
bool PassengerAiming() {
    CPlayerPed* me = FindPlayerPed(0);
    return Net().Online() && me && me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() != me && AimFromSeat(GetPad(0));
}
// In a car the mouse wheel and a pad's d-pad up and down are the radio (VEHICLE_RADIO_STATION_UP/DOWN set DPadUp and
// DPadDown). Leaning out of a passenger's window they go round the guns (PassengerWeaponWheel), and the radio
// (CAERadioTrackManager, its two calls) does not hear them meanwhile.
using StationFn = bool(__fastcall*)(CPad*, void*);
StationFn g_origNextStation = nullptr, g_origLastStation = nullptr;
bool __fastcall HookNextStation(CPad* pad, void* edx) { return !PassengerAiming() && g_origNextStation(pad, edx); }
bool __fastcall HookLastStation(CPad* pad, void* edx) { return !PassengerAiming() && g_origLastStation(pad, edx); }

bool __cdecl PassengerDriveBy() {
    const CPad* pad = GetPad(0);
    CPlayerPed* ped = FindPlayerPed(-1);
    if (!Net().Online() || !ped || pad->DisablePlayerControls || !AimFromSeat(pad) || cam::Mode() == 0x37) return false;  // MODE_AIMWEAPON_FROMCAR
    const CWeapon& w = ped->ActiveWeapon();
    void* info = w.type ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
    if (!info || Field<int32_t>(info, 0) != 1 || w.ammoTotal <= 0) return false;  // m_nWeaponFire: FIRE_INSTANT_HIT
    void* playerInfo = CallMethod<0x609FF0, void*>(ped);  // CPlayerPed::GetPlayerInfoForThisPlayerPed
    g_ourDriveBy.try_emplace(ped, playerInfo ? Field<uint8_t>(playerInfo, 0x153) : (uint8_t)1);
    return true;
}

int WeaponModel(int weaponType);

using GangDriveByFn = bool(__fastcall*)(void*, void*, CPed*);
GangDriveByFn g_origGangDriveBy = nullptr;
bool __fastcall HookGangDriveByProcess(void* task, void* edx, CPed* ped) {
    auto ours = g_ourDriveBy.find(ped);
    // The other player's copy leans out with its owner's gun: a pistol's model was not in memory here (a copy's guns come
    // without a blocking load), the task's start added none, and it fired from its fingers (issue #65). Our own player
    // with a rifle had none either: the task adds the models of the drive-by guns only.
    bool player = ped == (CPed*)FindPlayerPed(0) || Players::Instance().FindByPed(ped);
    // the gun's info the task keeps from its start: taken anew each step, as the gun in hand can change while leaning out
    // (PassengerWeaponWheel, the other player's copy following its owner's)
    if (player) Field<void*>(task, 0x30) = nullptr;  // CTaskSimpleGangDriveBy::m_pWeaponInfo
    auto gunInHand = [ped] {
        if (Field<void*>(ped, 0x4F4)) return;  // CPed::m_pWeaponObject
        if (int model = WeaponModel(ped->ActiveWeapon().type); model > 0) {
            if (IsModelLoaded(model)) CallMethod<0x5E5ED0>(ped, model);  // CPed::AddWeaponModel
            else RequestModel(model, 0);
        }
    };
    if (player) gunInHand();
    // let go: the task's own way out, command 7 (m_nNextCommand; a player's pad sets only 1 and 2 below 4). The rest of
    // the lean-out animation brings the passenger back in, and the task finishes when it is over. Finishing it at once
    // (m_bIsFinished) left the passenger leaning out of the window with the gun until two clicks.
    if (ours != g_ourDriveBy.end() && !AimFromSeat(GetPad(0))) Field<int8_t>(task, 0x0F) = 7;
    bool done = g_origGangDriveBy(task, edx, ped);
    if (player && !done) gunInHand();  // taken away in the task's own step: the frame is drawn with it (#35)
    if (done && ours != g_ourDriveBy.end()) {
        if (void* playerInfo = CallMethod<0x609FF0, void*>(ped)) Field<uint8_t>(playerInfo, 0x153) = ours->second;
        g_ourDriveBy.erase(ours);
    }
    return done;
}

// Two players dead or busted at about the same time come back at the same hospital or police station door, one inside
// the other: the one whose game was done first stood stuck in the other, still in its respawn view, until that ended.
// Ours comes back 1.5 m to the right when the other player's copy is at that door already (both at once: KeepApart).
using RestoreFn = void(__cdecl*)(CPlayerPed*, CVector, float);
RestoreFn g_origRestore = nullptr;
void __cdecl HookRestorePlayer(CPlayerPed* player, CVector pos, float heading) {
    // Killed or busted in the host's mission (its enemies go for every PLAYER1): back by the host, not at a hospital
    // across the map while the mission goes on. Only one who died near the host: where it was is still its place now.
    if (Net().Online() && !Net().IsHost() && player == FindPlayerPed(0) && HostMissionRecent(0))
        if (const RemotePlayer* host = Players::Instance().Get(Net().HostId());
            host && host->ped && host->hasState && host->mode == RemotePlayer::Mode::OnFoot && host->state.area == 0 &&
            !(host->state.flags & OnFootState::F_DEAD) && (player->GetPosition() - host->state.pos).Length() < 300.0f) {
            float h = host->state.rotation;
            pos = host->state.pos + CVector{std::cos(h), std::sin(h), 0.0f} * 2.0f;  // at its right hand
            heading = h * 57.29578f;
            LOGI("Respawn by the host: its mission goes on");
        }
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
    // A katana's forced kill takes the head off (CPed::RemoveBodyPart: bRemoveHead, m_nBodypartToRemove) and a respawn never
    // puts it back (CPlayerPed::SetInitialState leaves both): in single player no katana kills CJ so, but the other
    // player's copy is a PLAYER1 ped, whose katana does (dnya, 29.10: both came back headless)
    if (player == FindPlayerPed(0) && (player->PedFlags() & 0x8000u)) {
        player->PedFlags() &= ~0x8000u;
        Field<int8_t>(player, 0x754) = -1;  // as the constructor sets it
        LOGI("Respawn: the head is back");
    }
    g_origRestore(player, pos, heading);
}

// The copy dies by TASK_DIE, and that task always falls the same way (KO_shot_front): its owner fell otherwise (a
// spin, a skid back, hit in the face). Our TASK_DIE gets the owner's animation (G_PLAYER_DEATH) in its constructor.
int g_dieAnim = -1;  // set around our TASK_DIE: the default group's animation to fall with
using DieCtorFn = void*(__fastcall*)(void*, void*, int, int, int, float, float, bool, bool, int, bool);
DieCtorFn g_origDieCtor = nullptr;
void* __fastcall HookDieCtor(void* task, void* edx, int weapon, int group, int anim, float delta, float speed, bool stealth,
                             bool falling, int dir, bool railing) {
    if (g_dieAnim >= 0) group = 0, anim = g_dieAnim;  // ANIM_GROUP_DEFAULT
    return g_origDieCtor(task, edx, weapon, group, anim, delta, speed, stealth, falling, dir, railing);
}

// DELETE_CHAR deletes whoever it is given (CTheScripts::RemoveThisPed). A mission that clears "the player's car" for
// the people it brings deletes its passengers: in "Sweet's Girl" the host rode with the partner driving, the host's own
// CJ was deleted, and the host's game crashed on its next look at him (the partner's screen stayed dark). A player, ours
// or the other one's copy, only gets out of the car; vanilla scripts never delete a player.
// Ours riding beside the other player moves to a free back seat instead (the mission frees seats 0 and 1 for its own
// people): put out, the host was no longer in the car, and Sweet and the girl, his group, got out after him again and
// again (28.10). The other one's copy is moved by its own game (YieldClaimedSeat).
using RemovePedFn = void(__cdecl*)(CPed*);
RemovePedFn g_origRemoveThisPed = nullptr;
void __cdecl HookDeleteChar(CPed* ped) {
    if (ped && ped->IsPlayer()) {
        int seat = -1;
        if (CVehicle* v = ped->InVehicle() ? ped->Vehicle() : nullptr) {
            if (ped == FindPlayerPed(0) && v->Driver() != ped)
                for (int s = std::min<int>(v->MaxPassengers(), 8) - 1; s >= 0 && seat < 0; --s)
                    if (!v->Passengers()[s]) seat = s;
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            if (seat >= 0) Cmd(0x0430).I(GetPedRef(ped)).I(GetVehicleRef(v)).I(seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        }
        if (seat >= 0) LOGW("A script deleted a player (ours): moved to seat %d of its car instead", seat);
        else LOGW("A script deleted a player (%s): out of its car instead", ped == FindPlayerPed(0) ? "ours" : "a copy");
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
    // Aiming, the game turned the copy to where its owner's camera aims from where the copy stands: up to 7 degrees off.
    // A half turn on foot could also go round the other way. It faces as its owner.
    if (rp->mode == RemotePlayer::Mode::OnFoot && rp->hasState && !rp->animHeld && !self->InVehicle() &&
        !(rp->state.flags & OnFootState::F_DEAD) &&
        ((rp->state.pad.buttons & PadState::B_RS1) || std::fabs(WrapAngle(rp->state.rotation - self->CurrentRotation())) > 0.8f))
        self->CurrentRotation() = self->AimingRotation() = rp->state.rotation;
    Field<bool*>(self, 0x500) = nullptr;  // CPed::m_pGogglesState: taking its goggles off must not switch ours off
}

// A player's gun on foot fires after the frame's processing (CTaskSimpleUseGun::SetPedPosition from
// CWorld::ProcessPedsAfterPreRender), not in its ProcessControl, and only while the gun's info its task took is that of
// the skill read then (CPed::GetWeaponSkill: a player's comes from CStats). The copy's was taken with its owner's skills
// and met ours there: with their skills for that gun apart (poor, gangster, hitman) the copy aimed and never fired, and
// neither player saw the other's shots (Holder, 29.10). A shot due this frame fires in the copy's context: its skills,
// and where its owner aims instead of our camera.
using UseGunPositionFn = bool(__fastcall*)(void*, void*, CPed*);
UseGunPositionFn g_origUseGunPosition = nullptr;
bool __fastcall HookUseGunPosition(void* task, void* edx, CPed* ped) {
    // m_IsFiringGunRightHandThisFrame, m_IsFiringGunLeftHandThisFrame
    RemotePlayer* rp = g_inRemoteContext || !(Field<uint8_t>(task, 0x0D) & 3) ? nullptr : Players::Instance().FindByPed(ped);
    if (!rp || !rp->ped) return g_origUseGunPosition(task, edx, ped);
    LocalContext saved;
    SaveLocal(saved);
    ApplyRemote(*rp);
    g_inRemoteContext = true;
    bool r = g_origUseGunPosition(task, edx, ped);
    g_inRemoteContext = false;
    RestoreLocal(saved);
    return r;
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
    if (Net().Online() && !Cfg().cleanScreen) Players::Instance().DrawWeaponIcons();
}

// a colour for CHANGE_BLIP_COLOUR: the game's own (CRadar::GetRadarTraceColour: 0 red, 1 green, 2 blue, 3 white, 4
// yellow) or any other as RGBA (it takes a value over 8 as one); -1: none, or not one (64 bits: a red over 0x7F is an RGBA
// with the top bit set, it was taken for none)
int64_t BlipColour(const std::string& name) {
    static const std::pair<const char*, int> kNamed[] = {{"red", 0}, {"green", 1}, {"blue", 2}, {"white", 3}, {"yellow", 4}};
    for (auto& [n, c] : kNamed)
        if (name == n) return c;
    if (name.size() != 6 || name.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return -1;
    return (int64_t)((uint32_t)strtoul(name.c_str(), nullptr, 16) << 8 | 0xFF);
}

// The other player on the radar and the map as an arrow turned with its heading, the way CRadar::DrawBlips draws the
// game's own player (the centre sprite, DrawRotatingRadarSprite), in its mark's colour: a dot did not tell where it went
// (asked for in the chat on October 25). Its blip stays for the rest, not drawn (CHANGE_BLIP_DISPLAY 0). Drawn from
// Waypoints' hook of CRadar::DrawBlips: a second hook of ours on it was refused (MH_ERROR_ALREADY_CREATED).
bool g_arrowHook = false;
bool Arrows() { return g_arrowHook && Cfg().partnerArrow; }
void DrawArrows() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!Arrows() || !Net().Online() || !me) return;
    // SCREEN_STRETCH_X(5), RsGlobal.maximumWidth: smaller than CJ's own arrow (8), which it looked bigger than (27.10)
    uint32_t size = (uint32_t)(Ref<int32_t>(0xC17044) / 640.0f * 5.0f);
    bool topDown = Ref<uint16_t>(0xB6F1A8 + Ref<uint8_t>(0xB6F081) * 0x238u) == 1;  // TheCamera's active cam: MODE_TOPDOWN
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.blip || !rp.ped || !IsPedValid(rp.ped) || rp.ped->AreaCode() != me->AreaCode()) continue;
        // in the colour its player chose for itself. CRadar::GetRadarTraceColour: one of the game's (the allies' blue for
        // none: a threat as a friend), or RGBA as it is
        int64_t c = BlipColour(rp.markColour);
        uint32_t rgba = c > 8 ? (uint32_t)c : Call<0x584770, uint32_t>((int)(c < 0 ? 7 : c), true, true);
        CRGBA colour{(uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), 255};
        CEntity* e = rp.ped->InVehicle() && rp.ped->Vehicle() ? (CEntity*)rp.ped->Vehicle() : (CEntity*)rp.ped;
        CMatrix* m = e->Matrix();
        if (!m) continue;
        CVector p = e->GetPosition();
        CVector2D world{p.x, p.y}, radar, screen;
        Call<0x583530>(&radar, &world);   // CRadar::TransformRealWorldPointToRadarSpace
        Call<0x5832F0, float>(&radar);    // CRadar::LimitRadarPoint: on the edge when farther
        Call<0x583480>(&screen, &radar);  // CRadar::TransformRadarPointToScreenSpace
        float heading = std::atan2(-m->forward.x, m->forward.y);  // CPlaceable::GetHeading
        float angle = topDown ? heading + 3.14159265f : heading - (Ref<float>(0xBA8310) + 3.14159265f);  // m_fRadarOrientation
        // CRadar::RadarBlipSprites[RADAR_SPRITE_CENTRE]
        Call<0x584850>((void*)0xBAA258, screen.x, screen.y, angle, size, size, colour);  // CRadar::DrawRotatingRadarSprite
    }
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

// The game's recruit at the group key (CPlayerPed::ProcessGroupBehaviour) asks only for a Grove member's ped type, and a
// copy of the other game's has it: it joined our group before its hand-over, then joined again with it, or stayed in the
// group while its owner moved it (#33). Another game's person comes with its hand-over (Peds::AskForTheirs).
using JoinGroupFn = void(__fastcall*)(CPlayerPed*, void*, CPed*);
JoinGroupFn g_origJoinGroup = nullptr, g_origJoinGroup2 = nullptr;
bool TheirCopy(CPed* ped) {
    NetPed* np = Net().Online() ? Peds::Instance().Find(ped) : nullptr;
    return np && np->owner != Net().MyId();
}
void __fastcall HookJoinGroup(CPlayerPed* self, void* edx, CPed* ped) {  // CPlayerPed::MakeThisPedJoinOurGroup
    if (!TheirCopy(ped)) g_origJoinGroup(self, edx, ped);
}
void __fastcall HookJoinGroup2(CPlayerPed* self, void* edx, CPed* ped) {
    if (!TheirCopy(ped)) g_origJoinGroup2(self, edx, ped);
}

// A save whose player had a weapon type no weapon has (slot 2 of a host's save, four times on loading it: something
// outside the mod wrote it) crashed the load in CStreaming::RequestModel (0x404C95): CPedSaveStructure::Extract took the
// model of its GetWeaponInfo, read past the table. The slot is left empty (type 0: Extract skips it).
using PedExtractFn = void(__fastcall*)(uint8_t*, void*, CPed*);
PedExtractFn g_origPedExtract = nullptr;
void __fastcall HookPedExtract(uint8_t* s, void* edx, CPed* ped) {
    for (int i = 0; i < 13; ++i)  // CPedSaveStructure::m_aWeapons: CWeapon, type first
        if (int32_t& type = Field<int32_t>(s, 0x14 + i * 0x1C); type < 0 || type > kLastWeapon) {
            LOGW("A loaded player had weapon type %d in slot %d: left out", type, i);
            type = 0;
        }
    if (int8_t& slot = Field<int8_t>(s, 0x181); slot < -1 || slot > 12) slot = 0;  // the weapon in hand (SetCurrentWeapon)
    g_origPedExtract(s, edx, ped);
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

// A passenger keeps the gun it got in with (#83: in with an M4 on purpose, it sat with the Tec-9 or the Uzi). At the end
// of every get-in the game takes an SMG out for a driver's drive-by (CPed::RemoveWeaponWhenEnteringVehicle); a passenger
// leans out here with any instant-hit gun (PassengerDriveBy). With drive-bys off for the call the game puts the gun in hand
// away as in a car with no drive-by, and the lean-out brings it back (HookGangDriveByProcess). An SMG in hand, fists and
// melee weapons go the game's way.
using RemoveWeaponEnterFn = void(__fastcall*)(CPed*, void*, int);
RemoveWeaponEnterFn g_origRemoveWeaponEnter = nullptr;
void __fastcall HookRemoveWeaponEnter(CPed* ped, void* edx, int jetpack) {
    void* playerInfo = nullptr;
    if (Net().Online() && !jetpack && ped == (CPed*)FindPlayerPed(0)) {
        int task = ActivePrimaryTaskType(ped);
        const CWeapon& w = ped->ActiveWeapon();
        void* info = w.type ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
        if ((task == TASK_ENTER_CAR_AS_PASSENGER || task == TASK_ENTER_CAR_AS_PASSENGER_TIMED) && info &&
            Field<int32_t>(info, 0) == 1 && w.ammoTotal > 0 && w.type != 28 && w.type != 29 && w.type != 32)  // FIRE_INSTANT_HIT; Uzi, MP5, Tec-9
            playerInfo = CallMethod<0x609FF0, void*>(ped);  // CPlayerPed::GetPlayerInfoForThisPlayerPed
    }
    uint8_t canDriveBy = playerInfo ? Field<uint8_t>(playerInfo, 0x153) : 0;  // CPlayerInfo::m_bCanDoDriveBy
    if (playerInfo) Field<uint8_t>(playerInfo, 0x153) = 0;
    g_origRemoveWeaponEnter(ped, edx, jetpack);
    if (playerInfo) {
        Field<uint8_t>(playerInfo, 0x153) = canDriveBy;
        LOGI("In as a passenger with gun %d: kept in hand, no SMG instead", (int)ped->ActiveWeapon().type);
    }
}
// its car has gone on without it (on the move, or away): an enter animation would only run after it
bool CarGoneOn(CVehicle* v, CPed* ped, float maxSpeed = 3.0f) {
    return v->MoveSpeed().Length() * 50.0f > maxSpeed || (v->GetPosition() - ped->GetPosition()).Length() > 8.0f;
}
// A car its owner still walks up to (on foot: a jack of one rolling to a stop) is entered as there up to this speed. At
// 3 m/s the copy never went to the door, and the driver here, pulled out by the sync, played its jacked animation by a
// lamp post while the copy was warped in (28.10)
constexpr float kOwnerWalksUpSpeed = 8.0f;

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
    const auto* look = (const uint8_t*)(cam::Active() + 0x08);  // CCam::m_bLookingLeft, m_bLookingRight
    c.lookLeft = look[0] != 0, c.lookRight = look[1] != 0;
    c.driveByOff = !Field<uint8_t>(PlayerInfo(0), 0x153);  // CPlayerInfo::m_bCanDoDriveBy
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

// A model in memory, else asked for (it comes in a few frames, and a later try uses it): LoadModelNow loads the whole
// streaming queue with it, and the other player's weapon or phone stood this game still for a second or more (issue #43)
bool ModelReady(int model) {
    if (IsModelLoaded(model)) return true;
    RequestModel(model, 0);
    return false;
}

// a go-to task walks the player (900-939: TASK_SIMPLE_GO_TO_POINT .. TASK_COMPLEX_GOTO_DOOR_AND_OPEN 932 ..), not its pad;
// so does an entrance to the end of its walk-in (CEntryExitManager::ms_exitEnterState): the fade-out walks it on under
// TASK_SIMPLE_UNINTERRUPTABLE with its pad back on, and the copy ran off by that stick and the door's fixed camera
bool TaskWalks(CPed* ped) {
    int t = ActivePrimaryTaskType(ped);
    // a step of a script's sequence (a scene's walk to the van) lies under the sequence: its simplest active task
    void* intel = ped ? ped->Intelligence() : nullptr;
    void* leaf = intel ? CallMethod<0x6819D0, void*>((uint8_t*)intel + 4) : nullptr;  // CTaskManager::GetSimplestActiveTask
    int l = leaf ? CallVirtual<4, int>(leaf) : -1;                                    // CTask::GetTaskType
    return (t >= 900 && t < 940) || (l >= 900 && l < 940) || (ped == FindPlayerPed(0) && Ref<int32_t>(0x96A7CC) != 0);
}

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
    mem::HookFn(0x624ED0, &HookUseGunPosition, &g_origUseGunPosition, "CTaskSimpleUseGun::SetPedPosition");
    void* coopGame = nullptr;  // CGameLogic::IsCoopGameGoingOn, always false (Patches)
    mem::RedirectCall(0x644A1A, (const void*)&PassengerDriveBy, &coopGame);
    mem::Nop(0x644A27, 28);  // the SMG slot holds a weapon with ammo (PassengerDriveBy checks the gun in hand)
    mem::Nop(0x644A69, 6);   // jne: the camera is MODE_TWOPLAYER_IN_CAR_AND_SHOOTING
    mem::HookFn(0x62D3B0, &HookGangDriveByProcess, &g_origGangDriveBy, "CTaskSimpleGangDriveBy::ProcessPed");
    mem::HookFn(0x5E6370, &HookRemoveWeaponEnter, &g_origRemoveWeaponEnter, "CPed::RemoveWeaponWhenEnteringVehicle");
    if (!mem::RedirectCallFn(0x4EB72A, &HookNextStation, &g_origNextStation) ||  // CAERadioTrackManager -> CPad::NextStationJustUp
        !mem::RedirectCallFn(0x4EB753, &HookLastStation, &g_origLastStation))    // -> CPad::LastStationJustUp
        LOGW("The radio keeps the wheel for a passenger leaning out: no call at 0x4EB72A or 0x4EB753 (another mod?)");
    mem::HookFn(0x442060, &HookRestorePlayer, &g_origRestore, "CGameLogic::RestorePlayerStuffDuringResurrection");
    mem::HookFn(0x630040, &HookDieCtor, &g_origDieCtor, "CTaskComplexDie::CTaskComplexDie");
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
    mem::RedirectCallFn(0x5D46A4, &HookPedExtract, &g_origPedExtract);    // CPed::Load: CPedSaveStructure::Extract
    mem::RedirectCallFn(0x60D411, &HookJoinGroup, &g_origJoinGroup);      // CPlayerPed::ProcessGroupBehaviour, recruit
    mem::RedirectCallFn(0x60D4D5, &HookJoinGroup2, &g_origJoinGroup2);
    mem::RedirectCallFn(0x5D2D95, &HookLoadPedCount, &g_origLoadPedCount);  // in CPools::LoadPedPool
    mem::RedirectCallFn(0x5D2E07, &HookLoadedPlayerCtor, &g_origLoadedPlayerCtor);
    Net().SetHandler(proto::G_PLAYER_ONFOOT, [](uint8_t from, Reader& r) { Instance().HandleOnFoot(from, r); });
    Net().SetHandler(proto::G_TELEPORT, [](uint8_t from, Reader& r) { Instance().HandleTeleport(from, r); });
    // the colour a player chose for its own mark: its arrow and blip here (the host picked the partner's before)
    Net().SetHandler(proto::G_MARK_COLOUR, [](uint8_t from, Reader& r) {
        std::string colour = ToLower(r.Str(8));
        if (!r.Ok() || from == Net().MyId() || (!colour.empty() && BlipColour(colour) < 0)) return;
        RemotePlayer& rp = Instance().Touch(from);
        if (rp.markColour == colour) return;
        rp.markColour = colour;
        if (rp.blip) Cmd(0x0165).I(rp.blip).I(colour.empty() ? 2 : (int)BlipColour(colour)).Run();  // CHANGE_BLIP_COLOUR
        LOGI("Player %d's mark: %s", from, colour.empty() ? "default" : colour.c_str());
    });
    Net().SetHandler(proto::G_PLAYER_DEATH, [](uint8_t from, Reader& r) {
        int16_t anim = r.I16();
        uintptr_t grp = Ref<uintptr_t>(0xB4EA34);  // CAnimManager::ms_aAnimAssocGroups[0]: the default group (ped.ifp)
        int first = grp ? (int)Ref<uint32_t>(grp + 0x0C) : 0, count = grp ? (int)Ref<uint32_t>(grp + 0x08) : 0;
        RemotePlayer* rp = Instance().Get(from);
        if (!r.Ok() || !rp || anim < first || anim >= first + count) return;
        rp->dieAnim = anim;
        rp->dieMs = NowMs();
    });
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

void Players::OnSessionStart() { m_lastSend = m_markSentMs = 0; }

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
        UseStreetDoor(s.area, {s.x, s.y, s.z}, {s.x, s.y, s.z});
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
        Cmd(0x016A).I(500).I(1).Run();  // DO_FADE in: the reload went into black (Flow)
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
    // a CPlayerPed takes a ped group (its constructor, CPedGroups::AddGroup at 0x60D659) and does not check for none: with
    // all 8 taken (gangs at their hangouts, the scripts' groups) it wrote before CPedGroups::ms_groups
    bool groupFree = false;
    for (int i = 0; i < 8; ++i) groupFree |= !Ref<uint8_t>(0xC098E0 + (uintptr_t)i);  // CPedGroups::ms_activeGroups
    if (!groupFree) {
        static uint64_t warnedMs = 0;
        if (NowMs() - warnedMs > 10000) warnedMs = NowMs(), LOGW("No free ped group for the other player's copy: waiting");
        return;
    }
    const CVector p = rp.state.pos;
    int playerIndex = -1, handle = -1;
    // made high above its place: CREATE_PLAYER clears the space around itself of this game's random peds and cars
    // (the taxi the other player sits in), and it goes where it is right after
    float timeScale = Ref<float>(0xB7CB64);  // CTimer::ms_fTimeScale
    Cmd(0x0053).I(kRemoteSlot).F(p.x).F(p.y).F(p.z + 300.0f).Out(&playerIndex).Run();  // CREATE_PLAYER
    // a CPlayerPed's constructor sets the game's speed back to normal (SetInitialState, 0x60CD41): a stunt jump's slow
    // motion here ended in mid-air whenever the copy was made again (5 s without its states, its death)
    Ref<float>(0xB7CB64) = timeScale;
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
    // radar blip, unless the player hid them (partner_blip = 0: a shoot-out not knowing where the other is, issue #44)
    int blip = 0;
    if (Cfg().partnerBlip) Cmd(0x0187).I(handle).Out(&blip).Run();  // ADD_BLIP_FOR_CHAR
    if (blip) {
        Cmd(0x07E0).I(blip).I(1).Run();       // SET_BLIP_AS_FRIENDLY
        Cmd(0x0168).I(blip).I(2).Run();       // CHANGE_BLIP_SCALE
        Cmd(0x018B).I(blip).I(Arrows() ? 0 : 2).Run();  // CHANGE_BLIP_DISPLAY: radar only, no 3D arrow (our arrow: none)
        if (int64_t colour = BlipColour(rp.markColour); colour >= 0) Cmd(0x0165).I(blip).I((int)colour).Run();  // CHANGE_BLIP_COLOUR
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

static void StartEnter(RemotePlayer& rp, NetVehicle& nv, int8_t seat) {
    rp.enterTaskMs = NowMs();
    if (seat < 0)
        Cmd(0x05CB).I(rp.pedHandle).I(nv.handle).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
    else
        Cmd(0x05CA).I(rp.pedHandle).I(nv.handle).I(10000).I(SeatOrAny(nv.veh, seat)).Run();  // TASK_ENTER_CAR_AS_PASSENGER
}

void Players::HandleEnterCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    if (!rp || !rp->ped) return;
    NetVehicle* nv = Vehicles::Instance().Find(netId);
    // a car deleted in the last frame is still listed (Vehicles drops it in its OnPreProcess, after the messages). One
    // not here yet (its model loads) gets the enter when it comes: the copy stood still, then was warped in (Hh hh, 26.10)
    if (!nv || !Vehicles::Alive(*nv)) {
        rp->enterNetId = netId, rp->enterSeat = seat, rp->enterAskedMs = NowMs();
        return;
    }
    rp->enterNetId = 0;
    if (!CarGoneOn(nv->veh, rp->ped, rp->mode == RemotePlayer::Mode::OnFoot ? kOwnerWalksUpSpeed : 3.0f)) StartEnter(*rp, *nv, seat);
    else LOGD("Remote player %d enters vehicle %04X: not here, it went on (%.1f m/s, %.1f m off)", from, netId,
              nv->veh->MoveSpeed().Length() * 50.0f, (nv->veh->GetPosition() - rp->ped->GetPosition()).Length());
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
    CVector was = me->GetPosition();
    Cmd(0x04BB).I(area).Run();                 // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(area).Run();       // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(pos.x).F(pos.y).F(pos.z).Run();  // SET_CHAR_COORDINATES
    LightArea(area, pos);
    if (const RemotePlayer* rp = Get(from)) UseStreetDoor(area, pos, rp->streetPos.x != 0.0f ? rp->streetPos : was);
    ui::TheChat().AddSystem(Format(TR("Игрок %s переместил вас к себе.", "%s brought you over."), Net().PeerName(from).c_str()));
}

// The copy in a script person's place (RemotePlayer::avatar): where that one stands and faces, and the animation it
// plays (anim::Main) at its time. False: no such person now, or the copy sits in a vehicle (its own sync goes on).
bool FollowAvatar(RemotePlayer& rp) {
    CPed* av = GetPed(rp.avatar);
    CPlayerPed* ped = rp.ped;
    if (!av || !IsPedValid(av) || ped->InVehicle()) return false;
    ped->CurrentRotation() = ped->AimingRotation() = av->CurrentRotation();
    // frozen (SetAvatar), its matrix kept its old way while the angle turned: it struck across the pool table (28.10)
    CallMethod<0x43E0C0>(ped, av->CurrentRotation());  // CPlaceable::SetHeading
    SetEntityPosition(ped, av->GetPosition());
    ped->MoveSpeed() = CVector{};
    void* best = anim::Main(av->RwObject());
    void* clump = ped->RwObject();
    if (!best || !clump) return true;
    void* h = anim::Hierarchy(best);
    void* mine = nullptr;
    for (void* a = anim::First(clump); a; a = anim::Next(a))
        if (anim::Hierarchy(a) == h && anim::BlendDelta(a) >= 0.0f) mine = a;
    if (!mine) mine = anim::Blend(clump, h, anim::Flags(best), 8.0f);
    if (mine) {
        Field<float>(mine, 0x24) = Field<float>(best, 0x24);  // CAnimBlendAssociation::m_fSpeed
        if (std::fabs(anim::CurrentTime(mine) - anim::CurrentTime(best)) > 0.1f) anim::SetCurrentTime(mine, anim::CurrentTime(best));
    }
    rp.avatarAnim = h;
    return true;
}

// Both dancing on one club spot (BAR_AMBIENCE starts DANCE at its marker, each game at the same place: a contest brings
// the second player there): the other one dances a step to our right instead of inside us, where it was hidden (27.10).
// Each game sees the other at its right hand.
bool WeDance() {
    static uint64_t checkedMs = 0;
    static bool dancing = false;
    if (NowMs() - checkedMs < 250) return dancing;
    checkedMs = NowMs();
    dancing = false;
    for (void* s = Ref<void*>(0xA8B42C); s && !dancing; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        dancing = !_strnicmp(Field<char[8]>(s, 0x08), "dance", 8);
    return dancing;
}
CVector DanceSide(const OnFootState& st) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->InVehicle() || st.area != me->AreaCode() || !WeDance()) return {};
    CVector d = st.pos - me->GetPosition();
    if (d.x * d.x + d.y * d.y > 1.0f || std::fabs(d.z) > 1.5f) return {};
    float h = me->CurrentRotation();
    return CVector{std::cos(h) * 1.2f, std::sin(h) * 1.2f, 0.0f};
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
    // in its vehicle in its game, not yet in its copy here: its driving buttons made the copy run and jump about until
    // the warp (accelerate as sprint, brake as jump)
    if (rp.mode != RemotePlayer::Mode::OnFoot && !ped->InVehicle()) rp.padNew = CControllerState{};
    if (rp.enterNetId) {  // the enter that waited for its vehicle's copy
        NetVehicle* nv = Vehicles::Instance().Find(rp.enterNetId);
        if (NowMs() - rp.enterAskedMs > 3000) rp.enterNetId = 0;
        else if (nv && Vehicles::Alive(*nv)) {
            // its owner sat down meanwhile and drove off: no walk after the car (ApplyVehicleSeat seats it at once)
            if (!ped->InVehicle() && !EnteringCar(ped) &&
                !CarGoneOn(nv->veh, ped, rp.mode == RemotePlayer::Mode::OnFoot ? kOwnerWalksUpSpeed : 3.0f))
                StartEnter(rp, *nv, rp.enterSeat);
            rp.enterNetId = 0;
        }
    }

    // death is decided by the owner's game (the local ped has all proofs): play it once; after the
    // respawn a fresh ped replaces the dead one, which cannot be revived
    if (st.flags & OnFootState::F_DEAD) {
        if (!rp.deadShown) {
            // its game's fall comes in a message of its own, a moment after the state or before it: waited for briefly
            if (!rp.deadSinceMs) rp.deadSinceMs = NowMs();
            bool known = NowMs() - rp.dieMs < 3000;
            if (!known && NowMs() - rp.deadSinceMs < 300) return;
            rp.deadShown = true;
            LOGI("Remote player %d died (%s %d)", rp.id, known ? "its own fall" : "the default fall", known ? rp.dieAnim : 15);
            g_dieAnim = known ? rp.dieAnim : -1;
            Cmd(0x05BE).I(rp.pedHandle).Run();  // TASK_DIE
            g_dieAnim = -1;
        }
        return;
    }
    rp.deadSinceMs = 0;
    if (rp.deadShown) {
        rp.deadShown = false;
        DestroyRemote(rp);  // SpawnRemote runs next frame
        return;
    }
    if (rp.avatar >= 0 && FollowAvatar(rp)) return;

    ped->Health() = (float)st.health;
    ped->Armour() = (float)st.armour;
    if (st.area == 0) {
        rp.streetPos = st.pos;
    } else if (rp.doorArea != st.area || rp.doorPed != ped) {  // just went in: its blip at its door, not at the raw spot
        rp.doorArea = st.area;
        rp.doorPed = ped;
        void* door = StreetDoorOf(st.area, st.pos, rp.streetPos);
        Field<void*>(ped, 0x78C) = door;  // CPed::m_pEnex
        LOGD("Remote player %d is in area %d: its door %.8s", rp.id, st.area, door ? (const char*)door : "-");
    }
    if (st.area == 0 && rp.doorArea != 0) rp.doorArea = 0, Field<void*>(ped, 0x78C) = nullptr;
    // Through a shop's door the game's walk-in (CTaskComplexGotoDoorAndOpen) turns its player round after the push on the
    // door and walks it back to the street spot while its screen is dark: the other player saw it come back out. The copy
    // goes out of sight once it turns from the door, until its owner is inside (KeepApart hides it).
    if ((st.flags2 & OnFootState::F2_DOOR) && rp.mode == RemotePlayer::Mode::OnFoot) {
        // out on the other side (on its way out it comes out facing the street): the turn counts from there
        if (!rp.doorMs || st.area != rp.doorFromArea)
            rp.doorMs = NowMs(), rp.doorHeading = st.rotation, rp.doorFromArea = st.area, rp.goneInDoor = false;
        else if (std::fabs(WrapAngle(st.rotation - rp.doorHeading)) > 2.0f) rp.goneInDoor = true;
    } else {
        rp.doorMs = 0, rp.goneInDoor = false;
    }
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
    if (phone != (Field<int32_t>(ped, 0x740) == kCellphone) && ModelReady(kCellphone)) {
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
    if (jetpack != HasJetpack(ped) && rp.mode == RemotePlayer::Mode::OnFoot && NowMs() - rp.jetpackMs > 1000 &&
        (!jetpack || ModelReady(370))) {  // the jetpack's model
        rp.jetpackMs = NowMs();
        if (jetpack) {
            Cmd(0x07A7).I(rp.pedHandle).Run();   // TASK_JETPACK
        } else {
            Cmd(0x0792).I(rp.pedHandle).Run();   // CLEAR_CHAR_TASKS_IMMEDIATELY
        }
        LOGI("Remote player %d: jetpack %s", rp.id, jetpack ? "on" : "off");
    }

    // weapon in hand
    CWeapon& active = ped->ActiveWeapon();
    int weapon = st.weapon <= kLastWeapon ? st.weapon : 0;  // from the network
    if (int model = weapon != 0 ? WeaponModel(weapon) : 0;
        active.type != weapon && NowMs() - rp.lastWeaponTry > 500 && (model <= 0 || ModelReady(model))) {
        rp.lastWeaponTry = NowMs();
        if (weapon != 0) Cmd(0x01B2).I(rp.pedHandle).I(weapon).I(std::max<int>(st.ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
        Cmd(0x01B9).I(rp.pedHandle).I(weapon).Run();  // SET_CURRENT_CHAR_WEAPON
        // For a player's ped that command only picks the slot (m_nChosenWeapon), and CPlayerPed::ProcessWeaponSwitch takes
        // it on foot. In a car the copy kept the gun it got in with (the game's SMG for drive-bys) while its owner went
        // round its guns leaning out of the window (27.10): set at once there, the model only while it leans out.
        if (ped->InVehicle()) {
            CallMethod<0x5E6280>(ped, weapon);                                         // CPed::SetCurrentWeapon
            if (!rp.padNew.RightShoulder1 && model > 0) CallMethod<0x5E3990>(ped, model);  // CPed::RemoveWeaponModel
        }
    } else if (weapon != 0 && active.type == weapon) {
        active.ammoTotal = std::max<int>(st.ammo, 1);  // never run dry locally: the owner decides
        if (active.ammoInClip <= 0) active.ammoInClip = 1;
    }

    // the copy on a bike that is not ours (it is the other game's) falls only when its game says so: knocked off here
    // by this game's impacts it ran riderless until the next warp. On our bike it stays knockable, else our own
    // driver never fell (DamageKnockOffRider returns when either rider can never be knocked off).
    CVehicle* rv = ped->InVehicle() ? ped->Vehicle() : nullptr;
    uint32_t never = rv && rv->IsBike() && rv->Driver() != FindPlayerPed(0) ? 1u << 27 : 0;
    ped->PedFlags3() = (ped->PedFlags3() & ~(3u << 27)) | never;
    if (rp.mode != RemotePlayer::Mode::OnFoot) {
        ApplyVehicleSeat(rp);
        // A car through a door of its own (a mod garage moves it into CARMOD1-3, a stadium's events) changes its driver's
        // and its area in the driver's game only: the copy (culled by area) vanished, and our player riding along saw
        // the empty sky of an interior. The car takes its driver's area here, and so do we when we ride in it.
        if (CVehicle* v = rp.mode == RemotePlayer::Mode::Driver && ped->InVehicle() ? ped->Vehicle() : nullptr;
            v && v->Driver() == ped && v->AreaCode() != rp.state.area && !Ref<uint8_t>(0xB5F851)) {  // not in a cutscene
            v->AreaCode() = ped->AreaCode() = rp.state.area;
            if (CPlayerPed* me = FindPlayerPed(0); me->InVehicle() && me->Vehicle() == v && me->AreaCode() != rp.state.area) {
                int h = GetPedRef(me);
                Cmd(0x04BB).I(rp.state.area).Run();          // SET_AREA_VISIBLE
                Cmd(0x0860).I(h).I(rp.state.area).Run();     // SET_CHAR_AREA_VISIBLE
                LightArea(rp.state.area, me->GetPosition());
            }
            LOGI("Player %d's car went into area %d: here too", rp.id, rp.state.area);
        }
        return;
    }
    if (st.flags & OnFootState::F_IN_TRAIN) {
        RideTrain(rp);
        return;
    }
    // thrown off its bike in its game (on foot without an exit message) while our player rides behind it: ours falls too
    if (CPlayerPed* me = FindPlayerPed(0); rv && rv->IsBike() && rv->Driver() == ped && me->InVehicle() &&
                                           me->Vehicle() == rv && NowMs() - rp.exitTaskMs > 1000) {
        Cmd(0x0622).I(GetPedRef(me)).I(GetVehicleRef(rv)).Run();  // TASK_LEAVE_CAR_IMMEDIATELY
        rp.exitTaskMs = NowMs();
        LOGI("Player %d fell off the bike in its game: ours gets off too", rp.id);
    }
    // on foot but still sitting in a vehicle here: the exit animation gets a few seconds, then warp out. Not right after
    // its enter task either: the copy sat down before its owner's states said so and was warped out again.
    if (ped->InVehicle()) {
        if (NowMs() - rp.exitTaskMs > kEnterGraceMs && NowMs() - rp.enterTaskMs > kEnterGraceMs) {
            Cmd(0x0362).I(rp.pedHandle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            rp.exitTaskMs = NowMs();
        }
        return;
    }

    if (Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running: Missions places the ped in the scene

    // position: predict, then correct smoothly (teleport on large errors)
    float age = std::min(0.25f, (float)(NowMs() - rp.stateRecvMs) / 1000.0f);
    CVector side = DanceSide(st);
    CVector target = st.pos + st.vel * age + side;
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
    // A climb puts the copy on its wall's handholds by itself, a moment before or after its owner: pulled to its owner's
    // last spot meanwhile, it was dragged back over the edge it had just climbed and fell (FALL_glide) onto the top
    void* intel = ped->Intelligence();
    if (intel && CallMethod<0x681740, void*>((uint8_t*)intel + 4, 254)) rp.climbMs = NowMs();  // TASK_SIMPLE_CLIMB
    bool climbing = NowMs() - rp.climbMs < 500 && dist2d < 2.0f && std::fabs(err.z) < 2.5f;
    if (climbing) {
        // left to its own climb and the half second after it
    } else if (dist2d > kTeleportDist || std::fabs(err.z) > kTeleportDist || sunk) {
        CVector p = st.pos + side;
        Cmd(0x00A1).I(rp.pedHandle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES
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
        NowMs() - rp.enterTaskMs < (EnteringCar(ped) ? kEnterGraceMs : 1500))  // its own walk to the cab (Trains' TRAIN_ENTER)
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
    // getting out: its owner's states say "in the car" until it stands outside, and the copy, out first, was put back at
    // the wheel, then pulled out again
    if (NowMs() - rp.exitTaskMs < kEnterGraceMs) return;
    // give the enter animation time; otherwise put the player in place directly. Not once its car has gone on: its owner
    // sat down and drove off (a bike in a second), and the copy ran after it for those seconds before the warp (Hh hh,
    // Duke Nukem, 27.10)
    if (NowMs() - rp.enterTaskMs < kEnterGraceMs && EnteringCar(ped)) {
        if (!CarGoneOn(nv->veh, ped)) return;
        Cmd(0x0792).I(rp.pedHandle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: its walk to the door ends here
    }
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    // a player warped in as the driver of a car no player owned yet reports a car theft against FindPlayerPed, ours here
    // (CTaskSimpleCarSetPedInAsDriver, 0x64BBBA), and makes it a player's car: the local pad drove it until the next frame
    Field<uint8_t>(nv->veh, 0x42A) |= 2;  // CVehicle::bHasBeenOwnedByPlayer
    if (rp.seat < 0) {
        CPed* d = nv->veh->Driver();
        if (d && d != ped && d->IsPlayer()) return;  // our player: two drivers, Vehicles' ownership sorts it out
        // an NPC that its game pulled out (the jack went through there), while the copy's own jack here never got that
        // far: it sat at the wheel of a car the other player drives, the copy stood outside for good
        if (d && d != ped) {
            CVector p = d->GetPosition();
            Cmd(0x0362).I(GetPedRef(d)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            LOGI("Player %d drives vehicle %04X in its game: the driver here gets out", rp.id, nv->netId);
        }
        Cmd(0x036A).I(rp.pedHandle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        if (nv->veh->Driver() == ped) nv->veh->SetStatus(3);  // STATUS_PHYSICS: its owner's state moves it
    } else {
        if (SeatOrAny(nv->veh, rp.seat) < 0 || nv->veh->Passengers()[rp.seat]) return;  // no such seat, or taken
        Cmd(0x0430).I(rp.pedHandle).I(nv->handle).I(rp.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
    }
    rp.enterTaskMs = NowMs() - kEnterGraceMs + 1500;  // retry no more often than every 1.5 s
}

// Our group (Denise in "Burning Desire", our recruits) gets into a car with us when we take its wheel: the game tells the
// followers so (CEventLeaderEnteredCarAsDriver), and nobody tells them anything when we sit beside the other player at
// the wheel of his car (27.10). They take its free seats then, and get out when we do. Only those that just follow us,
// a few seconds after we sat down: a mission that seats its people itself (Sweet's Girl) does it first.
void Players::GroupRides(CPlayerPed* me) {
    uint64_t now = NowMs();
    if (now - m_groupRideMs < 500) return;
    m_groupRideMs = now;
    CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
    bool ride = v && v->Driver() && v->Driver() != me && FindByPed(v->Driver());
    int car = ride ? GetVehicleRef(v) : -1;
    if (m_rideCar >= 0 && car != m_rideCar) {  // we got out (or into another car): ours get out too
        if (CVehicle* was = GetVehicle(m_rideCar))
            for (int h : m_riders)
                if (CPed* p = GetPed(h); p && p->InVehicle() && p->Vehicle() == was) Cmd(0x05CD).I(h).I(m_rideCar).Run();  // TASK_LEAVE_CAR
        m_riders.clear();
    }
    if (car != m_rideCar) m_rideSinceMs = now;
    m_rideCar = car;
    void* pd = ride && now - m_rideSinceMs >= 3000 ? me->PlayerData() : nullptr;
    if (!pd || (Field<uint32_t>(pd, 0x34) & (1u << 10))) return;  // CPlayerPedData::m_bGroupNeverFollow
    uint32_t id = Field<uint32_t>(pd, 0x38);                        // CPlayerPedData::m_nPlayerGroup
    if (id >= 8 || !Ref<uint8_t>(0xC098E0 + id)) return;             // CPedGroups::ms_activeGroups
    uintptr_t group = 0xC09920 + id * 0x2D4;                         // CPedGroups::ms_groups
    if (!Ref<uint8_t>(group + 0x04)) return;                         // CPedGroup::m_bMembersEnterLeadersVehicle
    bool taken[8] = {};
    for (int i = 0; i < 7; ++i) {  // CPedGroupMembership::m_members: its followers (the 8th is its leader)
        CPed* p = Ref<CPed*>(group + 0x0C + (uintptr_t)i * 4);
        if (!p || !IsPedValid(p) || p->IsPlayer() || p->Health() <= 0.0f || p->InVehicle() || EnteringCar(p) ||
            (p->GetPosition() - v->GetPosition()).Length() > 25.0f || !p->Intelligence())
            continue;
        bool follows = false;  // TASK_COMPLEX_FOLLOW_LEADER_IN_FORMATION, _ANY_MEANS, TASK_COMPLEX_GANG_FOLLOWER
        for (int type : {913, 923, 1207})
            follows = follows || CallMethod<0x681740, void*>((uint8_t*)p->Intelligence() + 4, type);  // FindActiveTaskByType
        if (!follows) continue;
        int seat = -1;
        for (int s = 0; s < v->MaxPassengers() && s < 8 && seat < 0; ++s)
            if (!taken[s] && !v->Passengers()[s] && !Peds::Instance().SeatClaimed(v, s)) seat = s;
        if (seat < 0) return;  // no seat left
        taken[seat] = true;
        int h = GetPedRef(p);
        Cmd(0x05CA).I(h).I(car).I(20000).I(seat).Run();  // TASK_ENTER_CAR_AS_PASSENGER
        if (std::find(m_riders.begin(), m_riders.end(), h) == m_riders.end()) m_riders.push_back(h);
        LOGI("Our follower %d gets in with us (seat %d): the other player drives", h, seat);
    }
}

// A passenger changes its gun leaning out of the window (27.10): in a car the game lets nobody change weapons
// (m_bInVehicleDontAllowWeaponChange), and a friend riding with a bat or the wrong rifle got out, changed and got in
// again. The wheel up (or a pad's d-pad up) goes to the previous gun, as on foot, down to the next; round the guns that
// shoot from a window (instant-hit) with rounds. The one chosen stays in hand after getting out.
void Players::PassengerWeaponWheel(CPlayerPed* me) {
    CPad* pad = GetPad(0);
    if (!PassengerAiming() || pad->DisablePlayerControls || pad->bDisablePlayerCycleWeapon) return;
    bool prev = pad->NewState.DPadUp && !pad->OldState.DPadUp, next = pad->NewState.DPadDown && !pad->OldState.DPadDown;
    if (prev == next) return;
    int cur = Field<int8_t>(me, 0x718);  // CPed::m_nActiveWeaponSlot
    for (int i = 1; i < 13; ++i) {
        const CWeapon& w = me->Weapons()[((cur + (next ? i : -i)) % 13 + 13) % 13];
        void* info = w.type ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
        if (!info || Field<int32_t>(info, 0) != 1 || w.ammoTotal <= 0) continue;    // FIRE_INSTANT_HIT, rounds left
        CallMethod<0x5E6280>(me, (int)w.type);  // CPed::SetCurrentWeapon: its model in hand, the old one away
        Field<int32_t>(me, 0x70C) = 55;         // CPed::m_nSavedWeapon: none, the chosen gun stays after getting out
        LOGI("Passenger's gun: %d", (int)w.type);
        return;
    }
}

void Players::TryEnterAsPassenger() {
    if (!core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    // aiming at someone, G is the gang's key (recruiting a Grove member): CJ went for the parked car beside him instead
    if (me->InVehicle() || EnteringCar(me) || GetPad(0)->NewState.RightShoulder1 || me->TargetedObject()) return;
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
            if (!v->Passengers()[s] && !Peds::Instance().SeatClaimed(v, s)) {
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

// A gamepad has no G key: the enter button held half a second takes a passenger seat (a press alone still goes for the
// wheel, as in the game, and that walk to the driver's door gives way).
void Players::HoldForPassengerSeat() {
    CPlayerPed* me = FindPlayerPed(0);
    const CPad* pad = GetPad(0);
    if (!pad->NewState.ButtonTriangle || me->InVehicle() || pad->DisablePlayerControls) {
        m_enterHeldSince = 0, m_enterHeldUsed = false;
        return;
    }
    uint64_t now = NowMs();
    if (!m_enterHeldSince) m_enterHeldSince = now;
    if (m_enterHeldUsed || now - m_enterHeldSince < 500) return;
    m_enterHeldUsed = true;
    if (ActivePrimaryTaskType(me) == TASK_ENTER_CAR_AS_DRIVER) Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    TryEnterAsPassenger();
}

void Players::DetectCarTasks() {
    CPlayerPed* me = FindPlayerPed(0);
    // the enter or leave task anywhere in what it does: a script's sequence holds it under the sequence (the walk to the
    // van and in at the start of "Home Invasion"), and the other game's copy got in without the door's animation
    void* intel = me->Intelligence();
    auto find = [intel](int t) { return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, t) : nullptr; };  // FindActiveTaskByType
    void* task = nullptr;
    int type = -1;
    for (int t : {TASK_ENTER_CAR_AS_DRIVER, TASK_ENTER_CAR_AS_PASSENGER, TASK_ENTER_CAR_AS_PASSENGER_TIMED})
        if (!task && (task = find(t))) type = t;
    if (!task && find(TASK_LEAVE_CAR)) type = TASK_LEAVE_CAR;
    bool passenger = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_PASSENGER_TIMED;
    if ((type == TASK_ENTER_CAR_AS_DRIVER || passenger) && task) {
        auto* target = Field<CVehicle*>(task, 0x0C);  // CTaskComplexEnterCar::m_Car, ...AsPassengerTimed::m_TargetCar
        // The other player drives it: the enter key goes for a passenger seat at once, as G does. The driver's way
        // (held half a second for a seat) had been told to the other game already, and its copy dragged it out (#29).
        if (type == TASK_ENTER_CAR_AS_DRIVER && target && IsVehicleValid(target) && target->Driver() &&
            FindByPed(target->Driver()) && !Net().FriendlyFire()) {
            Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
            for (int s = 0; s < target->MaxPassengers() && s < 8; ++s)
                if (!target->Passengers()[s] && !Peds::Instance().SeatClaimed(target, s)) {
                    Cmd(0x05CA).I(GetPedRef(me)).I(GetVehicleRef(target)).I(10000).I(s).Run();  // TASK_ENTER_CAR_AS_PASSENGER
                    LOGI("The other player drives this car: a passenger seat (%d) instead", s);
                    break;
                }
            return;
        }
        if (target && target != m_enterReported && IsVehicleValid(target)) {
            m_enterReported = target;
            // what decides the SMG in hand once in (CPed::RemoveWeaponWhenEnteringVehicle, 0x5E6370): the saved weapon
            // empty (55), drive-bys allowed, rounds in the SMG slot (#42: sometimes it is not given)
            LOGD("Entering a car: saved weapon %d, drive-by %d, SMG slot %d with %d, in hand %d", Field<int32_t>(me, 0x70C),
                 Field<uint8_t>(PlayerInfo(0), 0x153), me->Weapons()[4].type, me->Weapons()[4].ammoTotal, me->ActiveWeapon().type);
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
    // ours from the first frame at its wheel: past the limit below, the copy's next update put its owner's parked state
    // back on it (the engine off), and the car did not go till got into again (issue #43)
    if (driver) Vehicles::Instance().TakeOwnership(*nv);
    // A car that brakes by itself (Hh hh, 27.10: "let go of the gas and it brakes"): what holds it, for the next report.
    // The game brakes a player's car for its script brakes or its controls taken (0x6ADDCC), pulls its handbrake when its
    // driver gets out or is dragged out, and brakes a car rolling backwards while the gas is held (0x6ADBF6): that one
    // filled the 28.10 logs with false alarms, as a helicopter did (it always brakes, 0x6C4D84). The pad is read through
    // the game's own calls (GInput's IV controls brake on LT); cars only.
    if (CPad* pad = GetPad(0); driver && v->VehicleSubClass() <= VEHICLE_TYPE_QUAD && v->Matrix() &&
                               now - m_selfBrakeLogMs > 10000 && v->MoveSpeed().Length() * 50.0f > 5.0f &&
                               ((v->BrakePedal() > 0.5f && !CallMethod<0x540080, int16_t>(pad) &&  // CPad::GetBrake
                                 !(CallMethod<0x5403F0, int16_t>(pad) &&                          // CPad::GetAccelerate
                                   v->MoveSpeed().Dot(v->Matrix()->forward) < 0.0f)) ||
                                ((v->VehicleFlagsLo() & 0x20u) && !CallMethod<0x540040, int16_t>(pad)))) {  // CPad::GetHandBrake
        m_selfBrakeLogMs = now;
        LOGI("Our car brakes with no brake pressed: brake %.2f, handbrake %d, gas %.2f, controls off %04X, script brakes %d, "
             "status %d, %.0f m/s",
             v->BrakePedal(), (v->VehicleFlagsLo() & 0x20u) != 0, v->GasPedal(), pad->DisablePlayerControls, pad->bApplyBrakes,
             v->Status(), v->MoveSpeed().Length() * 50.0f);
    }
    uint64_t interval = driver ? 33 : 100;
    if (!padChanged && now - m_lastSend < interval) return;
    m_lastSend = now;
    m_lastSentPad = common.pad;
    if (driver) {
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
        if (rp.ped) KeepApart(rp);
        if (rp.ped) KeepWalkSet(rp);
    }
}

// The copy walks with its owner's body (CPlayerPed::GetDefaultPlayerMotionGroup on its stats, applied while it is
// processed), but CClothes::RequestMotionGroupAnims keeps only this game's player's walk set in memory: a fat or a
// muscular partner walked like a plain CJ on the other screen. Its set is asked for too, once a second.
void Players::KeepWalkSet(RemotePlayer& rp) {
    if (rp.stats.size() != kStatsBlockSize || NowMs() - rp.walkSetMs < 1000) return;
    rp.walkSetMs = NowMs();
    constexpr size_t kFloats = 0xB79380 - kStatsBlock;  // CStats::StatTypesFloat
    float fat = *(const float*)&rp.stats[kFloats + 21 * 4], muscle = *(const float*)&rp.stats[kFloats + 23 * 4];  // STAT_FAT, STAT_MUSCLE
    const char* block = fat > 500.0f && fat > muscle ? "fat" : muscle > 500.0f ? "muscular" : nullptr;
    if (int i = block ? Call<0x4D3990, int>(block) : -1; i >= 0) RequestModel(anim::kIfpBase + i, 2);  // CAnimManager::GetAnimationBlockIndex
}

// A script that puts this game's player on a spot (a changing room, a mission's) puts it inside the other player's
// ped when that one stands there: the two peds got stuck in each other and the player's next scripted task never
// started (the shop waits for it with the controls taken). Two players inside each other ignore each other's collision
// until they part (CPhysical::m_pEntityIgnoredCollision, the game clears it once they stop touching).
void Players::KeepApart(RemotePlayer& rp) {
    CPed* other = rp.ped;
    CPlayerPed* me = FindPlayerPed(0);
    CVector d = me ? other->GetPosition() - me->GetPosition() : CVector{};
    bool inside = me && !me->InVehicle() && !other->InVehicle() && d.x * d.x + d.y * d.y < 0.4f * 0.4f && std::fabs(d.z) < 1.5f;
    // Both on one scripted spot (the range's booth, a barber's chair, the bench press, a changing room, a casino seat):
    // each game puts its own player there, and the two bodies stood in each other. The other one is hidden after a
    // second of it, until they part (a cutscene shows the partner its own way, ScenePartner).
    rp.overlapMs = inside ? (rp.overlapMs ? rp.overlapMs : NowMs()) : 0;
    bool hide = ((inside && NowMs() - rp.overlapMs > 1000) || rp.goneInDoor) && !Ref<uint8_t>(0xB5F851);  // CCutsceneMgr::ms_running
    if (hide != rp.hiddenOnSpot) {
        rp.hiddenOnSpot = hide;
        if (hide) other->Flags() &= ~0x80u;  // CEntity::m_bIsVisible
        else other->Flags() |= 0x80u;
        LOGI("Remote player %d %s", rp.id, !hide ? "shown again" : rp.goneInDoor ? "gone in through the door" : "hidden on a shared spot");
    }
    // In a shop or a minigame (CTheScripts::bMiniGameInProgress) the script walks our player onto its spot: the other one
    // standing near it (on the marker at the same time) kept ours from it, and the shop waited, both stuck, until the
    // first one left its menu. They pass through each other there.
    float apart = Ref<uint8_t>(0xA444A8) ? 1.5f : 0.4f;
    bool touching = me && !me->InVehicle() && !other->InVehicle() && d.x * d.x + d.y * d.y < apart * apart && std::fabs(d.z) < 1.5f;
    if (!touching) return;
    Field<CEntity*>(me, 0x128) = other;
    Field<CEntity*>(other, 0x128) = me;
}

// A call's phone goes away only as phone_out passes 1.57 s of its 2 (CTaskComplexUseMobilePhone): cut before that (a
// fight, a button), the phone stayed in the hand with no call, the gun not given back, in this game and on the copy
// (Dr.BaldBall, 28.10). Put away as the task does; only that task puts model 330 in a hand.
void Players::PutAwayStuckPhone(CPlayerPed* me) {
    if (Field<int32_t>(me, 0x740) != kCellphone || Field<int32_t>(me, 0x70C) == 55 || !me->Intelligence() ||  // m_nSavedWeapon
        CallMethod<0x681740, void*>((uint8_t*)me->Intelligence() + 4, 1600))  // FindActiveTaskByType: the phone task
        return;
    CallStd<0x6348F0>((CPed*)me);  // CPed__removeCellphone: the model off, the saved gun back
    LOGI("The phone stayed in hand after a call: put away");
}

// a person of this game on its way into that seat of v (a mission's TASK_ENTER_CAR_AS_PASSENGER with its seat)
bool OwnPersonWants(CVehicle* v, int seat, CPed* me) {
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p == me || p->IsPlayer() || !p->Intelligence()) continue;
        void* t = CallMethod<0x681740, void*>((uint8_t*)p->Intelligence() + 4, TASK_ENTER_CAR_AS_PASSENGER);  // FindActiveTaskByType
        if (!t || Field<CVehicle*>(t, 0x0C) != v) continue;  // CTaskComplexEnterCar::m_Car
        int door = Field<int32_t>(t, 0x14) ? Field<int32_t>(t, 0x14) : Field<int32_t>(t, 0x1C);  // m_TargetDoor, m_TargetSeat
        if (door && SeatOfTargetDoor(door) == seat) return true;
    }
    return false;
}

// A mission seats its people in the car of its CJ, and a player riding there beside the other one sat in one of those
// seats: "Sweet's Girl" puts the girl in seat 0 and Sweet in seat 1, "Drive-Thru" its three in 0, 1 and 2. The person
// could not get in, the mission waited (28.10: "нпс не садились, а выходили"). We move to a seat nobody claims, a person
// of the other game's mission (its copy) or of our own, or get out.
void Players::YieldClaimedSeat(CPlayerPed* me) {
    uint64_t now = NowMs();
    if (now - m_yieldMs < 500) return;
    m_yieldMs = now;
    CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
    int mine = v && v->Driver() && FindByPed(v->Driver()) ? PassengerSeatOf(v, me) : -2;
    auto claimed = [&](int s) { return Peds::Instance().SeatClaimed(v, s) || OwnPersonWants(v, s, me); };
    if (mine < 0 || !claimed(mine)) return;
    int seat = -1;
    for (int s = std::min<int>(v->MaxPassengers(), 8) - 1; s >= 0 && seat < 0; --s)
        if (!v->Passengers()[s] && !claimed(s)) seat = s;
    CVector p = me->GetPosition();
    Cmd(0x0362).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    if (seat >= 0) Cmd(0x0430).I(GetPedRef(me)).I(GetVehicleRef(v)).I(seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
    LOGI("Our seat %d is the seat of the other game's person: %s", mine, seat >= 0 ? "moved over" : "out of the car");
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
               (Field<int32_t>(me, 0x740) == kCellphone ? OnFootState::F_PHONE : 0) |  // CPed::m_nWeaponModelId
               (TaskWalks(me) ? OnFootState::F_TASK_WALK : 0);
    st.flags2 = Ref<int32_t>(0x96A7CC) != 0 ? OnFootState::F2_DOOR : 0;  // CEntryExitManager::ms_exitEnterState
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
    const auto* look = (const uint8_t*)(cam::Active() + 0x08);  // CCam::m_bLookingLeft, m_bLookingRight
    c.lookLeft = look[0] != 0, c.lookRight = look[1] != 0;
    c.driveByOff = !Field<uint8_t>(PlayerInfo(0), 0x153);  // CPlayerInfo::m_bCanDoDriveBy
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
    if (NowMs() - m_markSentMs > 15000) {  // again now and then: a player who came later or loaded a world has it too
        m_markSentMs = NowMs();
        Writer w(proto::G_MARK_COLOUR);
        w.Str(Cfg().markColour, 8);
        Net().Send(proto::CH_RELIABLE, w);
    }
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    // CBike::VehicleDamage calls CBike::DamageKnockOffRider (0x6B5A10) before its collision-proof test (0x6B8FAC): the
    // copy of the other player's bike, pushed by his states into kerbs and cars, threw our player off while he rode on.
    // A rider that is never knocked off (CPed+0x474 bits 27-28, SET_CHAR_CAN_BE_KNOCKED_OFF_BIKE) stops it for both:
    // his game decides his crash (ApplyState). What a script had set on our player comes back once we are off.
    CVehicle* bike = me->InVehicle() ? me->Vehicle() : nullptr;
    NetVehicle* bnv = bike && bike->IsBike() && bike->Driver() != me ? Vehicles::Instance().Find(bike) : nullptr;
    bool keepOn = bnv && Vehicles::Instance().IsMirror(*bnv);
    uint32_t& knock = me->PedFlags3();
    if (keepOn && m_knockOffSaved < 0) {
        m_knockOffSaved = (int8_t)((knock >> 27) & 3);
        knock = (knock & ~(3u << 27)) | (1u << 27);
    } else if (!keepOn && m_knockOffSaved >= 0) {
        knock = (knock & ~(3u << 27)) | ((uint32_t)m_knockOffSaved << 27);
        m_knockOffSaved = -1;
    }
    // our player's fall (CTaskComplexDie's animation, +0x10 group, +0x14 id) for its copies: the default group's only
    if (me->Health() > 0.0f) {
        m_deathSent = false;
    } else if (!m_deathSent && me->Intelligence()) {
        void* die = CallMethod<0x681740, void*>((uint8_t*)me->Intelligence() + 4, 217);  // FindActiveTaskByType: COMPLEX_DIE
        if (die && Field<int32_t>(die, 0x10) == 0) {
            Writer w(proto::G_PLAYER_DEATH);
            w.I16((int16_t)Field<int32_t>(die, 0x14));
            Net().Send(proto::CH_RELIABLE, w);
            LOGI("Died with animation %d", Field<int32_t>(die, 0x14));
        }
        m_deathSent = die != nullptr;
    }
    DetectCarTasks();
    HoldForPassengerSeat();
    FollowHost();
    GroupRides(me);
    YieldClaimedSeat(me);
    PutAwayStuckPhone(me);
    PassengerWeaponWheel(me);
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
        NowMs() - m_followAt < 1000 || SceneWatched())  // CCutsceneMgr::ms_running: the scene holds the partner meanwhile
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
    // No seat left for us in the host's vehicle: a bike, a forklift, Ryder's Picador with Ryder in it, Sweet's Greenwood
    // full of the gang, Smoke's bike with the host behind him. The partner gets one of its own to follow. A seat that a
    // copy of the host's people walks to is taken too: ours took Ryder's.
    CVehicle* hv = hp->InVehicle() ? hp->Vehicle() : nullptr;
    int model = hv ? hv->ModelIndex() : 0, type = hv ? hv->VehicleSubClass() : -1;
    bool land = type == VEHICLE_TYPE_AUTOMOBILE || type == VEHICLE_TYPE_MTRUCK || type == VEHICLE_TYPE_QUAD || (hv && hv->IsBike());
    bool full = hv && hv->Driver();
    for (int i = 0; hv && i < hv->MaxPassengers() && i < 8; ++i) full = full && (hv->Passengers()[i] || Peds::Instance().SeatClaimed(hv, i));
    if (!hv || GetVehicleRef(hv) == m_bikeFor || !land || !full) return;
    // within reach of the host: a partner the mission left farther comes by its moves (CS_FOLLOW) or /tp first
    if ((!flow::IsFollower() && !HostMissionRecent(20000)) || (hpos - mpos).Length() > 150.0f || me->AreaCode() != 0) return;
    m_followAt = NowMs();  // a try every second until the model is in and there is room
    int car = VehicleBeside(model);
    static int noRoomFor = -1;
    if (car == -2 && noRoomFor != GetVehicleRef(hv))
        noRoomFor = GetVehicleRef(hv), LOGI("No room next to us for a vehicle like the host's (model %d) yet", model);
    if (car < 0) return;
    m_bikeFor = GetVehicleRef(hv);  // once it stands: a failed try (no room, the model loading) tries again
    Cmd(0x01C3).I(car).Run();       // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary vehicle from now on
    ui::TheChat().AddSystem(type == VEHICLE_TYPE_BMX ? TR("Рядом с вами велосипед. Садитесь и езжайте за хостом.",
                                                          "There is a bicycle next to you. Get on and follow the host.")
                            : hv->IsBike()           ? TR("Рядом с вами мотоцикл. Садитесь и езжайте за хостом.",
                                                          "There is a motorbike next to you. Get on and follow the host.")
                                                     : TR("Рядом с вами машина. Садитесь и езжайте за хостом.",
                                                          "There is a vehicle next to you. Get in and follow the host."));
    LOGI("The host is in a vehicle with no seat for us (model %d): one for the partner too", model);
}

// the point over rp's head the name tag hangs from (back buffer coordinates) and its fade with distance
bool Players::TagAnchor(RemotePlayer& rp, float& sx, float& sy, float& alpha) {
    CPlayerPed* me = FindPlayerPed(0);
    // and none in a scene with the bars up (a cutscene's or a mission's on the engine), as the game shows no HUD there
    if (!rp.ped || !me || rp.ped->AreaCode() != me->AreaCode() || rp.hiddenOnSpot || cam::WideScreenOn()) return false;
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
    if (NowMs() - rp.heardMs < 300) name = ui::TalkingMark() + name;  // talking right now
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
    if (!Net().Online() || !Cfg().nameTags) return;
    for (auto& [id, rp] : m_remote) DrawNameTag(rp);
}

void Players::DrawRadarArrows() { DrawArrows(); }
void Players::SetRadarArrowsHooked(bool hooked) { g_arrowHook = hooked; }

void Players::SetMarkColour(const std::string& colour) {
    Cfg().markColour = colour;
    Ini ini;
    ini.Load(Cfg().iniPath);
    ini.Set("game", "mark_colour", colour);
    ini.Save(Cfg().iniPath);
    m_markSentMs = 0;  // goes out on the next frame
}

bool Players::OnCommand(const std::string& cmd, const std::vector<std::string>& args) {
    // our own mark on the other player's radar in a colour of our choice (the allies' blue was taken for a gang member's)
    if (cmd == "blip") {
        std::string colour = args.empty() ? "" : ToLower(args[0]);
        if (colour == "default" || colour == "off") colour.clear();
        if (!colour.empty() && BlipColour(colour) < 0) {
            ui::TheChat().AddError(TR("Цвет метки: white, red, green, blue, yellow, RRGGBB или default.",
                                      "The mark's colour: white, red, green, blue, yellow, RRGGBB or default."));
            return true;
        }
        SetMarkColour(colour);
        ui::TheChat().AddSystem(colour.empty() ? TR("Ваша метка у напарника снова обычного цвета.", "Your mark has its usual colour for your partner again.")
                                               : Format(TR("Ваша метка у напарника теперь %s.", "Your mark is %s for your partner now."), colour.c_str()));
        return true;
    }
    return false;  // /tp and /tphere: requests (Teleport.cpp), carried out here once the other player says yes
}

bool Players::TeleportAllowed(const RemotePlayer& other, bool bring) {
    CPlayerPed* me = FindPlayerPed(0);
    // into a building only through its door: one teleported in skipped the game's entry (the interior's people, its
    // light, the door), and the two games saw that interior differently. The host's story mission is the exception: it
    // takes the host to places with no door at all (a plane's hold in "Stowaway"), and the partner belongs there.
    if (!bring && other.state.area != 0) {
        if (other.id != Net().HostId() || !HostMissionRecent(0)) {
            ui::TheChat().AddError(TR("Нельзя переместиться к напарнику, пока он в здании.",
                                      "You cannot go over to your partner while they are inside a building."));
            return false;
        }
        if (me->InVehicle()) {
            ui::TheChat().AddError(TR("Выйдите из машины, чтобы переместиться к напарнику в здание.",
                                      "Get out of the vehicle to go over to your partner inside a building."));
            return false;
        }
    }
    if (bring && me->AreaCode() != 0) {
        ui::TheChat().AddError(TR("Нельзя позвать напарника к себе, пока вы в здании.",
                                  "You cannot bring your partner over while you are inside a building."));
        return false;
    }
    return true;
}

void Players::GoToPlayer(const RemotePlayer& other) {
    GoTo(other, other.state.pos + CVector{1.0f, 0, 0});
    ui::TheChat().AddSystem(Format(TR("Вы переместились к %s.", "You went over to %s."), Net().PeerName(other.id).c_str()));
}

void Players::Bring(uint8_t id) {
    CPlayerPed* me = FindPlayerPed(0);
    CVector p = me->GetPosition() + CVector{1.0f, 0, 0};
    Writer w(proto::G_TELEPORT);
    w.U8(id);
    w.F32(p.x);
    w.F32(p.y);
    w.F32(p.z - 1.0f);
    w.I8(me->AreaCode());
    Net().Send(proto::CH_RELIABLE, w);
    ui::TheChat().AddSystem(Format(TR("Игрок %s перемещён к вам.", "%s was brought over to you."), Net().PeerName(id).c_str()));
}

void UseStreetDoor(int area, const CVector& inside, const CVector& from) {
    if (area == 0) return;
    CPlayerPed* me = FindPlayerPed(0);
    void* door = StreetDoorOf(area, inside, from);
    if (!me || !door) return;
    CVector at = DoorSpot(door);
    Cmd(0x08AD).I(GetPedRef(me)).F(at.x).F(at.y).F(3.0f).Run();  // SET_CHAR_HAS_USED_ENTRY_EXIT
    // it is in there all the same, whatever area the command took from the door
    int set = me->AreaCode(), visible = Ref<int32_t>(0xB72914);  // CGame::currArea
    if (set != area) me->AreaCode() = (int8_t)area;
    if (visible != area) Cmd(0x04BB).I(area).Run();  // SET_AREA_VISIBLE
    LOGI("Into area %d as through the door at %.1f,%.1f (%.8s; the door set area %d, visible %d)", area, at.x, at.y,
         (const char*)door, set, visible);
}

void Players::GoTo(const RemotePlayer& by, const CVector& spot, bool onFoot) {
    CPlayerPed* me = FindPlayerPed(0);
    int handle = GetPedRef(me);
    CVector was = me->GetPosition();
    if (onFoot && me->InVehicle()) Cmd(0x0362).I(handle).F(spot.x).F(spot.y).F(spot.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    Cmd(0x04BB).I(by.state.area).Run();                                // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(by.state.area).Run();                      // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(spot.x).F(spot.y).F(spot.z - 1.0f).Run();  // SET_CHAR_COORDINATES
    LightArea(by.state.area, spot);
    UseStreetDoor(by.state.area, spot, by.streetPos.x != 0.0f ? by.streetPos : was);
}

// A spot with room and ground: right of us, left, behind, ahead, then the four between. At the right hand alone it came
// inside a wall or another car.
int Players::VehicleBeside(int model) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!IsModelLoaded(model)) return RequestModel(model), -1;  // in the background: a blocking load stalled the game
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")) return -1;
    CVector mpos = me->GetPosition();
    float h = me->CurrentRotation();
    CVector p;
    bool found = false;
    for (float turn : {-1.5708f, 1.5708f, 3.1416f, 0.0f, -0.7854f, 0.7854f, -2.3562f, 2.3562f}) {
        p = mpos + CVector{std::cos(h + turn) * 3.5f, std::sin(h + turn) * 3.5f, 0.0f};
        bool ground = false;
        float z = Call<0x5696C0, float>(p.x, p.y, mpos.z + 2.0f, &ground, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
        if (!ground || std::fabs(z - (mpos.z - 1.0f)) > 2.0f) continue;
        p.z = z + 0.5f;
        // nothing between us and the spot (a wall, a post: CWorld::GetIsLineOfSightClear), and no car or person on it
        // (IS_AREA_OCCUPIED, 1 m around; the ground is a building, so buildings not). It counts whatever its bounding
        // sphere reaches into the box: ours (1.25 m) reached the 2.5 m one from 3.5 m away at every spot, and no bike came.
        CVector chest = mpos, at = p + CVector{0, 0, 0.7f};
        if (!Call<0x56A490, bool>(&chest, &at, true, true, false, true, false, true, false) ||
            Cmd(0x0339).F(p.x - 1.0f).F(p.y - 1.0f).F(p.z - 0.3f).F(p.x + 1.0f).F(p.y + 1.0f).F(p.z + 2.0f).I(0).I(1).I(1).I(0).I(0).Run())
            continue;
        found = true;
        break;
    }
    if (!found) return -2;
    int car = -1;
    Cmd(0x00A5).I(model).F(p.x).F(p.y).F(p.z).Out(&car).Run();  // CREATE_CAR
    if (car < 0) return -1;
    Cmd(0x0175).I(car).F(h * 57.29578f).Run();  // SET_CAR_HEADING
    if (int area = me->AreaCode()) Cmd(0x0840).I(car).I(area).Run();  // SET_VEHICLE_AREA_VISIBLE: a stadium's arena
    MarkModelNotNeeded(model);                  // the car holds it now (the request kept it in memory for good)
    return car;
}

void Players::SetAvatar(uint8_t id, int pedHandle) {
    RemotePlayer* rp = Get(id);
    if (!rp || rp->avatar == pedHandle) return;
    if (rp->avatar >= 0 && rp->ped && rp->ped->RwObject()) {
        void* clump = rp->ped->RwObject();
        for (void* a = anim::First(clump); a; a = anim::Next(a))
            if (anim::Hierarchy(a) == rp->avatarAnim && anim::BlendDelta(a) >= 0.0f) anim::FadeOut(a);
        // its stance under it: a ped left with no animation crashes the game (see Peds.cpp)
        Call<0x4D4610, void*>(clump, Field<int32_t>(rp->ped, 0x4D4), 3 /* ANIM_ID_IDLE */, 4.0f);  // CAnimManager::BlendAnimation
    }
    // FREEZE_CHAR_POSITION: placed where the person stands (with no collision, that one stands in the table's edge)
    if (rp->ped) Cmd(0x04D7).I(rp->pedHandle).I(pedHandle >= 0 ? 1 : 0).Run();
    rp->avatar = pedHandle, rp->avatarAnim = nullptr;
    LOGI("Remote player %d %s", id, pedHandle >= 0 ? "takes a script person's place" : "is itself again");
}

Players::Ride Players::RideWith(const RemotePlayer& leader, bool asSeat) {
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* lv = leader.ped && leader.ped->InVehicle() ? leader.ped->Vehicle() : nullptr;
    if (!lv) return leader.mode == RemotePlayer::Mode::OnFoot ? Ride::None : Ride::Later;  // its copy not seated here yet
    if (me->InVehicle() && me->Vehicle() == lv) return Ride::Seat;
    int handle = GetPedRef(me);
    auto out = [&] {
        CVector p = me->GetPosition();
        if (me->InVehicle()) Cmd(0x0362).I(handle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    };
    // a seat its game's people do not walk to (Peds' claims) and nobody holds
    for (int i = 0; asSeat && i < lv->MaxPassengers() && i < 8; ++i)
        if (!lv->Passengers()[i] && !Peds::Instance().SeatClaimed(lv, i)) {
            out();
            Cmd(0x0430).I(handle).I(GetVehicleRef(lv)).I(i).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
            LOGI("Seated in player %d's vehicle (model %d, seat %d)", leader.id, lv->ModelIndex(), i);
            return Ride::Seat;
        }
    int type = lv->VehicleSubClass();
    if (type != VEHICLE_TYPE_AUTOMOBILE && type != VEHICLE_TYPE_MTRUCK && type != VEHICLE_TYPE_QUAD && !lv->IsBike()) return Ride::None;
    CVehicle* mine = me->InVehicle() ? me->Vehicle() : nullptr;
    if (mine && mine->Driver() == me && mine->ModelIndex() == lv->ModelIndex()) return Ride::OwnVehicle;  // one like it already
    int car = VehicleBeside(lv->ModelIndex());
    if (car < 0) return Ride::Later;
    out();
    Cmd(0x036A).I(handle).I(car).Run();  // WARP_CHAR_INTO_CAR
    Cmd(0x01C3).I(car).Run();            // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary vehicle from now on
    LOGI("A vehicle like player %d's for us (model %d)", leader.id, lv->ModelIndex());
    return Ride::OwnVehicle;
}

}  // namespace coop
