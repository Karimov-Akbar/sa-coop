#pragma once
// The player's skin (Skins.cpp): a ped model of the game, one of the mod's skin packs (coop\skinpacks) or one of the
// player's own (coop\skins, entities/skin), worn instead of CJ. The F4 window (pages/skins-menu) chooses it; it is kept
// in coop.ini, put on again in each game, and the other players see it as they see any model of ours (Progress,
// G_PLAYER_LOOK). In a cutscene it stands in for CJ.
#include <cstdint>
#include <string>
#include <vector>

namespace coop::skins {

enum Group : uint8_t { MEN, WOMEN, GANGS, SERVICE, PACKS, OWN, GROUPS };

struct Skin {
    int model = 0;     // the game's ped model (0 for an own skin: its number is given when it loads)
    Group group = MEN;
    std::string name;  // as the list shows it: the model's name in the game's files, an own skin's file name
    std::string file;  // an own skin's name (its files in coop\skins), a pack's "pack/name", empty for the game's
    std::string pack;  // a pack's skin: the title over its pack's rows (its name and its authors)
};

const char* GroupName(Group g);
// the game's ordinary people (7-288: no CJ, no story characters), by model, then the packs' skins and the player's own
const std::vector<Skin>& List();
// the own skins' and the packs' folders read anew (the window opens)
void Rescan();
// the model it is in, an own skin loaded now; -1: it would not load (why says why)
int ModelOf(const Skin& s, std::string* why = nullptr);
// the one our player wears by choice
bool IsChosen(const Skin& s);
bool AnyChosen();
// why our player cannot change now, empty: it can
std::string WhyNot();
// puts it on now and keeps it (null: CJ again); false: why says why
bool Wear(const Skin* s, std::string& why);
// the window closed: the own skins it loaded to show and nobody wears are taken out of memory
void LetUnusedGo();

}  // namespace coop::skins
