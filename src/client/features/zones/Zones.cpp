// Gang territories follow the host. The host's game runs the gang wars and the story; whenever a territory's gangs,
// colour or radar mode (flashing while it is attacked) change there, the partner's copy of that zone info changes too.
// A war the partner provokes away from the host is fought in the partner's game (the engine ties a war to its own
// player: the zone, the waves, their targets), and its outcome goes into the host's world (G_TURF_WAR).
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kZoneInfos = 0xBA1DF0;  // CTheZones::ZoneInfoArray[380], 0x11 bytes each
constexpr uintptr_t kZoneInfoCount = 0xBA1DE8;  // int16 CTheZones::TotalNumberOfZoneInfos
constexpr int kMaxZones = 380;

// a zone info without its drug dealers' counter (+0x0A, each game bumps it at random): gang densities (+0..9),
// colour (+0x0B..0x0E), population type and radar mode (+0x0F), races (+0x10)
struct ZoneBytes {
    uint8_t b[16];
    bool operator==(const ZoneBytes& o) const { return !memcmp(b, o.b, sizeof(b)); }
};

int Count() { return std::clamp<int>(Ref<int16_t>(kZoneInfoCount), 0, kMaxZones); }
ZoneBytes ReadZone(int i) {
    auto z = (const uint8_t*)(kZoneInfos + (uintptr_t)i * 0x11);
    ZoneBytes r;
    memcpy(r.b, z, 10);
    memcpy(r.b + 10, z + 0x0B, 6);
    return r;
}
void WriteZone(int i, const ZoneBytes& r) {
    auto z = (uint8_t*)(kZoneInfos + (uintptr_t)i * 0x11);
    memcpy(z, r.b, 10);
    memcpy(z + 0x0B, r.b + 10, 6);
}

std::vector<ZoneBytes> g_sent;  // host: what the partner has
uint64_t g_scanMs = 0;

void Send(const std::vector<int>& zones) {
    for (size_t at = 0; at < zones.size(); at += 100) {
        size_t n = std::min<size_t>(100, zones.size() - at);
        Writer w(proto::G_ZONE);
        w.U16((uint16_t)n);
        for (size_t k = at; k < at + n; ++k) {
            w.U16((uint16_t)zones[k]);
            w.Raw(ReadZone(zones[k]).b, 16);
        }
        Net().Send(proto::CH_RELIABLE, w);
    }
}

// ---------------------------------------------------------------- gang wars
// CGangWars statics
float& Provocation() { return Ref<float>(0x96AB5C); }
int32_t& WarState() { return Ref<int32_t>(0x96AB64); }   // the offensive war: 0 none, then its waves
int32_t& WarState2() { return Ref<int32_t>(0x96AB4C); }  // a defence
uint8_t& WarsActive() { return Ref<uint8_t>(0x96AB90); }  // bGangWarsActive (the story's "Doberman")
bool StoryHoldsWars() { return Ref<uint8_t>(0x96AB91) || Ref<int32_t>(0x96AB94); }  // bTrainingMission, NumSpecificZones
bool Fought() { return Call<0x443AC0, bool>(); }  // CGangWars::GangWarFightingGoingOn
enum : uint8_t { WAR_ALLOWED = 1, WAR_STORY = 2, WAR_MISSION = 4, WAR_DEFENCE = 8 };  // G_GANG_WAR's flags, host only
enum : uint8_t { TURF_START = 1, TURF_ZONE = 2, TURF_END = 3, TURF_STOP = 4 };
enum : uint8_t { TURF_WON = 0, TURF_LEFT = 1, TURF_CALLED_OFF = 2 };

// The engine charges no crime while a gang war is fought (CWanted::ReportCrimeNow asks
// CGangWars::GangWarFightingGoingOn). Fighting the other player's war beside it, a player got stars for every kill (#56):
// its game takes that war as its own for that, within 300 m of the player who fights it.
using WarFn = bool(__cdecl*)();
WarFn g_origWar[2] = {};
uint64_t g_otherWarUntil = 0;  // the other player said a war of its game is fought, until then
uint8_t g_otherWarFrom = 0xFF;
uint8_t g_hostWarFlags = WAR_ALLOWED;  // partner: what the host's world allows
bool g_hostWarShown = false;           // partner: the chat told of the host's war going on
bool g_sentWar = false;
uint8_t g_sentFlags = 0xFF;
uint64_t g_sentWarAt = 0;

