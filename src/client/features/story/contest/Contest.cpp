// Side activities as contests of two players (Contest.h).
//  * A run: this player plays a contest's activity in its own game, from its start to its end: a thread (BBCHAL, a
//    casino table's while its table is in use, DANCE), a side mission (POOL2, KICKSTA, BLOOD, CPRACE, TRIA) or the
//    lowrider meet's contest (LOWGAME in LOWR). Its result is the game's own: basketball's points ($7696), the dance's
//    ($21184), the lowrider contest's ($4072), Kickstart's ($33048), the money a table game won or lost, the time a
//    race, a triathlon or Blood Ring took to its end and the place there. main.scm's globals are read by byte offset, a
//    mission's locals in CTheScripts::LocalVariablesForCurrentMission (wiped at its launch).
//  * The challenge (G_CONTEST) is made in a run's first 20 s and binds it. The other's yes binds its run of the same
//    activity (and race): the one it plays now, or the next one it starts within 3 minutes. Each game sends its run's
//    state, and with both done each game tells who won. A table game left with no bet made does not count.
#include "features/story/contest/Contest.h"

#include <cmath>
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "features/story/missions/Missions.h"
#include "features/story/pool/Pool.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::contest {

namespace {
using namespace game;

enum : uint8_t { OP_CALL = 1, OP_ACCEPT = 2, OP_DECLINE = 3, OP_STATE = 4, OP_CANCEL = 5 };
enum : uint8_t { BASKET = 1, POOL, BLACKJACK, ROULETTE, WHEEL, DANCE, LOWRIDER, KICKSTART, BLOODRING, RACE, TRIATHLON, KINDS };
enum : uint8_t { WAITING = 0, PLAYING = 1, DONE = 2 };
enum : uint8_t { WHY_NO = 0, WHY_BUSY = 1, WHY_LATE = 2, WHY_MISSION = 3, WHY_TAKEN = 4 };
enum class Metric { POINTS, MONEY, TIME };

constexpr uint64_t kCallMs = 20000, kAnswerMs = 20000, kStartMs = 180000, kResultMs = 10000;
constexpr uintptr_t kActiveScripts = 0xA8B42C;  // CTheScripts::pActiveScripts
constexpr uintptr_t kMissionLocals = 0xA48960;  // CTheScripts::LocalVariablesForCurrentMission
constexpr uint32_t kBasketPoints = 7696, kDancePoints = 21184, kLowPoints = 4072, kKickPoints = 33048;
// the casino tables' scripts run near their tables (object brains): each says when its game is on
constexpr uint32_t kBlackjackTable = 36868, kWheelTable = 37760, kRoulettePlays = 13956;
// CPRACE: the race ($1408), its clock ($27840: when it started, the time itself against the clock), its stage 49@ (13
// on: over the line), the racers 256@ (1: against the clock), the place 450@. TRIA: which one ($7248), its start 767@
// (game ms), the place 942@. BLOOD: 91@ (1 won, 2 lost).
constexpr uint32_t kRaceIndex = 1408, kRaceClock = 27840, kTriathlonIndex = 7248;

int32_t Global(uint32_t off) { return Ref<int32_t>(addr::ScriptSpace + off); }
int32_t Local(int n) { return Ref<int32_t>(kMissionLocals + (uintptr_t)n * 4); }
uint32_t GameMs() { return Ref<uint32_t>(addr::TimeInMs); }
bool Named(void* script, const char* name) { return !_strnicmp(Field<char[8]>(script, 0x08), name, 8); }

Metric MetricOf(uint8_t kind) {
    switch (kind) {
        case POOL: case BLACKJACK: case ROULETTE: case WHEEL: return Metric::MONEY;
        case BLOODRING: case RACE: case TRIATHLON: return Metric::TIME;
        default: return Metric::POINTS;
    }
}

// what the texts call it ("Соревнование: бильярд"), picked when shown: the language can be switched in the menu
std::string KindName(uint8_t kind, int32_t variant) {
    switch (kind) {
        case BASKET: return TR("баскетбол", "basketball");
        case POOL: return TR("бильярд", "pool");
        case BLACKJACK: return TR("блэкджек", "blackjack");
        case ROULETTE: return TR("рулетка", "roulette");
        case WHEEL: return TR("колесо фортуны", "wheel of fortune");
        case DANCE: return TR("танцы", "dancing");
        case LOWRIDER: return TR("лоурайдеры", "lowriders");
        case KICKSTART: return "Kickstart";
        case BLOODRING: return "Blood Ring";
        case RACE: return variant == 25 ? "8-Track" : variant == 26 ? "Dirt Track" : TR("гонка", "race");
        case TRIATHLON:
            return variant == 2 ? TR("Beat the Cock! в Ред-Каунти", "Beat the Cock! in Red County")
                                : TR("Beat the Cock! в Лос-Сантосе", "Beat the Cock! in Los Santos");
    }
    return "?";
}

// what the one who said yes is to do
std::string HowTo(uint8_t kind, const std::string& caller) {
    switch (kind) {
        case BASKET: return TR("Начните испытание на очки на любой баскетбольной площадке.", "Start the points challenge on any basketball court.");
        case POOL: return TR("Сыграйте партию на деньги за другим столом.", "Play a game for money at another table.");
        case BLACKJACK: case ROULETTE: case WHEEL:
            return TR("Сядьте за такой же стол в казино: считается выигрыш или проигрыш за столом.",
                      "Sit at the same kind of casino table: what you win or lose there counts.");
        case DANCE: return TR("Потанцуйте в любом клубе: считаются очки.", "Dance in any club: the points count.");
        case LOWRIDER: return TR("Сыграйте конкурс на встрече лоурайдеров: считаются очки.", "Play the contest at the lowrider meet: the points count.");
        case KICKSTART: return TR("Пройдите Kickstart на стадионе: считаются очки.", "Play Kickstart at the stadium: the points count.");
        case BLOODRING: return TR("Пройдите Blood Ring на стадионе: считается время до победы.", "Play Blood Ring at the stadium: the time to win counts.");
        case RACE:
            return Format(TR("Начните ту же гонку с того же старта, что и игрок %s: считается время.",
                             "Start the same race from the same start as %s: the time counts."),
                          caller.c_str());
        case TRIATHLON: return TR("Начните тот же триатлон: считается время.", "Start the same triathlon: the time counts.");
    }
    return "";
}

struct Result {
    uint8_t state = WAITING;
    bool ok = false;      // TIME: over the line (Blood Ring: won)
    int32_t value = 0;    // points, money won (lost: below 0), ms
    int32_t place = 0;    // a race's
};

// ---------------------------------------------------------------- this player's run
struct Run {
    uint8_t kind = 0;
    int32_t variant = 0;
    uint32_t gameStart = 0;   // GameMs at its start; Blood Ring's GO (its clock shown) when seen
    bool clockSeen = false;
    int32_t moneyStart = 0;
    bool moneyMoved = false;  // a table game with no bet made does not count
    bool finished = false;    // TIME: over the line
    int32_t value = 0, place = 0;
    uint64_t callUntil = 0;   // its challenge can be made until then
    bool bound = false;       // the contest's
} g_run;

// ---------------------------------------------------------------- the contest
enum class Phase { OFF, CALLING, CALLED, ON };
struct Contest {
    Phase phase = Phase::OFF;
    uint8_t with = 0xFF;      // the other player (CALLING: none yet)
    uint8_t kind = 0;
    int32_t variant = 0;
    uint64_t until = 0;       // CALLING, CALLED: the answer's time; ON: our run's start's
    int declines = 0;         // CALLING: the other players who said no
    Result me, them;
    uint64_t sentMs = 0;
    int32_t sentValue = INT32_MIN;
    uint8_t sentState = 0xFF;
} g_c;
BoardView g_result;  // the result on top of the screen till then
uint64_t g_resultUntil = 0;

// a story mission runs: the host's own, or the host's in the partner's game (its dance and lowriders are the story's,
// and the partner's dance there ends a moment after the host's mission)
bool StoryRuns() {
    if (!Net().IsHost()) return HostMissionRecent(5000);
    void* m = RunningMission();
    return m && StoryMission(m);
}

std::string Who(uint8_t id) { return Net().PeerName(id); }
// "игрока Женя" with one other player, "других игроков" with more
std::string Others() {
    if (Net().Peers().size() == 1) return Format(TR("игрока %s", "%s"), Who(Net().Peers().begin()->first).c_str());
    return TR("других игроков", "the other players");
}

void Send(uint8_t op, uint8_t to, uint8_t why = WHY_NO) {
    Writer w(proto::G_CONTEST);
    w.U8(op);
    w.U8(to);
    w.U8(g_c.kind);
    w.I32(g_c.variant);
    w.U8(g_c.me.state);
    w.U8(g_c.me.ok);
    w.I32(g_c.me.value);
    w.I32(g_c.me.place);
    w.U8(why);
    Net().Send(proto::CH_RELIABLE, w);
}

// The activity this game plays now (0: none) and its variant (the race, the triathlon): its own, never a story's, and
// none while it takes part in another's joint activity (its game runs nothing of it)
uint8_t Playing(int32_t& variant) {
    variant = 0;
    if (StoryRuns() || SideLeader() != 0xFF) return 0;
    void* mission = RunningMission();
    if (mission) {
        if (Named(mission, "pool2")) return POOL;
        if (Named(mission, "kicksta")) return KICKSTART;
        if (Named(mission, "blood")) return BLOODRING;
        if (Named(mission, "cprace")) return variant = Global(kRaceIndex), RACE;
        if (Named(mission, "tria")) return variant = Global(kTriathlonIndex), TRIATHLON;
    }
    bool meet = mission && Named(mission, "lowr");
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00)) {
        if (Named(s, "bbchal")) return BASKET;
        if (meet && Named(s, "lowgame")) return LOWRIDER;
        if (!mission && Named(s, "dance")) return DANCE;
        if (Named(s, "blackj") && Global(kBlackjackTable)) return BLACKJACK;
        if (Named(s, "roulete") && Global(kRoulettePlays)) return ROULETTE;
        if (Named(s, "wof") && Global(kWheelTable)) return WHEEL;
    }
    return 0;
}

