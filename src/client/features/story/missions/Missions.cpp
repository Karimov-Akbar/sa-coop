// Story missions run only in the host's game; the partner sees what the mission shows.
//
//  * partner: mission triggers of main.scm are refused (CPlayerPed::CanPlayerStartMission returns false
//    while a main.scm thread asks). Shops, food, barbers... ask from streamed scripts and keep working;
//  * host: a trigger is refused too until the partner stands in the same marker (G_MISSION_WAIT shows it);
//  * host -> partner while a mission (or its trigger) runs:
//      texts       - CMessages small/big messages and help boxes (G_TEXT);
//      radar blips - blips created after the mission started, on the same vehicles/peds or coordinates (G_BLIP);
//      markers     - "locate" destination cylinders (CTheScripts::HighlightImportantArea, G_MARKER), race
//                    checkpoints and script spheres (G_CHECKPOINTS);
//      weapons     - what the mission gives its CJ (a spray can for the tags) and takes back (G_MISSION_WEAPON);
//      cutscenes   - the partner loads and plays the same cutscene (G_CUTSCENE);
//      screen      - fades and widescreen borders (G_FADE), onscreen timers/counters (G_ONSCREEN).
// Mission peds and vehicles are ordinary host entities (Peds/Vehicles).
#include <windows.h>
#undef GetObject  // windows.h renames it GetObjectA: game::GetObject is the script handle lookup

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "features/story/missions/Missions.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "features/story/cutscene-partner/ScenePartner.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

// ---------------------------------------------------------------- engine
constexpr uintptr_t kActiveScripts = 0xA8B42C;  // CTheScripts::pActiveScripts
constexpr uintptr_t kMainScriptSize = 0xA444C4;
constexpr uintptr_t kRadarTraces = 0xBA86F0;    // CRadar::ms_RadarTrace[175], 0x28 bytes
constexpr int kNumTraces = 175;  // vec2DRadarOrigin follows at 0xBAA248, then the HUD
constexpr uintptr_t kOnscreenTimer = 0xBA1788;  // CUserDisplay::OnscnTimer (COnscreenTimer, 0x154 bytes)
constexpr size_t kOnscreenSize = 0x154;
constexpr uintptr_t kCutsceneRunning = 0xB5F851, kCutsceneLoadStatus = 0xB5F84C, kCutsceneOffset = 0xBC4034;
constexpr int BLIP_CAR = 1, BLIP_CHAR = 2, BLIP_OBJECT = 3, BLIP_COORD = 4;

bool CutsceneRunning() { return Ref<uint8_t>(kCutsceneRunning) != 0; }
uintptr_t Trace(int i) { return kRadarTraces + (uintptr_t)i * 0x28; }

void* FindMissionScript() {
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00))
        if (Field<uint8_t>(s, 0xDC)) return s;  // m_bIsMission
    return nullptr;
}

// a property purchase (BUYPRO1) is no mission for the partner: it only shows the house, Property.cpp shares the purchase
// The save points' thread (PSAVE1) raises the mission flag over its save menu: no mission either (the partner's screen
// went dark with the host's save and "the host started a mission" came up).
bool g_saveFlag = false;  // the flag up is PSAVE1's

// The missions of side activities (main.scm's 3..10 and 113..133: the arcade and pool games, the shooting range, the
// gyms, the schools, the races, the vehicle jobs) are each player's own, as in a game of one's own. Counted as the host's
// story they paid the partner the host's taxi fares, refused the partner's own activities while they ran and filled its
// chat with "started a mission".
bool SideMission(void* script) {
    static const char* const kSide[] = {"none", "dual", "shtr", "grav", "otb", "pool2", "lowr", "zero5",
                                        "shrange", "gymls", "gymsf", "gymlv", "truck", "quarry", "boat", "bskool",
                                        "taxiodd", "ambulan", "firetru", "copcar", "burgjb", "freight", "pimp", "blood",
                                        "kicksta", "tria", "bcour", "mtbiker", "stunt"};
    const char* name = Field<char[8]>(script, 0x08);
    for (const char* side : kSide)
        if (!_strnicmp(name, side, 8)) return true;
    return false;
}

bool HostOnMission() {
    void* s = FindMissionScript();
    if (s && (!strncmp(Field<char[8]>(s, 0x08), "buypro1", 8) || SideMission(s))) return false;
    if (!core::IsOnMission()) g_saveFlag = false;
    return s || (core::IsOnMission() && !g_saveFlag);
}

// A save can bring the mission flag up with nothing in it left to lower it: a save house's save has it up and PSAVE1
// lowers it a second after the load, a quick save of a mod's or one made while dead keeps it. With it up the game shows
// no mission markers and no save disks, launches no story mission, and the partners see the host on a mission for good
// (both players of a session lost the story after a host swap). Lowered when a minute after the load nothing holds it.
uint64_t g_inheritedFlagMs = 0;
void HealInheritedFlag() {
    if (!g_inheritedFlagMs) return;
    if (!core::IsOnMission() || FindMissionScript()) {
        g_inheritedFlagMs = 0;  // its holder let it go, or a mission runs
    } else if (NowMs() - g_inheritedFlagMs > 60000) {
        Ref<int32_t>(addr::ScriptSpace + (uintptr_t)Ref<int32_t>(0xA476AC)) = 0;  // CTheScripts::OnAMissionFlag: $ONMISSION
        g_inheritedFlagMs = 0;
        LOGW("The loaded world kept the mission flag up with no script holding it: lowered");
    }
}

void Fade(float seconds, int16_t dir) { CallMethod<0x50AC20>((void*)addr::TheCamera, seconds, dir); }  // dir: 0 out, 1 in
float FadeAlpha() { return Ref<float>(addr::TheCamera + 0xBFC); }
bool Fading() { return Ref<uint8_t>(addr::TheCamera + 0x51) != 0; }
int16_t FadeDirection() { return Ref<int16_t>(addr::TheCamera + 0xC30); }
// Dark or going dark. A fade-out asked in this frame's messages has not moved the alpha yet (CCamera::ProcessFade runs
// in CGame::Process, after them): the host's "out" and "in" handled together (a skipped scene: DO_FADE 0 0, and two
// frames later the fade in) dropped the "in", the "out" went on, and the partner's screen stayed black for good.
bool Darkening() { return FadeAlpha() > 1.0f || (Fading() && FadeDirection() == 0); }
float FadeDuration() { return Ref<float>(addr::TheCamera + 0xC04); }
bool TheCameraWide() { return Ref<uint8_t>(addr::TheCamera + 0x3D) != 0; }
void SetWideScreen(bool on) {
    if (on) CallMethod<0x50C140>((void*)addr::TheCamera);
    else CallMethod<0x50C150>((void*)addr::TheCamera);
}

// ---------------------------------------------------------------- state
struct BlipRecord {  // what the host sends for one blip
    uint8_t index = 0;
    uint8_t type = 0, entKind = 0;  // entKind: 0 coordinates, 1 vehicle netId, 2 ped netId, 3 player id
    uint16_t entId = 0;
    CVector pos;
    uint32_t colour = 0;
    uint16_t size = 0;
    uint8_t sprite = 0, flags25 = 0, display = 0;

    void Write(Writer& w) const {
        w.U8(index);
        w.U8(type);
        w.U8(entKind);
        w.U16(entId);
        w.Vec({pos.x, pos.y, pos.z});
        w.U32(colour);
        w.U16(size);
        w.U8(sprite);
        w.U8(flags25);
        w.U8(display);
    }
    void Read(Reader& r) {
        index = r.U8();
        type = r.U8();
        entKind = r.U8();
        entId = r.U16();
        Vec3 p = r.Vec();
        pos = {p.x, p.y, p.z};
        colour = r.U32();
        size = r.U16();
        sprite = r.U8();
        flags25 = r.U8();
        display = r.U8();
    }
    bool operator==(const BlipRecord& o) const {
        // an entity's blip goes with the copy of the entity: its place counts only for a partner without that copy, and
        // every moving car or person with a blip went out again every 250 ms on the reliable channel
        return type == o.type && entKind == o.entKind && entId == o.entId && (pos - o.pos).Length() < (entKind ? 20.0f : 0.5f) &&
               colour == o.colour && size == o.size && sprite == o.sprite && flags25 == o.flags25 && display == o.display;
    }
};

enum : uint8_t { AREA_LOCATE = 0, AREA_SPHERE = 1 };
struct Area {
    uint32_t id;
    float x1, y1, x2, y2, z;  // a sphere: centre x1, y1, z and radius x2
    uint64_t seenMs;
    uint8_t kind = AREA_LOCATE;
};

enum : uint8_t { BLIP_SET = 1, BLIP_REMOVE = 2, BLIP_CLEAR = 3 };
enum : uint8_t { CS_LOAD = 1, CS_START = 2, CS_END = 3, CS_SKIP_VOTE = 4, CS_SKIP_STATE = 5, CS_SPOT = 6 };
enum : uint8_t { TXT_MSG = 1, TXT_MSG_Q, TXT_BIG, TXT_BIG_Q, TXT_HELP, TXT_CLEAR, TXT_CLEAR_SMALL };
enum : uint8_t { FADE_ACTIVE = 1, FADE_WIDE = 2, FADE_BLACK = 4 };

// host
bool g_active = false;                        // a mission or its trigger runs
std::set<uint32_t> g_baseline;                // (counter << 8 | index) of blips that existed before
std::map<uint8_t, BlipRecord> g_sentBlips;
std::map<uint32_t, Area> g_areas;
std::vector<uint8_t> g_lastAreasMsg, g_lastOnscreen;
uint64_t g_lastTick = 0, g_lastFadeMs = 0;
bool g_hostCutscene = false;
std::string g_missionName;
int32_t g_moneyAtStart = 0;  // the partner gets what the host earned during the mission
char g_cutsceneName[32] = {};  // last loaded cutscene, for partners joining during it

// mission markers: the host's mission starts once the partner stands in the marker too
uint64_t g_waitMs = 0;      // host: the last time a trigger was refused for that
bool g_waitSent = false;
bool g_hostWaits = false;   // partner
Area g_waitArea{};

// The other way round: a partner stands in a mission marker, but only the host starts missions. The partner's game tells
// the host (the same G_MISSION_WAIT, from a partner: a host of an older version ignores it), the host is told in the chat
// and on screen, and sees that marker lit.
struct PartnerWait {
    Area area{};
    uint64_t sinceMs = 0;
};
std::map<uint8_t, PartnerWait> g_partnersWaiting;  // host
bool g_atMarkerSent = false;                       // partner
uint64_t g_atMarkerCheckMs = 0;

// partner
struct PartnerBlip {
    int handle = 0;
    uint8_t entKind = 0;
    uint16_t entId = 0;
};
std::map<uint8_t, PartnerBlip> g_blips;
std::vector<Area> g_partnerAreas;
uint64_t g_partnerAreasMs = 0;
bool g_partnerMission = false, g_weFaded = false, g_weWide = false, g_onscreenSet = false;
uint64_t g_partnerMissionEndedAt = 0;
int g_csState = 0;  // 0 none, 1 loading, 2 running, 3 ending
int32_t g_csArea = 0;  // interior the host's cutscene plays in
bool g_csStartRequested = false;

bool Replicating() { return Net().Online() && Net().IsHost() && !Net().Peers().empty() && (g_active || HostOnMission()); }

// ---------------------------------------------------------------- partner: no mission triggers
void* g_currentScript = nullptr;
void ScriptFaded(int16_t dir, float seconds);
enum : uint8_t { WEAPON_GIVE = 1, WEAPON_TAKE = 2 };

// What the host's mission does to its CJ's weapons (GIVE_WEAPON_TO_CHAR $PLAYER_ACTOR, REMOVE_WEAPON_FROM_CHAR) the
// partner gets as well: the spray can of "Tagging Up Turf", the guns some missions hand out. Seen as the change of CJ's
// slots over a mission script's commands.
struct WeaponSlots {
    int32_t type[NUM_WEAPON_SLOTS], ammo[NUM_WEAPON_SLOTS];
    void Take(CPed* p) {
        for (int i = 0; i < NUM_WEAPON_SLOTS; ++i) type[i] = p->Weapons()[i].type, ammo[i] = p->Weapons()[i].ammoTotal;
    }
};
void SendMissionWeapon(uint8_t op, int weapon, int ammo, bool inHand) {
    Writer w(proto::G_MISSION_WEAPON);
    w.U8(op);
    w.U8((uint8_t)weapon);
    w.I32(ammo);
    w.U8(inHand);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Mission weapon %d %s (%d)", weapon, op == WEAPON_GIVE ? "given" : "taken", ammo);
}
// The count goes as it is now, not what was added: missions lower it too (Emmet's pistol 30000 for the practice and 10
// after it, the spray can given again with 1000 at the end), and the partner kept the big one (no number on the HUD).
void SendWeaponChanges(const WeaponSlots& was, CPed* cj) {
    for (int i = 0; i < NUM_WEAPON_SLOTS; ++i) {
        const CWeapon& now = cj->Weapons()[i];
        if (was.type[i] > 0 && now.type != was.type[i]) SendMissionWeapon(WEAPON_TAKE, was.type[i], 0, false);
        if (now.type > 0 && (now.type != was.type[i] || now.ammoTotal != was.ammo[i]))
            SendMissionWeapon(WEAPON_GIVE, now.type, now.ammoTotal, cj->ActiveWeapon().type == now.type);
    }
}

