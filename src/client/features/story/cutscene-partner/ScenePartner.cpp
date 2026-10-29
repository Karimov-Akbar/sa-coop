#include "features/story/cutscene-partner/ScenePartner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "features/story/cutscene-clips/SceneMocap.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"

namespace coop::scene {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kCutsceneObjects = 0xBC3F18, kNumCutsceneObjects = 0xBC3FE4;  // CCutsceneMgr::ms_pCutsceneObjects
constexpr uintptr_t kCutsceneName = 0xBC3F0C;                                         // CCutsceneMgr::ms_cutsceneName
constexpr uintptr_t kCameraMatrix = addr::TheCamera + 0x14;                         // CPlaceable::m_matrix
constexpr uintptr_t kCurrArea = 0xB72914;                                             // CGame::currArea
constexpr int kCigarette = 1485;                                                      // CJ_CIGGY
constexpr int8_t kAttractorSeat = 1, kAttractorStep = 9;                              // C2dEffectPedAttractor types
constexpr float kRejected = -1e9f;

// A variation played now and then instead of the act's loop: the person nods, thinks, shifts the weight, looks at
// the watch. Each starts and ends in its loop's pose (their root tracks stay within 6 cm of it), so the blends show
// no jump.
struct Fidget {
    const char* block;
    const char* anim;
    bool head;  // it moves the head itself: the look at the people of the scene lets go meanwhile
};
const Fidget kListen[] = {{"COP_AMBIENT", "Coplook_nod", true},   {"COP_AMBIENT", "Coplook_think", true},
                          {"PLAYIDLES", "shift", false},          {"PLAYIDLES", "shldr", false},
                          {"COP_AMBIENT", "Coplook_nod", true},   {"PLAYIDLES", "strleg", false},
                          {"PED", "XPRESSscratch", true},         {"PLAYIDLES", "time", true},
                          {"COP_AMBIENT", "Coplook_shake", true}, {"PED", "roadcross_gang", true}};
const Fidget kDealer[] = {{"DEALER", "DEALER_IDLE_01", false}, {"DEALER", "DEALER_IDLE_02", false},
                          {"DEALER", "DEALER_IDLE_03", false}, {"COP_AMBIENT", "Coplook_nod", true},
                          {"PLAYIDLES", "shift", false},       {"PED", "XPRESSscratch", true}};
const Fidget kSmoke[] = {{"SMOKING", "M_smk_drag", true}, {"SMOKING", "M_smk_tap", false}};
const Fidget kSeated[] = {{"MISC", "SEAT_watch", true}};

struct Act {
    const char* block;
    const char* anim;  // none: the clips of CJ listening in the cutscenes, one after another (SceneMocap)
    bool cigarette;
    float back;  // how far the animation's root track moves the body behind the ped's position
    const Fidget* fidgets = nullptr;
    size_t fidgetCount = 0;
};
// standing about, leaning on a wall, on a seat, on steps
const Act kStand[] = {{"DEALER", "DEALER_IDLE", false, 0, kDealer, std::size(kDealer)},
                      {"PED", "Idle_Gang1", false, 0, kListen, std::size(kListen)},
                      {"COP_AMBIENT", "Coplook_loop", false, 0, kListen, std::size(kListen)},
                      {"SMOKING", "M_smkstnd_loop", true, 0, kSmoke, std::size(kSmoke)}};
const Act kClips = {kMocapBlock, nullptr, false, 0};  // standing as CJ does in the cutscenes: in place of all but smoking
const Act kLean[] = {{"GANGS", "leanIDLE", false, 0.5f}, {"SMOKING", "M_smklean_loop", true, 0.24f}};
// SEAT_idle also moves the body 0.54 m down: the hips 0.46 m over the feet
const Act kSit[] = {{"PED", "SEAT_idle", false, 0.515f, kSeated, std::size(kSeated)}};
const Act kStep[] = {{"Attractors", "Stepsit_loop", false, 0.3f}};

template <size_t N>
const Act* Pick(const Act (&list)[N], uint8_t i) {
    return &list[i % N];
}

const Act* ActOf(const Spot& s) {
    switch (s.pose) {
        case POSE_STAND: return s.act % std::size(kStand) != 3 && MocapCount() > 1 ? &kClips : Pick(kStand, s.act);
        case POSE_WALK: return MocapCount() > 1 ? &kClips : Pick(kStand, 1);  // what he does when CJ stops
        case POSE_LEAN: return Pick(kLean, s.act);
        case POSE_SIT: return Pick(kSit, s.act);
        case POSE_STEP: return Pick(kStep, s.act);
        default: return nullptr;
    }
}

// The IFP block of an act, requested when it is not in memory yet (PED is always there).
bool BlockReady(const char* name) {
    if (!strcmp(name, "PED")) return true;
    if (!strcmp(name, kMocapBlock)) return MocapCount() > 0;  // ours: made in memory, never streamed
    int i = Call<0x4D3990, int>(name);  // CAnimManager::GetAnimationBlockIndex
    if (i < 0) return false;
    if (anim::BlockLoaded(i)) return true;
    RequestModel(anim::kIfpBase + i, 2);
    return false;
}

// ---- a living extra: the head turns to whoever talks, the face moves (the ped's own facial task)
constexpr uintptr_t kIkChainMan = 0xC15448;  // g_ikChainMan
void LookAt(CPed* ped, const CVector& at) {   // IKChainManager_c::LookAt: head and upper body to a world point
    static const char kPurpose[] = "coop partner";
    CVector p = at;
    CallMethod<0x618970, void>((void*)kIkChainMan, kPurpose, ped, (CEntity*)nullptr, 1500, -1, &p, true, 0.2f, 500, 3, true);
}
void StopLooking(CPed* ped) {
    if (CallMethod<0x6181A0, bool>((void*)kIkChainMan, ped)) CallMethod<0x618280, void>((void*)kIkChainMan, ped, 400u);  // IsLooking, AbortLookAt
}
enum Face { FACE_CURIOUS = 2, FACE_CHEWING = 8 };  // eFacialExpression
void SetFace(CPed* ped, Face f, int ms) {          // CTaskComplexFacial::SetRequest on the ped's facial task
    void* intel = ped->Intelligence();
    void* task = intel ? Field<void*>(intel, 0x04 + 0x14 + 3 * 4) : nullptr;  // CTaskManager::m_aSecondaryTasks[FACIAL_COMPLEX]
    int type = task ? CallVirtual<4, int>(task) : -1;
    if (type == 305) CallMethod<0x691230, void>(task, (int)f, ms, -1, 0);  // TASK_COMPLEX_FACIAL
    LOGD("Cutscene partner: face %d for %d ms%s", (int)f, ms, type == 305 ? "" : Format(" refused (facial task %d)", type).c_str());
}
uint32_t Mix(uint32_t a, uint32_t b) {  // a hash: the same draws in both games
    uint32_t h = a ^ (b * 0x9E3779B9u);
    h ^= h >> 16, h *= 0x85EBCA6Bu, h ^= h >> 13, h *= 0xC2B2AE35u, h ^= h >> 16;
    return h;
}

// ---- world queries (CWorld): collision
bool Seen(const CVector& a, const CVector& b) {  // nothing solid in between (see-through surfaces do not count)
    return Call<0x56A490, bool>(&a, &b, true, true, false, true, false, true, false);
}
bool Floor(float x, float y, float fromZ, float& z) {  // the first surface below (x, y, fromZ)
    bool found = false;
    z = Call<0x5696C0, float>(x, y, fromZ, &found, (CEntity**)nullptr);
    return found;
}
bool Free(const CVector& c, float r) {  // no building, vehicle or object there
    return !Call<0x569E20, CEntity*>(c, r, (CEntity*)nullptr, true, true, false, true, false, false);
}

CVector Flat(CVector v) {
    v.z = 0;
    float l = v.Length2D();
    return l > 1e-4f ? v * (1.0f / l) : CVector{0, 1, 0};
}
float HeadingOf(const CVector& dir) { return std::atan2(-dir.x, dir.y); }
CVector Forward(float heading) { return {-std::sin(heading), std::cos(heading), 0}; }
CVector Turn(const CVector& v, float a) { return {v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a), 0}; }
// where the spot's animation puts the body: leaning and sitting move it back from the ped's position
CVector BodyOf(const Spot& s) {
    const Act* a = ActOf(s);
    return a ? s.ground - Forward(s.heading) * a->back : s.ground;
}

CVector Transform(CEntity* e, const CVector& v, bool point) {  // model space -> world
    if (CMatrix* m = e->Matrix()) {
        CVector r = m->right * v.x + m->forward * v.y + m->up * v.z;
        return point ? r + m->pos : r;
    }
    float h = e->Placement().heading, c = std::cos(h), s = std::sin(h);
    CVector r{v.x * c - v.y * s, v.x * s + v.y * c, v.z};
    return point ? r + e->Placement().pos : r;
}

CEntity* CutsceneObject(int i) { return Ref<CEntity*>(kCutsceneObjects + (uintptr_t)i * 4); }
int NumCutsceneObjects() { return std::clamp(Ref<int32_t>(kNumCutsceneObjects), 0, 50); }
CEntity* CutsceneCj() {
    for (int i = 0; i < NumCutsceneObjects(); ++i)
        if (CEntity* o = CutsceneObject(i); o && o->ModelIndex() == 1) return o;  // "csplay"
    return nullptr;
}
bool IsPerson(CEntity* e) {  // the people of a cutscene are skinned; its props (furniture, doors) are not
    auto* rw = (uint8_t*)e->RwObject();
    return rw && rw[0] == 2 && Call<0x734A40, void*>(rw);  // RpClump with GetAnimHierarchyFromSkinClump
}

// ---- drawn triangles (render geometry) of an entity, in world coordinates
using TriangleFn = void (*)(const CVector&, const CVector&, const CVector&, void*);
struct Visit {
    TriangleFn fn;
    void* user;
};
void* __cdecl VisitAtomic(void* atomic, void* data) {
    auto* v = (Visit*)data;
    auto* geom = Field<uint8_t*>(atomic, 0x18);  // RpAtomic::geometry
    auto* frame = Field<void*>(atomic, 0x04);    // RwObject::parent
    if (!geom || !frame || !(Field<uint8_t>(atomic, 0x02) & 4)) return atomic;  // rpATOMICRENDER
    int tris = Field<int32_t>(geom, 0x10), verts = Field<int32_t>(geom, 0x14);
    auto* tr = Field<uint16_t*>(geom, 0x2C);    // RpTriangle: three vertex indices and the material
    auto* morph = Field<uint8_t*>(geom, 0x5C);  // RpMorphTarget[0]
    auto* vx = morph ? Field<CVector*>(morph, 0x14) : nullptr;
    auto* m = Call<0x7F0990, CMatrix*>(frame);  // RwFrameGetLTM
    if (!tr || !vx || !m || tris <= 0 || verts <= 0) return atomic;
    for (int k = 0; k < tris; ++k) {
        const uint16_t* t = tr + k * 4;
        if (t[0] >= verts || t[1] >= verts || t[2] >= verts) continue;
        CVector w[3];
        for (int j = 0; j < 3; ++j) {
            const CVector& p = vx[t[j]];
            w[j] = m->right * p.x + m->forward * p.y + m->up * p.z + m->pos;
        }
        v->fn(w[0], w[1], w[2], v->user);
    }
    return atomic;
}
void ForEachTriangle(CEntity* e, TriangleFn fn, void* user) {
    auto* rw = (uint8_t*)e->RwObject();
    if (!rw) return;
    Visit v{fn, user};
    if (rw[0] == 1) VisitAtomic(rw, &v);                              // rpATOMIC
    else if (rw[0] == 2) Call<0x749B70>(rw, &VisitAtomic, (void*)&v);  // RpClumpForAllAtomics
}

// The cutscene's cars are props of their own: big (the cube the cutscene keeps around each of its objects,
// CCutsceneMgr::UpdateCutsceneObjectBoundingBox, is several metres) and not skinned like its people. Their footprint
// comes from what is drawn, relative to the object's position.
struct Bounds {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
};
void Extend(const CVector& a, const CVector& b, const CVector& c, void* user) {
    auto* r = (Bounds*)user;
    for (const CVector* v : {&a, &b, &c})
        r->x0 = std::min(r->x0, v->x), r->x1 = std::max(r->x1, v->x), r->y0 = std::min(r->y0, v->y), r->y1 = std::max(r->y1, v->y);
}
bool IsCar(CEntity* e, Bounds& b) {
    void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)e->ModelIndex() * 4);
    auto* col = mi ? Field<uint8_t*>(mi, 0x14) : nullptr;  // CColModel: its box first
    if (!col || IsPerson(e)) return false;
    CVector lo = Field<CVector>(col, 0x00), hi = Field<CVector>(col, 0x0C);
    if (std::max(hi.x - lo.x, hi.y - lo.y) < 4.0f) return false;
    ForEachTriangle(e, &Extend, &b);
    CVector p = e->GetPosition();
    b.x0 -= p.x, b.x1 -= p.x, b.y0 -= p.y, b.y1 -= p.y;
    return std::max(b.x1 - b.x0, b.y1 - b.y0) >= 2.5f;
}

// ---- what is drawn around CJ, as a height map
// Cutscene sets often have walls and furniture without collision, and the cutscene's own props (tables, chairs,
// sofas) have none at all, so the drawn triangles are what counts. Per 10 cm cell, above CJ's floor: the highest
// thing a standing person bumps into, the highest flat surface at sitting height, and whether a floor is drawn.
constexpr float kCell = 0.1f, kBumpLow = 0.15f, kBumpHigh = 1.95f;
constexpr int kCells = 240;  // 24 m
struct Grid {
    float x0 = 0, y0 = 0, floor = -1000;
    uint8_t top[kCells * kCells];   // 2 cm steps, 0 nothing
    uint8_t seat[kCells * kCells];  // 1 cm steps
    uint8_t ground[kCells * kCells];
    uint8_t faces[kCells * kCells];  // which way the walls there are seen from: 8 directions, all for furniture tops

    bool Index(float x, float y, int& i) const {
        int cx = (int)std::floor((x - x0) / kCell), cy = (int)std::floor((y - y0) / kCell);
        if (cx < 0 || cy < 0 || cx >= kCells || cy >= kCells) return false;
        i = cy * kCells + cx;
        return true;
    }
    float Top(float x, float y) const {
        int i;
        return Index(x, y, i) ? top[i] * 0.02f : 0.0f;
    }
    float Seat(float x, float y) const {
        int i;
        return Index(x, y, i) ? seat[i] * 0.01f : 0.0f;
    }
    bool Ground(float x, float y) const {
        int i;
        return Index(x, y, i) && ground[i];
    }
    bool Clear(const CVector& p, float r) const {  // nothing to bump into within r of p
        for (float dy = -r; dy <= r + 1e-3f; dy += kCell)
            for (float dx = -r; dx <= r + 1e-3f; dx += kCell)
                if (dx * dx + dy * dy <= r * r + 1e-4f && Top(p.x + dx, p.y + dy) > 0) return false;
        return true;
    }
    // first thing at least `height` high along dir from p, within `range`; -1 none
    float Hit(const CVector& p, const CVector& dir, float range, float height) const {
        for (int k = 1; k * 0.05f <= range + 1e-3f; ++k)
            if (Top(p.x + dir.x * k * 0.05f, p.y + dir.y * k * 0.05f) >= height) return k * 0.05f;
        return -1;
    }
    void Add(const CVector& a, const CVector& b, const CVector& c);
} g_grid;

float SegDist2(float px, float py, const CVector& a, const CVector& b) {
    float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
    float t = l > 1e-8f ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / l, 0.0f, 1.0f) : 0.0f;
    float ex = a.x + dx * t - px, ey = a.y + dy * t - py;
    return ex * ex + ey * ey;
}