// the run's result so far (the last points seen stay when its thread is gone)
Result RunResult(uint8_t state) {
    Result r;
    r.state = state;
    r.ok = MetricOf(g_run.kind) != Metric::TIME || g_run.finished;
    r.value = g_run.value;
    r.place = g_run.place;
    return r;
}

std::string ValueText(uint8_t kind, const Result& r) {
    switch (MetricOf(kind)) {
        case Metric::MONEY: return r.value >= 0 ? Format("+$%d", r.value) : Format("-$%d", -r.value);
        case Metric::TIME: {
            if (!r.ok) return TR("без результата", "no result");
            int t = r.value / 100;  // tenths
            std::string s = Format("%d:%02d.%d", t / 600, t / 10 % 60, t % 10);
            if (r.place > 0) s += Format(TR(" (%d-е место)", " (place %d)"), r.place);
            return s;
        }
        default: return std::to_string(r.value);
    }
}

// > 0: a is better
int Compare(uint8_t kind, const Result& a, const Result& b) {
    if (MetricOf(kind) == Metric::TIME) {
        if (a.ok != b.ok) return a.ok ? 1 : -1;
        if (!a.ok) return 0;
        return a.value < b.value ? 1 : a.value > b.value ? -1 : 0;
    }
    return a.value > b.value ? 1 : a.value < b.value ? -1 : 0;
}

