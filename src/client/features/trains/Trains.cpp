// Trains both games share. A train is its own game's (a random one, a mission's): that game tells the others where it
// is (kind, track, distance along the rails, speed), and they show it as a mission train of the same kind kept where the
// real one is, set every frame. The copies are locked, the driver is in the train's game. Random trains come from the
// host's game while the players are together: the partner's game makes its own only while its player is far from the
// host (CTrain::DisableRandomTrains). Whoever rides a train sits in the cab of its copy (Players). The kind of a train is
// not kept on it: it is noted when the train is made (CTrain::CreateMissionTrain makes the random ones too).
// A copy's engine is open while no player drives it and no story mission of the host's runs: the player who gets in drives
// it in its own game (its pad, CTrain::ProcessControl), and the train's own game follows (TRAIN_DRIVE) with the copy of
// that player at its wheel (TRAIN_ENTER: its walk to the cab, which with no word there followed its owner up the step and
// dropped back, "landing"). Every copy was locked, and the partner could not get into a train at all (Holder, 29.10).
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { TRAIN_STATE = 1, TRAIN_GONE = 2, TRAIN_DRIVE = 3, TRAIN_ENTER = 4 };
constexpr uint16_t kMissionTrain = 0x20;  // CTrain::m_nTrainFlags: CTrain::DoTrainGenerationAndRemoval keeps it
constexpr uintptr_t kDisableRandomTrains = 0xC38010;  // CTrain::bDisableRandomTrains
constexpr uint16_t kFrontCarriage = 0x8, kClockwise = 0x40;  // CTrain::m_nTrainFlags (+0x5B8)
constexpr int kTrainModels[] = {449, 537, 538, 569, 570, 590};  // tram, freight, streak and their carriages
// The partner's game makes random trains this far from the host. The host's game removes its random trains 220 m from
// its camera (CTrain::DoTrainGenerationAndRemoval, 0x863994): the partner 220 to 600 m away saw none.
constexpr float kOwnRandomFrom = 220.0f;
constexpr float kSendWithin = 300.0f;  // a train is sent this near another player (a copy goes 3 s after the last state)

float& Speed(CVehicle* t) { return Field<float>(t, 0x5A4); }         // CTrain::m_fTrainSpeed
float& RailDistance(CVehicle* t) { return Field<float>(t, 0x5A8); }  // CTrain::m_fCurrentRailDistance
uint16_t Flags(CVehicle* t) { return Field<uint16_t>(t, 0x5B8); }
int8_t Track(CVehicle* t) { return Field<int8_t>(t, 0x5C0); }
CVehicle* NextCarriage(CVehicle* t) { return Field<CVehicle*>(t, 0x5D4); }

// ours
std::map<CVehicle*, uint8_t> g_kinds;  // engines CTrain::CreateMissionTrain made here, and their kind
std::map<CVehicle*, uint16_t> g_ids;   // the ones announced
uint16_t g_nextId = 1;
uint64_t g_sendMs = 0;
bool g_copying = false;                // a copy is being made: not ours

// the other games'
struct Copy {
    CVehicle* engine;
    uint8_t kind;
    float rail, speed;
    uint64_t ms;
};
std::map<uint32_t, Copy> g_copies;  // by owner << 16 | its id
bool g_randomOff = false;           // we switched the random trains off

// ours another game's player drives (its copy there): where that game has it, and whether it was a mission's before
struct Driven {
    float rail = 0, speed = 0;
    uint64_t ms = 0;
    bool wasMission = false;
};
std::map<CVehicle*, Driven> g_driven;
CVehicle* g_entering = nullptr;  // the copy our player last set off into

bool WeDrive(CVehicle* t) {
    CPlayerPed* me = FindPlayerPed(0);
    return me && me->InVehicle() && me->Vehicle() == t && t->Driver() == me;
}

using CreateTrainFn = void(__cdecl*)(CVector, bool, uint32_t, CVehicle**, CVehicle**, int32_t, int32_t, bool);
CreateTrainFn g_origCreateTrain = nullptr;
void __cdecl HookCreateMissionTrain(CVector pos, bool clockwise, uint32_t kind, CVehicle** first, CVehicle** last, int32_t node,
                                    int32_t track, bool mission) {
    g_origCreateTrain(pos, clockwise, kind, first, last, node, track, mission);
    if (first && *first && !g_copying) g_kinds[*first] = (uint8_t)kind;
}

