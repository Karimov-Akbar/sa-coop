// NPC synchronisation. Every game owns the peds it created (random population, mission peds); peds near
// the other player are announced and follow the owner on the other side as "mirrors": script-created
// peds with an empty decision maker and all proofs. What the owner's ped does is replayed with the
// game's own means so it looks native:
//  * walking/running   - "go to point" tasks towards the owner's position;
//  * aiming/shooting   - aim/shoot-at-coord tasks (gun pose, muzzle flash, sound, tracers);
//  * drive-bys         - the gang drive-by task (TASK_DRIVE_BY) at the same target, out of the mirror's car;
//  * entering/leaving  - enter/leave car tasks (door and seat animations);
//  * anything else     - the owner's current action animation (chat, phone, benches, fights, falls).
#include "entities/ped/Peds.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Peds& Peds::Instance() {
    static Peds p;
    return p;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Peds::Instance()); }
} g_reg;

// owned peds closer than "in" to the other player are announced and removed there beyond "out";
// vehicle occupants use the vehicle ranges so traffic is never seen without its driver
float SyncIn(CPed* p) { return p->InVehicle() ? 170.0f : 120.0f; }
float SyncOut(CPed* p) { return p->InVehicle() ? 220.0f : 150.0f; }
constexpr int kSpecialFirst = 290, kSpecialLast = 299;
constexpr int kEmptyDecisionMaker = 65542;  // SET_CHAR_DECISION_MAKER template "Empty"
constexpr int TASK_ENTER_CAR_AS_PASSENGER = 700, TASK_ENTER_CAR_AS_DRIVER = 701, TASK_LEAVE_CAR = 704;
constexpr int TASK_SIMPLE_USE_GUN = 1017, TASK_SIMPLE_GANG_DRIVEBY = 1022;
constexpr uint64_t kCarTaskGraceMs = 6000;  // let enter/leave animations finish before warping
constexpr uint16_t kNoAnim = 0xFFFF;

Vec3 V(const CVector& v) { return {v.x, v.y, v.z}; }
CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

void WriteState(Writer& w, const PedSyncState& s) {
    w.U32(s.time);
    w.Vec(V(s.pos));
    w.VecQ(V(s.vel), 30.0f);
    w.Angle(s.heading);
    w.U8(s.moveState);
    w.U8(s.health);
    w.U8(s.armour);
    w.U8(s.weapon);
    w.U8(s.flags);
    w.U16(s.vehNetId);
    w.I8(s.seat);
    w.I8(s.area);
    if (s.flags & PedSyncState::F_AIMING) {
        w.VecQ(V(s.aim), 200.0f);
        w.U8(s.aimKind);
        if (s.aimKind) w.U16(s.aimId);
    }
    w.U16(s.animBlock);
    if (s.animBlock != kNoAnim) {
        w.U16(s.animIdx);
        w.U16(s.animFlags);
        w.F32(s.animTime);
    }
    w.U8(s.walkGroup);
}

void ReadState(Reader& r, PedSyncState& s) {
    s.time = r.U32();
    s.pos = G(r.Vec());
    s.vel = G(r.VecQ(30.0f));
    s.heading = WrapAngle(r.Angle());
    s.moveState = r.U8();
    s.health = r.U8();
    s.armour = r.U8();
    s.weapon = r.U8();
    if (s.weapon > 46) s.weapon = 0;  // eWeaponType ends with the parachute: the weapon info past it is garbage
    s.flags = r.U8();
    s.vehNetId = r.U16();
    s.seat = r.I8();
    s.area = r.I8();
    if (s.flags & PedSyncState::F_AIMING) {
        s.aim = G(r.VecQ(200.0f));
        s.aimKind = r.U8();
        if (s.aimKind) s.aimId = r.U16();
    }
    s.animBlock = r.U16();
    if (s.animBlock != kNoAnim) {
        s.animIdx = r.U16();
        s.animFlags = r.U16();
        s.animTime = r.F32();
    }
    s.walkGroup = r.U8();
}

// A walk set whose IFP is in memory (asked for when not): a walk of a block not loaded crashed in CPed::SetMoveAnim
bool WalkGroupReady(int group) {
    if (group < 0 || group >= (int)Ref<uint32_t>(0xB4EA28)) return false;  // CAnimManager::ms_numAnimAssocDefinitions
    uintptr_t g = Ref<uintptr_t>(0xB4EA34) + (uintptr_t)group * 0x14;    // ms_aAnimAssocGroups
    uintptr_t block = Ref<uintptr_t>(g + 0x00);                           // CAnimBlendAssocGroup::m_pAnimBlock
    if (!block || !Ref<void*>(g + 0x04)) return false;                    // its associations not made yet
    if (Ref<uint8_t>(block + 0x10)) return true;                          // CAnimBlock::bLoaded
    RequestModel(anim::kIfpBase + (int)((block - anim::kBlocks) / 0x20), 2);
    return false;
}

using RequestSpecialFn = void(__cdecl*)(int, const char*, int);
RequestSpecialFn g_origRequestSpecial = nullptr;
// another character into the slot a player's copy wears (a cutscene, a mission, a mirror): CStreaming::
// RequestSpecialModel deletes the slot's model while that copy still has it on. It goes back to CJ first and gets its
// look again.
void MakeWay(int model, const char* name) {
    if (!name || !_stricmp(Peds::Instance().SpecialName(model).c_str(), name)) return;
    Peds::Instance().RespawnCopiesOf(model);
    for (auto& [id, other] : Players::Instance().All())
        if (RemotePlayer* rp = Players::Instance().Get(id); rp && rp->ped && rp->ped->ModelIndex() == model) {
            Players::SetModel(rp->ped, 0);
            rp->lookStale = true;
            LOGI("Player %d's special character makes way for %s", id, name);
        }
}

void __cdecl HookRequestSpecialModel(int model, const char* name, int flags) {
    MakeWay(model, name);
    if (name) Peds::Instance().RememberSpecialModel(model, name);
    g_origRequestSpecial(model, name, flags);
}

uint8_t LoadState(int model) { return Ref<uint8_t>(0x8E4CC0 + (uintptr_t)model * 0x14 + 0x10); }  // CStreaming::ms_aInfoForModel

uint8_t Clamp8(float v) { return (uint8_t)std::clamp(v, 0.0f, 255.0f); }

bool IsPedDead(CPed* p) {
    int st = p->PedState();
    return p->Health() <= 0.0f || st == 54 /*PEDSTATE_DIE*/ || st == 55 /*PEDSTATE_DEAD*/;
}

bool IsAlive(const NetPed& np) { return np.ped && IsPedValid(np.ped) && GetPedRef(np.ped) == np.handle; }

// CTaskManager::FindActiveTaskByType: searches the active primary chain and the secondary tasks
void* FindTask(CPed* ped, int type) {
    void* intel = ped->Intelligence();
    return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, type) : nullptr;
}

// a player getting in at the wheel of this vehicle here, ours or the other one's copy (its enter task's car at +0x0C):
// the vehicle's own count (CVehicle::m_nNumGettingIn) starts only at the door
bool PlayerGettingIn(CVehicle* v) {
    if (Field<uint8_t>(v, 0x485)) return true;
    auto entering = [v](CPed* p) {
        for (int t : {TASK_ENTER_CAR_AS_DRIVER, 713 /* TASK_ENTER_CAR_AS_DRIVER_TIMED */})
            if (void* task = p ? FindTask(p, t) : nullptr; task && Field<CVehicle*>(task, 0x0C) == v) return true;
        return false;
    };
    if (entering(FindPlayerPed(0))) return true;
    for (auto& [id, rp] : Players::Instance().All())
        if (entering(rp.ped)) return true;
    return false;
}

// The action animation the ped plays: anything but its own walk/run/idle set (CPed::m_nAnimGroup).
// Full-body animations while moving are turns and start/stop steps, also locomotion. A partial one (a punch, a guard)
// plays over a full-body one (the fight stance) and is the action while it lasts: the mirror keeps the stance under it.
void CaptureAnim(CPed* ped, PedSyncState& s) {
    // getting in or out of a car: the copy does it with its own task (F_LEAVING, the enter tasks), and the owner's door
    // and seat animations on top of it played twice (Ryder out of the van in "Home Invasion")
    if (FindTask(ped, TASK_LEAVE_CAR) || FindTask(ped, TASK_ENTER_CAR_AS_DRIVER) || FindTask(ped, TASK_ENTER_CAR_AS_PASSENGER)) return;
    int motion = Field<int32_t>(ped, 0x4D4);
    bool still = ped->MoveState() <= PEDMOVE_STILL;
    void* best = nullptr;
    bool bestPartial = false;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        if (anim::BlendAmount(a) < 0.3f || anim::BlendDelta(a) < 0.0f || anim::Group(a) == motion) continue;
        if (anim::Flags(a) & anim::F_INDESTRUCTIBLE) continue;  // the face's own (talk, gum: ANIMATION_FACIAL), not an action
        bool partial = (anim::Flags(a) & anim::F_PARTIAL) != 0;
        if (!partial && !still) continue;
        if (!best || partial > bestPartial || (partial == bestPartial && anim::BlendAmount(a) > anim::BlendAmount(best)))
            best = a, bestPartial = partial;
    }
    int block, idx;
    if (!best || !anim::Locate(anim::Hierarchy(best), block, idx)) return;
    s.animBlock = (uint16_t)block;
    s.animIdx = (uint16_t)idx;
    s.animFlags = anim::Flags(best);
    s.animTime = anim::CurrentTime(best);
}

void* FindPlaying(CPed* ped, int block, int idx) {
    int b, i;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
        if (anim::Locate(anim::Hierarchy(a), b, i) && b == block && i == idx) return a;
    return nullptr;
}

