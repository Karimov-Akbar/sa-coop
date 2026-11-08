// A race with the players who joined it (Race.h). CPRACE keeps its racers in arrays of its mission locals (the player among them at 442@) and
// its route in three more: what it compares is read from there.
#include "features/story/race/Race.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop::race {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { OP_STATE = 1, OP_DONE = 2, OP_WATCH = 3 };
constexpr uintptr_t kMissionLocals = 0xA48960;  // CTheScripts::LocalVariablesForCurrentMission
// CPRACE's locals: its stage (7..10 the countdown, 11 and 12 the race, 13 its player finished), the laps, the route's
// checkpoints (a lap), the racers, the player's index among them, a checkpoint's radii, the race checkpoint's type and
// size; arrays of 16 racers: the car, the ped, the laps done, the next checkpoint and its place; the route: x, y, z
constexpr int kAhead = 229;  // the racers ahead of its player, as its screen shows the place (450@ = 229@ + 1)
constexpr int kStage = 49, kLaps = 224, kCheckpoints = 250, kRacers = 256, kPlayer = 442, kRadius = 322, kType = 205,
              kSize = 325, kRacerPed = 395, kRacerLaps = 273, kRacerNext = 257, kNextX = 331, kNextY = 347, kNextZ = 363,
              kRouteX = 454, kRouteY = 536, kRouteZ = 618, kRoute = 82;
constexpr uint64_t kStateMs = 250, kQuietMs = 3000;

int32_t Int(int n) { return Ref<int32_t>(kMissionLocals + (uintptr_t)n * 4); }
float Float(int n) { return Ref<float>(kMissionLocals + (uintptr_t)n * 4); }
CVector Route(int i) { return {Float(kRouteX + i), Float(kRouteY + i), Float(kRouteZ + i)}; }

void* Cprace() {
    void* s = RunningMission();
    return s && !_strnicmp(Field<char[8]>(s, 0x08), "cprace", 8) ? s : nullptr;
}

struct Progress {
    bool done = false;
    int laps = 0, next = 0;
    float dist = 0;
};
// CPRACE's rule (its racers' loop at 0x10173D): who finished, then more laps, then a later checkpoint, then nearer to it
bool Ahead(const Progress& a, const Progress& b) {
    if (a.done != b.done) return a.done;
    if (a.laps != b.laps) return a.laps > b.laps;
    if (a.next != b.next) return a.next > b.next;
    return a.dist < b.dist;
}
bool RacerOf(int i, Progress& p) {
    CPed* ped = GetPed(Int(kRacerPed + i));
    if (!ped || ped->Health() <= 0.0f) return false;  // CPRACE leaves a dead one out too
    p.laps = Int(kRacerLaps + i), p.next = Int(kRacerNext + i);
    p.done = p.laps >= Int(kLaps) || Float(kNextZ + i) == 99999.0f;  // a finished one's mark
    p.dist = (ped->GetPosition() - CVector{Float(kNextX + i), Float(kNextY + i), Float(kNextZ + i)}).Length();
    return true;
}

// ---------------------------------------------------------------- the owner
struct Owner {
    Progress them;      // that player's run
    bool racing = false;
    bool told = false;  // its end said
    uint64_t sentMs = 0;
};
std::map<uint8_t, Owner> g_own;  // who joined our race

void SendDone(uint8_t with, Owner& o, uint8_t place, uint8_t racers, bool finished) {
    Writer w(proto::G_RACE);
    w.U8(OP_DONE), w.U8(with), w.U8(place), w.U8(racers), w.U8(finished ? 1 : 0);
    Net().Send(proto::CH_RELIABLE, w);
    o.told = true;
    LOGI("Race for two: %s %s, place %u of %u", Net().PeerName(with).c_str(), finished ? "finished" : "stopped", place, racers);
}