void Grid::Add(const CVector& a, const CVector& b, const CVector& c) {
    float za = a.z - floor, zb = b.z - floor, zc = c.z - floor;
    float zmin = std::min({za, zb, zc}), zmax = std::max({za, zb, zc});
    if (zmax < -0.15f || zmin > kBumpHigh) return;
    CVector n = (b - a).Cross(c - a);
    float len = n.Length();
    if (len < 1e-6f) return;
    bool flat = std::fabs(n.z) / len > 0.7f;
    if (!flat && zmax < kBumpLow) return;  // a kerb or a step: the feet go over it
    int cx0 = std::max(0, (int)std::floor((std::min({a.x, b.x, c.x}) - x0) / kCell));
    int cx1 = std::min(kCells - 1, (int)std::floor((std::max({a.x, b.x, c.x}) - x0) / kCell));
    int cy0 = std::max(0, (int)std::floor((std::min({a.y, b.y, c.y}) - y0) / kCell));
    int cy1 = std::min(kCells - 1, (int)std::floor((std::max({a.y, b.y, c.y}) - y0) / kCell));
    float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    bool edgeOn = std::fabs(d) < 1e-6f;  // a wall seen from above
    uint8_t wall = (uint8_t)(std::min(zmax, 2.5f) / 0.02f);
    // the drawn side of a wall: film sets put cameras behind walls that are only drawn from inside the room
    uint8_t side = 0xFF;
    if (!flat && n.Length2D() > 1e-6f)
        side = (uint8_t)(1u << (((int)std::floor(std::atan2(n.y, n.x) / 0.7853982f + 0.5f) + 8) % 8));
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) {
            float px = x0 + (cx + 0.5f) * kCell, py = y0 + (cy + 0.5f) * kCell;
            float l1 = 0, l2 = 0, l3 = 0;
            bool inside = false;
            if (!edgeOn) {
                l1 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / d;
                l2 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / d;
                l3 = 1 - l1 - l2;
                inside = l1 >= 0 && l2 >= 0 && l3 >= 0;
            }
            int i = cy * kCells + cx;
            if (flat) {
                if (!inside) continue;
                float z = l1 * za + l2 * zb + l3 * zc;
                if (z > -0.15f && z < 0.12f) {
                    ground[i] = 1;
                } else if (z >= kBumpLow && z <= kBumpHigh) {
                    top[i] = std::max(top[i], (uint8_t)(z / 0.02f));
                    faces[i] = 0xFF;
                    if (z >= 0.3f && z <= 0.7f) seat[i] = std::max(seat[i], (uint8_t)(z * 100));
                }
            } else {  // walls and the sides of furniture: every cell they pass, as high as they reach
                constexpr float kNear = kCell * kCell * 0.36f;
                if (!inside && SegDist2(px, py, a, b) > kNear && SegDist2(px, py, b, c) > kNear && SegDist2(px, py, c, a) > kNear)
                    continue;
                top[i] = std::max(top[i], wall);
                faces[i] |= side;
            }
        }
}

void AddTriangle(const CVector& a, const CVector& b, const CVector& c, void* grid) { ((Grid*)grid)->Add(a, b, c); }

// Everything drawn in this interior within 12 m of CJ: buildings and objects (no LODs), and the cutscene's props.
void BuildGrid(const CVector& cj, float floor) {
    Grid& g = g_grid;
    memset(g.top, 0, sizeof g.top);
    memset(g.seat, 0, sizeof g.seat);
    memset(g.ground, 0, sizeof g.ground);
    memset(g.faces, 0, sizeof g.faces);
    g.floor = floor;
    g.x0 = cj.x - kCells * kCell * 0.5f;
    g.y0 = cj.y - kCells * kCell * 0.5f;
    int area = Ref<int32_t>(kCurrArea);
    static CEntity* ents[1024];
    int16_t n = 0;
    Call<0x564A20>(&cj, 45.0f, false, &n, (int16_t)1024, ents, true, false, false, true, false);  // FindObjectsInRange
    CEntity* own[50];  // the cutscene's objects are world objects too: its people must stay out
    int owned = NumCutsceneObjects();
    for (int i = 0; i < owned; ++i) own[i] = CutsceneObject(i);
    for (int i = 0; i < n; ++i) {
        CEntity* e = ents[i];
        if (!(e->Flags() & 0x80) || Field<uint8_t>(e, 0x34)) continue;  // not drawn, or the LOD of others
        if (std::find(own, own + owned, e) != own + owned) continue;
        if (e->AreaCode() != area && e->AreaCode() != 13) continue;     // another interior
        void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)e->ModelIndex() * 4);
        void* col = mi ? Field<void*>(mi, 0x14) : nullptr;  // its bounds: CBaseModelInfo::m_pColModel
        if (col) {
            CVector centre = Transform(e, Field<CVector>(col, 0x18), true);
            if ((centre - cj).Length2D() - Field<float>(col, 0x24) > 17.0f) continue;
        } else if ((e->GetPosition() - cj).Length2D() > 30.0f) {
            continue;
        }
        ForEachTriangle(e, &AddTriangle, &g);
    }
    for (int i = 0; i < NumCutsceneObjects(); ++i)
        if (CEntity* o = CutsceneObject(i); o && o->ModelIndex() != 1 && !IsPerson(o)) ForEachTriangle(o, &AddTriangle, &g);
}

// nothing drawn between the camera and the point (the last 35 cm belong to the spot: its seat, its wall);
// a wall counts only when its drawn side faces the camera
bool GridSeen(const CVector& from, const CVector& to) {
    CVector d = to - from;
    float len = d.Length2D();
    int steps = (int)(len / 0.08f);
    uint8_t facing = 0;  // the directions a side must face to be seen from the camera
    for (int s = 0; s < 8; ++s)
        if (std::cos(s * 0.7853982f) * -d.x + std::sin(s * 0.7853982f) * -d.y > 0) facing |= (uint8_t)(1u << s);
    for (int k = 1; k < steps; ++k) {
        float t = (float)k / (float)steps;
        if ((1 - t) * len < 0.35f) break;
        CVector p = from + d * t;
        int i;
        if (!g_grid.Index(p.x, p.y, i) || !g_grid.top[i]) continue;
        if (g_grid.floor + g_grid.top[i] * 0.02f >= p.z && (g_grid.faces[i] & facing)) return false;
    }
    return true;
}

// ---- one shot of the cutscene
struct Body {  // a person on the screen
    float x, top, bottom, half, depth;
};

bool Project(const CVector& feet, float height, Body& b) {
    float hx, hy, hd, fx, fy, fd;
    if (!WorldToScreen(feet + CVector{0, 0, height}, hx, hy, hd) || !WorldToScreen(feet + CVector{0, 0, 0.05f}, fx, fy, fd))
        return false;
    b.x = (hx + fx) * 0.5f;
    b.top = hy;
    b.bottom = fy;
    b.half = std::fabs(fy - hy) * 0.2f;  // a person with the arms out is about 40% as wide as tall
    b.depth = std::min(hd, fd);
    return true;
}

// Someone of the scene: close to the camera the feet (or even the head) fall outside the projection, yet the
// body still covers a large part of the picture.
bool ProjectActor(const CVector& feet, Body& b) {
    float hx, hy, hd, cx, cy, cd, fx, fy, fd;
    bool head = WorldToScreen(feet + CVector{0, 0, 1.75f}, hx, hy, hd);
    bool chest = WorldToScreen(feet + CVector{0, 0, 1.1f}, cx, cy, cd);
    bool foot = WorldToScreen(feet + CVector{0, 0, 0.05f}, fx, fy, fd);
    if (!head && !chest) return false;
    float w = (float)ScreenWidth(), focal = w * 0.71f;  // about a 70 degree view
    float depth = chest ? cd : hd;
    b.x = chest ? cx : hx;
    b.top = head ? hy : -1e6f;
    b.bottom = foot ? fy : 1e6f;
    b.half = 0.3f * focal / std::max(depth, 0.3f);  // half a shoulder width
    b.depth = depth;
    return true;
}

bool ToScreenNow(const CVector& p, float& sx, float& sy, float& depth);

// The screen box around the skeleton of a cutscene actor: every bone counts, so arms out or a bent back do too
// (and a head right in front of the lens: the engine's own projection drops what is nearer than 1 m).
bool ProjectSkeleton(CEntity* e, Body& b) {
    auto* clump = (uint8_t*)e->RwObject();
    if (!clump || clump[0] != 2) return false;  // not an RpClump (props are atomics)
    void* hier = Call<0x734A40, void*>(clump);  // GetAnimHierarchyFromSkinClump
    if (!hier) return false;
    int n = Field<int32_t>(hier, 0x04);         // numNodes
    auto* m = Call<0x7C5120, uint8_t*>(hier);   // RpHAnimHierarchyGetMatrixArray: world matrices
    if (!m || n <= 0 || n > 128) return false;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, depth = 1e9f;
    for (int i = 0; i < n; ++i) {
        float sx, sy, d;
        if (!ToScreenNow(*(CVector*)(m + (size_t)i * 0x40 + 0x30), sx, sy, d)) continue;
        x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
        depth = std::min(depth, d);
    }
    if (x1 < x0) return false;
    float pad = std::max(8.0f, (y1 - y0) * 0.08f);  // flesh and the top of the head around the bones
    b.x = (x0 + x1) * 0.5f;
    b.half = (x1 - x0) * 0.5f + pad;
    b.top = y0 - pad * 1.5f;
    b.bottom = y1 + pad;
    b.depth = depth;
    return true;
}

bool Overlap(const Body& a, const Body& b, float share) {  // more than share of the narrower box
    float w = std::min(a.x + a.half, b.x + b.half) - std::max(a.x - a.half, b.x - b.half);
    float h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
    return w > share * 2 * std::min(a.half, b.half) && h > 0;
}

struct Actor {  // another person of the scene near CJ
    CEntity* e;
    CVector feet;
    Body body;
    bool onScreen;
};

// ---- the rest of the shot. The camera runs on the cutscene's spline (TheCamera.m_aPathArray, read the way
// CCam::Process_FlyBy 0x5B25F0 does) and the people walk their animations' root tracks, so both are known up to the
// next cut: the spot must suit the whole shot, not only its first frame (cameras pan, people walk about).
constexpr uintptr_t kPathFov = 0xB6F988, kPathRoll = 0xB6F98C, kPathPos = 0xB6F990, kPathTarget = 0xB6F994;
constexpr uintptr_t kClumpOffset = 0xB5F878;  // RpAnimBlend plugin offset: the CAnimBlendClumpData of a clump
constexpr uint16_t kModeFlyBy = 17;
constexpr float kCutKeys = 32.0f;  // keys closer than this (ms) are a cut: the camera jumps over them (0x8D0F80)

struct Spline {  // n keys of `stride` floats from d[1]: time (s), value(s), in tangent(s), out tangent(s)
    const float* d = nullptr;
    int n = 0, dims = 0, stride = 0;
    bool Load(uintptr_t at, int dimensions) {
        d = Ref<const float*>(at);
        dims = dimensions;
        stride = 1 + dims * 3;
        n = d ? (int)d[0] : 0;
        return n >= 2 && n < 4000;
    }
    const float* Key(int k) const { return d + 1 + k * stride; }
    float Time(int k) const { return (Key(k)[0] - d[1]) * 1000.0f; }  // ms on the camera's clock
    // a cubic Bezier from a key's value over its out tangent and the next key's in tangent, a straight line where
    // the out tangents lie on the values (0x5B2090)
    void At(float t, float* out) const {
        int k = 0;
        while (k < n - 2 && Time(k + 1) <= t) ++k;
        if (k < n - 2 && Time(k + 1) - Time(k) <= kCutKeys) ++k;  // a cut: the camera is on the next key at once
        const float *a = Key(k), *b = Key(k + 1);
        float span = Time(k + 1) - Time(k);
        float u = span > 0 ? std::clamp((t - Time(k)) / span, 0.0f, 1.0f) : 1.0f, v = 1 - u;
        bool line = true;  // the key's out tangents on its values in every dimension (0x5B2090, 1D 0x5B2330)
        for (int i = 0; i < dims; ++i) line = line && a[1 + 2 * dims + i] == a[1 + i];
        for (int i = 0; i < dims; ++i) {
            float p0 = a[1 + i], c0 = a[1 + 2 * dims + i], c1 = b[1 + dims + i], p1 = b[1 + i];
            out[i] = line ? p0 + (p1 - p0) * u : v * v * v * p0 + 3 * v * v * u * c0 + 3 * v * u * u * c1 + u * u * u * p1;
        }
    }
    // the next cut after t: two keys at (almost) the same time far apart; the shot ends at `from`, the next starts at `to`
    bool NextCut(float t, float jump, float& from, float& to) const {
        for (int k = 1; k < n; ++k) {
            if (Time(k) <= t || Time(k) - Time(k - 1) > kCutKeys) continue;
            float d2 = 0;
            for (int i = 1; i <= dims; ++i) d2 += (Key(k)[i] - Key(k - 1)[i]) * (Key(k)[i] - Key(k - 1)[i]);
            if (d2 > jump * jump) {
                from = Time(k - 1), to = Time(k);
                return true;
            }
        }
        return false;
    }
};

struct View {  // the camera at one moment of the shot and how it projects: screen = centre + (x/z, y/z) * k
    CVector pos, fwd, right, up;
    float kx = 0, ky = 0, cx = 0, cy = 0;
};

bool ToScreen(const View& v, const CVector& p, float& sx, float& sy, float& depth) {
    CVector d = p - v.pos;
    depth = d.Dot(v.fwd);
    if (depth < 0.3f) return false;
    sx = v.cx + d.Dot(v.right) / depth * v.kx;
    sy = v.cy - d.Dot(v.up) / depth * v.ky;
    return true;
}

struct CameraPath {
    Spline fov, roll, pos, target;
    bool Load() { return fov.Load(kPathFov, 1) && roll.Load(kPathRoll, 1) && pos.Load(kPathPos, 3) && target.Load(kPathTarget, 3); }
    void At(float t, View& v, float& f) const {
        float p[3], q[3], r = 0;
        pos.At(t, p);
        target.At(t, q);
        fov.At(t, &f);
        roll.At(t, &r);
        // the keys are relative to the camera's copy of the cutscene's offset: the cutscene's own one is lifted by
        // 1 m once the scene is set up (CCutsceneMgr::SetupCutsceneToStart)
        CVector o = Ref<CVector>(addr::TheCamera + 0x884);  // CCamera::m_vecCutSceneOffset
        v.pos = o + CVector{p[0], p[1], p[2]};
        v.fwd = (CVector{q[0], q[1], q[2]} - CVector{p[0], p[1], p[2]}).Normalized();
        float a = r * 0.0174533f + 1.5707963f;  // the roll tilts the up vector
        CVector left = CVector{std::cos(a), 0, std::sin(a)}.Cross(v.fwd).Normalized();
        v.up = v.fwd.Cross(left);
        v.right = left * -1.0f;
    }
};

// how the frame being drawn projects, with the game's own numbers (they scale with the field of view)
bool Calibrate(View& v) {
    float x0, y0, d0, x1, y1, d1, x2, y2, d2;
    CVector c = v.pos + v.fwd * 10.0f;
    if (!WorldToScreen(c, x0, y0, d0) || !WorldToScreen(c + v.right, x1, y1, d1) || !WorldToScreen(c + v.up, x2, y2, d2))
        return false;
    v.cx = x0, v.cy = y0, v.kx = (x1 - x0) * 10.0f, v.ky = (y0 - y2) * 10.0f;
    return std::fabs(v.kx) > 1.0f && std::fabs(v.ky) > 1.0f;
}

View g_now;  // the frame being analysed
bool g_nowOk = false;
bool ToScreenNow(const CVector& p, float& sx, float& sy, float& depth) {
    return g_nowOk ? ToScreen(g_now, p, sx, sy, depth) : WorldToScreen(p, sx, sy, depth);
}

// A person of the cutscene moves by its animation's root track (CCutsceneObject::ProcessControl adds the root's
// motion to its position), so the track tells where it will be.
struct Track {
    const uint8_t* seq = nullptr;  // CAnimBlendSequence of the root
    float time = 0;                // the animation's current time (s)
};
bool TrackOf(CEntity* e, Track& tr) {
    auto* clump = e ? (uint8_t*)e->RwObject() : nullptr;
    if (!clump || clump[0] != 2) return false;
    auto* data = Ref<uint8_t*>((uintptr_t)clump + Ref<uint32_t>(kClumpOffset));  // CAnimBlendClumpData
    auto* link = data ? Field<uint8_t*>(data, 0x00) : nullptr;                    // the first association's link (+4)
    auto* hier = link ? Field<uint8_t*>(link - 4, 0x14) : nullptr;                 // CAnimBlendAssociation::m_BlendHier
    auto* seqs = hier ? Field<uint8_t*>(hier, 0x04) : nullptr;
    if (!seqs || !Field<uint16_t>(hier, 0x08) || !(Field<uint16_t>(seqs, 0x04) & 2)) return false;  // the root moves
    tr.seq = seqs;
    tr.time = Field<float>(link - 4, 0x20);  // m_CurrentTime
    return true;
}
CVector RootAt(const uint8_t* seq, float t) {  // the root's translation at t (s); key frames hold time steps
    int frames = Field<uint16_t>(seq, 0x06);
    auto* f = Field<const uint8_t*>(seq, 0x08);
    bool packed = (Field<uint16_t>(seq, 0x04) & 4) != 0;
    CVector prev{};
    float at = 0, prevAt = 0;
    for (int i = 0; f && i < frames; ++i) {
        CVector p;
        if (packed) {  // KeyFrameTransCompressed: rotation, step (1/60 s), translation (1/1024 m)
            auto* k = (const int16_t*)(f + i * 16);
            at += k[4] / 60.0f;
            p = {k[5] / 1024.0f, k[6] / 1024.0f, k[7] / 1024.0f};
        } else {  // KeyFrameTrans
            auto* k = (const float*)(f + i * 32);
            at += k[4];
            p = {k[5], k[6], k[7]};
        }
        if (at >= t) return i == 0 || at <= prevAt ? p : prev + (p - prev) * ((t - prevAt) / (at - prevAt));
        prev = p;
        prevAt = at;
    }
    return prev;
}
CVector FeetLater(const Track& tr, const CVector& feet, float dt) {
    if (!tr.seq) return feet;
    CVector d = RootAt(tr.seq, tr.time + dt) - RootAt(tr.seq, tr.time);
    return feet + CVector{d.x, d.y, 0};
}

