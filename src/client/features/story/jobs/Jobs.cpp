// Joint vehicle jobs where both players work (see Jobs.h)
#include "features/story/jobs/Jobs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "features/story/missions/Missions.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"

namespace coop::jobs {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kMissionLocals = 0xA48960;  // CTheScripts::LocalVariablesForCurrentMission
constexpr uintptr_t kScriptParams = 0xA43C78;   // CTheScripts::ScriptParams: the arguments a command read
constexpr int kAmbulance = 416;
constexpr int kPatient = 77, kOwnAmbulance = 34;  // AMBULAN: the patient its subroutine handles, its player's ambulance

int32_t Local(int n) { return Ref<int32_t>(kMissionLocals + (uintptr_t)n * 4); }
int32_t Int(int i) { return ((const int32_t*)kScriptParams)[i]; }
float Float(int i) {
    float f;
    memcpy(&f, (const int32_t*)kScriptParams + i, 4);
    return f;
}
bool Named(void* script, const char* name) { return !_strnicmp(Field<char[8]>(script, 0x08), name, 8); }

// the copy of who joined our job
CPed* Helper() {
    uint8_t id = SideHelper();
    RemotePlayer* rp = id != 0xFF ? Players::Instance().Get(id) : nullptr;
    return rp && rp->ped && IsPedValid(rp->ped) ? rp->ped : nullptr;
}
CVehicle* CarOf(CPed* p) { return p->InVehicle() ? p->Vehicle() : nullptr; }

// as the LOCATE commands measure: each axis apart, no z for the 2D ones
bool Within(const CVector& a, const CVector& b, float x, float y, float z) {
    CVector d = a - b;
    return std::fabs(d.x) < x && std::fabs(d.y) < y && (z <= 0.0f || std::fabs(d.z) < z);
}

// AMBULAN's patients the other player's ambulance came for: the patient, the copy of that ambulance here
std::map<int, int> g_taken;
bool g_seatsOfTheirs = false;  // a patient was just called to theirs: the seat count the script asks next is of that one

int Ambulance(uint16_t op, CPlayerPed* me, CPed* helper) {
    int my = GetPedRef(me), patient = Local(kPatient);
    CVehicle* theirs = CarOf(helper);
    if (op == 0x0106 && Int(0) == my) {  // LOCATE_CHAR_IN_CAR_CHAR_3D $PLAYER patient: 50 m wakes it, 10 m calls it in
        g_seatsOfTheirs = false;
        CPed* who = GetPed(Int(1));
        if (!who) return -1;
        float x = Float(2), y = Float(3), z = Float(4);
        bool near = me->InVehicle() && Within(me->GetPosition(), who->GetPosition(), x, y, z);
        bool nearTheirs = theirs && Within(theirs->GetPosition(), who->GetPosition(), x, y, z);
        // far from its player a patient stands frozen with no collision loaded: by the other player too it moves
        if (x > 20.0f) return near || nearTheirs ? 1 : 0;
        if (Int(1) != patient) return -1;
        if (near) {
            g_taken.erase(patient);  // our ambulance comes first
            return -1;
        }
        if (!nearTheirs || theirs->ModelIndex() != kAmbulance) return -1;
        if (!g_taken.count(patient)) {
            for (auto i = g_taken.begin(); i != g_taken.end();) i = GetPed(i->first) ? std::next(i) : g_taken.erase(i);
            LOGI("Joint ambulance: patient %d goes to the other player's ambulance", patient);
        }
        g_taken[patient] = GetVehicleRef(theirs);
        g_seatsOfTheirs = true;
        return 1;
    }
    auto it = g_taken.find(patient);
    CVehicle* car = it != g_taken.end() ? GetVehicle(it->second) : nullptr;
    CPed* who = GetPed(patient);
    if (!car || !who || car != theirs) return -1;
    if (op == 0x00F4 && Int(0) == my && Int(1) == patient)  // NOT LOCATE_CHAR_IN_CAR_CHAR_2D $PLAYER patient 20: it drove off
        return Within(car->GetPosition(), who->GetPosition(), Float(2), Float(3), 0.0f) ? 1 : 0;
    // LOCATE_STOPPED_CHAR_IN_CAR_3D $PLAYER at the hospital (not the one drawing its corona, which is our player's own):
    // the patient is delivered by the car it sits in
    if (op == 0x0103 && Int(0) == my && Int(7) == 0 && who->InVehicle() && who->Vehicle() == car)
        return car->MoveSpeed().Length() < 0.02f && Within(car->GetPosition(), {Float(1), Float(2), Float(3)}, Float(4), Float(5), Float(6))
                   ? 1
                   : 0;
    return -1;
}

// The ambulance variable (34@) of the command, set to theirs: GET_(MAXIMUM_)NUMBER_OF_PASSENGERS right after the patient
// was called (is there room for it) and TASK_ENTER_CAR_AS_PASSENGER of that patient. A task given again after the
// script's own would be a second script event of the frame for the patient, and which one wins is not certain.
int32_t* AmbulanceBefore(uint16_t op, const uint8_t* ip, CPed* helper) {
    bool seats = op == 0x01E9 || op == 0x01EA;
    if (!seats && op != 0x05CA) return nullptr;
    int patient = Local(kPatient);
    auto it = g_taken.find(patient);
    CVehicle* car = it != g_taken.end() ? GetVehicle(it->second) : nullptr;
    const uint8_t* arg = ip + 2;
    if (seats) {
        bool asked = g_seatsOfTheirs;
        if (op == 0x01EA) g_seatsOfTheirs = false;  // the second of the pair
        if (!asked) return nullptr;
    } else {
        int32_t who = -1;
        if (!game::script::ArgValue(arg, who) || who != patient) return nullptr;
        arg = game::script::NextArg(arg);
    }
    int32_t* var = game::script::OutVar(arg);
    if (!car || car != CarOf(helper) || var != (int32_t*)(kMissionLocals + kOwnAmbulance * 4)) return nullptr;
    return var;
}

// LOCATE_CHAR_ANY_MEANS_CHAR_2D $PLAYER criminal (when it fights) and LOCATE_CHAR_ANY_MEANS_CAR_2D $PLAYER its car (a stuck
// getaway car is put back on the road only away from the player): either player
int Police(uint16_t op, CPlayerPed* me, CPed* helper) {
    if ((op != 0x00F2 && op != 0x0202) || Int(0) != GetPedRef(me)) return -1;
    CEntity* e = op == 0x00F2 ? (CEntity*)GetPed(Int(1)) : (CEntity*)GetVehicle(Int(1));
    if (!e) return -1;
    CVector at = e->GetPosition();
    return Within(me->GetPosition(), at, Float(2), Float(3), 0.0f) || Within(helper->GetPosition(), at, Float(2), Float(3), 0.0f) ? 1 : 0;
}

// TASK_KILL_CHAR_ON_FOOT criminal $PLAYER: its player variable set to the other player when that one is nearer
int32_t* PoliceBefore(uint16_t op, const uint8_t* ip, CPlayerPed* me, CPed* helper) {
    if (op != 0x05E2) return nullptr;
    int32_t criminal = -1;
    const uint8_t* arg = ip + 2;
    if (!game::script::ArgValue(arg, criminal)) return nullptr;
    int32_t* var = game::script::OutVar(game::script::NextArg(arg));
    CPed* who = GetPed(criminal);
    if (!var || *var != GetPedRef(me) || !who) return nullptr;
    float theirs = (helper->GetPosition() - who->GetPosition()).Length(), ours = (me->GetPosition() - who->GetPosition()).Length();
    return theirs < ours ? var : nullptr;
}

// LOCATE_CHAR_ANY_MEANS_CAR_3D $PLAYER burning car 50: far from its player it stands frozen with no collision loaded
int Fire(uint16_t op, CPlayerPed* me, CPed* helper) {
    if (op != 0x0205 || Int(0) != GetPedRef(me)) return -1;
    CVehicle* v = GetVehicle(Int(1));
    if (!v) return -1;
    CVector at = v->GetPosition();
    return Within(me->GetPosition(), at, Float(2), Float(3), Float(4)) || Within(helper->GetPosition(), at, Float(2), Float(3), Float(4)) ? 1
                                                                                                                                     : 0;
}

// STUNT, the BMX rings of the skate park: LOCATE_CHAR_IN_CAR_3D $PLAYER ring 2.5 takes a ring when its player rides
// through it. The other player's bike through one takes it too, and so does its time (28.10: "забирается?" "неа").
int Stunt(uint16_t op, CPlayerPed* me, CPed* helper) {
    if (op != 0x0100 || Int(0) != GetPedRef(me) || !CarOf(helper)) return -1;
    return Within(helper->GetPosition(), {Float(1), Float(2), Float(3)}, Float(4), Float(5), Float(6)) ? 1 : -1;
}

// KICKSTA: a corona counts as its player rides through it (LOCATE_CHAR_ANY_MEANS_3D $PLAYER corona 1.5, then IS_CHAR_IN_MODEL
// $PLAYER 468): the other player's Sanchez there takes it too, after the GO (257@, its 3-2-1, is 10 then). Its other
// IS_CHAR_IN_MODEL checks (the countdown off the bike) stay its own player's.
bool g_kickByHelper = false;
int Kickstart(uint16_t op, CPlayerPed* me, CPed* helper) {
    if (Int(0) != GetPedRef(me)) return -1;
    if (op == 0x00DD) {
        bool by = g_kickByHelper;
        g_kickByHelper = false;
        return by && Int(1) == 468 ? 1 : -1;
    }
    if (op != 0x00FE) return -1;
    CVehicle* bike = CarOf(helper);
    g_kickByHelper = Local(257) == 10 && bike && bike->ModelIndex() == 468 &&
                     Within(helper->GetPosition(), {Float(1), Float(2), Float(3)}, Float(4), Float(5), Float(6));
    return g_kickByHelper ? 1 : -1;
}

// BLOOD: its one LOCATE_CHAR_ANY_MEANS_3D $PLAYER, the checkpoint (+15 s on the clock, +300 health to its player's car):
// the other player's car through it takes it too
int BloodRing(uint16_t op, CPlayerPed* me, CPed* helper) {
    if (op != 0x00FE || Int(0) != GetPedRef(me) || !CarOf(helper)) return -1;
    return Within(helper->GetPosition(), {Float(1), Float(2), Float(3)}, Float(4), Float(5), Float(6)) ? 1 : -1;
}

// QUARRY: the other player's dozer copy pushes our rocks and bombs here (a physical car in our world), and the script counts
// them where they lie. "Rockfall" (34@ 5) takes a rock into its pit only within 30 m of its player (50 for two of them:
// 0x0023 30/50 > 78@, the rock at 62@..64@): within that of the other player too. "Sabotage" (34@ 7) blows a bomb pushed to
// safety once both its player and its dozer are 21 m from it (0x0021 78@ 21, 79@ 21; the bomb at 59@..61@): not while the
// other player is that near. The literal is the command's ScriptParams[0], the variable is read in place.
float LocalF(int n) { return Ref<float>(kMissionLocals + (uintptr_t)n * 4); }
int Quarry(uint16_t op, CPed* helper) {
    int stage = Local(34);
    if (op == 0x0023 && stage == 5 && (Float(0) == 30.0f || Float(0) == 50.0f))
        return (helper->GetPosition() - CVector{LocalF(62), LocalF(63), LocalF(64)}).Length() < Float(0) ? 1 : -1;
    if (op == 0x0021 && stage == 7 && Float(0) == 21.0f)
        return (helper->GetPosition() - CVector{LocalF(59), LocalF(60), LocalF(61)}).Length() < 21.0f ? 0 : -1;
    return -1;
}

// PIMP: a girl is picked up by its player's car (40@, the one it sat in at the start): within 7 m of her
// (LOCATE_CHAR_IN_CAR_CHAR_2D $PLAYER girl 7), stopped, she gets in (TASK_ENTER_CAR_AS_PASSENGER girl 40@), and her run to
// the client goes on with that car and its player in it (IS_CHAR_IN_CAR $PLAYER 40@: out of it 20 s ends the job; the
// locates of $PLAYER by her and the client). The other player's car at her first is that car for her run: 40@ is its copy
// here, and those checks of our player are its player's (29.10: "напарник не может подобрать"). The next girl our car
// picks up gives 40@ back. Its zone checks (where the clients come) stay our player's.
constexpr int kPimpCar = 40, kPimpGirls = 35, kPimpGirl = 37;
int g_pimpOwn = -1;  // our player's own car of the job: 40@ as it began
int Pimp(uint16_t op, CPlayerPed* me, CPed* helper) {
    if (Int(0) != GetPedRef(me)) return -1;
    int32_t& car = Ref<int32_t>(kMissionLocals + kPimpCar * 4);
    CVehicle* ours = CarOf(me);
    if (g_pimpOwn < 0 && ours && GetVehicleRef(ours) == car) g_pimpOwn = car;
    CVehicle* theirs = CarOf(helper);
    if (theirs && theirs->Driver() != helper) theirs = nullptr;  // its own wheel only
    bool swapped = theirs && car == GetVehicleRef(theirs);
    if (op == 0x00F4) {  // LOCATE_CHAR_IN_CAR_CHAR_2D $PLAYER who x y
        CPed* who = GetPed(Int(1));
        if (!who) return -1;
        CVector at = who->GetPosition();
        bool girl = Float(2) == 7.0f && Int(1) == Local(kPimpGirls + Local(kPimpGirl));
        bool oursNear = ours && Within(ours->GetPosition(), at, Float(2), Float(3), 0.0f);
        bool theirsNear = theirs && Within(theirs->GetPosition(), at, Float(2), Float(3), 0.0f);
        if (girl) {  // whose car takes her: ours first
            if (oursNear && g_pimpOwn >= 0 && ours == GetVehicle(g_pimpOwn) && car != g_pimpOwn) {
                car = g_pimpOwn;
                LOGI("Joint pimp job: our car picks the girl up");
            } else if (!oursNear && theirsNear && !swapped) {
                car = GetVehicleRef(theirs);
                LOGI("Joint pimp job: the other player's car picks the girl up");
                return 1;
            }
        }
        if (car == g_pimpOwn || !swapped) return -1;
        return theirsNear ? 1 : 0;
    }
    if (!swapped) return -1;
    if (op == 0x00DB && Int(1) == car) return 1;  // IS_CHAR_IN_CAR $PLAYER 40@: its player at its wheel (theirs above)
    if (op == 0x00F2 || op == 0x0104) {  // LOCATE_CHAR_ANY_MEANS_CHAR_2D/3D $PLAYER who: by its player
        CPed* who = GetPed(Int(1));
        if (!who) return -1;
        return Within(theirs->GetPosition(), who->GetPosition(), Float(2), Float(3), op == 0x0104 ? Float(4) : 0.0f) ? 1 : 0;
    }
    return -1;
}

// ---------------------------------------------------------------- burglary for two
// BURGJB lets its player into the burglary houses (ENABLE_BURGLARY_HOUSES), the game itself lifts their things
// (CPlayerInfo::FindObjectToSteal) and takes one out through the door with its carrier (CEntryExit::
// ProcessStealableObjects). The script counts the thing its player puts down 6 m behind the van (GET_OFFSET_FROM_CAR_IN_
// WORLD_COORDS 50@ 0 -6 0, within 3.5 x 3.5 x 2) with the enter button, and deletes it once let go (5 s at most).
constexpr int kBurglarVan = 609;  // BOXBURG: the job starts in it
// BURGJB's locals: the things in the van (48@), its noise heap (41@, a float), its player in a house (92@), the residents
// woke (98@)
constexpr int kVanThings = 48, kNoiseHeap = 41, kInHouse = 92, kWoke = 98;
// its noise meter ($21172, 'BURG25': the residents' brains, BURGLAR, wake at 100) and a resident up ($7172: one phoned
// the police or armed itself)
constexpr uint32_t kNoiseMeter = 21172, kResidentUp = 7172;
bool g_houses = false;  // the burglary houses are open here for the joint burglary
bool g_atVan = false;   // a thing in hand at the van's back
int g_stashed = -1;     // the thing put down there: into the van once let go
uint64_t g_stashedMs = 0;

CEntity* HeldBy(CPed* ped) { return CallMethod<0x5E02E0, CEntity*>(ped); }  // CPed::GetEntityThatThisPedIsHolding
int32_t& Global(uint32_t offset) { return Ref<int32_t>(addr::ScriptSpace + offset); }
bool OwnBurglary() {
    void* s = RunningMission();
    return s && Named(s, "burgjb");
}

CVehicle* VanNear(const CVector& at) {
    CVehicle* best = nullptr;
    float bestDist = 20.0f;
    CPoolHeader* pool = VehiclePool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (float d = (v->GetPosition() - at).Length(); v->ModelIndex() == kBurglarVan && d < bestDist) bestDist = d, best = v;
    }
    return best;
}

