#include "entities/vehicle/Vehicles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Vehicles& Vehicles::Instance() {
    static Vehicles v;
    return v;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Vehicles::Instance()); }
} g_reg;

constexpr uint8_t STATUS_PHYSICS = 3, STATUS_WRECKED = 5;
constexpr uint8_t VEHICLE_CREATED_RANDOM = 1, VEHICLE_CREATED_MISSION = 2, VEHICLE_CREATED_PARKED = 3;
constexpr float kTrafficIn = 170.0f;   // owned vehicles closer than this to the partner are announced (> car spawn range)
constexpr float kTrafficOut = 220.0f;  // and dropped from the network beyond this
// an aircraft is seen from much farther: a police helicopter over the other player, 60-100 m up, came and went for a
// partner a little way off (Hh hh, 26.10)
constexpr float kAirIn = 350.0f, kAirOut = 420.0f;
bool Aircraft(CVehicle* v) {
    int type = v->VehicleSubClass();
    return type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_FHELI || type == VEHICLE_TYPE_FPLANE;
}

void SetVehicleCreatedBy(CVehicle* v, uint8_t by) { CallMethod<0x6D5D70>(v, (int)by); }  // keeps CCarCtrl counters right

// A script of this game holds the car: its mission's list (CTheScripts::MissionCleanUp, 75 x {type, handle}, 1 a car)
// or the car STORE_CAR_CHAR_IS_IN stored (CTheScripts::StoreVehicleIndex). Such a car stays a mission car here.
bool ScriptHolds(CVehicle* v) {
    int h = GetVehicleRef(v);
    if (h == Ref<int32_t>(0xA447EC)) return true;
    for (int i = 0; i < 75; ++i)
        if (Ref<uint8_t>(0xA90850 + (uintptr_t)i * 8) == 1 && Ref<int32_t>(0xA90850 + (uintptr_t)i * 8 + 4) == h) return true;
    return false;
}

bool HasPlayerInside(CVehicle* v) {
    if (v->Driver() && v->Driver()->IsPlayer()) return true;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] && v->Passengers()[i]->IsPlayer()) return true;
    return false;
}

void SetVehicleMatrix(CVehicle* v, const CVector& pos, const Quat& q) {
    CMatrix* m = v->Matrix();
    if (!m) return;
    q.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(v);  // CEntity::UpdateRW
    CallMethod<0x532B00>(v);  // CEntity::UpdateRwFrame
}
constexpr int kDamageManager = 0x5A0, kBikeTyres = 0x65C;  // CAutomobile::m_damageManager, CBike::m_nWheelStatus

bool HasDamageManager(CVehicle* v) {  // CAutomobile and its subclasses
    switch (v->VehicleSubClass()) {
    case VEHICLE_TYPE_AUTOMOBILE: case VEHICLE_TYPE_MTRUCK: case VEHICLE_TYPE_QUAD: case VEHICLE_TYPE_HELI:
    case VEHICLE_TYPE_PLANE: case VEHICLE_TYPE_TRAILER: return true;
    }
    return false;
}

// shows the damage of the owner's vehicle: parts are reset, then rebuilt from the damage state the way
// a loaded save does it (no debris flies off, the parts are just gone or bent)
void ApplyDamage(CVehicle* v, const uint8_t (&damage)[20]) {
    uint8_t* dm = (uint8_t*)v + kDamageManager + 4;
    if (!memcmp(dm, damage, sizeof(damage))) return;
    CallMethod<0x6A3440>(v);  // CAutomobile::Fix
    memcpy(dm, damage, sizeof(damage));
    switch (v->VehicleSubClass()) {  // SetupDamageAfterLoad of the exact class
    case VEHICLE_TYPE_HELI: CallMethod<0x6C4550>(v); break;
    case VEHICLE_TYPE_PLANE: CallMethod<0x6CAC10>(v); break;
    case VEHICLE_TYPE_QUAD: CallMethod<0x6CE340>(v); break;
    default: CallMethod<0x6B3E90>(v); break;
    }
}

VehicleLook CaptureLook(CVehicle* v) {
    VehicleLook l;
    if (HasDamageManager(v)) memcpy(l.damage, (uint8_t*)v + kDamageManager + 4, sizeof(l.damage));
    if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy(l.bikeTyres, (uint8_t*)v + kBikeTyres, 2);
    memcpy(l.colors, v->Colors(), 4);
    memcpy(l.upgrades, v->Upgrades(), sizeof(l.upgrades));
    l.lock = (uint8_t)v->DoorLock();
    int pj = -1;
    Cmd(0x0988).I(GetVehicleRef(v)).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    l.paintjob = (int8_t)pj;
    return l;
}
}  // namespace

bool VehicleLook::operator==(const VehicleLook& o) const {
    return !memcmp(colors, o.colors, 4) && !memcmp(upgrades, o.upgrades, sizeof(upgrades)) && paintjob == o.paintjob &&
           !memcmp(damage, o.damage, sizeof(damage)) && !memcmp(bikeTyres, o.bikeTyres, 2) && lock == o.lock;
}

bool IsSyncableVehicle(CVehicle* v) { return !v->IsTrain(); }

// A mirror has the AI-driven status (STATUS_PHYSICS), so every physics step ran its AI steering
// (CCarCtrl::SteerAICarWithPhysics): with no mission it brakes and pulls the handbrake, and the next frame's
// network speed pushed it on, so a mirror lurched like a car that keeps braking. Mirrors are driven by the network.
using SteerFn = void(__cdecl*)(CVehicle*);
SteerFn g_origSteerAI = nullptr;
void __cdecl HookSteerAICarWithPhysics(CVehicle* v) {
    if (Net().Online() && v)
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) return;
    g_origSteerAI(v);
}

// CHeli::HeliSearchLights (0xC1C990) has 4 places and CHeli::AddHeliSearchLight does not check: with the other game's
// helicopters copied here a night chase can light more, and a 5th wrote over what follows the array
using AddSearchLightFn = void(__cdecl*)(const CVector&, const CVector&, float, float, uint32_t, uint8_t, uint8_t);
AddSearchLightFn g_origAddSearchLight = nullptr;
void __cdecl HookAddSearchLight(const CVector& from, const CVector& to, float radius, float power, uint32_t corona,
                                uint8_t clip, uint8_t shadow) {
    if (Ref<uint32_t>(0xC1C96C) < 4) g_origAddSearchLight(from, to, radius, power, corona, clip, shadow);  // NumberOfSearchLights
}

// The horn sounds for as long as the counter is up only in the player's own car (STATUS_PLAYER); in any other car the
// audio plays a random honking pattern. A mirror honks while its owner's horn does, as the owner hears it.
using HornStateFn = void(__fastcall*)(void*, void*, bool*, void*);
HornStateFn g_origHornState = nullptr;
void __fastcall HookGetHornState(void* audio, void* edx, bool* on, void* params) {
    g_origHornState(audio, edx, on, params);
    if (*on || !Net().Online()) return;
    auto* v = Field<CVehicle*>(params, 0x10);  // tVehicleParams::Vehicle
    if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv) && nv->hasState)
        *on = (nv->state.flags & VehicleState::F_HORN) != 0;
}