using ScriptProcessFn = void(__fastcall*)(void*, void*);
ScriptProcessFn g_origScriptProcess = nullptr;
bool Replicating();
void __fastcall HookScriptProcess(void* script, void* edx) {
    // a world load from inside the game: the old scripts would run frames without a player (null peds)
    if (core::IsRestarting()) return;
    void* prev = g_currentScript;
    g_currentScript = script;
    bool fading = Fading();
    int16_t dir = FadeDirection();
    float seconds = FadeDuration();
    CPlayerPed* cj = Field<uint8_t>(script, 0xDC) && Net().IsHost() && Replicating() ? FindPlayerPed(0) : nullptr;  // m_bIsMission
    WeaponSlots weapons;
    if (cj) weapons.Take(cj);
    bool save = !strncmp(Field<char[8]>(script, 0x08), "psave1", 8), flag = save && core::IsOnMission();
    g_origScriptProcess(script, edx);
    if (save && core::IsOnMission() != flag) g_saveFlag = !flag;
    g_currentScript = prev;
    if (cj && FindPlayerPed(0) == cj) SendWeaponChanges(weapons, cj);
    // a fade that started during this script's commands is the script's (DO_FADE)
    if (Fading() && (!fading || FadeDirection() != dir || FadeDuration() != seconds)) ScriptFaded(FadeDirection(), FadeDuration());
}

// CLEAR_AREA of the host's mission: alone, the game removes the empty ordinary cars from the place where the mission
// puts its own next, but a car the partner left there stayed (its copy is a mission car here): "Ryder" put Ryder's car
// on it at the pizza place, the car was wrecked and the mission failed. Those copies go now, their owners delete them.
using ClearAreaFn = void(__cdecl*)(CVector*, float, uint8_t);
ClearAreaFn g_origClearArea = nullptr;
void __cdecl HookScriptClearArea(CVector* at, float radius, uint8_t flag) {  // CWorld::ClearExcitingStuffFromArea
    g_origClearArea(at, radius, flag);
    if (!g_currentScript || !Field<uint8_t>(g_currentScript, 0xDC) || !Net().IsHost() || !Replicating()) return;
    std::vector<uint16_t> ids = Vehicles::Instance().ClearMirrors(*at, radius);
    if (ids.empty()) return;
    if (ids.size() > 255) ids.resize(255);
    Writer w(proto::G_VEH_CLEAR);
    w.U8((uint8_t)ids.size());
    for (uint16_t id : ids) w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("CLEAR_AREA at %.0f %.0f, radius %.0f: %zu cars of the other players cleared", at->x, at->y, radius, ids.size());
}

std::set<int> g_missionWeapons;  // partner: what the host's mission gave (only those it takes back)
std::map<int, std::pair<int, int>> g_replaced;  // partner: its own weapon a given one pushed out of the slot (type, ammo)
// The partner takes the host's mission state only while playing the host's world: one still in its own game (waiting
// for the world, or for the host's prologue) got the host's cutscenes, texts, weapons and rewards in it.
void HandleMissionWeapon(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    int weapon = r.U8();
    int ammo = r.I32();
    bool inHand = r.U8() != 0;
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || weapon <= 0 || weapon > 46) return;
    CPlayerPed* me = FindPlayerPed(0);
    int h = GetPedRef(me);
    auto give = [&](int type, int count) {
        int model = -1;
        Cmd(0x0781).I(type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
        if (model > 0) LoadModelNow(model);
        Cmd(0x01B2).I(h).I(type).I(std::clamp(count, 1, 99999)).Run();  // GIVE_WEAPON_TO_CHAR
    };
    if (op == WEAPON_GIVE) {
        WeaponSlots was;
        was.Take(me);
        give(weapon, ammo);
        Cmd(0x017B).I(h).I(weapon).I(std::clamp(ammo, 1, 99999)).Run();  // SET_CHAR_AMMO: the host's count, not ours + it
        if (inHand) Cmd(0x01B9).I(h).I(weapon).Run();  // SET_CURRENT_CHAR_WEAPON
        // a slot holds one weapon: the partner's own one is put back later, one of the same kind too (its ammo was
        // overwritten with the mission's, and taking the mission's took the partner's own gun)
        for (int i = 0; i < NUM_WEAPON_SLOTS; ++i)
            if (was.type[i] > 0 && me->Weapons()[i].type == weapon && !g_replaced.count(weapon))
                g_replaced[weapon] = {was.type[i], was.ammo[i]};
        g_missionWeapons.insert(weapon);
    } else if (g_missionWeapons.erase(weapon)) {
        Cmd(0x0555).I(h).I(weapon).Run();  // REMOVE_WEAPON_FROM_CHAR
        if (auto it = g_replaced.find(weapon); it != g_replaced.end()) {
            give(it->second.first, it->second.second);
            g_replaced.erase(it);
        }
    }
}

bool InArea(const Area& a, const CVector& p, float slack) {
    return p.x > std::min(a.x1, a.x2) - slack && p.x < std::max(a.x1, a.x2) + slack &&
           p.y > std::min(a.y1, a.y2) - slack && p.y < std::max(a.y1, a.y2) + slack &&
           (a.z < -50.0f || std::fabs(p.z - a.z) < 3.0f);  // -100: a 2D locate
}

// The marker the host stands in: the yellow ones of the story are the radar's contact points (the radar draws
// them, the trigger only checks where the player is); a locate command's marker drawn in the last second counts too.
bool MarkerAt(const CVector& p, Area& out) {
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!(Ref<uint8_t>(t + 0x25) & 2) || ((Ref<uint8_t>(t + 0x26) >> 2) & 0xF) != 5) continue;  // BLIP_CONTACT_POINT
        // a save house's or a property's marker is a contact point too: the host there held no mission, yet the partner
        // was told it waited
        int8_t sprite = Ref<int8_t>(t + 0x24);
        if (sprite == 31 || sprite == 32 || sprite == 35) continue;  // RADAR_SPRITE_PROPERTYG, PROPERTYR, SAVEGAME
        CVector c = Ref<CVector>(t + 0x08);
        if ((c - p).Length2D() < 2.0f && std::fabs(c.z - p.z) < 3.0f) {
            out = {0x40000002u, c.x - 1.5f, c.y - 1.5f, c.x + 1.5f, c.y + 1.5f, c.z, NowMs()};
            return true;
        }
    }
    for (auto& [id, a] : g_areas)
        if (a.kind == AREA_LOCATE && NowMs() - a.seenMs < 1000 && InArea(a, p, 0.0f)) {
            out = a;
            return true;
        }
    return false;
}

// host, in a trigger: the marker the host stands in holds every partner as well. Without a marker (a phone call,
// a story event) the mission starts as usual.
bool PartnersAtMarker(CPed* host) {
    Area marker;
    if (!MarkerAt(host->GetPosition(), marker)) return true;
    for (auto& [id, peer] : Net().Peers()) {
        RemotePlayer* rp = Players::Instance().Get(id);
        // a partner whose game sends nothing for 5 s (in the pause menu, loading, hung) cannot come: the host was held
        // at the marker for good. Such a player is away in SA-MP and MTA too.
        if (!rp || !rp->hasState || NowMs() - rp->stateRecvMs > 5000) continue;
        if (!rp->ped || !InArea(marker, rp->ped->GetPosition(), 1.0f)) {  // two can not stand on one spot
            g_waitMs = NowMs();
            g_waitArea = marker;
            return false;
        }
    }
    return true;
}

using CanStartFn = bool(__fastcall*)(CPlayerPed*, void*);
CanStartFn g_origCanStart = nullptr;
bool __fastcall HookCanPlayerStartMission(CPlayerPed* player, void* edx) {
    if (!player) return false;
    bool ok = g_origCanStart(player, edx);
    if (!ok || !Net().Online() || !g_currentScript) return ok;
    // the threads that start only side activities, no story missions: as in the single-player game for either player
    // (the partner plays it in his own game, while the host is not on a mission), and nobody waits at their markers
    static const char* const kSideThreads[] = {
        "gym",                                // gyms (GYMLS, GYMSF, GYMLV)
        "trace", "hotr", "kicks", "bloodr",   // race tournament and driving school, hotring, Kick Start, Blood Bowl
        "tri", "psch", "bikes", "bschoo",     // TRIA, flight school, bike school, boat school
        "shoot", "trucks", "quarrys",         // Ammu-Nation range, trucking, quarry
        "trainsl",                            // the airports' and stations' travel
        "psave1",                             // the save points: each saves his own game, nobody waits for the other
    };
    const char* thread = Field<char[8]>(g_currentScript, 0x08);
    for (const char* side : kSideThreads)
        if (!strcmp(thread, side)) return Net().IsHost() || !HostMissionRecent(0);
    auto* ip = Field<uint8_t*>(g_currentScript, 0x14);
    // main.scm threads ask it in mission triggers (not only inside markers, so no hint here)
    if (ip < (uint8_t*)addr::ScriptSpace || ip >= (uint8_t*)addr::ScriptSpace + Ref<int32_t>(kMainScriptSize)) return ok;
    return Net().IsHost() && PartnersAtMarker(player);
}

// A partner who joins the host after a new game's prologue does so in a new game of its own (flow::IsFollower), which
// must not play the prologue again: main.scm launches it ("INTRO") by itself, with no trigger. The mission's code is in
// the mission block by the time its thread starts: the thread ends at once instead.
using StartScriptFn = void*(__cdecl*)(uint8_t*);
StartScriptFn g_origStartScript = nullptr;
void* __cdecl HookStartNewScript(uint8_t* ip) {
    static const uint8_t kIntro[] = {0xA4, 0x03, 0x09, 'I', 'N', 'T', 'R', 'O', 0};  // SCRIPT_NAME 'INTRO'
    if (ip && flow::IsFollower() && !memcmp(ip, kIntro, sizeof kIntro)) {
        ip[0] = 0x4E, ip[1] = 0x00;  // TERMINATE_THIS_SCRIPT (it also ends "a mission runs")
        Fade(1.0f, 1);  // main.scm blacks the screen out before the prologue, which would fade it in
        LOGI("The prologue mission is the host's: this game's copy ends at once");
    }
    return g_origStartScript(ip);
}

void HandleMissionWait(uint8_t from, Reader& r) {
    bool on = r.U8() != 0;
    Area a{};
    if (on) a = {0x40000001u, r.F32(), r.F32(), r.F32(), r.F32(), r.F32(), 0};
    if (!r.Ok() || from == Net().MyId()) return;
    if (Net().IsHost()) {  // a partner at a mission marker
        bool was = g_partnersWaiting.count(from) != 0;
        if (on) {
            g_partnersWaiting[from] = {a, was ? g_partnersWaiting[from].sinceMs : NowMs()};
            if (!was) {
                LOGI("Player %u waits at a mission marker (%.1f,%.1f)", from, (a.x1 + a.x2) * 0.5f, (a.y1 + a.y2) * 0.5f);
                ui::TheChat().AddSystem(Format(TR("%s хочет начать миссию: встаньте в маркер, который он ждёт.",
                                                  "%s wants to start a mission: stand in the marker they are waiting at."),
                                               Net().PeerName(from).c_str()));
            }
        } else if (was) {
            g_partnersWaiting.erase(from);
            LOGI("Player %u does not wait at the marker any more", from);
        }
        return;
    }
    if (from != Net().HostId()) return;
    if (on != g_hostWaits) LOGI(on ? "The host waits at a mission marker" : "The host does not wait at the marker any more");
    g_hostWaits = on;
    g_waitArea = a;  // the partner's game may not have this marker yet (story progress comes on reconnect)
}

// ---------------------------------------------------------------- texts
char g_textPool[256][400];  // the game keeps text pointers (message queue, briefs): stable storage
unsigned g_textNext = 0;
const char* PoolText(const std::string& s) {
    char* p = g_textPool[g_textNext++ & 255];
    size_t n = std::min(s.size(), sizeof(g_textPool[0]) - 1);
    memcpy(p, s.data(), n);
    p[n] = 0;
    return p;
}

// GXT texts travel as (mission table, key hash) and each player shows his own game's text: the host and the
// partner may run different translations, whose raw strings come out as gibberish in the other font.
constexpr uintptr_t kTheText = 0xC1B340;  // CText: tkeyMain +0, tkeyMission +0x10, missionTableName +0x24
struct GxtKey {
    const char* text;
    uint32_t hash;
};
struct GxtKeys {
    GxtKey* data;
    uint32_t size;
};

std::string MissionTable() { return std::string((const char*)kTheText + 0x24, strnlen((const char*)kTheText + 0x24, 8)); }

uint32_t GxtHashOf(const char* text, bool& mission) {
    for (int m = 0; m < 2; ++m) {
        const GxtKeys& keys = Ref<GxtKeys>(kTheText + m * 0x10);
        for (uint32_t i = 0; i < keys.size; ++i)
            if (keys.data[i].text == text) {
                mission = m == 1;
                return keys.data[i].hash;
            }
    }
    return 0;  // not a GXT entry (built by a script or a mod): sent as is
}

