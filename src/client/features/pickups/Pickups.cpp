// Pickups both players see. Both games start from the same save, so their pickups match by model and place (the
// position is stored in eighths of a metre). What one player takes goes in the other game too:
//  * a pickup that comes back (weapons, health, armour on the street) is gone there for the same time;
//  * one that does not is removed and recorded as collected, so a script waiting for it (the host's mission) goes on;
//    a horseshoe or an oyster is counted there as well (the engine counts those, not the scripts);
//  * a snapshot photographed here counts there, an asset's takings collected here are gone there;
//  * a spray tag painted here is painted there.
// Pickups that appear in one game only are announced: the loot of a ped killed here, a dropped jetpack, the pickups of
// the host's mission. Pickups of the scripts both games run (the street weapons, the collectibles) are not, and neither
// are save disks, info icons and houses for sale, whose scripts react in each game (houses go through Property.cpp).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

// CPickups::aPickUps[620], 0x20 bytes: +0 revenue, +4 object, +8 ammo/money, +0xC regeneration time, +0x10 int16 x/y/z
// (eighths of a metre), +0x16 money per day, +0x18 model, +0x1A reference index, +0x1C type, +0x1D flags (1 disabled)
constexpr uintptr_t kPickups = 0x9788C0;
constexpr int kNumPickups = 620;
constexpr uintptr_t kTags = 0xA9A8C0;     // CTagManager::ms_tagDesc: {CEntity*, uint8 alpha}, 8 bytes
constexpr uintptr_t kNumTags = 0xA9AD70;  // CTagManager::ms_numTags
enum : uint8_t {
    PU_ON_STREET = 2, PU_ONCE = 3, PU_ONCE_TIMEOUT = 4, PU_ONCE_TIMEOUT_SLOW = 5, PU_MONEY = 8, PU_ON_STREET_SLOW = 15,
    PU_ASSET_REVENUE = 16, PU_MONEY_DOESNTDISAPPEAR = 19, PU_SNAPSHOT = 20, PU_ONCE_FOR_MISSION = 22,
};
enum : uint8_t { EV_TAKEN = 1, EV_TAKINGS = 2, EV_PHOTOGRAPHED = 3, EV_TAG = 4, EV_STATE = 5, EV_TAG_AREA = 6, EV_GONE = 7 };
constexpr int kModelInfo = 1239, kModelSave = 1277;  // info icon, save disk

uintptr_t Pickup(int i) { return kPickups + (uintptr_t)i * 0x20; }
uint8_t& Type(uintptr_t p) { return Ref<uint8_t>(p + 0x1C); }
int16_t Model(uintptr_t p) { return Ref<int16_t>(p + 0x18); }
int16_t* Pos(uintptr_t p) { return &Ref<int16_t>(p + 0x10); }
int16_t ModelIndexVar(uintptr_t a) { return Ref<int16_t>(a); }  // CModelInfo MI_* variables

bool g_applying = false;               // this module creates or takes a pickup itself
std::vector<int> g_announced;          // handles of pickups announced from here (for a restarted partner)

bool Shared(uint8_t type, int model) {
    switch (type) {
    case PU_ON_STREET: case PU_ONCE: case PU_ONCE_TIMEOUT: case PU_ONCE_TIMEOUT_SLOW: case PU_MONEY:
    case PU_ON_STREET_SLOW: case PU_ASSET_REVENUE: case PU_MONEY_DOESNTDISAPPEAR: case PU_ONCE_FOR_MISSION:
        return model != kModelInfo && model != kModelSave;
    }
    return false;
}