void Vehicles::OnInstallHooks() {
    mem::HookFn(0x437C20, &HookSteerAICarWithPhysics, &g_origSteerAI, "CCarCtrl::SteerAICarWithPhysics");
    mem::HookFn(0x4F61E0, &HookGetHornState, &g_origHornState, "CAEVehicleAudioEntity::GetHornState");
    mem::HookFn(0x6C45B0, &HookAddSearchLight, &g_origAddSearchLight, "CHeli::AddHeliSearchLight");
    Net().SetHandler(proto::G_VEH_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_VEH_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_VEH_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_VEH_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_VEH_STATUS, [](uint8_t from, Reader& r) { Instance().HandleStatus(from, r); });
    Net().SetHandler(proto::G_VEH_HANDOVER, [](uint8_t from, Reader& r) { Instance().HandleHandover(from, r); });
    Net().SetHandler(proto::G_VEH_SCRIPT_FREE, [](uint8_t from, Reader& r) { Instance().HandleScriptFree(from, r); });
    Net().SetHandler(proto::G_VEH_CLEAR, [](uint8_t from, Reader& r) { Instance().HandleClear(from, r); });
}

NetVehicle* Vehicles::Find(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it == m_byId.end() ? nullptr : &it->second;
}

NetVehicle* Vehicles::Find(CVehicle* v) {
    auto it = m_byPtr.find(v);
    return it == m_byPtr.end() ? nullptr : Find(it->second);
}

bool Vehicles::IsMirror(const NetVehicle& nv) const { return nv.owner != Net().MyId(); }

NetVehicle* Vehicles::EnsureNetworked(CVehicle* v) {
    // a train is the Trains module's (CREATE_CAR made a train off its rails in the other game): a player getting into its
    // cab announced it as a car
    if (!v || !IsVehicleValid(v) || !IsSyncableVehicle(v)) return nullptr;
    if (NetVehicle* nv = Find(v)) return nv;
    if (!Net().Online()) return nullptr;
    uint16_t id;
    do {
        id = (uint16_t)(((Net().MyId() + 1) << 12) | (m_nextId++ & 0x0FFF));
    } while (m_byId.count(id));
    NetVehicle& nv = m_byId[id];
    nv.netId = id;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = Net().MyId();
    nv.model = v->ModelIndex();
    nv.lastChangeMs = NowMs();
    m_byPtr[v] = id;
    Announce(nv);
    LOGI("Vehicle %04X (model %d) is now networked", id, nv.model);
    return &nv;
}

void Vehicles::Announce(const NetVehicle& nv) {
    CVehicle* v = nv.veh;
    VehicleSpawn sp;
    sp.netId = nv.netId;
    sp.model = nv.model;
    sp.owner = nv.owner;
    sp.pos = v->GetPosition();
    if (CMatrix* m = v->Matrix()) sp.rot = Quat::FromMatrix(*m);
    memcpy(sp.colors, v->Colors(), 4);
    sp.health = v->Health();
    sp.area = v->AreaCode();
    sp.lockState = (uint8_t)v->DoorLock();
    sp.script = nv.createdByUs ? nv.script : v->CreatedBy() == VEHICLE_CREATED_MISSION;
    sp.extras[0] = Field<int8_t>(v, 0x438), sp.extras[1] = Field<int8_t>(v, 0x439);  // CVehicle::m_anExtras
    Writer w(proto::G_VEH_CREATE);
    sp.Write(w);
    Net().Send(proto::CH_RELIABLE, w);
}

void Vehicles::Reannounce() {
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh)) {
            Announce(nv);
            SendLook(nv);
        }
}

void Vehicles::SendLook(NetVehicle& nv) {
    nv.look = CaptureLook(nv.veh);
    nv.hasLook = true;
    Writer w(proto::G_VEH_STATUS);
    w.U16(nv.netId);
    w.Raw(nv.look.colors, 4);
    for (int16_t u : nv.look.upgrades) w.I16(u);
    w.I8(nv.look.paintjob);
    w.Raw(nv.look.damage, sizeof(nv.look.damage));
    w.Raw(nv.look.bikeTyres, 2);
    w.U8(nv.look.lock);
    Net().Send(proto::CH_RELIABLE, w);
}

void Vehicles::HandleStatus(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleLook l;
    r.Raw(l.colors, 4);
    for (int16_t& u : l.upgrades) u = r.I16();
    l.paintjob = r.I8();
    r.Raw(l.damage, sizeof(l.damage));
    r.Raw(l.bikeTyres, 2);
    l.lock = r.U8();
    NetVehicle* nv = Find(id);
    if (!r.Ok() || !nv || nv->owner != from) return;
    nv->look = l;
    nv->hasLook = true;
}

// mod shop work done in the owner's game: same parts (models streamed in first), paintjob, colours
void Vehicles::ApplyLook(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    memcpy(v->Colors(), nv.look.colors, 4);
    int h = nv.handle;
    for (int slot = 0; slot < 15; ++slot) {
        int16_t want = nv.look.upgrades[slot], have = v->Upgrades()[slot];
        if (want == have) continue;
        if (have >= 0) Cmd(0x06E8).I(h).I(have).Run();  // REMOVE_VEHICLE_MOD
        // a part of a mod this game lacks crashes the streamer; IsValidModForVehicle: what a mod shop here offers it
        if (want < 0 || !IsValidModel(want) || !Call<0x49B010, bool>(want, v)) continue;
        Cmd(0x06E9).I(want).Run();                          // REQUEST_VEHICLE_MOD
        if (!Cmd(0x06EA).I(want).Run()) continue;           // HAS_VEHICLE_MOD_LOADED: try again next frame
        int dummy = 0;
        Cmd(0x06E7).I(h).I(want).Out(&dummy).Run();         // ADD_VEHICLE_MOD
    }
    int pj = -1;
    Cmd(0x0988).I(h).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    // CVehicle::SetRemap reads the model's four paintjobs at that index, unchecked
    if (nv.look.paintjob >= 0 && nv.look.paintjob < 4 && pj != nv.look.paintjob)
        Cmd(0x06ED).I(h).I(nv.look.paintjob).Run();  // GIVE_VEHICLE_PAINTJOB
    if (HasDamageManager(v)) ApplyDamage(v, nv.look.damage);
    else if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy((uint8_t*)v + kBikeTyres, nv.look.bikeTyres, 2);
    // the lock it had when it became known stayed: a mission car locked then held the partner who got in, after the
    // mission it was opened only in the owner's game (the Savanna of "Cesar Vialpando"); same rule as at the spawn
    v->DoorLock() = nv.look.lock == 2 && !nv.script ? 1 : nv.look.lock;
}

void Vehicles::TakeOwnership(NetVehicle& nv) {
    if (nv.owner == Net().MyId()) return;
    nv.owner = Net().MyId();
    nv.hasState = false;
    nv.lastChangeMs = NowMs();
    Release(nv);
    Writer w(proto::G_VEH_OWNER);
    w.U16(nv.netId);
    w.U8(nv.owner);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Took ownership of vehicle %04X", nv.netId);
}

