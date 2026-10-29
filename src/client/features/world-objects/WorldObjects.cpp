// Objects of the map (lamp posts, signs, fences, bins, hydrants, trees, boxes, windows) in both games. Each game makes
// them from the same map data, so an object is known by its model and the place its map entry (CDummyObject) holds.
// What a player does to one, the other game repeats: it breaks (CObject::ObjectDamage: smashed, in pieces, its damaged
// model), its window cracks or shatters, or it is knocked loose and moves. The game where it moves sends its pose until
// it lies still, and the other game's copy follows. A game that meets such an object later (it was not streamed in
// then, or it streamed out and in again) gets it as it was left, for a few minutes; after that the map puts it back as
// it always does.
#include <algorithm>
#include <cmath>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;

constexpr float kRange = 150.0f;               // objects this far from our player are looked at
constexpr uint64_t kKeepMs = 5 * 60 * 1000;    // a record still applies to objects made again this long
constexpr uint64_t kFollowMs = 1500;           // the other game's moves stop: it lies still (or the message got lost)
constexpr int kMaxSendsPerScan = 16;           // an explosion knocks dozens loose at once
enum : uint8_t { F_MOVED = 1, F_MOVING = 2 };  // not where the map put it; still moving

struct Thing {
    uint16_t model = 0;
    CVector origin;        // where its map entry puts it: the same in both games
    uint8_t stage = 0;     // 0 whole, 1 damaged (its damaged model, a cracked window), 2 broken (smashed, in pieces)
    bool moved = false, moving = false;
    CVector pos, speed, turn;
    Quat rot;
    uint8_t weapon = 0;    // what broke it (CObject::m_nLastWeaponDamage): the same proofs decide in the other game
    bool remote = false;   // the other game's: this one follows
    bool byCopy = false;   // its first change here was a copy's of the other game's car or person (CopyNear): not ours
    bool following = false;  // our copy plays the other game's moves
    uint64_t updatedMs = 0, sentMs = 0, appliedMs = 0;
};
std::vector<Thing> g_things;
uint64_t g_scanMs = 0, g_statsMs = 0;
int g_sent = 0, g_received = 0;  // messages since the last stats line

uint8_t ObjectType(CObject* o) { return Field<uint8_t>(o, 0x13C); }  // CObject::m_nObjectType: 1 the map's
CEntity* Dummy(CObject* o) {                                           // CObject::m_pDummyObject: its map entry
    auto* d = Field<CEntity*>(o, 0x170);
    auto* pool = Ref<CPoolHeader*>(0xB744A0);  // CPools::ms_pDummyPool, 0x38 bytes each
    uintptr_t off = (uintptr_t)d - (pool ? (uintptr_t)pool->objects : 0);
    if (!d || !pool || (uintptr_t)d < (uintptr_t)pool->objects || off % 0x38 || off / 0x38 >= (uintptr_t)pool->size ||
        pool->byteMap[off / 0x38] < 0)
        return nullptr;
    return d;
}

// what is the players' to break and move: not what the game moves itself or what explodes in the breaker's name
bool Shared(CObject* o) {
    if (ObjectType(o) != 1 || !o->Matrix() || !Dummy(o)) return false;
    if (Field<void*>(o, 0x138)) return false;  // m_pControlCodeList: garage doors, crossings, cranes, sam sites...
    uint8_t special = Field<uint8_t>(o, 0x145);  // m_nSpecialColResponseCase
    if (special >= 6 && special <= 9) return false;  // swinging and locked doors, hanging things, pool balls
    // pool tables (k_pooltablesm, k_pooltableb): POOL2 lowers its own by 10 cm for a game and raises it after, which no
    // record here follows (back where the map has it is no change): Pool.cpp moves the other game's
    if (o->ModelIndex() == 2964 || o->ModelIndex() == 3094) return false;
    void* info = Field<void*>(o, 0x160);  // m_pObjectInfo
    return !info || !Field<uint8_t>(info, 0x1F);  // m_bCausesExplosion: CObject::Explode names this game's player
}

