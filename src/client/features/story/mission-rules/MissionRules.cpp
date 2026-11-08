// A story mission runs in the host's game and asks about its own player: who hit its people, who shoots, who aims at them,
// who stands at the thing to take. Another player doing the same counted for nothing (all missions read through, 04.11).
// Here such a line is asked again for each other player's copy: the command's own handler runs once more with
// $PLAYER_ACTOR standing for the copy, so the game's own rules decide, for any number of players.
//  * every mission: HAS_CHAR_BEEN_DAMAGED_BY_CHAR, HAS_CAR_BEEN_DAMAGED_BY_CHAR, IS_CHAR_SHOOTING, IS_CHAR_SHOOTING_IN_AREA
//    of its player, and IS_PLAYER_TARGETTING_CHAR by what the partners tell they aim at (G_PLAYER_AIM);
//  * single lines (kSpots), by their place in the original main.scm and their opcode: a modded mission has other places
//    and is left as it is.
// Only lines whose sequel is safe for a player who is somewhere else are listed: one that goes on to move CJ would do that to
// the host, far from the spot. A longer stretch goes for the other player whole, its animation too, and leaves the host's
// controls alone. "Catalyst" has its own way: the partner stands on the train and its boxes are made here (HostThrow).
#include "features/story/mission-rules/MissionRules.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"

