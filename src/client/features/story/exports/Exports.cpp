// The export list at the Easter Basin docks for two. Main.scm's IMPEXPM keeps it in globals: the stage (three lists of ten
// cars), what is delivered of this one, and the imports table that fills up with every delivery. Each game ran its own:
// a car was crossed out and paid in the game of who delivered it. The other game did the same only with its player in
// the marker, its script taking the car's copy for a car brought to it, so two players far apart had two lists (04.11).
// The list is the world's now, the host's. A delivery goes to the other game, which crosses the car out, pays the same
// and redraws its board. The host says the list now and then: a partner's world can be older than the host's game.
#include <algorithm>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
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

constexpr uintptr_t kActiveScripts = 0xA8B42C;  // CTheScripts::pActiveScripts
constexpr uint16_t kSetUp = 1181;               // $4724: 99 once IMPEXPM has made its generators and tables
constexpr uint16_t kStage = 1049, kFlags = 1060, kImports = 1076, kAllDone = 1184;  // $4196, $4240, $4304, $4736
constexpr uint16_t kGenerators = 2830;          // $11320: the generators of the 30 cars, as many as the lists have
constexpr int kCars = 10, kImportSlots = 42, kLastStage = 2;
constexpr int kMode = 15, kStep = 16, kPaid = 29;  // 15@: 0 exports, 1 imports; 16@: a delivery's steps; 29@: what it pays
// IMPEXPM's loop right after its WAIT, and where it sets up its board and crosses after every delivery
constexpr uintptr_t kIdleIp = 0x1DCAF, kRedrawIp = 0x1D8D5;
constexpr int kProgressSlots[] = {16, 28, 40};  // the imports' slots whose delivery is PLAYER_MADE_PROGRESS
constexpr int32_t kMaxPay = 1000000;            // a car's value and the last list's $200000
constexpr int32_t kMaxMoney = 999999999;
constexpr int kExportedStat = 213;
constexpr uint64_t kResendMs = 15000;
constexpr uint64_t kHoldMs = 15000;  // a delivery of ours lasts seconds: its step can stay up after a death in it

struct List {
    int32_t stage, allDone, flags[kCars], imports[kImportSlots];
};
struct Incoming {
    int8_t car;
    int32_t paid;
    List list;
    uint64_t ms;
};

std::vector<Incoming> g_incoming;
List g_seen{}, g_sent{};
bool g_based = false, g_sentAny = false;
uint64_t g_sentMs = 0;

int32_t& Global(uint16_t n) { return Ref<int32_t>(addr::ScriptSpace + (uintptr_t)n * 4); }
int32_t Local(void* s, int n) { return Field<int32_t>(s, 0x3C + n * 4); }  // CRunningScript::m_aLocalVars
bool Same(const List& a, const List& b) { return !memcmp(&a, &b, sizeof a); }

void* Thread() {
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00))
        if (!_strnicmp(Field<char[8]>(s, 0x08), "impexpm", 8)) return s;
    return nullptr;
}

List Now() {
    List l;
    l.stage = Global(kStage);
    l.allDone = Global(kAllDone);
    for (int i = 0; i < kCars; ++i) l.flags[i] = Global(kFlags + i);
    for (int i = 0; i < kImportSlots; ++i) l.imports[i] = Global(kImports + i);
    return l;
}

void Put(const List& l) {
    Global(kStage) = l.stage;
    Global(kAllDone) = l.allDone;
    for (int i = 0; i < kCars; ++i) Global(kFlags + i) = l.flags[i];
    for (int i = 0; i < kImportSlots; ++i) Global(kImports + i) = l.imports[i];
}

bool Valid(const List& l) {
    if (l.stage < 0 || l.stage > kLastStage || (l.allDone != 0 && l.allDone != 1)) return false;
    return std::all_of(std::begin(l.flags), std::end(l.flags), [](int32_t f) { return f == 0 || f == 1; }) &&
           std::all_of(std::begin(l.imports), std::end(l.imports), [](int32_t m) { return m == -1 || (m >= 400 && m <= 611); });
}

int Filled(const List& l) { return (int)(std::find(std::begin(l.imports), std::end(l.imports), -1) - std::begin(l.imports)); }

void Send(const List& l, int car, int32_t paid) {
    Writer w(proto::G_EXPORT);
    w.I8((int8_t)car);
    w.I32(paid);
    w.Bytes(&l, sizeof l);
    Net().Send(proto::CH_RELIABLE, w);
}

// a full list goes on to the next as at the board (the script's list set-up): far from the docks ours would stay behind
void NextList(List& l) {
    if (!std::all_of(std::begin(l.flags), std::end(l.flags), [](int32_t f) { return f != 0; })) return;
    if (l.stage == kLastStage) {
        l.allDone = 1;
        return;
    }
    ++l.stage;
    std::fill(std::begin(l.flags), std::end(l.flags), 0);
}

