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
// An emote (Emotes.cpp) goes the same way: while the player plays one, the animation of its block is the activity.
// The blocks this side streamed in for the copies go back to the streamer once no copy plays them.
//
// The same channel (G_PLAYER_ANIM) carries them all.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/anims/Emotes.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

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
    "LAPDAN3",  "BOMBER",   "CARRY",    "PARACHUTE", "INT_HOUSE", "INT_OFFICE", "INT_SHOP"};

bool IsReplicated(int g, int id) {
    if (g == ANIM_GROUP_DEFAULT) return id >= ANIM_ID_PHONE_IN && id <= ANIM_ID_PHONE_TALK;  // phone_in, phone_out, phone_talk
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
};

// the activity animation the player plays now: the strongest one of those blocks or of the emote's
bool CurrentActivity(CPed* ped, Activity& out) {
    void* best = nullptr;
    int emote = emotes::PlayingBlock();
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        int block, idx;
        if (anim::BlendDelta(a) < 0.0f || anim::BlendAmount(a) < 0.3f || !anim::Locate(anim::Hierarchy(a), block, idx) ||
            (block != emote && !IsActivityBlock(block)))
            continue;
        if (best && anim::BlendAmount(a) <= anim::BlendAmount(best)) continue;
        best = a;
        out.block = (int16_t)block;
        out.idx = (int16_t)idx;
    }
    if (!best) return false;
    out.flags = anim::Flags(best);
    out.time = anim::CurrentTime(best);
    return true;
}

bool EnsureBlockLoaded(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    if (anim::BlockLoaded(idx)) return true;
    RequestModel(anim::kIfpBase + idx, 2);
    LoadAllRequestedModels(false);
    return anim::BlockLoaded(idx);
}

const char* BlockForGroup(int group) { return group == ANIM_GROUP_PLAYIDLES ? "playidles" : nullptr; }

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
        Net().SetHandler(proto::G_PLAYER_ANIM, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
    }

    void OnPostProcess() override {
        // in a cutscene both games animate the partner themselves (ScenePartner), idle variations included
        if (!Net().Online() || !core::IsPlayerReady() || Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running
        CPlayerPed* me = FindPlayerPed(0);
        SendIdle(me);
        SendActivity(me);
        ApplyActivities();
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
                if (!r.Ok() || w.a.block < 0 || w.a.idx < 0) w.a = Activity{};
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
            const char* block = BlockForGroup(group);
            if (block && !EnsureBlockLoaded(block)) return;
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
        m_streamed.clear();  // a new world
    }
    void OnSessionEnd() override {
        m_remote.clear();
        ReleaseUnused();
    }
    void OnPeerLeave(uint8_t id) override {
        m_remote.erase(id);
        ReleaseUnused();
    }

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
    };

    void SendIdle(CPlayerPed* me) {
        // the replicated animation currently playing on the local player (blending in or full)
        int group = -1, id = -1;
        for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
            if (!IsReplicated(anim::Group(a), anim::Id(a)) || anim::BlendDelta(a) < 0.0f) continue;
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

    void SendActivity(CPlayerPed* me) {
        Activity now;
        bool on = !me->InVehicle() && CurrentActivity(me, now);
        // the same one again shows up as its time going back (a looped one going back is its next lap)
        bool same = on && now.block == m_sent.block && now.idx == m_sent.idx;
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
            LOGI("Activity animation %s/%d (%s)", (const char*)anim::Block(now.block), now.idx, restarted ? "again" : "started");
        } else {
            w.U8(OP_ACTIVITY_END);
            LOGI("Activity animation ended");
        }
        Net().Send(proto::CH_RELIABLE, w);
        m_sent = on ? now : Activity{};
    }

    // the other players' activities on their copies, once each copy is on foot and the IFP is in memory
    void ApplyActivities() {
        for (auto& [id, w] : m_remote) {
            if (w.seq == w.appliedSeq) continue;
            RemotePlayer* rp = Players::Instance().Get(id);
            if (!rp || !rp->ped || rp->mode != RemotePlayer::Mode::OnFoot || !rp->ped->RwObject()) continue;
            void* clump = rp->ped->RwObject();
            void* h = nullptr;
            if (w.a.block >= 0 && !(h = anim::Resolve(w.a.block, w.a.idx))) {  // its IFP is being loaded
                if (w.a.block < Ref<int32_t>(0xB4EA30) && !anim::BlockLoaded(w.a.block)) m_streamed.insert(w.a.block);
                continue;
            }
            if (w.applied)
                for (void* a = anim::First(clump); a; a = anim::Next(a))
                    if (anim::Hierarchy(a) == w.applied && anim::BlendDelta(a) >= 0.0f) anim::FadeOut(a);
            if (h) {
                if (void* a = anim::Blend(clump, h, w.a.flags, 4.0f)) {
                    float t = w.a.time + (float)(NowMs() - w.recvMs) / 1000.0f, total = anim::TotalTime(h);
                    anim::SetCurrentTime(a, (w.a.flags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
                }
                LOGI("Remote player %d activity animation %s/%d", id, (const char*)anim::Block(w.a.block), w.a.idx);
            } else if (w.applied) {
                // its stance back: the player's task starts its walk set again only when its move state changes, and a
                // ped left with no animation crashes the game (see Peds.cpp)
                Call<0x4D4610, void*>(clump, Field<int32_t>(rp->ped, 0x4D4), 3 /* ANIM_ID_IDLE */, 4.0f);  // CAnimManager::BlendAnimation
                LOGI("Remote player %d activity animation ended", id);
            }
            w.applied = h;
            w.appliedSeq = w.seq;
            ReleaseUnused();
        }
    }

    // the blocks streamed in for the copies that no copy wants any more (the associations still blending out hold theirs)
    void ReleaseUnused() {
        for (auto it = m_streamed.begin(); it != m_streamed.end();) {
            bool used = false;
            for (auto& [id, w] : m_remote) used |= w.a.block == *it;
            if (used) {
                ++it;
                continue;
            }
            Call<0x409C10>(anim::kIfpBase + *it);  // CStreaming::SetModelIsDeletable
            it = m_streamed.erase(it);
        }
    }

    int m_lastGroup = -1, m_lastId = -1;
    Activity m_sent;
    float m_lastTime = 0.0f;
    std::map<uint8_t, Wanted> m_remote;
    std::set<int> m_streamed;  // IFP blocks requested here for the copies, not in memory before
};

struct Reg {
    Reg() { core::RegisterModule(&Anims::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
