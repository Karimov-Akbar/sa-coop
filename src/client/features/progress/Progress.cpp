// Separate progression and looks.
//  * the host progresses in its own save; the partner keeps a profile on the server (money, every stat:
//    fat, muscle, stamina, lung capacity, weapon/driving skills..., clothes with haircut and tattoos,
//    weapons, fighting style, max health/armour and the rewards of side missions: no tiring, fast reload,
//    fireproof, free jail and hospital). It is applied over the host's world after each world load and saved back
//    when it changes, and at once when the host saves its game. A partner without a profile starts from what the
//    world gave him; a new game of the host starts every profile over (the server keeps the old ones aside);
//  * both players broadcast their look (clothes description incl. fat/muscle) and their stats block: the
//    other game rebuilds the remote ped's clothes and uses those stats while processing it;
//  * each player has a look of its own: a partner brings the CJ of its own game into the host's world (the game it
//    played before joining, else its newest save) instead of keeping the host's CJ the world came with.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
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

constexpr uint32_t kProfileMagic = 0x46504353;  // "SCPF"
constexpr uint16_t kProfileVersion = 5;  // 2: the rewards of side missions (CPlayerInfo +0x14C..+0x152); 3: whose look;
                                         // 4: the gym's moves (CPed::m_nAllowedAttackMoves); 5: side activities (SideProgress)
constexpr uintptr_t kStatsBlock = 0xB79000;  // CStats::StatTypesInt + StatTypesFloat
constexpr size_t kStatsSize = 0x4C8;
// stats the world owns, not the player (CStats::StatTypesInt, id - 120): the profile keeps the values of the world it was
// saved in, and laid over a newer one it took the city back (STAT_CITY_UNLOCKED: wanted stars over the bridges, the
// hospitals, the radio) and the story's accomplished missions, gang strength and territory
// The collectibles' counts too (snapshots 231, horseshoes 241, oysters 243; the tags are counted on the map itself): what
// is left to pick up is the world's, and a partner's own count ran apart from it for good once the host's world rolled
// back to its last save (50 of 50 against 20 of 50, #4)
constexpr int kWorldStats[][2] = {{146, 148}, {180, 181}, {231, 231}, {241, 241}, {243, 243},
                                  {302, 342}};  // missions tried, passed, in game; no more hurricanes, city; collectibles; story
// and the story's progress (CStats::StatTypesFloat at +0x380: STAT_PROGRESS_MADE, STAT_TOTAL_PROGRESS): the menu's per cent
// stood where the partner's profile began (3.21% for a whole story played together, issue #59)
constexpr size_t kFloatStats = 0x380;
constexpr int kWorldFloatStats = 2;
constexpr size_t kClothesSize = sizeof(CPedClothesDesc);

void RebuildPlayer(CPlayerPed* ped, bool keepDescFatMuscle) { Call<0x5A82C0>(ped, keepDescFatMuscle); }  // CClothes::RebuildPlayer

// The partner's look in the host's world is its own, or one it was given (the host's CJ the world came with, on a
// first visit with no CJ of its own to bring, or in a profile older than version 3): that one gives way to its own CJ
// once there is one, unless the partner changed its clothes meanwhile.
bool g_lookOwn = false;
uint32_t g_givenKeys[28] = {};   // the clothes (model and texture keys) of a given look
std::vector<uint8_t> g_ownLook;  // the CJ of the last game of its own this player played (not a host's world)
// and kept on disk: joined right after the game's start, the CJ was taken from the newest save, often the host's world
// the player had saved as a partner, and the host's clothes became the partner's own
// in the player's own user files (next to the saves): copies of the game on one PC, each with its user_dir, keep their own
std::wstring OwnLookPath() {
    std::wstring dir = core::UserFilesDir();
    return dir.empty() ? std::wstring() : dir + L"coop_own_look.bin";
}
bool g_foreignGame = false;      // the game was started as a partner: a host's world, kept after leaving it too

bool LookIsOwn() { return g_lookOwn || memcmp(FindPlayerPed(0)->ClothesDesc(), g_givenKeys, sizeof g_givenKeys) != 0; }

// CJ's clothes in a save of this game: block 2 holds the player peds (pool ref, model, ped type, the CPed part with its
// size, then the CPlayerPedDataSaveStructure with its size: 0x84 bytes, the clothes at +8)
bool LookFromSave(const std::wstring& path, std::vector<uint8_t>& look) {
    std::vector<uint8_t> b;
    if (!ReadFileBytes(path, b)) return false;
    auto next = [&](size_t from) {
        for (size_t i = from; i + 5 <= b.size(); ++i)
            if (!memcmp(&b[i], "BLOCK", 5)) return i;
        return b.size();
    };
    size_t p = next(next(next(0) + 5) + 5) + 5;
    auto u32 = [&](size_t at) { return at + 4 <= b.size() ? *(const uint32_t*)&b[at] : 0xFFFFFFFFu; };
    uint32_t peds = u32(p);
    p += 4;
    for (uint32_t i = 0; i < peds && i < 8; ++i) {
        uint32_t type = u32(p + 8), size = u32(p + 12);
        if (size > 0x1000) return false;
        p += 16 + size;
        uint32_t dataSize = u32(p);
        p += 4;
        if (type == PED_TYPE_PLAYER1 && dataSize == 0x84 && p + 8 + kClothesSize <= b.size()) {
            look.assign(&b[p + 8], &b[p + 8] + kClothesSize);
            return true;
        }
        if (dataSize > 0x1000) return false;
        p += dataSize;
    }
    return false;
}

// Puts the player's own CJ on: with its body on a first visit (later the body is that of the profile's stats)
bool WearOwnLook(bool body) {
    std::vector<uint8_t> look = g_ownLook;
    std::wstring newest;
    if (look.empty() && (!ReadFileBytes(OwnLookPath(), look) || look.size() != kClothesSize)) look.clear();
    if (look.empty()) {
        FILETIME newestTime{};
        for (int slot = 1; slot <= 8; ++slot) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            std::wstring path = core::SlotPath(slot);
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                (newest.empty() || CompareFileTime(&a.ftLastWriteTime, &newestTime) > 0))
                newest = path, newestTime = a.ftLastWriteTime;
        }
        if (newest.empty() || !LookFromSave(newest, look)) return false;
    }
    const auto* desc = (const CPedClothesDesc*)look.data();
    CPlayerPed* me = FindPlayerPed(0);
    if (!memcmp(look.data(), me->ClothesDesc(), sizeof g_givenKeys)) {  // the host's world's own CJ: nothing of ours
        LOGI("The look found (%s) is the world's CJ: not taken as ours", newest.empty() ? "kept" : WideToUtf8(newest).c_str());
        return false;
    }
    if (body) Ref<float>(0xB793D4) = desc->fatStat, Ref<float>(0xB793DC) = desc->muscleStat;  // CStats: fat, muscle
    memcpy(me->ClothesDesc(), look.data(), kClothesSize);
    RebuildPlayer(me, false);  // fat and muscle from the stats
    LOGI("Own look put on (%s): torso %08X, head %08X", newest.empty() ? "the game played before" : WideToUtf8(newest).c_str(),
         desc->textureKeys[0], desc->textureKeys[1]);
    return true;
}