bool OtherWarHere() {
    if (!Net().Online() || NowMs() > g_otherWarUntil) return false;
    const RemotePlayer* rp = Players::Instance().Get(g_otherWarFrom);
    CPlayerPed* me = FindPlayerPed(0);
    return rp && rp->ped && me && (rp->ped->GetPosition() - me->GetPosition()).Length2D() < 300.0f;
}
bool __cdecl WarFought0() { return g_origWar[0]() || OtherWarHere(); }
bool __cdecl WarFought1() { return g_origWar[1]() || OtherWarHere(); }

// host: a war the partner fights, held for its zone; partner: its own
struct TurfWar {
    uint8_t owner = 0xFF;
    int zone = -1;
    ZoneBytes sent{};
    uint64_t endedMs = 0;
};
TurfWar g_partnerWar, g_ownWar;
int g_ownWarZoneEnded = -1;

void SendTurf(uint8_t op, int zone, const std::function<void(Writer&)>& more = nullptr) {
    Writer w(proto::G_TURF_WAR);
    w.U8(op);
    w.U16((uint16_t)zone);
    if (more) more(w);
    Net().Send(proto::CH_RELIABLE, w);
}

bool HostWithin(float range) {
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    return host && host->hasState && me && host->state.area == me->AreaCode() && (host->state.pos - me->GetPosition()).Length() < range;
}

std::string ZoneName(int zone) {  // the district's name as the map shows it (CTheZones navigation zone: its GXT label)
    for (int i = 0, n = std::min<int>(Ref<int16_t>(0xBA3794), 380); i < n; ++i) {  // TotalNumberOfNavigationZones
        uintptr_t z = 0xBA3798 + (uintptr_t)i * 0x20;                              // NavigationZoneArray
        if (Ref<int16_t>(z + 0x1C) != zone) continue;
        char label[9] = {};
        memcpy(label, (const char*)z + 8, 8);  // m_szTextKey
        const char* text = CallMethod<0x6A0050, const char*>((void*)0xC1B340, label);  // CText::Get
        if (!text) return std::string();
        std::string s;
        for (const char* c = text; *c; ++c)
            if (*c < 32 || *c > 126) return std::string();  // a translation's own font codes: the chat could not show them
            else s += *c;
        return s;
    }
    return std::string();
}

using VoidFn = void(__cdecl*)();
VoidFn g_origOffensive = nullptr, g_origDefensive = nullptr;
// The provocation (three gang kills) starts a war: the host's own, unless the partner fights one; the partner's own
// when the host is not near (then it is the host's, fed by the partner's kills: GANG_KILL), on no story mission and
// with no war of the host going on.
void __cdecl HookStartOffensiveGangWar() {
    if (!Net().Online()) return g_origOffensive();
    if (Net().IsHost()) {
        if (g_partnerWar.owner == 0xFF) return g_origOffensive();
        Provocation() = 0.0f;  // one war at a time: the partner's is fought
        return;
    }
    if (!(g_hostWarFlags & WAR_ALLOWED) || (g_hostWarFlags & (WAR_STORY | WAR_MISSION)) || NowMs() < g_otherWarUntil ||
        HostWithin(100.0f) || HostMissionRecent(0) || !flow::InHostWorld()) {
        Provocation() = 0.0f;  // what the engine does where no war can start
        return;
    }
    g_origOffensive();
    if (!WarState()) return;  // not a district to fight for
    g_ownWar = {};
    g_ownWar.owner = Net().MyId();
    g_ownWar.zone = (int)((Ref<uintptr_t>(0x96AB8C) - kZoneInfos) / 0x11);  // pZoneInfoToFightOver
    int nav = (int)((Ref<uintptr_t>(0x96AB88) - 0xBA3798) / 0x20);       // pZoneToFightOver
    uint8_t gang = (uint8_t)Ref<int32_t>(0x96AB84);
    SendTurf(TURF_START, g_ownWar.zone, [&](Writer& w) { w.U16((uint16_t)nav), w.U8(gang); });
    LOGI("Our gang war for zone %d (gang %u) starts here", g_ownWar.zone, gang);
}
// the host's district under attack waits while the partner's war is fought
void __cdecl HookStartDefensiveGangWar() {
    if (!Net().Online() || (Net().IsHost() && g_partnerWar.owner == 0xFF)) g_origDefensive();
}

