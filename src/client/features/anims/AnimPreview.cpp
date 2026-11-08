// The U window's preview (Emotes.h): our CJ's body, and for a pair the other player's facing it, playing the chosen emote
// in a picture of its own. A body is a copy of the ped's clump made as the game makes a ped's (CClumpModelInfo::
// CreateInstance: a bone hierarchy of its own with the skin's atomics pointed at it, the animation system on it), moved
// by the game's animation code and drawn into a texture by a RenderWare camera of ours once the frame's camera is done
// (the game draws a ped into its shadow so, CShadowCamera). Nothing of it is in the world: no physics, no tasks, nobody
// else sees it. The camera is the world's for its lights only (a ped's lighting takes the world's lights), turned to the
// bodies' fronts while it draws. It keeps the bodies whole in the picture, whatever the pose, and draws them on a floor
// of their own with their shadows.
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/anims/Emotes.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop::anims {

namespace {
using namespace game;

constexpr float kView = 0.27f;   // the camera's view window: half its field's tangent (30 degrees, a portrait lens)
constexpr float kFill = 0.9f;    // how much of the picture the bodies take
constexpr float kRise = 0.2f;    // the camera's height over its distance: a little from above, the floor seen
constexpr float kFloor = -1.0f;  // the ground under a ped's middle: its collision box's bottom (CTempColModels' ped)

struct RwMat {  // RwMatrix
    CVector right;
    uint32_t flags;
    CVector up;
    uint32_t pad1;
    CVector at;
    uint32_t pad2;
    CVector pos;
    uint32_t pad3;
};
static_assert(sizeof(RwMat) == 0x40);
constexpr uint32_t kOrthonormal = 3;  // rwMATRIXTYPEORTHONORMAL

struct RwRGBAReal {
    float r, g, b, a;
};

struct ImVertex {  // RwIm3DVertex (RxObjSpace3DVertex): taken lit as it is, its colour ARGB
    CVector pos, normal;
    uint32_t color;
    float u, v;
};
static_assert(sizeof(ImVertex) == 36);

RwMat Facing(const CVector& at, const CVector& pos) {  // a frame looking along `at`, its up as near the world's as can be
    RwMat m{};
    m.at = at.Normalized();
    CVector up = std::fabs(m.at.z) > 0.99f ? CVector{0, 1, 0} : CVector{0, 0, 1};
    m.right = up.Cross(m.at).Normalized();  // RenderWare's right of a camera is the picture's left
    m.up = m.at.Cross(m.right);
    m.pos = pos;
    m.flags = kOrthonormal;
    return m;
}

void SetFrame(void* frame, const RwMat& m) {
    Field<RwMat>(frame, 0x10) = m;  // RwFrame::modelling
    Call<0x7F0910>(frame);         // RwFrameUpdateObjects
}

CVector Min(const CVector& a, const CVector& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
CVector Max(const CVector& a, const CVector& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Body {
    void* clump = nullptr;
    void* hier = nullptr;  // its own RpHAnimHierarchy
    void* from = nullptr;  // the clump it is a copy of
    std::string playing;   // "block/anim" it plays, empty: standing
    void* anim = nullptr;  // that animation's hierarchy, blended on it
    bool shoulders = false;  // the skeleton has the bones CPed::ShoulderBoneRotation turns
    CVector lo, hi;          // where its bones are this frame
    CVector shift;           // where the root's walk goes, a ped's moving shift (nothing reads it)
};
Body g_body[2];
void* g_cam = nullptr;
int g_size = 0;  // the picture's side
bool g_camFailed = false;
uint16_t g_want = 0;  // the emote asked for (0: standing)
float g_pixels = 0.0f;  // how big the window shows the picture
uint64_t g_wantMs = 0, g_lastMs = 0;
bool g_drawn = false;
float g_yaw = 0.45f;
// what the camera keeps in the picture: the bones' extent across it and up it the last moments, for that emote and turn
struct Seen {
    float x0, x1, y0, y1;
};
Seen g_seen{};
bool g_seenSet = false;
uint16_t g_seenFor = 0;
float g_seenYaw = 0.0f;
CVector g_camAt;  // where the camera looks and how far it stands, eased
float g_camDist = 0.0f;
bool g_camSet = false;
// the way the body faces once into an emote, the camera turned after it: animations made for other places in the game
// turn the body round (the packs' dances showed their back)
float g_face = 0.0f, g_faceTo = 0.0f;
uint64_t g_faceAtMs = 0;

void Stand(void* clump, float delta) {
    if (void* idle = Call<0x4D42F0, void*>("IDLE_stance", (void*)anim::Block(0))) anim::Blend(clump, idle, anim::F_LOOPED, delta);
}

void FreeBody(Body& b) {
    if (b.clump) {
        Call<0x749B70>(b.clump, (void*)0x734B90, nullptr);  // RpClumpForAllAtomics(AtomicRemoveAnimFromSkinCB): the animation made for it
        Call<0x74A310>(b.clump);                            // RpClumpDestroy (its blending data and their blocks' references)
    }
    b = Body{};
}

bool MakeBody(Body& b, CPed* ped) {
    void* src = ped ? ped->RwObject() : nullptr;
    if (!src || Field<uint8_t>(src, 0) != 2) return false;  // rpCLUMP
    if (b.from == src && b.clump) return true;
    FreeBody(b);
    void* c = Call<0x749F70, void*>(src);  // RpClumpClone
    if (!c) return false;
    void* h = Call<0x734B10, void*>(c);  // GetAnimHierarchyFromClump: the copy's own, from its frames
    if (!h) {
        Call<0x74A310>(c);
        LOGW("Preview: the body has no bone hierarchy");
        return false;
    }
    Call<0x749B70>(c, (void*)0x4C4EF0, h);  // RpClumpForAllAtomics(SetHierarchyForSkinAtomic): the copied skin had the original's
    void* animation = Call<0x4D60E0, void*>(h);  // RpAnimBlendCreateAnimationForHierarchy
    Call<0x7CD5A0>(Field<void*>(h, 0x20), animation);  // RtAnimInterpolatorSetCurrentAnim(currentAnim, ...)
    Field<int32_t>(h, 0x00) = 0x3000;  // rpHANIMHIERARCHYUPDATEMODELLINGMATRICES | rpHANIMHIERARCHYUPDATELTMS
    Call<0x4D6720>(c);  // RpAnimBlendClumpInit
    // the root as a ped's: its data points at a moving shift (CPed::SetModelIndex, CCutsceneObject's too), so the root
    // goes through the velocity extraction (0x4D1680). Without it an emote laid over the standing took the root's place
    // from the sum of both weights (FrameUpdateCallBackSkinned 0x4D2B90) and a sitting CJ sank twice as deep
    Field<CVector*>(Field<void*>(c, Ref<uint32_t>(0xB5F878)), 0x0C) = &b.shift;  // CAnimBlendClumpData::m_PedPosition
    b.clump = c, b.hier = h, b.from = src;
    // the clavicles, upper arms and the "breasts" between them: without one of them the game's code writes before the
    // bones' matrices (RpHAnimIDGetIndex gives -1)
    b.shoulders = true;
    for (int bone : {21, 22, 31, 32, 301, 302}) b.shoulders = b.shoulders && Call<0x7C51A0, int>(h, bone) >= 0;
    Stand(c, 1000.0f);
    return true;
}

// The body plays that animation looped (a held pose once, staying on its end) once its block is in memory, laid over the
// standing as a script's TASK_PLAY_ANIM lays it over a ped's (always partial): the bones it has none of keep the
// standing's. The emotes' animations have 26 bones, IDLE_stance all 32: played whole with the standing blended out, the
// jaw, the brows and the belly had nothing to take from and the mouth went crooked (03.11).
void PlayOn(Body& b, const char* block, const char* name, bool hold) {
    std::string key = block && name ? std::string(block) + "/" + name : "";
    if (key == b.playing) return;
    void* h = nullptr;
    if (!key.empty()) {
        int i = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
        if (i < 0 || !NeedBlock(i)) return;
        if (!(h = Call<0x4D42F0, void*>(name, (void*)anim::Block(i)))) return;  // CAnimManager::GetAnimation
    }
    for (void* a = anim::First(b.clump); a; a = anim::Next(a))
        if (b.anim && anim::Hierarchy(a) == b.anim && anim::BlendDelta(a) >= 0.0f) anim::FadeOut(a);
    Stand(b.clump, 4.0f);
    if (h) anim::Blend(b.clump, h, (hold ? 0 : anim::F_LOOPED) | anim::F_PARTIAL, 4.0f);  // a held pose stays on its end
    b.anim = h;
    b.playing = key;
}

void FreeCamera() {
    g_drawn = false;
    if (!g_cam) return;
    if (void* world = Ref<void*>(0xC17038)) Call<0x750F50>(world, g_cam);  // RpWorldRemoveCamera
    void* frame = Field<void*>(g_cam, 0x04);  // RwObject::parent
    Call<0x804EF0>(g_cam, nullptr);
    if (frame) Call<0x7F05A0>(frame);
    for (int at : {0x60, 0x64})
        if (void* r = std::exchange(Field<void*>(g_cam, at), nullptr)) Call<0x7FB020>(r);
    Call<0x7EE4B0>(g_cam);
    g_cam = nullptr;
}

// RenderWare makes a depth raster with the screen's multisampling (_rwD3D9CreateZBufferRaster 0x4CCE48 passes
// D3DPRESENT_PARAMETERS' MultiSampleType on), a texture to draw into has none: with MSAA on the two would not match and
// nothing would be drawn. Ours is made with the screen's off; a lost device's rasters are made again with it (RenderWare
// restores them so), ours then once more
uint32_t& ScreenMultisample() { return Ref<uint32_t>(0xC9C050); }  // g_D3Dpp.MultiSampleType (MultiSampleQuality after)

bool DepthMultisampled() {
    void* z = g_cam && ScreenMultisample() ? Field<void*>(g_cam, 0x64) : nullptr;
    auto* surface = z ? Field<IDirect3DSurface9*>(z, Ref<int32_t>(0xB4E9E0)) : nullptr;  // _rwD3D9RasterExt's first
    D3DSURFACE_DESC desc{};
    return surface && SUCCEEDED(surface->GetDesc(&desc)) && desc.MultiSampleType != D3DMULTISAMPLE_NONE;
}

bool MakeCamera() {
    // a power of two near twice the side the window shows: each pixel there averages about four of ours, the edges come
    // out smooth (the game draws no smoothing of its own into a texture)
    int size = g_pixels <= 362.0f ? 512 : g_pixels <= 724.0f ? 1024 : 2048;
    if (g_cam && g_size == size && !DepthMultisampled()) return true;
    FreeCamera();
    if (g_camFailed) return false;
    void* world = Ref<void*>(0xC17038);  // Scene.m_pRpWorld
    void* cam = world ? Call<0x7EE4F0, void*>() : nullptr;  // RwCameraCreate
    void* frame = cam ? Call<0x7F0410, void*>() : nullptr;  // RwFrameCreate
    void* raster = frame ? Call<0x7FB230, void*>(size, size, 0, 5) : nullptr;  // RwRasterCreate(rwRASTERTYPECAMERATEXTURE)
    uint32_t* ms = &ScreenMultisample();
    uint32_t wasType = std::exchange(ms[0], 0u), wasQuality = std::exchange(ms[1], 0u);
    void* z = raster ? Call<0x7FB230, void*>(size, size, 0, 1) : nullptr;  // rwRASTERTYPEZBUFFER
    ms[0] = wasType, ms[1] = wasQuality;
    if (!z) {
        if (raster) Call<0x7FB020>(raster);  // RwRasterDestroy
        if (frame) Call<0x7F05A0>(frame);    // RwFrameDestroy
        if (cam) Call<0x7EE4B0>(cam);        // RwCameraDestroy
        g_camFailed = true;
        LOGW("Preview: its camera could not be made (%d)", size);
        return false;
    }
    Call<0x804EF0>(cam, frame);  // _rwObjectHasFrameSetFrame
    Field<void*>(cam, 0x60) = raster;  // RwCamera::frameBuffer
    Field<void*>(cam, 0x64) = z;       // RwCamera::zBuffer
    Call<0x7EE1D0>(cam, 0.3f);         // RwCameraSetNearClipPlane
    Call<0x7EE2A0>(cam, 40.0f);        // RwCameraSetFarClipPlane
    Call<0x7EE3A0>(cam, 1);            // RwCameraSetProjection(rwPERSPECTIVE)
    Call<0x750F20>(world, cam);        // RpWorldAddCamera
    g_cam = cam, g_size = size;
    LOGI("Preview: camera made (%d, the screen's multisampling %u)", size, ScreenMultisample());
    return true;
}

void FreeAll() {
    FreeBody(g_body[0]);
    FreeBody(g_body[1]);
    FreeCamera();
    g_seenSet = g_camSet = false;
}

// where the body's bones are: their matrices are the world's (the hierarchy's LTMs), the skin reaches a little past them
void Bones(Body& b) {
    b.lo = {1e9f, 1e9f, 1e9f}, b.hi = {-1e9f, -1e9f, -1e9f};
    int n = Field<int32_t>(b.hier, 0x04);       // numNodes
    auto* m = Field<RwMat*>(b.hier, 0x08);      // pMatrixArray
    for (int i = 0; m && i < n; ++i) b.lo = Min(b.lo, m[i].pos), b.hi = Max(b.hi, m[i].pos);
}

// a soft spot on the floor: `argb` in its middle fading to nothing at the rim
void Spot(float x, float y, float rx, float ry, uint32_t argb) {
    constexpr int kRim = 32;
    ImVertex v[kRim + 2]{};
    v[0].pos = {x, y, kFloor};
    v[0].color = argb;
    for (int i = 0; i <= kRim; ++i) {
        float a = 6.2831853f * (float)i / kRim;
        v[i + 1].pos = {x + std::cos(a) * rx, y + std::sin(a) * ry, kFloor};
        v[i + 1].color = argb & 0x00FFFFFF;
    }
    // no matrix: the vertices are the world's
    if (Call<0x7EF450, void*>(v, kRim + 2, (void*)nullptr, 8 | 16)) {  // RwIm3DTransform(rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA)
        Call<0x7EF6B0>(5);                                               // RwIm3DRenderPrimitive(rwPRIMTYPETRIFAN)
        Call<0x7EF520>();                                                // RwIm3DEnd
    }
}

void* __cdecl RenderAtomic(void* atomic, void*) {
    if (Field<uint8_t>(atomic, 2) & 0x04) Call<0x7491C0, void*>(atomic);  // rpATOMICRENDER: AtomicDefaultRenderCallBack
    return atomic;
}

// RwRenderStateSet and Get through RwEngineInstance->dOpenDevice. The states are the device's for everything after: ours
// go back as they were (the frame's drawing does not set each of them again, the culling among them)
using RenderStateFn = int(__cdecl*)(int, void*);
struct States {
    // rwRENDERSTATETEXTURERASTER, ZTESTENABLE, SHADEMODE, ZWRITEENABLE, SRCBLEND, DESTBLEND, VERTEXALPHAENABLE, FOGENABLE,
    // CULLMODE
    static constexpr int kWhich[] = {1, 6, 7, 8, 10, 11, 12, 14, 20};
    static constexpr int kCount = sizeof(kWhich) / sizeof(kWhich[0]);
    uintptr_t was[kCount] = {};
    static RenderStateFn Fn(int at) { return Field<RenderStateFn>(Ref<void*>(0xC97B24), at); }
    static void Set(int state, uintptr_t value) { Fn(0x20)(state, (void*)value); }
    States() {
        for (int i = 0; i < kCount; ++i) Fn(0x24)(kWhich[i], &was[i]);
        // the floor's first: no texture, its colours' alpha blended (rwBLENDSRCALPHA, rwBLENDINVSRCALPHA), the depth
        // tested but not written; no fog, no culling (rwCULLMODECULLNONE). The colours run smooth across a triangle
        // (rwSHADEMODEGOURAUD): the frame ends with flat shading (seen 03.11), which drew CJ in facets and the floor's
        // fans in their rims' colour, unseen
        const uintptr_t ours[kCount] = {0, 1, 2, 0, 5, 6, 1, 0, 1};
        for (int i = 0; i < kCount; ++i) Set(kWhich[i], ours[i]);
    }
    ~States() {
        for (int i = 0; i < kCount; ++i) Set(kWhich[i], was[i]);
    }
};

void Draw() {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!me || !MakeCamera() || !MakeBody(g_body[0], me)) return;
    const Emote* e = g_want ? Find(g_want) : nullptr;
    bool pair = e && e->kind == PAIR;
    if (pair) {
        // the other player's body: its copy here, the nearest one; ours again while there is none
        CPed* other = me;
        float best = 1e9f;
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.ped && rp.ped->RwObject() && (rp.ped->GetPosition() - me->GetPosition()).Length() < best)
                other = rp.ped, best = (rp.ped->GetPosition() - me->GetPosition()).Length();
        if (!MakeBody(g_body[1], other)) pair = false;
    } else {
        FreeBody(g_body[1]);
    }
    int bodies = pair ? 2 : 1;
    bool hold = e && (e->flags & HOLD);
    PlayOn(g_body[0], e ? e->block : nullptr, e ? e->main : nullptr, hold);
    if (pair) PlayOn(g_body[1], e->block, e->other ? e->other : e->main, hold);

    float dt = std::clamp((float)(NowMs() - g_lastMs) / 1000.0f, 0.0f, 0.1f);
    g_lastMs = NowMs();
    // ours faces the camera; a pair faces each other across it, as far apart as they will stand
    float half = pair ? e->apart * 0.5f : 0.0f;
    CVector lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < bodies; ++i) {
        Body& b = g_body[i];
        CVector forward = pair ? CVector{i == 0 ? 1.0f : -1.0f, 0, 0} : CVector{0, 1, 0};
        RwMat m{};
        m.up = forward;  // a ped's forward is its Y
        m.at = {0, 0, 1};
        m.right = forward.Cross(m.at);
        m.pos = {pair ? (i == 0 ? -half : half) : 0.0f, 0, 0};
        m.flags = kOrthonormal;
        SetFrame(Field<void*>(b.clump, 0x04), m);  // the clump's frame (RwObject::parent)
        Call<0x4D34F0>(b.clump, dt, true);         // RpAnimBlendClumpUpdateAnimations
        Call<0x7C51D0>(b.hier);                    // RpHAnimHierarchyUpdateMatrices
        // CJ's skin has bones between the clavicles and the upper arms no animation moves: the game turns them after each
        // update (CPed::PreRender for the player's model, CCutsceneObject for CSPLAY's). Without it the shoulders stayed
        // where the skin was made with the arms out and stuck out as boxes (the preview of 03.11)
        if (b.shoulders) Call<0x5DF560>(b.clump);  // CPed::ShoulderBoneRotation
        Bones(b);
        lo = Min(lo, b.lo), hi = Max(hi, b.hi);
    }
    if (lo.x > hi.x) return;

    // where it faces: forward of the line from its right thigh to its left one, taken once 0.4 s into the emote (a dance
    // that spins does not turn the camera with it)
    if (g_seenFor != g_want || !g_seenSet) g_faceAtMs = NowMs() + 400;
    if (g_faceAtMs && NowMs() >= g_faceAtMs) {
        g_faceAtMs = 0;
        auto* m = Field<RwMat*>(g_body[0].hier, 0x08);
        int l = Call<0x7C51A0, int>(g_body[0].hier, 41), r = Call<0x7C51A0, int>(g_body[0].hier, 51);  // RpHAnimIDGetIndex
        CVector fwd = l >= 0 && r >= 0 ? (m[l].pos - m[r].pos).Cross({0, 0, 1}) : CVector{0, 1, 0};
        if (fwd.Length2D() > 0.01f) g_faceTo = std::atan2(fwd.x, fwd.y);
    }
    if (pair) g_faceTo = 0.0f;
    float turn = WrapAngle(g_faceTo - g_face);
    g_face = g_camSet ? WrapAngle(g_face + turn * (1.0f - std::exp(-dt * 5.0f))) : g_faceTo;

    // the camera, turned round the bodies by the player (a pair seen from its side): its axes, from the bodies to it
    float yaw = pair ? g_yaw - 0.45f : g_yaw + g_face;
    CVector back = CVector{std::sin(yaw), std::cos(yaw), kRise}.Normalized();
    CVector side = (back * -1.0f).Cross({0, 0, 1}).Normalized(), up = back.Cross(side);
    auto eachBone = [&](auto fn) {
        for (int i = 0; i < bodies; ++i) {
            int n = Field<int32_t>(g_body[i].hier, 0x04);
            auto* m = Field<RwMat*>(g_body[i].hier, 0x08);
            for (int k = 0; m && k < n; ++k) fn(m[k].pos);
        }
    };
    // the bones on the picture: across it, up it, toward the camera
    Seen now{1e9f, -1e9f, 1e9f, -1e9f};
    float z0 = 1e9f, z1 = -1e9f;
    eachBone([&](const CVector& p) {
        float x = p.Dot(side), y = p.Dot(up), z = p.Dot(back);
        now = {std::min(now.x0, x), std::max(now.x1, x), std::min(now.y0, y), std::max(now.y1, y)};
        z0 = std::min(z0, z), z1 = std::max(z1, z);
    });
    now = {now.x0 - 0.15f, now.x1 + 0.15f, now.y0 - 0.1f, now.y1 + 0.22f};  // the skin past the bones, the head over its bone
    // what it keeps whole: where they went the last moments. It grows at once and shrinks slowly, so a loop does not make
    // the picture breathe and a held pose is closed in on once the body is down; a turn starts it again
    if (!g_seenSet || g_seenFor != g_want || g_seenYaw != yaw) g_seen = now, g_seenSet = true, g_seenFor = g_want, g_seenYaw = yaw;
    float shrink = 1.0f - std::exp(-dt / 2.0f);
    g_seen = {std::min(now.x0, g_seen.x0 + (now.x0 - g_seen.x0) * shrink), std::max(now.x1, g_seen.x1 + (now.x1 - g_seen.x1) * shrink),
              std::min(now.y0, g_seen.y0 + (now.y0 - g_seen.y0) * shrink), std::max(now.y1, g_seen.y1 + (now.y1 - g_seen.y1) * shrink)};
    float xm = (g_seen.x0 + g_seen.x1) * 0.5f, ym = (g_seen.y0 + g_seen.y1) * 0.5f, zMid = (z0 + z1) * 0.5f, w = kView * kFill;
    CVector mid = side * xm + up * ym + back * zMid;
    // as far as the widest way needs, and a bone nearer than the middle (drawn larger) as far again as it needs
    float need = std::max(1.0f, std::max(g_seen.x1 - g_seen.x0, g_seen.y1 - g_seen.y0) * 0.5f / w);
    eachBone([&](const CVector& p) {
        need = std::max(need, std::max(std::fabs(p.Dot(side) - xm) + 0.15f, std::fabs(p.Dot(up) - ym) + 0.2f) / w + p.Dot(back) - zMid);
    });
    float ease = g_camSet ? 1.0f - std::exp(-dt * 5.0f) : 1.0f;
    g_camAt = g_camAt + (mid - g_camAt) * ease;
    g_camDist += (need - g_camDist) * ease;
    g_camSet = true;
    CVector eye = g_camAt + back * g_camDist;
    SetFrame(Field<void*>(g_cam, 0x04), Facing(g_camAt - eye, eye));
    float window[2] = {kView, kView};
    Call<0x7EE410>(g_cam, window);  // RwCameraSetViewWindow

    // the lights a ped is drawn with: a warm one from near the camera above, a cool one from behind on the other side for
    // the outline, a fill everywhere. The frame's own lights come back after
    void* world = Ref<void*>(0xC17038);
    void* direct = Ref<void*>(0xC886EC);  // pDirect
    void* lightFrame = direct ? Field<void*>(direct, 0x04) : nullptr;
    if (!lightFrame || !world) return;
    RwMat savedLight = Field<RwMat>(lightFrame, 0x10);
    uint8_t savedFlags = Field<uint8_t>(direct, 0x02);
    RwRGBAReal fill{0.5f, 0.52f, 0.56f, 1.0f}, key{0.85f, 0.8f, 0.72f, 1.0f};
    Call<0x735D50>(&fill);             // SetAmbientColours
    Call<0x735D70>(&key);              // SetDirectionalColours
    Field<uint8_t>(direct, 0x02) = 1;  // rpLIGHTLIGHTATOMICS (ActivateDirectional)
    CVector lamp = g_camAt + CVector{std::sin(yaw + 0.4f), std::cos(yaw + 0.4f), 0.7f} * g_camDist;
    SetFrame(lightFrame, Facing(g_camAt - lamp, lamp));
    CVector rim = CVector{std::sin(yaw - 2.0f), std::cos(yaw - 2.0f), 0.5f}.Normalized();  // toward it
    Call<0x7359E0>(world);                                               // RemoveExtraDirectionalLights: none left of the frame's
    Call<0x735840>(world, rim.x, rim.y, rim.z, 0.35f, 0.45f, 0.6f);      // AddAnExtraDirectionalLight

    RwRGBA background{0x0C, 0x11, 0x17, 0xFF};
    Call<0x7EE340>(g_cam, &background, 3);  // RwCameraClear(rwCAMERACLEARIMAGE | rwCAMERACLEARZ)
    if (Call<0x7EE190, void*>(g_cam)) {      // RwCameraBeginUpdate
        {
            States states;
            // the floor: a lighter patch under the bodies, each one's shadow on it
            Spot((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (hi.x - lo.x) * 0.5f + 1.0f, (hi.y - lo.y) * 0.5f + 1.0f, 0xFF1C2733);
            for (int i = 0; i < bodies; ++i) {
                const Body& b = g_body[i];
                Spot((b.lo.x + b.hi.x) * 0.5f, (b.lo.y + b.hi.y) * 0.5f, (b.hi.x - b.lo.x) * 0.5f + 0.18f,
                     (b.hi.y - b.lo.y) * 0.5f + 0.18f, 0xB4000000);
            }
            States::Set(8, 1);   // the bodies write their depth
            States::Set(12, 0);  // and blend only where their materials ask
            for (int i = 0; i < bodies; ++i) Call<0x749B70>(g_body[i].clump, (void*)&RenderAtomic, nullptr);  // RpClumpForAllAtomics
        }
        Call<0x7EE180>(g_cam);  // RwCameraEndUpdate
        g_drawn = true;
    }
    Call<0x7359E0>(world);
    Field<uint8_t>(direct, 0x02) = savedFlags;
    SetFrame(lightFrame, savedLight);
    Call<0x735C40>();  // ReSetAmbientAndDirectionalColours
}

// after the frame's camera is done (Idle -> RwCameraEndUpdate, Core's hook before ours), before the picture goes to the
// screen: the preview drawn now shows in the window next frame
using EndUpdateFn = void*(__cdecl*)(void*);
EndUpdateFn g_origEndUpdate = nullptr;
void* __cdecl HookEndUpdate(void* camera) {
    void* r = g_origEndUpdate(camera);
    if (g_wantMs && NowMs() - g_wantMs < 250) Draw();
    return r;
}

class AnimPreview : public Module {
public:
    const char* Name() const override { return "AnimPreview"; }
    void OnInstallHooks() override { mem::RedirectCallFn(0x53EBF5, &HookEndUpdate, &g_origEndUpdate); }
    void OnPostProcess() override {
        // the window stopped asking without closing (the game ended under it): let go
        if (g_wantMs && NowMs() - g_wantMs > 2000) PreviewHide();
    }
    void OnGameShutdown() override { PreviewHide(); }
};
AnimPreview g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

void PreviewShow(const Emote* e, float pixels) {
    if (!g_wantMs) g_lastMs = NowMs();
    g_want = e ? e->id : 0;
    g_pixels = pixels;
    g_wantMs = NowMs();
}

void PreviewHide() {
    FreeAll();
    g_wantMs = 0;
}

void* PreviewTexture() {
    void* raster = g_cam && g_drawn ? Field<void*>(g_cam, 0x60) : nullptr;
    return raster ? Field<void*>(raster, Ref<int32_t>(0xB4E9E0)) : nullptr;  // _rwD3D9RasterExt::texture (_RwD3D9RasterExtOffset)
}

void PreviewTurn(float radians) { g_yaw = WrapAngle(g_yaw + radians); }

bool PreviewHolds(int block) {
    for (const Body& b : g_body)
        for (void* a = anim::First(b.clump); a; a = anim::Next(a)) {
            int blk = -1, idx = 0;
            if (anim::Locate(anim::Hierarchy(a), blk, idx) && blk == block) return true;
        }
    return false;
}

}  // namespace coop::anims