void Reset() {
    if (g_run.bound) g_run.bound = false;
    g_c = {};
}

void Settle() {
    std::string name = Who(g_c.with), kind = KindName(g_c.kind, g_c.variant);
    int c = Compare(g_c.kind, g_c.me, g_c.them);
    std::string verdict = c > 0 ? TR("побеждаете вы", "you win") : c < 0 ? Format(TR("побеждает %s", "%s wins"), name.c_str())
                                                                        : TR("ничья", "a draw");
    std::string scores = Format(TR("Вы - %s, %s - %s.", "You: %s, %s: %s."), ValueText(g_c.kind, g_c.me).c_str(), name.c_str(),
                                ValueText(g_c.kind, g_c.them).c_str());
    ui::TheChat().AddSystem(Format(TR("Соревнование (%s): %s. %s", "Contest (%s): %s. %s"), kind.c_str(), verdict.c_str(), scores.c_str()));
    g_result = {};
    g_result.title = kind;
    g_result.verdict = c > 0 ? 1 : c < 0 ? -1 : 2;
    g_result.me = {TR("Вы", "You"), ValueText(g_c.kind, g_c.me), c > 0};
    g_result.them = {name, ValueText(g_c.kind, g_c.them), c < 0};
    g_resultUntil = NowMs() + kResultMs;
    LOGI("Contest %u with %s over: ours %d (%d), theirs %d (%d), %s", g_c.kind, name.c_str(), g_c.me.value, g_c.me.ok,
         g_c.them.value, g_c.them.ok, c > 0 ? "we won" : c < 0 ? "they won" : "a draw");
    Reset();
}

