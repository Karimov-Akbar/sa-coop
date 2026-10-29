// Damage between the two games. Who decides a hit:
//  * NPCs and vehicles are damaged only by their owner's game: a hit of the local player on a mirror is
//    sent to the owner, which applies it with the game's own damage functions (the NPC then reacts to
//    the remote player as the attacker: flees, fights back, dies);
//  * shots of the remote player are only replayed here (pad emulation): their damage is dropped, the
//    shooter's game reports its hits itself. Vehicle collisions are the exception: they are simulated
//    by the victim's owner anyway, so they are never forwarded;
//  * everything else (NPC shots, explosions, fire) hurts whatever this game owns, the local player too;
//  * the other player's blasts go off here as well (Fire.cpp) and hurt the peds this game owns: its copies of them are
//    proof against blasts in the shooter's game. Cars take a blast there and the hit is forwarded, as a bullet's;
//  * an NPC's punch or blade on the remote player is sent to that player's game: the remote ped here has every proof,
//    and the NPC's mirror there only plays the animation (its gunfire needs nothing: the mirror there shoots for real).
//    A mirror's own blow on a player counts nowhere: its owner tells the blows. A blow goes with the hitter's move in its
//    game, and the victim's game reacts to that move (FightAs), from the side its player really turns to the hitter;
//  * a ped killed by the other player drops its money and weapon in the killer's game, where the killer is;
//  * a crime is charged by the game of whoever committed it: the owner keeps its own player's wanted level when it
//    applies the other player's hit, and the hitter's game reports the crime itself;
//  * a kill counts (stats, respect, rampages, gang war provocation) in the killer's game: the owner of the ped tells it.
//    The gang wars run in the host's game only, so the partner's kills of the enemy gangs near the host provoke them there;
//  * the players hurt each other only with friendly fire (the host's choice). The shooter's game reports its bullets
//    and blows on the other player (it sees them hit), the victim's game lets the other player's explosions, fire and
//    car hurt its player (the copy here is proof against them, so they never reach the shooter's game);
//  * during a scripted scene (the camera's wide screen) the other player's side hurts nothing here: its player keeps
//    playing while this one watches, and a mission failed when its people got hurt in the scene.
#include <algorithm>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
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

constexpr int WEAPON_LAST_MELEE = 15, WEAPON_PISTOL = 22, WEAPON_SNIPERRIFLE = 34, WEAPON_MINIGUN = 38,
              WEAPON_SPRAYCAN = 41, WEAPON_EXTINGUISHER = 42, WEAPON_RAMMEDBYCAR = 49, WEAPON_RUNOVERBYCAR = 50,
              WEAPON_EXPLOSION = 51, WEAPON_UZI_DRIVEBY = 52;
bool IsCollision(int weapon) { return weapon == WEAPON_RAMMEDBYCAR || weapon == WEAPON_RUNOVERBYCAR; }
// TARGET_PLAYER: an NPC hit the player (netId is the NPC), TARGET_PLAYER_BY_PLAYER: the sender hit the player itself,
// KILLED_BY_YOU: the receiver's hit killed the sender's ped (piece is the headshot), GANG_KILL: the partner killed an
// enemy gang member (weapon is its ped type) for the host's gang wars, TARGET_PED_BY_PED: the sender's ped hit the
// receiver's (a u16 of the attacker follows), TARGET_PED_BY_CAR: a car of the sender's game ran it down (a u16 of the car)
enum : uint8_t {
    TARGET_PED = 1, TARGET_VEHICLE = 2, TARGET_PLAYER = 3, TARGET_PLAYER_BY_PLAYER = 4, KILLED_BY_YOU = 5, GANG_KILL = 6,
    TARGET_PED_BY_PED = 7, TARGET_PED_BY_CAR = 8
};
constexpr int CRIME_DAMAGED_PED = 2, CRIME_DAMAGED_COP = 3, CRIME_DAMAGE_CAR = 4, CRIME_DAMAGE_COP_CAR = 5,
              CRIME_KILL_PED_WITH_CAR = 10, CRIME_KILL_COP_PED_WITH_CAR = 11;

