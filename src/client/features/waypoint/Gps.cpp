// The road route on the radar in a car, as GTA V and the SA GPS mods draw it: red to the player's own waypoint, or to the
// other player's when there is none, else yellow to the mission's destination. The game's car path finder
// (CPathFind::DoPathSearch) finds it about once a second; Waypoints' hook of CRadar::DrawBlips draws it first, under the
// blips.
#include "features/waypoint/Gps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kPaths = 0x96F050;    // ThePaths
constexpr uint32_t kNoNode = 0xFFFFFFFF;  // CNodeAddress: the area in the low word, the node in the high one
constexpr int kMaxNodes = 1000;

std::vector<CVector2D> g_route;  // the nodes from the player's end
uint32_t g_found[kMaxNodes];
CVector2D g_target, g_partner;
bool g_targetOn = false, g_partnerOn = false, g_requested = false;
float g_rect[4];           // our node request: min x, max x, min y, max y
float g_offAtSearch = 0;   // the player's distance from the route when it was found
float g_reach = -1;        // the share of the way the last route went (-1: none), for the log
uint64_t g_searchMs = 0, g_radarMs = 0;

// A limit adjuster that moves ThePaths (and its layout, perhaps) moves the game's references to it as well: then there is
// no route rather than reads of the wrong memory. LOAD_PATH_NODES_IN_AREA's "mov ecx, offset ThePaths" at 0x491483.
bool PathsInPlace() {
    static const bool ok = [] {
        bool same = Ref<uint8_t>(0x491483) == 0xB9 && Ref<uint32_t>(0x491484) == kPaths;
        if (!same) LOGW("GPS: ThePaths is not at 0096F050 (a limit adjuster moved it?), no route on the radar");
        return same;
    }();
    return ok;
}

uint8_t* AreaNodes(uint32_t area) { return Ref<uint8_t*>(kPaths + 0x804 + area * 4); }  // CPathFind::m_pPathNodes
// a node of a loaded area (CPathNode, 0x1C bytes), or null
const uint8_t* Node(uint32_t a) {
    uint32_t area = a & 0xFFFF, id = a >> 16;
    if (area >= 72 || !AreaNodes(area) || id >= Ref<uint32_t>(kPaths + 0xFA4 + area * 4)) return nullptr;  // m_anNumNodes
    return AreaNodes(area) + id * 0x1C;
}
CVector2D NodePos(const uint8_t* n) { return {Field<int16_t>(n, 0x08) / 8.0f, Field<int16_t>(n, 0x0A) / 8.0f}; }
float Dist(CVector2D a, CVector2D b) { return std::hypot(a.x - b.x, a.y - b.y); }

// The car node of the player's road network (flood fill) nearest to p on the map. A waypoint has no height (0), and the
// game's own pick weighs the height three times: on a hill it would take a road below.
uint32_t Nearest(CVector2D p, uint8_t fill) {
    uint32_t best = kNoNode;
    float bestD = 1e30f;
    for (uint32_t area = 0; area < 64; ++area) {
        const uint8_t* nodes = AreaNodes(area);
        uint32_t n = nodes ? Ref<uint32_t>(kPaths + 0x10C4 + area * 4) : 0;  // m_anNumVehicleNodes: they come first
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* node = nodes + i * 0x1C;
            if (node[0x17] != fill || (node[0x18] & 0x80)) continue;  // m_nFloodFill, m_bWaterNode
            CVector2D q = NodePos(node);
            float d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
            if (d < bestD) bestD = d, best = area | i << 16;
        }
    }
    return best;
}

