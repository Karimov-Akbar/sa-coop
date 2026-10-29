// Objects of the host's mission (the bottles of "Nines and AKs", boxes, barriers) for the partner. The host's game
// knows what its mission script created (CTheScripts::MissionCleanUp lists them, objects as type 3): it announces
// those, sends their moves and their end, and applies the hits the partner's player lands on the copies. The copies
// are frozen, and nothing in the partner's game damages them: a hit by its player goes to the host as
// CObject::ObjectDamage, and a copy breaks when the host's object did.
#include <cmath>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kMissionCleanUp = 0xA90850;  // CTheScripts::MissionCleanUp: 75 x {uint8 type, int32 handle}
enum : uint8_t { REMOVE_GONE = 0, REMOVE_BROKE = 1 };

struct NetObject {
    uint16_t netId = 0;
    CObject* obj = nullptr;
    int handle = -1;
    uint16_t model = 0;
    CVector pos;
    Quat rot;
    int8_t area = 0;
    bool broken = false;
    bool adopted = false;  // partner: its world had this object already (the host's world brought it)
    // attached to a vehicle in the host's game (a box in a van, a tarp on a train): there it never touches its vehicle,
    // here a solid copy sat inside the vehicle's copy and lifted it ("Robbing Uncle Sam": the van floated)
    bool attached = false;
    float scale = 1.0f;  // CObject::m_fScale (SET_OBJECT_SCALE)
    uint64_t sentMs = 0;
    uint8_t flags = 0;       // OBJ_LIFTABLE, OBJ_HELD_HOST
    uint8_t heldBy = 0xFF;   // host: the player whose copy carries it
    uint64_t enterMs = 0;    // host: that player pressed its enter button holding it
    uint64_t heldMs = 0;     // host: since then
    bool copyHeld = false;   // host: its copy took it up; partner: the host's copy was given it
};

// A crate the mission lets its player carry (SET_OBJECT_AS_STEALABLE: CObject::m_nObjectFlags +0x140, bIsLiftable): the
// partner takes the copy of it with the game's own enter button (CPlayerInfo::Process, FindObjectToSteal), and its
// mission counts the partner's crate as its player's (Missions: IS_CHAR_HOLDING_OBJECT and the rest of "Home Invasion").
// The other game's copy of whoever carries it takes the crate up as a player does (TakeUp), so both games show the lift,
// the carry and the put down at the van.
enum : uint8_t { OBJ_LIFTABLE = 1, OBJ_HELD_HOST = 2 };
enum : uint8_t { HOLD = 1, RELEASE = 2, HOLD_ENTER = 3, HOLD_DENY = 4 };
constexpr uint32_t kLiftable = 0x2000;
uint32_t& ObjectFlags(CObject* o) { return Field<uint32_t>(o, 0x140); }

// what a ped carries (CTaskSimpleHoldEntity, or its pick up or put down: their m_pEntityToHold)
CEntity* Held(CPed* ped) {
    void* intel = ped ? ped->Intelligence() : nullptr;
    if (!intel) return nullptr;
    for (int type : {307, 308, 309})  // TASK_SIMPLE_HOLD_ENTITY, _PICKUP_ENTITY, _PUTDOWN_ENTITY
        if (void* t = CallMethod<0x681740, void*>((uint8_t*)intel + 4, type)) return Field<CEntity*>(t, 0x08);
    return nullptr;
}

// The game's own pick up of a crate (CPlayerInfo::Process: CTaskComplexGoPickUpEntity with the carry animations, given as
// a script command event). A player ped holding in a CARRY group puts it down when its pad's enter button goes down
// (CTaskSimpleHoldEntity::ProcessPed), and a copy is processed under its owner's pad: it puts the crate down with its owner.
void TakeUp(CPed* ped, CObject* o) {
    void* intel = ped ? ped->Intelligence() : nullptr;
    void* task = intel ? Call<0x61A5A0, void*>(0x34) : nullptr;  // CTask::operator new
    if (!task) return;
    CallMethod<0x6919C0>(task, (CEntity*)o, 81);  // CTaskComplexGoPickUpEntity(entity, ANIM_GROUP_CARRY)
    alignas(4) uint8_t ev[0x18];
    CallMethod<0x4B0A00>(ev, 3, task, false);                // CEventScriptCommand(TASK_PRIMARY_PRIMARY, task, false)
    CallMethod<0x4AB420>((uint8_t*)intel + 0x68, ev, false);  // CEventGroup::Add (takes a copy)
    CallMethod<0x4B0A50>(ev);                                 // ~CEventScriptCommand
}