// who joined: a thing in hand put down at the van's back, as BURGJB takes it of its player. True: into the van now.
bool StashAtVan(CPlayerPed* me) {
    CEntity* held = HeldBy(me);
    if (me->InVehicle() || !held || !held->IsObject() || !(Field<uint32_t>(held, 0x140) & 0x2000)) return false;  // bIsLiftable
    CVehicle* van = VanNear(me->GetPosition());
    CMatrix* m = van ? van->Matrix() : nullptr;
    if (!m || !Within(me->GetPosition(), van->GetPosition() - m->forward * 6.0f, 3.5f, 3.5f, 2.0f)) return false;
    g_atVan = true;
    CPad* pad = GetPad(0);
    if (g_stashed != -1 || !pad->NewState.ButtonTriangle || pad->OldState.ButtonTriangle) return false;
    g_stashed = GetObjectRef((CObject*)held);
    g_stashedMs = NowMs();
    LOGI("Joint burglary: our thing (model %d) into the van", held->ModelIndex());
    return true;
}

// A house is furnished from a seed of its place (srand in Interior_c::Init), the same in both games. Each stealable thing
// has a record (g_interiorMan's m_Objects[32], 0x1C each: entity, model, interior, place, stolen), and a house furnished
// again leaves a stolen one out (Interior_c::PlaceObject). A thing one player lifts goes from the other's house as well:
// hidden, no collision, not liftable, its record stolen. Not deleted: the interior deletes its furniture as its player
// leaves (Interior_c::Unfurnish), and a second delete crashes.
constexpr uintptr_t kThings = 0xBB3A1C, kThingCount = 0xBB3A18;  // g_interiorMan (0xBAF670) +0x43AC, +0x43A8
uint8_t* Thing(int i) { return (uint8_t*)(kThings + (uintptr_t)i * 0x1C); }
int ThingCount() { return std::clamp(Ref<int32_t>(kThingCount), 0, 32); }
int ThingModel(int i) { return *(int32_t*)(Thing(i) + 4); }
CVector ThingPlace(int i) { return *(CVector*)(Thing(i) + 0x0C); }
// its object while it is there: the record keeps the pointer of a house left (deleted since)
CObject* ThingObject(int i) {
    auto* o = *(CObject**)Thing(i);
    return o && IsObjectValid(o) && o->ModelIndex() == ThingModel(i) ? o : nullptr;
}
struct Gone {
    int model;
    CVector at;
};
std::vector<Gone> g_gone;       // what the other player lifted: out of our houses too, as they are furnished
CEntity* g_liftTold = nullptr;  // the last thing of ours told about

