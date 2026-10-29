// Population control for co-op:
//  * near the host only the host generates random peds, traffic and parked cars (the partner sees the
//    host's population through Peds/Vehicles sync), unless the partner is wanted (police chases);
//  * entities owned by this game are not removed while the other player is close to them
//    (CPopulation::ManagePed / CCarCtrl::PossiblyRemoveVehicle measure distance to the nearest player);
//  * farther from this game's player, an ordinary ped or car the other player is nearer to goes to the other game
//    instead of being dropped here (it vanished in front of the other player).
#include <algorithm>
#include <cmath>

#include "features/population/Population.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

// partner closer than this to the host: the host's population (spawned 40-55 m around the host) covers both
constexpr float kShareRadius = 60.0f;
constexpr float kShareRadiusOff = 90.0f;  // hysteresis

bool g_suppress = false;
// in an interior only the game of the player who came in first makes its people: the second one filled the attractors
// the copies stand on (copies use none) before they were even spawned, and the host's generation never stopped
bool g_guest = false;
// partner: the host's game made the people of this interior (G_INTERIOR_POP)
int8_t g_hostPopArea = 0;
CVector g_hostPopAt;
uint64_t g_hostPopMs = 0;

bool OtherPlayerInside(const CVector& at);

int LocalWantedLevel() {
    void* wanted = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return wanted ? Field<int32_t>(wanted, 0x2C) : 0;
}

void UpdateSuppression() {
    bool want = false;
    // whose people and cars cover two players together: the host's, and while a joint side activity runs, its leader's.
    // A taxi job's fares, a pimp's clients and a vigilante's targets are the leading game's own people and cars: led by
    // the partner near the host, the job had none (#2)
    uint8_t source = SideLeading() ? Net().MyId() : SideLeader() != 0xFF ? SideLeader() : Net().HostId();
    if (Net().Online() && source != Net().MyId() && core::IsPlayerReady()) {
        CPlayerPed* me = FindPlayerPed(0);
        if (const RemotePlayer* src = Players::Instance().Get(source); src && src->hasState) {
            float d = (src->state.pos - me->GetPosition()).Length2D();
            bool sameArea = src->state.area == me->AreaCode();
            float limit = g_suppress ? kShareRadiusOff : kShareRadius;
            want = sameArea && d < limit && LocalWantedLevel() == 0;
        }
    }
    if (Net().Online() && core::IsPlayerReady()) {
        CPlayerPed* me = FindPlayerPed(0);
        if (me->AreaCode() == 0) g_guest = false;
        else want = want || (g_guest && OtherPlayerInside(me->GetPosition()));
        // indoors during the host's mission: its interiors hold who its script put there. The game fills a house's chairs
        // and television (its attractors) by the density, and "Home Invasion" puts it back to 1 while the host is out:
        // the partner alone in the house had people standing about
        if (!Net().IsHost() && me->AreaCode() != 0 && HostMissionRecent(0)) want = true;
    }
    if (want != g_suppress) LOGI("Population generation %s (player %d's)", want ? "handed over" : "local", want ? source : Net().MyId());
    // The engine adds one ped a frame at the edge of view: the empty streets around would fill slowly. The countdowns of
    // a game's start (and of a respawn) fill them at once, but they place people 10 m away in view and park cars next to
    // the player ("NPCs and cars pop up in front of me"): only behind a dark screen.
    // Not indoors: the countdown filled the house of "Home Invasion" with people behind its fades.
    if (g_suppress && !want && Ref<float>(addr::TheCamera + 0xBFC) > 200.0f &&  // CCamera's fade alpha
        core::IsPlayerReady() && FindPlayerPed(0)->AreaCode() == 0) {
        Ref<uint8_t>(0xC0EC23) = 2;  // CPopulation::m_CountDownToPedsAtStart
        Ref<uint8_t>(0x9690C0) = 2;  // CCarCtrl::CountDownToCarsAtStart
    }
    g_suppress = want;
}