CVehicle* Vehicles::FindAdoptable(uint16_t model, const CVector& pos) {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* best = nullptr;
    float bestD = 3.0f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->ModelIndex() != model || m_byPtr.count(v)) continue;
        if (me && me->InVehicle() && me->Vehicle() == v) continue;
        // only a car generator's (CCarGenerator::DoInternalProcessing makes them PARKED): both games have it at the same
        // spot. A script's car stays its own (released as an ordinary car later, its script lost it and crashed), and the
        // traffic's or a trainer's (RANDOM) is only a look-alike of the other game's car: it was taken over (#16)
        if (v->CreatedBy() != VEHICLE_CREATED_PARKED) continue;
        // a parked car both games have (a car generator's): one with people in it or on the move is this game's traffic,
        // its driver kept driving it against the copy's states
        if (v->Driver() || v->NumPassengers() || v->MoveSpeed().Length() > 0.01f) continue;
        float d = (v->GetPosition() - pos).Length();
        if (d < bestD) {
            bestD = d;
            best = v;
        }
    }
    return best;
}

// The vehicle pool's band between the game's own reserve (CCarCtrl::RemoveCarsIfThePoolGetsFull, 8) and ours held a
// copy back for good (a mission's cars among the last announced). It takes the place of our own empty random car the
// other game has no copy of, out of view, the farthest.
static bool FreeRandomCar() {
    CPoolHeader* pool = VehiclePool();
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* far = nullptr;
    float farD = 0.0f;
    for (int i = 0; pool && me && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->CreatedBy() != VEHICLE_CREATED_RANDOM || v->Driver() || v->NumPassengers() || Vehicles::Instance().Find(v) ||
            v->IsTrain() || CallMethod<0x534540, bool>(v))  // CEntity::GetIsOnScreen
            continue;
        if (float d = (v->GetPosition() - me->GetPosition()).Length(); d > farD) farD = d, far = v;
    }
    if (!far) return false;
    LOGD("Vehicle pool full: our empty car %.0f m away makes room for a copy", farD);
    Cmd(0x00A6).I(GetVehicleRef(far)).Run();  // DELETE_CAR
    return true;
}

CVehicle* Vehicles::SpawnMirror(const VehicleSpawn& sp) {
    if (!IsModelLoaded(sp.model)) {
        // streamed in the background, CreateMirror is retried every frame. A request with no flag lasts one streaming
        // update (CStreaming::PurgeRequestList at its end): a mission's car of a model this game had nowhere lost that race
        // for long, and the people in it waited for it ("Running Dog": the Vagos' car, "Mike Toreno": the van and the
        // bikers, 28.10). A script's car goes first; the flag clears once its read starts.
        RequestModel(sp.model, sp.script ? 0x10 /* PRIORITY_REQUEST */ : 0);
        return nullptr;
    }
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle") && !FreeRandomCar()) return nullptr;
    // its owner's components (a Picador's surfboard, a convertible's roof up or down): picked as its clump is made
    // (CVehicleModelInfo::ChooseComponent), at random unless asked for (ms_compsToUse, -2: at random). They differed
    // between the two games (27.10).
    auto* comps = (int8_t*)0x8A6458;  // CVehicleModelInfo::ms_compsToUse
    for (int i = 0; i < 2; ++i) comps[i] = sp.extras[i] >= -1 && sp.extras[i] <= 5 ? sp.extras[i] : -2;
    int handle = -1;
    Cmd(0x00A5).I(sp.model).F(sp.pos.x).F(sp.pos.y).F(sp.pos.z).Out(&handle).Run();  // CREATE_CAR
    comps[0] = comps[1] = -2;  // a model with no components takes none: not left for the next car
    CVehicle* v = GetVehicle(handle);
    if (!v) return nullptr;
    // CREATE_CAR marks the car as held by a script (CVehicle::bIsLocked), and a tow truck or a tractor never hooks such
    // a car (CAutomobile::TowTruckControl, 0x6A4366); a mission's car stays so, as in its owner's game
    if (!sp.script) v->VehicleFlagsLo() &= ~8u;
    SetVehicleMatrix(v, sp.pos, sp.rot);
    memcpy(v->Colors(), sp.colors, 4);
    v->Health() = sp.health;
    v->AreaCode() = sp.area;
    // a random car locked there does not lock the other player out; a mission's locked car stays shut for him too
    v->DoorLock() = sp.lockState == 2 && !sp.script ? 1 : sp.lockState;
    // the copy of a car the other game made in our view close by: what made it, for the next report of cars out of
    // nowhere (27.10)
    static uint64_t toldMs = 0;
    if (CPlayerPed* me = FindPlayerPed(0); me && NowMs() - toldMs > 10000 && CallMethod<0x534540, bool>(v)) {  // GetIsOnScreen
        if (float d = (sp.pos - me->GetPosition()).Length(); d < 25.0f) {
            toldMs = NowMs();
            LOGI("A copy of the other game's car (model %d) appeared %.0f m away in view", sp.model, d);
        }
    }
    return v;
}

void Vehicles::HandleCreate(uint8_t from, Reader& r) {
    VehicleSpawn sp;
    if (!sp.Read(r) || from == Net().MyId()) return;
    if (!core::IsPlayerReady()) return;
    if (NetVehicle* existing = Find(sp.netId)) {
        // announced again: its owner may have changed. Not a car of ours or another model: the id came twice (a game
        // that started over counts its ids from the start), and it handed our car to the other player.
        if (IsMirror(*existing) && existing->model == sp.model) existing->owner = sp.owner;
        return;
    }
    if (!IsVehicleModel(sp.model)) {
        LOGW("Vehicle %04X has an unknown model %d", sp.netId, sp.model);
        return;
    }
    if (CreateMirror(sp)) m_pending.erase(sp.netId);
    else m_pending[sp.netId] = sp;
}

bool Vehicles::CreateMirror(const VehicleSpawn& sp) {
    if (Find(sp.netId)) return true;  // announced again meanwhile and made already
    bool adopted = false;
    CVehicle* v = FindAdoptable(sp.model, sp.pos);
    if (v) {
        adopted = true;
        SetVehicleMatrix(v, sp.pos, sp.rot);
        memcpy(v->Colors(), sp.colors, 4);  // the same parked car can have other random colours here
        v->Health() = sp.health;
    } else {
        v = SpawnMirror(sp);
    }
    if (!v) return false;
    NetVehicle& nv = m_byId[sp.netId];
    nv.netId = sp.netId;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = sp.owner;
    nv.model = sp.model;
    nv.createdByUs = !adopted;
    nv.script = sp.script;
    if (adopted) Protect(nv);
    m_byPtr[v] = sp.netId;
    LOGI("Vehicle %04X (model %d) %s for player %d", sp.netId, sp.model, adopted ? "adopted" : "created", sp.owner);
    return true;
}

void Vehicles::Forget(uint16_t netId, bool deleteVehicle) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetVehicle& nv = it->second;
    if (deleteVehicle && nv.veh && IsVehicleValid(nv.veh)) {
        // a deleted car takes its occupants along: the other player's character gets out first
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.ped && IsPedValid(rp.ped) && rp.ped->InVehicle() && rp.ped->Vehicle() == nv.veh) {
                CVector p = rp.ped->GetPosition();
                Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            }
        StopEntering(nv.veh);
        Cmd(0x00A6).I(GetVehicleRef(nv.veh)).Run();  // DELETE_CAR
    }
    m_byPtr.erase(nv.veh);
    m_byId.erase(it);
}