// a copy lets go of the crate where it is (DROP_OBJECT: CTaskSimpleHoldEntity::DropEntity without the put down's move)
void LetGo(CPed* ped, CObject* o) {
    if (ped && Held(ped) == (CEntity*)o) Cmd(0x070B).I(GetPedRef(ped)).I(0).Run();
}

std::map<uint16_t, NetObject> g_owned;    // host: announced
std::map<uint16_t, NetObject> g_mirrors;  // partner: the copies
std::map<uint16_t, NetObject> g_pending;  // partner: waiting for the model
uint16_t g_nextId = 1;
uint64_t g_scanMs = 0;
bool g_applying = false;  // a damage this module applies itself
bool g_crateHintShown = false;  // partner: its own help by a crate it can take, once while the copies last

bool Alive(const NetObject& no) { return no.obj && IsObjectValid(no.obj) && GetObjectRef(no.obj) == no.handle; }
bool Broken(CObject* o) { return !o->IsVisible() && !(o->Flags() & 1); }  // smashed: no rendering, no collision
Quat RotationOf(CObject* o) { return o->Matrix() ? Quat::FromMatrix(*o->Matrix()) : Quat{}; }

// the copy as the host's object: no collision while that rides attached, its size
void ApplyRide(NetObject& no) {
    if (!Alive(no) || Broken(no.obj)) return;
    Cmd(0x0382).I(no.handle).I(no.attached ? 0 : 1).Run();  // SET_OBJECT_COLLISION
    Field<float>(no.obj, 0x15C) = no.scale;                // CObject::m_fScale (CObject::PreRender scales it)
}

void Place(CObject* o, const CVector& pos, const Quat& rot) {
    CMatrix* m = o->Matrix();
    if (!m) return;
    WorldRemove(o);
    rot.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    Call<0x563220>(o);        // CWorld::Add
}

uint8_t FlagsOf(CObject* o) {
    uint8_t f = (ObjectFlags(o) & kLiftable) ? OBJ_LIFTABLE : 0;
    if (Held(FindPlayerPed(0)) == (CEntity*)o) f |= OBJ_HELD_HOST;
    return f;
}