namespace coop::rules {

namespace {
using namespace game;
using game::script::Cmd;

using CommandHandlerFn = int8_t(__thiscall*)(void*, int);
constexpr uintptr_t kScriptParams = 0xA43C78;  // CTheScripts::ScriptParams: what the command just read
constexpr uintptr_t kMissionBlock = 0xA7A6A0;  // a mission's code runs here (LOAD_AND_LAUNCH_MISSION_INTERNAL)
int32_t& Actor() { return Ref<int32_t>(addr::ScriptSpace + 12); }  // $PLAYER_ACTOR
const int32_t* Params() { return (const int32_t*)kScriptParams; }
bool Named(void* script, const char* name) { return !_strnicmp(Field<char[8]>(script, 0x08), name, 8); }

// A line of a mission, by its offsets in main.scm (the mission's own and the line's, as tools/re/scm_disasm.py prints them).
// until: from a yes of another player on, the script's lines up to there go for that player. written: the line as it
// stands, NOT included, holds for some player (a NOT asked of all would be no for all). button: IS_BUTTON_PRESSED in the
// stretch, answered by that player's enter key (anyKey: yes at once); hint: the help of that place, for it.
struct Spot {
    const char* mission;
    uint32_t start, at;
    uint16_t op;
    uint32_t until;
    bool written;
    uint32_t button = 0;
    bool anyKey = false;
    const char* hint = nullptr;
};
constexpr Spot kSpots[] = {
    // "Ice Cold Killa": the phone is taken by walking into it, by a player on foot (the partner could not)
    {"synd4", 0x18B2F2, 0x18DE78, 0x00FE, 0, false},  // LOCATE_CHAR_ANY_MEANS_3D $PLAYER at the phone
    {"synd4", 0x18B2F2, 0x18DE9E, 0x00DF, 0, true},   // NOT IS_CHAR_IN_ANY_CAR $PLAYER
    // "Interdiction": the contraband, "Green Goo": the jar, both taken by coming to them
    {"des3", 0x1DA540, 0x1DDDA1, 0x00FF, 0, false},   // LOCATE_CHAR_ON_FOOT_3D
    {"des10", 0x1F0C91, 0x1F1A67, 0x00FE, 0, false},  // LOCATE_CHAR_ANY_MEANS_3D
    // "Black Project": the switch of the blast doors in the control tower only opens them
    {"maf4", 0x1E9490, 0x1ECB9D, 0x0102, 0, false},  // LOCATE_STOPPED_CHAR_ON_FOOT_3D
    // and its panels of the SAM sites and the launch bay's door: their animation goes on who stands there
    {"maf4", 0x1E9490, 0x1EED91, 0x0102, 0x1EEDF4, false},
    {"maf4", 0x1E9490, 0x1EF0FD, 0x0102, 0x1EF172, false},
    // "T-Bone Mendez": a package on the ground goes to who comes to it: the whole stretch, its grabbing animation too, is
    // that player's (the partner could not take them)
    {"driv2", 0x19D3BD, 0x19E695, 0x0474, 0x19E986, false, 0x19E6D2, true},  // LOCATE_CHAR_ANY_MEANS_OBJECT_3D
    // "Dam and Blast": a charge on a generator (the players' choice 04.11: the partner helps), and the host keeps its controls
    {"heist2", 0x23822E, 0x23B69E, 0x00FF, 0x23BA37, false, 0x23B6FF, false, "HEI2_6"},  // LOCATE_CHAR_ON_FOOT_3D
    // "Amphibious Assault": the bug on the tanker
    {"wuzi2", 0x16CF33, 0x16ED89, 0x00FF, 0x16EDE8, false, 0x16EDDB},  // LOCATE_CHAR_ON_FOOT_3D
    // "Cop Wheels": a bike taken up the Packer's ramp by who rides it (ATTACH_CAR_TO_CAR goes to its rider's game)
    {"heist4", 0x23CDD5, 0x23E4C2, 0x00DB, 0x23E6EA, false},  // IS_CHAR_IN_CAR $PLAYER bike
    // "Vertical Bird": a guard who sees the player on the carrier goes for the alarm, and the partner in his sight was nobody
    // (05.11). The two with a stretch then ask how near that player is. What follows only changes the guards' states.
    {"mansio2", 0x25B5E6, 0x261287, 0x09ED, 0x2612E6, false},  // HAS_CHAR_SPOTTED_CHAR_IN_FRONT $PLAYER
    {"mansio2", 0x25B5E6, 0x2614F8, 0x09ED, 0, false},
    {"mansio2", 0x25B5E6, 0x2647C7, 0x09ED, 0x264826, false},
    {"mansio2", 0x25B5E6, 0x264BA2, 0x09ED, 0, false},
};

// A stretch of script going for another player
struct Stretch {
    void* script = nullptr;
    int32_t actor = -1;
    uint32_t from = 0, to = 0;
    const Spot* spot = nullptr;
    uint8_t who = 0xFF;
} g_stretch;

// "Deconstruction" breaks a cabin by the car of its player only: the car touching it, its speed, the model (a bulldozer
// counts 30 times). The partner rammed them in its own bulldozer for nothing. The players with a car take the
// mission's check in turns, a pass each, and the car's check holds its place in the script's own variables.
constexpr uint32_t kDeconStart = 0x1515E0, kDeconFrom = 0x1557C6 - kDeconStart, kDeconTo = 0x155E79 - kDeconStart;
void DeconTurn(void* script) {
    static unsigned turn = 0;
    std::vector<int32_t> cars;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState && rp.ped->InVehicle()) cars.push_back(rp.pedHandle);
    if (cars.empty()) return;
    unsigned k = turn++ % (unsigned)(cars.size() + 1);
    if (k < cars.size()) g_stretch = {script, cars[k], kDeconFrom, kDeconTo, nullptr, 0xFF};
}

// The answer of the command at ip for that person, as it stands (NOT off) or as written (NOT on): its handler again
bool Ask(void* script, void* handler, int command, const uint8_t* ip, int32_t who, bool written) {
    uint8_t*& cur = Field<uint8_t*>(script, 0x14);     // CRunningScript::m_pCurrentIP
    uint8_t& cond = Field<uint8_t>(script, 0xC5);      // m_CondResult
    uint16_t& andOr = Field<uint16_t>(script, 0xD0);   // m_AndOrState
    uint8_t& notFlag = Field<uint8_t>(script, 0xD2);   // m_NotFlag
    uint8_t* after = cur;
    uint8_t c = cond, n = notFlag;
    uint16_t a = andOr;
    int32_t own = Actor();
    Actor() = who;
    andOr = 0;
    if (!written) notFlag = 0;
    cur = const_cast<uint8_t*>(ip) + 2;
    ((CommandHandlerFn)handler)(script, command);
    bool yes = cond != 0;
    Actor() = own;
    cur = after, cond = c, andOr = a, notFlag = n;
    return yes;
}

const Spot* SpotAt(void* script, uint16_t op, const uint8_t* ip) {
    uintptr_t rel = (uintptr_t)ip - kMissionBlock;
    for (const Spot& s : kSpots)
        if (s.op == op && rel == s.at - s.start && Named(script, s.mission)) return &s;
    return nullptr;
}

// the people another player aims at, by their net ids (what its game tells, G_PLAYER_AIM), and its enter key
enum : uint8_t { AIM_PED = 1, AIM_ENTER = 2, AIM_THROW = 3 };
std::map<uint8_t, uint16_t> g_aims;
std::map<uint8_t, uint64_t> g_enterMs;

int TargetAnswer(uint16_t op) {
    const int32_t* p = Params();
    if (p[0] != 0) return -1;  // IS_PLAYER_TARGETTING_*: player 0, the mission's
    if (op == 0x068C) {        // IS_PLAYER_TARGETTING_ANYTHING
        for (auto& [id, ped] : g_aims)
            if (ped) return 1;
        return -1;
    }
    CPed* who = GetPed(p[1]);
    NetPed* np = who ? Peds::Instance().Find(who) : nullptr;
    if (!np) return -1;
    for (auto& [id, ped] : g_aims)
        if (ped == np->netId) return 1;
    return -1;
}

// partner: who our player aims at, when it changes. Locked on (m_pTargetedObject) or under the mouse's crosshair
// (m_p3rdPersonMouseTarget), as IS_PLAYER_TARGETTING_CHAR asks.
void SendAim() {
    static uint16_t sent = 0;
    uint16_t now = 0;
    if (CPlayerPed* me = HostMissionRecent(0) ? FindPlayerPed(0) : nullptr) {
        CEntity* target = me->TargetedObject();
        CPed* ped = target && target->IsPed() ? (CPed*)target : Field<CPed*>(me, 0x79C);
        if (NetPed* np = ped ? Peds::Instance().Find(ped) : nullptr) now = np->netId;
    }
    const CPad* pad = GetPad(0);
    if (HostMissionRecent(0) && pad->NewState.ButtonTriangle && !pad->OldState.ButtonTriangle) {
        Writer w(proto::G_PLAYER_AIM);
        w.U8(AIM_ENTER);
        w.U16(0);
        Net().Send(proto::CH_RELIABLE, w);
    }
    if (now == sent) return;
    sent = now;
    Writer w(proto::G_PLAYER_AIM);
    w.U8(AIM_PED);
    w.U16(now);
    Net().Send(proto::CH_RELIABLE, w);
}

// "Catalyst": the box of another player, as the mission makes its own from the camera (0xC3680): in its next free place of
// the thrown boxes (110@ on, the model in 166@ on), so it catches and counts it as its player's.
void HostThrow(const CVector& from, const CVector& to, float power) {
    void* mission = RunningMission();
    CPlayerPed* me = FindPlayerPed(0);
    if (!mission || !Named(mission, "ryder3") || !me->AttachedTo() || !std::isfinite(power)) return;
    auto* locals = (int32_t*)0xA48960;  // CTheScripts::LocalVariablesForCurrentMission
    int i = locals[109];
    if (i < 0 || i > 9) return;
    if (GetObject(locals[110 + i])) Cmd(0x0108).I(locals[110 + i]).Run();  // DELETE_OBJECT
    int32_t box = -1;
    Cmd(0x029B).I(locals[127]).F(to.x).F(to.y).F(to.z).Out(&box).Run();  // CREATE_OBJECT_NO_OFFSET
    if (box == -1) return;
    Cmd(0x04D9).I(box).I(1).Run();  // SET_OBJECT_RECORDS_COLLISIONS
    Cmd(0x0392).I(box).I(1).Run();  // SET_OBJECT_DYNAMIC
    Cmd(0x0382).I(box).I(1).Run();  // SET_OBJECT_COLLISION
    CVector v = (to - from) * std::clamp(power * 0.5f, 10.0f, 50.0f);
    if (CVehicle* train = GetVehicle(locals[163])) v = v + train->MoveSpeed() * 50.0f;
    Cmd(0x0381).I(box).F(v.x).F(v.y).F(v.z).Run();  // SET_OBJECT_VELOCITY
    locals[110 + i] = box;
    locals[166 + i] = locals[127];
    locals[109] = (i + 1) % 10;
    AnnounceKeptObject(box);
}

// What the host's mission did to its game, for the partners by the host (G_MISSION_ENV). The SAM sites shoot down whoever
// flies past them in the game they are on in: "Vertical Bird" and "Dam and Blast" turn them off for the host alone. The
// darkness of the Crack Palace and the casino's vault ("End of the Line", "Breaking the Bank at Caligula's") fell on the
// host's screen only. "Burning Desire" lowers the fire's harm to its player: the partner in the same house
// burnt three times faster. The siren of the carrier in "Vertical Bird" was the host's alone (05.11): its sound bank comes
// with the mission's audio (MissionSpeech), the two events with this message.
enum : uint8_t { ENV_AREA51 = 1, ENV_CARRIER = 2, ENV_DARK = 3, ENV_FIRE = 4, ENV_ALARM = 5 };
constexpr int32_t kAlarmStart = 1161, kAlarmStop = 1162;  // AE_SCRIPT_VERTICAL_BIRD_ALARM_START, _STOP
bool g_changed[6] = {};
void SendEnv(uint8_t kind, int32_t a, int32_t b) {
    Writer w(proto::G_MISSION_ENV);
    w.U8(kind);
    w.I32(a);
    w.I32(b);
    Net().Send(proto::CH_RELIABLE, w);
}

void ApplyEnv(uint8_t kind, int32_t a, int32_t b) {
    CPlayerPed* me = FindPlayerPed(0);
    if (kind == ENV_AREA51) Cmd(0x07A8).I(a).Run();        // SET_AREA51_SAM_SITE
    else if (kind == ENV_CARRIER) Cmd(0x09E4).I(a).Run();  // SET_AIRCRAFT_CARRIER_SAM_SITE
    else if (kind == ENV_DARK) Cmd(0x0924).I(a).I(b).Run();  // SET_DARKNESS_EFFECT
    else if (kind == ENV_ALARM) Cmd(0x097A).F(0.0f).F(0.0f).F(0.0f).I(a).Run();  // REPORT_MISSION_AUDIO_EVENT_AT_POSITION
    else if (kind == ENV_FIRE && me) {
        float f;
        memcpy(&f, &a, 4);
        if (f > 0.0f && f <= 1.0f) Cmd(0x093E).I(GetPedRef(me)).F(f).Run();  // SET_CHAR_FIRE_DAMAGE_MULTIPLIER
    }
}

void HandleEnv(uint8_t from, Reader& r) {
    uint8_t kind = r.U8();
    int32_t a = r.I32(), b = r.I32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || kind < ENV_AREA51 ||
        kind > ENV_ALARM || (kind == ENV_ALARM && a != kAlarmStart && a != kAlarmStop))
        return;
    // the dark and the siren are where the host is: a partner off in another part of the city keeps its daylight and its quiet
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    bool here = kind == ENV_DARK ? a != 0 : kind == ENV_ALARM && a == kAlarmStart;
    if (here && (!host || !host->hasState || host->state.area != me->AreaCode() ||
                 (host->state.pos - me->GetPosition()).Length() > 300.0f))
        return;
    ApplyEnv(kind, a, b);
    g_changed[kind] = kind != ENV_ALARM || a == kAlarmStart;
    LOGI("The host's mission changed the world here too (%u: %d %d)", kind, a, b);
}