bool IsEngine(CVehicle* v) { return IsVehicleValid(v) && v->IsTrain() && (Flags(v) & kFrontCarriage); }

// CTrain::CreateMissionTrain without a node takes the nearest node of all four tracks (CTrain::FindClosestTrackNode,
// 0x6F6BD0) and drops the track it is given. Track 3 runs a few metres beside the main line through Los Santos: a copy
// made there went onto the other track, where the owner's rail distance is another place ("Catalyst": the partner saw
// only the tarp). The nearest node of the owner's own track keeps it there.
int NearestNode(int track, const CVector& p) {
    int n = Ref<int32_t>(0xC38014 + (uintptr_t)track * 4);               // NumTrackNodes
    auto* nodes = Ref<const uint8_t*>(0xC38024 + (uintptr_t)track * 4);  // pTrackNodes: x, y, z in 1/8 m, 10 bytes each
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; nodes && i < n; ++i) {
        auto* xyz = (const int16_t*)(nodes + (size_t)i * 10);
        float dx = xyz[0] / 8.0f - p.x, dy = xyz[1] / 8.0f - p.y, dz = xyz[2] / 8.0f - p.z;
        if (float d = dx * dx + dy * dy + dz * dz; d < bestD) bestD = d, best = i;
    }
    return best;
}