// The side activities keep their progress in main.scm globals of the world (schools, jobs, the gym, exports, burglary),
// and the host's world brought its own at every load: the partner's lessons, levels and medals started over, and the
// unique stunt jumps paid again. The profile keeps them and puts them back, with what is engine state: the jumps, the
// reward cars' generators and the assets' revenue pickups. Story globals, handles, the "running" flags and the
// girlfriends stay the world's: their agent script redoes a date the world has not seen, moving the dating stats.
struct GlobalRange {
    uint16_t first, last;
};
constexpr GlobalRange kSideGlobals[] = {
    {53, 53}, {86, 86}, {91, 92}, {94, 94}, {96, 98}, {100, 103}, {105, 105}, {107, 107}, {5187, 5187}, {7417, 7419},  // driving
    {1962, 1967}, {1969, 1987}, {2162, 2162}, {2164, 2191}, {2201, 2201},  // boat and bike schools
    {8159, 8159}, {8171, 8171}, {8174, 8180},                            // trucking, quarry
    {1487, 1493}, {180, 180}, {163, 163}, {8239, 8240}, {1900, 1900}, {1991, 1994},  // jobs done, taxi, freight, valet, ...
    {8153, 8158}, {5347, 5349},                                          // gym: trainers beaten, styles, today's workout
    {1049, 1049}, {1060, 1069}, {1076, 1117}, {1184, 1184}, {1790, 1791},  // exports, burglary
};
// the flight school's, only in a world whose story has passed "Learning to Fly" ($87): before that the school is the story's
constexpr GlobalRange kFlightGlobals[] = {{1942, 1952}, {1954, 1955}, {1959, 1961}};
// globals holding the reward cars' generators (an index into CTheCarGenerators::CarGeneratorArray, the same in both games)
constexpr uint16_t kRewardGenerators[] = {5178, 5183, 5184, 5185, 5186, 1988, 1989, 1990, 2192, 2193, 2194, 2759, 2760};
constexpr uint16_t kFlightGenerators[] = {1956, 1957, 1958, 5214};
struct Asset {
    uint16_t done;  // the global set when the job is done
    float x, y, z;  // where its revenue pickup is (the job's own CREATE_PROTECTION_PICKUP)
};
constexpr Asset kAssets[] = {{1492, -82.269f, -1135.526f, 0.585f},   {1493, 818.258f, 853.887f, 10.806f},
                             {1900, -1757.668f, 961.782f, 24.383f},  {1992, 1345.859f, -1757.432f, 13.008f},
                             {1994, -2593.882f, 59.247f, 3.836f},    {1993, 1887.997f, 2073.008f, 10.555f}};
constexpr uintptr_t kCarGenerators = 0xC27AD0;  // CTheCarGenerators::CarGeneratorArray: 500 x 0x20
constexpr uintptr_t kStuntJumps = 0xA9A888;     // CStuntJumpManager::mp_poolStuntJumps: 256 x 0x44, +0x40 done, +0x41 found

int32_t& Global(uint16_t n) { return Ref<int32_t>(addr::ScriptSpace + (uintptr_t)n * 4); }
template <size_t N>
bool Listed(const GlobalRange (&ranges)[N], uint16_t n) {
    for (const GlobalRange& r : ranges)
        if (n >= r.first && n <= r.last) return true;
    return false;
}
template <size_t N>
bool Listed(const uint16_t (&list)[N], uint16_t n) {
    return std::find(std::begin(list), std::end(list), n) != std::end(list);
}
uint8_t* Generator(uint16_t global) {
    int32_t i = Global(global);
    return i >= 0 && i < 500 ? (uint8_t*)(kCarGenerators + (uintptr_t)i * 0x20) : nullptr;
}
// CPool<CStuntJump>: objects, byte map (bit 7: free), size
uint8_t* StuntJump(int i) {
    auto* pool = Ref<uint8_t*>(kStuntJumps);
    if (!pool || i >= std::min(Ref<int32_t>((uintptr_t)pool + 8), 256) || (Ref<uint8_t*>((uintptr_t)pool + 4)[i] & 0x80)) return nullptr;
    return Ref<uint8_t*>((uintptr_t)pool) + (size_t)i * 0x44;
}
// A jump is known by its place (start and end boxes, camera): its slot changes when the world is loaded again in the
// same game (the pool's new entries go on from the last slot it gave out)
uint32_t JumpKey(const uint8_t* j) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 0x3C; ++i) h = (h ^ j[i]) * 16777619u;
    return h;
}

// A job's asset pays through a revenue pickup kept in the save: the host's world has the host's only. Made as the job's
// script makes it, where it is missing.
int AssetPickups() {
    int made = 0;
    for (const Asset& a : kAssets) {
        if (!Global(a.done)) continue;
        bool there = false;
        for (int i = 0; i < 620 && !there; ++i) {
            uintptr_t pu = 0x9788C0 + (uintptr_t)i * 0x20;  // CPickups::aPickUps: +0x10 x/y/z in eighths, +0x1C type
            CVector at{Ref<int16_t>(pu + 0x10) / 8.0f, Ref<int16_t>(pu + 0x12) / 8.0f, Ref<int16_t>(pu + 0x14) / 8.0f};
            there = Ref<uint8_t>(pu + 0x1C) == 16 && (at - CVector{a.x, a.y, a.z}).Length() < 3.0f;  // PICKUP_ASSET_REVENUE
        }
        if (there) continue;
        int h = -1;
        Cmd(0x04A6).F(a.x).F(a.y).F(a.z).I(2000).I(2000).Out(&h).Run();  // CREATE_PROTECTION_PICKUP
        ++made;
    }
    return made;
}