// The partner runs no missions, so its mission table slot holds the host's table (timer and counter labels
// are looked up by key at draw time too).
// shortcut: a partner-side script with its own table (gym dumbbells, valet) loses its texts while the host's
// mission prints; give it a CText of its own if that matters.
void UseMissionTable(const std::string& table) {
    static std::string missing;  // not in this game's GXT: don't reload (and clear the messages) for every text
    if (table.empty() || table == MissionTable() || table == missing) return;
    char name[9] = {};
    memcpy(name, table.data(), std::min<size_t>(table.size(), 8));
    CallMethod<0x69FBF0>((void*)kTheText, (const char*)name);  // CText::LoadMissionText
    missing = MissionTable() == table ? std::string() : table;
}

const char* GxtLocal(uint32_t hash, const std::string& table) {
    if (!hash) return nullptr;
    UseMissionTable(table);
    const GxtKeys& keys = Ref<GxtKeys>(kTheText + (table.empty() ? 0 : 0x10));
    uint32_t lo = 0, hi = keys.size;  // sorted by hash, as CKeyArray::BinarySearch expects
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, h = keys.data[mid].hash;
        if (h == hash) return keys.data[mid].text;
        if (h < hash) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

using AddMsgFn = void(__cdecl*)(const char*, uint32_t, uint16_t, bool);
using AddMsgNumFn = void(__cdecl*)(const char*, uint32_t, uint16_t, int, int, int, int, int, int, bool);
using AddBigFn = void(__cdecl*)(const char*, uint32_t, uint16_t);
using AddBigNumFn = void(__cdecl*)(const char*, uint32_t, uint16_t, int, int, int, int, int, int);
using HelpFn = void(__cdecl*)(const char*, bool, bool, bool);
using ClearFn = void(__cdecl*)(bool);
using VoidFn = void(__cdecl*)();
AddMsgFn g_origMsg, g_origMsgQ;
AddMsgNumFn g_origMsgNum, g_origMsgQNum;
AddBigFn g_origBig, g_origBigQ;
AddBigNumFn g_origBigNum, g_origBigQNum;
HelpFn g_origHelp;
ClearFn g_origClear;
VoidFn g_origClearSmall;

void SendText(uint8_t kind, const char* text, uint32_t time, uint16_t flag, const int* nums, uint8_t bools) {
    if (!Replicating()) return;
    // a running cutscene prints its subtitles itself, outside any script (CCutsceneMgr's text list, AddMessageJumpQ at
    // 0x5B187E), and so does the partner's copy of it: sent too, each line came twice
    if (Ref<uint8_t>(0xB5F851) && !CurrentScript()) return;  // CCutsceneMgr::ms_running
    Writer w(proto::G_TEXT);
    w.U8(kind);
    w.Str(text ? text : "", 399);
    w.U32(time);
    w.U16(flag);
    w.U8(nums ? 1 : 0);
    if (nums)
        for (int i = 0; i < 6; ++i) w.I32(nums[i]);
    w.U8(bools);
    bool mission = false;
    w.U32(text ? GxtHashOf(text, mission) : 0);
    w.Str(mission ? MissionTable() : std::string(), 8);
    Net().Send(proto::CH_RELIABLE, w);
}

void __cdecl HookMsg(const char* t, uint32_t time, uint16_t flag, bool brief) {
    SendText(TXT_MSG, t, time, flag, nullptr, brief);
    g_origMsg(t, time, flag, brief);
}
void __cdecl HookMsgQ(const char* t, uint32_t time, uint16_t flag, bool brief) {
    SendText(TXT_MSG_Q, t, time, flag, nullptr, brief);
    g_origMsgQ(t, time, flag, brief);
}
void __cdecl HookMsgNum(const char* t, uint32_t time, uint16_t flag, int a, int b, int c, int d, int e, int f, bool brief) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_MSG, t, time, flag, n, brief);
    g_origMsgNum(t, time, flag, a, b, c, d, e, f, brief);
}
void __cdecl HookMsgQNum(const char* t, uint32_t time, uint16_t flag, int a, int b, int c, int d, int e, int f, bool brief) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_MSG_Q, t, time, flag, n, brief);
    g_origMsgQNum(t, time, flag, a, b, c, d, e, f, brief);
}
// The host's own WASTED and BUSTED (CPlayerInfo::KillPlayer, CGameLogic) are not the mission's: the partner's HUD keeps
// that style's text until its own player respawns (CHud::ResetWastedText), so it stayed on the partner's screen.
bool HostDeathText(const char* t) {
    return t && (t == CallMethod<0x6A0050, const char*>((void*)kTheText, "DEAD") ||  // CText::Get
                 t == CallMethod<0x6A0050, const char*>((void*)kTheText, "BUSTED"));
}
void __cdecl HookBig(const char* t, uint32_t time, uint16_t style) {
    if (!HostDeathText(t)) SendText(TXT_BIG, t, time, style, nullptr, 0);
    g_origBig(t, time, style);
}
void __cdecl HookBigQ(const char* t, uint32_t time, uint16_t style) {
    SendText(TXT_BIG_Q, t, time, style, nullptr, 0);
    g_origBigQ(t, time, style);
}
void __cdecl HookBigNum(const char* t, uint32_t time, uint16_t style, int a, int b, int c, int d, int e, int f) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_BIG, t, time, style, n, 0);
    g_origBigNum(t, time, style, a, b, c, d, e, f);
}
void __cdecl HookBigQNum(const char* t, uint32_t time, uint16_t style, int a, int b, int c, int d, int e, int f) {
    int n[6] = {a, b, c, d, e, f};
    SendText(TXT_BIG_Q, t, time, style, n, 0);
    g_origBigQNum(t, time, style, a, b, c, d, e, f);
}
void __cdecl HookHelp(const char* t, bool quick, bool permanent, bool addToBrief) {
    SendText(TXT_HELP, t, 0, 0, nullptr, (quick ? 1 : 0) | (permanent ? 2 : 0) | (addToBrief ? 4 : 0));
    g_origHelp(t, quick, permanent, addToBrief);
}
void __cdecl HookClear(bool flag) {
    SendText(TXT_CLEAR, nullptr, 0, flag, nullptr, 0);
    g_origClear(flag);
}
void __cdecl HookClearSmall() {
    SendText(TXT_CLEAR_SMALL, nullptr, 0, 0, nullptr, 0);
    g_origClearSmall();
}

void HandleText(uint8_t from, Reader& r) {
    uint8_t kind = r.U8();
    std::string text = r.Str(399);
    uint32_t time = r.U32();
    uint16_t flag = r.U16();
    int n[6] = {};
    bool hasNums = r.U8() != 0;
    if (hasNums)
        for (int& v : n) v = r.I32();
    uint8_t bools = r.U8();
    uint32_t hash = r.U32();
    std::string table = r.Str(8);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (const char* own = GxtLocal(hash, table)) text = own;
    const char* t = text.empty() ? nullptr : PoolText(text);
    switch (kind) {
    case TXT_MSG:
        if (!t) return;
        if (hasNums) g_origMsgNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5], bools & 1);
        else g_origMsg(t, time, flag, bools & 1);
        break;
    case TXT_MSG_Q:
        if (!t) return;
        if (hasNums) g_origMsgQNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5], bools & 1);
        else g_origMsgQ(t, time, flag, bools & 1);
        break;
    case TXT_BIG:
        if (!t) return;
        if (hasNums) g_origBigNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5]);
        else g_origBig(t, time, flag);
        break;
    case TXT_BIG_Q:
        if (!t) return;
        if (hasNums) g_origBigQNum(t, time, flag, n[0], n[1], n[2], n[3], n[4], n[5]);
        else g_origBigQ(t, time, flag);
        break;
    case TXT_HELP: g_origHelp(t, bools & 1, (bools & 2) != 0, (bools & 4) != 0); break;
    case TXT_CLEAR: g_origClear(flag != 0); break;
    case TXT_CLEAR_SMALL: g_origClearSmall(); break;
    }
}

// ---------------------------------------------------------------- markers
using HighlightFn = void(__cdecl*)(uint32_t, float, float, float, float, float);
HighlightFn g_origHighlight = nullptr;
void __cdecl HookHighlight(uint32_t id, float x1, float y1, float x2, float y2, float z) {
    if (Net().Online() && Net().IsHost()) {
        if (!g_areas.count(id)) LOGD("Script marker %08X at %.1f,%.1f,%.1f", id, (x1 + x2) * 0.5f, (y1 + y2) * 0.5f, z);
        g_areas[id] = {id, x1, y1, x2, y2, z, NowMs()};
    }
    g_origHighlight(id, x1, y1, x2, y2, z);
}

// DRAW_SPHERE: a red circle a script draws in the frames it wants it (OG Loc's at Freddy's door), C3dMarkers::
// PlaceMarkerSet(id, type 1, position, radius, 255, 0, 0, 228, pulse 2048, 0.1, no rotation) from its command
using PlaceMarkerSetFn = void(__cdecl*)(uint32_t, uint16_t, CVector*, float, uint8_t, uint8_t, uint8_t, uint8_t, uint16_t,
                                        float, int16_t);
// the game's own until redirected: a site another mod changed is left alone, and this is still called
PlaceMarkerSetFn g_origDrawSphere = (PlaceMarkerSetFn)0x725BA0;
void __cdecl HookDrawSphere(uint32_t id, uint16_t type, CVector* pos, float radius, uint8_t r, uint8_t g, uint8_t b,
                            uint8_t a, uint16_t pulse, float pulseFraction, int16_t rotate) {
    if (Net().Online() && Net().IsHost() && pos) {
        if (!g_areas.count(id)) LOGD("Script sphere %08X at %.1f,%.1f,%.1f", id, pos->x, pos->y, pos->z);
        g_areas[id] = {id, pos->x, pos->y, radius, 0.0f, pos->z, NowMs(), AREA_SPHERE};
    }
    g_origDrawSphere(id, type, pos, radius, r, g, b, a, pulse, pulseFraction, rotate);
}

void DrawSphere(uint32_t id, const Area& a) {
    CVector at{a.x1, a.y1, a.z};
    g_origDrawSphere(id, 1, &at, a.x2, 255, 0, 0, 228, 2048, 0.1f, 0);
}

// The radar leaves the mission start points out (their letters and markers) while CTheScripts::IsPlayerOnAMission:
// the partner, on no mission of its own, kept seeing them during the host's mission.
using OnMissionFn = bool(__cdecl*)();
OnMissionFn g_origRadarOnMission = nullptr;
bool __cdecl RadarOnMission() {
    return g_origRadarOnMission() || (Net().Online() && !Net().IsHost() && HostMissionRecent(0));
}

void HandleMarkers(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    std::vector<Area> areas;
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        Area a{};
        a.id = r.U32();
        a.kind = r.U8();
        a.x1 = r.F32();
        a.y1 = r.F32();
        a.x2 = r.F32();
        a.y2 = r.F32();
        a.z = r.F32();
        areas.push_back(a);
    }
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    if (areas.size() != g_partnerAreas.size())
        LOGD("Host markers: %zu (spheres %d)", areas.size(),
             (int)std::count_if(areas.begin(), areas.end(), [](const Area& a) { return a.kind == AREA_SPHERE; }));
    g_partnerAreas = std::move(areas);
    g_partnerAreasMs = NowMs();
    static uint64_t loggedMs = 0;
    if (!g_partnerAreas.empty() && g_partnerAreasMs - loggedMs >= 5000) {
        loggedMs = g_partnerAreasMs;
        LOGD("Host markers: %zu, still drawn", g_partnerAreas.size());
    }
}

// Race checkpoints (CREATE_CHECKPOINT: CTheScripts::ScriptCheckpointArray, 20 x {used, id, CCheckpoint*}) and script
// spheres (ADD_SPHERE: CTheScripts::ScriptSphereArray, 16 x 0x18) of the host's scripts: the partner places the same.
constexpr uintptr_t kScriptCheckpoints = 0xA44070, kScriptSpheres = 0xA91268;
struct Marked {  // one record of G_CHECKPOINTS
    uint8_t slot = 0;   // 0..19 checkpoints, 32..47 spheres
    uint16_t type = 0;  // checkpoint type
    CVector pos, dir;
    float size = 0;
    bool operator==(const Marked& o) const {
        return slot == o.slot && type == o.type && (pos - o.pos).Length() < 0.05f && (dir - o.dir).Length() < 0.05f &&
               std::fabs(size - o.size) < 0.01f;
    }
};
std::vector<Marked> g_sentMarked;            // host
std::map<uint8_t, std::pair<Marked, int>> g_placed;  // partner: slot -> what and its script handle

std::vector<Marked> HostMarked() {
    std::vector<Marked> out;
    for (int i = 0; i < 20; ++i) {
        uintptr_t e = kScriptCheckpoints + (uintptr_t)i * 8;
        auto cp = Ref<uintptr_t>(e + 4);  // CCheckpoint*
        if (!Ref<uint8_t>(e) || !cp) continue;
        Marked m;
        m.slot = (uint8_t)i;
        m.type = Ref<uint16_t>(cp);  // the type it was made with, unless the player is in it right now (no matter here)
        if (m.type > 8) continue;
        m.pos = Ref<CVector>(cp + 0x10);
        m.dir = Ref<CVector>(cp + 0x1C);
        m.size = Ref<float>(cp + 0x2C);
        out.push_back(m);
    }
    for (int i = 0; i < 16; ++i) {
        uintptr_t s = kScriptSpheres + (uintptr_t)i * 0x18;
        if (!Ref<uint8_t>(s)) continue;
        Marked m;
        m.slot = (uint8_t)(32 + i);
        m.pos = Ref<CVector>(s + 8);
        m.size = Ref<float>(s + 0x14);
        out.push_back(m);
    }
    return out;
}

