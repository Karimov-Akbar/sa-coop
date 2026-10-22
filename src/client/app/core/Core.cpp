#include "shared/lib/core/Core.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>

#include "app/crash-handler/CrashHandler.h"
#include "app/patches/Patches.h"
#include "common/config/version/Version.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "features/story/missions/Missions.h"
#include "pages/connect-menu/ConnectMenu.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "widgets/overlay/Overlay.h"

namespace coop::core {

namespace {
std::vector<Module*>& Modules() {
    static std::vector<Module*> m;
    return m;
}


using VoidFn = void(__cdecl*)();
using MenuProcessFn = void(__fastcall*)(void* self, void* edx);
VoidFn g_origGameProcess = nullptr;
MenuProcessFn g_origMenuProcess = nullptr;
VoidFn g_origDebugTextIngame = nullptr;
VoidFn g_origDebugTextMenu = nullptr;
VoidFn g_origShutDownForRestart = nullptr;
VoidFn g_origInitWhenRestarting = nullptr;
bool g_restarting = false;  // between CGame::ShutDownForRestart and InitialiseWhenRestarting
int g_initDepth = 0;        // inside CGame::InitialiseWhenRestarting

uint32_t g_lastCommonFrame = 0xFFFFFFFF;
bool g_pendingAutoConnect = false;
bool g_gameStarted = false;
uint32_t g_frameNo = 0;

// ----- save file redirection -----
std::string g_redirectPath;  // ANSI path; when set, slot kRedirectSlot maps to this file
using GenFilenameFn = void(__fastcall*)(void*, void*, int, char*);
GenFilenameFn g_origGenerateGameFilename = nullptr;

void __fastcall HookGenerateGameFilename(void* self, void* edx, int slot, char* out) {
    if (slot == flow::kRedirectSlot && !g_redirectPath.empty()) {
        strcpy(out, g_redirectPath.c_str());
        return;
    }
    g_origGenerateGameFilename(self, edx, slot, out);
}

void CommonFrame();

void __cdecl HookGameProcess() {
    ++g_frameNo;
    CommonFrame();
    for (auto* m : Modules()) m->OnPreProcess();
    g_origGameProcess();
    // The frame a new game's player is made in (MAIN's CREATE_PLAYER): the game starts for the modules next frame
    // (OnGameStarted, G_RESYNC), and what they announced in this one was forgotten here and made twice there.
    if (!g_gameStarted && IsPlayerReady()) return;
    for (auto* m : Modules()) m->OnPostProcess();
}

void __fastcall HookMenuProcess(void* self, void* edx) {
    g_origMenuProcess(self, edx);
    // FrontendIdle: CGame::Process is not running
    if (game::GameState() != game::GAME_STATE_IDLE) {
        ++g_frameNo;
        CommonFrame();
    }
}

std::string g_screenshotPath, g_screenshotOverlayPath;
int g_screenshotScale = 2;
uint64_t g_screenshotAt = 0;

void RenderOverlay() {
    auto& g = ui::Gfx();
    if (!g_screenshotPath.empty() && NowMs() >= g_screenshotAt) {  // before our overlay is drawn
        if (!g.SaveFrame(g_screenshotPath, g_screenshotScale)) LOGW("Screenshot %s failed", g_screenshotPath.c_str());
        g_screenshotPath.clear();
    }
    if (!g.Begin()) return;
    ui::In().UpdateMouse(g.Width(), g.Height());
    if (IsPlayerReady())
        for (auto* m : Modules()) m->OnRender();
    ui::RenderOverlay();
    ui::TheChat().Render();
    ui::RenderConnectMenu();
    g.End();
    ui::In().EndFrame();
    if (!g_screenshotOverlayPath.empty() && NowMs() >= g_screenshotAt) {  // with our overlay
        if (!g.SaveFrame(g_screenshotOverlayPath, g_screenshotScale)) LOGW("Screenshot %s failed", g_screenshotOverlayPath.c_str());
        g_screenshotOverlayPath.clear();
    }
}

bool g_overlayDrawn = false;  // by the game's own last 2D call since the last Present

void __cdecl HookDebugTextIngame() {
    if (g_origDebugTextIngame) g_origDebugTextIngame();
    RenderOverlay();
    g_overlayDrawn = true;
}

void __cdecl HookDebugTextMenu() {
    if (g_origDebugTextMenu) g_origDebugTextMenu();
    RenderOverlay();
    g_overlayDrawn = true;
}

// The overlay is drawn from the game's last 2D call (Idle -> CDebug::DebugDisplayTextBuffer). A plugin that took that
// call without passing it on left a player with no chat, no F9 window and no invite code to give: after 30 frames
// without it (in the menu or in the game, not on a loading screen) it is drawn at IDirect3DDevice9::Present instead.
using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFn g_origPresent = nullptr;
HRESULT WINAPI HookPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    static int missed = 0;
    static bool said = false;
    int state = game::GameState();
    if (g_overlayDrawn) {
        g_overlayDrawn = false;
        missed = 0;
    } else if ((state == game::GAME_STATE_IDLE || state == game::GAME_STATE_FRONTEND_IDLE) && ++missed > 30 &&
               SUCCEEDED(dev->BeginScene())) {
        if (!said) {
            said = true;
            LOGW("The overlay's draw call is not reached (another plugin took it): drawn at Present instead");
        }
        RenderOverlay();
        dev->EndScene();
    }
    return g_origPresent(dev, src, dst, wnd, dirty);
}

void __cdecl HookShutDownForRestart() {
    LOGI("CGame::ShutDownForRestart");
    if (g_gameStarted) {
        g_gameStarted = false;
        for (auto* m : Modules()) m->OnGameShutdown();
        flow::OnGameShutdown();
    }
    // A load that fails restarts from inside InitialiseWhenRestarting (0x53C758) and runs the new game's scripts there
    // at once: holding them would leave the game without a player for good
    g_restarting = g_initDepth == 0;
    g_origShutDownForRestart();
}

void __cdecl HookInitialiseWhenRestarting() {
    g_restarting = false;  // the new world's scripts run inside (they create the player for a new game)
    ++g_initDepth;
    g_origInitWhenRestarting();
    --g_initDepth;
}

void HandleChatSubmit(const std::string& text) {
    if (!text.empty() && text[0] == '/') {
        auto parts = Split(text.substr(1), ' ');
        std::vector<std::string> args;
        for (size_t i = 1; i < parts.size(); ++i)
            if (!parts[i].empty()) args.push_back(parts[i]);
        std::string cmd = parts.empty() ? "" : ToLower(parts[0]);
        if (cmd == "disconnect" || cmd == "quitcoop") {
            flow::Leave();
            return;
        }
        if (cmd == "coop" || cmd == "menu") {
            ShowConnectMenu(true);
            return;
        }
        if (cmd == "debug") {
            Cfg().debugOverlay = !Cfg().debugOverlay;
            return;
        }
        for (auto* m : Modules())
            if (m->OnCommand(cmd, args)) return;
        if (Net().Online()) Net().SendChat(text);
        else ui::TheChat().AddError(TR("Вы не подключены к серверу.", "You are not connected to a server."));
        return;
    }
    if (Net().Online()) Net().SendChat(text);
    else ui::TheChat().AddError(TR("Вы не подключены. Окно SA:COOP открывается клавишей F9.",
                                   "You are not connected. Press F9 to open the SA:COOP window."));
}

void CommonFrame() {
    Heartbeat();
    if (g_lastCommonFrame == g_frameNo) return;
    g_lastCommonFrame = g_frameNo;

    if (!g_origPresent)
        if (auto* dev = (IDirect3DDevice9*)game::D3DDevice())
            g_origPresent = (PresentFn)mem::SwapVTable(*(void***)dev, 17, (void*)&HookPresent);  // IDirect3DDevice9::Present
    if (!ui::In().Installed()) {
        if (void* hwnd = game::GameWindow()) {
            ui::In().Install(hwnd);
            patches::FixPluginWrappers();  // the first frame: the ASI plugins are loaded
            ReclaimCrashFilter();
        }
    }
    static uint64_t lastPlacement = 0;
    if (NowMs() - lastPlacement > 1000) {
        lastPlacement = NowMs();
        patches::ApplyWindowPlacement();
    }

    if (g_pendingAutoConnect && game::GameState() >= game::GAME_STATE_FRONTEND_IDLE) {
        g_pendingAutoConnect = false;
        auto& cfg = Cfg();
        if (cfg.wantsHost) flow::Host(cfg.loadSlot, (uint16_t)cfg.port, cfg.password);
        else flow::Join(cfg.host, (uint16_t)cfg.port, cfg.password);
    }

    // detect "game became playable" before this frame's messages: handled first, they made copies (trains, peds,
    // cars, fires) that every module's OnGameStarted then forgot, left in the world untracked
    if (!g_gameStarted && IsPlayerReady()) {
        g_gameStarted = true;
        LOGI("Game started (player ready)");
        // The scripts are held from CGame::ShutDownForRestart to InitialiseWhenRestarting. A load that went another way
        // (a plugin's) never let them go: no script ran, PSAVE1 left the loaded save's mission flag up, no story markers.
        if (g_restarting) {
            g_restarting = false;
            LOGW("The scripts were still held after the world load (no InitialiseWhenRestarting seen): let go");
        }
        g_redirectPath.clear();
        patches::FixPluginWrappers();  // and the ones loaded later
        flow::OnGameStarted();
        for (auto* m : Modules()) m->OnGameStarted();
    }

    Net().Update();
    flow::Update();

    ui::TheChat().ProcessInput();
    ui::ProcessConnectMenu();
}

std::wstring CoopDirFromModule(void* hModule) {
    std::wstring dir = ModuleDir(hModule);
    std::wstring lower = dir;
    CharLowerBuffW(lower.data(), (DWORD)lower.size());
    if (lower.size() >= 6 && lower.compare(lower.size() - 6, 6, L"\\coop\\") == 0) return dir;
    return dir + L"coop\\";
}

void WriteDefaultConfig(const std::wstring& path) {
    bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;  // a new install speaks the language of Windows
    std::string text =
        std::string("; Настройки SA:COOP (файл в кодировке UTF-8). Ник, адрес и пароль проще менять в игре, в окне F9.\r\n"
                    "\r\n"
                    "[player]\r\n"
                    "; Ник: до 20 символов (буквы, цифры, знаки _-.[])\r\n") +
        (ru ? "name = Игрок\r\n" : "name = Player\r\n") +
        "; Язык окна F9, чата и подсказок: ru или en. Переключается и в окне F9.\r\n"
        "language = " + (ru ? "ru" : "en") + "\r\n"
        "\r\n"
        "[connect]\r\n"
        "; Поставьте 1, чтобы подключаться сразу при запуске игры, без окна F9.\r\n"
        "auto_connect = 0\r\n"
        "; Режим: join (подключиться к другу) или host (создать свой сервер)\r\n"
        "mode = join\r\n"
        "; Адрес друга (IP или имя компьютера) или код приглашения вида XXXX-XXXX-XXXX\r\n"
        "host = 127.0.0.1\r\n"
        "port = 7788\r\n"
        "password =\r\n"
        "; Для хоста: номер сохранения (1–8), которое загрузится при автозапуске. Если 0, сохранение выбирается в меню.\r\n"
        "load_slot = 0\r\n"
        "; Для хоста: поставьте 1, чтобы игроки могли ранить и убивать друг друга. Это галочка в окне F9.\r\n"
        "friendly_fire = 0\r\n"
        "\r\n"
        "[game]\r\n"
        "; Поставьте 1, чтобы играть в окне.\r\n"
        "windowed = 0\r\n"
        "; Игра не встаёт на паузу, когда окно не в фокусе. Для совместной игры это нужно.\r\n"
        "run_in_background = 1\r\n"
        "; Меню паузы не останавливает мир, чтобы напарник мог играть дальше.\r\n"
        "no_pause_menu = 1\r\n"
        "skip_intro = 1\r\n"
        "\r\n"
        "[voice]\r\n"
        "enabled = 1\r\n"
        "; Клавиша голосовой связи (код клавиши Windows, 66 = B)\r\n"
        "push_to_talk_key = 66\r\n"
        "; Поставьте 1, чтобы микрофон работал всегда, без клавиши.\r\n"
        "always_on = 0\r\n"
        "; Громкость напарника и усиление микрофона. Значение 1.0 оставляет их без изменений.\r\n"
        "volume = 1.0\r\n"
        "mic_gain = 1.0\r\n"
        "; Чем дальше напарник, тем тише его слышно, но совсем он не пропадает.\r\n"
        "proximity = 1\r\n"
        "\r\n"
        "[keys]\r\n"
        "; Коды клавиш Windows: чат (84 = T), сесть пассажиром (71 = G), список игроков (113 = F2),\r\n"
        "; колесо эмоций, пока клавиша зажата (74 = J)\r\n"
        "chat = 84\r\n"
        "enter_as_passenger = 71\r\n"
        "player_list = 113\r\n"
        "emote_wheel = 74\r\n"
        "\r\n"
        "[debug]\r\n"
        "; Отладочная информация на экране и подробный лог. Включайте, если вас попросили прислать лог.\r\n"
        "overlay = 0\r\n"
        "verbose_log = 0\r\n";
    WriteFileBytes(path, text.data(), text.size());
}

}  // namespace