void HideGone(CPlayerPed* me) {
    CEntity* mine = HeldBy(me);
    for (const Gone& g : g_gone)
        for (int i = 0; i < ThingCount(); ++i) {
            if (ThingModel(i) != g.model || (ThingPlace(i) - g.at).Length() > 0.05f) continue;
            Thing(i)[0x18] = 1;  // stolen: a house furnished again leaves it out
            CObject* o = ThingObject(i);
            if (!o || (CEntity*)o == mine || !(Field<uint32_t>(o, 0x140) & 0x2000)) continue;
            o->Flags() &= ~(0x80u | 1u);            // CEntity: m_bIsVisible, m_bUsesCollision
            Field<uint32_t>(o, 0x140) &= ~0x2000u;  // CObject: bIsLiftable
            LOGI("Joint burglary: a thing (model %d) the other player lifted is gone here too", g.model);
        }
}

// a thing of a house we hold, up from its place (not only walked to)
bool Lifted(CPlayerPed* me, Tell& t) {
    CEntity* held = HeldBy(me);
    if (!held) g_liftTold = nullptr;  // its place may hold the next thing
    if (!held || held == g_liftTold) return false;
    for (int i = 0; i < ThingCount(); ++i) {
        if ((CEntity*)ThingObject(i) != held) continue;
        CVector at = ThingPlace(i);
        if ((held->GetPosition() - at).Length() < 0.3f) return false;
        g_liftTold = held;
        t.taken = true, t.model = ThingModel(i), t.x = at.x, t.y = at.y, t.z = at.z;
        return true;
    }
    return false;
}

