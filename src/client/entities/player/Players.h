#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "entities/player/PlayerState.h"
#include "entities/vehicle/VehicleState.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

// The other player as seen on this machine: a real CPlayerPed (needed for clothes, fat/muscle
// and player animations) driven by the remote pad/camera through context switching.
struct RemotePlayer {
    enum class Mode { OnFoot, Driver, Passenger };

    uint8_t id = 0;
    game::CPlayerPed* ped = nullptr;
    int pedHandle = -1;
    int blip = 0;
    std::string markColour;  // the colour its owner chose for its mark here (G_MARK_COLOUR; empty: the allies' blue)

    bool hasState = false;
    OnFootState state;          // latest received state (pad/camera/health are also filled in vehicles)
    uint64_t stateRecvMs = 0;   // local time of reception
    uint32_t lastStateTime = 0; // sender time of the latest applied state (drops reordered packets)
    game::CControllerState padNew{}, padOld{};
    bool padFresh = false;      // a new pad state arrived since the last frame
    uint64_t lastWeaponTry = 0;

    Mode mode = Mode::OnFoot;
    uint16_t vehNetId = 0;
    int8_t seat = -1;           // -1 driver, 0.. passenger seats
    uint64_t enterTaskMs = 0;   // when we started the enter-car animation for this player
    uint16_t enterNetId = 0;    // an enter that came before its vehicle's copy here (its model loading), and when
    int8_t enterSeat = -1;
    uint64_t enterAskedMs = 0;
    uint64_t exitTaskMs = 0;
    bool deadShown = false;     // the owner reported death and the local ped plays it
    uint64_t deadSinceMs = 0;   // the owner's state said dead (its death animation may come a moment later)
    int16_t dieAnim = -1;       // the default group's animation its player fell with (G_PLAYER_DEATH), and when
    uint64_t dieMs = 0;
    game::CVector streetPos;  // where its owner was last seen in the street (its door into an interior: F8)
    int8_t doorArea = 0;      // the area its door (CPed::m_pEnex) was set for, and on which ped
    game::CPed* doorPed = nullptr;
    uint64_t jetpackMs = 0;     // when it was last given the jetpack
    std::vector<uint8_t> stats; // owner's CStats block (G_PLAYER_STATS): used while the ped is processed
    uint64_t heardMs = 0;       // its last voice frame was played (set by the voice feature)
    bool lookStale = false;     // a script here took its special character's slot: it went back to CJ, the look again
    uint64_t duckOffMs = 0;     // since when the copy's crouch differs from its owner's (a lost crouch press)
    uint64_t climbMs = 0;       // the copy climbed a wall by itself (CTaskSimpleClimb) last then
    bool animHeld = false;      // a full-body activity of its owner plays on it (Anims): its own tasks keep off it
    uint64_t overlapMs = 0;     // since when it stands inside our player (KeepApart)
    bool hiddenOnSpot = false;  // hidden while both stand on one scripted spot (or gone in through a door)
    uint64_t doorMs = 0;        // its owner goes through a door since (F2_DOOR), facing doorHeading then
    float doorHeading = 0;
    int8_t doorFromArea = 0;  // the area it was in then
    bool goneInDoor = false;    // it turned from the door on its way in: out of sight until it is inside
    uint64_t walkSetMs = 0;     // its body's walk set last asked for (KeepWalkSet)
    // a person of a script here whose place and animation it takes instead of its owner's (the bar's opponent at a pool
    // table when its owner plays there as the second player, Pool.cpp), by handle; -1: none
    int avatar = -1;
    void* avatarAnim = nullptr;  // the animation it plays of that person's
};

class Players : public Module {
public:
    const char* Name() const override { return "Players"; }
    void OnInstallHooks() override;
    void OnSessionStart() override;
    void OnSessionEnd() override;
    void OnPeerLeave(uint8_t id) override;
    void OnGameStarted() override;
    void OnGameShutdown() override;
    void OnPreProcess() override;
    void OnPostProcess() override;
    void OnRender() override;
    void DrawWeaponIcons();  // during the game's HUD
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override;

