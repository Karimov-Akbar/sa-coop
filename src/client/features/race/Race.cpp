// Races together. A player who steps into a race's marker holds its start for 15 s, and the other players who come and
// stand at the marker meanwhile race with it. The race runs in the game of the one who stepped in first (the owner):
// its mission (CPRACE) has its own racers, its route and its CJ, and knows nobody else. The others (the riders) get a
// car of the race's model in the grid place of one of its racers in their own game, the route, the laps and the countdown; each
// rider's game counts its own checkpoints as the mission counts the owner's, and the owner's game ranks the players.
// The first of the players wins the race's prize. The race's own racers are driven by its mission: when it ends in the
// owner's game (the owner finished or dropped out) they would only cruise about the track, so they are taken off it
// while players still race.
//
// The races: 8-Track (the Los Santos Forum, HOTR's marker, race 25 of CPRACE).
//
// CPRACE keeps its state in the mission's locals (CTheScripts::LocalVariablesForCurrentMission), read here as it runs:
//   49@ stage (7 the grid is set, 8..10 the countdown 3, 2, 1, 11 racing, 13 the player finished), 221@ area,
//   224@ laps, 250@ checkpoints in a lap, 205@ checkpoint type, 325@ its size, 322@..324@ the box a racer passes it in,
//   256@ racers, 442@ the player's racer, 229@ racers ahead of the player at the finish; per racer (16):
//   257@ its next checkpoint, 273@ laps done, 331@/347@/363@ the next checkpoint, 379@ car, 716@ model, colours
//   209@/215@ (6); the route (82): 454@ x, 536@ y, 618@ z.
#include "features/race/Race.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

// ---------------------------------------------------------------- the races
struct RaceKind {
    const char* thread;  // the main.scm thread whose marker starts it
    int index;           // $1408, CPRACE's race
    uint32_t marker;     // the marker's position in the script space (3 floats)
    const char* name;
    int prize;           // what the race pays its winner (CPRACE's ADD_SCORE)
    int stat;            // the skill its marker asks for (above 199), -1 none
    float exitX, exitY, exitZ, exitHeading;  // where the race mission puts its CJ back after an indoor race
    int laps;            // laps of the race (the mission's own count was too long), alone or together
};
const RaceKind kRaces[] = {
    {"hotr", 25, 6992, "8-Track", 10000, 160, 2693.184f, -1699.655f, 9.389f, 34.299f, 8},  // driving skill; 12 laps alone
};
const RaceKind* KindByThread(const char* thread) {
    for (const RaceKind& k : kRaces)
        if (!strcmp(thread, k.thread)) return &k;
    return nullptr;
}
const RaceKind* KindByIndex(int index) {
    for (const RaceKind& k : kRaces)
        if (k.index == index) return &k;
    return nullptr;
}

constexpr uint64_t kGatherMs = 15000;   // how long the marker waits for the others
constexpr uint64_t kReadyWaitMs = 8000; // the countdown waits that long at most for the riders' cars
constexpr uint64_t kAfterFirstMs = 180000;  // the others' time to finish after the first player did
constexpr uint64_t kRaceMaxMs = 900000;     // a race together ends after this anyway
constexpr uint64_t kOwnerSilentMs = 10000;  // a rider who hears nothing of the owner that long leaves
constexpr float kJoinRange = 4.0f;      // a rider joins standing this close to the marker (one can not stand on another)
constexpr int kRouteMax = 82;
constexpr float kHealthFactor = 2.0f;   // the race cars of a race together take this much more (they broke too soon)

// ---------------------------------------------------------------- the engine
constexpr uintptr_t kActiveScripts = 0xA8B42C;  // CTheScripts::pActiveScripts
constexpr uintptr_t kMissionLocals = 0xA48960;  // CTheScripts::LocalVariablesForCurrentMission[1024]
int32_t& Local(int i) { return Ref<int32_t>(kMissionLocals + (uintptr_t)i * 4); }
float& LocalF(int i) { return Ref<float>(kMissionLocals + (uintptr_t)i * 4); }
int32_t& Global(uint32_t offset) { return Ref<int32_t>(addr::ScriptSpace + offset); }
CVector GlobalVec(uint32_t offset) { return Ref<CVector>(addr::ScriptSpace + offset); }

enum : int {
    L_STAGE = 49, L_CP_TYPE = 205, L_AREA = 221, L_LAPS = 224, L_AHEAD = 229, L_CPS = 250, L_LAST_LAP = 251, L_RACERS = 256,
    L_BOX = 322, L_CP_SIZE = 325, L_ME = 442,
    A_NEXT = 257, A_LAP = 273, A_NEXT_X = 331, A_NEXT_Y = 347, A_CAR = 379, A_DRIVER = 395, A_MODEL = 716, A_COL1 = 209,
    A_COL2 = 215,
    A_ROUTE_X = 454, A_ROUTE_Y = 536, A_ROUTE_Z = 618,
};
constexpr uint32_t kRaceIndex = 1408;               // $1408: the race CPRACE runs
constexpr uint32_t kTimer = 64, kCountdownAt = 27840;  // $64 the game's time, $27840: the countdown's next step
constexpr uint32_t kGridHeading = 288;               // $288: the heading its grid's cars are set to

void* RaceScript() {
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00))
        if (Field<uint8_t>(s, 0xDC) && !strncmp(Field<char[8]>(s, 0x08), "cprace", 8)) return s;  // m_bIsMission
    return nullptr;
}

float DrivingStat(int stat) { return Call<0x558E40, float>((uint16_t)stat); }  // CStats::GetStatValue

void BigText(const char* text, uint32_t ms) { Call<0x69F2B0>(text, ms, (uint16_t)4); }  // CMessages::AddBigMessage
void Sound(int id) { Cmd(0x097A).F(0).F(0).F(0).I(id).Run(); }  // REPORT_MISSION_AUDIO_EVENT_AT_POSITION

// ---------------------------------------------------------------- the network
enum : uint8_t {
    OP_LOBBY = 1,      // owner -> all: u8 race, vec marker, u32 ms left
    OP_CANCEL = 2,     // owner -> all
    OP_JOIN = 3,       // rider -> all: u8 on
    OP_START = 4,      // owner -> all: the race and a grid spot per rider (see SendStart)
    OP_READY = 5,      // rider -> all: in its car on the grid
    OP_COUNT = 6,      // owner -> all: u8 3, 2, 1, 0 (go)
    OP_PROGRESS = 7,   // rider -> all: u8 lap, u8 next checkpoint, f32 distance to it
    OP_FINISH = 8,     // rider -> all: u8 out (died, gave up), u32 time ms
    OP_STANDINGS = 9,  // owner -> all: u8 n, per player u8 id, u8 state (0 racing, 1 finished, 2 out), u32 time
    OP_END = 10,       // owner -> all: u8 winner (255 none), u32 prize the owner's game pays it (0: its race paid)
    OP_BOTS = 11,      // owner -> all: the race mission ended, its racers go: u8 n, u16 car ids; u8 n, u16 driver ids
};

