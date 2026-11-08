#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coop::weapon_gift {

struct Owned {
    int type;
    int ammo;
    bool whole;  // goes as it is: a melee weapon, flowers, goggles, the parachute
};
// our weapons another player can be given (the F2 window)
std::vector<Owned> Ours();
// the weapon's name for the F2 window and the chat
const char* Name(int type);
// gives that player this weapon of ours with that many rounds (all of a whole one): false with the reason why not
bool Give(uint8_t to, int type, int ammo, std::string& error);

}  // namespace coop::weapon_gift
