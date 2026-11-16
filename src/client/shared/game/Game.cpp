#include "shared/game/Game.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/lib/log/Log.h"

namespace game {

CPoolHeader* PedPool() { return Ref<CPoolHeader*>(addr::PedPool); }
CPoolHeader* VehiclePool() { return Ref<CPoolHeader*>(addr::VehiclePool); }
CPoolHeader* ObjectPool() { return Ref<CPoolHeader*>(addr::ObjectPool); }

int GetPedRef(CPed* ped) { return ped ? Call<0x54FF60, int>(ped) : -1; }
CPed* GetPed(int handle) { return handle < 0 ? nullptr : Call<0x54FF90, CPed*>(handle); }
int GetVehicleRef(CVehicle* v) { return v ? Call<0x54FFC0, int>(v) : -1; }
CVehicle* GetVehicle(int handle) { return handle < 0 ? nullptr : Call<0x54FFF0, CVehicle*>(handle); }
int GetObjectRef(CObject* o) { return o ? Call<0x550020, int>(o) : -1; }
CObject* GetObject(int handle) { return handle < 0 ? nullptr : Call<0x550050, CObject*>(handle); }

static bool IsInPool(CPoolHeader* pool, void* p, int elemSize) {
    if (!pool || !p) return false;
    uintptr_t base = (uintptr_t)pool->objects;
    uintptr_t addr = (uintptr_t)p;
    if (addr < base) return false;
    uintptr_t off = addr - base;
    if (off % (uintptr_t)elemSize) return false;
    uintptr_t idx = off / (uintptr_t)elemSize;
    if (idx >= (uintptr_t)pool->size) return false;
    return pool->byteMap[idx] >= 0;  // bit 7 clear = slot in use
}

bool IsPedValid(CPed* ped) { return IsInPool(PedPool(), ped, kPedPoolElemSize); }
bool IsVehicleValid(CVehicle* v) { return IsInPool(VehiclePool(), v, kVehiclePoolElemSize); }
bool IsObjectValid(CObject* o) { return IsInPool(ObjectPool(), o, kObjectPoolElemSize); }

bool HasRoom(CPoolHeader* pool, int keep, const char* what) {
    if (!pool) return false;
    int free = 0;
    for (int i = 0; i < pool->size; ++i) free += pool->byteMap[i] < 0;
    if (free > keep) return true;
    static uint32_t warnedMs = 0;
    if (TimeMs() - warnedMs > 10000) {
        warnedMs = TimeMs();
        LOGW("The %s pool is nearly full (%d of %d free): copies wait", what, free, pool->size);
    }
    return false;
}

void RequestModel(int model, int flags) { Call<0x4087E0>(model, flags); }
void LoadAllRequestedModels(bool onlyPriority) { Call<0x40EA10>(onlyPriority); }
// CStreaming::ms_aInfoForModel's load states where the game itself reads them: the game's HAS_MODEL_LOADED does it at
// 0x47EFFF (cmp byte [edx*4 + 0x8E4CD0], 1). A limit adjuster (fastman92's) moves the table and corrects the game's
// places: its old place read "not loaded" for good, and no copy of the other game's people, cars or props was ever made
// (#82, in both games).
static uintptr_t LoadStates() {
    static const uint8_t kCmp[] = {0x80, 0x3C, 0x95};  // cmp byte [edx*4 + disp32], imm8
    uintptr_t at = memcmp((const void*)0x47EFFF, kCmp, sizeof kCmp) ? addr::StreamingInfo + 0x10 : Ref<uint32_t>(0x47F002);
    static bool told = false;
    if (!told) {
        told = true;
        if (at != addr::StreamingInfo + 0x10) LOGI("The streaming table is at %08X, not at 008E4CC0: a limit adjuster moved it", at - 0x10);
        ModelTablesKnown();  // says once where the model tables are
    }
    return at;
}
// an imm32 after the opcode bytes at `at`, or the game's own value when the bytes differ (ok cleared)
static uint32_t ImmAfter(uintptr_t at, std::initializer_list<uint8_t> op, uint32_t own, bool* ok = nullptr) {
    bool same = !memcmp((const void*)at, op.begin(), op.size());
    if (ok) *ok = *ok && same;
    return same ? Ref<uint32_t>(at + op.size()) : own;
}
// a count read from the code: the game's when it is out of reason
static int Sane(uint32_t v, uint32_t own) { return v >= own && v <= 0x40000 ? (int)v : (int)own; }

uintptr_t ModelInfoTable() { return ImmAfter(0x4089CF, {0x8B, 0x1C, 0xB5}, addr::ModelInfoPtrs); }  // mov ebx, [esi*4 + table]
int ModelCount() { return Sane(ImmAfter(0x4089C6, {0x81, 0xFE}, 20000), 20000); }                     // cmp esi, models
int IfpBase() { return Sane(ImmAfter(0x408A76, {0x81, 0xFE}, 25575), 25575); }                        // cmp esi, first IFP
int ResourceCount() { return Sane(ImmAfter(0x408AAA, {0x81, 0xFE}, 26230), 26230) + 86; }              // cmp esi, first SCM
uintptr_t AnimBlocksTable() { return ImmAfter(0x4D39A5, {0x2D}, 0xB5D4A0); }                         // sub eax, ms_aAnimBlocks
uintptr_t AnimationsTable() { return ImmAfter(0x4CDE57, {0x8D, 0x04, 0xC5}, 0xB4EA40); }             // lea eax, [eax*8 + table]

bool ModelTablesKnown() {
    bool ok = true;
    ImmAfter(0x4089CF, {0x8B, 0x1C, 0xB5}, 0, &ok);
    ImmAfter(0x4089C6, {0x81, 0xFE}, 0, &ok);
    ImmAfter(0x408A76, {0x81, 0xFE}, 0, &ok);
    ImmAfter(0x408AAA, {0x81, 0xFE}, 0, &ok);
    static bool told = false;
    if (!told) {
        told = true;
        if (!ok) LOGW("The game's model code is not as expected (another mod replaced it): the game's own model tables are used");
        else if (ModelInfoTable() != addr::ModelInfoPtrs || ModelCount() != 20000 || IfpBase() != 25575)
            LOGI("Model tables moved by a limit adjuster: infos at %08X, %d models, IFPs from %d, %d resources, anim blocks at %08X",
                 (unsigned)ModelInfoTable(), ModelCount(), IfpBase(), ResourceCount(), (unsigned)AnimBlocksTable());
    }
    return ok;
}

bool IsModelLoaded(int model) {
    // models, then txds, cols, ipls, dats, ifps, rrrs and scms
    return model >= 0 && model < ResourceCount() && Ref<uint8_t>(LoadStates() + (uintptr_t)model * 0x14) == 1;
}
int ModelLoadState(int model) { return model >= 0 && model < ResourceCount() ? Ref<uint8_t>(LoadStates() + (uintptr_t)model * 0x14) : -1; }
uint8_t* StreamingInfoOf(int model) {
    return model >= 0 && model < ResourceCount() ? (uint8_t*)(LoadStates() - 0x10 + (uintptr_t)model * 0x14) : nullptr;
}
bool LoadModelNow(int model) {
    if (!IsValidModel(model)) return false;
    if (IsModelLoaded(model)) return true;
    RequestModel(model, 2 | 0x10 /* GAME_REQUIRED | PRIORITY_REQUEST */);
    LoadAllRequestedModels(false);
    return IsModelLoaded(model);
}
// A copy's model. A plain request does not last here: each frame CStreaming::StreamVehiclesAndPeds lets go of the police, FBI
// and army models no wanted level of ours calls for, and that removes a model still only asked for (SetModelIsDeletable), a
// request's PRIORITY flag does not save it. In the other game they stand on a mission's request. The soldiers of "Robbing
// Uncle Sam" never came, their Patriots drove empty (found 04.11). KEEP_IN_MEMORY is what the game streams its own
// cars and people with: it survives that and PurgeRequestList, and once loaded the model is an ordinary one, nothing to let go.
void RequestCopyModel(int model, bool mission) { RequestModel(model, mission ? 0x8 | 0x10 /* first */ : 0x8); }

// A player going through a door: for the ~2 s of the fade the game parks up to 32 things around the door in area 13
// (CEntryExitManager::SetAreaCodeForVisibleObjects) and puts them back after it. Area 13 is a real interior too, so read
// plainly an empty car by the door stood where no player is: it left the network, the other game took its copy over, and
// after the fade both games had two (a car at Binco, found 04.11).
int8_t TrueArea(CEntity* e) {
    auto* list = (CEntity**)0x96A738;  // ms_visibleEntityList
    for (int i = 0, n = std::min(Ref<int32_t>(0x96A7DC) /* ms_numVisibleEntities */, 32); i < n; ++i)
        if (list[i] == e) return (int8_t)Ref<int32_t>(0x96A734);  // ms_oldAreaCode
    return e->AreaCode();
}

void MarkModelNotNeeded(int model) {
    // CStreaming::SetModelIsDeletable
    if (IsValidModel(model)) Call<0x409C10>(model);
}

void* ModelInfoOf(int model) {
    if (model < 0 || model >= ModelCount()) return nullptr;
    return Ref<void*>(ModelInfoTable() + (uintptr_t)model * 4);
}
static int ModelType(int model) {
    void* mi = ModelInfoOf(model);
    return mi ? CallVirtual<4, uint8_t>(mi) : 0;
}
bool IsValidModel(int model) { return ModelInfoOf(model) != nullptr; }
bool IsVehicleModel(int model) { return ModelType(model) == 6; }
bool IsPedModel(int model) { return ModelType(model) == 7; }
bool IsObjectModel(int model) { return IsValidModel(model) && !IsVehicleModel(model) && !IsPedModel(model); }

// CJ's csplay and player have bones 301 and 302 where the upper arms are; the game's people have them 16-20 cm away on the
// chest, and the skin packs' 5-95 cm (four of the beta pack are rigged as CJ). The upper arm's matrix copied into them
// (CPed::ShoulderBoneRotation) pulls the chest up to the shoulders, so that turn is for a model rigged as CJ alone. (The
// skins crooked in the cutscenes on 11.11 had another cause: features/skins, HookSetCutsceneAnim.) Read from the skin's
// own bones at rest: its skin-to-bone matrices (right, up, at, pos; a bone's origin is -pos in its axes).
bool ShoulderHelpers(void* clump) {
    void* atomic = clump ? Call<0x734820, void*>(clump) : nullptr;                     // GetFirstAtomic
    void* skin = atomic ? Call<0x7C7550, void*>(Field<void*>(atomic, 0x18)) : nullptr;  // RpSkinGeometryGetSkin(geometry)
    void* hier = skin ? Call<0x734A40, void*>(clump) : nullptr;                        // GetAnimHierarchyFromSkinClump
    auto* m = hier ? Call<0x7C7810, const float*>(skin) : nullptr;                     // RpSkinGetSkinToBoneMatrices
    int bones = m ? Call<0x7C77E0, int>(skin) : 0;                                     // RpSkinGetNumBones
    auto origin = [&](int bone, CVector& p) {
        int i = Call<0x7C51A0, int>(hier, bone);  // RpHAnimIDGetIndex: the skin's bones are in the hierarchy's order
        if (i < 0 || i >= bones) return false;
        const float* r = m + i * 16;
        p = {-(r[12] * r[0] + r[13] * r[1] + r[14] * r[2]), -(r[12] * r[4] + r[13] * r[5] + r[14] * r[6]),
             -(r[12] * r[8] + r[13] * r[9] + r[14] * r[10])};
        return true;
    };
    CVector arm, breast;
    for (auto [a, b] : {std::pair{22, 301}, std::pair{32, 302}})
        if (!origin(a, arm) || !origin(b, breast) || (arm - breast).Length() > 0.01f) return false;
    return true;
}

// CAnimManager::ms_aAnimAssocGroups (0x14 bytes each: the block, its associations) by the number of animgrp.dat's sets;
// block 0 is ped.ifp, read first (CAnimManager::LoadAnimFiles)
bool PedWalkGroup(int group) {
    if (group < 0 || group >= (int)Ref<uint32_t>(0xB4EA28)) return false;  // ms_numAnimAssocDefinitions
    uintptr_t g = Ref<uintptr_t>(0xB4EA34) + (uintptr_t)group * 0x14;
    uintptr_t ped = AnimBlocksTable();
    return Ref<uintptr_t>(g) == ped && Ref<void*>(g + 0x04) && Ref<uint8_t>(ped + 0x10);  // CAnimBlock::bLoaded
}
// ms_aAnimAssocDefinitions (0x30 bytes each, the name first): the game's 118 and animgrp.dat's after them (man is 118,
// woman 129, skate 138), as many as ms_numAnimAssocDefinitions says
int WalkGroupByName(const char* name) {
    int n = std::min<int>((int)Ref<uint32_t>(0xB4EA28), 200);
    for (int i = 0; name && *name && i < n; ++i)
        if (!_strnicmp((const char*)(0x8AA5A8 + (uintptr_t)i * 0x30), name, 16)) return i;
    return -1;
}

void WorldAdd(CEntity* e) { Call<0x563220>(e); }

void SetEntityPosition(CEntity* e, const CVector& pos) {
    if (!e) return;
    e->SetPositionRaw(pos);
    CallMethod<0x446F90>(e);  // CEntity::UpdateRW
    CallMethod<0x532B00>(e);  // CEntity::UpdateRwFrame
}

bool WorldToScreen(const CVector& world, float& sx, float& sy, float& depth) {
    RwV3d in{world.x, world.y, world.z}, out{};
    float w = 0, h = 0;
    // CSprite::CalcScreenCoors(const RwV3d& in, RwV3d* out, float* w, float* h, bool checkMaxVisible, bool checkMinVisible)
    // clip checks disabled: CDraw near/far values are not valid at the end of the frame; depth is checked here
    Call<0x70CE30, bool>(&in, &out, &w, &h, false, false);
    sx = out.x;
    sy = out.y;
    depth = out.z;
    return out.z > 1.0f && out.z < 1000.0f;
}
void WorldRemove(CEntity* e) { Call<0x563280>(e); }

CVector DoorSpot(void* enex) {
    const float* r = &Ref<float>((uintptr_t)enex + 0x08);  // CEntryExit::m_recEntrance
    return CVector{(r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f, Ref<float>((uintptr_t)enex + 0x18)};  // m_fEntranceZ
}

void* StreetDoorOf(int area, const CVector& inside, const CVector& hint) {
    auto* pool = Ref<CPoolHeader*>(0x96A7D8);  // CEntryExitManager::mp_poolEntryExits
    void* best = nullptr;
    float bestD = 1e30f;
    for (int i = 0; area != 0 && pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        uintptr_t e = (uintptr_t)pool->objects + (uintptr_t)i * 0x3C;
        uintptr_t link = Ref<uintptr_t>(e + 0x38);  // m_pLink: the door on the other side
        if (Ref<int8_t>(e + 0x32) != 0 || !link || Ref<int8_t>(link + 0x32) != area) continue;  // m_nArea: on the street
        CVector in = DoorSpot((void*)link);
        if ((in - inside).Length2D() > 150.0f) continue;  // another building of that area
        float d = (DoorSpot((void*)e) - hint).Length2D();
        if (d < bestD) bestD = d, best = (void*)e;
    }
    return best;
}

void LightArea(int area, const CVector& pos) {
    int sky = 0;
    float best = 150.0f * 150.0f;
    auto* pool = Ref<CPoolHeader*>(0x96A7D8);  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
    for (int i = 0; area != 0 && pool && i < pool->size; ++i) {
        uintptr_t e = (uintptr_t)pool->objects + (uintptr_t)i * 0x3C;
        if (pool->byteMap[i] < 0 || Ref<int8_t>(e + 0x32) != area) continue;  // m_nArea: the door is in this interior
        const float* r = &Ref<float>(e + 0x08);                                 // m_recEntrance
        float dx = (r[0] + r[2]) * 0.5f - pos.x, dy = (r[1] + r[3]) * 0.5f - pos.y;
        if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, sky = Ref<uint8_t>(e + 0x33);  // m_nSkyColor
    }
    if (sky) Call<0x55FEC0>(sky - 1, false);  // CTimeCycle::StartExtraColour
    else Call<0x55FF20>(false);              // CTimeCycle::StopExtraColour
}

}  // namespace game
