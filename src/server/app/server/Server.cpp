#include "app/server/Server.h"

#include <windows.h>

#include <algorithm>
#include <thread>

#include "common/api/net/Net.h"
#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop::server {

using namespace proto;

static constexpr uint32_t kColorServer = 0xFFC864FF;  // RGBA, orange-ish
static constexpr uint32_t kColorInfo = 0xA0C8FFFF;
static constexpr uint32_t kColorError = 0xFF6464FF;

bool Server::Start(const Config& cfg) {
    m_cfg = cfg;
    if (m_cfg.maxPlayers < 1) m_cfg.maxPlayers = 1;
    if (m_cfg.maxPlayers > kGamePlayers) {
        LOGW("В max_players указано %d, но игра поддерживает только %d игроков. Мест будет %d.", m_cfg.maxPlayers, kGamePlayers,
             kGamePlayers);
        m_cfg.maxPlayers = kGamePlayers;
    }
    EnsureDirectory(m_cfg.dataDir + L"profiles");
    EnsureDirectory(m_cfg.dataDir + L"world");

    if (!net::Init()) {
        LOGE("enet_initialize failed");
        return false;
    }
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = m_cfg.port;
    // a couple of spare peers so that "server full" can be reported politely
    m_host = enet_host_create(&addr, (size_t)m_cfg.maxPlayers + 2, CH_COUNT, 0, 0);
    if (!m_host) {
        LOGE("Не удалось открыть UDP-порт %u (занят другим процессом?)", m_cfg.port);
        return false;
    }
    enet_host_compress_with_range_coder(m_host);
    m_host->mtu = net::kMtu;
    if (m_cfg.internet) m_internet.Start(m_host, m_cfg.port, m_cfg.turn);

    // initial world: explicit file from the launcher, otherwise the last uploaded one
    if (!m_cfg.worldFile.empty()) {
        if (LoadWorld(m_cfg.worldFile, WideToUtf8(m_cfg.worldFile)))
            WriteFileBytes(WorldPath(), m_world.data(), m_world.size(), false);  // the network thread: no wait for the disk
    } else if (m_cfg.newGame) {
        DeleteFileW(WorldPath().c_str());  // an older game's world: partners follow the new one instead
    } else {
        LoadWorld(WorldPath(), "world.b");
    }

    for (int i = 0; i < kMaxPlayers; ++i) m_players[i].id = (uint8_t)i;
    LOGI("Сервер \"%s\" запущен на порту %u, мест: %d%s", m_cfg.name.c_str(), m_cfg.port, m_cfg.maxPlayers,
         m_cfg.password.empty() ? "" : ", с паролем");
    m_running = true;
    return true;
}

bool Server::LoadWorld(const std::wstring& path, const std::string& label) {
    std::vector<uint8_t> data;
    if (!ReadFileBytes(path, data) || data.size() < 1024) return false;
    m_world = std::move(data);
    m_worldLabel = label;
    LOGI("Сохранение мира загружено: %s (%u байт, crc %08X)", label.c_str(), (unsigned)m_world.size(),
         Crc32(m_world.data(), m_world.size()));
    return true;
}

void Server::Run() {
    while (m_running) {
        Service();
        m_internet.Service();
        if (std::string code; m_internet.TakeNewCode(code)) {
            SendInvite(nullptr);
            WriteFileBytes(m_cfg.dataDir + L"invite.txt", code.data(), code.size());  // for dedicated servers
        }
        std::vector<std::string> cmds;
        {
            std::lock_guard<std::mutex> lock(m_cmdMutex);
            cmds.swap(m_cmdQueue);
        }
        for (auto& c : cmds) HandleCommand(nullptr, c);
        if (m_worldWantedTime && NowMs() - m_worldWantedTime > 10000) {
            LOGW("Хост не прислал свежий мир за 10 с. Отправляю сохранённый");
            FlushWorldRequests(false);
        }
        if (NowMs() - m_lastPings > 2000) {
            m_lastPings = NowMs();
            SendPings();
            // drop peers that never sent HELLO
            for (auto& p : m_players)
                if (p.used && !p.joined && NowMs() - p.connectTime > 10000)
                    Kick(p, "клиент не прислал приветствие", "the client sent no greeting");
        }
    }
    for (auto& p : m_players)
        if (p.used && p.peer) enet_peer_disconnect(p.peer, 0);
    enet_host_flush(m_host);
    m_internet.Stop();
    enet_host_destroy(m_host);
    m_host = nullptr;
    net::Shutdown();
    WaitProfileWrites();  // the last profiles reach the disk before the process ends
    LOGI("Сервер остановлен");
}

