#pragma once
// What the host's story mission asks about its own player that any player may do (see MissionRules.cpp).
#include <cstdint>

namespace coop::rules {

// Before a command of the host's story mission: inside a stretch of script that goes for another player, the variable of
// $PLAYER_ACTOR stands for that player's copy for this one command (its value back in `was`, the variable returned).
int32_t* BeforeCommand(void* script, const uint8_t* ip, int32_t& was);

// After a condition of the host's story mission: 1 another player meets it, 0 the line as written fails for all, -1 the
// game's answer stands.
int Answer(void* script, void* handler, int command, const uint8_t* ip);

// After a command of the host's story mission: what it did to its own game that the partners' need as well.
void AfterCommand(uint16_t op, const uint8_t* ip);

// partner: the host's mission is over, what it changed here goes back
void MissionOver();

// host: a stretch of script going for another player skips this command (its player's controls stay its own)
bool SkipCommand(void* script, uint16_t op);

// partner: our player throws a box off the train of "Catalyst" (power 0..100)
void Throw(float power);

}  // namespace coop::rules
