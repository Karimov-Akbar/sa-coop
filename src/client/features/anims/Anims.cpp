// Replication of animations that the pad replay cannot reproduce.
//
// Idle animations: CTaskSimplePlayerOnFoot::PlayIdleAnimations (0x6872C0) starts a random
// "playidles" animation after 20 s of inactivity, judged by the *local* pad touch timer. For the
// remote player ped this would use the wrong timer and a different random choice, so the function is
// skipped for it; instead the owner reports the idle animation it plays and the other side blends the
// same animation on the remote ped. A phone call's (phone_in, phone_talk, phone_out of the ped group, from
// CTaskComplexUseMobilePhone) go the same way; the phone in the hand goes with the player's state (Players).
//
// What a player does at a place (a barber's chair, a tattoo, a meal, a drink machine, the gym, a dance, a parachute):
// a script or an attractor plays these on the player, never its pad. The owner reports the one it plays (IFP block and
// index, as for the mirrored peds) and the other game plays it on its copy of the player.
//
// The same channel (G_PLAYER_ANIM) carries both.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int ANIM_GROUP_PLAYIDLES = 49;
constexpr uint16_t ANIMATION_IDLE_FLAG = 0x200;  // set by PlayIdleAnimations on its animations

enum : uint8_t { OP_PLAY = 1, OP_STOP_IDLE = 2, OP_ACTIVITY = 3, OP_ACTIVITY_END = 4 };

// groups whose animations are replicated as events (they are not produced by the pad replay)
constexpr int kReplicatedGroups[] = {ANIM_GROUP_PLAYIDLES};
constexpr int ANIM_GROUP_DEFAULT = 0, ANIM_ID_PHONE_IN = 145, ANIM_ID_PHONE_TALK = 147;

// IFP blocks of what a player does at a place
constexpr const char* kActivityBlocks[] = {
    "HAIRCUTS", "TATTOOS",  "FOOD",     "VENDING", "GYMNASIUM", "BENCHPRESS", "FREEWEIGHTS", "DANCING", "CASINO",
    "BAR",      "CLOTHES",  "SHOP",     "BSKTBALL", "POOL",     "KISSING",    "STRIP",       "LAPDAN1", "LAPDAN2",
    "LAPDAN3",  "BOMBER",   "CARRY",    "PARACHUTE", "INT_HOUSE", "INT_OFFICE", "INT_SHOP", "GFUNK", "RUNNINGMAN",
    "WOP"};  // the last three: the dance minigame's moves (dance.scm), a girlfriend's dance

// a hit's flinch: the shot partials and HIT_* of the default group (SHOT_PARTIAL 28 .. HIT_BEHIND 40)
bool IsHitAnim(int id) { return id >= 28 && id <= 40; }
// and a blow's in a fist fight: HITA_1..3 of the fighting style's group (MELEE_1 33 .. PISTLWHP 45). A punch in a row
// plays those, not HIT_*: of 67 punches only 3 flinched on the copy.
bool IsFlinch(int g, int id) { return (g == ANIM_GROUP_DEFAULT && IsHitAnim(id)) || (g >= 33 && g <= 45 && id >= 219 && id <= 221); }

bool IsReplicated(int g, int id) {
    if (g == ANIM_GROUP_DEFAULT) return (id >= ANIM_ID_PHONE_IN && id <= ANIM_ID_PHONE_TALK) || IsHitAnim(id);  // the phone; flinches
    // a gun's reload (ANIM_ID_RELOAD, CROUCHRELOAD of the gun groups PYTHON .. SPRAYCAN and the player's rocket ones): the
    // copy's clip never runs dry (Players), so it never reloaded by itself
    if ((g >= 11 && g <= 31) || (g >= 57 && g <= 59)) return id == 226 || id == 227;
    if (IsFlinch(g, id)) return true;
    for (int r : kReplicatedGroups)
        if (r == g) return true;
    return false;
}

bool IsActivityBlock(int block) {
    const char* name = (const char*)anim::Block(block);  // CAnimBlock::name
    for (const char* b : kActivityBlocks)
        if (!_stricmp(name, b)) return true;
    return false;
}