void Server::QueueConsoleCommand(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_cmdQueue.push_back(line);
}

void Server::Service() {
    ENetEvent ev;
    int rc = enet_host_service(m_host, &ev, 5);
    while (rc > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                OnConnect(ev.peer);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                // clients send data=1 on a graceful quit; anything else is a lost connection
                OnDisconnect(ev.peer, ev.data != 1);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                OnPacket(ev.peer, ev.channelID, ev.packet->data, ev.packet->dataLength, ev.packet->flags);
                enet_packet_destroy(ev.packet);
                break;
            default:
                break;
        }
        rc = enet_host_check_events(m_host, &ev);
    }
}

void Server::OnConnect(ENetPeer* peer) {
    char ip[64];
    enet_address_get_host_ip(&peer->address, ip, sizeof(ip));
    Player* slot = nullptr;
    // every slot: a player coming back after a crash finds its old connection still holding one (it times out in 10 to
    // 20 s); the number of players is checked at the hello, where that old one is let go
    for (int i = 0; i < kMaxPlayers; ++i)
        if (!m_players[i].used) {
            slot = &m_players[i];
            break;
        }
    if (!slot) {
        LOGW("Отказ %s: сервер заполнен", ip);
        Writer w(S_REJECT);
        w.Str("на сервере нет свободных мест");
        w.Str("the server is full");
        net::Send(peer, CH_RELIABLE, w);
        enet_peer_disconnect_later(peer, 0);
        return;
    }
    uint8_t id = slot->id;
    *slot = Player{};
    slot->id = id;
    slot->used = true;
    slot->peer = peer;
    slot->connectTime = NowMs();
    peer->data = slot;
    enet_peer_timeout(peer, 0, 15000, 20000);  // as the client's: a lossy line dropped players now and then
    LOGI("Подключение от %s (слот %d)", ip, id);
}

void Server::OnDisconnect(ENetPeer* peer, bool timeout) {
    // a kicked player has left already: no slot holds its peer
    if (Player* p = FindByPeer(peer)) Leave(*p, timeout ? LEAVE_TIMEOUT : LEAVE_QUIT);
}

void Server::Leave(Player& p, LeaveReason why, const std::string& reason) {
    if (p.peer) p.peer->data = nullptr;
    bool wasJoined = p.joined, wasHost = p.isHost;
    std::string name = p.name;
    uint8_t id = p.id;
    p = Player{};
    p.id = id;
    m_worldWaiters.erase(std::remove(m_worldWaiters.begin(), m_worldWaiters.end(), id), m_worldWaiters.end());
    if (!wasJoined) return;
    Writer w(S_PLAYER_LEAVE);
    w.U8(id);
    w.U8(why);
    Broadcast(CH_RELIABLE, w);
    std::string msg, en;
    if (why == LEAVE_KICKED) {
        LOGI("%s (%d) выгнан%s%s", name.c_str(), id, reason.empty() ? "" : ": ", reason.c_str());
        msg = reason.empty() ? Format("%s выгнан с сервера.", name.c_str())
                             : Format("%s выгнан с сервера: %s.", name.c_str(), reason.c_str());
        en = reason.empty() ? Format("%s was kicked from the server.", name.c_str())
                            : Format("%s was kicked from the server: %s.", name.c_str(), reason.c_str());
    } else {
        LOGI("%s (%d) отключился%s", name.c_str(), id, why == LEAVE_TIMEOUT ? " (таймаут)" : "");
        msg = Format(why == LEAVE_TIMEOUT ? "%s потерял связь с сервером." : "%s вышел из игры.", name.c_str());
        en = Format(why == LEAVE_TIMEOUT ? "%s lost connection to the server." : "%s left the game.", name.c_str());
    }
    SendChat(nullptr, kServerId, why == LEAVE_KICKED ? kColorServer : kColorInfo, msg, en);
    if (wasHost) EnsureHost();
}

