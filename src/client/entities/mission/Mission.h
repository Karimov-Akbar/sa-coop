#pragma once
// The host's mission as the rest of the game sees it. The story feature (features/story/missions) runs the missions
// and answers these.
#include <cstdint>
#include <string>
#include <vector>

namespace coop {

// partner: the host's mission runs, or ended less than graceMs ago
bool HostMissionRecent(uint64_t graceMs);
// partner: the script name of the host's story mission that runs ("zero2"), empty for none
std::string HostMissionName();

// a person of this game this game put on a car (ATTACH_CHAR_TO_CAR, at offset): its copy in the other games goes on the
// copy of that car the same way (false: the person or the car is not shared yet)
bool SharePersonOnCar(bool on, int ped, int car, float x, float y, float z);

// partner: it watches a scene of the host's mission on the engine (the host's camera)
bool SceneWatched();

// a joint side activity (a vehicle job, a race) of this game's runs with another player in it
bool SideLeading();

// the player whose side activity this game takes part in (0xFF: none)
uint8_t SideLeader();

// the players who take part in this game's joint side activity
std::vector<uint8_t> SideHelpers();

// partner: on foot in a story mission's start point, no mission here or at the host's (the host's world after its
// progress loads there, and the host's start waits for it)
bool AtMissionMarker();

// this player plays a side activity a contest can be of (basketball's challenge, a dance, a casino table, a race), a
// contest of its is asked or on (features/story/contest), or a game of pool for two runs with it in it
bool ContestActivity();

// host: its mission (or the mission's trigger) runs and a partner is there to see it
bool HostReplicating();

// the script (CRunningScript) whose commands run right now, or null
void* CurrentScript();

// the host's story mission script (not a side activity's mission, not a thread)
bool StoryMission(void* script);

// host: that script is its story mission's or a thread it started (AUDIOL, which says the lines of its scenes)
bool MissionThread(void* script);

// the mission script that runs in this game (m_bIsMission), or null: on the partner always its own side activity
void* RunningMission();

// this game's player is at a school (driving, bikes, boats, flying): the other game's school has the same cars on the
// same spots
bool OwnSchool();

// partner: clothes the host's mission put on over this player's own look (the balaclava) are not its own to keep: its
// own keys go into clothes (a CPedClothesDesc), false when no mission clothes are on
bool MissionClothesUnder(void* clothes);

// partner: a weapon the host's mission gave is not this player's own to keep either: its own one the given one pushed
// out of the slot comes instead (type 0: none)
void MissionWeaponUnder(int& type, int& ammo);

// partner: this object is the copy of one of the host's mission objects (the host's crate, not a prop of ours)
bool MissionObjectCopy(void* object);

// the second player of a pool game for two: this object is the copy of one of the owner's table things (the opponent's
// cue in our hand among them: the owner's game has the cue itself, not a prop of ours)
bool PoolTableCopy(void* object);

// host: the net id of this object of the host's mission (0: not one); partner: the handle of its copy (-1: none here)
uint16_t MissionObjectNetId(void* object);
int MissionObjectCopyHandle(uint16_t netId);
// host: a story mission is about to move one of main.scm's own objects (a garage door, a shutter): announced now
void AnnounceScmObject(int handle);
// host: a story mission keeps its new object past its end (DONT_REMOVE_OBJECT): it leaves the list the partners' copies
// are made from, announced now
void AnnounceKeptObject(int handle);

// host: Big Smoke's courier (BCESAR2 by day, BCESAR3 by night: features/story/courier) runs in this game; that script is
// one of its two; the handles of its objects for the partners to see (its cash bags once off its car), up to max
bool CourierRuns();
bool CourierThread(void* script);
int CourierObjects(int* handles, int max);

// A shooting range round for two (SHRANGE, features/story/jobs). Owner: this object is a target of our joint round (the
// other game sends its hits, its copy's replayed shots here do not count); the other player hit it on its copy there
bool RangeTarget(void* object);
void RangeHit(uint8_t from, void* object, float damage, int weapon);
// joined: that player's thing of this model is a target of its round, solid here while it shows (Props)
bool RangeCopy(uint8_t owner, int model);
// our shot at a solid copy of the other game's thing (Props): it goes to that game, nothing lands here (false: not one)
bool PropCopyHit(void* object, void* damager, float damage, int weapon);

// host: the other player holds the copy of this mission object in its game (a crate of "Home Invasion"); holder: its
// copy here, enterMs: when it last pressed its enter button holding it
bool PartnerHolds(void* object, void** holder = nullptr, uint64_t* enterMs = nullptr);

}  // namespace coop