// the local pickup matching one of the other game (same model, within 1.5 m), or -1
int Find(int model, const int16_t* pos, uint8_t type) {
    int best = -1, bestDist = 13;
    for (int i = 0; i < kNumPickups; ++i) {
        uintptr_t p = Pickup(i);
        if (!Type(p) || Model(p) != model || (Type(p) == PU_SNAPSHOT) != (type == PU_SNAPSHOT)) continue;
        int16_t* q = Pos(p);
        int d = std::max({std::abs(q[0] - pos[0]), std::abs(q[1] - pos[1]), std::abs(q[2] - pos[2])});
        if (d < bestDist) best = i, bestDist = d;
    }
    return best;
}

void SendEvent(uint8_t what, uintptr_t p, uint8_t type) {
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(what);
    w.U16((uint16_t)Model(p));
    for (int k = 0; k < 3; ++k) w.I16(Pos(p)[k]);
    w.U8(type);
    Net().Send(proto::CH_RELIABLE, w);
}

// ---------------------------------------------------------------- this game's player takes a pickup
using PickupUpdateFn = bool(__fastcall*)(void*, void*, CPlayerPed*, CVehicle*, int);
PickupUpdateFn g_origPickupUpdate = nullptr;
bool __fastcall HookPickupUpdate(void* pickup, void* edx, CPlayerPed* player, CVehicle* veh, int playerId) {
    auto p = (uintptr_t)pickup;
    uint8_t type = Type(p);
    float takings = Ref<float>(p);
    uint8_t before[0x20];
    memcpy(before, pickup, sizeof(before));  // Remove() clears the type and the object
    bool taken = g_origPickupUpdate(pickup, edx, player, veh, playerId);
    if (!flow::InSharedWorld() || g_applying || player != FindPlayerPed(0) || !Shared(type, Model((uintptr_t)before)))
        return taken;
    if (taken) {
        SendEvent(EV_TAKEN, (uintptr_t)before, type);
    } else if (type == PU_ASSET_REVENUE && takings >= 10.0f && Ref<float>(p) == 0.0f) {
        SendEvent(EV_TAKINGS, p, type);
    }
    return taken;
}

// a snapshot photographed here (CPickups::PictureTaken removes it without recording)
using VoidFn = void(__cdecl*)();
VoidFn g_origPictureTaken = nullptr;
void __cdecl HookPictureTaken() {
    std::vector<std::pair<int, std::vector<uint8_t>>> snapshots;
    for (int i = 0; i < kNumPickups; ++i)
        if (Type(Pickup(i)) == PU_SNAPSHOT)
            snapshots.emplace_back(i, std::vector<uint8_t>((uint8_t*)Pickup(i), (uint8_t*)Pickup(i) + 0x20));
    g_origPictureTaken();
    if (!flow::InSharedWorld()) return;
    for (auto& [i, bytes] : snapshots)
        if (Type(Pickup(i)) != PU_SNAPSHOT) SendEvent(EV_PHOTOGRAPHED, (uintptr_t)bytes.data(), PU_SNAPSHOT);
}

// a tag painted here (CWorld::SprayPaintWorld's call of CTagManager::SetAlpha)
using SetAlphaFn = void(__cdecl*)(CEntity*, uint8_t);
// the game's own until redirected: a site another mod changed is left alone, and this is still called
SetAlphaFn g_origSetAlpha = (SetAlphaFn)0x49CEC0;
int TagIndex(CEntity* e) {
    int n = std::min(Ref<int32_t>(kNumTags), 150);
    for (int i = 0; i < n; ++i)
        if (Ref<CEntity*>(kTags + (uintptr_t)i * 8) == e) return i;
    return -1;
}
void __cdecl HookSprayTag(CEntity* tag, uint8_t alpha) {
    int i = TagIndex(tag);
    uint8_t was = i >= 0 ? Ref<uint8_t>(kTags + (uintptr_t)i * 8 + 4) : 0;
    g_origSetAlpha(tag, alpha);
    if (!flow::InSharedWorld() || g_applying || !tag || was > 228 || alpha <= 228) return;  // counts as sprayed above 228
    CVector pos = tag->GetPosition();
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(EV_TAG);
    w.Vec({pos.x, pos.y, pos.z});
    Net().Send(proto::CH_RELIABLE, w);
}