struct Activity {
    int16_t block = -1, idx = -1;
    uint16_t flags = 0;
    float time = 0.0f;
    // it holds the player (a task other than the plain on-foot one plays it: a script's animation, a fall, a parachute,
    // a machine), so the copy's own tasks must keep off it. A script plays even a skydive as a partial animation, so the
    // flags cannot tell; one carried while walking (a crate, a tray) does not hold
    bool held = false;
    // CAnimBlendAssociation::m_fSpeed: the gym's machines and the dance go at the player's pace (SET_CHAR_ANIM_SPEED);
    // the copy pedalled at 1.0 whatever its owner did
    float speed = 1.0f;
    bool roll = false;  // a crouched roll: it holds the copy too (its own stick would step it forward)
};

constexpr int TASK_SIMPLE_NAMED_ANIM = 401;         // what a script's TASK_PLAY_ANIM gives
// get up, fall, fall and get up, fall and stay down; busted (TASK_SIMPLE_BE_ARRESTED: the hands up for the cop)
constexpr int kDownTasks[] = {205, 207, 208, 209, 1102};
constexpr int TASK_COMPLEX_IN_AIR_AND_LAND = 240;

void* FindTask(CPed* ped, int type) {
    void* intel = ped->Intelligence();
    return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, type) : nullptr;  // CTaskManager::FindActiveTaskByType
}

// The activity animation the player plays now, the strongest of: one of those blocks; a script's TASK_PLAY_ANIM of any
// IFP (the parachute's freefall is PED/FALL_SKYDIVE, missions and minigames play theirs on CJ); and while it is down
// (knocked over, run over, thrown off a bike) any but its walk set, a face's, an arm's (secondary task) or a looped
// partial idle (IDLE_ARMED, IDLE_TIRED): the other game's copy has every proof and never fell by itself. The falls are
// partial themselves (KO_*, KD_*: laid over the rest, not the upper body only).
bool CurrentActivity(CPed* ped, Activity& out) {
    void* named = FindTask(ped, TASK_SIMPLE_NAMED_ANIM);
    void* script = named ? Field<void*>(named, 0x08) : nullptr;  // CTaskSimpleAnim::m_pAnim, only compared
    bool down = false;
    for (int t : kDownTasks) down = down || FindTask(ped, t);
    int walk = Field<int32_t>(ped, 0x4D4);  // CPed::m_nAnimGroup
    // a go-to task walks it (a shop door's walk-in): the copy walks by a stick of its own (Players), and the arm's push of
    // the door (Walk_DoorPartial) goes as an activity that does not hold it
    bool walked = TaskWalks(ped);
    void* best = nullptr;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        int block, idx;
        // one blending in counts at once: a slow cross-fade's gap went out as an end (the copy blended its idle in the air)
        if (anim::BlendDelta(a) < 0.0f || (anim::BlendDelta(a) == 0.0f && anim::BlendAmount(a) < 0.3f) ||
            !anim::Locate(anim::Hierarchy(a), block, idx))
            continue;
        uint16_t f = anim::Flags(a);
        bool fall = !(f & (anim::F_INDESTRUCTIBLE | anim::F_SECONDARY)) && (f & (anim::F_PARTIAL | anim::F_LOOPED)) !=
                    (anim::F_PARTIAL | anim::F_LOOPED) && anim::Group(a) != walk;
        bool push = walked && anim::Group(a) == 0 && anim::Id(a) == 153;  // ANIM_ID_WALK_DOORPARTIAL (a car door's are not)
        // A crouched roll (CROUCH_ROLL_L/R) starts on the stick held sideways while aiming: the copy, its stick a moment
        // late, rolled late, or made a crouched step instead by the split-screen rules (see Patches). It rolls as its owner.
        bool roll = anim::Group(a) == 0 && (anim::Id(a) == 57 || anim::Id(a) == 59);  // ANIM_GROUP_DEFAULT
        if (!IsActivityBlock(block) && a != script && !(down && fall) && !push && !roll) continue;
        if (best && anim::BlendAmount(a) <= anim::BlendAmount(best)) continue;
        best = a;
        out.roll = roll;
        out.block = (int16_t)block;
        out.idx = (int16_t)idx;
    }
    if (!best) return false;
    out.flags = anim::Flags(best);
    out.time = anim::CurrentTime(best);
    out.speed = Field<float>(best, 0x24);
    out.held = (ActivePrimaryTaskType(ped) != 0 && !walked) || out.roll;  // CTaskSimplePlayerOnFoot is type 0
    return true;
}