void HostSendMarked(bool force) {
    std::vector<Marked> now = Replicating() ? HostMarked() : std::vector<Marked>{};
    if (!force && now == g_sentMarked) return;
    g_sentMarked = now;
    Writer w(proto::G_CHECKPOINTS);
    w.U8((uint8_t)now.size());
    for (const Marked& m : now) {
        w.U8(m.slot);
        w.U16(m.type);
        w.Vec({m.pos.x, m.pos.y, m.pos.z});
        w.Vec({m.dir.x, m.dir.y, m.dir.z});
        w.F32(m.size);
    }
    Net().Send(proto::CH_RELIABLE, w);
}

void Unplace(int slot, int handle) {
    if (slot >= 32) Cmd(0x03BD).I(handle).Run();  // REMOVE_SPHERE
    else Cmd(0x06D6).I(handle).Run();             // DELETE_CHECKPOINT
}

void ClearPartnerMarked() {
    for (auto& [slot, placed] : g_placed) Unplace(slot, placed.second);
    g_placed.clear();
}

void HandleMarked(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    std::vector<Marked> now;
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        Marked m;
        m.slot = r.U8();
        m.type = r.U16();
        Vec3 p = r.Vec(), d = r.Vec();
        m.pos = {p.x, p.y, p.z};
        m.dir = {d.x, d.y, d.z};
        m.size = r.F32();
        if (m.type <= 8 && (m.slot < 20 || (m.slot >= 32 && m.slot < 48))) now.push_back(m);
    }
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    for (auto it = g_placed.begin(); it != g_placed.end();) {  // gone or changed
        auto same = std::find_if(now.begin(), now.end(), [&](const Marked& m) { return m == it->second.first; });
        if (same != now.end()) {
            ++it;
            continue;
        }
        Unplace(it->first, it->second.second);
        it = g_placed.erase(it);
    }
    for (const Marked& m : now) {
        if (g_placed.count(m.slot)) continue;
        // the engine writes past its arrays when they are full (CTheScripts::AddScriptCheckpoint): a free slot first
        bool sphere = m.slot >= 32, room = false;
        for (int i = 0; i < (sphere ? 16 : 20) && !room; ++i)
            room = !Ref<uint8_t>((sphere ? kScriptSpheres : kScriptCheckpoints) + (uintptr_t)i * (sphere ? 0x18 : 8));
        if (!room) continue;
        int h = -1;
        // the checkpoint keeps the direction to the point it was made to face (CCheckpoints::PlaceMarker normalises
        // point - pos): that point is a step along it
        CVector to = m.pos + m.dir;
        if (m.slot >= 32) Cmd(0x03BC).F(m.pos.x).F(m.pos.y).F(m.pos.z).F(m.size).Out(&h).Run();  // ADD_SPHERE
        else Cmd(0x06D5).I(m.type).F(m.pos.x).F(m.pos.y).F(m.pos.z).F(to.x).F(to.y).F(to.z).F(m.size).Out(&h).Run();  // CREATE_CHECKPOINT
        if (h != -1) g_placed[m.slot] = {m, h};
    }
}

// ---------------------------------------------------------------- cutscenes
using LoadCutsceneFn = void(__cdecl*)(const char*);
LoadCutsceneFn g_origLoadCutscene = nullptr;
VoidFn g_origStartCutscene = nullptr;

void SendCutscene(uint8_t op, const char* name = "", int32_t area = 0) {
    Writer w(proto::G_CUTSCENE);
    w.U8(op);
    w.Str(name, 32);
    w.I32(area);
    Net().Send(proto::CH_RELIABLE, w);
}
// the host's choice of the partner's spot for a shot, and when (the camera's clock) the shot begins
void SendSpot(const scene::Spot& s, float at) {
    Writer w(proto::G_CUTSCENE);
    w.U8(CS_SPOT);
    w.Str("", 32);
    w.I32(0);
    w.F32(at);
    w.Vec({s.ground.x, s.ground.y, s.ground.z});
    w.F32(s.heading);
    w.U8(s.pose);
    w.U8(s.act);
    Net().Send(proto::CH_RELIABLE, w);
}

// The cutscene CJ (model 1) takes its textures by name from the "player" TXD it shares with model 0, and that TXD
// holds whoever was dressed last: the remote player, whenever its look came in. Before a cutscene loads, model 0 is
// built for the story's CJ (the host) from that ped's clothes, with no ped rebuilt.
void DressCutsceneCj(CPlayerPed* cj) {
    auto* clothes = cj->ClothesDesc();
    auto* built = (CPedClothesDesc*)0xBC1C78;  // PlayerClothes: what model 0 was built with last
    MuscleFrom muscle(cj != FindPlayerPed(0) ? clothes : nullptr);  // the host's body on the partner's screen
    Call<0x5A81E0>(0u, clothes, built, false);  // CClothes::ConstructPedModel
    memcpy(built, clothes, sizeof(CPedClothesDesc));
}

// LoadCutsceneData builds the cutscene CJ from its own player (CClothes::RebuildCutscenePlayer, called at 0x4D5EA3):
// in a host's cutscene on the partner's screen it is built from the host's ped.
using CutscenePlayerFn = void(__cdecl*)(CPlayerPed*, int);
CutscenePlayerFn g_origCutscenePlayer = nullptr;
CPlayerPed* g_cutsceneCj = nullptr;
void __cdecl HookRebuildCutscenePlayer(CPlayerPed* ped, int model) {
    if (!g_cutsceneCj) return g_origCutscenePlayer(ped, model);
    // the host's body as its look came: the original writes this game's fat and muscle stats into the description first
    MuscleFrom muscle(g_cutsceneCj->ClothesDesc());
    Call<0x5A81E0>((uint32_t)model, g_cutsceneCj->ClothesDesc(), (CPedClothesDesc*)nullptr, true);  // CClothes::ConstructPedModel
}

void __cdecl HookLoadCutscene(const char* name) {
    if (Net().Online() && Net().IsHost() && name) {
        // a script that clears one cutscene and loads the next in the same frame (the prologue does): the end goes
        // out first, otherwise the partner takes it for the end of the one it is loading
        if (g_hostCutscene) {
            SendCutscene(CS_END);
            g_hostCutscene = false;
            LOGI("Cutscene ended (replicated before the next one)");
        }
        strncpy(g_cutsceneName, name, sizeof(g_cutsceneName) - 1);
        SendCutscene(CS_LOAD, name);
        LOGI("Cutscene %s loading (replicated)", name);
    }
    if (Net().Online() && Net().IsHost()) {
        scene::Prepare();  // before the cutscene's own streaming requests
        DressCutsceneCj(FindPlayerPed(0));
    }
    g_origLoadCutscene(name);
}
int32_t& CurrentArea() { return Ref<int32_t>(0xB72914); }  // CGame::currArea: the interior being drawn

void __cdecl HookStartCutscene() {
    if (Net().Online() && Net().IsHost()) SendCutscene(CS_START, "", CurrentArea());
    g_origStartCutscene();
}

// ---- the partner in cutscenes (ScenePartner.cpp picks the spot and the animation in each shot);
// on the partner's screen the cutscene CJ wears the host's clothes and the host's own ped is hidden
constexpr uintptr_t kCutsceneObjects = 0xBC3F18, kNumCutsceneObjects = 0xBC3FE4;  // CCutsceneMgr::ms_pCutsceneObjects
bool g_inScene = false, g_sceneHeld = false;
std::string g_lastPassed;      // host: CStats::LastMissionPassedName as last seen
uint64_t g_progressAt = 0;     // host: when the story's progress goes to the partners (0: nothing to send)
int g_csCar = -1, g_csSeat = -1;  // the vehicle (handle) and seat (-1: driver) a cutscene's loading took us out of
CVector g_scenePos;
float g_sceneHeading = 0;
int8_t g_sceneArea = 0;

CEntity* CutsceneCj() {
    int n = std::clamp(Ref<int32_t>(kNumCutsceneObjects), 0, 50);
    for (int i = 0; i < n; ++i)
        if (CEntity* o = Ref<CEntity*>(kCutsceneObjects + (uintptr_t)i * 4); o && o->ModelIndex() == 1) return o;  // "csplay"
    return nullptr;
}

// Our player waits at the cutscene's place from its start (hidden and frozen): the collision there loads around him
// and stays, so where the scene puts him he stands on the floor instead of falling (a fall cancels the animation).
void EnterCutsceneScene(const CVector& at, int32_t area) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!g_inScene) {
        g_inScene = true;
        g_scenePos = me->GetPosition();
        g_sceneHeading = me->CurrentRotation();
        g_sceneArea = me->AreaCode();
    }
    int h = GetPedRef(me);
    me->AreaCode() = (int8_t)area;
    Cmd(0x04D7).I(h).I(1).Run();                     // FREEZE_CHAR_POSITION
    Cmd(0x00A1).I(h).F(at.x).F(at.y).F(at.z).Run();  // SET_CHAR_COORDINATES
}

void StandNextToCutsceneCj() {
    scene::Spot spot;
    if (!scene::CurrentSpot(spot)) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (CEntity* cj = CutsceneCj()) me->AreaCode() = cj->AreaCode();
    scene::Hold(me, spot);
    if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped) {
        host->ped->Flags() &= ~0x80u;  // the cutscene CJ is the host
        Cmd(0x04D7).I(host->pedHandle).I(1).Run();  // FREEZE_CHAR_POSITION: no collision around it now
    }
}

void LeaveCutsceneScene() {
    if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped) {
        host->ped->Flags() |= 0x80;
        Cmd(0x04D7).I(host->pedHandle).I(0).Run();  // FREEZE_CHAR_POSITION off
    }
    scene::ReleaseAll();
    if (!g_inScene) return;
    g_inScene = false;
    CPlayerPed* me = FindPlayerPed(0);
    int h = GetPedRef(me);
    Cmd(0x04BB).I(g_sceneArea).Run();      // SET_AREA_VISIBLE
    Cmd(0x0860).I(h).I(g_sceneArea).Run();  // SET_CHAR_AREA_VISIBLE
    Cmd(0x04D7).I(h).I(0).Run();  // FREEZE_CHAR_POSITION off (a scene that ended before its first spot held nobody)
    Cmd(0x00A1).I(h).F(g_scenePos.x).F(g_scenePos.y).F(g_scenePos.z - 1.0f).Run();
    me->CurrentRotation() = me->AimingRotation() = g_sceneHeading;
}

// back into the vehicle the cutscene's loading took us out of, as a mission script does with CJ
void ReturnToCutsceneCar() {
    CVehicle* v = g_csCar >= 0 ? GetVehicle(g_csCar) : nullptr;
    int h = GetPedRef(FindPlayerPed(0));
    // only where it was left: a car that moved or sank meanwhile (its ground was not loaded) stays where it is
    bool there = v && (v->GetPosition() - g_scenePos).Length() < 6.0f;
    if (there && v->Health() > 0.0f && !(g_csSeat < 0 ? v->Driver() : v->Passengers()[g_csSeat])) {
        if (g_csSeat < 0) Cmd(0x036A).I(h).I(g_csCar).Run();  // WARP_CHAR_INTO_CAR
        else Cmd(0x0430).I(h).I(g_csCar).I(g_csSeat).Run();    // WARP_CHAR_INTO_CAR_AS_PASSENGER
        LOGI("Back in the vehicle the cutscene took us out of");
    }
    g_csCar = g_csSeat = -1;
}

void PartnerEndCutscene() {
    if (CutsceneRunning()) Call<0x5B1700>();  // CCutsceneMgr::SkipCutscene
    g_csState = 3;
}

// what the mission script does after a cutscene: CLEAR_CUTSCENE resets ms_running
void PartnerClearCutscene() {
    Cmd(0x02EA).Run();  // CLEAR_CUTSCENE
    LeaveCutsceneScene();
    ReturnToCutsceneCar();
    Cmd(0x02EB).Run();            // RESTORE_CAMERA_JUMPCUT
    // the cutscene fades out at its end; in a mission the host's fades follow (G_FADE), otherwise the screen
    // would stay black
    if (!g_partnerMission && FadeAlpha() > 1.0f) Fade(0.5f, 1);
    Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL on
    if (g_weWide || TheCameraWide()) SetWideScreen(false);
    g_weWide = false;
    g_csState = 0;
    LOGI("Host cutscene finished here");
}

// ---- skipping a cutscene takes everybody: the skip button only asks, the host skips once all players said yes
// (the partner's copy of the cutscene ends with the host's)
using SkipButtonFn = bool(__cdecl*)();
SkipButtonFn g_origSkipButton = nullptr;
bool g_skipHeld = false;         // the button was down in the last frame: one press, one vote
bool g_skipOpen = false;         // a vote is on
uint64_t g_skipSince = 0;        // host: when it began
std::set<uint8_t> g_skipYes;     // who said yes (as the host last told)
uint8_t g_skipNo = 0xFF;         // who wants to watch on (shown for a moment)
uint64_t g_skipNoMs = 0;

