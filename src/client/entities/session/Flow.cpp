#include "entities/session/Flow.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/firewall/Firewall.h"
#include "shared/ui/chat/Chat.h"

namespace coop::flow {

using namespace proto;

namespace {
Phase g_phase = Phase::Idle;
std::string g_error;
int g_hostSlot = 0;          // slot chosen for hosting (0 = current game)
uint16_t g_port = 7788;
std::string g_password, g_hostAddr;
bool g_isHosting = false;
uint64_t g_phaseTime = 0;
int g_connectAttempts = 0;

HANDLE g_serverProcess = nullptr;
HANDLE g_serverJob = nullptr;

bool g_worldUploadPending = false;  // host: a partner asked for a fresh world
bool g_keepSent = false;            // host: the stored world goes out meanwhile, a fresh one once it can be saved
bool g_prologue = false;            // host: a new game's prologue runs (it cannot be saved)
bool g_afterPrologue = false;       // host: and is over, no save of this game has gone out yet
bool g_prologueWait = false;        // partner: the host plays a new game's prologue
bool g_worldNone = false;           // partner: the server has no world for us yet (the host is still in the menu)
uint64_t g_gameStartedAt = 0, g_snapshotTriedAt = 0;
bool g_follower = false;            // partner: in a new game of its own next to the host, until its first save
bool g_progressUpload = false;      // host: the next snapshot goes to the partners in the game as well
std::vector<uint8_t> g_pendingWorld;  // partner: the host's world after its progress, loaded at a safe moment
uint64_t g_pendingSince = 0, g_pendingToldMs = 0;  // partner: when it came, when the log last said what holds it
uint64_t g_reloadFadeMs = 0;                       // partner: when the screen began to go black for that load
SavedSpot g_spot;
bool g_hasSpot = false;

void SetPhase(Phase p) {
    g_phase = p;
    g_phaseTime = NowMs();
}

void Fail(const std::string& why) {
    g_error = why;
    g_hasSpot = false;  // a reload that failed: its spot is no use to the next world
    g_pendingWorld.clear();
    LOGW("Flow error: %s", why.c_str());
    ui::TheChat().AddError(why);
    Net().Disconnect();
    SetPhase(Phase::Idle);
}

// A server of this install left by an earlier game whose process hung after a crash holds the port: the new server
// cannot open it and this game would join the old one (a wrong password, an old world)
void StopLeftoverServers(const std::wstring& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"coop-server.exe")) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && !_wcsicmp(path, exe.c_str()) && TerminateProcess(p, 0))
            LOGW("A server left by an earlier game was stopped (pid %lu)", pe.th32ProcessID);
        CloseHandle(p);
    }
    CloseHandle(snap);
}

// Friends reach the server through Windows Firewall, and many could not ("the server does not answer", on a home
// network too): its "Windows Security Alert" stays behind the full-screen game, and cancelled or unseen it leaves rules
// that block coop-server.exe; ticked for private networks only, it does not let it through on a public one. The host
// is asked for Windows' permission (its UAC prompt) to allow the server on every network instead, at each start of the
// server while it is still kept out.
void LetServerThroughFirewall(const std::wstring& exe) {
    firewall::State state = firewall::Check(exe);
    LOGI("Windows Firewall for the server: %s", state == firewall::State::Allowed   ? "allowed"
                                                : state == firewall::State::Blocked ? "blocked"
                                                : state == firewall::State::NoRule  ? "no rule for it"
                                                                                    : "not readable");
    if ((state != firewall::State::Blocked && state != firewall::State::NoRule) || !Cfg().firewallPrompt) return;
    ui::TheChat().AddSystem(TR("Брандмауэр Windows не пропускает друзей к серверу. Подтвердите запрос Windows, и мод разрешит coop-server.exe.",
                               "Windows Firewall keeps friends from the server. Confirm the Windows prompt and the mod will allow coop-server.exe."));
    if (!firewall::AllowElevated(exe))
        ui::TheChat().AddError(TR("Без этого разрешения друг не подключится. Разрешите coop-server.exe в брандмауэре Windows вручную.",
                                  "Without it your friend cannot connect. Allow coop-server.exe in Windows Firewall yourself."));
}

// Why Windows did not start the server, in words. 4550-4583 are its code integrity policy: on Windows 11 that is Smart
// App Control, which runs no program without a digital signature (error 4551 in a player's report).
std::string StartError(DWORD err) {
    if (err == ERROR_VIRUS_INFECTED || err == ERROR_VIRUS_DELETED || err == ERROR_ACCESS_DENIED)
        return Format(TR("Антивирус не дал запустить coop-server.exe (ошибка %lu). Добавьте папку игры в исключения антивируса.",
                         "An antivirus kept coop-server.exe from starting (error %lu). Add the game folder to the antivirus exclusions."),
                      err);
    if (err >= 4550 && err <= 4583)
        return Format(TR("Windows не дал запустить сервер (ошибка %lu). Интеллектуальное управление приложениями не пускает "
                         "программы без цифровой подписи, а у coop-server.exe её нет. К другу можно подключиться и так: для "
                         "этого сервер не нужен.",
                         "Windows kept the server from starting (error %lu). Smart App Control runs no program without a "
                         "digital signature, and coop-server.exe has none. You can still join a friend: that needs no server."),
                      err);
    if (err == ERROR_ACCESS_DISABLED_BY_POLICY)
        return Format(TR("Запуск coop-server.exe запрещён правилами Windows (ошибка %lu): групповой политикой, AppLocker или "
                         "родительским контролем.",
                         "Windows rules forbid starting coop-server.exe (error %lu): a group policy, AppLocker or parental "
                         "controls."),
                      err);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND || err == ERROR_BAD_EXE_FORMAT)
        return Format(TR("Файл coop\\coop-server.exe пропал или повреждён (ошибка %lu). Распакуйте архив мода заново.",
                         "The file coop\\coop-server.exe is missing or damaged (error %lu). Unpack the mod again."),
                      err);
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   (wchar_t*)&text, 0, nullptr);
    std::string why = text ? Trim(WideToUtf8(text)) : std::string();
    if (text) LocalFree(text);
    if (why.empty()) return Format(TR("Не удалось запустить сервер (ошибка %lu).", "Could not start the server (error %lu)."), err);
    return Format(TR("Не удалось запустить сервер (ошибка %lu: %s).", "Could not start the server (error %lu: %s)."), err, why.c_str());
}

