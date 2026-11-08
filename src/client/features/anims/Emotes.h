#pragma once
// Emotes: animations a player starts on its own CJ from the U window or with /anims (Emotes.cpp), alone or together with
// the other player, the players' own animation files (coop\anims), and the window's preview (AnimPreview.cpp). What our
// CJ plays reaches the other games as any script animation of ours does (Anims.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace coop::anims {

enum Kind : uint8_t { EMOTION, POSE, ACTION, DANCE, PAIR, PACKS, CUSTOM, KINDS };
enum : uint8_t {
    LOOP = 1,   // plays until the player ends it (sprint or enter, Space or F)
    HOLD = 2,   // its last frame stays until the player ends it
    PART = 4,   // the upper body only: CJ walks on meanwhile
    MOVES = 8,  // CJ goes where it takes him (a flip ends metres away), as a script's with lock X and Y on
};

struct Emote {
    uint16_t id = 0;              // its number for /anims
    Kind kind = EMOTION;
    std::string name;             // in the language of the interface
    const char* block = "";       // its IFP block
    const char* in = nullptr;     // played once first
    const char* main = "";
    const char* out = nullptr;    // played once as it is stopped
    uint8_t flags = 0;
    const char* other = nullptr;  // a pair: what the other player plays (null: main too)
    float apart = 0.0f;           // a pair: metres between the two, face to face
    std::string pack;             // the packs' tab: the title over the pack's rows (its name and its author)
    const char* outBlock = nullptr;  // out's block when not block (a pack's pose on the ground gets up with the game's)
};

const char* KindName(Kind k);
const std::vector<Emote>& List();  // in the window's order (the names in the current language)
const Emote* Find(uint16_t id);

// why our CJ cannot start one now, empty: it can
std::string WhyNot();
// starts it on our CJ; a pair asks the other player first. False: the reason is in why
bool Play(const Emote& e, std::string& why);

// the other player's call to do a pair together: our answer (Y, N) and the centre box's line
bool Key(bool yes);
std::string Text(std::string& hint);

// the window's preview: our CJ's body (with the other player's for a pair) playing that emote in a picture of its own,
// kept while the window asks for it every frame (`pixels`: how big the window shows it); PreviewTexture is the picture
// (IDirect3DTexture9*, null: none yet)
void PreviewShow(const Emote* e, float pixels);
void PreviewHide();
void* PreviewTexture();
void PreviewTurn(float radians);

// a block loaded from coop\anims or coop\packs: its index differs between games, the others know it by its name
bool IsCustomBlock(int block);
// the IFP block in memory; asked for when it is not (false meanwhile), and let go again once neither our CJ nor the
// preview uses it (Emotes.cpp, ReleaseBlocks)
bool NeedBlock(int block);
// the preview's bodies play (or blend out) an animation of that block: it must stay in memory
bool PreviewHolds(int block);
// the folders of the players' own animation files and of the mod's packs
std::string CustomFolder();
std::string PacksFolder();

}  // namespace coop::anims