using UpdatePopFn = void(__cdecl*)(bool);
UpdatePopFn g_origPopUpdate = nullptr;
void __cdecl HookPopulationUpdate(bool generate) { g_origPopUpdate(generate && !g_suppress); }

using VoidFn = void(__cdecl*)();
VoidFn g_origGenerateRandomCars = nullptr;
void __cdecl HookGenerateRandomCars() {
    if (g_suppress) return;
    g_origGenerateRandomCars();
}

VoidFn g_origCarGenerators = nullptr;
void __cdecl HookCarGeneratorsProcess() {
    if (g_suppress) return;
    g_origCarGenerators();
}

// the other player is in the game (states keep coming): what it is given goes nowhere while it loads a world
bool CanTake(uint8_t who) {
    const RemotePlayer* rp = Players::Instance().Get(who);
    return rp && rp->ped && NowMs() - rp->stateRecvMs < 1500;
}

// nearest remote player position (same interior) within maxDist, or nullptr
const CVector* NearestRemote(const CVector& p, float maxDist, int8_t area, uint8_t* who = nullptr) {
    const CVector* best = nullptr;
    float bestD = maxDist;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != area) continue;
        float d = (rp.state.pos - p).Length2D();
        if (d < bestD) {
            bestD = d;
            best = &rp.state.pos;
            if (who) *who = id;
        }
    }
    return best;
}

// The other player looks at p: within 60 m of its camera and ~60 degrees of where it looks (its state carries the
// camera). The engine's culling knows only this game's camera: off it and over 25 m a ped went in 4 s, in front of the
// other player ("they vanish when my partner turns around").
bool RemoteSees(const CVector& p, int8_t area, float within = 60.0f) {
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != area || NowMs() - rp.stateRecvMs > 1500) continue;
        CVector d = p - (rp.state.pos + rp.state.cam.sourceOffset);
        const CVector& f = rp.state.cam.front;
        float len = d.Length();
        if (len < within && d.x * f.x + d.y * f.y + d.z * f.z > len * 0.5f) return true;
    }
    return false;
}

using ManagePedFn = void(__cdecl*)(CPed*, const CVector*);
ManagePedFn g_origManagePed = nullptr;
void __cdecl HookManagePed(CPed* ped, const CVector* centre) {
    // a script let it go to fade out (REMOVE_CHAR_ELEGANTLY: CPed::bFadeOut, +0x470 bit 3): kept or handed over, a
    // failed mission's Catalina stood twice at the restart
    if (Net().Online() && ped && centre && !(ped->PedFlags2() & 8)) {
        CVector p = ped->GetPosition();
        float local = (p - *centre).Length2D();
        int8_t area = AreaOf(ped);
        uint8_t who = 0;
        const CVector* r = NearestRemote(p, local, area, &who);
        // far from this game's player and clearly nearer the other one: the other game keeps it (the margin keeps a ped
        // from going back and forth between two players standing together). Past this game's ground (collision loads
        // ~120 m around its player) it goes even in view here: it would fall through the map
        if (r && local > 60.0f && (p - *r).Length2D() < local - 30.0f && CanTake(who) &&
            Peds::Instance().QueueGiveAway(ped, who, local >= 100.0f))
            return;
        // the other player still needs it (next to it, or in its view) while this game has ground under it
        if (local < 100.0f && ped->Health() > 0.0f && ((r && (p - *r).Length2D() < 50.0f) || RemoteSees(p, area))) return;
        if (r) {
            g_origManagePed(ped, r);
            return;
        }
    }
    g_origManagePed(ped, centre);
}

