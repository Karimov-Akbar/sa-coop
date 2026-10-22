#pragma once
// The host's mission as the rest of the game sees it. The story feature (features/story/missions) runs the missions
// and answers these.
#include <cstdint>

namespace coop {

// partner: the host's mission runs, or ended less than graceMs ago
bool HostMissionRecent(uint64_t graceMs);

// host: its mission (or the mission's trigger) runs and a partner is there to see it
bool HostReplicating();

// the script (CRunningScript) whose commands run right now, or null
void* CurrentScript();

// Races together (features/race answers these).
// A side activity's thread asks to launch its mission: false holds it while the players gather for a race together.
bool SideStartAllowed(const char* thread);
// this game runs a race the other players race in: its race mission is not the host's mission for the partners
bool OwnRaceTogether();

}  // namespace coop