bool StartServerProcess(const std::wstring& worldPath, bool newGame) {
    auto& cfg = Cfg();
    std::wstring exe = cfg.coopDir + L"coop-server.exe";
    if (!FileExists(exe)) {
        Fail(TR("Нет файла coop\\coop-server.exe. Скорее всего, его удалил антивирус: добавьте папку игры в исключения "
                "антивируса и распакуйте архив мода заново.",
                "The file coop\\coop-server.exe is missing. Most likely an antivirus removed it: add the game folder to the "
                "antivirus exclusions and unpack the mod again."));
        return false;
    }
    StopLeftoverServers(exe);
    LetServerThroughFirewall(exe);
    std::wstring cmd = L"\"" + exe + L"\" --port " + std::to_wstring(g_port) + L" --data \"" + cfg.coopDir +
                       Utf8ToWide(cfg.serverData) + L"\" --name \"" + Utf8ToWide(cfg.nickname) + L"\" --host \"" +
                       Utf8ToWide(cfg.nickname) + L"\"";
    if (!g_password.empty()) cmd += L" --password \"" + Utf8ToWide(g_password) + L"\"";
    if (!worldPath.empty()) cmd += L" --world \"" + worldPath + L"\"";
    if (newGame) cmd += L" --new-game";
    cmd += cfg.friendlyFire ? L" --friendly-fire 1" : L" --friendly-fire 0";  // the checkbox wins over server.ini
    cmd += cfg.partnerTeleport ? L" --partner-teleport 1" : L" --partner-teleport 0";

    if (!g_serverJob) {
        g_serverJob = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_serverJob, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    // Started running, not suspended to be put in the job first: a process made suspended and resumed is how malware
    // hollows one out, and antiviruses watch for it. The server starts no process of its own before the job takes it.
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cfg.coopDir.c_str(), &si, &pi)) {
        Fail(StartError(GetLastError()));
        return false;
    }
    // the server dies together with the game
    AssignProcessToJobObject(g_serverJob, pi.hProcess);
    CloseHandle(pi.hThread);
    g_serverProcess = pi.hProcess;
    LOGI("Local server started (pid %lu)", pi.dwProcessId);
    return true;
}

void StopServerProcess() {
    if (g_serverProcess) {
        TerminateProcess(g_serverProcess, 0);
        CloseHandle(g_serverProcess);
        g_serverProcess = nullptr;
    }
}

bool ServerProcessAlive() {
    return g_serverProcess && WaitForSingleObject(g_serverProcess, 0) == WAIT_TIMEOUT;
}

void UploadWorld(const std::vector<uint8_t>& data, const std::string& label) {
    Writer b(C_WORLD_BEGIN);
    b.U32((uint32_t)data.size());
    b.U32(Crc32(data.data(), data.size()));
    b.Str(label, 128);
    Net().Send(CH_BULK, b);
    for (size_t off = 0; off < data.size(); off += kWorldChunk) {
        size_t n = std::min(kWorldChunk, data.size() - off);
        Writer c(C_WORLD_CHUNK);
        c.U32((uint32_t)off);
        c.Bytes(data.data() + off, n);
        Net().Send(CH_BULK, c);
    }
    Writer e(C_WORLD_END);
    Net().Send(CH_BULK, e);
    LOGI("World snapshot uploaded (%u bytes)", (unsigned)data.size());
}

void OnOnline() {
    auto& net = Net();
    if (net.IsHost()) {
        if (g_hostSlot > 0 && !core::IsInGame()) {
            ui::TheChat().AddSystem(Format(TR("Загружаем сохранение из слота %d...", "Loading the save from slot %d..."), g_hostSlot));
            if (!core::LoadSaveFile(core::SlotPath(g_hostSlot))) {
                Fail(Format(TR("Не удалось загрузить сохранение из слота %d.", "Could not load the save from slot %d."), g_hostSlot));
                return;
            }
            SetPhase(Phase::LoadingWorld);
        } else if (g_hostSlot < 0 && !core::IsInGame()) {
            ui::TheChat().AddSystem(TR("Сервер создан. Начинаем новую игру...", "The server is up. Starting a new game..."));
            core::StartNewGame();
            SetPhase(Phase::LoadingWorld);
        } else {
            SetPhase(core::IsInGame() ? Phase::Playing : Phase::Idle);
            if (!core::IsInGame())
                ui::TheChat().AddSystem(TR("Сервер создан. Начните новую игру или загрузите сохранение.",
                                           "The server is up. Start a new game or load a save."));
            else g_worldUploadPending = true;  // give the server a fresh world
        }
    } else {
        g_follower = g_prologueWait = g_worldNone = false;  // a new session: the server says again whether the host is in a prologue
        // what was waiting from a session that dropped is older than the world asked for now (it was loaded over it)
        g_pendingWorld.clear();
        g_hasSpot = false;
        ui::TheChat().AddSystem(TR("Запрашиваем мир у хоста...", "Asking the host for the world..."));
        Writer w(C_WORLD_REQUEST);
        net.Send(CH_BULK, w);
        SetPhase(Phase::WaitingWorld);
    }
}

}  // namespace