// The residents' brains (BURGLAR) wake at a full meter or when one is shot at or hurt, and then phone the police or
// fight (a resident up). Who joined keeps BURGJB's meter of its own player here: the sound level where it stands, x1.6,
// what is over 20 heaps up, 1% fades a frame. Full for 0.2 s or a resident up: woken, 10 s to get out of the house, then
// 3 stars; out of the house in time, nothing. The owner's own is BURGJB's.
float g_noise = 0.0f;
uint64_t g_fullMs = 0;     // the meter full since then
uint64_t g_wokeMs = 0;     // the residents woke for us then
int g_left = -1;           // the seconds to get out last shown
bool g_meterShown = false;
bool g_ownerWoke = false;  // owner: its residents' waking told

bool HasText(const char* key) {
    const char* t = CallMethod<0x6A0050, const char*>((void*)0xC1B340, key);  // CText::Get (TheText): the BURGLAR table
    return t && *t;
}
void Print(const char* key, int ms) {
    if (HasText(key)) Cmd(0x00BB).S8(key).I(ms).I(1).Run();  // PRINT
}
void ShowMeter(bool on) {  // added again when the owner's counters came over it
    if (on || g_meterShown) ShowOwnCounter(kNoiseMeter, "BURG25", on);
    g_meterShown = on;
}
void QuietDown() {  // out of the house, or no burglary: as BURGJB, all over
    ShowMeter(false);
    if (g_noise > 0.0f || g_wokeMs || g_fullMs) Global(kNoiseMeter) = 0, Global(kResidentUp) = 0;
    g_noise = 0.0f, g_fullMs = 0, g_wokeMs = 0, g_left = -1;
}

