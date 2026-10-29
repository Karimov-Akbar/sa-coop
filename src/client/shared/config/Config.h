#pragma once
#include <string>

namespace coop {

// Client settings, coop\coop.ini (written by the launcher, editable by hand).
struct ClientConfig {
    // [player]
    std::string nickname = "Player";
    bool english = false;             // language = en: the interface in English instead of Russian

    // [connect]
    bool autoConnect = false;  // the F9 menu by default; launchers and test setups switch it on
    std::string host = "127.0.0.1";
    int port = 7788;
    std::string password;
    bool wantsHost = false;   // started from "Create server" in the launcher
    int loadSlot = 0;         // host: save slot (1..8) to load automatically, -1 a new game, 0 = choose in the menu
    bool friendlyFire = false;  // host: the players can hurt each other
    bool partnerTeleport = true;  // host: the partners may use /tp and /tphere

    // [game]
    bool windowed = false;
    bool windowedStrict = false;      // test setups: quit at once if windowed mode could not be applied
    int windowX = -1, windowY = -1;   // -1 = centre
    int width = 0, height = 0;        // 0 = use the game's video mode
    bool borderless = false;          // windowed without a frame, up to the whole screen (a fullscreen look)
    std::string windowTitle;          // the window's caption (empty: the game's): tells two copies apart to a recorder
    bool multiInstance = false;       // allow several game copies (testing)
    bool runInBackground = true;      // keep simulating when the window is not focused
    bool streamCache = true;          // the game's files read through Windows' cache (off: past it, as the game does)
    bool noPauseMenu = true;          // the world keeps running while the pause menu is open
    std::string userDir;              // custom "GTA San Andreas User Files" directory (empty = auto)
    bool skipIntro = true;
    bool partnerBlip = true;          // the other players' marks on the radar and the map
    std::string markColour;           // our own mark's colour there, as the other players see it: white, red, green,
                                      // blue, yellow or RRGGBB (empty: the allies' blue)
    bool partnerArrow = true;         // ...drawn as an arrow turned with their heading, as the game draws its player
    bool nameTags = true;             // their names, health and armour over their heads (off for a blind shoot-out)
    bool gps = true;                  // the road route to the map target on the radar while driving

    // [voice]
    bool voiceEnabled = true;
    int voiceKey = 0x42;              // VK_B, push to talk
    bool voiceAlwaysOn = false;
    float voiceVolume = 1.0f;
    float micGain = 1.0f;
    bool voiceProximity = true;       // fade with distance
    int micDevice = -1;               // WAVE_MAPPER
    int speakerDevice = -1;

    // [keys]
    int chatKey = 0x54;               // T
    int passengerKey = 0x47;          // G
    int playerListKey = 0x71;         // F2

    // [debug]
    bool debugOverlay = false;
    bool cleanScreen = false;         // the mod draws nothing over the game (chat, names, menus): video recording
    bool verboseLog = false;
    bool forceRelay = false;          // invite codes: skip the direct attempt (relay tests)
    bool firewallPrompt = true;       // hosting: ask Windows to let the server through its firewall (off in test setups)
    std::string serverData = "server-data";  // the hosted server's folder in coop\ (test setups keep theirs apart)

    std::wstring coopDir;             // directory with coop.dll (…\coop\)
    std::wstring gameDir;             // directory with gta_sa.exe
    std::wstring iniPath;

    bool Load(const std::wstring& coopDirectory);
};

ClientConfig& Cfg();

}  // namespace coop

// A text of the interface in both languages, where it is used: TR("Подключиться", "Connect").
#define TR(ru, en) (::coop::Cfg().english ? (en) : (ru))
