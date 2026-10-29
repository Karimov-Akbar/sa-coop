#pragma once
// Pool for two over the network. POOL2, the bar's pool game, has a two-player mode of its own: the console's second pad
// joins at the bet screen (START on it: SET_SCRIPT_COOP_GAME, 178@ = 1), the turns go by the pad in 180@ (0 the game's
// player, 1 the second pad), and the second player plays in the bar opponent's place (214@). Here the game of the one at
// the table (the owner) runs it: the other player's buttons and mouse come as its second pad, and its copy stands in for
// the opponent. The other player watches the owner's camera, the balls and itself in that place.
#include <cstdint>

namespace coop::pool {

// owner: the other player said yes to its pool contest, and POOL2's bet screen takes it as the second pad (false: no
// pool game of ours runs, or it is one for two already)
bool Ask(uint8_t partner);
// owner: a game for two runs at our table
bool Owning();
// that player's game shows us its table: its texts and help are ours to show (Missions' MissionSource)
bool TextsFrom(uint8_t id);
// Missions' command hook, after a command of any script: an answer to put in place of its own for the second pad and
// the PC (IS_BUTTON_PRESSED, IS_PC_USING_JOYPAD), and for the second player's own table (no game there); -1 none
int Answer(void* script, uint16_t op);
// the same for the rest of a mission script's: the second player's sticks and mouse into the outputs, a game's winner
void AfterCommand(void* script, uint16_t op, const uint8_t* ip);

}  // namespace coop::pool
