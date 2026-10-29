// Map waypoints. Each game sends its player's waypoint (the target set on the pause menu map) and draws the other
// players' ones on its radar and map with the same sprite. They are drawn rather than added as script blips, so
// they never get into a save.
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/waypoint/Gps.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kTargetBlip = addr::MenuManager + 0x2C;  // FrontEndMenuManager.m_nTargetBlipIndex

std::map<uint8_t, CVector2D> g_remote;  // the other players' waypoints
bool g_sentOn = false;
CVector2D g_sent;
uint64_t g_pollMs = 0;

// the blip the map menu placed, while it is still in use
bool LocalWaypoint(CVector2D& out) {
    int h = Ref<int32_t>(kTargetBlip);
    if (!h) return false;
    uintptr_t t = 0xBA86F0 + (h & 0xFFFF) * 0x28;  // CRadar::ms_RadarTrace
    if (!(Ref<uint8_t>(t + 0x25) & 2) || Ref<uint16_t>(t + 0x14) != (uint16_t)(h >> 16)) return false;  // in use, counter
    const CVector& p = Ref<CVector>(t + 0x08);
    out = {p.x, p.y};
    return true;
}

// both the radar (a tail jump from CRadar::DrawRadar) and the menu map draw their blips here, and the other players'
// arrows (Players) after them: one hook of the function for both
using DrawBlipsFn = void(__cdecl*)();
DrawBlipsFn g_origDrawBlips = nullptr;
void __cdecl HookDrawBlips() {
    gps::DrawRoute(g_remote);  // the route first: under the blips
    g_origDrawBlips();
    Players::Instance().DrawRadarArrows();
    for (auto& [id, pos] : g_remote) {
        CVector2D radar, screen;
        Call<0x583530>(&radar, &pos);     // CRadar::TransformRealWorldPointToRadarSpace
        Call<0x5832F0, float>(&radar);    // CRadar::LimitRadarPoint: onto the radar's edge
        Call<0x583480>(&screen, &radar);  // CRadar::TransformRadarPointToScreenSpace
        Call<0x585FF0>((uint16_t)41, screen.x, screen.y, (uint8_t)255);  // CRadar::DrawRadarSprite, RADAR_SPRITE_WAYPOINT
    }
}

class Waypoints : public Module {
public:
    const char* Name() const override { return "Waypoints"; }

    void OnInstallHooks() override {
        Players::SetRadarArrowsHooked(mem::HookFn(0x588050, &HookDrawBlips, &g_origDrawBlips, "CRadar::DrawBlips"));
        Net().SetHandler(proto::G_WAYPOINT, [](uint8_t from, Reader& r) {
            bool on = r.U8() != 0;
            CVector2D p{r.F32(), r.F32()};
            if (!r.Ok()) return;
            LOGI("Waypoint of player %d: %s %.0f, %.0f", from, on ? "set" : "cleared", p.x, p.y);
            if (on) g_remote[from] = p;
            else g_remote.erase(from);
        });
    }
    void OnSessionStart() override {
        g_remote.clear();
        g_sentOn = false;
    }
    void OnSessionEnd() override { g_remote.clear(); }
    void OnPeerJoin(uint8_t) override { g_sentOn = false; }  // a newcomer gets the current one
    void OnPeerLeave(uint8_t id) override { g_remote.erase(id); }

    // a save keeps its waypoint, so the host's world gave the partner the host's waypoint as his own
    void OnGameStarted() override {
        int& h = Ref<int32_t>(kTargetBlip);
        if (!Net().Online() || Net().IsHost() || !h) return;
        Call<0x587CE0>(h);  // CRadar::ClearBlip
        h = 0;
    }

    void OnPostProcess() override {
        if (!Net().Online() || NowMs() - g_pollMs < 500) return;
        g_pollMs = NowMs();
        CVector2D p;
        bool on = core::IsPlayerReady() && LocalWaypoint(p);
        if (on == g_sentOn && (!on || std::fabs(p.x - g_sent.x) + std::fabs(p.y - g_sent.y) < 1.0f)) return;
        g_sentOn = on;
        g_sent = p;
        Writer w(proto::G_WAYPOINT);
        w.U8(on);
        w.F32(p.x);
        w.F32(p.y);
        Net().Send(proto::CH_RELIABLE, w);
    }

    static Waypoints& Instance() {
        static Waypoints m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Waypoints::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
