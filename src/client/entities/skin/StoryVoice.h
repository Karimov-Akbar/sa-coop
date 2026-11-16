#pragma once
// The voice of a skin in the story's lines outside the cutscenes (a mission's dialogue, the phone calls): StoryVoice.cpp.
#include <string>

namespace coop::skin {

// MissionSpeech: the mission audio slot (0-3) was asked for a line by the host's game (true) or by a script of this
// game (false): whose player the line's CJ is
void StoryLineFrom(int slot, bool hosts);
// A dialogue subtitle as it is shown while the others' lines play without CJ's name: without it too. False: shown as it
// is. hosts: the line is the host's game's, a partner shows it
bool StoryLine(const char* text, bool hosts, std::string& shown);

}  // namespace coop::skin