// ---------------------------------------------------------------- side jobs and challenges done for both
// A job or a challenge one player finishes counts for the other too: its "done" global, its first-completion reward (as
// the job's script gives it: max health, armour, fireproof, taxis with nitro...) and, in the host's world, its progress
// and odd job. The world's own (the challenges) were the host's only: a partner's run was gone at its next reload.
enum : uint8_t {
    RW_NONE, RW_PARAMEDIC, RW_VIGILANTE, RW_FIREFIGHTER, RW_BURGLARY, RW_TAXI, RW_PIMP, RW_ASSET, RW_STAMINA, RW_BLOOD_BOWL
};
struct DoneFlag {
    uint16_t global;
    uint8_t reward;
    bool progress, oddJob;  // PLAYER_MADE_PROGRESS 1, REGISTER_ODDJOB_MISSION_PASSED: the host's world
    bool world;             // the world's flag (not kept in the partner's profile)
};
constexpr DoneFlag kDoneFlags[] = {
    {1487, RW_PARAMEDIC, true, true, false},   {1488, RW_VIGILANTE, true, true, false},  {1489, RW_FIREFIGHTER, true, true, false},
    {1490, RW_BURGLARY, false, false, false},  {1491, RW_TAXI, true, true, false},       {1492, RW_ASSET, true, true, false},
    {1493, RW_ASSET, true, true, false},       {1991, RW_PIMP, true, true, false},       {1992, RW_ASSET, true, true, false},
    {1993, RW_ASSET, true, true, false},       {1994, RW_ASSET, true, true, false},      {8240, RW_NONE, false, true, false},
    {8239, RW_NONE, true, true, false},        {1900, RW_ASSET, false, false, false},  // freight 1 and 2, valet
    {90, RW_NONE, true, true, true},           {1941, RW_BLOOD_BOWL, true, true, true},  {1806, RW_STAMINA, false, true, true},
    {2795, RW_NONE, true, false, true},        {2796, RW_NONE, true, false, true},       {1801, RW_NONE, true, true, true},
};  // the last six: Kickstart, Blood Bowl, the triathlons, the BMX and NRG-500 challenges, the Chiliad Challenge
std::map<uint16_t, int32_t> g_doneSeen;  // the flags as this game last saw them (a change of ours goes out)

const DoneFlag* DoneFlagOf(uint16_t global) {
    for (const DoneFlag& d : kDoneFlags)
        if (d.global == global) return &d;
    return nullptr;
}

void SendDone(const std::vector<std::pair<uint16_t, int32_t>>& done) {
    if (done.empty()) return;
    Writer w(proto::G_SIDE_DONE);
    w.U8((uint8_t)done.size());
    for (auto [g, v] : done) w.U16(g), w.I32(v);
    Net().Send(proto::CH_RELIABLE, w);
}

// the other player's completion here: the flag, its reward, and in the host's world its progress
void TakeDone(const DoneFlag& d, int32_t value) {
    if (Global(d.global)) return;  // done here already: the reward came then
    Global(d.global) = value;
    g_doneSeen[d.global] = value;
    CPlayerPed* me = FindPlayerPed(0);
    switch (d.reward) {
    case RW_PARAMEDIC: Call<0x55C180>((uint16_t)24, Call<0x558E40, float>((uint16_t)24) * 1.5f); break;  // max health x2.5
    case RW_VIGILANTE: Cmd(0x055F).I(0).I(50).Run(), Cmd(0x035F).I(GetPedRef(me)).I(150).Run(); break;  // max armour, armour
    case RW_FIREFIGHTER: Cmd(0x055D).I(0).I(1).Run(); break;  // MAKE_PLAYER_FIRE_PROOF
    case RW_BURGLARY: Cmd(0x0330).I(0).I(1).Run(); break;     // SET_PLAYER_NEVER_GETS_TIRED
    case RW_TAXI: Ref<uint8_t>(0x969130 + 91) = 1; break;     // CCheat::m_aCheatsActive: taxis with nitro (not toggled)
    case RW_PIMP: Ref<uint8_t>(0x969130 + 90) = 1; break;     // the pimp's cheat
    case RW_ASSET: AssetPickups(); break;
    case RW_STAMINA: {
        float& stamina = Ref<float>(kStatsBlock + kFloatStats + 22 * 4);
        stamina = std::max(stamina, 1000.0f);
        break;
    }
    case RW_BLOOD_BOWL:
        if (Net().IsHost()) Cmd(0x014C).I(Global(5188)).I(101).Run();  // SWITCH_CAR_GENERATOR: its reward car, for good
        break;
    }
    if (Net().IsHost()) {  // the world's progress and its odd jobs (the partner's follow the host's world)
        if (d.progress) Cmd(0x030C).I(1).Run();  // PLAYER_MADE_PROGRESS
        if (d.oddJob) Cmd(0x0595).Run();        // REGISTER_ODDJOB_MISSION_PASSED
    }
    LOGI("The other player's completion %u: here too (reward %u)", d.global, d.reward);
}

void HandleDone(uint8_t from, Reader& r) {
    std::vector<std::pair<uint16_t, int32_t>> done(r.U8());
    for (auto& [g, v] : done) g = r.U16(), v = r.I32();
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
    for (auto [g, v] : done)
        if (const DoneFlag* d = DoneFlagOf(g); d && v > 0 && v < 256) TakeDone(*d, v);
}

// every second: a flag of ours newly up goes to the other player
void WatchDone(bool rebase) {
    std::vector<std::pair<uint16_t, int32_t>> up;
    for (const DoneFlag& d : kDoneFlags) {
        int32_t v = Global(d.global), &seen = g_doneSeen[d.global];
        if (!rebase && v > 0 && seen == 0) up.push_back({d.global, v});
        seen = v;
    }
    SendDone(up);
}

std::map<uint16_t, int32_t> WorldDoneFlags() {
    std::map<uint16_t, int32_t> world;
    for (const DoneFlag& d : kDoneFlags)
        if (!d.world) world[d.global] = Global(d.global);
    return world;
}

// The partner's profile goes over the host's world: of the jobs either finished while the other was away, the host's
// reward comes here and the partner's goes to the host
void MergeDoneFlags(const std::map<uint16_t, int32_t>& world) {
    std::vector<std::pair<uint16_t, int32_t>> ours;
    for (const DoneFlag& d : kDoneFlags) {
        if (d.world) continue;
        int32_t mine = Global(d.global), theirs = world.at(d.global);
        if (mine > 0 && theirs == 0) ours.push_back({d.global, mine});
        if (theirs > 0 && mine == 0) TakeDone(d, theirs);
    }
    SendDone(ours);
    WatchDone(true);
}