// Our player's hit on a copy is our crime (its owner applies the hit charging nobody), charged as the game charges its
// player's own hits (CEventDamage::ReportCriminalEvent, gunshots as damage to a car included): queued, and the police
// who see it give the stars through the mission's wanted multiplier. CCrime::ReportCrime gave a star at once with a cop
// near, past a multiplier of 0: the partner got stars in missions that give the host none.
void ChargeCrime(CPed* victim, int pedType, int weapon) {
    bool cop = pedType == PED_TYPE_COP;
    int crime;
    if (weapon <= 9) crime = cop ? CRIME_DAMAGED_COP : CRIME_DAMAGED_PED;  // up to the chainsaw
    else if (weapon <= 40 || weapon == WEAPON_SPRAYCAN) crime = cop ? CRIME_DAMAGE_COP_CAR : CRIME_DAMAGE_CAR;
    else if (IsCollision(weapon)) crime = cop ? CRIME_KILL_COP_PED_WITH_CAR : CRIME_KILL_PED_WITH_CAR;
    else return;
    CPlayerPed* me = FindPlayerPed(0);
    void* wanted = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    if (!me || !wanted) return;
    bool dontCare = Call<0x608A40, bool>(pedType);  // CPedType::PoliceDontCareAboutCrimesAgainstPedType
    CVector at = me->GetPosition();
    CallMethod<0x562410>(wanted, crime, &at, (uint32_t)(uintptr_t)victim, dontCare);  // CWanted::RegisterCrime
}

// The engine charges a crime to the player in focus (CEventGroup::Add -> ReportCriminalEvent -> FindPlayerWanted):
// the other player's hits, applied here, raised this player's wanted level. It is put back as it was (CWanted up to
// its police scanner, an audio entity left alone).
struct KeepWanted {
    uint8_t* wanted = nullptr;
    uint8_t saved[0x21C];
    KeepWanted() {
        CPlayerPed* me = FindPlayerPed(0);
        if (me && me->PlayerData()) wanted = (uint8_t*)me->Wanted();
        if (wanted) memcpy(saved, wanted, sizeof saved);
    }
    ~KeepWanted() {
        if (wanted) memcpy(wanted, saved, sizeof saved);
    }
};

// SET_CHAR_ONLY_DAMAGED_BY_PLAYER and SET_CAR_ONLY_DAMAGED_BY_PLAYER (a mission's target, a vigilante's criminals)
// let through only FindPlayerPed's hits (CEventDamage::AffectsPed, CVehicle::CanVehicleBeDamaged): the other
// player is a player too, so the flag is off while its hit is applied
struct AsPlayersHit {
    uint32_t& flags;
    bool had;
    explicit AsPlayersHit(CPhysical* e) : flags(e->PhysicalFlags()), had(flags & CPhysical::PF_INVULNERABLE) {
        flags &= ~CPhysical::PF_INVULNERABLE;
    }
    ~AsPlayersHit() {
        if (had) flags |= CPhysical::PF_INVULNERABLE;
    }
};

// a bullet or a blow: the shooter's game sees where it hits
bool IsDirect(int weapon) {
    return weapon <= WEAPON_LAST_MELEE || (weapon >= WEAPON_PISTOL && weapon <= WEAPON_SNIPERRIFLE) || weapon == WEAPON_MINIGUN ||
           weapon == WEAPON_UZI_DRIVEBY;
}
// a spray never reaches a mirror (mirrors are fire-proof and CShotInfo skips them): the owner applies the remote
// player's own spray on its peds
bool IsSpray(int weapon) { return weapon == WEAPON_SPRAYCAN || weapon == WEAPON_EXTINGUISHER; }

CPed* DriverOf(CEntity* e) { return e && e->IsVehicle() ? ((CVehicle*)e)->Driver() : nullptr; }

bool IsLocalPlayerEntity(CEntity* e) {
    CPlayerPed* me = FindPlayerPed(0);
    return e && me && (e == me || DriverOf(e) == me);
}

bool IsRemotePlayerEntity(CEntity* e) {
    if (!e) return false;
    CPed* ped = e->IsPed() ? (CPed*)e : DriverOf(e);
    return ped && Players::Instance().FindByPed(ped);
}

// the other player, its car, or a car or ped its game owns (a copy here)
bool FromOtherSide(CEntity* e) {
    if (!e) return false;
    if (IsRemotePlayerEntity(e)) return true;
    if (e->IsVehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && Vehicles::Instance().IsMirror(*nv);
    }
    NetPed* np = e->IsPed() ? Peds::Instance().Find((CPed*)e) : nullptr;
    return np && np->owner != Net().MyId();
}