    RemotePlayer* FindByPed(game::CPed* ped);
    RemotePlayer* Get(uint8_t id);
    // the copy's model as SET_PLAYER_MODEL changes a player's (0: CJ in his clothes); false if it cannot be loaded
    static bool SetModel(game::CPlayerPed* ped, int model);
    const std::map<uint8_t, RemotePlayer>& All() const { return m_remote; }
    // our own mark in this colour on the other players' radar (BlipColour's names, empty: the allies' blue), saved in
    // coop.ini and sent to them
    void SetMarkColour(const std::string& colour);
    // the other players as arrows turned with their heading, at the end of CRadar::DrawBlips (the radar and the menu
    // map): Waypoints' hook of it calls this, and says whether it is in (MinHook takes one hook per function)
    void DrawRadarArrows();
    static void SetRadarArrowsHooked(bool hooked);
    // "G": enter the nearest vehicle as a passenger (prefers the partner's car).
    void TryEnterAsPassenger();
    // the vehicle this game's player drove last, while it exists (by handle: a reused pool slot is another car)
    game::CVehicle* LastCar() const { return game::GetVehicle(m_lastCar); }
    // our player to a spot by another player, as /tp: into that player's area (its interior's door used as a door),
    // out of its vehicle first when onFoot (else the vehicle goes along)
    void GoTo(const RemotePlayer& by, const game::CVector& spot, bool onFoot = false);
    // /tp (go over to that player) and /tphere (bring it over) once it said yes (Teleport.cpp): allowed now? (if not,
    // the chat says why: buildings are entered through their doors), and done
    bool TeleportAllowed(const RemotePlayer& other, bool bring);
    void GoToPlayer(const RemotePlayer& other);
    void Bring(uint8_t id);
    // a vehicle of this model beside our player on free ground: its handle (the script's: MARK_CAR_AS_NO_LONGER_NEEDED
    // is the caller's), -1 not yet (its model comes in the background, the pool is full), -2 no room around us
    int VehicleBeside(int model);
    // our player with another one in its vehicle: in a free seat of it (asSeat), else at the wheel of one like it beside
    // us. Later: try again (its copy not seated here yet, the model loading, no room); None: nothing for us (it walks,
    // or it is in a boat, a plane, a train)
    enum class Ride { Later, Seat, OwnVehicle, None };
    Ride RideWith(const RemotePlayer& leader, bool asSeat);
    // that player's copy takes a script person's place and animation (RemotePlayer::avatar); -1 gives it back its own
    void SetAvatar(uint8_t id, int pedHandle);

    static Players& Instance();

private:
    void SendLocalState();
    void SendVehicleState(game::CVehicle* v);
    void DetectCarTasks();
    void FollowHost();
    void HoldForPassengerSeat();
    void GroupRides(game::CPlayerPed* me);
    void YieldClaimedSeat(game::CPlayerPed* me);
    void PutAwayStuckPhone(game::CPlayerPed* me);
    void PassengerWeaponWheel(game::CPlayerPed* me);
    void HandleOnFoot(uint8_t from, Reader& r);
    void HandleDriver(uint8_t from, Reader& r);
    void HandlePassenger(uint8_t from, Reader& r);
    void HandleEnterCar(uint8_t from, Reader& r);
    void HandleExitCar(uint8_t from, Reader& r);
    void HandleTeleport(uint8_t from, Reader& r);
    void SpawnRemote(RemotePlayer& rp);
    void DestroyRemote(RemotePlayer& rp);
    void ApplyState(RemotePlayer& rp);
    void ApplyVehicleSeat(RemotePlayer& rp);
    void DrawNameTag(RemotePlayer& rp);
    void KeepApart(RemotePlayer& rp);
    void KeepWalkSet(RemotePlayer& rp);
    void RideTrain(RemotePlayer& rp);
    bool TagAnchor(RemotePlayer& rp, float& sx, float& sy, float& alpha);
    RemotePlayer& Touch(uint8_t from);

    std::map<uint8_t, RemotePlayer> m_remote;
    uint64_t m_lastSend = 0;
    uint64_t m_markSentMs = 0;  // our mark's colour went out last then
    PadState m_lastSentPad;
    game::CVehicle* m_enterReported = nullptr;
    bool m_exitReported = false;
    uint64_t m_followAt = 0;
    int m_bikeFor = -1;  // the host's vehicle (no seat left in it) the partner got one for, by handle: a later car in its slot is another
    int m_lastCar = -1;
    int8_t m_knockOffSaved = -1;
    uint64_t m_enterHeldSince = 0;  // the enter button (triangle) held on foot since
    bool m_enterHeldUsed = false;
    bool m_deathSent = false;  // our player's death animation went out (G_PLAYER_DEATH)  // our player's own knock-off setting while it rides behind on the other player's bike
    uint64_t m_selfBrakeLogMs = 0;  // our car braking with no brake pressed, last told then
    uint64_t m_groupRideMs = 0;
    uint64_t m_yieldMs = 0;
    int m_rideCar = -1;             // the other player's car we ride in, by handle, and since when
    uint64_t m_rideSinceMs = 0;
    std::vector<int> m_riders;      // our followers we sent into it
};

// helpers shared with other modules
int ActivePrimaryTaskType(game::CPed* ped, void** taskOut = nullptr);
bool TaskWalks(game::CPed* ped);  // a go-to task walks it (a door's walk-in, a script's), not its pad
// our player was put into interior `area` at `inside` by the mod (a teleport, a follow): the street door it came
// through, as a door's use sets it (SET_CHAR_HAS_USED_ENTRY_EXIT); `from`: where it stood before
void UseStreetDoor(int area, const game::CVector& inside, const game::CVector& from);
// The people on their way into v stop, before v is deleted: some of their tasks use it unchecked once it is gone (the
// door opened from outside, MTA guards 0x646026).
void StopEntering(game::CVehicle* v);
int PassengerSeatOf(game::CVehicle* v, game::CPed* ped);
// An enter-car task keeps the door it goes to (node 8..11), not the passenger seat (the script command turns a seat
// into a door): the seat back for the network.
int SeatOfTargetDoor(int door);
// a passenger seat this vehicle has, or -1 (any): a seat it does not have gives the game a garbage door and a crash
int SeatOrAny(game::CVehicle* v, int seat);
// distance to the nearest remote player in the same interior (huge when there is none), and who it is
float RemoteDistance(const game::CVector& p, int8_t area, uint8_t* who = nullptr);

}  // namespace coop