void RegisterModule(Module* m) { Modules().push_back(m); }

bool IsInCoopSession() { return Net().Online(); }

bool IsInGame() { return game::GameState() == game::GAME_STATE_IDLE; }

bool IsPlayerReady() { return IsInGame() && game::FindPlayerPed(0) != nullptr; }

bool IsRestarting() { return g_restarting; }

bool IsOnMission() {
    int32_t off = game::Ref<int32_t>(0xA476AC);  // CTheScripts::OnAMissionFlag (offset in script space)
    if (off <= 0 || off > 200000) return false;
    return game::Ref<int32_t>(game::addr::ScriptSpace + (uintptr_t)off) != 0;
}

void PeerResynced(uint8_t id) {
    for (auto* m : Modules()) m->OnPeerResync(id);
}

void Notify(const std::string& utf8, uint32_t argb) { ui::TheChat().Add(utf8, argb); }
void RequestScreenshot(const std::string& bmpPath, bool withOverlay, int scale, uint32_t delayMs) {
    (withOverlay ? g_screenshotOverlayPath : g_screenshotPath) = bmpPath;
    g_screenshotScale = std::max(scale, 1);
    g_screenshotAt = NowMs() + delayMs;
}

void ShowConnectMenu(bool show) { ui::SetConnectMenuVisible(show); }
bool ConnectMenuVisible() { return ui::IsConnectMenuVisible(); }