// what an offensive war can do to a district: Grove (gang 1) only up, the enemies only down, no more in all
bool WarChange(const ZoneBytes& was, const ZoneBytes& now) {
    int sumWas = 0, sumNow = 0;
    for (int g = 0; g < 10; ++g) {
        sumWas += was.b[g], sumNow += now.b[g];
        if (g == 1 ? now.b[g] < was.b[g] : now.b[g] > was.b[g]) return false;
    }
    return sumNow <= sumWas;
}

void EndPartnerWar(const char* why) {
    if (g_partnerWar.owner == 0xFF) return;
    LOGI("The partner's gang war for zone %d: %s", g_partnerWar.zone, why);
    g_partnerWar = {};
    g_partnerWar.endedMs = NowMs();
    Call<0x572440>(false);  // CTheZones::FillZonesWithGangColours: what every war's end does
}

void HandleTurf(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    int zone = r.U16();
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || !flow::InSharedWorld() || zone >= Count()) return;
    std::string who = Net().PeerName(from);
    if (op == TURF_STOP) {  // to the partner: its war is called off
        if (Net().IsHost() || from != Net().HostId() || g_ownWar.zone != zone || !WarState()) return;
        Call<0x4464C0>(true);  // CGangWars::EndGangWar
        LOGI("Our gang war for zone %d: called off by the host", zone);
        return;
    }
    if (!Net().IsHost()) return;
    if (op == TURF_START) {
        r.U16(), r.U8();
        // the host's own war, a defence, the story's say over the wars, a story mission: no war of the partner now
        if (WarState() || WarState2() || !WarsActive() || StoryHoldsWars() || core::IsOnMission() || g_partnerWar.owner != 0xFF) {
            SendTurf(TURF_STOP, zone);
            LOGI("%s's gang war for zone %d: not now", who.c_str(), zone);
            return;
        }
        g_partnerWar.owner = from, g_partnerWar.zone = zone;
        g_partnerWar.sent = ReadZone(zone);
        std::string name = ZoneName(zone);
        ui::TheChat().AddSystem(name.empty() ? Format(TR("%s начинает войну банд за район.", "%s starts a gang war for a district."), who.c_str())
                                             : Format(TR("%s начинает войну банд за район %s.", "%s starts a gang war for %s."),
                                                      who.c_str(), name.c_str()));
        LOGI("%s's gang war for zone %d starts", who.c_str(), zone);
    } else if (op == TURF_ZONE) {
        ZoneBytes z;
        r.Raw(z.b, 16);
        if (!r.Ok() || from != g_partnerWar.owner || zone != g_partnerWar.zone) return;
        ZoneBytes now = ReadZone(zone);
        if (!WarChange(now, z)) return LOGW("%s's gang war for zone %d: a change it cannot make, not taken", who.c_str(), zone);
        memcpy(now.b, z.b, 14);  // the gangs and the colour; of +0x0F the radar mode (flashing) only
        now.b[14] = (uint8_t)((now.b[14] & ~0x60) | (z.b[14] & 0x60));
        WriteZone(zone, now);
    } else if (op == TURF_END) {
        uint8_t result = r.U8();
        if (!r.Ok() || from != g_partnerWar.owner || zone != g_partnerWar.zone) return;
        std::string name = ZoneName(zone);
        if (result == TURF_WON)
            ui::TheChat().AddSystem(name.empty() ? Format(TR("%s отбивает район у врагов.", "%s takes the district from the enemy."), who.c_str())
                                                 : Format(TR("%s отбивает у врагов район %s.", "%s takes %s from the enemy."), who.c_str(),
                                                          name.c_str()));
        EndPartnerWar(result == TURF_WON ? "won" : result == TURF_LEFT ? "left" : "called off");
    }
}

