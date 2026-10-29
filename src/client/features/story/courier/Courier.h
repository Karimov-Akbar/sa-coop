#pragma once
#include <cstdint>

namespace coop::courier {

// host, after each command of a courier's thread (Missions' command hook): its check of its player near a place, a car,
// a person or a cash bag answered for a partner there too (1; -1: as it is)
int Answer(void* script, uint16_t op);
// host, after each command of a courier's thread: what it paid its player (ADD_SCORE) goes to the partners nearby as well
void AfterCommand(void* script, uint16_t op);

}  // namespace coop::courier