void Server::OnPacket(ENetPeer* peer, uint8_t channel, const uint8_t* data, size_t size, uint32_t flags) {
    Player* p = FindByPeer(peer);
    if (!p || size == 0) return;
    uint8_t id = data[0];
    Reader r(data + 1, size - 1);
    if (!p->joined) {
        if (id == C_HELLO) HandleHello(*p, r);
        return;
    }
    if (IsRelayed(id)) {
        Relay(*p, channel, data, size, flags);
        return;
    }
    switch (id) {
        case C_CHAT: {
            std::string text = r.Str(kMaxChatLen * 4);
            if (r.Ok()) HandleChat(*p, text);
            break;
        }
        case C_PROFILE_SAVE: {
            auto blob = r.Bytes(64 * 1024);
            if (r.Ok()) SaveProfile(*p, blob);
            break;
        }
        case C_WORLD_BEGIN:
            HandleWorldBegin(*p, r);
            break;
        case C_WORLD_CHUNK:
            HandleWorldChunk(*p, r);
            break;
        case C_WORLD_END:
            HandleWorldEnd(*p);
            break;
        case C_WORLD_REQUEST:
            RequestWorld(*p);
            break;
        case C_WORLD_KEEP:
            if (p->isHost) FlushWorldRequests(false);
            break;
        case C_WORLD_NEWGAME:
            if (p->isHost) HostStartedNewGame();
            break;
        case C_WORLD_FOLLOW:
            if (p->isHost) FollowersJoin();
            break;
        case C_STATUS: {
            uint8_t st = r.U8();
            bool becameInGame = st == STATUS_INGAME && p->status != STATUS_INGAME;
            p->status = st;
            // the host is in a game again (it loaded a save): the partners play the world it left
            if (becameInGame && p->isHost)
                for (auto& o : m_players)
                    if (o.used && o.joined && !o.isHost && o.status == STATUS_INGAME) o.staleWorld = true;
            // the host just entered the game: partners waiting for a world can get a fresh one
            if (becameInGame && p->isHost && !m_worldWaiters.empty() && !m_worldWantedTime) {
                Writer w(S_WORLD_WANTED);
                net::Send(p->peer, CH_RELIABLE, w);
                m_worldWantedTime = NowMs();
            }
            break;
        }
        default:
            LOGW("Неизвестное сообщение %u от %s", id, p->name.c_str());
            break;
    }
}

void Server::HandleHello(Player& p, Reader& r) {
    Hello h;
    auto reject = [&](const std::string& why, const std::string& en) {
        LOGW("Отказ \"%s\": %s", h.name.c_str(), why.c_str());
        Writer w(S_REJECT);
        w.Str(why);
        w.Str(en);
        net::Send(p.peer, CH_RELIABLE, w);
        enet_peer_disconnect_later(p.peer, 0);
        p.joined = false;
    };
    // the version comes first in every hello: an older client's shorter one was told "damaged data" instead
    bool read = h.Read(r);
    if (h.protocol != 0 && h.protocol != kProtocolVersion)
        return reject(Format("у вас другая версия мода (у сервера %u, у вас %u). Версия должна быть одинаковой у обоих",
                             kProtocolVersion, h.protocol),
                      Format("your version of the mod differs (the server has %u, you have %u). Both players need the "
                             "same version", kProtocolVersion, h.protocol));
    if (!read) return reject("клиент прислал повреждённые данные", "the client sent damaged data");
    std::string name = SanitizeNickname(h.name);
    if (name.size() < 2)
        return reject("в нике должно быть от 2 до 20 символов (буквы, цифры, знаки _-.[])",
                      "a nickname needs 2 to 20 characters (letters, digits, _-.[])");
    if (ReservedFileName(name))  // the profile is a file named after it: profiles\con.bin is a device on Windows 10
        return reject("такой ник занят системой Windows, выберите другой", "this nickname is reserved by Windows, pick another one");
    if (Player* old = FindByName(name)) {
        // the same player again while its old connection has not timed out (its game crashed, the line dropped): the
        // old one goes when it has been silent for 3 s (ENet pings a live game twice a second)
        enet_uint32 silent = old->peer ? m_host->serviceTime - old->peer->lastReceiveTime : 0;  // ENet's clock, wraps
        if (silent < 3000 || silent >= 0x80000000u)
            return reject("игрок с таким ником уже на сервере", "a player with this nickname is already here");
        ENetPeer* ghost = old->peer;
        LOGI("%s подключается снова: старое соединение закрыто", name.c_str());
        Leave(*old, LEAVE_TIMEOUT);
        enet_peer_reset(ghost);
    }
    if (JoinedCount() >= m_cfg.maxPlayers) return reject("на сервере нет свободных мест", "the server is full");
    if (!m_cfg.password.empty() && h.password != m_cfg.password) return reject("неверный пароль", "wrong password");

    p.name = name;
    p.joined = true;
    bool hostExists = false;
    for (auto& o : m_players)
        if (o.joined && o.isHost && &o != &p) hostExists = true;
    if (!hostExists) {
        bool preferred = !m_cfg.hostName.empty() && ToLower(m_cfg.hostName) == ToLower(name);
        // the first player becomes host unless the server reserves the role for a specific nickname
        if (m_cfg.hostName.empty() || preferred || h.wantsHost) p.isHost = true;
    }

    Welcome wel;
    wel.yourId = p.id;
    wel.hostId = 255;
    for (auto& o : m_players) {
        if (!o.joined) continue;
        if (o.isHost) wel.hostId = o.id;
        if (&o != &p) wel.players.push_back({o.id, o.name, o.isHost});
    }
    wel.serverName = m_cfg.name;
    wel.motd = m_cfg.motd;
    // the default greeting has an English one, a greeting of the server's owner is shown as it is
    wel.motdEn = m_cfg.motd == Config{}.motd ? "Welcome! Type /help to see the commands." : "";
    wel.friendlyFire = m_cfg.friendlyFire;
    wel.partnerTeleport = m_cfg.partnerTeleport;
    wel.worldAvailable = !m_world.empty();
    Writer w(S_WELCOME);
    wel.Write(w);
    net::Send(p.peer, CH_RELIABLE, w);

    Writer j(S_PLAYER_JOIN);
    j.U8(p.id);
    j.Str(p.name, 64);
    j.Bool(p.isHost);
    Broadcast(CH_RELIABLE, j, p.id);

    SendProfile(p);
    SendInvite(&p);
    LOGI("%s вошёл как игрок %d%s (версия %s)", p.name.c_str(), p.id, p.isHost ? " [ХОСТ]" : "", h.version.c_str());
    SendChat(nullptr, kServerId, kColorInfo, Format("%s зашёл в игру%s.", p.name.c_str(), p.isHost ? " (хост)" : ""),
             Format("%s joined the game%s.", p.name.c_str(), p.isHost ? " (host)" : ""));
    // with a reserved host nickname the role waits for that player
    if (wel.hostId == 255 && !p.isHost && m_cfg.hostName.empty()) EnsureHost();
}

