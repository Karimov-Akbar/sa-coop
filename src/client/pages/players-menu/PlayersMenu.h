#pragma once
#include <cstdint>
#include <string>

namespace coop::ui {

// The F2 window: the players with their role, money and ping; another player's row gives that player money of ours (the
// game's $ sign). It takes the mouse and the keys while open, as F9 does.
void SetPlayersMenuVisible(bool visible);
bool IsPlayersMenuVisible();
void TogglePlayersMenu();  // F2: opened online and in the game only
void RenderPlayersMenu();
// tests (the debug bot): the window open with the gift row for that player, the amount typed in
void TestGift(uint8_t to, const std::wstring& amount);

}  // namespace coop::ui