// the places now: that player's and ours among the racers and those who joined
void Places(const Owner& o, int& theirs, int& ours, int& racers) {
    int n = std::clamp(Int(kRacers), 0, 16), me = Int(kPlayer);
    Progress own;
    bool haveOwn = me >= 0 && me < n && RacerOf(me, own);
    theirs = ours = 1, racers = n + (int)g_own.size();
    for (int i = 0; i < n; ++i) {
        Progress p;
        if (!RacerOf(i, p)) continue;
        if (Ahead(p, o.them)) ++theirs;
        if (i != me && haveOwn && Ahead(p, own)) ++ours;
    }
    for (auto& [id, other] : g_own) {
        if (&other != &o && Ahead(other.them, o.them)) ++theirs;
        if (haveOwn && Ahead(other.them, own)) ++ours;
    }
}

void OwnerRun(uint8_t with, Owner& o);

void OwnerUpdate() {
    void* s = Cprace();
    std::vector<uint8_t> helpers = s && SideLeading() ? SideHelpers() : std::vector<uint8_t>{};
    for (auto it = g_own.begin(); it != g_own.end();) {  // its end, or that player left it
        if (std::find(helpers.begin(), helpers.end(), it->first) != helpers.end()) {
            ++it;
            continue;
        }
        if (it->second.racing && !it->second.told) {
            int theirs, ours, racers;
            Places(it->second, theirs, ours, racers);
            SendDone(it->first, it->second, (uint8_t)theirs, (uint8_t)racers, false);
        }
        it = g_own.erase(it);
    }
    for (uint8_t with : helpers) {
        if (!g_own.count(with)) LOGI("Race for two with %s", Net().PeerName(with).c_str());
        OwnerRun(with, g_own[with]);
    }
}

void OwnerRun(uint8_t with, Owner& o) {
    const RemotePlayer* rp = Players::Instance().Get(with);
    if (!rp || !rp->ped) return;
    int stage = Int(kStage), count = Int(kCheckpoints), laps = Int(kLaps);
    if (stage < 7 || count <= 0 || count > kRoute || laps <= 0) return;
    Progress& t = o.them;
    CVector at = rp->ped->GetPosition();
    if (stage < 11) {
        t = {};  // the countdown: from the start line
    } else if (!t.done && !o.told) {
        o.racing = true;
        CVector next = Route(t.next), d = at - next;
        if (std::fabs(d.x) < Float(kRadius) && std::fabs(d.y) < Float(kRadius + 1) && std::fabs(d.z) < Float(kRadius + 2)) {
            if (++t.next >= count) t.next = 0, ++t.laps;  // as CPRACE does for its racers
            if (t.laps >= laps) {
                int done = 1, n = std::clamp(Int(kRacers), 0, 16);  // the ones that finished before it
                for (int i = 0; i < n; ++i)
                    if (Progress p; RacerOf(i, p) && p.done) ++done;
                for (auto& [id, other] : g_own) done += &other != &o && other.them.done;
                t.done = true;
                SendDone(with, o, (uint8_t)done, (uint8_t)(n + (int)g_own.size()), true);
                return;
            }
        }
    }
    if (o.told || NowMs() - o.sentMs < kStateMs) return;
    o.sentMs = NowMs();
    t.dist = (at - Route(t.next)).Length();
    int theirs, ours, racers;
    Places(o, theirs, ours, racers);
    bool last = t.laps == laps - 1 && t.next == count - 1;  // its next one is the finish
    CVector next = Route(t.next), after = Route((t.next + 1) % count);
    Writer w(proto::G_RACE);
    w.U8(OP_STATE), w.U8(with), w.U8((uint8_t)theirs), w.U8((uint8_t)racers), w.U8((uint8_t)ours);
    w.U8((uint8_t)std::min(t.laps + 1, laps)), w.U8((uint8_t)laps);
    w.Vec({next.x, next.y, next.z}), w.Vec({after.x, after.y, after.z});
    w.U8(last ? 1 : (uint8_t)std::clamp(Int(kType), 0, 8)), w.F32(last ? 6.0f : Float(kSize));
    Net().Send(proto::CH_STATE, w);
}

