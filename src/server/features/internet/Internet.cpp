#include "features/internet/Internet.h"

#include <random>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop::server {

namespace {
Internet* g_self = nullptr;
}

void Internet::Start(ENetHost* host, uint16_t port, const std::string& turnSpec) {
    m_host = host;
    m_port = port;
    m_turnEnabled = !turnSpec.empty();
    if (m_turnEnabled) m_turn.Start(turnSpec, port);
    g_self = this;
    host->intercept = &Internet::Intercept;
    m_quit = false;
    m_thread = std::thread(&Internet::Worker, this);
}

void Internet::Stop() {
    m_quit = true;
    m_signal.Stop();
    m_turn.Stop();
    if (m_thread.joinable()) m_thread.join();
    if (m_upnp.ok) {
        nat::UpnpUnmapPort(m_upnp, m_port);
        m_upnp.ok = false;
    }
    if (m_host) m_host->intercept = nullptr;
    g_self = nullptr;
}

std::string Internet::Code() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_code;
}

bool Internet::TakeNewCode(std::string& code) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_codeNew) return false;
    m_codeNew = false;
    code = m_code;
    return true;
}

// STUN answers and punch packets arrive on the game port: they never reach ENet's protocol code
int ENET_CALLBACK Internet::Intercept(ENetHost* host, ENetEvent*) {
    Internet* self = g_self;
    const uint8_t* data = host->receivedData;
    size_t len = host->receivedDataLength;
    if (!self) return 0;
    if (len >= sizeof(nat::kPunchMagic) - 1 && !memcmp(data, nat::kPunchMagic, sizeof(nat::kPunchMagic) - 1)) return 1;
    if (len == sizeof(nat::kDiscoverMagic) - 1 && !memcmp(data, nat::kDiscoverMagic, len)) {
        std::string reply = nat::kHereMagic + std::to_string(self->m_port);
        ENetBuffer buf{reply.size(), (void*)reply.data()};
        enet_socket_send(host->socket, &host->receivedAddress, &buf, 1);
        return 1;
    }
    if (!nat::IsStunPacket(data, len)) return 0;
    std::lock_guard<std::mutex> lock(self->m_mutex);
    nat::Endpoint mapped;
    if (self->m_stunWaiting && nat::ParseStunResponse(data, len, self->m_txid, mapped)) {
        self->m_mapped = mapped;
        self->m_stunWaiting = false;
    }
    return 1;
}

