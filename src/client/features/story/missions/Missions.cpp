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
//      screen      - fades and widescreen borders (G_FADE), onscreen timers/counters (G_ONSCREEN); a partner near
//                    the host stands still through a widescreen scene of the mission;
//  * host -> partner at any time: the story's mission start points of the radar (G_CONTACTS).
// Mission peds and vehicles are ordinary host entities (Peds/Vehicles).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
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
#include "features/story/contest/Contest.h"
#include "features/story/courier/Courier.h"
#include "features/story/cutscene-partner/ScenePartner.h"
#include "features/story/jobs/Jobs.h"
#include "features/story/minigames/Minigames.h"
#include "features/story/pool/Pool.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

// the call of each script command's handler in CRunningScript::Process goes through here (see Missions::OnInstallHooks)
extern "C" void CoopScriptCommandStub();

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
// a blip handle is its trace's counter << 16 | index; the engine frees an entity's blip with the entity
bool BlipAlive(int handle) {
    int i = handle & 0xFFFF;
    return i < kNumTraces && (Ref<uint8_t>(Trace(i) + 0x25) & 2) && Ref<uint16_t>(Trace(i) + 0x14) == (uint16_t)(handle >> 16);
}

void* FindMissionScript() {
    for (void* s = Ref<void*>(kActiveScripts); s; s = Field<void*>(s, 0x00))
        if (Field<uint8_t>(s, 0xDC)) return s;  // m_bIsMission
    return nullptr;
}

// The threads that start only side activities, no story missions: as in a game of one's own for either player (the
// partner plays them in its own game while the host is not on a mission), and nobody waits at their markers
const char* const kSideThreads[] = {
    "gym",                                // gyms (GYMLS, GYMSF, GYMLV)
    "trace", "hotr", "kicks", "bloodr",   // race tournament and driving school, hotring, Kick Start, Blood Bowl
    "tri", "psch", "bikes", "bschoo",     // TRIA, flight school, bike school, boat school
    "shoot", "trucks", "quarrys",         // Ammu-Nation range, trucking, quarry
    "trainsl",                            // the airports' and stations' travel
    "psave1",                             // the save points: each saves his own game, nobody waits for the other
    "gfagnt",                             // the girlfriends' dates: the host waited at her door, the partner never dated
};
bool SideThread(const char* name) {
    for (const char* side : kSideThreads)
        if (!strncmp(name, side, 8)) return true;
    return false;
}

// What launched the mission that runs (LOAD_AND_LAUNCH_MISSION from a side thread: the races, the driving and flight
// schools) and what raised the mission flag with no mission yet (TRACE and PSCH fade out for a while first, the save
// points' menu, the valet job and the random phone calls hold it). Counted as the host's story, the partner got the
// host's race timers and prize, a black screen, the help for a call it never had, and could start nothing of its own.
char g_launcher[9] = {}, g_flagBy[9] = {};
bool SideFlag() { return SideThread(g_flagBy) || !_strnicmp(g_flagBy, "valet", 5) || !strncmp(g_flagBy, "mob_ran", 8); }

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

// a property purchase (BUYPRO1) is no mission for the partner: it only shows the house, Property.cpp shares the purchase
bool HostOnMission() {
    if (void* s = FindMissionScript()) {
        // its first frame, before its SCRIPT_NAME (CRunningScript::Init names it "noname"): a vehicle job or a BMX
        // challenge was the host's story for 13 ms, and the partner got "the host started a mission", its end and 5 s
        // of a host's mission refusing its own activities (the gym). The story's start comes a frame later.
        if (!strncmp(Field<char[8]>(s, 0x08), "noname", 8)) return false;
        return strncmp(Field<char[8]>(s, 0x08), "buypro1", 8) && !SideMission(s) && !SideThread(g_launcher);
    }
    return core::IsOnMission() && !SideFlag();
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
// CEntryExitManager::mp_poolEntryExits: both games load the same entrances in the same order, so an index names one
constexpr uintptr_t kEnexPool = 0x96A7D8;
constexpr size_t kEnexSize = 0x3C;  // sizeof(CEntryExit)
int16_t EnexIndex(void* enex) {
    auto* pool = Ref<CPoolHeader*>(kEnexPool);
    if (!enex || !pool || (uint8_t*)enex < pool->objects) return -1;
    size_t i = (size_t)((uint8_t*)enex - pool->objects) / kEnexSize;
    return i < (size_t)pool->size && pool->byteMap[i] >= 0 ? (int16_t)i : -1;
}
void* EnexAt(int16_t i) {
    auto* pool = Ref<CPoolHeader*>(kEnexPool);
    return pool && i >= 0 && i < pool->size && pool->byteMap[i] >= 0 ? pool->objects + (size_t)i * kEnexSize : nullptr;
}

struct BlipRecord {  // what the host sends for one blip
    uint8_t index = 0;
    uint8_t type = 0, entKind = 0;  // entKind: 0 coordinates, 1 vehicle netId, 2 ped netId, 3 player id, 4 object netId
    uint16_t entId = 0;
    CVector pos;
    uint32_t colour = 0;
    uint16_t size = 0;
    uint8_t sprite = 0, flags25 = 0, display = 0;
    // the entrance it is bound to (SET_BLIP_ENTRY_EXIT), by its index in the entrances' pool, -1: none. A blip inside
    // an interior (the pizza counter in "Ryder") is drawn at that entrance's street door; sent as it was, the partner
    // saw the interior's raw map spot, far out in Red County
    int16_t enex = -1;

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
        w.I16(enex);
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
        enex = r.I16();
    }
    bool operator==(const BlipRecord& o) const {
        // an entity's blip goes with the copy of the entity: its place counts only for a partner without that copy, and
        // every moving car or person with a blip went out again every 250 ms on the reliable channel
        return type == o.type && entKind == o.entKind && entId == o.entId && (pos - o.pos).Length() < (entKind ? 20.0f : 0.5f) &&
               colour == o.colour && size == o.size && sprite == o.sprite && flags25 == o.flags25 && display == o.display &&
               enex == o.enex;
    }
};

enum : uint8_t { AREA_LOCATE = 0, AREA_SPHERE = 1, AREA_CORONA = 2 };
struct Area {
    uint32_t id;
    float x1, y1, x2, y2, z;  // a sphere: centre x1, y1, z and radius x2; a corona: there, size x2, colour y2 (0xRRGGBB)
    uint64_t seenMs;
    uint8_t kind = AREA_LOCATE;
    bool mission = false;  // drawn by a mission's script (the others draw this game's own: the mod shops' while in a car)
    bool streamed = false;  // drawn by a streamed script (a shop's, a barber's): never a mission's start
};

enum : uint8_t { BLIP_SET = 1, BLIP_REMOVE = 2, BLIP_CLEAR = 3 };
enum : uint8_t { CS_LOAD = 1, CS_START = 2, CS_END = 3, CS_SKIP_VOTE = 4, CS_SKIP_STATE = 5, CS_SPOT = 6, CS_FOLLOW = 7 };
enum : uint8_t { TXT_MSG = 1, TXT_MSG_Q, TXT_BIG, TXT_BIG_Q, TXT_HELP, TXT_CLEAR, TXT_CLEAR_SMALL, TXT_CS_LINE, TXT_TUNE };
enum : uint8_t { FADE_ACTIVE = 1, FADE_WIDE = 2, FADE_BLACK = 4 };

// host
bool g_active = false;                        // a mission or its trigger runs
bool g_courierBlips = false;                  // host: Big Smoke's courier runs (no mission): its blips go as a mission's
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
char g_waitBy[9] = {};      // host: the script whose trigger it was
bool g_waitSent = false;
bool g_hostWaits = false;   // partner
Area g_waitArea{};

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
bool g_hostBlack = false;  // partner: the host's screen was black by its script at its last G_FADE
bool g_weFroze = false;  // our control is taken while the host's mission plays a scene
uint64_t g_partnerMissionEndedAt = 0;
std::string g_hostMission;  // partner: the script name of the host's story mission (G_MISSION_START)
// "Mike Toreno" (DRIV3) raises $7200: main.scm's APCHECK then opens the SF airport's main gate as its player comes within
// 65 m, its guard lying dead. Only the host's game ran the mission, and the partner came to a shut gate (MozzisRus, 29.10).
constexpr uint32_t kAirportGateOpen = 7200;
bool g_gateOpenSet = false;  // partner: we raised it
int g_csState = 0;  // 0 none, 1 loading, 2 running, 3 ending
int32_t g_csArea = 0;  // interior the host's cutscene plays in
bool g_csStartRequested = false;

bool Replicating() { return Net().Online() && Net().IsHost() && !Net().Peers().empty() && (g_active || HostOnMission()); }

// ---------------------------------------------------------------- shared side activities
// A vehicle job or a race runs in the game of who started it. While its start shows, he may call the other player in
// (Y), and the other, saying yes, gets its markers, checkpoints, timers and texts as the partner gets the host's story
// mission: he rides along or follows, helps (puts its burning cars out, shoots its criminals) and is paid what it paid at
// its end. Its people and cars are each game's own anyway; its levels (the paramedic's endless sprint) stay with who
// played it. Either player may own one, the host or the partner.
enum : uint8_t { SIDE_OFFER = 1, SIDE_ACCEPT = 2, SIDE_DECLINE = 3, SIDE_END = 4, SIDE_LEAVE = 5, SIDE_PAY = 6, SIDE_DROP = 7,
                 SIDE_RESYNC = 8, SIDE_LOOT = 9, SIDE_TAKEN = 10, SIDE_ALARM = 11 };
bool SideJob(const std::string& name) {
    static const char* const kJobs[] = {"taxiodd", "ambulan", "firetru", "copcar", "pimp",   "freight", "bcour",   "mtbiker",
                                        "stunt",   "cprace",  "burgjb",  "truck",  "tria",   "kicksta", "blood",   "quarry",
                                        "valet",   "shrange"};
    for (const char* j : kJobs)
        if (!_stricmp(name.c_str(), j)) return true;
    return false;
}
// the stadiums' events and races, in arenas of their own (areas 14 Kickstart, 15 Blood Ring, 7 8-Track, 4 Dirt Track)
bool StadiumJob(const std::string& name) { return name == "kicksta" || name == "blood" || name == "cprace"; }
// What the call and its notices name it: "зовёт вас на совместное задание в такси" (to), "конец совместной гонки"
// (of). Picked when shown: the language can be switched in the connect menu.
struct JobNames {
    const char *to, *of;
};
JobNames NamesOf(const std::string& name) {
    auto is = [&](const char* s) { return !_stricmp(name.c_str(), s); };
    if (is("taxiodd")) return {TR("совместное задание в такси", "a joint taxi job"), TR("совместного задания в такси", "the joint taxi job")};
    if (is("ambulan")) return {TR("совместное задание скорой", "a joint paramedic job"), TR("совместного задания скорой", "the joint paramedic job")};
    if (is("firetru"))
        return {TR("совместное задание пожарного", "a joint firefighter job"), TR("совместного задания пожарного", "the joint firefighter job")};
    if (is("copcar")) return {TR("совместное задание полиции", "a joint vigilante job"), TR("совместного задания полиции", "the joint vigilante job")};
    if (is("pimp")) return {TR("совместное задание сутенёра", "a joint pimp job"), TR("совместного задания сутенёра", "the joint pimp job")};
    if (is("freight"))
        return {TR("совместное задание на поезде", "a joint freight train job"), TR("совместного задания на поезде", "the joint freight train job")};
    if (is("bcour")) return {TR("совместную доставку", "a joint courier job"), TR("совместной доставки", "the joint courier job")};
    if (is("mtbiker"))
        return {TR("совместный заезд на Чилиад", "a joint Mount Chiliad ride"), TR("совместного заезда на Чилиад", "the joint Mount Chiliad ride")};
    if (is("stunt")) return {TR("совместные трюки", "joint stunts"), TR("совместных трюков", "the joint stunts")};
    if (is("burgjb")) return {TR("совместное ограбление", "a joint burglary"), TR("совместного ограбления", "the joint burglary")};
    if (is("truck"))
        return {TR("совместную грузоперевозку", "a joint trucking job"), TR("совместной грузоперевозки", "the joint trucking job")};
    if (is("tria")) return {TR("совместный триатлон", "a joint triathlon"), TR("совместного триатлона", "the joint triathlon")};
    if (is("kicksta")) return {TR("совместный Kickstart", "a joint Kickstart"), TR("совместного Kickstart", "the joint Kickstart")};
    if (is("blood")) return {TR("совместный Blood Ring", "a joint Blood Ring"), TR("совместного Blood Ring", "the joint Blood Ring")};
    if (is("quarry")) return {TR("совместную работу в карьере", "a joint quarry job"), TR("совместной работы в карьере", "the joint quarry job")};
    if (is("valet"))
        return {TR("совместную смену парковщика", "a joint valet shift"), TR("совместной смены парковщика", "the joint valet shift")};
    if (is("shrange"))
        return {TR("совместную стрельбу в тире", "a joint shooting range round"),
                TR("совместной стрельбы в тире", "the joint shooting range round")};
    return {TR("совместную гонку", "a joint race"), TR("совместной гонки", "the joint race")};  // cprace
}
std::string JobTo(const std::string& name) { return SideJob(name) ? NamesOf(name).to : name; }
std::string JobOf(const std::string& name) { return SideJob(name) ? NamesOf(name).of : name; }
// The jobs where who joins gets a vehicle of the job of its own and works too (BoardSide, Jobs.cpp): what it is told,
// and the one who called it (both take the other's name). Null for the rest.
const char* JoinerHint(const std::string& name) {
    if (name == "taxiodd")
        return TR("Рядом с игроком %s у вас будет своё такси, и подработка в нём начнётся сама. Ваши пассажиры появятся, когда вы "
                  "отъедете подальше. Деньги за всех пассажиров получаете оба.",
                  "You get a taxi of your own next to %s, and its job starts by itself. Your fares turn up once you drive a bit "
                  "farther off. You both get the money for every fare.");
    if (name == "ambulan")
        return TR("Рядом с игроком %s у вас будет своя скорая. Больные садятся к любому из вас: остановитесь рядом с больным и "
                  "отвезите его в больницу.",
                  "You get an ambulance of your own next to %s. The patients get into either one: stop next to a patient and take "
                  "them to hospital.");
    if (name == "firetru")
        return TR("Рядом с игроком %s у вас будет своя пожарная машина. Тушите огонь вместе: потушенное вами засчитывается.",
                  "You get a fire truck of your own next to %s. Put out the fires together: the ones you put out count.");
    if (name == "copcar")
        return TR("Рядом с игроком %s у вас будет своя полицейская машина. Ловите преступников вместе: убитые вами засчитываются.",
                  "You get a police car of your own next to %s. Hunt the criminals together: the ones you kill count.");
    if (name == "burgjb")
        return TR("Дома для ограбления открыты и для вас. Выносите вещи и кладите их у задних дверей фургона игрока %s (F): они "
                  "засчитываются в общую добычу.",
                  "The burglary houses are open to you too. Carry things out and put them down at the back of %s's van (F): they "
                  "count for the loot of both.");
    if (name == "kicksta")
        return TR("Вы окажетесь на стадионе рядом с игроком %s, и у вас будет свой мотоцикл. Короны, которые берёте вы, идут в общий счёт.",
                  "You get to the stadium next to %s, on a bike of your own. The coronas you take count for the score of both.");
    if (name == "blood")
        return TR("Вы окажетесь на арене рядом с игроком %s в такой же машине. Контрольные точки, до которых доезжаете вы, тоже "
                  "добавляют время.",
                  "You get to the arena next to %s in a car like theirs. The checkpoints you reach add time too.");
    if (name == "quarry")
        return TR("Когда игрок %s сядет в бульдозер или самосвал, рядом с вами появится такой же. Засчитываются камни и бомбы, "
                  "которые вы столкнёте бульдозером, и ваши удары по самосвалу подрывников. Самосвал с грузом не задевайте: груз "
                  "упадёт или взорвётся.",
                  "Once %s gets into the bulldozer or the dumper, you get one like it. The rocks and bombs you push with your "
                  "bulldozer count, and so do your rams on the bombers' dumper. Keep clear of the loaded dumper: its load falls off "
                  "or blows up.");
    if (name == "valet")
        return TR("Машину гостя может отогнать любой из вас: остановите её на отмеченном месте, и она засчитается в смену игрока "
                  "%s. Деньги за смену получаете оба.",
                  "Either of you can park the guest's car: stop it on the marked bay, and it counts for %s's shift. You both get "
                  "the shift's money.");
    if (name == "shrange")
        return TR("Вы встанете в кабинку тира рядом с игроком %s и получите то же оружие. Стреляйте по мишеням прямо перед "
                  "кабинкой: ваши попадания идут в общий счёт.",
                  "You stand in the range booth next to %s with the same gun. Shoot the targets right in front of the booth: "
                  "your hits count for both of you.");
    return nullptr;
}
const char* CallerHint(const std::string& name) {
    if (name == "burgjb")
        return TR("%s тоже выносит вещи из домов и кладёт их в ваш фургон.", "%s carries things out of the houses into your van too.");
    if (name == "taxiodd")
        return TR("%s получает своё такси и возит своих пассажиров: деньги за всех пассажиров получаете оба.",
                  "%s gets their own taxi and drives their own fares: you both get the money for every fare.");
    if (name == "ambulan")
        return TR("%s получает свою скорую: больные садятся к любому из вас.", "%s gets their own ambulance: the patients get into either one.");
    if (name == "firetru")
        return TR("%s получает свою пожарную машину и тушит огонь вместе с вами.", "%s gets their own fire truck and puts out the fires with you.");
    if (name == "copcar")
        return TR("%s получает свою полицейскую машину и ловит преступников вместе с вами.",
                  "%s gets their own police car and hunts the criminals with you.");
    if (name == "kicksta")
        return TR("%s получает свой мотоцикл и тоже собирает короны в ваш счёт.", "%s gets a bike of their own and takes coronas for your score too.");
    if (name == "blood")
        return TR("%s получает такую же машину и тоже добавляет вам время на контрольных точках.",
                  "%s gets a car like yours and adds time at the checkpoints too.");
    if (name == "quarry")
        return TR("%s получает такой же бульдозер или самосвал и толкает камни и бомбы вместе с вами.",
                  "%s gets a bulldozer or a dumper like yours and pushes the rocks and bombs with you.");
    if (name == "valet")
        return TR("%s тоже отгоняет машины гостей: они засчитываются в вашу смену. Не уходите от отеля, иначе смена закончится.",
                  "%s parks the guests' cars too: they count for your shift. Do not leave the hotel, or the shift ends.");
    if (name == "shrange")
        return TR("%s стреляет рядом с вами по вашим мишеням, и попадания идут в общий счёт.",
                  "%s shoots at your targets next to you, and the hits count for both of you.");
    return nullptr;
}
struct Side {
    std::string name;         // our own activity that may be shared, while it runs
    uint64_t offerUntil = 0;  // its call can be made until then (0: made, or too late)
    bool offered = false;     // made, no answer yet
    int32_t moneyAtStart = 0;
    int32_t paid = 0;         // what who joined ours has had of it so far
    uint8_t with = 0xFF;      // who joined ours
    uint8_t owner = 0xFF;     // whose we joined
    std::string joined;       // and its name
    uint8_t inviteFrom = 0xFF;  // a call to answer
    std::string inviteName;
    uint64_t inviteUntil = 0;
    uint64_t boardUntil = 0, boardTryMs = 0;  // joined: brought to its owner's vehicle until then (BoardSide)
    // joined a taxi job, our own one runs too: one fare at a time near its player, each drives its own (27.10: both work,
    // not one with the other sitting in its car). Its texts, marks and timers are ours then, the money goes to both.
    bool parallel = false;
    int32_t received = 0;  // what the other's job paid us while our own ran: not ours to pay on
    uint64_t taxiAt = 0;   // joined a taxi job, at the wheel of our own taxi: our own job starts then (StartOwnTaxi)
} g_side;
bool SideOwning() { return g_side.with != 0xFF; }
bool SideJoined() { return g_side.owner != 0xFF; }
// our valet shift (valet.scm, a streamed script, no mission) with the other player in it: its texts, marks and answers go
// as a side activity's
bool JointValet(void* script) { return Net().Online() && SideOwning() && g_side.name == "valet" && jobs::ValetThread(script); }
// A joint side activity's coronas (KICKSTA's 33, drawn each frame by DRAW_CORONA): to the other player as its markers, by
// their place (the command's own id is its ip, the same for all of them)
void NoteCorona() {
    const auto* p = (const float*)0xA43C78;  // CTheScripts::ScriptParams: x y z size type flare r g b
    const auto* n = (const int32_t*)p;
    uint32_t id = 0x20000000u | ((uint32_t)(int)(p[0] * 4.0f) & 0x3FFFu) << 14 | ((uint32_t)(int)(p[1] * 4.0f) & 0x3FFFu);
    g_areas[id] = {id, p[0], p[1], p[3], (float)((n[6] & 0xFF) << 16 | (n[7] & 0xFF) << 8 | (n[8] & 0xFF)), p[2], NowMs(), AREA_CORONA, true};
}
// The game whose mission this one shows: the host's story mission, or the side activity it joined
bool MissionSource(uint8_t from) {
    return (from == Net().HostId() && !Net().IsHost()) || (SideJoined() && !g_side.parallel && from == g_side.owner) ||
           pool::TextsFrom(from);
}
void SendSide(uint8_t op, uint8_t to, const std::string& name = "", int32_t money = 0) {
    Writer w(proto::G_SIDE);
    w.U8(op);
    w.U8(to);
    w.Str(name, 8);
    w.I32(money);
    Net().Send(proto::CH_RELIABLE, w);
}

// ---------------------------------------------------------------- partner: no mission triggers
void* g_currentScript = nullptr;
bool MissionScriptRuns() { return g_currentScript && Field<uint8_t>(g_currentScript, 0xDC); }  // m_bIsMission
// a streamed script's (clothes.scm, the barber's, the shops'): its code is neither main.scm's nor in the mission block
bool StreamedScriptRuns() {
    if (!g_currentScript || MissionScriptRuns()) return false;
    auto* ip = Field<uint8_t*>(g_currentScript, 0x14);  // m_pCurrentIP
    return ip < (uint8_t*)addr::ScriptSpace || ip >= (uint8_t*)addr::ScriptSpace + Ref<int32_t>(kMainScriptSize);
}
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
WeaponSlots* g_tickWeapons = nullptr;  // host: CJ's weapons before the mission script's commands of this frame
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
// our player rides in the other player's car (a copy of it here)
bool PassengerOfOther() {
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me && me->InVehicle() ? me->Vehicle() : nullptr;
    return v && v->Driver() != me && v->Driver() && Players::Instance().FindByPed(v->Driver());
}

// host: its mission's script moved CJ far (the next city, a trip skipped) or into another area (an interior only a
// script reaches, the motel and its roof in "Reuniting the Families"). The partners near the old place come along once
// the host can move again (CS_FOLLOW): they were left behind, often with the door shut. Nearer moves count as well when a
// partner is close by (the swim to the ladder of "The Da Nang Thang" ends with CJ set on the deck, 50 m up and over), or
// when CJ was a vehicle's gunner (that one's helicopter downed, CJ set in the water by the ship: the partner was in town,
// 28.10).
constexpr float kMovedNear = 30.0f, kMovedFar = 150.0f, kPartnerByOldPlace = 60.0f;
bool g_movedPending = false;
bool g_movedOffVehicle = false;  // CJ rode a vehicle (ATTACH_CHAR_TO_CAR) before the move
CVector g_movedFrom;
int8_t g_movedFromArea = 0;

