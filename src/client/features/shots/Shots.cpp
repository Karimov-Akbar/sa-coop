// The other player's shots from a vehicle that its copy here does not fire itself (#76). A drive-by mod (any weapon at
// the wheel, an aim of its own) shoots by its own code, which runs for its own player only: the copy here sat still while
// that player shot, and neither saw the other's shots. Each game tells the bullets its player fires from a vehicle
// (CWeapon::DoBulletImpact: where from, where to, the weapon), and the other game draws them on that player's copy: the
// tracer, the muzzle flash and the shot's sound. A copy that fired itself meanwhile (the game's own drive-by goes over by
// its pad) is left to its own shots, so they do not come twice. The hits count as before: the shooter's game reports them.
// The guns fixed on a vehicle go over the same way: the copy never fires them (CVehicle::ProcessWeapons runs for a player's
// vehicle only). A boat's (the Predator's) fire by CVehicle::FireFixedMachineGuns (HookFixedRound); an aircraft's (the
// Hunter's, the Seasparrow's, the Rustler's) by CVehicle::FirePlaneGuns, a CWeapon::FireInstantHit whose shooter is the
// aircraft itself, not the pilot: those went over from 12.11 on, before the other player neither heard nor saw them.
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uint64_t kWaitMs = 200;      // a bullet is drawn this late: the copy's own shot by the pad comes meanwhile
constexpr uint64_t kOwnShotMs = 400;   // a copy that fired itself this recently draws its own
constexpr float kDrawRange = 200.0f;
constexpr int kMaxPerFrame = 8;

struct Bullet {
    Vec3 from, to;
    uint8_t weapon;
};
struct Queued {
    uint8_t player;
    Bullet b;
    uint64_t at;
};
std::vector<Bullet> g_ours;   // this frame's, to send
std::vector<Queued> g_theirs;
std::map<uint8_t, uint64_t> g_copyFired;  // when each player's copy here fired a bullet of its own
std::map<uint8_t, uint64_t> g_soundMs;    // its last shot's sound here: a shotgun's pellets are one shot
std::map<uint8_t, uint64_t> g_traceMs;    // its last shot's trace here: one a shot (a shotgun's pellets too)
std::map<uint8_t, uint64_t> g_gunsToldMs; // its vehicle's guns last drawn here, for the log
uint64_t g_aircraftToldMs = 0;            // our aircraft's guns last sent, for the log

using DoBulletImpactFn = void(__fastcall*)(void*, void*, CEntity*, CEntity*, CVector*, CVector*, void*, int);
DoBulletImpactFn g_origImpact = nullptr;
void __fastcall HookDoBulletImpact(void* weapon, void* edx, CEntity* by, CEntity* victim, CVector* start, CVector* end, void* hit,
                                   int incremental) {
    g_origImpact(weapon, edx, by, victim, start, end, hit, incremental);
    if (!Net().Online() || !by || !start || !end) return;
    if (RemotePlayer* rp = by->IsPed() ? Players::Instance().FindByPed((CPed*)by) : nullptr) {
        g_copyFired[rp->id] = NowMs();
        // its whole way, as the game draws the rounds of the guns fixed on vehicles (FireOneInstantHitRound): of a
        // player's shot the game draws a short faint piece at random, none in its first-person aims standing still, and
        // the other player saw no trace of the copy's shots (29.10)
        if (uint64_t& last = g_traceMs[rp->id]; NowMs() - last >= 50) {
            last = NowMs();
            CVector to = victim && hit ? *(CVector*)hit : *end;  // CColPoint::m_vecPoint: where it stopped
            Call<0x723750>(start, &to, 0.02f, 750u, (uint8_t)150);  // CBulletTraces::AddTrace
        }
        return;
    }
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* car = me && me->InVehicle() ? me->Vehicle() : nullptr;
    // its guns (CVehicle::FirePlaneGuns, 0x6D4E7C gives FireInstantHit the vehicle); a tank's shell is the tank's too
    // (CAutomobile::TankControl), and goes as its explosion
    int sub = car ? car->VehicleSubClass() : -1;
    bool aircraft = car && by == (CEntity*)car && (sub == VEHICLE_TYPE_HELI || sub == VEHICLE_TYPE_PLANE);
    if (!car || ((CPed*)by != me && !aircraft) || (int)g_ours.size() >= kMaxPerFrame) return;
    CVector to = victim && hit ? *(CVector*)hit : *end;  // CColPoint::m_vecPoint: where it stopped
    // an aircraft's round is drawn as the fixed guns' (38: Draw), whichever gun it was (31 or 38)
    uint8_t type = aircraft ? 38 : (uint8_t)Field<int32_t>(weapon, 0x00);  // CWeapon::m_eWeaponType
    g_ours.push_back({{start->x, start->y, start->z}, {to.x, to.y, to.z}, type});
    if (aircraft && NowMs() - g_aircraftToldMs > 5000) LOGD("Our aircraft's guns fire: the rounds go to the other games");
    if (aircraft) g_aircraftToldMs = NowMs();
}

// a round of the guns fixed on a vehicle (CVehicle::FireFixedMachineGuns -> FireOneInstantHitRound): our player's, as a
// bullet to the line's end or what it hit there
using RoundFn = void(__cdecl*)(CVector*, CVector*, int);
RoundFn g_origRound = nullptr;
void __cdecl HookFixedRound(CVector* start, CVector* end, int damage) {
    g_origRound(start, end, damage);
    CPlayerPed* me = FindPlayerPed(0);
    if (!Net().Online() || !start || !end || !me || !me->InVehicle() || (int)g_ours.size() >= kMaxPerFrame ||
        (me->Vehicle()->GetPosition() - *start).Length() > 15.0f)
        return;
    uint8_t cp[0x2C] = {};  // CColPoint: m_vecPoint first
    CEntity* hit = nullptr;
    CVector to = *end;
    if (Call<0x56BA00, bool>(start, end, (void*)cp, &hit, true, true, true, true, true, true, false, false) && hit)  // CWorld::ProcessLineOfSight
        to = *(CVector*)cp;
    g_ours.push_back({{start->x, start->y, start->z}, {to.x, to.y, to.z}, 38});  // drawn as a minigun's
}

