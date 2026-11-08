// The story's music minigames played by two.
//  * The beats and their keys are in the music file: both games playing the same beat track get the same ones. The
//    dance judges against its own playback; the lowrider contest against the host's (below).
//  * Lowrider contest ("Cesar Vialpando", SWEET6): the host bounces CJ's car as ever (the LOWGAME thread judges it);
//    the partner bounces a car of its own beside it, a girl of its own on the door as Cesar's on CJ's, under the meet's
//    cameras and the arrows of BDISPLY started in its game, judged here by LOWGAME's own rules, and its points go into
//    the host's score ($4072) against the same rival. Its arrows run on the host's playback position: each game on its
//    own drifted apart, and the arrows of the one whose music stream stalled stood still.
//  * Dance ("Life's a Beach", MUSIC1): the partner's game runs the same DANCE script beside the host, with its own
//    prompts, moves and feedback; its score goes into the host's ($21184, 2500 to pass).
// The host holds the minigame's end a few seconds for the partner's last points. A partner not there changes nothing.
#include "features/story/minigames/Minigames.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop::minigames {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { BEAT_PRELOAD = 1, BEAT_PLAY = 2, BEAT_STOP = 3, BEAT_DANCE = 5, BEAT_TIME = 6 };
enum : uint8_t { SCORE_JOIN = 1, SCORE_POINTS = 2, SCORE_DONE = 3 };
enum : uint8_t { KIND_LOWRIDER = 1, KIND_DANCE = 2 };
constexpr uint32_t kBdisply = 0x1CDEA;  // main.scm: the arrows' thread
constexpr int kDanceScript = 35;        // the streamed DANCE (dance.scm)
constexpr int kLoanerModel = 567, kHydraulics = 1087;  // a Savanna with hydraulics for a partner with no lowrider
constexpr int kGirlModel = 64;                         // HFYPRO (Cesar's girl in CJ's car is BFYPRO)
constexpr uintptr_t kStreamThread = 0xB5F8B8 + 0xE14;  // AEAudioHardware.m_pStreamThread

// SWEET6's cameras at the meet, for CJ's car there: the opening shot (0x9A41C) and the five it goes round every 5 s or
// on the camera button, each looking at the car (0x9A771)
const CVector kMeetCar{1793.496f, -1907.423f, 12.399f};
const CVector kOpenCam{1788.516f, -1910.318f, 14.890f}, kOpenLook{1789.355f, -1909.843f, 14.627f};
const CVector kMeetCams[5] = {{1803.978f, -1909.369f, 15.067f}, {1795.369f, -1900.999f, 14.179f},
                              {1788.793f, -1902.990f, 13.720f}, {1789.377f, -1912.195f, 14.067f},
                              {1796.409f, -1912.231f, 13.832f}};

// main.scm globals by byte offset (as the disassembly prints them)
int32_t& Var(uint32_t off) { return Ref<int32_t>(addr::ScriptSpace + off); }
constexpr uint32_t kLowRunning = 4068, kLowScore = 4072, kLowHit = 4172, kBeatStop = 4176, kBeatShown = 4192;
constexpr uint32_t kDanceFlags = 21180, kDanceScore = 21184, kDanceQuit = 21192;
// the dance's five fixed cameras, set by the mission that starts it (MUSIC1, a club): six float arrays of five (position
// x, y, z, then the point it looks at), $21260..$21376
constexpr uint32_t kDanceCams = 21260;
constexpr int kDanceCamFloats = 30;

std::string ScriptName(void* script) {
    return script ? std::string(Field<char[8]>(script, 0x08), strnlen(Field<char[8]>(script, 0x08), 8)) : std::string();
}

// ---------------------------------------------------------------- host
struct Taking {  // a partner who plays along
    int32_t lastBeat = -1, danceTotal = 0;
    bool done = false;
};
struct Host {
    uint8_t kind = 0;
    bool joined = false, done = false;
    uint64_t holdUntil = 0;  // the minigame's end held for the partner's last points
    std::map<uint8_t, Taking> took;
} g_host;

void SendBeat(uint8_t op, const std::function<void(Writer&)>& more = nullptr) {
    Writer w(proto::G_BEAT);
    w.U8(op);
    if (more) more(w);
    Net().Send(proto::CH_RELIABLE, w);
}

// Our playback position of the contest's track, ten times a second (the partner's arrows run on it)
void SendBeatTime() {
    static uint64_t lastMs = 0;
    if (NowMs() - lastMs < 100) return;
    lastMs = NowMs();
    int status = 0;
    Cmd(0x0953).Out(&status).Run();  // GET_BEAT_TRACK_STATUS: 3 playing
    int32_t t = status == 3 ? CallMethod<0x4F1530, int32_t>((void*)kStreamThread) : -1;  // CAEStreamThread::GetTrackPlayTime
    if (t < 0) return;
    Writer w(proto::G_BEAT);
    w.U8(BEAT_TIME);
    w.I32(t);
    Net().Send(proto::CH_STATE, w);
}