// A mirror is the other game's car: this game's population and scripts leave it alone (a car of the population taken
// over by the other player was deleted here as soon as this player got far, with the other player's character in it).
void Vehicles::Protect(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    if (!IsVehicleValid(v) || v->CreatedBy() == VEHICLE_CREATED_MISSION) return;
    if (!nv.createdByUs && !nv.origBy) nv.origBy = v->CreatedBy();
    SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
}

// Ours again: what it was in this game before, and a mirror spawned here an ordinary car (a script's car of the other
// game stays a mission car: dropping it here would make the owner drop it in the middle of its mission)
void Vehicles::Release(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    if (nv.crashProofHere && IsVehicleValid(v)) v->PhysicalFlags() &= ~CPhysical::PF_COLLISION_PROOF;
    nv.crashProofHere = false;
    uint8_t by = nv.createdByUs ? (nv.script ? VEHICLE_CREATED_MISSION : VEHICLE_CREATED_RANDOM) : nv.origBy;
    if (IsVehicleValid(v) && by && v->CreatedBy() != by && !ScriptHolds(v)) SetVehicleCreatedBy(v, by);
    nv.origBy = 0;
}

// The owner let the vehicle go: a mirror spawned here goes too (unless this player sits in it), an instance this game had
// before (adopted, or ours until the other player took it) is this game's again, a script's car its script's.
void Vehicles::Drop(uint16_t netId) {
    NetVehicle* nv = Find(netId);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool inside = me && me->InVehicle() && me->Vehicle() == nv->veh;
    bool gone = nv->createdByUs && !inside;
    if (!gone) Release(*nv);
    Forget(netId, gone);
}

std::vector<uint16_t> Vehicles::ClearMirrors(const CVector& at, float radius) {
    std::vector<uint16_t> ids;
    // only where the mission puts its own: a clear of 500 or 1000 m took cars the other players had parked across the
    // town, and out of their safe house garages too (issue #69: "the cars vanish")
    radius = std::min(radius, 60.0f);
    for (auto& [id, nv] : m_byId) {
        CVehicle* v = nv.veh;
        // one the mission took for itself stays (CESAR1 stores the lowrider the partner drives, $8788: deleted here, the
        // mission saw it wrecked and never faded the screen back in)
        if (!IsMirror(nv) || nv.script || !IsVehicleValid(v) || v->Driver() || ScriptHolds(v)) continue;
        bool empty = true;
        for (int i = 0; i < 8; ++i) empty = empty && !v->Passengers()[i];
        CVector p = v->GetPosition(), d = p - at;
        if (empty && d.x * d.x + d.y * d.y < radius * radius && !Call<0x448900, bool>(&p))  // CGarages::IsPointWithinHideOutGarage
            ids.push_back(id);
    }
    for (uint16_t id : ids) Forget(id, true);
    return ids;
}

// the owner's side of ClearMirrors: what the host's copy no longer has goes here too, as the area's ordinary cars went
void Vehicles::HandleClear(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    for (uint8_t i = 0; i < n && r.Ok() && from == Net().HostId(); ++i) {
        uint16_t id = r.U16();
        NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
        if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) continue;
        if (nv->veh->Driver() || HasPlayerInside(nv->veh)) {  // got in meanwhile: the host gets its copy back
            Announce(*nv);
            continue;
        }
        LOGI("Vehicle %04X (model %d) cleared away by the host's mission", id, nv->model);
        StopEntering(nv->veh);
        Cmd(0x00A6).I(nv->handle).Run();  // DELETE_CAR: its removal goes out with the other vanished ones (Update)
    }
}

void Vehicles::ForgetOwnedBy(uint8_t owner) {
    std::vector<uint16_t> ids;
    for (auto& [vid, nv] : m_byId)
        if (nv.owner == owner) ids.push_back(vid);
    for (auto it = m_pending.begin(); it != m_pending.end();) it = it->second.owner == owner ? m_pending.erase(it) : std::next(it);
    for (uint16_t vid : ids) Drop(vid);
}

// Its owner deleted an ordinary car by this game's player: after the host's death its game clears the streets, and a
// police car could not go to this game (its crew changes games only to chase): it vanished in front of the player. It
// stays as this game's car, with whoever sits in it (Peds keeps them), as on a hand-over. A mission's or a wreck still goes,
// and so does an aircraft: a police helicopter kept here hung in the air with no police AI to fly it.
bool Vehicles::KeepsWhenDeleted(const NetVehicle& nv) const {
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = nv.veh;
    if (!me || !IsMirror(nv) || !IsVehicleValid(v) || nv.script || v->Status() == STATUS_WRECKED || ScriptHolds(v)) return false;
    // the host's mission clears the ground of its scene (CLEAR_AREA of the lowrider contest): a car kept here came
    // back there as ours, into the middle of it
    if (!Net().IsHost() && HostMissionRecent(0)) return false;
    int type = v->VehicleSubClass();
    bool aircraft = type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_FHELI || type == VEHICLE_TYPE_FPLANE;
    return !aircraft && type != VEHICLE_TYPE_TRAIN && v->AreaCode() == me->AreaCode() &&
           (v->GetPosition() - me->GetPosition()).Length() < 80.0f;
}

void Vehicles::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok()) return;
    m_pending.erase(id);
    NetVehicle* nv = Find(id);
    if (!nv || nv->owner != from) return;
    CPlayerPed* me = FindPlayerPed(0);
    // gone from its owner's game inside a safe house's garage: the host's store holds it now (a door shut on it, or the
    // host kept the partner's car). Kept here as this game's own, it stood there twice, filled the garage and came back
    // out of it again and again (CJ's garage, 28.10).
    if (CVehicle* v = nv->veh; IsVehicleValid(v) && !(me && me->InVehicle() && me->Vehicle() == v) && !ScriptHolds(v)) {
        CVector p = v->GetPosition();
        if (Call<0x448900, bool>(&p)) {  // CGarages::IsPointWithinHideOutGarage
            LOGI("Vehicle %04X (model %d): its owner put it away in a garage: gone here too", id, nv->model);
            return Forget(id, true);
        }
    }
    if (KeepsWhenDeleted(*nv)) {
        LOGI("Vehicle %04X (model %d): its owner deleted it by us: kept", id, nv->model);
        return TakeOver(*nv, from);
    }
    // Its owner deleted it with our player riding inside ("Home Invasion" deletes the van at the lockup): ours stayed in
    // it as its own car, jammed in a garage door that is shut here, and fell through the floor. Out next to its owner's
    // player, the mission put that one where the scene goes on; still in the van in its last state, it is about to be
    // put there (the van out of reach of the end's scene left the partner in the lockup, not watching it).
    if (me && IsVehicleValid(nv->veh) && me->InVehicle() && me->Vehicle() == nv->veh && nv->veh->Driver() != me) {
        CVector p = nv->veh->GetPosition();
        if (const RemotePlayer* rp = Players::Instance().Get(from); rp && rp->hasState && (rp->state.pos - p).Length() < 100.0f)
            p = rp->state.pos + CVector{1.0f, 0.0f, 0.0f};
        Cmd(0x0362).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
        LOGI("Vehicle %04X: its owner deleted it with us inside: out by its player", id);
    }
    Drop(id);
}

