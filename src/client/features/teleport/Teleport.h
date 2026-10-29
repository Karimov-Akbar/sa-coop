#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coop::teleport {

// /tp (us over to that player) and /tphere (that player over to us) as typed in the chat: a request the other player
// answers with Y or N, carried out here once it says yes. False: not one of them, or offline
bool Command(const std::string& cmd, const std::vector<std::string>& args);
// our answer to a request waiting for it (Y yes, N no): false when none waits
bool Key(bool yes);
// the centre box's line while a request waits for our answer, and its hint
std::string Text(std::string& hint);

}  // namespace coop::teleport