Writer Msg(uint8_t op) {
    Writer w(proto::G_RACE);
    w.U8(op);
    return w;
}

// ---------------------------------------------------------------- state
struct Lobby {
    bool open = false;
    uint8_t owner = 0;
    const RaceKind* kind = nullptr;
    CVector marker;
    uint64_t endsMs = 0;
};
Lobby g_lobby;

struct Racer {  // owner: a player in the race, itself included
    uint8_t lap = 0, next = 0;
    float dist = 0;
    uint8_t state = 0;  // 0 racing, 1 finished, 2 out
    uint32_t timeMs = 0;
    bool ready = false;
};

struct Spot {
    CVector pos;
    float heading;  // degrees
};

// owner
uint64_t g_askedMs = 0;              // its marker's thread asked to start this recently
std::set<uint8_t> g_riders;          // who stands at the marker
bool g_launching = false;            // the race mission was let start, with riders
bool g_startSent = false, g_holding = false;
uint64_t g_launchMs = 0, g_startMs = 0, g_goMs = 0, g_firstDoneMs = 0, g_standingsMs = 0;
int g_stage = -1;
bool g_ownerPassed = false;          // its race paid it (it beat the race's own racers too)
std::map<uint8_t, Racer> g_racers;
std::vector<uint16_t> g_botCars, g_botDrivers;  // the race's own racers, by network id, while its mission runs
bool g_botsSent = false;
std::vector<uint8_t> g_startRiders;  // the riders and their places, chosen a moment before the start goes out
std::vector<Spot> g_spots;
uint64_t g_preparedMs = 0;
int g_carHealth = 1000;               // the race cars' health, the riders' too

// rider
bool g_joined = false;
struct Ride {
    bool on = false;
    uint8_t owner = 0;
    const RaceKind* kind = nullptr;
    int area = 0, laps = 0, cps = 0, cpType = 0;
    float cpSize = 6.0f;
    CVector box;
    std::vector<CVector> route;
    int car = -1, checkpoint = -1, blip = -1;
    int lap = 0, next = 0, health = 1000;
    bool go = false, done = false;
    uint64_t goMs = 0, sentMs = 0, endMs = 0, heardMs = 0;
} g_ride;

// both: the race's racers to delete, whichever game has them by now (they go to the nearest player's game), until then
std::map<uint16_t, uint64_t> g_doomedCars, g_doomedPeds;  // id -> give up at

// both: the order of the players, the owner's game says it
std::vector<std::pair<uint8_t, uint8_t>> g_standings;  // id, state

std::string Nick(uint8_t id) { return id == Net().MyId() ? Cfg().nickname : Net().PeerName(id); }

int SecondsLeft() { return g_lobby.endsMs > NowMs() ? (int)((g_lobby.endsMs - NowMs() + 999) / 1000) : 0; }

std::string TimeText(uint32_t ms) { return Format("%u:%02u.%u", ms / 60000, ms / 1000 % 60, ms / 100 % 10); }

// ---------------------------------------------------------------- owner: the lobby
void CancelLobby(const char* why) {
    if (!g_lobby.open || g_lobby.owner != Net().MyId()) return;
    LOGI("Race lobby closed: %s", why);
    g_lobby = {};
    g_riders.clear();
    Net().Send(proto::CH_RELIABLE, Msg(OP_CANCEL));
}

void OpenLobby(const RaceKind& kind) {
    g_lobby = {true, Net().MyId(), &kind, GlobalVec(kind.marker), NowMs() + kGatherMs};
    g_riders.clear();
    Writer w = Msg(OP_LOBBY);
    w.U8((uint8_t)kind.index);
    w.Vec({g_lobby.marker.x, g_lobby.marker.y, g_lobby.marker.z});
    w.U32((uint32_t)kGatherMs);
    Net().Send(proto::CH_RELIABLE, w);
    ui::TheChat().AddSystem(Format(TR("Гонка %s: ждём других игроков %d с. Кто встанет в маркер, поедет с вами.",
                                      "%s: waiting %d s for the others. Whoever stands at the marker races with you."),
                                   kind.name, (int)(kGatherMs / 1000)));
    LOGI("Race lobby opened: %s", kind.name);
}

// every player of the session stands at the marker: no need to wait out the time
bool EverybodyJoined() {
    for (auto& [id, peer] : Net().Peers())
        if (!g_riders.count(id)) return false;
    return true;
}

void ResetOwner() {
    g_botCars.clear();
    g_botDrivers.clear();
    g_launching = g_startSent = g_holding = g_ownerPassed = g_botsSent = false;
    g_startRiders.clear();
    g_spots.clear();
    g_preparedMs = 0;
    g_carHealth = 1000;
    g_launchMs = g_startMs = g_goMs = g_firstDoneMs = 0;
    g_stage = -1;
    g_racers.clear();
    g_riders.clear();
}

// ---------------------------------------------------------------- owner: the race
CVector Flat(const CVector& v) { return CVector{v.x, v.y, 0}.Normalized(); }