// CEventInAir::AffectsPed: a copy holding its owner's activity (a parachute, a knock into the air) starts no fall of its
// own. CTaskSimpleInAir blended FALL_fall over the owner's animation, and the copy fell where its owner skydived.
using InAirAffectsFn = bool(__fastcall*)(void*, void*, CPed*);
InAirAffectsFn g_origInAirAffects = nullptr;
bool __fastcall HookInAirAffectsPed(void* ev, void* edx, CPed* ped) {
    RemotePlayer* rp = Players::Instance().FindByPed(ped);
    if (rp && rp->animHeld) {
        static uint64_t saidMs = 0;
        if (NowMs() - saidMs > 5000) saidMs = NowMs(), LOGD("Remote player %d: its own fall in the air kept off", rp->id);
        return false;
    }
    return g_origInAirAffects(ev, edx, ped);
}

// A full-body animation that plays something: the frame update reads the first one's data without looking (0x4D1750),
// and a body left with none stands in the bind pose
bool HasBodyPose(CPed* ped) {
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
        if (!(anim::Flags(a) & anim::F_PARTIAL) && anim::BlendDelta(a) >= 0.0f && Field<void*>(anim::Hierarchy(a), 4))
            return true;
    return false;
}

std::string DescribeAnims(CPed* ped) {
    std::string s;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        int block = -1, idx = -1;
        anim::Locate(anim::Hierarchy(a), block, idx);
        s += Format(" %s/%d:%.2f/%.1f/%04X%s", block >= 0 ? (const char*)anim::Block(block) : "?", block >= 0 ? idx : anim::Id(a),
                    anim::BlendAmount(a), anim::BlendDelta(a), anim::Flags(a), Field<void*>(anim::Hierarchy(a), 4) ? "" : "!seq");
    }
    return s.empty() ? " none" : s;
}

// the block in memory, else asked for: this idle is left out and a later one plays. Loaded at once it took the whole
// streaming queue with it, and the game stood still for a second or more (issue #43).
bool EnsureBlockLoaded(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    if (anim::BlockLoaded(idx)) return true;
    RequestModel(anim::kIfpBase + idx, 2);
    return false;
}

const char* BlockForGroup(int group) { return group == ANIM_GROUP_PLAYIDLES ? "playidles" : nullptr; }

// A clothes shop's changing room door (props2.ide: CJ_SUBURB_DOOR_2, CJ_GAP_DOOR_, CJ_PRO_DOOR_01, CJ_BINCO_DOOR,
// CJ_Victim_DOOR, CJ_DS_DOOR): each game's clothes.scm makes its own and swings it (PLAY_OBJECT_ANIM) for its own player
// only, so the other player's copy walked into the booth through a shut door (28.10). The copy's walk in or out swings
// the one here, as the script pairs them: CLO_In and CLO_Pose_Out with CLO_Pose_Out_O, CLO_Out and CLO_Pose_In with
// CLO_Pose_In_O.
constexpr int kBoothDoors[] = {2873, 2875, 2876, 2877, 2878, 2879};