// Every task a script gives takes one of 128 records (CPedScriptedTaskRecord::ms_scriptedTasks) until the ped is
// done with it. With all of them taken the game writes the new one in front of the array and crashes later. When
// that happens, finished records are let go first; failing that, one record goes (a script may then see that ped's
// task as over) and the log tells which commands hold them.
constexpr uintptr_t kTaskRecords = 0xC0B1E8;  // 20 bytes each: opcode, event, task, ped, status
using VacantSlotFn = int(__cdecl*)();
VacantSlotFn g_origVacantSlot = nullptr;
int __cdecl HookGetVacantSlot() {
    int i = g_origVacantSlot();
    if (i >= 0) return i;
    Call<0x6085E0>();  // CPedScriptedTaskRecord::Process
    if ((i = g_origVacantSlot()) >= 0) return i;
    static uint64_t loggedAt = 0;
    if (NowMs() - loggedAt > 10000) {
        loggedAt = NowMs();
        std::map<int, int> byOpcode;
        int mirrors = 0;
        for (int k = 0; k < 128; ++k) {
            uintptr_t r = kTaskRecords + (uintptr_t)k * 20;
            ++byOpcode[Ref<int32_t>(r)];
            if (CPed* p = Ref<CPed*>(r + 0x0C); p && Peds::Instance().Find(p) && Peds::Instance().Find(p)->owner != Net().MyId()) ++mirrors;
        }
        std::string s;
        for (auto& [op, n] : byOpcode) s += Format(" %04X:%d", op & 0xFFFF, n);
        LOGW("All 128 scripted task records are taken (%d by mirrored peds), by opcode:%s", mirrors, s.c_str());
    }
    CallMethod<0x608350>((void*)kTaskRecords);  // CPedScriptedTaskRecordData::Flush
    return 0;
}

// A task given by script that the ped has not taken up yet: its event waits (a ped this game does not process now,
// out where its collision is not loaded). Another one would only take another record.
bool TaskPending(CPed* ped) {
    for (int k = 0; k < 128; ++k) {
        uintptr_t r = kTaskRecords + (uintptr_t)k * 20;
        if (Ref<CPed*>(r + 0x0C) == ped && Ref<void*>(r + 0x04)) return true;  // m_ped, m_event
    }
    return false;
}
// A mirror replays its owner's full-body action (a fight stance) with no task behind it, so the game's own move
// animation (CPed::SetMoveAnim) blends its walk or idle over it whenever its move state changes: the stance faded
// and started over after every punch. While such an action holds the mirror, its move animation stays as it is.
using SetMoveAnimFn = void(__fastcall*)(CPed*, void*);
SetMoveAnimFn g_origSetMoveAnim = nullptr;
void __fastcall HookSetMoveAnim(CPed* ped, void* edx) {
    if (NetPed* np = Peds::Instance().Find(ped); np && np->owner != Net().MyId() && np->animFull) return;
    // the other player's copy holding its owner's activity (Anims): an idle over it at a move state change undid a fall
    if (RemotePlayer* rp = Players::Instance().FindByPed(ped); rp && rp->animHeld) return;
    g_origSetMoveAnim(ped, edx);
}

// A gang war's start or end gives every gang member in the pool but Grove a task (CGangWars::TellGangMembersTo): the
// copies of the other game's gangsters among them got one of their own beside their owner's. They are civilians for it.
using TellGangFn = void(__cdecl*)(bool);
TellGangFn g_origTellGang = nullptr;
void __cdecl HookTellGangMembersTo(bool ending) {
    std::vector<std::pair<CPed*, int32_t>> copies;
    CPoolHeader* pool = PedPool();
    for (int i = 0; Net().Online() && pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (NetPed* np = Peds::Instance().Find(ped); np && np->owner != Net().MyId() && ped->PedType() != PED_TYPE_CIVMALE) {
            copies.push_back({ped, ped->PedType()});
            ped->PedType() = PED_TYPE_CIVMALE;
        }
    }
    g_origTellGang(ending);
    for (auto& [ped, type] : copies) ped->PedType() = type;
}

// People a car honks at or crawls up to (CEventHandler::ComputeVehiclePotentialCollisionResponse) shake a fist, turn to
// look or flee only for a player's car (STATUS_PLAYER): the other player's is a copy here and they stood as if it were
// nobody's. It counts as a player's for that call.
using VehCollisionFn = void(__fastcall*)(void*, void*, void*, void*, void*);
VehCollisionFn g_origVehCollision = nullptr;
void __fastcall HookVehCollisionResponse(void* handler, void* edx, void* e, void* active, void* simplest) {
    auto* v = e ? Field<CVehicle*>(e, 0x14) : nullptr;  // CEventPotentialGetRunOver::m_Vehicle
    bool asPlayer = Net().Online() && v && v->Status() == 3 /* STATUS_PHYSICS */ && v->Driver() &&
                    Players::Instance().FindByPed(v->Driver());
    if (asPlayer) v->SetStatus(0);  // STATUS_PLAYER
    g_origVehCollision(handler, edx, e, active, simplest);
    if (asPlayer) v->SetStatus(3);
}

// What a script other than the host's story mission makes, the same script makes in the other game for its own player
// (Ammu-Nation's seller, the range's shooters, a casino's dealers, the gym's boxers, an airport's guard): announced as
// well, each player saw two of everyone on the same spots. Each game keeps its own, as with pickups. Big Smoke's
// courier runs in the host's game alone (Courier.cpp): its people go over as the host's others do.
using SetCreatedByFn = void(__fastcall*)(CPed*, void*, uint8_t);
SetCreatedByFn g_origSetCreatedBy = nullptr;
void __fastcall HookSetCharCreatedBy(CPed* ped, void* edx, uint8_t by) {
    g_origSetCreatedBy(ped, edx, by);
    void* s = CurrentScript();
    // A burglary house's residents (InteriorGroup_c::SetupHousePeds: a BURGLAR brain each) come with the house in both
    // games, and the other game's copies stood in the same beds: the resident twice, one floating. Each game keeps its
    // own outside the host's story mission ("Home Invasion" shares the colonel's family as before).
    if (by == 2 && !(Net().IsHost() ? HostReplicating() : HostMissionRecent(0)) &&
        CallMethod<0x598240, void*>((void*)0xBAF670, ped))  // InteriorManager_c::GetPedsInteriorGroup (g_interiorMan)
        Peds::Instance().MarkLocal(ped, false);
    else if (by == 2 /* PED_MISSION */ && s && !game::script::Running() && !StoryMission(s) && !CourierThread(s))
        Peds::Instance().MarkLocal(ped, Field<uint8_t>(s, 0xDC) != 0);  // m_bIsMission: a side job's
}

// A ped's shot. Its owner's game told its shots by the clip emptying, and the infinite ammo cheat (FULLCLIP,
// CCheat::m_aCheatsActive[72], each game its own) keeps every ped's clip full (0x7428A6): the host's people only aimed in
// the partner's game (Holder, 29.10), and that game's count of its copies' shots read 0.
using WeaponFireFn = bool(__fastcall*)(CWeapon*, void*, CEntity*, CVector*, CVector*, CEntity*, CVector*, CVector*);
WeaponFireFn g_origWeaponFire = nullptr;
bool __fastcall HookWeaponFire(CWeapon* w, void* edx, CEntity* by, CVector* origin, CVector* muzzle, CEntity* target,
                               CVector* at, CVector* alt) {
    bool fired = g_origWeaponFire(w, edx, by, origin, muzzle, target, at, alt);
    if (fired && w->type >= 22 && w->type <= 38 && by && by->IsPed() && Net().Online())  // the guns: pistol to minigun
        Peds::Instance().ShotFired((CPed*)by);
    return fired;
}

// The frame update of a ped's animations takes the first one without looking (velocity extraction, 0x4D1750), so a
// ped left with none crashes the game: the game's own peds always keep their stance or walk under the rest. A mirror's
// action (a fight stance) fades out when its owner's ends, while its move animation was held back (above) and the
// game starts one only when the move state changes: games crashed half a second after a mirrored fight.
bool HasLastingBodyAnim(CPed* ped) {
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
        if (!(anim::Flags(a) & anim::F_PARTIAL) && anim::BlendDelta(a) >= 0.0f) return true;
    return false;
}

// Our gym (GYMLS, GYMSF, GYMLV) has its trainer in the ring with our player: the fight (TASK_KILL_CHAR_ON_FOOT at the
// player after SET_CHAR_HEALTH 1000; LS's sparring pair, going for a player in its corner, keeps 100) and, after the
// first win, a new trainer for the lesson (widescreen, nobody else within 8 m). Kept local as the gym's other people,
// ours boxed nobody in the other game ("дерёшься с невидимым", 28.10).
bool GymOpponent(CPed* ped, CPed* me) {
    void* m = RunningMission();
    if (!me || !m || _strnicmp((const char*)m + 0x08, "gym", 3)) return false;
    void* kill = FindTask(ped, 1000);  // TASK_COMPLEX_KILL_PED_ON_FOOT
    if (kill && Field<CPed*>(kill, 0x10) == me && ped->Health() > 100.0f) return true;  // its target
    return cam::WideScreenOn() && !IsPedDead(me) && (ped->GetPosition() - me->GetPosition()).Length() < 8.0f;
}
}  // namespace

// The people an interior brings (InteriorGroup_c::SetupPeds: a police station's officers, a shop's customers) keep
// area 0 inside: they count as in the interior of a player near them, ours or the other one's (the owner who walked
// out took them out with it: they vanished for the player who stayed inside, and never went to its game)
int8_t AreaOf(CPed* ped) {
    int8_t area = ped->AreaCode();
    if (area != 0) return area;
    CVector p = ped->GetPosition();
    // interiors lie high above the map; a few stand at street level (the Atrium, the Lil' Probe Inn), and the people of
    // the street around them counted as inside and vanished for the player outside
    if (p.z < 500.0f) return 0;
    CPlayerPed* me = FindPlayerPed(0);
    if (me && me->AreaCode() != 0 && (p - me->GetPosition()).Length() < 80.0f) return me->AreaCode();
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.state.area != 0 && (p - rp.state.pos).Length() < 80.0f) return rp.state.area;
    return 0;
}

void Peds::RememberSpecialModel(int model, const char* name) {
    if (model >= kSpecialFirst && model <= kSpecialLast) m_specialNames[model] = name;
    m_ourSpecials.erase(model);  // a script's or a cutscene's now
}

// A slot we asked for (SpecialModelFor) stayed required for good: after ten names a new special copy (Ryder, Sweet, OG
// Loc) waited for a cutscene to free one. Worn by a copy, the model is kept by its reference, and goes with the last one.
void Peds::SpecialWorn(int model) {
    if (m_ourSpecials.erase(model)) MarkModelNotNeeded(model);
}

std::string Peds::SpecialName(int model) const {
    auto it = m_specialNames.find(model);
    return it == m_specialNames.end() ? std::string() : it->second;
}