void Server::HandleChat(Player& p, const std::string& rawText) {
    std::string text = rawText;
    for (char& c : text)  // control characters: the chat draws none, Trim and Split would disagree on them
        if ((unsigned char)c < 0x20) c = ' ';
    text = Trim(text);
    if (text.empty()) return;
    if (text[0] == '/') {
        HandleCommand(&p, text);
        return;
    }
    // limit by characters, not bytes
    std::wstring wt = Utf8ToWide(text);
    if (wt.size() > kMaxChatLen) text = WideToUtf8(wt.substr(0, kMaxChatLen));
    LOGI("<%s> %s", p.name.c_str(), text.c_str());
    SendChat(nullptr, p.id, 0xFFFFFFFF, text);
}

void Server::HandleCommand(Player* p, const std::string& text) {
    std::string line = Trim(text);
    if (!line.empty() && line[0] == '/') line = Trim(line.substr(1));
    auto parts = Split(line, ' ');
    parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());  // doubled spaces
    std::string cmd = parts.empty() ? "" : ToLower(parts[0]);
    std::string rest = line.size() > cmd.size() ? Trim(line.substr(cmd.size())) : "";
    auto reply = [&](uint32_t color, const std::string& msg, const std::string& en = {}) {
        if (p)
            SendChat(p, kServerId, color, msg, en);
        else
            LOGI("%s", msg.c_str());
    };
    bool admin = !p || p->isHost;

    if (cmd == "help" || cmd == "?") {
        reply(kColorServer, "Команды: /players, /pm <id> <текст>, /me <действие>",
              "Commands: /players, /pm <id> <text>, /me <action>");
        if (admin)
            reply(kColorServer, "Для хоста: /kick <ник или номер> [причина], /say <текст>",
                  "For the host: /kick <nickname or number> [reason], /say <text>");
        if (admin || m_cfg.partnerTeleport)
            reply(kColorServer, "В игре: /tp (переместиться к напарнику), /tphere (позвать напарника к себе). Напарник отвечает Y или N",
                  "In the game: /tp (go to your partner), /tphere (bring your partner to you). Your partner answers Y or N");
        reply(kColorServer, "F2: игроки, у кого сколько денег, перевод денег", "F2: the players, their money, giving money");
    } else if (cmd == "players" || cmd == "status") {
        for (auto& o : m_players) {
            if (!o.joined) continue;
            static const char* st[] = {"в меню", "загрузка", "в игре", "пауза"};
            static const char* stEn[] = {"in the menu", "loading", "in the game", "paused"};
            int s = o.status < 4 ? o.status : 0;
            uint32_t ping = o.peer ? o.peer->roundTripTime : 0;
            reply(kColorInfo, Format("[%d] %s%s, пинг %u мс, %s", o.id, o.name.c_str(), o.isHost ? " (хост)" : "", ping, st[s]),
                  Format("[%d] %s%s, ping %u ms, %s", o.id, o.name.c_str(), o.isHost ? " (host)" : "", ping, stEn[s]));
        }
    } else if (cmd == "pm" && parts.size() >= 3 && p) {
        Player* to = FindById(atoi(parts[1].c_str()));
        std::string msg = rest.size() > parts[1].size() ? Trim(rest.substr(parts[1].size())) : std::string();
        if (!to || !to->joined) return reply(kColorError, "Нет такого игрока.", "There is no such player.");
        SendChat(to, kServerId, 0xFFFF80FF, Format("[ЛС от %s] %s", p->name.c_str(), msg.c_str()),
                 Format("[PM from %s] %s", p->name.c_str(), msg.c_str()));
        reply(0xFFFF80FF, Format("[ЛС для %s] %s", to->name.c_str(), msg.c_str()),
              Format("[PM to %s] %s", to->name.c_str(), msg.c_str()));
    } else if (cmd == "me" && p && !rest.empty()) {
        SendChat(nullptr, kServerId, 0xC8A0FFFF, Format("* %s %s", p->name.c_str(), rest.c_str()));
    } else if (cmd == "kick") {
        if (!admin) return reply(kColorError, "Выгнать игрока может только хост.", "Only the host can kick players.");
        if (parts.size() < 2)
            return reply(kColorError, "Укажите игрока: /kick <ник или номер> [причина].",
                         "Name the player: /kick <nickname or number> [reason].");
        // the nickname is what the player list shows, the number comes from /players
        Player* t = FindByName(parts[1]);
        if (!t && parts[1].find_first_not_of("0123456789") == std::string::npos) t = FindById(atoi(parts[1].c_str()));
        if (!t || !t->joined) return reply(kColorError, "Нет такого игрока.", "There is no such player.");
        if (t == p) return reply(kColorError, "Себя выгнать нельзя.", "You cannot kick yourself.");
        std::string reason = Trim(rest.substr(parts[1].size()));
        while (!reason.empty() && reason.back() == '.') reason.pop_back();  // the message adds its own period
        Kick(*t, reason);
    } else if (cmd == "say" && admin && !rest.empty()) {
        SendChat(nullptr, kServerId, kColorServer, "[Сервер] " + rest, "[Server] " + rest);
    } else if (cmd == "quit" && !p) {
        Stop();
    } else if (cmd == "tp" || cmd == "tphere" || cmd == "voice" || cmd == "save" || cmd == "skin") {
        // handled by the game client locally; nothing to do on the server
    } else {
        reply(kColorError, "Такой команды нет. Чтобы увидеть список команд, напишите /help.",
              "There is no such command. Type /help to see the commands.");
    }
}