// The riders' places: in pairs behind the grid's back row, along the road. The road behind the grid is the end of a
// lap, so it runs through the route's last checkpoints (on 8-Track it bends to the right there; a straight line
// behind the grid ran into the stadium's wall). Each place goes back along the road from the last car, half a metre at
// a time, until a car there touches none of the race's cars and none of the places before it: a rider's car made inside
// another one stuck to it and the two threw each other into the air.
std::vector<Spot> GridSpots(size_t n, int cps, int model, const std::vector<CVector>& given) {
    int me = Local(L_ME), racers = std::clamp(Local(L_RACERS), 1, 16);
    std::vector<Spot> out;
    CVehicle* mine = GetVehicle(Local(A_CAR + me));
    if (!mine) return out;
    // the way the grid faces: the race mission sets every car of its grid, its CJ's too, to the heading in $288
    // (SET_CAR_HEADING ... $288)
    float gridHeading = Ref<float>(addr::ScriptSpace + kGridHeading);
    CVector fwd{-std::sin(gridHeading / 57.29578f), std::cos(gridHeading / 57.29578f), 0};
    CVector back = mine->GetPosition();
    LOGI("Grid: heading %.0f, route %.1f %.1f -> %.1f %.1f, the owner's car at %.1f %.1f %.1f facing %.0f", gridHeading,
         LocalF(A_ROUTE_X), LocalF(A_ROUTE_Y), LocalF(A_ROUTE_X + 1), LocalF(A_ROUTE_Y + 1), back.x, back.y, back.z,
         mine->GetHeading() * 57.29578f);
    for (int i = 0; i < racers; ++i)
        if (CVehicle* v = GetVehicle(Local(A_CAR + i)); v && (v->GetPosition() - back).Dot(fwd) < 0) back = v->GetPosition();
    for (const CVector& p : given)
        if ((p - back).Dot(fwd) < 0) back = p;
    LOGI("Grid: %d racers, the back one at %.1f %.1f", racers, back.x, back.y);
    // the road backwards: the back car, then the route's checkpoints from the last one, each farther behind
    std::vector<CVector> road{back};
    for (int i = cps - 1, taken = 0; i >= 1 && taken < 6; --i) {
        CVector p{LocalF(A_ROUTE_X + i), LocalF(A_ROUTE_Y + i), LocalF(A_ROUTE_Z + i)};
        if ((p - back).Dot(fwd) < (road.back() - back).Dot(fwd) - 2.0f) road.push_back(p), ++taken;
    }
    if (road.size() < 2) road.push_back(back - fwd * 100.0f);
    float length = 5.0f;  // the car's
    float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    if (Cmd(0x07E4).I(model).Out(&x0).Out(&y0).Out(&z0).Out(&x1).Out(&y1).Out(&z1).Run(), y1 - y0 > 1.0f) length = y1 - y0;  // GET_MODEL_DIMENSIONS
    float width = x1 - x0 > 1.0f ? x1 - x0 : 2.2f;
    std::vector<CVector> taken;  // the race's cars, then the places given
    for (int i = 0; i < racers; ++i)
        if (CVehicle* v = GetVehicle(Local(A_CAR + i))) taken.push_back(v->GetPosition());
    taken.insert(taken.end(), given.begin(), given.end());
    // the place `along` metres back along the road, `side` metres to the right of a car facing the race. Near the grid
    // a car faces as the grid's cars do, farther back it turns with the road: the line to the lap's last checkpoint cuts
    // across the bend and a car set along it stood askew on the track.
    auto place = [&](float along, float side, CVector& carFwd) {
        float t = std::clamp((along - 8.0f) / 20.0f, 0.0f, 1.0f), back0 = along;  // along is used up below
        size_t seg = 0;
        while (seg + 2 < road.size() && along > (road[seg + 1] - road[seg]).Length2D()) along -= (road[seg + 1] - road[seg]).Length2D(), ++seg;
        CVector a = road[seg], b = road[seg + 1], dir = Flat(b - a);  // backwards
        CVector p = a + dir * along;
        p.z = a.z + (b.z - a.z) * std::clamp(along / std::max(0.1f, (b - a).Length2D()), 0.0f, 1.0f);
        p = (back - fwd * back0) * (1.0f - t) + p * t;  // straight behind the grid first
        carFwd = Flat(fwd * (1.0f - t) + dir * -t);
        return p + CVector{carFwd.y, -carFwd.x, 0} * side;
    };
    auto clear = [&](const CVector& p, const CVector& carFwd) {
        CVector right{carFwd.y, -carFwd.x, 0};
        for (const CVector& q : taken) {  // two cars facing about the same way: their boxes, with some room
            CVector d = q - p;
            if (std::fabs(d.Dot(carFwd)) < length + 0.8f && std::fabs(d.Dot(right)) < width + 0.6f) return false;
        }
        return true;
    };
    float along = 3.5f;
    for (size_t k = 0; k < n; ++k) {
        float side = (k % 2) ? 2.5f : -2.5f;
        CVector fwdHere, p = place(along, side, fwdHere);
        for (int tries = 0; tries < 80 && !clear(p, fwdHere); ++tries) along += 0.5f, p = place(along, side, fwdHere);
        taken.push_back(p);
        out.push_back({p, std::atan2(-fwdHere.x, fwdHere.y) * 57.29578f});
        if (k % 2) along += length + 1.0f;  // the next pair
    }
    return out;
}

// The riders take the grid places of the race's own racers from the back, one each: a place on the grid is on the
// track and faces the race, behind the grid 8-Track bends into a tunnel. Those racers go before the start is sent, so
// that their copies are gone in the riders' games when the riders' cars are made there. Riders beyond the racers stand
// behind the grid.
void PrepareStart() {
    int me = Local(L_ME), racers = std::clamp(Local(L_RACERS), 1, 16), cps = std::clamp(Local(L_CPS), 2, kRouteMax);
    CVehicle* mine = GetVehicle(Local(A_CAR + me));
    if (!mine) return;
    g_startRiders.clear();
    for (uint8_t id : g_riders)
        if (Net().Peers().count(id)) g_startRiders.push_back(id);
    // the race's cars, its CJ's included, take twice as much (the mission gives only its racers 2200 on 8-Track, its
    // CJ's car keeps 1000)
    for (int i = 0; i < racers; ++i)
        if (CVehicle* v = GetVehicle(Local(A_CAR + i))) {
            int health = (int)(std::max(v->Health(), 1000.0f) * kHealthFactor);
            Cmd(0x0224).I(Local(A_CAR + i)).I(health).Run();  // SET_CAR_HEALTH
            if (i == me) g_carHealth = health;
        }
    float gh = Ref<float>(addr::ScriptSpace + kGridHeading) / 57.29578f;
    CVector fwd{-std::sin(gh), std::cos(gh), 0};
    std::vector<std::pair<float, int>> bots;  // how far ahead, racer
    for (int i = 0; i < racers; ++i)
        if (CVehicle* v = GetVehicle(Local(A_CAR + i)); v && i != me && GetPed(Local(A_DRIVER + i)))
            bots.push_back({(v->GetPosition() - mine->GetPosition()).Dot(fwd), i});
    std::sort(bots.begin(), bots.end());
    g_spots.clear();
    std::vector<CVector> given;
    for (size_t k = 0; k < g_startRiders.size() && k < bots.size(); ++k) {
        int i = bots[k].second;
        CVehicle* v = GetVehicle(Local(A_CAR + i));
        g_spots.push_back({v->GetPosition(), v->GetHeading() * 57.29578f});
        given.push_back(v->GetPosition());
        Cmd(0x009B).I(Local(A_DRIVER + i)).Run();  // DELETE_CHAR
        Cmd(0x00A6).I(Local(A_CAR + i)).Run();     // DELETE_CAR
        LOGI("Racer %d's grid place goes to player %d", i, g_startRiders[k]);
    }
    if (g_spots.size() < g_startRiders.size()) {
        std::vector<Spot> more = GridSpots(g_startRiders.size() - g_spots.size(), cps, Local(A_MODEL + me), given);
        g_spots.insert(g_spots.end(), more.begin(), more.end());
    }
    g_preparedMs = NowMs();
}

