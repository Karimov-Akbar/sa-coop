// The speech of the host's missions (the lines people say outside cutscenes) in the partner's game too. A mission
// loads a line into one of the four mission audio slots, attaches it to whoever says it (or puts it at a place) and
// plays it; the subtitle comes with the texts (Missions.cpp). The host's game reports these calls of its mission
// scripts (CAudioEngine::PreloadMissionAudio, Attach..., SetMissionAudioPosition, PlayLoadedMissionAudio,
// ClearMissionAudio) and the partner's game makes the same ones: the line comes from the partner's copy of the speaker,
// so it is as loud as that person is near. A line said by nobody the partner's game has (a person far away, a phone
// call) is heard only near the host.
#include <cmath>
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kAudioEngine = 0xB6BC90;  // AudioEngine
constexpr int kSlots = 4;
constexpr float kNearHost = 50.0f;          // a line of nobody here is heard this near the host
constexpr uint64_t kLoadWaitMs = 5000;      // a line still loading this long after its play is dropped
enum : uint8_t { OP_LOAD = 1, OP_ATTACH = 2, OP_POSITION = 3, OP_PLAY = 4, OP_CLEAR = 5 };
enum : uint8_t { TO_NOBODY = 0, TO_HOST = 1, TO_PARTNER = 2, TO_PED = 3, TO_VEHICLE = 4 };

using PreloadFn = void(__fastcall*)(void*, void*, uint8_t, int32_t);
using SlotFn = void(__fastcall*)(void*, void*, uint8_t);
using PositionFn = void(__fastcall*)(void*, void*, uint8_t, CVector*);
using AttachFn = void(__fastcall*)(void*, void*, uint8_t, CEntity*);
PreloadFn g_origPreload = nullptr;
SlotFn g_origPlay = nullptr, g_origClear = nullptr;
PositionFn g_origPosition = nullptr;
AttachFn g_origAttachPed = nullptr, g_origAttachObject = nullptr, g_origAttachPhysical = nullptr;

void* Engine() { return (void*)kAudioEngine; }

// a script of the host's story mission (or a thread it started: AUDIOL says the lines of "Cesar Vialpando"'s scenes), or
// a story phone call between missions (main.scm's MOB_LA1, MOB_SF and the others raise the mission flag, no mission runs:
// the partner heard nothing of the call, Dr.BaldBall 28.10; MOB_RAN's are side calls, not replicated), with a partner to
// hear it
bool Sharing() {
    void* s = CurrentScript();
    return HostReplicating() && (MissionThread(s) || (s && !RunningMission() && !strncmp(Field<char[8]>(s, 0x08), "mob_", 4)));
}

Writer Begin(uint8_t op, uint8_t slot) {
    Writer w(proto::G_MISSION_AUDIO);
    w.U8(op);
    w.U8(slot);
    return w;
}

void SendAttach(uint8_t slot, CEntity* e) {
    uint8_t to = TO_NOBODY;
    uint16_t id = 0;
    if (e && e->IsPed()) {
        if (e == FindPlayerPed(0)) to = TO_HOST;
        else if (Players::Instance().FindByPed((CPed*)e)) to = TO_PARTNER;
        else if (NetPed* np = Peds::Instance().Find((CPed*)e)) to = TO_PED, id = np->netId;
    } else if (e && e->IsVehicle()) {
        if (NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e)) to = TO_VEHICLE, id = nv->netId;
    }
    Writer w = Begin(OP_ATTACH, slot);
    w.U8(to);
    w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void __fastcall HookPreload(void* ae, void* edx, uint8_t slot, int32_t sample) {
    g_origPreload(ae, edx, slot, sample);
    if (!Sharing() || slot >= kSlots) return;
    Writer w = Begin(OP_LOAD, slot);
    w.I32(sample);
    Net().Send(proto::CH_RELIABLE, w);
}
void __fastcall HookAttachPed(void* ae, void* edx, uint8_t slot, CEntity* e) {
    g_origAttachPed(ae, edx, slot, e);
    if (Sharing() && slot < kSlots) SendAttach(slot, e);
}
void __fastcall HookAttachObject(void* ae, void* edx, uint8_t slot, CEntity* e) {
    g_origAttachObject(ae, edx, slot, e);
    if (Sharing() && slot < kSlots) SendAttach(slot, nullptr);  // objects are not shared: heard near the host
}
void __fastcall HookAttachPhysical(void* ae, void* edx, uint8_t slot, CEntity* e) {
    g_origAttachPhysical(ae, edx, slot, e);
    if (Sharing() && slot < kSlots) SendAttach(slot, e);
}
void __fastcall HookPosition(void* ae, void* edx, uint8_t slot, CVector* pos) {
    g_origPosition(ae, edx, slot, pos);
    if (!Sharing() || slot >= kSlots || !pos) return;
    Writer w = Begin(OP_POSITION, slot);
    w.Vec({pos->x, pos->y, pos->z});
    Net().Send(proto::CH_RELIABLE, w);
}
void __fastcall HookPlay(void* ae, void* edx, uint8_t slot) {
    g_origPlay(ae, edx, slot);
    if (Sharing() && slot < kSlots) Net().Send(proto::CH_RELIABLE, Begin(OP_PLAY, slot));
}
void __fastcall HookClear(void* ae, void* edx, uint8_t slot) {
    g_origClear(ae, edx, slot);
    if (Sharing() && slot < kSlots) Net().Send(proto::CH_RELIABLE, Begin(OP_CLEAR, slot));
}