// The mission's effects (CREATE_FX_SYSTEM and its kin: the smoke of the burning house of "Burning Desire", the searchlights' beams
// of "Black Project", the sparks of the finale) burnt in the host's game only. Made here by the same commands, on the
// same things. A script's effects are kept in an array of 32 (CTheScripts::ScriptEffectSystemArray): ours stay well under it.
enum : uint8_t { FX_MAKE = 1, FX_PLAY = 2, FX_STOP = 3, FX_PLAY_KILL = 4, FX_KILL = 5 };
constexpr size_t kMaxFx = 24;
std::map<int32_t, int32_t> g_fx;  // partner: the host's handle, ours

float ArgFloat(const uint8_t* a) {
    float f = 0.0f;
    if (a && a[0] == 0x06) memcpy(&f, a + 1, 4);
    else if (int32_t v; script::ArgValue(a, v)) memcpy(&f, &v, 4);
    return f;
}

void SendFxMade(uint16_t op, const uint8_t* ip) {
    int32_t handle = Params()[0];  // the command stored it here last
    const uint8_t* a = ip + 2;
    if (a[0] != 0x0E) return;
    std::string name((const char*)a + 2, a[1]);
    a = script::NextArg(a);
    uint8_t kind = 0;
    uint16_t id = 0;
    if (op != 0x064B) {
        int32_t e = -1;
        if (!script::ArgValue(a, e)) return;
        a = script::NextArg(a);
        if (op == 0x066B || op == 0x066C) {
            NetVehicle* nv = GetVehicle(e) ? Vehicles::Instance().Find(GetVehicle(e)) : nullptr;
            if (nv) kind = 1, id = nv->netId;
        } else if (op == 0x0669 || op == 0x066A) {
            CPed* ped = GetPed(e);
            NetPed* np = ped ? Peds::Instance().Find(ped) : nullptr;
            if (ped == FindPlayerPed(0)) kind = 3;
            else if (np) kind = 2, id = np->netId;
        } else if (uint16_t object = GetObject(e) ? MissionObjectNetId(GetObject(e)) : 0) {
            kind = 4, id = object;
        }
        if (!kind) return;  // not shared (yet): this one stays here
    }
    bool direction = op == 0x066A || op == 0x066C || op == 0x066E;
    float v[6] = {};
    for (int i = 0; i < (direction ? 6 : 3); ++i) v[i] = ArgFloat(a), a = script::NextArg(a);
    int32_t flag = 0;
    script::ArgValue(a, flag);
    Writer w(proto::G_MISSION_FX);
    w.U8(FX_MAKE);
    w.I32(handle);
    w.U16(op);
    w.Str(name, 32);
    w.U8(kind);
    w.U16(id);
    for (float f : v) w.F32(f);
    w.U8(flag ? 1 : 0);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendFx(uint8_t what) {
    Writer w(proto::G_MISSION_FX);
    w.U8(what);
    w.I32(Params()[0]);
    Net().Send(proto::CH_RELIABLE, w);
}

int FxOn(uint8_t kind, uint16_t id) {
    if (kind == 1) {
        NetVehicle* nv = Vehicles::Instance().Find(id);
        return nv && Vehicles::Alive(*nv) ? nv->handle : -1;
    }
    if (kind == 2) return Peds::Instance().HandleOf(id);
    if (kind == 4) return MissionObjectCopyHandle(id);
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    return host && host->ped ? host->pedHandle : -1;
}

void MakeFx(int32_t handle, uint16_t op, const std::string& name, uint8_t kind, uint16_t id, const float* v, bool flag) {
    if (g_fx.size() >= kMaxFx || name.empty()) return;
    for (char c : name)
        if (!isalnum((unsigned char)c) && c != '_') return;
    for (int i = 0; i < 6; ++i)
        if (!std::isfinite(v[i])) return;
    bool plain = op == 0x064B, direction = op == 0x066A || op == 0x066C || op == 0x066E;
    if (!plain && op != 0x0669 && op != 0x066B && op != 0x066D && !direction) return;
    int on = plain ? 0 : FxOn(kind, id);
    if (on < 0) return;
    Cmd c(op);
    c.S(name.c_str());
    if (!plain) c.I(on);
    for (int i = 0; i < (direction ? 6 : 3); ++i) c.F(v[i]);
    int32_t ours = -1;
    c.I(flag ? 1 : 0).Out(&ours).Run();
    if (ours != -1) g_fx[handle] = ours;
}

void HandleFx(uint8_t from, Reader& r) {
    uint8_t what = r.U8();
    int32_t handle = r.I32();
    if (from != Net().HostId() || Net().IsHost()) return;
    if (what == FX_MAKE) {
        uint16_t op = r.U16();
        std::string name = r.Str(32);
        uint8_t kind = r.U8();
        uint16_t id = r.U16();
        float v[6];
        for (float& f : v) f = r.F32();
        bool flag = r.U8() != 0;
        if (r.Ok() && core::IsPlayerReady() && flow::InHostWorld()) MakeFx(handle, op, name, kind, id, v, flag);
        return;
    }
    auto it = g_fx.find(handle);
    if (!r.Ok() || !core::IsPlayerReady() || it == g_fx.end()) return;
    if (what == FX_PLAY) Cmd(0x064C).I(it->second).Run();        // PLAY_FX_SYSTEM
    else if (what == FX_STOP) Cmd(0x064E).I(it->second).Run();   // STOP_FX_SYSTEM
    else if (what == FX_PLAY_KILL || what == FX_KILL) {
        Cmd(what == FX_KILL ? 0x0650 : 0x064F).I(it->second).Run();  // KILL_FX_SYSTEM, PLAY_AND_KILL_FX_SYSTEM
        g_fx.erase(it);
    }
}

class MissionRules : public Module {
public:
    const char* Name() const override { return "MissionRules"; }
    void OnInstallHooks() override {
        Net().SetHandler(proto::G_PLAYER_AIM, [](uint8_t from, Reader& r) {
            uint8_t op = r.U8();
            uint16_t ped = r.U16();
            if (!r.Ok() || !Net().IsHost() || from == Net().MyId() || !core::IsPlayerReady()) return;
            if (op == AIM_PED) g_aims[from] = ped;
            else if (op == AIM_ENTER) g_enterMs[from] = NowMs();
            else if (op == AIM_THROW) {
                Vec3 a = r.Vec(), b = r.Vec();
                float power = r.F32();
                if (r.Ok() && std::isfinite(a.x + a.y + a.z + b.x + b.y + b.z)) HostThrow({a.x, a.y, a.z}, {b.x, b.y, b.z}, power);
            }
        });
        Net().SetHandler(proto::G_MISSION_ENV, &HandleEnv);
        Net().SetHandler(proto::G_MISSION_FX, &HandleFx);
    }
    void OnPostProcess() override {
        if (Net().Online() && !Net().IsHost() && core::IsPlayerReady()) SendAim();
    }
    void OnPeerLeave(uint8_t id) override {
        g_aims.erase(id);
        g_enterMs.erase(id);
    }
    void OnSessionEnd() override {
        g_aims.clear();
        g_enterMs.clear();
        g_stretch = {};
    }
};
MissionRules g_rules;
struct Reg {
    Reg() { core::RegisterModule(&g_rules); }
} g_reg;
}  // namespace