// An ordinary car of this game (traffic, a parked one) the other player is at while this game's player is far: this
// game would drop it soon (CCarCtrl removes cars beyond its player's reach) and it vanished in front of the other player.
// The other game takes it over with whoever sits in it, as its own, and this game's one goes.
static bool CanGiveAway(CVehicle* v) {
    uint8_t by = v->CreatedBy();
    if ((by != VEHICLE_CREATED_RANDOM && by != VEHICLE_CREATED_PARKED) || v->Status() == STATUS_WRECKED ||
        Field<CVehicle*>(v, 0x4C4) || Field<CVehicle*>(v, 0x4C8) ||  // towing or towed: the pair stays together
        !CallMethod<0x6D1180, bool>(v) ||                             // CVehicle::CanBeDeleted: the engine's own test
        CallMethod<0x534540, bool>(v) || ScriptHolds(v))               // CEntity::GetIsOnScreen: it does not vanish in view
        return false;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats) {  // its occupants go along: only this game's own passers-by the other game knows
        NetPed* np = p ? Peds::Instance().Find(p) : nullptr;
        if (p && (p->IsPlayer() || Players::Instance().FindByPed(p) || !np || np->owner != Net().MyId() ||
                  p->CreatedBy() != 1 /* PED_GAME */ || !Peds::Instance().CanGiveAwayType(*np)))
            return false;
    }
    return true;
}

void Vehicles::GiveAway(NetVehicle& nv, uint8_t to) {
    CVehicle* v = nv.veh;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats)  // the peds first: the car's message drives them on
        if (NetPed* np = p ? Peds::Instance().Find(p) : nullptr) Peds::Instance().GiveAway(np->netId, to);
    Writer w(proto::G_VEH_HANDOVER);
    w.U16(nv.netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Vehicle %04X (model %d) handed over to player %d", nv.netId, nv.model, to);
    Forget(nv.netId, true);
}

bool Vehicles::QueueGiveAway(CVehicle* v, uint8_t to) {
    NetVehicle* nv = Find(v);
    if (!nv || IsMirror(*nv) || !CanGiveAway(v)) return false;
    m_giveAway[v] = to;
    return true;
}

void Vehicles::HandleHandover(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()) return;
    m_pending.erase(id);
    NetVehicle* nv = Find(id);
    if (!nv) return;
    if (nv->owner != from) {  // this game took it just before: the giver deleted its own, it gets this one back
        if (nv->owner == Net().MyId() && IsVehicleValid(nv->veh)) Announce(*nv);
        return;
    }
    if (to != Net().MyId() || !IsVehicleValid(nv->veh)) return Forget(id, true);
    TakeOver(*nv, from);
}

void Vehicles::TakeOver(NetVehicle& nv, uint8_t from) {
    CVehicle* v = nv.veh;
    int h = nv.handle;
    uint16_t id = nv.netId;
    Release(nv);  // a mirror's collision proof stayed on the car, and this game's own traffic never took crash damage
    if (!ScriptHolds(v)) SetVehicleCreatedBy(v, VEHICLE_CREATED_RANDOM);  // this game's traffic now
    if (CPed* driver = v->Driver(); driver && !driver->IsPlayer() && !Players::Instance().FindByPed(driver)) {
        Cmd(0x00A8).I(h).Run();           // CAR_WANDER_RANDOMLY
        Cmd(0x00AD).I(h).F(12.0f).Run();  // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(h).I(0).Run();      // SET_CAR_DRIVING_STYLE: stops for cars and lights
    }
    Forget(id, false);
    LOGI("Vehicle %04X (model %d) taken over from player %d", id, (int)v->ModelIndex(), from);
}

void Vehicles::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleState st;
    if (!st.Read(r)) return;
    ApplyDriverState(from, id, st);
}

void Vehicles::ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st) {
    NetVehicle* nv = Find(netId);
    if (!nv || nv->owner != from) return;
    if (nv->hasState && (int32_t)(st.time - nv->lastTime) < 0) return;
    if ((st.flags ^ (nv->hasState ? nv->state.flags : 0)) & VehicleState::F_HORN)
        LOGD("Vehicle %04X: horn %s", netId, (st.flags & VehicleState::F_HORN) ? "on" : "off");
    if ((st.extras ^ (nv->hasState ? nv->state.extras : 0)) & VehicleState::X_LIGHT)
        LOGD("Vehicle %04X: searchlight %s", netId, (st.extras & VehicleState::X_LIGHT) ? "on" : "off");
    nv->lastTime = st.time;
    nv->state = st;
    nv->hasState = true;
    nv->recvMs = NowMs();
}

void Vehicles::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t owner = r.U8();
    if (!r.Ok()) return;
    NetVehicle* nv = Find(id);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool iDrive = me && nv->veh && me->InVehicle() && me->Vehicle() == nv->veh && nv->veh->Driver() == me;
    if (iDrive && owner != Net().MyId()) {
        // the host keeps it (the server makes any player the host, player 1 too), between others the lower id
        bool theyKeep = from == owner && !Net().IsHost() && (owner == Net().HostId() || owner < Net().MyId());
        if (!theyKeep) {
            // we are driving it right now: keep ownership, tell the others again
            Writer w(proto::G_VEH_OWNER);
            w.U16(id);
            w.U8(Net().MyId());
            Net().Send(proto::CH_RELIABLE, w);
            return;
        }
        // It drives it too, in its game (both took the seat at once, or it pulled us out and our game never saw it).
        // Each kept answering the other's claim while both drove, each showed the other standing frozen beside the car.
        // Our player gives way and sits beside it if there is room.
        int h = GetPedRef(me);
        CVector p = me->GetPosition();
        Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        if (SeatOrAny(nv->veh, 0) == 0 && !nv->veh->Passengers()[0])
            Cmd(0x0430).I(h).I(nv->handle).I(0).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        LOGI("Vehicle %04X: player %d drives it as well, ours gives way", id, owner);
    }
    bool wasMine = nv->owner == Net().MyId();
    nv->owner = owner;
    nv->hasState = false;
    if (owner == Net().MyId()) {
        Release(*nv);
    } else if (wasMine) {
        nv->heldHere = IsVehicleValid(nv->veh) && ScriptHolds(nv->veh);
        Protect(*nv);
    }
}

// The script that held this car here let go (its mission ended): the owner's copy, kept a mission car for it, is an
// ordinary car again (else it never went away and stood twice when announced again)
void Vehicles::HandleScriptFree(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
    if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) return;
    nv->script = false;
    if (nv->createdByUs && nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh))
        SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
}

