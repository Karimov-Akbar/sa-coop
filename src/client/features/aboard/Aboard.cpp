// A ride on a plane, a helicopter or a boat with no seat for this player (the players' idea, 09.11): the Shamal and the
// other planes without passenger seats, and the boats, where a second player standing on the deck hurt the boat and fell
// into the water every 50 m. The passenger key by such a vehicle that another player drives puts our CJ on it as a
// mission puts its gunner (ATTACH_CHAR_TO_CAR: no collision with it, the game's camera of an attached player): on a boat
// on a seat of its model (kBoats) with his gun in hand, in an aircraft the first in sight in the seat beside the pilot's
// where the model has one, the others hidden behind the front seats. The other games put his copy there the same way
// (G_PED_ATTACH, Missions). The enter key takes him off: beside an aircraft, with a parachute up in the air, or where he
// sits on the boat. His view: from behind the vehicle, or the cabin's or his gun's (the camera change key, 13.11).
#include <algorithm>
#include <cmath>
#include <utility>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

struct Ride {
    bool on = false;
    uint16_t car = 0;     // the vehicle's network id
    bool air = false;     // an aircraft
    bool hidden = false;  // inside it, out of sight
    CVector at;           // where on it now, and the gun in hand: said again to a player who comes in later
    int weapon = 0;
    uint8_t pose = 0;     // his seat's (HoldSeat): 0 none, 1 a car's seat, 2 a bench
    uint8_t side = 0;     // the way it faces (the turret's direction: 0 ahead, 1 left, 2 back, 3 right)
    bool seated = false;
    bool unseen = false;  // SET_CHAR_VISIBLE 0 on our CJ: hidden, or in the cabin view his own head is in
    CVector sit, rise;    // there seated (or hidden), and where he stands up to shoot
    int freeAimWas = -1;  // CPlayerPedData::m_bFreeAiming before the boat's aim over the shoulder (AimOverShoulder)
} g_ride;

bool Aircraft(CVehicle* v) { return v->VehicleSubClass() == VEHICLE_TYPE_HELI || v->VehicleSubClass() == VEHICLE_TYPE_PLANE; }
bool Boat(CVehicle* v) { return v->VehicleSubClass() == VEHICLE_TYPE_BOAT; }
bool CutsceneOn() { return Ref<uint8_t>(0xB5F851) != 0; }  // CCutsceneMgr::ms_running

// The view from his place in an aircraft (13.11, the user's choice "Из кабины"): the game's camera of an attached player
// sat on him inside and turned with the aircraft ("был пилота осмотр"). The camera is attached to the aircraft instead
// (ATTACH_CAMERA_TO_VEHICLE: MODE_ATTACHCAM puts it and the point it looks at in the vehicle's axes, the world's up, so a
// bank does not tilt the view) at the eyes over his place, and the mouse or the right stick turn the look about the
// aircraft as the game's first-person aims turn (CCam::Process_Rocket, 0x511B50).
constexpr uint16_t kModeAttachCam = 47;
constexpr uintptr_t kCamLookAt = 0xB6F96C, kCamAttached = 0xB6F984;  // CCamera::m_vecAttachedCamLookAt, m_pAttachedEntity
struct Cabin {
    bool on = false;
    float yaw = 0, pitch = 0;  // about the aircraft, 0 ahead; yaw to the right, pitch up
    CVector eyes;              // in its axes
} g_cabin;

void CabinOn(int vehicle, const CVector& seat) {
    g_cabin = {true, 0.0f, 0.0f, CVector{seat.x, seat.y + 0.08f, seat.z + 0.62f}};  // as Process_1stPerson takes a front seat's
    const CVector& e = g_cabin.eyes;
    Cmd(0x0679).I(vehicle).F(e.x).F(e.y).F(e.z).F(e.x).F(e.y + 5.0f).F(e.z).F(0.0f).I(2).Run();  // ATTACH_CAMERA_TO_VEHICLE, a cut
}