// The route from the car to the target, else to a point halfway there, a quarter of the way...: the game's search gives
// up past 4950 nodes (aNodesToBeCleared), a long way through a city. A target in areas not loaded yet gives the route
// to the nearest loaded road, and the next search goes on from there.
void Search(const CVector& from, CVector2D to) {
    g_route.clear();
    uint32_t start = kNoNode, last = kNoNode;
    // CPathFind::FindNodeClosestToCoors(car nodes, any distance): the height helps here (a bridge over a road)
    CallMethod<0x44F460>((void*)kPaths, &start, from.x, from.y, from.z, 0, 999999.0f, 0, 0, 0, 0, 0);
    const uint8_t* s = Node(start);
    float reach = -1;
    for (float t = 1.0f; s && t > 0.1f && reach < 0; t *= 0.5f) {
        CVector2D p{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
        uint32_t end = Nearest(p, s[0x17]);
        if (end == kNoNode || end == start) break;
        if (end == last) continue;
        last = end;
        int16_t count = 0;
        float distance = 0.0f;
        // CPathFind::DoPathSearch(car nodes, origin and its node, target, out nodes, out count, room, out distance, max
        // distance, the target's node, max depth, keep to the lane, a node to avoid, water links, boats)
        CallMethod<0x4515D0>((void*)kPaths, 0, from.x, from.y, from.z, start, p.x, p.y, 0.0f, g_found, &count, kMaxNodes,
                             &distance, 999999.0f, &end, 999999.0f, 0, kNoNode, 0, 0);
        for (int i = 0; i < count && i < kMaxNodes; ++i) {
            const uint8_t* n = Node(g_found[i]);
            if (!n) break;
            g_route.push_back(NodePos(n));
        }
        if (g_route.size() >= 2) reach = t;
        else g_route.clear();
    }
    if (reach != g_reach) {
        if (reach < 0) LOGD("GPS: no route to %.0f, %.0f", to.x, to.y);
        else LOGD("GPS: %u nodes, %.0f%% of the way to %.0f, %.0f", (unsigned)g_route.size(), reach * 100.0f, to.x, to.y);
        g_reach = reach;
    }
}

// The path node areas between the player and the target (up to 1.5 km ahead), loaded the way LOAD_PATH_NODES_IN_AREA has
// them loaded: the game keeps only those within 350 m of the player. Scripts use the same single request, and a script's
// is never overwritten: it lasts until its RELEASE_PATH_NODES or the mission's cleanup.
bool OurRequest() {  // m_loadAreaRequestPending and its rectangle
    return g_requested && Ref<uint8_t>(kPaths + 0x3C6C) && !memcmp(&Ref<float>(kPaths + 0x3C70), g_rect, sizeof g_rect);
}
void Request(const CVector& from, CVector2D to) {
    if (Ref<uint8_t>(kPaths + 0x3C6C) && !OurRequest()) {
        g_requested = false;
        return;
    }
    constexpr float kAhead = 1500.0f, kMargin = 200.0f;  // the margin: the road the target lies off
    g_rect[0] = std::max(std::min(from.x, to.x), from.x - kAhead) - kMargin;
    g_rect[1] = std::min(std::max(from.x, to.x), from.x + kAhead) + kMargin;
    g_rect[2] = std::max(std::min(from.y, to.y), from.y - kAhead) - kMargin;
    g_rect[3] = std::min(std::max(from.y, to.y), from.y + kAhead) + kMargin;
    g_requested = true;
    CallMethod<0x450D70>((void*)kPaths, g_rect[0], g_rect[1], g_rect[2], g_rect[3]);  // MakeRequestForNodesToBeLoaded
}
void Drop() {
    if (OurRequest()) CallMethod<0x44DD00>((void*)kPaths);  // CPathFind::ReleaseRequestedNodes
    g_requested = g_targetOn = false;
    g_route.clear();
    g_reach = -1;
}

// The route from across the player: the search starts it at the node nearest to the car, as often behind it as not. The
// segment the player is along, and the point on it.
size_t Beside(CVector2D p, CVector2D& at) {
    for (size_t i = 0;; ++i) {
        const CVector2D &a = g_route[i], &b = g_route[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
        float t = l > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 1.0f;
        if (t < 1.0f || i + 2 >= g_route.size() || i >= 32) {
            t = std::clamp(t, 0.0f, 1.0f);
            at = {a.x + dx * t, a.y + dy * t};
            return i;
        }
    }
}
float OffRoute(CVector2D p) {
    CVector2D at;
    Beside(p, at);
    return Dist(p, at);
}

// The player's own map waypoint (Waypoints reads it the same way), else the other player's it shows: a red route. With
// neither, the mission's destination (a plain coordinate blip on the radar, the nearest): a yellow one (Thomas, 28.10:
// "для целей желтый а для метки красная").
bool g_mission = false;  // the target is a mission's destination
bool g_routeMission = false;  // the route found goes to one
bool Target(CVector2D& out, CVector2D from) {
    int h = Ref<int32_t>(addr::MenuManager + 0x2C);  // FrontEndMenuManager.m_nTargetBlipIndex
    uint32_t i = h & 0xFFFF;
    uintptr_t t = 0xBA86F0 + i * 0x28;  // CRadar::ms_RadarTrace[175]
    g_mission = false;
    if (h && i < 175 && (Ref<uint8_t>(t + 0x25) & 2) && Ref<uint16_t>(t + 0x14) == (uint16_t)(h >> 16)) {  // in use, counter
        out = {Ref<float>(t + 0x08), Ref<float>(t + 0x0C)};
        return true;
    }
    if (g_partnerOn) {
        out = g_partner;
        return true;
    }
    float best = 1e30f;
    for (int k = 0; k < 175; ++k) {
        uintptr_t b = 0xBA86F0 + (uintptr_t)k * 0x28;
        uint8_t bits = Ref<uint8_t>(b + 0x26);
        // tracked (in use) and not short-range, a coordinate blip (m_nBlipType 4) shown on the radar (m_nBlipDisplayFlag 2
        // or 3), no sprite (a contact point, a shop or the waypoint have one)
        if ((Ref<uint8_t>(b + 0x25) & 6) != 2 || ((bits >> 2) & 0xF) != 4 || !(bits & 2) || Ref<uint8_t>(b + 0x24)) continue;
        CVector2D p{Ref<float>(b + 0x08), Ref<float>(b + 0x0C)};
        if (float d = Dist(p, from); d < best) best = d, out = p, g_mission = true;
    }
    return g_mission;
}

CVector2D ToRadar(CVector2D world) {
    CVector2D r;
    Call<0x583530>(&r, &world);  // CRadar::TransformRealWorldPointToRadarSpace
    return r;
}

// a..b of radar space inside the radar's circle (the unit one: LimitRadarPoint's limit, the map's mask) as a band w pixels
// to each side. Its ends inside go on by w, so that the next band covers the notch at a bend.
void Band(CVector2D a, CVector2D b, float w, const CRGBA& c) {
    float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy, k = a.x * dx + a.y * dy;
    float d = k * k - l * (a.x * a.x + a.y * a.y - 1.0f);  // |a + t (b - a)| = 1
    if (l < 1e-12f || d <= 0) return;
    float t0 = (-k - std::sqrt(d)) / l, t1 = (-k + std::sqrt(d)) / l;
    if (t1 <= 0 || t0 >= 1) return;
    CVector2D from{a.x + dx * std::max(t0, 0.0f), a.y + dy * std::max(t0, 0.0f)};
    CVector2D to{a.x + dx * std::min(t1, 1.0f), a.y + dy * std::min(t1, 1.0f)};
    CVector2D p, q;
    Call<0x583480>(&p, &from);  // CRadar::TransformRadarPointToScreenSpace
    Call<0x583480>(&q, &to);
    float ex = q.x - p.x, ey = q.y - p.y, len = std::sqrt(ex * ex + ey * ey);
    if (len < 0.01f) return;
    ex *= w / len;
    ey *= w / len;
    if (t0 <= 0) p.x -= ex, p.y -= ey;
    if (t1 >= 1) q.x += ex, q.y += ey;
    // CSprite2d::Draw2DPolygon: a fan of its corners 3, 4, 2, 1
    Call<0x7285B0>(p.x + ey, p.y - ex, q.x + ey, q.y - ex, p.x - ey, p.y + ex, q.x - ey, q.y + ex, &c);
}

class Gps : public Module {
public:
    const char* Name() const override { return "Gps"; }

    void OnGameShutdown() override { Drop(); }

    void OnPostProcess() override {
        CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
        CVehicle* car = me ? me->VehicleIfIn() : nullptr;
        int type = car ? car->VehicleSubClass() : -1;
        CVector from = car ? car->GetPosition() : CVector{};
        CVector2D to, here{from.x, from.y};
        // in a car, a quad, a bike or a bicycle (no boat, plane, helicopter or train), at its wheel or beside the driver
        // (28.10: the passenger looked for the route too), outside, out of a cutscene (CCutsceneMgr::ms_running), with a
        // target
        bool on = Cfg().gps && car && ((type >= 0 && type <= VEHICLE_TYPE_QUAD) || car->IsBike()) && !me->AreaCode() &&
                  !Ref<int32_t>(0xB72914) && !Ref<uint8_t>(0xB5F851) && Target(to, here) && PathsInPlace();
        if (!on || Dist(here, to) < 30.0f) {
            Drop();
            return;
        }
        uint64_t now = NowMs();
        if (now - g_radarMs > 1000) return;  // the radar's map is not drawn (the HUD is off, a scene): nothing to show
        // at once for a new target or when the car has left the route, else once a second
        bool urgent = !g_targetOn || Dist(to, g_target) > 5.0f ||
                      (g_route.size() >= 2 && OffRoute(here) > g_offAtSearch + 40.0f);
        if (now - g_searchMs < (urgent ? 250u : 1000u)) return;
        g_searchMs = now;
        g_target = to;
        g_targetOn = true;
        g_routeMission = g_mission;
        Request(from, to);
        Search(from, to);
        g_offAtSearch = g_route.size() >= 2 ? OffRoute(here) : 0.0f;
    }

    static Gps& Instance() {
        static Gps m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Gps::Instance()); }
} g_reg;
}  // namespace