void SendDamage(uint8_t kind, uint8_t owner, uint16_t netId, int weapon, float damage, int piece, int dir, const CVector& pos,
                uint16_t attacker = 0, CEntity* hitter = nullptr) {
    Writer w(proto::G_DAMAGE);
    w.U8(kind);
    w.U8(owner);
    w.U16(netId);
    w.U8((uint8_t)weapon);
    w.F32(damage);
    w.U8((uint8_t)piece);
    w.U8((uint8_t)dir);
    w.Vec({pos.x, pos.y, pos.z});
    if (kind == TARGET_PED_BY_PED || kind == TARGET_PED_BY_CAR) w.U16(attacker);
    if (weapon <= WEAPON_LAST_MELEE) {  // the blow's move here (FightAs there): two bytes an older build leaves unread
        void* intel = hitter && hitter->IsPed() ? ((CPed*)hitter)->Intelligence() : nullptr;
        void* fight = intel ? CallMethod<0x600F30, void*>(intel) : nullptr;  // CPedIntelligence::GetTaskFighting
        w.U8(fight ? Field<uint8_t>(fight, 0x24) : 0xFF);  // CTaskSimpleFight::m_nComboSet
        w.U8(fight ? Field<uint8_t>(fight, 0x25) : 0xFF);  // m_nCurrentMove
    }
    Net().Send(proto::CH_RELIABLE, w);
}

// The reaction to a blow (CEventDamage::ComputeDamageAnim: the style's HitA_1..3, a combo's knock-down) and a katana's
// kill (ComputeWillForceDeath) come from the hitter's fight (GetTaskFighting: its secondary attack task). A mirror has
// none, so the victim flinched the same way whatever hit it, and a copy's replayed fight may be a move off (29.10: the
// partner's reactions did not fit the blows). For the damage call the slot shows a fight in the move the hitter's game
// had; only those two bytes of it are read, and the slot gets its own task back right after.
struct FightAs {
    void** slot = nullptr;
    void* saved = nullptr;
    alignas(4) uint8_t task[0x28] = {};  // sizeof(CTaskSimpleFight)
    FightAs(CEntity* hitter, uint8_t set, uint8_t move) {
        if (!hitter || !hitter->IsPed() || set < 4 || set > 16 || move > 4) return;  // m_aComboData's 13 sets, HIT_1..FIGHTIDLE
        auto* intel = (uint8_t*)((CPed*)hitter)->Intelligence();
        if (!intel) return;
        slot = (void**)(intel + 4 + 0x14);  // CTaskManager::m_aSecondaryTasks[TASK_SECONDARY_ATTACK]
        saved = *slot;
        *(uintptr_t*)task = 0x86D684;  // CTaskSimpleFight's vtable (its GetTaskType says 1016)
        task[0x24] = set, task[0x25] = move;
        *slot = task;
    }
    ~FightAs() {
        if (slot) *slot = saved;
    }
    FightAs(const FightAs&) = delete;
    FightAs& operator=(const FightAs&) = delete;
};

// A blow on our player is judged from where the hitter stands to our player as it turns here (ComputePedShotSide, as
// FightHitPed does): the other game judged it against its copy of our player, whose heading lags a little behind
int SideHere(CEntity* hitter, int weapon, int dir) {
    CPlayerPed* me = FindPlayerPed(0);
    if (weapon > WEAPON_LAST_MELEE || !hitter || !me) return dir;
    CVector at = hitter->GetPosition();
    return (at - me->GetPosition()).Length() > 3.0f ? dir : Call<0x5F13F0, int>((CPed*)me, &at);  // CPedGeometryAnalyser::ComputePedShotSide
}

using GenerateDamageFn = bool(__cdecl*)(CPed*, CEntity*, int, int, int, int);
GenerateDamageFn g_origGenerateDamage = nullptr;