void SendSkipState() {
    Writer w(proto::G_CUTSCENE);
    w.U8(CS_SKIP_STATE);
    w.Str("", 32);
    w.I32(0);
    w.U8(g_skipOpen);
    w.U8(g_skipNo);
    w.U8((uint8_t)g_skipYes.size());
    for (uint8_t id : g_skipYes) w.U8(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void CloseSkipVote(uint8_t no) {
    if (!g_skipOpen && no == 0xFF) return;
    g_skipOpen = false;
    g_skipYes.clear();
    g_skipNo = no;
    g_skipNoMs = no == 0xFF ? 0 : NowMs();
    if (Net().IsHost()) SendSkipState();
}

// a vote from a player (the host's own too); the host runs the vote
void SkipVote(uint8_t who, bool yes) {
    if (!Net().IsHost()) {
        Writer w(proto::G_CUTSCENE);
        w.U8(CS_SKIP_VOTE);
        w.Str("", 32);
        w.I32(0);
        w.U8(yes);
        Net().Send(proto::CH_RELIABLE, w);
        if (yes) g_skipYes.insert(who);  // shown at once, the host's answer follows
        return;
    }
    if (!CutsceneRunning()) return;
    if (!yes) return CloseSkipVote(who);
    if (!g_skipOpen) g_skipSince = NowMs(), g_skipNo = 0xFF, g_skipNoMs = 0;
    g_skipOpen = true;
    g_skipYes.insert(who);
    LOGI("Cutscene skip: player %u agreed (%zu of %zu)", who, g_skipYes.size(), Net().Peers().size() + 1);
    SendSkipState();
}

// CCutsceneMgr::Update_overlay asks every frame of a cutscene whether to skip it (0x5B1947)
bool __cdecl HookSkipButton() {
    bool down = g_origSkipButton();
    if (!Net().Online()) return down;
    down = down && game::IsForeground();  // the game also says yes for a window in the background
    bool press = down && !g_skipHeld;
    g_skipHeld = down;
    if (press && !g_skipYes.count(Net().MyId())) SkipVote(Net().MyId(), true);
    return false;
}

// a player whose game is in the host's world (a partner still waiting in the menu, say during a new game's prologue,
// sees no cutscene and has no say)
bool InHostWorld(uint8_t id) {
    if (id == Net().MyId()) return true;
    const RemotePlayer* rp = Players::Instance().Get(id);
    return rp && rp->ped;
}

void HostSkipUpdate() {
    if (!g_skipOpen) return;
    if (!CutsceneRunning() || NowMs() - g_skipSince > 15000) return CloseSkipVote(0xFF);  // over, or no answer
    if (!g_skipYes.count(Net().MyId()) || game::cam::Mode() != 17) return;  // MODE_FLYBY: the cutscene plays
    for (auto& [id, peer] : Net().Peers())
        if (InHostWorld(id) && !g_skipYes.count(id)) return;
    LOGI("Cutscene skipped: everybody agreed");
    CloseSkipVote(0xFF);
    Call<0x5B1700>();  // CCutsceneMgr::SkipCutscene
}

void HandleSkip(uint8_t from, uint8_t op, Reader& r) {
    if (op == CS_SKIP_VOTE) {
        bool yes = r.U8() != 0;
        if (r.Ok() && Net().IsHost()) SkipVote(from, yes);
        return;
    }
    bool open = r.U8() != 0;
    uint8_t no = r.U8(), n = r.U8();
    std::set<uint8_t> yes;
    for (uint8_t i = 0; i < n; ++i) yes.insert(r.U8());
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    g_skipOpen = open;
    g_skipYes = std::move(yes);
    g_skipNo = no;
    g_skipNoMs = !open && no != 0xFF ? NowMs() : 0;  // a vote closes with the name of who wants to watch on
}

void HandleCutscene(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    std::string name = r.Str(32);
    int32_t area = r.I32();
    if (op == CS_SKIP_VOTE || op == CS_SKIP_STATE) return HandleSkip(from, op, r);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (op == CS_LOAD) {
        // the previous one is cleared first, even when its end came in the same frame: otherwise our ped stays
        // where the previous scene put it (an interior's exit marker fires and takes the camera)
        if (g_csState == 2) PartnerEndCutscene();
        if (g_csState != 0) PartnerClearCutscene();
        // A partner in an aircraft or a boat, or in the air on foot (a parachute, a jetpack), stays out of the scene: its
        // loading sets the player out of the vehicle (CCutsceneMgr::LoadCutsceneData_preload), and after it the player
        // came back where it had been, in mid-air or in the water, with the aircraft gone on.
        CPlayerPed* me = FindPlayerPed(0);
        int type = me->InVehicle() && me->Vehicle() ? me->Vehicle()->VehicleSubClass() : -1;
        bool flying = type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_BOAT ||
                      type == VEHICLE_TYPE_FHELI || type == VEHICLE_TYPE_FPLANE || (type < 0 && !(me->PedFlags() & 1));  // bIsStanding
        if (flying) {
            LOGI("Host cutscene %s: not here, our player is in the air or on the water", name.c_str());
            return;
        }
        LOGI("Host cutscene %s: loading here", name.c_str());
        // the loading sets us out of a vehicle (CCutsceneMgr::LoadCutsceneData_preload): we get back in afterwards
        if (CPlayerPed* me = FindPlayerPed(0); me->InVehicle() && me->Vehicle() && g_csCar < 0) {
            g_csCar = GetVehicleRef(me->Vehicle());
            g_csSeat = me->Vehicle()->Driver() == me ? -1 : PassengerSeatOf(me->Vehicle(), me);
        }
        scene::Prepare();  // before the cutscene's own streaming requests
        if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped) {
            DressCutsceneCj(host->ped);  // the story's CJ is the host
            g_cutsceneCj = host->ped;
        }
        g_origLoadCutscene(name.c_str());
        g_cutsceneCj = nullptr;
        g_csState = 1;
        g_csStartRequested = false;
    } else if (op == CS_START) {
        g_csStartRequested = true;
        g_csArea = area;
    } else if (op == CS_SPOT) {
        float at = r.F32();
        Vec3 p = r.Vec();
        scene::Spot s;
        s.ground = {p.x, p.y, p.z};
        s.heading = r.F32();
        s.pose = (scene::Pose)std::min<uint8_t>(r.U8(), scene::POSE_WALK);
        s.act = r.U8();
        if (r.Ok()) scene::FollowSpot(s, at);
    } else if (op == CS_END) {
        LOGI("Host cutscene ended (state %d, running %d)", g_csState, CutsceneRunning());
        if (g_csState == 2) PartnerEndCutscene();
        else if (g_csState == 1) g_csState = 3;
    }
}

void UpdatePartnerCutscene() {
    if (g_csState == 1 && g_csStartRequested && Ref<int32_t>(kCutsceneLoadStatus) == 2) {
        CVector o = Ref<CVector>(kCutsceneOffset);
        Cmd(0x01B4).I(0).I(0).Run();                   // SET_PLAYER_CONTROL off
        Cmd(0x04BB).I(g_csArea).Run();                 // SET_AREA_VISIBLE: cutscenes in interiors
        EnterCutsceneScene(o, g_csArea);
        Cmd(0x03CB).F(o.x).F(o.y).F(o.z).Run();        // LOAD_SCENE around the cutscene
        g_origStartCutscene();
        g_csState = 2;
        LOGI("Host cutscene started here");
    } else if (g_csState == 2) {
        StandNextToCutsceneCj();
    } else if (g_csState == 3 && (!CutsceneRunning() || Call<0x5B0570, bool>())) {  // CCutsceneMgr::HasCutsceneFinished
        PartnerClearCutscene();
    }
}

// ---------------------------------------------------------------- blips
void HostScanBlips(bool force) {
    std::set<uint8_t> present;
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!(Ref<uint8_t>(t + 0x25) & 2)) continue;  // m_bInUse
        uint32_t key = ((uint32_t)Ref<uint16_t>(t + 0x14) << 8) | (uint32_t)i;
        if (g_baseline.count(key)) continue;
        BlipRecord b;
        b.index = (uint8_t)i;
        b.type = (Ref<uint8_t>(t + 0x26) >> 2) & 0xF;
        b.display = Ref<uint8_t>(t + 0x26) & 3;
        b.colour = Ref<uint32_t>(t + 0x00);
        b.size = Ref<uint16_t>(t + 0x1C);
        b.sprite = Ref<uint8_t>(t + 0x24);
        b.flags25 = Ref<uint8_t>(t + 0x25);
        b.pos = Ref<CVector>(t + 0x08);
        int handle = Ref<int32_t>(t + 0x04);
        if (b.type == BLIP_CAR) {
            CVehicle* v = GetVehicle(handle);
            if (!v) continue;
            b.pos = v->GetPosition();
            if (NetVehicle* nv = Vehicles::Instance().Find(v)) b.entKind = 1, b.entId = nv->netId;
        } else if (b.type == BLIP_CHAR) {
            CPed* p = GetPed(handle);
            if (!p) continue;
            b.pos = p->GetPosition();
            if (p == FindPlayerPed(0)) b.entKind = 3, b.entId = Net().MyId();
            else if (RemotePlayer* rp = Players::Instance().FindByPed(p)) b.entKind = 3, b.entId = rp->id;
            else if (NetPed* np = Peds::Instance().Find(p)) b.entKind = 2, b.entId = np->netId;
        } else if (b.type == BLIP_OBJECT) {
            CObject* o = GetObject(handle);
            if (!o) continue;
            b.pos = o->GetPosition();
        } else if (b.type != BLIP_COORD && b.type != 5 && b.type != 6) {
            continue;  // pickups: not replicated yet; airstrips (8): the game's own, for a player in a plane
        }
        present.insert(b.index);
        auto it = g_sentBlips.find(b.index);
        if (!force && it != g_sentBlips.end() && it->second == b) continue;
        g_sentBlips[b.index] = b;
        Writer w(proto::G_BLIP);
        w.U8(BLIP_SET);
        b.Write(w);
        Net().Send(proto::CH_RELIABLE, w);
    }
    for (auto it = g_sentBlips.begin(); it != g_sentBlips.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        Writer w(proto::G_BLIP);
        w.U8(BLIP_REMOVE);
        w.U8(it->first);
        Net().Send(proto::CH_RELIABLE, w);
        it = g_sentBlips.erase(it);
    }
}

// a blip handle is its trace's counter << 16 | index; the engine frees an entity's blip with the entity
bool BlipAlive(int handle) {
    int i = handle & 0xFFFF;
    return i < kNumTraces && (Ref<uint8_t>(Trace(i) + 0x25) & 2) && Ref<uint16_t>(Trace(i) + 0x14) == (uint16_t)(handle >> 16);
}

void RemovePartnerBlip(PartnerBlip& pb) {
    if (pb.handle) Cmd(0x0164).I(pb.handle).Run();  // REMOVE_BLIP
    pb.handle = 0;
}

void ClearPartnerBlips() {
    for (auto& [i, pb] : g_blips) RemovePartnerBlip(pb);
    g_blips.clear();
}

void ApplyBlip(const BlipRecord& b) {
    // the entity the blip belongs to, if this game has it
    int ent = -1;
    uint8_t kind = b.entKind;
    if (kind == 1) {
        NetVehicle* nv = Vehicles::Instance().Find(b.entId);
        ent = nv && IsVehicleValid(nv->veh) ? nv->handle : -1;
    } else if (kind == 2) {
        ent = Peds::Instance().HandleOf(b.entId);
    } else if (kind == 3) {
        CPed* p = b.entId == Net().MyId() ? (CPed*)FindPlayerPed(0) : nullptr;
        if (const RemotePlayer* rp = Players::Instance().Get((uint8_t)b.entId); rp && b.entId != Net().MyId()) p = rp->ped;
        ent = p ? GetPedRef(p) : -1;
    }
    if (ent < 0) kind = 0;  // not here (yet): a coordinate blip at its position

    PartnerBlip& pb = g_blips[b.index];
    if (pb.handle && !BlipAlive(pb.handle)) pb.handle = 0;  // went with its entity (a mirror made again)
    if (pb.handle && (pb.entKind != kind || (kind && pb.entId != b.entId))) RemovePartnerBlip(pb);
    if (!pb.handle) {
        int h = 0;
        if (kind == 1) Cmd(0x0186).I(ent).Out(&h).Run();       // ADD_BLIP_FOR_CAR
        else if (kind != 0) Cmd(0x0187).I(ent).Out(&h).Run();  // ADD_BLIP_FOR_CHAR
        else Cmd(0x018A).F(b.pos.x).F(b.pos.y).F(b.pos.z).Out(&h).Run();  // ADD_BLIP_FOR_COORD
        pb.handle = h;
        pb.entKind = kind;
        pb.entId = b.entId;
        if (!h) return;
    }
    // the rest of the look is copied field by field
    uintptr_t t = Trace(pb.handle & 0xFFFF);
    Ref<uint32_t>(t + 0x00) = b.colour;
    Ref<uint16_t>(t + 0x1C) = b.size;
    Ref<uint8_t>(t + 0x24) = b.sprite;
    Ref<uint8_t>(t + 0x25) = (uint8_t)(b.flags25 | 2);  // keep m_bInUse
    Ref<uint8_t>(t + 0x26) = (uint8_t)((Ref<uint8_t>(t + 0x26) & ~3) | (b.display & 3));
    if (kind == 0) Ref<CVector>(t + 0x08) = b.pos;
}