// The view from behind (13.11, the user's words "хочу вид от 3 лица ... переключение вида", the default): the vehicle's
// follow camera, as its driver's (POINT_CAMERA_AT_CAR with MODE_CAM_ON_A_STRING, a cut: Zero's RC planes in ZER2 have it
// on a vehicle CJ is not in; CCam::Process_FollowCar_SA looks at its target's driver and passengers, never at the player,
// and turns with the mouse or the right stick). The camera change key (V, the pad's Select) goes to the cabin in an
// aircraft, to the gun's own view on a boat, and back. Tied on, the player has the game's camera of an attached gunner
// (MODE_HELICANNON_1STPERSON, CPed::AttachPedToEntity): seen again, whatever held the camera meanwhile (a scene of the
// host's mission) is done with it, and the chosen view comes back. On a boat he shoots from the gun's view (the user's
// "встаёт стрелять"): from behind the fire button is held off, as SET_PLAYER_FIRE_BUTTON holds it, else his gun fired
// from the lap wherever he faced; in an aircraft always (no gun there). What it was before comes back (a script may hold
// it too).
constexpr uint16_t kModeCamOnAString = 18, kModeHeliCannon = 45, kModeAimAttached = 65;
constexpr uintptr_t kCamTarget = addr::TheCamera + 0x958;  // CCamera::m_pTargetEntity (CCamera::TakeControl, 0x50C85F)
enum class View : uint8_t { Behind, Own };
View g_view = View::Behind;  // the player's choice, kept from ride to ride
bool g_behind = false;       // our follow camera on the vehicle is asked for
uint64_t g_behindMs = 0;
int g_fireWas = -1;          // CPad::bDisablePlayerFireWeapon before we held it

void HoldFire(bool hold) {
    uint8_t& lock = GetPad(0)->bDisablePlayerFireWeapon;
    if (hold && g_fireWas < 0) g_fireWas = lock, lock = 1;
    else if (!hold && g_fireWas >= 0) lock = (uint8_t)g_fireWas, g_fireWas = -1;
}

void ViewOff(CVehicle* v) {
    bool cabin = std::exchange(g_cabin.on, false), behind = std::exchange(g_behind, false);
    uint16_t mode = cam::Mode();
    CEntity* target = Ref<CEntity*>(kCamTarget);  // a reference: null once the vehicle is gone
    // RESTORE_CAMERA_JUMPCUT, ours only: a cutscene holds the camera and gives it back itself, and a camera another took
    // since (a scene's on another car) is that one's. No vehicle: the session's end, its copy about to go
    if (!CutsceneOn() && ((cabin && mode == kModeAttachCam) ||
                          (behind && mode == kModeCamOnAString && (!target || !v || target == (CEntity*)v))))
        Cmd(0x02EB).Run();
}

void CabinLook(CPlayerPed* me, CVehicle* v) {
    // another took the camera (a cutscene, a scene of the host's mission): it stays that one's
    if (!g_cabin.on || cam::Mode() != kModeAttachCam || Ref<CVehicle*>(kCamAttached) != v) return;
    const float fov = 70.0f / 80.0f;  // MODE_ATTACHCAM's own field of view (Process_AttachedCam)
    float mx = Ref<float>(0xB73418 + 0x0C), my = Ref<float>(0xB73418 + 0x10);  // CPad::NewMouseControllerState: this frame's move
    if (mx != 0.0f || my != 0.0f) {
        g_cabin.yaw += 3.0f * mx * fov * Ref<float>(0xB6EC1C);    // CCamera::m_fMouseAccelHorzntl
        g_cabin.pitch += 4.0f * my * fov * Ref<float>(0xB6EC18);  // m_fMouseAccelVertical
    } else {
        CPad* pad = GetPad(0);
        float lr = CallMethod<0x540BD0, int16_t>(pad, (CPed*)me), ud = CallMethod<0x540CC0, int16_t>(pad, (CPed*)me);  // LookAround*
        g_cabin.yaw += (lr < 0.0f ? -1.0f : 1.0f) * lr * lr / 10000.0f * fov / 17.5f * TimeStep();
        g_cabin.pitch += (ud < 0.0f ? -1.0f : 1.0f) * ud * ud / 22500.0f * fov / 14.0f * TimeStep();
    }
    g_cabin.yaw = std::remainder(g_cabin.yaw, 6.2831853f);
    g_cabin.pitch = std::clamp(g_cabin.pitch, -1.3f, 1.3f);
    float c = std::cos(g_cabin.pitch);
    Ref<CVector>(kCamLookAt) = g_cabin.eyes + CVector{std::sin(g_cabin.yaw) * c, std::cos(g_cabin.yaw) * c, std::sin(g_cabin.pitch)} * 5.0f;
}