void SendCreate(const NetObject& no) {
    Writer w(proto::G_OBJ_CREATE);
    w.U16(no.netId);
    w.U16(no.model);
    w.Vec({no.pos.x, no.pos.y, no.pos.z});
    no.rot.Write(w);
    w.I8(no.area);
    w.U8(no.broken);
    w.U8(no.attached);
    w.F32(no.scale);
    w.U8(no.flags);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendRemove(uint16_t netId, uint8_t kind, uint8_t weapon) {
    Writer w(proto::G_OBJ_REMOVE);
    w.U16(netId);
    w.U8(kind);
    w.U8(weapon);
    Net().Send(proto::CH_RELIABLE, w);
}

NetObject* FindByObject(std::map<uint16_t, NetObject>& m, CObject* o) {
    for (auto& [id, no] : m)
        if (no.obj == o && Alive(no)) return &no;
    return nullptr;
}

// host: announce new mission objects, report moves, breaks and ends
void HostScan() {
    std::vector<int> handles;
    if (HostReplicating())
        for (int i = 0; i < 75; ++i)
            if (uintptr_t e = kMissionCleanUp + (uintptr_t)i * 8; Ref<uint8_t>(e) == 3)  // MISSION_CLEANUP_ENTITY_TYPE_OBJECT
                handles.push_back(Ref<int32_t>(e + 4));
    // the cash bags Big Smoke's courier dropped (Courier.cpp): no mission's, the courier runs outside missions
    int bags[8];
    for (int i = 0, n = CourierObjects(bags, 8); i < n; ++i) handles.push_back(bags[i]);
    {
        for (int h : handles) {
            CObject* o = GetObject(h);
            if (!o || FindByObject(g_owned, o)) continue;
            NetObject no;
            no.netId = g_nextId++;
            no.obj = o;
            no.handle = h;
            no.model = o->ModelIndex();
            no.pos = o->GetPosition();
            no.rot = RotationOf(o);
            no.area = o->AreaCode();
            no.broken = Broken(o);
            no.attached = o->AttachedTo() != nullptr;
            no.scale = Field<float>(o, 0x15C);
            no.flags = FlagsOf(o);
            no.sentMs = NowMs();
            g_owned[no.netId] = no;
            SendCreate(no);
            LOGI("Mission object %04X (model %d) announced", no.netId, no.model);
        }
    }
    for (auto it = g_owned.begin(); it != g_owned.end();) {
        NetObject& no = it->second;
        if (!Alive(no) || no.obj->ModelIndex() != no.model) {
            SendRemove(no.netId, REMOVE_GONE, 0);
            it = g_owned.erase(it);
            continue;
        }
        if (Broken(no.obj)) {
            if (!no.broken) {
                no.broken = true;
                SendRemove(no.netId, REMOVE_BROKE, Field<uint8_t>(no.obj, 0x148));  // CObject::m_nLastWeaponDamage
                LOGI("Mission object %04X broke", no.netId);
            }
        } else {
            no.broken = false;
            CVector pos = no.obj->GetPosition();
            Quat rot = RotationOf(no.obj);
            float turn = std::fabs(rot.x - no.rot.x) + std::fabs(rot.y - no.rot.y) + std::fabs(rot.z - no.rot.z) +
                         std::fabs(rot.w - no.rot.w);
            bool attached = no.obj->AttachedTo() != nullptr;
            float scale = Field<float>(no.obj, 0x15C);
            uint8_t flags = no.heldBy != 0xFF ? no.flags : FlagsOf(no.obj);  // carried by the partner: as it was
            int8_t area = (int8_t)no.obj->AreaCode();  // a crate carried out of the house goes into the street's area
            // and again every 2 s at rest: the state channel can lose the last one, and a pushed box stood half-way there
            bool moved = (pos - no.pos).Length() > 0.02f || turn > 0.01f || attached != no.attached || scale != no.scale ||
                         flags != no.flags || area != no.area;
            if ((moved && NowMs() - no.sentMs >= 100) || NowMs() - no.sentMs >= 2000) {
                no.pos = pos;
                no.rot = rot;
                no.attached = attached;
                no.scale = scale;
                no.flags = flags;
                no.area = area;
                no.sentMs = NowMs();
                Writer w(proto::G_OBJ_SYNC);
                w.U16(no.netId);
                w.Vec({pos.x, pos.y, pos.z});
                rot.Write(w);
                w.U8(attached);
                w.F32(scale);
                w.U8(flags);
                w.I8(area);
                Net().Send(proto::CH_STATE, w);
            }
        }
        ++it;
    }
}

// partner: break the copy as the host's object broke (the same damage path shows the same shards)
void BreakCopy(NetObject& no, uint8_t weapon) {
    if (!Alive(no) || Broken(no.obj)) return;
    CVector p = no.obj->GetPosition(), d{0, 0, 1};
    g_applying = true;
    CallMethod<0x5A0D90>(no.obj, 10000.0f, &p, &d, (CEntity*)nullptr, (int)weapon);  // CObject::ObjectDamage
    g_applying = false;
    if (!Broken(no.obj)) {  // nothing to smash on it: just gone
        Cmd(0x0750).I(no.handle).I(0).Run();  // SET_OBJECT_VISIBLE
        Cmd(0x0382).I(no.handle).I(0).Run();  // SET_OBJECT_COLLISION
    }
}

void DeleteCopy(NetObject& no) {
    if (Alive(no) && !no.adopted) Cmd(0x0108).I(no.handle).Run();  // DELETE_OBJECT
}

// the same object in the same place, not a copy yet: the host's world, loaded here, brought it along
CObject* FindTwin(const NetObject& no) {
    CPoolHeader* pool = ObjectPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        if (o->ModelIndex() != no.model || (o->GetPosition() - no.pos).Length() > 0.25f || FindByObject(g_mirrors, o)) continue;
        return o;
    }
    return nullptr;
}

void ClearCopies() {
    g_crateHintShown = false;
    for (auto& [id, no] : g_mirrors) DeleteCopy(no);
    g_mirrors.clear();
    g_pending.clear();
}