void gps::DrawRoute(const std::map<uint8_t, game::CVector2D>& remote) {
    using namespace game;
    g_partnerOn = !remote.empty();
    if (g_partnerOn) g_partner = remote.begin()->second;
    // where CRadar::DrawMap draws the radar's map: not the menu's (m_bDrawingMap), not the radar of blips only
    // (m_nRadarMode 1), not inside (CGame::currArea)
    if (Ref<uint8_t>(addr::MenuManager + 0x59) || Ref<int32_t>(addr::MenuManager + 0x24) == 1 || Ref<int32_t>(0xB72914))
        return;
    g_radarMs = NowMs();
    if (g_route.size() < 2) return;
    CVector2D at;
    size_t i = Beside(Ref<CVector2D>(0xBAA248), at);  // CRadar::vec2DRadarOrigin: the player
    float w = ScreenHeight() / 448.0f * 1.2f;        // 5-6 pixels across at 1080p
    // red to a waypoint, yellow to a mission's destination (Thomas, 28.10)
    const CRGBA colour = g_routeMission ? CRGBA{240, 200, 40, 255} : CRGBA{225, 45, 45, 255};
    for (CVector2D a = ToRadar(at); i + 1 < g_route.size(); ++i) {
        CVector2D b = ToRadar(g_route[i + 1]);
        Band(a, b, w, colour);
        a = b;
    }
}

}  // namespace coop