void SaveConnectSettings() {
    auto& cfg = Cfg();
    Ini ini;
    ini.Load(cfg.iniPath);
    ini.Set("player", "name", cfg.nickname);
    ini.Set("player", "language", cfg.english ? "en" : "ru");
    ini.Set("connect", "mode", cfg.wantsHost ? "host" : "join");  // the menu opens on the same tab next time
    ini.Set("connect", "host", cfg.host);
    ini.SetInt("connect", "port", cfg.port);
    ini.Set("connect", "password", cfg.password);
    ini.SetInt("connect", "friendly_fire", cfg.friendlyFire);
    ini.SetInt("connect", "partner_teleport", cfg.partnerTeleport);
    ini.Save(cfg.iniPath);
}

Phase GetPhase() { return g_phase; }
const std::string& LastError() { return g_error; }

std::wstring PartnerWorldPath() { return Cfg().coopDir + L"world\\partner_world.b"; }

std::string StatusText() {
    auto& net = Net();
    switch (g_phase) {
        case Phase::Idle:
            if (net.Online()) return net.IsHost() ? TR("Сервер работает", "Server running") : TR("Подключено", "Connected");
            return g_error.empty() ? TR("Нет подключения", "Not connected") : g_error;
        case Phase::StartingServer:
            return TR("Запускаем сервер...", "Starting the server...");
        case Phase::Connecting: {
            if (g_isHosting) return TR("Запускаем сервер...", "Starting the server...");
            nat::Endpoint ep;
            uint8_t key = 0;
            if (nat::ParseInviteCode(g_hostAddr, ep, key)) return TR("Подключаемся по коду...", "Connecting by code...");
            return Format(TR("Подключаемся к %s:%u...", "Connecting to %s:%u..."), g_hostAddr.c_str(), g_port);
        }
        case Phase::WaitingWorld: {
            float p = net.WorldProgress();
            if (p >= 0) return Format(TR("Получаем мир хоста: %d%%", "Receiving the host's world: %d%%"), (int)(p * 100));
            return g_prologueWait ? TR("Хост проходит пролог", "The host plays the prologue")
                                  : TR("Ждём мир от хоста...", "Waiting for the host's world...");
        }
        case Phase::LoadingWorld:
            return TR("Загружаем мир...", "Loading the world...");
        case Phase::Playing:
            return net.IsHost() ? TR("Вы хост", "You are the host") : TR("Вы в мире хоста", "You are in the host's world");
    }
    return "";
}