struct Sample {  // a moment later in the shot
    float dt;    // seconds after the frame analysed
    View view;
    CVector cj;
    std::vector<CVector> actors;  // as Scene::actors
    std::vector<CVector> cars;    // as Scene::cars
};
std::vector<Sample> g_ahead;
float g_shotFrom = 0, g_shotTo = 0;  // the camera's clock (ms) at the next cut: this shot ends, the next one starts

// The host's game decides and the partner's follows: each spot goes with the moment (the camera's clock) its
// shot begins, and the partner's copy of the cutscene takes it on when it gets there (it runs a little behind).
struct Decision {
    Spot spot;
    float at;
};
std::deque<Decision> g_follow;  // partner: the host's choices still to come
bool g_decided = false;         // host: a choice not sent yet
float g_decidedAt = 0;
struct {
    CVector cj;
    float at;
    bool armed;
} g_check{};  // where CJ's track says CJ will be (review logs)

struct Car {  // a vehicle of the cutscene (a prop: no collision, it may drive through the partner)
    int index = -1;  // its cutscene object
    CVector pos;
    Bounds b;  // its footprint around pos
    Track track;
    bool Covers(const CVector& p, const CVector& at, float margin) const {  // p on it when the car is at `at`
        CVector d = p - at;
        return d.x > b.x0 - margin && d.x < b.x1 + margin && d.y > b.y0 - margin && d.y < b.y1 + margin;
    }
};

struct Scene {
    CVector cam, cj;
    CVector camFwd;
    float fov = 70;
    float floor = 0;  // under CJ
    Body cjBody{};
    bool cjOnScreen = false;
    bool cjInCar = false;  // CJ within a car's footprint (InCar tells whether he sits in it)
    std::vector<Actor> actors;
    std::vector<Car> cars;
    float w = 0, h = 0;
};

float PoseHeight(Pose p) { return p == POSE_SIT || p == POSE_STEP ? 1.35f : 1.8f; }
float PoseBonus(Pose p) { return p == POSE_SIT ? 1.6f : p == POSE_STEP ? 1.3f : p == POSE_LEAN ? 1.0f : 0.0f; }

Spot g_spot;
bool g_haveSpot = false;
Spot g_seat;  // his seat in a car of this cutscene: a shot he keeps out of (the camera at it) gives it back after
bool g_haveSeat = false;
Scene g_lastScene;
bool g_live = false;  // a cutscene with CJ was analysed in the last frame
CVector g_lastCam, g_lastCj;
int g_shot = 0;
uint32_t g_seed = 0;
char g_cutscene[9] = {};

// head and body in the picture (the legs may be cut off, as in a medium shot)
bool InFrame(const Scene& sc, const Body& b) {
    float waist = b.top + (b.bottom - b.top) * 0.55f;
    return b.depth >= 1.8f && b.x - b.half >= sc.w * 0.06f && b.x + b.half <= sc.w * 0.94f && b.top >= sc.h * 0.03f &&
           waist <= sc.h * 0.97f;
}
bool OnScreen(const Scene& sc, const Body& b) {  // any part of it in the picture
    return b.depth >= 0.5f && b.x + b.half > 0 && b.x - b.half < sc.w && b.bottom > 0 && b.top < sc.h;
}
// someone the picture shows, the middle of the body in it: not an arm at the edge of a shot over the shoulder
bool Shown(const Scene& sc, const Body& b) { return OnScreen(sc, b) && b.x > 0 && b.x < sc.w; }

// The camera and the people over the rest of the shot, about every quarter of a second up to the cut.
void BuildAhead(const Scene& sc) {
    g_ahead.clear();
    CameraPath path;
    if (cam::Mode() != kModeFlyBy || !path.Load()) return;
    float now = Ref<float>(cam::Active() + 0x90), fovNow = 0;  // CCam::m_fTimeElapsedFloat: the spline's clock (ms)
    View cur;
    path.At(now, cur, fovNow);
    // the spline must give the camera being drawn, otherwise its clock or its keys are not what they seem
    if ((cur.pos - sc.cam).Length() > 0.5f || cur.fwd.Dot(sc.camFwd) < 0.99f || !Calibrate(cur)) {
        LOGD("Cutscene camera off its spline (%.2f m, %.3f): the shot is judged by its first frame", (cur.pos - sc.cam).Length(),
             cur.fwd.Dot(sc.camFwd));
        return;
    }
    float end = path.pos.Time(path.pos.n - 1), next = end;
    for (const Spline* s : {&path.pos, &path.target, &path.fov}) {
        float from, to;
        if (s->NextCut(now, s == &path.fov ? 1.0f : 0.3f, from, to) && from < end) end = from, next = to;
    }
    g_shotFrom = end;
    g_shotTo = next;
    Track cjTrack;
    TrackOf(CutsceneCj(), cjTrack);
    std::vector<Track> tracks(sc.actors.size());
    for (size_t i = 0; i < sc.actors.size(); ++i) TrackOf(sc.actors[i].e, tracks[i]);
    CVector cjFeet{sc.cj.x, sc.cj.y, sc.floor};
    int steps = std::clamp((int)((end - now) / 250.0f), 1, 16);
    for (int i = 1; i <= steps; ++i) {
        float t = now + (end - 30.0f - now) * (float)i / (float)steps;  // the last one just before the cut
        if (t <= now) break;
        Sample m;
        m.dt = (t - now) / 1000.0f;
        float f = fovNow;
        path.At(t, m.view, f);
        float zoom = std::tan(fovNow * 0.00872665f) / std::tan(std::clamp(f, 5.0f, 170.0f) * 0.00872665f);  // tan(fov/2)
        m.view.kx = cur.kx * zoom, m.view.ky = cur.ky * zoom, m.view.cx = cur.cx, m.view.cy = cur.cy;
        m.cj = FeetLater(cjTrack, cjFeet, m.dt);
        for (size_t k = 0; k < sc.actors.size(); ++k) m.actors.push_back(FeetLater(tracks[k], sc.actors[k].feet, m.dt));
        for (const Car& c : sc.cars) m.cars.push_back(FeetLater(c.track, c.pos, m.dt));
        g_ahead.push_back(std::move(m));
    }
}

bool ProjectAt(const View& v, const CVector& feet, float height, Body& b) {
    float hx, hy, hd, fx, fy, fd;
    if (!ToScreen(v, feet + CVector{0, 0, height}, hx, hy, hd) || !ToScreen(v, feet + CVector{0, 0, 0.05f}, fx, fy, fd))
        return false;
    b.x = (hx + fx) * 0.5f;
    b.top = std::min(hy, fy), b.bottom = std::max(hy, fy);
    b.half = (b.bottom - b.top) * 0.2f;
    b.depth = std::min(hd, fd);
    return true;
}
bool PersonAt(const View& v, const CVector& feet, Body& b) {  // someone of the scene later on: as wide as the shoulders
    if (!ProjectAt(v, feet, 1.75f, b)) return false;
    b.half = std::max(b.half, 0.3f * std::fabs(v.kx) / std::max(b.depth, 0.3f));
    return true;
}

enum Reject { R_DISTANCE, R_CROWD, R_FRAME, R_COVERS_CJ, R_COVERS_ACTOR, R_SOLID, R_HIDDEN, R_FLOOR, R_FRONT, R_LATER, R_COUNT };

struct Later {  // the spot over the rest of the shot
    int in = 0, edge = 0, out = 0;
    bool bad = false;
};
// The camera in him or about to be (a camera following CJ goes where the partner walks behind him): the body
// fills the picture and projecting it says nothing.
bool AtCamera(const CVector& cam, const CVector& feet, float height) {
    return (cam - feet).Length2D() < 1.3f && cam.z > feet.z - 0.5f && cam.z < feet.z + height + 0.5f;
}

// In the picture of a later moment: he covers someone, or he is bigger than the people in it (the camera may come
// up close later).
float Covered(const Body& a, const Body& b);
bool InWayAt(const Scene& sc, const Sample& m, const Body& b, bool behindOk) {
    Body p;
    float tallest = 0, hidden = 0;
    auto blocks = [&](float share) {  // as InPeoplesWay: at the back of the group, mostly in sight
        if (!Overlap(b, p, share)) return false;
        if (!behindOk || b.depth < p.depth + 1.0f) return true;
        hidden += Covered(b, p);
        return hidden > 0.6f;
    };
    if (PersonAt(m.view, m.cj, p) && OnScreen(sc, p)) {
        if (blocks(0.0f)) return true;
        tallest = Shown(sc, p) ? p.bottom - p.top : 0;
    }
    for (const CVector& a : m.actors)
        if (PersonAt(m.view, a, p) && OnScreen(sc, p)) {
            if (blocks(0.1f)) return true;
            tallest = std::max(tallest, Shown(sc, p) ? p.bottom - p.top : 0);
        }
    return b.bottom - b.top > std::max(tallest * 1.15f, sc.h * 0.3f);
}

// nobody walks into it, it covers nobody, and the camera keeps it in the picture or away from it
Later RateLater(const Scene& sc, const Spot& s, const CVector& body, bool behindOk = false) {
    Later r;
    float height = PoseHeight(s.pose), room = s.pose == POSE_SIT || s.pose == POSE_STEP ? 0.55f : 0.75f;
    for (const Sample& m : g_ahead) {
        r.bad = (m.cj - body).Length2D() < 1.0f || AtCamera(m.view.pos, body, height);
        for (const CVector& a : m.actors) r.bad = r.bad || (a - body).Length2D() < room;
        for (size_t k = 0; k < sc.cars.size(); ++k) r.bad = r.bad || sc.cars[k].Covers(body, m.cars[k], 0.6f);
        Body b;
        if (r.bad) return r;
        if (!ProjectAt(m.view, body, height, b) || !OnScreen(sc, b)) {
            ++r.out;
            continue;
        }
        r.bad = InWayAt(sc, m, b, behindOk);
        if (r.bad) return r;
        if (!GridSeen(m.view.pos, body + CVector{0, 0, height * 0.6f})) ++r.out;  // behind a wall for a moment
        else if (InFrame(sc, b)) ++r.in;
        else ++r.edge;
    }
    return r;
}

// An extra keeps out of the scene's way: no bigger on the screen than its people (a third of the screen when none
// is in the picture), and not between two of them nearer to the camera than they are, where the talk goes on.
// (onScreen means only that the skeleton projects: CJ next to the camera, out of the picture but for an elbow, would
// let the partner fill the screen.)
bool InTheWay(const Scene& sc, const Body& b) {
    std::vector<const Body*> people;
    if (sc.cjOnScreen && Shown(sc, sc.cjBody)) people.push_back(&sc.cjBody);
    for (const Actor& a : sc.actors)
        if (a.onScreen && Shown(sc, a.body)) people.push_back(&a.body);
    float tallest = 0;
    for (const Body* p : people) tallest = std::max(tallest, p->bottom - p->top);
    if (b.bottom - b.top > std::max(tallest * 1.15f, sc.h * 0.3f)) return true;
    for (const Body* p : people)
        for (const Body* q : people)
            if (p->x < b.x && b.x < q->x && b.depth < std::max(p->depth, q->depth) - 0.3f) return true;
    return false;
}
int g_rejects[R_COUNT];
int g_lastReject = -1;
float Rejected(Reject r) {
    ++g_rejects[r];
    g_lastReject = r;
    return kRejected;
}
int g_attractors[16];  // ped attractor types seen around CJ in the last analysis (logs)

// a floor at CJ's level under the spot, solid or only drawn (the partner is frozen there, so drawn is enough)
bool FloorAt(const Scene& sc, CVector& g) {
    float z;
    if (Floor(g.x, g.y, sc.floor + 1.0f, z) && std::fabs(z - sc.floor) <= 0.45f) {
        g.z = z;
        return true;
    }
    g.z = sc.floor;
    return g_grid.Ground(g.x, g.y);
}

// The spot itself, whatever the camera: near CJ (within `far`), and nobody of the scene or a car of it there. -1 when so.
int Crowded(const Scene& sc, const Spot& s, float& clear, float far = 6.0f) {
    bool seated = s.pose == POSE_SIT || s.pose == POSE_STEP;
    CVector body = BodyOf(s);  // where the animation puts it
    float d = (body - sc.cj).Length2D();
    if (d < 1.3f || d > far) return R_DISTANCE;
    clear = 9.0f;
    for (const Actor& a : sc.actors) clear = std::min(clear, (a.feet - body).Length2D());
    if (clear < (seated ? 0.55f : 0.75f)) return R_CROWD;
    for (const Car& c : sc.cars)
        if (c.Covers(body, c.pos, 0.6f)) return R_CROWD;
    return AtCamera(sc.cam, body, PoseHeight(s.pose)) ? R_FRAME : -1;
}
// How much of box a the box b covers (0..1)
float Covered(const Body& a, const Body& b) {
    float w = std::min(a.x + a.half, b.x + b.half) - std::max(a.x - a.half, b.x - b.half);
    float h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
    return w > 0 && h > 0 ? w * h / std::max(1.0f, 2 * a.half * (a.bottom - a.top)) : 0.0f;
}

// In the picture: covering nobody of the scene and out of its way. -1 when so. Behind (a metre farther from the camera
// than whom he overlaps) he may stand at the back of the group, if most of him still shows: kept off every person of
// the shot, he was out of the picture through most medium shots of a group.
int InPeoplesWay(const Scene& sc, const Body& b, bool behindOk = false) {
    Body cj = sc.cjBody;  // in a close-up CJ bends and waves about more than his root track tells: keep off
    if (cj.depth < 2.0f) cj.half *= 1.5f;
    float hidden = 0;
    auto blocks = [&](const Body& p, float share) {
        if (!Overlap(b, p, share)) return false;
        if (!behindOk || b.depth < p.depth + 1.0f) return true;
        hidden += Covered(b, p);
        return hidden > 0.6f;
    };
    if (sc.cjOnScreen && blocks(cj, 0.0f)) return R_COVERS_CJ;
    for (const Actor& a : sc.actors)  // nobody of the scene behind or in front of the partner
        if (a.onScreen && blocks(a.body, 0.1f)) return R_COVERS_ACTOR;
    return InTheWay(sc, b) ? R_FRONT : -1;
}
// Room for the body: a wall to lean on with nothing between, free floor at CJ's level (its height goes into the
// spot), nothing where the hips and the head are. -1 when so.
int NoRoom(const Scene& sc, Spot& s) {
    CVector& g = s.ground;
    bool seated = s.pose == POSE_SIT || s.pose == POSE_STEP;
    if (s.pose == POSE_LEAN) {  // the feet on free floor and nothing between the back and the wall
        if (!g_grid.Clear(g, 0.22f) || g_grid.Hit(g, Forward(s.heading) * -1.0f, ActOf(s)->back - 0.03f, kBumpLow) >= 0)
            return R_SOLID;
    } else if (!seated && !g_grid.Clear(g, 0.3f)) {
        return R_SOLID;
    }
    if (!seated && !FloorAt(sc, g)) return R_FLOOR;
    if (!seated && (!Free(g + CVector{0, 0, 0.65f}, 0.28f) || !Free(g + CVector{0, 0, 1.45f}, 0.28f)))  // the costly one
        return R_SOLID;
    return -1;
}

// For the log and the reviews: the share of the shot the partner stands in the picture (his middle within the frame,
// no wall in between), now and at the samples ahead. -1 riding or hidden. A place he stays at counted as seen before.
float SeenShare(const Scene& sc, const Spot& s) {
    if (s.pose == POSE_HIDDEN || s.pose == POSE_CAR) return -1.0f;
    float height = PoseHeight(s.pose);
    CVector body = BodyOf(s), chest = body + CVector{0, 0, height * 0.6f}, head = body + CVector{0, 0, height - 0.15f};
    // walls by the world's collision, once: the obstacle map keeps a cell's highest point only, and a porch's roof
    // hid everything under it (CAT_2's cabin, Sweet's door)
    if (!Seen(sc.cam, chest) && !Seen(sc.cam, head)) return 0.0f;
    Body b;
    int seen = Project(body, height, b) && Shown(sc, b) ? 1 : 0;
    for (const Sample& m : g_ahead)
        if (ProjectAt(m.view, body, height, b) && Shown(sc, b)) ++seen;
    return (float)seen / (float)(1 + g_ahead.size());
}

