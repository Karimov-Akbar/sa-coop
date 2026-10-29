#pragma once
#include <cstdint>
#include <map>
#include <set>

#include "entities/vehicle/VehicleState.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

// What a mod shop / pay'n'spray changes: colours, installed upgrades (model per slot), paintjob.
struct VehicleLook {
    uint8_t colors[4] = {};
    int16_t upgrades[15];
    int8_t paintjob = -1;
    // body damage: CDamageManager without its first float (engine, wheels, doors, lights, panels),
    // the 2 tyres of a bike
    uint8_t damage[20] = {};
    uint8_t bikeTyres[2] = {};
    uint8_t lock = 0;  // CVehicle::m_nDoorLock: a mission locks and unlocks its cars after they are known to both
    VehicleLook() {
        for (int16_t& u : upgrades) u = -1;
    }
    bool operator==(const VehicleLook& o) const;
};

// A vehicle known to both players under a network id.
//  * owner == my id  -> the local game simulates it and sends its state;
//  * owner != my id  -> "mirror": the vehicle follows the received states.
struct NetVehicle {
    uint64_t groundCheckMs = 0;  // mirror: whether this game has ground under its target (checked now and then)
    bool groundless = false;
    uint16_t netId = 0;
    game::CVehicle* veh = nullptr;
    int handle = -1;
    uint8_t owner = 0;
    uint16_t model = 0;
    bool createdByUs = false;  // we created it as a mirror (delete it when the owner removes it)
    bool script = false;       // a script's car in the game that announced it
    uint8_t origBy = 0;        // what this game had it created by before it became a mirror here (0: spawned as one)
    bool heldHere = false;     // a script of this game held it when it became a mirror here
    bool crashProofHere = false;  // made collision-proof here as a mirror (taken off when it is this game's again)
    uint64_t bumpMs = 0;           // mirror: our player's car last touched it then

    bool hasState = false;
    VehicleState state;
    uint64_t recvMs = 0;
    uint32_t lastTime = 0;
    uint64_t lastSendMs = 0;
    uint64_t lastChangeMs = 0;  // owned: last time it moved or was used
    float sentHealth = -1;      // owned: the health last sent (a car on its roof or on fire loses it standing still)
    VehicleLook look;            // owned: last sent; mirror: wanted
    bool hasLook = false;
    uint64_t lookCheckMs = 0, lookSentMs = 0;
};

class Vehicles : public Module {
public:
    void ForgetOwnedBy(uint8_t owner);  // its game started over: its mirrors go, the others are this game's again
    const char* Name() const override { return "Vehicles"; }
    void OnInstallHooks() override;
    void OnSessionStart() override;
    void OnSessionEnd() override;
    void OnPeerLeave(uint8_t id) override;
    void OnGameStarted() override;
    void OnGameShutdown() override;
    void OnPreProcess() override;
    void OnPostProcess() override;

    static Vehicles& Instance();

    NetVehicle* Find(uint16_t netId);
    // the record's vehicle is still there (one deleted in the last frame stays listed until OnPreProcess, after the
    // network handlers that use its handle)
    static bool Alive(const NetVehicle& nv) { return nv.veh && game::GetVehicle(nv.handle) == nv.veh; }
    NetVehicle* Find(game::CVehicle* v);
    // Makes a local vehicle known to the other player (announces it) and returns its record.
    NetVehicle* EnsureNetworked(game::CVehicle* v);
    // Local player became the driver of `nv`: take ownership.
    void TakeOwnership(NetVehicle& nv);
    bool IsMirror(const NetVehicle& nv) const;
    // a mirror this game keeps when its owner deletes it: an ordinary car by this game's player (HandleRemove)
    bool KeepsWhenDeleted(const NetVehicle& nv) const;
    // Applies a state received from the vehicle's driver (Players module, G_PLAYER_DRIVER).
    void ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st);
    // a partner (re)started its game: announce our vehicles again
    void Reannounce();
    // this game would drop the car, the other player is nearer: it goes to that player's game (false: not ours to give)
    bool QueueGiveAway(game::CVehicle* v, uint8_t to);
    // a car each game makes of its own at one place as its player comes near (the police impound's, IMPND): not announced
    // while nobody drives it (a copy of the other game's stood on top of it)
    void KeepLocal(game::CVehicle* v);
    // The host's mission cleared an area: the other players' empty cars there go from this game at once (their copies
    // are mission cars here, CLEAR_AREA left them). Returns their ids for the owners (G_VEH_CLEAR).
    std::vector<uint16_t> ClearMirrors(const game::CVector& at, float radius);

private:
    void Announce(const NetVehicle& nv);
    bool CreateMirror(const VehicleSpawn& sp);
    void ScanTraffic();
    void HandleCreate(uint8_t from, Reader& r);
    void HandleRemove(uint8_t from, Reader& r);
    void HandleSync(uint8_t from, Reader& r);
    void HandleOwner(uint8_t from, Reader& r);
    void HandleStatus(uint8_t from, Reader& r);
    void SendLook(NetVehicle& nv);
    void ApplyLook(NetVehicle& nv);
    void ApplyMirror(NetVehicle& nv, float dt);
    void SendState(NetVehicle& nv);
    void Forget(uint16_t netId, bool deleteVehicle);
    void Drop(uint16_t netId);  // the owner let it go: a spawned mirror goes, an instance of this game stays
    void Protect(NetVehicle& nv);
    void Release(NetVehicle& nv);
    void GiveAway(NetVehicle& nv, uint8_t to);
    void HandleHandover(uint8_t from, Reader& r);
    void TakeOver(NetVehicle& nv, uint8_t from);  // a mirror becomes this game's own car (handed over or kept)
    void HandleScriptFree(uint8_t from, Reader& r);
    void HandleClear(uint8_t from, Reader& r);
    void KeepCarWhileInside(game::CPlayerPed* me);
    void ReleaseKeptCar();
    game::CVehicle* SpawnMirror(const VehicleSpawn& sp);
    game::CVehicle* FindAdoptable(uint16_t model, const game::CVector& pos);

    std::map<uint16_t, NetVehicle> m_byId;
    std::map<game::CVehicle*, uint16_t> m_byPtr;
    std::map<uint16_t, VehicleSpawn> m_pending;  // announced mirrors waiting for their model to stream in
    std::map<game::CVehicle*, uint8_t> m_giveAway;  // to hand over after the population code (vehicle -> player)
    std::set<int> m_local;                           // handles of KeepLocal's cars
    uint16_t m_nextId = 1;
    uint64_t m_lastScan = 0;
    int m_lastDriven = -1, m_kept = -1;  // handles: the car the local player last drove, and it kept while inside
    uint8_t m_keptBy = 0;               // what it was created by before
    int8_t m_lastArea = 0;
};

// Vehicles that are synchronised at all (trains are left to each game). An RC toy is its player's game's car with
// nobody in it: that player stays on foot in it (CPed::bInVehicle), and the other game sees the toy fly or drive.
bool IsSyncableVehicle(game::CVehicle* v);

}  // namespace coop