void __fastcall HookScriptProcess(void* script, void* edx) {
    // a world load from inside the game: the old scripts would run frames without a player (null peds)
    if (core::IsRestarting()) return;
    // the mod shop's menu is the driver's: a passenger of the other player's car got one too (both paid, and the copy's
    // parts were the owner's again a moment later)
    if (Net().Online() && !strncmp(Field<char[8]>(script, 0x08), "carmod", 6) && PassengerOfOther()) return;
    void* prev = g_currentScript;
    g_currentScript = script;
    bool fading = Fading();
    int16_t dir = FadeDirection();
    float seconds = FadeDuration();
    CPlayerPed* cj = Field<uint8_t>(script, 0xDC) && Net().IsHost() && Replicating() ? FindPlayerPed(0) : nullptr;  // m_bIsMission
    WeaponSlots weapons;
    if (cj) weapons.Take(cj);
    WeaponSlots* prevWeapons = g_tickWeapons;
    g_tickWeapons = cj ? &weapons : nullptr;
    bool flag = core::IsOnMission();
    CVector before = cj ? cj->GetPosition() : CVector{};
    int8_t areaBefore = cj ? cj->AreaCode() : 0;
    bool onVehicle = cj && cj->AttachedTo();
    g_origScriptProcess(script, edx);
    g_tickWeapons = prevWeapons;
    if (!flag && core::IsOnMission()) memcpy(g_flagBy, Field<char[8]>(script, 0x08), 8);  // who raised it (SideFlag)
    g_currentScript = prev;
    if (cj && FindPlayerPed(0) == cj) SendWeaponChanges(weapons, cj);
    if (cj && FindPlayerPed(0) == cj && !g_movedPending) {
        float moved = (cj->GetPosition() - before).Length();
        if (moved > kMovedNear || (cj->AreaCode() != areaBefore && moved >= 3.0f)) {
            g_movedPending = true;
            g_movedOffVehicle = onVehicle;
            g_movedFrom = before;
            g_movedFromArea = areaBefore;
        }
    }
    // a fade that started during this script's commands is the script's (DO_FADE); a streamed script's (a shop's, the
    // gym's) stays with the host
    if (Fading() && (!fading || FadeDirection() != dir || FadeDuration() != seconds) && !Field<uint8_t>(script, 0xC7))
        ScriptFaded(FadeDirection(), FadeDuration());
}

// The host's mission edits the map in its own game only: SET_USES_COLLISION_OF_CLOSEST_OBJECT_OF_TYPE opens the
// Chinatown gate in "Mountain Cloud Boys" (the partner ran into a closed one), the bank's safe in "Small Town Bank",
// SWAP_NEAREST_BUILDING_MODEL the building site of "Deconstruction", SET_VISIBILITY_OF_CLOSEST_OBJECT_OF_TYPE the
// crack factory of "Yay Ka-Boom-Boom". The same commands run in the partner's game (G_MAP_EDIT), again after a resync.
struct MapEdit {
    uint16_t op;
    CVector at;
    float radius;
    int32_t a, b;  // the model and on/off, or the model and the one it becomes
};
std::vector<MapEdit> g_mapEdits;  // host: this mission's, for a partner that joins or reloads
// partner: the host's, applied whenever we come near (the map far away is not in memory, and comes back as it was built)
struct PartnerEdit {
    MapEdit e;
    bool applied = false;
};
std::vector<PartnerEdit> g_partnerEdits;

int32_t ResolvedModel(int32_t m) {  // a script's object model below 0 is an index into CTheScripts::UsedObjectArray
    return m < 0 && m > -395 ? Ref<int32_t>(0xA44B70 + (uintptr_t)(-m) * 0x1C + 0x18) : m;
}

void SendMapEdit(const MapEdit& e) {
    Writer w(proto::G_MAP_EDIT);
    w.U16(e.op);
    w.Vec({e.at.x, e.at.y, e.at.z});
    w.F32(e.radius);
    w.I32(e.a);
    w.I32(e.b);
    Net().Send(proto::CH_RELIABLE, w);
}

// The host's mission moves, turns or freezes the car CJ uses (SET_CAR_COORDINATES, SET_CAR_HEADING,
// FREEZE_CAR_POSITION: 86 missions), and the partner may be the one driving it: here it is a copy, and its owner's
// states undid the command at once ("Home Invasion" freezes the van at the lockup). The owner runs it on its car.
void SendCarCommand(uint16_t op, const int32_t* p) {
    CVehicle* v = GetVehicle(p[0]);
    NetVehicle* nv = v ? Vehicles::Instance().Find(v) : nullptr;
    if (!nv || !Vehicles::Instance().IsMirror(*nv)) return;
    Writer w(proto::G_VEH_SCRIPT);
    w.U16(nv->netId);
    w.U8(nv->owner);
    w.U16(op);
    for (int i = 1; i < 4; ++i) w.I32(p[i]);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("The mission's command %04X on car %04X goes to its driver's game (player %d)", op, nv->netId, nv->owner);
}

void HandleCarCommand(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t owner = r.U8();
    uint16_t op = r.U16();
    int32_t p[3];
    for (int32_t& v : p) v = r.I32();
    NetVehicle* nv = Vehicles::Instance().Find(id);
    if (!r.Ok() || from != Net().HostId() || owner != Net().MyId() || !core::IsPlayerReady() || !nv ||
        Vehicles::Instance().IsMirror(*nv) || !Vehicles::Alive(*nv))
        return;
    const auto* f = (const float*)p;
    if (op == 0x00AB && std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]))
        Cmd(0x00AB).I(nv->handle).F(f[0]).F(f[1]).F(f[2]).Run();  // SET_CAR_COORDINATES
    else if (op == 0x0175 && std::isfinite(f[0]))
        Cmd(0x0175).I(nv->handle).F(f[0]).Run();  // SET_CAR_HEADING
    else if (op == 0x0519)
        Cmd(0x0519).I(nv->handle).I(p[0] ? 1 : 0).Run();  // FREEZE_CAR_POSITION
    else
        return;
    LOGI("The host's mission ran %04X on our car %04X", op, id);
}

void HostSendPolicy(bool force, bool clearWanted = false);

// FrontEndMenuManager.m_bShowSubtitles. Off, PRINT and PRINT_NOW drop a "~z~" line (the mission's dialogue) before
// CMessages sees it, and the partner got no line of the host's mission at all. They run with it on, the lines go to the
// partner and stay off the host's screen (HookMsg).
uint8_t& ShowSubtitles() { return Ref<uint8_t>(0xBA678C); }
bool g_forwardOnly = false;
// host: its story mission took the camera (POINT_CAMERA_AT_*, ATTACH_CAMERA_*, SET_CAMERA_BEHIND/IN_FRONT_OF_CHAR),
// until the camera looks at the player again. Doors, stunt jumps and RC toys take it too, but not by a mission's command.
bool g_scriptCam = false;
bool CameraTake(uint16_t op) { return op == 0x0158 || op == 0x0159 || op == 0x0160 || (op >= 0x0679 && op <= 0x067E) || op == 0x0943 || op == 0x0944; }
bool DialogueLine(const char* t) { return t && t[0] == '~' && t[1] == 'z' && t[2] == '~'; }

// The partner's crate counts as the host's player's (Objects: the partner carries the copy, the real one goes in its
// copy's hands): IS_CHAR_HOLDING_OBJECT and LOCATE_CHAR_ON_FOOT_OBJECT_3D of the mission's player say yes for it, and in
// "Home Invasion" the truck takes it from the partner (its LOCATE_CHAR_ON_FOOT_3D at the truck and the enter button that
// follows). The command's own answer is replaced through the game's own way of setting it (CRunningScript::
// UpdateCompareFlag, after its condition state was put back), so NOT and the AND/OR chains come out as for its own.
int32_t MissionLocal(int n) { return Ref<int32_t>(0xA48960 + (uintptr_t)n * 4); }  // CTheScripts::LocalVariablesForCurrentMission
void* g_crateAtTruck = nullptr;  // the mission whose truck check just passed for the partner
// GUNS1's LOCATE_CHAR_ON_FOOT_3D $PLAYER_ACTOR 154@ 155@ 24.0 1.5 1.5 2.0 0: the place to load a crate
const uint8_t kGunsTruck[] = {0xFF, 0x00, 0x02, 0x0C, 0x00, 0x03, 0x9A, 0x00, 0x03, 0x9B, 0x00, 0x06, 0x00, 0x00, 0xC0, 0x41, 0x06,
                              0x00, 0x00, 0xC0, 0x3F, 0x06, 0x00, 0x00, 0xC0, 0x3F, 0x06, 0x00, 0x00, 0x00, 0x40, 0x04, 0x00};
void SendHint(bool on, const char* key);  // below
int CrateAnswer(void* script, uint16_t op, const uint8_t* ip) {
    const auto* p = (const int32_t*)0xA43C78;  // CTheScripts::ScriptParams
    int32_t cj = Ref<int32_t>(addr::ScriptSpace + 12);  // $PLAYER_ACTOR
    if ((op == 0x0737 || op == 0x0475) && p[0] == cj && PartnerHolds(GetObject(p[1]))) return 1;
    std::string name(Field<char[8]>(script, 0x08), strnlen(Field<char[8]>(script, 0x08), 8));
    if (op == 0x00FF && p[0] == cj && name == "guns1" && !memcmp(ip, kGunsTruck, sizeof kGunsTruck)) {
        int crate = MissionLocal(36);  // 36@: the crate carried, 60@: one is
        void* holder = nullptr;
        if (MissionLocal(60) != 1 || crate < 0 || crate > 6 || !PartnerHolds(GetObject(MissionLocal(117 + crate)), &holder))
            return -1;
        float f[6];
        memcpy(f, &p[1], sizeof f);  // x, y, z, the box's half sizes
        CVector at = ((CPed*)holder)->GetPosition();
        bool in = std::fabs(at.x - f[0]) <= f[3] && std::fabs(at.y - f[1]) <= f[4] && std::fabs(at.z - f[2]) <= f[5];
        g_crateAtTruck = in ? script : nullptr;
        static uint64_t hintMs = 0;
        if (in && NowMs() - hintMs > 2500) hintMs = NowMs(), SendHint(true, "RYD1_30");  // "press ... to put the box in the van"
        return in;  // only the one who carries it loads it
    }
    if (op == 0x00E1 && g_crateAtTruck == script && p[0] == 0 && p[1] == 15) {  // IS_BUTTON_PRESSED 0 15, right after
        g_crateAtTruck = nullptr;
        uint64_t enter = 0;
        int crate = MissionLocal(36);
        PartnerHolds(crate >= 0 && crate <= 6 ? GetObject(MissionLocal(117 + crate)) : nullptr, nullptr, &enter);
        return enter && NowMs() - enter < 1000;
    }
    return -1;
}

// What the mission puts on its player the partner wears as well, over its own look, until the mission takes it off or
// ends: the balaclava of "Home Invasion", and the sneak's walk that comes with it (CPlayerPed::ProcessAnimGroups). The
// host compares its player's clothes at each BUILD_PLAYER_MODEL with those before the mission's first clothes command.
constexpr int kClothesKeys = 28;  // CPedClothesDesc: 10 model keys, then 18 texture keys (fat and muscle after them)
uint32_t g_lookBase[kClothesKeys];  // host: before the mission dressed its player
bool g_lookBaseSet = false;
uint32_t g_ownKeys[kClothesKeys], g_wantKeys[kClothesKeys];  // partner: its own look, and what it wears now
bool g_ownKeysSet = false, g_lookPending = false;

void SendMissionLook() {
    const auto* now = (const uint32_t*)FindPlayerPed(0)->ClothesDesc();
    std::vector<std::pair<uint8_t, uint32_t>> changed;
    for (int i = 0; i < kClothesKeys; ++i)
        if (now[i] != g_lookBase[i]) changed.push_back({(uint8_t)i, now[i]});
    Writer w(proto::G_MISSION_LOOK);
    w.U8((uint8_t)changed.size());
    for (auto [i, key] : changed) w.U8(i), w.U32(key);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("The mission's clothes: %zu keys changed, sent", changed.size());
}

void ApplyMissionLook(bool evenDriving = false) {
    CPlayerPed* me = FindPlayerPed(0);
    g_lookPending = !evenDriving && me->PedState() == 50;  // PEDSTATE_DRIVING: rebuilt on foot
    if (g_lookPending) return;
    memcpy(me->ClothesDesc(), g_wantKeys, sizeof g_wantKeys);
    Call<0x5A82C0>(me, false);  // CClothes::RebuildPlayer
}

void RestoreOwnLook(bool evenDriving = false) {
    if (!g_ownKeysSet || !core::IsPlayerReady()) return;
    memcpy(g_wantKeys, g_ownKeys, sizeof g_wantKeys);
    g_ownKeysSet = false;
    ApplyMissionLook(evenDriving);
    LOGI("The mission's clothes are off, our own look again");
}

// Joined a burglary: the balaclava its owner's BURGJB puts on its own player, worn over our own look as a host's mission
// dresses us (28.10: "не надевается маска"), and our own look again after it
bool g_burglaryDressed = false;
void BurglaryLook(bool on) {
    if (on == g_burglaryDressed || !core::IsPlayerReady()) return;
    g_burglaryDressed = on;
    // off at once, in the van too, as BURGJB takes its own player's off (GIVE_PLAYER_CLOTHES, BUILD_PLAYER_MODEL): put off
    // till the player was on foot, it stayed on the one who joined after the burglary was called off (29.10)
    if (!on) return RestoreOwnLook(true);
    if (!g_ownKeysSet) memcpy(g_ownKeys, FindPlayerPed(0)->ClothesDesc(), sizeof g_ownKeys), g_ownKeysSet = true;
    Cmd(0x087B).I(0).S("BALACLAVA").S("BALACLAVA").I(17).Run();  // GIVE_PLAYER_CLOTHES_OUTSIDE_SHOP, as BURGJB does
    Cmd(0x070D).I(0).Run();                                       // BUILD_PLAYER_MODEL
    LOGI("Joint burglary: the balaclava on, over our own look");
}

void HandleMissionLook(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || n > kClothesKeys) return;
    uint32_t want[kClothesKeys];
    memcpy(want, g_ownKeysSet ? g_ownKeys : (const uint32_t*)FindPlayerPed(0)->ClothesDesc(), sizeof want);
    for (int k = 0; k < n; ++k) {
        uint8_t i = r.U8();
        uint32_t key = r.U32();
        if (!r.Ok() || i >= kClothesKeys) return;
        want[i] = key;
    }
    if (!n) return RestoreOwnLook();
    if (!g_ownKeysSet) memcpy(g_ownKeys, FindPlayerPed(0)->ClothesDesc(), sizeof g_ownKeys), g_ownKeysSet = true;
    memcpy(g_wantKeys, want, sizeof want);
    ApplyMissionLook();
    LOGI("The host's mission dresses us: %u keys", n);
}

// A scene's people move as in the host's game: the mission's animation files are in the partner's memory before its
// scenes play them (a gesture of a second was over before its file came in, and the copy stood still), and the faces
// talk with the mission's lines (START_CHAR_FACIAL_TALK: lips that moved only in the host's game).
std::set<int> g_missionIfps;  // partner: the animation files the host's mission keeps, kept here too

// a script command's text parameter at p (its type byte first)
std::string ScriptText(const uint8_t* p) {
    if (p[0] == 0x09) return std::string((const char*)p + 1, strnlen((const char*)p + 1, 8));    // 8 characters
    if (p[0] == 0x0F) return std::string((const char*)p + 1, strnlen((const char*)p + 1, 16));   // 16 characters
    if (p[0] == 0x0E) return std::string((const char*)p + 2, strnlen((const char*)p + 2, p[1]));  // its length first
    return std::string();
}

void SendMissionIfp(bool on, const std::string& name) {
    if (name.empty()) return;
    Writer w(proto::G_MISSION_IFP);
    w.U8(on);
    w.Str(name, 32);
    Net().Send(proto::CH_RELIABLE, w);
}

void HandleMissionIfp(uint8_t from, Reader& r) {
    bool on = r.U8() != 0;
    std::string name = r.Str(32);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || name.empty()) return;
    int idx = Call<0x4D3990, int>(name.c_str());  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return;
    if (on && g_missionIfps.insert(idx).second) {
        RequestModel(anim::kIfpBase + idx, 0x0C);  // STREAMING_MISSION_REQUIRED | KEEP_IN_MEMORY, as REQUEST_ANIMATION
        LOGD("The host's mission's animations %s: loading here too", name.c_str());
    } else if (!on && g_missionIfps.erase(idx)) {
        Call<0x409C90>(anim::kIfpBase + idx);  // CStreaming::SetMissionDoesntRequireModel
    }
}

void ReleaseMissionIfps() {
    for (int idx : g_missionIfps) Call<0x409C90>(anim::kIfpBase + idx);  // CStreaming::SetMissionDoesntRequireModel
    g_missionIfps.clear();
}

// a person of the host's game as the partner's game knows it: 3 a player by its id, 2 a shared ped by its net id
bool PersonRef(int handle, uint8_t& kind, uint16_t& id) {
    CPed* p = GetPed(handle);
    if (p && p == FindPlayerPed(0)) kind = 3, id = Net().MyId();
    else if (NetPed* np = p ? Peds::Instance().Find(p) : nullptr) kind = 2, id = np->netId;
    else return false;
    return true;
}
int PersonHandle(uint8_t kind, uint16_t id) {
    if (kind == 2) return Peds::Instance().HandleOf(id);
    if (kind == 3 && id == Net().MyId()) return GetPedRef(FindPlayerPed(0));
    const RemotePlayer* rp = kind == 3 ? Players::Instance().Get((uint8_t)id) : nullptr;
    return rp && rp->ped ? rp->pedHandle : -1;
}

// A person the mission lets through the closest object of a model (the colonel of "Home Invasion" lies in his bed): its
// copy was pushed out of the bed and hung above it. The copy gets the same, once it is here (it may come after).
struct NoCollide {
    uint8_t kind;
    uint16_t id;
    CVector at;
    float radius;
    int32_t model;
    uint8_t state;
    uint64_t until;
};
std::vector<NoCollide> g_noCollide;  // partner: waiting for their copies
void SendNoCollide(const int32_t* p) {
    uint8_t kind;
    uint16_t id;
    if (!PersonRef(p[6], kind, id)) return;
    const auto* f = (const float*)p;
    Writer w(proto::G_PED_NO_COLLIDE);
    w.U8(kind);
    w.U16(id);
    w.Vec({f[0], f[1], f[2]});
    w.F32(f[3]);
    w.I32(ResolvedModel(p[4]));
    w.U8(p[5] ? 1 : 0);
    Net().Send(proto::CH_RELIABLE, w);
}
void HandleNoCollide(uint8_t from, Reader& r) {
    NoCollide n{};
    n.kind = r.U8();
    n.id = r.U16();
    Vec3 v = r.Vec();
    n.radius = r.F32();
    n.model = r.I32();
    n.state = r.U8();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
        !(n.radius > 0.0f && n.radius < 50.0f) || n.model <= 0 || n.model >= 20000)
        return;
    n.at = {v.x, v.y, v.z};
    n.until = NowMs() + 10000;
    g_noCollide.push_back(n);
}
void ApplyNoCollides() {
    for (auto it = g_noCollide.begin(); it != g_noCollide.end();) {
        int handle = PersonHandle(it->kind, it->id);
        if (handle >= 0 && handle != GetPedRef(FindPlayerPed(0))) {
            Cmd(0x0985).F(it->at.x).F(it->at.y).F(it->at.z).F(it->radius).I(it->model).I(it->state).I(handle).Run();
            LOGD("The host's mission's person %04X goes through model %d's object here too", it->id, it->model);
        }
        if (handle >= 0 || NowMs() > it->until) it = g_noCollide.erase(it);
        else ++it;
    }
}

// A person the mission puts on a car (ATTACH_CHAR_TO_CAR: the girl beside CJ in his lowrider at Cesar's meet): the copy
// stood inside the car's copy and was thrown out and up into the air. It goes on the copy the same way, once both
// copies are here, and its own position sync leaves it alone while it is on (Peds).
struct PersonOnCar {
    bool on;
    uint8_t kind;
    uint16_t id, car;
    CVector offset;
    int32_t heading, weapon;
    float range;
    uint64_t until;
};
std::vector<PersonOnCar> g_personOnCar;  // waiting for their copies
bool SendPersonOnCar(bool on, const int32_t* p) {
    uint8_t kind;
    uint16_t id;
    if (!PersonRef(p[0], kind, id)) return false;
    PersonOnCar a{on, kind, id, 0, {}, 0, 0, 0.0f, 0};
    if (on) {
        CVehicle* v = GetVehicle(p[1]);
        NetVehicle* nv = v ? Vehicles::Instance().EnsureNetworked(v) : nullptr;
        if (!nv) return false;
        const auto* f = (const float*)p;
        a.car = nv->netId;
        a.offset = CVector{f[2], f[3], f[4]};
        a.heading = p[5], a.range = f[6], a.weapon = p[7];
    }
    Writer w(proto::G_PED_ATTACH);
    w.U8(on);
    w.U8(a.kind);
    w.U16(a.id);
    w.U16(a.car);
    w.Vec({a.offset.x, a.offset.y, a.offset.z});
    w.I32(a.heading);
    w.F32(a.range);
    w.I32(a.weapon);
    Net().Send(proto::CH_RELIABLE, w);
    return true;
}
void HandlePersonOnCar(uint8_t from, Reader& r) {
    PersonOnCar a{};
    a.on = r.U8() != 0;
    a.kind = r.U8();
    a.id = r.U16();
    a.car = r.U16();
    Vec3 o = r.Vec();
    a.heading = r.I32();
    a.range = r.F32();
    a.weapon = r.I32();
    if (!r.Ok() || from == Net().MyId() || !std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z) ||
        std::fabs(o.x) > 20.0f || std::fabs(o.y) > 20.0f || std::fabs(o.z) > 20.0f || !std::isfinite(a.range) ||
        a.weapon < 0 || a.weapon > 46)
        return;
    a.offset = {o.x, o.y, o.z};
    a.until = NowMs() + 10000;
    // the latest word on that person: a detach must not run before an attach still waiting for the copy
    std::erase_if(g_personOnCar, [&](const PersonOnCar& w) { return w.kind == a.kind && w.id == a.id; });
    g_personOnCar.push_back(a);
}
void ApplyPersonsOnCars() {
    for (auto it = g_personOnCar.begin(); it != g_personOnCar.end();) {
        int ped = PersonHandle(it->kind, it->id);
        CPed* p = ped >= 0 ? GetPed(ped) : nullptr;
        NetVehicle* nv = it->on ? Vehicles::Instance().Find(it->car) : nullptr;
        // on: out of the seat first (the mission took her out of it in the same frame, the copy follows its state)
        bool ready = p && p != FindPlayerPed(0) && (!it->on || (nv && Vehicles::Alive(*nv) && !p->InVehicle()));
        if (ready) {
            // A player's copy goes on as a mission's person does. As a player it took the game's player branch
            // (CPed::AttachPedToEntity), which sets this game's camera to the gunner's (CCamera::SetNewPlayerWeaponMode): the
            // partner had the host's crosshair at the start marker of "The Da Nang Thang" ("Reuniting the Families" too, 28.10)
            int32_t& type = Field<int32_t>(p, 0x598);  // CPed::m_nPedType: 0 and 1 are the players
            int32_t was = type;
            if (it->kind == 3) type = 4;  // PED_TYPE_CIVMALE
            if (it->on)
                Cmd(0x0464).I(ped).I(nv->handle).F(it->offset.x).F(it->offset.y).F(it->offset.z).I(it->heading).F(it->range)
                    .I(it->weapon).Run();  // ATTACH_CHAR_TO_CAR
            else
                Cmd(0x0465).I(ped).Run();  // DETACH_CHAR_FROM_CAR
            type = was;
            LOGD("The host's mission's person %04X %s here too", it->id, it->on ? "on its car" : "off its car");
        }
        if (ready || NowMs() > it->until) it = g_personOnCar.erase(it);
        else ++it;
    }
}

// a scene's head turns (CJ looks at Ryder as Ryder talks): the copies look at the copies
void SendLookAt(const int32_t* p) {
    uint8_t kind, tkind;
    uint16_t id, tid;
    if (!PersonRef(p[0], kind, id) || !PersonRef(p[1], tkind, tid)) return;
    Writer w(proto::G_MISSION_LOOK_AT);
    w.U8(kind);
    w.U16(id);
    w.U8(tkind);
    w.U16(tid);
    w.I32(p[2]);
    Net().Send(proto::CH_RELIABLE, w);
}
void HandleLookAt(uint8_t from, Reader& r) {
    uint8_t kind = r.U8();
    uint16_t id = r.U16();
    uint8_t tkind = r.U8();
    uint16_t tid = r.U16();
    int32_t ms = r.I32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    int who = PersonHandle(kind, id), whom = PersonHandle(tkind, tid);
    if (who < 0 || whom < 0 || who == whom || who == GetPedRef(FindPlayerPed(0))) return;  // our own head is ours
    Cmd(0x05BF).I(who).I(whom).I(std::clamp(ms, -1, 60000)).Run();  // TASK_LOOK_AT_CHAR
}

