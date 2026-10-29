#pragma once
// The road route to the map target on the radar while driving (Gps.cpp).
#include <cstdint>
#include <map>

#include "shared/game/Types.h"

namespace coop::gps {

// Waypoints' hook of CRadar::DrawBlips calls it before the blips, so the route lies under them. remote: the other
// players' waypoints it draws (the route goes to the first one when this player has none of its own).
void DrawRoute(const std::map<uint8_t, game::CVector2D>& remote);

}  // namespace coop::gps