// How good the spot is for this shot; kRejected when it does not fit at all. Cheap checks first. Loose: he need not
// be in the picture as a whole, nor seen (cut by its edge, half behind something), only out of the people's way.
float Rate(const Scene& sc, Spot& s, bool loose = false) {
    g_lastReject = -1;
    float clear;
    // the loose places go farther: in a crowd around CJ (CESAR1A, GROVE1B) the room the camera shows is behind it
    if (int r = Crowded(sc, s, clear, loose ? 8.0f : 6.0f); r >= 0) return Rejected((Reject)r);
    CVector body = BodyOf(s);
    float d = (body - sc.cj).Length2D();
    Body b;
    float height = PoseHeight(s.pose);
    bool projected = Project(body, height, b), shown = projected && OnScreen(sc, b);
    if (loose ? shown && b.depth < 1.5f : !projected || !InFrame(sc, b)) return Rejected(R_FRAME);
    if (int r = shown ? InPeoplesWay(sc, b, loose) : -1; r >= 0) return Rejected((Reject)r);
    if (int r = NoRoom(sc, s); r >= 0) return Rejected((Reject)r);
    body.z = s.ground.z;
    CVector head = body + CVector{0, 0, height - 0.15f}, chest = body + CVector{0, 0, height * 0.6f};
    if (!loose && (!GridSeen(sc.cam, head) || !GridSeen(sc.cam, chest) || !Seen(sc.cam, head) || !Seen(sc.cam, chest)))
        return Rejected(R_HIDDEN);
    // the rest of the shot: in the picture most of the time, seldom cut by its edge
    Later later = RateLater(sc, s, body, loose);
    int n = 1 + later.in + later.edge + later.out;
    if (later.bad || (!loose && ((1 + later.in) * 10 < n * 6 || later.edge * 4 > n))) return Rejected(R_LATER);
    // a loose place still in sight for a good part of the shot: one behind a wall or out at the frame's edge was taken
    // as well, and he stood unseen while the camera had room for him
    float seen = loose ? SeenShare(sc, s) : 1.0f;
    if (seen < 0.3f) return Rejected(R_HIDDEN);
    // to one side, as an extra: a third of the way in from an edge
    float score = PoseBonus(s.pose) - std::fabs(d - 2.2f) * 0.5f + std::min(clear, 2.5f) * 0.3f -
                  (shown ? std::fabs(std::fabs(b.x / sc.w - 0.5f) - 0.25f) * 0.8f : 0.4f) + 0.5f * (float)(1 + later.in) / (float)n;
    if (shown && b.bottom - b.top < sc.h * 0.12f) score -= 1.0f;  // too small to notice
    if (g_haveSpot && g_spot.pose == s.pose && (g_spot.ground - s.ground).Length2D() < 0.3f) score += 2.5f;  // stays put
    if (loose) score += seen;  // the more of the shot he is seen, the better
    return score;
}

struct Tried {  // for the review map
    Spot spot;
    float score;
    int reject;
};
std::vector<Tried> g_tried;

struct Best {
    Spot spot;
    float score = kRejected;
    bool loose = false;  // Rate's loose rules
    void Offer(const Scene& sc, Spot s) {
        float r = Rate(sc, s, loose);
        g_tried.push_back({s, r, g_lastReject});
        if (r > score) spot = s, score = r;
    }
};

// seats and steps: the ped attractors of benches, chairs and stairs around CJ
void OfferAttractors(const Scene& sc, Best& best) {
    CEntity* ents[96];
    int16_t n = 0;
    Call<0x564A20>(&sc.cj, 9.0f, false, &n, (int16_t)96, ents, true, false, false, true, true);  // CWorld::FindObjectsInRange
    for (int i = 0; i < n; ++i) {
        CEntity* e = ents[i];
        void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)e->ModelIndex() * 4);
        if (!mi) continue;
        int count = Field<uint8_t>(mi, 0x0D);  // m_nNum2dEffects
        for (int k = 0; k < count; ++k) {
            auto* fx = CallMethod<0x4C4C70, uint8_t*>(mi, k);  // CBaseModelInfo::Get2dEffect
            if (!fx || fx[0x0C] != 3) continue;                  // ped attractor
            int8_t type = (int8_t)fx[0x34];
            if (type >= 0 && type < 16) ++g_attractors[type];
            if (type != kAttractorSeat && type != kAttractorStep) continue;
            Spot s;
            s.pose = type == kAttractorSeat ? POSE_SIT : POSE_STEP;
            s.ground = Transform(e, *(CVector*)fx, true);
            s.heading = HeadingOf(Flat(Transform(e, *(CVector*)(fx + 0x1C), false)));  // m_vecUseDir, as the engine seats peds
            float z;
            if (!Floor(s.ground.x, s.ground.y, s.ground.z + 0.3f, z) || std::fabs(z - sc.floor) > 0.6f) continue;
            s.ground.z = z;
            best.Offer(sc, s);
        }
    }
}

// Chairs, sofas and benches as they are drawn, the cutscene's props among them: a flat seat at sitting height,
// a backrest behind it and room for the legs in front.
constexpr float kSeatHeight = 0.4f;  // the seat SEAT_idle is made for: its hips are 0.46 m over the feet
void OfferSeats(const Scene& sc, Best& best) {
    const Act* sit = Pick(kSit, 0);
    for (float dy = -4.6f; dy <= 4.6f; dy += 0.2f)
        for (float dx = -4.6f; dx <= 4.6f; dx += 0.2f) {
            CVector p{sc.cj.x + dx, sc.cj.y + dy, sc.floor};
            float h = g_grid.Seat(p.x, p.y);
            if (h < 0.35f || h > 0.62f || std::fabs(g_grid.Top(p.x, p.y) - h) > 0.03f) continue;  // nothing over the seat
            bool wide = true;  // a seat, not the edge of something
            for (const CVector& o : {CVector{0.15f, 0, 0}, CVector{-0.15f, 0, 0}, CVector{0, 0.15f, 0}, CVector{0, -0.15f, 0}})
                wide = wide && std::fabs(g_grid.Seat(p.x + o.x, p.y + o.y) - h) < 0.06f;
            if (!wide) continue;
            for (int k = 0; k < 8; ++k) {
                CVector back{std::cos(k * 0.7854f), std::sin(k * 0.7854f), 0};
                float b = g_grid.Hit(p, back, 0.4f, h + 0.25f);  // the backrest, the probe where the hips go
                if (b < 0.15f) continue;
                CVector face = back * -1.0f;
                float edge = -1;  // the front of the seat: deep sofas too, a bed or a table has none
                for (int e = 1; e <= 16 && edge < 0; ++e)
                    if (g_grid.Top(p.x + face.x * e * 0.05f, p.y + face.y * e * 0.05f) < 0.3f) edge = e * 0.05f;
                if (edge < 0) continue;
                // the hips 20 cm off the backrest, the thighs over the front edge, the feet where the animation wants them
                CVector hips = p + back * b + face * 0.2f, feet = hips + face * sit->back;
                if (edge + b - 0.2f > 0.5f || std::fabs(g_grid.Seat(hips.x, hips.y) - h) > 0.06f) continue;
                if (g_grid.Top(feet.x, feet.y) > 0.3f) continue;  // a table or a wall where the legs go
                feet.z = sc.floor + std::clamp(h - kSeatHeight, -0.1f, 0.15f);
                best.Offer(sc, Spot{feet, HeadingOf(face), POSE_SIT, 0});
            }
        }
}

// A wall right behind p: the back against it, the face out of it. Two points 35 cm apart give its direction.
void OfferLean(const Scene& sc, const CVector& p, const CVector& face, Best& best) {
    CVector back = face * -1.0f, side{-back.y, back.x, 0};
    float d0 = g_grid.Hit(p, back, 0.9f, 1.6f);
    if (d0 < 0) return;
    CVector a = p + side * 0.35f, b = p - side * 0.35f;
    float da = g_grid.Hit(a, back, d0 + 0.4f, 1.6f), db = g_grid.Hit(b, back, d0 + 0.4f, 1.6f);
    if (da < 0 || db < 0) return;  // a post or a lamp, not a wall
    CVector wa = a + back * da, wb = b + back * db;
    CVector along = Flat(wb - wa), n{-along.y, along.x, 0};
    if (n.Dot(face) < 0) n = n * -1.0f;
    if (n.Dot(face) < 0.8f) return;  // the wall turns away here
    // the back 9 cm off the wall: the ped's position as far out as its animation moves the body back
    uint8_t act = (uint8_t)((g_seed / 4) % 2);
    CVector spot = (wa + wb) * 0.5f + n * (Pick(kLean, act)->back + 0.09f);
    spot.z = sc.floor;
    best.Offer(sc, Spot{spot, HeadingOf(n), POSE_LEAN, act});
}

// Free floor: around CJ, and where the camera looks (in a close-up only the background is in the picture).
void OfferFloor(const Scene& sc, Best& best) {
    std::vector<CVector> places;
    for (float r : {1.4f, 1.9f, 2.5f, 3.2f, 4.0f, 5.0f})
        for (int a = 0; a < 24; ++a) {
            float ang = (float)a * (6.2831853f / 24.0f);
            places.push_back(sc.cj + CVector{std::cos(ang) * r, std::sin(ang) * r, 0.0f});
        }
    CVector view = Flat(sc.camFwd);
    for (int k = -12; k <= 12; ++k)
        for (float d = 1.5f; d <= 12.0f; d += 0.5f) places.push_back(sc.cam + Turn(view, k * 0.0873f) * d);
    for (CVector p : places) {
        float d = (p - sc.cj).Length2D();
        if (d < 1.3f || d > (best.loose ? 8.0f : 6.0f)) continue;
        p.z = sc.floor;
        // faces CJ, turned a little to the camera like an extra on a film set
        CVector face = Flat(Flat(sc.cj - p) + Flat(sc.cam - p) * 0.6f);
        best.Offer(sc, Spot{p, HeadingOf(face), POSE_STAND, (uint8_t)(g_seed % 4)});
        OfferLean(sc, p, face, best);
    }
}

// Any part of the partner at the spot shows in this shot, now or later (a shoulder at the edge counts). With walls
// off, all that is in the frame counts, behind a wall too, with room for the arms: the drawn walls are only as
// tall as their highest point, so a porch roof hides what stands under it.
bool Shows(const Scene& sc, const Spot& s, bool walls = true) {
    Body b;
    float height = PoseHeight(s.pose);
    CVector body = BodyOf(s), chest = body + CVector{0, 0, height * 0.6f};
    auto in = [&](Body b, const CVector& from) {
        if (!walls) b.half *= 1.5f;
        return OnScreen(sc, b) && (!walls || GridSeen(from, chest));
    };
    if (Project(body, height, b) && in(b, sc.cam)) return true;
    for (const Sample& m : g_ahead)
        if (ProjectAt(m.view, body, height, b) && in(b, m.view.pos)) return true;
    return false;
}

// Places chosen by hand for a cutscene (coop\cutscenes.txt): "NAME x y z heading pose [act]" (heading in degrees,
// pose stand, lean, sit or steps) or "NAME hidden" (the partner keeps out of the whole cutscene). A cutscene with
// places of its own uses only them, still checked shot by shot.
struct Places {
    bool hidden = false;
    std::vector<Spot> spots;
};
const Places* PlacesOf(const char* cutscene) {
    static std::map<std::string, Places> table;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        FILE* f = _wfopen((Cfg().coopDir + L"cutscenes.txt").c_str(), L"r");
        char line[256], name[16], pose[16];
        for (int act = 0; f && fgets(line, sizeof line, f);) {
            Spot sp;
            float deg = 0;
            int n = sscanf(line, "%15s %f %f %f %f %15s %d", name, &sp.ground.x, &sp.ground.y, &sp.ground.z, &deg, pose, &act);
            if (n < 1 || name[0] == '#') continue;
            for (char* c = name; *c; ++c) *c = (char)toupper((uint8_t)*c);
            Places& pl = table[name];
            if (strstr(line, " hidden")) {
                pl.hidden = true;
                continue;
            }
            if (n < 6) continue;
            static const char* names[] = {"stand", "lean", "sit", "steps"};
            int k = 0;
            while (k < 4 && strcmp(pose, names[k])) ++k;
            if (k == 4) continue;
            sp.pose = (Pose)k;
            sp.act = (uint8_t)(n == 7 ? act : 0);
            sp.heading = deg * 0.0174533f;
            pl.spots.push_back(sp);
        }
        if (f) fclose(f);
        LOGI("Cutscene places by hand: %zu cutscenes", table.size());
    }
    auto it = table.find(cutscene);
    return it == table.end() ? nullptr : &it->second;
}

// May the partner stay where an earlier shot put him? People do not vanish between two shots: this one may show him
// anywhere, cut by the frame's edge or small at the back, as long as he covers nobody, keeps out of the scene's way
// and nobody walks into him, and the scene is still where he is. CJ walking off across the room is no reason to go
// (it was within 9 m: he went after CJ from shot to shot, 28.10: "нап не должен перемещаться из кадра в кадр"); CJ on
// another floor or far away is.
const char* g_staysNo = "";  // why he did not stay (logs)
bool Stays(const Scene& sc, const Spot& s) {
    bool seated = s.pose == POSE_SIT || s.pose == POSE_STEP;
    CVector body = BodyOf(s);
    float d = (body - sc.cj).Length2D();
    if (d < 1.0f) return g_staysNo = "CJ at him", false;
    if (d > 25.0f || std::fabs(s.ground.z - sc.floor) > 1.0f) return g_staysNo = "the scene went elsewhere", false;
    for (const Actor& a : sc.actors)
        if ((a.feet - body).Length2D() < (seated ? 0.55f : 0.75f)) return g_staysNo = "a person on his spot", false;
    for (const Car& c : sc.cars)
        if (c.Covers(body, c.pos, 0.6f)) return g_staysNo = "a car on his spot", false;
    if (AtCamera(sc.cam, body, PoseHeight(s.pose))) return g_staysNo = "the camera at him", false;
    Body b;
    if (Project(body, PoseHeight(s.pose), b) && OnScreen(sc, b) && (b.depth < 1.5f || InPeoplesWay(sc, b, true) >= 0))
        return g_staysNo = "in front of the people", false;
    if (RateLater(sc, s, body, true).bad) return g_staysNo = "somebody walks into him later", false;
    return true;
}

// No place in the picture and none to stay at: he stands by CJ where this shot does not look, so that a later shot
// looking that way finds him there (Stays) rather than him popping up.
bool OutOfPicture(const Scene& sc, Spot& out) {
    float best = kRejected, clear;
    for (float r : {1.9f, 2.5f, 3.2f, 1.4f, 4.0f})
        for (int a = 0; a < 24; ++a) {
            float ang = (float)a * (6.2831853f / 24.0f), score = -std::fabs(r - 2.2f);
            Spot s{sc.cj + CVector{std::cos(ang) * r, std::sin(ang) * r, 0.0f}, 0, POSE_STAND, (uint8_t)(g_seed % 4)};
            s.ground.z = sc.floor;
            s.heading = HeadingOf(Flat(sc.cj - s.ground));
            if (score <= best || Crowded(sc, s, clear) >= 0 || Shows(sc, s, false) || NoRoom(sc, s) >= 0 ||
                RateLater(sc, s, BodyOf(s)).bad)
                continue;
            best = score, out = s;
        }
    return best > kRejected;
}

// The last place before none: next to CJ on his floor, where nobody of the scene stands and not at the camera, with room
// for him if the floor is known, in the picture if it can be. The first seconds of an interior had no floor yet (its
// buildings still streaming in): every place was refused, and he was hidden (SWEET3A, SWEET6A, INTRO1A).
bool Beside(const Scene& sc, Spot& out) {
    float best = kRejected, clear;
    for (float r : {1.6f, 2.2f, 1.3f, 2.8f})
        for (int a = 0; a < 16; ++a) {
            float ang = (float)a * (6.2831853f / 16.0f);
            Spot s{sc.cj + CVector{std::cos(ang) * r, std::sin(ang) * r, 0.0f}, 0, POSE_STAND, (uint8_t)(g_seed % 4)};
            s.ground.z = sc.floor;
            s.heading = HeadingOf(Flat(sc.cj - s.ground));
            if (Crowded(sc, s, clear) >= 0) continue;
            Spot t = s;
            bool room = NoRoom(sc, t) < 0;
            float score = -std::fabs(r - 1.8f) + (room ? 2.0f : 0.0f) + (Shows(sc, room ? t : s) ? 1.0f : 0.0f);
            if (score > best) best = score, out = room ? t : s;
        }
    return best > kRejected;
}

// A bone's world position from the entity's skin (cutscene people and peds alike): BONE_PELVIS 2
bool BoneAt(CEntity* e, int bone, CVector& out) {
    auto* clump = e ? (uint8_t*)e->RwObject() : nullptr;
    void* hier = clump && clump[0] == 2 ? Call<0x734A40, void*>(clump) : nullptr;  // GetAnimHierarchyFromSkinClump
    auto* m = hier ? Call<0x7C5120, uint8_t*>(hier) : nullptr;                     // RpHAnimHierarchyGetMatrixArray
    int i = m ? Call<0x7C51A0, int>(hier, bone) : -1;                            // RpHAnimIDGetIndex
    if (i < 0) return false;
    out = *(CVector*)(m + (size_t)i * 0x40 + 0x30);
    return true;
}

