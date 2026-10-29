// Big Smoke's Cash: Cesar's tip about the courier that carries Smoke's money through the countryside. Two streamed
// scripts of main.scm run it in the host's game (the partner's refuses them, as every main.scm trigger): BCESAR2 by day
// on Mondays and Fridays, a Patriot with six cash bags that fly off as it takes damage ($300 each, picked up within
// 3 m), and BCESAR3 by night on Wednesdays and Saturdays, a rider on a Sanchez whose package turns into a pickup where
// he dies (up to $2000, less the longer he rides). Only the host's game runs a courier; for its partners:
//  * the courier's texts and blips come as a mission's do (Missions), the bags once off the car as the mission's objects
//    (Objects), the package as a mission's pickup (Pickups);
//  * the checks of its player near a place, the courier or a bag take a partner there as well: a partner picks up the
//    bags and keeps the courier going (Answer);
//  * what it pays its player goes to the partners within 300 m too (AfterCommand, G_MONEY);
//  * its vehicles take damage from the player only (SET_CAR_ONLY_DAMAGED_BY_PLAYER): a partner's car ramming one counts
//    as the player's (CVehicle::CanVehicleBeDamaged), as the partner's bullets do everywhere (Damage.cpp). The same goes for
//    the bombers' dumper of a joint quarry (QuarryTarget).
#include "features/story/courier/Courier.h"

#include <cmath>
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kActiveScripts = 0xA8B42C;  // CTheScripts::pActiveScripts
constexpr uintptr_t kScriptParams = 0xA43C78;   // CTheScripts::ScriptParams: the arguments the last command read
constexpr uint32_t kBags = 33264;               // BCESAR2: its six cash bags' handles ($33264..$33284)
constexpr int kDayCar = 26, kNightBike = 17;    // the Patriot (BCESAR2's 26@) and the Sanchez (BCESAR3's 17@)
constexpr float kShareRange = 300.0f;
constexpr uint8_t kMoneyShare = 4;  // G_MONEY's op (Money.cpp)

bool Named(void* s, const char* name) { return !_strnicmp(Field<char[8]>(s, 0x08), name, 8); }
void* Thread(const char* name) {
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00))
        if (Named(s, name)) return s;
    return nullptr;
}
int32_t Local(void* s, int n) { return Field<int32_t>(s, 0x3C + n * 4); }  // CRunningScript::m_aLocalVars
int32_t Int(int i) { return ((const int32_t*)kScriptParams)[i]; }
float Float(int i) { return ((const float*)kScriptParams)[i]; }
bool Within(const CVector& a, const CVector& b, float x, float y, float z) {
    CVector d = a - b;
    return std::fabs(d.x) < x && std::fabs(d.y) < y && (z <= 0.0f || std::fabs(d.z) < z);
}

bool CourierVehicle(CVehicle* v) {
    if (void* s = Thread("bcesar2"); s && GetVehicle(Local(s, kDayCar)) == v) return true;
    if (void* s = Thread("bcesar3"); s && GetVehicle(Local(s, kNightBike)) == v) return true;
    return false;
}

// QUARRY's "Destroy" (34@ 16): the bombers' dumper (91@), damaged by the player only, in our quarry the other player joined
bool QuarryTarget(CVehicle* v) {
    void* s = SideLeading() ? RunningMission() : nullptr;
    auto local = [](int n) { return Ref<int32_t>(0xA48960 + (uintptr_t)n * 4); };  // CTheScripts::LocalVariablesForCurrentMission
    return s && Named(s, "quarry") && local(34) == 16 && GetVehicle(local(91)) == v;
}

bool OfRemotePlayer(CEntity* e) {
    CPed* ped = e->IsPed() ? (CPed*)e : e->IsVehicle() ? ((CVehicle*)e)->Driver() : nullptr;
    return ped && Players::Instance().FindByPed(ped);
}