// the copy can be taken up here while the host's object can be, and nobody carries it
void ApplyLiftable(NetObject& no) {
    if (!Alive(no) || no.adopted) return;
    bool lift = (no.flags & OBJ_LIFTABLE) && !(no.flags & OBJ_HELD_HOST);
    ObjectFlags(no.obj) = lift ? ObjectFlags(no.obj) | kLiftable : ObjectFlags(no.obj) & ~kLiftable;
}

// partner: our player carries the copy of a host's object (taken up with the game's enter button)
uint16_t g_holding = 0;    // its net id, 0: none
bool g_enterWas = false;  // the enter button, for its press
void SendHold(uint8_t op, uint16_t id, const NetObject* no = nullptr) {
    Writer w(proto::G_OBJ_HOLD);
    w.U8(op);
    w.U16(id);
    if (op == RELEASE && no) {
        CVector p = no->obj->GetPosition();
        w.Vec({p.x, p.y, p.z});
        RotationOf(no->obj).Write(w);
    }
    Net().Send(proto::CH_RELIABLE, w);
}
// "Home Invasion" shows its player GUNS118 ("press ... to pick the box up") by a crate, once: the partner gets its own
void CrateHint(CPlayerPed* me, bool holding) {
    if (g_crateHintShown || holding || me->InVehicle()) return;
    for (auto& [id, no] : g_mirrors) {
        if (!Alive(no) || !(no.flags & OBJ_LIFTABLE) || (no.flags & OBJ_HELD_HOST)) continue;
        CVector d = no.obj->GetPosition() - me->GetPosition();
        if (std::fabs(d.x) > 1.5f || std::fabs(d.y) > 1.5f || std::fabs(d.z) > 1.5f) continue;
        const char* text = CallMethod<0x6A0050, const char*>((void*)0xC1B340, "GUNS118");  // CText::Get: its table is here
        if (text && *text) Cmd(0x03E5).S8("GUNS118").Run();  // PRINT_HELP
        g_crateHintShown = true;
        return;
    }
}

void WatchHold() {
    CPlayerPed* me = FindPlayerPed(0);
    CEntity* e = Held(me);
    uint16_t now = 0;
    for (auto& [id, no] : g_mirrors)
        if (Alive(no) && (CEntity*)no.obj == e) now = id;
    CrateHint(me, e != nullptr);
    bool enter = GetPad(0)->NewState.ButtonTriangle != 0;
    if (now && now == g_holding && enter && !g_enterWas) SendHold(HOLD_ENTER, now);
    g_enterWas = enter;
    if (now == g_holding) return;
    if (g_holding) {
        auto it = g_mirrors.find(g_holding);
        if (it != g_mirrors.end() && Alive(it->second)) SendHold(RELEASE, g_holding, &it->second);
        else SendHold(RELEASE, g_holding);
        LOGI("Mission object %04X: put down", g_holding);
    }
    g_holding = now;
    if (now) {
        SendHold(HOLD, now);
        LOGI("Mission object %04X: taken up", now);
    }
}

// host: the partner's copy carries the real one, taken up by the game's task; a copy that did not take it within 2 s (busy
// with something else) holds it in front of its chest
void FollowHolder(NetObject& no) {
    const RemotePlayer* rp = Players::Instance().Get(no.heldBy);
    if (!rp || !rp->ped || !Alive(no)) return;
    no.obj->AreaCode() = rp->ped->AreaCode();  // through the house's door with its carrier (seen only in its own area)
    if (Held(rp->ped) == (CEntity*)no.obj) {
        no.copyHeld = true;
        return;
    }
    if (no.copyHeld || NowMs() - no.heldMs < 2000) return;  // put down by its copy (the release is on its way), or lifting
    if (no.obj->AttachedTo() != (CEntity*)rp->ped) {
        Cmd(0x069B).I(no.handle).I(rp->pedHandle).F(0.0f).F(0.45f).F(0.25f).F(0.0f).F(0.0f).F(0.0f).Run();  // ATTACH_OBJECT_TO_CHAR
        no.obj->AreaCode() = rp->ped->AreaCode();
    }
}

