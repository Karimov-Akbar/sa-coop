#pragma once
#include <cstdint>
#include <string>

namespace coop::ui {

// The F4 window: the game's people by tabs (men, women, gangs, service) with their model numbers, the chosen one standing
// in its preview, worn with Enter or the button (features/skins). It takes the mouse and the keys while open.
void ToggleSkinsMenu();
void SetSkinsMenuVisible(bool visible);
bool IsSkinsMenuVisible();
void RenderSkinsMenu();
// the stand's frames (the debug bot): the window open on that skin, a model number or an own skin's name; "-" closes it
void TestSkinsMenu(const std::string& item);

}  // namespace coop::ui