void SendStart(const RaceKind& kind) {
    int me = Local(L_ME), cps = std::clamp(Local(L_CPS), 2, kRouteMax);
    if (!GetVehicle(Local(A_CAR + me))) return;
    const std::vector<uint8_t>& riders = g_startRiders;

    Writer w = Msg(OP_START);
    w.U8((uint8_t)kind.index);
    w.U8((uint8_t)Local(L_AREA));
    w.U8((uint8_t)Local(L_LAPS));
    w.U8((uint8_t)Local(L_CP_TYPE));
    w.F32(LocalF(L_CP_SIZE));
    w.Vec({LocalF(L_BOX), LocalF(L_BOX + 1), LocalF(L_BOX + 2)});
    w.U16((uint16_t)Local(A_MODEL + me));
    w.U8((uint8_t)Local(A_COL1 + std::min(me, 5)));
    w.U8((uint8_t)Local(A_COL2 + std::min(me, 5)));
    w.U8((uint8_t)Local(A_NEXT + me));  // where every racer starts counting
    w.U16((uint16_t)std::min(g_carHealth, 65535));
    w.U8((uint8_t)cps);
    for (int i = 0; i < cps; ++i) w.Vec({LocalF(A_ROUTE_X + i), LocalF(A_ROUTE_Y + i), LocalF(A_ROUTE_Z + i)});
    const std::vector<Spot>& spots = g_spots;
    w.U8((uint8_t)std::min(spots.size(), riders.size()));
    for (size_t k = 0; k < spots.size() && k < riders.size(); ++k) {
        w.U8(riders[k]);
        w.Vec({spots[k].pos.x, spots[k].pos.y, spots[k].pos.z});
        w.F32(spots[k].heading);
        LOGI("Grid place of player %d: %.1f %.1f %.1f, heading %.0f", riders[k], spots[k].pos.x, spots[k].pos.y, spots[k].pos.z,
             spots[k].heading);
    }
    Net().Send(proto::CH_RELIABLE, w);

    g_racers.clear();
    g_racers[Net().MyId()] = {};
    g_racers[Net().MyId()].ready = true;
    for (uint8_t id : riders) g_racers[id] = {};
    g_startSent = g_holding = true;
    g_startMs = NowMs();
    LOGI("Race %s started with %zu riders, %d laps of %d checkpoints", kind.name, riders.size(), Local(L_LAPS), cps);
}

void SendCount(int n) {
    Writer w = Msg(OP_COUNT);
    w.U8((uint8_t)n);
    Net().Send(proto::CH_RELIABLE, w);
}

void OwnProgress() {
    Racer& r = g_racers[Net().MyId()];
    if (r.state) return;
    int me = Local(L_ME);
    r.lap = (uint8_t)Local(A_LAP + me);
    r.next = (uint8_t)Local(A_NEXT + me);
    if (CPlayerPed* p = FindPlayerPed(0))
        r.dist = (p->GetPosition() - CVector{LocalF(A_NEXT_X + me), LocalF(A_NEXT_Y + me), 0}).Length2D();
}

void SendStandings() {
    std::vector<std::pair<uint8_t, Racer>> order(g_racers.begin(), g_racers.end());
    std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) {
        const Racer &x = a.second, &y = b.second;
        if ((x.state == 1) != (y.state == 1)) return x.state == 1;
        if (x.state == 1) return x.timeMs < y.timeMs;
        if ((x.state == 2) != (y.state == 2)) return y.state == 2;
        if (x.lap != y.lap) return x.lap > y.lap;
        if (x.next != y.next) return x.next > y.next;
        return x.dist < y.dist;
    });
    Writer w = Msg(OP_STANDINGS);
    w.U8((uint8_t)order.size());
    g_standings.clear();
    for (auto& [id, r] : order) {
        w.U8(id);
        w.U8(r.state);
        w.U32(r.timeMs);
        g_standings.push_back({id, r.state});
    }
    Net().Send(proto::CH_RELIABLE, w);
}

void Finished(uint8_t id, bool out, uint32_t timeMs) {
    auto it = g_racers.find(id);
    if (it == g_racers.end() || it->second.state) return;
    it->second.state = out ? 2 : 1;
    it->second.timeMs = timeMs;
    if (!out && !g_firstDoneMs) g_firstDoneMs = NowMs();
    LOGI("Race: player %d %s (%s)", id, out ? "is out" : "finished", TimeText(timeMs).c_str());
    SendStandings();
}

void EndRace() {
    SendStandings();
    uint8_t winner = 255;
    uint32_t best = 0;
    for (auto& [id, r] : g_racers)
        if (r.state == 1 && (winner == 255 || r.timeMs < best)) winner = id, best = r.timeMs;
    uint32_t prize = 0;
    const RaceKind* kind = g_lobby.kind;
    if (winner != 255 && kind && !(winner == Net().MyId() && g_ownerPassed)) prize = (uint32_t)kind->prize;
    Writer w = Msg(OP_END);
    w.U8(winner);
    w.U32(prize);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Race over: winner %d, prize %u from the mod", winner, prize);
    if (winner == Net().MyId() && prize) Cmd(0x0109).I(0).I((int)prize).Run();  // ADD_SCORE
    ui::TheChat().AddSystem(winner == 255 ? std::string(TR("Гонка окончена: никто из игроков не доехал.",
                                                          "The race is over: none of the players finished."))
                                          : Format(TR("Гонка окончена. Первым из игроков приехал %s.",
                                                      "The race is over. The first of the players: %s."),
                                                   Nick(winner).c_str()));
    g_lobby = {};
    ResetOwner();
}

// the race's racers but the owner's own, by network id (their cars and drivers are networked as any other)
void RememberBots() {
    int me = Local(L_ME), racers = std::clamp(Local(L_RACERS), 1, 16);
    g_botCars.clear();
    g_botDrivers.clear();
    for (int i = 0; i < racers; ++i) {
        if (i == me) continue;
        if (CVehicle* v = GetVehicle(Local(A_CAR + i)))
            if (NetVehicle* nv = Vehicles::Instance().Find(v)) g_botCars.push_back(nv->netId);
        if (CPed* p = GetPed(Local(A_DRIVER + i)))
            if (NetPed* np = Peds::Instance().Find(p)) g_botDrivers.push_back(np->netId);
    }
}

void Doom(const std::vector<uint16_t>& cars, const std::vector<uint16_t>& peds) {
    uint64_t until = NowMs() + 10000;
    for (uint16_t id : cars) g_doomedCars[id] = until;
    for (uint16_t id : peds) g_doomedPeds[id] = until;
}

// the race mission is over here while players still race: its racers would cruise about the track in their way
void SendBotsGone() {
    g_botsSent = true;
    bool racing = std::any_of(g_racers.begin(), g_racers.end(),
                              [](auto& e) { return e.first != Net().MyId() && e.second.state == 0; });
    if (!racing || (g_botCars.empty() && g_botDrivers.empty())) return;
    Writer w = Msg(OP_BOTS);
    w.U8((uint8_t)g_botCars.size());
    for (uint16_t id : g_botCars) w.U16(id);
    w.U8((uint8_t)g_botDrivers.size());
    for (uint16_t id : g_botDrivers) w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
    Doom(g_botCars, g_botDrivers);
    LOGI("Race mission over with players still racing: its %zu racers go", g_botCars.size());
}