// ---------------------------------------------------------------- the stats tab, shared
// Both players' stats tab goes on together: each game sends what changed in it every second, counters as amounts added
// (distances, fares, kills, photos...), records as values (the higher, a best time or place the lower), and the host the
// world's own. What each game counts by itself stays its own: the body and the gym, lungs, guns, money, dates, respect.
// The skills hard to get (driving, flying, a bike, a bicycle, gambling) go to the better of the two.
enum : uint8_t { R_ADD = 1, R_MAX = 2, R_MIN = 3, R_SET = 4 };
struct StatRange {
    uint16_t first, last;
    uint8_t rule;
};
constexpr StatRange kSharedStats[] = {
    {3, 9, R_ADD},       {26, 27, R_ADD},     {36, 36, R_ADD},     {41, 41, R_ADD},     {121, 132, R_ADD},  // distances, kills
    {138, 138, R_ADD},   {149, 155, R_ADD},   {161, 163, R_ADD},   {166, 169, R_ADD},   {171, 171, R_ADD},  // jobs, gangs
    {173, 173, R_ADD},   {177, 179, R_ADD},   {182, 183, R_ADD},   {191, 192, R_ADD},   {194, 197, R_ADD},
    {201, 201, R_ADD},   {217, 218, R_ADD},   {234, 235, R_ADD},   {238, 240, R_ADD},
    {2, 2, R_MAX},       {10, 12, R_MAX},     {17, 19, R_MAX},     {40, 40, R_MAX},     {51, 51, R_MAX},    // records
    {81, 81, R_MAX},     {139, 143, R_MAX},   {157, 160, R_MAX},   {174, 175, R_MAX},   {205, 211, R_MAX},  // and skills
    {215, 215, R_MAX},   {223, 223, R_MAX},   {229, 230, R_MAX},
    {48, 49, R_MIN},     {172, 172, R_MIN},   {202, 202, R_MIN},   {212, 212, R_MIN},   {219, 222, R_MIN},  // best times
    {246, 250, R_MIN},   {258, 301, R_MIN},
    {0, 1, R_SET},       {146, 148, R_SET},   {180, 181, R_SET},   {302, 342, R_SET},  // the world's, host -> partner
    {231, 231, R_SET},   {241, 241, R_SET},   {243, 243, R_SET},  // and its collectibles' counts (kWorldStats)
};
uint8_t RuleOf(uint16_t id) {
    for (const StatRange& r : kSharedStats)
        if (id >= r.first && id <= r.last) return r.rule;
    return 0;
}
uint32_t& StatBits(uint16_t id) {  // ids 0..81 are floats (StatTypesFloat), 120..342 ints
    return Ref<uint32_t>(id < 82 ? kStatsBlock + kFloatStats + id * 4u : kStatsBlock + (id - 120u) * 4u);
}
double StatNum(uint16_t id, uint32_t bits) {
    if (id >= 82) return (double)(int32_t)bits;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
uint32_t StatBitsOf(uint16_t id, double v) {
    if (id >= 82) return (uint32_t)(int32_t)std::llround(v);
    float f = (float)v;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

struct StatEntry {
    uint16_t id;
    uint8_t rule;
    uint32_t bits;
};
uint32_t g_statBase[343];  // the shared stats as last seen: what changed since goes out
bool g_statBased = false;  // false: rebase first (a world loaded, a profile went on, a player came)
std::vector<StatEntry> g_statQueue;  // arrived before this game could take them (a world loading, no profile yet)
bool g_statAnswer = false;  // a full set came: ours goes back once we are ready

template <typename F>
void ForSharedStats(F f) {
    for (const StatRange& r : kSharedStats)
        for (uint16_t id = r.first; id <= r.last; ++id) f(id, r.rule);
}

void SendStats(uint8_t full, const std::vector<StatEntry>& out) {
    for (size_t at = 0; at < out.size(); at += 200) {
        size_t n = std::min<size_t>(200, out.size() - at);
        Writer w(proto::G_STAT_SYNC);
        w.U8(full);
        w.U8((uint8_t)n);
        for (size_t i = at; i < at + n; ++i) w.U16(out[i].id), w.U8(out[i].rule), w.U32(out[i].bits);
        Net().Send(proto::CH_RELIABLE, w);
    }
}

// all our records, skills and (the host) world stats: a newcomer takes the better ones
void SendFullStats(uint8_t full) {
    std::vector<StatEntry> out;
    ForSharedStats([&](uint16_t id, uint8_t rule) {
        if (rule == R_ADD || (rule == R_SET && !Net().IsHost())) return;
        out.push_back({id, rule, StatBits(id)});
    });
    SendStats(full, out);
}

void ApplyStat(const StatEntry& e) {
    uint32_t& cur = StatBits(e.id);
    double a = StatNum(e.id, cur), b = StatNum(e.id, e.bits);
    if (e.rule == R_ADD) {
        b = std::clamp(b, 0.0, e.id < 82 ? 1e6 : 100000.0);
        cur = StatBitsOf(e.id, a + b);
        g_statBase[e.id] = StatBitsOf(e.id, StatNum(e.id, g_statBase[e.id]) + b);  // ours did not change: nothing goes back
        return;
    }
    if (e.rule == R_MAX && b > a) cur = e.bits;
    else if (e.rule == R_MIN && b > 0 && (a == 0 || b < a)) cur = e.bits;
    else if (e.rule == R_SET && !Net().IsHost()) cur = e.bits;
    g_statBase[e.id] = cur;
}

void HandleStatSync(uint8_t from, Reader& r) {
    uint8_t full = r.U8();
    std::vector<StatEntry> in(r.U8());
    for (StatEntry& e : in) e.id = r.U16(), e.rule = r.U8(), e.bits = r.U32();
    if (!r.Ok() || from == Net().MyId()) return;
    for (const StatEntry& e : in) {
        // only the listed stats with their own rule: the message comes from another game
        if (e.id > 342 || (e.id >= 82 && e.id < 120) || RuleOf(e.id) != e.rule || (e.rule == R_SET && from != Net().HostId()))
            continue;
        if (g_statQueue.size() < 2000) g_statQueue.push_back(e);
    }
    if (full == 1) g_statAnswer = true;
    LOGD("Shared stats: %zu from player %u%s", in.size(), from, full ? " (all records)" : "");
}

// every second, in the shared world (the partner: once its profile is on)
void StatTick(bool ready) {
    if (!ready) return;
    if (!g_statBased) {  // a load nobody's stats came from: taken as it is, then our records go out
        ForSharedStats([](uint16_t id, uint8_t) { g_statBase[id] = StatBits(id); });
        g_statBased = true;
        SendFullStats(1);
        WatchDone(true);
    }
    for (const StatEntry& e : g_statQueue) ApplyStat(e);
    g_statQueue.clear();
    if (g_statAnswer) g_statAnswer = false, SendFullStats(2);
    std::vector<StatEntry> out;
    ForSharedStats([&](uint16_t id, uint8_t rule) {
        uint32_t cur = StatBits(id), &base = g_statBase[id];
        if (cur == base || (rule == R_SET && !Net().IsHost())) return;
        if (rule != R_ADD) out.push_back({id, rule, cur});
        else if (double d = StatNum(id, cur) - StatNum(id, base); d > 0) out.push_back({id, rule, StatBitsOf(id, d)});
        base = cur;  // a counter that went down (a fare folded into the odd jobs' total) only moves the base
    });
    // many at once is a world or a profile nobody announced: taken as it is
    if (out.size() > 40) LOGI("Shared stats: %zu changed at once, taken as they are", out.size());
    else SendStats(0, out);
    WatchDone(false);
}

void WriteSideProgress(Writer& w) {
    std::vector<uint16_t> globals;
    for (const GlobalRange& r : kSideGlobals)
        for (uint16_t n = r.first; n <= r.last; ++n) globals.push_back(n);
    for (const GlobalRange& r : kFlightGlobals)
        for (uint16_t n = r.first; n <= r.last; ++n) globals.push_back(n);
    w.U16((uint16_t)globals.size());
    for (uint16_t n : globals) w.U16(n), w.I32(Global(n));
    std::vector<uint16_t> gens(std::begin(kRewardGenerators), std::end(kRewardGenerators));
    gens.insert(gens.end(), std::begin(kFlightGenerators), std::end(kFlightGenerators));
    w.U8((uint8_t)gens.size());
    for (uint16_t g : gens) {
        uint8_t* gen = Generator(g);
        w.U16(g);
        w.U16(gen ? *(uint16_t*)(gen + 0x1A) : 0);  // CCarGenerator::m_nGenerateCount: 0 off
        w.U8(gen ? gen[0x0D] & 0x08 : 0);           // bPlayerHasAlreadyOwnedCar
    }
    std::vector<std::pair<uint32_t, uint8_t>> jumps;
    for (int i = 0; i < 256; ++i)
        if (const uint8_t* j = StuntJump(i)) jumps.push_back({JumpKey(j), (uint8_t)((j[0x40] ? 1 : 0) | (j[0x41] ? 2 : 0))});
    w.U16((uint16_t)jumps.size());
    for (auto [key, bits] : jumps) w.U32(key), w.U8(bits);
}

struct SideProgress {
    std::vector<std::pair<uint16_t, int32_t>> globals;
    std::vector<std::tuple<uint16_t, uint16_t, uint8_t>> generators;
    std::vector<std::pair<uint32_t, uint8_t>> jumps;  // the jump's key, done (1) and found (2)
};

void ReadSideProgress(Reader& r, SideProgress& p) {
    for (int n = r.U16(); n > 0 && n <= 4096 && r.Ok(); --n) {
        uint16_t g = r.U16();
        p.globals.push_back({g, r.I32()});
    }
    for (int n = r.U8(); n > 0 && r.Ok(); --n) {
        uint16_t g = r.U16(), count = r.U16();
        p.generators.push_back({g, count, r.U8()});
    }
    for (int n = r.U16(); n > 0 && n <= 256 && r.Ok(); --n) {
        uint32_t key = r.U32();
        p.jumps.push_back({key, r.U8()});
    }
}

void ApplySideProgress(const SideProgress& p) {
    bool flight = Global(87) == 1;  // the world's "Learning to Fly" passed: read before anything is written
    int globals = 0, generators = 0, jumps = 0, done = 0, pickups = 0;
    std::map<uint16_t, int32_t> world = WorldDoneFlags();  // the host's completions, before the profile's go over them
    for (auto [n, v] : p.globals)  // only the listed ones: a profile is a file from the server
        if (Listed(kSideGlobals, n) || (flight && Listed(kFlightGlobals, n))) Global(n) = v, ++globals;
    // the gym's "enough for today" is a date ($21388 day, $21392 month): one of the partner's later than the world's (the
    // host's older save) kept every machine shut for days (#34). As the gym itself sees the date: CClock's day and month.
    int day = Ref<uint8_t>(0xB70154), month = Ref<uint8_t>(0xB70155);
    if (Global(5348) > month || (Global(5348) == month && Global(5347) > day)) {
        LOGI("The gym's workout of %d.%d is past the world's date (%d.%d): the machines are free", Global(5347), Global(5348),
             day, month);
        Global(5347) = Global(5348) = Global(5349) = 0;
    }
    for (auto [g, count, owned] : p.generators) {
        uint8_t* gen = Listed(kRewardGenerators, g) || (flight && Listed(kFlightGenerators, g)) ? Generator(g) : nullptr;
        if (!gen || !count || *(uint16_t*)(gen + 0x1A)) continue;  // only one the partner earned and the world has off
        Cmd(0x014C).I(Global(g)).I(count > 100 ? 101 : count).Run();  // SWITCH_CAR_GENERATOR (101: for good)
        gen[0x0D] |= owned & 0x08;
        ++generators;
    }
    for (int i = 0; i < 256; ++i) {
        uint8_t* j = StuntJump(i);
        if (!j) continue;
        uint32_t key = JumpKey(j);
        for (auto [k, bits] : p.jumps)
            if (k == key) j[0x40] = bits & 1, j[0x41] = (bits >> 1) & 1, ++jumps;
        done += j[0x40] != 0;
    }
    if (!p.jumps.empty()) Ref<int32_t>(0xA9A8A0) = done;  // CStuntJumpManager::m_iNumCompleted: all done pays the bonus
    MergeDoneFlags(world);
    pickups = AssetPickups();
    LOGI("Side progress back: %d values, %d reward cars, %d stunt jumps (%d done), %d asset pickups", globals, generators,
         jumps, done, pickups);
}

std::vector<uint8_t> CaptureProfile() {
    CPlayerPed* me = FindPlayerPed(0);
    CPlayerInfo& info = PlayerInfos()[0];
    Writer w;
    w.U32(kProfileMagic);
    w.U16(kProfileVersion);
    w.I32(info.money);
    w.Bytes((void*)kStatsBlock, kStatsSize);
    uint8_t clothes[kClothesSize];
    memcpy(clothes, me->ClothesDesc(), kClothesSize);
    MissionClothesUnder(clothes);  // a mask the host's mission put on is not the player's own
    w.Bytes(clothes, kClothesSize);
    w.U8(NUM_WEAPON_SLOTS);
    std::string guns;
    for (int i = 0; i < NUM_WEAPON_SLOTS; ++i) {
        int type = me->Weapons()[i].type, ammo = me->Weapons()[i].ammoTotal;
        MissionWeaponUnder(type, ammo);  // a gun the host's mission gave is not the player's own
        w.U8((uint8_t)type);
        w.I32(ammo);
        if (type > 0) guns += " " + std::to_string(type) + "x" + std::to_string(ammo);
    }
    // a partner left with no weapons after a load (Holder, 29.10): what the profile holds, said when it changes
    static std::string lastGuns = "-";
    if (guns != lastGuns) LOGI("Profile weapons:%s", guns.empty() ? " none" : guns.c_str()), lastGuns = guns;
    w.U8(me->FightingStyle());
    w.U8(info.maxHealth);
    w.U8(info.maxArmour);
    const uint8_t* perks = (const uint8_t*)&info + 0x14C;  // m_bDoesNotGetTired, m_bFastReload, m_bFireProof
    w.U8(perks[0]);
    w.U8(perks[1]);
    w.U8(perks[2]);
    w.U8(perks[5]);  // m_bGetOutOfJailFree
    w.U8(perks[6]);  // m_bFreeHealthCare
    w.U8(LookIsOwn());
    w.U8(Field<uint8_t>(me, 0x72E));  // CPed::m_nAllowedAttackMoves
    WriteSideProgress(w);
    return w.Buffer();
}

bool ApplyProfile(const std::vector<uint8_t>& blob, bool& lookOwn) {
    Reader r(blob.data(), blob.size());
    if (r.U32() != kProfileMagic) return false;
    uint16_t version = r.U16();
    if (version < 1 || version > kProfileVersion) return false;
    int32_t money = r.I32();
    std::vector<uint8_t> stats = r.Bytes(kStatsSize);
    std::vector<uint8_t> clothes = r.Bytes(kClothesSize);
    uint8_t n = r.U8();
    struct Slot {
        uint8_t type;
        int32_t ammo;
    } slots[NUM_WEAPON_SLOTS] = {};
    for (int i = 0; i < n && i < NUM_WEAPON_SLOTS; ++i) slots[i] = {r.U8(), r.I32()};
    uint8_t fight = r.U8(), maxHealth = r.U8(), maxArmour = r.U8();
    uint8_t perks[5] = {};
    if (version >= 2)
        for (uint8_t& b : perks) b = r.U8();
    uint8_t own = version >= 3 ? r.U8() : 0;
    uint8_t moves = version >= 4 ? r.U8() : 0;
    SideProgress side;
    if (version >= 5) ReadSideProgress(r, side);
    if (!r.Ok() || stats.size() != kStatsSize || clothes.size() != kClothesSize) return false;
    lookOwn = own != 0;

    CPlayerPed* me = FindPlayerPed(0);
    CPlayerInfo& info = PlayerInfos()[0];
    int h = GetPedRef(me);
    info.money = info.displayMoney = money;
    int32_t world[kStatsSize / 4];
    memcpy(world, (void*)kStatsBlock, kStatsSize);
    memcpy((void*)kStatsBlock, stats.data(), kStatsSize);
    for (const auto& range : kWorldStats)
        for (int id = range[0]; id <= range[1]; ++id) Ref<int32_t>(kStatsBlock + (uintptr_t)(id - 120) * 4) = world[id - 120];
    for (int id = 0; id < kWorldFloatStats; ++id)
        Ref<int32_t>(kStatsBlock + kFloatStats + (uintptr_t)id * 4) = world[kFloatStats / 4 + id];  // the floats' bits
    memcpy(me->ClothesDesc(), clothes.data(), kClothesSize);
    RebuildPlayer(me, false);  // body from the profile's fat/muscle stats
    Cmd(0x048F).I(h).Run();    // REMOVE_ALL_CHAR_WEAPONS
    std::string guns;
    for (const Slot& s : slots) {
        if (s.type == 0 || s.type > 46) continue;  // eWeaponType ends with the parachute
        int model = -1;
        Cmd(0x0781).I(s.type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
        if (model > 0) LoadModelNow(model);
        Cmd(0x01B2).I(h).I(s.type).I(std::max(s.ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
        guns += " " + std::to_string(s.type) + "x" + std::to_string(s.ammo);
    }
    LOGI("Profile weapons given:%s", guns.empty() ? " none" : guns.c_str());
    Cmd(0x01B9).I(h).I(0).Run();  // fists in hand
    me->FightingStyle() = fight;
    if (version >= 4) Field<uint8_t>(me, 0x72E) = moves;
    info.maxHealth = maxHealth;
    info.maxArmour = maxArmour;
    if (version >= 2) {  // the host's rewards came with its world: the partner has its own
        uint8_t* own = (uint8_t*)&info + 0x14C;
        own[0] = perks[0], own[1] = perks[1], own[2] = perks[2], own[5] = perks[3], own[6] = perks[4];
    }
    if (version >= 5) ApplySideProgress(side);
    return true;
}

struct RemoteLook {
    std::vector<uint8_t> desc;
    uint16_t model = 0;    // a skin mod's player model (SET_PLAYER_MODEL), 0 = CJ in his clothes
    std::string special;   // a special character's (models 290-299 are filled by name, each game its own)
    uint8_t style = 0, moves = 0;  // the gym's fighting style (CPed +0x72D, +0x72E): the copy fights as its owner does
    int appliedTo = -1;    // handle of the ped it was last rebuilt on: a respawned ped can reuse the old one's address
    uint64_t waitMs = 0;   // a special character asked for here since
};
std::map<uint8_t, RemoteLook> g_looks;

constexpr uint64_t kSpecialWaitMs = 10000;  // a special character that does not come in this long: CJ instead

void HandleLook(uint8_t from, Reader& r) {
    std::vector<uint8_t> desc = r.Bytes(kClothesSize);
    uint16_t model = r.U16();
    std::string special = r.Str(16);
    uint8_t style = r.U8(), moves = r.U8();
    if (!r.Ok() || desc.size() != kClothesSize || from == Net().MyId()) return;
    RemoteLook& l = g_looks[from];
    if (l.style != style) LOGI("Player %d fights in style %d (moves %d)", from, style, moves);
    l.style = style;
    l.moves = moves;
    if (l.desc != desc || l.model != model || l.special != special) l.appliedTo = -1, l.waitMs = 0;
    l.desc = std::move(desc);
    l.model = model;
    l.special = std::move(special);
}

void HandleStats(uint8_t from, Reader& r) {
    std::vector<uint8_t> stats = r.Bytes(kStatsSize);
    if (!r.Ok() || stats.size() != kStatsSize || from == Net().MyId()) return;
    if (RemotePlayer* rp = Players::Instance().Get(from)) rp->stats = std::move(stats);
}

// The profile's stats go over the world's at once: the game takes the jump for a rise and shows its "Respect -
// Upgraded" help (GXT hash 3B797709), over the cutscene too. That help goes for a few seconds after a profile.
bool ShowsRespectUpgrade() {
    struct Key {
        const char* text;
        uint32_t hash;
    };
    const auto* keys = Ref<Key*>(0xC1B340);   // TheText's main table: sorted by hash
    uint32_t n = Ref<uint32_t>(0xC1B340 + 4), lo = 0, hi = n;
    while (keys && lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (keys[mid].hash == 0x3B797709u) return !strncmp((const char*)0xBAA7A0, keys[mid].text, 40);  // CHud::m_pHelpMessage
        if (keys[mid].hash < 0x3B797709u) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

using GenericSaveFn = bool(__cdecl*)(int);
GenericSaveFn g_origMenuSave = nullptr;
void PartnerSaved();
bool __cdecl HookMenuSave(int file) {
    bool ok = g_origMenuSave(file);
    if (ok && Net().Online() && Net().IsHost()) {
        Net().Send(proto::CH_RELIABLE, Writer(proto::G_HOST_SAVED));
        flow::RequestFreshWorld();  // the server keeps the world as saved
        LOGI("The host saved its game: the partners save their profiles");
    } else if (ok && Net().Online()) {
        PartnerSaved();  // at a save point of his own game: his money, skills and look go to the host's server too
    }
    return ok;
}

class Progress : public Module {
public:
    const char* Name() const override { return "Progress"; }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x619081, &HookMenuSave, &g_origMenuSave);  // C_PcSave::SaveSlot -> GenericSave
        Net().onProfile = [](const std::vector<uint8_t>& blob) {
            Progress& p = Instance();
            // an empty profile after a real one, or during a prologue, in the same session (the first one of a session
            // is the newcomer's): the host began a new game, which this game is not in yet
            if (blob.empty() && p.m_profileReceived && (!p.m_profile.empty() || !flow::InHostWorld())) p.m_resetPending = true;
            p.m_profile = blob;
            p.m_profileReceived = true;
            p.m_applied = false;
        };
        Net().SetHandler(proto::G_PLAYER_LOOK, &HandleLook);
        Net().SetHandler(proto::G_PLAYER_STATS, &HandleStats);
        Net().SetHandler(proto::G_STAT_SYNC, &HandleStatSync);
        Net().SetHandler(proto::G_SIDE_DONE, &HandleDone);
        Net().SetHandler(proto::G_HOST_SAVED, [](uint8_t from, Reader&) {
            if (from != Net().HostId() || Net().IsHost() || !Instance().m_applied || !core::IsPlayerReady()) return;
            Instance().SaveNow(true);
            ui::TheChat().AddSystem(TR("Хост сохранил игру. Ваши деньги, навыки и внешность тоже сохранены.",
                                       "The host saved the game. Your money, skills and look are saved too."));
        });
    }

    void OnSessionEnd() override {
        m_profileReceived = m_applied = m_resetPending = m_reset = false;
        m_profile.clear();
        g_looks.clear();
        g_statBased = false;
        g_statQueue.clear();
    }
    void OnSessionStart() override { g_statBased = false; }
    void OnPeerJoin(uint8_t) override { g_statBased = false; }  // the newcomer gets our records and skills
    void OnGameStarted() override {  // a new world: the partner's profile goes on top again
        m_applied = false;
        g_statBased = false;
        g_foreignGame = Net().Online() && !Net().IsHost();
        if (m_resetPending && flow::InHostWorld()) m_reset = true, m_resetPending = false;  // the new game's world
    }

    void OnPostProcess() override {
        if (core::IsPlayerReady() && !g_foreignGame) {  // a game of its own: the look it brings along
            const auto* d = (const uint8_t*)FindPlayerPed(0)->ClothesDesc();
            bool clothes = g_ownLook.size() != kClothesSize || memcmp(g_ownLook.data(), d, sizeof g_givenKeys);
            g_ownLook.assign(d, d + kClothesSize);
            if (clothes) WriteFileBytes(OwnLookPath(), d, kClothesSize);  // on a change of clothes (fat and muscle drift)
        }
        if (!Net().Online() || !core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        uint64_t now = NowMs();

        // only in the host's world: a game of its own (joined from single player, waiting) or an old world during a
        // new game's prologue is no profile, it would have gone to the server as one
        bool hostWorld = flow::InHostWorld();
        if (!Net().IsHost() && hostWorld && m_profileReceived && !m_applied && !m_resetPending) {
            m_applied = true;
            bool own = false;
            if (m_profile.empty()) {
                ui::TheChat().AddSystem(m_reset ? TR("Хост начал новую игру. Деньги, оружие и навыки у вас тоже начинаются заново.",
                                                     "The host started a new game. Your money, weapons and skills start over too.")
                                                : TR("Вы здесь впервые. Дальше деньги, навыки и внешность у вас будут свои.",
                                                     "You are here for the first time. From now on your money, skills and look "
                                                     "are your own."));
                m_reset = false;
            } else if (ApplyProfile(m_profile, own)) {
                LOGI("Profile applied (%zu bytes)", m_profile.size());
                m_quietUntil = now + 5000;
                ui::TheChat().AddSystem(TR("Загружены ваши деньги, навыки и внешность.", "Your money, skills and look are loaded."));
            } else {
                LOGW("Profile rejected (bad format), starting from the world's state");
            }
            g_lookOwn = own || WearOwnLook(m_profile.empty());
            if (!g_lookOwn) memcpy(g_givenKeys, me->ClothesDesc(), sizeof g_givenKeys);
            m_saved = m_profile;
            m_lastSaveMs = now;
            g_statBased = false;  // the profile's stats are ours: the shared ones start from them
        }

        if (m_quietUntil && Ref<uint8_t>(0xB5F851)) m_quietUntil = std::max(m_quietUntil, now + 3000);  // a cutscene: later
        if (now < m_quietUntil && ShowsRespectUpgrade()) {
            Cmd(0x03E6).Run();  // CLEAR_HELP
            m_quietUntil = 0;
            LOGI("The respect help the profile caused is cleared");
        }

        // look for the other game: at once when it changes (a changing room's items came by a check a second, in bursts
        // with some left out, #21), and every 10 s for late joiners
        {
            std::vector<uint8_t> desc((uint8_t*)me->ClothesDesc(), (uint8_t*)me->ClothesDesc() + kClothesSize);
            uint16_t model = me->ModelIndex();
            uint16_t fight = me->FightingStyle() | Field<uint8_t>(me, 0x72E) << 8;
            bool changed = desc != m_sentLook || model != m_sentModel || fight != m_sentFight;
            if (changed && !m_sentLook.empty()) m_lookChangedMs = now;
            if ((changed && now - m_lookSentMs >= 200) || now - m_lookSentMs > 10000) {
                Writer w(proto::G_PLAYER_LOOK);
                w.Bytes(desc.data(), desc.size());
                w.U16(model);
                w.Str(model >= 290 && model <= 299 ? Peds::Instance().SpecialName(model) : std::string(), 16);
                w.U8((uint8_t)fight);
                w.U8((uint8_t)(fight >> 8));
                Net().Send(proto::CH_RELIABLE, w);
                m_sentLook = std::move(desc);
                m_sentModel = model;
                m_sentFight = fight;
                m_lookSentMs = now;
            }
        }
        // stats: on change (3 s apart at most) and every 10 s
        if (now - m_lastLookMs >= 1000) {
            m_lastLookMs = now;
            StatTick(Net().IsHost() ? flow::InSharedWorld() && !Net().Peers().empty() : hostWorld && m_applied);
            std::vector<uint8_t> stats((uint8_t*)kStatsBlock, (uint8_t*)kStatsBlock + kStatsSize);
            if ((stats != m_sentStats && now - m_statsSentMs > 3000) || now - m_statsSentMs > 10000) {
                Writer w(proto::G_PLAYER_STATS);
                w.Bytes(stats.data(), stats.size());
                Net().Send(proto::CH_RELIABLE, w);
                m_sentStats = std::move(stats);
                m_statsSentMs = now;
            }
        }

        // remote looks: rebuild the remote ped once per new description (on foot: the rebuild swaps its clump)
        for (auto& [id, l] : g_looks) {
            RemotePlayer* rp = Players::Instance().Get(id);
            if (rp && rp->ped && l.style) rp->ped->FightingStyle() = l.style, Field<uint8_t>(rp->ped, 0x72E) = l.moves;
            if (!rp || !rp->ped || (rp->pedHandle == l.appliedTo && !rp->lookStale) || rp->ped->InVehicle()) continue;
            int model = l.model;
            if (model >= 290 && model <= 299) {  // here it is in whichever slot holds that name
                model = Peds::Instance().SpecialModelFor(l.special);
                if (model < 0) {
                    if (!l.waitMs) l.waitMs = now;
                    if (now - l.waitMs < kSpecialWaitMs) continue;  // still coming in
                    model = 0;
                }
            }
            bool skin = model != 0 && Players::SetModel(rp->ped, model);
            if (skin) Peds::Instance().SpecialWorn(model);
            if (!skin) {
                Players::SetModel(rp->ped, 0);
                memcpy(rp->ped->ClothesDesc(), l.desc.data(), kClothesSize);
                MuscleFrom muscle(rp->ped->ClothesDesc());
                RebuildPlayer(rp->ped, true);  // fat/muscle from the description, not from our stats
            }
            l.appliedTo = rp->pedHandle;
            rp->lookStale = false;
            LOGI("Look of player %d applied (model %d%s%s)", id, skin ? model : 0, l.special.empty() ? "" : ", ",
                 l.special.c_str());
        }

        // the partner's profile goes to the server when it changed (checked every 30 s), and 3 s after a new look settled
        // (bought clothes, a haircut): the host's world, reloaded at the next mission's marker, took the purchase back (#21)
        bool lookToSave = !Net().IsHost() && m_lookChangedMs > m_lastSaveMs && now - m_lookChangedMs >= 3000;
        if ((now - m_lastSaveMs >= 30000 || lookToSave) && hostWorld) SaveNow();
    }

    void SaveNow(bool always = false) {
        if (!Net().Online() || Net().IsHost() || !m_applied || !core::IsPlayerReady() || !flow::InHostWorld()) return;
        m_lastSaveMs = NowMs();
        std::vector<uint8_t> blob = CaptureProfile();
        if (blob == m_saved && !always) return;
        Writer w(proto::C_PROFILE_SAVE);
        w.Bytes(blob.data(), blob.size());
        Net().Send(proto::CH_RELIABLE, w);
        m_saved = std::move(blob);
        m_profile = m_saved;  // a reload of the world re-applies the latest state
        LOGI("Profile saved (%zu bytes)", m_saved.size());
    }

    static Progress& Instance() {
        static Progress p;
        return p;
    }

private:
    std::vector<uint8_t> m_profile, m_saved, m_sentLook, m_sentStats;
    uint16_t m_sentModel = 0;
    uint16_t m_sentFight = 0;
    bool m_profileReceived = false, m_applied = false;
    bool m_resetPending = false, m_reset = false;  // a new game emptied the profile: applied in the next world
    uint64_t m_lastSaveMs = 0, m_lastLookMs = 0, m_lookSentMs = 0, m_statsSentMs = 0, m_quietUntil = 0;
    uint64_t m_lookChangedMs = 0;  // our look last changed
};

void PartnerSaved() {
    if (!flow::InHostWorld()) return;
    Progress::Instance().SaveNow(true);
    ui::TheChat().AddSystem(TR("Ваши деньги, навыки и внешность сохранены и у хоста.",
                               "Your money, skills and look are saved on the host's side too."));
}

struct Reg {
    Reg() { core::RegisterModule(&Progress::Instance()); }
} g_reg;
}  // namespace

void SaveProfileNow() { Progress::Instance().SaveNow(); }

}  // namespace coop