void ReleaseHeld(NetObject& no, const CVector* pos, const Quat* rot) {
    if (no.heldBy == 0xFF) return;
    const RemotePlayer* rp = Players::Instance().Get(no.heldBy);
    no.heldBy = 0xFF;
    if (!Alive(no)) return;
    LetGo(rp ? rp->ped : nullptr, no.obj);
    if (no.obj->AttachedTo()) Cmd(0x0682).I(no.handle).F(0.0f).F(0.0f).F(0.0f).I(0).Run();  // DETACH_OBJECT
    if (pos && rot) Place(no.obj, *pos, *rot);
    Cmd(0x0382).I(no.handle).I(1).Run();  // SET_OBJECT_COLLISION
    if (no.flags & OBJ_LIFTABLE) ObjectFlags(no.obj) |= kLiftable;
}

void HandleHold(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint16_t id = r.U16();
    if (!r.Ok() || !core::IsPlayerReady() || from == Net().MyId()) return;
    if (op == HOLD_DENY) {  // to the partner: the host's player took it first
        if (from != Net().HostId() || id != g_holding) return;
        Cmd(0x070B).I(GetPedRef(FindPlayerPed(0))).I(0).Run();  // DROP_OBJECT
        LOGI("Mission object %04X: the host has it, ours dropped", id);
        return;
    }
    if (!Net().IsHost()) return;
    auto it = g_owned.find(id);
    if (it == g_owned.end() || !Alive(it->second)) return;
    NetObject& no = it->second;
    if (op == HOLD) {
        if (Held(FindPlayerPed(0)) == (CEntity*)no.obj || (no.heldBy != 0xFF && no.heldBy != from)) {
            Writer w(proto::G_OBJ_HOLD);
            w.U8(HOLD_DENY);
            w.U16(id);
            Net().Send(proto::CH_RELIABLE, w);
            return;
        }
        no.heldBy = from, no.enterMs = 0, no.heldMs = NowMs(), no.copyHeld = false;
        Cmd(0x0382).I(no.handle).I(0).Run();  // SET_OBJECT_COLLISION: carried
        ObjectFlags(no.obj) &= ~kLiftable;   // our player cannot take it from the partner's hands
        if (const RemotePlayer* rp = Players::Instance().Get(from); rp && rp->ped) TakeUp(rp->ped, no.obj);
        LOGI("Mission object %04X: carried by player %d", id, from);
    } else if (op == RELEASE) {
        Vec3 p = r.Vec();
        Quat q;
        q.Read(r);
        if (no.heldBy != from) return;
        CVector pos{p.x, p.y, p.z};
        bool placed = r.Ok() && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        ReleaseHeld(no, placed ? &pos : nullptr, placed ? &q : nullptr);
        LOGI("Mission object %04X: put down by player %d", id, from);
    } else if (op == HOLD_ENTER && no.heldBy == from) {
        no.enterMs = NowMs();
    }
}

void CreatePending() {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        NetObject no = it->second;
        if (!IsModelLoaded(no.model)) {
            RequestModel(no.model);
            ++it;
            continue;
        }
        if (!HasRoom(ObjectPool(), kKeepObjects, "object")) break;
        it = g_pending.erase(it);
        int h = -1;
        CObject* o = FindTwin(no);
        no.adopted = o != nullptr;
        if (o) h = GetObjectRef(o);
        else Cmd(0x029B).I(no.model).F(no.pos.x).F(no.pos.y).F(no.pos.z).Out(&h).Run();  // CREATE_OBJECT_NO_OFFSET
        if (!o) {
            o = h >= 0 ? GetObject(h) : nullptr;
            if (o) Field<uint8_t>(o, 0x13C) = 6;  // OBJECT_MISSION2: kept out of saves (see Props)
        }
        if (!o) continue;
        Cmd(0x0550).I(h).I(1).Run();  // FREEZE_OBJECT_POSITION
        o->AreaCode() = no.area;
        Place(o, no.pos, no.rot);
        no.obj = o;
        no.handle = h;
        bool broken = no.broken;
        no.broken = false;
        NetObject& copy = g_mirrors[no.netId] = no;
        ApplyRide(copy);
        ApplyLiftable(copy);
        if (broken) BreakCopy(copy, 0);
        LOGI("Mission object %04X (model %d) %s", no.netId, no.model, no.adopted ? "found here" : "copied");
    }
}

