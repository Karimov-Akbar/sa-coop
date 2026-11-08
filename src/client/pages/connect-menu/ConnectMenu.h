#pragma once

namespace coop::ui {

// In-game "SA:COOP" window (F9 / shown automatically in the main menu): nickname, address,
// port, password, "Connect" and "Create server" (with save slot selection).
void SetConnectMenuVisible(bool visible);
bool IsConnectMenuVisible();
// a key pressed while its keys page waits for one: taken as that action's key (true: the key is used up)
bool ConnectMenuKey(int vk);
void ProcessConnectMenu();  // per frame, before rendering
void RenderConnectMenu();

}  // namespace coop::ui
