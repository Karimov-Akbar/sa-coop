#pragma once
#include <string>

namespace coop {

// a race together gathers or runs: the line for the status box ("8-Track · старт через 12 с · участников: 3",
// "Круг 2/12 · место среди игроков: 1/3"), empty otherwise
std::string RaceStatusText();

}  // namespace coop