// Our race for the other player riding with us or near us without racing itself: our place as our screen shows it,
// twice a second
uint64_t g_watchSentMs = 0;
void SendWatch() {
    void* s = Cprace();
    int stage = s ? Int(kStage) : 0, racers = s ? Int(kRacers) : 0;
    if (!s || stage < 11 || stage > 13 || racers < 2 || racers > 16 || NowMs() - g_watchSentMs < 500) return;
    g_watchSentMs = NowMs();
    Writer w(proto::G_RACE);
    w.U8(OP_WATCH), w.U8(0xFF), w.U8((uint8_t)std::clamp(Int(kAhead) + 1, 1, racers)), w.U8((uint8_t)racers);
    Net().Send(proto::CH_STATE, w);
}

// ---------------------------------------------------------------- the one who joined
struct Joined {
    uint8_t from = 0xFF;
    uint64_t heardMs = 0;
    int place = 0, racers = 0, ownerPlace = 0, lap = 0, laps = 0;
    CVector next;
    int checkpoint = -1, blip = -1;
} g_join;

void Unmark() {
    if (g_join.checkpoint != -1) Cmd(0x06D6).I(g_join.checkpoint).Run();  // DELETE_CHECKPOINT
    if (g_join.blip != -1) Cmd(0x0164).I(g_join.blip).Run();               // REMOVE_BLIP
    g_join.checkpoint = g_join.blip = -1;
}
void JoinEnd() {
    if (g_join.from == 0xFF) return;
    Unmark();
    g_join = {};
}

void ReadState(uint8_t from, Reader& r) {
    int place = r.U8(), racers = r.U8(), ownerPlace = r.U8(), lap = r.U8(), laps = r.U8();
    Vec3 n = r.Vec(), a = r.Vec();
    int type = r.U8();
    float size = r.F32();
    if (!r.Ok() || !std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(a.z) || !std::isfinite(size) || size <= 0.0f || size > 50.0f || type > 8 || racers > 19)
        return;
    if (g_join.from != from) {
        JoinEnd();
        g_join.from = from;
        ui::TheChat().AddSystem(TR("Гонка вдвоём: вы тоже соревнуетесь, место видно справа.",
                                   "A race for two: you race too, your place shows on the right."));
        LOGI("Race for two at %s's race", Net().PeerName(from).c_str());
    }
    g_join.heardMs = NowMs();
    g_join.place = place, g_join.racers = racers, g_join.ownerPlace = ownerPlace, g_join.lap = lap, g_join.laps = laps;
    CVector next{n.x, n.y, n.z};
    if (g_join.checkpoint != -1 && (next - g_join.next).Length() < 0.5f) return;
    Unmark();  // our next checkpoint moved on
    g_join.next = next;
    Cmd(0x06D5).I(type).F(n.x).F(n.y).F(n.z).F(a.x).F(a.y).F(a.z).F(size).Out(&g_join.checkpoint).Run();  // CREATE_CHECKPOINT
    Cmd(0x018A).F(n.x).F(n.y).F(n.z).Out(&g_join.blip).Run();  // ADD_BLIP_FOR_COORD
}

// "2-е место из 5": Russian puts the ending after the number
std::string Place(int place, int of) { return Format(TR("%d-е место из %d", "place %d of %d"), place, of); }

