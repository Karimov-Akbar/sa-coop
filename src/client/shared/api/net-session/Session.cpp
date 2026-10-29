#include "shared/api/net-session/Session.h"

#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace proto;

Session& Net() {
    static Session s;
    return s;
}

void Session::Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password,
                      bool wantsHost) {
    Disconnect();
    m_nick = nick;
    m_password = password;
    m_wantsHost = wantsHost;
    m_failReason.clear();
    m_peers.clear();
    m_inviteCode.clear();
    m_worldRetries = 0;
    m_myId = m_hostId = 255;
    m_state = State::Connecting;
    if (!m_net.Start(host, port)) {
        m_state = State::Failed;
        m_failReason = TR("Не удалось запустить сеть.", "Could not start the network.");
    }
}

void Session::Disconnect(const std::string& reason) {
    bool wasOnline = m_state == State::Online;
    m_net.Stop();
    m_net.TakeConnectEvent();
    m_net.TakeDisconnectEvent();
    if (m_state == State::Online || m_state == State::Connecting) m_state = State::Offline;
    m_peers.clear();
    m_world.clear();
    m_worldExpected = m_worldRecv = 0;
    if (wasOnline && onOffline) onOffline(reason);
}

const PeerInfo* Session::Peer(uint8_t id) const {
    auto it = m_peers.find(id);
    return it == m_peers.end() ? nullptr : &it->second;
}

std::string Session::PeerName(uint8_t id) const {
    if (id == m_myId) return m_nick;
    auto* p = Peer(id);
    return p ? p->name : Format(TR("игрок %d", "player %d"), id);
}

void Session::SendChat(const std::string& text) {
    Writer w(C_CHAT);
    w.Str(text, kMaxChatLen * 4);
    Send(CH_RELIABLE, w);
}

void Session::Update() {
    if (m_net.TakeConnectEvent()) {
        Hello h;
        h.protocol = kProtocolVersion;
        h.version = COOP_VERSION_STR;
        h.name = m_nick;
        h.password = m_password;
        h.wantsHost = m_wantsHost;
        h.exeSize = kSupportedExeSize;
        Writer w(C_HELLO);
        h.Write(w);
        m_net.Send(CH_RELIABLE, w);
    }

    m_inbox.clear();
    m_net.Poll(m_inbox);
    for (auto& pkt : m_inbox) {
        if (pkt.data.empty()) continue;
        uint8_t id = pkt.data[0];
        if (IsRelayed(id)) {
            if (pkt.data.size() < 2 || m_state != State::Online) continue;
            uint8_t from = pkt.data[1];
            // a player who left: its late state made its character again (with its old time, so the next player under
            // that number had its states dropped as old ones)
            if (!m_peers.count(from)) continue;
            Reader r(pkt.data.data() + 2, pkt.data.size() - 2);
            if (m_handlers[id]) m_handlers[id](from, r);
        } else {
            Reader r(pkt.data.data() + 1, pkt.data.size() - 1);
            HandleService(id, r);
        }
    }

    if (m_net.TakeDisconnectEvent()) {
        bool wasOnline = m_state == State::Online;
        if (m_state == State::Connecting) {
            m_state = State::Failed;
            if (m_failReason.empty() && m_net.SignalFailed())
                // the code's channel is how the host opens its NAT to us: unreachable (blocked in some networks), only an
                // open port gets us in
                m_failReason = TR("Не удалось подключиться: нет связи с ntfy.sh, через который код приглашения открывает "
                                  "путь к хосту. Возможно, этот сайт недоступен в вашей сети. Попробуйте VPN или попросите "
                                  "хоста открыть порт на роутере (UPnP или проброс порта).",
                                  "Could not connect: ntfy.sh, through which the invite code opens the way to the host, cannot "
                                  "be reached. It may be blocked in your network. Try a VPN, or ask the host to open the port "
                                  "on the router (UPnP or port forwarding).");
            if (m_failReason.empty() && m_net.ViaCode()) {
                // the code was read and the host told: the way it opens through the routers did not (a provider's NAT,
                // mobile internet), which no firewall setting fixes (#13: Radmin VPN on, the firewall off)
                LOGW("Код приглашения: хост оповещён, но ни прямой путь, ни ретранслятор не ответили");
                m_failReason = TR("Сервер не отвечает. Код приглашения сработал, но путь к хосту не открылся: обычно мешает роутер "
                                  "или мобильный интернет у одного из игроков. Помогут Radmin VPN или ZeroTier у обоих (тогда "
                                  "входите по их адресу) или проброс порта на роутере хоста (UDP, по умолчанию 7788). Брандмауэр и "
                                  "антивирус хоста тоже стоит проверить.",
                                  "The server does not answer. The invite code worked, but the way to the host did not open: "
                                  "usually a router or mobile internet on one side is in the way. Radmin VPN or ZeroTier on both "
                                  "sides (then join by their address) or a port forwarded on the host's router (UDP, 7788 by "
                                  "default) will help. The host's firewall and antivirus are worth a check too.");
            }
            if (m_failReason.empty())
                // the host's own game is in through 127.0.0.1: what keeps a friend out is mostly its firewall or antivirus
                m_failReason = TR("Сервер не отвечает. Чаще всего к нему не пускает брандмауэр или антивирус хоста: пусть хост "
                                  "создаст игру заново и подтвердит запрос Windows. Проверьте также адрес и порт.",
                                  "The server does not answer. Most often the host's firewall or antivirus keeps you out: let the "
                                  "host create the game again and confirm the Windows prompt. Also check the address and the port.");
        } else if (m_state == State::Online) {
            m_state = State::Failed;
            if (m_failReason.empty()) m_failReason = TR("Соединение с сервером потеряно.", "Lost connection to the server.");
        }
        m_net.Stop();
        m_peers.clear();
        if (wasOnline && onOffline) onOffline(m_failReason);
    }
}