// CWeapon::GenerateDamageEvent(victim, creator, weapon, damage, bodyPart, direction): every ped damage
// (bullets, melee, explosions, fire, vehicle hits) goes through it. It tells whether the hit counts (blood, a fight
// hit): one decided in the other game still does here.
bool __cdecl HookGenerateDamageEvent(CPed* victim, CEntity* creator, int weapon, int damage, int piece, int dir) {
    if (Net().Online() && victim && core::IsPlayerReady()) {
        // a copy dies of the hit that killed it in its owner's game: the game picks its fall (Peds::ApplyMirror)
        if (NetPed* dn = Peds::Instance().Find(victim); dn && dn->dying)
            return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
        if (cam::WideScreenOn() && FromOtherSide(creator)) return true;  // see the top of the file
        bool fromRemote = IsRemotePlayerEntity(creator);
        if (fromRemote && victim == FindPlayerPed(0))  // see the top of the file
            return Net().FriendlyFire() && !IsDirect(weapon) ? g_origGenerateDamage(victim, creator, weapon, damage, piece, dir) : true;
        // A mirrored ped's blow is its owner's to tell (TARGET_PLAYER from there). A mirror fighting here too (29.10: 29FE,
        // 1C47, 1F13) hit our player a second time out of step, and its blows on the copy went there as this game's
        if (weapon <= WEAPON_LAST_MELEE && !fromRemote && creator && creator->IsPed() &&
            (victim == FindPlayerPed(0) || Players::Instance().FindByPed(victim)))
            if (NetPed* np = Peds::Instance().Find((CPed*)creator); np && np->owner != Net().MyId()) return true;
        if (fromRemote && weapon == WEAPON_EXPLOSION) {  // see the top of the file
            NetPed* np = Peds::Instance().Find(victim);
            if ((np && np->owner != Net().MyId()) || Players::Instance().FindByPed(victim)) return true;
            KeepWanted keep;
            return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
        }
        if (!IsCollision(weapon) && fromRemote && !IsSpray(weapon)) return true;
        if (RemotePlayer* rp = Players::Instance().FindByPed(victim)) {
            if (weapon <= WEAPON_LAST_MELEE && creator && creator->IsPed() && !IsLocalPlayerEntity(creator)) {
                NetPed* attacker = Peds::Instance().Find((CPed*)creator);
                SendDamage(TARGET_PLAYER, rp->id, attacker ? attacker->netId : 0, weapon, (float)damage, piece, dir,
                           victim->GetPosition(), 0, creator);
                LOGD("NPC %04X hit player %d: weapon %d, damage %d", attacker ? attacker->netId : 0, rp->id, weapon, damage);
                return true;
            }
            if (Net().FriendlyFire() && IsDirect(weapon) && IsLocalPlayerEntity(creator)) {
                SendDamage(TARGET_PLAYER_BY_PLAYER, rp->id, 0, weapon, (float)damage, piece, dir, victim->GetPosition(), 0, creator);
                LOGI("Hit player %d: weapon %d, damage %d", rp->id, weapon, damage);
                return true;
            }
        }
        if (NetPed* np = Peds::Instance().Find(victim); np && np->owner != Net().MyId()) {
            if (!IsCollision(weapon) && IsLocalPlayerEntity(creator) && !np->deadApplied) {
                SendDamage(TARGET_PED, np->owner, np->netId, weapon, (float)damage, piece, dir, victim->GetPosition(), 0, creator);
                np->lastHitMs = NowMs();
                np->hitWeapon = (uint8_t)weapon, np->hitPiece = (uint8_t)piece, np->hitDir = (uint8_t)dir;
                ChargeCrime(victim, np->pedType, weapon);
                LOGI("Hit ped %04X of player %d: weapon %d, damage %d", np->netId, np->owner, weapon, damage);
            } else if (!IsCollision(weapon) && creator && creator->IsPed() && !((CPed*)creator)->IsPlayer() && !np->deadApplied) {
                // one of our people hit it (a gang fight between the two games' people): its owner applies it, from its copy
                // of ours (copies have every proof, and our gangs' fists and bullets hurt nobody over there)
                NetPed* by = Peds::Instance().Find((CPed*)creator);
                if (!by || by->owner == Net().MyId()) {
                    SendDamage(TARGET_PED_BY_PED, np->owner, np->netId, weapon, (float)damage, piece, dir, victim->GetPosition(),
                               by ? by->netId : 0, creator);
                    LOGD("Our ped %04X hit ped %04X of player %d: weapon %d, damage %d", by ? by->netId : 0, np->netId, np->owner,
                         weapon, damage);
                }
            }
            return true;
        }
    }
    return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
}