// the doomed racers this game has: deleted (the other games' copies go with them, G_VEH_REMOVE / the peds' removal)
void DeleteDoomed() {
    uint64_t now = NowMs();
    for (auto it = g_doomedPeds.begin(); it != g_doomedPeds.end();) {
        CPed* p = Peds::Instance().FindOwned(it->first);
        if (p && !p->IsPlayer()) {
            Cmd(0x009B).I(GetPedRef(p)).Run();  // DELETE_CHAR
            LOGD("Race racer %04X deleted", it->first);
        }
        it = (p || now > it->second) ? g_doomedPeds.erase(it) : std::next(it);
    }
    for (auto it = g_doomedCars.begin(); it != g_doomedCars.end();) {
        NetVehicle* nv = Vehicles::Instance().Find(it->first);
        bool mine = nv && !Vehicles::Instance().IsMirror(*nv) && Vehicles::Alive(*nv);
        if (mine && !(nv->veh->Driver() && nv->veh->Driver()->IsPlayer())) {
            Cmd(0x00A6).I(nv->handle).Run();  // DELETE_CAR
            LOGD("Race racer's car %04X deleted", it->first);
        }
        it = (mine || now > it->second) ? g_doomedCars.erase(it) : std::next(it);
    }
}

// The race's own number of laps, alone or together, online or not: set until the race is on (the race's data, read
// again while it sets up, would bring the mission's own back), with the last lap the mission keeps beside it.
void SetLaps() {
    if (!RaceScript()) return;
    const RaceKind* kind = KindByIndex(Global(kRaceIndex));
    if (!kind || Local(L_STAGE) >= 11 || Local(L_LAPS) == kind->laps) return;
    Local(L_LAPS) = kind->laps;
    Local(L_LAST_LAP) = kind->laps - 1;
}

void OwnerUpdate() {
    uint64_t now = NowMs();
    if (g_lobby.open && g_lobby.owner == Net().MyId() && !g_launching) {
        if (now - g_askedMs > 1000) CancelLobby("the owner left the marker");
        return;
    }
    if (!g_launching) return;
    void* script = RaceScript();
    if (!script) {
        if (!g_startSent && now - g_launchMs < 10000) return;  // the mission is being loaded
        if (!g_startSent) {
            CancelLobby("the race mission did not start");
            ResetOwner();
            return;
        }
        if (Racer& me = g_racers[Net().MyId()]; !me.state) Finished(Net().MyId(), true, 0);  // failed, quit, died
        if (!g_botsSent) SendBotsGone();
        if (!g_goMs) {  // it ended before the go: the riders have nothing to race
            for (auto& [id, r] : g_racers)
                if (!r.state) r.state = 2;
        }
    } else {
        int stage = Local(L_STAGE);
        const RaceKind* kind = KindByIndex(Global(kRaceIndex));
        if (!g_startSent && stage >= 7 && kind) {
            if (!g_preparedMs) PrepareStart();
            else if (now - g_preparedMs > 300) SendStart(*kind);  // the racers' removal has gone out first
        }
        if (g_holding) {  // the countdown stays at "3" until the riders sit in their cars
            bool ready = std::all_of(g_racers.begin(), g_racers.end(), [](auto& e) { return e.second.ready; });
            if (!ready && now - g_startMs < kReadyWaitMs) {
                if (stage == 8) Global(kCountdownAt) = Global(kTimer) + 999;
            } else {
                g_holding = false;
                SendCount(3);
            }
        }
        if (stage != g_stage && g_startSent) {
            if (stage == 9) SendCount(2);
            else if (stage == 10) SendCount(1);
            else if (stage == 11 && g_stage < 11) {
                SendCount(0);
                g_goMs = now;
            }
            if (stage == 13 && g_stage < 13) {
                g_ownerPassed = Local(L_AHEAD) == 0;
                Finished(Net().MyId(), false, (uint32_t)(now - g_goMs));
            }
            g_stage = stage;
        }
        if (stage >= 11 && stage < 13) OwnProgress();
        if (g_startSent) RememberBots();
    }
    if (!g_startSent) return;
    for (auto& [id, r] : g_racers)  // the riders who left the session
        if (id != Net().MyId() && !r.state && !Net().Peers().count(id)) Finished(id, true, 0);
    bool over = std::all_of(g_racers.begin(), g_racers.end(), [](auto& e) { return e.second.state != 0; });
    if (over || (g_firstDoneMs && now - g_firstDoneMs > kAfterFirstMs) || now - g_startMs > kRaceMaxMs) {
        for (auto& [id, r] : g_racers)
            if (!r.state) r.state = 2;
        EndRace();
        return;
    }
    if (now - g_standingsMs > 500) {
        g_standingsMs = now;
        SendStandings();
    }
}

// ---------------------------------------------------------------- rider
void ClearMarkers() {
    if (g_ride.checkpoint != -1) Cmd(0x06D6).I(g_ride.checkpoint).Run();  // DELETE_CHECKPOINT
    if (g_ride.blip != -1) Cmd(0x0164).I(g_ride.blip).Run();              // REMOVE_BLIP
    g_ride.checkpoint = g_ride.blip = -1;
}

// the next checkpoint as the race mission shows the owner's: a flag on the last one
void ShowNext() {
    ClearMarkers();
    if (g_ride.done) return;
    const CVector& p = g_ride.route[g_ride.next];
    if (g_ride.lap == g_ride.laps - 1 && g_ride.next == g_ride.cps - 1) {
        Cmd(0x06D5).I(1).F(p.x).F(p.y).F(p.z).F(0).F(0).F(0).F(6.0f).Out(&g_ride.checkpoint).Run();  // CREATE_CHECKPOINT
    } else {
        const CVector& to = g_ride.route[(g_ride.next + 1) % g_ride.cps];
        Cmd(0x06D5).I(g_ride.cpType).F(p.x).F(p.y).F(p.z).F(to.x).F(to.y).F(to.z).F(g_ride.cpSize).Out(&g_ride.checkpoint).Run();
    }
    if (g_ride.area == 0) {  // the race shows no blip in an interior (a stadium) either
        Cmd(0x018A).F(p.x).F(p.y).F(p.z).Out(&g_ride.blip).Run();  // ADD_BLIP_FOR_COORD
        Cmd(0x0165).I(g_ride.blip).I(0).Run();                    // CHANGE_BLIP_COLOUR
        Cmd(0x018B).I(g_ride.blip).I(2).Run();                    // CHANGE_BLIP_DISPLAY: blip only
    }
}

void SendFinish(bool out, uint32_t timeMs) {
    Writer w = Msg(OP_FINISH);
    w.U8(out);
    w.U32(timeMs);
    Net().Send(proto::CH_RELIABLE, w);
}