void SendFace(bool talk, int handle, int ms) {
    uint8_t kind = 0;
    uint16_t id = 0;
    if (!PersonRef(handle, kind, id)) return;
    Writer w(proto::G_MISSION_FACE);
    w.U8(kind);
    w.U16(id);
    w.I32(talk ? std::max(ms, 1) : 0);
    Net().Send(proto::CH_RELIABLE, w);
}

void HandleMissionFace(uint8_t from, Reader& r) {
    uint8_t kind = r.U8();
    uint16_t id = r.U16();
    int32_t ms = r.I32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    int handle = PersonHandle(kind, id);
    if (handle < 0 || handle == GetPedRef(FindPlayerPed(0))) return;
    if (ms > 0) Cmd(0x0673).I(handle).I(std::min(ms, 60000)).Run();  // START_CHAR_FACIAL_TALK
    else Cmd(0x0674).I(handle).Run();                                // STOP_CHAR_FACIAL_TALK
}

// The mission's doors of its cars (the van's back doors of "Home Invasion" open for the crates): the vehicle state
// carries no door, the partner saw them shut. Its door commands run on the copy too, and again on a new copy: a car
// in no player's area stops being networked, and comes back under a new id (the van, while the host was in the house).
bool CarDoorCommand(uint16_t op) { return op == 0x0657 || op == 0x0508 || op == 0x0689 || op == 0x068A || op == 0x08A6; }
struct DoorCommand {
    uint16_t op;
    int32_t door, more;
};
struct CarDoors {
    std::vector<DoorCommand> commands;  // since the last CLOSE_ALL_CAR_DOORS, the latest of each door
    uint16_t sentTo = 0;                // the net id they went to
};
std::map<int, CarDoors> g_carDoors;  // host: by the car's script handle, for its story mission's time

void SendCarDoors(int handle, CarDoors& cd) {
    CVehicle* v = GetVehicle(handle);
    NetVehicle* nv = v ? Vehicles::Instance().Find(v) : nullptr;
    if (!nv) return;
    for (const DoorCommand& c : cd.commands) {
        Writer w(proto::G_CAR_DOOR);
        w.U16(nv->netId);
        w.U16(c.op);
        w.I32(c.door);
        w.I32(c.more);
        Net().Send(proto::CH_RELIABLE, w);
    }
    cd.sentTo = nv->netId;
}
void SendCarDoor(uint16_t op, const int32_t* p) {
    CarDoors& cd = g_carDoors[p[0]];
    DoorCommand c{op, op == 0x0508 ? 0 : p[1], op == 0x0689 || op == 0x08A6 ? p[2] : 0};
    if (op == 0x0508) cd.commands.clear();
    else cd.commands.erase(std::remove_if(cd.commands.begin(), cd.commands.end(), [&](const DoorCommand& o) { return o.door == c.door; }),
                           cd.commands.end());
    cd.commands.push_back(c);
    SendCarDoors(p[0], cd);
}
// host, every second: a car whose copy came back under another id gets its doors again
void ResendCarDoors() {
    for (auto it = g_carDoors.begin(); it != g_carDoors.end();) {
        CVehicle* v = GetVehicle(it->first);
        if (!v) {
            it = g_carDoors.erase(it);
            continue;
        }
        NetVehicle* nv = Vehicles::Instance().Find(v);
        if (nv && nv->netId != it->second.sentTo) SendCarDoors(it->first, it->second);
        ++it;
    }
}
void HandleCarDoor(uint8_t from, Reader& r) {
    uint16_t id = r.U16(), op = r.U16();
    int32_t door = r.I32(), more = r.I32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !CarDoorCommand(op) || door < 0 || door > 5)
        return;
    NetVehicle* nv = Vehicles::Instance().Find(id);
    if (!nv || !Vehicles::Alive(*nv)) return;
    LOGD("The host's mission's door command %04X (door %d) on its car %04X", op, door, id);
    float f;
    memcpy(&f, &more, 4);
    if (op == 0x0508) Cmd(op).I(nv->handle).Run();
    else if (op == 0x0689) Cmd(op).I(nv->handle).I(door).I(more).Run();
    else if (op == 0x08A6) Cmd(op).I(nv->handle).I(door).F(std::isfinite(f) ? f : 1.0f).Run();
    else Cmd(op).I(nv->handle).I(door).Run();
}

// "Home Invasion"'s noise meter ($26820, GUNS1_5) is each player's own. The partner heaps its own up as GUNS1 does for its
// player: the sound level where it stands, x1.6, what is over 20 adds up, 1% fades a frame. Full, it wakes the colonel in
// the host's game too: right after the mission set its meter, the host's game sets it full for the check that follows.
// The partner's meter is its own counter, shown while it is in the house (the host's was shown while the host was there).
constexpr uint32_t kNoiseVar = 26820;
bool g_noiseMission = false;  // partner: the host's mission showed its noise meter: the house's part runs
float g_ownNoise = 0.0f;
uint64_t g_loudSentMs = 0, g_partnerLoudMs = 0;
// our meter in COnscreenTimer's counters: added in a free one, or taken away
void ShowOwnNoise(bool on) { ShowOwnCounter(kNoiseVar, "GUNS1_5", on); }
// our counter's entry as it is now (its display state), to put back over the host's block in the same slot
bool TakeOwnEntry(uint32_t varOffset, uint8_t* entry, int& slot) {
    auto* t = (uint8_t*)kOnscreenTimer;
    for (int i = 0; i < 4; ++i)
        if (*(uint32_t*)(t + 0x40 + i * 0x44) == varOffset) {
            memcpy(entry, t + 0x40 + i * 0x44, 0x44);
            slot = i;
            return true;
        }
    return false;
}
void UpdateOwnNoise() {
    CPlayerPed* me = FindPlayerPed(0);
    bool inside = g_noiseMission && me->AreaCode() != 0;
    ShowOwnNoise(inside);
    if (!inside) {
        g_ownNoise = 0.0f;
        return;
    }
    float level = 0.0f;
    CVector p = me->GetPosition();
    Cmd(0x0855).I(-1).F(p.x).F(p.y).F(p.z).Out(&level).Run();  // GET_SOUND_LEVEL_AT_COORDS
    level = std::isfinite(level) ? level * 1.6f : 0.0f;
    if (level > 20.0f) g_ownNoise += level - 20.0f;
    g_ownNoise = std::min(g_ownNoise * 0.99f, 1000.0f);
    int v = std::clamp((int)g_ownNoise, 0, 100);
    Ref<int32_t>(addr::ScriptSpace + kNoiseVar) = v;
    if (v >= 100 && NowMs() - g_loudSentMs > 2000) {
        g_loudSentMs = NowMs();
        Writer w(proto::G_NOISE);
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Our noise meter is full: the colonel hears us");
    }
}

// A help of the host's mission meant for the partner's own doing (the crate it carries is at the van's back: RYD1_30)
void SendHint(bool on, const char* key) {
    Writer w(proto::G_MISSION_HINT);
    w.U8(on);
    w.Str(key, 8);
    Net().Send(proto::CH_RELIABLE, w);
}
void HandleHint(uint8_t from, Reader& r) {
    bool on = r.U8() != 0;
    std::string key = r.Str(8);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || key.empty()) return;
    const char* text = CallMethod<0x6A0050, const char*>((void*)0xC1B340, key.c_str());  // CText::Get (TheText)
    if (on && text && *text) Cmd(0x03E5).S8(key.c_str()).Run();  // PRINT_HELP: sent again while it holds
    else if (!on) Cmd(0x03E6).Run();                    // CLEAR_HELP
}

// The threads the host's story mission starts (START_NEW_SCRIPT) and theirs: its helpers, as its own. "Cesar
// Vialpando" says its lines in AUDIOL (a main.scm thread loads, attaches and plays each one, its face talks): the scenes
// on the engine had subtitles and no voices for the partner. Each by its CRunningScript and the code it began at, as one
// ended and its slot taken by another thread is no more the mission's.
std::vector<std::pair<void*, const uint8_t*>> g_missionThreads;
void NoteMissionThread() {
    void* s = Ref<void*>(0xA8B42C);  // CTheScripts::pActiveScripts: the new thread heads the list
    if (!s) return;
    const uint8_t* base = Field<const uint8_t*>(s, 0x10);  // m_pBaseIP
    for (auto& t : g_missionThreads)
        if (t.first == s) return void(t.second = base);  // a slot of the pool (96 threads) used again: one line each
    g_missionThreads.push_back({s, base});
}

void SendText(uint8_t kind, const char* text, uint32_t time, uint16_t flag, const int* nums, uint8_t bools);  // below

// main.scm's APGATE slides an airport gate open for its own player alone: within 10 m (with flying skill 180 from outside)
// or at its inner side (4.5 m), and shuts it once that player is 20 m off. Each game opened the gates for its own player,
// and the other player's copy drove through a shut one (MozzisRus, 29.10: open for the host, shut for the partner, or the
// other way round). These checks take the other player too; the outer side's (3 m, the licence help) stays our player's.
// shortcut: only where APCHECK serves the gate in this game (our player within 65 m of it); counting the other player there
// too would tie APCHECK to one site while the two are at two airports
int AirportGateAnswer(void* script, uint16_t op) {
    if ((op != 0x0471 && op != 0x00FE) || _strnicmp(Field<char[8]>(script, 0x08), "apgate", 8)) return -1;
    const auto* p = (const int32_t*)0xA43C78;  // CTheScripts::ScriptParams, as the command read them
    const auto* f = (const float*)p;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || p[0] != GetPedRef(me) || (op == 0x00FE && f[4] != 4.5f)) return -1;
    // LOCATE_CHAR_ANY_MEANS_OBJECT_2D $PLAYER gate 10 10 (20 20 to shut), LOCATE_CHAR_ANY_MEANS_3D $PLAYER inner side 4.5
    CObject* gate = op == 0x0471 ? GetObject(p[1]) : nullptr;
    if (op == 0x0471 && !gate) return -1;
    CVector at = gate ? gate->GetPosition() : CVector{f[1], f[2], f[3]};
    float rx = gate ? f[2] : f[4], ry = gate ? f[3] : f[5], rz = gate ? 0.0f : f[6];
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.ped || !rp.hasState || rp.ped->AreaCode() != 0) continue;
        CVector d = (rp.ped->InVehicle() ? rp.ped->Vehicle()->GetPosition() : rp.ped->GetPosition()) - at;  // as the game
        if (std::fabs(d.x) < rx && std::fabs(d.y) < ry && (rz <= 0.0f || std::fabs(d.z) < rz)) return 1;
    }
    return -1;
}

// Every command of every script, with the handler CRunningScript::Process took for it from the table (its opcode / 100),
// as Process calls it: the opcode read, the instruction pointer past it, the NOT flag set.
using CommandHandlerFn = int8_t(__thiscall*)(void*, int);
int8_t ScriptCommand(void* script, void* handler, int command) {
    const uint8_t* ip = Field<uint8_t*>(script, 0x14) - 2;  // CRunningScript::m_pCurrentIP: past the command's opcode
    uint16_t op = (uint16_t)command;
    bool forwardOnly = (op == 0x00BB || op == 0x00BC) && !ShowSubtitles() && Net().Online() && Net().IsHost() && Replicating();
    if (forwardOnly) ShowSubtitles() = 1, g_forwardOnly = true;
    bool dressing = (op == 0x087B || op == 0x0784 || op == 0x070D) && Net().Online() && Net().IsHost() && Replicating() &&
                    StoryMission(script);  // GIVE_PLAYER_CLOTHES_OUTSIDE_SHOP, GIVE_PLAYER_CLOTHES, BUILD_PLAYER_MODEL
    if (dressing && op != 0x070D && !g_lookBaseSet)
        memcpy(g_lookBase, FindPlayerPed(0)->ClothesDesc(), sizeof g_lookBase), g_lookBaseSet = true;
    uint8_t cond = Field<uint8_t>(script, 0xC5);     // CRunningScript::m_bCondResult
    uint16_t andOr = Field<uint16_t>(script, 0xD0);  // m_nLogicalOp
    // TRUCK's ADD_BLIP_FOR_COORD of the drop into 76@: with the trailer off the cab as its player got in, its second
    // stage starts with the trailer lost and hooking it up adds the blip once more. The first one stayed on both maps for
    // good (27.10: two players knock the trailer loose, the other's truck copy stands at the same spot). Taken first.
    if (op == 0x018A && Field<uint8_t>(script, 0xDC) && !_strnicmp(Field<char[8]>(script, 0x08), "truck", 8))
        if (int old = MissionLocal(76); BlipAlive(old) && ((Ref<uint8_t>(Trace(old & 0xFFFF) + 0x26) >> 2) & 0xF) == BLIP_COORD) {
            Call<0x587CE0>(old);  // CRadar::ClearBlip
            LOGI("Trucking: its drop's blip was set again over a live one: the old one removed");
        }
    // a joint job of ours: the other player's ambulance or the other player itself for this one command (Jobs.cpp)
    int32_t jobWas = 0;
    int32_t* jobVar = Net().Online() && Field<uint8_t>(script, 0xDC) ? jobs::BeforeCommand(script, op, ip, jobWas) : nullptr;
    int8_t r = ((CommandHandlerFn)handler)(script, command);
    if (jobVar) *jobVar = jobWas;
    if (op == 0x004F && Net().Online() && Net().IsHost() && MissionThread(script)) NoteMissionThread();  // START_NEW_SCRIPT
    // PLAY_MISSION_PASSED_TUNE: the passed mission's jingle played in the host's game only, the partner saw the text in
    // silence
    if (op == 0x0394 && Net().Online()) SendText(TXT_TUNE, nullptr, 0, (uint16_t)Ref<int32_t>(0xA43C78), nullptr, 0);
    if ((op == 0x0737 || op == 0x0475 || op == 0x00FF || op == 0x00E1) && Net().Online() && Net().IsHost() && Replicating() &&
        StoryMission(script))
        if (int answer = CrateAnswer(script, op, ip); answer >= 0) {
            Field<uint8_t>(script, 0xC5) = cond;
            Field<uint16_t>(script, 0xD0) = andOr;
            CallMethod<0x4859D0>(script, (uint8_t)answer);  // CRunningScript::UpdateCompareFlag
        }
    if (forwardOnly) ShowSubtitles() = 0, g_forwardOnly = false;
    if (dressing && op == 0x070D && g_lookBaseSet) SendMissionLook();
    if (CarDoorCommand(op) && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script))
        SendCarDoor(op, (const int32_t*)0xA43C78);  // CTheScripts::ScriptParams
    // CSET_VAR_INT_TO_LVAR_FLOAT $26820 167@: "Home Invasion" just set its noise meter; the partner's full one counts
    if (op == 0x0090 && ip[2] == 0x02 && *(const uint16_t*)(ip + 3) == kNoiseVar && NowMs() - g_partnerLoudMs < 1500 &&
        Net().Online() && Net().IsHost() && StoryMission(script))
        Ref<int32_t>(addr::ScriptSpace + kNoiseVar) = 100;
    if ((op == 0x04ED || op == 0x04EF || op == 0x0673 || op == 0x0674) && Net().Online() && Net().IsHost() && Replicating() &&
        MissionThread(script)) {
        const auto* p = (const int32_t*)0xA43C78;  // CTheScripts::ScriptParams
        if (op == 0x04ED || op == 0x04EF) SendMissionIfp(op == 0x04ED, ScriptText(ip + 2));  // REQUEST_ANIMATION, REMOVE_ANIMATION
        else SendFace(op == 0x0673, p[0], p[1]);
    }
    if (op == 0x05BF && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script))
        SendLookAt((const int32_t*)0xA43C78);  // TASK_LOOK_AT_CHAR, as the command read it
    if (op == 0x0985 && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script))
        SendNoCollide((const int32_t*)0xA43C78);  // SET_CHAR_USES_COLLISION_CLOSEST_OBJECT_OF_TYPE
    if ((op == 0x0464 || op == 0x0465) && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script))
        SendPersonOnCar(op == 0x0464, (const int32_t*)0xA43C78);  // ATTACH_CHAR_TO_CAR, DETACH_CHAR_FROM_CAR
    // CJ made a vehicle's gunner and back: the gun is the vehicle's (CPed::AttachPedToEntity gives it, the detach takes it
    // back), not one the mission gives to keep. The partner walked about with the host's minigun (28.10).
    if ((op == 0x0464 || op == 0x0465) && g_tickWeapons && *(const int32_t*)0xA43C78 == GetPedRef(FindPlayerPed(0)))
        g_tickWeapons->Take(FindPlayerPed(0));
    if ((op == 0x00AB || op == 0x0175 || op == 0x0519) && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script))
        SendCarCommand(op, (const int32_t*)0xA43C78);  // CTheScripts::ScriptParams
    if (CameraTake(op) && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script)) g_scriptCam = true;
    if (Net().Online() && Net().IsHost()) minigames::AfterCommand(script, op, ip, Replicating() && StoryMission(script));
    if (Net().Online()) contest::AfterCommand(script, op, ip);
    if (Net().Online() && Net().IsHost()) courier::AfterCommand(script, op);
    if (JointValet(script)) jobs::ValetAfter(op, ip);
    if (op == 0x024F && Net().Online() && SideOwning() && Field<uint8_t>(script, 0xDC)) NoteCorona();  // DRAW_CORONA
    // POOL2 at a table for two: the second pad's answers are the other player's, and the second player's own table (its
    // attractor, no mission) offers it no game (Pool.cpp); a joint job of ours: the other player counts as its second
    // player (Jobs.cpp)
    if (Net().Online()) {
        bool mission = Field<uint8_t>(script, 0xDC);
        int answer = pool::Answer(script, op);
        if (answer < 0 && mission) answer = jobs::Answer(script, op);
        if (answer < 0 && !mission) answer = courier::Answer(script, op);
        if (answer < 0 && !mission) answer = AirportGateAnswer(script, op);
        if (answer < 0 && JointValet(script)) answer = jobs::ValetAnswer(op);
        // in the other's valet shift ours stays off: VALET_L and VALET take our player for out of the hotel's grounds
        if (answer < 0 && op == 0x00A3 && SideJoined() && g_side.joined == "valet" && !_strnicmp(Field<char[8]>(script, 0x08), "valet", 5))
            answer = 0;
        if (answer >= 0) {
            Field<uint8_t>(script, 0xC5) = cond;
            Field<uint16_t>(script, 0xD0) = andOr;
            CallMethod<0x4859D0>(script, (uint8_t)answer);  // CRunningScript::UpdateCompareFlag
        } else if (mission) {
            pool::AfterCommand(script, op, ip);
        }
    }
    // CLEAR_WANTED_LEVEL of the mission's player, SET_INT_STAT of the city (the end of "The Green Sabre")
    if ((op == 0x0110 || (op == 0x0629 && *(const int32_t*)0xA43C78 == 181)) && Net().Online() && Net().IsHost() && Replicating() &&
        StoryMission(script))
        HostSendPolicy(true, op == 0x0110);
    if ((op == 0x0363 || op == 0x088D || op == 0x03B6) && Net().Online() && Net().IsHost() && Replicating() && StoryMission(script)) {
        const auto* p = (const float*)0xA43C78;  // CTheScripts::ScriptParams, as the command read them
        MapEdit e{op, {p[0], p[1], p[2]}, p[3], ResolvedModel(((const int32_t*)p)[4]), ((const int32_t*)p)[5]};
        if (op == 0x03B6) e.b = ResolvedModel(e.b);
        g_mapEdits.push_back(e);
        SendMapEdit(e);
        LOGI("The mission's map edit %04X at %.1f,%.1f (model %d, %d) goes to the partners", op, e.at.x, e.at.y, e.a, e.b);
    }
    return r;
}

void HandleMapEdit(uint8_t from, Reader& r) {
    uint16_t op = r.U16();
    Vec3 v = r.Vec();
    float radius = r.F32();
    int32_t a = r.I32(), b = r.I32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() ||
        (op != 0x0363 && op != 0x088D && op != 0x03B6) || !(radius > 0.0f && radius < 200.0f) || a <= 0 || a >= 20000 ||
        (op == 0x03B6 && (b <= 0 || b >= 20000)))
        return;
    // one edit of the same thing at the same spot replaces the older (the gate shut again)
    CVector at{v.x, v.y, v.z};
    for (auto it = g_partnerEdits.begin(); it != g_partnerEdits.end();)
        it = it->e.op == op && it->e.a == a && (it->e.at - at).Length() < 1.0f ? g_partnerEdits.erase(it) : std::next(it);
    g_partnerEdits.push_back({MapEdit{op, at, radius, a, b}, false});
    LOGI("The host's mission edits the map: %04X at %.1f,%.1f (model %d, %d)", op, v.x, v.y, a, b);
}

void ApplyPartnerEdits() {
    CPlayerPed* me = FindPlayerPed(0);
    for (PartnerEdit& pe : g_partnerEdits) {
        float d = (me->GetPosition() - pe.e.at).Length();
        if (pe.applied && d > 300.0f) pe.applied = false;  // streamed out: it comes back unedited
        if (pe.applied || d > 150.0f) continue;
        pe.applied = true;
        Cmd(pe.e.op).F(pe.e.at.x).F(pe.e.at.y).F(pe.e.at.z).F(pe.e.radius).I(pe.e.a).I(pe.e.b).Run();
        LOGI("The host's map edit %04X at %.1f,%.1f done here", pe.e.op, pe.e.at.x, pe.e.at.y);
    }
}

// HAS_CHAR_SPOTTED_CHAR and HAS_CHAR_SPOTTED_CHAR_IN_FRONT (CPed::OurPedCanSeeThisEntity) ask about the mission's
// player alone: the guards, the dealers and the targets of the host's mission never noticed the partner walking past
// them. Its copy counts as the host's other half.
using CanSeeFn = bool(__fastcall*)(CPed*, void*, CEntity*, bool);
CanSeeFn g_origCanSee = nullptr;
bool __fastcall HookSpottedChar(CPed* ped, void* edx, CEntity* target, bool spotted) {
    bool seen = g_origCanSee(ped, edx, target, spotted);
    if (seen || !Net().Online() || !Net().IsHost() || target != (CEntity*)FindPlayerPed(0) || !Replicating() ||
        !StoryMission(g_currentScript))
        return seen;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.ped->AreaCode() == ped->AreaCode() && g_origCanSee(ped, edx, rp.ped, spotted)) {
            LOGD("The mission's ped saw player %d, not the host: spotted all the same", id);
            return true;
        }
    return false;
}

// The host's mission brings in or takes out a part of the map (REQUEST_IPL, REMOVE_IPL, REMOVE_IPL_DISCREETLY): the weed
// field of "Are You Going To San Fierro?" (TRUTHSFARM), the crack lab of "Yay Ka-Boom-Boom", Carter's block in the
// finale. In the partner's game the field stood bare and its fire burnt nothing there; the same part goes there (G_IPL),
// again after a resync. The script's three calls with the part's slot are taken (0x46B6ED, 0x46B715, 0x46B73D).
struct IplEdit {
    uint8_t op;  // 0 in (CIplStore::RequestIplAndIgnore), 1 out (RemoveIplAndIgnore), 2 out when far (RemoveIplWhenFarAway)
    std::string name;
};
std::vector<IplEdit> g_iplEdits;  // host: this mission's, for a partner that joins or reloads
using IplFn = void(__cdecl*)(int);
IplFn g_origIpl[3] = {};

void SendIpl(const IplEdit& e) {
    Writer w(proto::G_IPL);
    w.U8(e.op);
    w.Str(e.name, 32);
    Net().Send(proto::CH_RELIABLE, w);
}

template <int Op>
void __cdecl HookScriptIpl(int slot) {
    g_origIpl[Op](slot);
    if (!Net().Online() || !Net().IsHost() || !Replicating() || !StoryMission(g_currentScript)) return;
    const char* name = Call<0x404A60, const char*>(slot);  // CIplStore::GetIplName
    if (!name || !*name) return;
    g_iplEdits.push_back({(uint8_t)Op, name});
    SendIpl(g_iplEdits.back());
    LOGI("The mission's map part %s %s: sent", name, Op == 0 ? "comes in" : "goes");
}

