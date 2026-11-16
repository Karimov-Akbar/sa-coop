#pragma once
// The other players' map waypoints on the radar and the map (Waypoint.cpp).

namespace coop::waypoint {

// stand: for that long the radar shows two waypoints by our player, the game's red one 40 m north and a partner's one in a
// colour (green) 40 m east, to see that the copy of the sprite in a colour looks and sits as the game's one
void TestColours(float seconds);

}  // namespace coop::waypoint