void ReleaseHold() {
    if (!g_host.holdUntil) return;
    g_host.holdUntil = 0;
    if (g_host.kind == KIND_LOWRIDER) Var(kLowRunning) = 0;               // the mission goes on to the result
    else Var(kDanceFlags) &= 0x7FFFFFFF;                                  // bit 31: the dance runs
    LOGI("Minigame %u: its end goes on (%s)", g_host.kind, g_host.done ? "the partner is done" : "no word from the partner");
}

void HandleScore(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), kind = r.U8();
    int32_t beat = r.I32(), points = r.I32();
    if (!r.Ok() || !Net().IsHost() || from == Net().MyId() || kind != g_host.kind || !g_host.kind) return;
    Taking& t = g_host.took[from];
    if (op == SCORE_JOIN) {
        g_host.joined = true;
        LOGI("Minigame %u: player %u plays along", kind, from);
    } else if (op == SCORE_POINTS && kind == KIND_LOWRIDER) {
        if (beat <= t.lastBeat) return;  // each beat once
        t.lastBeat = beat;
        Var(kLowScore) += std::clamp(points, 0, 500);
    } else if (op == SCORE_POINTS && kind == KIND_DANCE) {
        if (points > t.danceTotal) Var(kDanceScore) += std::min(points - t.danceTotal, 200);
        t.danceTotal = std::max(t.danceTotal, points);  // a quit zeroes only the partner's own run
    } else if (op == SCORE_DONE) {
        t.done = true;
        for (auto& [id, other] : g_host.took)  // the end waits for the last of those who play along
            if (!other.done) return;
        g_host.done = true;
        ReleaseHold();
    }
}

// ---------------------------------------------------------------- partner
struct Partner {
    uint8_t kind = 0;
    int stage = 0;  // 1 getting ready, 2 playing
    int16_t track = 0;
    CVector at;
    float heading = 0.0f;
    int car = -1;
    bool loaner = false, play = false;
    int32_t lastBeat = -1;
    int combo = 0, perfect = 0, good = 0, bad = 0, judged = 0;  // LOWGAME's $4120, $4108, $4112, $4116
    uint8_t index = 0;
    int32_t sent = -1;
    uint64_t since = 0;
    bool sawThread = false;
    float cams[kDanceCamFloats] = {};
    // the lowrider contest's own: the girl on our door and her mood (LOWGAME's $4080: -1 idle, 0..5), its counts of good
    // and bad beats (30@, 31@), our last move (19@), a missed beat (23@); the move she plays before her level's loop
    int girl = -1;
    bool girlOnCar = false, girlShared = false, ifp = false;
    int level = -1, shown = -1, cheers = 0, boos = 0, lastMove = 0;
    bool slip = false;
    const char* once = nullptr;
    const char* loop = nullptr;
    uint64_t onceMs = 0;
    // the meet's cameras on our car: -2 none, -1 the opening shot, 0..4 the round
    CVector hostCar, camShift;
    int cam = -2;
    uint64_t camMs = 0, placedMs = 0;
    bool camButton = false;
} g_part;

// The host's playback position of the contest's track, our clock's offset from it. Our own track starts as late as the
// host's word of its start comes, as do these: our arrows and judging run on them and stay with our music, and a stall
// of either game's stream no more moves the two apart nor holds the arrows still.
struct BeatClock {
    bool have = false;
    int64_t offset = 0;
    uint64_t recvMs = 0;
} g_clock;
void HearBeatTime(int32_t t) {
    int64_t sample = (int64_t)t - (int64_t)NowMs();
    if (!g_clock.have || std::abs(sample - g_clock.offset) > 250) g_clock.offset = sample;
    else g_clock.offset += (sample - g_clock.offset) / 8;  // the network's jitter smoothed out
    g_clock.have = true, g_clock.recvMs = NowMs();
}
// CAEAudioHardware::GetBeatInfo's read of the track's position: what GET_BEAT_PROXIMITY tells BDISPLY and our judge
using PlayTimeFn = int32_t(__fastcall*)(void*, void*);
PlayTimeFn g_origPlayTime = nullptr;
int32_t __fastcall HookBeatPlayTime(void* thread, void* edx) {
    int32_t own = g_origPlayTime(thread, edx);
    if (own < 0 || g_part.kind != KIND_LOWRIDER || g_part.stage != 2 || !g_clock.have || NowMs() - g_clock.recvMs > 2000)
        return own;
    return (int32_t)std::max<int64_t>(0, (int64_t)NowMs() + g_clock.offset);
}