// partner: the war of its own game, watched every frame (its district to the host as it changes, its end)
void WatchOwnWar() {
    if (g_ownWar.owner == 0xFF) return;
    bool fought = WarState() != 0;
    if (fought && (HostMissionRecent(0) || !Net().Online())) {  // a story mission ends a war (the engine does too)
        Call<0x4464C0>(true);  // CGangWars::EndGangWar
        fought = WarState() != 0;
    }
    ZoneBytes z = ReadZone(g_ownWar.zone);
    if (!(z == g_ownWar.sent)) {
        g_ownWar.sent = z;
        SendTurf(TURF_ZONE, g_ownWar.zone, [&](Writer& w) { w.Raw(z.b, 16); });
    }
    if (fought) return;
    bool won = true;
    for (int g = 0; g < 10; ++g) won = won && (g == 1 || z.b[g] == 0);
    SendTurf(TURF_END, g_ownWar.zone, [&](Writer& w) { w.U8(won ? TURF_WON : TURF_LEFT); });
    LOGI("Our gang war for zone %d: %s", g_ownWar.zone, won ? "won" : "over");
    g_ownWarZoneEnded = g_ownWar.zone;
    g_ownWar = {};
    g_ownWar.endedMs = NowMs();
}

void HandleGangWar(uint8_t from, Reader& r) {
    bool fought = r.U8() != 0;
    uint8_t flags = r.U8();
    int zone = r.U16();
    if (!r.Ok() || from == Net().MyId()) return;
    g_otherWarUntil = fought ? NowMs() + 6000 : 0;
    g_otherWarFrom = from;
    if (from != Net().HostId() || Net().IsHost()) return;
    g_hostWarFlags = flags;
    // the host's war in the partner's chat too, as the partner's own war is in the host's (#72)
    if (fought != g_hostWarShown && core::IsPlayerReady() && flow::InHostWorld()) {
        g_hostWarShown = fought;
        std::string who = Net().PeerName(from), name = zone < Count() ? ZoneName(zone) : std::string();
        if (!fought)
            ui::TheChat().AddSystem(Format(TR("Война банд у игрока %s закончилась.", "%s's gang war is over."), who.c_str()));
        else if (flags & WAR_DEFENCE)
            ui::TheChat().AddSystem(name.empty() ? Format(TR("%s отбивает нападение банд на свой район.", "%s fights off a gang attack."), who.c_str())
                                                 : Format(TR("%s отбивает нападение банд на район %s.", "%s fights off a gang attack on %s."),
                                                          who.c_str(), name.c_str()));
        else
            ui::TheChat().AddSystem(name.empty() ? Format(TR("%s начинает войну банд за район.", "%s starts a gang war for a district."), who.c_str())
                                                 : Format(TR("%s начинает войну банд за район %s.", "%s starts a gang war for %s."),
                                                          who.c_str(), name.c_str()));
        LOGI("The host's gang war%s: %s", (flags & WAR_DEFENCE) ? " (a defence)" : "", fought ? "fought" : "over");
    }
    // the host's world says whether wars are on yet ("Doberman"): ours follows it, the snapshot may be older
    if (core::IsPlayerReady() && flow::InHostWorld()) WarsActive() = (flags & WAR_ALLOWED) ? 1 : 0;
}

// the war of this game and (the host) what its world allows: on a change and every 3 s while a war is fought
void SendGangWar() {
    bool war = Fought();
    uint8_t flags = 0;
    if (Net().IsHost())
        flags = (WarsActive() ? WAR_ALLOWED : 0) | (StoryHoldsWars() ? WAR_STORY : 0) | (core::IsOnMission() ? WAR_MISSION : 0) |
                (war && WarState2() && !WarState() ? WAR_DEFENCE : 0);
    if (war == g_sentWar && flags == g_sentFlags && !(war && NowMs() - g_sentWarAt > 3000)) return;
    g_sentWar = war, g_sentFlags = flags, g_sentWarAt = NowMs();
    uintptr_t info = Ref<uintptr_t>(0x96AB8C);  // CGangWars::pZoneInfoToFightOver
    int zone = war && info >= kZoneInfos && info < kZoneInfos + (uintptr_t)Count() * 0x11 ? (int)((info - kZoneInfos) / 0x11) : 0xFFFF;
    Writer w(proto::G_GANG_WAR);
    w.U8(war ? 1 : 0);
    w.U8(flags);
    w.U16((uint16_t)zone);
    Net().Send(proto::CH_RELIABLE, w);
}

