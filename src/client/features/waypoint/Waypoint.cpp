// Map waypoints. Each game sends its player's waypoint (the target set on the pause menu map) and draws the other
// players' ones on its radar and map with the same sprite. They are drawn rather than added as script blips, so
// they never get into a save. A player who chose a colour for its mark has its waypoint drawn in it, the host too: its
// nickname, its mark and its waypoint in one colour, whose is whose (the players' wish and the user's word, 10.11). With
// none chosen the host's is the game's red, a partner's red with two players and its arrow's blue with more. Our own, which
// the game draws, is in our colour too when we chose one. The GPS route goes to ours, else to the one set last, in the
// colour of who set it.
#include <algorithm>
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "features/waypoint/Waypoint.h"

#include "entities/player/Players.h"
#include "features/waypoint/Gps.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kTargetBlip = addr::MenuManager + 0x2C;  // FrontEndMenuManager.m_nTargetBlipIndex

struct Remote {
    CVector2D pos;
    uint64_t setMs = 0;  // put there then: the route goes to the latest one (it went to the lowest id's, 10.11 audit)
};
std::map<uint8_t, Remote> g_remote;  // the other players' waypoints
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
constexpr int kWaypointSprite = 41;  // RADAR_SPRITE_WAYPOINT
constexpr uintptr_t kBlipSprites = 0xBAA250;  // CRadar::RadarBlipSprites: CSprite2d, its RwTexture* alone

// The waypoint's sprite is a red cross on a black disc, and a colour the game draws it in only darkens the red: a copy in a
// player's colour (its red turned into that colour, the black kept), made once for each colour from the game's texture
// (RwRaster: 0x0C width, 0x10 height, 0x14 depth, 0x18 stride once locked). Null when it could not be made: red then.
std::map<uint32_t, void*> g_tinted;  // RwTexture* by colour
void* TintedWaypoint(const CRGBA& c) {
    uint32_t key = (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
    if (auto it = g_tinted.find(key); it != g_tinted.end()) return it->second;
    if (g_tinted.size() >= 8) {  // the colours were changed over and over
        for (auto& [k, t] : g_tinted)
            if (t) Call<0x7F3820>(t);  // RwTextureDestroy (its raster with it)
        g_tinted.clear();
    }
    void*& made = g_tinted[key];  // stays null for a colour whose copy failed
    auto* tex = Ref<uint8_t*>(kBlipSprites + kWaypointSprite * 4);
    auto* src = tex ? Field<uint8_t*>(tex, 0x00) : nullptr;  // RwTexture::raster
    // a mod's sprite kept compressed (DXT) is no row of pixels, whatever depth its file names: read as one it gave noise and
    // ran past its end (RwD3D9RasterIsCompressed, 0x820C90)
    if (!src || Field<int32_t>(src, 0x14) != 32 || Call<0x820C90, int>(src)) return nullptr;
    int w = Field<int32_t>(src, 0x0C), h = Field<int32_t>(src, 0x10);
    void* dst = Call<0x7FB230, void*>(w, h, 32, 0x0504);  // RwRasterCreate: rwRASTERTYPETEXTURE | rwRASTERFORMAT8888
    auto* in = dst ? Call<0x7FB2D0, uint8_t*>(src, (uint8_t)0, 2) : nullptr;  // RwRasterLock(rwRASTERLOCKREAD)
    int inStride = Field<int32_t>(src, 0x18);
    auto* out = in ? Call<0x7FB2D0, uint8_t*>(dst, (uint8_t)0, 1) : nullptr;  // rwRASTERLOCKWRITE
    if (out) {
        int outStride = Field<int32_t>(dst, 0x18), top = 1;
        for (int y = 0; y < h; ++y)  // B G R A: its red is how bright it is
            for (int x = 0; x < w; ++x) top = std::max<int>(top, in[y * inStride + x * 4 + 2]);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = in + y * inStride + x * 4;
                uint8_t* q = out + y * outStride + x * 4;
                int light = std::min(255, p[2] * 255 / top);
                q[0] = (uint8_t)(light * c.b / 255), q[1] = (uint8_t)(light * c.g / 255), q[2] = (uint8_t)(light * c.r / 255);
                q[3] = p[3];
            }
    }
    if (out) Call<0x7FAEC0>(dst);  // RwRasterUnlock
    if (in) Call<0x7FAEC0>(src);
    if (!out) {
        if (dst) Call<0x7FB020>(dst);  // RwRasterDestroy
        LOGW("Waypoint: no copy of its sprite in a colour, the partners' ones stay red");
        return nullptr;
    }
    made = Call<0x7F37C0, void*>(dst);  // RwTextureCreate
    if (made)  // RwTexture::filterAddressing: the game's addressing, linear (the copy has no mipmaps)
        Field<uint32_t>(made, 0x50) = (Field<uint32_t>(tex, 0x50) & ~0xFFu) | 2u;
    return made;
}

