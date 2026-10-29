#include "shared/game/Game.h"

#include <cstring>

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
    }
    return at;
}
bool IsModelLoaded(int model) {
    // models, then txds, cols, ipls, dats, ifps, rrrs and scms (26316 in all)
    return model >= 0 && model < 26316 && Ref<uint8_t>(LoadStates() + (uintptr_t)model * 0x14) == 1;
}
int ModelLoadState(int model) { return model >= 0 && model < 26316 ? Ref<uint8_t>(LoadStates() + (uintptr_t)model * 0x14) : -1; }
bool LoadModelNow(int model) {
    if (!IsValidModel(model)) return false;
    if (IsModelLoaded(model)) return true;
    RequestModel(model, 2 | 0x10 /* GAME_REQUIRED | PRIORITY_REQUEST */);
    LoadAllRequestedModels(false);
    return IsModelLoaded(model);
}
void MarkModelNotNeeded(int model) {
    // CStreaming::SetModelIsDeletable
    if (IsValidModel(model)) Call<0x409C10>(model);
}

static void* ModelInfo(int model) {
    if (model < 0 || model >= 20000) return nullptr;
    return Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)model * 4);
}
static int ModelType(int model) {
    void* mi = ModelInfo(model);
    return mi ? CallVirtual<4, uint8_t>(mi) : 0;
}
bool IsValidModel(int model) { return ModelInfo(model) != nullptr; }
bool IsVehicleModel(int model) { return ModelType(model) == 6; }
bool IsPedModel(int model) { return ModelType(model) == 7; }
bool IsObjectModel(int model) { return IsValidModel(model) && !IsVehicleModel(model) && !IsPedModel(model); }

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