// out of the stadium where the race mission puts its CJ (a little aside: several come out), without the race's car
void LeaveRace() {
    if (!g_ride.on) return;
    ClearMarkers();
    CPlayerPed* me = FindPlayerPed(0);
    // still in the race's interior: one who died there is in a hospital already
    if (me && me->Health() > 0 && core::IsPlayerReady() && g_ride.area != 0 && me->AreaCode() == g_ride.area) {
        int handle = GetPedRef(me);
        float a = (float)Net().MyId() * 1.3f;
        CVector p = CVector{g_ride.kind->exitX, g_ride.kind->exitY, g_ride.kind->exitZ} + CVector{std::cos(a), std::sin(a), 0} * 1.5f;
        if (me->InVehicle()) Cmd(0x0362).I(handle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        else Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z).Run();                  // SET_CHAR_COORDINATES
        Cmd(0x0173).I(handle).F(g_ride.kind->exitHeading).Run();  // SET_CHAR_HEADING
        Cmd(0x0860).I(handle).I(0).Run();  // SET_CHAR_AREA_VISIBLE
        Cmd(0x04BB).I(0).Run();            // SET_AREA_VISIBLE
        LightArea(0, p);
        Cmd(0x03CB).F(p.x).F(p.y).F(p.z).Run();  // LOAD_SCENE
        Cmd(0x01B4).I(0).I(1).Run();             // SET_PLAYER_CONTROL on
        Cmd(0x0373).Run();                       // SET_CAMERA_BEHIND_PLAYER
        Cmd(0x02EB).Run();                       // RESTORE_CAMERA_JUMPCUT
    }
    if (CVehicle* v = GetVehicle(g_ride.car); v && !(me && me->InVehicle() && me->Vehicle() == v))
        Cmd(0x00A6).I(g_ride.car).Run();  // DELETE_CAR
    else if (g_ride.car != -1)
        Cmd(0x01C3).I(g_ride.car).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED
    LOGI("Left the race");
    g_ride = {};
    g_standings.clear();
}

void HandleStart(uint8_t from, Reader& r) {
    Ride ride;
    ride.owner = from;
    ride.kind = KindByIndex(r.U8());
    ride.area = r.U8();
    ride.laps = r.U8();
    ride.cpType = r.U8();
    ride.cpSize = r.F32();
    Vec3 b = r.Vec();
    ride.box = {b.x, b.y, b.z};
    int model = r.U16();
    int col1 = r.U8(), col2 = r.U8();
    ride.next = r.U8();
    ride.health = r.U16();
    ride.cps = r.U8();
    for (int i = 0; i < ride.cps && r.Ok(); ++i) {
        Vec3 p = r.Vec();
        ride.route.push_back({p.x, p.y, p.z});
    }
    bool mine = false;
    CVector spot;
    float heading = 0;
    int n = r.U8();
    for (int i = 0; i < n && r.Ok(); ++i) {
        uint8_t id = r.U8();
        Vec3 p = r.Vec();
        float h = r.F32();
        if (id == Net().MyId()) mine = true, spot = {p.x, p.y, p.z}, heading = h;
    }
    if (!r.Ok() || !ride.kind || from != g_lobby.owner || ride.cps < 2 || ride.laps < 1 || ride.next >= ride.cps ||
        !IsVehicleModel(model))
        return;
    bool wasJoined = g_joined;
    g_lobby.open = false;  // the race is on: nobody joins any more
    g_joined = false;
    if (!mine) {
        if (wasJoined)
            ui::TheChat().AddError(TR("Гонка началась без вас: вас не успели записать.", "The race started without you: you were not counted in time."));
        return;
    }
    CPlayerPed* me = FindPlayerPed(0);
    auto cannot = [](const char* why) {
        SendFinish(true, 0);  // the owner does not wait for this one
        ui::TheChat().AddError(TR("Не получилось выйти на старт.", "Could not get to the start."));
        LOGW("Not in the race: %s", why);
    };
    if (!me || !core::IsPlayerReady() || me->InVehicle()) return cannot("not on foot");
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle") || !LoadModelNow(model)) return cannot("no car");
    int handle = GetPedRef(me);
    Cmd(0x0792).I(handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    Cmd(0x04BB).I(ride.area).Run();  // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(ride.area).Run();  // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(spot.x).F(spot.y).F(spot.z + 1.0f).Run();  // SET_CHAR_COORDINATES
    Cmd(0x04E4).F(spot.x).F(spot.y).Run();  // REQUEST_COLLISION
    Cmd(0x03CB).F(spot.x).F(spot.y).F(spot.z).Run();  // LOAD_SCENE
    LightArea(ride.area, spot);
    float ground = -200.0f;
    Cmd(0x02CE).F(spot.x).F(spot.y).F(spot.z + 2.0f).Out(&ground).Run();  // GET_GROUND_Z_FOR_3D_COORD
    if (std::fabs(ground - spot.z) < 3.0f) spot.z = ground;  // CREATE_CAR puts the car's base there
    Cmd(0x00A5).I(model).F(spot.x).F(spot.y).F(spot.z).Out(&ride.car).Run();  // CREATE_CAR
    if (ride.car < 0) return cannot("CREATE_CAR failed");
    Cmd(0x0175).I(ride.car).F(heading).Run();  // SET_CAR_HEADING
    Cmd(0x0840).I(ride.car).I(ride.area).Run();  // SET_VEHICLE_AREA_VISIBLE
    Cmd(0x0229).I(ride.car).I(col1).I(col2).Run();  // CHANGE_CAR_COLOUR
    Cmd(0x0224).I(ride.car).I(std::max(ride.health, 1000)).Run();  // SET_CAR_HEALTH: as the owner's race car
    LOGI("Race car %d at %.1f %.1f %.1f (ground %.1f), heading %.0f", ride.car, spot.x, spot.y, spot.z, ground, heading);
    Cmd(0x036A).I(handle).I(ride.car).Run();  // WARP_CHAR_INTO_CAR
    Cmd(0x01B4).I(0).I(0).Run();  // SET_PLAYER_CONTROL off until the go
    Cmd(0x0373).Run();  // SET_CAMERA_BEHIND_PLAYER
    Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT
    ride.on = true;
    ride.heardMs = NowMs();
    g_ride = ride;
    ShowNext();
    Net().Send(proto::CH_RELIABLE, Msg(OP_READY));
    ui::TheChat().AddSystem(Format(TR("Гонка %s: %d кругов. Первый из игроков получит $%d.",
                                      "%s: %d laps. The first of the players gets $%d."),
                                   ride.kind->name, ride.laps, ride.kind->prize));
    LOGI("Racing in %s's %s: %d laps of %d checkpoints, model %d", Nick(from).c_str(), ride.kind->name, ride.laps, ride.cps, model);
}

void RiderUpdate() {
    uint64_t now = NowMs();
    // standing at another player's marker: joined
    if (g_lobby.open && g_lobby.owner != Net().MyId()) {
        CPlayerPed* me = FindPlayerPed(0);
        bool near = me && core::IsPlayerReady() && !me->InVehicle() && me->AreaCode() == 0 &&
                    (me->GetPosition() - g_lobby.marker).Length2D() < kJoinRange &&
                    std::fabs(me->GetPosition().z - g_lobby.marker.z) < 2.0f;
        bool can = g_lobby.kind->stat < 0 || DrivingStat(g_lobby.kind->stat) > 199.0f;
        static bool told = false;
        if (near && !can && !told) {
            told = true;
            ui::TheChat().AddError(TR("Для этой гонки нужен навык вождения выше.", "This race needs a higher driving skill."));
        }
        if (!near) told = false;
        bool join = near && can;
        if (join != g_joined) {
            g_joined = join;
            Writer w = Msg(OP_JOIN);
            w.U8(join);
            Net().Send(proto::CH_RELIABLE, w);
            LOGI(join ? "Joined %s's race" : "Left %s's race", Nick(g_lobby.owner).c_str());
        }
    }
    if (!g_ride.on) return;
    if (!g_ride.endMs && now - g_ride.heardMs > kOwnerSilentMs) {
        ui::TheChat().AddError(TR("Хозяин гонки пропал: гонка окончена.", "The race's owner is gone: the race is over."));
        g_ride.endMs = now - 5000;
    }
    if (g_ride.endMs && now - g_ride.endMs > 5000) {
        LeaveRace();
        return;
    }
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !core::IsPlayerReady()) return;
    if (me->Health() <= 0 && !g_ride.done) {  // dead: out of the race, the game takes the player to a hospital
        g_ride.done = true;
        ClearMarkers();
        SendFinish(true, 0);
        LOGI("Out of the race: dead");
        return;
    }
    if (!g_ride.go || g_ride.done) return;
    CVector pos = me->GetPosition();
    const CVector& cp = g_ride.route[g_ride.next];
    // passed as LOCATE_CHAR_IN_CAR_3D sees it: in a car, in the box around the checkpoint
    if (me->InVehicle() && std::fabs(pos.x - cp.x) <= g_ride.box.x && std::fabs(pos.y - cp.y) <= g_ride.box.y &&
        std::fabs(pos.z - cp.z) <= g_ride.box.z) {
        Sound(1058);
        if (++g_ride.next >= g_ride.cps) {
            ++g_ride.lap;
            g_ride.next = 0;
            if (g_ride.lap >= g_ride.laps) {
                g_ride.done = true;
                uint32_t t = (uint32_t)(now - g_ride.goMs);
                SendFinish(false, t);
                BigText("FINISH", 3000);
                ui::TheChat().AddSystem(Format(TR("Финиш: %s.", "Finished: %s."), TimeText(t).c_str()));
            }
        }
        ShowNext();
    }
    if (now - g_ride.sentMs > 200) {
        g_ride.sentMs = now;
        Writer w = Msg(OP_PROGRESS);
        w.U8((uint8_t)g_ride.lap);
        w.U8((uint8_t)g_ride.next);
        w.F32((pos - g_ride.route[g_ride.next]).Length2D());
        Net().Send(proto::CH_STATE, w);
    }
}