int Peds::SpecialModelFor(const std::string& name) {
    if (name.empty()) return -1;
    for (int m = kSpecialFirst; m <= kSpecialLast; ++m)
        if (!_stricmp(SpecialName(m).c_str(), name.c_str()) && LoadState(m) != 0) return IsModelLoaded(m) ? m : -1;
    // a name this game's archives lack (another player's mods): RequestSpecialModel takes the garbage position and size
    // of a failed CDirectory::FindItem (0x409F76) and the streamer crashed on it
    void* dir = Ref<void*>(0x8E48D0);  // CStreaming::ms_pExtraObjectsDir
    if (!dir || !CallMethod<0x532450, void*>(dir, name.c_str())) {  // CDirectory::FindItem
        static std::set<std::string> told;  // retried every 100 ms: once is enough for a report
        if (told.insert(name).second) LOGW("Special character %s of the other game is missing here: its copy is not shown", name.c_str());
        return -1;
    }
    for (int m = kSpecialLast; m >= kSpecialFirst; --m)
        if (LoadState(m) == 0) {  // nothing in it, nothing asked for
            g_origRequestSpecial(m, name.c_str(), 2);  // STREAMING_GAME_REQUIRED: until a copy wears it (SpecialWorn)
            m_specialNames[m] = name;
            m_ourSpecials.insert(m);
            LOGI("Special character %s asked for in slot %d", name.c_str(), m);
            return -1;
        }
    return -1;
}

void Peds::OnInstallHooks() {
    mem::HookFn(0x409D10, &HookRequestSpecialModel, &g_origRequestSpecial, "CStreaming::RequestSpecialModel");
    mem::HookFn(0x608540, &HookGetVacantSlot, &g_origVacantSlot, "CPedScriptedTaskRecord::GetVacantSlot");
    mem::HookFn(0x5E4A00, &HookSetMoveAnim, &g_origSetMoveAnim, "CPed::SetMoveAnim");
    mem::HookFn(0x4C0BD0, &HookVehCollisionResponse, &g_origVehCollision, "CEventHandler::ComputeVehiclePotentialCollisionResponse");
    mem::HookFn(0x444530, &HookTellGangMembersTo, &g_origTellGang, "CGangWars::TellGangMembersTo");
    mem::HookFn(0x5E47E0, &HookSetCharCreatedBy, &g_origSetCreatedBy, "CPed::SetCharCreatedBy");
    mem::HookFn(0x742300, &HookWeaponFire, &g_origWeaponFire, "CWeapon::Fire");
    Net().SetHandler(proto::G_PED_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_PED_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_PED_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_PED_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_RESYNC, [](uint8_t from, Reader&) {
        if (!core::IsPlayerReady()) return;
        // its game started over: what it owned before is gone there
        Instance().DropMirrorsOf(from, false);
        Vehicles::Instance().ForgetOwnedBy(from);
        Vehicles::Instance().Reannounce();  // vehicles first: peds inside them need the vehicle
        Instance().Reannounce();
        core::PeerResynced(from);
    });
}

NetPed* Peds::Find(CPed* ped) {
    auto it = m_byPtr.find(ped);
    if (it == m_byPtr.end()) return nullptr;
    auto it2 = m_byId.find(it->second);
    return it2 == m_byId.end() ? nullptr : &it2->second;
}

CPed* Peds::FindOwned(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it != m_byId.end() && it->second.owner == Net().MyId() && IsAlive(it->second) ? it->second.ped : nullptr;
}

int Peds::HandleOf(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it != m_byId.end() && IsAlive(it->second) ? it->second.handle : -1;
}

PedSyncState Peds::Capture(NetPed& np) {
    CPed* ped = np.ped;
    uint64_t now = NowMs();
    PedSyncState s;
    s.time = (uint32_t)now;
    s.pos = ped->GetPosition();
    s.vel = ped->MoveSpeed() * 50.0f;
    s.heading = ped->CurrentRotation();
    s.moveState = (uint8_t)std::clamp(ped->MoveState(), 0, 7);
    s.health = Clamp8(ped->Health());
    s.armour = Clamp8(ped->Armour());
    s.weapon = (uint8_t)ped->ActiveWeapon().type;
    s.area = AreaOf(ped);
    s.flags = (IsPedDead(ped) ? PedSyncState::F_DEAD : 0) | (ped->IsDucking() ? PedSyncState::F_DUCKING : 0) |
              (ped->PedFlags() & (1u << 29) ? PedSyncState::F_NO_DRAG : 0);  // CPed::bDontDragMeOutCar
    s.walkGroup = (uint8_t)std::clamp(Field<int32_t>(ped, 0x4D4), 0, 0xFE);  // CPed::m_nAnimGroup

    // guns: shots show up as the clip emptying (the gun task fires on its own schedule), on foot and out of a car
    CWeapon& w = ped->ActiveWeapon();
    if (w.type != np.lastWeapon) np.lastClip = -1;
    if (np.lastClip >= 0 && w.ammoInClip < np.lastClip) np.lastShotMs = now;
    np.lastClip = w.ammoInClip;
    np.lastWeapon = (uint8_t)w.type;
    auto aimAt = [&](CEntity* target, CVector tp) {
        if (!target && tp.Length() < 1.0f)
            tp = s.pos + CVector{-std::sin(s.heading) * 10.0f, std::cos(s.heading) * 10.0f, 0.0f};
        s.flags |= PedSyncState::F_AIMING;
        s.aim = tp - s.pos;
        if (target && target->IsPed()) {  // a ped target is tracked by the mirror's own task
            auto* tped = (CPed*)target;
            if (tped == FindPlayerPed(0)) {
                s.aimKind = 1;
                s.aimId = Net().MyId();
            } else if (RemotePlayer* rp = Players::Instance().FindByPed(tped)) {
                s.aimKind = 1;
                s.aimId = rp->id;
            } else if (NetPed* tn = Find(tped)) {
                s.aimKind = 2;
                s.aimId = tn->netId;
            }
        }
        if (now - np.lastShotMs < 600) s.flags |= PedSyncState::F_FIRING;
    };

    if (CVehicle* v = ped->VehicleIfIn()) {
        if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v)) {
            s.flags |= PedSyncState::F_IN_VEHICLE;
            s.vehNetId = nv->netId;
            s.seat = (int8_t)PassengerSeatOf(v, ped);
            if (FindTask(ped, TASK_LEAVE_CAR)) s.flags |= PedSyncState::F_LEAVING;
            // a drive-by: CTaskSimpleGangDriveBy::m_pTargetEntity, else m_vecCoords (an offset from a target)
            if (void* db = FindTask(ped, TASK_SIMPLE_GANG_DRIVEBY)) {
                auto* target = Field<CEntity*>(db, 0x34);
                aimAt(target, target ? target->GetPosition() : Field<CVector>(db, 0x38));
            }
        }
        return s;
    }

    void* enter = FindTask(ped, TASK_ENTER_CAR_AS_DRIVER);
    bool asDriver = enter != nullptr;
    if (!enter) enter = FindTask(ped, TASK_ENTER_CAR_AS_PASSENGER);
    if (enter) {
        auto* target = Field<CVehicle*>(enter, 0x0C);  // CTaskComplexEnterCar::m_Car
        // m_TargetDoor once at the door, before that the door the go-to-door task was given (m_TargetSeat). Neither for
        // a passenger of any seat (AMBULAN's patients, seat -1): the game picks the door at the car. Sent as seat 0, the
        // copy went to the front door and was warped to the back seat later (28.10): until then it walks after its owner.
        int door = Field<int32_t>(enter, 0x14) ? Field<int32_t>(enter, 0x14) : Field<int32_t>(enter, 0x1C);
        if (target && IsVehicleValid(target) && IsSyncableVehicle(target) && (asDriver || door))
            if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(target)) {
                s.flags |= PedSyncState::F_ENTERING;
                s.vehNetId = nv->netId;
                s.seat = asDriver ? -1 : (int8_t)SeatOfTargetDoor(door);
                return s;
            }
        if (target && (asDriver || door)) return s;
    }

    if (void* gun = FindTask(ped, TASK_SIMPLE_USE_GUN)) {
        auto* target = Field<CEntity*>(gun, 0x1C);  // CTaskSimpleUseGun::m_pTarget, else m_vecTarget
        aimAt(target, target ? target->GetPosition() : Field<CVector>(gun, 0x20));
        return s;
    }
    CaptureAnim(ped, s);
    return s;
}

void Peds::Announce(NetPed& np) {
    PedSyncState s = Capture(np);
    Writer w(proto::G_PED_CREATE);
    w.U16(np.netId);
    w.U16(np.model);
    w.U8(np.pedType);
    w.Str(np.special, 16);
    WriteState(w, s);
    Net().Send(proto::CH_RELIABLE, w);
    np.lastSendMs = NowMs();
}

void Peds::Reannounce() {
    for (auto& [id, np] : m_byId)
        if (np.owner == Net().MyId() && IsAlive(np)) Announce(np);
}

void Peds::SendState(NetPed& np) {
    Writer w(proto::G_PED_SYNC);
    w.U16(np.netId);
    WriteState(w, Capture(np));
    Net().Send(proto::CH_STATE, w);
    np.lastSendMs = NowMs();
}

void Peds::RespawnCopiesOf(int model) {
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId() || !IsAlive(np) || np.ped->ModelIndex() != model) continue;
        Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR: SpawnMirror makes it again
        m_byPtr.erase(np.ped);
        np.ped = nullptr;
        np.handle = -1;
        LOGI("Mirror ped %04X makes way for another special character", id);
    }
}

void Peds::Forget(uint16_t netId, bool deletePed) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetPed& np = it->second;
    if (deletePed && IsAlive(np)) Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR
    m_byPtr.erase(np.ped);
    m_byId.erase(it);
}

static int OurWantedLevel();