// ---------------------------------------------------------------- pickups that appear in one game only
using GenerateFn = int(__cdecl*)(float, float, float, uint32_t, int, uint32_t, uint32_t, int, char*);
GenerateFn g_origGenerate = nullptr;
bool Announced(int type, int model) {
    if (g_applying || !flow::InSharedWorld() || !core::IsPlayerReady() || PlayerInFocus() != 0) return false;  // the remote
    if (!Shared((uint8_t)type, model)) return false;                                  // player's drops are its game's
    if (void* script = CurrentScript())  // a script: only the host's mission is not run by both games, and Big Smoke's courier
        return Net().IsHost() && ((Field<uint8_t>(script, 0xDC) && HostReplicating()) || CourierThread(script));  // m_bIsMission
    return true;  // the engine: loot, a dropped jetpack, gang war rewards
}
void SendCreate(uintptr_t p) {
    Writer w(proto::G_PICKUP_CREATE);
    w.U16((uint16_t)Model(p));
    for (int k = 0; k < 3; ++k) w.I16(Pos(p)[k]);
    w.U8(Type(p));
    w.U32(Ref<uint32_t>(p + 0x08));  // ammo, money
    w.U16(Ref<uint16_t>(p + 0x16));  // money per day
    w.U8(Ref<uint8_t>(p + 0x1D) & 2);  // empty
    Net().Send(proto::CH_RELIABLE, w);
}
int __cdecl HookGenerateNewOne(float x, float y, float z, uint32_t model, int type, uint32_t ammo, uint32_t moneyPerDay,
                               int isEmpty, char* message) {
    int handle = g_origGenerate(x, y, z, model, type, ammo, moneyPerDay, isEmpty, message);
    if (handle >= 0 && Announced(type & 0xFF, (int)model)) {
        SendCreate(Pickup(handle & 0xFFFF));
        g_announced.push_back(handle);
    }
    return handle;
}

// The host's courier script takes its package's pickup away (laid on the ground anew, or the courier's end): the
// partners' copy goes as well. A mission's pickup stays there as before.
using RemovePickUpFn = void(__cdecl*)(int);
RemovePickUpFn g_origRemovePickUp = nullptr;
void __cdecl HookRemovePickUp(int handle) {
    bool tell = handle != -1 && (handle & 0xFFFF) < kNumPickups && Net().Online() && Net().IsHost() &&
                CourierThread(CurrentScript()) && std::find(g_announced.begin(), g_announced.end(), handle) != g_announced.end();
    int16_t pos[3] = {};
    uint16_t model = 0;
    if (tell) {
        uintptr_t p = Pickup(handle & 0xFFFF);
        for (int k = 0; k < 3; ++k) pos[k] = Pos(p)[k];
        model = (uint16_t)Model(p);
    }
    g_origRemovePickUp(handle);
    if (!tell) return;
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(EV_GONE);
    w.U16(model);
    for (int16_t c : pos) w.I16(c);
    Net().Send(proto::CH_RELIABLE, w);
}