// ---------------------------------------------------------------- partner
struct Slot {
    bool loaded = false;  // a line of the host's is in it
    bool heard = false;   // attached to somebody here or put at a place
    bool playing = false; // its play came: started once it is in memory
    uint64_t playMs = 0;
};
Slot g_slots[kSlots];

bool NearHost() {
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    return host && host->hasState && me && host->state.area == me->AreaCode() &&
           (host->state.pos - me->GetPosition()).Length2D() < kNearHost;
}

CEntity* Speaker(uint8_t to, uint16_t id) {
    switch (to) {
        case TO_HOST: {
            const RemotePlayer* host = Players::Instance().Get(Net().HostId());
            return host ? (CEntity*)host->ped : nullptr;
        }
        case TO_PARTNER: return FindPlayerPed(0);
        case TO_PED: {
            int h = Peds::Instance().HandleOf(id);
            return h >= 0 ? (CEntity*)GetPed(h) : nullptr;
        }
        case TO_VEHICLE: {
            NetVehicle* nv = Vehicles::Instance().Find(id);
            return nv && IsVehicleValid(nv->veh) ? (CEntity*)nv->veh : nullptr;
        }
        default: return nullptr;
    }
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), slot = r.U8();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || slot >= kSlots) return;
    Slot& s = g_slots[slot];
    switch (op) {
        case OP_LOAD: {
            int32_t sample = r.I32();
            if (!r.Ok() || sample < 0) return;
            // our own player on the phone: its call's lines are in these slots (the host's line is not played here)
            if (void* intel = FindPlayerPed(0)->Intelligence(); intel && CallMethod<0x681740, void*>((uint8_t*)intel + 4, 1600)) {
                s = Slot{};
                return;
            }
            g_origPreload(Engine(), nullptr, slot, sample);
            s = Slot{};
            s.loaded = true;
            LOGD("Mission speech: slot %d loads %d", slot, sample);
            break;
        }
        case OP_ATTACH: {
            uint8_t to = r.U8();
            uint16_t id = r.U16();
            if (!r.Ok() || !s.loaded) return;
            if (CEntity* e = Speaker(to, id)) {
                if (e->IsPed()) g_origAttachPed(Engine(), nullptr, slot, e);
                else g_origAttachPhysical(Engine(), nullptr, slot, e);
                s.heard = true;
            }
            break;
        }
        case OP_POSITION: {
            Vec3 p = r.Vec();
            if (!r.Ok() || !s.loaded) return;
            CVector at{p.x, p.y, p.z};
            g_origPosition(Engine(), nullptr, slot, &at);
            s.heard = true;
            break;
        }
        case OP_PLAY:
            if (!s.loaded) return;
            if (!s.heard && !NearHost()) {  // nobody here says it: far from the host it is not heard
                LOGD("Mission speech: slot %d is not heard here", slot);
                return;
            }
            s.playing = true;
            s.playMs = NowMs();
            break;
        case OP_CLEAR:
            if (s.loaded) g_origClear(Engine(), nullptr, slot);
            s = Slot{};
            break;
        default: break;
    }
}

// a line plays once it is in memory here (it may still be loading when the host's started)
void PlayLoaded() {
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (!s.playing) continue;
        if (CallMethod<0x5072A0, int8_t>(Engine(), (uint8_t)i) == 1) {  // CAudioEngine::GetMissionAudioLoadingStatus: loaded
            g_origPlay(Engine(), nullptr, (uint8_t)i);
            s.playing = false;
        } else if (NowMs() - s.playMs > kLoadWaitMs) {
            s.playing = false;
        }
    }
}

class MissionSpeech : public Module {
public:
    const char* Name() const override { return "MissionSpeech"; }
    void OnInstallHooks() override {
        mem::HookFn(0x507290, &HookPreload, &g_origPreload, "CAudioEngine::PreloadMissionAudio");
        mem::HookFn(0x5072B0, &HookPlay, &g_origPlay, "CAudioEngine::PlayLoadedMissionAudio");
        mem::HookFn(0x5072F0, &HookClear, &g_origClear, "CAudioEngine::ClearMissionAudio");
        mem::HookFn(0x507300, &HookPosition, &g_origPosition, "CAudioEngine::SetMissionAudioPosition");
        mem::HookFn(0x507310, &HookAttachPed, &g_origAttachPed, "CAudioEngine::AttachMissionAudioToPed");
        mem::HookFn(0x507320, &HookAttachObject, &g_origAttachObject, "CAudioEngine::AttachMissionAudioToObject");
        mem::HookFn(0x507330, &HookAttachPhysical, &g_origAttachPhysical, "CAudioEngine::AttachMissionAudioToPhysical");
        Net().SetHandler(proto::G_MISSION_AUDIO, &Handle);
    }
    void OnPreProcess() override {
        if (Net().Online() && !Net().IsHost() && core::IsPlayerReady()) PlayLoaded();
    }
    void OnSessionEnd() override { Reset(); }
    void OnGameStarted() override { Reset(); }

private:
    static void Reset() {
        for (Slot& s : g_slots) s = Slot{};
    }
};

MissionSpeech g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