uint8_t StageOf(CObject* o) {
    uint32_t f = o->Flags();
    if (!(f & 1) && !(f & 0x80)) return 2;  // no collision and not drawn: smashed, in pieces, shattered
    if ((f & 0x200) || (Field<uint32_t>(o, 0x140) & 0x10)) return 1;  // m_bRenderDamaged, bHasBrokenGlass
    return 0;
}
bool Moving(CObject* o) { return !(o->Flags() & 4); }  // CEntity::m_bIsStatic off: knocked loose

bool Turned(CObject* o, CEntity* d) {
    if (Field<uint32_t>(o, 0x140) & 0x100) return false;  // bIsLampPost: poles and trees sway in the wind (their matrix)
    const CMatrix* m = o->Matrix();
    if (const CMatrix* dm = d->Matrix()) return m->up.Dot(dm->up) < 0.999f || m->forward.Dot(dm->forward) < 0.999f;
    float a = std::remainder(o->GetHeading() - d->GetHeading(), 6.2831853f);
    return m->up.z < 0.999f || std::fabs(a) > 0.045f;
}

Thing* Find(uint16_t model, const CVector& origin) {
    for (Thing& t : g_things)
        if (t.model == model && (t.origin - origin).Length() < 0.1f) return &t;
    return nullptr;
}

void Send(const Thing& t, bool reliable) {
    Writer w(proto::G_WORLD_OBJ);
    w.U16(t.model);
    w.Vec({t.origin.x, t.origin.y, t.origin.z});
    w.U8(t.stage);
    w.U8((t.moved ? F_MOVED : 0) | (t.moving ? F_MOVING : 0));
    w.U8(t.weapon);
    if (t.moved) {
        w.Vec({t.pos.x, t.pos.y, t.pos.z});
        t.rot.Write(w);
        w.Vec({t.speed.x, t.speed.y, t.speed.z});
        w.Vec({t.turn.x, t.turn.y, t.turn.z});
    }
    Net().Send(reliable ? proto::CH_RELIABLE : proto::CH_STATE, w);
    ++g_sent;
}

// to a pose: out of the world and back (the sector lists, and the moving list a loose object is on)
void Place(CObject* o, const CVector& pos, const Quat& rot, bool loose, const CVector& speed, const CVector& turn) {
    WorldRemove(o);
    if (bool(Moving(o)) != loose) CallVirtual<4>(o, !loose);  // CObject::SetIsStatic
    rot.ToMatrix(*o->Matrix());
    o->Matrix()->pos = pos;
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    ((CPhysical*)o)->MoveSpeed() = loose ? speed : CVector{};
    ((CPhysical*)o)->TurnSpeed() = loose ? turn : CVector{};
    WorldAdd(o);
}

// as the other game broke it: the same damage path shows the same pieces
void Break(CObject* o, uint8_t stage, uint8_t weapon) {
    CVector p = o->GetPosition();
    if (Call<0x46A760, bool>(o)) {  // CGlass::IsObjectGlass: windows break in CGlass, not in ObjectDamage
        if (stage >= 2) Call<0x71BC40>((CEntity*)o, 1000.0f, 0.0f, 0.0f, 0.0f, p.x, p.y, p.z, false);  // WindowRespondsToCollision
        else Field<uint32_t>(o, 0x140) |= 0x10;  // bHasBrokenGlass: cracked
        return;
    }
    CVector up{0, 0, 1};
    for (int i = 0; i < 2 && StageOf(o) < stage; ++i)  // a model that changes first and then smashes takes two
        CallMethod<0x5A0D90>(o, 10000.0f, &p, &up, (CEntity*)nullptr, (int)weapon);  // CObject::ObjectDamage
    if (stage >= 2 && StageOf(o) < 2) o->Flags() &= ~(1u | 0x80u);  // nothing to break into (or proof here): just gone
}

// a copy here in the state the record has (it was made after the change, or the change is the other game's); while
// the other game moves it, our physics carries it on between its poses
void CatchUp(CObject* o, Thing& t, bool driven) {
    if (StageOf(o) < t.stage) Break(o, t.stage, t.weapon);
    if (!t.moved || StageOf(o) >= 2) return;
    if (driven) {
        if (t.appliedMs != t.updatedMs || !t.following) Place(o, t.pos, t.rot, true, t.speed, t.turn);
    } else if (t.following || Moving(o) || (o->GetPosition() - t.pos).Length() > 0.1f) {
        Place(o, t.pos, t.rot, false, {}, {});  // where it lies
    }
    t.appliedMs = t.updatedMs;
    t.following = driven;
}