void Peds::ScanOwned() {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t myId = Net().MyId();
    uint64_t now = NowMs();

    for (auto it = m_local.begin(); it != m_local.end();) it = GetPed(it->first) ? std::next(it) : m_local.erase(it);
    HideTwins();

    // peds this game would have dropped (CPopulation::ManagePed) while the partner is nearer go to the partner's game
    for (auto& [ped, to] : m_giveAway)
        if (NetPed* np = IsPedValid(ped) ? Find(ped) : nullptr; np && np->owner == myId && IsAlive(*np) && !IsPedDead(ped))
            GiveAway(np->netId, to);
    m_giveAway.clear();

    // owned peds that vanished or went out of the partner's range
    std::vector<uint16_t> gone;
    for (auto& [id, np] : m_byId)
        if (np.owner == myId && (!IsAlive(np) || RemoteDistance(np.ped->GetPosition(), AreaOf(np.ped)) > SyncOut(np.ped)))
            gone.push_back(id);
    for (uint16_t id : gone) {
        Writer w(proto::G_PED_REMOVE);
        w.U16(id);
        Net().Send(proto::CH_RELIABLE, w);
        Forget(id, false);
    }

    // a wanted other player (while we are not): our cops by it go to its game (CanGiveAwayType). Not for one star: one cop
    // chases then, and the others handed over stood by the player and kept its star from fading (issue #56)
    bool chased = false;
    uint8_t chasedBy = 0;
    CVector chasedAt;
    if (me && OurWantedLevel() == 0)
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.hasState && rp.state.wanted > 1 && !(rp.state.flags & OnFootState::F_DEAD) && rp.state.area == me->AreaCode())
                chased = true, chasedBy = id, chasedAt = rp.state.pos;

    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (ped == me || ped->IsPlayer() || Players::Instance().FindByPed(ped)) continue;
        NetPed* np = Find(ped);
        if (np && np->owner != myId) continue;  // mirror
        float d = RemoteDistance(ped->GetPosition(), AreaOf(ped));
        if (!np) {
            if (d > SyncIn(ped)) continue;
            // a body, or one fading out: the copy of a body its owner removed stays here as this game's (HandleRemove).
            // Announced, it came back to its owner's game standing, died there again and dropped its money each time.
            if (IsPedDead(ped) || (ped->PedFlags2() & 8)) continue;  // CPed::bFadeOut
            // The people a house brings (InteriorGroup_c::SetupPeds: area 0, high up) are made when the player stands
            // under it outside: during the host's mission those of an interior the host is not in stay out of the
            // partner's game ("Home Invasion": a family in the colonel's house, cleared by the mission as its player comes in)
            if (Net().IsHost() && HostReplicating() && ped->CreatedBy() == 1 /* PED_GAME */ && ped->AreaCode() == 0 &&
                ped->GetPosition().z > 500.0f && (!me || me->AreaCode() == 0))
                continue;
            // made by a script each game runs for its own player (MarkLocal); one that drives off is ours to show, and a
            // side job's people (a taxi fare, the vigilante's criminals) are shown out in the street, the gym's trainer in
            // its ring with our player
            if (auto it = m_local.find(GetPedRef(ped)); it != m_local.end() && !ped->InVehicle() &&
                (!it->second || (AreaOf(ped) != 0 && !GymOpponent(ped, me))))
                continue;
            if (CVehicle* v = ped->VehicleIfIn(); v && !IsSyncableVehicle(v)) continue;
            uint16_t id;
            do {
                id = (uint16_t)(((myId + 1) << 12) | (m_nextId++ & 0x0FFF));
            } while (m_byId.count(id));
            NetPed& n = m_byId[id];
            n.netId = id;
            n.ped = ped;
            n.handle = GetPedRef(ped);
            n.owner = myId;
            n.model = ped->ModelIndex();
            n.pedType = (uint8_t)ped->PedType();
            if (auto it = m_specialNames.find(n.model); it != m_specialNames.end()) n.special = it->second;
            m_byPtr[ped] = id;
            Announce(n);
            continue;
        }
        if (np->pedType == PED_TYPE_COP && chased) {
            CVector p = ped->GetPosition();
            if ((p - chasedAt).Length() < 50.0f && (p - me->GetPosition()).Length() > 20.0f && QueueGiveAway(ped, chasedBy))
                LOGD("Cop %04X goes to the game of wanted player %d", np->netId, chasedBy);
        }
        uint64_t interval = d < 40.0f ? 100 : (d < 80.0f ? 200 : 400);
        if (now - np->lastSendMs >= interval) SendState(*np);
    }
}

// The game keeps its ped pool only 8 short of full (CPopulation::RemovePedsIfThePoolGetsFull, 0x616300): with a crowd
// doubled by mods (MixSets) a game sat between that and our reserve, and a copy waited for good ("Robbing Uncle Sam": the
// mission's soldiers, announced last, never came). It takes the place of our own random ped the other game has no copy
// of, out of view, the farthest: the free count stays where it was.
static bool FreeRandomPed() {
    CPoolHeader* pool = PedPool();
    CPlayerPed* me = FindPlayerPed(0);
    CPed* far = nullptr;
    float farD = 0.0f;
    for (int i = 0; pool && me && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->IsPlayer() || p->InVehicle() || p->CreatedBy() != 1 /* PED_GAME */ || Peds::Instance().Find(p) ||
            !CallMethod<0x5DF100, bool>(p) || CallMethod<0x534540, bool>(p))  // CanBeDeleted, GetIsOnScreen
            continue;
        if (float d = (p->GetPosition() - me->GetPosition()).Length(); d > farD) farD = d, far = p;
    }
    if (!far) return false;
    LOGD("Ped pool full: our passer-by %.0f m away makes room for a copy", farD);
    Call<0x610F20>(far);  // CPopulation::RemovePed
    return true;
}

// A copy of one of the other game's passers-by makes room for a copy of its mission's people: the farthest, out of view.
// It comes back once there is room.
bool Peds::EvictRandomCopy() {
    CPlayerPed* me = FindPlayerPed(0);
    NetPed* far = nullptr;
    float farD = 0.0f;
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId() || !IsAlive(np) || np.pedType >= 24 /* PED_TYPE_MISSION1.. */ ||
            (np.model >= kSpecialFirst && np.model <= kSpecialLast) || np.ped->InVehicle() || CallMethod<0x534540, bool>(np.ped))
            continue;  // GetIsOnScreen
        if (float d = (np.ped->GetPosition() - me->GetPosition()).Length(); d > farD) farD = d, far = &np;
    }
    if (!far) return false;
    LOGD("Ped pool full: the copy %04X %.0f m away makes room for a mission's", far->netId, farD);
    Cmd(0x009B).I(far->handle).Run();  // DELETE_CHAR
    m_byPtr.erase(far->ped);
    far->ped = nullptr;
    far->handle = -1;
    return true;
}

CPed* Peds::SpawnMirror(NetPed& np) {
    const PedSyncState& st = np.state;
    int model = np.model;
    if (model >= kSpecialFirst && model <= kSpecialLast) {
        if (np.special.empty()) return nullptr;
        // in whichever slot holds that name here, or a free one: the owner's slot can belong to someone here (a
        // mission's person, another copy), and RequestSpecialModel took the model from under them
        model = SpecialModelFor(np.special);
        if (model < 0) return nullptr;
    } else if (!IsPedModel(model)) {
        static std::set<int> told;
        if (told.insert(model).second) LOGW("Ped %04X of player %d has model %d this game lacks: its copy is not shown", np.netId, np.owner, model);
        return nullptr;
    }
    bool missionPed = np.pedType >= 24 /* PED_TYPE_MISSION1.. */ || (np.model >= kSpecialFirst && np.model <= kSpecialLast);
    if (!IsModelLoaded(model)) {
        // streamed in the background, the spawn is retried (a blocking load per ped stalls the frame). A request with no
        // flag lasts one CStreaming::Update (its PurgeRequestList): the model of a mission's person this game has nowhere
        // lost that race for a minute (the dealer of "Cleaning the Hood", BMYDRUG, 28.10). Such one goes first; the flag
        // clears once its read starts.
        RequestModel(model, missionPed ? 0x10 /* PRIORITY_REQUEST */ : 0);
        return nullptr;
    }
    // the other game's passers-by leave room for its mission's people: the soldiers of "Robbing Uncle Sam", announced last,
    // never came while the street's copies filled the pool
    if (!HasRoom(PedPool(), missionPed ? kKeepPeds : kKeepPeds + 6, "ped") && !FreeRandomPed() && !(missionPed && EvictRandomCopy()))
        return nullptr;
    // A copy is of its owner's type where the class stays a civilian's (CREATE_CHAR makes a CCivilianPed for all but
    // cops, medics and firemen, whose classes bring their own AI): gang members, dealers, criminals, hookers and bums of
    // the other game were plain civilians to this game's gangs, and no fight between the two games' people ever began.
    // It reacts to nothing by itself (the empty decision maker below); this game's people react to it.
    bool typed = (np.pedType >= PED_TYPE_GANG1 && np.pedType <= PED_TYPE_DEALER) ||
                 (np.pedType >= PED_TYPE_CRIMINAL && np.pedType <= PED_TYPE_PROSTITUTE);
    int pedType = typed ? np.pedType : np.pedType == PED_TYPE_CIVFEMALE ? PED_TYPE_CIVFEMALE : PED_TYPE_CIVMALE;
    int handle = -1;
    if (st.flags & PedSyncState::F_IN_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        if (!nv || !Vehicles::Alive(*nv)) return nullptr;  // wait for the vehicle
        if (st.seat < 0) {
            if (nv->veh->Driver()) return nullptr;
            Cmd(0x0129).I(nv->handle).I(pedType).I(model).Out(&handle).Run();  // CREATE_CHAR_INSIDE_CAR
        } else {
            if (st.seat >= 8 || nv->veh->Passengers()[st.seat]) return nullptr;
            Cmd(0x01C8).I(nv->handle).I(pedType).I(model).I(st.seat).Out(&handle).Run();  // CREATE_CHAR_AS_PASSENGER
        }
    } else {
        Cmd(0x009A).I(pedType).I(model).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Out(&handle).Run();  // CREATE_CHAR
    }
    CPed* ped = GetPed(handle);
    if (!ped) return nullptr;
    SpecialWorn(model);
    Cmd(0x060B).I(handle).I(kEmptyDecisionMaker).Run();  // SET_CHAR_DECISION_MAKER: reacts to nothing by itself
    Cmd(0x02AB).I(handle).I(1).I(1).I(1).I(1).I(1).Run();  // SET_CHAR_PROOFS: damage is decided by the owner
    Cmd(0x087E).I(handle).I(0).Run();                      // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD false
    ped->CurrentRotation() = ped->AimingRotation() = st.heading;
    ped->AreaCode() = st.area;
    np.ped = ped;
    np.handle = handle;
    m_byPtr[ped] = np.netId;
    return ped;
}