void SendScore(uint8_t op, int32_t beat, int32_t points) {
    Writer w(proto::G_BEAT_SCORE);
    w.U8(op);
    w.U8(g_part.kind);
    w.I32(beat);
    w.I32(points);
    Net().Send(proto::CH_RELIABLE, w);
}

bool Eligible(const CVector& at, float range) {
    CPlayerPed* me = FindPlayerPed(0);
    return flow::InHostWorld() && core::IsPlayerReady() && me->Health() > 0.0f && !RunningMission() &&
           (me->GetPosition() - at).Length2D() < range;  // flat: the dance's spot has z -100 (the ground there)
}

// The lowrider's input: the right stick, past 64, in eight directions as LOWGAME reads it (0x1C934): 13 up, 11 up-right,
// 15 up-left, 10 right, 9 left, 16 down-right, 12 down-left, 14 down
int Direction(int x, int y) {
    if (x * x + y * y <= 64 * 64) return 0;
    float a = std::atan2((float)x, (float)-y) * 57.29578f;  // 0 up, positive to the right
    float m = std::fabs(a);
    if (m < 15.0f) return 13;
    if (m < 75.0f) return a > 0 ? 11 : 15;
    if (m < 105.0f) return a > 0 ? 10 : 9;
    if (m < 165.0f) return a > 0 ? 16 : 12;
    return 14;
}

// LOWGAME's grade of a beat (0x1A5AD): t ms from it, key the beat's, dir the stick's; -1 not yet. 0 perfect, 1 good,
// 2 bad, 3 too late or missed, 4 too early, 5 the wrong way
int Grade(int t, int key, int dir) {
    if (!dir) return t < -200 ? 3 : -1;
    if (dir != key) return 5;
    int a = std::abs(t);
    return a < 35 ? 0 : a < 50 ? 1 : a < 100 ? 2 : t < 0 ? 3 : 4;
}

// ---- the girl on our door: Cesar's mission seats one beside CJ, on the door of a Savanna (0x9A517), as a passenger
// in any other car; LOWGAME moves her (0x1ABAB..0x1B947)
void GirlAnim(const char* name, bool loop) {  // TASK_PLAY_ANIM_NON_INTERRUPTABLE, a move held on its last frame
    Cmd(0x0812).I(g_part.girl).S(name).S("LOWRIDER").F(4.0f).I(loop).I(0).I(0).I(loop ? 0 : 1).I(-1).Run();
}
void GirlPlay(const char* once, const char* loop) {  // a move (if any), then the loop
    GirlAnim(once ? once : loop, !once);
    g_part.once = once, g_part.loop = loop, g_part.onceMs = NowMs();
}
// of her move, 1 once it is over (the task starts its animation a frame or two after it is given)
float GirlProgress() {
    if (!g_part.once) return 1.0f;
    float t = NowMs() - g_part.onceMs < 250 ? 0.0f : 1.0f;
    if (Cmd(0x0611).I(g_part.girl).S(g_part.once).Run()) Cmd(0x0613).I(g_part.girl).S(g_part.once).Out(&t).Run();  // IS_CHAR_PLAYING_ANIM
    return t;
}
void SpawnGirl() {
    if (!IsModelLoaded(kGirlModel)) LoadModelNow(kGirlModel);  // behind the host's black screen
    int girl = -1;
    Cmd(0x009A).I(24).I(kGirlModel).F(g_part.at.x).F(g_part.at.y).F(g_part.at.z + 2.0f).Out(&girl).Run();  // CREATE_CHAR
    if (girl < 0) return;
    g_part.girl = girl;
    Cmd(0x0430).I(girl).I(g_part.car).I(0).Run();                  // WARP_CHAR_INTO_CAR_AS_PASSENGER
    if (!Cmd(0x0137).I(g_part.car).I(kLoanerModel).Run()) return;  // IS_CAR_MODEL
    Cmd(0x0792).I(girl).Run();                                     // CLEAR_CHAR_TASKS_IMMEDIATELY
    Cmd(0x0464).I(girl).I(g_part.car).F(0.62f).F(0.31f).F(-0.167f).I(0).F(0.0f).I(0).Run();  // ATTACH_CHAR_TO_CAR
    g_part.girlOnCar = true;
    if (!Cmd(0x04EE).S("LOWRIDER").Run()) Cmd(0x04ED).S("LOWRIDER").Run(), g_part.ifp = true;  // REQUEST_ANIMATION
}
// her mood rises a level for six good beats and falls one for two bad ones (from the second up), she bounces at each new
// move of ours and slips back to the first after a missed beat
void MoveGirl(int move) {
    int& level = g_part.level;
    if (!g_part.girlOnCar || !GetPed(g_part.girl) || !Cmd(0x04EE).S("LOWRIDER").Run()) return;  // HAS_ANIMATION_LOADED
    static const char* const kLoop[] = {"LRGIRL_L0_LOOP", "LRGIRL_L1_LOOP", "LRGIRL_L2_LOOP",
                                        "LRGIRL_L3_LOOP", "LRGIRL_L4_LOOP", "LRGIRL_L5_LOOP"};
    static const char* const kUp[] = {"LRGIRL_L0_TO_L1", "LRGIRL_L1_TO_L2", "LRGIRL_L2_TO_L3", "LRGIRL_L3_TO_L4", "LRGIRL_L4_TO_L5"};
    static const char* const kBounce[] = {"LRGIRL_L0_BNCE", "LRGIRL_L1_BNCE", "LRGIRL_L2_BNCE",
                                          "LRGIRL_L3_BNCE", "LRGIRL_L4_BNCE", "LRGIRL_L5_BNCE"};
    if (g_part.shown == -1 && !g_part.loop) GirlPlay(nullptr, "LRGIRL_IDLELOOP");
    if (level == -1 && move) level = 0;
    if (level == 0 && g_part.cheers > 5) level = 1, g_part.cheers = 0;
    if (level == 1 && g_part.cheers > 5) level = 2, g_part.cheers = g_part.boos = 0;
    for (int n = 2; n <= 4; ++n)
        if (level == n) {
            if (g_part.cheers > 5) level = n + 1, g_part.cheers = g_part.boos = 0;
            if (g_part.boos > 1) level = n - 1, g_part.boos = 0;
        }
    if (level == 5 && g_part.boos > 1) level = 4, g_part.cheers = g_part.boos = 0;
    if (level != g_part.shown) {
        int from = g_part.shown;
        if (from == -1 && level == 0) GirlPlay("LRGIRL_IDLE_TO_L0", kLoop[0]);
        else if (from >= 0 && level == from + 1) GirlPlay(kUp[from], kLoop[level]);
        else if (from > 1 && level == 1) GirlPlay("LRGIRL_L345_TO_L1", kLoop[1]);
        else if (from == 1 && level == 0) GirlPlay("LRGIRL_L12_TO_L0", kLoop[0]);
        g_part.shown = level;
    }
    // a new move: a bounce, unless she is still in the most of a change of level
    if (move && move != g_part.lastMove) {
        g_part.lastMove = move;
        if (level >= 0 && (!g_part.once || strstr(g_part.once, "_BNCE") || GirlProgress() >= 0.9f))
            GirlPlay(kBounce[level], g_part.loop ? g_part.loop : kLoop[level]);
    }
    if (g_part.slip) {
        g_part.slip = false;
        if (!move && level > 0) {
            if (!Cmd(0x0611).I(g_part.girl).S("LRGIRL_BDBNCE").Run()) GirlPlay("LRGIRL_BDBNCE", kLoop[1]);
            level = g_part.shown = 1;
        }
    }
    if (g_part.once && GirlProgress() >= 0.99f) {
        g_part.once = nullptr;
        GirlAnim(g_part.loop, true);
    }
}