// The partner's player hits a copy: the host's object takes it. Nothing else damages a copy.
using ObjectDamageFn = void(__fastcall*)(CObject*, void*, float, CVector*, CVector*, CEntity*, int);
ObjectDamageFn g_origObjectDamage = nullptr;
// the other game's car or person, as this game's copy of it
bool OtherGames(CEntity* e) {
    if (e->IsVehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && Vehicles::Instance().IsMirror(*nv);
    }
    if (!e->IsPed()) return false;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped == e) return true;
    NetPed* np = Peds::Instance().Find((CPed*)e);
    return np && np->owner != Net().MyId();
}

void __fastcall HookObjectDamage(CObject* o, void* edx, float damage, CVector* origin, CVector* dir, CEntity* damager,
                                 int weapon) {
    // A map object (a lamp post, a sign) breaks in the game whose car or person really hit it, and WorldObjects sends
    // that over. This game's copy of the other game's car runs a little off the real one's path: it broke posts
    // the real one went past, and the other player saw them fall by themselves.
    // The host's mission objects the same: the other player's game sends its hits (G_OBJ_DAMAGE), and the replayed shots
    // of its copy here counted them twice. So do the targets of our joint range round (Props, RangeHit).
    if (!g_applying && damager && Net().Online() && (Field<uint8_t>(o, 0x13C) == 1 || FindByObject(g_owned, o) || RangeTarget(o)) &&
        OtherGames(damager))
        return;
    // our copy of a target of the other player's range round: our shot goes to its game, nothing lands here
    if (!g_applying && Net().Online() && PropCopyHit(o, damager, damage, weapon)) return;
    if (!g_applying && Net().Online() && !g_mirrors.empty()) {
        if (NetObject* no = FindByObject(g_mirrors, o)) {
            CPlayerPed* me = FindPlayerPed(0);
            if (damager && me && (damager == me || (me->InVehicle() && damager == (CEntity*)me->Vehicle()))) {
                Writer w(proto::G_OBJ_DAMAGE);
                w.U16(no->netId);
                w.F32(damage);
                w.U8((uint8_t)weapon);
                Net().Send(proto::CH_RELIABLE, w);
            }
            return;
        }
    }
    g_origObjectDamage(o, edx, damage, origin, dir, damager, weapon);
}

// The host's mission asks whether its people and objects were photographed ("Badlands", "Photo Opportunity",
// "Architectural Espionage"), and the partner's camera marked only their copies: the partner's game tells the host which.
constexpr uint32_t kPedPhotographed = 1u << 18;  // CPed::bHasBeenPhotographed (the second flags)
constexpr uint32_t kObjectPhotographed = 0x1000;  // CObject::bIsPhotographed
using TakePhotoFn = bool(__fastcall*)(void*, void*, CEntity*, CVector*);
TakePhotoFn g_origTakePhoto = nullptr;
bool __fastcall HookTakePhotograph(void* weapon, void* edx, CEntity* owner, CVector* point) {
    bool taken = g_origTakePhoto(weapon, edx, owner, point);  // false for the other player's camera (Players)
    LOGI("Camera shot by %s: %s (camera mode %d)", owner == (CEntity*)FindPlayerPed(0) ? "us" : "another", taken ? "taken" : "none",
         (int)cam::Mode());
    if (!taken || !Net().Online() || Net().IsHost() || !core::IsPlayerReady()) return taken;
    std::vector<uint16_t> peds, objects;
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        NetPed* np = Peds::Instance().Find(p);
        if (!np || np->owner != Net().HostId() || !(p->PedFlags2() & kPedPhotographed) || peds.size() >= 100) continue;
        p->PedFlags2() &= ~kPedPhotographed;  // told once
        peds.push_back(np->netId);
    }
    for (auto& [id, no] : g_mirrors)
        if (Alive(no) && (Field<uint32_t>(no.obj, 0x140) & kObjectPhotographed) && objects.size() < 100) {
            Field<uint32_t>(no.obj, 0x140) &= ~kObjectPhotographed;
            objects.push_back(id);
        }
    if (peds.empty() && objects.empty()) return taken;
    Writer w(proto::G_PHOTO);
    w.U8((uint8_t)peds.size());
    for (uint16_t id : peds) w.U16(id);
    w.U8((uint8_t)objects.size());
    for (uint16_t id : objects) w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Our photo holds %d of the host's people and %d of its objects", (int)peds.size(), (int)objects.size());
    return taken;
}

