#pragma once
#include <cstdint>

namespace coop::ui {

// The U window: the emotes by tabs (emotions, poses, actions, dances, together, own) with their numbers for /anims, the
// chosen one playing on a copy of CJ in its preview. Opened in the game while CJ may play one (else the chat says why).
// It takes the mouse and the keys while open (the arrows, Enter, U and Esc).
void ToggleAnimsMenu();
void SetAnimsMenuVisible(bool visible);
bool IsAnimsMenuVisible();
// when U opened it (0: closed): a cheat code beginning with U typed on closes it and goes to the game
uint64_t AnimsMenuOpenedMs();
void RenderAnimsMenu();
// tests (the debug bot): the window open on that emote
void TestAnimsMenu(uint16_t emote);

}  // namespace coop::ui