void Server::HandleWorldBegin(Player& p, Reader& r) {
    uint32_t size = r.U32();
    uint32_t crc = r.U32();
    std::string label = r.Str(128);
    if (!r.Ok() || !p.isHost || size < 1024 || size > 4 * 1024 * 1024) {
        p.upload.clear();
        return;
    }
    p.upload.assign(size, 0);
    p.uploadCrc = crc;
    p.uploadLabel = label;
}

void Server::HandleWorldChunk(Player& p, Reader& r) {
    uint32_t off = r.U32();
    auto bytes = r.Bytes(kWorldChunk);
    // off + size wraps in 32 bits: the bound is checked without the sum
    if (!r.Ok() || p.upload.empty() || off > p.upload.size() || bytes.size() > p.upload.size() - off) return;
    memcpy(p.upload.data() + off, bytes.data(), bytes.size());
}

void Server::HandleWorldEnd(Player& p) {
    if (p.upload.empty()) return;
    uint32_t crc = Crc32(p.upload.data(), p.upload.size());
    if (crc != p.uploadCrc) {
        LOGW("Сохранение мира от %s повреждено (crc %08X != %08X)", p.name.c_str(), crc, p.uploadCrc);
    } else {
        m_world.swap(p.upload);
        m_worldLabel = p.uploadLabel;
        WriteFileBytes(WorldPath(), m_world.data(), m_world.size(), false);  // the network thread: no wait for the disk
        LOGI("Хост %s обновил сохранение мира: %s (%u байт)", p.name.c_str(), m_worldLabel.c_str(),
             (unsigned)m_world.size());
        m_prologue = false;
        // partners who joined after the prologue in a new game of their own switch to the host's world now, and after
        // the host's story progress every partner in the game gets it (it loads at a safe moment)
        bool progress = m_worldLabel == "progress";
        for (auto& o : m_players)
            if (o.used && o.joined && !o.isHost && (o.follower || o.staleWorld || (progress && o.status == STATUS_INGAME))) {
                o.follower = o.staleWorld = false;
                SendWorld(o);
                // one that had asked for it as well got it twice (FlushWorldRequests below), and loaded it twice
                m_worldWaiters.erase(std::remove(m_worldWaiters.begin(), m_worldWaiters.end(), o.id), m_worldWaiters.end());
            }
    }
    p.upload.clear();
    p.upload.shrink_to_fit();
    FlushWorldRequests(true);
}