// ---- SWEET6's cameras on our car (at that meet only): the opening shot, then round the five every 5 s or on the camera
// button, as the host's go round its own
void PlaceCamera() {
    CVector src = (g_part.cam < 0 ? kOpenCam : kMeetCams[g_part.cam]) + g_part.camShift, look = kOpenLook + g_part.camShift;
    if (CVehicle* v = g_part.cam >= 0 ? GetVehicle(g_part.car) : nullptr) look = v->GetPosition();
    Cmd(0x015F).F(src.x).F(src.y).F(src.z).F(0.0f).F(0.0f).F(0.0f).Run();  // SET_FIXED_CAMERA_POSITION
    Cmd(0x0160).F(look.x).F(look.y).F(look.z).I(2).Run();                 // POINT_CAMERA_AT_POINT, a jump cut
    g_part.placedMs = NowMs();
}
void StartCamera() {
    if ((g_part.hostCar - kMeetCar).Length() > 30.0f) return;  // another place: our own camera
    g_part.camShift = g_part.at - g_part.hostCar;
    g_part.cam = -1, g_part.camMs = NowMs();
    Cmd(0x0826).I(0).Run();  // DISPLAY_HUD: off, as the host's
    Cmd(0x0581).I(0).Run();  // DISPLAY_RADAR
    PlaceCamera();
}
void TurnCamera() {
    if (g_part.cam == -2 || SceneWatched()) return;
    bool button = Cmd(0x00E1).I(0).I(13).Run();  // IS_BUTTON_PRESSED: the camera button
    if ((button && !g_part.camButton) || NowMs() - g_part.camMs > 5000) {
        g_part.cam = (g_part.cam + 1) % 5, g_part.camMs = NowMs();
        PlaceCamera();
    } else if (Ref<uint8_t>(addr::TheCamera + 0x2B) && NowMs() - g_part.placedMs > 500) {  // m_bLookingAtPlayer: ours was taken back
        PlaceCamera();
    }
    g_part.camButton = button;
}