// The title of the mission a save was made after, by its GXT key (CStats::LastMissionPassedName), as the English game
// names it: a translated game's own save name is in its font's codes, which our fonts cannot show, and the list showed
// no name at all (Kruptor asked for it on October 25). The stats are a save's 17th block (CGenericGameStorage::GenericSave),
// the key after their floats, ints and kills by ped type (CStats::Save: 0x148, 0x37C and 0x80 bytes).
const std::pair<const char*, const char*> kMissionTitles[] = {
    {"ITBEG", "In the Beginning"}, {"INTRO_1", "Big Smoke"}, {"INTRO_2", "Ryder"}, {"SWEET_1", "Tagging up Turf"},
    {"SWEET1B", "Cleaning the Hood"}, {"SWEET_3", "Drive-Thru"}, {"SWEET_2", "Nines and AK's"},
    {"SWEET_4", "Drive-By"}, {"SWEET_5", "Sweet's Girl"}, {"SWEET_6", "Cesar Vialpando"}, {"SWEET_7", "Los Sepulcros"},
    {"CRASH_2", "Doberman"}, {"CRASH_1", "Burning Desire"}, {"CRASH_3", "Gray Imports"}, {"RYDER_1", "Home Invasion"},
    {"RYDER_3", "Catalyst"}, {"RYDER_2", "Robbing Uncle Sam"}, {"SMOKE_1", "OG Loc"}, {"SMOKE_2", "Running Dog"},
    {"SMOKE_3", "Wrong Side of the Tracks"}, {"SMOKE_4", "Just Business"}, {"STRAP_1", "Life's a Beach"},
    {"STRAP_2", "Madd Dogg's Rhymes"}, {"STRAP_3", "Management Issues"}, {"STRAP_4", "House Party"},
    {"CESAR_1", "High Stakes, Low-rider"}, {"LA1FIN1", "Reuniting the Families"}, {"LA1FIN2", "The Green Sabre"},
    {"BCRASH1", "Badlands"}, {"BCESAR4", "Wu Zi Mu"}, {"BCES4_2", "Farewell, my love..."},
    {"TRAILER", "King in Exile"}, {"TRUTH_1", "Body Harvest"}, {"TRUTH_2", "Are you going to San Fierro?"},
    {"GAR_1", "Wear Flowers in your Hair"}, {"GAR_2", "Deconstruction"}, {"SCRA_1", "555 WE TIP"},
    {"SCRA_2", "Snail Trail"}, {"WUZI_1", "Mountain Cloud Boys"}, {"FAR_4", "Ran Fa Li"}, {"FAR_5", "Lure"},
    {"WUZI_2", "Amphibious Assault"}, {"WUZI_4", "The Da Nang Thang"}, {"SYND_1", "Photo Opportunity"},
    {"SYND_2", "Jizzy"}, {"SYND_3", "Outrider"}, {"SYND_4", "Ice Cold Killa"}, {"SYND_6", "Toreno's Last Flight"},
    {"SYND_7", "Yay Ka-Boom-Boom"}, {"SYND_5", "Pier 69"}, {"FAR_2", "T-Bone Mendez"}, {"FAR_3", "Mike Toreno"},
    {"STEAL_1", "Zeroing In"}, {"STEAL_2", "Test Drive"}, {"STEAL_4", "Customs Fast Track"},
    {"STEAL_5", "Puncture Wounds"}, {"FAR_1", "Back to School"}, {"ZERO_1", "Air Raid"}, {"ZERO_2", "Supply Lines..."},
    {"ZERO_4", "New Model Army"}, {"BEEFY", "Beefy Baron"}, {"DESERT1", "Monster"}, {"DESERT2", "Highjack"},
    {"DESERT3", "Interdiction"}, {"DESERT4", "Verdant Meadows"}, {"DESERT6", "N.O.E."}, {"DESERT9", "Stowaway"},
    {"DESERT8", "Black Project"}, {"DESER10", "Green Goo"}, {"DESERT5", "Learning to Fly"},
    {"CASINO1", "Fender Ketchup"}, {"CASEEN2", "Explosive Situation"}, {"CASINO3", "You've had your Chips"},
    {"CASINO7", "Fish in a Barrel"}, {"CASINO4", "Don Peyote"}, {"CASINO5", "Intensive Care"},
    {"CASINO6", "The Meat Business"}, {"CASINO9", "Freefall"}, {"CASIN10", "Saint Mark's Bistro"},
    {"VCRASH1", "Misappropriation"}, {"VCRASH2", "High Noon"}, {"DOC_2", "Madd Dogg"},
    {"HEIST_1", "Architectural Espionage"}, {"HEIST_3", "Key to her Heart"}, {"HEIST_2", "Dam and Blast"},
    {"HEIST_4", "Cop Wheels"}, {"HEIST_5", "Up, Up and Away!"}, {"HEIST_9", "Breaking the Bank at Caligula's"},
    {"MAN_1", "A Home in the Hills"}, {"MAN_2", "Vertical Bird"}, {"MAN_3", "Home Coming"},
    {"MAN_5", "Cut Throat Business"}, {"GROVE_1", "Beat Down on B Dup"}, {"GROVE_2", "Grove 4 Life"},
    {"RIOT_1", "Riot"}, {"RIOT_2", "Los Desperados"}, {"RIOT_4", "End of the Line"}, {"TRUCK", "Trucking"},
    {"QUARRY", "Quarry"}, {"TAXI_M", "Taxi Driver"}, {"AMBUL_M", "Paramedic"}, {"FIRE_M", "Firefighter"},
    {"COP_M", "Vigilante"}, {"BURGLAR", "Burglar"}, {"PIMP", "Pimping"}, {"STAD_01", "Dirt Track"},
    {"STAD_02", "Kickstart"}, {"STAD_03", "8-Track"}, {"STAD_04", "Blood Ring"}, {"TRACE_1", "Race Tournament"},
    {"BOTTY", "Beat the Cock!"}, {"COUR1", "Courier"}, {"MOUNTN", "The Chiliad Challenge"}, {"BMX", "BMX"},
    {"NRG500", "NRG-500"},
};
const char* TitleInSave(const std::vector<uint8_t>& data) {
    static const char kTag[] = "BLOCK";
    size_t at = 0;
    for (int block = 0; block < 17; ++block) {
        auto it = std::search(data.begin() + (ptrdiff_t)(block ? at + 5 : 0), data.end(), kTag, kTag + 5);
        if (it == data.end()) return nullptr;
        at = (size_t)(it - data.begin());
    }
    size_t key = at + 5 + 0x148 + 0x37C + 0x80;
    if (key + 8 > data.size()) return nullptr;
    std::string name((const char*)data.data() + key, strnlen((const char*)data.data() + key, 8));
    for (auto& [k, title] : kMissionTitles)
        if (name == k) return title;
    return nullptr;
}

