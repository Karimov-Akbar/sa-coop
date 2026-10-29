#pragma once
// Joint vehicle jobs where both players work (27.10: "раз хост зовет напарника совместно выполнять миссию, то они оба
// должны это делать"). The job runs in the game of the one who started it (the owner), and the one who joined drives a
// vehicle of the job of its own (its copy in the owner's game). The owner's game takes that copy for a second player of
// the job: AMBULAN's patients get into its ambulance and count as delivered when it stops at the hospital, FIRETRU's
// burning cars and COPCAR's criminals wake up when it comes near them, and the criminals fight the nearer of the two.
// STUNT's BMX rings count when its bike rides through them. KICKSTA's coronas count when its Sanchez rides through them,
// BLOOD's checkpoints when its car does, and QUARRY's rocks and bombs its dozer pushes (its copy in the owner's world is a
// physical car) count where they lie. In a joint valet shift (valet.scm, a streamed script) the guest's car it parks
// counts in the owner's shift. At a joint shooting range (SHRANGE) it stands in the owner's booth with the owner's gun,
// and its hits on its copies of the targets break the owner's ones as the owner's shots.
// The fires it puts out and the criminals it kills count by themselves (Fire, Damage). In the taxi job, which has one
// fare at a time near its own player, each player drives their own fares instead (Missions: SideUpdate). In a burglary
// both carry things out of the houses into the one van: the other player's game lets it into the houses, keeps its noise
// meter and takes the thing it puts down at the van's back, and the owner's BURGJB counts it (48@). A thing either one
// lifts goes from the other's house too, and the residents woken in one game wake in the same house of the other. The
// houses each game shuts at its start by chance are open in both while the burglary is joint.
#include <cstdint>
#include <string>
#include <vector>

namespace coop::jobs {

// What a joint burglary of this game has to tell the other one (Missions sends it as G_SIDE)
struct Tell {
    bool loot = false;   // joined: a thing of ours went into the van
    bool taken = false;  // we lifted a thing of a house: it goes from the other's house too (model, its place)
    bool alarm = false;  // the residents woke for us (where we are): those of the same house there wake too
    int model = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
// every frame: the job this game takes part in (joined) and its own job with the other player in it (owned), empty
// for none
std::vector<Tell> Update(const std::string& joined, const std::string& owned);
// joined: the line for the box with a thing in hand at the van's back (Missions' SideCallText), empty otherwise
std::string LootText(std::string& hint);
// owner: the other player put a thing into our burglary van; the things in it now (0: no burglary of ours runs)
int Stashed();
// the other player lifted the thing of a house standing at this place: ours goes too
void Taken(int model, float x, float y, float z);
// the residents woke for the other player there: ours of the same house wake as well
void Alarm(float x, float y, float z);
// joined: the burglary's noise meter (its variable) this game shows of its own player, not the owner's; 0 none
uint32_t NoiseVar();
// a world was loaded: its entrances are new (their hours are their own, none of a joint burglary's to give back)
void WorldLoaded();

// Missions' command hook, before a command of this game's joint job: the variable the command takes a vehicle or a
// player from, set to the other player's for this one command (a patient goes into its ambulance, a criminal fights it),
// null for none. The hook puts `was` back right after the command.
int32_t* BeforeCommand(void* script, uint16_t op, const uint8_t* ip, int32_t& was);
// after a command of this game's joint job: the answer to put in place of the script's own (a LOCATE of its player, true
// for the other player too), -1 none
int Answer(void* script, uint16_t op);

// the valet's shift (valet.scm, with its player at work: $7480): its thread, or null
bool ValetThread(void* script);
void* ValetShift();
// after a command of our valet thread in a shift the other player joined: GET_DRIVER_OF_CAR and GET_CAR_CHAR_IS_USING
// made our player's for the other one's car; and the answer for its player's IS_CHAR_IN_CAR and LOCATE_CHAR_ANY_MEANS_CAR_3D
// (-1 none)
void ValetAfter(uint16_t op, const uint8_t* ip);
int ValetAnswer(uint16_t op);

// SHRANGE waits at its marker (stage 0: it runs from the moment its player walks into the range's room), no round yet
bool RangeWaits(void* script);

}  // namespace coop::jobs