// CPed::KillPedWithCar(car, impulse, ...): a car knocks a ped down. The driver's game decides it: a mirror that the
// local player's car hits is sent to its owner (a mirror is collision-proof and would stand like a stone), and the
// remote player's car here hits nobody by itself, but this game's player with friendly fire.
using KillWithCarFn = void(__fastcall*)(CPed*, void*, CVehicle*, float, int);
KillWithCarFn g_origKillWithCar = nullptr;
void __fastcall HookKillPedWithCar(CPed* ped, void* edx, CVehicle* car, float impulse, int flag) {
    if (Net().Online() && car && core::IsPlayerReady()) {
        if (cam::WideScreenOn() && FromOtherSide(car)) return;
        CPed* driver = car->Driver();
        if (driver && Players::Instance().FindByPed(driver)) {
            // With friendly fire the other player's car hits ours, but not one that lies where it was pulled out, or gets
            // up: the jacker's copy drove off at once (its owner's gas held) over our player and killed it.
            auto busy = [&](int type) { return CallMethod<0x681740, void*>((uint8_t*)ped->Intelligence() + 4, type) != nullptr; };
            bool down = ped->Intelligence() && (busy(823) || busy(824) || busy(825) || busy(818) || busy(821) ||  // be dragged out
                                                busy(205) || busy(208));                                          // get up
            if (Net().FriendlyFire() && ped == FindPlayerPed(0) && !down) g_origKillWithCar(ped, edx, car, impulse, flag);
            return;
        }
        if (NetPed* np = Peds::Instance().Find(ped); np && np->owner != Net().MyId()) {
            // a car this game drives (its traffic, one rolling empty, one our player rides in) runs down the other game's
            // people too: there they stood like stones in its way, here they went on walking
            NetVehicle* nv = Vehicles::Instance().Find(car);
            bool byUs = driver == FindPlayerPed(0);
            bool ourCar = !byUs && nv && !Vehicles::Instance().IsMirror(*nv) && !(driver && driver->IsPlayer());
            if ((byUs || ourCar) && !np->deadApplied && NowMs() - np->lastRamMs > 500) {
                np->lastRamMs = NowMs();
                float hit = std::clamp(impulse, 1.0f, 5000.0f);
                if (byUs) {
                    np->lastHitMs = NowMs();
                    np->hitWeapon = WEAPON_RAMMEDBYCAR;
                    SendDamage(TARGET_PED, np->owner, np->netId, WEAPON_RAMMEDBYCAR, hit, 0, 0, ped->GetPosition());
                    ChargeCrime(ped, np->pedType, WEAPON_RAMMEDBYCAR);
                } else {
                    SendDamage(TARGET_PED_BY_CAR, np->owner, np->netId, WEAPON_RAMMEDBYCAR, hit, 0, 0, ped->GetPosition(), nv->netId);
                }
                LOGI("Rammed ped %04X of player %d (impulse %.0f, %s)", np->netId, np->owner, impulse, byUs ? "our player" : "our car");
                // ...and it falls here at once, as its owner's does there: it stood like a stone in the car's way, the car
                // stopped against it, and it fell only when its owner's state came (Duke Nukem, 27.10). The hit is the
                // game's own (its collision proof off for it), its harm stays the owner's: health high for the hit,
                // then back, no crime of it here (KeepWanted)
                int h = GetPedRef(ped);
                float health = ped->Health();
                Cmd(0x02AB).I(h).I(1).I(1).I(1).I(0).I(1).Run();  // SET_CHAR_PROOFS: collision off
                ped->Health() = 1000.0f;
                {
                    KeepWanted keep;
                    g_origKillWithCar(ped, edx, car, impulse, flag);
                }
                ped->Health() = health;
                Cmd(0x02AB).I(h).I(1).I(1).I(1).I(1).I(1).Run();  // SET_CHAR_PROOFS: all again
            }
            return;
        }
    }
    g_origKillWithCar(ped, edx, car, impulse, flag);
}

// A knife's stealth kill hurts its victim itself (ProcessPlayerWeapon: CEventDamage and ComputeDamageResponse, then the
// victim's own CTaskSimpleStealthKill), never through GenerateDamageEvent: the other player's copy, run here under its
// owner's pad, killed this player with friendly fire off (issue #58). Between the players it is an ordinary stab then.
using StealthTestFn = bool(__fastcall*)(void*, void*, CPed*, bool);
StealthTestFn g_origStealthTest = nullptr;
bool __fastcall HookTestForStealthKill(void* intel, void* edx, CPed* target, bool full) {
    if (Net().Online() && !Net().FriendlyFire() && target && target->IsPlayer()) return false;
    return g_origStealthTest(intel, edx, target, full);
}

using InflictDamageFn = void(__fastcall*)(CVehicle*, void*, CEntity*, int, float, CVector);
InflictDamageFn g_origInflictDamage = nullptr;