std::map<uint8_t, BlipRecord> g_partnerBlipRecords;  // to retry entity attachment when mirrors appear

void HandleBlip(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (op == BLIP_SET) {
        BlipRecord b;
        b.Read(r);
        if (!r.Ok()) return;
        g_partnerBlipRecords[b.index] = b;
        ApplyBlip(b);
    } else if (op == BLIP_REMOVE) {
        uint8_t idx = r.U8();
        if (!r.Ok()) return;
        if (auto it = g_blips.find(idx); it != g_blips.end()) {
            RemovePartnerBlip(it->second);
            g_blips.erase(it);
        }
        g_partnerBlipRecords.erase(idx);
    } else if (op == BLIP_CLEAR) {
        ClearPartnerBlips();
        g_partnerBlipRecords.clear();
    }
}

// ---------------------------------------------------------------- scenes on the engine
// A mission's scene on the engine (no cutscene file: its script moves the camera and the actors, "Ryder" after the
// haircut) showed on the host's screen only: the partners kept their own camera. While the host's mission script holds
// the camera (CCamera::m_bLookingAtPlayer off, no cutscene), the host sends where it looks from, and a partner close by on
// foot or in a car watches through it: its camera follows, its player stands still (a car it drives coasts). Space gives
// it its own camera back, and Space again (standing still: Space is also the sprint and the handbrake) lets it watch
// again; one far away is only told (it can come over with /tp). The actors are the host's peds and the host's CJ,
// which its game mirrors anyway.
constexpr float kSceneWatchDist = 100.0f;
bool g_sceneSent = false;  // host
uint64_t g_sceneSentMs = 0;
CVector g_sceneLastSrc;

struct SceneCam {
    CVector src, front, up;
    float fov = 70.0f;
};
bool g_sceneOn = false;        // partner: the host's scene runs
uint64_t g_sceneRecvMs = 0;
SceneCam g_sceneWant, g_sceneShown;
bool g_sceneWatching = false, g_sceneOptOut = false, g_sceneCut = false, g_spaceWasDown = false;
uint64_t g_sceneShownMs = 0, g_sceneOffMs = 0;

bool ScriptHoldsCamera() {
    return !Ref<uint8_t>(addr::TheCamera + 0x2B) && !Ref<uint8_t>(0xB5F851);  // !m_bLookingAtPlayer, !CCutsceneMgr::ms_running
}

void HostSceneUpdate(bool replicating) {
    bool on = replicating && ScriptHoldsCamera();
    uint64_t now = NowMs();
    if (on) {
        if (now - g_sceneSentMs < 50) return;
        g_sceneSentMs = now;
        CVector src = cam::Source();
        bool cut = !g_sceneSent || (src - g_sceneLastSrc).Length() > 3.0f;  // a new shot: the partner jumps there
        g_sceneLastSrc = src;
        Writer w(proto::G_SCENE_CAMERA);
        w.U8(1);
        w.Vec({src.x, src.y, src.z});
        w.Vec({cam::Front().x, cam::Front().y, cam::Front().z});
        w.Vec({cam::Up().x, cam::Up().y, cam::Up().z});
        w.F32(cam::Fov());
        w.U8(cut);
        Net().Send(proto::CH_STATE, w);
        if (!g_sceneSent) LOGI("Scene on the engine: its camera goes to the partners");
        g_sceneSent = true;
    } else if (g_sceneSent) {
        g_sceneSent = false;
        Writer w(proto::G_SCENE_CAMERA);
        w.U8(0);
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Scene on the engine over");
    }
}

void StopWatchingScene() {
    if (!g_sceneWatching) return;
    g_sceneWatching = false;
    Cmd(0x02EB).Run();                 // RESTORE_CAMERA_JUMPCUT
    Cmd(0x0373).Run();                 // SET_CAMERA_BEHIND_PLAYER
    Cmd(0x01B4).I(0).I(1).Run();       // SET_PLAYER_CONTROL on
    LOGI("Own camera again");
}

void HandleSceneCamera(uint8_t from, Reader& r) {
    bool on = r.U8() != 0;
    SceneCam c;
    bool cut = false;
    if (on) {
        Vec3 s0 = r.Vec(), f = r.Vec(), u = r.Vec();
        c.src = {s0.x, s0.y, s0.z};
        c.front = {f.x, f.y, f.z};
        c.up = {u.x, u.y, u.z};
        c.fov = r.F32();
        cut = r.U8() != 0;
    }
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    if (!on) {
        if (g_sceneOn) LOGI("The host's scene is over");
        g_sceneOn = g_sceneOptOut = false;
        g_sceneOffMs = NowMs();
        StopWatchingScene();
        return;
    }
    // its camera comes unreliably, its end reliably: one more camera update can arrive after the end (the channels keep
    // no order between them) and would start the scene again for a second
    if (!g_sceneOn && NowMs() - g_sceneOffMs < 500) return;
    if (!g_sceneOn) LOGI("The host's scene on the engine started");
    g_sceneOn = true;
    g_sceneRecvMs = NowMs();
    g_sceneWant = c;
    g_sceneCut = g_sceneCut || cut;
}

bool CanWatchScene() {
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    return me && host && host->hasState && host->state.area == me->AreaCode() &&
           (host->state.pos - me->GetPosition()).Length() < kSceneWatchDist && g_csState == 0;
}

// the player stands still (on foot or in a car that has stopped): Space is no sprint or handbrake then
bool StandingStill() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return false;
    CVector v = me->InVehicle() && me->Vehicle() ? me->Vehicle()->MoveSpeed() : me->MoveSpeed();
    return v.Length() * 50.0f < 0.5f;  // m/s
}

void PartnerSceneUpdate() {
    uint64_t now = NowMs();
    if (g_sceneOn && now - g_sceneRecvMs > 1000) {  // the host is gone or its game stalls
        g_sceneOn = g_sceneOptOut = false;
        StopWatchingScene();
    }
    bool space = game::IsForeground() && (GetAsyncKeyState(VK_SPACE) & 0x8000);
    bool pressed = space && !g_spaceWasDown;
    g_spaceWasDown = space;
    if (g_sceneWatching && pressed) {
        g_sceneOptOut = true;
        StopWatchingScene();
        return;
    }
    if (pressed && g_sceneOn && g_sceneOptOut && StandingStill()) {
        g_sceneOptOut = false;  // back to the scene
        LOGI("Back to the host's scene");
    }
    bool watch = g_sceneOn && !g_sceneOptOut && CanWatchScene();
    if (!watch) {
        StopWatchingScene();
        return;
    }
    if (!g_sceneWatching) {
        g_sceneWatching = true;
        g_sceneCut = true;
        Cmd(0x01B4).I(0).I(0).Run();  // SET_PLAYER_CONTROL off: the player stands while it watches
        LOGI("Watching the host's scene");
    }
    // a new shot far from this player (the scene shows another place): the world there is loaded first, as the host's
    // script did for its own camera
    if (g_sceneCut)
        if (CPlayerPed* me = FindPlayerPed(0); me && (g_sceneWant.src - me->GetPosition()).Length() > 150.0f)
            Cmd(0x03CB).F(g_sceneWant.src.x).F(g_sceneWant.src.y).F(g_sceneWant.src.z).Run();  // LOAD_SCENE
    // follow the host's camera smoothly between its updates (20 a second), jump on a new shot
    float k = g_sceneCut ? 1.0f : 1.0f - std::exp(-(float)(now - g_sceneShownMs) * 0.012f);
    g_sceneShownMs = now;
    g_sceneShown.src = g_sceneShown.src + (g_sceneWant.src - g_sceneShown.src) * k;
    g_sceneShown.front = g_sceneShown.front + (g_sceneWant.front - g_sceneShown.front) * k;
    g_sceneCut = false;
    CVector at = g_sceneShown.src + g_sceneShown.front * 5.0f;
    Cmd(0x015F).F(g_sceneShown.src.x).F(g_sceneShown.src.y).F(g_sceneShown.src.z).F(0).F(0).F(0).Run();  // SET_FIXED_CAMERA_POSITION
    Cmd(0x0160).F(at.x).F(at.y).F(at.z).I(2).Run();  // POINT_CAMERA_AT_POINT, jump cut
}

// ---------------------------------------------------------------- screen state, timers

// The host's fades the partner sees are those of its scripts (a fade started while a script runs). Its other
// fades are its own: going through a door, a respawn. Following those darkened the partner's screen at every door of
// the host and made the light pulse.
bool g_scriptBlack = false;  // the latest script fade of the host went to black
float g_scriptFadeSec = 0.5f;

// changesOnly: the check every 200 ms of a mission sends a change at once and the same state every 2 s (it went out every
// 200 ms on the reliable channel, ahead of real events when packets were lost)
void HostSendScreen(bool active, bool changesOnly = false) {
    bool black = g_scriptBlack && (FadeAlpha() > 100.0f || (Fading() && FadeDirection() == 0));  // dark or going dark
    uint8_t flags = (active ? FADE_ACTIVE : 0) | (TheCameraWide() ? FADE_WIDE : 0) | (black ? FADE_BLACK : 0);
    static uint8_t sentFlags = 0xFF;
    static uint64_t sentMs = 0;
    if (changesOnly && flags == sentFlags && NowMs() - sentMs < 2000) return;
    sentFlags = flags;
    sentMs = NowMs();
    Writer w(proto::G_FADE);
    w.U8(flags);
    w.F32(g_scriptFadeSec);
    Net().Send(proto::CH_RELIABLE, w);
}

void ScriptFaded(int16_t dir, float seconds) {
    if (!Net().Online() || !Net().IsHost()) return;
    g_scriptBlack = dir == 0;
    g_scriptFadeSec = std::clamp(seconds, 0.1f, 5.0f);
    if (!Replicating()) return;
    HostSendScreen(true);
    LOGD("Script fade %s (%.1f s) sent", g_scriptBlack ? "out" : "in", g_scriptFadeSec);
}

void HandleScreen(uint8_t from, Reader& r) {
    uint8_t flags = r.U8();
    float duration = r.F32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (!(flags & FADE_ACTIVE)) {
        if (g_weFaded && Darkening()) Fade(0.5f, 1);
        if (g_weWide && g_csState == 0) SetWideScreen(false);
        g_weFaded = g_weWide = false;
        return;
    }
    bool wide = (flags & FADE_WIDE) != 0;
    if (wide != TheCameraWide() && g_csState == 0) {
        SetWideScreen(wide);
        g_weWide = wide;
    }
    duration = std::clamp(duration, 0.1f, 5.0f);
    if (flags & FADE_BLACK) {
        if (FadeAlpha() < 250.0f && !(Fading() && FadeDirection() == 0)) {
            Fade(duration, 0);
            LOGD("Host's script fade: out");
        }
        g_weFaded = true;
    } else if (g_weFaded) {
        if (Darkening() && !(Fading() && FadeDirection() == 1)) {
            Fade(duration, 1);
            LOGD("Host's script fade: in");
        } else if (!Darkening()) {
            g_weFaded = false;
        }
    }
}

void HostSendOnscreen() {
    // the timer block refers to script variables: their values travel with it
    std::vector<uint8_t> block((uint8_t*)kOnscreenTimer, (uint8_t*)kOnscreenTimer + kOnscreenSize);
    bool any = Ref<uint8_t>(kOnscreenTimer + 0x38) != 0;  // clock enabled
    for (int i = 0; i < 4; ++i) any |= Ref<uint8_t>(kOnscreenTimer + 0x40 + i * 0x44 + 0x3E) != 0;
    if (!any && block == g_lastOnscreen) return;  // nothing shown and nothing changed
    Writer w(proto::G_ONSCREEN);
    w.Bytes(block.data(), block.size());
    auto var = [&](uint32_t off) { w.I32(off && off + 4 <= (uint32_t)Ref<int32_t>(kMainScriptSize) ? Ref<int32_t>(addr::ScriptSpace + off) : 0); };
    var(Ref<uint32_t>(kOnscreenTimer + 0x00));  // clock
    for (int i = 0; i < 4; ++i) {
        uintptr_t c = kOnscreenTimer + 0x40 + (uintptr_t)i * 0x44;
        var(Ref<uint32_t>(c + 0x00));
        var(Ref<uint32_t>(c + 0x04));
    }
    w.Str(MissionTable(), 8);  // the labels are GXT keys, usually of the mission's table
    // nothing shown any more goes once, so reliably: lost, the partner kept the last timer until the mission's end
    Net().Send(any ? proto::CH_STATE : proto::CH_RELIABLE, w);
    g_lastOnscreen = std::move(block);
}