void Internet::Worker() {
    // 1. the router forwards the port (works for most home routers; useless behind carrier NAT)
    m_upnp = nat::UpnpMapPort(m_port, "SA:COOP server");
    if (m_upnp.ok) LOGI("UPnP: порт %u проброшен. Внешний адрес роутера: %s", m_port, m_upnp.externalIp.c_str());
    else LOGI("UPnP недоступен (%s). Будем пробивать NAT", m_upnp.error.c_str());

    // 2. the address the internet sees for our socket
    auto askStun = [this](const ENetAddress& addr) {
        uint8_t req[20];
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            nat::BuildStunRequest(req, m_txid);
            m_stunWaiting = true;
        }
        ENetBuffer buf{sizeof(req), req};
        enet_socket_send(m_host->socket, &addr, &buf, 1);
    };
    ENetAddress stunAddr{};  // the server that answered
    for (const std::string& server : nat::StunServers()) {
        if (m_quit) return;
        size_t colon = server.rfind(':');
        ENetAddress addr{};
        if (enet_address_set_host(&addr, server.substr(0, colon).c_str()) != 0) continue;
        addr.port = (enet_uint16)atoi(server.c_str() + colon + 1);
        askStun(addr);
        for (int i = 0; i < 20 && !m_quit; ++i) {
            Sleep(100);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stunWaiting) break;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mapped.Valid()) {
            stunAddr = addr;
            break;
        }
        m_stunWaiting = false;
    }

    nat::Endpoint ep;
    nat::Endpoint upnpEp;
    if (m_upnp.ok && nat::Endpoint::Parse(m_upnp.externalIp + ":" + std::to_string(m_port), upnpEp) &&
        !nat::IsPrivateIp(upnpEp.ip)) {
        ep = upnpEp;  // directly reachable
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        ep = m_mapped;
    }
    // the relay address goes to the signalling channel (or into the code when nothing else works)
    nat::Endpoint relay;
    for (int i = 0; i < 150 && m_turnEnabled && !m_quit && !(relay = m_turn.Relayed()).Valid(); ++i) Sleep(100);
    if (!ep.Valid()) ep = relay;
    if (!ep.Valid()) {
        LOGW("Внешний адрес не определён (нет интернета?). Подключиться можно только по локальной сети или по IP");
        return;
    }
    std::random_device rd;
    std::string code = nat::MakeInviteCode(ep, (uint8_t)rd());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_code = code;
        m_codeNew = true;
    }
    LOGI("Код приглашения: %s (внешний адрес %s)", code.c_str(), ep.ToString().c_str());

    // 3. joiners behind NAT announce their endpoint; we answer with packets that open our NAT for them
    m_signal.Start(nat::kDefaultSignalUrl, nat::SignalTopic(code), [this](const std::string& msg) { QueuePunch(msg); });
    if (relay.Valid() && relay.ip != ep.ip) {
        nat::SignalPublish(nat::kDefaultSignalUrl, nat::SignalTopic(code), "RELAY " + relay.ToString());
        LOGI("Ретранслятор для друзей за любым NAT: %s", relay.ToString().c_str());
    }

    // 4. while the server runs: renew a finite UPnP lease, and keep the port's mapping alive. A router gave a forwarded
    //    port a new external port after a minute of silence (12.11), and the code given to a player who came later led
    //    nowhere (#133, #138: a new port and code helped). A STUN request every 15 s keeps it; a changed one is logged.
    nat::Endpoint coded;  // what the code says when it came from STUN
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ep.ip == m_mapped.ip && ep.port == m_mapped.port) coded = ep;
    }
    uint64_t renewAt = NowMs() + (uint64_t)m_upnp.leaseSeconds * 500, keepAt = NowMs() + 15000, checkAt = 0;
    bool told = false;
    while (!m_quit) {
        Sleep(500);
        uint64_t now = NowMs();
        if (m_upnp.ok && m_upnp.leaseSeconds > 0 && now >= renewAt) {
            nat::UpnpResult again = nat::UpnpMapPort(m_port, "SA:COOP server");
            if (again.ok) m_upnp = again;
            renewAt = NowMs() + (uint64_t)m_upnp.leaseSeconds * 500;
        }
        if (stunAddr.host && now >= keepAt) {
            keepAt = now + 15000, checkAt = now + 3000;
            askStun(stunAddr);
        }
        if (checkAt && now >= checkAt) {
            checkAt = 0;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (coded.Valid() && m_mapped.Valid() && (m_mapped.ip != coded.ip || m_mapped.port != coded.port) && !told) {
                told = true;
                LOGW("Внешний адрес сервера сменился (%s): код приглашения больше не ведёт сюда", m_mapped.ToString().c_str());
            }
        }
    }
}

void Internet::QueuePunch(const std::string& message) {
    // "PUNCH 1.2.3.4:5678"
    if (message.rfind("PUNCH ", 0) != 0) return;
    nat::Endpoint ep;
    if (!nat::Endpoint::Parse(message.substr(6), ep)) return;
    ENetAddress addr{};
    addr.host = ep.ip;
    addr.port = ep.port;
    LOGI("Пробивка NAT к %s", ep.ToString().c_str());
    if (m_turnEnabled) m_turn.AllowPeer(ep.ip);  // TURN forwards only peers we named
    std::lock_guard<std::mutex> lock(m_mutex);
    m_punches.push_back({addr, 10, 0});
}

void Internet::Service() {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t now = NowMs();
    for (auto it = m_punches.begin(); it != m_punches.end();) {
        if (now >= it->nextMs) {
            ENetBuffer buf{sizeof(nat::kPunchMagic) - 1, (void*)nat::kPunchMagic};
            enet_socket_send(m_host->socket, &it->addr, &buf, 1);
            it->nextMs = now + 200;
            if (--it->left <= 0) {
                it = m_punches.erase(it);
                continue;
            }
        }
        ++it;
    }
}

}  // namespace coop::server