int32_t* BeforeCommand(void* script, const uint8_t* ip, int32_t& was) {
    uintptr_t rel = (uintptr_t)ip - kMissionBlock;
    if (rel == kDeconFrom && !g_stretch.script && Named(script, "decon")) DeconTurn(script);
    if (!g_stretch.script || g_stretch.script != script) return nullptr;
    if (rel < g_stretch.from || rel >= g_stretch.to || !GetPed(g_stretch.actor)) {
        g_stretch = {};
        return nullptr;
    }
    was = Actor();
    Actor() = g_stretch.actor;
    return &Actor();
}

int Answer(void* script, void* handler, int command, const uint8_t* ip) {
    uint16_t op = (uint16_t)command;
    if (op == 0x0457 || op == 0x068C) return TargetAnswer(op);
    // the button of that place, pressed in the game of who stands there
    if (op == 0x00E1 && g_stretch.script == script && g_stretch.spot && g_stretch.spot->button &&
        (uintptr_t)ip - kMissionBlock == g_stretch.spot->button - g_stretch.spot->start) {
        if (g_stretch.spot->anyKey) return 1;
        auto press = g_enterMs.find(g_stretch.who);
        return press != g_enterMs.end() && NowMs() - press->second < 1000;
    }
    int32_t own = Actor();
    bool any = ((op == 0x051A || op == 0x051C) && Params()[1] == own) || ((op == 0x02E0 || op == 0x02D6) && Params()[0] == own);
    const Spot* spot = any ? nullptr : SpotAt(script, op, ip);
    if (!any && !spot) return -1;
    bool written = spot && spot->written;
    if (Ask(script, handler, command, ip, own, written)) return -1;  // our player does: the game's own answer
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.ped || !rp.hasState || (rp.state.flags & OnFootState::F_DEAD) || !Ask(script, handler, command, ip, rp.pedHandle, written))
            continue;
        if (spot && spot->until) g_stretch = {script, rp.pedHandle, spot->at - spot->start, spot->until - spot->start, spot, id};
        if (spot) LOGD("%.8s: the line at %06X goes for player %d", Field<char[8]>(script, 0x08), spot->at, id);
        if (spot && spot->hint) {  // the mission shows it to its own player alone
            Writer w(proto::G_MISSION_HINT);
            w.U8(1);
            w.Str(spot->hint, 8);
            Net().Send(proto::CH_RELIABLE, w);
        }
        return written ? !Field<uint8_t>(script, 0xD2) : 1;  // UpdateCompareFlag puts the NOT on again
    }
    return -1;
}