// Through a door this game deletes all its passers-by (CEntryExit::TransitionFinished -> CPopulation::
// RemoveAllRandomPeds): the street around the other player emptied whenever this player entered a shop, and the
// interior's people vanished for the one who stayed inside when the other walked out. Those near the other player go
// to its game first.
VoidFn g_origRemoveAllRandom = nullptr;
void __cdecl HookRemoveAllRandomPeds() {
    CPoolHeader* pool = PedPool();
    for (int i = 0; Net().Online() && core::IsPlayerReady() && pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        NetPed* np = Peds::Instance().Find(ped);
        if (!np || np->owner != Net().MyId() || !Peds::CanGiveAwayType(*np) || ped->Health() <= 0.0f ||
            ped->CreatedBy() != 1 /* PED_GAME */ || ped->InVehicle() || (ped->PedFlags2() & 8) ||  // fading out
            !CallMethod<0x5DF100, bool>(ped))  // CPed::CanBeDeleted
            continue;
        // our player is in the new area already while it still stands at the door: a street ped is the street's
        int8_t area = ped->GetPosition().z > 900.0f ? AreaOf(ped) : ped->AreaCode();
        uint8_t who = 0;
        if (NearestRemote(ped->GetPosition(), 60.0f, area, &who) && CanTake(who)) Peds::Instance().GiveAway(np->netId, who);
    }
    g_origRemoveAllRandom();
}

uint64_t g_carGivenMs = 0;

using PossiblyRemoveFn = void(__cdecl*)(CVehicle*);
PossiblyRemoveFn g_origPossiblyRemove = nullptr;
void __cdecl HookPossiblyRemoveVehicle(CVehicle* v) {
    // kept for the other player while this game has ground under it (see HookManagePed), then given to it; a wreck
    // goes as usual (kept, they filled the vehicle pool around two players)
    uint8_t who = 0;
    if (Net().Online() && v && v->Status() != 5 /* STATUS_WRECKED */)
        if (const CVector* r = NearestRemote(v->GetPosition(), 140.0f, v->AreaCode(), &who)) {
            CVector me = FindPlayerPed(0)->GetPosition();
            float local = (v->GetPosition() - me).Length2D();
            if (local < 100.0f) return;
            // The game makes its traffic around its camera: one far from the player (a cutscene, the host's scene
            // watched through the host's camera) made cars "far" at once, which went to the other game and were made
            // again, 299 in 4 minutes into the host's full pool (#6). Given only then: our camera near our player, the
            // other player looking at it, one a second.
            if ((cam::Source() - me).Length() < 50.0f && (v->GetPosition() - *r).Length2D() < local - 40.0f &&
                RemoteSees(v->GetPosition(), v->AreaCode(), 140.0f) && CanTake(who)) {
                if (NowMs() - g_carGivenMs < 1000) return;  // kept till its turn
                if (Vehicles::Instance().QueueGiveAway(v, who)) {
                    g_carGivenMs = NowMs();
                    return;
                }
            }
        }
    g_origPossiblyRemove(v);
}

// The people of an interior (the entrance's: a police station's officers, a shop's customers; a furnished house's) come
// in the game of the player who is there first, and the other player sees that game's (Peds): each game had its own,
// and none of the other's.
// `at`: where in the interior (one area holds several interiors far apart)
bool OtherPlayerInside(const CVector& at) {
    int area = Ref<int32_t>(0xB72914);  // CGame::currArea: the interior this game's player is in (or just came into)
    if (!Net().Online() || !core::IsPlayerReady() || area == 0) return false;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.state.area == area && (rp.state.pos - at).Length() < 80.0f) return true;
    return false;
}

bool HostPopulated(int8_t area, const CVector& at) {
    return !Net().IsHost() && NowMs() - g_hostPopMs < 5000 && g_hostPopArea == area && (g_hostPopAt - at).Length() < 80.0f;
}

// A house's people (InteriorGroup_c::SetupPeds, its family about the furniture) during the host's mission: none here.
using SetupPedsFn = void(__fastcall*)(void*, void*);
SetupPedsFn g_origSetupPeds = nullptr;
void __fastcall HookSetupPeds(void* group, void* edx) {
    if (Net().Online() && !Net().IsHost() && HostMissionRecent(0)) return;
    g_origSetupPeds(group, edx);
}

