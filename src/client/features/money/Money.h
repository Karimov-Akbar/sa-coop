#pragma once
#include <cstdint>
#include <string>

namespace coop::money {

// what that player has, as its game said last (false: not known yet)
bool Of(uint8_t id, int32_t& money);
// gives that player this much of our money (the F2 window): false with the reason why not
bool Give(uint8_t to, int32_t amount, std::string& error);

}  // namespace coop::money
