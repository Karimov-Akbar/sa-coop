// coop-server.exe — SA:COOP relay server.
//
//   coop-server.exe [--port N] [--name "..."] [--password X] [--world <save file> | --new-game] [--host <nick>]
//                   [--friendly-fire 0|1] [--partner-teleport 0|1] [--config server.ini] [--data <dir>]
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "app/server/Server.h"
#include "common/config/version/Version.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

using namespace coop;

static server::Server g_server;

static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        g_server.Stop();
        Sleep(300);
        return TRUE;
    }
    return FALSE;
}

static void WriteDefaultConfig(const std::wstring& path) {
    const char* text =
        "; Настройки сервера SA:COOP\r\n"
        "[server]\r\n"
        "name = SA:COOP server\r\n"
        "port = 7788\r\n"
        "; Оставьте пустым, если пароль не нужен.\r\n"
        "password =\r\n"
        "max_players = 2\r\n"
        "; Ник игрока, который всегда будет хостом. Если пусто, хостом становится тот, кто зашёл первым.\r\n"
        "host =\r\n"
        "motd = Добро пожаловать! Чтобы увидеть список команд, напишите /help.\r\n"
        "; Поставьте 1, чтобы игроки могли ранить и убивать друг друга.\r\n"
        "friendly_fire = 0\r\n"
        "; Поставьте 0, чтобы напарники не могли перемещаться командами /tp и /tphere.\r\n"
        "partner_teleport = 1\r\n"
        "; Игра через интернет (UPnP, код приглашения, пробивка NAT). Если поставить 0, останется только локальная сеть.\r\n"
        "internet = 1\r\n"
        "; Ретранслятор на случай, если друг не может подключиться по коду.\r\n"
        "; С бесплатным аккаунтом metered.ca:   turn = metered:<имя приложения>:<API-ключ>\r\n"
        "; С любым TURN-сервером:               turn = turn:<адрес>:<порт>:<логин>:<пароль>\r\n"
        "turn =\r\n";
    WriteFileBytes(path, text, strlen(text));
}

static bool Writable(const std::wstring& dir) {
    EnsureDirectory(dir);
    std::wstring probe = dir + L"write-test.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// The server runs as its invoker with no file virtualization (its manifest, since 0.3.0): in a game under Program Files
// it wrote nothing, and the partners lost their progress every session. Then its data goes to the user's own folder,
// and what 0.2.x wrote to the VirtualStore (UAC's redirection of those writes) comes along once.
static std::wstring UserDataDir(const std::wstring& wanted) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(buf) + L"\\SA-COOP\\server-data\\";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) && wanted.size() > 3 && wanted[1] == L':') {
        std::wstring virtualStore = std::wstring(buf) + L"\\VirtualStore\\" + wanted.substr(3);
        if (std::filesystem::exists(virtualStore, ec)) {
            std::filesystem::create_directories(dir, ec);
            std::filesystem::copy(virtualStore, dir, std::filesystem::copy_options::recursive, ec);
        }
    }
    return Writable(dir) ? dir : L"";
}

// The game's voice chat asks first whether Windows' audio drivers survive being loaded (issue #80): on some machines the
// first WinMM call crashes in them, and in the game it took the game down. Here the crash is only the answer.
static LONG WINAPI ProbeCrashed(EXCEPTION_POINTERS* e) {
    TerminateProcess(GetCurrentProcess(), e->ExceptionRecord->ExceptionCode);  // no dialog: the code is the answer
    return EXCEPTION_EXECUTE_HANDLER;
}
static int ProbeAudio() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(ProbeCrashed);
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = 16000;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = 32000;
    HWAVEIN in = nullptr;
    if (waveInOpen(&in, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) waveInClose(in);
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) waveOutClose(out);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--probe-audio")) return ProbeAudio();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"SA:COOP server");

    std::wstring dir = ModuleDir();
    std::wstring configPath = dir + L"server.ini";
    std::wstring dataDir = dir + L"server-data\\";
    for (int i = 1; i + 1 < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--config") configPath = argv[++i];
        if (a == L"--data") {
            dataDir = argv[++i];
            if (!dataDir.empty() && dataDir.back() != L'\\') dataDir += L'\\';
        }
    }
    std::wstring wantedDir = dataDir;
    if (!Writable(dataDir))
        if (std::wstring own = UserDataDir(dataDir); !own.empty()) dataDir = own;
    log::Open(dataDir + L"server.log", true);
    if (dataDir != wantedDir) LOGW("Нет записи в %s: данные сервера в %s", WideToUtf8(wantedDir).c_str(), WideToUtf8(dataDir).c_str());

    if (!FileExists(configPath)) WriteDefaultConfig(configPath);
    Ini ini;
    ini.Load(configPath);
    server::Config cfg;
    cfg.name = ini.Get("server", "name", cfg.name);
    cfg.port = (uint16_t)ini.GetInt("server", "port", kDefaultPort);
    cfg.password = ini.Get("server", "password", "");
    cfg.maxPlayers = ini.GetInt("server", "max_players", 2);
    cfg.hostName = ini.Get("server", "host", "");
    cfg.motd = ini.Get("server", "motd", cfg.motd);
    cfg.friendlyFire = ini.GetBool("server", "friendly_fire", false);
    cfg.partnerTeleport = ini.GetBool("server", "partner_teleport", true);
    cfg.internet = ini.GetBool("server", "internet", true);
    cfg.turn = Trim(ini.Get("server", "turn", ""));
    cfg.dataDir = dataDir;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? WideToUtf8(argv[++i]) : std::string(); };
        if (a == L"--port") cfg.port = (uint16_t)atoi(next().c_str());
        else if (a == L"--name") cfg.name = next();
        else if (a == L"--password") cfg.password = next();
        else if (a == L"--host") cfg.hostName = next();
        else if (a == L"--max") cfg.maxPlayers = atoi(next().c_str());
        else if (a == L"--world" && i + 1 < argc) cfg.worldFile = argv[++i];
        else if (a == L"--new-game") cfg.newGame = true;
        else if (a == L"--friendly-fire") cfg.friendlyFire = next() == "1";
        else if (a == L"--partner-teleport") cfg.partnerTeleport = next() == "1";
        else if (a == L"--config" || a == L"--data") ++i;
    }

    LOGI("%s сервер %s (протокол %u)", COOP_NAME, COOP_VERSION_STR, kProtocolVersion);
    if (!g_server.Start(cfg)) {
        LOGE("Сервер не запущен. Нажмите Enter для выхода.");
        getchar();
        return 1;
    }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    LOGI("Команды консоли: players, kick <ник или номер> [причина], say <текст>, quit");

    std::thread input([] {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        wchar_t buf[512];
        DWORD read = 0;
        while (ReadConsoleW(in, buf, 511, &read, nullptr) && read > 0) {
            std::wstring line(buf, read);
            while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
            if (!line.empty()) g_server.QueueConsoleCommand(WideToUtf8(line));
        }
    });
    input.detach();

    g_server.Run();
    return 0;
}