bool Noise(CPlayerPed* me) {
    int residents = 0;
    Cmd(0x0926).I(23).Out(&residents).Run();  // GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT: the BURGLAR brains
    if (me->AreaCode() == 0 || residents <= 0 || !Cmd(0x0867).I(0).Run()) {  // IS_PROCEDURAL_INTERIOR_ACTIVE
        QuietDown();
        return false;
    }
    uint64_t now = NowMs();
    if (g_wokeMs) {
        int left = 10 - (int)((now - g_wokeMs) / 1000);
        if (left == g_left || left < 0) return false;
        g_left = left;
        if (left == 0) {
            if (!Cmd(0x010F).I(0).I(4).Run()) Cmd(0x010E).I(0).I(3).Run();  // IS_WANTED_LEVEL_GREATER, ALTER_WANTED_LEVEL_NO_DROP
            Print("BURG60", 5000);
        } else if (const char* key = left > 1 ? "BURG58" : "BURG59"; HasText(key)) {
            Cmd(0x01E4).S8(key).I(left).I(1000).I(1).Run();  // PRINT_WITH_NUMBER
        }
        return false;
    }
    ShowMeter(true);
    float level = 0.0f;
    CVector p = me->GetPosition();
    Cmd(0x0855).I(-1).F(p.x).F(p.y).F(p.z).Out(&level).Run();  // GET_SOUND_LEVEL_AT_COORDS
    level = std::isfinite(level) ? level * 1.6f : 0.0f;
    if (level > 20.0f) g_noise += level - 20.0f;
    g_noise = std::min(g_noise * 0.99f, 1000.0f);
    int32_t& meter = Global(kNoiseMeter);
    meter = std::clamp((int)g_noise, 0, 100);
    g_fullMs = meter >= 100 ? (g_fullMs ? g_fullMs : now) : 0;
    if ((!g_fullMs || now - g_fullMs <= 200) && Global(kResidentUp) != 1) return false;
    g_wokeMs = now, g_left = -1;  // the brains heard it: BURGJB clears its meter then
    g_noise = 0.0f, meter = 0;
    ShowMeter(false);
    Print("BURG55", 5000);
    LOGI("Joint burglary: the residents woke for us");
    return true;
}

// A burglary house with the game's second burglary bit (0x400) is shut for good at each game's start half the time: its
// hours come out as none (CEntryExit's constructor, CGeneral::RandomBool(50)), in each game its own. The house the owner
// went into was shut in the other game: no marker at its door and no way in (28.10). While a burglary is joint, those
// houses are open in both games; their hours come back after it. The ways out (the linked exits) have their hours.
constexpr uintptr_t kEnexPool = 0x96A7D8;  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
bool g_allOpen = false;
std::vector<int> g_opened;  // the entrances given hours here
uint8_t* Hours(CPoolHeader* pool, int i) { return (uint8_t*)pool->objects + (uintptr_t)i * 0x3C + 0x34; }  // m_nTimeOn, m_nTimeOff
void OpenAllHouses(bool on) {
    CPoolHeader* pool = Ref<CPoolHeader*>(kEnexPool);
    if (on == g_allOpen || !pool) return;
    g_allOpen = on;
    if (!on) {
        for (int i : g_opened)
            if (i < pool->size && pool->byteMap[i] >= 0 && Hours(pool, i)[1] == 24) Hours(pool, i)[1] = 0;
        g_opened.clear();
        return;
    }
    for (int i = 0; i < pool->size; ++i) {
        uint8_t* h = Hours(pool, i);
        if (pool->byteMap[i] >= 0 && (*(uint16_t*)(h - 4) & 0x1000) && h[0] == 0 && h[1] == 0) h[1] = 24, g_opened.push_back(i);
    }
    LOGI("Joint burglary: %zu burglary houses shut in this game are open too", g_opened.size());
}