// our run's state to the other, on a change and twice a second while it goes on (its line shows our points)
void SendState(bool force) {
    if (g_c.phase != Phase::ON) return;
    uint64_t now = NowMs();
    bool changed = g_c.me.state != g_c.sentState || (g_c.me.value != g_c.sentValue && now - g_c.sentMs >= 500);
    if (!force && !changed) return;
    g_c.sentState = g_c.me.state, g_c.sentValue = g_c.me.value, g_c.sentMs = now;
    Send(OP_STATE, g_c.with);
    if (g_c.me.state == DONE && g_c.them.state == DONE) Settle();
}

void Cancel(uint8_t why, const std::string& line) {
    if (g_c.phase == Phase::ON || g_c.phase == Phase::CALLED) Send(g_c.phase == Phase::ON ? OP_CANCEL : OP_DECLINE, g_c.with, why);
    if (!line.empty()) ui::TheChat().AddSystem(line);
    LOGI("Contest %u called off (%u)", g_c.kind, why);
    Reset();
}

// ---- a run starts, goes on, ends
void StartRun(uint8_t kind, int32_t variant) {
    g_run = {};
    g_run.kind = kind, g_run.variant = variant;
    g_run.gameStart = GameMs();
    g_run.moneyStart = PlayerInfos()[0].money;
    g_run.callUntil = g_c.phase == Phase::OFF && !Net().Peers().empty() ? NowMs() + kCallMs : 0;
    LOGI("Contest activity %u (%d) started", kind, variant);
    if (g_c.phase != Phase::ON || g_c.kind != kind || g_c.me.state != WAITING) return;
    if (variant != g_c.variant) {  // another race than the contest's
        ui::TheChat().AddSystem(Format(kind == RACE ? TR("Это другая гонка. Нужна та же, что и у игрока %s.",
                                                         "This is another race. The contest is the same one as %s's.")
                                                    : TR("Это другой триатлон. Нужен тот же, что и у игрока %s.",
                                                         "This is another triathlon. The contest is the same one as %s's."),
                                       Who(g_c.with).c_str()));
        return;
    }
    g_run.bound = true;
    g_c.me = RunResult(PLAYING);
    SendState(true);
}

void UpdateRun() {
    int32_t money = PlayerInfos()[0].money;
    if (money != g_run.moneyStart) g_run.moneyMoved = true;
    switch (g_run.kind) {
        case BASKET: g_run.value = Global(kBasketPoints); break;
        case DANCE: g_run.value = Global(kDancePoints); break;
        case LOWRIDER: g_run.value = Global(kLowPoints); break;
        case KICKSTART: g_run.value = Global(kKickPoints); break;
        case POOL: case BLACKJACK: case ROULETTE: case WHEEL: g_run.value = money - g_run.moneyStart; break;
        case RACE:
            if (!g_run.finished && Local(49) >= 13) {
                g_run.finished = true;
                g_run.place = Local(450);
                g_run.value = Local(256) == 1 ? Global(kRaceClock) : (int32_t)(GameMs() - (uint32_t)Global(kRaceClock));
                LOGI("Contest race %d: over the line in %d ms, place %d", g_run.variant, g_run.value, g_run.place);
            }
            break;
        case BLOODRING:
            if (!g_run.finished && Local(91) == 1) {
                g_run.finished = true;
                g_run.value = (int32_t)(GameMs() - g_run.gameStart);  // from its GO (its clock shown), else its start
                LOGI("Contest Blood Ring won in %d ms", g_run.value);
            }
            break;
        default: break;  // the triathlon's line: AfterCommand
    }
    if (g_run.bound) {
        g_c.me = RunResult(PLAYING);
        SendState(false);
    }
}