// The parts a driver moves: nozzles, hoists and forks (m_wMiscComponentAngle), the landing gear, a turret. Nothing ticks
// them on a vehicle without a player's status, so the owner's values stand here.
void ApplyParts(CVehicle* v, const VehicleState& st) {
    // only what this vehicle has as well (VehicleState::Capture's rules): a net id that meant another vehicle here wrote
    // a plane's gear (CPlane::SetGearUp) or a tank's turret into a bike or a boat
    int type = v->VehicleSubClass(), model = v->ModelIndex();
    if ((st.extras & VehicleState::X_BIKE) && (type == VEHICLE_TYPE_BIKE || type == VEHICLE_TYPE_BMX)) {
        Field<float>(v, 0x648) = Field<float>(v, 0x64C) = st.lean;  // LeanAngle, DesiredLeanAngle: as its owner shows it
        Field<float>(v, 0x650) = st.leanFwd;                          // LeanFwd
        // its rider stands up on the pedals as the owner's does: the copy's status counts no taps, it only sat (#9).
        // A motorbike (CBike) has no such field: the object ends before it
        if (type == VEHICLE_TYPE_BMX) Field<float>(v, 0x818) = (st.flags & VehicleState::F_SPRINT) ? 10.0f : 0.0f;
    }
    if ((st.extras & VehicleState::X_MISC) && (type <= VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_TRAILER))
        Field<uint16_t>(v, 0x86C) = st.misc;
    if ((st.extras & VehicleState::X_GEAR) && type == VEHICLE_TYPE_PLANE) {
        float& gear = Field<float>(v, 0x9CC);
        if (st.gear >= 1.0f && gear < 1.0f) CallMethod<0x6CAC20>(v);         // CPlane::SetGearUp: no wheels
        else if (st.gear == 0.0f && gear != 0.0f) CallMethod<0x6CAC70>(v);  // CPlane::SetGearDown
        gear = st.gear;
        Field<float>(v, 0x994) = std::clamp(st.gas - st.brake, -1.0f, 1.0f);  // m_fAccelerationBreakStatus: jet flames
    }
    if ((st.extras & VehicleState::X_TURRET) && (model == 432 || model == 601 || model == 407)) {
        Field<float>(v, 0x94C) = st.turretYaw;
        Field<float>(v, 0x950) = st.turretPitch;
    }
}

// Hydraulics of a car a remote player drives. Its game keeps the wheel heights only while computing them from the
// right stick, so they are computed here from his stick the same way; a mirror has the mission-car status for which
// CAutomobile::HydraulicControl takes them from CVehicle::m_aSpecialHydraulicData (4 slots for the whole game).
void ApplyHydraulics(CVehicle* v, const VehicleState& st) {
    if (v->VehicleSubClass() != VEHICLE_TYPE_AUTOMOBILE || !(Field<uint32_t>(v, 0x38C) & 0x20000)) return;  // bHydraulicInst
    RemotePlayer* rp = v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    if (!rp) return;
    const PadState& pad = rp->state.pad;
    float wheel[4] = {1, 1, 1, 1};  // front left, rear left, front right, rear right; all up while the jump is held
    if (!(pad.buttons & PadState::B_SHOCKR)) {
        float lr = pad.rx, ud = pad.ry;
        float jump = std::sqrt(ud * ud + lr * lr) * 1.5f / 128.0f, a = std::atan2(lr, ud) - 0.7853982f;
        float rr = std::cos(a) * jump, fr = std::sin(a) * jump;
        wheel[0] = std::max(0.0f, -rr);
        wheel[1] = std::max(0.0f, -fr);
        wheel[2] = std::max(0.0f, fr);
        wheel[3] = std::max(0.0f, rr);
    }
    bool active = st.misc != 0 || wheel[0] + wheel[1] + wheel[2] + wheel[3] > 0.0f;
    int8_t slot = Field<int8_t>(v, 0x48B);  // CVehicle::m_vehicleSpecialColIndex
    if (active && slot < 0 && CallMethod<0x6DF3D0, bool>(v)) slot = Field<int8_t>(v, 0x48B);  // GetSpecialColModel
    if (slot >= 0 && slot < 4) memcpy((void*)(0xC1CB60 + (uintptr_t)slot * 0x28 + 0x18), wheel, sizeof(wheel));
}

