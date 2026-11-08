#pragma once
// The partner's character in the host's cutscenes: where it is in each shot and what it does there.
// The host's game picks the spot (the partner's follows) from the cutscene camera, the actors and what is drawn
// around CJ (cutscene sets often have no collision): a chair, sofa or steps (sits down), a wall behind it (leans on
// it, maybe smoking) or free floor (stands about), always on CJ's floor, inside the frame, seen by the camera and
// covering nobody, out of the way of the scene's people and cars, and so for the whole shot: the cutscene camera's
// spline and the animation tracks tell how the shot goes on.
#include <cstdint>
#include <string>

#include "shared/game/Game.h"

namespace coop::scene {

// POSE_WALK: along with CJ, behind him to one side (act: 0 left, 1 right, 2 right behind), as his root track goes
// POSE_CAR: riding in a car of the scene, Spot::ground in the car's own frame, Spot::act the car's cutscene object
// POSE_WAY: along the way of the scene (cutscene_ways.txt, in coop.asi), each game by its own CJ's clock
enum Pose : uint8_t { POSE_STAND, POSE_LEAN, POSE_SIT, POSE_STEP, POSE_HIDDEN, POSE_WALK, POSE_CAR, POSE_WAY };

struct Spot {
    game::CVector ground;  // where the feet are
    float heading = 0;     // radians
    Pose pose = POSE_STAND;
    uint8_t act = 0;       // which animation of the pose
    bool walk = false;     // walked to from the spot before (the host found the way free), not a jump
    bool viaSet = false;   // the walk turns once at via, round what stands on the straight line (a table, a sofa)
    game::CVector via;
    uint8_t rank = 0;      // which partner it is for (ForRank)
    uint8_t seats = 0;     // POSE_CAR: seats free for the next partners,
    game::CVector seat[2];  // as Spot::ground
};

// A cutscene is being loaded: the partner's animations, the cigarette and what the map needs for this scene are loaded
// with it (streaming is slow once the cutscene runs, and takes no animation files at all).
void Prepare(const char* cutscene);
// Looks at the frame being drawn: on a cut to another shot the spot is chosen again. Call it from the render
// hook while a cutscene runs: only then does the screen projection match the camera of the shot.
void Analyse();
// host: the spot just chosen and the moment (the camera's clock, ms) its shot begins, once per shot
bool TakeDecision(Spot& s, float& at);
// partner: the host's choices; Follow (instead of Analyse) takes each on when this game's cutscene gets there
void FollowSpot(const Spot& s, float at);
void Follow();
// The spot for the current shot. False: no cutscene with CJ (yet) to place the partner in.
bool CurrentSpot(Spot& out);
int ShotNumber();                     // grows with every new shot
float ShotLeftMs();                   // until the current shot's cut, by the camera's spline; -1 unknown
const char* Describe(const Spot& s);  // "lean GANGS/leanIDLE", for logs
// The spot as the host's game chose it is the first partner's: the next partners' stand by it, the same in all games
Spot ForRank(Spot s, int rank);
// Keeps the ped on the spot with the spot's animation. Call every frame while the cutscene runs.
void Hold(game::CPed* ped, const Spot& s);
// The cutscene is over: animations, props and freezing off for every ped that was held.
void ReleaseAll();

// Review harness (tools/cs_map.py draws them): the last shot (camera, CJ, actors, every spot tried and why it
// was dropped) and the collision around CJ (buildings and objects, in world coordinates).
void DumpShot(const std::string& path);
void DumpWorld(const std::string& path);
game::CVector LastCj();

}  // namespace coop::scene