void Session::HandleService(uint8_t id, Reader& r) {
    auto& chat = ui::TheChat();
    switch (id) {
        case S_WELCOME: {
            Welcome w;
            if (!w.Read(r)) break;
            m_myId = w.yourId;
            m_hostId = w.hostId;
            m_serverName = w.serverName;
            m_friendlyFire = w.friendlyFire;
            m_partnerTeleport = w.partnerTeleport;
            m_peers.clear();
            for (auto& p : w.players) m_peers[p.id] = PeerInfo{p.id, p.name, p.isHost, 0};
            m_state = State::Online;
            LOGI("Вошли на сервер \"%s\" как игрок %d, хост: %d", m_serverName.c_str(), m_myId, m_hostId);
            chat.AddSystem(IsHost() ? TR("Сервер запущен. Вы хост.", "The server is up. You are the host.")
                                    : Format(TR("Вы подключились к серверу «%s».", "You joined the server \"%s\"."),
                                             m_serverName.c_str()));
            if (!w.motd.empty()) chat.Add(Cfg().english && !w.motdEn.empty() ? w.motdEn : w.motd, 0xFFA0C8FF);
            if (m_friendlyFire)
                chat.AddSystem(TR("Урон по напарнику включён: вы можете ранить и убить друг друга.",
                                  "Friendly fire is on: you can hurt and kill each other."));
            if (!m_partnerTeleport && !IsHost())
                chat.AddSystem(TR("Хост запретил напарникам команды /tp и /tphere.", "The host turned /tp and /tphere off for partners."));
            if (onOnline) onOnline();
            break;
        }
        case S_REJECT:
        case S_KICKED: {
            std::string reason = r.Str(), en = r.Str();
            if (Cfg().english && !en.empty()) reason = en;
            if (id == S_REJECT) m_failReason = Format(TR("Не удалось войти: %s.", "Could not join: %s."), reason.c_str());
            else if (reason.empty()) m_failReason = TR("Вас выгнали с сервера.", "You were kicked from the server.");
            else m_failReason = Format(TR("Вас выгнали с сервера: %s.", "You were kicked from the server: %s."), reason.c_str());
            LOGW("%s", m_failReason.c_str());
            // shown once: a failed connect by the flow, a kick by onOffline when the connection closes
            break;
        }
        case S_PLAYER_JOIN: {
            PeerInfo p;
            p.id = r.U8();
            p.name = r.Str(64);
            p.isHost = r.Bool();
            if (!r.Ok()) break;
            m_peers[p.id] = p;
            if (p.isHost) m_hostId = p.id;
            if (onPeerJoin) onPeerJoin(p.id);
            break;
        }
        case S_PLAYER_LEAVE: {
            uint8_t pid = r.U8();
            if (onPeerLeave) onPeerLeave(pid);
            m_peers.erase(pid);
            break;
        }
        case S_HOST_CHANGED: {
            m_hostId = r.U8();
            for (auto& [pid, p] : m_peers) p.isHost = pid == m_hostId;
            if (onHostChanged) onHostChanged(m_hostId);
            break;
        }
        case S_CHAT: {
            uint8_t from = r.U8();
            uint32_t rgba = r.U32();
            std::string text = r.Str(), en = r.Str();
            if (!r.Ok()) break;
            if (from == kServerId) {
                chat.Add(Cfg().english && !en.empty() ? en : text, ui::RgbaToArgb(rgba));
            } else {
                uint32_t nameColor = from == m_hostId ? 0xFF64B4FF : 0xFF78DC78;
                char hex[16];
                snprintf(hex, sizeof(hex), "{%06X}", nameColor & 0xFFFFFF);
                chat.Add(std::string(hex) + PeerName(from) + "{FFFFFF}: " + text, 0xFFFFFFFF);
            }
            break;
        }
        case S_PINGS: {
            uint8_t n = r.U8();
            for (uint8_t i = 0; i < n && r.Ok(); ++i) {
                uint8_t pid = r.U8();
                uint16_t ping = r.U16();
                auto it = m_peers.find(pid);
                if (it != m_peers.end()) it->second.ping = ping;
            }
            break;
        }
        case S_PROFILE: {
            auto blob = r.Bytes(64 * 1024);
            if (r.Ok() && onProfile) onProfile(blob);
            break;
        }
        case S_WORLD_BEGIN: {
            m_worldExpected = r.U32();
            m_worldCrc = r.U32();
            m_worldLabel = r.Str(128);
            m_worldRecv = 0;
            if (m_worldExpected > 8 * 1024 * 1024) m_worldExpected = 0;
            m_world.assign(m_worldExpected, 0);
            break;
        }
        case S_WORLD_CHUNK: {
            uint32_t off = r.U32();
            auto bytes = r.Bytes(kWorldChunk);
            // off + size wraps in 32 bits: the bound is checked without the sum
            if (!r.Ok() || off > m_world.size() || bytes.size() > m_world.size() - off) break;
            memcpy(m_world.data() + off, bytes.data(), bytes.size());
            m_worldRecv += (uint32_t)bytes.size();
            break;
        }
        case S_WORLD_END: {
            uint32_t crc = Crc32(m_world.data(), m_world.size());
            if (crc != m_worldCrc) {
                LOGE("Сохранение мира повреждено при передаче (crc %08X != %08X)", crc, m_worldCrc);
                // asked again three times at most: a world too big to take (over 8 MB) came "damaged" for ever
                if (++m_worldRetries <= 3) {
                    chat.AddError(TR("Мир пришёл с ошибкой. Запрашиваем ещё раз...", "The world arrived damaged. Asking for it again..."));
                    Writer w(C_WORLD_REQUEST);
                    Send(CH_BULK, w);
                } else {
                    chat.AddError(TR("Мир снова пришёл с ошибкой. Попробуйте подключиться заново.",
                                     "The world arrived damaged again. Try to connect again."));
                }
            } else {
                m_worldRetries = 0;
                if (onWorld) onWorld(m_world, m_worldLabel);
            }
            m_world.clear();
            m_worldExpected = m_worldRecv = 0;
            break;
        }
        case S_WORLD_NONE:
            if (onWorldNone) onWorldNone();
            break;
        case S_WORLD_NEWGAME:
            if (onWorldNewGame) onWorldNewGame();
            break;
        case S_WORLD_WAIT:
            if (onWorldWait) onWorldWait();
            break;
        case S_INVITE: {
            std::string code = r.Str(32);
            if (r.Ok() && code != m_inviteCode) {
                m_inviteCode = code;
                if (onInvite) onInvite(code);
            }
            break;
        }
        case S_WORLD_WANTED:
            if (onWorldWanted) onWorldWanted();
            break;
        default:
            LOGW("Unknown service message %u", id);
            break;
    }
}

}  // namespace coop