bool FreeSeat(CVehicle* v) {
    for (int s = 0; s < v->MaxPassengers() && s < 8; ++s)
        if (!v->Passengers()[s]) return true;
    return false;
}

// the driver's seat in its model (CVehicleModelInfo::m_pVehicleStruct, the dummies: a boat's is the first one, as
// CVehicleModelInfo::GetFrontSeatPosn takes it). Only a boat's driver sits right at it: the front seat's dummy is the
// passenger's, and CPed::SetPedPositionInCar puts the driver of anything but a boat or a bike at its mirror (fchs at
// 0x5DF9BF). Taken as it was, the place beside the pilot was the pilot's own, and the cabin view sat inside him (13.11,
// the partner's screenshot in the Shamal: the pilot's shoulders and arms from within)
CVector DriverSeat(CVehicle* v) {
    void* info = ModelInfoOf(v->ModelIndex());
    auto* parts = info ? Field<const uint8_t*>(info, 0x5C) : nullptr;
    if (!parts) return {};
    CVector seat = *(const CVector*)(parts + (Boat(v) ? 0 : 4) * 12);
    if (!Boat(v)) seat.x = -seat.x;
    return seat;
}

// beside the driver's seat, behind it when that one is in the middle (a fighter's)
CVector BesideDriver(CVehicle* v) {
    CVector seat = DriverSeat(v);
    bool middle = std::fabs(seat.x) < 0.3f;
    return {middle ? seat.x : -seat.x, middle ? seat.y - 0.9f : seat.y, seat.z};
}

// A boat of another model (a mod's) has no row of kBoats: our CJ stands beside its driver, the second and third partner a
// little before and behind, on the deck behind that: where a line down from above hits the boat, the ped's root a metre
// over its feet.
CVector SpotOn(CVehicle* v) {
    CVector seat = DriverSeat(v);
    CVector o = BesideDriver(v);
    // the host aboard a partner's vehicle takes that partner's place, the driver's seat being its own
    const RemotePlayer* driver = v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    if (int rank = PlaceRank(Net().MyId(), driver ? driver->id : 0xFF); rank > 0) o.y += rank == 1 ? -0.8f : 0.8f;
    CMatrix* m = v->Matrix();
    void* col = m ? CallMethod<0x535300, void*>(v) : nullptr;  // CEntity::GetColModel: its box, min and max
    if (!col) return o;
    o.y -= 0.6f;
    o.z = seat.z + 0.8f;  // no deck found: about standing at the seat
    CVector at = m->pos + m->right * o.x + m->forward * o.y;
    CVector from = at + m->up * (Field<float>(col, 0x14) + 1.0f), to = at + m->up * Field<float>(col, 0x08);
    uint8_t cp[0x2C] = {};  // CColPoint: m_vecPoint first
    CEntity* hit = nullptr;
    // CWorld::ProcessLineOfSight: the vehicles alone
    if (Call<0x56BA00, bool>(&from, &to, (void*)cp, &hit, false, true, false, false, false, false, false, false) && hit == (CEntity*)v)
        o.z = (*(const CVector*)cp - m->pos).Dot(m->up) + 1.0f;
    return o;
}