// joined: what the door of a burglary house by our player holds, as the game checks it before it shows its marker and
// lets the player in (CEntryExitManager::Update, CEntryExit::TransitionStarted). Two tests in a row (27-28.10) left the
// second player with no marker and no way in at the doors the owner used, with the hours opened. Logged on a change.
void LogDoorByUs(CPlayerPed* me) {
    static uint64_t lastMs = 0;
    static std::string told;
    CPoolHeader* pool = Ref<CPoolHeader*>(kEnexPool);
    if (NowMs() - lastMs < 1000 || !pool || me->InVehicle()) return;
    lastMs = NowMs();
    CVector p = me->GetPosition();
    for (int i = 0; i < pool->size; ++i) {
        uint8_t* e = (uint8_t*)pool->objects + (uintptr_t)i * 0x3C;
        uint16_t flags = *(uint16_t*)(e + 0x30);
        const float* r = (const float*)(e + 0x08);  // m_recEntrance
        if (pool->byteMap[i] < 0 || !(flags & 0x1000) || std::hypot(p.x - (r[0] + r[2]) * 0.5f, p.y - (r[1] + r[3]) * 0.5f) > 4.0f)
            continue;
        char line[320];
        snprintf(line, sizeof line,
                 "%.8s %d: flags %04X, hours %d-%d, area %d, link %d, in it %d; entering %d, all off %d, houses on %d, "
                 "controls off %04X, cutscene %d, may start %d, area now %d",
                 (const char*)e, i, flags, e[0x34], e[0x35], (int8_t)e[0x32], *(void**)(e + 0x38) != nullptr,
                 CallMethod<0x43E460, bool>((void*)e, &p),  // CEntryExit::IsInArea
                 Ref<int32_t>(0x96A7CC), Ref<int8_t>(0x96A7C8), Ref<uint8_t>(0x96A730),  // ms_exitEnterState, ms_bDisabled, ms_bBurglaryHousesEnabled
                 GetPad(0)->DisablePlayerControls, Ref<uint8_t>(0xB5F852),  // CCutsceneMgr::ms_cutsceneProcessing
                 CallMethod<0x609590, bool>(me), Ref<int32_t>(0xB72914));   // CPlayerPed::CanPlayerStartMission, CGame::currArea
        if (told != line) {
            told = line;
            LOGI("Joint burglary, a house's door by us: %s", line);
        }
        return;
    }
}

// owner: its residents woke (98@, or a resident up) while its player is in a house; told once
bool OwnerWoke() {
    bool woke = OwnBurglary() && Local(kInHouse) == 1 && (Local(kWoke) == 1 || Global(kResidentUp) == 1);
    bool tell = woke && !g_ownerWoke;
    g_ownerWoke = woke;
    return tell;
}

// ---------------------------------------------------------------- the shooting range for two
// SHRANGE puts its player in the middle booth of its range (lane 1) between two rivals and counts each lane's broken
// target parts ($32388: 8 a lane, a frame and its 7 parts), whoever broke them: the lane first to break 3 targets wins
// rounds 1 and 2. Round 3's one target takes the shots of all three, and its new broken parts go to whoever
// IS_CHAR_SHOOTING says fired, a rival asked first. Who joined stands in the owner's booth with the owner's gun and shoots
// its copies of the targets (Props makes them solid there): each hit goes to the owner's game (PropCopyHit), which breaks
// its own part as its player's shot (RangeHit). Its own range ($7452, SHOOT's latch $104) stays off meanwhile.
constexpr uint32_t kRangeParts = 32388, kRangeStage = 32280, kRangeArea = 32296, kRangeOn = 7452, kShootLatch = 104;
uint64_t g_rangeHitMs = 0;  // owner: the other player broke a part of our middle lane then
bool g_rangeJoined = false;
int g_rangeGun = 0;              // joined: the owner's gun we were given
std::pair<int, int> g_rangeOwn;  // and ours of its slot before it (type, ammo)

int RangeLane(void* o) {  // owner: the lane of a target part of our joint round, -1 none
    void* s = o && SideLeading() && Helper() ? RunningMission() : nullptr;
    if (!s || !Named(s, "shrange")) return -1;
    int h = GetObjectRef((CObject*)o);
    for (int i = 0; i < 24; ++i)
        if (Global(kRangeParts + (uint32_t)i * 4) == h) return i / 8;
    return -1;
}

// Round 3 asks IS_CHAR_SHOOTING for a frame's new broken parts of the middle lane: one the other player just broke is
// our player's, not a rival's that fired meanwhile
int RangeShooting(CPlayerPed* me) {
    if (!g_rangeHitMs || NowMs() - g_rangeHitMs > 250) return -1;
    if (Int(0) != GetPedRef(me)) return 0;
    g_rangeHitMs = 0;
    return 1;
}

void GiveGun(int h, int type, int ammo) {
    int model = -1;
    Cmd(0x0781).I(type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
    if (model > 0) LoadModelNow(model);
    Cmd(0x01B2).I(h).I(type).I(ammo).Run();  // GIVE_WEAPON_TO_CHAR
}
// joined: the owner's gun (0: none), ours of that slot back after
void RangeGun(CPlayerPed* me, int gun) {
    if (gun == g_rangeGun) return;
    int h = GetPedRef(me);
    if (g_rangeGun) {
        Cmd(0x0555).I(h).I(g_rangeGun).Run();  // REMOVE_WEAPON_FROM_CHAR
        if (g_rangeOwn.first > 0) GiveGun(h, g_rangeOwn.first, std::max(g_rangeOwn.second, 1));
    }
    g_rangeOwn = {0, 0};
    if (gun) {
        CWeapon was[NUM_WEAPON_SLOTS];
        memcpy(was, me->Weapons(), sizeof was);
        GiveGun(h, gun, 9999);
        Cmd(0x01B9).I(h).I(gun).Run();  // SET_CURRENT_CHAR_WEAPON
        for (int i = 0; i < NUM_WEAPON_SLOTS; ++i)
            if (me->Weapons()[i].type == gun) g_rangeOwn = {was[i].type, was[i].ammoTotal};
    }
    g_rangeGun = gun;
    LOGI("Joint range: gun %d", gun);
}

void RangeJoined(bool on) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || (!on && !g_rangeJoined)) return;
    g_rangeJoined = on;
    Global(kRangeOn) = on;  // the clerk lets our shots be, and SHOOT starts no range of ours
    if (!on) return RangeGun(me, 0);
    Global(kShootLatch) = 1;  // nor as our waiting one's clean-up lowers $7452 (SHOOT drops it in the street)
    if (RangeWaits(RunningMission())) Global(kRangeArea) = -1;  // it ends as when its player walks out
    // the owner's gun while we are in its range's room: pistol, shotgun, Uzi, AK-47 by its level
    const RemotePlayer* rp = Players::Instance().Get(SideLeader());
    bool there = rp && rp->hasState && rp->state.area && me->AreaCode() == rp->state.area;
    int gun = there ? rp->state.weapon : 0;
    if (!there) RangeGun(me, 0);
    else if (gun == 22 || gun == 25 || gun == 28 || gun == 30) RangeGun(me, gun);
}
}  // namespace