void Vehicles::ApplyMirror(NetVehicle& nv, float dt) {
    if (!nv.hasState) return;
    CVehicle* v = nv.veh;
    const VehicleState& st = nv.state;
    CPlayerPed* me = FindPlayerPed(0);

    if (nv.heldHere && !ScriptHolds(v)) {  // the script here let go of it (its mission ended): the owner may drop it
        nv.heldHere = false;
        Writer w(proto::G_VEH_SCRIPT_FREE);
        w.U16(nv.netId);
        Net().Send(proto::CH_RELIABLE, w);
    }
    if (v->CreatedBy() != VEHICLE_CREATED_MISSION) Protect(nv);  // a script let it go meanwhile (its mission ended)
    // A crash dents and burns a car in its owner's game. Here one heavy hit (a Rhino's ram counts 15 times) blew the copy
    // up at once (CAutomobile::VehicleDamage), its people died here with it, and the wreck stayed while the car drove on.
    if (!(v->PhysicalFlags() & CPhysical::PF_COLLISION_PROOF)) {
        v->PhysicalFlags() |= CPhysical::PF_COLLISION_PROOF;
        nv.crashProofHere = true;
    }
    // burnt out: only where it lies is followed. Each game's blast throws the wreck its own way, and the copy stayed
    // where its own blast threw it (cars on their roofs here, on their wheels there)
    bool wreck = v->Status() == STATUS_WRECKED;
    if (!wreck && (st.flags & VehicleState::F_WRECKED)) {
        Cmd(0x020B).I(nv.handle).Run();  // EXPLODE_CAR: it blew up in the owner's game
        return;
    }
    // towed in its owner's game: hooked here to the copy of its tower too. A tow truck or a tractor hooks a car only by
    // its driver's pad (CAutomobile::TowTruckControl); linked, the two stop colliding and the hoist holds it up
    CVehicle* tower = nullptr;
    if (!wreck && (st.extras & VehicleState::X_TOW))
        if (NetVehicle* tn = Find(st.tower); tn && tn->veh != v && IsVehicleValid(tn->veh)) tower = tn->veh;
    if (tower && !Field<CVehicle*>(v, 0x4C4) && !Field<CVehicle*>(tower, 0x4C8))  // m_pTowingVehicle, m_pVehicleBeingTowed
        if (CallVirtual<61, bool>(v, tower, false))                                 // CVehicle::SetTowLink (vtable +0xF4)
            LOGD("Vehicle %04X hooked to %04X here", nv.netId, st.tower);
    // a remote player (or nobody) drives it: no local pad input must reach this vehicle. A trailer keeps its own
    // status: an abandoned one looks for a cab to hitch (CTrailer::ScanForTowLink), whoever's game that cab is in.
    // A towed one keeps STATUS_IS_TOWED: with any other its ProcessControl breaks the link (0x6B32A7)
    if (!wreck && v->Driver() != me && v->VehicleSubClass() != VEHICLE_TYPE_TRAILER && !tower) v->SetStatus(STATUS_PHYSICS);

    // Struck by our player's car: driven to its owner's place each frame, the copy stood like a wall in the car's way and
    // stopped it (Duke Nukem, 27.10), its owner's game learning of the hit only from its copy of our car. For a moment
    // after a touch it goes by the hit here as a car of this game would, then eases back to where its owner has it.
    if (CVehicle* mine = me && me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me ? me->Vehicle() : nullptr;
        mine && mine != v && !HasPlayerInside(v) && !Aircraft(v) &&
        (CallMethod<0x543540, bool>(v, mine) || CallMethod<0x543540, bool>(mine, v)))  // CPhysical::GetHasCollidedWith
        nv.bumpMs = NowMs();
    uint64_t sinceBump = NowMs() - nv.bumpMs;

    float age = std::min(0.3f, (float)(NowMs() - nv.recvMs) / 1000.0f);
    CVector target = st.pos + st.vel * age;
    // the turn as far on as the position: pulled back each frame to where the state left it, a car spun on the spot
    // (W+S, a donut) turned slower here and jerked back with every state (issue #22)
    Quat targetRot = Quat::Turned(st.rot, st.turn, age);
    CVector cur = v->GetPosition();
    CVector err = target - cur;
    float dist = err.Length();
    bool resting = st.vel.Length() < 0.05f && dist < 0.15f;
    // no ground under it in this game (its collision is not loaded here): physics would drop it through the map,
    // so it stands exactly where its owner has it
    if (NowMs() - nv.groundCheckMs > 250) {
        nv.groundCheckMs = NowMs();
        bool found = false;
        Call<0x5696C0, float>(target.x, target.y, target.z + 2.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
        nv.groundless = !found;
    }
    if (nv.groundless) {
        SetVehicleMatrix(v, target, targetRot);
        v->MoveSpeed() = CVector{};
        v->TurnSpeed() = CVector{};
    } else if (dist > 10.0f) {
        SetVehicleMatrix(v, target, targetRot);
        v->MoveSpeed() = st.vel * (1.0f / 50.0f);
        v->TurnSpeed() = st.turn * (1.0f / 50.0f);
    } else if (sinceBump < 600) {
        // the hit's own motion, this game's physics
    } else if (!resting) {
        bool easing = sinceBump < 2000;  // back from a hit: slowly
        CVector cmd = st.vel + err * (easing ? 1.5f : 4.0f);  // converge in ~1/4 s
        v->MoveSpeed() = cmd * (1.0f / 50.0f);
        v->TurnSpeed() = st.turn * (1.0f / 50.0f);
        if (CMatrix* m = v->Matrix()) {
            Quat q = Quat::Slerp(Quat::FromMatrix(*m), targetRot, std::min(1.0f, dt * (easing ? 3.0f : 12.0f)));
            q.ToMatrix(*m);
            CallMethod<0x446F90>(v);
        }
    }
    if (wreck) return;
    // Under 250 health a car burns and blows up 5 s later by its own timer: the copy went up apart from its owner's car
    // (often still driving) and stayed a wreck. It burns and smokes here as there, and blows up with it (F_WRECKED).
    int type = v->VehicleSubClass();
    if (type == VEHICLE_TYPE_BIKE || type == VEHICLE_TYPE_BMX)
        Field<float>(v, 0x7BC) = std::min(Field<float>(v, 0x7BC), 4000.0f);  // CBike::m_BlowUpTimer
    else if (type != VEHICLE_TYPE_BOAT && type != VEHICLE_TYPE_TRAIN)
        Field<float>(v, 0x8E4) = std::min(Field<float>(v, 0x8E4), 4000.0f);  // CAutomobile::m_fBurnTimer
    if (nv.hasLook && NowMs() - nv.lookCheckMs >= 500) {
        nv.lookCheckMs = NowMs();
        ApplyLook(nv);
    }
    v->SteerAngle() = st.steer;
    v->GasPedal() = st.gas;
    v->BrakePedal() = st.brake;
    // CVehicle::bIsHandbrakeOn as its owner's: left on here (parked, or a car this game's player got out of), it held the
    // copy's rear wheels for good, and the car went on with its brake lights lit and its tyres smoking as if it braked.
    // A traffic driver pulls it for a moment as it stops: coming late, it locked a copy's wheels on the move ("now and then
    // the cars drive on the handbrake", #23). On the move it is taken from a player at the wheel only.
    bool playerDrives = v->Driver() && Players::Instance().FindByPed(v->Driver());
    bool still = st.vel.x * st.vel.x + st.vel.y * st.vel.y + st.vel.z * st.vel.z < 1.0f;  // under 1 m/s
    if ((st.flags & VehicleState::F_HANDBRAKE) && (playerDrives || still)) v->VehicleFlagsLo() |= 0x20u;
    else v->VehicleFlagsLo() &= ~0x20u;
    v->Health() = st.health;
    v->SetEngineOn((st.flags & VehicleState::F_ENGINE) != 0);
    v->SetSiren((st.flags & VehicleState::F_SIREN) != 0);
    int16_t alarm = Field<int16_t>(v, 0x45C);  // CVehicle::m_nAlarmState: a car alarm keeps its time in the horn counter
    if (alarm == 0 || alarm == -1) Field<uint32_t>(v, 0x514) = (st.flags & VehicleState::F_HORN) ? 1 : 0;  // the siren's yelp asks it
    ApplyParts(v, st);
    ApplyHydraulics(v, st);
}

void Vehicles::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = CreateMirror(it->second) ? m_pending.erase(it) : std::next(it);
    float dt = TimeStep() / 50.0f;
    std::vector<uint16_t> dead;
    for (auto& [id, nv] : m_byId) {
        if (!nv.veh || !IsVehicleValid(nv.veh) || nv.veh->ModelIndex() != nv.model) {
            dead.push_back(id);
            continue;
        }
        if (IsMirror(nv)) ApplyMirror(nv, dt);
    }
    for (uint16_t id : dead) {
        NetVehicle* nv = Find(id);
        if (nv && !IsMirror(*nv)) {
            Writer w(proto::G_VEH_REMOVE);
            w.U16(id);
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (nv) m_byPtr.erase(nv->veh);
        m_byId.erase(id);
    }
}

void Vehicles::SendState(NetVehicle& nv) {
    VehicleState st = VehicleState::Capture(nv.veh);
    st.time = (uint32_t)NowMs();
    if (CVehicle* t = Field<CVehicle*>(nv.veh, 0x4C4))  // CVehicle::m_pTowingVehicle: the other game hooks its copies too
        if (NetVehicle* tn = Find(t)) {
            st.extras |= VehicleState::X_TOW;
            st.tower = tn->netId;
        }
    nv.sentHealth = st.health;
    Writer w(proto::G_VEH_SYNC);
    w.U16(nv.netId);
    st.Write(w);
    Net().Send(proto::CH_STATE, w);
}

void Vehicles::ScanTraffic() {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    std::vector<uint16_t> gone;
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh) && !HasPlayerInside(nv.veh) &&
            RemoteDistance(nv.veh->GetPosition(), nv.veh->AreaCode()) > (Aircraft(nv.veh) ? kAirOut : kTrafficOut))
            gone.push_back(id);
    for (uint16_t id : gone) {
        Writer w(proto::G_VEH_REMOVE);
        w.U16(id);
        Net().Send(proto::CH_RELIABLE, w);
        // a copy of the other game's car this player took: off the network an ordinary car (a mission one stayed forever)
        if (NetVehicle* nv = Find(id); nv && nv->createdByUs && IsVehicleValid(nv->veh) &&
                                       nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh) &&
                                       GetVehicleRef(nv->veh) != m_kept)
            SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
        Forget(id, false);
    }
    for (auto it = m_local.begin(); it != m_local.end();) it = GetVehicle(*it) ? std::next(it) : m_local.erase(it);
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (m_byPtr.count(v) || !IsSyncableVehicle(v) || (m_local.count(GetVehicleRef(v)) && !v->Driver())) continue;
        if (RemoteDistance(v->GetPosition(), v->AreaCode()) < (Aircraft(v) ? kAirIn : kTrafficIn)) EnsureNetworked(v);
    }
}

