// Zero's missions for two (0.3.3, the players' idea, chosen 31.10). In "Air Raid" (ZERO1) the host shoots Berkley's RC
// Barons from the turret on Zero's roof, in "Supply Lines..." (ZERO2) it flies an RC Baron, in "New Model Army" (ZERO4)
// an RC Goblin over the war table (area 10), and the partner had nothing to do. While the host's turret or toy is on,
// the partner's game gives it one of its own: a minigun beside the host's on the roof (an RC Baron there first: the
// players wanted the gun, and the host's turret took the toy for one of Berkley's), an RC Baron on Zero's roof, an RC
// Goblin by the host's on the table. The toy's gun hits with no damager (CWeapon::FireOneInstantHitRound): Damage
// counts such a round as ours while our toy is out, so its hits on the host's targets go to the host's game. Each game
// tells the other one which toy is its player's, and without friendly fire the guns spare it. The enter key gives the
// partner back its CJ for the rest of the host's part.
//
// CRemote::GivePlayerRemoteControlledCar (0x45AB10) puts the toy on the ground under the point given and disables the
// player's own vehicle, which is why the missions sit CJ in one. On foot the sticks would walk CJ and the fire button
// shoot his gun as well: the partner stands frozen with bare hands meanwhile. Streaming follows the toy
// (CStreaming::Update), but collision and the world's interiors go by the player's area: for the war table CJ goes into
// area 10 where he stands, frozen in the air, and back once the floor of the street is there again. The toy waits for
// the floor it starts from (ToyPlace), and a partner far from the shop is brought to its door first (ComeOver).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::rctoys {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { OP_PHASE = 1, OP_TOY = 2 };
constexpr int kBaron = 464, kGoblin = 501;
constexpr int kMinigun = 362, kGun = 38;  // a phase of the minigun's model: a turret (WEAPONTYPE_MINIGUN)
constexpr int kTurretBase = 1485;         // what CJ is fixed to, out of sight (ZERO1's own is a model of its mission)
// Zero's roof, where "Supply Lines..." gives its player the RC Baron (83@..85@, heading 40.09)
const CVector kRoof{-2205.44f, 128.99f, 57.33f};
constexpr float kRoofHeading = 40.09f;
const CVector kTable{-973.759f, 1077.152f, 1350.399f};  // "New Model Army": the host's RC Goblin on the war table
// where our CJ waits in area 10 meanwhile: beside the host's, which the mission parks on a BMX at -1004.1,1000.8,1349.7
const CVector kParked{-1006.5f, 1000.8f, 1349.7f};
const CVector kTableView{-982.635f, 1055.88f, 1348.926f};  // a camera of ZERO4's own over the table
// the street at the shop's door: between where ZERO4 (-2244.7,132.2) and ZERO2 (-2244.4,136.9) put CJ back, in neither
const CVector kDoor{-2244.55f, 134.5f, 34.32f};
constexpr float kNearShop = 300.0f;                      // a partner farther from Zero's shop is brought to its door
constexpr uint64_t kAgainMs = 8000;                      // a new toy after ours was blown up

CVehicle* Toy() { return PlayerInfos()[0].remoteVehicle; }  // CPlayerInfo::m_pRemoteVehicle

// The toy given up: REMOVE_RC_BUGGY (CPlayerInfo::BlowUpRCBuggy(false)) only takes it from the player, and a plane left in
// the air fell and blew up, which the host saw as the partner blowing up ("Supply Lines", 01.11). It goes with it.
void DropToy() {
    CVehicle* t = Toy();
    int h = t ? GetVehicleRef(t) : -1;
    Cmd(0x04DB).Run();  // REMOVE_RC_BUGGY
    if (h >= 0 && GetVehicle(h)) Cmd(0x00A6).I(h).Run();  // DELETE_CAR
}

struct Phase {
    bool on = false;
    uint16_t model = 0;
    CVector at;
    float heading = 0;
    int8_t area = 0;
    bool operator==(const Phase& o) const { return on == o.on && model == o.model && area == o.area; }
};
Phase g_sent;   // host: what the partners were told
Phase g_phase;  // partner: the host's

