#pragma once
// The voice of a skin in the cutscenes (SceneVoice.cpp): a cutscene whose CJ is made of a skin plays its own track of
// coop\voices, where another voice says his lines and nobody names him, and shows its subtitles without his name. The
// game's process step only.
#include <string>

namespace coop::skin {

// The model the CJ of the cutscene that loads here is made of: the skin the story's CJ wears (our player in our own
// cutscene, the host in the host's on a partner's screen). 0: he is CJ, and CSPLAY stays
int SceneCjModel();
// the cutscene that plays here has that skin's voice in place of CJ's
bool SceneVoiceOn();
// a subtitle of that cutscene as it is to be shown: without CJ's name, empty when it was his name alone. False: as it is
// (the game's sound plays, or the line does not name him)
bool SceneLine(const char* text, std::string& shown);
// A woman's skin, by its ped type (peds.ide: CIVFEMALE, PROSTITUTE): which voice's files are its own
bool SkinIsFemale(int model);

}  // namespace coop::skin