void Vehicles::KeepLocal(CVehicle* v) {
    if (!v || !m_local.insert(GetVehicleRef(v)).second) return;
    LOGD("Vehicle %d (model %d) of the police impound stays this game's", GetVehicleRef(v), (int)v->ModelIndex());
}

// The car the player came in waits outside while the player is in an interior. An interior lies kilometres from its
// door (CCarCtrl::PossiblyRemoveVehicle measures on the map), so the game dropped the car, or handed it to the other
// player's game as an ordinary car that went as soon as that player got far too: both found nothing outside.
void Vehicles::KeepCarWhileInside(CPlayerPed* me) {
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) m_lastDriven = GetVehicleRef(me->Vehicle());
    int8_t area = me->AreaCode();
    if (area != 0 && m_lastArea == 0 && m_kept < 0) {
        CVehicle* v = GetVehicle(m_lastDriven);
        uint8_t by = v ? v->CreatedBy() : 0;
        if (v && v->AreaCode() == 0 && (by == VEHICLE_CREATED_RANDOM || by == VEHICLE_CREATED_PARKED)) {
            m_kept = m_lastDriven;
            m_keptBy = by;
            SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
            LOGI("The player's car (model %d) is kept outside while the player is in interior %d", v->ModelIndex(), area);
        }
    } else if (area == 0 && m_lastArea != 0) {
        ReleaseKeptCar();
    }
    m_lastArea = area;
}

void Vehicles::ReleaseKeptCar() {
    CVehicle* v = m_kept >= 0 ? GetVehicle(m_kept) : nullptr;
    if (v && v->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(v)) SetVehicleCreatedBy(v, m_keptBy);
    m_kept = -1;
}

void Vehicles::OnPostProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint64_t now = NowMs();
    KeepCarWhileInside(me);
    for (auto& [v, to] : m_giveAway)  // still ours and still free to go
        if (NetVehicle* nv = IsVehicleValid(v) ? Find(v) : nullptr; nv && !IsMirror(*nv) && CanGiveAway(v)) GiveAway(*nv, to);
    m_giveAway.clear();
    // a trailer or a car this player's vehicle tows is simulated here from now on
    if (CVehicle* mine = me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me ? me->Vehicle() : nullptr) {
        int n = 0;  // a baggage train: each trailer tows the next
        for (auto* towed = Field<CVehicle*>(mine, 0x4C8); towed && n < 4; towed = Field<CVehicle*>(towed, 0x4C8), ++n)
            if (NetVehicle* tn = Find(towed); tn && IsMirror(*tn)) TakeOwnership(*tn);  // CVehicle::m_pVehicleBeingTowed
    }
    if (now - m_lastScan >= 250) {
        m_lastScan = now;
        ScanTraffic();
    }
    for (auto& [id, nv] : m_byId) {
        if (IsMirror(nv)) {
            // keep the visual controls after the physics step (wheels, brake lights)
            if (nv.hasState) {
                nv.veh->SteerAngle() = nv.state.steer;
                nv.veh->BrakePedal() = nv.state.brake;
                ApplyParts(nv.veh, nv.state);  // the step just leaned a bike again by its speed change
                // CHeli::ProcessControl just switched its searchlight off (no police AI, no player here) and
                // CHeli::PreRender (0x6C5420) adds the light to this frame at night: as the owner's
                if (nv.veh->VehicleSubClass() == VEHICLE_TYPE_HELI) {
                    Field<uint8_t>(nv.veh, 0xA11) = (nv.state.extras & VehicleState::X_LIGHT) != 0;  // m_bSearchLightEnabled
                    Field<CVector>(nv.veh, 0x9F4) = nv.state.lightTarget;                          // m_vecSearchLightTarget
                    Field<float>(nv.veh, 0xA00) = nv.state.lightPower;                             // m_fSearchLightIntensity
                }
            }
            continue;
        }
        // owned: mod shop changes; repeated every 10 s for mirrors that were still streaming their model
        if (now - nv.lookCheckMs >= 1000) {
            nv.lookCheckMs = now;
            if (!nv.hasLook || !(CaptureLook(nv.veh) == nv.look) || now - nv.lookSentMs > 10000) {
                SendLook(nv);
                nv.lookSentMs = now;
            }
        }
        // owned: the local driver's state goes in G_PLAYER_DRIVER (Players); here only unoccupied vehicles
        if (me && nv.veh->Driver() == me) {
            nv.lastChangeMs = now;
            continue;
        }
        // A car of the host's mission (its id has the host's prefix) the partner drove was the partner's game's from then
        // on, and the host's mission moving, freezing, mending or deleting it only touched its copy there, which the
        // owner's states undid. Left standing empty for 2 s, it goes back to the host.
        if (nv.script && !Net().IsHost() && (nv.netId >> 12) == Net().HostId() + 1 && !nv.veh->Driver() &&
            !HasPlayerInside(nv.veh) && now - nv.lastChangeMs > 2000) {
            nv.owner = Net().HostId();
            nv.hasState = false;
            Protect(nv);
            Writer w(proto::G_VEH_OWNER);
            w.U16(nv.netId);
            w.U8(nv.owner);
            Net().Send(proto::CH_RELIABLE, w);
            LOGI("Vehicle %04X of the host's mission goes back to the host", nv.netId);
            continue;
        }
        float speed = nv.veh->MoveSpeed().Length() * 50.0f;
        // an overturned or burning car loses ~200 hp/s standing still: its copy's smoke (white to black by health)
        // and fire lagged 2 s behind
        if (speed > 0.1f || std::fabs(nv.veh->Health() - nv.sentHealth) >= 1.0f) nv.lastChangeMs = now;
        bool active = now - nv.lastChangeMs < 3000;
        bool towed = Field<CVehicle*>(nv.veh, 0x4C4) != nullptr;  // CVehicle::m_pTowingVehicle
        uint64_t interval = towed ? 50 : active ? 100 : 2000;
        if (now - nv.lastSendMs >= interval) {
            nv.lastSendMs = now;
            SendState(nv);
        }
    }
}

void Vehicles::OnSessionEnd() {
    ReleaseKeptCar();
    // mirrors stay in the world as this game's cars (the other game's scripts hold nothing here any more)
    for (auto& [id, nv] : m_byId)
        if (IsMirror(nv)) {
            nv.script = false;
            Release(nv);
        }
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
}

void Vehicles::OnPeerLeave(uint8_t id) {
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = it->second.owner == id ? m_pending.erase(it) : std::next(it);
    // The vehicles of the leaving player stay in the world and become ours (its scripts are gone with it). They are
    // networked again under our own ids: its ids, kept, came again from its game when it rejoined (it counts from 1).
    std::vector<uint16_t> ids;
    for (auto& [vid, nv] : m_byId)
        if (nv.owner == id) {
            nv.owner = Net().MyId();
            nv.script = false;
            Release(nv);
            ids.push_back(vid);
        }
    for (uint16_t vid : ids) Forget(vid, false);
}

void Vehicles::OnSessionStart() {
    m_nextId = (uint16_t)NowMs();  // another start than the last process's: the other game may still know its ids
}

void Vehicles::OnGameStarted() {
    m_lastDriven = m_kept = -1;  // a new world: the old handles mean other cars
    m_lastArea = 0;
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
    m_giveAway.clear();
    m_local.clear();
}

void Vehicles::OnGameShutdown() { OnGameStarted(); }

}  // namespace coop