void HandleRace(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (!r.Ok() || from == Net().MyId()) return;
    bool owner = g_lobby.owner == Net().MyId() && (g_lobby.open || g_launching);
    switch (op) {
        case OP_LOBBY: {
            const RaceKind* kind = KindByIndex(r.U8());
            Vec3 p = r.Vec();
            uint32_t left = r.U32();
            if (!r.Ok() || !kind || g_ride.on || g_launching) return;
            if (g_lobby.open && g_lobby.owner == Net().MyId()) {
                if (from > Net().MyId()) return;  // both stepped in at once: the lower id's race
                CancelLobby("another player's lobby came at the same time");
            }
            g_lobby = {true, from, kind, {p.x, p.y, p.z}, NowMs() + std::min<uint32_t>(left, (uint32_t)kGatherMs)};
            g_joined = false;
            ui::TheChat().AddSystem(Format(TR("%s собирает гонку %s: встаньте в её маркер за %d с, чтобы поехать вместе.",
                                              "%s is gathering a race, %s: stand at its marker within %d s to race together."),
                                           Nick(from).c_str(), kind->name, (int)(left / 1000)));
            break;
        }
        case OP_CANCEL:
            if (g_lobby.owner != from || g_launching) return;
            if (g_joined) ui::TheChat().AddSystem(TR("Гонку отменили.", "The race was called off."));
            g_lobby = {};
            g_joined = false;
            break;
        case OP_JOIN: {
            bool on = r.U8() != 0;
            if (!r.Ok() || !owner || g_startSent) return;  // counted until the grid is set
            if (on && g_riders.insert(from).second)
                ui::TheChat().AddSystem(Format(TR("%s едет с вами.", "%s races with you."), Nick(from).c_str()));
            else if (!on && g_riders.erase(from))
                ui::TheChat().AddSystem(Format(TR("%s передумал ехать.", "%s does not race after all."), Nick(from).c_str()));
            break;
        }
        case OP_START:
            HandleStart(from, r);
            break;
        case OP_READY:
            if (owner && g_racers.count(from)) g_racers[from].ready = true;
            break;
        case OP_COUNT: {
            int n = r.U8();
            if (!r.Ok() || !g_ride.on || from != g_ride.owner) return;
            g_ride.heardMs = NowMs();
            static const char* const kCount[] = {"GO!", "1", "2", "3"};
            BigText(kCount[std::min(n, 3)], n ? 1100 : 800);
            Sound(n ? 1056 : 1057);
            if (!n && !g_ride.go) {
                g_ride.go = true;
                g_ride.goMs = NowMs();
                Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL on
            }
            break;
        }
        case OP_PROGRESS: {
            uint8_t lap = r.U8(), next = r.U8();
            float dist = r.F32();
            if (!r.Ok() || !owner || !g_racers.count(from)) return;
            Racer& rc = g_racers[from];
            rc.lap = lap;
            rc.next = next;
            rc.dist = dist;
            break;
        }
        case OP_FINISH: {
            bool out = r.U8() != 0;
            uint32_t t = r.U32();
            if (r.Ok() && owner) Finished(from, out, t);
            break;
        }
        case OP_BOTS: {
            std::vector<uint16_t> cars, peds;
            for (int i = 0, n = r.U8(); i < n && r.Ok(); ++i) cars.push_back(r.U16());
            for (int i = 0, n = r.U8(); i < n && r.Ok(); ++i) peds.push_back(r.U16());
            if (r.Ok()) Doom(cars, peds);
            break;
        }
        case OP_STANDINGS: {
            int n = r.U8();
            std::vector<std::pair<uint8_t, uint8_t>> list;
            for (int i = 0; i < n && r.Ok(); ++i) {
                uint8_t id = r.U8(), state = r.U8();
                r.U32();
                list.push_back({id, state});
            }
            if (r.Ok() && g_ride.on && from == g_ride.owner) g_standings = list, g_ride.heardMs = NowMs();
            break;
        }
        case OP_END: {
            uint8_t winner = r.U8();
            uint32_t prize = r.U32();
            if (!r.Ok() || !g_ride.on || from != g_ride.owner) return;
            if (winner == Net().MyId() && prize) Cmd(0x0109).I(0).I((int)prize).Run();  // ADD_SCORE
            if (winner == Net().MyId()) BigText("WINNER!", 3000);
            ui::TheChat().AddSystem(winner == 255 ? std::string(TR("Гонка окончена: никто из игроков не доехал.",
                                                                  "The race is over: none of the players finished."))
                                    : winner == Net().MyId()
                                        ? Format(TR("Вы первый из игроков! Приз: $%u.", "You are the first of the players! Prize: $%u."), prize)
                                        : Format(TR("Гонка окончена. Первым из игроков приехал %s.",
                                                    "The race is over. The first of the players: %s."),
                                                 Nick(winner).c_str()));
            g_ride.done = true;
            g_ride.endMs = NowMs();
            ClearMarkers();
            break;
        }
    }
}