void AfterCommand(uint16_t op, const uint8_t* ip) {
    const int32_t* p = Params();
    if (op == 0x064B || (op >= 0x0669 && op <= 0x066E)) return SendFxMade(op, ip);
    if (op == 0x064C) return SendFx(FX_PLAY);
    if (op == 0x064E) return SendFx(FX_STOP);
    if (op == 0x064F) return SendFx(FX_PLAY_KILL);
    if (op == 0x0650 || op == 0x0976) return SendFx(FX_KILL);
    if (op == 0x07A8) SendEnv(ENV_AREA51, p[0], 0);
    else if (op == 0x09E4) SendEnv(ENV_CARRIER, p[0], 0);
    else if (op == 0x0924) SendEnv(ENV_DARK, p[0], p[1]);
    else if (op == 0x093E && p[0] == Actor()) SendEnv(ENV_FIRE, p[1], 0);
    else if (op == 0x097A && (p[3] == kAlarmStart || p[3] == kAlarmStop)) SendEnv(ENV_ALARM, p[3], 0);
}

bool SkipCommand(void* script, uint16_t op) {
    if (op != 0x01B4 || !g_stretch.spot || g_stretch.script != script) return false;  // SET_PLAYER_CONTROL
    uintptr_t rel = (uintptr_t)(Field<uint8_t*>(script, 0x14) - 2) - kMissionBlock;
    if (rel < g_stretch.from || rel >= g_stretch.to) return false;
    CallMethod<0x464080>(script, (int16_t)2);  // CRunningScript::CollectParameters
    return true;
}