bool OnKeyDown(int vk, bool repeat) {
    auto& cfg = Cfg();
    if (ui::IsConnectMenuVisible()) {
        if (vk == VK_ESCAPE || vk == VK_F9) {
            if (!repeat) ui::SetConnectMenuVisible(false);
            return true;
        }
        return true;  // the menu owns the keyboard while open
    }
    if (ui::TheChat().IsOpen()) return true;
    if (repeat) return false;
    if (vk == VK_ESCAPE && CutsceneSkipDecline()) return true;  // "watch on", not the pause menu
    if (vk == VK_F9) {
        ui::SetConnectMenuVisible(true);
        return true;
    }
    bool inMenu = game::MenuActive();
    if (vk == cfg.chatKey && !inMenu && (IsPlayerReady() || Net().Online())) {
        ui::TheChat().Open();
        MSG m;  // the key's own letter, queued already by TranslateMessage ("t", or "е" in a Russian layout)
        PeekMessageW(&m, nullptr, WM_CHAR, WM_CHAR, PM_REMOVE);
        return true;
    }
    if (vk == cfg.playerListKey) {
        ui::TogglePlayerList();
        return true;
    }
    if (vk == cfg.passengerKey && !inMenu && IsPlayerReady()) {
        Players::Instance().TryEnterAsPassenger();
        return false;  // G still reaches the game (gang recruiting)
    }
    return false;
}