// One of the scene's people's hips, else its root. The skeleton is updated only while it is drawn: one not drawn since
// the last shot is where it was then, metres from the car it rides in now.
CVector HipsOf(CEntity* e) {
    CVector hips;
    return BoneAt(e, 2, hips) && (hips - e->GetPosition()).Length() < 1.5f ? hips : e->GetPosition();
}

// The game's vehicle a car of the cutscene stands for: the cutscene loads it as a model of its own, named "cs" and the
// vehicle's ("csgreenwood" greenwoo, "cscopcarla92" copcarla, "cszr350b" zr350), without the vehicle's seat dummies.
// -1 for anything else: any big prop counted as a car (the palms of CAS_3, the yard of STEAL_5), CJ by it rode, and the
// partner sat in the air.
int VehicleOf(CEntity* e) {
    int model = e->ModelIndex();
    if (model >= 400 && model <= 611) return model;
    for (int i = 0; i < std::clamp(Ref<int32_t>(0xBC3FE8), 0, 50); ++i) {  // CCutsceneMgr::ms_numLoadObjectNames
        if (Ref<int32_t>(0xBC31C0 + i * 4) != model) continue;             // ms_iModelIndex
        const char* name = (const char*)(0xBC38C8 + i * 32);               // ms_cLoadObjectName
        if (_strnicmp(name, "cs", 2) != 0) return -1;
        std::string n(name + 2, strnlen(name + 2, 29));
        for (size_t len = n.size(); len >= 4; --len) {
            int id = -1;
            if (Call<0x4C5940, void*>(n.substr(0, len).c_str(), &id) && id >= 400 && id <= 611) return id;  // CModelInfo::GetModelInfo
        }
        return -1;
    }
    return -1;
}

// The seats of that vehicle (its driver's too): cars and monster trucks only, as many as its doors give
// (CVehicleModelInfo::GetMaximumNumberOfPassengersFromNumberOfDoors). 0 for anything nobody sits in like that. The doors
// are counted as the vehicle's model loads (none known for Greenwood, Glendale, Washington and Bravura in their
// cutscenes): loaded for it once, then let go.
int SeatsOf(int vehicle) {
    static std::map<int, int> known;
    if (vehicle < 0) return 0;
    if (auto it = known.find(vehicle); it != known.end()) return it->second;
    void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)vehicle * 4);
    int type = mi ? Field<int32_t>(mi, 0x3C) : -1;  // m_nVehicleType: 0 car, 1 monster truck (bikes, boats, aircraft: no)
    if (type != 0 && type != 1) return known[vehicle] = 0;
    bool had = IsModelLoaded(vehicle);
    if (!had) RequestModel(vehicle, 2 | 0x10), LoadAllRequestedModels(true);  // GAME_REQUIRED | PRIORITY_REQUEST
    if (!IsModelLoaded(vehicle)) return 0;  // the next shot tries again
    int seats = Field<uint8_t>(mi, 0x4C) ? 1 + Call<0x4C89B0, int>(vehicle) : 0;  // m_nNumDoors (none: seat dummies are read)
    if (!had) MarkModelNotNeeded(vehicle);
    return known[vehicle] = seats;
}

// The room inside a car of the scene, in its own space: what is drawn of it less a margin. Hips there sit in it,
// not by a door, at the boot or on the bonnet (CJ behind the car in BCESA5W rode, the partner in the air behind it).
struct Cabin {
    const CMatrix* m;
    CVector lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    float roof0 = 1e9f, roof1 = -1e9f;  // the roof's length: what is drawn within 15 cm of the top
    CVector Local(const CVector& w) const {
        CVector d = w - m->pos;
        return {d.Dot(m->right), d.Dot(m->forward), d.Dot(m->up)};
    }
    CVector World(const CVector& l) const { return m->pos + m->right * l.x + m->forward * l.y + m->up * l.z; }
    bool Holds(const CVector& p) const {
        return p.x > lo.x + 0.3f && p.x < hi.x - 0.3f && p.y > lo.y + 0.6f && p.y < hi.y - 0.6f && p.z > lo.z && p.z < hi.z;
    }
};
void ExtendCabin(const CVector& a, const CVector& b, const CVector& c, void* user) {
    auto* r = (Cabin*)user;
    for (const CVector* v : {&a, &b, &c}) {
        CVector l = r->Local(*v);
        r->lo = {std::min(r->lo.x, l.x), std::min(r->lo.y, l.y), std::min(r->lo.z, l.z)};
        r->hi = {std::max(r->hi.x, l.x), std::max(r->hi.y, l.y), std::max(r->hi.z, l.z)};
    }
}
void ExtendRoof(const CVector& a, const CVector& b, const CVector& c, void* user) {
    auto* r = (Cabin*)user;
    for (const CVector* v : {&a, &b, &c})
        if (CVector l = r->Local(*v); l.z > r->hi.z - 0.15f) r->roof0 = std::min(r->roof0, l.y), r->roof1 = std::max(r->roof1, l.y);
}

struct Ride {
    bool seated = false;  // CJ sits in a car of the scene
    bool drives = false;  // and it drives off within the shot
};

// CJ rides in a car of the scene: the partner rides along in a seat nobody of the scene sits in, the other one of CJ's
// row first. Hips within a seat's width (0.55 m) of a seat take it. As many in it as the car has seats (four in the
// four-door car of SWEET2B): no ride, he sat in somebody.
bool InCar(const Scene& sc, Spot& out, Ride& ride) {
    CEntity* cjEntity = CutsceneCj();
    for (size_t k = 0; k < sc.cars.size(); ++k) {
        const Car& c = sc.cars[k];
        CEntity* e = c.index >= 0 ? CutsceneObject(c.index) : nullptr;
        if (!e || !e->Matrix() || !cjEntity || !c.Covers(sc.cj, c.pos, 0.0f)) continue;
        int vehicle = VehicleOf(e), seats = SeatsOf(vehicle);
        if (!seats) {
            LOGD("Cutscene object %d (model %d) is no car with seats (vehicle %d)", c.index, (int)e->ModelIndex(), vehicle);
            continue;
        }
        Cabin cab{e->Matrix()};
        ForEachTriangle(e, &ExtendCabin, &cab);
        ForEachTriangle(e, &ExtendRoof, &cab);
        // the hips, not the root: CJ's root wanders as he turns in his seat to talk
        CVector cj = cab.Local(HipsOf(cjEntity));
        if (!cab.Holds(cj)) {
            LOGD("Cutscene car %d: CJ by it, not in it (%.2f,%.2f,%.2f of %.2f..%.2f, %.2f..%.2f)", vehicle, cj.x, cj.y, cj.z,
                 cab.lo.x, cab.hi.x, cab.lo.y, cab.hi.y);
            continue;
        }
        ride.seated = true;
        for (const Sample& s : g_ahead) ride.drives = ride.drives || (s.cars[k] - c.pos).Length2D() > 2.0f;
        std::vector<CVector> people{cj};
        for (const Actor& a : sc.actors)
            if (CVector p = cab.Local(HipsOf(a.e)); cab.Holds(p) && std::fabs(p.z - cj.z) < 1.0f) people.push_back(p);
        LOGD("Cutscene car %d: %d seats, %d people in it", vehicle, seats, (int)people.size());
        if ((int)people.size() >= seats) return false;
        auto taken = [&](const CVector& s) {
            for (const CVector& p : people)
                if (std::fabs(p.x - s.x) < 0.55f && std::fabs(p.y - s.y) < 0.6f) return true;
            return false;
        };
        // a seat the camera is in or right by over the shot: his shoulder across the picture (SYND_3A's close-ups of CJ
        // from the back seat)
        auto atCamera = [&](const CVector& seat) {
            CVector hips = cab.World(seat), head = hips + e->Matrix()->up * 0.75f;
            auto near = [&](const CVector& cam, const CVector& moved) {
                CVector a = hips + moved, ab = head - hips;
                float t = std::clamp((cam - a).Dot(ab) / ab.Dot(ab), 0.0f, 1.0f);
                return (cam - (a + ab * t)).Length() < 0.9f;
            };
            if (near(sc.cam, {})) return true;
            for (const Sample& s : g_ahead)
                if (near(s.view.pos, s.cars[k] - c.pos)) return true;
            return false;
        };
        // a seat between the camera and CJ or someone of the scene: the shot looks at CJ through the empty seat by him
        // (SYND_3A, through the passenger's window), and the partner there hid CJ
        auto hidesSomeone = [&](const CVector& seat) {
            CVector feet = cab.World(seat) - e->Matrix()->up * 0.45f;
            Body b, p;
            auto hides = [&](const Body& who) { return b.depth < who.depth - 0.2f && Covered(who, b) > 0.3f; };
            if (Project(feet, PoseHeight(POSE_SIT), b) && OnScreen(sc, b)) {
                if (sc.cjOnScreen && hides(sc.cjBody)) return true;
                for (const Actor& a : sc.actors)
                    if (a.onScreen && hides(a.body)) return true;
            }
            for (const Sample& m : g_ahead) {
                if (!ProjectAt(m.view, feet + (m.cars[k] - c.pos), PoseHeight(POSE_SIT), b) || !OnScreen(sc, b)) continue;
                if (PersonAt(m.view, m.cj, p) && OnScreen(sc, p) && hides(p)) return true;
                for (const CVector& a : m.actors)
                    if (PersonAt(m.view, a, p) && OnScreen(sc, p) && hides(p)) return true;
            }
            return false;
        };
        auto bad = [&](const CVector& seat) { return atCamera(seat) || hidesSomeone(seat); };
        // the car's front (the models face either way, and on the model's own heading he sat facing the boot): the way
        // CJ in it faces, across his shoulders (bones 21 and 31), else where the drawn car reaches further beyond the
        // roof (a bonnet is longer than a boot)
        float front = cab.roof0 - cab.lo.y > cab.hi.y - cab.roof1 ? 3.14159265f : 0.0f;
        CVector lc, rc, root = cjEntity->GetPosition();
        if (BoneAt(cjEntity, 31, lc) && BoneAt(cjEntity, 21, rc) && (lc - root).Length() < 1.5f && (rc - root).Length() < 1.5f)
            if (float f = e->Matrix()->up.Cross(rc - lc).Dot(e->Matrix()->forward); std::fabs(f) > 0.1f)
                front = f < 0.0f ? 3.14159265f : 0.0f;
        // the seat he has in this car keeps for the scene (the next shot's CJ sits a little otherwise), until someone
        // of the scene sits there
        bool kept = g_haveSeat && g_seat.act == c.index && cab.Holds(g_seat.ground) && !taken(g_seat.ground);
        if (kept && !bad(g_seat.ground)) return out = g_seat, true;
        // the other seat of CJ's row, then (four seats and more) the other row. Both rows are under the roof, so the
        // other one is towards the roof's middle from CJ, whichever way the model faces (cutscene cars face either way,
        // and a bonnet is longer than a boot: a "row" taken a metre off the wrong way was on the bonnet)
        float side = std::max(std::fabs(cj.x), 0.4f), dy = cj.y > (cab.roof0 + cab.roof1) * 0.5f ? -1.1f : 1.1f;
        std::vector<CVector> tries{{-cj.x, cj.y, cj.z}};
        if (seats > 2)
            for (float x : {-side, side}) tries.push_back({x, cj.y + dy, cj.z});
        // the seat he has keeps (one of a new shot's camera would not do: out of that shot rather than to another seat)
        bool free = kept;
        for (const CVector& s : tries) {
            if (kept || !cab.Holds(s) || taken(s)) continue;
            free = true;
            if (!bad(s)) return g_haveSeat = true, out = g_seat = Spot{s, front, POSE_CAR, (uint8_t)c.index}, true;
        }
        // a free seat only where the camera is or looks through: out of this shot rather than in its way (no seat at all:
        // no ride)
        if (free) return out = Spot{{sc.cj.x, sc.cj.y, sc.floor}, 0.0f, POSE_HIDDEN, 0}, true;  // on the floor, not in the air
        return false;
    }
    return false;
}

// ---- walking with CJ. A shot where CJ walks takes the partner along, a couple of metres behind him and to one
// side, on the path of CJ's own root track (CJ went there: there is floor and room), with CJ's walk from the
// cutscenes. Both games work the path out from their own CJ, so they agree.
constexpr float kWalkGap = 1.7f;    // behind CJ along his path
constexpr float kWalkSide = 0.75f;  // and to the side of it
constexpr float kPathStep = 0.05f;  // s between the samples of CJ's track
constexpr float kPathJump = 0.5f;   // m in one step: the cutscene put CJ somewhere else (a cut), no walk

struct Path {
    const uint8_t* seq = nullptr;  // CJ's root sequence it was read from
    std::vector<CVector> at;       // the root's translation every kPathStep s of the animation
} g_path;

// CJ's root track as samples, read once per cutscene (the key frames hold time steps, as RootAt reads them)
const std::vector<CVector>& PathOf(const Track& tr) {
    if (g_path.seq == tr.seq) return g_path.at;
    g_path.seq = tr.seq;
    g_path.at.clear();
    int frames = Field<uint16_t>(tr.seq, 0x06);
    auto* f = Field<const uint8_t*>(tr.seq, 0x08);
    bool packed = (Field<uint16_t>(tr.seq, 0x04) & 4) != 0;
    CVector prev{};
    float at = 0, prevAt = 0;
    for (int i = 0; f && i < frames; ++i) {
        CVector p;
        if (packed) {
            auto* k = (const int16_t*)(f + i * 16);
            at += k[4] / 60.0f;
            p = {k[5] / 1024.0f, k[6] / 1024.0f, k[7] / 1024.0f};
        } else {
            auto* k = (const float*)(f + i * 32);
            at += k[4];
            p = {k[5], k[6], k[7]};
        }
        if (i == 0) prev = p, prevAt = at;
        for (float t; (t = (float)g_path.at.size() * kPathStep) <= at;)
            g_path.at.push_back(at > prevAt ? prev + (p - prev) * std::clamp((t - prevAt) / (at - prevAt), 0.0f, 1.0f) : p);
        prev = p, prevAt = at;
    }
    return g_path.at;
}

// CJ's feet at the animation's time t, from where they are at its time now (the track is relative)
CVector CjAt(const std::vector<CVector>& path, float now, const CVector& feetNow, float t) {
    auto root = [&](float s) {
        float x = std::clamp(s / kPathStep, 0.0f, (float)(path.size() - 1));
        size_t i = (size_t)x;
        return i + 1 < path.size() ? path[i] + (path[i + 1] - path[i]) * (x - (float)i) : path[i];
    };
    return feetNow + (root(t) - root(now));
}
CVector CjFeet(CEntity* cj) {  // as BuildScene finds the floor under CJ
    CVector p = cj->GetPosition();
    float z;
    p.z = Floor(p.x, p.y, p.z + 0.2f, z) && p.z - z < 2.0f ? z : p.z - 1.0f;
    return p;
}

// The partner walking with CJ at the animation's time t: kWalkGap back along CJ's path from where CJ is then
// (behind where CJ started, if CJ has not come that far), kWalkSide to one side (-1 left, 1 right, 0 right behind).
CVector WalkPoint(const std::vector<CVector>& path, float now, const CVector& feetNow, const CVector& cjFwd, float t, int side) {
    CVector cj = CjAt(path, now, feetNow, t), p = cj;
    float left = kWalkGap, s = t;
    for (int k = 0; k < 600 && left > 0 && s > 0; ++k) {  // 30 s back at most
        s = std::max(0.0f, s - kPathStep);
        CVector q = CjAt(path, now, feetNow, s);
        float d = (q - p).Length2D();
        if (d > kPathJump) break;  // CJ came here by a cut: behind his start here
        if (d >= left) p = p + (q - p) * (left / d), left = 0;
        else p = q, left -= d;
    }
    CVector way = cj - p;  // the way CJ went from here
    way = way.Length2D() > 0.05f ? Flat(way) : Flat(cjFwd);
    p = p - way * left;
    return p + CVector{way.y, -way.x, 0} * ((float)side * kWalkSide);
}
struct Walker {
    CVector feet;
    float heading = 0, speed = 0;  // m/s of the animation's time
};
Walker WalkerAt(const std::vector<CVector>& path, float now, const CVector& feetNow, const CVector& cjFwd, float t, int side) {
    Walker w;
    CVector a = WalkPoint(path, now, feetNow, cjFwd, t, side), way = a - WalkPoint(path, now, feetNow, cjFwd, t - 0.15f, side);
    if (way.Length2D() > kPathJump) way = WalkPoint(path, now, feetNow, cjFwd, t + 0.15f, side) - a;  // a cut just before
    w.feet = a;
    w.speed = way.Length2D() / 0.15f;
    w.heading = HeadingOf(w.speed > 0.3f ? Flat(way) : Flat(CjAt(path, now, feetNow, t) - a));  // standing: to CJ
    return w;
}