// The boats' seats, read off their models in the game's own gta3.img (13.11; the user's words "изучи модели ... для
// правильного положения игрока сидя или стоя", "учитывай еще места для 3 - 4 игроков", the choice "Сидит, встаёт стрелять",
// and after build 15, where the second and third stood on the Jetmax's rear seat's back: "На лодках есть сидения сзади.
// Поэтому изучи нормально и посади игроков. Не должны они стоять"). E:\GTA COOP Tests\scripts\analysis\aboard: deckmap.py
// and boardseats.py read the seats, boatseats.py puts the game's skeleton on each in its pose (fam1.dff under the first key
// of CAR_sitp or SEAT_idle) and makes these rows. By rank (PlaceRank, up to three aboard): beside a driver who sits (the
// front seat's dummy mirrored: a boat's driver sits right at it), on the rear seat, the benches along the sides and the
// transom, the tubes of the Dinghy and the Coastguard. The root is the point tied on: the pose takes the pelvis off it
// (SEAT_idle 0.53 m down and back), its feet on the floor or a few centimetres over it. The Tropic's driver sits in its
// closed wheelhouse up top: its players sit on the bench of its open deck aft. In the gun's view he stands up at `rise`,
// a metre over the floor before his seat.
struct BoatSeat {
    CVector at;    // the root seated
    uint8_t side;  // the turret's direction the seat faces
    uint8_t pose;  // 1 CAR_sitp, 2 SEAT_idle
    CVector rise;
};
struct BoatSeats {
    int model;
    BoatSeat seat[3];
};
const BoatSeats kBoats[] = {
    {430, {{{-0.47f, -1.97f, 1.07f}, 3, 2, {-0.45f, -2.00f, 1.00f}}, {{0.44f, -2.03f, 1.07f}, 1, 2, {0.42f, -2.00f, 1.00f}},
           {{-0.08f, -2.82f, 1.07f}, 0, 2, {-0.05f, -2.70f, 1.00f}}}},  // Predator: the benches aft, along the sides and the transom
    {446, {{{-0.69f, 0.27f, 1.08f}, 0, 1, {-0.94f, -0.24f, 1.68f}}, {{-0.45f, -1.03f, 1.02f}, 0, 1, {-0.45f, -0.45f, 1.68f}},
           {{0.45f, -1.03f, 1.02f}, 0, 1, {0.45f, -0.45f, 1.68f}}}},  // Squalo: beside the driver, the rear seat
    {452, {{{-0.50f, 3.00f, 0.83f}, 0, 1, {-0.06f, 2.94f, 1.34f}}, {{-0.53f, 1.03f, 1.34f}, 0, 2, {-0.50f, 1.20f, 1.34f}},
           {{0.47f, 1.03f, 1.34f}, 0, 2, {0.50f, 1.20f, 1.34f}}}},  // Speeder: beside the driver, the rear seats
    {453, {{{-0.79f, -2.52f, 1.07f}, 3, 2, {-0.75f, -2.20f, 1.10f}}, {{0.79f, -2.58f, 1.07f}, 1, 2, {0.75f, -2.20f, 1.10f}},
           {{0.03f, -0.95f, 1.01f}, 2, 2, {0.00f, -1.30f, 1.10f}}}},  // Reefer: the benches aft (crates behind), the wheelhouse's sill
    {454, {{{-1.11f, -2.57f, 1.18f}, 3, 2, {-1.05f, -2.60f, 1.18f}}, {{-1.11f, -3.42f, 1.18f}, 3, 2, {-1.05f, -3.45f, 1.18f}},
           {{-1.11f, -4.27f, 1.18f}, 3, 2, {-1.05f, -4.30f, 1.18f}}}},  // Tropic: the bench of the open deck aft
    {472, {{{-0.51f, -0.97f, 1.02f}, 3, 2, {-0.40f, -1.00f, 0.98f}}, {{0.51f, -1.03f, 1.02f}, 1, 2, {0.40f, -1.00f, 0.98f}},
           {{-0.51f, -1.97f, 1.02f}, 3, 2, {-0.40f, -2.00f, 0.98f}}}},  // Coastguard: its tubes
    {473, {{{-0.43f, -0.07f, 0.94f}, 0, 2, {-0.40f, 0.00f, 0.96f}}, {{0.37f, -0.07f, 0.94f}, 0, 2, {0.40f, 0.00f, 0.96f}},
           {{0.44f, 0.17f, 1.12f}, 1, 2, {0.45f, 0.20f, 0.96f}}}},  // Dinghy: the seat aft, a tube
    {484, {{{-0.83f, -2.97f, 1.81f}, 3, 2, {-0.75f, -3.00f, 1.71f}}, {{0.83f, -3.03f, 1.81f}, 1, 2, {0.75f, -3.00f, 1.71f}},
           {{-0.83f, -3.87f, 1.81f}, 3, 2, {-0.75f, -3.90f, 1.71f}}}},  // Marquis: the cockpit's benches
    {493, {{{0.67f, 0.62f, 0.91f}, 0, 1, {0.47f, 0.09f, 1.47f}}, {{-0.45f, -0.73f, 0.81f}, 0, 1, {-0.45f, -0.20f, 1.47f}},
           {{0.45f, -0.73f, 0.81f}, 0, 1, {0.45f, -0.20f, 1.47f}}}},  // Jetmax: beside the driver, the rear seat
    {595, {{{-0.48f, 0.92f, -0.00f}, 0, 1, {-0.48f, 0.20f, 0.58f}}, {{-0.53f, -2.32f, 0.91f}, 0, 2, {-0.50f, -2.10f, 0.58f}},
           {{0.47f, -2.32f, 0.91f}, 0, 2, {0.50f, -2.10f, 0.58f}}}},  // Launch: beside the driver, the after deck's edge
};