std::vector<SaveSlotInfo> ListSaveSlots() {
    std::vector<SaveSlotInfo> out;
    for (int s = 1; s <= 8; ++s) {
        SaveSlotInfo info;
        info.slot = s;
        std::vector<uint8_t> data;
        std::wstring path = core::SlotPath(s);
        if (FileExists(path) && ReadFileBytes(path, data) && data.size() > 0x200 && memcmp(data.data(), "BLOCK", 5) == 0) {
            info.exists = true;
            // CSimpleVariablesSaveStructure follows the "BLOCK" tag; the save name is GXT-encoded,
            // so only show it when it is plain ASCII.
            const uint8_t* sv = data.data() + 5;
            std::string name;
            for (int i = 0; i < 100 && sv[4 + i]; ++i) name += (char)sv[4 + i];
            bool ascii = !name.empty();
            for (char c : name)
                if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) ascii = false;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad);
            SYSTEMTIME st{};
            FILETIME local;
            FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &st);
            info.title = Format(TR("Слот %d · %02d.%02d.%04d, %02d:%02d", "Slot %d · %02d.%02d.%04d, %02d:%02d"), s, st.wDay,
                                st.wMonth, st.wYear, st.wHour, st.wMinute);
            if (ascii) info.title += " · " + name;
            else if (const char* title = TitleInSave(data)) info.title += std::string(" · ") + title;
        } else {
            info.title = Format(TR("Слот %d · пусто", "Slot %d · empty"), s);
        }
        out.push_back(info);
    }
    return out;
}

void Host(int slot, uint16_t port, const std::string& password) {
    auto& cfg = Cfg();
    g_error.clear();
    g_isHosting = true;
    g_hostSlot = slot;
    g_port = port;
    g_password = password;
    cfg.wantsHost = true;
    cfg.port = port;
    cfg.password = password;
    SaveConnectSettings();
    std::wstring world;
    if (slot > 0) {
        world = core::SlotPath(slot);
        if (!FileExists(world)) {
            Fail(Format(TR("Слот %d пуст.", "Slot %d is empty."), slot));
            return;
        }
    } else if (slot == 0 && core::IsPlayerReady()) {
        std::vector<uint8_t> snap;
        if (core::SaveWorldSnapshot(snap)) world = Cfg().coopDir + L"world\\host_snapshot.b";
    }
    StopServerProcess();
    if (!StartServerProcess(world, slot < 0)) return;
    SetPhase(Phase::StartingServer);
    g_connectAttempts = 0;
}

void Join(const std::string& hostArg, uint16_t port, const std::string& password) {
    auto& cfg = Cfg();
    // "file:<path>": the invite code written by a server (dedicated servers, test setups)
    std::string host = hostArg;
    if (host.rfind("file:", 0) == 0) {
        std::vector<uint8_t> data;
        std::wstring path = Utf8ToWide(host.substr(5));
        if (path.size() < 2 || path[1] != L':') path = cfg.coopDir + path;
        if (ReadFileBytes(path, data)) host = Trim(std::string(data.begin(), data.end()));
    }
    g_error.clear();
    g_isHosting = false;
    g_hostAddr = host;
    g_port = port;
    g_password = password;
    cfg.host = hostArg;  // keep "file:" so the next start reads a fresh code
    cfg.wantsHost = false;
    cfg.port = port;
    cfg.password = password;
    SaveConnectSettings();
    if (core::IsInGame()) {
        ui::TheChat().AddSystem(TR("Подключаемся. Вместо текущей игры загрузится мир хоста.",
                                   "Connecting. The host's world will load instead of your current game."));
    }
    Net().Connect(host, port, cfg.nickname, password, false);
    SetPhase(Phase::Connecting);
    g_connectAttempts = 1;
}

void Leave() {
    SaveProfileNow();  // saved every 30 s otherwise: up to that much of the partner's progress was lost on leaving
    Net().Disconnect();
    StopServerProcess();
    g_isHosting = g_follower = g_prologueWait = g_worldNone = g_progressUpload = g_hasSpot = false;
    g_pendingWorld.clear();
    SetPhase(Phase::Idle);
    ui::TheChat().AddSystem(TR("Вы вышли из совместной игры.", "You left the co-op game."));
}

namespace {
void LoadHostWorld(const std::vector<uint8_t>& data);

// a reload must not cut into anything: on foot, alive, no cutscene, no mission here or at the host's
int WantedLevel() {
    void* wanted = game::Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return wanted ? game::Field<int32_t>(wanted, 0x2C) : 0;  // CWanted::m_nWantedLevel
}

// Casino tables, the barber's chair, a meal, the gym's machines, basketball and the dance raise no mission flag: the
// world reloaded under them. The player's own control and its plain on-foot task (type 0) say it does nothing of that.
// What keeps the host's world from loading now, or null. Stars wait (the host's world wipes the stars of a chase) unless
// anyStars: the forbidden territories of an old world give 4 that never fade.
const char* ReloadBlocker(bool anyStars) {
    game::CPlayerPed* me = game::FindPlayerPed(0);
    if (!core::IsPlayerReady() || !me) return "no player";
    if (me->InVehicle()) return "in a vehicle";
    if (me->Health() <= 0.0f) return "dead";
    if (game::Ref<uint8_t>(0xB5F851)) return "a cutscene";
    if (core::IsOnMission()) return "a mission here";
    if (HostMissionRecent(5000)) return "the host's mission";
    if (game::GetPad(0)->DisablePlayerControls != 0) return "no control";
    if (ActivePrimaryTaskType(me) != 0) return "busy";
    if (!anyStars && WantedLevel() != 0) return "wanted";
    return nullptr;
}

void SaveSpot() {
    game::CPlayerPed* me = game::FindPlayerPed(0);
    game::CVector p = me->GetPosition();
    g_spot = {};
    g_spot.x = p.x, g_spot.y = p.y, g_spot.z = p.z;
    g_spot.heading = me->CurrentRotation();
    g_spot.area = me->AreaCode();
    g_spot.health = me->Health();
    g_spot.armour = me->Armour();
    g_spot.activeWeapon = me->ActiveWeapon().type;
    for (int i = 0; i < 13; ++i) g_spot.weapons[i] = me->Weapons()[i].type, g_spot.ammo[i] = me->Weapons()[i].ammoTotal;
    // the car comes back only if it is free, whole and this game's: a wreck would burn out again next to the partner,
    // and a car someone sits in, or the other player's, would stand twice
    game::CVehicle* car = Players::Instance().LastCar();
    NetVehicle* nv = car ? Vehicles::Instance().Find(car) : nullptr;
    bool empty = car && !car->Driver() && car->NumPassengers() == 0;
    if (empty && car->Status() != 5 && car->Health() >= 250.0f && !(nv && Vehicles::Instance().IsMirror(*nv)) &&
        (car->GetPosition() - p).Length() < 60.0f) {
        game::CVector c = car->GetPosition();
        game::CMatrix* m = car->Matrix();
        g_spot.carModel = car->ModelIndex();
        g_spot.carX = c.x, g_spot.carY = c.y, g_spot.carZ = c.z;
        g_spot.carHeading = m ? std::atan2(-m->forward.x, m->forward.y) : 0.0f;
        g_spot.carHealth = car->Health();
        memcpy(g_spot.carColors, car->Colors(), 4);
    }
    g_hasSpot = true;
}
}  // namespace