void SwingBoothDoor(CPed* copy, int block, void* h, float t) {
    void* b = (void*)anim::Block(block);
    if (_stricmp((const char*)b, "CLOTHES")) return;
    auto is = [&](const char* n) { return h == Call<0x4D42F0, void*>(n, b); };  // CAnimManager::GetAnimation
    const char* name = is("CLO_IN") || is("CLO_POSE_OUT") ? "CLO_POSE_OUT_O" : is("CLO_OUT") || is("CLO_POSE_IN") ? "CLO_POSE_IN_O" : nullptr;
    void* swing = name ? Call<0x4D42F0, void*>(name, b) : nullptr;
    CPoolHeader* pool = swing ? ObjectPool() : nullptr;
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        auto* clump = (uint8_t*)o->RwObject();
        if (std::find(std::begin(kBoothDoors), std::end(kBoothDoors), o->ModelIndex()) == std::end(kBoothDoors) || !clump ||
            clump[0] != 2 /* rpCLUMP */ || (o->GetPosition() - copy->GetPosition()).Length() > 4.0f)
            continue;
        if (!Call<0x4D6760, bool>(clump)) Call<0x4D6720>(clump);  // RpAnimBlendClumpIsInitialized, RpAnimBlendClumpInit
        // flags 0 as the script's: once, held shut on its last frame (BlendAnimation restarts a finished one)
        if (void* a = anim::Blend(clump, swing, 0, 4.0f)) anim::SetCurrentTime(a, std::min(t, anim::TotalTime(swing)));
        LOGD("The other player's changing room door: %s", name);
        return;
    }
}

// a group's block is in memory and its associations are made (a gun's group comes with the gun in the copy's hand)
bool GroupReady(int group) {
    uintptr_t g = Ref<uintptr_t>(0xB4EA34) + (uintptr_t)group * 0x14;  // CAnimManager::ms_aAnimAssocGroups
    uintptr_t block = Ref<uintptr_t>(g + 0x00);                         // CAnimBlendAssocGroup::m_pAnimBlock
    return block && Ref<void*>(g + 0x04) && Ref<uint8_t>(block + 0x10);  // associations, CAnimBlock::bLoaded
}

using PlayIdleFn = void(__fastcall*)(void*, void*, CPlayerPed*);
PlayIdleFn g_origPlayIdle = nullptr;
void __fastcall HookPlayIdleAnimations(void* task, void* edx, CPlayerPed* player) {
    if (Players::Instance().FindByPed(player)) return;  // remote player: idles come from the network
    g_origPlayIdle(task, edx, player);
}

class Anims : public Module {
public:
    const char* Name() const override { return "Anims"; }