struct Place {
    CVector at;            // seated (pose), standing, or hidden there
    uint8_t pose = 0;
    uint8_t side = 0;
    bool hidden = false;
    CVector rise;          // a boat's seat's place to stand up to shoot
};

// In an aircraft (13.11, the user's words "Рядом с пилотом как то не то для остальных 2 игроков"): the first in sight in
// the seat beside the pilot where the model has two front seats side by side and no player of the game's sits there (the
// Shamal's cockpit has them, its cabin behind is empty in the model), the second and third hidden behind the pilot's seat
// and that one, no longer all three by the pilot with one view; behind him in the middle when that seat is taken. A
// fighter's one seat in the middle: all hidden behind it, a step apart.
Place AircraftPlace(CVehicle* v, int rank) {
    CVector beside = BesideDriver(v);  // the front seat's dummy: the passenger's
    if (std::fabs(beside.x) < 0.3f) return {{beside.x, beside.y - 0.9f * (float)rank, beside.z}, 0, 0, true, {}};
    if (rank == 0 && !v->Passengers()[0]) return {beside, 1, 0, false, beside};
    float x = rank == 1 ? -beside.x : rank == 2 ? beside.x : 0.0f;
    float back = rank == 0 ? 1.8f : 0.9f;
    return {{x, beside.y - back, beside.z}, 0, 0, true, {}};
}

// ours by PlaceRank (the host aboard a partner's vehicle takes that partner's rank): three or four players, up to three aboard
Place PlaceOn(CVehicle* v) {
    const RemotePlayer* driver = v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    int rank = std::clamp(PlaceRank(Net().MyId(), driver ? driver->id : 0xFF), 0, 2);
    if (Aircraft(v)) return AircraftPlace(v, rank);
    for (const BoatSeats& b : kBoats)
        if (b.model == v->ModelIndex()) return {b.seat[rank].at, b.seat[rank].pose, b.seat[rank].side, false, b.seat[rank].rise};
    return {SpotOn(v)};
}

// Seated, or standing up to shoot in the gun's view: CPed::m_vecTurretOffset (0x76C), the turret's direction (0x778) and
// m_fTurretAngleB (0x77C, radians), which CPed::PositionAttachedPed reads each frame, so the attachment stays (a new one
// would give the gun anew and reset the camera); seated he faces the seat's way. The other games hear the place again
// (Missions moves the copy).
void Seat(CPlayerPed* me, int vehicle, bool sit) {
    g_ride.seated = sit;
    g_ride.at = sit ? g_ride.sit : g_ride.rise;
    uint8_t side = sit ? g_ride.side : 0;
    Field<CVector>(me, 0x76C) = g_ride.at;
    Field<uint16_t>(me, 0x778) = side;
    Field<float>(me, 0x77C) = sit ? 0.0f : 6.2831853f;
    ShareAboard(true, vehicle, g_ride.at.x, g_ride.at.y, g_ride.at.z, side, sit ? 0.0f : 360.0f, g_ride.weapon, g_ride.hidden,
                sit ? g_ride.pose : 0);
    LOGI("Aboard: %s", sit ? "seated" : "stands up by the seat");
}

// SET_CHAR_VISIBLE on our CJ, said once a change: hidden in an aircraft, and in the cabin view the one seated beside the
// pilot too, the camera being in his head there (his copy in the other games stays in sight)
void Unseen(CPlayerPed* me, bool unseen) {
    if (unseen == g_ride.unseen) return;
    g_ride.unseen = unseen;
    Cmd(0x0337).I(GetPedRef(me)).I(!unseen).Run();
}