void Update() {
    auto& net = Net();
    // report our status to the server (used for world hand-over and the player list)
    static int lastStatus = -1;
    int status = core::IsPlayerReady() ? STATUS_INGAME : (game::GameState() == game::GAME_STATE_IDLE ? STATUS_LOADING : STATUS_MENU);
    if (net.Online() && status != lastStatus) {
        Writer w(C_STATUS);
        w.U8((uint8_t)status);
        net.Send(CH_RELIABLE, w);
        lastStatus = status;
    }
    if (!net.Online()) lastStatus = -1;
    static bool wasOnline = false;
    bool online = net.Online();
    if (online && !wasOnline) OnOnline();
    if (!online && wasOnline && g_phase != Phase::Idle) {
        SetPhase(Phase::Idle);
        g_error = net.FailReason();
    }
    wasOnline = online;
    // the screen went black for a reload of the host's world that is off now (the session dropped): back from black
    if (g_reloadFadeMs && (g_pendingWorld.empty() || g_phase != Phase::Playing) && core::IsPlayerReady()) {
        g_reloadFadeMs = 0;
        game::script::Cmd(0x016A).I(300).I(1).Run();  // DO_FADE in
    }

    switch (g_phase) {
        case Phase::StartingServer:
            if (!ServerProcessAlive()) {
                Fail(TR("Сервер не запустился: порт уже занят, или его остановил антивирус. Подробности в файле "
                        "coop\\server-data\\server.log (если игра стоит в Program Files, то в %LOCALAPPDATA%\\SA-COOP\\server-data), "
                        "а если он пустой, добавьте папку игры в исключения антивируса.",
                        "The server did not start: the port is taken, or an antivirus stopped it. See coop\\server-data\\server.log "
                        "(%LOCALAPPDATA%\\SA-COOP\\server-data for a game in Program Files), and if it is empty, add the game "
                        "folder to the antivirus exclusions."));
                break;
            }
            if (NowMs() - g_phaseTime > 400) {
                net.Connect("127.0.0.1", g_port, Cfg().nickname, g_password, true);
                SetPhase(Phase::Connecting);
                g_connectAttempts = 1;
            }
            break;
        case Phase::Connecting:
            if (net.GetState() == Session::State::Failed) {
                if (g_isHosting && g_connectAttempts < 5 && ServerProcessAlive()) {
                    ++g_connectAttempts;
                    net.Connect("127.0.0.1", g_port, Cfg().nickname, g_password, true);
                } else {
                    Fail(net.FailReason());
                }
            }
            break;
        case Phase::WaitingWorld:
            if (NowMs() - g_phaseTime > 120000 && net.WorldProgress() < 0 && !g_prologueWait && !g_worldNone)
                Fail(TR("Хост так и не прислал мир.", "The host never sent the world."));
            break;
        case Phase::LoadingWorld:
            break;
        case Phase::Playing:
            if (!g_pendingWorld.empty() && !net.IsHost()) {
                // The load holds the picture still for a second or so: players took it for a lag after every passed mission,
                // and behind a fade for a black screen out of nowhere. It waits for the next mission's start point, where the
                // screen goes dark for the mission anyway and the host's start waits for it, or ten minutes with no mission.
                const char* why = ReloadBlocker(NowMs() - g_pendingSince > 60000);
                if (!why && !AtMissionMarker() && NowMs() - g_pendingSince < 600000) why = "the next mission's start point";
                if (why) {
                    if (g_reloadFadeMs) g_reloadFadeMs = 0, game::script::Cmd(0x016A).I(300).I(1).Run();  // DO_FADE: not now
                    if (NowMs() - g_pendingToldMs > 30000) {
                        g_pendingToldMs = NowMs();
                        LOGI("The host's world waits: %s", why);
                    }
                    break;
                }
                // into black first: the load holds the picture still for a second or so, which players took for a lag
                if (!g_reloadFadeMs) {
                    g_reloadFadeMs = NowMs();
                    game::script::Cmd(0x016A).I(300).I(0).Run();  // DO_FADE out
                    break;
                }
                if (NowMs() - g_reloadFadeMs < 350) break;
                g_reloadFadeMs = 0;
                std::vector<uint8_t> data = std::move(g_pendingWorld);
                g_pendingWorld.clear();
                SaveSpot();
                SaveProfileNow();  // money, skills and look as they are now go on top of the new world
                ui::TheChat().AddSystem(TR("Хост продвинулся по сюжету. Загружаем его мир, вы останетесь на своём месте...",
                                           "The host moved on in the story. Loading their world, you stay where you are..."));
                LoadHostWorld(data);
                if (g_phase != Phase::LoadingWorld) game::script::Cmd(0x016A).I(300).I(1).Run();  // it failed: back from black
            }
            break;
        default:
            break;
    }

    // host: deliver a fresh snapshot of the world when someone needs it. A new game's prologue is a mission and cannot
    // be saved: the host plays it alone, partners wait. Once it is over (CJ gets on the bike, still unsaveable) they
    // join in a new game of their own, and the first save goes to them.
    if (online && net.IsHost() && core::IsPlayerReady()) {
        bool prologue = core::InPrologue();
        if (prologue != g_prologue) {
            g_prologue = prologue;
            Writer w(prologue ? C_WORLD_NEWGAME : C_WORLD_FOLLOW);  // NEWGAME: the server's world is an older game's
            net.Send(CH_BULK, w);
            if (!prologue) g_worldUploadPending = g_afterPrologue = true;
            LOGI(prologue ? "New game: its prologue runs, partners wait for its end" : "The prologue is over: partners join");
        }
        // a new game launches its prologue in the first frames: no snapshot before it does
        if (g_worldUploadPending && !prologue && NowMs() - g_gameStartedAt > 5000 && NowMs() - g_snapshotTriedAt > 1000) {
            g_snapshotTriedAt = NowMs();
            std::vector<uint8_t> snap;
            if (core::SaveWorldSnapshot(snap)) {
                UploadWorld(snap, g_progressUpload ? "progress" : "host snapshot");  // the server passes "progress" on
                g_progressUpload = false;
                g_worldUploadPending = g_keepSent = g_afterPrologue = false;
            } else if (g_afterPrologue && !g_keepSent) {  // the prologue's bike: who asks now joins in a new game
                Writer w(C_WORLD_FOLLOW);
                net.Send(CH_BULK, w);
                g_keepSent = true;
            } else if (!g_keepSent && (core::IsOnMission() || game::FindPlayerPed(0)->InVehicle())) {
                // no snapshot during a mission or in a vehicle: the stored world now, a fresh one once possible
                Writer w(C_WORLD_KEEP);
                net.Send(CH_BULK, w);
                g_keepSent = true;
            }
        }
    }
}