using PopulateInteriorFn = void(__cdecl*)(int, CVector);
PopulateInteriorFn g_origPopulateInterior = nullptr;
void __cdecl HookPopulateInterior(int count, CVector at) {  // CEntryExit::GenerateAmbientPeds, on coming in
    int8_t area = (int8_t)Ref<int32_t>(0xB72914);  // CGame::currArea
    // our player is still at the door outside, `at` is in the interior
    g_guest = OtherPlayerInside(at) || HostPopulated(area, at);
    if (g_guest) {
        LOGI("The interior's people are the other game's (%d not made here)", count);
        return;
    }
    // the host's mission runs: its interiors hold who its script put there ("Home Invasion"'s house, through the door the
    // mission opens, had a family walking about in the partner's game only)
    if (!Net().IsHost() && Net().Online() && HostMissionRecent(0)) {
        LOGI("The host's mission runs: no people of our own in this interior (%d not made)", count);
        return;
    }
    g_origPopulateInterior(count, at);
    if (Net().Online() && Net().IsHost()) {  // two through the door at once: each saw the other still outside
        Writer w(proto::G_INTERIOR_POP);
        w.I8(area);
        w.Vec({at.x, at.y, at.z});
        Net().Send(proto::CH_RELIABLE, w);
    }
}

// CPopulation::Update keeps the interior's attractors filled (Ammu-Nation's customers) by this game's own count: the
// other game's people there are copies on no attractor, so a player who came in second put a second one on each
using AttractorPedsFn = int(__cdecl*)(CVector, float, float, float, float, int, int);
AttractorPedsFn g_origAttractorPeds = nullptr;
int __cdecl HookAttractorPeds(CVector at, float r1, float r2, float r3, float r4, int dm, int count) {
    return OtherPlayerInside(at) ? 0 : g_origAttractorPeds(at, r1, r2, r3, r4, dm, count);
}

// We came in together with the host, each before the other's state said so: the host's people stay, ours go
void HandleInteriorPop(uint8_t from, Reader& r) {
    int8_t area = r.I8();
    Vec3 v = r.Vec();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    CVector at{v.x, v.y, v.z};
    g_hostPopArea = area, g_hostPopAt = at, g_hostPopMs = NowMs();
    CPlayerPed* me = FindPlayerPed(0);
    if (g_guest || area == 0 || me->AreaCode() != area || (me->GetPosition() - at).Length() > 80.0f) return;
    int n = 0;
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        NetPed* np = Peds::Instance().Find(ped);
        if (ped->IsPlayer() || ped->CreatedBy() != 1 /* PED_GAME */ || ped->InVehicle() || (np && np->owner != Net().MyId()) ||
            (ped->GetPosition() - at).Length() > 80.0f || !CallMethod<0x5DF100, bool>(ped))  // CPed::CanBeDeleted
            continue;
        Call<0x610F20>(ped);  // CPopulation::RemovePed (the host's copies of ours go as they vanish: Peds)
        ++n;
    }
    g_guest = true;
    LOGI("Came into interior %d with the host: its game's people stay, ours (%d) go", area, n);
}

using SetupPedsFn = void(__fastcall*)(void*, void*);
SetupPedsFn g_origSetupInteriorPeds = nullptr;
void __fastcall HookSetupInteriorPeds(void* group, void* edx) {  // InteriorGroup_c::Update asks again every frame
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !OtherPlayerInside(me->GetPosition())) g_origSetupInteriorPeds(group, edx);
}

// This game judges a new person's or car's spot by its own camera alone: the other player nearby saw people appear
// 5-45 m in front of it and cars just ahead (their copies). A spot in its view waits for the next try, as one in ours.
int g_skipped[2] = {};  // spots skipped for the other player's view: people, cars (logged every 10 s)
uint64_t g_skippedMs = 0;

bool SeenByOther(const CVector& p, float within) {
    if (!Net().Online() || !core::IsPlayerReady() || !RemoteSees(p, FindPlayerPed(0)->AreaCode(), within)) return false;
    ++g_skipped[within > 50.0f ? 1 : 0];
    if (NowMs() - g_skippedMs > 10000) {
        LOGD("Spots in the other player's view skipped: %d for people, %d for cars", g_skipped[0], g_skipped[1]);
        g_skipped[0] = g_skipped[1] = 0;
        g_skippedMs = NowMs();
    }
    return true;
}