// CVehicle::InflictDamage(damager, weapon, damage, position): bullets, explosions and fire on vehicles
void __fastcall HookInflictDamage(CVehicle* v, void* edx, CEntity* damager, int weapon, float damage, CVector pos) {
    if (Net().Online() && v && core::IsPlayerReady()) {
        if (!IsCollision(weapon) && IsRemotePlayerEntity(damager)) return;
        if (cam::WideScreenOn() && FromOtherSide(damager)) return;
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) {
            if (!IsCollision(weapon) && IsLocalPlayerEntity(damager))
                SendDamage(TARGET_VEHICLE, nv->owner, nv->netId, weapon, damage, 0, 0, pos);
            return;
        }
    }
    g_origInflictDamage(v, edx, damager, weapon, damage, pos);
}

using RegisterKillFn = void(__cdecl*)(CPed*, CEntity*, int, bool);
RegisterKillFn g_origRegisterKill = nullptr;

// CEventHandler::RegisterKill(ped, killer, weapon, headshot): a ped died of a hit. This game's ped killed by the other
// player is credited to that player's game and counts here as killed by somebody else (see the top of the file).
void __cdecl HookRegisterKill(CPed* ped, CEntity* killer, int weapon, bool headshot) {
    CPlayerPed* me = FindPlayerPed(0);
    if (Net().Online() && ped && killer && killer->IsPed() && core::IsPlayerReady()) {
        if (RemotePlayer* rp = Players::Instance().FindByPed((CPed*)killer)) {
            if (NetPed* np = Peds::Instance().Find(ped); np && np->owner == Net().MyId())
                SendDamage(KILLED_BY_YOU, rp->id, np->netId, weapon, 1.0f, headshot, 0, ped->GetPosition());
            killer = nullptr;
        } else if (killer == me && !Net().IsHost() && !me->InVehicle() &&
                   (ped->PedType() == PED_TYPE_GANG1 || ped->PedType() == PED_TYPE_GANG3)) {
            SendDamage(GANG_KILL, Net().HostId(), 0, ped->PedType(), 1.0f, 0, 0, ped->GetPosition());
        }
    }
    g_origRegisterKill(ped, killer, weapon, headshot);
}