void Throw(float power) {
    CVector from = cam::Source(), to = from + cam::Front();
    Writer w(proto::G_PLAYER_AIM);
    w.U8(AIM_THROW);
    w.U16(0);
    w.Vec({from.x, from.y, from.z});
    w.Vec({to.x, to.y, to.z});
    w.F32(power);
    Net().Send(proto::CH_RELIABLE, w);
}

void MissionOver() {
    if (!core::IsPlayerReady()) return;
    float one = 1.0f;
    int32_t fire;
    memcpy(&fire, &one, 4);
    if (g_changed[ENV_AREA51]) ApplyEnv(ENV_AREA51, 1, 0);
    if (g_changed[ENV_CARRIER]) ApplyEnv(ENV_CARRIER, 1, 0);
    if (g_changed[ENV_DARK]) ApplyEnv(ENV_DARK, 0, -1);
    if (g_changed[ENV_FIRE]) ApplyEnv(ENV_FIRE, fire, 0);
    if (g_changed[ENV_ALARM]) ApplyEnv(ENV_ALARM, kAlarmStop, 0);
    memset(g_changed, 0, sizeof g_changed);
    for (auto& [host, ours] : g_fx) Cmd(0x0650).I(ours).Run();  // KILL_FX_SYSTEM
    g_fx.clear();
}

}  // namespace coop::rules