void SendGone(uint16_t id) {
    Writer w(proto::G_TRAIN);
    w.U8(TRAIN_GONE);
    w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendOurs() {
    for (auto it = g_kinds.begin(); it != g_kinds.end();) {
        CVehicle* t = it->first;
        auto id = g_ids.find(t);
        if (!IsEngine(t)) {
            if (id != g_ids.end()) SendGone(id->second), g_ids.erase(id);
            it = g_kinds.erase(it);
            continue;
        }
        if (id == g_ids.end()) id = g_ids.emplace(t, g_nextId++).first;
        CVector p = t->GetPosition();
        if (RemoteDistance(p, 0) > kSendWithin) {
            ++it;
            continue;
        }
        Writer w(proto::G_TRAIN);
        w.U8(TRAIN_STATE);
        w.U16(id->second);
        w.U8(it->second);
        w.I8(Track(t));
        w.U8((Flags(t) & kClockwise) ? 1 : 0);
        w.F32(RailDistance(t));
        w.F32(Speed(t));
        w.Vec({p.x, p.y, p.z});
        Net().Send(proto::CH_STATE, w);
        ++it;
    }
}

// the copy goes with whoever is in it put out first (a vehicle deleted with peds inside leaves them dangling)
void RemoveCopy(const Copy& c) {
    if (!IsEngine(c.engine)) return;
    for (CVehicle* t = c.engine; t; t = NextCarriage(t)) {
        CVector p = t->GetPosition();
        if (CPed* d = t->Driver()) Cmd(0x0362).I(GetPedRef(d)).F(p.x).F(p.y).F(p.z + 3.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        for (int s = 0; s < 8; ++s)
            if (CPed* q = t->Passengers()[s]) Cmd(0x0362).I(GetPedRef(q)).F(p.x).F(p.y).F(p.z + 3.0f).Run();
        // CTrain::RemoveOneMissionTrain deletes the carriages without CWorld::RemoveReferencesToDeletedObject: a player
        // standing on the copy's roof (a mission's train, deleted and made again by its script) kept standing on freed
        // memory
        Call<0x565510>(t);
    }
    Call<0x6F5DC0>(c.engine);  // CTrain::RemoveOneMissionTrain
}

void RemoveCopies(int owner = -1) {
    for (auto it = g_copies.begin(); it != g_copies.end();)
        if (owner < 0 || (int)(it->first >> 16) == owner) RemoveCopy(it->second), it = g_copies.erase(it);
        else ++it;
}

CVehicle* OursById(uint16_t id) {
    for (auto& [t, i] : g_ids)
        if (i == id && IsEngine(t)) return t;
    return nullptr;
}

// the other player at the wheel of its copy of our train: where it drives it, or its walk to the cab
void HandleDriver(uint8_t from, uint8_t op, uint16_t id, Reader& r) {
    uint8_t owner = r.U8();
    CVehicle* t = owner == Net().MyId() ? OursById(id) : nullptr;
    if (!r.Ok() || !t || t->Driver() == FindPlayerPed(0)) return;  // ours at its wheel here: ours drives
    if (op == TRAIN_ENTER) {
        RemotePlayer* rp = Players::Instance().Get(from);
        if (!rp || !rp->ped || rp->ped->InVehicle()) return;
        rp->enterTaskMs = NowMs();
        Cmd(0x05CB).I(rp->pedHandle).I(GetVehicleRef(t)).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
        LOGI("Player %d gets into our train %d", from, id);
        return;
    }
    float rail = r.F32(), speed = r.F32();
    if (!r.Ok() || !std::isfinite(rail) || !std::isfinite(speed) || std::fabs(rail) > 1e6f || std::fabs(speed) > 50.0f) return;
    Driven& d = g_driven[t];
    if (!d.ms) {
        d.wasMission = (Flags(t) & kMissionTrain) != 0;
        LOGI("Player %d drives our train %d", from, id);
    }
    d.rail = rail, d.speed = speed, d.ms = NowMs();
}

// our player sets off into the cab of a copy: the copy of our player there walks in too
void SendEntering() {
    void* task = nullptr;
    CVehicle* target = ActivePrimaryTaskType(FindPlayerPed(0), &task) == 701 ? Field<CVehicle*>(task, 0x0C) : nullptr;  // TASK_COMPLEX_ENTER_CAR_AS_DRIVER
    if (target == g_entering) return;
    g_entering = target;
    for (auto& [key, c] : g_copies)
        if (target && c.engine == target && target->DoorLock() == 1) {  // open: no player drives it there
            Writer w(proto::G_TRAIN);
            w.U8(TRAIN_ENTER);
            w.U16((uint16_t)key);
            w.U8((uint8_t)(key >> 16));
            Net().Send(proto::CH_RELIABLE, w);
        }
}

void SendDriven() {
    for (auto& [key, c] : g_copies)
        if (IsEngine(c.engine) && WeDrive(c.engine)) {
            Writer w(proto::G_TRAIN);
            w.U8(TRAIN_DRIVE);
            w.U16((uint16_t)key);
            w.U8((uint8_t)(key >> 16));
            w.F32(RailDistance(c.engine));
            w.F32(Speed(c.engine));
            Net().Send(proto::CH_STATE, w);
        }
}

void ForgetDriven() {  // our trains are their own again
    for (auto& [t, d] : g_driven)
        if (IsEngine(t) && !d.wasMission) Field<uint16_t>(t, 0x5B8) &= ~kMissionTrain;
    g_driven.clear();
    g_entering = nullptr;
}

void HandleTrain(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint16_t id = r.U16();
    if (!r.Ok() || !core::IsPlayerReady()) return;
    if (op == TRAIN_DRIVE || op == TRAIN_ENTER) return HandleDriver(from, op, id, r);
    uint32_t key = (uint32_t)from << 16 | id;
    auto it = g_copies.find(key);
    if (op == TRAIN_GONE) {
        if (it != g_copies.end()) RemoveCopy(it->second), g_copies.erase(it);
        return;
    }
    uint8_t kind = r.U8();
    int8_t track = r.I8();
    bool clockwise = r.U8() != 0;
    float rail = r.F32(), speed = r.F32();
    Vec3 p = r.Vec();
    if (!r.Ok() || kind > 15 || track < 0 || track > 3) return;  // CTrain's kinds and tracks
    // CTrain::ProcessControl brings the distance back onto the track a length at a time: NaN or a huge one froze it
    if (!std::isfinite(rail) || !std::isfinite(speed) || std::fabs(rail) > 1e6f || std::fabs(speed) > 50.0f) return;
    if (it != g_copies.end() && (it->second.kind != kind || !IsEngine(it->second.engine))) {
        RemoveCopy(it->second);
        g_copies.erase(it);
        it = g_copies.end();
    }
    if (it == g_copies.end()) {
        CVector at{p.x, p.y, p.z};
        int node = NearestNode(track, at);
        if (node < 0) return;  // no such track here
        // asked and streamed in the background, the copy comes with a later state: six blocking loads at once (the
        // whole streaming queue flushed) were a freeze each time a train came within 300 m
        bool loaded = true;
        for (int model : kTrainModels)
            if (!IsModelLoaded(model)) RequestModel(model), loaded = false;
        if (!loaded) return;
        CVehicle *first = nullptr, *last = nullptr;
        g_copying = true;
        g_origCreateTrain(at, clockwise, kind, &first, &last, node, track, true);
        g_copying = false;
        if (!first) return;
        for (CVehicle* t = first; t; t = NextCarriage(t)) t->DoorLock() = 2;  // CARLOCK_LOCKED: driven in its game
        it = g_copies.emplace(key, Copy{first, kind, rail, speed, NowMs()}).first;
        LOGI("Train %d of player %d (kind %d, track %d) copied on track %d", id, from, kind, track, Track(first));
    }
    it->second.rail = rail;
    it->second.speed = speed;
    it->second.ms = NowMs();
}

// the partner near the host makes no random trains: the host's game makes them for both
void SwitchRandomTrains() {
    bool off = false;
    if (!Net().IsHost()) {
        RemotePlayer* host = Players::Instance().Get(Net().HostId());
        CPlayerPed* me = FindPlayerPed(0);
        off = host && host->ped && (host->ped->GetPosition() - me->GetPosition()).Length2D() < kOwnRandomFrom;
    }
    if (off == g_randomOff) return;
    Ref<uint8_t>(kDisableRandomTrains) = off ? 1 : 0;
    g_randomOff = off;
}

class Trains : public Module {
public:
    const char* Name() const override { return "Trains"; }

    void OnInstallHooks() override {
        mem::HookFn(0x6F7550, &HookCreateMissionTrain, &g_origCreateTrain, "CTrain::CreateMissionTrain");
        Net().SetHandler(proto::G_TRAIN, &HandleTrain);
    }
    void OnSessionEnd() override {
        RemoveCopies();
        ForgetDriven();
        g_ids.clear();
        if (g_randomOff) Ref<uint8_t>(kDisableRandomTrains) = 0, g_randomOff = false;
    }
    void OnPeerLeave(uint8_t id) override { RemoveCopies(id); }
    void OnPeerResync(uint8_t id) override { RemoveCopies(id); }  // its game started over
    void OnGameStarted() override {
        g_copies.clear();  // the old world's trains are gone
        g_driven.clear();
        g_entering = nullptr;
        g_kinds.clear();
        g_ids.clear();
        g_randomOff = false;
    }

    void OnPreProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        SwitchRandomTrains();
        // where the real train is now: its place and speed as sent, moved on by the time since (a step is 20 ms)
        for (auto it = g_copies.begin(); it != g_copies.end();) {
            Copy& c = it->second;
            if (!IsEngine(c.engine) || NowMs() - c.ms > 3000) {
                RemoveCopy(c);
                it = g_copies.erase(it);
                continue;
            }
            if (WeDrive(c.engine)) {  // our pad moves it (CTrain::ProcessControl), its owner follows
                c.ms = NowMs();
                ++it;
                continue;
            }
            // open while no player drives it and no story mission of the host's runs (its trains are its own)
            CPed* drv = c.engine->Driver();
            c.engine->DoorLock() = (drv && drv->IsPlayer()) || HostMissionRecent(0) ? 2 : 1;  // CARLOCK_LOCKED, CARLOCK_UNLOCKED
            Speed(c.engine) = c.speed;
            RailDistance(c.engine) = c.rail + c.speed * (float)(NowMs() - c.ms) / 20.0f;
            ++it;
        }
        // ours the other player drives: where its game has it, kept as a mission's from the clear-out far from our camera
        // (a mission's clean-up releases all mission trains: set every frame)
        for (auto it = g_driven.begin(); it != g_driven.end();) {
            CVehicle* t = it->first;
            Driven& d = it->second;
            if (!IsEngine(t) || NowMs() - d.ms > 1000) {
                if (IsEngine(t) && !d.wasMission) Field<uint16_t>(t, 0x5B8) &= ~kMissionTrain;
                it = g_driven.erase(it);
                continue;
            }
            Field<uint16_t>(t, 0x5B8) |= kMissionTrain;
            Speed(t) = d.speed;
            RailDistance(t) = d.rail + d.speed * (float)(NowMs() - d.ms) / 20.0f;
            ++it;
        }
    }

    void OnPostProcess() override {
        if (!Net().Online() || Net().Peers().empty() || !core::IsPlayerReady()) return;
        SendEntering();
        if (NowMs() - g_sendMs < 100) return;
        g_sendMs = NowMs();
        SendOurs();
        SendDriven();
    }

    static Trains& Instance() {
        static Trains m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Trains::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
