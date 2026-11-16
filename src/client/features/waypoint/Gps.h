#pragma once
// The road route to the map target on the radar while driving (Gps.cpp).
#include <cstdint>
#include <map>

#include "shared/game/Types.h"

namespace coop::gps {

// Waypoints' hook of CRadar::DrawBlips calls it before the blips, so the route lies under them. remote: the other
// players' waypoint the route goes to when this player has none of its own (the one set last), null for none;
// remoteColour: the colour its player chose for its mark, null for none (a red route).
void DrawRoute(const game::CVector2D* remote, const game::CRGBA* remoteColour);

}  // namespace coop::gps