void Peds::HandleCreate(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint16_t model = r.U16();
    uint8_t pedType = r.U8();
    std::string special = r.Str(16);
    PedSyncState st;
    ReadState(r, st);
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || m_byId.count(id)) return;
    NetPed& np = m_byId[id];
    np.netId = id;
    np.owner = from;
    np.model = model;
    np.pedType = pedType;
    np.special = special;
    np.state = st;
    np.hasState = true;
    np.recvMs = NowMs();
    SpawnMirror(np);  // may fail for now: retried from ApplyMirror
}

void Peds::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    // One in a car this game keeps (Vehicles::KeepsWhenDeleted: deleted by its owner next to our player) or has kept: it
    // stays in it as this game's own, and the car drives on (the crew of a police car after the host's death)
    if (NetPed& np = it->second; IsAlive(np) && !IsPedDead(np.ped) && np.ped->InVehicle() && np.ped->Vehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find(np.ped->Vehicle());
        if (!nv || !Vehicles::Instance().IsMirror(*nv) || Vehicles::Instance().KeepsWhenDeleted(*nv)) {
            LOGI("Ped %04X: its owner deleted it in a car we keep: kept", id);
            Adopt(np);
            return Forget(id, false);
        }
    }
    // a body: its owner fades it out (CPed::bFadeOut) before it goes; deleted here at once, it vanished in a blink
    if (NetPed& np = it->second; IsAlive(np) && IsPedDead(np.ped) && !np.ped->InVehicle()) {
        np.ped->Money() = 0;  // this game's own ped from now on: its death task would drop the money again
        Cmd(0x034F).I(np.handle).Run();  // REMOVE_CHAR_ELEGANTLY
        return Forget(id, false);
    }
    Forget(id, true);
}

bool Peds::CanGiveAwayType(const NetPed& np) {
    // One with a script brain stays with its game (the cashier of a food place, Ammu-Nation's seller, the barber, the
    // tattooist: the population put it at its attractor and the brain runs it): handed to the other player's game as the
    // owner walked out, it stood there as a copy with neither, the attractor looked empty, and that game put its own
    // cashier beside it
    if (np.ped && ((Field<uint8_t>(np.ped, 0x476) & 0x80) || (Field<uint8_t>(np.ped, 0x477) & 1)))  // bHasAScriptBrain, bWaitingForScriptBrainToLoad
        return false;
    return np.pedType != PED_TYPE_COP && np.pedType != PED_TYPE_MEDIC && np.pedType != PED_TYPE_FIREMAN;
}

static int OurWantedLevel() {
    void* wanted = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return wanted ? Field<int32_t>(wanted, 0x2C) : 0;
}

// Police chase only their own game's player (FindPlayerWanted): the other game's cops walked past a wanted player as if
// it were not there. A cop on foot goes to the game of a wanted player while ours is not wanted, and is a cop there
// (Adopt): its own police AI takes up the chase.
bool Peds::CanGiveAwayType(const NetPed& np, uint8_t to) {
    if (np.pedType != PED_TYPE_COP) return CanGiveAwayType(np);
    const RemotePlayer* rp = Players::Instance().Get(to);
    return rp && rp->hasState && rp->state.wanted > 1 && !(rp->state.flags & OnFootState::F_DEAD) && OurWantedLevel() == 0;
}

// A passer-by of this game: alive, on foot, not a script's (a mission's ped stays with its script), not the player's
// recruit and not in view (CPed::CanBeDeleted and CEntity::GetIsOnScreen, as the engine's own removal)
bool Peds::QueueGiveAway(CPed* ped, uint8_t to, bool noGround) {
    NetPed* np = Find(ped);
    if (!np || np->owner != Net().MyId() || ped->InVehicle() || ped->CreatedBy() != 1 /* PED_GAME */ || IsPedDead(ped) ||
        !CanGiveAwayType(*np, to) || !CallMethod<0x5DF100, bool>(ped) || (!noGround && CallMethod<0x534540, bool>(ped)))  // CanBeDeleted, GetIsOnScreen
        return false;
    // one busy with a threat (fighting, shooting at, fleeing from someone) stays with its game while that has ground:
    // handed over, it turned into a calm stroller in the middle of its fight
    if (!noGround)
        for (int task : {1000, 1001, 1002, 909, 911, 927})  // kill ped on foot (3), flee entity, smart flee entity, flee any means
            if (FindTask(ped, task)) return false;
    m_giveAway[ped] = to;
    return true;
}

// Our player wants one of the other game's people: a Grove member at the group key (CPlayerPed::ProcessGroupBehaviour
// recruits PED_TYPE_GANG2, and a copy has its owner's type since 0.3.0: Players keeps the game's own recruit off copies),
// a hooker next to our stopped car (she solicits only FindPlayerPed). Its owner hands it over (G_PED_OWNER naming
// ourselves is the request), and it joins us here.
void Peds::AskForTheirs() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !core::IsPlayerReady()) return;
    auto ask = [&](NetPed& np) {
        if (NowMs() - np.askedMs < 2000) return;
        np.askedMs = NowMs();
        Writer w(proto::G_PED_OWNER);
        w.U16(np.netId);
        w.U8(Net().MyId());
        Net().Send(proto::CH_RELIABLE, w);
        LOGD("Asked player %d for its ped %04X (type %d)", np.owner, np.netId, np.pedType);
    };
    CPed* target = me->TargetedObject() && me->TargetedObject()->IsPed() ? (CPed*)me->TargetedObject() : Field<CPed*>(me, 0x79C);
    if (target && !me->InVehicle() && CallMethod<0x541190, bool>(GetPad(0)))  // lock-on or mouse target; CPad::GetGroupControlForward
        if (NetPed* np = Find(target); np && np->owner != Net().MyId() && np->pedType == PED_TYPE_GANG2 && !IsPedDead(target))
            ask(*np);
    CVehicle* car = me->InVehicle() ? me->Vehicle() : nullptr;
    if (car && car->Driver() == me && car->MoveSpeed().Length() * 50.0f < 1.0f)
        for (auto& [id, np] : m_byId)
            if (np.owner != Net().MyId() && np.pedType == PED_TYPE_PROSTITUTE && IsAlive(np) && !np.ped->InVehicle() &&
                (np.ped->GetPosition() - car->GetPosition()).Length() < 8.0f)
                ask(np);
}

void Peds::GiveAway(uint16_t netId, uint8_t to) {
    Writer w(proto::G_PED_OWNER);
    w.U16(netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGD("Ped %04X handed over to player %d", netId, to);
    Forget(netId, true);
}

// The owner handed its ped over: the mirror is this game's own from now on, an ordinary ped (its own decision maker
// and a wander task, CTheScripts::CleanUpThisPed); in a car handed over with it, it drives on.
void Peds::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end()) return;
    NetPed& np = it->second;
    // the other player asks for our ped (AskForTheirs): a passer-by goes to its game, as on a hand-over
    if (np.owner == Net().MyId() && to == from && from != Net().MyId()) {
        if (IsAlive(np) && !IsPedDead(np.ped) && np.ped->CreatedBy() == 1 /* PED_GAME */ && !np.ped->InVehicle() &&
            CanGiveAwayType(np) && CallMethod<0x5DF100, bool>(np.ped)) {  // CPed::CanBeDeleted: no mission's, no one's recruit
            LOGI("Player %d asked for ped %04X: it goes to its game", from, id);
            GiveAway(id, from);
        }
        return;
    }
    if (np.owner != from) return;
    if (to != Net().MyId() || !IsAlive(np) || IsPedDead(np.ped)) return Forget(id, true);
    bool asked = NowMs() - np.askedMs < 4000;
    uint8_t type = np.pedType;
    Adopt(np);
    // asked for it to recruit: it joins now (the key was let go before it was ours), unless a script closed the group
    if (CPlayerPed* me = FindPlayerPed(0); asked && type == PED_TYPE_GANG2 && me->PlayerData() &&
                                          !(Field<uint32_t>(me->PlayerData(), 0x34) & 0x100)) {
        CallMethod<0x60C840>(me, np.ped);  // CPlayerPed::MakeThisPedJoinOurGroup
        LOGI("Ped %04X joins our group", id);
    }
    Forget(id, false);
    LOGD("Ped %04X taken over from player %d", id, from);
}

// a mirror becomes this game's own ped: it can be hurt, drops its loot, and the population manages it
void Peds::Adopt(NetPed& np) {
    // A cop's copy is a civilian (SpawnMirror): a real cop of its model takes its place (CREATE_CHAR type 6 makes a
    // CCopPed, its model kept), released to the police's own AI (CleanUpThisPed: CTaskComplexWanderCop, which pursues)
    if (np.pedType == PED_TYPE_COP && !np.ped->InVehicle()) {
        CVector p = np.ped->GetPosition();
        int cop = -1;
        Cmd(0x009A).I(PED_TYPE_COP).I((int)np.ped->ModelIndex()).F(p.x).F(p.y).F(p.z - 1.0f).Out(&cop).Run();  // CREATE_CHAR
        if (CPed* c = GetPed(cop)) {
            c->CurrentRotation() = c->AimingRotation() = np.ped->CurrentRotation();
            c->AreaCode() = np.ped->AreaCode();
            m_byPtr.erase(np.ped);
            Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR: the stand-in
            np.ped = c;
            np.handle = cop;
            Cmd(0x01C2).I(cop).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED
            LOGI("Cop %04X is a cop of this game now (model %d)", np.netId, (int)c->ModelIndex());
            return;
        }
    }
    Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();  // SET_CHAR_PROOFS off
    Cmd(0x087E).I(np.handle).I(1).Run();                      // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD
    Cmd(0x01C2).I(np.handle).Run();                           // MARK_CHAR_AS_NO_LONGER_NEEDED
    // A copy reacts to nothing (SpawnMirror): ours now, it is what its owner had again and acts by its kind (a gang member
    // stood still under fire). Cops are made cops above; medics and firemen are other classes: they stay civilians.
    if (np.pedType > PED_TYPE_COP && np.pedType != PED_TYPE_MEDIC && np.pedType != PED_TYPE_FIREMAN && np.pedType <= 31) {
        // off the count of the type it was made with, onto its own: else each one left a civilian too many and a gang
        // member, dealer or hooker too few in the population's counts, and the streets' mix drifted in a long session
        Call<0x611570>(np.ped, true);  // CPopulation::UpdatePedCount
        np.ped->PedType() = np.pedType;
        Call<0x611570>(np.ped, false);
    }
    CallMethod<0x5E06E0>(np.ped);  // CPed::SetPedDefaultDecisionMaker: the one of its stats, now that no script holds it
    // a script ped's senses (CPed::SetCharCreatedBy): heard and saw 30 m and called no friends; a passer-by's again
    void* in = np.ped->Intelligence();
    bool gang = np.pedType >= PED_TYPE_GANG1 && np.pedType <= PED_TYPE_GANG10;
    Field<float>(in, 0xBC) = Field<float>(in, 0xC0) = gang ? 40.0f : 15.0f;  // m_fHearingRange, m_fSeeingRange
    Field<uint32_t>(in, 0xC4) = 3;                                            // m_nDmNumPedsToScan
    Field<float>(in, 0xC8) = 15.0f;                                           // m_fDmRadius
}