// ---------------------------------------------------------------- the other game's player took one
void Take(int i, uint8_t type) {
    uintptr_t p = Pickup(i);
    if (type == PU_ON_STREET || type == PU_ON_STREET_SLOW) {
        // as if taken here: back after the same time (CPickup::Update)
        if (Ref<uint8_t>(p + 0x1D) & 1) return;
        uint32_t wait = type == PU_ON_STREET ? 30000 : Model(p) == ModelIndexVar(0x8CD5D0) ? 300000 : 360000;  // bribe
        Ref<uint32_t>(p + 0x0C) = TimeMs() + wait;
        CallMethod<0x454CF0>((void*)p);  // CPickup::GetRidOfObjects
        Ref<uint8_t>(p + 0x1D) |= 1;
        return;
    }
    // The other player's find counts here too, as CPickups::PickedUpHorseShoe / PickedUpOyster count it (its luck share
    // of all of them for a horseshoe). The $100, the $100 000 for the last one and the message are the finder's: this
    // game paid them as well.
    int model = Model(p);
    if (model == ModelIndexVar(0x8CD754)) {
        Call<0x55C180>((uint16_t)241, 1.0f);  // CStats::IncrementStat: horseshoes collected
        Call<0x55C180>((uint16_t)233, 1000.0f / std::max(Call<0x558E40, float>((uint16_t)242), 1.0f));  // luck / all of them
    } else if (model == ModelIndexVar(0x8CD750)) {
        Call<0x55C180>((uint16_t)243, 1.0f);  // oysters collected
    }
    CallMethod<0x4556C0>((void*)p);  // CPickup::Remove
    Call<0x455240>(i);                // CPickups::AddToCollectedPickupsArray: HAS_PICKUP_BEEN_COLLECTED
}

// a horseshoe, an oyster or a snapshot: the collectibles of the world
bool Collectible(uintptr_t p) {
    int model = Model(p);
    return Type(p) == PU_SNAPSHOT || model == ModelIndexVar(0x8CD754) || model == ModelIndexVar(0x8CD750);
}

// host: the world as it is now, for a partner whose game (re)started: the painted tags and the collectibles still lying
// there. Its world save can be older (no save during a mission or a ride), and only the live events had said the rest:
// after a rejoin the partner's tags were unpainted and the oysters and snapshots back.
void SendState() {
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(EV_STATE);
    std::vector<CVector> painted;
    for (int i = 0, n = std::min(Ref<int32_t>(kNumTags), 150); i < n; ++i)
        if (auto* tag = Ref<CEntity*>(kTags + (uintptr_t)i * 8); tag && Ref<uint8_t>(kTags + (uintptr_t)i * 8 + 4) > 228)
            painted.push_back(tag->GetPosition());
    w.U8((uint8_t)painted.size());
    for (const CVector& v : painted) w.Vec({v.x, v.y, v.z});
    std::vector<int> present;
    for (int i = 0; i < kNumPickups; ++i)
        if (Type(Pickup(i)) && Collectible(Pickup(i))) present.push_back(i);
    w.U16((uint16_t)present.size());
    for (int i : present) {
        w.U16((uint16_t)Model(Pickup(i)));
        for (int k = 0; k < 3; ++k) w.I16(Pos(Pickup(i))[k]);
    }
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("World state for a restarted partner: %zu tags painted, %zu collectibles left", painted.size(), present.size());
}

void ApplyState(Reader& r) {
    std::vector<CVector> painted(r.U8());
    for (CVector& v : painted) {
        Vec3 p = r.Vec();
        v = {p.x, p.y, p.z};
    }
    struct Left {
        int model;
        int16_t pos[3];
    };
    std::vector<Left> left(std::min<int>(r.U16(), kNumPickups));
    for (Left& l : left) {
        l.model = r.U16();
        for (auto& c : l.pos) c = r.I16();
    }
    if (!r.Ok()) return;
    int tags = 0, gone = 0;
    for (int i = 0, n = std::min(Ref<int32_t>(kNumTags), 150); i < n; ++i) {
        auto* tag = Ref<CEntity*>(kTags + (uintptr_t)i * 8);
        uint8_t& alpha = Ref<uint8_t>(kTags + (uintptr_t)i * 8 + 4);
        if (!tag || alpha > 228) continue;
        for (const CVector& v : painted)
            if ((tag->GetPosition() - v).Length() < 1.0f) {
                if (void* atomic = tag->RwObject()) Call<0x49CD30>(atomic, (uint8_t)255);  // CTagManager::SetAlpha(RpAtomic*): quietly
                alpha = 255;
                ++tags;
                break;
            }
    }
    if (tags) Call<0x49CDE0>();  // CTagManager::UpdateNumTagged
    for (int i = 0; i < kNumPickups; ++i) {
        uintptr_t p = Pickup(i);
        if (!Type(p) || !Collectible(p)) continue;
        bool there = false;
        for (const Left& l : left) {
            const int16_t* q = Pos(p);
            there = there || (l.model == Model(p) && std::max({std::abs(q[0] - l.pos[0]), std::abs(q[1] - l.pos[1]), std::abs(q[2] - l.pos[2])}) < 13);
        }
        if (there) continue;
        CallMethod<0x4556C0>((void*)p);  // CPickup::Remove: found in the host's world (its finder's stats counted it)
        ++gone;
    }
    LOGI("The host's world as it is now: %d tags painted, %d collectibles gone here", tags, gone);
}