// The gun's view on a boat over his shoulder (14.11, the user's "на v надо 3 лицо а не первое"): the game's camera of one
// tied on with a weapon that aims, as it has the pool cue (MODE_AIMWEAPON_ATTACHED with free aim, CPed::AttachPedToEntity),
// in place of the gunner's first person the attach sets (MODE_HELICANNON_1STPERSON). CCamera::Using1stPersonWeaponMode
// counts both, so he aims and fires as before; the detach clears either (CPlayerPed::ClearWeaponTarget). His free aim
// comes back as it was when he is off.
void AimOverShoulder(CPlayerPed* me, bool on) {
    uint32_t& flags = Field<uint32_t>(me->PlayerData(), 0x34);  // CPlayerPedData flags: bit 3 m_bFreeAiming
    if (on) {
        if (cam::WeaponMode() == kModeHeliCannon) cam::WeaponMode() = kModeAimAttached;
        if (g_ride.freeAimWas < 0) g_ride.freeAimWas = (flags >> 3) & 1;
        flags |= 8u;
    } else if (g_ride.freeAimWas >= 0) {
        flags = (flags & ~8u) | ((uint32_t)g_ride.freeAimWas << 3);
        g_ride.freeAimWas = -1;
    }
}

// Seated he faces his seat's way. CPed::PositionAttachedPed holds an attached ped's heading within the turret's range
// about its direction, but not a player's (0x5DFF1B: ped types 0 and 1 skip it), so ours kept the way the gun's view had
// turned him: aimed left, he sat facing left after it (14.11). The same turn as the game's from the turret's direction.
void FaceSeat(CPlayerPed* me, CVehicle* v) {
    CMatrix* m = v->Matrix();
    if (!m) return;
    static const float kTurn[4] = {0.0f, 1.5707963f, 3.1415927f, -1.5707963f};  // ahead, left, back, right
    float h = std::remainder(std::atan2(-m->forward.x, m->forward.y) + kTurn[g_ride.side & 3], 6.2831853f);
    me->CurrentRotation() = me->AimingRotation() = h;
}

void ApplyView(CPlayerPed* me, CVehicle* v, int vehicle) {
    if (!g_ride.air) AimOverShoulder(me, true);
    uint16_t mode = cam::Mode();
    bool behind = mode == kModeCamOnAString && Ref<CEntity*>(kCamTarget) == (CEntity*)v;
    bool cabin = mode == kModeAttachCam && Ref<CVehicle*>(kCamAttached) == v;
    bool own = mode == kModeHeliCannon || mode == kModeAimAttached;  // the game's camera of one tied on: nobody else's over it now
    if (g_view == View::Behind) {
        if ((own || cabin) && NowMs() - g_behindMs > 1000) {  // asked once a second at most: a cinematic camera refuses it
            g_cabin.on = false, g_behind = true, g_behindMs = NowMs();
            Cmd(0x0158).I(vehicle).I(kModeCamOnAString).I(2).Run();  // POINT_CAMERA_AT_CAR, a cut
        }
    } else if (g_ride.air) {
        if (own || behind) g_behind = false, CabinOn(vehicle, g_ride.sit);
        else CabinLook(me, v);
    } else if (behind) {
        g_behind = false;
        Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT: the gun's own view
    }
    Unseen(me, g_ride.hidden || (g_ride.air && g_cabin.on && cam::Mode() == kModeAttachCam));
    HoldFire(g_ride.air || g_view == View::Behind);
    bool sit = g_ride.pose && (g_ride.air || g_view == View::Behind);  // in an aircraft he stays seated
    if (sit != g_ride.seated) Seat(me, vehicle, sit);
    if (g_ride.seated) FaceSeat(me, v);
    HoldSeat(me, g_ride.seated ? g_ride.pose : 0);
}

// off it standing: a seated one's root is lower than a standing one's and the detach leaves him where he is
void StandForOff(CPlayerPed* me) {
    HoldFire(false);
    AimOverShoulder(me, false);
    HoldSeat(me, 0);
    if (!g_ride.seated || !me->AttachedTo()) return;
    Field<CVector>(me, 0x76C) = g_ride.rise;
    CallMethod<0x5DFDF0>(me);  // CPed::PositionAttachedPed: there now
    g_ride.seated = false;
}