bool RangeWaits(void* script) { return script && Named(script, "shrange") && Global(kRangeStage) == 0; }

std::vector<Tell> Update(const std::string& joined, const std::string& owned) {
    RangeJoined(joined == "shrange");
    if (owned != "pimp") g_pimpOwn = -1;  // a job of ours with the other player in it no more
    std::vector<Tell> tell;
    bool burglary = joined == "burgjb", owning = owned == "burgjb";
    if (burglary != g_houses) {
        if (burglary || !OwnBurglary()) Cmd(0x09E6).I(burglary ? 1 : 0).Run();  // ENABLE_BURGLARY_HOUSES (ours keeps them)
        g_houses = burglary;
        LOGI("Burglary houses %s here for the joint burglary", burglary ? "open" : "shut");
    }
    OpenAllHouses(burglary || owning);
    CPlayerPed* me = FindPlayerPed(0);
    if (g_stashed != -1) {
        CObject* o = GetObject(g_stashed);
        if (!o || !me || HeldBy(me) != (CEntity*)o || NowMs() - g_stashedMs > 5000) {
            if (o) Cmd(0x0108).I(g_stashed).Run();  // DELETE_OBJECT: into the van (out of its house by now, as BURGJB's)
            g_stashed = -1;
        }
    }
    g_atVan = false;
    if (!me || (!burglary && !owning)) {
        if (!burglary) QuietDown();
        g_gone.clear(), g_liftTold = nullptr, g_ownerWoke = false;
        return tell;
    }
    HideGone(me);
    if (Tell t; Lifted(me, t)) tell.push_back(t);
    if (owning ? OwnerWoke() : Noise(me)) {
        Tell t;
        CVector p = me->GetPosition();
        t.alarm = true, t.x = p.x, t.y = p.y, t.z = p.z;
        tell.push_back(t);
    }
    if (burglary && StashAtVan(me)) {
        Tell t;
        t.loot = true;
        tell.push_back(t);
    }
    if (burglary) LogDoorByUs(me);
    return tell;
}

std::string LootText(std::string& hint) {
    if (!g_atVan || g_stashed != -1) return "";
    hint = TR("F - положить", "F: put it in");
    return TR("Положить вещь в фургон?", "Put the thing into the van?");
}

int Stashed() {
    if (!OwnBurglary()) return 0;
    int32_t& things = Ref<int32_t>(kMissionLocals + kVanThings * 4);
    things += 1;
    Cmd(0x097A).F(-1000.0f).F(-1000.0f).F(-1000.0f).I(1058).Run();  // REPORT_MISSION_AUDIO_EVENT_AT_POSITION: as BURGJB's
    LOGI("Joint burglary: the other player's thing into our van (%d)", things);
    return things;
}

void Taken(int model, float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || g_gone.size() >= 256) return;
    g_gone.push_back({model, {x, y, z}});
}

void Alarm(float x, float y, float z) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || (me->GetPosition() - CVector{x, y, z}).Length() > 50.0f)
        return;  // another house
    if (OwnBurglary()) {  // its heap full: BURGJB's meter and the brains do the rest
        if (Local(kInHouse) != 1 || Local(kWoke) == 1 || Global(kResidentUp) == 1) return;
        Ref<float>(kMissionLocals + kNoiseHeap * 4) = 150.0f;
    } else if (g_houses && !g_wokeMs) {
        g_noise = 150.0f;
    } else {
        return;
    }
    LOGI("Joint burglary: the residents woke for the other player in our house too");
}

uint32_t NoiseVar() { return g_houses ? kNoiseMeter : 0; }

void WorldLoaded() {  // the entrances were made anew with hours of their own: none of ours to give back
    g_allOpen = false;
    g_opened.clear();
    g_rangeJoined = false, g_rangeGun = 0, g_rangeOwn = {0, 0};  // its own weapons: no range gun among them
}

int32_t* BeforeCommand(void* script, uint16_t op, const uint8_t* ip, int32_t& was) {
    if (op != 0x01E9 && op != 0x01EA && op != 0x05CA && op != 0x05E2) return nullptr;
    CPed* helper = SideLeading() ? Helper() : nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    if (!helper || !me) return nullptr;
    int32_t* var = nullptr;
    int32_t now = 0;
    if (Named(script, "ambulan") && (var = AmbulanceBefore(op, ip, helper))) now = g_taken[Local(kPatient)];
    else if (Named(script, "copcar") && (var = PoliceBefore(op, ip, me, helper))) now = GetPedRef(helper);
    if (!var) return nullptr;
    was = *var;
    *var = now;
    return var;
}