// The host's mission wipes or paints the tags of an area (SET_TAG_STATUS_IN_AREA: "Tagging Up Turf" cleans its own): the
// partner's stayed painted, it could not spray them, and the mission waited for tags it never got.
using AlphaInAreaFn = void(__cdecl*)(const float*, uint8_t);
AlphaInAreaFn g_origAlphaInArea = nullptr;
void __cdecl HookSetAlphaInArea(const float* rect, uint8_t alpha) {
    g_origAlphaInArea(rect, alpha);
    if (!Net().Online() || !Net().IsHost() || !HostReplicating() || !StoryMission(CurrentScript())) return;
    Writer w(proto::G_PICKUP_EVENT);
    w.U8(EV_TAG_AREA);
    for (int k = 0; k < 4; ++k) w.F32(rect[k]);
    w.U8(alpha);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("The mission sets the tags in %.0f,%.0f .. %.0f,%.0f to %u: sent", rect[0], rect[1], rect[2], rect[3], alpha);
}

void HandleEvent(uint8_t from, Reader& r) {
    uint8_t what = r.U8();
    if (what == EV_STATE || what == EV_TAG_AREA) {
        if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
        if (what == EV_STATE) return ApplyState(r);
        float rect[4];
        for (float& f : rect) f = r.F32();
        uint8_t alpha = r.U8();
        if (!r.Ok() || !g_origAlphaInArea) return;
        g_origAlphaInArea(rect, alpha);
        LOGI("The host's mission set the tags in %.0f,%.0f .. %.0f,%.0f to %u: here too", rect[0], rect[1], rect[2], rect[3], alpha);
        return;
    }
    if (what == EV_GONE) {
        uint16_t model = r.U16();
        int16_t pos[3];
        for (int16_t& c : pos) c = r.I16();
        if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
        for (int i = 0; i < kNumPickups; ++i) {
            uintptr_t p = Pickup(i);
            const int16_t* q = Pos(p);
            if (!Type(p) || Model(p) != model || std::max({std::abs(q[0] - pos[0]), std::abs(q[1] - pos[1]), std::abs(q[2] - pos[2])}) >= 13)
                continue;
            g_applying = true;
            CallMethod<0x4556C0>((void*)p);  // CPickup::Remove
            g_applying = false;
            LOGD("The host's courier took its pickup %d away: here too", model);
        }
        return;
    }
    if (what == EV_TAG) {
        Vec3 v = r.Vec();
        if (!r.Ok() || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
        int n = std::min(Ref<int32_t>(kNumTags), 150);
        for (int i = 0; i < n; ++i) {
            auto* tag = Ref<CEntity*>(kTags + (uintptr_t)i * 8);
            if (!tag || (tag->GetPosition() - CVector{v.x, v.y, v.z}).Length() > 1.0f) continue;
            if (Ref<uint8_t>(kTags + (uintptr_t)i * 8 + 4) <= 228) {
                g_applying = true;
                g_origSetAlpha(tag, 255);  // shows "tags x of 100" as here
                g_applying = false;
                LOGI("Tag %d painted by player %d", i, from);
            }
            return;
        }
        return;
    }
    int model = r.U16();
    int16_t pos[3];
    for (auto& c : pos) c = r.I16();
    uint8_t type = r.U8();
    if (!r.Ok() || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
    int i = Find(model, pos, type);
    if (i < 0) return;
    uintptr_t p = Pickup(i);
    g_applying = true;
    if (what == EV_TAKINGS) {
        Ref<float>(p) = 0.0f;
    } else if (what == EV_PHOTOGRAPHED) {
        CallMethod<0x4556C0>((void*)p);  // CPickup::Remove
        Call<0x55C180>((uint16_t)231, 1.0f);  // CStats::IncrementStat: snapshots taken
    } else {
        Take(i, Type(p));
    }
    g_applying = false;
    LOGI("Pickup %d (model %d) taken by player %d", i, model, from);
}

void HandleCreate(uint8_t from, Reader& r) {
    int model = r.U16();
    int16_t pos[3];
    for (auto& c : pos) c = r.I16();
    uint8_t type = r.U8();
    uint32_t ammo = r.U32();
    uint16_t perDay = r.U16();
    bool empty = r.U8() != 0;
    // a model of a mod this game lacks crashes the streamer, then CPickup::GiveUsAPickUpObject (0x456809)
    if (!r.Ok() || !core::IsPlayerReady() || !flow::InSharedWorld() || !Shared(type, model) || !IsObjectModel(model) ||
        Find(model, pos, type) >= 0)
        return;
    if (!IsModelLoaded(model)) RequestModel(model);
    g_applying = true;
    g_origGenerate(pos[0] / 8.0f, pos[1] / 8.0f, pos[2] / 8.0f, (uint32_t)model, type, ammo, perDay, empty, nullptr);
    g_applying = false;
    LOGD("Pickup (model %d, type %d) from player %d", model, type, from);
}

class Pickups : public Module {
public:
    const char* Name() const override { return "Pickups"; }

    void OnInstallHooks() override {
        mem::HookFn(0x457410, &HookPickupUpdate, &g_origPickupUpdate, "CPickup::Update");
        mem::HookFn(0x456A70, &HookPictureTaken, &g_origPictureTaken, "CPickups::PictureTaken");
        mem::HookFn(0x456F20, &HookGenerateNewOne, &g_origGenerate, "CPickups::GenerateNewOne");
        mem::HookFn(0x4573D0, &HookRemovePickUp, &g_origRemovePickUp, "CPickups::RemovePickUp");
        mem::RedirectCallFn(0x565C5E, &HookSprayTag, &g_origSetAlpha);  // CWorld::SprayPaintWorld -> CTagManager::SetAlpha
        mem::RedirectCallFn(0x498D54, &HookSetAlphaInArea, &g_origAlphaInArea);  // SET_TAG_STATUS_IN_AREA
        mem::RedirectCallFn(0x498D6A, &HookSetAlphaInArea, &g_origAlphaInArea);
        Net().SetHandler(proto::G_PICKUP_EVENT, &HandleEvent);
        Net().SetHandler(proto::G_PICKUP_CREATE, &HandleCreate);
    }
    void OnSessionEnd() override { g_announced.clear(); }
    void OnGameStarted() override { g_announced.clear(); }
    void OnPeerResync(uint8_t) override {
        if (Net().IsHost() && flow::InSharedWorld()) SendState();
        // a restarted game lost what was announced to it: what is still here goes again
        std::vector<int> alive;
        for (int h : g_announced) {
            int i = h & 0xFFFF;
            if (i >= kNumPickups || Ref<uint16_t>(Pickup(i) + 0x1A) != (uint16_t)(h >> 16) || !Type(Pickup(i))) continue;
            SendCreate(Pickup(i));
            alive.push_back(h);
        }
        g_announced = std::move(alive);
    }

    static Pickups& Instance() {
        static Pickups m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Pickups::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