void EndRun() {
    LOGI("Contest activity %u ended: %d", g_run.kind, g_run.value);
    if (g_run.bound && g_c.phase != Phase::OFF) {
        if (MetricOf(g_run.kind) == Metric::MONEY && !g_run.moneyMoved) {  // left with no bet made: the next one counts
            g_c.me = {};
            g_c.until = NowMs() + kStartMs;
            if (g_c.phase == Phase::ON) {
                SendState(true);
                ui::TheChat().AddSystem(TR("Эта игра не считается: ставок не было.", "This game does not count: no bets were made."));
            }
        } else {
            g_c.me = RunResult(DONE);
            SendState(true);
        }
    }
    g_run = {};
}

void Track() {
    int32_t variant = 0;
    uint8_t kind = core::IsPlayerReady() && flow::InSharedWorld() ? Playing(variant) : 0;
    if (g_run.kind && (kind != g_run.kind || variant != g_run.variant)) EndRun();
    if (kind && !g_run.kind) StartRun(kind, variant);
    if (g_run.kind) UpdateRun();
}

// ---- the challenge and its answer
void Call() {
    g_c = {};
    g_c.phase = Phase::CALLING;
    g_c.kind = g_run.kind, g_c.variant = g_run.variant;
    g_c.until = NowMs() + kAnswerMs;
    g_c.me = RunResult(PLAYING);
    g_run.bound = true;
    g_run.callUntil = 0;
    SideCallWithdraw();  // the run is not one to play together now
    Send(OP_CALL, 0xFF);
    LOGI("Contest %u (%d) offered", g_c.kind, g_c.variant);
}

