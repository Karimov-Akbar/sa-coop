// Entrances (shops, barbers, houses) open and close as in the host's game. Its missions open some for their time
// (the prologue closes the barbers, "Ryder" opens one for the haircut): the partner's game does not run those
// missions and kept them shut. Each entrance keeps its state in its flags (they are what a save stores), and the host
// sends them when they change, except the bits about where this game's player went in. An entrance is found by its
// name and place: with other mods (more interiors) or entrances a script made later its slot here can differ.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/mission/Mission.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kEnexPool = 0x96A7D8;  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
// ENTERED_WITHOUT_EXIT, USED_REWARD_ENTRANCE: this player's history. DELETE_ENEX too: this game's streaming sets it as the
// door's building goes while its player is inside (CEntity::RemoveEffects), and the door is deleted as that player comes
// out. The host's, here, deleted the partner's door of the burglary house the host was in: its marker went (28.10).
constexpr uint16_t kOwnBits = 0x8000 | 0x2000 | 0x10;

CPoolHeader* Pool() { return Ref<CPoolHeader*>(kEnexPool); }
uintptr_t Entry(CPoolHeader* pool, int i) { return (uintptr_t)pool->objects + (uintptr_t)i * 0x3C; }  // +0 m_szName[8]
uint16_t* Flags(CPoolHeader* pool, int i) { return &Ref<uint16_t>(Entry(pool, i) + 0x30); }
void Center(uintptr_t e, float& x, float& y) {  // of CEntryExit::m_recEntrance
    const float* r = &Ref<float>(e + 0x08);
    x = (r[0] + r[2]) * 0.5f;
    y = (r[1] + r[3]) * 0.5f;
}

std::vector<int32_t> g_sent;  // host: what the partner has, -1 for a free slot
uint64_t g_scanMs = 0;

// The host's mission can shut an interior behind its own CJ ("Cleaning the Hood" switches LACRAK off once CJ and Ryder
// are out of the crack den): a partner still inside was locked in, and a reload of the host's world put it back there
// (the save keeps the den shut). The ways out of the interior our player stands in stay open here.
void KeepWayOut(CPoolHeader* pool) {
    int8_t area = FindPlayerPed(0)->AreaCode();
    if (area == 0) return;
    for (int i = 0; i < pool->size; ++i)
        if (pool->byteMap[i] >= 0 && Ref<int8_t>(Entry(pool, i) + 0x32) == area) *Flags(pool, i) |= 0x4000;  // m_nArea, ENABLE_ACCESS
}

class Entrances : public Module {
public:
    const char* Name() const override { return "Entrances"; }

    void OnInstallHooks() override {
        Net().SetHandler(proto::G_ENEX, [](uint8_t from, Reader& r) {
            int n = r.U16();
            CPoolHeader* pool = Pool();
            if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !flow::InHostWorld() || !pool) return;
            for (int k = 0; k < n; ++k) {
                char name[8];
                r.Raw(name, 8);
                float x = r.F32(), y = r.F32();
                uint16_t flags = r.U16();
                if (!r.Ok()) return;
                for (int i = 0; i < pool->size; ++i) {
                    float ex, ey;
                    if (pool->byteMap[i] < 0 || _strnicmp(name, (const char*)Entry(pool, i), 8)) continue;
                    Center(Entry(pool, i), ex, ey);
                    if (std::fabs(ex - x) > 2.0f || std::fabs(ey - y) > 2.0f) continue;
                    uint16_t* f = Flags(pool, i);
                    // a gym's door, which the host's GYM thread shuts on any mission of its outdoors (a taxi fare): the
                    // partner's own stays open but in the host's story mission (#34)
                    if (!_strnicmp(name, "GYM", 3) && !(flags & 0x4000) && !HostMissionRecent(0)) flags |= *f & 0x4000;
                    if (n < 10) LOGD("Entrance %d (%.8s): flags %04X -> %04X", i, name, *f, flags);
                    *f = (uint16_t)((*f & kOwnBits) | (flags & ~kOwnBits));
                    break;
                }
            }
            LOGD("Entrances: %d from the host", n);
        });
    }
    void OnSessionEnd() override { g_sent.clear(); }
    void OnGameStarted() override { g_sent.clear(); }
    void OnPeerResync(uint8_t) override { g_sent.clear(); }  // the next scan sends them all

    void OnPostProcess() override {
        CPoolHeader* pool = Pool();
        if (!Net().Online() || !core::IsPlayerReady() || !pool) return;
        if (!Net().IsHost()) return KeepWayOut(pool);
        if (NowMs() - g_scanMs < 500) return;
        g_scanMs = NowMs();
        bool all = g_sent.size() != (size_t)pool->size;
        g_sent.resize(pool->size, -1);
        std::vector<std::pair<uint16_t, uint16_t>> changed;
        for (int i = 0; i < pool->size; ++i) {
            int32_t now = pool->byteMap[i] < 0 ? -1 : (*Flags(pool, i) & ~kOwnBits);
            if (!all && now == g_sent[i]) continue;
            g_sent[i] = now;
            if (now >= 0) changed.emplace_back((uint16_t)i, (uint16_t)now);
        }
        if (changed.empty() || Net().Peers().empty()) return;
        for (size_t at = 0; at < changed.size(); at += 200) {
            size_t n = std::min<size_t>(200, changed.size() - at);
            Writer w(proto::G_ENEX);
            w.U16((uint16_t)n);
            for (size_t k = at; k < at + n; ++k) {
                uintptr_t e = Entry(pool, changed[k].first);
                float x, y;
                Center(e, x, y);
                w.Raw((const void*)e, 8);
                w.F32(x);
                w.F32(y);
                w.U16(changed[k].second);
            }
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (!all) LOGD("Entrances: %zu changed", changed.size());  // mostly entrances the map streams in and out
    }

    static Entrances& Instance() {
        static Entrances m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Entrances::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