void HandleOnscreen(uint8_t from, Reader& r) {
    std::vector<uint8_t> block = r.Bytes(kOnscreenSize);
    int32_t values[9];
    for (int32_t& v : values) v = r.I32();
    std::string table = r.Str(8);
    if (!r.Ok() || block.size() != kOnscreenSize || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    UseMissionTable(table);
    int32_t mainSize = Ref<int32_t>(kMainScriptSize);
    auto set = [&](uint32_t off, int32_t v) {  // off from the network: off + 4 would wrap past 0xFFFFFFFB
        if (off && mainSize >= 4 && off <= (uint32_t)mainSize - 4) Ref<int32_t>(addr::ScriptSpace + off) = v;
    };
    uint8_t* b = block.data();
    set(*(uint32_t*)(b + 0x00), values[0]);
    for (int i = 0; i < 4; ++i) {
        set(*(uint32_t*)(b + 0x40 + i * 0x44 + 0x00), values[1 + i * 2]);
        set(*(uint32_t*)(b + 0x40 + i * 0x44 + 0x04), values[2 + i * 2]);
    }
    memcpy((void*)kOnscreenTimer, b, kOnscreenSize);
    g_onscreenSet = true;
}

void ClearPartnerOnscreen() {
    if (!g_onscreenSet) return;
    // the display flags come back every frame while a variable is set (COnscreenTimer::ProcessForDisplay), and the
    // host's mission clears its counters in its last frame, which never reached us
    CallMethod<0x44CBC0>((void*)kOnscreenTimer);  // COnscreenTimer::Init
    g_onscreenSet = false;
}

// ---------------------------------------------------------------- mission start/end
void HandleMissionStart(uint8_t from, Reader& r) {
    std::string name = r.Str(16);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
    if (!g_partnerMission) ui::TheChat().AddSystem(Format(TR("%s начал миссию.", "%s started a mission."), Net().PeerName(from).c_str()));
    g_partnerMission = true;
    LOGI("Host mission started (%s)", name.c_str());
}

void PartnerMissionCleanup() {
    // what the mission did not take back stays (the host keeps it too); a gun of the partner's own it went over keeps
    // the larger count (Emmet's pistol is left with 10)
    if (CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr)
        for (auto& [weapon, own] : g_replaced)
            for (int i = 0; i < NUM_WEAPON_SLOTS; ++i)
                if (own.first == weapon && me->Weapons()[i].type == weapon && me->Weapons()[i].ammoTotal < own.second)
                    Cmd(0x017B).I(GetPedRef(me)).I(weapon).I(std::min(own.second, 99999)).Run();  // SET_CHAR_AMMO
    g_missionWeapons.clear();
    g_replaced.clear();
    ClearPartnerBlips();
    g_partnerBlipRecords.clear();
    g_partnerAreas.clear();
    ClearPartnerMarked();
    ClearPartnerOnscreen();
    // a cutscene loaded and never started (the mission failed in between) hid the player and took its controls too
    if (g_csState == 2) PartnerEndCutscene();
    if (g_csState != 0) PartnerClearCutscene();
    if (g_weFaded && Darkening()) Fade(0.5f, 1);
    if (g_weWide) SetWideScreen(false);
    g_weFaded = g_weWide = false;
}

void HandleMissionEnd(uint8_t from, Reader&) {
    if (from != Net().HostId() || Net().IsHost()) return;
    if (g_partnerMission) ui::TheChat().AddSystem(TR("Миссия закончилась.", "The mission is over."));
    g_partnerMission = false;
    g_partnerMissionEndedAt = NowMs();
    PartnerMissionCleanup();
    LOGI("Host mission ended");
}

void HandleReward(uint8_t from, Reader& r) {
    int32_t money = r.I32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || money <= 0 || money > 10000000) return;
    PlayerInfos()[0].money += money;
    ui::TheChat().AddSystem(Format(TR("Награда за миссию: $%d.", "Mission reward: $%d."), money));
}

// ---------------------------------------------------------------- module
void SendWait(bool waiting) {
    Writer w(proto::G_MISSION_WAIT);
    w.U8(waiting);
    if (waiting)
        for (float f : {g_waitArea.x1, g_waitArea.y1, g_waitArea.x2, g_waitArea.y2, g_waitArea.z}) w.F32(f);
    Net().Send(proto::CH_RELIABLE, w);
}

// host: a partner's game (re)started — send the running mission's state again
void MissionsResync() {
    if (!Net().IsHost()) return;
    if (g_waitSent) SendWait(true);  // waiting at a marker: sent only on change, a partner new here has not seen it
    if (!g_active) return;
    Writer w(proto::G_MISSION_START);
    w.Str(g_missionName, 16);
    Net().Send(proto::CH_RELIABLE, w);
    HostScanBlips(true);
    HostSendMarked(true);
    g_lastAreasMsg.clear();
    g_lastOnscreen.clear();
    HostSendScreen(true);  // the fade it joins into: a new game starts black until its prologue fades in
    // A cutscene loaded and not started yet goes too (CCutsceneMgr::ms_cutsceneLoadStatus): its start came alone and was
    // ignored. The start with its interior: area 0 hid the interior the scene plays in.
    bool loaded = Ref<int32_t>(0xB5F84C) != 0;
    if ((CutsceneRunning() || loaded) && g_cutsceneName[0]) {
        SendCutscene(CS_LOAD, g_cutsceneName);
        if (CutsceneRunning()) {
            SendCutscene(CS_START, "", CurrentArea());
            if (scene::Spot s; scene::CurrentSpot(s)) SendSpot(s, 0);
        }
    }
}

class Missions : public Module {
public:
    const char* Name() const override { return "Missions"; }
    void OnPeerResync(uint8_t) override { MissionsResync(); }

    void OnInstallHooks() override {
        mem::HookFn(0x469F00, &HookScriptProcess, &g_origScriptProcess, "CRunningScript::Process");
        mem::RedirectCallFn(0x484361, &HookScriptClearArea, &g_origClearArea);  // CLEAR_AREA (opcode 0395)
        mem::HookFn(0x609590, &HookCanPlayerStartMission, &g_origCanStart, "CPlayerPed::CanPlayerStartMission");
        mem::HookFn(0x464C20, &HookStartNewScript, &g_origStartScript, "CTheScripts::StartNewScript");
        mem::HookFn(0x69F0B0, &HookMsg, &g_origMsg, "CMessages::AddMessage");
        mem::HookFn(0x69F1E0, &HookMsgQ, &g_origMsgQ, "CMessages::AddMessageJumpQ");
        mem::HookFn(0x69E360, &HookMsgNum, &g_origMsgNum, "CMessages::AddMessageWithNumber");
        mem::HookFn(0x69E4E0, &HookMsgQNum, &g_origMsgQNum, "CMessages::AddMessageJumpQWithNumber");
        mem::HookFn(0x69F2B0, &HookBig, &g_origBig, "CMessages::AddBigMessage");
        mem::HookFn(0x69F370, &HookBigQ, &g_origBigQ, "CMessages::AddBigMessageQ");
        mem::HookFn(0x69E5F0, &HookBigNum, &g_origBigNum, "CMessages::AddBigMessageWithNumber");
        mem::HookFn(0x69E6E0, &HookBigQNum, &g_origBigQNum, "CMessages::AddBigMessageWithNumberQ");
        mem::HookFn(0x588BE0, &HookHelp, &g_origHelp, "CHud::SetHelpMessage");
        mem::HookFn(0x69DCD0, &HookClear, &g_origClear, "CMessages::ClearMessages");
        mem::HookFn(0x69DD30, &HookClearSmall, &g_origClearSmall, "CMessages::ClearSmallMessagesOnly");
        mem::HookFn(0x485E00, &HookHighlight, &g_origHighlight, "CTheScripts::HighlightImportantArea");
        mem::RedirectCallFn(0x4845F8, &HookDrawSphere, &g_origDrawSphere);  // DRAW_SPHERE -> C3dMarkers::PlaceMarkerSet
        mem::RedirectCallFn(0x586D81, &RadarOnMission, &g_origRadarOnMission);  // CRadar::DrawCoordBlip
        mem::RedirectCallFn(0x585C4E, &RadarOnMission, &g_origRadarOnMission);  // CRadar::Draw3dMarkers
        mem::HookFn(0x4D5E80, &HookLoadCutscene, &g_origLoadCutscene, "CCutsceneMgr::LoadCutsceneData");
        mem::HookFn(0x5B1460, &HookStartCutscene, &g_origStartCutscene, "CCutsceneMgr::StartCutscene");
        mem::RedirectCallFn(0x4D5EA3, &HookRebuildCutscenePlayer, &g_origCutscenePlayer);  // in LoadCutsceneData
        mem::RedirectCallFn(0x5B1947, &HookSkipButton, &g_origSkipButton);  // the skip check of CCutsceneMgr::Update_overlay
        Net().SetHandler(proto::G_TEXT, &HandleText);
        Net().SetHandler(proto::G_BLIP, &HandleBlip);
        Net().SetHandler(proto::G_MARKER, &HandleMarkers);
        Net().SetHandler(proto::G_MISSION_WAIT, &HandleMissionWait);
        Net().SetHandler(proto::G_CUTSCENE, &HandleCutscene);
        Net().SetHandler(proto::G_FADE, &HandleScreen);
        Net().SetHandler(proto::G_ONSCREEN, &HandleOnscreen);
        Net().SetHandler(proto::G_MISSION_START, &HandleMissionStart);
        Net().SetHandler(proto::G_MISSION_END, &HandleMissionEnd);
        Net().SetHandler(proto::G_MISSION_REWARD, &HandleReward);
        Net().SetHandler(proto::G_MISSION_WEAPON, &HandleMissionWeapon);
        Net().SetHandler(proto::G_CHECKPOINTS, &HandleMarked);
        Net().SetHandler(proto::G_SCENE_CAMERA, &HandleSceneCamera);
    }

    // The markers partners wait at are lit in the host's game before its frame: a 3D marker shows only when it was placed
    // before C3dMarkers::Update (CSpecialFX::Update, in CGame::Process), which makes it active; the rendering draws the
    // active ones and then clears them, so one placed after the frame was cleared unseen.
    void OnPreProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || !Net().IsHost() || g_active) return;
        for (auto& [id, w] : g_partnersWaiting)
            g_origHighlight(w.area.id ^ 0x20000000u, w.area.x1, w.area.y1, w.area.x2, w.area.y2, w.area.z);
    }

    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        HealInheritedFlag();  // in a game of one's own too: such a save is broken there as well
        if (!Net().Online()) return;
        if (Net().IsHost()) HostUpdate();
        else PartnerUpdate();
    }
    // the partner's spot in a cutscene is chosen with the camera of the frame being drawn
    void OnRender() override {
        if (!CutsceneRunning()) return;
        if (Net().Online() && !Net().IsHost()) scene::Follow();  // the host chooses the spot
        else scene::Analyse();
    }

    void OnSessionStart() override {
        if (!Net().IsHost())
            ui::TheChat().AddSystem(TR("Сюжетные миссии запускает хост, а вы проходите их вместе с ним.",
                                       "The host starts the story missions, and you play them together."));
    }
    void OnPeerLeave(uint8_t id) override {
        // the host gone in the middle of its mission (its game crashed) while a dedicated server goes on: its fade-out
        // had no fade-in to follow, and the partner's screen stayed dark
        if (Net().IsHost() || id != Net().HostId() || !core::IsPlayerReady()) return;
        if (g_csState == 2) PartnerEndCutscene();
        if (g_csState != 0) PartnerClearCutscene();
        PartnerMissionCleanup();
    }
    void OnSessionEnd() override {
        if (core::IsPlayerReady()) {  // the host's cutscene, blips, markers and fades stay no longer
            if (g_csState == 2) PartnerEndCutscene();
            if (g_csState != 0) PartnerClearCutscene();
            PartnerMissionCleanup();
        }
        Reset();
    }
    void OnGameStarted() override {
        Reset();
        g_inheritedFlagMs = core::IsOnMission() ? NowMs() : 0;
        g_lastPassed.assign((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));  // what the loaded game had
        g_progressAt = 0;
    }
    void OnGameShutdown() override {
        // the host's world goes (a game loaded, a restart): its cutscene and mission end for the partners too, who
        // stayed frozen in the cutscene, waiting for an end that never came, and then at the next mission marker
        if (Net().Online() && Net().IsHost()) {
            if (g_hostCutscene) SendCutscene(CS_END);
            if (g_active) {
                Writer w(proto::G_BLIP);
                w.U8(BLIP_CLEAR);
                Net().Send(proto::CH_RELIABLE, w);
                Net().Send(proto::CH_RELIABLE, Writer(proto::G_MISSION_END));
            }
            if (g_waitSent) SendWait(false);
        }
        Reset();
    }