// BURGJB's lockup: it opens with its player within 20 m of it and the van there (LOCATE_CHAR_ANY_MEANS_3D, once in the
// script) and takes the van in with that player at its wheel (LOCATE_CHAR_IN_CAR_3D, once). The other player drove the
// van there and stood before a shut door (28.10).
int Burglary(uint16_t op, CPlayerPed* me, CPed* helper) {
    if ((op != 0x00FE && op != 0x0100) || Int(0) != GetPedRef(me) || (op == 0x0100 && !CarOf(helper))) return -1;
    return Within(helper->GetPosition(), {Float(1), Float(2), Float(3)}, Float(4), Float(5), Float(6)) ? 1 : -1;
}
int Answer(void* script, uint16_t op) {
    if (op != 0x0106 && op != 0x00F4 && op != 0x0103 && op != 0x00F2 && op != 0x0202 && op != 0x0205 && op != 0x0100 && op != 0x00FE &&
        op != 0x00DD && op != 0x0021 && op != 0x0023 && op != 0x02E0 && op != 0x00DB && op != 0x0104)
        return -1;
    CPed* helper = SideLeading() ? Helper() : nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    if (!helper || !me) return -1;
    if (Named(script, "ambulan")) return Ambulance(op, me, helper);
    if (Named(script, "copcar")) return Police(op, me, helper);
    if (Named(script, "firetru")) return Fire(op, me, helper);
    if (Named(script, "stunt")) return Stunt(op, me, helper);
    if (Named(script, "burgjb")) return Burglary(op, me, helper);
    if (Named(script, "kicksta")) return Kickstart(op, me, helper);
    if (Named(script, "blood")) return BloodRing(op, me, helper);
    if (Named(script, "quarry")) return Quarry(op, helper);
    if (Named(script, "shrange")) return op == 0x02E0 ? RangeShooting(me) : -1;
    if (Named(script, "pimp")) return Pimp(op, me, helper);
    return -1;
}

// ---------------------------------------------------------------- the valet's shift for two
// valet.scm serves one guest's car at a time ($43736): its player at the wheel (GET_DRIVER_OF_CAR == $PLAYER_ACTOR) gets a
// parking bay, and the car counts stopped within 4 m of it (LOCATE_STOPPED_CAR_3D, the car's own check). The other
// player's copy at that wheel stands for our player: the car is its game's then, our copy of it rests where it stops.
constexpr uint32_t kValetShift = 7480, kValetCar = 43736;
bool ValetThread(void* script) { return script && Named(script, "valet"); }
void* ValetShift() {
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (ValetThread(s)) return Global(kValetShift) == 1 ? s : nullptr;
    return nullptr;
}
void ValetAfter(uint16_t op, const uint8_t* ip) {
    if (op != 0x046C && op != 0x0811) return;
    CPed* helper = Helper();
    CPlayerPed* me = FindPlayerPed(0);
    int32_t first = 0;
    int32_t* out = game::script::OutVar(game::script::NextArg(ip + 2));  // valet.scm's are globals
    if (!helper || !me || !out || !game::script::ArgValue(ip + 2, first)) return;
    // GET_DRIVER_OF_CAR of the guest's car: the other player drives it for us (a "stolen" car otherwise, dropped)
    if (op == 0x046C && first == Global(kValetCar) && *out == GetPedRef(helper)) *out = GetPedRef(me);
    // GET_CAR_CHAR_IS_USING $PLAYER_ACTOR: its valets keep off the car the other player takes too
    else if (op == 0x0811 && first == GetPedRef(me) && *out == -1)
        Cmd(0x0811).I(GetPedRef(helper)).Out(out).Run();
}
// IS_CHAR_IN_CAR and LOCATE_CHAR_ANY_MEANS_CAR_3D of $PLAYER_ACTOR: its clean-up of cars in the way spares the other's
int ValetAnswer(uint16_t op) {
    if (op != 0x00DB && op != 0x0205) return -1;
    CPed* helper = Helper();
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = GetVehicle(Int(1));
    if (!helper || !me || !v || Int(0) != GetPedRef(me)) return -1;
    if (op == 0x00DB) return CarOf(helper) == v ? 1 : -1;
    return Within(helper->GetPosition(), v->GetPosition(), Float(2), Float(3), Float(4)) ? 1 : -1;
}

}  // namespace coop::jobs

namespace coop {

bool RangeTarget(void* object) { return jobs::RangeLane(object) >= 0; }

void RangeHit(uint8_t from, void* object, float damage, int weapon) {  // the middle lane is our player's
    if (from != SideHelper() || jobs::RangeLane(object) != 1 || !(damage > 0.0f && damage < 1e6f) || weapon > 58) return;
    auto* o = (game::CObject*)object;
    bool shown = o->IsVisible();
    game::CVector at = o->GetPosition(), up{0.0f, 0.0f, 1.0f};
    game::CallMethod<0x5A0D90>(o, damage, &at, &up, (game::CEntity*)game::FindPlayerPed(0), weapon);  // CObject::ObjectDamage
    if (shown && !o->IsVisible()) jobs::g_rangeHitMs = NowMs();  // broken: round 3 asks who fired
}

bool RangeCopy(uint8_t owner, int model) {
    if (!jobs::g_rangeJoined || owner != SideLeader()) return false;
    for (int m = 319; m <= 326; ++m)  // TARGET_FRAME..TARGET_HEAD, the script's -319..-326 (CTheScripts::UsedObjectArray)
        if (model == game::Ref<int32_t>(0xA44B70 + (uintptr_t)m * 0x1C + 0x18)) return true;
    return false;
}

}  // namespace coop