void Answer(bool yes) {
    uint8_t from = g_c.with;
    std::string name = Who(from), kind = KindName(g_c.kind, g_c.variant);
    if (!yes) {
        Send(OP_DECLINE, from, WHY_NO);
        LOGI("Contest of %s declined", name.c_str());
        Reset();
        return;
    }
    g_c.phase = Phase::ON;
    g_c.until = NowMs() + kStartMs;
    g_c.me = {};
    Send(OP_ACCEPT, from);
    // the one who said yes comes over (27.10: "the partner does not come as the second one next to me"): onto the
    // caller's dance spot, where the club starts our own dance as we stand on its marker (the other one dances at our
    // right hand, Players' DanceSide), or beside it at its court
    std::string how = HowTo(g_c.kind, name);
    if ((g_c.kind == DANCE || g_c.kind == BASKET) && g_run.kind != g_c.kind)
        if (const RemotePlayer* rp = Players::Instance().Get(from); rp && rp->ped && rp->hasState) {
            float h = rp->ped->CurrentRotation(), side = g_c.kind == DANCE ? 0.0f : 1.5f;
            Players::Instance().GoTo(*rp, rp->state.pos + CVector{std::cos(h) * side, std::sin(h) * side, 0.0f}, true);
            how = g_c.kind == DANCE ? Format(TR("Вы на танцполе рядом с игроком %s: постойте на маркере, и начнётся танец. Считаются очки.",
                                                "You are on the dance floor next to %s: stand on the marker and the dance starts. The points count."),
                                             name.c_str())
                                    : Format(TR("Вы на площадке рядом с игроком %s: возьмите мяч и начните испытание на очки.",
                                                "You are on the court next to %s: take the ball and start the points challenge."),
                                             name.c_str());
            LOGI("Over to %s for the contest", name.c_str());
        }
    if (g_c.kind == POOL)  // a game for two at the caller's table while its bet screen is up (Pool.cpp); if not, OP_FAIL says
        how = Format(TR("Сейчас начнётся партия вдвоём за столом игрока %s.", "A game for two at %s's table starts now."), name.c_str());
    ui::TheChat().AddSystem(Format(TR("Вы приняли вызов: %s. %s На начало игры есть 3 минуты.",
                                      "You took the challenge: %s. %s You have 3 minutes to start."),
                                   kind.c_str(), how.c_str()));
    LOGI("Contest %u of %s accepted", g_c.kind, name.c_str());
    if (g_run.kind == g_c.kind && g_run.variant == g_c.variant && !g_run.bound) {  // playing it now: this run counts
        g_run.bound = true;
        g_c.me = RunResult(PLAYING);
    }
    SendState(true);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8(), kind = r.U8();
    int32_t variant = r.I32();
    Result res;
    res.state = r.U8();
    res.ok = r.U8() != 0;
    res.value = r.I32();
    res.place = r.I32();
    uint8_t why = r.U8();
    if (!r.Ok() || (to != 0xFF && to != Net().MyId()) || from == Net().MyId() || !kind || kind >= KINDS || res.state > DONE) return;
    std::string name = Who(from);
    if (op == OP_CALL) {
        bool busy = g_c.phase != Phase::OFF || !core::IsPlayerReady() || !flow::InSharedWorld() || StoryRuns() || SideLeader() != 0xFF;
        if (busy) {
            Writer w(proto::G_CONTEST);  // our own contest's fields are not this one's
            w.U8(OP_DECLINE), w.U8(from), w.U8(kind), w.I32(variant), w.U8(0), w.U8(0), w.I32(0), w.I32(0), w.U8(WHY_BUSY);
            Net().Send(proto::CH_RELIABLE, w);
            LOGI("Contest %u of %s declined: busy", kind, name.c_str());
            return;
        }
        g_c = {};
        g_c.phase = Phase::CALLED;
        g_c.with = from, g_c.kind = kind, g_c.variant = variant;
        g_c.until = NowMs() + kAnswerMs;
        g_c.them = res;
        LOGI("Contest %u (%d) offered by %s", kind, variant, name.c_str());
    } else if (op == OP_ACCEPT) {
        if (g_c.phase != Phase::CALLING || kind != g_c.kind) {  // over, or another took it first
            Writer w(proto::G_CONTEST);
            w.U8(OP_CANCEL), w.U8(from), w.U8(kind), w.I32(variant), w.U8(0), w.U8(0), w.I32(0), w.I32(0);
            w.U8(g_c.phase == Phase::OFF ? WHY_NO : WHY_TAKEN);
            Net().Send(proto::CH_RELIABLE, w);
            return;
        }
        g_c.phase = Phase::ON;
        g_c.with = from;
        g_c.until = NowMs() + kStartMs;
        g_c.them = res;
        ui::TheChat().AddSystem(Format(TR("%s принимает вызов: %s.", "%s takes the challenge: %s."), name.c_str(),
                                       KindName(kind, variant).c_str()));
        LOGI("Contest %u accepted by %s", kind, name.c_str());
        SendState(true);
        if (kind == POOL) pool::Ask(from);  // our table for two, while its bet screen is up (Pool.cpp)
    } else if (op == OP_DECLINE) {
        if (g_c.phase != Phase::CALLING || kind != g_c.kind) return;
        ui::TheChat().AddSystem(Format(why == WHY_BUSY   ? TR("%s сейчас не может соревноваться.", "%s cannot play a contest now.")
                                       : why == WHY_LATE ? TR("Нет ответа на вызов от игрока %s.", "No answer to the challenge from %s.")
                                                         : TR("%s отказывается от соревнования.", "%s declines the contest."),
                                       name.c_str()));
        if (++g_c.declines >= (int)Net().Peers().size()) Reset();
    } else if (op == OP_STATE) {
        if (g_c.phase != Phase::ON || from != g_c.with || kind != g_c.kind) return;
        g_c.them = res;
        if (g_c.me.state == DONE && g_c.them.state == DONE) Settle();
    } else if (op == OP_CANCEL) {
        if (from != g_c.with || (g_c.phase != Phase::ON && g_c.phase != Phase::CALLED)) return;
        // no verb in the past after a nick: it does not tell who plays behind it
        const char* line = why == WHY_LATE      ? TR("Соревнование отменено: за 3 минуты игра у игрока %s не началась.",
                                                     "The contest is off: %s did not start playing in 3 minutes.")
                           : why == WHY_MISSION ? TR("Соревнование отменено: у игрока %s началась миссия.",
                                                     "The contest is off: %s is on a mission now.")
                           : why == WHY_TAKEN   ? TR("Игрок %s уже участвует в другом соревновании.", "%s is in another contest already.")
                                                : TR("Игрок %s отменяет соревнование.", "%s calls the contest off.");
        ui::TheChat().AddSystem(Format(line, name.c_str()));
        LOGI("Contest %u called off by %s (%u)", g_c.kind, name.c_str(), why);
        Reset();
    }
}