// What is crossed out stops spawning, and the board by a player at the docks shows it: the script's own way back to its
// board, as after a delivery. Far from the docks it sets the board up on the player's way in, in a menu or a delivery
// on the way out of them.
void Show(void* s, const List& l) {
    for (int i = 0; i < kCars; ++i)
        if (int g = Global(kGenerators + l.stage * kCars + i); l.flags[i] && g >= 0 && g < 500) Cmd(0x014C).I(g).I(0).Run();  // SWITCH_CAR_GENERATOR
    uint8_t*& ip = Field<uint8_t*>(s, 0x14);  // CRunningScript::m_pCurrentIP
    if (ip == (uint8_t*)(addr::ScriptSpace + kIdleIp) && !Local(s, kMode) && !Local(s, kStep)) ip = (uint8_t*)(addr::ScriptSpace + kRedrawIp);
}

// The other game's delivery. A player in the marker delivers the same car in its own game, and both are paid so: with the
// car crossed out here this one is not counted twice.
void TakeDelivery(void* s, const Incoming& in) {
    List l = Now();
    if (in.list.stage < l.stage || (in.list.stage == l.stage && l.flags[in.car])) return;
    int before = Filled(l), after = Filled(in.list);
    if (in.list.stage > l.stage) memcpy(l.flags, in.list.flags, sizeof l.flags);
    else
        for (int i = 0; i < kCars; ++i) l.flags[i] |= in.list.flags[i];
    l.stage = in.list.stage;
    l.allDone |= in.list.allDone;
    memcpy(l.imports, in.list.imports, sizeof l.imports);
    List shown = l;
    NextList(l);
    Put(l);
    g_seen = l;
    Cmd(0x0623).I(kExportedStat).I(1).Run();  // INCREMENT_INT_STAT
    if (Net().IsHost())  // the world's progress: the partner's follows it
        for (int slot : kProgressSlots)
            if (before <= slot && slot < after) Cmd(0x030C).I(1).Run();  // PLAYER_MADE_PROGRESS
    int32_t& money = PlayerInfos()[0].money;
    money = (int32_t)std::min<int64_t>((int64_t)money + in.paid, kMaxMoney);
    if (!Net().IsHost()) SaveProfileNow();  // a world reload puts the profile's money back
    Show(s, shown);
    ui::TheChat().AddSystem(Format(TR("Машина сдана на экспорт. Вам $%d.", "A car was exported. You get $%d."), in.paid));
    LOGI("The other player's export: car %d of list %d, $%d", in.car, in.list.stage, in.paid);
}

void TakeList(void* s, const List& list) {
    if (Same(list, Now())) return;
    Put(list);
    g_seen = list;
    Show(s, list);
    LOGI("The host's export list: list %d", list.stage);
}

void Handle(uint8_t from, Reader& r) {
    Incoming in;
    in.car = r.I8();
    in.paid = r.I32();
    std::vector<uint8_t> bytes = r.Bytes(sizeof(List));
    if (!r.Ok() || from == Net().MyId() || bytes.size() != sizeof(List) || in.car < -1 || in.car >= kCars || in.paid < 0 ||
        in.paid > kMaxPay || g_incoming.size() >= 32)
        return;
    memcpy(&in.list, bytes.data(), sizeof(List));
    if (!Valid(in.list) || (in.car < 0 && (from != Net().HostId() || Net().IsHost()))) return;
    in.ms = NowMs();
    g_incoming.push_back(in);
}

class Exports : public Module {
public:
    const char* Name() const override { return "Exports"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_EXPORT, &Handle); }
    void OnSessionEnd() override { g_incoming.clear(), g_based = g_sentAny = false; }
    void OnGameStarted() override { g_based = g_sentAny = false; }

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || !flow::InSharedWorld() || flow::HostWorldLoading() || Global(kSetUp) != 99) return;
        void* s = Thread();
        if (!s) return;
        List now = Now();
        if (!g_based) g_seen = now, g_based = true;
        for (int i = 0; i < kCars; ++i)
            if (now.stage == g_seen.stage && now.flags[i] && !g_seen.flags[i]) {
                Send(now, i, Local(s, kPaid));
                g_sent = now;  // it carries the list
                LOGI("Our export: car %d of list %d, $%d", i, now.stage, Local(s, kPaid));
            }
        g_seen = now;
        // Our delivery of the same car counts it itself: looked at after it
        bool delivering = !Local(s, kMode) && Local(s, kStep) > 0;
        if (!g_incoming.empty() && (!delivering || NowMs() - g_incoming.front().ms >= kHoldMs)) {
            for (const Incoming& in : g_incoming) {
                if (in.car >= 0) TakeDelivery(s, in);
                else TakeList(s, in.list);
            }
            g_incoming.clear();
            now = Now();
        }
        if (Net().IsHost() && !Net().Peers().empty() && (!g_sentAny || !Same(now, g_sent) || NowMs() - g_sentMs >= kResendMs)) {
            Send(now, -1, 0);
            g_sent = now, g_sentAny = true, g_sentMs = NowMs();
        }
    }
};

struct Reg {
    Reg() {
        static Exports m;
        core::RegisterModule(&m);
    }
} g_reg;
}  // namespace
}  // namespace coop
