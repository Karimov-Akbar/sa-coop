#pragma once
// A side activity played as a contest of two players. Who starts one may challenge the other (U) in its first 20
// seconds; who says yes (Y) plays the same in its own game, and when both are done each game tells who won. The
// activities: basketball's points challenge, pool, the casino's blackjack, roulette and wheel of fortune (not the
// machines), dancing in a club, the lowrider meet, the stadiums' Kickstart, Blood Ring, 8-Track and Dirt Track, the
// races and the triathlons. Each player plays its own game as ever: the contest only reads the result.
#include <cstdint>
#include <string>

namespace coop::contest {

// the box's line: a challenge to make or to answer ("Соревнование: бильярд. Вызвать игрока Женя?"), the keys in hint;
// empty when there is nothing to say
std::string Text(std::string& hint);
// U (the challenge), Y or N (the answer). False when there was nothing to do (the key goes on to the game).
bool Key(int vk);
// the contest's board on top of the screen while one is on, and its result a while after
struct BoardRow {
    std::string name, value;
    bool leads = false;  // ahead now (or the winner)
};
struct BoardView {
    std::string title;  // the activity
    BoardRow me, them;
    std::string wait, how;  // our game not started yet: the time left to start it, and how
    int verdict = 0;        // the result: 1 we won, -1 the other one did, 2 a draw; 0 the contest goes on
};
bool Board(BoardView& out);  // false: none to show
// tests (the debug bot): the board as a contest of that kind (Contest.cpp; 0 basketball) shows it (1 our game not
// started, 2 both playing, 3 we won, 4 we lost)
void Preview(int step, int kind = 0);
// this player's activity may be a contest now (a side activity's call box offers it too: Y together, U a contest)
bool CanCall();
// the activity was called to be played together (a side activity's call): no contest of it
void JointCalled();
// a command of a script just ran (the triathlon's finish, Blood Ring's start)
void AfterCommand(void* script, uint16_t op, const uint8_t* ip);
// the contest is played together instead (pool for two at the caller's table, Pool.cpp): over with no result line
void Drop();

}  // namespace coop::contest