void FinishLowrider() {
    Var(kBeatStop) = 1;  // BDISPLY takes its arrows away and ends
    int status = 0;
    Cmd(0x0953).Out(&status).Run();       // GET_BEAT_TRACK_STATUS
    if (status == 3) Cmd(0x0955).Run();  // STOP_BEAT_TRACK
    Cmd(0x0221).I(0).I(0).Run();         // APPLY_BRAKES_TO_PLAYERS_CAR off
    Cmd(0x09EE).I(0).Run();              // FORCE_BIG_MESSAGE_AND_COUNTER off
    if (g_part.cam != -2) {
        if (!SceneWatched()) {  // the host's scene after the contest has the camera already
            Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT
            Cmd(0x0373).Run();  // SET_CAMERA_BEHIND_PLAYER
        }
        Cmd(0x0826).I(1).Run();  // DISPLAY_HUD
        Cmd(0x0581).I(1).Run();  // DISPLAY_RADAR
    }
    if (g_part.girl >= 0 && GetPed(g_part.girl)) Cmd(0x009B).I(g_part.girl).Run();  // DELETE_CHAR
    if (g_part.ifp) Cmd(0x04EF).S("LOWRIDER").Run();                             // REMOVE_ANIMATION
    if (GetVehicle(g_part.car)) {
        Cmd(0x020A).I(g_part.car).I(1).Run();  // LOCK_CAR_DOORS: unlocked
        if (g_part.loaner) Cmd(0x01C3).I(g_part.car).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED
    }
    SendScore(SCORE_DONE, 0, 0);
    LOGI("Lowrider contest: our part is over");
    g_part = {};
    g_clock = {};
}

void StartLowrider(int16_t track, const CVector& hostCar, float heading) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!Eligible(hostCar, 60.0f)) return;
    CVector right = PartnerSide(heading, Net().MyId(), 4.5f);
    CVector at = hostCar + right;  // beside the host's car, the same way
    int car = -1;
    bool loaner = false;
    if (CVehicle* v = me->InVehicle() ? me->Vehicle() : nullptr; v && v->Driver() == me && Cmd(0x0803).I(GetVehicleRef(v)).Run())
        car = GetVehicleRef(v);  // DOES_CAR_HAVE_HYDRAULICS: its own lowrider
    if (car < 0) {
        if (!IsModelLoaded(kLoanerModel)) LoadModelNow(kLoanerModel);  // behind the host's black screen
        Cmd(0x00A5).I(kLoanerModel).F(at.x).F(at.y).F(at.z).Out(&car).Run();  // CREATE_CAR
        if (car < 0) return;
        loaner = true;
        Cmd(0x06E9).I(kHydraulics).Run();  // REQUEST_VEHICLE_MOD
        if (!Cmd(0x06EA).I(kHydraulics).Run()) LoadModelNow(kHydraulics);  // HAS_VEHICLE_MOD_LOADED
        int mod = -1;
        Cmd(0x06E7).I(car).I(kHydraulics).Out(&mod).Run();  // ADD_VEHICLE_MOD
    }
    int h = GetPedRef(me);
    if (!me->InVehicle() || GetVehicleRef(me->Vehicle()) != car) {
        if (me->InVehicle()) {
            CVector p = me->GetPosition();
            Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        Cmd(0x036A).I(h).I(car).Run();  // WARP_CHAR_INTO_CAR
    }
    Cmd(0x00AB).I(car).F(at.x).F(at.y).F(at.z).Run();             // SET_CAR_COORDINATES
    Cmd(0x0175).I(car).F(heading * 57.29578f).Run();               // SET_CAR_HEADING
    Cmd(0x020A).I(car).I(4).Run();                                 // LOCK_CAR_DOORS: locked in
    Cmd(0x0221).I(0).I(1).Run();                                   // APPLY_BRAKES_TO_PLAYERS_CAR
    Cmd(0x09EE).I(1).Run();                                        // FORCE_BIG_MESSAGE_AND_COUNTER
    Cmd(0x0952).I(track).Run();                                    // PRELOAD_BEAT_TRACK
    g_part = {};
    g_clock = {};
    g_part.kind = KIND_LOWRIDER, g_part.stage = 1, g_part.track = track, g_part.at = at, g_part.heading = heading;
    g_part.car = car, g_part.loaner = loaner, g_part.since = NowMs(), g_part.hostCar = hostCar;
    SpawnGirl();
    LOGI("Lowrider contest: our car beside the host's (%s, %s)", loaner ? "a loaner" : "our own",
         g_part.girlOnCar ? "a girl on its door" : g_part.girl >= 0 ? "a girl beside us" : "no girl");
}