// CJ walks in this shot, or has just been walking at the cut
bool CjWalks(const Scene& sc, const Track& tr) {
    const auto& path = PathOf(tr);
    if (path.size() < 2) return false;
    CVector feet{sc.cj.x, sc.cj.y, sc.floor}, p = feet;
    float walked = 0;  // over the last second, not across a cut of his track
    for (float s = tr.time - kPathStep; s >= tr.time - 1.0f && s >= 0; s -= kPathStep) {
        CVector q = CjAt(path, tr.time, feet, s);
        if ((q - p).Length2D() > kPathJump) break;
        walked += (q - p).Length2D(), p = q;
    }
    for (const Sample& m : g_ahead) walked = std::max(walked, (m.cj - feet).Length2D());
    return walked >= 1.0f;
}

// The walk over the whole shot: room where he goes (the side step off CJ's path too), nobody of the scene or a
// car there, and in the picture nobody covered and no bigger than the people in it.
bool WalkFits(const Scene& sc, const Track& tr, int side) {
    const auto& path = PathOf(tr);
    CEntity* cj = CutsceneCj();
    if (path.size() < 2 || !cj || !cj->Matrix()) return false;
    CVector feet{sc.cj.x, sc.cj.y, sc.floor}, fwd = cj->Matrix()->forward;
    auto clear = [&](const Walker& w, const CVector& cjAt, const std::vector<CVector>& actors, const std::vector<CVector>* cars) {
        if (!g_grid.Clear(w.feet, 0.25f) || (w.feet - cjAt).Length2D() < 0.9f) return false;
        for (const CVector& a : actors)
            if ((a - w.feet).Length2D() < 0.6f) return false;
        for (size_t k = 0; k < sc.cars.size(); ++k)
            if (sc.cars[k].Covers(w.feet, cars ? (*cars)[k] : sc.cars[k].pos, 0.5f)) return false;
        return true;
    };
    std::vector<CVector> actors;
    for (const Actor& a : sc.actors) actors.push_back(a.feet);
    Walker w = WalkerAt(path, tr.time, feet, fwd, tr.time, side);
    Body b;
    if (!clear(w, feet, actors, nullptr) || AtCamera(sc.cam, w.feet, 1.8f)) return false;
    if (Project(w.feet, 1.8f, b) && OnScreen(sc, b) && (b.depth < 1.5f || InPeoplesWay(sc, b) >= 0)) return false;
    for (const Sample& m : g_ahead) {
        w = WalkerAt(path, tr.time, feet, fwd, tr.time + m.dt, side);
        if (!clear(w, m.cj, m.actors, &m.cars) || AtCamera(m.view.pos, w.feet, 1.8f)) return false;
        if (ProjectAt(m.view, w.feet, 1.8f, b) && OnScreen(sc, b) && (b.depth < 1.5f || InWayAt(sc, m, b, false))) return false;
    }
    return true;
}

const char* g_how = "";  // how the shot's spot came about (logs)
// The spot is out of the picture, no place in the scene (OutOfPicture, or kept from such): the next shot looks for one
// in the picture before it keeps him there. Chosen so at the first shot of "Cleaning the Hood" (SWEET1B), he stood out
// of the picture for all ten shots after it, seen 0% of them (28.10).
bool g_spotOut = false;

Spot Choose(const Scene& sc) {
    g_staysNo = "";
    const Places* own = PlacesOf(g_cutscene);
    Ride ride;
    if (!(own && own->hidden)) {  // CJ rides: in his car with him
        Spot seat;
        g_how = " (rides)";
        if (sc.cjInCar && InCar(sc, seat, ride)) return seat;
    }
    // no seat for him in CJ's car: he stands by it, or, if it drives off, where he was or out of the picture (a new
    // place by the kerb every shot along the road would show him there again and again). A seat of before is no place.
    bool stand = !(own && own->hidden), may = stand && !ride.drives;
    bool before = stand && g_haveSpot && g_spot.pose != POSE_HIDDEN && g_spot.pose != POSE_CAR;
    Track tr;
    bool track = TrackOf(CutsceneCj(), tr) && PathOf(tr).size() > 1;
    CEntity* cj = CutsceneCj();
    CVector cjFwd = cj && cj->Matrix() ? cj->Matrix()->forward : CVector{0, 1, 0};
    // where he is now: a walk has got on, it stands there if this shot is no walk
    Spot prev = g_spot;
    if (before && prev.pose == POSE_WALK && track) {
        Walker w = WalkerAt(PathOf(tr), tr.time, CVector{sc.cj.x, sc.cj.y, sc.floor}, cjFwd, tr.time, (int)prev.act - 1);
        prev = Spot{w.feet, w.heading, POSE_STAND, 0};  // the clips, not a cigarette out of nowhere
    }
    // CJ walks: the partner goes along, on his side of CJ if he was walking already, unless that is a jump the
    // shot shows
    if (may && !(own && !own->spots.empty()) && track && MocapWalkCount() && MocapCount() > 1 && CjWalks(sc, tr)) {
        int first = before && g_spot.pose == POSE_WALK ? (int)g_spot.act - 1 : 1;
        for (int side : {first, first ? -first : 1, first ? 0 : -1}) {
            if (!WalkFits(sc, tr, side)) continue;
            Walker w = WalkerAt(PathOf(tr), tr.time, CVector{sc.cj.x, sc.cj.y, sc.floor}, cjFwd, tr.time, side);
            if (before && (prev.ground - w.feet).Length2D() > 1.0f && Shows(sc, prev, false)) break;
            g_how = " (walks)";
            return Spot{w.feet, w.heading, POSE_WALK, (uint8_t)(side + 1)};
        }
    }
    // A shot keeps him where he stood whenever it can, seen in it or not. Kept only where half of the next shot showed
    // him, he went where each shot did: from the wall to Sweet and on to the back of the room at every cut ("второй
    // Сиджей постоянно телепортируется", 28.10; the players chose one place for the whole scene).
    // Only where the shot shows him at all: kept where this shot never does, he was out of the picture for most of a
    // scene (29.10, SWEET1B: 8 of 12 shots unseen at the place he stayed at), and a place in the picture comes first.
    // Moved only then, he does not jump about in front of the camera: where he stood is not in this shot.
    g_how = " (stays)";
    if (before && !g_spotOut && Stays(sc, prev) && SeenShare(sc, prev) > 0.0f) return prev;
    auto offer = [&](Best& to) {
        if (own && !own->spots.empty()) {
            for (const Spot& sp : own->spots) to.Offer(sc, sp);
        } else {
            OfferAttractors(sc, to);
            OfferSeats(sc, to);
            OfferFloor(sc, to);
        }
    };
    Best best;
    if (may) offer(best);
    g_how = "";
    if (best.score > kRejected) return best.spot;
    // Nothing in the picture fits this shot well (a close-up, a cramped room): a looser place the camera still shows
    // first (cut by its edge, half behind something). Kept where he was or out of the picture before that, he was seen
    // 8% of the time in 13 hard cutscenes of the review (37% with this and the rules above).
    Best loose;
    loose.loose = true;
    if (may) offer(loose);
    g_how = " (loose)";
    if (loose.score > kRejected && Shows(sc, loose.spot, false)) return loose.spot;
    // then he stays where he was, out of the picture now, or stands by out of the picture (not at the places chosen by
    // hand: those alone)
    g_how = " (stays)";
    if (before && Stays(sc, prev)) return prev;
    Spot out;
    g_how = " (out of the picture)";
    if (stand && !(own && !own->spots.empty()) && OutOfPicture(sc, out)) return out;
    // the camera takes in all around CJ: in the picture anyway
    g_how = " (loose)";
    if (loose.score > kRejected) return loose.spot;
    // CJ in a car, or no room near him at all: where we were if the camera does not see that place, otherwise
    // out of the cutscene for this shot
    g_how = "";
    if (before && !Shows(sc, prev)) return prev;
    Spot near;
    g_how = " (beside)";
    if (may && !(own && !own->spots.empty()) && Beside(sc, near)) return near;
    g_how = "";
    Spot hidden = before ? prev : Spot{{sc.cj.x, sc.cj.y, sc.floor}, 0, POSE_STAND, 0};  // on the floor, not in the air
    hidden.pose = POSE_HIDDEN;
    return hidden;
}

bool BuildScene(Scene& sc) {
    CEntity* cj = CutsceneCj();
    CMatrix* cam = Ref<CMatrix*>(kCameraMatrix);
    if (!cj || !cj->Matrix() || !cam) return false;
    sc.cj = cj->GetPosition();
    sc.cam = cam->pos;
    sc.camFwd = cam->forward;
    sc.fov = cam::Fov();
    float z;
    sc.floor = Floor(sc.cj.x, sc.cj.y, sc.cj.z + 0.2f, z) && sc.cj.z - z < 2.0f ? z : sc.cj.z - 1.0f;
    sc.w = (float)ScreenWidth();
    sc.h = (float)ScreenHeight();
    g_now = View{sc.cam, cam->forward, cam->right, cam->up};
    g_nowOk = Calibrate(g_now);
    sc.cjOnScreen = ProjectSkeleton(cj, sc.cjBody) || ProjectActor(CVector{sc.cj.x, sc.cj.y, sc.floor}, sc.cjBody);
    for (int i = 0; i < NumCutsceneObjects(); ++i) {
        CEntity* o = CutsceneObject(i);
        // the people of the scene within 20 m (they may walk up), not those hidden until their part comes
        if (!o || o == cj || !(o->Flags() & 0x80) || (o->GetPosition() - sc.cj).Length() > 20.0f || !IsPerson(o)) continue;
        Actor a;
        a.e = o;
        a.feet = o->GetPosition();
        a.feet.z = sc.floor;
        a.onScreen = ProjectSkeleton(o, a.body) || ProjectActor(a.feet, a.body);
        sc.actors.push_back(a);
    }
    for (int i = 0; i < NumCutsceneObjects(); ++i) {
        CEntity* o = CutsceneObject(i);
        Car c{};
        if (!o || o == cj || !(o->Flags() & 0x80) || !IsCar(o, c.b)) continue;
        c.index = i;
        c.pos = o->GetPosition();
        TrackOf(o, c.track);
        sc.cjInCar = sc.cjInCar || c.Covers(sc.cj, c.pos, 0.0f);
        sc.cars.push_back(c);
    }
    return true;
}

// ---- who of the scene talks: the jaw moves against the head (bone 5) while a line is said. Cutscene faces have a
// rig of their own (bones 5001-5022, "jaw1" is 5015), other people's jaw is bone 8.
constexpr float kTalking = 0.05f;  // jaw movement (axes, fading over a third of a second): 0.1-0.5 saying a line, ~0 silent
struct Talk {
    float jaw[6] = {};   // the jaw's up and forward axes in the head's frame, last frame
    float activity = 0;  // how much it moved lately: fades in a third of a second
    CVector head;        // where the head is (world)
    bool primed = false, seen = false;
};
std::map<CEntity*, Talk> g_talk;
uint64_t g_talkAt = 0;

bool HeadAndJaw(CEntity* e, CVector& head, float jaw[6]) {
    auto* clump = (uint8_t*)e->RwObject();
    void* hier = clump && clump[0] == 2 ? Call<0x734A40, void*>(clump) : nullptr;  // GetAnimHierarchyFromSkinClump
    auto* m = hier ? Call<0x7C5120, uint8_t*>(hier) : nullptr;                     // RpHAnimHierarchyGetMatrixArray
    int h = m ? Call<0x7C51A0, int>(hier, 5) : -1, j = m ? Call<0x7C51A0, int>(hier, 5015) : -1;  // RpHAnimIDGetIndex
    if (h < 0) return false;
    if (j < 0) j = Call<0x7C51A0, int>(hier, 8);
    auto* H = (const float*)(m + (size_t)h * 0x40);  // RwMatrix rows: right, up, at, pos
    head = {H[12], H[13], H[14]};
    std::fill(jaw, jaw + 6, 0.0f);
    if (j < 0) return true;  // a face that never talks
    auto* J = (const float*)(m + (size_t)j * 0x40);
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 3; ++b)
            jaw[a * 3 + b] = J[(1 + a) * 4] * H[b * 4] + J[(1 + a) * 4 + 1] * H[b * 4 + 1] + J[(1 + a) * 4 + 2] * H[b * 4 + 2];
    return true;
}

// every drawn frame: the bones are where the frame shows them
void UpdateTalk() {
    uint64_t now = NowMs();
    float keep = std::exp(-std::clamp((float)(now - g_talkAt), 1.0f, 200.0f) / 350.0f);
    g_talkAt = now;
    for (auto& [e, t] : g_talk) t.seen = false;
    for (int i = 0; i < NumCutsceneObjects(); ++i) {
        CEntity* o = CutsceneObject(i);
        float jaw[6];
        CVector head;
        if (!o || !(o->Flags() & 0x80) || !IsPerson(o) || !HeadAndJaw(o, head, jaw)) continue;
        Talk& t = g_talk[o];
        if (!t.primed) LOGD("Cutscene person model %d: %s", o->ModelIndex(), std::count(jaw, jaw + 6, 0.0f) == 6 ? "no jaw" : "jaw");
        float moved = 0;
        for (int k = 0; k < 6; ++k) moved += std::fabs(jaw[k] - t.jaw[k]);
        t.activity = t.activity * keep + (t.primed ? std::min(moved, 0.1f) : 0.0f);  // a cut makes the jaw jump
        std::copy(jaw, jaw + 6, t.jaw);
        t.head = head;
        t.primed = t.seen = true;
    }
    for (auto it = g_talk.begin(); it != g_talk.end();) it = it->second.seen ? std::next(it) : g_talk.erase(it);
}

// ---- peds held in the scene
struct Held {
    int handle = -1;
    Spot spot;
    bool placed = false;
    int stage = 0;  // 0 loading the animation, 1 the cigarette is in the hand, 2 the loop plays, 3 a variation plays
    int prop = -1;  // the cigarette
    uint64_t checkedAt = 0;
    // the living extra: its draws come from the spot and the cutscene's clock, so both games show the same
    uint32_t seed = 0;
    uint32_t slot = ~0u, faceSlot = ~0u, turns = 0;
    float fidgetAt = -1, faceAt = -1;  // the cutscene clock (ms) when the drawn variation, the gum, begin
    float loopClock = -1;              // and when the loop first played at this spot
    uint8_t pick = 0;
    const Fidget* fidget = nullptr;  // the variation playing
    const char* clip = nullptr;      // the cutscene clip playing, and how many came before it at this spot
    uint32_t clips = 0;
    bool walking = false;            // walking with CJ: the walk plays (else the clips)
    const char* walk = nullptr;
    float walkSpeed = 1, heading = 0;
    uint64_t speedAt = 0;
    uint64_t fidgetSince = 0, lookedAt = 0, talkSeenAt = 0;
    CEntity* looking = nullptr;
    CEntity* lastTalker = nullptr;
    bool lookOn = false, wasTalking = false;
};
std::map<CPed*, Held> g_held;

// Whom the partner looks at: who talks in front of it, and a while after the line; otherwise CJ or the nearest one
// in front. Now and then a glance at someone else.
CEntity* Attention(Held& h, CPed* ped, uint64_t now, float clock) {
    CVector me = ped->GetPosition(), face = Forward(h.spot.heading);
    std::vector<CEntity*> front;
    for (auto& [e, t] : g_talk) {
        CVector d = t.head - me;
        float l = d.Length2D();
        if (l > 0.5f && l < 15.0f && (d.x * face.x + d.y * face.y) > -0.2f * l) front.push_back(e);  // ~100 degrees
    }
    if (front.empty()) return nullptr;
    std::sort(front.begin(), front.end(), [](CEntity* a, CEntity* b) {  // the same order in both games
        return a->ModelIndex() != b->ModelIndex() ? a->ModelIndex() < b->ModelIndex() : g_talk[a].head.x < g_talk[b].head.x;
    });
    CEntity* target = nullptr;
    float best = kTalking;
    for (CEntity* e : front)
        if (g_talk[e].activity > best) target = e, best = g_talk[e].activity;
    // the one listened to keeps the look while still talking, unless someone else is clearly louder
    bool held = h.lastTalker && h.lastTalker != target && std::count(front.begin(), front.end(), h.lastTalker);
    if (held && g_talk[h.lastTalker].activity > kTalking * 0.6f && best < g_talk[h.lastTalker].activity * 1.5f) target = h.lastTalker;
    if (target) {
        h.lastTalker = target, h.talkSeenAt = now;
    } else if (h.lastTalker && now - h.talkSeenAt < 2500 && std::count(front.begin(), front.end(), h.lastTalker)) {
        target = h.lastTalker;
    } else if (CEntity* cj = CutsceneCj(); cj && std::count(front.begin(), front.end(), cj)) {
        target = cj;
    } else {
        for (CEntity* e : front)
            if (!target || (g_talk[e].head - me).Length2D() < (g_talk[target].head - me).Length2D()) target = e;
    }
    if (clock >= 0 && front.size() > 1) {  // a glance of a second at someone else, in some of every 6 s
        uint32_t slot = (uint32_t)(clock / 6000.0f), r = Mix(h.seed ^ 0x2545F491u, slot);
        float at = slot * 6000.0f + 1500.0f + (float)((r >> 8) % 3000);
        if (r % 100 < 30 && clock >= at && clock < at + 1200.0f) {
            CEntity* other = front[(r >> 20) % front.size()];
            if (other != target) return other;
        }
    }
    return target;
}