// The host plays a new game's prologue alone (it is a mission and cannot be saved), and the stored world belongs to an
// older game. Partners wait until the prologue lets CJ get on the bike, those playing the old world too, and start the
// new game with nothing from the old one.
void Server::HostStartedNewGame() {
    if (!m_world.empty()) {
        m_world.clear();
        m_worldLabel.clear();
        DeleteFileW(WorldPath().c_str());
        LOGI("Хост начал новую игру: сохранённый мир устарел и удалён");
    }
    ResetProfiles();
    m_prologue = true;
    m_worldWantedTime = 0;
    for (auto& o : m_players)
        if (o.joined && !o.isHost && (o.status == STATUS_INGAME || o.status == STATUS_LOADING) &&
            std::find(m_worldWaiters.begin(), m_worldWaiters.end(), o.id) == m_worldWaiters.end())
            m_worldWaiters.push_back(o.id);
    for (uint8_t id : m_worldWaiters)
        if (Player* w = FindById(id); w && w->joined) TellToWait(*w);
}

// The profiles of an older game go aside (profiles\old\<date>\: a new game started by mistake loses nothing), and the
// partners connected now get an empty one.
void Server::ResetProfiles() {
    WaitProfileWrites();  // an older game's profile written after this would come back
    std::wstring dir = m_cfg.dataDir + L"profiles\\";
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d-%02d-%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring old = dir + L"old\\" + stamp + L"\\";
    int moved = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.bin").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        EnsureDirectory(old);
        do {
            if (MoveFileW((dir + fd.cFileName).c_str(), (old + fd.cFileName).c_str())) ++moved;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (moved) LOGI("Новая игра: профили напарников (%d) перенесены в profiles\\old\\%s", moved, WideToUtf8(stamp).c_str());
    for (auto& o : m_players)
        if (o.joined && !o.isHost) SendProfile(o);
}

void Server::TellToWait(Player& p) {
    Writer w(S_WORLD_WAIT);
    net::Send(p.peer, CH_BULK, w);
}

// The prologue is over, yet the host rides the bike and cannot be saved: the waiting partners start a new game of
// their own and join the host there until its first save.
void Server::FollowersJoin() {
    m_prologue = false;
    m_worldWantedTime = 0;
    auto waiters = m_worldWaiters;
    m_worldWaiters.clear();
    for (uint8_t id : waiters)
        if (Player* w = FindById(id); w && w->joined) {
            Writer n(S_WORLD_NEWGAME);
            net::Send(w->peer, CH_BULK, n);
            w->follower = true;
            LOGI("Игрок %s присоединяется к хосту после пролога", w->name.c_str());
        }
}

void Server::RequestWorld(Player& p) {
    for (uint8_t id : m_worldWaiters)
        if (id == p.id) {
            if (m_prologue) TellToWait(p);  // asked again (reconnected): it still waits
            return;
        }
    if (m_prologue) {  // nothing to load until the prologue is over
        m_worldWaiters.push_back(p.id);
        TellToWait(p);
        return;
    }
    Player* host = nullptr;
    for (auto& o : m_players)
        if (o.joined && o.isHost && &o != &p) host = &o;
    if (host && host->status == STATUS_INGAME) {
        // ask the host for an up-to-date snapshot; the stored world is used if it does not answer
        m_worldWaiters.push_back(p.id);
        if (!m_worldWantedTime) {
            Writer w(S_WORLD_WANTED);
            net::Send(host->peer, CH_RELIABLE, w);
            m_worldWantedTime = NowMs();
        }
        SendChat(&p, kServerId, kColorInfo, "Хост сохраняет мир. Подождите...", "The host is saving the world. Please wait...");
        return;
    }
    // Nothing to send yet, or a host still in the menu: wait until the host loads a game and uploads it. The world
    // stored here is its last session's, and a partner who took it played another world than the save the host loaded.
    if (m_world.empty() || host) {
        m_worldWaiters.push_back(p.id);
        Writer w(S_WORLD_NONE);
        net::Send(p.peer, CH_BULK, w);
        SendChat(&p, kServerId, kColorInfo, "Хост ещё не загрузил игру. Ждём...", "The host has not loaded a game yet. Waiting...");
        return;
    }
    SendWorld(p);
}

void Server::FlushWorldRequests(bool fresh) {
    m_worldWantedTime = 0;
    if (m_world.empty()) {  // nothing to give yet: they wait on (the partner gave up after 2 minutes without this)
        for (uint8_t id : m_worldWaiters)
            if (Player* w = FindById(id); w && w->joined) net::Send(w->peer, CH_BULK, Writer(S_WORLD_NONE));
        return;
    }
    auto waiters = m_worldWaiters;
    m_worldWaiters.clear();
    for (uint8_t id : waiters)
        if (Player* w = FindById(id); w && w->joined) {
            // the stored world (the host could not save in a car, or did not answer): its next upload goes to them too,
            // else they played last session's world until the host passed a mission
            w->staleWorld = !fresh;
            SendWorld(*w);
        }
}

void Server::SendWorld(Player& p) {
    if (m_world.empty()) {
        Writer w(S_WORLD_NONE);
        net::Send(p.peer, CH_BULK, w);
        return;
    }
    Writer b(S_WORLD_BEGIN);
    b.U32((uint32_t)m_world.size());
    b.U32(Crc32(m_world.data(), m_world.size()));
    b.Str(m_worldLabel, 128);
    net::Send(p.peer, CH_BULK, b);
    for (size_t off = 0; off < m_world.size(); off += kWorldChunk) {
        size_t n = std::min(kWorldChunk, m_world.size() - off);
        Writer c(S_WORLD_CHUNK);
        c.U32((uint32_t)off);
        c.Bytes(m_world.data() + off, n);
        net::Send(p.peer, CH_BULK, c);
    }
    Writer e(S_WORLD_END);
    net::Send(p.peer, CH_BULK, e);
    LOGI("Отправлено сохранение мира игроку %s (%u байт)", p.name.c_str(), (unsigned)m_world.size());
}

std::wstring Server::ProfilePath(const std::string& name) const {
    return m_cfg.dataDir + L"profiles\\" + Utf8ToWide(ToLower(name)) + L".bin";
}

void Server::SendProfile(Player& p) {
    std::vector<uint8_t> data;
    ReadFileBytes(ProfilePath(p.name), data);
    Writer w(S_PROFILE);
    w.Bytes(data.data(), data.size());
    net::Send(p.peer, CH_RELIABLE, w);
}

void Server::SendInvite(Player* to) {
    std::string code = m_internet.Code();
    if (code.empty()) return;
    Writer w(S_INVITE);
    w.Str(code, 32);
    if (to) net::Send(to->peer, CH_RELIABLE, w);
    else Broadcast(CH_RELIABLE, w);
}

// A profile goes to the disk on a thread of its own: flushed for a power cut, its write waited for the disk on the
// network thread and stalled the relay of every state each time a partner sent its profile (~30 s: freezes, #43)
void Server::SaveProfile(Player& p, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(m_writeMutex);
    m_profileWrites[ProfilePath(p.name)] = data;
    if (m_writing) return;
    m_writing = true;
    std::thread([this] { WriteProfiles(); }).detach();
}

void Server::WriteProfiles() {
    for (;;) {
        std::wstring path;
        std::vector<uint8_t> data;
        {
            std::lock_guard<std::mutex> lock(m_writeMutex);
            if (m_profileWrites.empty()) {
                m_writing = false;
                return;
            }
            auto it = m_profileWrites.begin();
            path = it->first;
            data = std::move(it->second);
            m_profileWrites.erase(it);
        }
        EnsureDirectory(m_cfg.dataDir + L"profiles");
        if (!WriteFileBytes(path, data.data(), data.size())) LOGW("Не удалось записать профиль %s", WideToUtf8(path).c_str());
    }
}

void Server::WaitProfileWrites() {
    for (int i = 0; i < 500; ++i) {
        {
            std::lock_guard<std::mutex> lock(m_writeMutex);
            if (!m_writing) return;
        }
        Sleep(10);
    }
}

void Server::Relay(Player& from, uint8_t channel, const uint8_t* data, size_t size, uint32_t flags) {
    if (channel >= CH_COUNT) return;
    // [id][sender][payload]
    ENetPacket* pkt = enet_packet_create(nullptr, size + 1, flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED));
    pkt->data[0] = data[0];
    pkt->data[1] = from.id;
    memcpy(pkt->data + 2, data + 1, size - 1);
    bool sent = false;
    for (auto& o : m_players) {
        if (!o.joined || &o == &from) continue;
        // states for a player still loading or in the menu piled up there and came stale all at once (voice goes on:
        // players talk while one of them loads)
        if (channel == CH_STATE && o.status != STATUS_INGAME) continue;
        sent = enet_peer_send(o.peer, channel, pkt) == 0 || sent;  // a peer going away takes nothing
    }
    if (!sent) enet_packet_destroy(pkt);
}