// LOWGAME's word on every four beats, given ahead of the next one's points (0x1C5BF): four perfect ones are a combo
// (DNC_002 once one runs: ten times the points), more good than bad keep it going (DNC_004), else it ends (DNC_003)
void Combo() {
    if (g_part.judged < 4) return;
    const char* text;
    if (g_part.perfect == 4) {
        text = g_part.combo >= 1 ? "DNC_002" : "DNC_004";
        g_part.combo = g_part.combo >= 1 ? 2 : 1;
    } else if (g_part.good > g_part.bad || g_part.combo == 2) {
        text = "DNC_004";
        g_part.combo = 1;
    } else {
        text = "DNC_003";
        g_part.combo = 0;
    }
    Cmd(0x00BA).S8(text).I(50000).I(7).Run();  // PRINT_BIG: up till the next word
    g_part.judged = g_part.perfect = g_part.good = g_part.bad = 0;
}

void JudgeBeat(int dir) {
    int t = 0, key = 0, n = 0;
    Cmd(0x07B1).I(0).Out(&t).Out(&key).Out(&n).Run();  // GET_BEAT_PROXIMITY: the nearest beat
    if (n == g_part.lastBeat) return;
    int grade = Grade(t, key, dir);
    if (grade < 0) return;
    g_part.lastBeat = n;
    Combo();
    int points = (grade == 0 ? 50 : grade == 1 ? 20 : grade == 2 ? 10 : 0) * (g_part.combo == 2 ? 10 : 1);
    ++g_part.judged;
    if (grade == 0) ++g_part.perfect;
    else if (grade <= 2) ++g_part.good;  // LOWGAME counts a bad hit with the good ones ($4112)
    else ++g_part.bad;
    if (grade <= 1) ++g_part.cheers;
    else ++g_part.boos;
    g_part.slip = grade >= 3;
    Var(kLowHit) = grade == 0 ? 3 : grade == 1 ? 2 : 1;  // BDISPLY's flash of the ring
    Cmd(0x015B).I(0).I(200).I(grade == 0 ? 255 : grade == 1 ? 200 : grade == 2 ? 110 : 50).Run();  // SHAKE_PAD
    SendScore(SCORE_POINTS, n, points);
}

void PlayLowrider() {
    int t = 0, key = 0, n = 0;
    Cmd(0x07B1).I(-1).Out(&t).Out(&key).Out(&n).Run();  // GET_BEAT_PROXIMITY: the last beat
    if (key == 33) return FinishLowrider();             // the song's end
    int lx = 0, ly = 0, rx = 0, ry = 0;
    Cmd(0x0494).I(0).Out(&lx).Out(&ly).Out(&rx).Out(&ry).Run();  // GET_POSITION_OF_ANALOGUE_STICKS
    int dir = Direction(rx, ry);
    JudgeBeat(dir);
    MoveGirl(dir);
    TurnCamera();
}

void StartDance(uint8_t index, const CVector& at, float heading, int16_t track, const float* cams) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!Eligible(at, 40.0f)) return;
    CVector right = PartnerSide(heading * 0.0174533f, Net().MyId(), 1.8f);
    g_part = {};
    g_part.kind = KIND_DANCE, g_part.stage = 1, g_part.index = index, g_part.track = track, g_part.heading = heading;
    g_part.at = at + right;  // beside the host, out of its CLEAR_AREA
    g_part.since = NowMs();
    for (int i = 0; i < kDanceCamFloats; ++i)  // the host's cameras moved over to our spot: the x and y arrays
        g_part.cams[i] = cams[i] + (i / 5 % 3 == 0 ? right.x : i / 5 % 3 == 1 ? right.y : 0.0f);
    if (me->InVehicle()) {  // out where it is (the spot's z is -100, the ground the dance finds)
        CVector p = me->GetPosition();
        Cmd(0x0362).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    Cmd(0x08A9).I(index).Run();  // STREAM_SCRIPT
    LOGI("Dance: ours beside the host's");
}

void* DanceThread() {
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (ScriptName(s) == "dance") return s;
    return nullptr;
}