class Zones : public Module {
public:
    const char* Name() const override { return "Zones"; }

    void OnInstallHooks() override {
        // CGangWars::Update: the provocation and the attack countdown start the wars
        mem::RedirectCallFn(0x4466DC, &HookStartOffensiveGangWar, &g_origOffensive);
        mem::RedirectCallFn(0x446E46, &HookStartDefensiveGangWar, &g_origDefensive);
        mem::RedirectCallFn(0x562167, &WarFought0, &g_origWar[0]);  // CWanted::ReportCrimeNow, twice
        mem::RedirectCallFn(0x56218E, &WarFought1, &g_origWar[1]);
        Net().SetHandler(proto::G_GANG_WAR, &HandleGangWar);
        Net().SetHandler(proto::G_TURF_WAR, &HandleTurf);
        Net().SetHandler(proto::G_ZONE, [](uint8_t from, Reader& r) {
            int n = r.U16();
            if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
            for (int k = 0; k < n; ++k) {
                int i = r.U16();
                ZoneBytes z;
                r.Raw(z.b, 16);
                if (!r.Ok() || i >= Count()) return;
                // the district our own war is fought for is ours till it ends (and the host's echo of it after)
                if (i == g_ownWar.zone || (i == g_ownWarZoneEnded && NowMs() - g_ownWar.endedMs < 2000)) continue;
                WriteZone(i, z);
            }
            LOGD("Territories: %d zone infos from the host", n);
        });
    }
    void OnSessionEnd() override {
        g_sent.clear();
        g_partnerWar = g_ownWar = {};
        g_sentWar = false, g_sentFlags = 0xFF, g_otherWarUntil = 0, g_hostWarFlags = WAR_ALLOWED, g_hostWarShown = false;
    }
    void OnGameStarted() override {
        g_sent.clear();
        g_ownWar = {};  // the world went with its war
        // the host's world loaded here with a war of the host's on (a defence it was told of, its own offensive): that war
        // is fought in the host's game, and here it kept the partner's own from starting
        if (Net().Online() && !Net().IsHost() && (WarState() || WarState2())) {
            WarState() = WarState2() = 0;
            Call<0x572440>(false);  // CTheZones::FillZonesWithGangColours
            LOGI("A gang war of the host's world: the host's to fight, not here");
        }
        if (Net().Online() && Net().IsHost() && g_partnerWar.owner != 0xFF) SendTurf(TURF_STOP, g_partnerWar.zone), g_partnerWar = {};
        g_sentFlags = 0xFF;
    }
    void OnPeerResync(uint8_t) override { g_sent.clear(), g_sentFlags = 0xFF; }  // the next scan sends everything
    void OnPeerJoin(uint8_t) override { g_sentWar = false, g_sentFlags = 0xFF; }  // the newcomer hears of a war going on
    void OnPeerLeave(uint8_t id) override {
        if (Net().IsHost() && id == g_partnerWar.owner) EndPartnerWar("its player left");
    }

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        SendGangWar();
        if (!Net().IsHost()) WatchOwnWar();
        if (!Net().IsHost() || NowMs() - g_scanMs < 500) return;
        g_scanMs = NowMs();
        int n = Count();
        bool all = g_sent.size() != (size_t)n;
        g_sent.resize(n);
        std::vector<int> changed;
        for (int i = 0; i < n; ++i) {
            ZoneBytes z = ReadZone(i);
            if (all || !(z == g_sent[i])) {
                g_sent[i] = z;
                changed.push_back(i);
            }
        }
        if (!changed.empty() && !Net().Peers().empty()) {
            Send(changed);
            if (!all) LOGI("Territories: %zu zone infos changed", changed.size());
        }
    }

    static Zones& Instance() {
        static Zones m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Zones::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