void Update() {
    Track();
    uint64_t now = NowMs();
    if (g_c.phase == Phase::CALLED && now > g_c.until) {
        Cancel(WHY_LATE, "");
    } else if (g_c.phase == Phase::CALLING && now > g_c.until + 3000) {  // the other's own 20 s end a moment later
        ui::TheChat().AddSystem(Format(TR("Нет ответа на вызов от %s.", "No answer to the challenge from %s."), Others().c_str()));
        Reset();
    } else if (g_c.phase == Phase::ON && g_c.me.state == WAITING) {
        if (StoryRuns())
            Cancel(WHY_MISSION, TR("Соревнование отменено: началась миссия.", "The contest is off: a mission started."));
        else if (now > g_c.until)
            Cancel(WHY_LATE, TR("Соревнование отменено: игра не началась за 3 минуты.", "The contest is off: you did not start playing in 3 minutes."));
    }
}

class ContestModule : public Module {
public:
    const char* Name() const override { return "Contest"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_CONTEST, &Handle); }
    void OnPostProcess() override {
        if (!Net().Online()) return;
        Update();
    }
    void OnPeerLeave(uint8_t id) override {
        if (g_c.phase != Phase::OFF && (g_c.with == id || (g_c.phase == Phase::CALLING && Net().Peers().empty()))) {
            if (g_c.phase == Phase::ON)
                ui::TheChat().AddSystem(Format(TR("Соревнование отменено: игрока %s больше нет в игре.", "The contest is off: %s left."),
                                               Who(id).c_str()));
            Reset();
        }
    }
    void OnPeerResync(uint8_t id) override {
        if (g_c.with == id && g_c.phase == Phase::ON) {
            ui::TheChat().AddSystem(Format(TR("Соревнование отменено: игра игрока %s перезапустилась.",
                                              "The contest is off: %s's game restarted."),
                                           Who(id).c_str()));
            Reset();
        }
    }
    void OnSessionEnd() override { g_run = {}, g_c = {}, g_resultUntil = 0; }
    void OnGameStarted() override { g_run = {}, g_c = {}, g_resultUntil = 0; }
};
ContestModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

std::string Text(std::string& hint) {
    hint.clear();
    if (!Net().Online()) return "";
    if (g_c.phase == Phase::CALLED) {
        hint = TR("Y - принять, N - отказаться", "Y: accept, N: decline");
        return Format(TR("%s вызывает вас на соревнование: %s", "%s challenges you: %s"), Who(g_c.with).c_str(),
                      KindName(g_c.kind, g_c.variant).c_str());
    }
    // pool: the bet screen takes the other player as its second player (Pool.cpp), a bet made first starts the game alone
    if (g_c.phase == Phase::CALLING)
        return Format(g_c.kind == POOL ? TR("Ждём ответа %s. Не делайте ставку: партия будет вдвоём.",
                                            "Waiting for %s to answer. Make no bet: the game is for two.")
                                       : TR("Ждём ответа %s", "Waiting for %s to answer"),
                      Others().c_str());
    if (CanCall() && !SideCallOpen()) {  // with a call to play together it is in that box
        hint = TR("U - вызвать", "U: challenge");
        if (g_run.kind == POOL)
            return Format(TR("Бильярд вдвоём за этим столом: вызвать %s?", "Pool for two at this table: challenge %s?"), Others().c_str());
        return Format(TR("Соревнование: %s. Вызвать %s?", "Contest: %s. Challenge %s?"), KindName(g_run.kind, g_run.variant).c_str(),
                      Others().c_str());
    }
    return "";
}

bool Key(int vk) {
    if (!Net().Online()) return false;
    if (g_c.phase == Phase::CALLED && (vk == 'Y' || vk == 'N')) {
        Answer(vk == 'Y');
        return true;
    }
    if (vk == 'U' && CanCall()) {
        Call();
        return true;
    }
    return false;
}