void UpdatePartner() {
    if (!g_part.kind) return;
    bool hostOn = HostMissionRecent(0);
    if (g_part.kind == KIND_LOWRIDER) {
        if (!hostOn || !GetVehicle(g_part.car)) return FinishLowrider();
        if (g_part.stage == 1) {
            MoveGirl(0);
            if (g_part.girlOnCar && !g_part.girlShared)  // her copy in the host's game on its copy of our car the same way
                g_part.girlShared = SharePersonOnCar(true, g_part.girl, g_part.car, 0.62f, 0.31f, -0.167f);
            int status = 0;
            Cmd(0x0953).Out(&status).Run();
            if (!g_part.play || status != 2) {
                if (NowMs() - g_part.since > 30000) FinishLowrider();
                return;
            }
            Cmd(0x0954).Run();  // PLAY_BEAT_TRACK
            Var(kBeatShown) = 0;
            Cmd(0x004F).I((int32_t)kBdisply).End().Run();  // START_NEW_SCRIPT BDISPLY: the arrows
            SendScore(SCORE_JOIN, 0, 0);
            g_part.stage = 2;
            StartCamera();
            return;
        }
        return PlayLowrider();
    }
    // the dance
    if (g_part.stage == 1) {
        if (!Cmd(0x08AB).I(g_part.index).Run()) {  // HAS_STREAMED_SCRIPT_LOADED
            if (NowMs() - g_part.since > 10000) g_part = {};
            return;
        }
        memcpy(&Var(kDanceCams), g_part.cams, sizeof g_part.cams);  // no mission of ours set them
        Var(kDanceQuit) = 0;
        Cmd(0x0913).I(g_part.index).F(g_part.at.x).F(g_part.at.y).F(-100.0f).F(g_part.heading).I(g_part.track).I(-1).End().Run();
        SendScore(SCORE_JOIN, 0, 0);
        g_part.stage = 2, g_part.since = NowMs();
        return;
    }
    void* dance = DanceThread();
    if (dance) {
        g_part.sawThread = true;
        if (!hostOn) Var(kDanceQuit) = 1;  // the host's mission is over: ours ends cleanly
        if (Var(kDanceScore) != g_part.sent) g_part.sent = Var(kDanceScore), SendScore(SCORE_POINTS, 0, g_part.sent);
        return;
    }
    if (!g_part.sawThread && NowMs() - g_part.since < 5000) return;
    SendScore(SCORE_DONE, 0, 0);
    Cmd(0x090F).I(g_part.index).Run();  // MARK_STREAMED_SCRIPT_AS_NO_LONGER_NEEDED
    LOGI("Dance: ours is over");
    g_part = {};
}

void HandleBeat(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    if (op == BEAT_PRELOAD) {
        int16_t track = r.I16();
        uint8_t kind = r.U8();
        Vec3 at = r.Vec();
        float heading = r.F32();
        if (r.Ok() && kind == KIND_LOWRIDER && track >= 0 && track < 14 && std::isfinite(heading) && !g_part.kind)
            StartLowrider(track, {at.x, at.y, at.z}, heading);
    } else if (op == BEAT_PLAY) {
        if (g_part.kind == KIND_LOWRIDER) g_part.play = true;
    } else if (op == BEAT_TIME) {
        int32_t t = r.I32();
        if (r.Ok() && g_part.kind == KIND_LOWRIDER && t >= 0) HearBeatTime(t);
    } else if (op == BEAT_STOP) {
        if (g_part.kind == KIND_LOWRIDER) FinishLowrider();
    } else if (op == BEAT_DANCE) {
        uint8_t index = r.U8();
        Vec3 at = r.Vec();
        float heading = r.F32();
        int16_t track = r.I16();
        float cams[kDanceCamFloats];
        bool finite = std::isfinite(heading);
        for (float& c : cams) c = r.F32(), finite = finite && std::isfinite(c);
        if (r.Ok() && finite && index == kDanceScript && track >= 0 && track < 14 && !g_part.kind)
            StartDance(index, {at.x, at.y, at.z}, heading, track, cams);
    }
}

class Minigames : public Module {
public:
    const char* Name() const override { return "Minigames"; }
    void OnInstallHooks() override {
        Net().SetHandler(proto::G_BEAT, &HandleBeat);
        Net().SetHandler(proto::G_BEAT_SCORE, &HandleScore);
        if (!mem::RedirectCallFn(0x4D8FEF, &HookBeatPlayTime, &g_origPlayTime))  // CAEAudioHardware::GetBeatInfo
            LOGW("Beat clock hook skipped: no call at 0x4D8FEF (another mod?)");
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (Net().IsHost()) {
            if (g_host.holdUntil && (g_host.done || NowMs() > g_host.holdUntil)) ReleaseHold();
            if (g_host.kind && !g_host.holdUntil && !HostReplicating()) g_host = {};
            if (g_host.kind == KIND_LOWRIDER && !Net().Peers().empty()) SendBeatTime();
        } else {
            UpdatePartner();
        }
    }
    void OnSessionEnd() override {
        if (g_part.kind == KIND_LOWRIDER && core::IsPlayerReady()) FinishLowrider();  // our camera, HUD and girl back
        g_host = {}, g_part = {}, g_clock = {};
    }
    void OnGameStarted() override { g_host = {}, g_part = {}, g_clock = {}; }
    void OnPeerLeave(uint8_t id) override {
        g_host.took.erase(id);
        g_host.joined = !g_host.took.empty();
        if (!g_host.joined) ReleaseHold();
    }

    static Minigames& Instance() {
        static Minigames m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Minigames::Instance()); }
} g_reg;
}  // namespace