void Off(CPlayerPed* me, CVehicle* v) {
    ViewOff(v);
    StandForOff(me);
    int h = GetPedRef(me);
    bool alive = me->Health() > 0.0f;
    bool found = false;
    CVector at = me->GetPosition();
    float ground = Call<0x5696C0, float>(at.x, at.y, at.z, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
    bool high = g_ride.air && (!found || at.z - ground > 6.0f);  // in an aircraft up in the air
    if (high && alive && LoadModelNow(371)) Cmd(0x01B2).I(h).I(46).I(1).Run();  // GIVE_WEAPON_TO_CHAR: a parachute
    if (me->AttachedTo()) Cmd(0x0465).I(h).Run();  // DETACH_CHAR_FROM_CAR: the gun held before back in hand
    if (g_ride.unseen) Cmd(0x0337).I(h).I(1).Run();  // SET_CHAR_VISIBLE
    if (v && alive && Aircraft(v)) {  // out of the cabin beside it, not inside its body
        CMatrix* m = v->Matrix();
        void* col = CallMethod<0x535300, void*>(v);
        CVector p = v->GetPosition() + (m ? m->right * ((col ? Field<float>(col, 0x0C) : 3.0f) + 1.5f) : CVector{});
        Cmd(0x00A1).I(h).F(p.x).F(p.y).F(p.z).Run();  // SET_CHAR_COORDINATES
    }
    ShareAboard(false, -1, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0, g_ride.hidden, 0);
    LOGI("Aboard: off %04X%s", g_ride.car, high ? " in the air, with a parachute" : "");
    g_ride = {};
}

// The host's mission moved the host and takes us along (Players::onHostMove). Attached, our CJ stayed on the vehicle
// wherever the move put him, and a move into an interior left him on it outside with that interior's world drawn around
// (found in the 10.11 audit). A seat's rule: with another player still at its wheel the vehicle brings him, out in the
// street in any case; else he is off it, where he is, and the move takes him. To a start point he is off it in any case.
bool MovedByHost(bool indoors, bool onFoot) {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!g_ride.on || !me) return false;
    NetVehicle* nv = Vehicles::Instance().Find(g_ride.car);
    CVehicle* v = nv && Vehicles::Alive(*nv) ? nv->veh : nullptr;
    const RemotePlayer* driver = v && v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    if (driver && !onFoot && (!indoors || driver->mode != RemotePlayer::Mode::OnFoot)) return true;
    ViewOff(v);
    StandForOff(me);
    int h = GetPedRef(me);
    if (me->AttachedTo()) Cmd(0x0465).I(h).Run();    // DETACH_CHAR_FROM_CAR
    if (g_ride.unseen) Cmd(0x0337).I(h).I(1).Run();  // SET_CHAR_VISIBLE
    ShareAboard(false, -1, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0, g_ride.hidden, 0);
    LOGI("Aboard: off %04X, the host's mission takes us along", g_ride.car);
    g_ride = {};
    return false;
}