void RequestFreshWorld() {
    g_worldUploadPending = true;
    g_keepSent = false;  // a new request gets its own answer
}

// The server keeps us waiting for the host's game (it gives no stored world while the host is in the menu): no time
// limit then, as in the prologue. The partner gave up after 2 minutes, "the host never sent the world".
void OnWorldNone() {
    if (Net().IsHost()) return;
    g_worldNone = true;
    g_phaseTime = NowMs();
}

void OnHostInPrologue() {
    if (Net().IsHost() || g_prologueWait) return;
    g_prologueWait = true;
    g_pendingWorld.clear();  // the old game's progress is no world to load any more
    g_hasSpot = false;
    g_phaseTime = NowMs();
    ui::TheChat().AddSystem(TR("Хост проходит пролог. Вы появитесь рядом с ним, когда CJ сядет на велосипед.",
                               "The host plays the prologue. You will appear next to them when CJ gets on the bike."));
}

void OnNewGameFromHost() {
    if (Net().IsHost()) return;
    ui::TheChat().AddSystem(TR("Хост прошёл пролог. Загружаемся, вы появитесь рядом с ним...",
                               "The host finished the prologue. Loading, you will appear next to them..."));
    g_follower = true;
    g_prologueWait = false;
    core::StartNewGame();
    SetPhase(Phase::LoadingWorld);
}

bool IsFollower() { return g_follower && Net().Online() && !Net().IsHost(); }

bool InHostWorld() { return Net().Online() && g_phase == Phase::Playing && !g_prologueWait; }
bool HostWorldWaiting() { return !g_pendingWorld.empty(); }
bool HostWorldLoading() { return g_reloadFadeMs != 0 || g_phase == Phase::LoadingWorld; }

bool InSharedWorld() { return Net().Online() && (Net().IsHost() || InHostWorld()); }

// The story moved on in the host's game (a mission passed, the call that unlocks something after it): a new
// snapshot goes to the partners already playing, who load it, staying where they are.
void WorldProgressed() {
    if (!Net().Online() || !Net().IsHost()) return;
    g_worldUploadPending = g_progressUpload = true;
    LOGI("Story progress: the partners get the world again");
}

bool TakeSavedSpot(SavedSpot& out) {
    if (!g_hasSpot) return false;
    out = g_spot;
    g_hasSpot = false;
    return true;
}