// What touched a thing at its first change here: the nearest car within 8 m or person on foot within 2 m, a copy of the
// other game's one or this game's own (its player, its cars and people, the ones it took over)
bool CopyNear(const CVector& at) {
    float best = 1e9f;
    bool copy = false;
    uint8_t my = Net().MyId();
    if (CPoolHeader* pool = VehiclePool())
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
            float d = (v->GetPosition() - at).Length();
            if (d >= 8.0f || d >= best) continue;
            best = d;
            NetVehicle* nv = Vehicles::Instance().Find(v);
            copy = nv && nv->owner != my;
        }
    if (CPoolHeader* pool = PedPool())
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
            float d = (p->GetPosition() - at).Length();
            if (p->InVehicle() || d >= 2.0f || d >= best) continue;
            best = d;
            NetPed* np = Peds::Instance().Find(p);
            copy = Players::Instance().FindByPed(p) || (np && np->owner != my);
        }
    return copy;
}

void Scan() {
    CPlayerPed* me = FindPlayerPed(0);
    CPoolHeader* pool = ObjectPool();
    if (!me || !pool) return;
    CVector mp = me->GetPosition();
    uint64_t now = NowMs();
    int sends = 0;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        if (!Shared(o) || (o->GetPosition() - mp).Length2D() > kRange) continue;
        CEntity* d = Dummy(o);
        CVector at = o->GetPosition(), origin = d->GetPosition();
        Thing* t = Find(o->ModelIndex(), origin);
        if (t && t->remote) {
            // the other game moves it right now, or this copy is behind its last word: as there
            bool driven = t->moving && now - t->updatedMs < kFollowMs;
            if (driven || (StageOf(o) <= t->stage && (!Moving(o) || t->following))) {
                CatchUp(o, *t, driven);
                continue;
            }
        } else if (t && !Moving(o) && (StageOf(o) < t->stage || (t->moved && !t->moving && (at - t->pos).Length() > 0.1f))) {
            CatchUp(o, *t, false);  // ours, made again since (streamed out and in): as it was left
            continue;
        }
        uint8_t stage = StageOf(o);
        bool moving = Moving(o) && stage < 2, moved = moving || (at - origin).Length() > 0.05f || Turned(o, d);
        if (stage == 0 && !moved) continue;  // as the map made it
        if (!t) {
            g_things.push_back({});
            t = &g_things.back();
            t->model = o->ModelIndex();
            t->origin = origin;
        }
        bool changed = t->remote || stage != t->stage || moved != t->moved || moving != t->moving ||
                       (moved && !moving && (at - t->pos).Length() > 0.1f);
        // A copy of the other game's car or person at it at its first change here: the copy clipped it off its owner's
        // way (it runs a little off the real one), and the other game tells its own change if there was one there. Sent,
        // posts and traffic lights fell in the other game by themselves (28.10). It stays as it is here.
        if (!t->remote && !t->byCopy && t->stage == 0 && !t->moved && changed && CopyNear(at)) {
            t->byCopy = true;
            LOGD("World object %d at %.1f,%.1f: changed here by a copy of the other game's, not sent", t->model, origin.x, origin.y);
        }
        if (t->byCopy && !t->remote) {
            t->stage = stage, t->moved = moved, t->moving = moving, t->pos = at, t->updatedMs = now;
            continue;
        }
        // A loose one that hardly moves (rocking on a slope, bobbing in the water: the game never makes it static again)
        // went out ten times a second, and then once a second for as long as it lay there: 170 messages in 10 s from 21
        // objects (#18). On the move it goes ten times a second; turning on the spot by a few degrees, once a second;
        // within 5 cm and those degrees of what went last, it lies there and goes no more.
        Quat rot = Quat::FromMatrix(*o->Matrix());
        float dot = std::fabs(rot.x * t->rot.x + rot.y * t->rot.y + rot.z * t->rot.z + rot.w * t->rot.w);
        bool stirring = moving && (at - t->pos).Length() > 0.05f, rocking = moving && !stirring && dot < 0.9995f;  // ~3.6 degrees
        if (!changed && !(stirring && now - t->sentMs >= 100u) && !(rocking && now - t->sentMs >= 1000u)) continue;
        if (sends >= kMaxSendsPerScan) continue;
        ++sends;
        if (t->remote) LOGD("World object %d at %.1f,%.1f: ours now", t->model, origin.x, origin.y);
        t->remote = t->following = false;
        t->stage = stage;
        t->moved = moved;
        t->moving = moving;
        t->pos = at;
        t->rot = rot;
        t->speed = ((CPhysical*)o)->MoveSpeed();
        t->turn = ((CPhysical*)o)->TurnSpeed();
        t->weapon = Field<uint8_t>(o, 0x148);  // m_nLastWeaponDamage
        t->updatedMs = t->sentMs = now;
        Send(*t, changed);
        if (changed && !moving) LOGD("World object %d at %.1f,%.1f: stage %d%s", t->model, origin.x, origin.y, stage, moved ? ", moved" : "");
    }
    // records nobody needs any more
    g_things.erase(std::remove_if(g_things.begin(), g_things.end(),
                                  [&](const Thing& t) { return now - t.updatedMs > kKeepMs; }),
                   g_things.end());
}