// ---------------------------------------------------------------- one riding with the racer or near it
struct Watch {
    uint8_t from = 0xFF;
    uint64_t heardMs = 0;
    int place = 0, racers = 0;
} g_watch;
void ReadWatch(uint8_t from, Reader& r) {
    int place = r.U8(), racers = r.U8();
    if (!r.Ok() || racers < 2 || racers > 16 || place < 1 || place > racers || g_join.from == from) return;  // racing ourselves
    const RemotePlayer* rp = Players::Instance().Get(from);
    CPlayerPed* me = FindPlayerPed(0);
    if (!rp || !rp->ped || !me || rp->ped->AreaCode() != me->AreaCode() || (rp->ped->GetPosition() - me->GetPosition()).Length() > 150.0f)
        return;  // elsewhere: its race is not ours to watch
    if (g_watch.from != from) LOGI("Watching %s's race", Net().PeerName(from).c_str());
    g_watch = {from, NowMs(), place, racers};
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8();
    if (!r.Ok() || (to != Net().MyId() && to != 0xFF) || from == Net().MyId() || !core::IsPlayerReady()) return;
    if (op == OP_WATCH) return ReadWatch(from, r);
    if (to != Net().MyId()) return;
    if (op == OP_STATE) return ReadState(from, r);
    if (op != OP_DONE || from != g_join.from) return;
    int place = r.U8(), racers = r.U8();
    bool finished = r.U8() != 0;
    if (!r.Ok()) return;
    ui::TheChat().AddSystem(Format(finished ? TR("Финиш: %s.", "Finish: %s.") : TR("Гонка окончена: %s.", "The race is over: %s."),
                                   Place(place, racers).c_str()));
    LOGI("Race for two: %s, place %d of %d", finished ? "finished" : "over", place, racers);
    JoinEnd();
}

class RaceModule : public Module {
public:
    const char* Name() const override { return "Race"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_RACE, &Handle); }
    void OnSessionEnd() override { JoinEnd(), g_own.clear(); }
    void OnGameShutdown() override { g_join = {}, g_own.clear(); }  // the world's checkpoints and blips go with it
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        OwnerUpdate();
        SendWatch();
        if (g_join.from != 0xFF && NowMs() - g_join.heardMs > kQuietMs) JoinEnd();  // its race is over there
        if (g_watch.from != 0xFF && NowMs() - g_watch.heardMs > 2000) g_watch = {};
    }
    void OnRender() override {
        if (!core::IsPlayerReady() || MenuActive()) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), x = g.Width() - 48 * s, y = g.Height() * 0.34f;
        if (g_join.from == 0xFF && g_watch.from != 0xFF) {  // the place of the one we ride with
            g.Text(ui::FONT_MENU_SMALL, x, y, Format(TR("Место игрока %s", "%s's place"), Net().PeerName(g_watch.from).c_str()),
                   0xFFE2C063, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_MENU_SMALL);
            g.GameText(ui::GAME_FONT_PRICEDOWN, x, y, Format("%d/%d", g_watch.place, g_watch.racers), 52 * s, 0xFFFFFFFF, ui::TEXT_RIGHT);
            return;
        }
        if (g_join.from == 0xFF || !g_join.racers) return;
        g.Text(ui::FONT_MENU_SMALL, x, y, TR("Место", "Place"), 0xFFE2C063, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
        y += g.LineHeight(ui::FONT_MENU_SMALL);
        g.GameText(ui::GAME_FONT_PRICEDOWN, x, y, Format("%d/%d", g_join.place, g_join.racers), 52 * s, 0xFFFFFFFF, ui::TEXT_RIGHT);
        y += 56 * s;
        if (g_join.laps > 1) {
            g.Text(ui::FONT_SMALL, x, y, Format(TR("Круг %d из %d", "Lap %d of %d"), g_join.lap, g_join.laps), 0xFFFFFFFF,
                   ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_SMALL);
        }
        g.Text(ui::FONT_SMALL, x, y, Format("%s: %d/%d", Net().PeerName(g_join.from).c_str(), g_join.ownerPlace, g_join.racers),
               0xFFB4C8DC, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
    }

    static RaceModule& Instance() {
        static RaceModule m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&RaceModule::Instance()); }
} g_reg;
}  // namespace

bool ForTwo() { return !g_own.empty() && Cprace(); }

}  // namespace coop::race