namespace {
#pragma pack(push, 1)
struct ScmSizes {  // the story's sizes in main.scm's header and in a save (CTheScripts::Save), in this order
    uint32_t main, largestMission;
    uint16_t missions, exclusiveMissions;
    uint32_t largestMissionLocals;
};
#pragma pack(pop)

// The game puts a save's running scripts at their places in main.scm and never checks that the save comes from the same
// main.scm (CTheScripts::Load reads the sizes and drops them): a save of another main.scm crashes it in the first
// scripts it runs. The save must have this game's sizes: those of its loaded main.scm, or of one it may load.
bool SameMainScm(const std::vector<uint8_t>& save) {
    auto inSave = [&](const ScmSizes& s) {
        auto b = (const uint8_t*)&s;
        return std::search(save.begin(), save.end(), b, b + sizeof s) != save.end();
    };
    if (game::Ref<uint32_t>(0xA444C4))  // CTheScripts::MainScriptSize: the game has loaded its main.scm
        return inSave({game::Ref<uint32_t>(0xA444C4), game::Ref<uint32_t>(0xA444C0), game::Ref<uint16_t>(0xA444BC),
                       game::Ref<uint16_t>(0xA444B8), game::Ref<uint32_t>(0xA444B4)});
    std::vector<std::wstring> files = {Cfg().gameDir + L"data\\script\\main.scm"};
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(Cfg().gameDir + L"modloader", ec), end; !ec && it != end; it.increment(ec))
        if (!_wcsicmp(it->path().filename().c_str(), L"main.scm")) files.push_back(it->path().wstring());
    bool known = false;
    for (const auto& f : files) {
        std::vector<uint8_t> scm;
        size_t pos = 0;  // main.scm opens with GOTOs over its variables and its models, to the missions' segment
        bool ok = ReadFileBytes(f, scm);
        for (int i = 0; i < 2 && ok; ++i) {
            ok = pos + 7 <= scm.size() && !memcmp(&scm[pos], "\x02\x00\x01", 3);
            if (ok) pos = *(const uint32_t*)&scm[pos + 3];
        }
        ScmSizes s;
        if (!ok || pos + 8 + sizeof s > scm.size()) continue;
        memcpy(&s, &scm[pos + 8], sizeof s);  // past the segment's GOTO and its number
        if (inSave(s)) return true;
        known = true;
    }
    return !known;
}

void LoadHostWorld(const std::vector<uint8_t>& data) {
    if (!SameMainScm(data)) {
        Fail(TR("Мир хоста не загрузится, потому что ваш файл data\\script\\main.scm не такой, как у хоста. Нужны "
                "одинаковые версия игры и моды, которые меняют сюжет.",
                "The host's world cannot load because your data\\script\\main.scm differs from the host's. You both "
                "need the same game version and the same mods that change the story."));
        return;
    }
    EnsureDirectory(Cfg().coopDir + L"world");
    std::wstring path = PartnerWorldPath();
    // An antivirus holding the file a moment, a folder with no write access (a game in Program Files, Windows' controlled
    // folder access): a few tries, then the user's own folder.
    bool written = false;
    for (int i = 0; i < 5 && !written; ++i) {
        written = WriteFileBytes(path, data.data(), data.size());
        if (!written) {
            LOGW("The world save did not go to %s (error %lu)", WideToUtf8(path).c_str(), GetLastError());
            Sleep(100);
        }
    }
    wchar_t local[MAX_PATH];
    if (DWORD n = written ? 0 : GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH); n && n < MAX_PATH) {
        std::wstring dir = std::wstring(local) + L"\\SA-COOP\\world\\";
        EnsureDirectory(dir);
        path = dir + L"partner_world.b";
        written = WriteFileBytes(path, data.data(), data.size());
        LOGI("The world save goes to %s instead: %s", WideToUtf8(path).c_str(), written ? "written" : "failed too");
    }
    if (!written) {
        Fail(TR("Не удалось записать сохранение мира на диск.", "Could not write the world save to disk."));
        return;
    }
    ui::TheChat().AddSystem(g_follower ? TR("Пролог позади. Загружаем мир хоста...", "The prologue is over. Loading the host's world...")
                                       : TR("Мир хоста получен. Загружаем...", "Got the host's world. Loading..."));
    g_follower = g_prologueWait = false;
    if (!core::LoadSaveFile(path)) {
        Fail(TR("Мир хоста не загрузился. Скорее всего, у вас разные версии игры или модов.",
                "The host's world did not load. Most likely your game or mod versions differ."));
        return;
    }
    SetPhase(Phase::LoadingWorld);
}
}  // namespace

void OnWorldReceived(const std::vector<uint8_t>& data) {
    if (Net().IsHost()) return;
    g_worldNone = false;
    if (g_phase == Phase::Playing && core::IsPlayerReady() && !g_follower) {  // the host's progress: at the next start point
        g_pendingWorld = data;
        g_pendingSince = g_pendingToldMs = NowMs();
        LOGI("The host's world after its progress came: kept for the next mission's start point");
        return;
    }
    LoadHostWorld(data);
}

void OnGameStarted() {
    g_gameStartedAt = NowMs();
    g_prologue = g_afterPrologue = false;  // another world: its own story
    if (g_phase == Phase::LoadingWorld || (Net().Online() && g_phase != Phase::Playing)) SetPhase(Phase::Playing);
    // the host's save carries the host's wanted level: the partner starts without it
    if (Net().Online() && !Net().IsHost()) game::script::Cmd(0x0110).I(0).Run();  // CLEAR_WANTED_LEVEL
    if (Net().Online() && Net().IsHost()) g_worldUploadPending = true;
}

void OnGameShutdown() {
    if (g_phase == Phase::Playing) SetPhase(Phase::LoadingWorld);
}

}  // namespace coop::flow