using CanBeDamagedFn = bool(__fastcall*)(CVehicle*, void*, CEntity*, int, bool*);
CanBeDamagedFn g_origCanBeDamaged = nullptr;
bool __fastcall HookCanBeDamaged(CVehicle* v, void* edx, CEntity* damager, int weapon, bool* fireOrBullet) {
    bool asPlayers = damager && Net().Online() && (v->PhysicalFlags() & CPhysical::PF_INVULNERABLE) && OfRemotePlayer(damager) &&
                     ((Net().IsHost() && CourierVehicle(v)) || QuarryTarget(v));
    if (!asPlayers) return g_origCanBeDamaged(v, edx, damager, weapon, fireOrBullet);
    v->PhysicalFlags() &= ~CPhysical::PF_INVULNERABLE;
    bool r = g_origCanBeDamaged(v, edx, damager, weapon, fireOrBullet);
    v->PhysicalFlags() |= CPhysical::PF_INVULNERABLE;
    return r;
}

class Courier : public Module {
public:
    const char* Name() const override { return "Courier"; }
    void OnInstallHooks() override {
        mem::HookFn(0x6D1280, &HookCanBeDamaged, &g_origCanBeDamaged, "CVehicle::CanVehicleBeDamaged");
    }

    static Courier& Instance() {
        static Courier c;
        return c;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Courier::Instance()); }
} g_reg;
}  // namespace

bool CourierThread(void* script) { return script && (Named(script, "bcesar2") || Named(script, "bcesar3")); }

bool CourierRuns() { return Net().Online() && Net().IsHost() && (Thread("bcesar2") || Thread("bcesar3")); }

int CourierObjects(int* handles, int max) {
    int n = 0;
    if (!CourierRuns() || !Thread("bcesar2")) return 0;
    for (int i = 0; i < 6 && n < max; ++i) {
        int h = Ref<int32_t>(addr::ScriptSpace + kBags + (uintptr_t)i * 4);
        if (CObject* o = GetObject(h); o && !o->AttachedTo()) handles[n++] = h;  // on the car they are its load, not shown
    }
    return n;
}

namespace courier {

int Answer(void* script, uint16_t op) {
    if (op != 0x00EC && op != 0x00FE && op != 0x00F2 && op != 0x0104 && op != 0x0205 && op != 0x0474) return -1;
    if (!Net().Online() || !Net().IsHost() || !CourierThread(script)) return -1;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || Int(0) != GetPedRef(me)) return -1;
    CVector at;
    float x, y, z = 0.0f;
    if (op == 0x00EC) {  // LOCATE_CHAR_ANY_MEANS_2D: a place
        at = {Float(1), Float(2), 0.0f}, x = Float(3), y = Float(4);
    } else if (op == 0x00FE) {  // ..._3D
        at = {Float(1), Float(2), Float(3)}, x = Float(4), y = Float(5), z = Float(6);
    } else {  // ..._CHAR_2D, _CHAR_3D, _CAR_3D, _OBJECT_3D: the courier, his car, a bag
        CEntity* e = op == 0x00F2 || op == 0x0104 ? (CEntity*)GetPed(Int(1))
                     : op == 0x0205              ? (CEntity*)GetVehicle(Int(1))
                                                 : (CEntity*)GetObject(Int(1));
        if (!e) return -1;
        at = e->GetPosition(), x = Float(2), y = Float(3);
        if (op != 0x00F2) z = Float(4);
    }
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState && rp.ped->Health() > 0.0f && Within(rp.ped->GetPosition(), at, x, y, z)) return 1;
    return -1;
}

void AfterCommand(void* script, uint16_t op) {
    if (op != 0x0109 || !Net().Online() || !Net().IsHost() || !CourierThread(script)) return;  // ADD_SCORE $PLAYER money
    int32_t paid = Int(1);
    CPlayerPed* me = FindPlayerPed(0);
    if (paid <= 0 || !me) return;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.ped || !rp.hasState || (rp.ped->GetPosition() - me->GetPosition()).Length() > kShareRange) continue;
        Writer w(proto::G_MONEY);
        w.U8(kMoneyShare);
        w.U8(id);
        w.I32(paid);
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Big Smoke's courier paid $%d: the same to player %d", paid, id);
    }
}

}  // namespace courier
}  // namespace coop