void Handle(uint8_t from, Reader& r) {
    int n = std::min<int>(r.U8(), kMaxPerFrame);
    uint64_t at = NowMs() + kWaitMs;
    for (int i = 0; i < n; ++i) {
        Bullet b{r.Vec(), r.Vec(), r.U8()};
        if (!r.Ok()) return;
        if (b.weapon >= 22 && b.weapon <= 38) g_theirs.push_back({from, b, at});  // the guns: pistol to minigun
    }
}

void Draw(RemotePlayer& rp, const Bullet& b) {
    CVector from{b.from.x, b.from.y, b.from.z}, to{b.to.x, b.to.y, b.to.z};
    CPlayerPed* me = FindPlayerPed(0);
    if (!rp.ped || !me || rp.ped->AreaCode() != me->AreaCode() || (rp.ped->GetPosition() - me->GetPosition()).Length() > kDrawRange)
        return;
    CVector dir = to - from;
    float len = dir.Length();
    if (len < 0.01f || len > 1000.0f) return;
    dir = dir * (1.0f / len);
    // A round of a vehicle's guns (38: HookFixedRound, an aircraft's in HookDoBulletImpact) goes its whole way, as
    // FireOneInstantHitRound draws a boat's (0x73AFC7) and the copy's own shots are drawn (29.10); the game's short piece
    // at random (0x726AF0) stays for the shots a player fires from the window
    bool fixed = b.weapon == 38;
    if (fixed) Call<0x723750>(&from, &to, 0.02f, 750u, (uint8_t)150);            // CBulletTraces::AddTrace
    else Call<0x726AF0>(&from, &to, (int)b.weapon, (CEntity*)rp.ped);           // CBulletTraces::AddTrace
    CallMethod<0x4A0DE0>((void*)0xA9AE00, (CEntity*)nullptr, &from, &dir, true);  // g_fx.TriggerGunshot: the muzzle flash
    if (uint64_t& told = g_gunsToldMs[rp.id]; fixed && NowMs() - told > 5000)
        LOGD("Player %d's vehicle guns fire: drawn here", rp.id);
    if (fixed) g_gunsToldMs[rp.id] = NowMs();
    uint64_t& sound = g_soundMs[rp.id];
    if (NowMs() - sound < 60) return;
    sound = NowMs();
    // A round of a vehicle's guns (38) sounds as the game sounds it (CVehicle::FireFixedMachineGuns and FirePlaneGuns:
    // AE_WEAPON_FIRE_PLANE with the M4, from the vehicle). As a minigun's fire the shared weapon audio started the minigun's
    // loop on the copy, and the loop never stopped: after a police boat's guns the other player's copy kept sounding of a
    // minigun on foot until a save was loaded (09.11).
    CEntity* source = fixed && rp.ped->InVehicle() && rp.ped->Vehicle() ? (CEntity*)rp.ped->Vehicle() : (CEntity*)rp.ped;
    CallMethod<0x506F40>((void*)0xB6BC90, fixed ? 149 : 145, fixed ? 31 : (int)b.weapon, source);  // AudioEngine.ReportWeaponEvent
}

class Shots : public Module {
public:
    const char* Name() const override { return "Shots"; }
    void OnInstallHooks() override {
        mem::HookFn(0x73B550, &HookDoBulletImpact, &g_origImpact, "CWeapon::DoBulletImpact");
        mem::RedirectCallFn(0x73E0BA, &HookFixedRound, &g_origRound);  // CVehicle::FireFixedMachineGuns, its two rounds
        mem::RedirectCallFn(0x73E1F7, &HookFixedRound, &g_origRound);
        Net().SetHandler(proto::G_BULLET, &Handle);
    }
    void OnSessionEnd() override { g_ours.clear(), g_theirs.clear(), g_copyFired.clear(), g_traceMs.clear(); }
    void OnPeerLeave(uint8_t id) override {
        g_copyFired.erase(id);
        g_theirs.erase(std::remove_if(g_theirs.begin(), g_theirs.end(), [id](const Queued& q) { return q.player == id; }), g_theirs.end());
    }
    void OnPostProcess() override {
        if (!Net().Online()) return g_ours.clear();
        if (!g_ours.empty()) {
            Writer w(proto::G_BULLET);
            w.U8((uint8_t)g_ours.size());
            for (const Bullet& b : g_ours) w.Vec(b.from), w.Vec(b.to), w.U8(b.weapon);
            Net().Send(proto::CH_STATE, w);
            g_ours.clear();
        }
        uint64_t now = NowMs();
        for (auto it = g_theirs.begin(); it != g_theirs.end();) {
            if (it->at > now) {
                ++it;
                continue;
            }
            RemotePlayer* rp = Players::Instance().Get(it->player);
            auto fired = g_copyFired.find(it->player);
            bool own = fired != g_copyFired.end() && now - fired->second < kOwnShotMs + kWaitMs;
            if (rp && core::IsPlayerReady() && !own) Draw(*rp, it->b);
            it = g_theirs.erase(it);
        }
    }

    static Shots& Instance() {
        static Shots s;
        return s;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Shots::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