void HandleIpl(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    std::string name = r.Str(32);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || op > 2 ||
        name.empty())
        return;
    int slot = Call<0x404AC0, int>(name.c_str());  // CIplStore::FindIplSlot
    if (slot < 0) return LOGW("The host's map part %s is not in this game", name.c_str());
    if (op == 0) Call<0x405850>(slot);       // CIplStore::RequestIplAndIgnore
    else if (op == 1) Call<0x405890>(slot);  // CIplStore::RemoveIplAndIgnore
    else Call<0x4058D0>(slot);               // CIplStore::RemoveIplWhenFarAway
    LOGI("The host's map part %s %s here", name.c_str(), op == 0 ? "comes in" : "goes");
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

// The police rules of the host's mission (no stars in the hood's gang fights, the police kept off in its scenes:
// SET_MAX_WANTED_LEVEL, SET_WANTED_MULTIPLIER, SET_POLICE_IGNORE_PLAYER, SET_EVERYONE_IGNORE_PLAYER) hold in the
// partner's game too: it got the stars, and its cops shot at the host's copy there for real.
void* PlayerWanted() { return Call<0x56E230, void*>(-1); }  // FindPlayerWanted
// STAT_CITY_UNLOCKED (CStats::StatTypesInt[181 - 120]): the cities of the story so far. The forbidden territories give 4
// stars past it (CGameLogic::SetPlayerWantedLevelForForbiddenTerritories), and "The Green Sabre" raises it at its end
int32_t& CityStat() { return Ref<int32_t>(0xB79000 + (181 - 120) * 4); }
uint8_t g_sentMax = 0xFF, g_sentBackOff = 0xFF, g_sentCity = 0xFF;
float g_sentMult = -1.0f;
bool g_policyApplied = false;  // partner: the host's rules are on ours
struct Policy {
    uint8_t max = 6, backOff = 0;
    float mult = 1.0f;
    // SET_PED_DENSITY_MULTIPLIER, SET_CAR_DENSITY_MULTIPLIER: "Home Invasion" empties the house (0), and the partner's game
    // filled it with its own people while the host carried a crate outside
    float peds = 1.0f, cars = 1.0f;
} g_ownPolicy;  // partner: its rules before the host's (the early game's world allows 4 stars, not 6)
float& PedDensity() { return Ref<float>(0x8D2530); }  // CPopulation::PedDensityMultiplier
float& CarDensity() { return Ref<float>(0x8A5B20); }  // CCarCtrl::CarDensityMultiplier
float g_sentPeds = -1.0f, g_sentCars = -1.0f;

// clearWanted: the mission wiped its player's stars (CLEAR_WANTED_LEVEL), the partner's go too
void HostSendPolicy(bool force, bool clearWanted) {
    void* wanted = PlayerWanted();
    if (!wanted) return;
    uint8_t max = (uint8_t)Ref<int32_t>(0x8CDEE4), backOff = Field<uint8_t>(wanted, 0x1E) & 5;  // MaximumWantedLevel; police, everybody
    float mult = Field<float>(wanted, 0x14);                                                      // CWanted::m_Multiplier
    uint8_t city = (uint8_t)std::clamp(CityStat(), 0, 255);
    float peds = PedDensity(), cars = CarDensity();
    if (!force && !clearWanted && max == g_sentMax && backOff == g_sentBackOff && mult == g_sentMult && city == g_sentCity &&
        peds == g_sentPeds && cars == g_sentCars)
        return;
    g_sentMax = max, g_sentBackOff = backOff, g_sentMult = mult, g_sentCity = city, g_sentPeds = peds, g_sentCars = cars;
    Writer w(proto::G_MISSION_POLICY);
    w.U8(max);
    w.U8(backOff);
    w.F32(mult);
    w.U8(city);
    w.U8(clearWanted);
    w.F32(peds);
    w.F32(cars);
    Net().Send(proto::CH_RELIABLE, w);
}

void ApplyPolicy(const Policy& p) {
    void* wanted = PlayerWanted();
    if (!wanted) return;
    Call<0x561E70>((int)p.max);  // CWanted::SetMaximumWantedLevel (the chaos cap with it)
    Field<float>(wanted, 0x14) = p.mult;
    Field<uint8_t>(wanted, 0x1E) = (uint8_t)((Field<uint8_t>(wanted, 0x1E) & ~5) | (p.backOff & 5));
    PedDensity() = p.peds;
    CarDensity() = p.cars;
    LOGI("Host mission's police rules: max wanted %u, multiplier %.2f, back off %u, density of people %.2f and cars %.2f", p.max,
         p.mult, p.backOff, p.peds, p.cars);
}

void HandlePolicy(uint8_t from, Reader& r) {
    uint8_t max = r.U8(), backOff = r.U8();
    float mult = r.F32();
    uint8_t city = r.U8();
    bool clearWanted = r.U8() != 0;
    float peds = r.F32(), cars = r.F32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || max > 6 ||
        !(mult >= 0.0f && mult <= 10.0f) || city > 4 || !(peds >= 0.0f && peds <= 10.0f) || !(cars >= 0.0f && cars <= 10.0f))
        return;
    if (!g_policyApplied)
        if (void* wanted = PlayerWanted())
            g_ownPolicy = {(uint8_t)Ref<int32_t>(0x8CDEE4), (uint8_t)(Field<uint8_t>(wanted, 0x1E) & 5), Field<float>(wanted, 0x14),
                           PedDensity(), CarDensity()};
    ApplyPolicy({max, backOff, mult, peds, cars});
    g_policyApplied = true;
    // the world we play is the host's from before its mission: the city it opened comes now, or the countryside the story
    // takes us to gave 4 stars for good, and with stars the host's new world never loaded (Flow's ReloadBlocker)
    if (city > CityStat()) {
        LOGI("The host's story opened city level %u (ours %d)", city, CityStat());
        CityStat() = city;
    }
    if (clearWanted) {
        Cmd(0x0110).I(0).Run();  // CLEAR_WANTED_LEVEL
        LOGI("The host's mission cleared the wanted level: ours too");
    }
}

std::set<int> g_missionWeapons;  // partner: what the host's mission gave (only those it takes back)
std::map<int, std::pair<int, int>> g_replaced;  // partner: its own weapon a given one pushed out of the slot (type, ammo)

void GiveWeaponNow(int h, int type, int count) {
    int model = -1;
    Cmd(0x0781).I(type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
    if (model > 0) LoadModelNow(model);
    Cmd(0x01B2).I(h).I(type).I(std::clamp(count, 1, 99999)).Run();  // GIVE_WEAPON_TO_CHAR
}
// what the mission gave goes, the partner's own gun it pushed out comes back
void TakeBackMissionWeapon(CPlayerPed* me, int weapon) {
    int h = GetPedRef(me);
    Cmd(0x0555).I(h).I(weapon).Run();  // REMOVE_WEAPON_FROM_CHAR
    if (auto it = g_replaced.find(weapon); it != g_replaced.end()) {
        GiveWeaponNow(h, it->second.first, it->second.second);
        g_replaced.erase(it);
    }
}

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
    auto give = [&](int type, int count) { GiveWeaponNow(h, type, count); };
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
        TakeBackMissionWeapon(me, weapon);
    }
}

bool InArea(const Area& a, const CVector& p, float slack) {
    return p.x > std::min(a.x1, a.x2) - slack && p.x < std::max(a.x1, a.x2) + slack &&
           p.y > std::min(a.y1, a.y2) - slack && p.y < std::max(a.y1, a.y2) + slack &&
           (a.z < -50.0f || std::fabs(p.z - a.z) < 3.0f);  // -100: a 2D locate
}

// A story mission's start point: a contact point of the radar in use (BLIP_CONTACT_POINT). A save house's or a
// property's marker is one too, but no mission waits there.
bool MissionContact(uintptr_t t) {
    if (!(Ref<uint8_t>(t + 0x25) & 2) || ((Ref<uint8_t>(t + 0x26) >> 2) & 0xF) != 5) return false;
    uint8_t sprite = Ref<uint8_t>(t + 0x24);
    // RADAR_SPRITE_PROPERTYG, PROPERTYR, SAVEGAME, and the gyms' (main.scm's three at their doors: "waiting for the host"
    // at every gym door, #34)
    return sprite != 31 && sprite != 32 && sprite != 35 && sprite != 54;
}

// The story's start point within range of p: the yellow ones are the radar's contact points (the radar draws them, the
// trigger only checks where the player is)
bool ContactMarkerAt(const CVector& p, float range, Area& out) {
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!MissionContact(t)) continue;
        CVector c = Ref<CVector>(t + 0x08);
        if ((c - p).Length2D() < range && std::fabs(c.z - p.z) < 3.0f) {
            out = {0x40000002u, c.x - 1.5f, c.y - 1.5f, c.x + 1.5f, c.y + 1.5f, c.z, NowMs()};
            return true;
        }
    }
    return false;
}

// The marker the host stands in: a start point, or a locate command's marker drawn in the last second. A shop's own
// (the changing room's of clothes.scm) starts no mission: the host stood waiting for the partner in Binco's.
bool MarkerAt(const CVector& p, Area& out) {
    if (ContactMarkerAt(p, 2.0f, out)) return true;
    for (auto& [id, a] : g_areas)
        if (a.kind == AREA_LOCATE && !a.streamed && NowMs() - a.seenMs < 1000 && InArea(a, p, 0.0f)) {
            out = a;
            return true;
        }
    return false;
}

// The start points (Sweet's "S", Ryder's "R" and their markers) are set by the script of the mission that passes, in
// the host's game: the partner had them only from the host's world, loaded at a quiet moment (on foot, no stars). It
// kept a passed mission's point, inactive, and missed a new one, often till the next mission. The host sends its points
// as they change; the partner's radar takes them.
struct Contact {
    CVector pos;
    uint8_t sprite = 0, flags25 = 0, display = 0;
    uint32_t colour = 0;
    uint16_t size = 0;
    int16_t enex = -1;  // inside an interior: the entrance whose street door shows it, as for the mission's blips
    bool Same(const Contact& o) const {  // the same point: its look changes (a mission hides them)
        return sprite == o.sprite && (pos - o.pos).Length2D() < 2.0f && std::fabs(pos.z - o.pos.z) < 3.0f;
    }
};
std::vector<Contact> g_hostContacts;  // partner: the host's points, as last sent
bool g_haveHostContacts = false;

Contact ContactAt(uintptr_t t) {
    Contact c;
    c.pos = Ref<CVector>(t + 0x08);
    c.sprite = Ref<uint8_t>(t + 0x24);
    c.flags25 = Ref<uint8_t>(t + 0x25);
    c.display = Ref<uint8_t>(t + 0x26) & 3;
    c.colour = Ref<uint32_t>(t + 0x00);
    c.size = Ref<uint16_t>(t + 0x1C);
    c.enex = EnexIndex(Ref<void*>(t + 0x20));  // tRadarTrace::m_pEntryExit
    return c;
}

void SetContactLook(uintptr_t t, const Contact& c) {
    Ref<uint32_t>(t + 0x00) = c.colour;
    Ref<uint16_t>(t + 0x1C) = c.size;
    Ref<void*>(t + 0x20) = EnexAt(c.enex);
    Ref<uint8_t>(t + 0x25) = (uint8_t)(c.flags25 | 2);  // keep m_bInUse
    Ref<uint8_t>(t + 0x26) = (uint8_t)((Ref<uint8_t>(t + 0x26) & ~3) | c.display);
}

// host: on a change, and every 5 s for a partner who came meanwhile
void HostSendContacts() {
    static std::vector<uint8_t> sent;
    static uint64_t sentMs = 0;
    Writer w(proto::G_CONTACTS);
    std::vector<Contact> list;
    for (int i = 0; i < kNumTraces; ++i)
        if (MissionContact(Trace(i))) list.push_back(ContactAt(Trace(i)));
    w.U8((uint8_t)std::min<size_t>(list.size(), 64));
    for (size_t i = 0; i < list.size() && i < 64; ++i) {
        const Contact& c = list[i];
        w.Vec({c.pos.x, c.pos.y, c.pos.z});
        w.U8(c.sprite);
        w.U8(c.flags25);
        w.U8(c.display);
        w.U32(c.colour);
        w.U16(c.size);
        w.I16(c.enex);
    }
    if (w.Buffer() == sent && NowMs() - sentMs < 5000) return;
    sent = w.Buffer();
    sentMs = NowMs();
    Net().Send(proto::CH_RELIABLE, w);
}

// partner: a point the host has no more goes, one it has comes, each looks as the host's
void TakeHostContacts() {
    if (!g_haveHostContacts || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    std::vector<bool> found(g_hostContacts.size());
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!MissionContact(t)) continue;
        Contact mine = ContactAt(t);
        size_t k = 0;
        while (k < g_hostContacts.size() && (found[k] || !g_hostContacts[k].Same(mine))) ++k;
        if (k == g_hostContacts.size()) {
            Call<0x587C10>(i);  // CRadar::ClearActualBlip
            LOGI("Mission start point (sprite %d) at %.0f,%.0f gone: the host's game has it no more", mine.sprite, mine.pos.x, mine.pos.y);
            continue;
        }
        found[k] = true;
        SetContactLook(t, g_hostContacts[k]);
    }
    for (size_t k = 0; k < g_hostContacts.size(); ++k) {
        if (found[k]) continue;
        const Contact& c = g_hostContacts[k];
        int handle = -1;
        Cmd(0x02A7).F(c.pos.x).F(c.pos.y).F(c.pos.z).I(c.sprite).Out(&handle).Run();  // ADD_SPRITE_BLIP_FOR_CONTACT_POINT
        if (handle >= 0 && (handle & 0xFFFF) < kNumTraces) SetContactLook(Trace(handle & 0xFFFF), c);
        LOGI("Mission start point (sprite %d) at %.0f,%.0f added: the host's game has it", c.sprite, c.pos.x, c.pos.y);
    }
}

void HandleContacts(uint8_t from, Reader& r) {
    if (Net().IsHost() || from != Net().HostId()) return;
    uint8_t n = r.U8();
    std::vector<Contact> list;
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        Contact c;
        Vec3 p = r.Vec();
        c.pos = {p.x, p.y, p.z};
        c.sprite = r.U8();
        c.flags25 = r.U8();
        c.display = r.U8() & 3;
        c.colour = r.U32();
        c.size = r.U16();
        c.enex = r.I16();
        list.push_back(c);
    }
    if (!r.Ok()) return;
    g_hostContacts = std::move(list);
    g_haveHostContacts = true;
    TakeHostContacts();
}

// The partner in a start point first (on foot, no mission anywhere): the host gets a call to come (MissionWaitText, Y
// takes it there) and its trigger starts the mission once both stand in it. The partner's game also says (G_MISSION_WAIT
// the other way) when the host's world waits to load in it, which after a passed mission happens at the next start point
// (flow), or loads now: the host's start waits for that, a mission started under the load ran in the world it replaced.
enum : uint8_t { PW_AT = 1, PW_WORLD = 2, PW_LOADING = 4 };
struct PartnerAt {
    uint8_t flags = 0;
    Area marker{};
    int8_t area = 0;
    uint64_t ms = 0;         // when it said so last
    uint64_t heldSince = 0;  // host: since when its world holds our start
};
std::map<uint8_t, PartnerAt> g_partnersAt;  // host
uint8_t g_callDeclined = 0xFF;              // host: N to this partner's call, till it leaves the point
uint8_t g_atSent = 0;                       // partner: the flags it sent last

// partner: on foot in one of the start points (the host's, see TakeHostContacts), no mission here or at the host's
bool PartnerAtMarker(Area& out) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!flow::InHostWorld() || me->InVehicle() || me->Health() <= 0.0f || HostMissionRecent(0) || FindMissionScript()) return false;
    return ContactMarkerAt(me->GetPosition(), 2.5f, out);
}

void PartnerSendAt() {
    static uint64_t sentMs = 0;
    static CVector sentAt;
    Area marker{};
    bool at = PartnerAtMarker(marker);
    uint8_t flags = (at ? PW_AT : 0) | (flow::HostWorldWaiting() ? PW_WORLD : 0) | (flow::HostWorldLoading() ? PW_LOADING : 0);
    CVector c{(marker.x1 + marker.x2) * 0.5f, (marker.y1 + marker.y2) * 0.5f, marker.z};
    bool other = at && (c - sentAt).Length() > 1.0f;
    if (flags == g_atSent && !other && (!flags || NowMs() - sentMs < 1000)) return;  // a change at once, the same every second
    if (flags == PW_AT && (g_atSent != PW_AT || other)) LOGI("At a mission start point (%.1f,%.1f): the host is called", c.x, c.y);
    g_atSent = flags, sentMs = NowMs(), sentAt = c;
    Writer w(proto::G_MISSION_WAIT);
    w.U8(flags);
    for (float f : {marker.x1, marker.y1, marker.x2, marker.y2, marker.z}) w.F32(f);
    w.I8(FindPlayerPed(0)->AreaCode());
    Net().Send(proto::CH_RELIABLE, w);
}

// host: the partner's world holds our start: it loads it now, or stands here waiting to (10 s at most: something of its
// own kept the load off, the stars of a chase, a task)
bool PartnerWorldHolds(uint8_t id, const RemotePlayer* rp, const Area& marker) {
    auto it = g_partnersAt.find(id);
    if (it == g_partnersAt.end()) return false;
    PartnerAt& p = it->second;
    if ((p.flags & PW_LOADING) && NowMs() - p.ms < 15000) return true;
    bool here = rp && rp->ped && InArea(marker, rp->ped->GetPosition(), 1.0f);
    if (!(p.flags & PW_WORLD) || !here) {
        p.heldSince = 0;
        return false;
    }
    if (!p.heldSince) p.heldSince = NowMs(), LOGI("Mission start point: %s's game loads the story first", Net().PeerName(id).c_str());
    return NowMs() - p.heldSince < 10000;
}

// host: a partner who waits for us in a start point outdoors (its call: MissionWaitText, Y answers), or 0xFF
uint8_t PartnerCalling() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!core::IsPlayerReady() || HostOnMission() || FindMissionScript() || CutsceneRunning() || me->Health() <= 0.0f ||
        GetPad(0)->DisablePlayerControls)
        return 0xFF;
    Area mine;
    if (MarkerAt(me->GetPosition(), mine)) return 0xFF;  // in one ourselves: our trigger has it
    for (auto& [id, at] : g_partnersAt)
        if (at.flags == PW_AT && at.area == 0 && id != g_callDeclined && NowMs() - at.ms < 2500) return id;
    return 0xFF;
}