// The command's first argument as written in the script, a number or a global. START_NEW_STREAMED_SCRIPT reads the
// new thread's arguments through CTheScripts::ScriptParams too: after it there was the dance's -1 instead of its index
// 35, and the partner never danced.
static int32_t FirstArg(const uint8_t* ip) {
    switch (ip ? ip[2] : 0) {
        case 0x01: return *(const int32_t*)(ip + 3);
        case 0x02: return Var(*(const uint16_t*)(ip + 3));
        case 0x04: return (int8_t)ip[3];
        case 0x05: return *(const int16_t*)(ip + 3);
        default: return -1;
    }
}

void AfterCommand(void* script, uint16_t op, const uint8_t* ip, bool story) {
    const auto* p = (const int32_t*)0xA43C78;  // CTheScripts::ScriptParams, as the command read them
    if (op == 0x0952 && story) {               // PRELOAD_BEAT_TRACK of the mission: the lowrider contest
        CPlayerPed* me = FindPlayerPed(0);
        CVehicle* v = me && me->InVehicle() ? me->Vehicle() : nullptr;
        if (!v) return;
        g_host = {};
        g_host.kind = KIND_LOWRIDER;
        CVector at = v->GetPosition();
        float heading = v->GetHeading();
        int16_t track = (int16_t)p[0];
        SendBeat(BEAT_PRELOAD, [&](Writer& w) { w.I16(track), w.U8(KIND_LOWRIDER), w.Vec({at.x, at.y, at.z}), w.F32(heading); });
        LOGI("Lowrider contest (track %d): the partners may join", track);
    } else if (op == 0x0954 && story && g_host.kind == KIND_LOWRIDER) {  // PLAY_BEAT_TRACK
        SendBeat(BEAT_PLAY);
    } else if (op == 0x0955 && g_host.kind == KIND_LOWRIDER) {  // STOP_BEAT_TRACK, LOWGAME's at the song's end
        SendBeat(BEAT_STOP);
    } else if (op == 0x0913 && story && FirstArg(ip) == kDanceScript) {  // START_NEW_STREAMED_SCRIPT: the dance
        // the new thread heads the active list, named "noname" by CRunningScript::Init till its SCRIPT_NAME 'DANCE' runs
        // (waiting for "dance" here, the partner never danced)
        void* dance = Ref<void*>(0xA8B42C);
        if (!dance || (ScriptName(dance) != "noname" && ScriptName(dance) != "dance")) return;
        const auto* loc = (const int32_t*)((uint8_t*)dance + 0x3C);  // its locals 0@..5@
        float x, y, z, heading;
        memcpy(&x, &loc[0], 4), memcpy(&y, &loc[1], 4), memcpy(&z, &loc[2], 4), memcpy(&heading, &loc[3], 4);
        if (loc[5] != -1) return;  // a dance against a club's dancer: the host's own
        g_host = {};
        g_host.kind = KIND_DANCE;
        int16_t track = (int16_t)loc[4];
        SendBeat(BEAT_DANCE, [&](Writer& w) {
            w.U8((uint8_t)kDanceScript), w.Vec({x, y, z}), w.F32(heading), w.I16(track);
            for (int i = 0; i < kDanceCamFloats; ++i) w.F32(Ref<float>(addr::ScriptSpace + kDanceCams + i * 4));
        });
        LOGI("Dance (track %d): the partners may join", track);
    }
    // the minigame's end, held while the partner's last points are on their way: LOWGAME's SET_VAR_INT $4068 0, DANCE's
    // CLEAR_GLOBAL_VAR_BIT_CONST $21180 31 (the mission reads the score right after)
    if (!g_host.joined || g_host.done || g_host.holdUntil || !ip || ip[2] != 0x02) return;
    uint16_t var = *(const uint16_t*)(ip + 3);
    std::string name = ScriptName(script);
    if (op == 0x0004 && g_host.kind == KIND_LOWRIDER && name == "lowgame" && var == kLowRunning && Var(kLowRunning) == 0) {
        Var(kLowRunning) = 1;
        g_host.holdUntil = NowMs() + 3000;
    } else if (op == 0x08C0 && g_host.kind == KIND_DANCE && name == "dance" && var == kDanceFlags) {
        Var(kDanceFlags) |= (int32_t)0x80000000;
        g_host.holdUntil = NowMs() + 3000;
    }
    if (g_host.holdUntil) LOGI("Minigame %u: its end waits for the partner's last points", g_host.kind);
}

bool OwnDance() { return g_part.kind == KIND_DANCE && g_part.stage == 2; }

bool HostMinigame() { return g_host.kind != 0; }

bool PartnerShowsOwn(void* script) {
    return g_host.kind == KIND_LOWRIDER && g_host.joined && ScriptName(script) == "lowgame";
}

}  // namespace coop::minigames