std::wstring UserFilesDir() {
    const char* p = (const char*)0xC92368;  // gta_user_dir_path
    std::string s(p);
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    w.resize(wcslen(w.c_str()));
    if (!w.empty() && w.back() != L'\\') w += L'\\';
    return w;
}

std::wstring SlotPath(int slot) { return UserFilesDir() + L"GTASAsf" + std::to_wstring(slot) + L".b"; }

static std::string ToAnsiPath(const std::wstring& w) {
    BOOL bad = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &bad);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, s.data(), n, nullptr, &bad);
    s.resize(strlen(s.c_str()));
    if (bad) {
        // fall back to the 8.3 short name which is always ASCII
        wchar_t shortPath[MAX_PATH];
        if (GetShortPathNameW(w.c_str(), shortPath, MAX_PATH)) {
            n = WideCharToMultiByte(CP_ACP, 0, shortPath, -1, nullptr, 0, nullptr, nullptr);
            s.assign(n, '\0');
            WideCharToMultiByte(CP_ACP, 0, shortPath, -1, s.data(), n, nullptr, nullptr);
            s.resize(strlen(s.c_str()));
        }
    }
    return s;
}

bool LoadSaveFile(const std::wstring& path) {
    if (!FileExists(path)) {
        LOGE("LoadSaveFile: no file %s", WideToUtf8(path).c_str());
        return false;
    }
    g_redirectPath = ToAnsiPath(path);
    if (g_redirectPath.size() >= 250) {
        LOGE("Save path too long");
        g_redirectPath.clear();
        return false;
    }
    const uintptr_t mm = game::addr::MenuManager;
    // CGenericGameStorage::CheckSlotDataValid(slot) — reads the file through GenerateGameFilename
    bool valid = game::Call<0x5D1380, bool>(flow::kRedirectSlot);
    if (!valid) {
        LOGE("Save file is not valid: %s", g_redirectPath.c_str());
        g_redirectPath.clear();
        return false;
    }
    game::Ref<uint8_t>(mm + 0x15F) = (uint8_t)flow::kRedirectSlot;  // m_SelectedSlot
    if (!game::Ref<uint8_t>(mm + 0xE9)) {                            // !m_bMainMenuSwitch -> in game
        game::CallMethod<0x573330>((void*)mm);                        // DoSettingsBeforeStartingAGame
    }
    game::Ref<uint8_t>(mm + 0x32) = 1;        // m_bDontDrawFrontEnd
    game::Ref<uint8_t>(0xB72910) = 0;         // CGame::bMissionPackGame
    game::Ref<uint8_t>(mm + 0x60) = 1;        // m_bLoadingData
    LOGI("Loading save %s", g_redirectPath.c_str());
    return true;
}