private:
    static void Reset() {
        g_active = g_hostCutscene = false;
        g_baseline.clear();
        g_sentBlips.clear();
        g_areas.clear();
        g_blips.clear();  // the game removes its blips with the world
        g_partnerBlipRecords.clear();
        g_partnerAreas.clear();
        g_placed.clear();  // checkpoints and spheres too
        g_sentMarked.clear();
        g_missionWeapons.clear();
        g_partnerMission = g_weFaded = g_weWide = g_onscreenSet = false;
        g_waitSent = g_hostWaits = false;
        g_waitMs = 0;
        g_partnersWaiting.clear();
        g_atMarkerSent = false;
        if (g_sceneWatching) StopWatchingScene();
        g_sceneOn = g_sceneOptOut = g_sceneSent = false;
        g_csState = 0;
        g_inScene = false;
        g_csCar = g_csSeat = -1;
        g_skipOpen = false;
        g_skipYes.clear();
        g_skipNoMs = 0;
    }

    static void HostUpdate() {
        if (g_scriptBlack && !Fading() && FadeAlpha() < 1.0f) g_scriptBlack = false;  // clear again, whoever faded it in
        void* script = FindMissionScript();
        bool active = HostOnMission();
        std::string name = script ? std::string(Field<char[8]>(script, 0x08), strnlen(Field<char[8]>(script, 0x08), 8)) : "";
        // the flag up with no mission script for long (a modded build had it so for a whole session): the story's markers
        // stay hidden and the partners count the host as on a mission. Said once in the log.
        static uint64_t flagOnlySince = 0;
        static bool flagOnlyLogged = false;
        if (!active || script) {
            flagOnlySince = 0;
            flagOnlyLogged = false;
        } else if (!flagOnlySince) {
            flagOnlySince = NowMs();
        } else if (!flagOnlyLogged && NowMs() - flagOnlySince > 60000) {
            flagOnlyLogged = true;
            LOGW("The mission flag ($1636) has been up for a minute with no mission script running");
        }
        if (active && !g_active) {
            // blips that exist now are not the mission's
            g_baseline.clear();
            for (int i = 0; i < kNumTraces; ++i)
                if (Ref<uint8_t>(Trace(i) + 0x25) & 2) g_baseline.insert(((uint32_t)Ref<uint16_t>(Trace(i) + 0x14) << 8) | (uint32_t)i);
            g_sentBlips.clear();
            g_missionName.clear();
            g_moneyAtStart = PlayerInfos()[0].money;
            LOGI("Mission started (%s)", name.c_str());
        }
        if (active && name != g_missionName && !name.empty()) {
            g_missionName = name;
            Writer w(proto::G_MISSION_START);
            w.Str(name, 16);
            Net().Send(proto::CH_RELIABLE, w);
        } else if (active && !g_active) {
            Writer w(proto::G_MISSION_START);
            w.Str("", 16);
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (!active && g_active) {
            Writer w(proto::G_BLIP);
            w.U8(BLIP_CLEAR);
            Net().Send(proto::CH_RELIABLE, w);
            Net().Send(proto::CH_RELIABLE, Writer(proto::G_MISSION_END));
            if (int32_t earned = PlayerInfos()[0].money - g_moneyAtStart; earned > 0) {
                Writer rw(proto::G_MISSION_REWARD);
                rw.I32(earned);
                Net().Send(proto::CH_RELIABLE, rw);
            }
            HostSendScreen(false);
            g_sentMarked.clear();  // the partner clears its copies at the mission's end
            g_scriptBlack = false;
            g_sentBlips.clear();
            g_areas.clear();
            LOGI("Mission ended (%s)", g_missionName.c_str());
            // the story moved on: a mission passed (CStats::LastMissionPassedName). The call that often follows one
            // (the next script) unlocks more, like the gym after Sweet's third: the partners reload the world once,
            // after both, so they wait 30 s for it, or until the next script is over
            std::string passed((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));
            if (passed != g_lastPassed) {
                g_lastPassed = passed;
                g_progressAt = NowMs() + 30000;
            } else if (g_progressAt) {
                g_progressAt = NowMs() + 1000;
            }
        }
        g_active = active;
        if (g_progressAt && !active && NowMs() >= g_progressAt) {
            g_progressAt = 0;
            flow::WorldProgressed();
        }

        // the markers partners wait at are lit here too (OnPreProcess, before the game's frame); gone with a partner who left
        for (auto it = g_partnersWaiting.begin(); it != g_partnersWaiting.end();)
            it = Net().Peers().count(it->first) ? std::next(it) : g_partnersWaiting.erase(it);

        // the partner's screen shows where the host waits
        bool waiting = !active && NowMs() - g_waitMs < 1500;
        if (waiting != g_waitSent) {
            g_waitSent = waiting;
            LOGI(waiting ? "Mission marker at %.1f,%.1f: waiting for the partner" : "Mission marker at %.1f,%.1f: not waiting any more",
                 (g_waitArea.x1 + g_waitArea.x2) * 0.5f, (g_waitArea.y1 + g_waitArea.y2) * 0.5f);
            SendWait(waiting);
        }

        HostSkipUpdate();
        HostSceneUpdate(active || Replicating());
        // cutscene end
        bool cs = CutsceneRunning();
        if (g_hostCutscene && !cs) {
            SendCutscene(CS_END);
            LOGI("Cutscene ended (replicated)");
        }
        g_hostCutscene = cs;
        scene::Spot spot;
        float at;
        if (scene::TakeDecision(spot, at)) SendSpot(spot, at);
        if (cs && scene::CurrentSpot(spot)) {  // the partner joins the scene (and its game takes the same spot)
            CEntity* cj = CutsceneCj();
            for (auto& [id, rp] : Players::Instance().All())
                if (rp.ped) {
                    // the scene's interior, not the one its game reports: before its cutscene starts there and after
                    // it ends it is elsewhere, and a ped of another area is not drawn (it vanished from the scene)
                    if (cj) rp.ped->AreaCode() = cj->AreaCode();
                    scene::Hold(rp.ped, spot);
                }
            g_sceneHeld = true;
        } else if (!cs && g_sceneHeld) {
            g_sceneHeld = false;
            scene::ReleaseAll();
        }

        if (!active || Net().Peers().empty()) return;
        uint64_t now = NowMs();
        if (now - g_lastFadeMs >= 200) {
            g_lastFadeMs = now;
            HostSendScreen(true, true);
        }
        if (now - g_lastTick < 250) return;
        g_lastTick = now;
        HostScanBlips(false);
        HostSendOnscreen();
        HostSendMarked(false);
        // destination markers drawn during the last frames
        Writer w(proto::G_MARKER);
        std::vector<Area> live;
        for (auto it = g_areas.begin(); it != g_areas.end();) {
            if (now - it->second.seenMs > 300) {
                it = g_areas.erase(it);
                continue;
            }
            live.push_back(it->second);
            ++it;
        }
        w.U8((uint8_t)std::min<size_t>(live.size(), 32));
        for (size_t i = 0; i < live.size() && i < 32; ++i) {
            const Area& a = live[i];
            w.U32(a.id);
            w.U8(a.kind);
            w.F32(a.x1);
            w.F32(a.y1);
            w.F32(a.x2);
            w.F32(a.y2);
            w.F32(a.z);
        }
        // the partner draws them only within 1 s of the latest list (a host gone or paused takes them away): an unchanged
        // list goes again every 500 ms, or a fixed destination's marker went out after a second there
        static uint64_t repeatedMs = 0;
        if (w.Buffer() != g_lastAreasMsg) {
            g_lastAreasMsg = w.Buffer();
            repeatedMs = now;
            Net().Send(proto::CH_RELIABLE, w);
        } else if (!live.empty() && now - repeatedMs >= 500) {
            repeatedMs = now;
            Net().Send(proto::CH_STATE, w);
        }
    }

    static void PartnerUpdate() {
        UpdatePartnerCutscene();
        PartnerSceneUpdate();
        // The host's vote to skip while no cutscene runs here (it never started): nothing here can answer it, and the
        // host waited 15 s for this player. A scene a little late here gets 3 s to start and be watched.
        static uint64_t voteWithoutScene = 0;
        if (!g_skipOpen || CutsceneRunning() || g_skipYes.count(Net().MyId())) voteWithoutScene = 0;
        else if (!voteWithoutScene) voteWithoutScene = NowMs();
        else if (NowMs() - voteWithoutScene > 3000) SkipVote(Net().MyId(), true);
        // our own missions (a mini-game, a side job), for the log: reports of a partner stuck in one had nothing on it
        static std::string own;
        void* script = FindMissionScript();
        std::string name = script ? std::string(Field<char[8]>(script, 0x08), strnlen(Field<char[8]>(script, 0x08), 8)) : "";
        if (name != own) {
            if (name.empty()) LOGI("Own mission ended (%s)", own.c_str());
            else LOGI("Own mission started (%s)", name.c_str());
            own = name;
        }
        // in a mission marker the host is told (its game starts the mission once the host stands in it too)
        if (NowMs() - g_atMarkerCheckMs >= 250) {
            g_atMarkerCheckMs = NowMs();
            Area at{};
            CPlayerPed* me = FindPlayerPed(0);
            bool atMarker = me && flow::InHostWorld() && !HostMissionRecent(0) && !g_hostWaits && !me->InVehicle() &&
                            MarkerAt(me->GetPosition(), at);
            if (atMarker != g_atMarkerSent) {
                g_atMarkerSent = atMarker;
                LOGI(atMarker ? "At a mission marker (%.1f,%.1f): the host is told" : "Left the mission marker", (at.x1 + at.x2) * 0.5f,
                     (at.y1 + at.y2) * 0.5f);
                Writer w(proto::G_MISSION_WAIT);
                w.U8(atMarker);
                if (atMarker)
                    for (float f : {at.x1, at.y1, at.x2, at.y2, at.z}) w.F32(f);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
        if (g_hostWaits) g_origHighlight(g_waitArea.id, g_waitArea.x1, g_waitArea.y1, g_waitArea.x2, g_waitArea.y2, g_waitArea.z);
        // markers are drawn every frame while the host keeps them (stale after 1 s: host gone/paused)
        if (NowMs() - g_partnerAreasMs < 1000)
            for (const Area& a : g_partnerAreas) {
                if (a.kind == AREA_SPHERE) DrawSphere(a.id ^ 0x40000000u, a);
                else g_origHighlight(a.id ^ 0x40000000u, a.x1, a.y1, a.x2, a.y2, a.z);
            }
        // entity blips whose mirror appeared later
        static uint64_t last = 0;
        if (NowMs() - last > 500) {
            last = NowMs();
            for (auto& [i, b] : g_partnerBlipRecords) {
                auto it = g_blips.find(i);
                bool coordFallback = it != g_blips.end() && it->second.entKind == 0 && b.entKind != 0;
                if (coordFallback || b.entKind == 0) ApplyBlip(b);  // also moves coordinate blips of moving entities
            }
        }
    }
};

Missions g_missions;
struct Reg {
    Reg() { core::RegisterModule(&g_missions); }
} g_reg;
}  // namespace

std::string CutsceneSkipText(std::string& hint) {
    hint.clear();
    if (!Net().Online()) return "";
    uint8_t me = Net().MyId();
    if (!g_skipOpen) {
        bool no = g_skipNo != 0xFF && g_skipNo != me && g_skipNoMs && NowMs() - g_skipNoMs < 3000;
        return no ? Format(TR("%s хочет досмотреть", "%s wants to keep watching"), Net().PeerName(g_skipNo).c_str()) : "";
    }
    std::vector<uint8_t> players{me};
    for (auto& [id, peer] : Net().Peers())
        if (!Net().IsHost() || InHostWorld(id)) players.push_back(id);
    std::string yes, wait;
    int yesCount = 0;
    for (uint8_t id : players) {
        if (id == me) continue;
        std::string& list = g_skipYes.count(id) ? yes : wait;
        list += (list.empty() ? "" : ", ") + Net().PeerName(id);
        yesCount += g_skipYes.count(id) ? 1 : 0;
    }
    if (g_skipYes.count(me)) return wait.empty() ? "" : Format(TR("Ждём ответа %s", "Waiting for %s"), wait.c_str());
    hint = TR("Enter — пропустить, Esc — смотреть дальше", "Enter: skip, Esc: keep watching");
    return Format(yesCount > 1 ? TR("%s хотят пропустить катсцену", "%s want to skip the cutscene")
                               : TR("%s хочет пропустить катсцену", "%s wants to skip the cutscene"),
                  yes.c_str());
}

void CutsceneSkipPress() {
    if (Net().Online() && !g_skipYes.count(Net().MyId())) SkipVote(Net().MyId(), true);
}

bool CutsceneSkipDecline() {
    if (!Net().Online() || !g_skipOpen || g_skipYes.count(Net().MyId())) return false;
    SkipVote(Net().MyId(), false);
    return true;
}

std::string MissionWaitText() {
    if (!Net().Online()) return "";
    if (Net().IsHost()) {
        if (g_waitSent) return TR("Ждём напарника", "Waiting for your partner");
        for (auto& [id, w] : g_partnersWaiting)
            if (Net().Peers().count(id))
                return Format(TR("%s ждёт вас у маркера миссии", "%s is waiting for you at the mission marker"), Net().PeerName(id).c_str());
        return "";
    }
    if (g_sceneWatching) return TR("Сцена хоста · Пробел — своя камера", "The host's scene · Space: your own camera");
    if (g_sceneOn && g_sceneOptOut)
        return TR("Сцена хоста · Пробел на месте — смотреть", "The host's scene · Space standing still: watch it");
    if (g_sceneOn)
        return TR("Хост смотрит сцену миссии · /tp — к хосту", "The host watches a mission scene · /tp: go to the host");
    if (g_hostWaits)
        return Format(TR("%s ждёт вас у маркера миссии", "%s is waiting for you at the mission marker"), Net().PeerName(Net().HostId()).c_str());
    if (g_atMarkerSent)
        return Format(TR("Миссии начинает хост: %s получил приглашение", "The host starts missions: %s has been invited"),
                      Net().PeerName(Net().HostId()).c_str());
    return "";
}

bool HostReplicating() { return Replicating(); }

void* CurrentScript() { return g_currentScript; }

bool HostMissionRecent(uint64_t graceMs) { return g_partnerMission || NowMs() - g_partnerMissionEndedAt < graceMs; }

}  // namespace coop