using ClearForPedFn = bool(__cdecl*)(const CVector&, float, int, CEntity**, bool, bool, bool);
ClearForPedFn g_origClearForPed = nullptr;
bool __cdecl HookClearForPed(const CVector& p, float r, int n, CEntity** out, bool cars, bool peds, bool objects) {
    return !SeenByOther(p, 42.5f) && g_origClearForPed(p, r, n, out, cars, peds, objects);
}

using CarCoorsFn = bool(__cdecl*)(CVector, float, float, float, bool, float, float, CVector*, void*, void*, float*, bool, bool);
CarCoorsFn g_origCarCoors = nullptr;
bool __cdecl HookCarCoors(CVector at, float r, float dx, float dy, bool anyDir, float a6, float a7, CVector* out, void* node1,
                          void* node2, float* dist, bool invert, bool room) {
    return g_origCarCoors(at, r, dx, dy, anyDir, a6, a7, out, node1, node2, dist, invert, room) && !SeenByOther(*out, 90.0f);
}

class Population : public Module {
public:
    const char* Name() const override { return "Population"; }
    void OnInstallHooks() override {
        mem::RedirectCallFn(0x59690F, &HookSetupInteriorPeds, &g_origSetupInteriorPeds);  // InteriorGroup_c::Update
        mem::RedirectCallFn(0x43E983, &HookPopulateInterior, &g_origPopulateInterior);    // CEntryExit::GenerateAmbientPeds
        mem::RedirectCallFn(0x59690F, &HookSetupPeds, &g_origSetupPeds);                // InteriorGroup_c::Update
        mem::RedirectCallFn(0x4407B7, &HookRemoveAllRandomPeds, &g_origRemoveAllRandom);  // CEntryExit::TransitionFinished
        mem::RedirectCallFn(0x616837, &HookAttractorPeds, &g_origAttractorPeds);          // CPopulation::Update
        // CPedPlacement::IsPositionClearForPed in AddToPopulation where "not clear" gives up (its other two calls use
        // the entities it lists, or go on when it says no), CCarCtrl::GenerateCarCreationCoors2 in GenerateOneRandomCar
        for (uintptr_t at : {0x614C8F, 0x6151FA}) mem::RedirectCallFn(at, &HookClearForPed, &g_origClearForPed);
        mem::RedirectCallFn(0x430455, &HookCarCoors, &g_origCarCoors);
        // and the same spot search where the police, an ambulance or a fire engine come out (GenerateOneEmergencyServicesCar)
        // and at 0x444B37: they came out right in front of the other player ("out of nowhere", 27.10)
        for (uintptr_t at : {0x42B86A, 0x444B37}) mem::RedirectCallFn(at, &HookCarCoors, &g_origCarCoors);
        Net().SetHandler(proto::G_INTERIOR_POP, &HandleInteriorPop);
        mem::HookFn(0x616650, &HookPopulationUpdate, &g_origPopUpdate, "CPopulation::Update");
        mem::HookFn(0x4341C0, &HookGenerateRandomCars, &g_origGenerateRandomCars, "CCarCtrl::GenerateRandomCars");
        mem::HookFn(0x6F3F40, &HookCarGeneratorsProcess, &g_origCarGenerators, "CTheCarGenerators::Process");
        mem::HookFn(0x611FC0, &HookManagePed, &g_origManagePed, "CPopulation::ManagePed");
        mem::HookFn(0x424F80, &HookPossiblyRemoveVehicle, &g_origPossiblyRemove, "CCarCtrl::PossiblyRemoveVehicle");
    }
    void OnPreProcess() override { UpdateSuppression(); }
    void OnSessionEnd() override { g_suppress = g_guest = false; }
};

Population g_pop;
struct Reg {
    Reg() { core::RegisterModule(&g_pop); }
} g_reg;
}  // namespace

bool PopulationSuppressed() { return g_suppress; }

}  // namespace coop