    void OnInstallHooks() override {
        mem::HookFn(0x6872C0, &HookPlayIdleAnimations, &g_origPlayIdle, "CTaskSimplePlayerOnFoot::PlayIdleAnimations");
        mem::HookFn(0x4B0C00, &HookInAirAffectsPed, &g_origInAirAffects, "CEventInAir::AffectsPed");
        Net().SetHandler(proto::G_PLAYER_ANIM, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
    }

    void OnPostProcess() override {
        // in a cutscene both games animate the partner themselves (ScenePartner), idle variations included
        bool on = Net().Online() && core::IsPlayerReady() && !Ref<uint8_t>(0xB5F851);  // CCutsceneMgr::ms_running
        if (on) {
            CPlayerPed* me = FindPlayerPed(0);
            SendIdle(me);
            SendHit(me);
            SendActivity(me);
            ApplyActivities();
        }
        // after the activities came: decided before, the frame they came in let the copy's own fall in the air start
        // (CEventInAir), and its FALL_fall faded the owner's skydive out at once
        for (auto& [id, w] : m_remote)
            if (RemotePlayer* rp = Players::Instance().Get(id)) {
                bool held = on && Holds(rp->ped, w);
                if (held != rp->animHeld) LOGD("Remote player %d %s", id, held ? "held by its owner's activity" : "free of the activity");
                rp->animHeld = held;
            }
        if (on) KeepPoses();
    }

    void Handle(uint8_t from, Reader& r) {
        uint8_t op = r.U8();
        if (op == OP_ACTIVITY || op == OP_ACTIVITY_END) {
            Wanted& w = m_remote[from];
            w.a = Activity{};
            if (op == OP_ACTIVITY) {
                w.a.block = r.I16();
                w.a.idx = r.I16();
                w.a.flags = r.U16();
                w.a.time = r.F32();
                w.a.held = r.U8() != 0;
                w.a.speed = r.F32();
                if (!r.Ok() || w.a.block < 0 || w.a.idx < 0 || !(w.a.speed >= 0.0f && w.a.speed <= 10.0f)) w.a = Activity{};
            }
            w.recvMs = NowMs();
            ++w.seq;
            return;
        }
        RemotePlayer* rp = Players::Instance().Get(from);
        if (!rp || !rp->ped || !core::IsPlayerReady()) return;
        void* clump = rp->ped->RwObject();
        if (!clump) return;
        if (op == OP_PLAY) {
            int group = r.I16();
            int id = r.I16();
            float delta = r.F32();
            if (!r.Ok() || !IsReplicated(group, id)) return;
            if (IsFlinch(group, id) && rp->animHeld) return;  // down: the activity shows it
            const char* block = BlockForGroup(group);
            if (block && !EnsureBlockLoaded(block)) return;
            if (!block && group != ANIM_GROUP_DEFAULT && !GroupReady(group)) return;  // not in hand here (yet)
            uintptr_t grp = Ref<uintptr_t>(0xB4EA34) + (uintptr_t)group * 0x14;  // CAnimManager::ms_aAnimAssocGroups
            int first = (int)Ref<uint32_t>(grp + 0x0C), count = (int)Ref<uint32_t>(grp + 0x08);  // m_IdOffset, m_NumAnims
            if (id < first || id >= first + count) return;  // from the network
            void* a = Call<0x4D4610, void*>(clump, group, id, std::clamp(delta, 1.0f, 50.0f));  // CAnimManager::BlendAnimation
            if (a) anim::Flags(a) |= ANIMATION_IDLE_FLAG;
            LOGI("Remote player %d animation: group %d id %d (%s)", from, group, id, a ? "ok" : "failed");
        } else if (op == OP_STOP_IDLE) {
            for (void* a = anim::First(clump); a; a = anim::Next(a))
                if ((anim::Flags(a) & ANIMATION_IDLE_FLAG) && IsReplicated(anim::Group(a), anim::Id(a))) {
                    anim::BlendDelta(a) = -8.0f;
                    anim::Flags(a) |= anim::F_AUTO_REMOVE;  // deleted once out
                }
        }
    }

    void OnGameStarted() override {
        m_lastGroup = m_lastId = -1;
        m_sent = Activity{};
        m_remote.clear();
    }
    void OnSessionEnd() override { m_remote.clear(); }
    void OnPeerLeave(uint8_t id) override { m_remote.erase(id); }

    static Anims& Instance() {
        static Anims a;
        return a;
    }

private:
    struct Wanted {
        Activity a;
        uint64_t recvMs = 0;
        uint32_t seq = 0, appliedSeq = 0;
        void* applied = nullptr;  // the hierarchy blended on the copy
        bool overWalk = false;    // it was laid over the copy's own walk (partial, not holding it: a door's push)
    };

    void SendIdle(CPlayerPed* me) {
        // the replicated animation currently playing on the local player (blending in or full)
        int group = -1, id = -1;
        for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
            if (!IsReplicated(anim::Group(a), anim::Id(a)) || anim::BlendDelta(a) < 0.0f) continue;
            if (IsFlinch(anim::Group(a), anim::Id(a))) continue;  // SendHit's
            group = anim::Group(a);
            id = anim::Id(a);
            break;
        }
        if (group == m_lastGroup && id == m_lastId) return;
        Writer w(proto::G_PLAYER_ANIM);
        if (id >= 0) {
            w.U8(OP_PLAY);
            w.I16((int16_t)group);
            w.I16((int16_t)id);
            w.F32(8.0f);
        } else {
            w.U8(OP_STOP_IDLE);
        }
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Local animation %s: group %d id %d", id >= 0 ? "started" : "stopped", id >= 0 ? group : m_lastGroup,
             id >= 0 ? id : m_lastId);
        m_lastGroup = group;
        m_lastId = id;
    }

    // A hit makes the player flinch for half a second (a partial over the body), in its own game only: the copy has every
    // proof (hits are applied where their target is owned) and never flinched under fire. Each new flinch goes out once.
    void SendHit(CPlayerPed* me) {
        for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
            if (!IsFlinch(anim::Group(a), anim::Id(a)) || anim::BlendDelta(a) < 0.0f) continue;
            if (a == m_lastHit) return;
            m_lastHit = a;
            Writer w(proto::G_PLAYER_ANIM);
            w.U8(OP_PLAY);
            w.I16((int16_t)anim::Group(a));
            w.I16((int16_t)anim::Id(a));
            w.F32(8.0f);
            Net().Send(proto::CH_RELIABLE, w);
            LOGD("Hit animation %d sent", anim::Id(a));
            return;
        }
        m_lastHit = nullptr;
    }

    void SendActivity(CPlayerPed* me) {
        Activity now;
        bool on = !me->InVehicle() && CurrentActivity(me, now);
        // the same one again shows up as its time going back (a looped one going back is its next lap)
        bool same = on && now.block == m_sent.block && now.idx == m_sent.idx && now.held == m_sent.held;
        // a new pace goes out at most four times a second (the gym bike's changes with every tap)
        bool paced = same && std::fabs(now.speed - m_sent.speed) > 0.1f && NowMs() - m_pacedMs > 250;
        if (paced) same = false, m_pacedMs = NowMs();
        bool restarted = same && !(now.flags & anim::F_LOOPED) && now.time + 0.25f < m_lastTime;
        m_lastTime = on ? now.time : 0.0f;
        if (on ? same && !restarted : m_sent.block < 0) return;
        Writer w(proto::G_PLAYER_ANIM);
        if (on) {
            w.U8(OP_ACTIVITY);
            w.I16(now.block);
            w.I16(now.idx);
            w.U16(now.flags);
            w.F32(now.time);
            w.U8(now.held);
            w.F32(now.speed);
            if (paced) LOGD("Activity animation %s/%d at speed %.2f", (const char*)anim::Block(now.block), now.idx, now.speed);
            else LOGI("Activity animation %s/%d (%s)", (const char*)anim::Block(now.block), now.idx, restarted ? "again" : "started");
        } else {
            w.U8(OP_ACTIVITY_END);
            LOGI("Activity animation ended");
        }
        Net().Send(proto::CH_RELIABLE, w);
        m_sent = on ? now : Activity{};
    }

    // the copy plays its owner's full-body activity: Players, Peds and the in-air hook keep its own tasks off it
    static bool Holds(CPed* ped, const Wanted& w) {
        if (!ped || !w.applied || !w.a.held || ped->InVehicle() || !ped->RwObject()) return false;
        for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
            if (anim::Hierarchy(a) == w.applied) return anim::BlendDelta(a) >= 0.0f;
        return false;
    }

    // A copy whose last full-body animation went (or lost its data) stands in the bind pose: the T-pose seen while its
    // owner played a minigame. Its walk set's idle comes back, and its animations go to the log once to find out why.
    void KeepPoses() {
        for (auto& [id, rp] : Players::Instance().All()) {
            CPed* ped = rp.ped;
            if (!ped || !ped->RwObject() || ped->InVehicle() || ped->Health() <= 0.0f || HasBodyPose(ped)) continue;
            uint64_t& at = m_poseFixMs[id];
            if (NowMs() - at < 1000) continue;
            bool first = at == 0;
            at = NowMs();
            if (first) LOGW("Remote player %d had no body animation:%s", id, DescribeAnims(ped).c_str());
            Call<0x4D4610, void*>(ped->RwObject(), Field<int32_t>(ped, 0x4D4), 3 /* ANIM_ID_IDLE */, 4.0f);  // CAnimManager::BlendAnimation
        }
    }

    // the other players' activities on their copies, once each copy is on foot and the IFP is in memory
    void ApplyActivities() {
        for (auto& [id, w] : m_remote) {
            if (w.seq == w.appliedSeq) continue;
            RemotePlayer* rp = Players::Instance().Get(id);
            // in a script person's place its animations are that one's (Players' FollowAvatar): its owner's wait
            if (!rp || !rp->ped || rp->mode != RemotePlayer::Mode::OnFoot || !rp->ped->RwObject() || rp->avatar >= 0) continue;
            void* clump = rp->ped->RwObject();
            void* h = nullptr;
            if (w.a.block >= 0 && !(h = anim::Resolve(w.a.block, w.a.idx))) continue;  // its IFP is being loaded
            if (rp->ped->InVehicle()) {
                // thrown off its bike or out of its car in its game: the fall plays at its place now, not after the wait
                // for an exit animation (Players warps it out standing 4 s later)
                if (!h || !w.a.held) continue;
                const auto& p = rp->state.pos;
                Cmd(0x0362).I(rp->pedHandle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
                rp->exitTaskMs = NowMs();
            }
            // its own fall in the air blends FALL_fall over anything: it ends here, the in-air hook keeps a new one off
            if (h && w.a.held && FindTask(rp->ped, TASK_COMPLEX_IN_AIR_AND_LAND)) {
                CallMethod<0x601640>(rp->ped->Intelligence(), true);  // CPedIntelligence::FlushImmediately
                LOGD("Remote player %d: its fall in the air ended for the activity", id);
            }
            // the same animation at a new pace (or a new restart time) keeps its blend: only its speed changes
            if (h && h == w.applied) {
                void* kept = nullptr;
                for (void* a = anim::First(clump); a; a = anim::Next(a))
                    if (anim::Hierarchy(a) == h && anim::BlendDelta(a) >= 0.0f) kept = a;
                float expected = w.a.time + (float)(NowMs() - w.recvMs) / 1000.0f * w.a.speed;
                if (kept && ((w.a.flags & anim::F_LOOPED) || std::fabs(anim::CurrentTime(kept) - expected) < 0.5f)) {
                    Field<float>(kept, 0x24) = w.a.speed;
                    w.appliedSeq = w.seq;
                    continue;
                }
            }
            if (w.applied)
                for (void* a = anim::First(clump); a; a = anim::Next(a))
                    if (anim::Hierarchy(a) == w.applied && anim::BlendDelta(a) >= 0.0f) anim::FadeOut(a);
            if (h) {
                void* a = anim::Blend(clump, h, w.a.flags, 4.0f);
                if (a) {
                    Field<float>(a, 0x24) = w.a.speed;
                    float t = w.a.time + (float)(NowMs() - w.recvMs) / 1000.0f * w.a.speed, total = anim::TotalTime(h);
                    anim::SetCurrentTime(a, (w.a.flags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
                    SwingBoothDoor(rp->ped, w.a.block, h, t);
                }
                LOGI("Remote player %d activity animation %s/%d (flags %04X%s)", id, (const char*)anim::Block(w.a.block), w.a.idx,
                     w.a.flags, a ? "" : ", not blended");
            } else if (w.applied && !w.overWalk) {
                // its stance back: the player's task starts its walk set again only when its move state changes, and a
                // ped left with no animation crashes the game (see Peds.cpp). A crouched one keeps its crouch: the idle over
                // it stood it up after a roll, and the crouch came back 0.4 s later (Players). Not after one laid over its
                // walk: the idle stopped the copy walking in through a door, and it started off again (WALK_start).
                bool crouched = rp->ped->IsDucking();
                Call<0x4D4610, void*>(clump, crouched ? 0 : Field<int32_t>(rp->ped, 0x4D4),
                                      crouched ? 55 /* ANIM_ID_WEAPON_CROUCH */ : 3 /* ANIM_ID_IDLE */, 4.0f);  // CAnimManager::BlendAnimation
                LOGI("Remote player %d activity animation ended", id);
            }
            w.applied = h;
            w.overWalk = h && (w.a.flags & anim::F_PARTIAL) && !w.a.held;
            w.appliedSeq = w.seq;
        }
    }

    int m_lastGroup = -1, m_lastId = -1;
    void* m_lastHit = nullptr;  // the flinch last sent (its association while it plays)
    Activity m_sent;
    float m_lastTime = 0.0f;
    uint64_t m_pacedMs = 0;
    std::map<uint8_t, Wanted> m_remote;
    std::map<uint8_t, uint64_t> m_poseFixMs;  // a copy's idle last given back (KeepPoses)
};

struct Reg {
    Reg() { core::RegisterModule(&Anims::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