void Peds::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    PedSyncState st;
    ReadState(r, st);
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    NetPed& np = it->second;
    if ((int32_t)(st.time - np.state.time) < 0) return;  // reordered
    np.state = st;
    np.recvMs = NowMs();
}

void Peds::ApplyMirror(NetPed& np) {
    const PedSyncState& st = np.state;
    uint64_t now = NowMs();
    if (!np.ped) {
        if (np.deadApplied) return;  // a body the game took away: made again, it stood up and died once more
        if (now - np.lastTaskMs >= 100) {
            np.lastTaskMs = now;
            SpawnMirror(np);
        }
        return;
    }
    CPed* ped = np.ped;
    if (!IsAlive(np)) {  // deleted by the game (should not happen to mission peds): spawn again
        m_byPtr.erase(ped);
        np.ped = nullptr;
        return;
    }
    ped->AreaCode() = st.area;
    if (st.flags & PedSyncState::F_DEAD) {
        if (!np.deadApplied) {
            np.deadApplied = true;
            // dead on foot in its game (a cyclist shot falls off its bike there), still riding here: it died in the seat,
            // and the body sat on the bike's copy and flew about with it (Hh hh, 26.10). Off first, where its body lies.
            if (ped->InVehicle() && !(st.flags & PedSyncState::F_IN_VEHICLE))
                Cmd(0x0362).I(np.handle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();  // proofs off so it can die
            bool ours = now - np.lastHitMs < 2000;
            if (ours) {  // our player killed it: the loot falls here (its owner drops none)
                Cmd(0x087E).I(np.handle).I(1).Run();  // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD
                bool payless = np.pedType == PED_TYPE_COP || np.pedType == PED_TYPE_MEDIC || np.pedType == PED_TYPE_FIREMAN;
                // SET_CHAR_MONEY: a script ped drops only the money a script gave it
                Cmd(0x03FE).I(np.handle).I(payless ? 0 : ped->Money()).Run();
            }
            // It falls where its owner's lies (a running copy lagged behind it), and of our hit as the game throws one:
            // TASK_DIE plays one backward fall for every death ("they just fall over"). No killer: its owner's
            // KILLED_BY_YOU credits the kill once.
            if (!ped->InVehicle() && (ped->GetPosition() - st.pos).Length() < 6.0f) SetEntityPosition(ped, st.pos);
            if (ours && !ped->InVehicle()) {
                np.dying = true;
                Call<0x73A530, bool>(ped, (CEntity*)nullptr, (int)np.hitWeapon, 1000, (int)np.hitPiece, (int)np.hitDir);  // CWeapon::GenerateDamageEvent
                np.dying = false;
                LOGI("Mirror %04X dies of our hit (weapon %d, piece %d, dir %d)", np.netId, np.hitWeapon, np.hitPiece, np.hitDir);
            }
            if (ped->Health() > 0.0f) Cmd(0x05BE).I(np.handle).Run();  // TASK_DIE: killed by someone else
            np.corpseMs = now;
        } else if (!ped->InVehicle() && now - np.corpseMs > 500 &&
                   (st.vel.Length() > 2.0f || (ped->GetPosition() - st.pos).Length() > 1.5f)) {
            // Its body goes where its owner's does: one thrown far after its death (a "super punch" mod flings it 20 m,
            // a blast) still lay where the copy fell (issue #22). It flies along at its owner's speed between states.
            np.corpseMs = now;
            SetEntityPosition(ped, st.pos);
            ped->MoveSpeed() = st.vel * (1.0f / 50.0f);
        }
        return;
    }
    ped->Health() = std::max<float>(1.0f, st.health);
    ped->Armour() = st.armour;
    // a driver the owner's mission keeps at the wheel (Smoke on the bike of "Just Business"): pulled off here, our player
    // took the mission's bike over, and on the owner Smoke sat on what had become a copy and went nowhere
    uint32_t& pf = ped->PedFlags();
    pf = (st.flags & PedSyncState::F_NO_DRAG) ? pf | (1u << 29) : pf & ~(1u << 29);
    np.lastClip = ped->ActiveWeapon().ammoInClip;
    np.lastWeapon = (uint8_t)ped->ActiveWeapon().type;

    if (ped->ActiveWeapon().type != st.weapon && now - np.weaponMs > 1000) {
        np.weaponMs = now;
        if (st.weapon != 0) {
            int wmodel = -1;
            Cmd(0x0781).I(st.weapon).Out(&wmodel).Run();  // GET_WEAPONTYPE_MODEL
            if (wmodel > 0 && !IsModelLoaded(wmodel)) {
                // a mission person's gun goes first: with no flag the request lasts one streaming update (see SpawnMirror)
                bool missionPed = np.pedType >= 24 /* PED_TYPE_MISSION1.. */ || (np.model >= kSpecialFirst && np.model <= kSpecialLast);
                RequestModel(wmodel, missionPed ? 0x10 /* PRIORITY_REQUEST */ : 0);
                return;
            }
            // 25000: a ped's ammo from there on is never spent (0x7428BA), its owner decides when it shoots; 500 ran out
            // in a long fight, and the copy only aimed after it
            Cmd(0x01B2).I(np.handle).I(st.weapon).I(25000).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x01B9).I(np.handle).I(st.weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    }
    // A cop holds a pistol the police way, in both hands (CPed::GetWeaponSkill: WEAPSKILL_COP for a pistol of a
    // PED_TYPE_COP), and the copy of a cop is a civilian here (it must not count as police): it held it in one hand.
    // Only the pistol has that skill.
    if (np.pedType == PED_TYPE_COP) Field<uint8_t>(ped, 0x72C) = ped->ActiveWeapon().type == 22 ? 3 : 1;  // CPed::m_nWeaponSkill

    // the copy here of what the owner's ped aims at (the real player when it is ours), -1: the point st.pos + st.aim
    auto aimHandle = [&]() -> int {
        if (st.aimKind == 1) {
            CPed* p = st.aimId == Net().MyId() ? FindPlayerPed(0) : nullptr;
            if (const RemotePlayer* rp = Players::Instance().Get((uint8_t)st.aimId); rp && st.aimId != Net().MyId()) p = rp->ped;
            return p ? GetPedRef(p) : -1;
        }
        if (st.aimKind == 2)
            if (auto it = m_byId.find(st.aimId); it != m_byId.end() && IsAlive(it->second)) return it->second.handle;
        return -1;
    };

    // ---- vehicles: the game's own enter/leave tasks, warps only as a fallback
    if (st.flags & PedSyncState::F_IN_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        if (!nv || !Vehicles::Alive(*nv)) return;
        bool inside = ped->InVehicle() && ped->Vehicle() == nv->veh;
        if (inside && (st.flags & PedSyncState::F_LEAVING) && !np.leaveTaskMs) {
            Cmd(0x05CD).I(np.handle).I(nv->handle).Run();  // TASK_LEAVE_CAR
            np.leaveTaskMs = now;
        }
        int seat = inside ? PassengerSeatOf(nv->veh, ped) : -2;
        if (seat == st.seat) {
            if (np.leaveTaskMs) return;  // getting out: TASK_LEAVE_CAR ends a drive-by by itself
            // a drive-by: the game's gang drive-by task leans out, aims and fires at the owner's target; its shots hurt
            // what this game owns, the player too, as a mirror's gunfire on foot does
            bool aiming = (st.flags & PedSyncState::F_AIMING) && st.weapon != 0;
            int target = aiming ? aimHandle() : -1;
            CVector at = st.pos + st.aim;
            int rate = (st.flags & PedSyncState::F_FIRING) ? 90 : 0;  // m_nFrequencyPercentage: 0 leans out and aims only
            if (void* db = FindTask(ped, TASK_SIMPLE_GANG_DRIVEBY)) {
                if (!aiming || target != np.gunTargetHandle) {
                    CallMethod<0x601420>(ped->Intelligence(), true, false);  // CPedIntelligence::ClearTasks: seated again
                } else {
                    int8_t& freq = Field<int8_t>(db, 0x13);
                    if (rate && !freq) Field<int16_t>(db, 0x16) = -1;  // m_nAttackTimer: drawn again (rate 0 drew up to 10 s)
                    freq = (int8_t)rate;
                    if (target < 0) Field<CVector>(db, 0x38) = at;  // m_vecCoords: the point follows the owner's
                }
            } else if (aiming && ped->ActiveWeapon().type == st.weapon && now - np.gunTaskMs > 1000 && !TaskPending(ped)) {
                if (target >= 0) at = CVector{};  // with a target char the point is an offset from it
                // TASK_DRIVE_BY char, target char, target car, x, y, z, abort radius (the owner ends it), style
                // AI_ALL_DIRN, right-hand seat (the seat decides), fire rate
                Cmd(0x0713).I(np.handle).I(target).I(-1).F(at.x).F(at.y).F(at.z).F(1000.0f).I(8).I(0).I(rate).Run();
                np.gunTargetHandle = target;
                np.gunTaskMs = now;
            }
            return;
        }
        if (now - np.lastTaskMs < 1000) return;
        if (!inside && np.leaveTaskMs && now - np.leaveTaskMs < kCarTaskGraceMs) return;  // getting out like the owner
        if (!inside && np.enterVeh == st.vehNetId && now - np.enterTaskMs < kCarTaskGraceMs) return;  // still getting in
        // someone here is getting in (our player jacking it): the owner's state that it was pulled out comes next, a
        // warp back into the seat put the driver back in front of our player
        if (!inside && Field<uint8_t>(nv->veh, 0x485)) return;  // CVehicle::m_nNumGettingIn
        np.lastTaskMs = now;
        if (ped->InVehicle()) {
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(np.handle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        if (st.seat < 0) {
            if (!nv->veh->Driver()) Cmd(0x036A).I(np.handle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        } else if (SeatOrAny(nv->veh, st.seat) >= 0 && !nv->veh->Passengers()[st.seat]) {
            Cmd(0x0430).I(np.handle).I(nv->handle).I(st.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        }
        return;
    }
    if (ped->InVehicle()) {
        // in before its owner, who still walks up to this car (a mission's people walk, the copy's task runs): it stays.
        // Taken for "out with no leave task", it was warped out, stood by the door and was warped in again when its owner
        // sat down (28.10: "они сначала садятся, потом телепортируются").
        if (NetVehicle* ev = (st.flags & PedSyncState::F_ENTERING) ? Vehicles::Instance().Find(st.vehNetId) : nullptr;
            ev && ev->veh == ped->Vehicle())
            return;
        // pulled out in its game by a player whose copy here gets in now, or ours (a jack): that jack here pulls it out with
        // the game's own animation. Warped out first, it stood up by the door with none (27.10).
        if (PlayerGettingIn(ped->Vehicle()) && (!np.jackWaitMs || now - np.jackWaitMs < kCarTaskGraceMs)) {
            if (!np.jackWaitMs) np.jackWaitMs = now;
            return;
        }
        np.jackWaitMs = 0;
        // out with no leave task in its game (thrown off a bike, out of a crash, pulled out): out here at once, where it
        // is (TASK_LEAVE_CAR climbed off politely, or sat on a sliding bike for seconds)
        if (!np.leaveTaskMs || now - np.leaveTaskMs > kCarTaskGraceMs) {
            Cmd(0x0362).I(np.handle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            np.leaveTaskMs = now;
        }
        return;
    }
    np.jackWaitMs = 0;  // out by the jack: a later one waits again
    // crouching as its owner (behind cover in a shootout): the flag came but was never applied
    if (bool duck = (st.flags & PedSyncState::F_DUCKING) != 0; duck != ped->IsDucking() && now - np.duckMs > 400) {
        np.duckMs = now;
        LOGD("Mirror %04X %s (gun task %d)", np.netId, duck ? "ducks" : "stands up", FindTask(ped, 1017) != nullptr);
        if (duck) {
            CallMethod<0x601230>(ped->Intelligence(), (uint16_t)0);  // CPedIntelligence::SetTaskDuckSecondary
            // That duck (DUCK_TASK_CONTROLLED) lasts while a task keeps it up: its owner's attack does, here nothing did.
            // It ended 4 frames later, came back every 0.4 s and cleared the gun's animation each time, so a cop's copy
            // never fired crouched. It is held as a script's (DUCK_SCRIPT_CONTROLLED) till its owner stands up.
            if (void* d = CallMethod<0x6010A0, void*>(ped->Intelligence(), true))  // CPedIntelligence::GetTaskDuck
                CallMethod<0x6924F0, bool>(d, 4);                                // CTaskSimpleDuck::SetControlType
        } else {
            CallMethod<0x601390>(ped->Intelligence());  // CPedIntelligence::ClearTaskDuckSecondary
        }
    }
    np.leaveTaskMs = 0;
    if (st.flags & PedSyncState::F_ENTERING) {
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        if (nv && Vehicles::Alive(*nv) && np.enterVeh != st.vehNetId) {
            if (st.seat < 0) Cmd(0x05CB).I(np.handle).I(nv->handle).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
            else Cmd(0x05CA).I(np.handle).I(nv->handle).I(10000).I(SeatOrAny(nv->veh, st.seat)).Run();  // TASK_ENTER_CAR_AS_PASSENGER
            np.enterVeh = st.vehNetId;
            np.enterTaskMs = now;
            np.lastTaskMove = 0;
        }
        if (np.enterVeh == st.vehNetId) return;  // the task walks to the car
    } else if (now - np.enterTaskMs > kCarTaskGraceMs) {
        np.enterVeh = 0;
    }

    // ---- on foot
    float age = std::min(0.5f, (float)(now - np.recvMs) / 1000.0f);
    CVector target = st.pos + st.vel * age;
    CVector cur = ped->GetPosition();
    CVector err = target - cur;
    float d = err.Length2D();
    // on a car as its owner's (ATTACH_CHAR_TO_CAR, the girl beside CJ in his lowrider at Cesar's meet): the attachment
    // places it, its animations still play
    bool onCar = ped->AttachedTo() != nullptr;
    // In the air in its game (a SWAT officer on its helicopter's rope, a fall): gravity pulled the copy down here and the
    // 4 m rule below put it back up, again and again. It hangs where its owner is until that lands.
    if (!onCar && (err.z > 1.0f || np.hanging)) {
        bool found = false;
        float ground = Call<0x5696C0, float>(target.x, target.y, target.z, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
        bool was = np.hanging;
        np.hanging = found && target.z - ground > 2.5f;
        if (np.hanging && !was) LOGD("Mirror %04X hangs in the air there (%.1f m up)", np.netId, target.z - ground);
        if (np.hanging) {
            SetEntityPosition(ped, target);
            ped->MoveSpeed() = {0, 0, 0};
            ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
            np.lastTaskMove = 0;
            return;
        }
    }
    if (!onCar && (d > 6.0f || std::fabs(err.z) > 4.0f)) {
        Cmd(0x00A1).I(np.handle).F(target.x).F(target.y).F(target.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        np.lastTaskMove = 0;
        np.gunMode = 0;
        return;
    }

    // guns
    if ((st.flags & PedSyncState::F_AIMING) && st.weapon != 0) {
        CVector aimAt = st.pos + st.aim;
        uint8_t mode = (st.flags & PedSyncState::F_FIRING) ? 2 : 1;
        int targetHandle = aimHandle();
        bool moved = targetHandle < 0 && (aimAt - np.gunTarget).Length() > 1.5f;
        if (mode != np.gunMode || targetHandle != np.gunTargetHandle || moved || now - np.gunTaskMs > 1500) {
            if (targetHandle >= 0)  // TASK_SHOOT_AT_CHAR / TASK_AIM_GUN_AT_CHAR: follows a moving target itself
                Cmd(mode == 2 ? 0x074D : 0x0635).I(np.handle).I(targetHandle).I(2000).Run();
            else  // TASK_SHOOT_AT_COORD / TASK_AIM_GUN_AT_COORD
                Cmd(mode == 2 ? 0x0668 : 0x0667).I(np.handle).F(aimAt.x).F(aimAt.y).F(aimAt.z).I(2000).Run();
            np.gunMode = mode;
            np.gunTargetHandle = targetHandle;
            np.gunTarget = aimAt;
            np.gunTaskMs = now;
        }
        np.lastTaskMove = 0;
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        // Its owner's ped steps while it aims (a cop sidestepping round the player: GunMove_L/R/FWD/BWD), and the copy
        // stood and slid. Its gun task steps the same way: CTaskSimpleUseGun::m_MoveCmd (+0x14: x to the right, y back) is
        // set whole, since at a high frame rate ControlGunMove's small steps never got past the task's 0.1 to move.
        if (void* gun = FindTask(ped, 1017)) {  // TASK_SIMPLE_USE_GUN
            float h = ped->CurrentRotation(), speed = st.vel.Length2D(), x = 0.0f, y = 0.0f;
            if (speed > 0.4f) {
                x = (st.vel.x * std::cos(h) + st.vel.y * std::sin(h)) / speed;
                y = (st.vel.x * std::sin(h) - st.vel.y * std::cos(h)) / speed;
            }
            Field<float>(gun, 0x14) = x, Field<float>(gun, 0x18) = y;
            Field<bool>(gun, 0x0A) = speed > 0.4f;  // m_HasMoveControl
        }
        return;
    }
    if (np.gunMode) {
        np.gunMode = 0;
        np.lastTaskMove = 0;
    }

    // the walk its owner's script gave it (SET_ANIM_GROUP_FOR_CHAR: a drunk, an old man): the copy walked as its model
    if (st.walkGroup != 0xFF && Field<int32_t>(ped, 0x4D4) != st.walkGroup && WalkGroupReady(st.walkGroup)) {
        Field<int32_t>(ped, 0x4D4) = st.walkGroup;
        CallMethod<0x5E4A00>(ped);  // CPed::SetMoveAnim (through its hook: a held action keeps its pose)
        LOGD("Mirror %04X walks with set %d", np.netId, st.walkGroup);
    }

    // action animation (a restart of the same one shows up as its time going back)
    // a looped one going back is its next lap, not a restart (a fight stance would start over every lap)
    bool restarted = st.animBlock != kNoAnim && !(st.animFlags & anim::F_LOOPED) && st.animTime + 0.25f < np.animTime;
    np.animTime = st.animTime;
    if (st.animBlock != np.animBlock || st.animIdx != np.animIdx || restarted) {
        bool partial = st.animBlock != kNoAnim && (st.animFlags & anim::F_PARTIAL);
        bool sameBase = st.animBlock == np.baseBlock && st.animIdx == np.baseIdx;
        bool wasOverlay = np.animBlock != kNoAnim && (np.animBlock != np.baseBlock || np.animIdx != np.baseIdx);
        if (wasOverlay)  // a punch or a guard that ended or changed
            if (void* a = FindPlaying(ped, np.animBlock, np.animIdx)) anim::FadeOut(a);
        if (np.baseBlock != kNoAnim && !partial && (!sameBase || restarted)) {  // the full-body one changes or ends
            if (void* a = FindPlaying(ped, np.baseBlock, np.baseIdx)) anim::FadeOut(a);
            np.baseBlock = kNoAnim;
        }
        np.animBlock = kNoAnim;
        if (st.animBlock != kNoAnim) {
            // the stance an overlay played over is still there: it carries on instead of starting over
            void* a = np.baseBlock != kNoAnim && sameBase ? FindPlaying(ped, np.baseBlock, np.baseIdx) : nullptr;
            if (a) {
                anim::BlendDelta(a) = 4.0f;
            } else if (void* h = anim::Resolve(st.animBlock, st.animIdx)) {  // not loaded yet: retried next frame
                if ((a = anim::Blend(ped->RwObject(), h, st.animFlags, 4.0f))) {
                    float total = anim::TotalTime(h), t = st.animTime + age;
                    LOGD("Mirror %04X plays %d/%d at %.2f of %.2f s (flags %04X%s)", np.netId, st.animBlock, st.animIdx, t, total,
                         st.animFlags, restarted ? ", again" : "");
                    anim::SetCurrentTime(a, (st.animFlags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
                    np.lastTaskMove = 0;
                }
            }
            if (a) {
                np.animBlock = st.animBlock;
                np.animIdx = st.animIdx;
                if (!partial) np.baseBlock = st.animBlock, np.baseIdx = st.animIdx;
            }
        }
        np.animFull = np.baseBlock != kNoAnim;  // a full-body action holds it, also under an overlay
    }
    if (onCar) return;
    if (np.animFull) {  // a full-body action plays: hold the position without locomotion tasks
        if (d > 2.0f) SetEntityPosition(ped, CVector{target.x, target.y, cur.z});  // a scene's warp: there at once
        else if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
        return;
    }

    // walking/running with the normal animations towards the owner's position
    float speed = st.vel.Length2D();
    if (speed > 0.4f || d > 1.2f) {
        uint8_t move = st.moveState >= 7 ? 7 : (st.moveState >= 5 || speed > 3.5f ? 6 : 4);
        CVector goal = target + st.vel * 0.6f;  // aim slightly ahead so the walk does not stop between updates
        bool due = now - np.lastTaskMs > 400 || move != np.lastTaskMove || (goal - np.lastTaskTarget).Length2D() > 1.5f;
        if (due && !TaskPending(ped)) {
            // that far ahead along its old way is in a wall once its owner turns a corner, and the copy walked into the
            // building (issue #43): then it goes where its owner is (CWorld::GetIsLineOfSightClear: buildings, objects)
            CVector from = cur + CVector{0, 0, 0.5f}, to = goal + CVector{0, 0, 0.5f}, walkTo = goal;
            if ((goal - st.pos).Length2D() > 0.3f && !Call<0x56A490, bool>(&from, &to, true, false, false, true, false, false, false))
                walkTo = st.pos;
            Cmd(0x05D3).I(np.handle).F(walkTo.x).F(walkTo.y).F(walkTo.z).I(move).I(2000).Run();  // TASK_GO_STRAIGHT_TO_COORD
            np.lastTaskMs = now;
            np.lastTaskTarget = goal;
            np.lastTaskMove = move;
        }
        // the task alone leaves it behind (a waiting task, something in its way only here, a straight line)
        if (d > 1.5f) SetEntityPosition(ped, cur + CVector{err.x * 0.05f, err.y * 0.05f, 0});
    } else {
        if (np.lastTaskMove != 1 && !TaskPending(ped)) {
            Cmd(0x05BA).I(np.handle).I(1000).Run();  // TASK_STAND_STILL
            np.lastTaskMove = 1;
        }
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
    }
}

void Peds::OnPreProcess() {
    bool ready = Net().Online() && core::IsPlayerReady();
    if (ready && !m_wasReady) {
        // Our game just became playable: whatever the partner announced before is missing here. Sent before any
        // OnPostProcess (the vehicles' scan comes first there): the partner's G_RESYNC handler drops all we announced
        // before it, and the cars of the new world announced in the same frame were gone there for good.
        Writer w(proto::G_RESYNC);
        Net().Send(proto::CH_RELIABLE, w);
    }
    m_wasReady = ready;
    if (!ready) return;
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId()) continue;
        ApplyMirror(np);
        CPed* ped = np.ped;
        if (!IsAlive(np) || !ped->RwObject() || ped->InVehicle() || IsPedDead(ped) || HasLastingBodyAnim(ped)) continue;
        // its stance comes back, as the game would give it (see HasLastingBodyAnim)
        Call<0x4D4610, void*>(ped->RwObject(), Field<int32_t>(ped, 0x4D4), 3 /* ANIM_ID_IDLE */, 4.0f);  // CAnimManager::BlendAnimation
        np.baseBlock = kNoAnim;
        np.animFull = false;
        LOGD("Mirror %04X had no lasting animation: its stance is back", np.netId);
    }
}

void Peds::OnPostProcess() {
    bool ready = Net().Online() && core::IsPlayerReady();
    if (!ready || NowMs() - m_lastScan < 50) return;
    m_lastScan = NowMs();
    ScanOwned();
    AskForTheirs();
    if (NowMs() - m_lastStatsMs >= 5000) {
        m_lastStatsMs = NowMs();
        int owned = 0, mirrors = 0, spawned = 0, acting = 0, aiming = 0, gunTasks = 0;
        for (auto& [id, np] : m_byId) {
            if (np.owner == Net().MyId()) {
                ++owned;
                continue;
            }
            ++mirrors;
            spawned += np.ped != nullptr;
            acting += np.animBlock != kNoAnim;
            aiming += np.gunMode != 0 && IsAlive(np) && !IsPedDead(np.ped);  // the dead keep their last aim
            gunTasks += IsAlive(np) && (FindTask(np.ped, TASK_SIMPLE_USE_GUN) || FindTask(np.ped, TASK_SIMPLE_GANG_DRIVEBY));
        }
        LOGI("Peds: %d announced, %d mirrors (%d spawned, %d animated, %d aiming, %d with a gun task, %d shots)", owned,
             mirrors, spawned, acting, aiming, gunTasks, m_mirrorShots);
        m_mirrorShots = 0;
    }
}

void Peds::OnSessionEnd() {
    // mirrors stay in the world as ordinary random peds
    for (auto& [id, np] : m_byId)
        if (np.owner != Net().MyId() && IsAlive(np)) {
            Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();
            Cmd(0x01C2).I(np.handle).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED (default decision maker, wander task)
        }
    m_byId.clear();
    m_byPtr.clear();
    m_wasReady = false;
    for (int ref : m_hiddenTwins)  // the copies that stood on them are this game's people now
        if (CPed* p = GetPed(ref)) p->Flags() |= 0x80u;
    m_hiddenTwins.clear();
}

// The player left: its people stay in this game as its own (deleted, they left its cars driverless in the street, as MTA
// keeps the elements and picks a new syncer).
void Peds::OnPeerLeave(uint8_t id) { DropMirrorsOf(id, true); }

void Peds::DropMirrorsOf(uint8_t id, bool keep) {
    std::vector<uint16_t> mirrors;
    for (auto& [pid, np] : m_byId)
        if (np.owner == id) mirrors.push_back(pid);
    for (uint16_t pid : mirrors) {
        NetPed& np = m_byId[pid];
        bool adopt = keep && IsAlive(np) && !IsPedDead(np.ped);
        if (adopt) Adopt(np);
        Forget(pid, !adopt);
    }
}

bool Peds::SeatClaimed(CVehicle* v, int seat) {
    NetVehicle* nv = Vehicles::Instance().Find(v);
    if (!nv) return false;
    uint64_t now = NowMs();
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId() || !np.hasState || now - np.recvMs > 3000) continue;
        const PedSyncState& st = np.state;
        if (!(st.flags & (PedSyncState::F_IN_VEHICLE | PedSyncState::F_ENTERING)) || st.vehNetId != nv->netId || st.seat != seat)
            continue;
        if (IsAlive(np) && np.ped->InVehicle() && np.ped->Vehicle() == v) continue;  // already there: the seat shows it
        return true;
    }
    return false;
}

// "Ryder": the host's JFUD deletes the pizzeria's cashier for the mission, which puts its own one 0.9 m away. The
// partner's JFUD (it ran if its player came in by the door) kept its cashier, which stood calm beside the copy with its
// hands up: "два НПС на месте одного" (28.10). Ours of a script of this game is hidden while a copy of the host's
// mission person of its model stands on it, and shown again when they part.
void Peds::HideTwins() {
    for (auto it = m_hiddenTwins.begin(); it != m_hiddenTwins.end();) it = m_local.count(*it) ? std::next(it) : m_hiddenTwins.erase(it);
    bool mission = !Net().IsHost() && HostMissionRecent(0);
    for (auto& [ref, sideJob] : m_local) {
        CPed* own = GetPed(ref);
        if (!own || sideJob) continue;
        bool twin = false;
        for (auto& [id, np] : m_byId)
            twin = twin || (mission && np.owner == Net().HostId() && np.pedType >= 24 /* PED_TYPE_MISSION1.. */ && IsAlive(np) &&
                            np.ped->ModelIndex() == own->ModelIndex() && (np.ped->GetPosition() - own->GetPosition()).Length() < 1.5f);
        if (twin == (m_hiddenTwins.count(ref) != 0)) continue;
        if (twin) {
            own->Flags() &= ~0x80u;  // CEntity::m_bIsVisible
            m_hiddenTwins.insert(ref);
            LOGI("Ped %d (model %d) of this game hidden: a copy of the host's mission person stands on it", ref, (int)own->ModelIndex());
        } else {
            own->Flags() |= 0x80u;
            m_hiddenTwins.erase(ref);
        }
    }
}

void Peds::ShotFired(CPed* ped) {
    NetPed* np = Find(ped);
    if (!np) return;
    if (np->owner == Net().MyId()) np->lastShotMs = NowMs();
    else ++m_mirrorShots;
}

void Peds::MarkLocal(CPed* ped, bool sideJob) {
    m_local[GetPedRef(ped)] = sideJob;
    void* s = CurrentScript();  // none for a house's residents
    LOGD("Ped %d (model %d) made by %.8s stays this game's", GetPedRef(ped), (int)ped->ModelIndex(), s ? (const char*)s + 0x08 : "a house");
}

void Peds::OnGameStarted() {
    m_byId.clear();
    m_byPtr.clear();
    m_giveAway.clear();
    m_local.clear();
    m_hiddenTwins.clear();
    m_wasReady = false;  // ask the partner for its peds again once the player is ready
}

void Peds::OnGameShutdown() { OnGameStarted(); }

}  // namespace coop