// host: Y to the call. Into the point beside the partner (its middle, or 0.9 m past it from a partner standing there),
// on foot: our trigger starts the mission.
void GoToPartnerMarker(uint8_t id) {
    const PartnerAt& at = g_partnersAt[id];
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* rp = Players::Instance().Get(id);
    CVector c{(at.marker.x1 + at.marker.x2) * 0.5f, (at.marker.y1 + at.marker.y2) * 0.5f, at.marker.z};
    CVector partner = rp && rp->ped ? rp->ped->GetPosition() : c + CVector{0.0f, 0.0f, 1.0f};
    CVector d = c - partner;
    d.z = 0.0f;
    CVector to = c;
    if (d.Length() < 0.9f) to = c + (d.Length() > 0.05f ? d.Normalized() : CVector{1.0f, 0.0f, 0.0f}) * 0.9f;
    to.z = partner.z;
    int h = GetPedRef(me);
    if (me->InVehicle()) Cmd(0x0362).I(h).F(to.x).F(to.y).F(to.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    Cmd(0x04BB).I(0).Run();                                                 // SET_AREA_VISIBLE
    Cmd(0x0860).I(h).I(0).Run();                                            // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(h).F(to.x).F(to.y).F(to.z - 1.0f).Run();                  // SET_CHAR_COORDINATES
    Cmd(0x0173).I(h).F(std::atan2(to.x - partner.x, partner.y - to.y) * 57.29578f).Run();  // SET_CHAR_HEADING: to the partner
    LightArea(0, to);
    LOGI("Went over to %s's mission start point at %.1f,%.1f", Net().PeerName(id).c_str(), c.x, c.y);
}

// host, in a trigger: the marker the host stands in holds every partner as well. Without a marker (a phone call,
// a story event) the mission starts as usual.
bool PartnersAtMarker(CPed* host) {
    Area marker;
    if (!MarkerAt(host->GetPosition(), marker)) return true;
    for (auto& [id, peer] : Net().Peers()) {
        RemotePlayer* rp = Players::Instance().Get(id);
        if (PartnerWorldHolds(id, rp, marker)) {
            g_waitMs = NowMs();
            g_waitArea = marker;
            if (g_currentScript) memcpy(g_waitBy, Field<char[8]>(g_currentScript, 0x08), 8);
            return false;
        }
        // a partner whose game sends nothing for 5 s (in the pause menu, loading, hung) cannot come: the host was held
        // at the marker for good. Such a player is away in SA-MP and MTA too.
        if (!rp || !rp->hasState || NowMs() - rp->stateRecvMs > 5000) continue;
        if (!rp->ped || !InArea(marker, rp->ped->GetPosition(), 1.0f)) {  // two can not stand on one spot
            g_waitMs = NowMs();
            g_waitArea = marker;
            if (g_currentScript) memcpy(g_waitBy, Field<char[8]>(g_currentScript, 0x08), 8);
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
    if (SideThread(Field<char[8]>(g_currentScript, 0x08)))  // in the other's quarry, ours at its marker would fade out for good
        return (Net().IsHost() || !HostMissionRecent(0)) && !(SideJoined() && g_side.joined == "quarry");
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
void StartParallel();  // below
void* __cdecl HookStartNewScript(uint8_t* ip) {
    // LOAD_AND_LAUNCH_MISSION_INTERNAL starts the mission at CTheScripts' mission block (0x4899FA, 0x489A7A): its launcher
    if (ip == (uint8_t*)0xA7A6A0) {
        g_launcher[0] = 0;
        if (g_currentScript) memcpy(g_launcher, Field<char[8]>(g_currentScript, 0x08), 8);
        // a taxi job of our own in the other's: each drives its own fares (Side::parallel), before its first command
        static const uint8_t kTaxi[] = {0xA4, 0x03, 0x09, 'T', 'A', 'X', 'I', 'O', 'D', 'D', 0};  // SCRIPT_NAME 'TAXIODD'
        bool ownTaxi = SideJoined() && !_stricmp(g_side.joined.c_str(), "taxiodd") && !memcmp(ip, kTaxi, sizeof kTaxi);
        if (ownTaxi) StartParallel();
        // Taking part in the other player's side activity, this game started its own as its player stood at the same
        // start (on the bike at the BMX ground, in the race's marker): leaving for it split the joint one in two (#19).
        // Ours ends at once; R3 and ODDVEH launch again only when the player gets in again.
        if (SideJoined() && !ownTaxi &&
            (SideThread(g_launcher) || !_strnicmp(g_launcher, "r3", 8) || !_strnicmp(g_launcher, "oddveh", 8) ||
             !_strnicmp(g_launcher, "cesar", 8))) {
            ip[0] = 0x4E, ip[1] = 0x00;  // TERMINATE_THIS_SCRIPT
            LOGI("Our own side activity (launched by %s) not started: we take part in the other player's", g_launcher);
            // SHOOT raised the mission flag for its range, which ends now with no clean-up to lower it: the game stayed
            // on a mission for good. Down again, and SHOOT's latch up until the street ($104), or it launched it again
            if (!_strnicmp(g_launcher, "shoot", 8))
                Ref<int32_t>(addr::ScriptSpace + 1636) = 0, Ref<int32_t>(addr::ScriptSpace + 104) = 1;
        }
    }
    static const uint8_t kIntro[] = {0xA4, 0x03, 0x09, 'I', 'N', 'T', 'R', 'O', 0};  // SCRIPT_NAME 'INTRO'
    if (ip && flow::IsFollower() && !memcmp(ip, kIntro, sizeof kIntro)) {
        ip[0] = 0x4E, ip[1] = 0x00;  // TERMINATE_THIS_SCRIPT (it also ends "a mission runs")
        Fade(1.0f, 1);  // main.scm blacks the screen out before the prologue, which would fade it in
        LOGI("The prologue mission is the host's: this game's copy ends at once");
    }
    return g_origStartScript(ip);
}

void HandleMissionWait(uint8_t from, Reader& r) {
    if (Net().IsHost()) {  // a partner's: where it stands, what its game waits for
        PartnerAt p;
        p.flags = r.U8();
        p.marker = {0x40000002u, r.F32(), r.F32(), r.F32(), r.F32(), r.F32(), 0};
        p.area = r.I8();
        if (!r.Ok() || from == Net().MyId()) return;
        PartnerAt& was = g_partnersAt[from];
        p.ms = NowMs();
        p.heldSince = was.heldSince;
        if (!(p.flags & PW_AT) && g_callDeclined == from) g_callDeclined = 0xFF;
        was = p;
        return;
    }
    bool on = r.U8() != 0;
    Area a{};
    if (on) a = {0x40000001u, r.F32(), r.F32(), r.F32(), r.F32(), r.F32(), 0};
    if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
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
    LOGD("The host's mission text table %s %s", name, missing.empty() ? "loaded" : "is not in this game");
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

// A running cutscene prints its subtitles itself, outside any script (CCutsceneMgr's text list, AddMessageJumpQ at
// 0x5B187E), and so does the partner's copy of it. The host's line goes along all the same, with its moment in the
// cutscene: a partner whose game has no such key (another translation) was left with none (#55). The partner shows the
// host's line once its own copy is past that moment and printed nothing like it (its copy may run seconds behind).
bool CutsceneLine() { return Ref<uint8_t>(0xB5F851) && !CurrentScript(); }  // CCutsceneMgr::ms_running
int CutsceneMs() { return (int)(Ref<float>(0xBC3F08) * 1000.0f); }        // CCutsceneMgr::ms_cutsceneTimerS
std::deque<std::pair<std::string, uint64_t>> g_csLines;  // partner: the lines its copy printed lately
bool PrintedLately(const char* t, bool record) {
    if (!t || !*t) return false;
    uint64_t now = NowMs();
    while (!g_csLines.empty() && now - g_csLines.front().second > 15000) g_csLines.pop_front();
    for (const auto& line : g_csLines)
        if (line.first == t) return true;
    if (record) g_csLines.emplace_back(t, now);
    return false;
}
struct HostLine {
    std::string text;
    uint32_t time;
    uint16_t flag;
    int at;  // ms into the host's cutscene
};
std::deque<HostLine> g_hostLines;  // partner
void ShowHostLines() {
    if (!Ref<uint8_t>(0xB5F851)) return;  // not yet: the host's lines wait for our copy to start
    while (!g_hostLines.empty() && CutsceneMs() > g_hostLines.front().at + 300) {
        HostLine l = std::move(g_hostLines.front());
        g_hostLines.pop_front();
        if (PrintedLately(l.text.c_str(), false)) continue;
        uint32_t left = l.time > (uint32_t)(CutsceneMs() - l.at) ? l.time - (CutsceneMs() - l.at) : 0;
        if (left > 500) g_origMsgQ(PoolText(l.text), left, l.flag, false);
        LOGD("Cutscene subtitle from the host shown (this game printed none at %d ms)", l.at);
    }
}

void SendText(uint8_t kind, const char* text, uint32_t time, uint16_t flag, const int* nums, uint8_t bools) {
    if (CutsceneLine()) {
        if (kind != TXT_MSG_Q || !text || !Net().Online()) return;
        std::string line(text, strnlen(text, 40));  // the line as this game found it (empty: its key is not here)
        for (char& c : line) c = c >= 32 && c < 127 ? c : '?';
        LOGD("Cutscene subtitle here: \"%s\"", line.c_str());
        kind = TXT_CS_LINE;
        static int at[6];
        at[0] = CutsceneMs();
        nums = at;
    }
    // Big Smoke's courier (Courier.cpp), a streamed script outside any mission: its lines, not the help to answer its call
    bool courier = Net().Online() && Net().IsHost() && CourierThread(CurrentScript()) && kind != TXT_HELP;
    courier = courier || JointValet(CurrentScript());  // the valet's shift the other player joined: a streamed script too
    if (!Replicating() && !SideOwning() && !pool::Owning() && !courier) return;
    // a streamed script's (a shop, the gym, a casino table, a vending machine the host uses during its mission): its own
    if (void* sc = CurrentScript(); sc && Field<uint8_t>(sc, 0xC7) && !courier) return;  // CRunningScript::m_bIsExternal
    // another mod's script (a CLEO trainer's menu and hints, #36): its code is outside main.scm and the mission block
    if (void* sc = CurrentScript(); sc && !courier) {
        auto* ip = Field<uint8_t*>(sc, 0x14);  // m_pCurrentIP
        if (ip < (uint8_t*)addr::ScriptSpace || ip >= (uint8_t*)addr::ScriptSpace + 200000 + 69000) return;
    }
    if (minigames::PartnerShowsOwn(CurrentScript())) return;  // the lowrider judge's texts: the partner judges its own
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
    if (g_forwardOnly && DialogueLine(t)) return;  // the host plays without subtitles
    g_origMsg(t, time, flag, brief);
}
void __cdecl HookMsgQ(const char* t, uint32_t time, uint16_t flag, bool brief) {
    SendText(TXT_MSG_Q, t, time, flag, nullptr, brief);
    if (g_forwardOnly && DialogueLine(t)) return;
    if (CutsceneLine() && !Net().IsHost()) PrintedLately(t, true);
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
bool HostScene();
void __cdecl HookHelp(const char* t, bool quick, bool permanent, bool addToBrief) {
    // a story mission's help is about its player's own doing ("press ... to pick the box up" by the host's crate, the
    // noise): the partner's game shows its own (Objects, the noise below); a minigame's tells both how to play, and one
    // during a scene on the engine is part of what the partner watches (the barber's tips after the haircut)
    if (!Replicating() || minigames::HostMinigame() || HostScene())
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
    if (!r.Ok() || !MissionSource(from) || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (const char* own = GxtLocal(hash, table)) text = own;
    const char* t = text.empty() ? nullptr : PoolText(text);
    if ((kind == TXT_MSG || kind == TXT_MSG_Q) && !ShowSubtitles() && DialogueLine(t)) return;  // our own setting: off
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
    case TXT_TUNE: Cmd(0x0394).I(flag).Run(); break;  // PLAY_MISSION_PASSED_TUNE
    case TXT_CS_LINE:  // waits for our own copy of the cutscene to get there (ShowHostLines)
        if (!t || !hasNums) return;
        // no copy here (it was not loaded: our player in the air or on the water): the host's line now
        if (g_csState == 0) {
            if (ShowSubtitles()) g_origMsgQ(t, time, flag, false);
            return;
        }
        if (g_hostLines.size() < 64) g_hostLines.push_back({t, time, flag, n[0]});
        break;
    }
}

// ---------------------------------------------------------------- markers
using HighlightFn = void(__cdecl*)(uint32_t, float, float, float, float, float);
HighlightFn g_origHighlight = nullptr;
void __cdecl HookHighlight(uint32_t id, float x1, float y1, float x2, float y2, float z) {
    if (Net().Online() && (Net().IsHost() || SideOwning())) {
        if (!g_areas.count(id)) LOGD("Script marker %08X at %.1f,%.1f,%.1f", id, (x1 + x2) * 0.5f, (y1 + y2) * 0.5f, z);
        g_areas[id] = {id, x1, y1, x2, y2, z, NowMs(), AREA_LOCATE, MissionScriptRuns() || JointValet(g_currentScript)};
        g_areas[id].streamed = StreamedScriptRuns();
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
    if (Net().Online() && (Net().IsHost() || SideOwning()) && pos) {
        if (!g_areas.count(id)) LOGD("Script sphere %08X at %.1f,%.1f,%.1f", id, pos->x, pos->y, pos->z);
        g_areas[id] = {id, pos->x, pos->y, radius, 0.0f, pos->z, NowMs(), AREA_SPHERE, MissionScriptRuns()};
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
    if (!r.Ok() || !MissionSource(from)) return;
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
// STUNT turns each of its rings 10 degrees a frame (SET_CHECKPOINT_HEADING, the only script that does): every update
// of the host's then had a new direction, and the partner's ring was made anew four times a second, its spin in jumps
// (29.10: "анимация колец подлагивает"). A placed one that only turned stays, and turns here as there.
std::map<uint8_t, float> g_turning;  // partner: slot -> its heading here
bool SamePlace(const Marked& a, const Marked& b) {
    return a.slot == b.slot && a.type == b.type && (a.pos - b.pos).Length() < 0.05f && std::fabs(a.size - b.size) < 0.01f;
}
void TurnMarked() {
    for (auto it = g_turning.begin(); it != g_turning.end();) {
        auto placed = g_placed.find(it->first);
        if (placed == g_placed.end()) {
            it = g_turning.erase(it);
            continue;
        }
        it->second = std::fmod(it->second + 10.0f, 360.0f);
        Cmd(0x0996).I(placed->second.second).F(it->second).Run();  // SET_CHECKPOINT_HEADING
        ++it;
    }
}

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
    std::vector<Marked> now = Replicating() || SideOwning() ? HostMarked() : std::vector<Marked>{};
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
    g_turning.clear();
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
    if (!r.Ok() || !MissionSource(from) || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    for (auto it = g_placed.begin(); it != g_placed.end();) {  // gone or changed
        auto same = std::find_if(now.begin(), now.end(), [&](const Marked& m) { return m == it->second.first; });
        if (same != now.end()) {
            ++it;
            continue;
        }
        auto turned = std::find_if(now.begin(), now.end(), [&](const Marked& m) { return SamePlace(m, it->second.first); });
        if (turned != now.end() && it->first < 32) {  // only its direction: turned, not made anew
            if (!g_turning.count(it->first))
                g_turning[it->first] = std::atan2(turned->dir.y, turned->dir.x) * 57.29578f;  // as SET_CHECKPOINT_HEADING sets it
            it->second.first = *turned;
            ++it;
            continue;
        }
        g_turning.erase(it->first);
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
    // A cutscene prints its subtitles itself, by keys of the mission's text table (CCutsceneMgr's text list). The partner
    // loaded the host's table only with the first text of the mission, and a mission's first cutscene comes before any:
    // its lines were not found there and the subtitles were missing.
    if (op == CS_LOAD) w.Str(MissionTable(), 8);
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
    // LoadCutsceneData has just rebuilt our own player if its body had changed (RebuildPlayerIfNeeded, 0x4D5E8B), with
    // our clothes' textures: the host's chain, watch, glasses or hat then had none and its mesh crashed the game (#72).
    // Model 0 is dressed for the host right before.
    if (memcmp((const void*)0xBC1C78, g_cutsceneCj->ClothesDesc(), sizeof(CPedClothesDesc)) != 0) {  // PlayerClothes
        static bool told = false;
        if (!told) LOGI("Cutscene CJ: the player's textures were of another look, dressed for the host again");
        told = true;
    }
    DressCutsceneCj(g_cutsceneCj);
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
bool OwnActivity();

void PartnerClearCutscene() {
    Cmd(0x02EA).Run();  // CLEAR_CUTSCENE
    LeaveCutsceneScene();
    ReturnToCutsceneCar();
    Cmd(0x02EB).Run();            // RESTORE_CAMERA_JUMPCUT
    // the cutscene fades out at its end; in a mission the host's fades follow (G_FADE), otherwise the screen
    // would stay black. A mission that blacks out, clears its cutscene and fades in in one frame ("Drive-Thru", "Ryder",
    // "Cleaning the Hood") had its fade in here before the cutscene's end, and our copy's own last-second fade-out came
    // after it: the partner's screen stayed black until the next fade of the host's. What the host's screen is now counts.
    if (!g_partnerMission && FadeAlpha() > 1.0f) Fade(0.5f, 1);
    else if (g_partnerMission && !OwnActivity()) {
        if (!g_hostBlack && Darkening()) {
            Fade(0.5f, 1);
            LOGI("Host cutscene: the host's screen is already clear, ours fades in");
        } else if (g_hostBlack) {
            g_weFaded = true;  // the host's fade in brings ours back
        }
    }
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

// Our player in an aircraft or a boat, or on foot in the air (a parachute, a jetpack): no host's scene for it. On foot the
// flag alone (bIsStanding) failed a cutscene that came right after another (LA1FIN2, then BCRAS2): the first one's end
// had just put us down, and it was not set yet. So high above the ground as well.
bool InAirOrWater() {
    CPlayerPed* me = FindPlayerPed(0);
    int type = me->InVehicle() && me->Vehicle() ? me->Vehicle()->VehicleSubClass() : -1;
    if (type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_BOAT || type == VEHICLE_TYPE_FHELI ||
        type == VEHICLE_TYPE_FPLANE)
        return true;
    if (type >= 0 || (me->PedFlags() & 1)) return false;
    CVector p = me->GetPosition();
    bool found = false;
    float ground = Call<0x5696C0, float>(p.x, p.y, p.z, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
    return found && p.z - ground > 3.0f;
}

void StopWatching(bool restore);
bool NearHost(float range);

// Riding with the host in a car one of its mission's people drives (Sweet's of "Tagging Up Turf"): the host got out at the
// tags, the partner did not in time and was driven off with Sweet to the next district, then moved with the car back to
// the yard (Nikolai, 26.10). Out with the host.
void LeaveWithHost() {
    static bool hostWasIn = false;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    bool hostIn = v && host && host->ped && host->ped->InVehicle() && host->ped->Vehicle() == v;
    CPed* driver = v ? v->Driver() : nullptr;
    if (hostWasIn && !hostIn && g_partnerMission && g_csState == 0 && !CutsceneRunning() && driver && driver != me &&
        !driver->IsPlayer() && !Players::Instance().FindByPed(driver)) {
        Cmd(0x05CD).I(GetPedRef(me)).I(GetVehicleRef(v)).Run();  // TASK_LEAVE_CAR
        LOGI("The host got out of the car its mission's driver drives: so do we");
    }
    hostWasIn = hostIn;
}

// The host's mission put the host in a vehicle where it moved it (the Shamal in the air at "Freefall"'s end): set down
// beside the host up there, the partner fell. It gets a seat in that vehicle as soon as its copy is here.
uint16_t g_followVeh = 0;
uint64_t g_followVehUntil = 0;
void SeatWithHost() {
    if (!g_followVeh) return;
    CPlayerPed* me = FindPlayerPed(0);
    NetVehicle* nv = Vehicles::Instance().Find(g_followVeh);
    if (NowMs() > g_followVehUntil || me->InVehicle()) {
        g_followVeh = 0;
        return;
    }
    if (!nv || !Vehicles::Alive(*nv)) return;  // its copy comes with the next states near it
    g_followVeh = 0;
    for (int seat = 0; seat < nv->veh->MaxPassengers(); ++seat)
        if (!nv->veh->Passengers()[seat]) {
            Cmd(0x0430).I(GetPedRef(me)).I(nv->handle).I(seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
            LOGI("Seated in the host's vehicle %04X where its mission put it", nv->netId);
            return;
        }
}

void HandleCutscene(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    std::string name = r.Str(32);
    int32_t area = r.I32();
    if (op == CS_SKIP_VOTE || op == CS_SKIP_STATE) return HandleSkip(from, op, r);
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    if (op == CS_LOAD) {
        std::string table = r.Str(8);
        if (r.Ok() && !table.empty()) UseMissionTable(table);  // its subtitles' keys
        g_hostLines.clear(), g_csLines.clear();                   // the last cutscene's lines are no more
        // the previous one is cleared first, even when its end came in the same frame: otherwise our ped stays
        // where the previous scene put it (an interior's exit marker fires and takes the camera)
        if (g_csState == 2) PartnerEndCutscene();
        if (g_csState != 0) PartnerClearCutscene();
        StopWatching(false);  // the cutscene's camera from now on (our persisting scene camera would hold it)
        // A partner in an aircraft or a boat, or in the air on foot (a parachute, a jetpack): the loading sets the player
        // out of the vehicle (CCutsceneMgr::LoadCutSceneFile), and after it the player came back where it had been, in
        // mid-air or in the water, with the aircraft gone on. One that flew or sailed along with the host (300 m) is set
        // down beside it when the host is on the ground, and stays there after the scene, its vehicle left: it missed
        // every scene of the story's end otherwise (#14). Farther, or with the host up in the air too, it stays out.
        if (InAirOrWater()) {
            const RemotePlayer* host = Players::Instance().Get(Net().HostId());
            CPlayerPed* me = FindPlayerPed(0);
            CVehicle* hv = host && host->ped && host->ped->InVehicle() ? host->ped->Vehicle() : nullptr;
            int hvType = hv ? hv->VehicleSubClass() : -1;
            bool hostGrounded = hv ? hvType != VEHICLE_TYPE_HELI && hvType != VEHICLE_TYPE_PLANE && hvType != VEHICLE_TYPE_BOAT &&
                                         hvType != VEHICLE_TYPE_FHELI && hvType != VEHICLE_TYPE_FPLANE
                                   : host && host->ped && (host->ped->PedFlags() & 1);  // bIsStanding
            if (!hostGrounded || host->state.area != 0 || me->AreaCode() != 0 || !NearHost(300.0f)) {
                LOGI("Host cutscene %s: not here, our player is in the air or on the water", name.c_str());
                return;
            }
            CEntity* by = hv ? (CEntity*)hv : (CEntity*)host->ped;
            CMatrix* m = by->Matrix();
            CVector p = by->GetPosition() + (m ? m->right : CVector{1.0f, 0.0f, 0.0f}) * (hv ? 3.0f : 1.5f);  // its right
            bool found = false;
            float ground = Call<0x5696C0, float>(p.x, p.y, p.z + 2.0f, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
            if (found) p.z = ground + 1.0f;
            int h = GetPedRef(me);
            if (me->InVehicle()) Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            else Cmd(0x00A1).I(h).F(p.x).F(p.y).F(p.z - 1.0f).Run();          // SET_CHAR_COORDINATES
            g_csCar = g_csSeat = -1;
            LOGI("Host cutscene %s: our player was in the air or on the water, set down by the host for it", name.c_str());
        }
        LOGI("Host cutscene %s: loading here", name.c_str());
        // the loading sets us out of a vehicle (CCutsceneMgr::LoadCutsceneData_preload): we get back in afterwards
        if (CPlayerPed* me = FindPlayerPed(0); me->InVehicle() && me->Vehicle() && g_csCar < 0) {
            g_csCar = GetVehicleRef(me->Vehicle());
            g_csSeat = me->Vehicle()->Driver() == me ? -1 : PassengerSeatOf(me->Vehicle(), me);
        }
        scene::Prepare();  // before the cutscene's own streaming requests
        if (const RemotePlayer* host = Players::Instance().Get(Net().HostId()); host && host->ped)
            g_cutsceneCj = host->ped;  // the story's CJ is the host: dressed as LoadCutsceneData builds it
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
        s.pose = (scene::Pose)std::min<uint8_t>(r.U8(), scene::POSE_CAR);
        s.act = r.U8();
        if (r.Ok()) scene::FollowSpot(s, at);
    } else if (op == CS_FOLLOW) {
        Vec3 to = r.Vec(), from = r.Vec();
        int8_t fromArea = r.I8();
        float heading = r.F32();
        CPlayerPed* me = FindPlayerPed(0);
        if (!r.Ok() || area < 0 || area > 255) return;
        // Wherever the partner is: the host's story goes on there (the hotel's roof, a plane's hold, the countryside after
        // "The Green Sabre"). A rule of 60 m at a change of area and 300 m otherwise left behind a partner that chased the
        // plane of "Stowaway" or waited at the motel of "Reuniting the Families". Not out of its own activity or a shop.
        const char* busy = FindMissionScript()      ? "an activity of its own"
                           : Ref<uint8_t>(0xA444A8) ? "a minigame or a shop"  // CTheScripts::bMiniGameInProgress
                           : me->Health() <= 0.0f   ? "dead"
                                                    : nullptr;
        if (busy) {
            LOGI("The host's mission moved it to area %d: we stay (%s)", area, busy);
            return;
        }
        CVector p = CVector{to.x, to.y, to.z} + CVector{std::cos(heading), std::sin(heading), 0.0f} * 1.5f;  // its right hand
        if (g_inScene) {  // the scene's end puts us there, not back at the old place
            g_scenePos = p, g_sceneArea = (int8_t)area, g_sceneHeading = heading, g_csCar = -1;
        } else {
            CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
            // riding with the host: its car brings us
            if (v && v->Driver() != me && area == 0 && Players::Instance().FindByPed(v->Driver())) return;
            int h = GetPedRef(me);
            if (v && (area != 0 || v->Driver() != me)) Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            Cmd(0x04BB).I(area).Run();                           // SET_AREA_VISIBLE
            Cmd(0x0860).I(h).I(area).Run();                      // SET_CHAR_AREA_VISIBLE
            Cmd(0x00A1).I(h).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES (a car we drive outdoors comes along)
            Cmd(0x0173).I(h).F(heading * 57.29578f).Run();       // SET_CHAR_HEADING
            LightArea(area, p);
            UseStreetDoor(area, p, CVector{from.x, from.y, from.z});
        }
        if (const RemotePlayer* host = Players::Instance().Get(Net().HostId());
            host && host->hasState && host->mode != RemotePlayer::Mode::OnFoot && area == 0 && !g_inScene)
            g_followVeh = host->vehNetId, g_followVehUntil = NowMs() + 3000;
        LOGI("The host's mission moved it %.0f m (area %d to %d): following",
             (CVector{to.x, to.y, to.z} - CVector{from.x, from.y, from.z}).Length(), fromArea, area);
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
        b.enex = EnexIndex(Ref<void*>(t + 0x20));  // tRadarTrace::m_pEntryExit
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
            if (uint16_t id = MissionObjectNetId(o)) b.entKind = 4, b.entId = id;  // on its copy: the arrow over it moves with it
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
    } else if (kind == 4) {
        ent = MissionObjectCopyHandle(b.entId);
    }
    if (ent < 0) kind = 0;  // not here (yet): a coordinate blip at its position

    PartnerBlip& pb = g_blips[b.index];
    if (pb.handle && !BlipAlive(pb.handle)) pb.handle = 0;  // went with its entity (a mirror made again)
    if (pb.handle && (pb.entKind != kind || (kind && pb.entId != b.entId))) RemovePartnerBlip(pb);
    if (!pb.handle) {
        int h = 0;
        if (kind == 1) Cmd(0x0186).I(ent).Out(&h).Run();       // ADD_BLIP_FOR_CAR
        else if (kind == 4) Cmd(0x0188).I(ent).Out(&h).Run();  // ADD_BLIP_FOR_OBJECT
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
    Ref<void*>(t + 0x20) = EnexAt(b.enex);  // the radar puts it at that entrance's door while this player is outside
}

std::map<uint8_t, BlipRecord> g_partnerBlipRecords;  // to retry entity attachment when mirrors appear

void HandleBlip(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (!MissionSource(from) || !core::IsPlayerReady() || !flow::InHostWorld()) return;
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

// TRUCK's drops (its 102@, 103@). A blip it left at one (see ScriptCommand) went into the saves and the host's world for
// the partners: a loaded world with no mission running loses it (a sprite-less coordinate blip of TRUCK's scale 4 there).
const float kTruckDrops[][2] = {
    {-187.4f, -277.0f}, {58.0f, -256.7f}, {95.9f, -154.4f}, {809.8f, -598.0f}, {1403.8f, 399.4f}, {1338.3f, 348.9f},
    {1449.7f, 2358.9f}, {1037.5f, 2131.3f}, {988.0f, 2080.4f}, {1288.7f, 1195.2f}, {2467.9f, 1950.1f},
    {2792.7f, 2578.3f}, {2271.5f, 2791.7f}, {2596.5f, 1738.6f}, {2818.8f, 912.5f}, {2706.5f, 827.3f}, {1627.7f, 688.4f},
    {1504.5f, 981.1f}, {1724.0f, 1590.1f}, {1727.8f, 2338.0f}, {2413.7f, -2113.7f}, {2785.0f, -2455.4f},
    {2112.7f, -2070.4f}, {1763.6f, -2070.4f}, {-1888.6f, -1711.8f}, {-2117.2f, -2380.5f}, {-1545.4f, -2747.0f},
    {-1407.4f, 2646.0f}, {-2245.6f, 2371.7f}, {-1360.6f, 2068.1f}, {274.3f, 1382.8f}, {628.9f, 1714.9f},
    {635.0f, 1213.8f}, {-915.0f, 2012.1f}, {385.8f, 2595.6f}, {-1557.0f, -441.3f}, {-2659.6f, 1380.6f},
    {-1650.9f, 437.6f}, {-1745.1f, 37.9f}, {56.7f, -268.4f}, {100.4f, -155.1f}, {815.0f, -605.1f}, {1401.1f, 398.8f},
    {1343.1f, 345.7f}, {-189.2f, -273.0f}, {1042.2f, 2130.5f}, {983.1f, 2076.4f}, {2469.6f, 1949.5f},
    {2807.8f, 2609.9f}, {2248.7f, 2775.1f}, {2603.2f, 1730.5f}, {2710.4f, 850.1f}, {1588.9f, 715.3f},
    {1446.3f, 1000.5f}, {1713.8f, 2329.1f}, {1450.8f, 2360.7f}, {2759.0f, -2446.0f}, {2104.0f, -2089.0f},
    {1751.0f, -2067.0f}, {2412.0f, -2106.0f}, {-2001.0f, -2388.1f}, {-1560.7f, -2728.4f}, {-1835.0f, -1647.7f},
    {-1945.9f, 2377.9f}, {-1471.5f, 1864.8f}, {-1286.7f, 2695.9f}, {613.8f, 1692.4f}, {620.4f, 1249.3f},
    {-533.2f, 1995.9f}, {325.1f, 2543.3f}, {246.6f, 1435.2f}, {-2629.1f, 1387.4f}, {-1530.8f, 487.2f},
    {-1745.1f, 27.8f}, {-1549.9f, -434.9f}};
void ClearTruckLeftovers() {
    if (FindMissionScript()) return;
    for (int i = 0; i < kNumTraces; ++i) {
        uintptr_t t = Trace(i);
        if (!(Ref<uint8_t>(t + 0x25) & 2) || ((Ref<uint8_t>(t + 0x26) >> 2) & 0xF) != BLIP_COORD || Ref<uint8_t>(t + 0x24) ||
            Ref<uint16_t>(t + 0x1C) != 4)
            continue;  // in use, coordinates, no sprite, TRUCK's CHANGE_BLIP_SCALE 4
        const CVector& p = Ref<CVector>(t + 0x08);
        for (const auto& d : kTruckDrops)
            if (std::fabs(p.x - d[0]) < 1.0f && std::fabs(p.y - d[1]) < 1.0f) {
                LOGI("A trucking drop's blip left in the loaded world at %.0f,%.0f: removed", p.x, p.y);
                Call<0x587C10>(i);  // CRadar::ClearActualBlip
                break;
            }
    }
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

// The partner runs no story mission (only the host starts them): a mission script here is its own side activity (a taxi
// fare, a race, a gym). The host's mission screen went over it: its timer was replaced, its fares faded to black.
bool OwnActivity() { return FindMissionScript() != nullptr; }

// A scene of the host's mission plays in its game alone (widescreen borders, the host's control taken: CJ and the boys
// over the cemetery wall in "Los Sepulcros"). The partner drove on, pushed one of its people, and the scene stood for
// minutes. A partner near the host stands still through it, as the host does.
void FreezeForScene(bool on) {
    if (on == g_weFroze) return;
    g_weFroze = on;
    Cmd(0x01B4).I(0).I(on ? 0 : 1).Run();  // SET_PLAYER_CONTROL
    LOGI(on ? "The host's mission plays a scene: our control taken" : "The host's scene is over: our control back");
}

bool NearHost(float range) {
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    return host && host->hasState && me && host->state.area == me->AreaCode() && (host->state.pos - me->GetPosition()).Length() < range;
}

// ---------------------------------------------------------------- scenes on the engine
// A mission's scene without a cutscene file: the script holds the camera (a fixed shot, a dolly, a camera on a car)
// with the bars up or the player's control off. The partner saw only its own camera, or a frozen CJ. The host streams the
// camera it drew (G_SCENE_CAM): 20 times a second while it moves, at once on a cut, 4 times a second while it stands,
// the end reliably. A partner near enough watches it through the engine's own scripted camera, the one these scenes use
// (CAMERA_PERSIST_POS/TRACK/FOV and CCamera's m_vecMoveLinear, m_vecTrackLinear, m_fFOVNew, which CCam::Process takes
// at its end): the world streams around that camera by itself, FOV zooms included, no jump cut every frame.
enum : uint8_t { SCENE_SAMPLE = 1, SCENE_END = 2 };
enum : uint8_t { SCENE_WIDE = 1, SCENE_CONTROL_OFF = 2 };

// host
bool g_hostScene = false;
uint8_t g_hostSceneId = 0;

bool HostScene() {
    uintptr_t c = addr::TheCamera;
    if (Ref<uint8_t>(c + 0x2B) || CutsceneRunning() || !HostOnMission()) g_scriptCam = false;  // m_bLookingAtPlayer again
    if (!Replicating() || !HostOnMission() || CutsceneRunning() || cam::Mode() == 17 /* MODE_FLYBY */) return false;
    bool wide = TheCameraWide();
    bool off = (GetPad(0)->DisablePlayerControls & 0x20) != 0;  // bPlayerSafe: SET_PLAYER_CONTROL off (not a door or a call)
    return (g_scriptCam && (wide || off)) || (wide && off);
}

void SendSceneEnd() {
    Writer w(proto::G_SCENE_CAM);
    w.U8(SCENE_END);
    w.U8(g_hostSceneId);
    Net().Send(proto::CH_RELIABLE, w);
}

void HostSceneUpdate() {
    static uint8_t shot = 0;
    static uint64_t sentMs = 0;
    static CVector lastSrc, sentSrc, sentFront;
    static float sentFov = 0.0f;
    if (!HostScene()) {
        if (g_hostScene) {
            SendSceneEnd();
            LOGI("Scene %u on the engine: over", g_hostSceneId);
        }
        g_hostScene = false;
        return;
    }
    uintptr_t c = addr::TheCamera;
    CVector src, front;
    float fov;
    if (Ref<uint8_t>(c + 0x58)) {  // m_bTransitionState: the drawn pose is interpolated in CCamera, not in the cam
        src = Ref<CVector>(c + 0x9A4), front = Ref<CVector>(c + 0x984), fov = Ref<float>(c + 0xEC);  // matrix pos, forward
    } else {
        src = cam::Source(), front = cam::Front(), fov = cam::Fov();  // before the plugins' SetFOV (widescreen fixes)
    }
    bool cut = !g_hostScene || Ref<uint8_t>(c + 0x2A) || (src - lastSrc).Length() > 3.0f;  // m_bJust_Switched, a jump
    if (!g_hostScene) {
        ++g_hostSceneId, shot = 0;
        LOGI("Scene %u on the engine: the partners see it", g_hostSceneId);
    }
    if (cut) ++shot;
    g_hostScene = true, lastSrc = src;
    bool moved = (src - sentSrc).Length() > 0.01f || (front - sentFront).Length() > 0.001f || std::fabs(fov - sentFov) > 0.05f;
    if (!cut && NowMs() - sentMs < (moved ? 50u : 250u)) return;
    sentMs = NowMs(), sentSrc = src, sentFront = front, sentFov = fov;
    Writer w(proto::G_SCENE_CAM);
    w.U8(SCENE_SAMPLE);
    w.U8(g_hostSceneId);
    w.U8(shot);
    w.U32((uint32_t)sentMs);
    w.Vec({src.x, src.y, src.z});
    w.Vec({front.x, front.y, front.z});
    w.F32(fov);
    w.I8((int8_t)CurrentArea());
    w.U8((TheCameraWide() ? SCENE_WIDE : 0) | ((GetPad(0)->DisablePlayerControls & 0x20) ? SCENE_CONTROL_OFF : 0));
    Net().Send(proto::CH_STATE, w);
}

// partner
struct SceneSample {
    uint8_t scene = 0, shot = 0, flags = 0;
    uint32_t ms = 0;  // the host's clock
    CVector src, front;
    float fov = 70.0f;
    int8_t area = 0;
    uint64_t recv = 0;
};
SceneSample g_sceneLast, g_scenePrev;  // the last two of the scene's current shot
bool g_sceneHave = false, g_watching = false;
uint8_t g_sceneEnded = 0;
uint64_t g_sceneEndedMs = 0, g_shownMs = 0;
CVector g_shownSrc, g_shownFront;
float g_shownFov = 70.0f;
uint8_t g_shownScene = 0, g_shownShot = 0;

void HandleSceneCam(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (op == SCENE_END) {
        uint8_t id = r.U8();
        if (!r.Ok() || from != Net().HostId() || Net().IsHost()) return;
        g_sceneEnded = id, g_sceneEndedMs = NowMs();
        if (g_sceneHave && g_sceneLast.scene == id) g_sceneHave = false;
        return;
    }
    SceneSample s;
    s.scene = r.U8(), s.shot = r.U8(), s.ms = r.U32();
    Vec3 a = r.Vec(), b = r.Vec();
    s.fov = r.F32(), s.area = r.I8(), s.flags = r.U8();
    s.src = {a.x, a.y, a.z}, s.front = {b.x, b.y, b.z}, s.recv = NowMs();
    float fl = s.front.Length();
    if (!r.Ok() || op != SCENE_SAMPLE || from != Net().HostId() || Net().IsHost() || !std::isfinite(s.src.x) ||
        !std::isfinite(s.src.y) || !std::isfinite(s.src.z) || !(fl > 0.5f && fl < 1.5f) || !(s.fov > 5.0f && s.fov < 170.0f))
        return;
    if (s.scene == g_sceneEnded && NowMs() - g_sceneEndedMs < 2000) return;  // a late one of a scene over (no common order)
    bool same = g_sceneHave && s.scene == g_sceneLast.scene;
    if (same && (int32_t)(s.ms - g_sceneLast.ms) <= 0) return;  // an older one, arrived late
    g_scenePrev = same && s.shot == g_sceneLast.shot ? g_sceneLast : s;
    g_sceneLast = s;
    g_sceneHave = true;
}

bool SceneWatchable(bool keep) {
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    if (!flow::InHostWorld() || OwnActivity() || g_csState != 0 || g_inScene || !host || !host->hasState) return false;
    if (me->Health() <= 0.0f || PlayerInfos()[0].playerState != 0 || InAirOrWater()) return false;  // dead, busted
    if (g_sceneLast.area != CurrentArea()) return false;
    // its people exist here only within the ped sync's reach of us (120 m on foot): a view farther away is empty
    float toHost = (host->state.pos - me->GetPosition()).Length(), toCamera = (g_sceneLast.src - me->GetPosition()).Length();
    if (toHost > (keep ? 150.0f : 100.0f) || toCamera > (keep ? 200.0f : 150.0f)) return false;
    // our own camera free: no shop, door or stunt camera of our own (m_bLookingAtPlayer, m_bPlayerIsInGarage)
    return keep || (Ref<uint8_t>(addr::TheCamera + 0x2B) && !Ref<uint8_t>(addr::TheCamera + 0x26));
}

void StartWatching() {
    const SceneSample& s = g_sceneLast;
    g_shownSrc = s.src, g_shownFront = s.front, g_shownFov = s.fov;
    g_shownScene = s.scene, g_shownShot = s.shot, g_shownMs = NowMs();
    CVector look = s.src + s.front * 10.0f;
    Cmd(0x015F).F(s.src.x).F(s.src.y).F(s.src.z).F(0.0f).F(0.0f).F(0.0f).Run();  // SET_FIXED_CAMERA_POSITION
    Cmd(0x0160).F(look.x).F(look.y).F(look.z).I(2).Run();  // POINT_CAMERA_AT_POINT, a jump cut: the script's camera
    Cmd(0x0925).Run();                                     // CAMERA_RESET_NEW_SCRIPTABLES
    Cmd(0x0930).I(1).Run();                                // CAMERA_PERSIST_POS
    Cmd(0x092F).I(1).Run();                                // CAMERA_PERSIST_TRACK
    Cmd(0x0931).I(1).Run();                                // CAMERA_PERSIST_FOV
    g_watching = true;
    FreezeForScene(true);
    LOGI("The host's scene %u on the engine: watching", s.scene);
}

void StopWatching(bool restore) {
    if (!g_watching) return;
    g_watching = false;
    Cmd(0x0925).Run();  // CAMERA_RESET_NEW_SCRIPTABLES: the persisting scene camera would hold the next one (a cutscene)
    if (restore) {
        Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT
        Cmd(0x0373).Run();  // SET_CAMERA_BEHIND_PLAYER
    }
    FreezeForScene(g_weWide && TheCameraWide());  // the host's bars still up: the scene's own rule holds us
    LOGI("The host's scene on the engine: over here");
}

// "Predict, then correct", as the copies of the players: the shot's motion carries the camera on between samples, a new
// shot or a jump of over 5 m snaps it
void ApplySceneCamera() {
    const SceneSample& l = g_sceneLast;
    const SceneSample& p = g_scenePrev;
    uint64_t now = NowMs();
    float frame = std::min(0.1f, (now - g_shownMs) / 1000.0f);
    g_shownMs = now;
    CVector v, vf;
    float vfov = 0.0f;
    uint32_t dtMs = l.ms - p.ms;
    if (p.scene == l.scene && p.shot == l.shot && dtMs >= 5 && dtMs < 1000) {
        float dt = dtMs / 1000.0f;
        v = (l.src - p.src) * (1.0f / dt), vf = (l.front - p.front) * (1.0f / dt), vfov = (l.fov - p.fov) / dt;
        if (v.Length() > 60.0f) v = vf = CVector{}, vfov = 0.0f;
    }
    float age = std::min(0.1f, (now - l.recv) / 1000.0f);
    CVector want = l.src + v * age, wantFront = l.front + vf * age;
    float wantFov = l.fov + vfov * age;
    if (l.scene != g_shownScene || l.shot != g_shownShot || (want - g_shownSrc).Length() > 5.0f) {
        g_shownSrc = want, g_shownFront = wantFront, g_shownFov = wantFov;
        g_shownScene = l.scene, g_shownShot = l.shot;
    } else {
        float k = 1.0f - std::exp(-12.0f * frame);
        g_shownSrc += (want - g_shownSrc) * k;
        g_shownFront += (wantFront - g_shownFront) * k;
        g_shownFov += (wantFov - g_shownFov) * k;
    }
    g_shownFront = g_shownFront.Normalized();
    Ref<CVector>(addr::TheCamera + 0xCE0) = g_shownSrc;                          // m_vecMoveLinear: the cam's source
    Ref<CVector>(addr::TheCamera + 0xC80) = g_shownSrc + g_shownFront * 10.0f;  // m_vecTrackLinear: where it looks
    Ref<float>(addr::TheCamera + 0xCB8) = std::clamp(g_shownFov, 5.0f, 170.0f);  // m_fFOVNew
}

// every frame before the game's (the camera's processing takes the three values at its end)
void PartnerSceneUpdate() {
    bool fresh = g_sceneHave && NowMs() - g_sceneLast.recv < 1500;
    bool watchable = fresh && SceneWatchable(g_watching);
    if (g_watching && !watchable) StopWatching(true);
    else if (!g_watching && watchable) StartWatching();
    if (g_watching) ApplySceneCamera();
}

void HandleScreen(uint8_t from, Reader& r) {
    uint8_t flags = r.U8();
    float duration = r.F32();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld()) return;
    g_hostBlack = (flags & FADE_ACTIVE) && (flags & FADE_BLACK);
    if ((flags & FADE_ACTIVE) && OwnActivity()) return;
    if (!(flags & FADE_ACTIVE)) {
        if (g_weFaded && Darkening()) Fade(0.5f, 1);
        if (g_weWide && g_csState == 0) SetWideScreen(false);
        g_weFaded = g_weWide = false;
        StopWatching(true);
        FreezeForScene(false);
        return;
    }
    bool wide = (flags & FADE_WIDE) != 0;
    if (wide != TheCameraWide() && g_csState == 0) {
        SetWideScreen(wide);
        g_weWide = wide;
    }
    // a cutscene file is the cutscene's own (it holds the partner in its scene). Not a pilot or a swimmer: they fell or sank.
    if (g_csState == 0) FreezeForScene(g_watching || (wide && !InAirOrWater() && (g_weFroze || NearHost(300.0f))));
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
        } else if (!Fading() && FadeAlpha() < 1.0f) {
            // clear and settled only: a fade in asked in this very frame leaves the alpha up for now, and a fade-out
            // still to come (our copy's cutscene ending) found nobody to bring the screen back
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
    if (!r.Ok() || block.size() != kOnscreenSize || !MissionSource(from) || !core::IsPlayerReady() || !flow::InHostWorld() ||
        OwnActivity() || minigames::OwnDance())  // our own dance counts its own score
        return;
    UseMissionTable(table);
    int32_t mainSize = Ref<int32_t>(kMainScriptSize);
    auto set = [&](uint32_t off, int32_t v) {  // off from the network: off + 4 would wrap past 0xFFFFFFFB
        if (off && mainSize >= 4 && off <= (uint32_t)mainSize - 4) Ref<int32_t>(addr::ScriptSpace + off) = v;
    };
    uint8_t* b = block.data();
    set(*(uint32_t*)(b + 0x00), values[0]);
    for (int i = 0; i < 4; ++i) {
        uint8_t* c = b + 0x40 + i * 0x44;  // COnscreenCounterEntry: variable, max variable, its text key
        uint32_t var = *(uint32_t*)c;
        if (var == kNoiseVar && !memcmp(c + 0x08, "GUNS1_5", 8)) {  // the noise meter: ours (UpdateOwnNoise)
            g_noiseMission = true;
            memset(c, 0, 0x44);  // shown here while our player is in the house, not while the host is
            continue;
        }
        if (var && var == jobs::NoiseVar()) {  // a joint burglary's noise meter: our player's (Jobs.cpp), not the owner's
            memset(c, 0, 0x44);
            continue;
        }
        set(var, values[1 + i * 2]);
        set(*(uint32_t*)(c + 0x04), values[2 + i * 2]);
    }
    // Our noise meter goes back into the slot it had, as it was: added anew each time it flashed as a new counter
    uint8_t own[0x44];
    int ownSlot = -1;
    uint32_t ownVar = g_noiseMission ? kNoiseVar : jobs::NoiseVar();
    bool had = ownVar && TakeOwnEntry(ownVar, own, ownSlot);
    memcpy((void*)kOnscreenTimer, b, kOnscreenSize);
    g_onscreenSet = true;
    if (had && FindPlayerPed(0)->AreaCode() != 0) {
        auto* t = (uint8_t*)kOnscreenTimer;
        int slot = *(uint32_t*)(t + 0x40 + ownSlot * 0x44) == 0 ? ownSlot : -1;
        for (int i = 0; slot < 0 && i < 4; ++i)
            if (*(uint32_t*)(t + 0x40 + i * 0x44) == 0) slot = i;
        if (slot >= 0) memcpy(t + 0x40 + slot * 0x44, own, 0x44);
    }
    if (g_noiseMission) ShowOwnNoise(FindPlayerPed(0)->AreaCode() != 0);
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
    g_hostMission = name;
    LOGI("Host mission started (%s)", name.c_str());
}

void PartnerMissionCleanup() {
    if (core::IsPlayerReady()) StopWatching(true);
    RestoreOwnLook();
    ReleaseMissionIfps();
    if (g_noiseMission && core::IsPlayerReady()) ShowOwnNoise(false);
    g_noiseMission = false;
    // what the mission did not take back stays (the host keeps it too); a gun of the partner's own it went over keeps
    // the larger count (Emmet's pistol is left with 10)
    if (CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr)
        for (auto& [weapon, own] : g_replaced)
            for (int i = 0; i < NUM_WEAPON_SLOTS; ++i)
                if (own.first == weapon && me->Weapons()[i].type == weapon && me->Weapons()[i].ammoTotal < own.second)
                    Cmd(0x017B).I(GetPedRef(me)).I(weapon).I(std::min(own.second, 99999)).Run();  // SET_CHAR_AMMO
    g_missionWeapons.clear();
    g_replaced.clear();
    if (g_policyApplied && core::IsPlayerReady()) ApplyPolicy(g_ownPolicy);
    g_partnerEdits.clear();
    if (g_gateOpenSet) Ref<int32_t>(addr::ScriptSpace + kAirportGateOpen) = 0, g_gateOpenSet = false;  // as DRIV3's end
    g_policyApplied = false;
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
    FreezeForScene(false);
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

// The destination markers drawn during the last frames. The other game draws them only within 1 s of the latest list
// (a host gone or paused takes them away): an unchanged list goes again every 500 ms, or a fixed destination's marker
// went out after a second there.
void SendAreas(uint64_t now) {
    Writer w(proto::G_MARKER);
    std::vector<Area> live;
    for (auto it = g_areas.begin(); it != g_areas.end();) {
        if (now - it->second.seenMs > 300) {
            it = g_areas.erase(it);
            continue;
        }
        if (it->second.mission) live.push_back(it->second);
        ++it;
    }
    w.U8((uint8_t)std::min<size_t>(live.size(), 40));  // Kickstart's 33 coronas and its markers
    for (size_t i = 0; i < live.size() && i < 40; ++i) {
        const Area& a = live[i];
        w.U32(a.id);
        w.U8(a.kind);
        w.F32(a.x1);
        w.F32(a.y1);
        w.F32(a.x2);
        w.F32(a.y2);
        w.F32(a.z);
    }
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

// Another's mission's markers while their list is fresh (stale after 1 s: that game gone or paused), each frame before
// CGame::Process: its CSpecialFX::Update makes the 3D markers placed so far this frame active (C3dMarkers::Update), and
// one placed after it is destroyed unseen when the frame is drawn (Render3dMarkers). Placed after the frame's process
// as the scripts' blips are, the partner never saw a red cylinder of the host's mission.
void PlaceMissionMarkers() {
    if (NowMs() - g_partnerAreasMs < 1000)
        for (const Area& a : g_partnerAreas) {
            if (a.kind == AREA_SPHERE) {
                DrawSphere(a.id ^ 0x40000000u, a);
            } else if (a.kind == AREA_CORONA) {
                CVector at{a.x1, a.y1, a.z};
                uint32_t c = (uint32_t)a.y2;
                Call<0x6FC580>(a.id ^ 0x40000000u, (CEntity*)nullptr, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)255, &at,
                               a.x2, 450.0f, (uint8_t)0, (uint8_t)0, (uint8_t)1, (uint8_t)0, (uint8_t)0, 0.0f, false, 1.5f, false, 15.0f,
                               false, false);  // CCoronas::RegisterCorona, as DRAW_CORONA
            } else {
                g_origHighlight(a.id ^ 0x40000000u, a.x1, a.y1, a.x2, a.y2, a.z);
            }
        }
}

// The blips of another's mission's entities whose copy came later
void ShowMissionMarks() {
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

// ---------------------------------------------------------------- shared side activities: the call and its answers
std::string OtherName() { return Net().Peers().empty() ? std::string() : Net().PeerName(Net().Peers().begin()->first); }
bool CanCall() {  // not from our own taxi job in the other's (Side::parallel)
    return !g_side.name.empty() && !SideOwning() && !SideJoined() && !g_side.offered && NowMs() < g_side.offerUntil &&
           !Net().Peers().empty();
}
// what this game showed of the activity it joined goes with it
// The owner's stadium script takes only its own player out of the arena, which has no door: who joined goes out at that
// script's own exit (2 m aside of it, the stadium's marker stands a few metres off and would start a run of our own)
void LeaveArena() {
    struct Exit {
        const char* job;
        int8_t area;
        float x, y, z;
    };
    static const Exit kExits[] = {{"kicksta", 14, 1105.210f, 1604.649f, 12.055f}, {"blood", 15, -2126.3f, -440.6f, 34.5f},
                                  {"cprace", 7, 2693.184f, -1699.655f, 9.389f},     {"cprace", 4, 1101.781f, 1604.578f, 11.551f}};
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !core::IsPlayerReady()) return;
    for (const Exit& e : kExits) {
        if (g_side.joined != e.job || me->AreaCode() != e.area) continue;
        int h = GetPedRef(me);
        if (me->InVehicle()) Cmd(0x0362).I(h).F(e.x + 2.0f).F(e.y).F(e.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        Cmd(0x04BB).I(0).Run();                                                 // SET_AREA_VISIBLE
        Cmd(0x0860).I(h).I(0).Run();                                            // SET_CHAR_AREA_VISIBLE
        Cmd(0x00A1).I(h).F(e.x + 2.0f).F(e.y).F(e.z).Run();                     // SET_CHAR_COORDINATES
        LOGI("Out of the %s arena (area %d) as the joint run ended", e.job, e.area);
        return;
    }
}

void DropJoined() {
    LeaveArena();
    g_side.owner = 0xFF;
    g_side.joined.clear();
    g_side.boardUntil = 0;
    g_side.parallel = false;
    g_side.taxiAt = 0;
    ClearPartnerBlips();
    g_partnerBlipRecords.clear();
    g_partnerAreas.clear();
    ClearPartnerMarked();
    ClearPartnerOnscreen();
}
// joined a taxi job, our own one starts: what the other's showed here goes (its marks and timers over our own job's),
// to come back when ours is over (SIDE_RESYNC)
void StartParallel() {
    g_side.parallel = true;
    ClearPartnerBlips();
    g_partnerBlipRecords.clear();
    g_partnerAreas.clear();
    ClearPartnerMarked();
    ClearPartnerOnscreen();
    LOGI("Our own taxi job next to %s's: the fares' money goes to both", Net().PeerName(g_side.owner).c_str());
}
// the money our own job brought since its start, less what the other's paid us meanwhile: the other player gets it too
void PayOther(uint8_t to) {
    if (int32_t earned = PlayerInfos()[0].money - g_side.moneyAtStart - g_side.received; earned > g_side.paid) {
        SendSide(SIDE_PAY, to, g_side.name, earned - g_side.paid);
        g_side.paid = earned;
    }
}
// this game has an activity or a mission of its own now: one at a time
void LeaveSide() {
    if (!SideJoined()) return;
    SendSide(SIDE_LEAVE, g_side.owner, g_side.joined);
    LOGI("Left %s's side activity (%s)", Net().PeerName(g_side.owner).c_str(), g_side.joined.c_str());
    DropJoined();
}
void ForgetSent() {  // the owner's copy of what went to the joined game
    g_sentBlips.clear();
    g_sentMarked.clear();
    g_lastOnscreen.clear();
    g_lastAreasMsg.clear();
}

void CourierUpdate();

// Who joins another's job is brought to it (27.10: the partner had to drive over first): next to its owner, then at the
// wheel of a vehicle like the owner's beside it where each works with its own (the taxi, the ambulance, the fire truck,
// the police car: both work, Jobs.cpp; races, bikes; a pimp's car has its one seat for the girl), or in a free seat of
// the owner's vehicle where the job's people leave one (the burglar's van, the truck). Tried twice a second for 15 s:
// the owner's vehicle copy comes with its model, the spot needs room.
void BoardSide() {
    if (!g_side.boardUntil || NowMs() - g_side.boardTryMs < 500) return;
    g_side.boardTryMs = NowMs();
    RemotePlayer* owner = SideJoined() ? Players::Instance().Get(g_side.owner) : nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    if (!owner || NowMs() > g_side.boardUntil) {
        if (owner) LOGI("Not brought to %s's vehicle in time", Net().PeerName(g_side.owner).c_str());
        g_side.boardUntil = 0;
        return;
    }
    const std::string& job = g_side.joined;
    // indoors: the owner's booth, a step behind it at its left (SHRANGE's 4 ranges: the middle booth, the heading down the lanes)
    if (job == "shrange") {
        static const float kBooths[4][4] = {{293.684f, -24.555f, 1000.523f, 0.0f}, {303.079f, -64.173f, 1000.523f, 270.0f},
                                            {300.102f, -133.906f, 1003.055f, 90.0f}, {299.452f, -163.346f, 998.610f, 90.0f}};
        if (!owner->hasState || !owner->state.area) return;
        for (const auto& b : kBooths) {
            CVector booth{b[0], b[1], b[2] + 1.0f}, d = owner->state.pos - booth;
            if (std::fabs(d.x) > 25.0f || std::fabs(d.y) > 25.0f || std::fabs(d.z) > 10.0f) continue;
            float h = b[3] * 0.0174533f;
            CVector ahead{-std::sin(h), std::cos(h), 0.0f}, across{std::cos(h), std::sin(h), 0.0f};
            Players::Instance().GoTo(*owner, booth - across * 0.8f - ahead * 0.6f, true);
            Cmd(0x0173).I(GetPedRef(me)).F(b[3]).Run();  // SET_CHAR_HEADING
            g_side.boardUntil = 0;
            ui::TheChat().AddSystem(Format(TR("Вы рядом с игроком %s.", "You are next to %s."), Net().PeerName(g_side.owner).c_str()));
            return;
        }
        return;
    }
    bool stadium = StadiumJob(job), quarry = job == "quarry";
    // jobs are outdoors, but a stadium's arena
    if (!owner->ped || !owner->hasState || (owner->state.area != 0 && !stadium) || CutsceneRunning()) return;
    CVector at = owner->ped->GetPosition();
    bool ownVehicle = job == "bcour" || job == "mtbiker" || job == "stunt" || job == "cprace" || job == "tria" || job == "pimp" ||
                      job == "taxiodd" || job == "ambulan" || job == "firetru" || job == "copcar" || job == "kicksta" ||
                      job == "blood" || quarry;
    // a passenger of its vehicle already: out first, a vehicle of our own beside it has no room inside its own
    bool inTheirs = ownVehicle && me->InVehicle() && owner->ped->InVehicle() && me->Vehicle() == owner->ped->Vehicle();
    // the quarry starts on foot by its office (its sanchez only takes its player to the dozer or the dumper): our own vehicle
    // nearby is not taken from under us meanwhile
    bool riding = quarry && me->InVehicle() && me->Vehicle()->Driver() == me;
    if ((me->GetPosition() - at).Length() > (riding ? 300.0f : 30.0f) || inTheirs || me->AreaCode() != owner->state.area) {
        float h = owner->ped->CurrentRotation();
        Players::Instance().GoTo(*owner, at + CVector{std::cos(h) * 3.0f, std::sin(h) * 3.0f, 0.0f}, true);  // its right hand
        LOGI("Over to %s for the joint job", Net().PeerName(g_side.owner).c_str());
        return;
    }
    Players::Ride ride = Players::Instance().RideWith(*owner, !ownVehicle);
    if (ride == Players::Ride::Later) return;
    // Kickstart's player walks to its bike after the intro, a stadium race seats its player only after the marker, the
    // quarry's drives a dozer or a dumper only after its sanchez: tried again until then (one like the owner's for us)
    if (stadium && ride == Players::Ride::None && owner->mode == RemotePlayer::Mode::OnFoot) return;
    int model = owner->ped->InVehicle() ? owner->ped->Vehicle()->ModelIndex() : -1;
    if (quarry && !((model == 486 || model == 406) && me->InVehicle() && me->Vehicle()->ModelIndex() == model)) return;  // DOZER, DUMPER
    g_side.boardUntil = 0;
    if (ride == Players::Ride::OwnVehicle && job == "taxiodd") g_side.taxiAt = NowMs() + 1500;
    std::string who = Net().PeerName(g_side.owner);
    if (ride == Players::Ride::Seat)
        ui::TheChat().AddSystem(Format(TR("Вы в машине игрока %s.", "You are in %s's vehicle."), who.c_str()));
    else if (ride == Players::Ride::OwnVehicle)
        ui::TheChat().AddSystem(Format(TR("Вы за рулём такой же машины, как у игрока %s.", "You drive one like %s's."), who.c_str()));
    else
        ui::TheChat().AddSystem(Format(TR("Вы рядом с игроком %s.", "You are next to %s."), who.c_str()));
}

// Joined a taxi job and seated at the wheel of a taxi of our own: our own job starts by itself, as R3 starts it at 2
// (28.10: the second player was told to press 2 and did not, drove to the owner's fare instead, and it got in the
// owner's taxi only)
void StartOwnTaxi() {
    if (!g_side.taxiAt || NowMs() < g_side.taxiAt) return;
    g_side.taxiAt = 0;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
    if (!SideJoined() || g_side.joined != "taxiodd" || g_side.parallel || FindMissionScript() || core::IsOnMission() || !v ||
        v->Driver() != me || (v->ModelIndex() != 420 && v->ModelIndex() != 438))  // IS_CHAR_IN_TAXI: TAXI, CABBIE
        return;
    Ref<int32_t>(addr::ScriptSpace + 728) = 1;  // $728, R3's own after its launch: no second one
    Cmd(0x00BA).S8("TAXI_M").I(6000).I(5).Run();  // PRINT_BIG, as R3
    Cmd(0x0417).I(121).Run();                     // LOAD_AND_LAUNCH_MISSION_INTERNAL TAXIODD
    LOGI("Our own taxi job started by itself at the wheel of our taxi");
}

void SideUpdate() {
    void* script = FindMissionScript();
    // Our own one its launcher started as we joined the other's ends at its first command (HookStartNewScript); taken for
    // an activity of ours meanwhile, it made us leave the joint one in the same frame (the BMX rings, 28.10)
    if (const uint8_t* ip = script ? Field<const uint8_t*>(script, 0x14) : nullptr; ip && ip[0] == 0x4E && ip[1] == 0x00)
        script = nullptr;  // TERMINATE_THIS_SCRIPT
    if (jobs::RangeWaits(script)) script = nullptr;  // the range runs from the room's door on: its round is the activity
    if (!script) script = jobs::ValetShift();  // the valet's shift: a streamed script, no mission
    std::string name = script ? std::string(Field<char[8]>(script, 0x08), strnlen(Field<char[8]>(script, 0x08), 8)) : "";
    bool shareable = SideJob(name) && !(Net().IsHost() && HostOnMission());
    if (script && SideJoined() && !g_side.parallel) LeaveSide();
    if (shareable && name != g_side.name) {  // ours started: who joins sees its marks from its start
        g_side.name = name;
        g_side.offerUntil = NowMs() + 20000;
        g_side.offered = false;
        g_side.moneyAtStart = PlayerInfos()[0].money;
        g_side.paid = 0;
        g_side.received = 0;
        g_baseline.clear();
        for (int i = 0; i < kNumTraces; ++i)
            if (Ref<uint8_t>(Trace(i) + 0x25) & 2) g_baseline.insert(((uint32_t)Ref<uint16_t>(Trace(i) + 0x14) << 8) | (uint32_t)i);
        ForgetSent();
        LOGI("Own side activity started (%s): the other player may be called in", name.c_str());
    } else if (!shareable && !g_side.name.empty()) {  // ours is over
        if (SideOwning()) {
            // the rest of it
            int32_t earned = std::max(0, PlayerInfos()[0].money - g_side.moneyAtStart - g_side.received - g_side.paid);
            Writer w(proto::G_BLIP);
            w.U8(BLIP_CLEAR);
            Net().Send(proto::CH_RELIABLE, w);
            SendSide(SIDE_END, g_side.with, g_side.name, earned);
            LOGI("Shared side activity over (%s): $%d more for %s too", g_side.name.c_str(), earned, Net().PeerName(g_side.with).c_str());
        } else if (g_side.parallel && SideJoined()) {
            PayOther(g_side.owner);  // the rest of our own taxi job's
        }
        g_side.name.clear();
        g_side.with = 0xFF;
        g_side.offered = false;
        g_side.offerUntil = 0;
        ForgetSent();
    }
    if (g_side.parallel && !script) {  // our own taxi job is over: the other's shows here again
        g_side.parallel = false;
        if (SideJoined()) SendSide(SIDE_RESYNC, g_side.owner, g_side.joined);
        LOGI("Our own taxi job over: back to %s's", Net().PeerName(g_side.owner).c_str());
    }
    uint64_t now = NowMs();
    if (SideOwning() || g_side.parallel) {
        static uint64_t tick = 0;
        if (now - tick >= 250) {
            tick = now;
            if (SideOwning()) {
                HostScanBlips(false);
                HostSendOnscreen();
                HostSendMarked(false);
                SendAreas(now);
            }
            // who joined is paid each fare as it comes, not all at the end: it saw no money while it helped, and none at
            // all when it left before the end (#73). Its own taxi job in ours pays us so (Side::parallel).
            PayOther(SideOwning() ? g_side.with : g_side.owner);
        }
    }
    if (SideJoined() && Net().IsHost()) ShowMissionMarks();  // the partner does it in PartnerUpdate
    if (g_side.inviteFrom != 0xFF && (now > g_side.inviteUntil || script)) {  // unanswered, or busy now
        SendSide(SIDE_DECLINE, g_side.inviteFrom, g_side.inviteName);
        g_side.inviteFrom = 0xFF;
    }
    StartOwnTaxi();
    BurglaryLook(SideJoined() && g_side.joined == "burgjb");
    CourierUpdate();
    // a joint burglary: a thing of ours into the van, a thing we lifted, the residents woken for us (Jobs.cpp)
    const std::string& joint = SideOwning() ? g_side.name : g_side.joined;
    uint8_t other = SideOwning() ? g_side.with : g_side.owner;
    for (const jobs::Tell& t : jobs::Update(SideJoined() ? g_side.joined : std::string(), SideOwning() ? g_side.name : std::string())) {
        if (t.loot) SendSide(SIDE_LOOT, other, joint);
        auto at = [&](uint8_t op, int32_t value) {
            Writer w(proto::G_SIDE);
            w.U8(op), w.U8(other), w.Str(joint, 8), w.I32(value), w.Vec({t.x, t.y, t.z});
            Net().Send(proto::CH_RELIABLE, w);
        };
        if (t.taken) at(SIDE_TAKEN, t.model);
        if (t.alarm) at(SIDE_ALARM, 0);
    }
}

// The courier for two (#28): only the leader's script (BCOUR) threw packages, at its player's button, and counted those
// in its rings, so who joined could not deliver. Its game throws one of its own the same way (look left or right and
// fire, from its vehicle: across at 30 m/s with a lift, at 15 by a ring), and one of its in a ring (the leader's, their
// copies here) goes to the leader: its game puts its own last package there (763@ at 793@, made anew when gone), and
// the script's own check (LOCATE_OBJECT_3D) counts it.
struct Thrown {
    int handle;
    uint64_t bornMs;
};
std::vector<Thrown> g_thrown;
uint64_t g_throwMs = 0;
int g_courierPackage = -1;  // leader: a package made for a delivery of the other's, until the script took it
uint64_t g_courierPackageMs = 0;

void CourierUpdate() {
    int model = ResolvedModel(-228);  // the courier's package
    uint64_t now = NowMs();
    if (g_courierPackage != -1 && now - g_courierPackageMs > 10000) {  // the script did not take it (its player got off)
        if (GetObject(g_courierPackage)) Cmd(0x0108).I(g_courierPackage).Run();  // DELETE_OBJECT
        g_courierPackage = -1;
    }
    bool joined = SideJoined() && !_stricmp(g_side.joined.c_str(), "bcour") && IsObjectModel(model);
    for (auto it = g_thrown.begin(); it != g_thrown.end();) {
        CObject* o = GetObject(it->handle);
        bool hit = false;
        if (o && joined)
            for (auto& [slot, placed] : g_placed) {
                CVector d = o->GetPosition() - placed.first.pos;
                if (slot >= 32 || std::fabs(d.x) > 1.2f || std::fabs(d.y) > 1.7f || std::fabs(d.z) > 3.0f) continue;
                Writer w(proto::G_SIDE);
                w.U8(SIDE_DROP);
                w.U8(g_side.owner);
                w.Str(g_side.joined, 8);
                w.I32(0);
                w.Vec({placed.first.pos.x, placed.first.pos.y, placed.first.pos.z});
                Net().Send(proto::CH_RELIABLE, w);
                LOGI("Our courier package went into a ring at %.1f,%.1f,%.1f", placed.first.pos.x, placed.first.pos.y,
                     placed.first.pos.z);
                hit = true;
                break;
            }
        if (!o || hit || !joined || now - it->bornMs > 8000) {
            if (o) Cmd(0x0108).I(it->handle).Run();  // DELETE_OBJECT
            it = g_thrown.erase(it);
        } else {
            ++it;
        }
    }
    if (!joined) return;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr;
    CPad* pad = GetPad(0);
    bool left = pad->NewState.LeftShoulder2 != 0, right = pad->NewState.RightShoulder2 != 0;
    if (!IsModelLoaded(model)) RequestModel(model);
    if (!v || !(left || right) || !pad->NewState.ButtonCircle || pad->OldState.ButtonCircle || pad->DisablePlayerControls ||
        now - g_throwMs < 300 || g_thrown.size() >= 8 || !IsModelLoaded(model) || !HasRoom(ObjectPool(), kKeepObjects, "object"))
        return;
    g_throwMs = now;
    bool byRing = false;  // as the script: a short drop when its player is within 5 m of a ring
    for (auto& [slot, placed] : g_placed) {
        CVector d = me->GetPosition() - placed.first.pos;
        if (slot < 32 && std::fabs(d.x) < 5.0f && std::fabs(d.y) < 5.0f && std::fabs(d.z) < 5.0f) byRing = true;
    }
    CVector at = v->GetPosition();
    int h = -1;
    Cmd(0x029B).I(model).F(at.x).F(at.y).F(at.z).Out(&h).Run();  // CREATE_OBJECT_NO_OFFSET
    if (h == -1) return;
    float side = right ? -1.0f : 1.0f;  // look right (E) throws as BCOUR's first subroutine, look left (Q) as its second
    Cmd(0x0382).I(h).I(1).Run();  // SET_OBJECT_COLLISION
    Cmd(0x0392).I(h).I(1).Run();  // SET_OBJECT_DYNAMIC
    Cmd(0x035C).I(h).I(GetVehicleRef(v)).F(byRing ? side : 0.0f).F(0.0f).F(byRing ? 1.3f : 0.7f).Run();  // PLACE_OBJECT_RELATIVE_TO_CAR
    CMatrix* m = v->Matrix();
    float heading = m ? std::atan2(-m->forward.x, m->forward.y) : me->CurrentRotation();
    float speed = (right ? 1.0f : -1.0f) * (byRing ? 15.0f : 30.0f);
    Cmd(0x038C).I(h).F(std::cos(heading) * speed).F(std::sin(heading) * speed).F(byRing ? 1.0f : 8.5f).Run();  // ADD_TO_OBJECT_VELOCITY
    g_thrown.push_back({h, now});
}

// leader: the other's package went into one of our rings
void CourierDelivery(const CVector& ring) {
    void* script = FindMissionScript();
    if (!script || _strnicmp(Field<char[8]>(script, 0x08), "bcour", 8)) return;
    int last = MissionLocal(793);  // the script's last package: 763@(793@), 30 of them
    if (last < 0 || last >= 30) return;
    int32_t& handle = Ref<int32_t>(0xA48960 + (uintptr_t)(763 + last) * 4);  // CTheScripts::LocalVariablesForCurrentMission
    CObject* o = GetObject(handle);
    int model = ResolvedModel(-228);
    if (!o) {
        if (!IsModelLoaded(model) || !HasRoom(ObjectPool(), kKeepObjects, "object")) return;
        int h = -1;
        Cmd(0x029B).I(model).F(ring.x).F(ring.y).F(ring.z).Out(&h).Run();  // CREATE_OBJECT_NO_OFFSET
        if (h == -1 || !(o = GetObject(h))) return;
        handle = h;
        g_courierPackage = h, g_courierPackageMs = NowMs();
    }
    Cmd(0x01BC).I(handle).F(ring.x).F(ring.y).F(ring.z).Run();  // SET_OBJECT_COORDINATES
    o->MoveSpeed() = CVector{};
    LOGI("The other player's courier package into our ring at %.1f,%.1f,%.1f: our package %d put there", ring.x, ring.y,
         ring.z, handle);
}

void HandleSide(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8();
    std::string name = r.Str(8);
    int32_t money = r.I32();
    if (!r.Ok() || (to != 0xFF && to != Net().MyId()) || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
    bool job = SideJob(name);
    std::string who = Net().PeerName(from);
    if (op == SIDE_OFFER) {
        void* own = FindMissionScript();
        if (name == "shrange" && jobs::RangeWaits(own)) own = nullptr;  // ours waiting at its marker ends as we join (Jobs.cpp)
        bool busy = own || jobs::ValetShift() || SideJoined() || (Net().IsHost() ? HostOnMission() : HostMissionRecent(0));
        if (!job || busy) {
            SendSide(SIDE_DECLINE, from, name);
            LOGI("%s's call to %s declined: busy", who.c_str(), name.c_str());
            return;
        }
        g_side.inviteFrom = from, g_side.inviteName = name, g_side.inviteUntil = NowMs() + 20000;
        LOGI("%s calls us in (%s)", who.c_str(), name.c_str());
    } else if (op == SIDE_ACCEPT) {
        if (name != g_side.name || SideOwning()) {  // over already, or the other joined first
            SendSide(SIDE_END, from, name);
            return;
        }
        g_side.with = from, g_side.offered = false;
        ForgetSent();  // all it has now goes to the newcomer
        if (name == "kicksta" || name == "blood") contest::Drop();  // one score for both now
        ui::TheChat().AddSystem(Format(TR("%s принимает приглашение на %s.", "%s accepts your invitation to %s."), who.c_str(),
                                       JobTo(name).c_str()));
        // what the one who joined is told, the one who called is too: it gets a vehicle of the job and works too
        if (const char* hint = CallerHint(name)) ui::TheChat().AddSystem(Format(hint, who.c_str()));
        LOGI("%s joined our side activity (%s)", who.c_str(), name.c_str());
    } else if (op == SIDE_DECLINE) {
        if (name != g_side.name || !g_side.offered) return;
        g_side.offered = false;
        ui::TheChat().AddSystem(Format(TR("%s отклоняет приглашение на %s.", "%s declines your invitation to %s."), who.c_str(),
                                       JobTo(name).c_str()));
    } else if (op == SIDE_END) {
        if (!SideJoined() || from != g_side.owner) return;
        DropJoined();
        ui::TheChat().AddSystem(Format(TR("Конец %s.", "That is the end of %s."), JobOf(name).c_str()));
        if (money > 0 && money <= 10000000) {
            PlayerInfos()[0].money += money;
            g_side.received += money;  // our own taxi job goes on: not its money
            ui::TheChat().AddSystem(Format(TR("Награда за задание: $%d.", "Reward for the job: $%d."), money));
        }
        LOGI("%s's side activity over (%s), $%d", who.c_str(), name.c_str(), money);
    } else if (op == SIDE_DROP) {
        Vec3 p = r.Vec();
        if (r.Ok() && SideOwning() && from == g_side.with && !_stricmp(name.c_str(), "bcour")) CourierDelivery({p.x, p.y, p.z});
    } else if (op == SIDE_PAY) {
        // the owner's job's money, or the fares of the taxi job of its own who joined our taxi job drives (Side::parallel)
        bool owner = SideJoined() && from == g_side.owner, helper = SideOwning() && from == g_side.with;
        if ((!owner && !helper) || money <= 0 || money > 10000000) return;
        PlayerInfos()[0].money += money;
        g_side.received += money;
        ui::TheChat().AddSystem(owner ? Format(TR("За задание: +$%d.", "For the job: +$%d."), money)
                                      : Format(TR("За пассажиров игрока %s: +$%d.", "For %s's fares: +$%d."), who.c_str(), money));
        LOGI("%s's side activity paid us $%d", who.c_str(), money);
    } else if (op == SIDE_RESYNC) {
        if (!SideOwning() || from != g_side.with) return;
        ForgetSent();  // its own taxi job is over: what ours shows goes to it again
        LOGI("%s is back to our side activity (%s)", who.c_str(), name.c_str());
    } else if (op == SIDE_LOOT) {
        if (!SideOwning() || from != g_side.with) return;
        if (int things = jobs::Stashed())
            ui::TheChat().AddSystem(Format(TR("%s кладёт вещь в фургон. Вещей в фургоне: %d.", "%s puts a thing into the van. Things in it: %d."),
                                           who.c_str(), things));
    } else if (op == SIDE_TAKEN || op == SIDE_ALARM) {  // a joint burglary's house: the same in both games (Jobs.cpp)
        Vec3 p = r.Vec();
        bool partner = (SideOwning() && from == g_side.with) || (SideJoined() && from == g_side.owner);
        if (!r.Ok() || !partner || name != "burgjb") return;
        if (op == SIDE_TAKEN) jobs::Taken(money, p.x, p.y, p.z);
        else jobs::Alarm(p.x, p.y, p.z);
    } else if (op == SIDE_LEAVE) {
        if (from != g_side.with) return;
        g_side.with = 0xFF;
        ui::TheChat().AddSystem(Format(TR("%s выходит из %s.", "%s leaves %s."), who.c_str(), JobOf(name).c_str()));
        LOGI("%s left our side activity (%s)", who.c_str(), name.c_str());
    }
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
    HostSendPolicy(true);
    for (const MapEdit& e : g_mapEdits) SendMapEdit(e);
    for (const IplEdit& e : g_iplEdits) SendIpl(e);
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
        // CTheScripts::Process -> CRunningScript::Process, its only call. Not its entry: our jump there ended on 0x469F04,
        // where another plugin puts a call of its own, and the game jumped to E8xxxxxx on the first script (issue #52)
        mem::RedirectCallFn(0x46A21B, &HookScriptProcess, &g_origScriptProcess);
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
        Net().SetHandler(proto::G_MISSION_POLICY, &HandlePolicy);
        Net().SetHandler(proto::G_MAP_EDIT, &HandleMapEdit);
        Net().SetHandler(proto::G_IPL, &HandleIpl);
        mem::RedirectCallFn(0x4831D9, &HookSpottedChar, &g_origCanSee);  // HAS_CHAR_SPOTTED_CHAR
        mem::RedirectCallFn(0x47B2A0, &HookSpottedChar, &g_origCanSee);  // HAS_CHAR_SPOTTED_CHAR_IN_FRONT
        mem::RedirectCallFn(0x46B6ED, &HookScriptIpl<0>, &g_origIpl[0]);  // REQUEST_IPL
        mem::RedirectCallFn(0x46B715, &HookScriptIpl<1>, &g_origIpl[1]);  // REMOVE_IPL
        mem::RedirectCallFn(0x46B73D, &HookScriptIpl<2>, &g_origIpl[2]);  // REMOVE_IPL_DISCREETLY
        Net().SetHandler(proto::G_VEH_SCRIPT, &HandleCarCommand);
        // The scripts' commands: CRunningScript::Process reads each one and calls its handler itself (ProcessOneCommand,
        // 0x469EB0, has no caller in 1.0 US: a hook on it saw only this mod's own commands, never a mission's). Its call
        // (push ecx; mov ecx, esi; call edx; test al, al) goes through the stub; CLEO changes only the table's address.
        static const uint8_t kHandlerCall[] = {0x51, 0x8B, 0xCE, 0xFF, 0xD2, 0x84, 0xC0};
        if (mem::Check(0x469FF2, kHandlerCall, sizeof kHandlerCall)) {
            uint8_t call[sizeof kHandlerCall] = {0xE8, 0, 0, 0, 0, 0x90, 0x90};
            int32_t rel = (int32_t)((uintptr_t)&CoopScriptCommandStub - (0x469FF2 + 5));
            memcpy(call + 1, &rel, 4);
            mem::Write(0x469FF2, call, sizeof call);
        } else {
            LOGW("Script commands: unexpected bytes at 0x469FF2 (another mod?), the mission's commands are not watched");
        }
        Net().SetHandler(proto::G_FADE, &HandleScreen);
        Net().SetHandler(proto::G_ONSCREEN, &HandleOnscreen);
        Net().SetHandler(proto::G_MISSION_START, &HandleMissionStart);
        Net().SetHandler(proto::G_MISSION_END, &HandleMissionEnd);
        Net().SetHandler(proto::G_MISSION_REWARD, &HandleReward);
        Net().SetHandler(proto::G_MISSION_WEAPON, &HandleMissionWeapon);
        Net().SetHandler(proto::G_CHECKPOINTS, &HandleMarked);
        Net().SetHandler(proto::G_CONTACTS, &HandleContacts);
        Net().SetHandler(proto::G_SIDE, &HandleSide);
        Net().SetHandler(proto::G_SCENE_CAM, &HandleSceneCam);
        Net().SetHandler(proto::G_MISSION_LOOK, &HandleMissionLook);
        Net().SetHandler(proto::G_MISSION_IFP, &HandleMissionIfp);
        Net().SetHandler(proto::G_MISSION_FACE, &HandleMissionFace);
        Net().SetHandler(proto::G_CAR_DOOR, &HandleCarDoor);
        Net().SetHandler(proto::G_MISSION_LOOK_AT, &HandleLookAt);
        Net().SetHandler(proto::G_PED_NO_COLLIDE, &HandleNoCollide);
        Net().SetHandler(proto::G_PED_ATTACH, &HandlePersonOnCar);
        Net().SetHandler(proto::G_MISSION_HINT, &HandleHint);
        Net().SetHandler(proto::G_NOISE, [](uint8_t from, Reader&) {
            if (Net().IsHost() && from != Net().MyId()) g_partnerLoudMs = NowMs();
        });
    }

    void OnPreProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (!Net().IsHost()) {
            PartnerSceneUpdate();
            if (g_hostWaits) g_origHighlight(g_waitArea.id, g_waitArea.x1, g_waitArea.y1, g_waitArea.x2, g_waitArea.y2, g_waitArea.z);
        }
        if (!Net().IsHost() || SideJoined()) PlaceMissionMarkers(), TurnMarked();
    }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        HealInheritedFlag();  // in a game of one's own too: such a save is broken there as well
        if (!Net().Online()) return;
        if (Net().IsHost()) HostUpdate();
        else PartnerUpdate(), ShowHostLines();
        SideUpdate();
        BoardSide();
        if (!g_personOnCar.empty()) ApplyPersonsOnCars();
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
        g_partnersAt.erase(id);
        if (id == g_callDeclined) g_callDeclined = 0xFF;
        if (id == g_side.with) g_side.with = 0xFF;  // a shared side activity goes on alone
        if (id == g_side.inviteFrom) g_side.inviteFrom = 0xFF;
        if (id == g_side.owner && core::IsPlayerReady()) DropJoined();
        // the host gone in the middle of its mission (its game crashed) while a dedicated server goes on: its fade-out
        // had no fade-in to follow, and the partner's screen stayed dark
        if (Net().IsHost() || id != Net().HostId() || !core::IsPlayerReady()) return;
        if (g_csState == 2) PartnerEndCutscene();
        if (g_csState != 0) PartnerClearCutscene();
        if (g_partnerMission) g_partnerMission = false, g_partnerMissionEndedAt = NowMs();  // no trainer at the gym after it (#34)
        PartnerMissionCleanup();
    }
    void OnSessionEnd() override {
        if (core::IsPlayerReady()) {  // the host's cutscene, blips, markers and fades stay no longer
            if (g_csState == 2) PartnerEndCutscene();
            if (g_csState != 0) PartnerClearCutscene();
            // left in the middle of the host's mission: the weapons it gave go as its end would have taken them (the
            // AK with 99999 rounds stayed for good after a dropped connection)
            if (!g_missionWeapons.empty()) {
                std::vector<int> given(g_missionWeapons.begin(), g_missionWeapons.end());
                for (int weapon : given) TakeBackMissionWeapon(FindPlayerPed(0), weapon);
                LOGI("Left the host's mission: %zu weapon(s) it gave taken back", given.size());
            }
            PartnerMissionCleanup();
            for (const Thrown& p : g_thrown)  // courier packages in the air
                if (GetObject(p.handle)) Cmd(0x0108).I(p.handle).Run();  // DELETE_OBJECT
            if (g_courierPackage != -1 && GetObject(g_courierPackage)) Cmd(0x0108).I(g_courierPackage).Run();
            jobs::Update("", "");  // the burglary houses shut again in a game of one's own, its meter gone
            BurglaryLook(false);
        }
        g_thrown.clear();
        g_courierPackage = -1;
        Reset();
        g_atSent = 0;  // kept over a game's restart: the host learns the load is over from the change
    }
    void OnGameStarted() override {
        // a world loaded in the middle of the host's mission: what it gave is still to be taken back at its end (the
        // reload gives the partner its weapons again, the given ones with them)
        std::set<int> given = g_partnerMission ? g_missionWeapons : std::set<int>{};
        Reset();
        g_thrown.clear();  // the old world's objects: their handles may be new ones' now
        g_courierPackage = -1;
        g_missionWeapons = std::move(given);
        g_inheritedFlagMs = core::IsOnMission() ? NowMs() : 0;
        g_lastPassed.assign((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));  // what the loaded game had
        g_progressAt = 0;
        ClearTruckLeftovers();
        jobs::WorldLoaded();
        g_burglaryDressed = false;  // the loaded world brings its own look
        jobs::Update("", "");  // the loaded world shut its burglary houses: opened again if a joint burglary goes on
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
            if (g_hostScene) SendSceneEnd();
        }
        Reset();
    }

private:
    static void Reset() {
        g_side = Side{};
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
        g_partnersAt.clear();
        g_callDeclined = 0xFF;
        g_csState = 0;
        g_inScene = false;
        g_csCar = g_csSeat = -1;
        g_movedPending = false;
        g_mapEdits.clear();
        g_iplEdits.clear();
        for (PartnerEdit& pe : g_partnerEdits) pe.applied = false;  // a new world: built as it was, edited again
        g_sentMax = g_sentBackOff = 0xFF;
        g_sentMult = -1.0f;
        g_policyApplied = false;
        g_skipOpen = false;
        g_skipYes.clear();
        g_skipNoMs = 0;
        g_hostContacts.clear();  // the new world's own points stand till the host's next set
        g_haveHostContacts = false;
        g_weFroze = false;
        g_watching = g_sceneHave = g_hostScene = g_scriptCam = false;  // the new world's camera starts clean
    }

    static void HostUpdate() {
        if (g_scriptBlack && !Fading() && FadeAlpha() < 1.0f) g_scriptBlack = false;  // clear again, whoever faded it in
        void* script = FindMissionScript();
        bool active = HostOnMission();
        if (!active) g_lookBaseSet = false, g_carDoors.clear(), g_missionThreads.clear();
        static uint64_t doorsMs = 0;
        if (active && !g_carDoors.empty() && NowMs() - doorsMs >= 1000) doorsMs = NowMs(), ResendCarDoors();
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
        // Big Smoke's courier runs outside missions (Courier.cpp): its blips (its car, the bags, the package) go to the
        // partners as a mission's do, from the blips there before it on
        bool courier = !active && CourierRuns() && !Net().Peers().empty();
        if (courier != g_courierBlips) {
            g_courierBlips = courier;
            g_sentBlips.clear();
            g_baseline.clear();
            if (courier) {
                for (int i = 0; i < kNumTraces; ++i)
                    if (Ref<uint8_t>(Trace(i) + 0x25) & 2) g_baseline.insert(((uint32_t)Ref<uint16_t>(Trace(i) + 0x14) << 8) | (uint32_t)i);
                LOGI("Big Smoke's courier runs: its blips go to the partners");
            } else {
                Writer w(proto::G_BLIP);
                w.U8(BLIP_CLEAR);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
        static uint64_t courierScanMs = 0;
        if (courier && NowMs() - courierScanMs >= 250) courierScanMs = NowMs(), HostScanBlips(false);
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
            if (!g_missionName.empty() || g_active) LOGI("Mission named (%s)", name.c_str());  // the flag came up first
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
            // the story moved on: a mission passed (CStats::LastMissionPassedName). A failed attempt pays the partner
            // nothing: the money the host picked up from the dead in it was no reward
            std::string passed((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));
            if (int32_t earned = PlayerInfos()[0].money - g_moneyAtStart; earned > 0 && passed != g_lastPassed) {
                Writer rw(proto::G_MISSION_REWARD);
                rw.I32(earned);
                Net().Send(proto::CH_RELIABLE, rw);
            }
            HostSendScreen(false);
            g_mapEdits.clear();    // its gates and buildings stay as it left them in both games
            g_iplEdits.clear();    // and so do its parts of the map
            g_sentMarked.clear();  // the partner clears its copies at the mission's end
            g_scriptBlack = false;
            g_sentBlips.clear();
            g_areas.clear();
            LOGI("Mission ended (%s)", g_missionName.c_str());
            // The call that often follows a passed mission (the next script) unlocks more, like the gym after Sweet's
            // third: the partners reload the world once, after both, so they wait 30 s for it, or until the next script
            // is over
            if (passed != g_lastPassed) {
                g_lastPassed = passed;
                g_progressAt = NowMs() + 30000;
            } else if (g_progressAt) {
                g_progressAt = NowMs() + 1000;
            }
        }
        g_active = active;
        // a side activity's mission that passes a story step (the flight school is DESERT5, the driving school DSKOOL)
        // ends no host mission here: the partners still get the world it unlocked
        if (!active) {
            std::string passed((const char*)0xB78A00, strnlen((const char*)0xB78A00, 8));  // CStats::LastMissionPassedName
            if (passed != g_lastPassed) {
                g_lastPassed = passed;
                if (!g_progressAt) g_progressAt = NowMs() + 30000;
            }
        }
        if (g_progressAt && !active && NowMs() >= g_progressAt) {
            g_progressAt = 0;
            flow::WorldProgressed();
        }

        // the partner's screen shows where the host waits
        bool waiting = !active && NowMs() - g_waitMs < 1500;
        if (waiting != g_waitSent) {
            g_waitSent = waiting;
            float x = (g_waitArea.x1 + g_waitArea.x2) * 0.5f, y = (g_waitArea.y1 + g_waitArea.y2) * 0.5f;
            if (waiting) LOGI("Mission marker at %.1f,%.1f: waiting for the partner (%s's trigger)", x, y, g_waitBy);
            else LOGI("Mission marker at %.1f,%.1f: not waiting any more", x, y);
            SendWait(waiting);
        }

        HostSkipUpdate();
        // cutscene end
        bool cs = CutsceneRunning();
        // the mission moved us: sent once we can move again (no scene, no fade, control back). A camera trick in between
        // (the gym shown from 270 m away) is over by then and we are back where we were: nothing to follow. A scene on the
        // engine with CJ standing at the new place goes at once: the partner left at the old one saw none of it (INTRO1's
        // street after INTRO1B, 19 s, "Ah shit, here we go again").
        CPlayerPed* me = FindPlayerPed(0);
        bool standing = me->InVehicle() ? !InAirOrWater() : (me->PedFlags() & 1) != 0;  // bIsStanding
        if (g_movedPending && !cs && FadeAlpha() < 1.0f && (!GetPad(0)->DisablePlayerControls || (HostScene() && standing))) {
            g_movedPending = false;
            CVector p = me->GetPosition();
            float moved = (p - g_movedFrom).Length();
            bool near = moved > kMovedNear && (g_movedOffVehicle || RemoteDistance(g_movedFrom, g_movedFromArea) < kPartnerByOldPlace);
            if (moved > kMovedFar || (me->AreaCode() != g_movedFromArea && moved >= 3.0f) || near) {
                Writer w(proto::G_CUTSCENE);
                w.U8(CS_FOLLOW);
                w.Str("", 32);
                w.I32(me->AreaCode());
                w.Vec({p.x, p.y, p.z});
                w.Vec({g_movedFrom.x, g_movedFrom.y, g_movedFrom.z});
                w.I8(g_movedFromArea);
                w.F32(me->CurrentRotation());
                Net().Send(proto::CH_RELIABLE, w);
                LOGI("The mission moved us %.0f m (area %d to %d): partners near the old place come along", moved,
                     g_movedFromArea, me->AreaCode());
            }
        }
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

        static uint64_t contactsMs = 0;
        if (!Net().Peers().empty() && NowMs() - contactsMs >= 1000) contactsMs = NowMs(), HostSendContacts();

        HostSceneUpdate();
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
        HostSendPolicy(false);
        SendAreas(now);
    }

    static void PartnerUpdate() {
        UpdatePartnerCutscene();
        if (g_lookPending && FindPlayerPed(0)->PedState() != 50) ApplyMissionLook();
        // not the balaclava of a joint burglary (no host mission runs then): taken off 8 ms after it went on (28.10)
        if (g_ownKeysSet && !HostMissionRecent(0) && !g_burglaryDressed) RestoreOwnLook();
        UpdateOwnNoise();
        if (!g_noCollide.empty()) ApplyNoCollides();
        SeatWithHost();
        LeaveWithHost();
        static uint64_t editsMs = 0, contactsMs = 0;
        if (!g_partnerEdits.empty() && NowMs() - editsMs > 500 && flow::InHostWorld()) editsMs = NowMs(), ApplyPartnerEdits();
        // every frame while it runs: a world loaded meanwhile brings the flag down
        if (g_partnerMission && !_stricmp(g_hostMission.c_str(), "driv3") && flow::InHostWorld()) {
            if (!g_gateOpenSet) LOGI("The host's DRIV3: the SF airport's main gate opens here too ($7200)");
            Ref<int32_t>(addr::ScriptSpace + kAirportGateOpen) = 1, g_gateOpenSet = true;
        }
        if (NowMs() - contactsMs >= 1000) contactsMs = NowMs(), TakeHostContacts();  // our own scripts may set theirs again
        PartnerSendAt();
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
        ShowMissionMarks();
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
    hint = TR("Enter - пропустить, Esc - смотреть дальше", "Enter: skip, Esc: keep watching");
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

std::string SideCallText(std::string& hint) {
    hint.clear();
    if (!Net().Online()) return "";
    if (g_side.inviteFrom != 0xFF) {
        hint = TR("Y - да, N - нет", "Y: yes, N: no");
        return Format(TR("%s зовёт вас на %s", "%s invites you to %s"), Net().PeerName(g_side.inviteFrom).c_str(),
                      JobTo(g_side.inviteName).c_str());
    }
    if (CanCall()) {  // a race or a triathlon may be a contest instead (Contest.cpp)
        hint = contest::CanCall() ? TR("Y - позвать, U - соревноваться", "Y: invite, U: contest") : TR("Y - позвать", "Y: invite");
        return Format(TR("Позвать %s на %s?", "Invite %s to %s?"), OtherName().c_str(), JobTo(g_side.name).c_str());
    }
    if (g_side.offered && !g_side.name.empty())
        return Format(TR("Ждём ответа %s на приглашение", "Waiting for %s to answer"), OtherName().c_str());
    return jobs::LootText(hint);  // a thing in hand at the burglar's van
}

bool SideCallKey(bool yes) {
    if (!Net().Online()) return false;
    if (g_side.inviteFrom != 0xFF) {
        uint8_t from = g_side.inviteFrom;
        std::string name = g_side.inviteName;
        g_side.inviteFrom = 0xFF;
        void* own = FindMissionScript();
        if (name == "shrange" && jobs::RangeWaits(own)) own = nullptr;
        if (yes && !own && !jobs::ValetShift()) {
            g_side.owner = from, g_side.joined = name;
            // BoardSide; a train takes no passenger. The quarry's player gets to its dozer or dumper a while after the start,
            // a stadium's player to its vehicle after the intro or the marker
            if (name != "freight")
                g_side.boardUntil = NowMs() + (name == "quarry" ? 120000 : StadiumJob(name) ? 60000 : 15000), g_side.boardTryMs = 0;
            SendSide(SIDE_ACCEPT, from, name);
            ui::TheChat().AddSystem(Format(TR("Вы приняли приглашение %s на %s.", "You accepted %s's invitation to %s."),
                                           Net().PeerName(from).c_str(), JobTo(name).c_str()));
            // a vehicle of the job of our own next to the owner (BoardSide), and what to do with it
            if (const char* hint = JoinerHint(name)) ui::TheChat().AddSystem(Format(hint, Net().PeerName(from).c_str()));
            LOGI("Joined %s's side activity (%s)", Net().PeerName(from).c_str(), name.c_str());
        } else {
            SendSide(SIDE_DECLINE, from, name);
        }
        return true;
    }
    if (yes && CanCall()) {
        g_side.offered = true;
        contest::JointCalled();
        SendSide(SIDE_OFFER, 0xFF, g_side.name);
        LOGI("Called the other player in (%s)", g_side.name.c_str());
        return true;
    }
    // a partner's call from a mission's start point, when the box shows it (see MissionWaitText)
    std::string hint;
    uint8_t id = Net().IsHost() && SideCallText(hint).empty() ? PartnerCalling() : 0xFF;
    if (id == 0xFF) return false;
    if (yes) GoToPartnerMarker(id);
    else g_callDeclined = id;
    return true;
}

std::string MissionWaitText(std::string& hint) {
    hint.clear();
    if (!Net().Online()) return "";
    if (Net().IsHost()) {
        if (g_waitSent) return TR("Ждём напарника", "Waiting for your partner");
        uint8_t id = PartnerCalling();
        if (id == 0xFF) return "";
        hint = TR("Y - переместиться к маркеру, N - нет", "Y: go to the marker, N: no");
        return Format(TR("%s ждёт вас у маркера миссии", "%s is waiting for you at the mission marker"), Net().PeerName(id).c_str());
    }
    std::string host = Net().PeerName(Net().HostId());
    if (g_hostWaits) return Format(TR("%s ждёт вас у маркера миссии", "%s is waiting for you at the mission marker"), host.c_str());
    if (g_atSent == PW_AT) return Format(TR("Ждём %s у маркера миссии", "Waiting for %s at the mission marker"), host.c_str());
    return "";
}

extern "C" __attribute__((used)) int8_t __cdecl CoopScriptCommand(void* script, void* handler, int command) {
    return ScriptCommand(script, handler, command);
}

bool HostReplicating() { return Replicating(); }

bool SideCallOpen() { return Net().Online() && CanCall(); }
void SideCallWithdraw() { g_side.offerUntil = 0; }

void ShowOwnCounter(uint32_t varOffset, const char* label, bool on) {
    auto* t = (uint8_t*)kOnscreenTimer;
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < 4; ++i) {
        uint32_t var = *(uint32_t*)(t + 0x40 + i * 0x44);  // COnscreenCounterEntry::m_nVarId
        if (var == varOffset) slot = i;
        else if (!var && freeSlot < 0) freeSlot = i;
    }
    char key[10] = {};
    strncpy(key, label, sizeof key - 1);
    if (on && slot < 0 && freeSlot >= 0) {
        CallMethod<0x44CDA0>((void*)kOnscreenTimer, varOffset, 1, key, (uint16_t)freeSlot);  // COnscreenTimer::AddCounter: a bar
        t[0x40 + freeSlot * 0x44 + 0x3F] = 0;  // m_bFlashWhenFirstDisplayed: GUNS1 turns it off too
    } else if (!on && slot >= 0) {
        CallMethod<0x44CE80>((void*)kOnscreenTimer, varOffset);  // COnscreenTimer::ClearCounter
    }
}

bool SideLeading() { return Net().Online() && SideOwning(); }
uint8_t SideLeader() { return Net().Online() ? g_side.owner : 0xFF; }
uint8_t SideHelper() { return Net().Online() ? g_side.with : 0xFF; }

void MissionWeaponUnder(int& type, int& ammo) {
    if (Net().IsHost() || !g_missionWeapons.count(type)) return;
    auto it = g_replaced.find(type);
    type = it != g_replaced.end() ? it->second.first : 0;
    ammo = it != g_replaced.end() ? it->second.second : 0;
}

bool MissionClothesUnder(void* clothes) {
    if (!g_ownKeysSet) return false;
    memcpy(clothes, g_ownKeys, sizeof g_ownKeys);
    return true;
}

void* CurrentScript() { return g_currentScript; }

bool StoryMission(void* script) { return script && Field<uint8_t>(script, 0xDC) && HostOnMission(); }  // m_bIsMission

bool MissionThread(void* script) {
    if (StoryMission(script)) return true;
    if (!script || !HostOnMission()) return false;
    for (auto& [s, base] : g_missionThreads)
        if (s == script && Field<const uint8_t*>(script, 0x10) == base) return true;
    return false;
}

bool SharePersonOnCar(bool on, int ped, int car, float x, float y, float z) {
    int32_t p[8] = {ped, car};  // ATTACH_CHAR_TO_CAR's: heading 0, its range 0.0, no weapon
    memcpy(&p[2], &x, 4), memcpy(&p[3], &y, 4), memcpy(&p[4], &z, 4);
    return Net().Online() && SendPersonOnCar(on, p);
}

void* RunningMission() { return FindMissionScript(); }

bool HostMissionRecent(uint64_t graceMs) { return g_partnerMission || NowMs() - g_partnerMissionEndedAt < graceMs; }
bool SceneWatched() { return g_watching; }

bool AtMissionMarker() {
    Area marker;
    return Net().Online() && !Net().IsHost() && core::IsPlayerReady() && PartnerAtMarker(marker);
}

}  // namespace coop

// esi: the script, edx: the handler, ecx: the command; the loop after the call reads al's test
extern "C" __attribute__((naked)) void CoopScriptCommandStub() {
    __asm__ volatile(
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "pushl %esi\n\t"
        "calll _CoopScriptCommand\n\t"
        "addl $12, %esp\n\t"
        "testb %al, %al\n\t"
        "ret\n\t");
}