bool StartNewGame() {  // what the menu's "Start new game, yes" does (MENU_ACTION_STANDARD_GAME)
    const uintptr_t mm = game::addr::MenuManager;
    g_redirectPath.clear();
    game::Ref<uint8_t>(0xB72910) = 0;       // CGame::bMissionPackGame
    game::Ref<uint8_t>(mm + 0x60) = 0;      // m_bLoadingData: no save
    game::CallMethod<0x573330>((void*)mm);  // DoSettingsBeforeStartingAGame: m_bStartGameLoading
    game::Ref<uint8_t>(mm + 0x32) = 1;      // m_bDontDrawFrontEnd
    LOGI("Starting a new game");
    return true;
}

bool InPrologue() {  // a new game's first mission ("INTRO"): it starts by itself and ends once CJ gets on the bike
    for (void* s = game::Ref<void*>(0xA8B42C); s; s = game::Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (game::Field<uint8_t>(s, 0xDC) && !_strnicmp((const char*)s + 0x08, "intro", 8)) return true;  // m_bIsMission, m_szName
    return false;
}

bool g_snapshot = false;
bool SavingSnapshot() { return g_snapshot; }

bool SaveWorldSnapshot(std::vector<uint8_t>& out) {
    // the game never saves with the player in a vehicle: loading such a save leaves no player
    if (!IsPlayerReady() || IsOnMission() || game::FindPlayerPed(0)->InVehicle()) return false;
    std::wstring tmp = Cfg().coopDir + L"world\\host_snapshot.b";
    EnsureDirectory(Cfg().coopDir + L"world");
    std::string ansi = ToAnsiPath(tmp);
    if (ansi.size() >= 250) return false;
    // CGenericGameStorage::ms_SaveFileName — GenericSave writes to this path
    char* saveName = (char*)0xC16DB8;
    char backup[260];
    memcpy(backup, saveName, sizeof(backup));
    strcpy(saveName, ansi.c_str());
    // GenericSave ends with CPad::UpdatePads (0x5D171F), a second update in this frame that ate the keys pressed just
    // now: the pads, the keyboard and the mouse are put back as they were (CPad::NewMouseControllerState and Old)
    const std::pair<uintptr_t, size_t> input[] = {{game::addr::Pads, 2 * sizeof(game::CPad)}, {game::addr::NewKeyState, 0x270},
                                                  {game::addr::OldKeyState, 0x270}, {0xB73418, 0x28}};
    std::vector<uint8_t> kept;
    for (auto [at, size] : input) kept.insert(kept.end(), (uint8_t*)at, (uint8_t*)at + size);
    g_snapshot = true;
    bool ok = game::Call<0x5D13E0, bool>(0);  // CGenericGameStorage::GenericSave
    g_snapshot = false;
    for (size_t pos = 0; auto [at, size] : input) memcpy((void*)at, kept.data() + pos, size), pos += size;
    memcpy(saveName, backup, sizeof(backup));
    if (!ok) {
        LOGW("GenericSave failed");
        return false;
    }
    return ReadFileBytes(tmp, out) && out.size() > 1024;
}

void Init(void* hModule) {
    auto& cfg = Cfg();
    std::wstring coopDir = CoopDirFromModule(hModule);
    EnsureDirectory(coopDir);
    if (!FileExists(coopDir + L"coop.ini")) WriteDefaultConfig(coopDir + L"coop.ini");
    cfg.Load(coopDir);  // reads -coopcfg; does not log
    std::wstring logPath = cfg.iniPath.substr(0, cfg.iniPath.size() - 4) + L".log";
    log::Open(logPath);
    LOGI("%s %s (protocol %u) loaded, coop dir: %s, config: %s", COOP_NAME, COOP_VERSION_STR, kProtocolVersion,
         WideToUtf8(coopDir).c_str(), WideToUtf8(cfg.iniPath).c_str());
    if (!cfg.verboseLog) log::SetMinLevel(log::Level::Info);
    InstallCrashHandler(coopDir);
    if (cfg.verboseLog) StartHangDetector(15);

    // only gta_sa.exe 1.0 US is supported: every address in this mod is version specific
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    GetFileAttributesExW(exePath, GetFileExInfoStandard, &fad);
    LOGI("gta_sa.exe size: %u", (unsigned)fad.nFileSizeLow);
    // 1.0 US signature (both HOODLUM and "compact" executables share all addresses)
    if (*(uint32_t*)0x82457C != 0x94BF) {
        LOGE("Unsupported gta_sa.exe (size %u). SA:COOP requires GTA SA 1.0 US.", (unsigned)fad.nFileSizeLow);
        MessageBoxW(nullptr,
                    TR(L"SA:COOP работает только с GTA San Andreas версии 1.0 US.\n"
                       L"Если игра из Steam или Rockstar Games Launcher, откатите её до 1.0 US\n"
                       L"программой GTA SA Downgrader.\n\n"
                       L"Пока мод выключен.",
                       L"SA:COOP works only with GTA San Andreas 1.0 US.\n"
                       L"If your game comes from Steam or the Rockstar Games Launcher, downgrade it to 1.0 US\n"
                       L"with GTA SA Downgrader.\n\n"
                       L"The mod stays off for now."),
                    L"SA:COOP", MB_ICONWARNING);
        return;
    }

    if (!mem::Init()) return;

    patches::Apply();

    // frame hooks (call-site redirections; chain to whatever was there before)
    mem::RedirectCallFn(0x53E981, &HookGameProcess, &g_origGameProcess);      // Idle -> CGame::Process
    mem::RedirectCallFn(0x53E7A5, &HookMenuProcess, &g_origMenuProcess);      // FrontendIdle -> CMenuManager::Process
    mem::RedirectCallFn(0x53EBE4, &HookDebugTextIngame, &g_origDebugTextIngame);  // Idle -> DebugDisplayTextBuffer
    mem::RedirectCallFn(0x53E86C, &HookDebugTextMenu, &g_origDebugTextMenu);  // FrontendIdle -> DebugDisplayTextBuffer
    mem::HookFn(0x6190A0, &HookGenerateGameFilename, &g_origGenerateGameFilename, "C_PcSave::GenerateGameFilename");
    mem::HookFn(0x53C550, &HookShutDownForRestart, &g_origShutDownForRestart, "CGame::ShutDownForRestart");
    mem::HookFn(0x53C680, &HookInitialiseWhenRestarting, &g_origInitWhenRestarting, "CGame::InitialiseWhenRestarting");

    for (auto* m : Modules()) {
        LOGI("Module: %s", m->Name());
        m->OnInstallHooks();
    }

    ui::TheChat().onSubmit = HandleChatSubmit;

    auto& net = Net();
    net.onOnline = [] {
        for (auto* m : Modules()) m->OnSessionStart();
    };
    net.onOffline = [](const std::string& reason) {
        if (!reason.empty()) ui::TheChat().AddError(reason);  // our own disconnects explain themselves
        for (auto* m : Modules()) m->OnSessionEnd();
    };
    net.onPeerJoin = [](uint8_t id) {
        ui::TheChat().AddSystem(Format(TR("%s подключается...", "%s is connecting..."), Net().PeerName(id).c_str()));
        for (auto* m : Modules()) m->OnPeerJoin(id);
    };
    net.onPeerLeave = [](uint8_t id) {
        for (auto* m : Modules()) m->OnPeerLeave(id);
    };
    net.onHostChanged = [](uint8_t id) {
        for (auto* m : Modules()) m->OnHostChanged(id);
    };
    net.onWorld = [](const std::vector<uint8_t>& data, const std::string&) { flow::OnWorldReceived(data); };
    net.onWorldWanted = [] { flow::RequestFreshWorld(); };
    net.onWorldNewGame = [] { flow::OnNewGameFromHost(); };
    net.onWorldWait = [] { flow::OnHostInPrologue(); };
    net.onWorldNone = [] { flow::OnWorldNone(); };
    net.onInvite = [](const std::string& code) {
        if (Net().IsHost())
            ui::TheChat().AddSystem(Format(TR("Код для друга: %s. Скопировать его можно в окне F9.",
                                              "Code for your friend: %s. You can copy it in the F9 window."),
                                           code.c_str()));
    };

    // auto-connect (launcher / test setups) is performed on the first main-menu frame:
    // the user files directory and the network stack are not ready inside DllMain
    g_pendingAutoConnect = cfg.autoConnect;
    LOGI("Initialisation complete");
}

}  // namespace coop::core