// RADAR_SPRITE_WAYPOINT in a colour: the game draws it (CRadar::DrawRadarSprite), with our copy in its sprite for that call.
// Drawn by our own code it was larger than the game's: a widescreen fix gives the radar's sprites sizes of its own (10.11).
void DrawWaypoint(float x, float y, const CRGBA& colour) {
    void*& sprite = Ref<void*>(kBlipSprites + kWaypointSprite * 4);  // CSprite2d::m_pTexture
    void* own = sprite;
    if (void* tinted = TintedWaypoint(colour)) sprite = tinted;
    Call<0x585FF0>((uint16_t)kWaypointSprite, x, y, (uint8_t)255);
    sprite = own;
}

uint64_t g_testUntil = 0;  // TestColours

void DrawTestWaypoints() {
    CPlayerPed* me = NowMs() < g_testUntil && core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!me) return;
    for (int i = 0; i < 2; ++i) {
        CVector p = me->GetPosition();
        CVector2D at{p.x + (i ? 40.0f : 0.0f), p.y + (i ? 0.0f : 40.0f)}, radar, screen;
        Call<0x583530>(&radar, &at);      // CRadar::TransformRealWorldPointToRadarSpace
        Call<0x5832F0, float>(&radar);    // CRadar::LimitRadarPoint
        Call<0x583480>(&screen, &radar);  // CRadar::TransformRadarPointToScreenSpace
        if (i) DrawWaypoint(screen.x, screen.y, CRGBA{40, 200, 60, 255});
        else Call<0x585FF0>((uint16_t)kWaypointSprite, screen.x, screen.y, (uint8_t)255);
    }
}

using DrawBlipsFn = void(__cdecl*)();
DrawBlipsFn g_origDrawBlips = nullptr;
void __cdecl HookDrawBlips() {
    const Remote* latest = nullptr;
    uint8_t setter = 0xFF;
    for (auto& [id, w] : g_remote)
        if (!latest || w.setMs > latest->setMs) latest = &w, setter = id;
    // the route first, under the blips: in the colour of the player who set that waypoint, when it chose one
    const RemotePlayer* by = latest ? Players::Instance().Get(setter) : nullptr;
    CRGBA routeColour = by && !by->markColour.empty() ? MarkRgba(*by) : CRGBA{};
    gps::DrawRoute(latest ? &latest->pos : nullptr, by && !by->markColour.empty() ? &routeColour : nullptr);
    // our own waypoint, the game's, in our colour when we chose one (the user's word, 10.11): the copy in its sprite's slot
    // for the game's drawing
    void*& sprite = Ref<void*>(kBlipSprites + kWaypointSprite * 4);  // CSprite2d::m_pTexture
    void* stock = sprite;
    if (MarkColourChosen(Cfg().markColour))
        if (void* tinted = TintedWaypoint(MarkRgba(Cfg().markColour))) sprite = tinted;
    g_origDrawBlips();
    sprite = stock;
    Players::Instance().DrawRadarArrows();
    bool many = Net().Peers().size() > 1;  // more than one partner
    for (auto& [id, w] : g_remote) {
        CVector2D radar, screen;
        Call<0x583530>(&radar, &w.pos);   // CRadar::TransformRealWorldPointToRadarSpace
        Call<0x5832F0, float>(&radar);    // CRadar::LimitRadarPoint: onto the radar's edge
        Call<0x583480>(&screen, &radar);  // CRadar::TransformRadarPointToScreenSpace
        const RemotePlayer* rp = Players::Instance().Get(id);
        if (rp && (!rp->markColour.empty() || (many && id != Net().HostId()))) DrawWaypoint(screen.x, screen.y, MarkRgba(*rp));
        else Call<0x585FF0>((uint16_t)kWaypointSprite, screen.x, screen.y, (uint8_t)255);  // CRadar::DrawRadarSprite
    }
    DrawTestWaypoints();
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
            auto it = g_remote.find(from);
            if (!on) g_remote.erase(from);
            // the same one again (sent anew to a player who came in) keeps its time
            else if (it != g_remote.end() && std::fabs(it->second.pos.x - p.x) + std::fabs(it->second.pos.y - p.y) < 1.0f) it->second.pos = p;
            else g_remote[from] = {p, NowMs()};
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

void waypoint::TestColours(float seconds) { g_testUntil = NowMs() + (uint64_t)(seconds * 1000.0f); }

}  // namespace coop