struct Own {
    bool out = false;      // our toy or turret, given for the host's
    bool turret = false;   // the turret: our CJ on base
    int base = -1;         // its object
    int heavy = 0, heavyAmmo = 0;  // what the minigun's slot held before
    bool table = false;    // our CJ moved into area 10 for it
    bool camera = false;   // our camera on the table while it loads
    uint64_t waitUntil = 0;  // waiting for the floor under the toy's place before the toy (8 s at most)
    bool declined = false;  // left with the enter key, or no floor came: none again in this part of the host's
    bool cameOver = false;  // brought to the shop's door in this part of the host's
    uint64_t cameraMs = 0;  // the camera last put back on our toy
    int weapon = 0;        // the gun he held
    CVector stood;         // where he stood
    uint64_t goneMs = 0;   // our toy went then
    uint64_t floorUntil = 0;  // back in the street: frozen until its floor is there (5 s at most)
} g_own;
uint16_t g_toyTold = 0;

void SendPhase(const Phase& p) {
    Writer w(proto::G_RC);
    w.U8(OP_PHASE), w.U8(p.on ? 1 : 0), w.U16(p.model), w.Vec({p.at.x, p.at.y, p.at.z}), w.F32(p.heading), w.I8(p.area);
    Net().Send(proto::CH_RELIABLE, w);
}

// host: its Zero mission's part with the turret or a toy
Phase HostPhase() {
    void* s = RunningMission();
    CPlayerPed* me = FindPlayerPed(0);
    if (!s || !me) return {};
    const char* name = Field<char[8]>(s, 0x08);
    CPhysical* on = me->AttachedTo();
    CVehicle* toy = Toy();
    if (!_strnicmp(name, "zero1", 8) && on && on->IsObject()) {  // its turret: the object out of sight CJ is fixed to
        CMatrix* m = on->Matrix();
        float heading = m ? std::atan2(-m->forward.x, m->forward.y) * 57.29578f : 90.0f;
        return {true, kMinigun, on->GetPosition(), heading, 0};
    }
    if (!_strnicmp(name, "zero2", 8) && toy) return {true, kBaron, kRoof, kRoofHeading, 0};
    if (!_strnicmp(name, "zero4", 8) && toy && toy->ModelIndex() == kGoblin) return {true, kGoblin, kTable, 0.0f, 10};
    return {};
}