void HandleDamage(uint8_t from, Reader& r) {
    uint8_t kind = r.U8(), owner = r.U8();
    uint16_t netId = r.U16();
    int weapon = r.U8();
    float damage = r.F32();
    int piece = r.U8(), dir = r.U8();
    Vec3 p = r.Vec();
    uint16_t by = kind == TARGET_PED_BY_PED || kind == TARGET_PED_BY_CAR ? r.U16() : 0;
    uint8_t fightSet = 0xFF, fightMove = 0xFF;  // the blow's move in the hitter's game (none from an older build)
    if (weapon <= WEAPON_LAST_MELEE && r.Remaining() >= 2) fightSet = r.U8(), fightMove = r.U8();
    if (!r.Ok() || owner != Net().MyId() || !core::IsPlayerReady() || damage <= 0.0f || damage > 10000.0f) return;
    // the weapon info (80 entries), the ped pieces (to the head, 9) and the four hit directions are read at these unchecked
    if (weapon > 58 /* WEAPON_FLARE */ || piece > 9 || dir > 3) return;
    if (cam::WideScreenOn()) return;  // a scripted scene: the other game's hits are dropped (see the top of the file)
    const RemotePlayer* rp = Players::Instance().Get(from);
    CEntity* attacker = rp ? rp->ped : nullptr;
    if (kind == TARGET_PED) {
        if (CPed* ped = Peds::Instance().FindOwned(netId)) {
            {
                KeepWanted keep;
                AsPlayersHit player(ped);
                CVehicle* car = rp && rp->ped && rp->ped->InVehicle() ? rp->ped->Vehicle() : nullptr;
                // A member of our player's group (Sweet, Ryder, the Grove members called in) takes the other player's
                // stray bullet as from its leader: from the copy, no player to the game's rules for a player's own
                // group, Sweet fired back and chased the partner ("Sweet's Girl", #11). Enemies answer it as before.
                CEntity* by = attacker;
                if (CPlayerPed* me = FindPlayerPed(0); me && attacker) {
                    void* group = Call<0x5F7E80, void*>(ped);  // CPedGroups::GetPedsGroup
                    if (group && group == Call<0x5F7E80, void*>(me)) by = me;
                }
                FightAs fight(by, fightSet, fightMove);
                if (weapon == WEAPON_RAMMEDBYCAR && car) g_origKillWithCar(ped, nullptr, car, damage, 0);
                else if (weapon != WEAPON_RAMMEDBYCAR) g_origGenerateDamage(ped, by, weapon, (int)damage, piece, dir);
            }
            LOGI("Player %d hit our ped %04X: weapon %d, damage %.0f, health now %.0f", from, netId, weapon, damage, ped->Health());
            if (ped->Health() <= 0.0f) {  // the loot falls in the killer's game
                Cmd(0x087E).I(GetPedRef(ped)).I(0).Run();  // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD false
                ped->Money() = 0;
            }
        }
    } else if (kind == TARGET_PED_BY_CAR) {
        NetVehicle* nv = Vehicles::Instance().Find(by);
        if (CPed* ped = Peds::Instance().FindOwned(netId); ped && nv && Vehicles::Alive(*nv)) {
            KeepWanted keep;
            g_origKillWithCar(ped, nullptr, nv->veh, damage, 0);
            LOGI("A car of player %d hit our ped %04X (impulse %.0f)", from, netId, damage);
        }
    } else if (kind == TARGET_PED_BY_PED) {
        if (CPed* ped = Peds::Instance().FindOwned(netId)) {
            int h = by ? Peds::Instance().HandleOf(by) : -1;  // our copy of its ped: the attacker it fights back
            CPed* hitter = h >= 0 ? GetPed(h) : nullptr;
            KeepWanted keep;
            FightAs fight(hitter, fightSet, fightMove);
            g_origGenerateDamage(ped, hitter, weapon, (int)damage, piece, dir);
            LOGD("Ped %04X of player %d hit our ped %04X: weapon %d, damage %.0f", by, from, netId, weapon, damage);
        }
    } else if (kind == TARGET_PLAYER_BY_PLAYER) {
        if (!Net().FriendlyFire()) return;
        {
            KeepWanted keep;
            FightAs fight(attacker, fightSet, fightMove);
            g_origGenerateDamage(FindPlayerPed(0), attacker, weapon, (int)damage, piece, SideHere(attacker, weapon, dir));
        }
        LOGI("Player %d hit us: weapon %d, damage %.0f, health now %.0f", from, weapon, damage, FindPlayerPed(0)->Health());
    } else if (kind == TARGET_PLAYER) {
        int h = Peds::Instance().HandleOf(netId);
        CPed* npc = h >= 0 ? GetPed(h) : nullptr;
        {
            FightAs fight(npc, fightSet, fightMove);
            g_origGenerateDamage(FindPlayerPed(0), npc, weapon, (int)damage, piece, SideHere(npc, weapon, dir));
        }
        LOGI("An NPC %04X of player %d hit us: weapon %d, damage %.0f (fight set %d, move %d)", netId, from, weapon, damage,
             fightSet == 0xFF ? -1 : fightSet, fightMove == 0xFF ? -1 : fightMove);
    } else if (kind == TARGET_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(netId);
        if (nv && !Vehicles::Instance().IsMirror(*nv) && IsVehicleValid(nv->veh)) {
            KeepWanted keep;
            AsPlayersHit player(nv->veh);
            g_origInflictDamage(nv->veh, nullptr, attacker, weapon, damage, CVector{p.x, p.y, p.z});
        }
    } else if (kind == KILLED_BY_YOU) {
        int h = Peds::Instance().HandleOf(netId);  // its copy here: a civilian, it counts as what its owner has
        if (CPed* ped = h >= 0 ? GetPed(h) : nullptr) {
            NetPed* np = Peds::Instance().Find(ped);
            int32_t type = ped->PedType();
            if (np) ped->PedType() = np->pedType;
            HookRegisterKill(ped, FindPlayerPed(0), weapon, piece != 0);
            ped->PedType() = type;
        }
        LOGI("We killed ped %04X of player %d: weapon %d", netId, from, weapon);
    } else if (kind == GANG_KILL && Net().IsHost()) {
        const CVector& me = FindPlayerPed(0)->GetPosition();
        if ((weapon == PED_TYPE_GANG1 || weapon == PED_TYPE_GANG3) && std::hypot(me.x - p.x, me.y - p.y) < 100.0f)
            Call<0x443950>(weapon);  // CGangWars::AddKillToProvocation
    }
}

