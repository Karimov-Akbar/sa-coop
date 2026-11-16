#pragma once
// The players' own skins and the mod's skin packs (OwnSkins.cpp): coop\skins\<name>.dff with <name>.txd, and the same in
// coop\skinpacks\<pack>\, each loaded when first needed into a model number this game leaves free. The other games know
// one by its key (G_PLAYER_LOOK): a game with the same files shows it, one without shows the game's ped of that name, else
// CJ in his clothes. The game's process step or the overlay only.
#include <string>
#include <vector>

namespace coop::skin {

// the folder's skins (a .dff with a .txd of the same name), by name in lower case, sorted; read anew when fresh
const std::vector<std::string>& OwnNames(bool fresh = false);
// the mod's skin packs (coop\skinpacks\<pack>\), the same way as "pack/name", read with OwnNames
const std::vector<std::string>& PackSkins();
// the model that skin (a name, or a pack's "pack/name") is in, loaded now when it is not; -1: no such skin, or it would
// not load (why says why)
int OwnModel(const std::string& key, std::string* why = nullptr);
// the key of the own or pack skin loaded in that model, empty when it is none
std::string OwnName(int model);
// a model number given to an own skin (loaded or not): never asked of the streaming, it has nothing on the disc
bool IsOwn(int model);
// the game's ordinary ped of that name (peds.ide, 7-288; a pack's skin by the name after its pack), -1: none
int StandardModel(const std::string& key);
// own skins nothing wears and nothing shows are taken out of memory, all but keep
void LetUnusedGo(const std::string& keep);
// the game closes, its peds gone: every own skin out of memory while RenderWare is up (Module::OnGameExit)
void LetAllGo();

}  // namespace coop::skin