void HandlePhoto(uint8_t from, Reader& r) {
    if (!Net().IsHost() || !core::IsPlayerReady()) return;
    int peds = 0, objects = 0;
    for (int n = r.U8(); n > 0 && r.Ok(); --n)
        if (CPed* p = Peds::Instance().FindOwned(r.U16()); p && r.Ok()) p->PedFlags2() |= kPedPhotographed, ++peds;
    for (int n = r.U8(); n > 0 && r.Ok(); --n)
        if (auto it = g_owned.find(r.U16()); r.Ok() && it != g_owned.end() && Alive(it->second))
            Field<uint32_t>(it->second.obj, 0x140) |= kObjectPhotographed, ++objects;
    LOGI("Player %d photographed %d of our people and %d of our objects", from, peds, objects);
}

class Objects : public Module {
public:
    const char* Name() const override { return "Objects"; }

    void OnInstallHooks() override {
        mem::HookFn(0x5A0D90, &HookObjectDamage, &g_origObjectDamage, "CObject::ObjectDamage");
        mem::RedirectCallFn(0x74278B, &HookTakePhotograph, &g_origTakePhoto);  // CWeapon::Fire, the camera (Players' too)
        Net().SetHandler(proto::G_PHOTO, &HandlePhoto);
        Net().SetHandler(proto::G_OBJ_HOLD, &HandleHold);
        Net().SetHandler(proto::G_OBJ_CREATE, [](uint8_t from, Reader& r) {
            NetObject no;
            no.netId = r.U16();
            no.model = r.U16();
            Vec3 p = r.Vec();
            no.pos = {p.x, p.y, p.z};
            no.rot.Read(r);
            no.area = r.I8();
            no.broken = r.U8() != 0;
            no.attached = r.U8() != 0;
            no.scale = r.F32();
            no.flags = r.U8();
            if (!std::isfinite(no.scale) || no.scale <= 0.0f || no.scale > 50.0f) no.scale = 1.0f;
            // not in the game yet (a partner waits in the menu): the host announces them again once it is (resync)
            if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
            if (!IsObjectModel(no.model)) {  // a model of a mod this game lacks: the streamer crashes on it
                LOGW("Mission object %04X has an unknown model %d", no.netId, no.model);
                return;
            }
            if (auto it = g_mirrors.find(no.netId); it != g_mirrors.end()) {  // announced again: a fresh copy
                DeleteCopy(it->second);
                g_mirrors.erase(it);
            }
            g_pending[no.netId] = no;
            RequestModel(no.model);
        });
        Net().SetHandler(proto::G_OBJ_SYNC, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            Vec3 p = r.Vec();
            Quat q;
            q.Read(r);
            bool attached = r.U8() != 0;
            float scale = r.F32();
            uint8_t flags = r.U8();
            int8_t area = r.I8();
            if (!r.Ok() || from != Net().HostId() || !core::IsPlayerReady()) return;
            if (!std::isfinite(scale) || scale <= 0.0f || scale > 50.0f) scale = 1.0f;
            if (auto it = g_pending.find(id); it != g_pending.end()) {
                it->second.pos = {p.x, p.y, p.z};
                it->second.rot = q;
                it->second.attached = attached;
                it->second.scale = scale;
                it->second.flags = flags;
                it->second.area = area;
            } else if (auto m = g_mirrors.find(id); m != g_mirrors.end() && Alive(m->second)) {
                NetObject& no = m->second;
                if (no.flags != flags) no.flags = flags, ApplyLiftable(no);
                if (id == g_holding) return;  // in our player's hands: the hold task moves it
                no.area = area;
                no.obj->AreaCode() = area;
                // in the host's hands: on its copy's chest (the copy plays the host's carry, Anims), through the doors with it
                const RemotePlayer* host = Players::Instance().Get(Net().HostId());
                if ((flags & OBJ_HELD_HOST) && host && host->ped) {
                    no.copyHeld = true;
                    if (no.obj->AttachedTo() != (CEntity*)host->ped)
                        Cmd(0x069B).I(no.handle).I(host->pedHandle).F(0.0f).F(0.45f).F(0.25f).F(0.0f).F(0.0f).F(0.0f).Run();  // ATTACH_OBJECT_TO_CHAR
                    no.obj->AreaCode() = host->ped->AreaCode();
                    return;
                }
                if (no.copyHeld) {
                    no.copyHeld = false;
                    if (no.obj->AttachedTo()) Cmd(0x0682).I(no.handle).F(0.0f).F(0.0f).F(0.0f).I(0).Run();  // DETACH_OBJECT
                }
                Place(no.obj, {p.x, p.y, p.z}, q);
                if (m->second.attached != attached || m->second.scale != scale) {
                    m->second.attached = attached;
                    m->second.scale = scale;
                    ApplyRide(m->second);
                }
            }
        });
        Net().SetHandler(proto::G_OBJ_REMOVE, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            uint8_t kind = r.U8(), weapon = r.U8();
            if (!r.Ok() || from != Net().HostId() || !core::IsPlayerReady()) return;
            if (kind == REMOVE_BROKE) {
                if (auto it = g_pending.find(id); it != g_pending.end()) it->second.broken = true;
                if (auto it = g_mirrors.find(id); it != g_mirrors.end()) BreakCopy(it->second, weapon);
                return;
            }
            g_pending.erase(id);
            if (auto it = g_mirrors.find(id); it != g_mirrors.end()) {
                DeleteCopy(it->second);
                g_mirrors.erase(it);
            }
        });
        Net().SetHandler(proto::G_OBJ_DAMAGE, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            float damage = r.F32();
            uint8_t weapon = r.U8();
            if (!r.Ok() || !Net().IsHost() || !core::IsPlayerReady()) return;
            auto it = g_owned.find(id);
            if (it == g_owned.end() || !Alive(it->second)) return;
            CObject* o = it->second.obj;
            CVector p = o->GetPosition(), d{0, 0, 1};
            // the mission counts its player's hits: the partner's count as his
            CallMethod<0x5A0D90>(o, damage, &p, &d, (CEntity*)FindPlayerPed(0), (int)weapon);  // CObject::ObjectDamage
            LOGI("Mission object %04X hit by player %d (%.0f)", id, from, damage);
        });
    }

    void OnSessionEnd() override {
        ClearCopies();
        g_owned.clear();
    }
    void OnPeerLeave(uint8_t id) override {
        for (auto& [netId, no] : g_owned)
            if (no.heldBy == id) ReleaseHeld(no, nullptr, nullptr);  // it falls where its carrier left it
        if (id == Net().HostId() || Net().IsHost()) ClearCopies();
    }
    void OnPeerResync(uint8_t id) override {
        if (!Net().IsHost()) {  // the host's game started over: the objects of its old world are gone
            if (id == Net().HostId()) ClearCopies();
            return;
        }
        for (auto& [netId, no] : g_owned)
            if (Alive(no)) SendCreate(no);
    }
    void OnGameStarted() override {  // a new world: the objects of the old one are gone
        g_holding = 0;
        g_owned.clear();
        g_mirrors.clear();
        g_pending.clear();
    }
    void OnGameShutdown() override { OnGameStarted(); }  // nothing to touch while the world loads

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (Net().IsHost()) {
            if (NowMs() - g_scanMs >= 100) {
                g_scanMs = NowMs();
                HostScan();
            }
            for (auto& [netId, no] : g_owned)
                if (no.heldBy != 0xFF) FollowHolder(no);
        } else {
            if (!g_pending.empty()) CreatePending();
            WatchHold();
        }
    }

    static Objects& Instance() {
        static Objects m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Objects::Instance()); }
} g_reg;
}  // namespace

bool MissionObjectCopy(void* object) {
    for (auto& [id, no] : g_mirrors)
        if (no.obj == object && Alive(no)) return true;
    return false;
}

uint16_t MissionObjectNetId(void* object) {
    for (auto& [id, no] : g_owned)
        if (no.obj == object && Alive(no)) return id;
    return 0;
}

int MissionObjectCopyHandle(uint16_t netId) {
    auto it = g_mirrors.find(netId);
    return it != g_mirrors.end() && Alive(it->second) ? it->second.handle : -1;
}

bool PartnerHolds(void* object, void** holder, uint64_t* enterMs) {
    if (!object) return false;
    for (auto& [id, no] : g_owned) {
        if (no.obj != object || no.heldBy == 0xFF || !Alive(no)) continue;
        const RemotePlayer* rp = Players::Instance().Get(no.heldBy);
        if (!rp || !rp->ped) return false;
        if (holder) *holder = rp->ped;
        if (enterMs) *enterMs = no.enterMs;
        return true;
    }
    return false;
}

}  // namespace coop