// The engine charges a crime to the player in focus (FindPlayerWanted). In this game the other player's car is driven
// by its copy, a PLAYER1 ped: what that car ran over at walking pace (CTaskSimpleKillPedWithCar raises a hit-and-run),
// the cops it made dive out of its way, its blasts and its fires raised this player's wanted level ("I get the stars
// when my partner runs people over"). Its own game charges it; here those reports go nowhere.
bool ByCopy(CEntity* e) { return Net().Online() && IsRemotePlayerEntity(e); }

using ReportCrimeFn = void(__cdecl*)(int, CEntity*, CPed*);
ReportCrimeFn g_origReportCrime = nullptr;
void __cdecl HookReportCrime(int type, CEntity* victim, CPed* by) {  // CCrime::ReportCrime
    if (!ByCopy(by)) g_origReportCrime(type, victim, by);
}

using ReportEventFn = void(__fastcall*)(void*, void*, CPed*);
ReportEventFn g_origHitAndRunReport = nullptr, g_origDamageReport = nullptr, g_origVehDamageReport = nullptr,
              g_origKnockOffReport = nullptr;
void __fastcall HookHitAndRunReport(void* ev, void* edx, CPed* p) {  // CEventVehicleHitAndRun: m_vehicle
    if (!ByCopy(Field<CEntity*>(ev, 0x10))) g_origHitAndRunReport(ev, edx, p);
}
void __fastcall HookDamageReport(void* ev, void* edx, CPed* p) {  // CEventDamage: m_pSourceEntity
    if (!ByCopy(Field<CEntity*>(ev, 0x14))) g_origDamageReport(ev, edx, p);
}
void __fastcall HookVehDamageReport(void* ev, void* edx, CPed* p) {  // CEventVehicleDamage: m_attacker
    if (!ByCopy(Field<CEntity*>(ev, 0x18))) g_origVehDamageReport(ev, edx, p);
}
void __fastcall HookKnockOffReport(void* ev, void* edx, CPed* p) {  // CEventKnockOffBike: m_ped
    if (!ByCopy(Field<CEntity*>(ev, 0x30))) g_origKnockOffReport(ev, edx, p);
}

using DiveFn = void(__fastcall*)(void*, void*, CPed*);
DiveFn g_origDive = nullptr;
void __fastcall HookEvasiveDive(void* task, void* edx, CPed* ped) {  // the car it dives from (+0x08)
    if (!ByCopy(Field<CEntity*>(task, 0x08))) return g_origDive(task, edx, ped);
    KeepWanted keep;  // RegisterCrime_Immediately: no event to drop
    g_origDive(task, edx, ped);
}

class Damage : public Module {
public:
    const char* Name() const override { return "Damage"; }
    void OnInstallHooks() override {
        mem::HookFn(0x73A530, &HookGenerateDamageEvent, &g_origGenerateDamage, "CWeapon::GenerateDamageEvent");
        mem::HookFn(0x6D7C90, &HookInflictDamage, &g_origInflictDamage, "CVehicle::InflictDamage");
        mem::HookFn(0x5F0360, &HookKillPedWithCar, &g_origKillWithCar, "CPed::KillPedWithCar");
        mem::HookFn(0x601E00, &HookTestForStealthKill, &g_origStealthTest, "CPedIntelligence::TestForStealthKill");
        mem::HookFn(0x4B9340, &HookRegisterKill, &g_origRegisterKill, "CEventHandler::RegisterKill");
        mem::HookFn(0x532010, &HookReportCrime, &g_origReportCrime, "CCrime::ReportCrime");
        mem::HookFn(0x4B27D0, &HookHitAndRunReport, &g_origHitAndRunReport, "CEventVehicleHitAndRun::ReportCriminalEvent");
        mem::HookFn(0x4B3440, &HookDamageReport, &g_origDamageReport, "CEventDamage::ReportCriminalEvent");
        mem::HookFn(0x4B50B0, &HookVehDamageReport, &g_origVehDamageReport, "CEventVehicleDamage::ReportCriminalEvent");
        mem::HookFn(0x4B4E80, &HookKnockOffReport, &g_origKnockOffReport, "CEventKnockOffBike::ReportCriminalEvent");
        mem::HookFn(0x655F20, &HookEvasiveDive, &g_origDive, "CTaskSimpleEvasiveDive::StartAnim");
        Net().SetHandler(proto::G_DAMAGE, &HandleDamage);
    }
};

Damage g_damage;
struct Reg {
    Reg() { core::RegisterModule(&g_damage); }
} g_reg;
}  // namespace

}  // namespace coop