// Each game has a copy of the other player, and that copy pushes things too (a mirror car runs a little ahead of the
// real one): the game of the player nearer to it moves it, the host's when both are about as near
bool OursToMove(const CVector& at, uint8_t from) {
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* rp = Players::Instance().Get(from);
    if (!me || !rp || !rp->hasState) return false;
    float mine = (me->GetPosition() - at).Length(), theirs = (rp->state.pos - at).Length();
    return mine + 5.0f < theirs || (std::fabs(mine - theirs) <= 5.0f && Net().IsHost());
}

void Handle(uint8_t from, Reader& r) {
    uint16_t model = r.U16();
    Vec3 o = r.Vec();
    uint8_t stage = r.U8(), flags = r.U8(), weapon = r.U8();
    Vec3 p{}, s{}, tn{};
    Quat q;
    if (flags & F_MOVED) {
        p = r.Vec();
        q.Read(r);
        s = r.Vec();
        tn = r.Vec();
    }
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || stage > 2 || !IsValidModel(model)) return;
    ++g_received;
    CVector origin{o.x, o.y, o.z};
    Thing* t = Find(model, origin);
    if ((flags & F_MOVING) && OursToMove(origin, from)) return;  // our physics moves it; where it lies still comes
    if (!t) {
        g_things.push_back({});
        t = &g_things.back();
        t->model = model;
        t->origin = origin;
    }
    t->remote = true;
    t->stage = std::max(t->stage, stage);
    t->moved = (flags & F_MOVED) != 0;
    t->moving = (flags & F_MOVING) != 0;
    t->pos = {p.x, p.y, p.z};
    t->rot = q;
    t->speed = {s.x, s.y, s.z};
    t->turn = {tn.x, tn.y, tn.z};
    t->weapon = weapon;
    t->updatedMs = NowMs();
}

class WorldObjects : public Module {
public:
    const char* Name() const override { return "WorldObjects"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_WORLD_OBJ, &Handle); }
    void OnSessionEnd() override { g_things.clear(); }
    void OnGameStarted() override {  // a new world: what was broken came back
        for (Thing& t : g_things) t.following = false;
    }
    void OnPeerResync(uint8_t) override {  // its world is new: what both games know goes again
        for (const Thing& t : g_things)
            if (!t.moving) Send(t, true);
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || NowMs() - g_scanMs < 100) return;
        g_scanMs = NowMs();
        Scan();
        if ((g_sent || g_received) && NowMs() - g_statsMs >= 10000) {
            g_statsMs = NowMs();
            LOGI("World objects: %d messages sent, %d received, %zu known", g_sent, g_received, g_things.size());
            g_sent = g_received = 0;
        }
    }
};

WorldObjects g_worldObjects;
struct Reg {
    Reg() { core::RegisterModule(&g_worldObjects); }
} g_reg;
}  // namespace

}  // namespace coop