void Server::SendChat(Player* to, uint8_t fromId, uint32_t rgba, const std::string& text, const std::string& en) {
    Writer w(S_CHAT);
    w.U8(fromId);
    w.U32(rgba);
    w.Str(text, kMaxChatLen * 4 + 64);
    w.Str(en, kMaxChatLen * 4 + 64);
    if (to)
        net::Send(to->peer, CH_RELIABLE, w);
    else
        Broadcast(CH_RELIABLE, w);
}

void Server::Broadcast(uint8_t channel, const Writer& w, int exceptId) {
    for (auto& o : m_players)
        if (o.joined && o.id != exceptId) net::Send(o.peer, channel, w);
}

// The player leaves the game at once: its game confirms the disconnect only later, a hung one never (ENet drops it after
// the 20 s timeout), and the others would see the player in the list until then.
void Server::Kick(Player& p, const std::string& reason, const std::string& reasonEn) {
    if (!p.peer) return;
    Writer w(S_KICKED);
    w.Str(reason);
    w.Str(reasonEn);
    net::Send(p.peer, CH_RELIABLE, w);
    enet_peer_disconnect_later(p.peer, 0);
    Leave(p, LEAVE_KICKED, reason);
}

void Server::EnsureHost() {
    for (auto& o : m_players)
        if (o.joined && o.isHost) return;
    if (!m_cfg.hostName.empty()) return;  // the reserved host will take the role when (re)joining
    for (auto& o : m_players) {
        if (!o.joined) continue;
        o.isHost = true;
        Writer w(S_HOST_CHANGED);
        w.U8(o.id);
        Broadcast(CH_RELIABLE, w);
        SendChat(nullptr, kServerId, kColorServer, Format("%s теперь хост.", o.name.c_str()),
                 Format("%s is the host now.", o.name.c_str()));
        LOGI("Новый хост: %s", o.name.c_str());
        return;
    }
}

void Server::SendPings() {
    Writer w(S_PINGS);
    uint8_t n = 0;
    for (auto& o : m_players)
        if (o.joined) ++n;
    if (!n) return;
    w.U8(n);
    for (auto& o : m_players) {
        if (!o.joined) continue;
        w.U8(o.id);
        w.U16((uint16_t)std::min<uint32_t>(o.peer->roundTripTime, 65535));
    }
    Broadcast(CH_RELIABLE, w);
}

Player* Server::FindByPeer(ENetPeer* peer) {
    for (auto& p : m_players)
        if (p.used && p.peer == peer) return &p;
    return nullptr;
}

Player* Server::FindById(int id) {
    if (id < 0 || id >= kMaxPlayers || !m_players[id].used) return nullptr;
    return &m_players[id];
}

Player* Server::FindByName(const std::string& name) {
    std::string l = ToLower(name);
    for (auto& p : m_players)
        if (p.joined && ToLower(p.name) == l) return &p;
    return nullptr;
}

int Server::JoinedCount() const {
    int n = 0;
    for (auto& p : m_players) n += p.joined;
    return n;
}

}  // namespace coop::server
