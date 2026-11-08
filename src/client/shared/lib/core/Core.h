#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coop {

// Base class for sync subsystems. All callbacks run on the game thread.
class Module {
public:
    virtual ~Module() = default;
    virtual const char* Name() const = 0;
    virtual void OnInstallHooks() {}            // once at startup (before the game initialises)
    virtual void OnSessionStart() {}            // handshake completed
    virtual void OnSessionEnd() {}              // disconnected
    virtual void OnPeerJoin(uint8_t id) {}
    virtual void OnPeerLeave(uint8_t id) {}
    virtual void OnPeerResync(uint8_t id) {}   // its game (re)started: what it had of ours is gone there
    virtual void OnHostChanged(uint8_t newHost) {}
    virtual void OnGameStarted() {}             // a game (new or loaded) became playable
    virtual void OnGameShutdown() {}            // restarting / loading another save
    virtual void OnPreProcess() {}              // before CGame::Process
    virtual void OnPostProcess() {}             // after CGame::Process
    virtual void OnRender() {}                  // HUD overlay (renderer is active)
    // Local chat command (without the leading '/'). Return true if handled.
    virtual bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) { return false; }
};

namespace core {

void Init(void* hModule);
void RegisterModule(Module* m);
void PeerResynced(uint8_t id);  // dispatches Module::OnPeerResync

// Hotkey routing from the window procedure. Returns true if the key must not reach the game.
bool OnKeyDown(int vk, bool repeat);
bool IsInCoopSession();
bool IsInGame();          // a game is loaded and running (not the main menu)
bool IsPlayerReady();     // local CPlayerPed exists and the game is in GAME_STATE_IDLE
bool IsRestarting();      // the world is being torn down for a load: no player, scripts must not run

// UI helpers
void Notify(const std::string& utf8, uint32_t argb = 0xFFFFFFFF);  // message in the chat area
// tests: a frame as a BMP (scale 2: half size), without our overlay unless asked, the first one after delayMs
void RequestScreenshot(const std::string& bmpPath, bool withOverlay = false, int scale = 2, uint32_t delayMs = 0);
void ShowConnectMenu(bool show);
bool ConnectMenuVisible();
bool InterfaceHidden();  // F7: the game's HUD and our chat and labels are not drawn (the windows a key opens are)

// Game flow
bool LoadSaveFile(const std::wstring& path);   // loads an arbitrary save file (from menu or in game)
bool StartNewGame();                           // as the menu's "Start new game" (from menu or in game)
bool SaveWorldSnapshot(std::vector<uint8_t>& out);  // host: serialises the current game into memory
bool SavingSnapshot();  // that save is going on (the player saves nothing: it is the partners' copy)
bool IsOnMission();
bool InPrologue();  // a new game's first mission runs (main.scm "INTRO", until CJ gets on the bike)
std::wstring UserFilesDir();
std::wstring SlotPath(int slot);  // 1..8

}  // namespace core
}  // namespace coop