int MyPlace() {
    for (size_t i = 0; i < g_standings.size(); ++i)
        if (g_standings[i].first == Net().MyId()) return (int)i + 1;
    return 0;
}

// A rider's own lap, place and time, where the race mission draws them for its CJ (right of the screen): the mission's
// panel is the owner's only, and the lap of the owner was no use to the others.
void DrawRidePanel() {
    if (!g_ride.on || !g_ride.go || g_ride.done || g_ride.endMs || !core::IsPlayerReady()) return;
    auto& g = ui::Gfx();
    float s = g.Scale(), lh = g.LineHeight(ui::FONT_MENU), w = 300 * s, x = g.Width() - w - 40 * s, y = g.Height() * 0.30f;
    int place = MyPlace();
    uint32_t t = (uint32_t)(NowMs() - g_ride.goMs);
    std::string lines[] = {
        Format(TR("Круг %d/%d", "Lap %d/%d"), std::min(g_ride.lap + 1, g_ride.laps), g_ride.laps),
        place ? Format(TR("Место %d/%zu", "Place %d/%zu"), place, g_standings.size()) : std::string(),
        TimeText(t),
    };
    g.Rect(x, y, w, lh * 3 + 24 * s, 0xA0000000);
    for (const std::string& line : lines) {
        y += 8 * s;
        if (!line.empty()) g.Text(ui::FONT_MENU, x + w - 16 * s, y, line, 0xFFE2C063, ui::TEXT_RIGHT | ui::TEXT_SHADOW);
        y += lh;
    }
}

class Race : public Module {
public:
    const char* Name() const override { return "Race"; }

    void OnInstallHooks() override { Net().SetHandler(proto::G_RACE, &HandleRace); }

    void OnPostProcess() override {
        if (core::IsPlayerReady()) SetLaps();
        if (!Net().Online()) return;
        OwnerUpdate();
        RiderUpdate();
        if (!g_doomedCars.empty() || !g_doomedPeds.empty()) DeleteDoomed();
    }

    void OnRender() override {
        if (Net().Online()) DrawRidePanel();
    }

    void OnPeerLeave(uint8_t id) override {
        if (g_lobby.owner == id) {
            if (g_joined) ui::TheChat().AddSystem(TR("Хозяин гонки вышел: гонка отменена.", "The race's owner left: the race is off."));
            g_lobby = {};
            g_joined = false;
        }
        g_riders.erase(id);
        if (g_ride.on && g_ride.owner == id) LeaveRace();
    }

    void OnSessionEnd() override { Reset(); }
    void OnGameShutdown() override {  // a load: this game's race and markers are gone
        if (g_launching && g_startSent && Net().Online()) {
            Writer w = Msg(OP_END);
            w.U8(255);
            w.U32(0);
            Net().Send(proto::CH_RELIABLE, w);
        }
        CancelLobby("the game is reloaded");
        Reset();
    }

    static Race& Instance() {
        static Race m;
        return m;
    }

private:
    static void Reset() {
        g_ride = {};  // the world goes away with its car and markers
        g_doomedCars.clear();
        g_doomedPeds.clear();
        g_lobby = {};
        g_joined = false;
        g_standings.clear();
        ResetOwner();
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Race::Instance()); }
} g_reg;
}  // namespace

// ---------------------------------------------------------------- the rest of the game asks
bool SideStartAllowed(const char* thread) {
    const RaceKind* kind = KindByThread(thread);
    if (!kind || !Net().Online() || Net().Peers().empty()) return true;
    if (g_ride.on || (g_lobby.open && g_lobby.owner != Net().MyId())) return false;  // someone else's race
    if (g_launching) return false;  // the riders of the last race together have not finished yet
    if (!g_lobby.open) OpenLobby(*kind);
    g_askedMs = NowMs();
    if (NowMs() < g_lobby.endsMs && !EverybodyJoined()) return false;
    if (g_riders.empty()) {  // nobody came: an ordinary race of one's own
        CancelLobby("nobody joined");
        return true;
    }
    g_launching = true;
    g_launchMs = NowMs();
    LOGI("Race %s goes with %zu riders", kind->name, g_riders.size());
    return true;
}

bool OwnRaceTogether() { return g_launching && RaceScript(); }

std::string RaceStatusText() {
    if (!Net().Online()) return "";
    if (g_lobby.open && g_lobby.owner == Net().MyId() && !g_launching)
        return Format(TR("%s · старт через %d с · с вами: %zu", "%s · starts in %d s · with you: %zu"), g_lobby.kind->name,
                      SecondsLeft(), g_riders.size());
    if (g_lobby.open && g_joined)
        return Format(TR("%s · вы едете с %s · старт через %d с", "%s · racing with %s · starts in %d s"), g_lobby.kind->name,
                      Nick(g_lobby.owner).c_str(), SecondsLeft());
    if (g_lobby.open && g_lobby.owner != Net().MyId()) {
        CPlayerPed* me = FindPlayerPed(0);
        if (me && (me->GetPosition() - g_lobby.marker).Length2D() < 100.0f)
            return Format(TR("%s собирает гонку %s · встаньте в маркер · %d с", "%s gathers a race, %s · stand at the marker · %d s"),
                          Nick(g_lobby.owner).c_str(), g_lobby.kind->name, SecondsLeft());
    }
    int place = MyPlace();
    if (g_ride.on && !g_ride.endMs) {
        if (g_ride.done) return place ? Format(TR("Финиш · место среди игроков: %d/%zu", "Finished · place among the players: %d/%zu"), place, g_standings.size()) : "";
        if (!g_ride.go) return TR("Приготовьтесь к старту", "Get ready to start");
        return "";  // the race panel shows the lap (DrawRidePanel)
    }
    if (g_launching && g_startSent && place)
        return Format(TR("Место среди игроков: %d/%zu", "Place among the players: %d/%zu"), place, g_standings.size());
    return "";
}

}  // namespace coop