// the passenger key found no free seat near: the nearest aircraft or boat another player drives with none for us
bool Board() {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!Net().Online() || g_ride.on || !me || me->InVehicle() || me->AttachedTo() || me->Health() <= 0.0f ||
        GetPad(0)->DisablePlayerControls || CutsceneOn())
        return false;
    CPoolHeader* pool = VehiclePool();
    CVehicle* best = nullptr;
    float bestD = 8.0f;
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        float d = (v->GetPosition() - me->GetPosition()).Length();
        if (d < bestD && (Aircraft(v) || Boat(v)) && v->Status() != 5 /* wrecked */ && v->Driver() &&
            Players::Instance().FindByPed(v->Driver()) && !FreeSeat(v))
            best = v, bestD = d;
    }
    NetVehicle* nv = best ? Vehicles::Instance().Find(best) : nullptr;
    if (!nv) return false;
    Place place = PlaceOn(best);
    bool air = Aircraft(best);
    int gun = me->ActiveWeapon().type;
    int weapon = !air && gun >= 22 && gun <= 34 ? gun : 0;  // the pistols to the rifles: shoots from a boat
    bool sit = place.pose && (air || g_view == View::Behind);
    CVector o = place.pose && !sit ? place.rise : place.at;
    uint8_t side = sit ? place.side : 0;
    float range = sit ? 0.0f : 360.0f;  // seated he faces the seat's way
    int h = GetPedRef(me);
    Cmd(0x0464).I(h).I(nv->handle).F(o.x).F(o.y).F(o.z).I(side).F(range).I(weapon).Run();  // ATTACH_CHAR_TO_CAR
    if (!me->AttachedTo()) return false;
    // the view comes with the game's camera of one tied on (ApplyView, from the next frame), our CJ hidden there too
    g_ride = {true, nv->netId, air, place.hidden, o, weapon, place.pose, place.side, sit, false, place.at, place.rise};
    g_behindMs = 0;
    Unseen(me, place.hidden);
    ShareAboard(true, nv->handle, o.x, o.y, o.z, side, range, weapon, place.hidden, sit ? place.pose : 0);
    ui::TheChat().AddSystem(TR("Вы на борту. Клавиша посадки в машину высадит вас. Клавиша смены камеры меняет вид.",
                               "You are aboard. The enter key takes you off. The camera change key changes the view."));
    LOGI("Aboard %04X (model %d) at %.2f,%.2f,%.2f%s%s", nv->netId, nv->model, o.x, o.y, o.z, place.hidden ? ", hidden inside" : "",
         sit ? (place.pose == 2 ? ", seated on a bench" : ", seated") : "");
    return true;
}

class Aboard : public Module {
public:
    const char* Name() const override { return "Aboard"; }
    void OnInstallHooks() override {
        Players::Instance().onNoSeat = &Board;
        Players::Instance().onHostMove = &MovedByHost;
    }

    void OnPostProcess() override {
        if (!g_ride.on) return;
        CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
        if (!me) return void(g_ride = {});
        NetVehicle* nv = Vehicles::Instance().Find(g_ride.car);
        CVehicle* v = nv && Vehicles::Alive(*nv) ? nv->veh : nullptr;
        const CPad* pad = GetPad(0);
        bool leave = pad->NewState.ButtonTriangle && !pad->OldState.ButtonTriangle;  // the enter key, after the game's frame
        if (leave || !v || v->Status() == 5 || me->AttachedTo() != (CPhysical*)v || me->Health() <= 0.0f || CutsceneOn() ||
            !Net().Online()) {
            Off(me, v);
            return;
        }
        // the camera change key (V, the pad's Select), after the game's frame as the enter key: the game itself only
        // clicks for it there (CCamera::CamControl, 0x52B6BC)
        if (pad->NewState.Select && !pad->OldState.Select && !MenuActive()) {
            g_view = g_view == View::Behind ? View::Own : View::Behind;
            LOGI("Aboard: the view %s", g_view == View::Behind ? "from behind" : g_ride.air ? "from the cabin" : "of the gun");
        }
        ApplyView(me, v, nv->handle);
    }
    // the copies of the other games' vehicles go with the session, and one of them under our CJ held him in the air
    void OnSessionEnd() override {
        if (g_ride.on && core::IsPlayerReady()) Off(FindPlayerPed(0), nullptr);
    }
    void OnGameShutdown() override {
        HoldFire(false);
        g_ride = {}, g_cabin = {}, g_behind = false;
    }
    // A player whose game started after we got on had our copy loose by the vehicle, in plain view inside a plane and
    // pushing a boat (10.11 audit): the word of it goes once more. The games that have it leave it (Missions).
    void OnPeerResync(uint8_t) override {
        NetVehicle* nv = g_ride.on ? Vehicles::Instance().Find(g_ride.car) : nullptr;
        if (nv && Vehicles::Alive(*nv))
            ShareAboard(true, nv->handle, g_ride.at.x, g_ride.at.y, g_ride.at.z, g_ride.seated ? g_ride.side : 0,
                        g_ride.seated ? 0.0f : 360.0f, g_ride.weapon, g_ride.hidden, g_ride.seated ? g_ride.pose : 0);
    }

    static Aboard& Instance() {
        static Aboard a;
        return a;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Aboard::Instance()); }
} g_reg;

}  // namespace

}  // namespace coop