void StartFidget(Held& h, const Fidget& f, uint64_t now) {
    if (!BlockReady(f.block)) return;
    Cmd(0x0605).I(h.handle).S(f.anim).S(f.block).F(4.0f).I(0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, once
    h.stage = 3, h.fidget = &f, h.fidgetSince = now;
    LOGD("Cutscene partner: %s/%s", f.block, f.anim);
}

// the clips act: the next one drawn from the spot (the same in both games), never the one that ends
void NextClip(Held& h, float blend) {
    size_t n = MocapCount();
    uint32_t r = Mix(h.seed, ++h.clips);
    const char* c = MocapClip(r % n);
    if (c == h.clip) c = MocapClip(r % n + 1);
    h.clip = c;
    // TASK_PLAY_ANIM, once, holding its last frame until the next one takes over
    Cmd(0x0605).I(h.handle).S(c).S(kMocapBlock).F(blend).I(0).I(0).I(0).I(1).I(-1).Run();
    h.checkedAt = NowMs();
    LOGD("Cutscene partner: clip %s", c);
}
// what the act plays now
const char* AnimNow(const Held& h, const Act* a) { return a->anim ? a->anim : h.clip ? h.clip : "-"; }

// Every frame while the loop or a variation plays: now and then a variation (the same moments in both games: the
// cutscene's clock), a nod when a line ends, the head to the people of the scene, a stick of gum.
void Live(CPed* ped, Held& h, const Act* a) {
    uint64_t now = NowMs();
    float clock = cam::Mode() == kModeFlyBy ? Ref<float>(cam::Active() + 0x90) : -1.0f;
    bool talking = h.lastTalker && g_talk.count(h.lastTalker) && g_talk[h.lastTalker].activity > kTalking;
    bool settled = h.loopClock >= 0 && clock > h.loopClock + 1500.0f;
    if (h.wasTalking && !talking && h.stage == 2 && settled && Mix(h.seed, ++h.turns) % 100 < 40)  // the line listened to ends
        for (size_t i = 0; i < a->fidgetCount; ++i)
            if (!strcmp(a->fidgets[i].anim, "Coplook_nod")) {
                StartFidget(h, a->fidgets[i], now);
                break;
            }
    h.wasTalking = talking;
    if (!a->anim) {  // the next clip blends in over the last quarter second of this one
        void* as = h.stage == 2 && h.clip && !h.walking ? Call<0x4D6870, void*>(ped->RwObject(), h.clip) : nullptr;  // RpAnimBlendClumpGetAssociation
        if (as && anim::CurrentTime(as) >= anim::TotalTime(anim::Hierarchy(as)) - 0.25f) NextClip(h, 4.0f);
    } else if (h.stage == 3) {  // back to the loop just before the variation ends
        void* as = Call<0x4D6870, void*>(ped->RwObject(), h.fidget->anim);  // RpAnimBlendClumpGetAssociation
        if (as ? anim::CurrentTime(as) >= anim::TotalTime(anim::Hierarchy(as)) - 0.3f : now - h.fidgetSince > 500) {
            Cmd(0x0605).I(h.handle).S(a->anim).S(a->block).F(4.0f).I(1).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, looped
            h.stage = 2, h.checkedAt = now, h.fidget = nullptr;
        }
    } else if (h.stage == 2 && a->fidgetCount && clock >= 0) {
        float phase = (float)(h.seed % 6000);
        uint32_t slot = (uint32_t)((clock + phase) / 6000.0f);
        if (slot != h.slot) {  // in seven of ten stretches of 6 s, one variation
            uint32_t r = Mix(h.seed, slot);
            h.slot = slot;
            h.fidgetAt = r % 100 < 70 ? slot * 6000.0f - phase + 300.0f + (float)((r >> 8) % 3000) : -1.0f;
            h.pick = (uint8_t)((r >> 20) % a->fidgetCount);
        }
        if (h.loopClock < 0) h.loopClock = clock;
        if (h.fidgetAt >= 0 && clock >= h.fidgetAt) {
            bool afterLoop = h.fidgetAt >= h.loopClock + 1500.0f;  // the loop's pose first, a moment after a move
            h.fidgetAt = -1;
            if (afterLoop) StartFidget(h, a->fidgets[h.pick], now);
        }
    }
    CEntity* target = h.fidget && h.fidget->head ? nullptr : Attention(h, ped, now, clock);
    if (target != h.looking) {
        bool talks = target && g_talk[target].activity > kTalking;
        if (talks && Mix(h.seed, ++h.turns) % 100 < 50) SetFace(ped, FACE_CURIOUS, 1200);  // the brows up at a new voice
        LOGD("Cutscene partner looks at %s (jaw %.3f)", !target ? "nobody" : target == CutsceneCj() ? "CJ" : Format("model %d", target->ModelIndex()).c_str(),
             target ? g_talk[target].activity : 0.0f);
        h.looking = target;
    }
    if (!target) {
        if (h.lookOn) StopLooking(ped), h.lookOn = false;
    } else if (now - h.lookedAt > 200) {  // it follows the head as it moves
        LookAt(ped, g_talk[target].head);
        if (!h.lookOn) LOGD("Cutscene partner: look-at %s", CallMethod<0x6181A0, bool>((void*)kIkChainMan, ped) ? "on" : "refused");
        h.lookedAt = now, h.lookOn = true;
    }
    if (clock >= 0) {  // gum now and then: in a third of every 15 s
        uint32_t slot = (uint32_t)(clock / 15000.0f);
        if (slot != h.faceSlot) {
            uint32_t r = Mix(h.seed ^ 0x5F3759DFu, slot);
            h.faceSlot = slot;
            h.faceAt = r % 100 < 35 ? slot * 15000.0f + 2000.0f + (float)((r >> 8) % 8000) : -1.0f;
        }
        if (h.faceAt >= 0 && clock >= h.faceAt) h.faceAt = -1, SetFace(ped, FACE_CHEWING, 3500);
    }
}
}  // namespace

void Prepare() {
    for (const auto& list : {std::vector<Act>(std::begin(kStand), std::end(kStand)), std::vector<Act>(std::begin(kLean), std::end(kLean)),
                             std::vector<Act>(std::begin(kSit), std::end(kSit)), std::vector<Act>(std::begin(kStep), std::end(kStep))})
        for (const Act& a : list) {
            BlockReady(a.block);
            for (size_t i = 0; i < a.fidgetCount; ++i) BlockReady(a.fidgets[i].block);
        }
    RequestModel(kCigarette);
    LoadAllRequestedModels(false);
}

// The view matrix is rebuilt only when the world is drawn: on a black screen (a fade) it still holds an old camera,
// and nothing projects right. A point 10 m ahead of the camera must come out in the middle of the screen.
bool ViewIsCurrent(const Scene& sc) {
    float x = 0, y = 0, d = 0;
    bool ok = WorldToScreen(sc.cam + sc.camFwd * 10.0f, x, y, d) && std::fabs(d - 10.0f) < 0.5f &&
              std::fabs(x - sc.w * 0.5f) < sc.w * 0.05f && std::fabs(y - sc.h * 0.5f) < sc.h * 0.05f;
    static uint64_t logged = 0;
    if (!ok && NowMs() - logged > 2000) {
        logged = NowMs();
        LOGD("Cutscene view not drawn yet (10 m ahead shows at %.0f,%.0f, depth %.1f)", x, y, d);
    }
    return ok;
}

void Analyse() {
    UpdateTalk();
    Scene sc;
    g_live = BuildScene(sc);
    if (!g_live || !ViewIsCurrent(sc)) return;
    const char* name = (const char*)kCutsceneName;
    if (strncmp(name, g_cutscene, 8) != 0) {  // another cutscene: its own look for the partner
        strncpy(g_cutscene, name, 8);
        g_seed = 2166136261u;
        for (const char* c = g_cutscene; *c; ++c) g_seed = (g_seed ^ (uint8_t)*c) * 16777619u;
        g_haveSpot = g_haveSeat = false;
    }
    float clock = Ref<float>(cam::Active() + 0x90);
    if (g_check.armed && clock >= g_check.at) {
        g_check.armed = false;
        LOGD("Cutscene %s: CJ's root track said %.2f,%.2f, CJ is at %.2f,%.2f (%.2f m off)", g_cutscene, g_check.cj.x,
             g_check.cj.y, sc.cj.x, sc.cj.y, (g_check.cj - sc.cj).Length2D());
    }
    // a new shot: the spline's next cut is passed (some cuts barely move the camera), or the camera or CJ jumped
    bool onSpline = cam::Mode() == kModeFlyBy && !g_ahead.empty();
    if (onSpline && clock >= g_shotFrom && clock < g_shotTo) return;  // the frames between two keys of a cut
    bool cut = !g_haveSpot || (sc.cam - g_lastCam).Length() > 2.0f || (sc.cj - g_lastCj).Length() > 2.0f ||
               (onSpline && clock >= g_shotTo);
    g_lastCam = sc.cam;
    g_lastCj = sc.cj;
    if (!cut) return;
    float start = onSpline && clock >= g_shotTo ? g_shotTo : g_haveSpot ? clock : 0.0f;  // where this shot begins
    auto t0 = std::chrono::steady_clock::now();
    BuildGrid(sc.cj, sc.floor);  // every shot: props move between shots
    BuildAhead(sc);
    std::fill(std::begin(g_rejects), std::end(g_rejects), 0);
    std::fill(std::begin(g_attractors), std::end(g_attractors), 0);
    g_tried.clear();
    bool keptOut = g_spotOut;
    g_spot = Choose(sc);
    g_spotOut = !strcmp(g_how, " (out of the picture)") || (keptOut && !strcmp(g_how, " (stays)"));
    g_decided = true, g_decidedAt = start;
    g_lastScene = sc;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string kinds;
    for (int t = 0; t < 16; ++t)
        if (g_attractors[t]) kinds += Format(" %d:%d", t, g_attractors[t]);
    if (!kinds.empty()) LOGD("  ped attractors around (type:count):%s", kinds.c_str());
    g_haveSpot = true;
    ++g_shot;
    LOGD("Cutscene %s shot %d (%.1f s ahead%s): %s%s%s%s in %.1f ms, seen %.0f%% (rejected: distance %d, crowd %d, frame %d, "
         "covers CJ %d, covers actor %d, solid %d, floor %d, not seen %d, in the way %d, later in the shot %d)",
         g_cutscene, g_shot, g_ahead.empty() ? 0.0f : g_ahead.back().dt, sc.cjInCar ? ", CJ in a car" : "",
         Describe(g_spot), g_how, *g_staysNo ? ", not staying: " : "", g_staysNo, ms, SeenShare(sc, g_spot) * 100.0f, g_rejects[R_DISTANCE],
         g_rejects[R_CROWD], g_rejects[R_FRAME], g_rejects[R_COVERS_CJ], g_rejects[R_COVERS_ACTOR], g_rejects[R_SOLID],
         g_rejects[R_FLOOR], g_rejects[R_HIDDEN], g_rejects[R_FRONT], g_rejects[R_LATER]);
    // the people's root tracks are read from the engine's own data: CJ's is checked against where CJ gets to
    if (!g_ahead.empty()) {
        const Sample& mid = g_ahead[g_ahead.size() / 2];
        g_check = {mid.cj, clock + mid.dt * 1000.0f, true};
    }
}

bool TakeDecision(Spot& s, float& at) {
    if (!g_decided) return false;
    g_decided = false;
    s = g_spot, at = g_decidedAt;
    return true;
}

void FollowSpot(const Spot& s, float at) { g_follow.push_back({s, at}); }

void Follow() {
    UpdateTalk();
    g_live = CutsceneCj() != nullptr;
    if (!g_live || cam::Mode() != kModeFlyBy) return;
    float clock = Ref<float>(cam::Active() + 0x90);
    bool due = false;
    for (; !g_follow.empty() && g_follow.front().at <= clock + 30.0f; g_follow.pop_front()) g_spot = g_follow.front().spot, due = true;
    if (!due) return;
    g_haveSpot = true;
    ++g_shot;
    LOGD("Cutscene shot %d from the host: %s", g_shot, Describe(g_spot));
}

bool CurrentSpot(Spot& out) {
    if (!g_live || !g_haveSpot) return false;
    out = g_spot;
    return true;
}

int ShotNumber() { return g_shot; }

float ShotLeftMs() {
    if (!g_live || g_ahead.empty() || cam::Mode() != kModeFlyBy) return -1;
    return g_shotFrom - Ref<float>(cam::Active() + 0x90);
}

const char* Describe(const Spot& s) {
    static char buf[96];
    static const char* names[] = {"stand", "lean", "sit", "steps", "hidden", "walk", "rides"};
    const Act* a = ActOf(s);
    snprintf(buf, sizeof(buf), "%s %s/%s at %.1f,%.1f,%.1f", names[s.pose], a ? a->block : "-", a && a->anim ? a->anim : "-",
             s.ground.x, s.ground.y, s.ground.z);
    return buf;
}