void Back(CPlayerPed* me) {
    int h = GetPedRef(me);
    if (g_own.turret) {
        Cmd(0x0465).I(h).Run();  // DETACH_CHAR_FROM_CAR, from an object too: the engine puts the gun he held back in hand
        if (g_own.base >= 0) Cmd(0x0108).I(g_own.base).Run();  // DELETE_OBJECT
        Cmd(0x0555).I(h).I(kGun).Run();  // REMOVE_WEAPON_FROM_CHAR: the minigun was the turret's
        if (g_own.heavy > 0 && g_own.heavyAmmo > 0) {  // his own weapon of that slot back
            int model = -1;
            Cmd(0x0781).I(g_own.heavy).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
            if (model > 0) LoadModelNow(model);
            Cmd(0x01B2).I(h).I(g_own.heavy).I(g_own.heavyAmmo).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x02AB).I(h).I(0).I(0).I(0).I(0).I(0).Run();  // SET_CHAR_PROOFS off
        Cmd(0x00A1).I(h).F(g_own.stood.x).F(g_own.stood.y).F(g_own.stood.z - 1.0f).Run();  // SET_CHAR_COORDINATES (feet)
        g_own.base = -1;
        g_own.turret = false;
    } else if (g_own.table) {
        // to Zero's door, by the host's war room: where he had stood was a street of the city for a partner who came from
        // there, and leaving the table looked like a teleport away ("New Model Army", 01.11)
        g_own.stood = kDoor;
        Cmd(0x0860).I(h).I(0).Run();  // SET_CHAR_AREA_VISIBLE: the street again
        Cmd(0x04BB).I(0).Run();       // SET_AREA_VISIBLE
        Cmd(0x03CB).F(g_own.stood.x).F(g_own.stood.y).F(g_own.stood.z).Run();  // LOAD_SCENE: its floor first
        Cmd(0x00A1).I(h).F(g_own.stood.x).F(g_own.stood.y).F(g_own.stood.z - 1.0f).Run();  // SET_CHAR_COORDINATES (feet)
        g_own.floorUntil = NowMs() + 5000;
    } else {
        Cmd(0x04D7).I(h).I(0).Run();  // FREEZE_CHAR_POSITION off
    }
    if (g_own.camera) Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT: the table's view while it loaded
    if (g_own.weapon) Cmd(0x01B9).I(h).I(g_own.weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    g_own.out = g_own.table = g_own.camera = false;
    g_own.waitUntil = 0;
    g_own.weapon = 0;
    g_own.goneMs = NowMs();
}

void TakeAway(CPlayerPed* me) {
    g_own.declined = g_own.cameOver = false;  // the host's next part gives one again
    if (!g_own.out && !g_own.waitUntil) return;
    if (Toy()) DropToy();
    Back(me);
    LOGI("Zero's mission: our toy or turret taken away (the host's part is over)");
}

// beside the host's turret: the roof's floor as high as the turret's, room for CJ
bool TurretSpot(CVector& out) {
    int first = std::max(PartnerRank(Net().MyId()), 0) * 4;  // the next partners begin elsewhere round the host's
    for (float r : {2.5f, 3.5f, 1.8f})
        for (int k = 0; k < 12; ++k) {
            float a = (float)(k + first) * 0.5235988f;
            CVector p = g_phase.at + CVector{std::cos(a) * r, std::sin(a) * r, 0.0f};
            bool found = false;
            float z = Call<0x5696C0, float>(p.x, p.y, g_phase.at.z + 1.5f, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
            if (!found || std::fabs(z - g_phase.at.z) > 0.6f) continue;
            CVector body{p.x, p.y, z + 1.0f};
            if (Call<0x569E20, CEntity*>(body, 0.4f, (CEntity*)nullptr, true, true, false, true, false, false)) continue;  // CWorld::TestSphereAgainstWorld
            out = {p.x, p.y, z};
            return true;
        }
    return false;
}

// "Air Raid": a minigun as the mission makes the host's, an object out of sight with no collision and CJ fixed to it
void GiveTurret(CPlayerPed* me) {
    CVector at;
    if (!TurretSpot(at)) {
        g_own.goneMs = NowMs();  // again in a while
        LOGW("Zero's mission: no room for our turret by the host's at %.1f,%.1f,%.1f", g_phase.at.x, g_phase.at.y, g_phase.at.z);
        return;
    }
    int h = GetPedRef(me);
    Cmd(0x0107).I(kTurretBase).F(at.x).F(at.y).F(at.z).Out(&g_own.base).Run();  // CREATE_OBJECT
    if (g_own.base < 0) return;
    g_own.stood = me->GetPosition();
    g_own.weapon = me->ActiveWeapon().type;
    void* info = Call<0x743C60, void*>(kGun, (uint8_t)1);  // CWeaponInfo::GetWeaponInfo
    const CWeapon& had = me->Weapons()[info ? std::clamp(Field<int32_t>(info, 0x14), 0, NUM_WEAPON_SLOTS - 1) : 7];  // m_nSlot
    g_own.heavy = had.type, g_own.heavyAmmo = had.ammoTotal;
    Cmd(0x0382).I(g_own.base).I(0).Run();                // SET_OBJECT_COLLISION
    Cmd(0x0750).I(g_own.base).I(0).Run();                // SET_OBJECT_VISIBLE
    Cmd(0x0177).I(g_own.base).F(g_phase.heading).Run();  // SET_OBJECT_HEADING
    // as ZERO1 fixes its player: this offset, the whole round, the minigun (the attachment gives it)
    Cmd(0x04F4).I(h).I(g_own.base).F(-0.25f).F(-0.5f).F(1.25f).I(2).F(360.0f).I(kGun).Run();  // ATTACH_CHAR_TO_OBJECT
    Cmd(0x01B9).I(h).I(kGun).Run();                   // SET_CURRENT_CHAR_WEAPON
    Cmd(0x0858).I(0).F(270.0f).F(300.0f).Run();       // SET_HEADING_FOR_ATTACHED_PLAYER, as ZERO1
    Cmd(0x02AB).I(h).I(1).I(1).I(1).I(1).I(1).Run();  // SET_CHAR_PROOFS: Berkley's planes bomb the roof
    g_own.out = g_own.turret = true;
    ui::TheChat().AddSystem(TR("У вас свой пулемёт рядом с хостом. Сбивайте самолёты вместе с ним. Клавиша посадки в машину вернёт вас на место.",
                               "You have a minigun of your own next to the host. Shoot the planes down together. The enter key takes you back."));
    LOGI("Zero's mission: our turret at %.1f,%.1f,%.1f (slot's own weapon %d x%d)", at.x, at.y, at.z, g_own.heavy, g_own.heavyAmmo);
}

// Beside the host's toy, on the floor it starts from (Zero's roof, the war table 5 m under the host's RC Goblin) with
// room around: CRemote puts the toy on the ground under the point given. None until that floor is loaded: from the
// street the table was not there, the toy fell into the void under it and the screen stayed green (31.10). The check
// took the table itself, 5 m under the host's toy, for no floor, and the partner stood in the void 8 s at a time
// (31.10, the fourth test).
bool ToyPlace(CVector& out) {
    float below = g_phase.area ? 7.0f : 1.5f;
    int first = std::max(PartnerRank(Net().MyId()), 0) * 4;
    for (float r : {6.0f, 4.0f, 8.0f})
        for (int k = 0; k < 12; ++k) {
            float a = g_phase.heading * 0.0174533f + (float)(k + first) * 0.5235988f;
            CVector p = g_phase.at + CVector{std::cos(a) * r, std::sin(a) * r, 0.0f};
            bool found = false;
            float z = Call<0x5696C0, float>(p.x, p.y, g_phase.at.z + 1.0f, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
            if (!found || z < g_phase.at.z - below || z > g_phase.at.z + 1.0f) continue;
            CVector body{p.x, p.y, z + 1.2f};
            if (Call<0x569E20, CEntity*>(body, 1.0f, (CEntity*)nullptr, true, true, false, true, false, false)) continue;  // CWorld::TestSphereAgainstWorld
            out = {p.x, p.y, z + 0.5f};
            return true;
        }
    return false;
}

void Launch(const CVector& at) {
    Cmd(0x046E).I(0).F(at.x).F(at.y).F(at.z).F(g_phase.heading).I(g_phase.model).Run();  // GIVE_REMOTE_CONTROLLED_MODEL_TO_PLAYER
    // made in area 0: at the war table (area 10) the partner flew a toy nobody saw, himself included (01.11); ZERO4 sets
    // its own toys there the same way. Before the toy is shared, so the other game's copy is there too.
    if (CVehicle* t = g_phase.area ? Toy() : nullptr) Cmd(0x0840).I(GetVehicleRef(t)).I(g_phase.area).Run();  // SET_VEHICLE_AREA_VISIBLE
    // off, as Zero's missions have it: on, the fire button blew the toy up instead of shooting (31.10, CAutomobile's
    // STATUS_REMOTE_CONTROLLED: CarGunJustDown and not CVehicle::bDisableRemoteDetonation)
    Cmd(0x048A).I(0).Run();  // SET_ENABLE_RC_DETONATE
    Cmd(0x04D6).I(0).Run();  // SET_ENABLE_RC_DETONATE_ON_CONTACT
    g_own.out = true;
    ui::TheChat().AddSystem(g_phase.model == kGoblin
                                ? TR("У вас свой вертолёт на радиоуправлении с магнитом. Кнопка огня поднимает и сбрасывает бочки и доски. "
                                     "Клавиша посадки в машину вернёт вас к CJ.",
                                     "You have an RC helicopter of your own with a magnet. The fire button picks barrels and planks up "
                                     "and drops them. The enter key takes you back to CJ.")
                                : TR("У вас свой самолёт на радиоуправлении. Помогайте хосту сбивать цели. Клавиша посадки в машину вернёт вас к CJ.",
                                     "You have an RC plane of your own. Help the host shoot the targets. The enter key takes you back to CJ."));
    LOGI("Zero's mission: our toy (model %d, area %d) at %.1f,%.1f,%.1f", g_phase.model, g_phase.area, at.x, at.y, at.z);
}

// waiting for the floor under the toy's place (ToyPlace): at once when it is there
void ToyWait(CPlayerPed* me) {
    if (!g_phase.on || !flow::InHostWorld()) return Back(me);
    CVector at;
    if (ToyPlace(at)) {
        g_own.waitUntil = 0;
        g_own.camera = false;  // CRemote puts the camera on the toy
        return Launch(at);
    }
    if (NowMs() < g_own.waitUntil) return;
    LOGW("Zero's mission: no floor for our toy by %.1f,%.1f,%.1f in 8 s, none in this part", g_phase.at.x, g_phase.at.y, g_phase.at.z);
    Back(me);
    g_own.declined = true;  // not the void again and again
}

void GiveToy(CPlayerPed* me) {
    int h = GetPedRef(me);
    g_own.stood = me->GetPosition();
    g_own.weapon = me->ActiveWeapon().type;
    Cmd(0x01B9).I(h).I(0).Run();  // SET_CURRENT_CHAR_WEAPON: bare hands, the fire button fires the toy's gun
    Cmd(0x04D7).I(h).I(1).Run();  // FREEZE_CHAR_POSITION: the sticks drive the toy, not CJ
    if (g_phase.area) {
        // collision streams by where our CJ is: he waits in area 10 by the host's parked one, the camera on the table
        // rather than on him in the void
        CVector at = g_phase.at;
        Cmd(0x0860).I(h).I(g_phase.area).Run();                          // SET_CHAR_AREA_VISIBLE
        Cmd(0x04BB).I(g_phase.area).Run();                               // SET_AREA_VISIBLE
        Cmd(0x00A1).I(h).F(kParked.x).F(kParked.y).F(kParked.z).Run();  // SET_CHAR_COORDINATES
        Cmd(0x03CB).F(at.x).F(at.y).F(at.z).Run();                       // LOAD_SCENE
        Cmd(0x04E4).F(at.x).F(at.y).Run();                               // REQUEST_COLLISION
        Cmd(0x015F).F(kTableView.x).F(kTableView.y).F(kTableView.z).F(0.0f).F(0.0f).F(0.0f).Run();  // SET_FIXED_CAMERA_POSITION
        Cmd(0x0160).F(at.x).F(at.y).F(at.z).I(2).Run();                  // POINT_CAMERA_AT_POINT
        g_own.table = g_own.camera = true;
    }
    g_own.waitUntil = NowMs() + 8000;
    ToyWait(me);
}

// The host's mission moved the host to Zero's shop, and the partner follows once CJ stands in the street again (CS_FOLLOW):
// ZERO2 and ZERO4 park him in a frozen vehicle out of the way meanwhile. The partner the host started /mission zero2 next
// to, 2.8 km away, got no plane (31.10). Brought to the shop's door as the follow brings it: not out of an activity of
// its own, a shop or death; a car it drives comes along.
void ComeOver(CPlayerPed* me) {
    g_own.cameOver = true;
    if (RunningMission() || Ref<uint8_t>(0xA444A8) || me->Health() <= 0.0f) return;  // CTheScripts::bMiniGameInProgress
    int h = GetPedRef(me);
    CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
    float far = (me->GetPosition() - kDoor).Length2D();
    if (v && (me->AreaCode() != 0 || v->Driver() != me)) Cmd(0x0362).I(h).F(kDoor.x).F(kDoor.y).F(kDoor.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    Cmd(0x04BB).I(0).Run();                                           // SET_AREA_VISIBLE
    Cmd(0x0860).I(h).I(0).Run();                                      // SET_CHAR_AREA_VISIBLE
    Cmd(0x03CB).F(kDoor.x).F(kDoor.y).F(kDoor.z).Run();               // LOAD_SCENE: the roof's floor with it
    Cmd(0x00A1).I(h).F(kDoor.x).F(kDoor.y).F(kDoor.z - 1.0f).Run();  // SET_CHAR_COORDINATES (feet)
    LOGI("Zero's mission: %.0f m from the shop, brought to its door", far);
}

// back in the street from the war table: frozen until its floor is under CJ (after a session's end too)
bool FloorWait(CPlayerPed* me) {
    if (!g_own.floorUntil) return false;
    bool found = false;
    float z = Call<0x5696C0, float>(g_own.stood.x, g_own.stood.y, g_own.stood.z + 1.0f, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
    if ((!found || z < g_own.stood.z - 2.5f) && NowMs() < g_own.floorUntil) return true;
    Cmd(0x04D7).I(GetPedRef(me)).I(0).Run();  // FREEZE_CHAR_POSITION off
    g_own.floorUntil = 0;
    LOGI("Back in the street from the war table: %s", found ? "on its floor" : "no floor found in 5 s");
    return false;
}

void PartnerUpdate(CPlayerPed* me) {
    if (g_own.waitUntil) return ToyWait(me);
    if (g_own.out && (g_own.turret ? !me->AttachedTo() : !Toy())) {  // shot down, or something took him off the turret
        LOGI("Zero's mission: our %s is gone", g_own.turret ? "turret" : "toy");
        Back(me);
        return;
    }
    if (!g_phase.on || !flow::InHostWorld()) return TakeAway(me);
    const CPad* pad = GetPad(0);
    if (g_own.out && pad->NewState.ButtonTriangle && !pad->OldState.ButtonTriangle) {  // the enter key: his CJ back
        LOGI("Zero's mission: we leave our %s for the rest of the host's part", g_own.turret ? "turret" : "toy");
        if (Toy()) DropToy();
        Back(me);
        g_own.declined = true;
        return;
    }
    // The camera on our toy (CRemote gives it with CCamera::TakeControl, the target at +0x958). A scene of the host's on
    // the engine takes it and gives it back to CJ: the sticks flew a toy out of sight while CJ stood frozen on the screen
    // ("я застрял", 31.10).
    if (g_own.out && !g_own.turret && Toy() && !SceneWatched() && !Ref<uint8_t>(0xB5F851) &&
        Ref<CEntity*>(addr::TheCamera + 0x958) != (CEntity*)Toy() && NowMs() - g_own.cameraMs > 1000) {
        g_own.cameraMs = NowMs();
        CallMethod<0x50C7C0>((void*)addr::TheCamera, (CEntity*)Toy(), 0x12, 1, 1);  // CCamera::TakeControl, MODE_BEHINDCAR as CRemote
        LOGI("Zero's mission: the camera on our toy again");
    }
    // no toy while the host's scene holds the camera: one given then was never in the picture
    if (g_own.out || g_own.declined || Toy() || Ref<uint8_t>(0xB5F851) || SceneWatched() ||  // CCutsceneMgr::ms_running
        NowMs() - g_own.goneMs < kAgainMs)
        return;
    bool turret = g_phase.model == kMinigun;
    // far from the shop: brought to its door once, for a toy (the turret's partner comes along with the host's own move)
    if ((me->GetPosition() - kRoof).Length2D() > kNearShop) {
        if (!turret && !g_own.cameOver) ComeOver(me);
        return;
    }
    if (me->InVehicle() || me->AreaCode() != 0) return;
    int model = turret ? kTurretBase : g_phase.model;
    if (!IsModelLoaded(model) || (turret && !IsModelLoaded(kMinigun))) {
        RequestModel(model, 0);  // in the background, a frame or two
        if (turret) RequestModel(kMinigun, 0);
        return;
    }
    if (turret) GiveTurret(me);
    else GiveToy(me);
}

// each game: the toy its player drives, for the other one's guns to spare
void TellToy() {
    CVehicle* toy = Toy();
    NetVehicle* nv = toy ? Vehicles::Instance().Find(toy) : nullptr;
    uint16_t id = nv ? nv->netId : 0;
    if (id == g_toyTold || (toy && !nv)) return;  // a new toy is not shared yet: next frame
    g_toyTold = id;
    Writer w(proto::G_RC);
    w.U8(OP_TOY), w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (op == OP_TOY) {
        uint16_t id = r.U16();
        if (r.Ok() && from != Net().MyId())
            if (RemotePlayer* rp = Players::Instance().Get(from)) rp->toyNetId = id;
        return;
    }
    Phase p;
    p.on = r.U8() != 0;
    p.model = r.U16();
    Vec3 a = r.Vec();
    p.heading = r.F32();
    p.area = r.I8();
    if (!r.Ok() || op != OP_PHASE || from != Net().HostId() || Net().IsHost() ||
        (p.on && p.model != kBaron && p.model != kGoblin && p.model != kMinigun) ||
        !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) || !std::isfinite(p.heading) || (p.area != 0 && p.area != 10))
        return;
    p.at = {a.x, a.y, a.z};
    if (p.on != g_phase.on) LOGI("The host's Zero mission: its turret or toy %s", p.on ? "on" : "off");
    g_phase = p;
}

class RcToys : public Module {
public:
    const char* Name() const override { return "RcToys"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_RC, &Handle); }
    void OnPeerJoin(uint8_t) override { g_sent = {}, g_toyTold = 0; }  // the newcomer hears it all again
    void OnSessionEnd() override {
        if (CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr) TakeAway(me);
        g_phase = g_sent = {};
        g_toyTold = 0;
    }
    void OnGameShutdown() override { g_own = {}, g_phase = g_sent = {}, g_toyTold = 0; }  // the toy goes with the world
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        if (FloorWait(me) || !Net().Online()) return;
        if (Net().IsHost()) {
            if (Phase p = HostPhase(); !(p == g_sent)) {
                g_sent = p;
                SendPhase(p);
                LOGI("Zero's mission: our turret or toy %s (the partner gets model %d)", p.on ? "on" : "off", p.model);
            }
        } else {
            PartnerUpdate(me);
        }
        TellToy();
    }

    static RcToys& Instance() {
        static RcToys m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&RcToys::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop::rctoys