bool Board(BoardView& out) {
    if (!Net().Online()) return false;
    if (NowMs() < g_resultUntil) return out = g_result, true;
    if (g_c.phase != Phase::ON) return false;
    auto shown = [](const Result& r, bool me) -> std::string {
        if (r.state == DONE) return ValueText(g_c.kind, r);
        if (r.state == WAITING) return TR("не начата", "not started");
        if (MetricOf(g_c.kind) == Metric::TIME && !r.ok) return me ? TR("играете", "playing") : TR("играет", "playing");
        return ValueText(g_c.kind, r);  // the points so far, or the time at the line
    };
    out = {};
    out.title = KindName(g_c.kind, g_c.variant);
    out.me = {TR("Вы", "You"), shown(g_c.me, true)};
    out.them = {Who(g_c.with), shown(g_c.them, false)};
    // ahead: both playing (a time only once both are over the line)
    if (g_c.me.state != WAITING && g_c.them.state != WAITING && (MetricOf(g_c.kind) != Metric::TIME || (g_c.me.ok && g_c.them.ok))) {
        int c = Compare(g_c.kind, g_c.me, g_c.them);
        out.me.leads = c > 0, out.them.leads = c < 0;
    }
    if (g_c.me.state == WAITING) {  // the 3 minutes to start our game, and how (it was in the chat only)
        uint64_t left = g_c.until > NowMs() ? (g_c.until - NowMs()) / 1000 : 0;
        out.wait = Format(TR("Начните свою игру: осталось %d:%02d", "Start your game: %d:%02d left"), (int)(left / 60), (int)(left % 60));
        out.how = HowTo(g_c.kind, Who(g_c.with));
    }
    return true;
}

void Preview(int step, int kind) {
    if (Net().Peers().empty() || step < 1 || step > 4) return;
    if (kind <= 0 || kind >= KINDS) kind = BASKET;
    bool time = MetricOf((uint8_t)kind) == Metric::TIME;
    int32_t unit = time ? 10000 : 1;  // a time in ms
    g_resultUntil = 0;
    g_c = {};
    g_c.phase = Phase::ON, g_c.with = Net().Peers().begin()->first, g_c.kind = (uint8_t)kind, g_c.variant = 1;
    g_c.until = NowMs() + kStartMs - 19000;
    g_c.them = {PLAYING, !time, 12 * unit, 0};
    if (step >= 2) g_c.me = {PLAYING, !time, 18 * unit, 0};
    if (step >= 3) {
        g_c.me = {DONE, true, 24 * unit, time ? 2 : 0};
        g_c.them = {DONE, true, 16 * unit, time ? 1 : 0};
        if ((Compare(g_c.kind, g_c.me, g_c.them) > 0) != (step == 3)) std::swap(g_c.me, g_c.them);
        Settle();
    }
}

bool CanCall() {
    return Net().Online() && g_run.kind && !g_run.bound && g_c.phase == Phase::OFF && NowMs() < g_run.callUntil &&
           !Net().Peers().empty() && !SideLeading();
}

void JointCalled() { g_run.callUntil = 0; }

void Drop() {
    if (g_c.phase == Phase::OFF) return;
    LOGI("Contest %u played together instead", g_c.kind);
    Reset();
}

void AfterCommand(void* script, uint16_t op, const uint8_t* ip) {
    if (op != 0x0394 && op != 0x00BC && op != 0x014E) return;  // PLAY_MISSION_PASSED_TUNE, PRINT_NOW, DISPLAY_ONSCREEN_TIMER
    if (!g_run.kind || g_run.finished || !script || !Field<uint8_t>(script, 0xDC)) return;  // our side mission's
    if (op == 0x014E) {  // Blood Ring's clock shown at its GO: its time counts from there
        if (g_run.kind == BLOODRING && !g_run.clockSeen && Named(script, "blood")) g_run.gameStart = GameMs(), g_run.clockSeen = true;
        return;
    }
    if (g_run.kind != TRIATHLON || !Named(script, "tria")) return;
    // its winner's tune, or "Loser!" (RACES_8) at the line: both only when our racer is over it
    if (op == 0x00BC && !(ip && ip[2] == 0x09 && !strncmp((const char*)ip + 3, "RACES_8", 8))) return;
    g_run.finished = true;
    g_run.place = Local(942);
    g_run.value = (int32_t)(GameMs() - (uint32_t)Local(767));
    LOGI("Contest triathlon %d: over the line in %d ms, place %d", g_run.variant, g_run.value, g_run.place);
}

}  // namespace coop::contest