// Walking along with CJ (both games: the path comes from their own CJ): put on it every frame, stepping on the spot
// with CJ's walk from the cutscenes as fast as the path goes, the clips when CJ stops.
void HoldWalk(CPed* ped, Held& h, const Spot& s, bool start) {
    CEntity* cj = CutsceneCj();
    Track tr;
    if (!cj || !cj->Matrix() || !TrackOf(cj, tr) || PathOf(tr).size() < 2) return;  // stays where it was
    uint64_t now = NowMs();
    Walker w = WalkerAt(PathOf(tr), tr.time, CjFeet(cj), cj->Matrix()->forward, tr.time, (int)s.act - 1);
    if (start) {
        if (h.placed) Cmd(0x0792).I(h.handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: the previous act stops
        if (ped->InVehicle()) Cmd(0x0362).I(h.handle).F(w.feet.x).F(w.feet.y).F(w.feet.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        Cmd(0x04D7).I(h.handle).I(1).Run();  // FREEZE_CHAR_POSITION: moved by hand, not by physics (sets lack collision)
        if (h.prop >= 0) Cmd(0x0108).I(h.prop).Run(), h.prop = -1;  // DELETE_OBJECT
        h.seed = Mix(Mix((uint32_t)std::lround(s.ground.x * 100.0f), (uint32_t)std::lround(s.ground.y * 100.0f)), POSE_WALK * 8u + s.act);
        h.stage = 0, h.walking = false, h.clip = nullptr, h.clips = 0, h.fidget = nullptr, h.slot = ~0u, h.loopClock = -1;
        h.heading = w.heading;
    }
    h.spot = s, h.placed = true;
    float turn = std::remainder(w.heading - h.heading, 6.2831853f), most = 5.0f * TimeStep() / 50.0f;  // as fast as a person turns
    h.heading += std::clamp(turn, -most, most);
    if (CMatrix* m = ped->Matrix()) {
        float c = std::cos(h.heading), sn = std::sin(h.heading);
        m->right = {c, sn, 0}, m->forward = {-sn, c, 0}, m->up = {0, 0, 1};
    }
    SetEntityPosition(ped, w.feet + CVector{0, 0, CallMethod<0x536BE0, float>(ped)});  // CEntity::GetDistanceFromCentreOfMassToBaseOfModel
    ped->CurrentRotation() = ped->AimingRotation() = h.heading;
    ped->PedFlags() = (ped->PedFlags() | 0x3u) & ~0x200u;  // bIsStanding, bWasStanding, not bIsInTheAir
    ped->Flags() |= 0x80;
    Field<float>(ped, 0x12C) = Field<float>(cj, 0x12C);  // lit like CJ
    bool moving = w.speed > (h.walking ? 0.25f : 0.6f);
    if (h.stage < 2 || moving != h.walking) {
        h.walking = moving, h.stage = 2;
        if (moving) {
            size_t i = Mix(h.seed, ++h.clips) % MocapWalkCount();
            h.walk = MocapWalk(i), h.walkSpeed = MocapWalkSpeed(i), h.speedAt = 0;
            Cmd(0x0605).I(h.handle).S(h.walk).S(kMocapBlock).F(4.0f).I(1).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, looped
            h.checkedAt = now;
            LOGD("Cutscene partner walks: %s (%.2f m/s)", h.walk, w.speed);
        } else {
            NextClip(h, 4.0f);
        }
    }
    if (h.walking && now - h.speedAt > 100) {  // the steps as fast as the path goes
        h.speedAt = now;
        Cmd(0x0393).I(h.handle).S(h.walk).F(std::clamp(w.speed / h.walkSpeed, 0.6f, 1.8f)).Run();  // SET_CHAR_ANIM_SPEED
    }
    Live(ped, h, &kClips);
    if (now - h.checkedAt > 500) {  // anything may end the animation: it starts again
        h.checkedAt = now;
        if (!Cmd(0x0611).I(h.handle).S(h.walking ? h.walk : AnimNow(h, &kClips)).Run()) h.stage = 0;  // IS_CHAR_PLAYING_ANIM
    }
}

// Riding with CJ: in the car's own frame every frame as it drives, sitting as a passenger does, the head to who talks
const Act kRide = {"PED", "CAR_sitp", false, 0.0f};
void HoldInCar(CPed* ped, Held& h, const Spot& s, bool start) {
    CEntity* car = s.act < NumCutsceneObjects() ? CutsceneObject(s.act) : nullptr;
    CMatrix* m = car ? car->Matrix() : nullptr;
    if (!m) {  // the scene has no such car any more: out of it until the next spot
        ped->Flags() &= ~0x80u;
        return;
    }
    if (start) {
        if (h.placed) Cmd(0x0792).I(h.handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: the previous act stops
        Cmd(0x04D7).I(h.handle).I(1).Run();            // FREEZE_CHAR_POSITION: moved by hand
        if (h.prop >= 0) Cmd(0x0108).I(h.prop).Run(), h.prop = -1;  // DELETE_OBJECT: no cigarette in a car
        h.stage = 0;
    }
    h.spot = s, h.placed = true;
    // the car's front (the heading of the spot): its model may face backwards, and he sat facing the boot
    float facing = std::cos(s.heading) < 0.0f ? -1.0f : 1.0f;
    if (CMatrix* pm = ped->Matrix()) pm->right = m->right * facing, pm->forward = m->forward * facing, pm->up = m->up;
    // the seat is where CJ's hips would sit, and the passenger's animation holds the hips 7 cm from the ped's origin. Its
    // own skeleton is no measure of that: an old one (not drawn yet) sent the partner off further every frame, 2800 km.
    SetEntityPosition(ped, m->pos + m->right * s.ground.x + m->forward * s.ground.y + m->up * s.ground.z);
    ped->CurrentRotation() = ped->AimingRotation() = HeadingOf(m->forward * facing);
    ped->PedFlags() = (ped->PedFlags() | 0x3u) & ~0x200u;  // bIsStanding, bWasStanding, not bIsInTheAir
    ped->Flags() |= 0x80;
    if (CEntity* cj = CutsceneCj()) Field<float>(ped, 0x12C) = Field<float>(cj, 0x12C);  // lit like CJ
    if (h.stage < 2) {
        Cmd(0x0605).I(h.handle).S("CAR_sitp").S("PED").F(4.0f).I(1).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, looped
        h.stage = 2, h.checkedAt = NowMs();
        LOGD("Cutscene partner rides in object %d at %.2f,%.2f,%.2f of it", s.act, s.ground.x, s.ground.y, s.ground.z);
    } else if (NowMs() - h.checkedAt > 500) {  // anything may end the animation: it starts again
        h.checkedAt = NowMs();
        if (!Cmd(0x0611).I(h.handle).S("CAR_sitp").Run()) h.stage = 0;  // IS_CHAR_PLAYING_ANIM
    }
    if (h.stage == 2) Live(ped, h, &kRide);
}

void Hold(CPed* ped, const Spot& s) {
    Held& h = g_held[ped];
    int handle = GetPedRef(ped);
    bool newAct = !h.placed || h.spot.pose != s.pose || h.spot.act != s.act;
    bool moved = !h.placed || (h.spot.ground - s.ground).Length() > 0.05f || std::fabs(h.spot.heading - s.heading) > 0.01f;
    h.handle = handle;
    if (s.pose == POSE_WALK) return HoldWalk(ped, h, s, newAct);
    if (s.pose == POSE_CAR) return HoldInCar(ped, h, s, newAct || moved);
    if (moved) {
        if (h.placed) Cmd(0x0792).I(handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: no landing from a fall here
        // still seated (its on-foot state has not come yet): moving it would move the vehicle, so out first
        if (ped->InVehicle()) Cmd(0x0362).I(handle).F(s.ground.x).F(s.ground.y).F(s.ground.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        Cmd(0x04D7).I(handle).I(1).Run();  // FREEZE_CHAR_POSITION: the scene's collision may not be loaded here
        Cmd(0x00A1).I(handle).F(s.ground.x).F(s.ground.y).F(s.ground.z).Run();  // SET_CHAR_COORDINATES (feet)
        Cmd(0x0173).I(handle).F(s.heading * 57.29578f).Run();                   // SET_CHAR_HEADING
        h.stage = 0;  // the move ends the animation task: it starts again
    }
    const Act* a = ActOf(s);
    if (newAct) {
        if (h.stage > 0) Cmd(0x0792).I(handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: the previous act stops
        if (h.prop >= 0 && (!a || !a->cigarette)) {
            Cmd(0x0108).I(h.prop).Run();  // DELETE_OBJECT
            h.prop = -1;
        }
        h.stage = 0;
    }
    if (moved || newAct) {  // the variations start over, drawn from the new spot
        h.seed = Mix(Mix((uint32_t)std::lround(s.ground.x * 100.0f), (uint32_t)std::lround(s.ground.y * 100.0f)), s.pose * 8u + s.act);
        h.slot = ~0u, h.fidgetAt = -1, h.loopClock = -1, h.fidget = nullptr, h.clip = nullptr, h.clips = 0, h.walking = false;
    }
    h.spot = s;
    h.placed = true;
    ped->CurrentRotation() = ped->AimingRotation() = s.heading;
    // frozen, it never lands after a move: "in the air", it would fall (hidden too: it lands when it shows again)
    // and the fall cancels the animation
    ped->PedFlags() = (ped->PedFlags() | 0x3u) & ~0x200u;  // bIsStanding, bWasStanding, not bIsInTheAir
    if (!a) {  // nowhere to be in this shot
        ped->Flags() &= ~0x80u;
        return;
    }
    ped->Flags() |= 0x80;  // bIsVisible: cutscenes hide players
    // lit like CJ: a frozen ped keeps the light of the place it stood before (white at night)
    if (CEntity* cj = CutsceneCj()) Field<float>(ped, 0x12C) = Field<float>(cj, 0x12C);  // m_fContactSurfaceBrightness
    if (h.stage == 0) {
        bool ready = BlockReady(a->block);
        if (a->cigarette && !IsModelLoaded(kCigarette)) {
            RequestModel(kCigarette);
            ready = false;
        }
        if (!ready) return;
        if (a->cigarette) {
            if (h.prop < 0 && HasRoom(ObjectPool(), kKeepObjects, "object"))
                Cmd(0x0107).I(kCigarette).F(s.ground.x).F(s.ground.y).F(s.ground.z + 1.0f).Out(&h.prop).Run();  // CREATE_OBJECT
            // TASK_PICK_UP_OBJECT: into the right hand, no pick-up animation
            if (h.prop >= 0) Cmd(0x070A).I(handle).I(h.prop).F(0.0f).F(0.0f).F(0.0f).I(6).I(16).S("NULL").S("NULL").I(-1).Run();
        }
        h.stage = 1;
        return;  // the animation starts next frame, once the hand holds the cigarette
    }
    if (h.stage == 1) {
        if (!a->anim) NextClip(h, 8.0f);
        else Cmd(0x0605).I(handle).S(a->anim).S(a->block).F(8.0f).I(1).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, looped
        h.stage = 2, h.checkedAt = NowMs();  // the task starts it next frame
        LOGD("Cutscene partner: %s/%s started", a->block, AnimNow(h, a));
    }
    Live(ped, h, a);
    // anything may end the animation (an event of the ped, the sync of a remote player): it starts again
    if (h.stage == 2 && NowMs() - h.checkedAt > 500) {
        h.checkedAt = NowMs();
        if (!Cmd(0x0611).I(handle).S(AnimNow(h, a)).Run()) {  // IS_CHAR_PLAYING_ANIM
            LOGD("Cutscene partner: %s/%s stopped, starting it again (ped flags %08X)", a->block, AnimNow(h, a), ped->PedFlags());
            Cmd(0x0792).I(handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: whatever took over
            h.stage = 0;
        }
    }
}

void ReleaseAll() {
    for (auto& [ped, h] : g_held) {
        if (GetPed(h.handle) == ped) {
            if (h.lookOn) StopLooking(ped);
            if (h.stage > 0) Cmd(0x0792).I(h.handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
            Cmd(0x04D7).I(h.handle).I(0).Run();              // FREEZE_CHAR_POSITION off
            ped->Flags() |= 0x80;
        }
        if (h.prop >= 0) Cmd(0x0108).I(h.prop).Run();  // DELETE_OBJECT
    }
    g_held.clear();
    g_talk.clear();
    g_haveSpot = g_haveSeat = false;
    g_live = false;
    g_cutscene[0] = 0;
    g_follow.clear();
    g_decided = false;
}

// ---- review harness
void DumpShot(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    const Scene& sc = g_lastScene;
    auto body = [&](const Body& b) { fprintf(f, " %.1f %.1f %.1f %.1f %.2f", b.x, b.top, b.bottom, b.half, b.depth); };
    fprintf(f, "shot %s %d\n", g_cutscene, g_shot);
    fprintf(f, "cam %.3f %.3f %.3f %.4f %.4f %.4f %.1f\n", sc.cam.x, sc.cam.y, sc.cam.z, sc.camFwd.x, sc.camFwd.y,
            sc.camFwd.z, sc.fov);
    fprintf(f, "screen %.0f %.0f\n", sc.w, sc.h);
    fprintf(f, "cj %.3f %.3f %.3f %.3f %d", sc.cj.x, sc.cj.y, sc.cj.z, sc.floor, sc.cjOnScreen);
    body(sc.cjBody);
    fprintf(f, "\n");
    for (const Actor& a : sc.actors) {
        fprintf(f, "actor %.3f %.3f %.3f %d", a.feet.x, a.feet.y, a.feet.z, a.onScreen);
        body(a.body);
        fprintf(f, "\n");
    }
    fprintf(f, "spot %.3f %.3f %.3f %.3f %d %d\n", g_spot.ground.x, g_spot.ground.y, g_spot.ground.z, g_spot.heading,
            g_spot.pose, g_spot.act);
    // the rest of the shot: the camera's path and the people's tracks
    for (const Sample& m : g_ahead) {
        fprintf(f, "ahead %.2f %.3f %.3f %.3f %.4f %.4f %.4f %.3f %.3f", m.dt, m.view.pos.x, m.view.pos.y, m.view.pos.z,
                m.view.fwd.x, m.view.fwd.y, m.view.fwd.z, m.cj.x, m.cj.y);
        for (const CVector& a : m.actors) fprintf(f, " %.3f %.3f", a.x, a.y);
        fprintf(f, "\n");
    }
    for (const Tried& t : g_tried)
        fprintf(f, "try %.3f %.3f %.3f %.3f %d %.2f %d\n", t.spot.ground.x, t.spot.ground.y, t.spot.ground.z,
                t.spot.heading, t.spot.pose, t.score, t.reject);
    // the height map the spots were checked against: per row, top (2 cm) and seat (1 cm) in hex, ground as 0/1
    fprintf(f, "grid %.2f %.2f %.3f %d %.2f\n", g_grid.x0, g_grid.y0, g_grid.floor, kCells, kCell);
    for (int y = 0; y < kCells; ++y) {
        fprintf(f, "gt ");
        for (int x = 0; x < kCells; ++x) fprintf(f, "%02x", g_grid.top[y * kCells + x]);
        fprintf(f, "\ngs ");
        for (int x = 0; x < kCells; ++x) fprintf(f, "%02x", g_grid.seat[y * kCells + x]);
        fprintf(f, "\ngg ");
        for (int x = 0; x < kCells; ++x) fputc('0' + g_grid.ground[y * kCells + x], f);
        fprintf(f, "\n");
    }
    fclose(f);
}

struct WorldDump {
    FILE* f;
    float lo, hi;
    char tag;
};
void DumpTriangle(const CVector& a, const CVector& b, const CVector& c, void* user) {
    auto* d = (WorldDump*)user;
    if (std::max({a.z, b.z, c.z}) < d->lo || std::min({a.z, b.z, c.z}) > d->hi) return;
    fprintf(d->f, "%c %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", d->tag, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z);
}

void DumpWorld(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    const Scene& sc = g_lastScene;
    CEntity* ents[512];
    int16_t n = 0;
    Call<0x564A20>(&sc.cj, 30.0f, false, &n, (int16_t)512, ents, true, false, false, true, false);  // FindObjectsInRange
    float lo = sc.floor - 1.0f, hi = sc.floor + 3.5f;  // what a person on CJ's floor can touch
    int area = Ref<int32_t>(kCurrArea);
    for (int i = 0; i < n; ++i) {
        CEntity* e = ents[i];
        if (e->AreaCode() != area && e->AreaCode() != 13) continue;
        void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)e->ModelIndex() * 4);
        void* col = mi ? Field<void*>(mi, 0x14) : nullptr;     // CBaseModelInfo::m_pColModel
        uint8_t* data = col ? Field<uint8_t*>(col, 0x2C) : nullptr;  // CColModel::m_pColData
        CVector p = e->GetPosition();
        fprintf(f, "E %d %d %.2f %.2f %.2f\n", e->ModelIndex(), (int)(Field<uint8_t>(e, 0x36) & 7), p.x, p.y, p.z);
        if ((e->Flags() & 0x80) && !Field<uint8_t>(e, 0x34)) {  // drawn, not a LOD: walls without collision too
            WorldDump d{f, lo, hi, 'R'};
            ForEachTriangle(e, &DumpTriangle, &d);
        }
        if (!data) continue;
        int spheres = *(uint16_t*)data, boxes = *(uint16_t*)(data + 2), tris = *(uint16_t*)(data + 4);
        auto* sp = *(uint8_t**)(data + 0x08);
        auto* bx = *(uint8_t**)(data + 0x0C);
        auto* vx = *(int16_t**)(data + 0x14);
        auto* tr = *(uint16_t**)(data + 0x18);
        for (int k = 0; sp && k < spheres; ++k) {
            const uint8_t* s = sp + k * 0x14;
            CVector c = Transform(e, *(const CVector*)s, true);
            float r = *(const float*)(s + 0x0C);
            if (c.z + r >= lo && c.z - r <= hi) fprintf(f, "S %.2f %.2f %.2f %.2f %d\n", c.x, c.y, c.z, r, s[0x10]);
        }
        for (int k = 0; bx && k < boxes; ++k) {
            const uint8_t* b = bx + k * 0x1C;
            const CVector& mn = *(const CVector*)b;
            const CVector& mx = *(const CVector*)(b + 0x0C);
            fprintf(f, "B %d", b[0x18]);
            for (int c = 0; c < 8; ++c) {
                CVector w = Transform(e, CVector{c & 1 ? mx.x : mn.x, c & 2 ? mx.y : mn.y, c & 4 ? mx.z : mn.z}, true);
                fprintf(f, " %.2f %.2f %.2f", w.x, w.y, w.z);
            }
            fprintf(f, "\n");
        }
        for (int k = 0; vx && tr && k < tris; ++k) {
            const uint16_t* t = tr + k * 4;  // a, b, c, material | light
            CVector v[3];
            for (int j = 0; j < 3; ++j) {
                const int16_t* c = vx + t[j] * 3;
                v[j] = Transform(e, CVector{c[0] / 128.0f, c[1] / 128.0f, c[2] / 128.0f}, true);
            }
            float zmin = std::min({v[0].z, v[1].z, v[2].z}), zmax = std::max({v[0].z, v[1].z, v[2].z});
            if (zmax < lo || zmin > hi) continue;
            fprintf(f, "T %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %d\n", v[0].x, v[0].y, v[0].z, v[1].x, v[1].y,
                    v[1].z, v[2].x, v[2].y, v[2].z, t[3] & 0xFF);
        }
    }
    // the cutscene's own objects: people are skinned, the rest are props (furniture, doors) without collision
    for (int i = 0; i < NumCutsceneObjects(); ++i) {
        CEntity* o = CutsceneObject(i);
        if (!o || o->ModelIndex() == 1 || (o->GetPosition() - sc.cj).Length() > 20.0f) continue;
        bool person = IsPerson(o);
        CVector p = o->GetPosition();
        Bounds cb;
        bool car = IsCar(o, cb);
        fprintf(f, "P %d %d %.2f %.2f %.2f %d %.1f %.1f\n", o->ModelIndex(), person, p.x, p.y, p.z, car, cb.x1 - cb.x0,
                cb.y1 - cb.y0);
        if (!person) {
            WorldDump d{f, lo, hi, 'Q'};
            ForEachTriangle(o, &DumpTriangle, &d);
        }
    }
    fclose(f);
}

CVector LastCj() { return g_lastScene.cj; }

}  // namespace coop::scene
