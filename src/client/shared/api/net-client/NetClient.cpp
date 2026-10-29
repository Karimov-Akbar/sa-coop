#include "shared/api/net-client/NetClient.h"

#include <enet/enet.h>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/api/net/Net.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "shared/config/Config.h"

namespace coop {

namespace {
// joining by invite code: STUN answers and the host's punch packets come to our ENet socket
uint8_t g_stunTx[12];
nat::Endpoint g_mapped;
std::atomic<bool> g_stunWaiting{false};
ENetAddress g_lanServer{};  // answer to the LAN discovery
std::atomic<bool> g_signalFailed{false};  // the invite code's channel (ntfy.sh) did not take our address
std::atomic<bool> g_viaCode{false};       // the last connection went by an invite code

int ENET_CALLBACK Intercept(ENetHost* h, ENetEvent*) {
    const uint8_t* d = h->receivedData;
    size_t n = h->receivedDataLength;
    if (n >= sizeof(nat::kPunchMagic) - 1 && !memcmp(d, nat::kPunchMagic, sizeof(nat::kPunchMagic) - 1)) return 1;
    size_t hereLen = sizeof(nat::kHereMagic) - 1;
    if (n > hereLen && n < hereLen + 6 && !memcmp(d, nat::kHereMagic, hereLen)) {
        g_lanServer.host = h->receivedAddress.host;
        g_lanServer.port = (enet_uint16)atoi(std::string((const char*)d + hereLen, n - hereLen).c_str());
        return 1;
    }
    if (!nat::IsStunPacket(d, n)) return 0;
    nat::Endpoint ep;
    if (g_stunWaiting && nat::ParseStunResponse(d, n, g_stunTx, ep)) {
        g_mapped = ep;
        g_stunWaiting = false;
    }
    return 1;
}

// Tells the host (through the signalling channel) where our socket is seen from the internet, so it can
// open its NAT for us, and opens ours towards it. A host with UPnP is reachable without this.
// The host's TURN relay (if it runs one) is announced on the same channel: "RELAY ip:port".
ENetAddress FindRelay(const std::string& code) {
    ENetAddress relay{};
    nat::HttpResponse r =
        nat::HttpRequest("GET", std::string(nat::kDefaultSignalUrl) + "/" + nat::SignalTopic(code) + "/json?poll=1&since=all");
    for (size_t at = 0; r.status == 200 && (at = r.body.find("\"message\":\"RELAY ", at)) != std::string::npos; ++at) {
        size_t start = at + 17, end = r.body.find('"', start);
        nat::Endpoint ep;
        if (end != std::string::npos && nat::Endpoint::Parse(r.body.substr(start, end - start), ep)) relay = {ep.ip, ep.port};
    }
    return relay;
}

void PrepareNat(ENetHost* client, ENetAddress& server, ENetAddress& relay, const std::string& code) {
    client->intercept = &Intercept;
    g_mapped = {};
    g_signalFailed = false;
    for (const std::string& s : nat::StunServers()) {
        size_t colon = s.rfind(':');
        ENetAddress stun{};
        if (enet_address_set_host(&stun, s.substr(0, colon).c_str()) != 0) continue;
        stun.port = (enet_uint16)atoi(s.c_str() + colon + 1);
        uint8_t req[20];
        nat::BuildStunRequest(req, g_stunTx);
        g_stunWaiting = true;
        ENetBuffer buf{sizeof(req), req};
        enet_socket_send(client->socket, &stun, &buf, 1);
        ENetEvent ev;
        for (int i = 0; i < 30 && g_stunWaiting; ++i) enet_host_service(client, &ev, 50);
        if (g_mapped.Valid()) break;
    }
    g_stunWaiting = false;
    if (g_mapped.Valid()) {
        bool ok = nat::SignalPublish(nat::kDefaultSignalUrl, nat::SignalTopic(code), "PUNCH " + g_mapped.ToString());
        g_signalFailed = !ok;
        LOGI("Наш внешний адрес: %s. Хост %s", g_mapped.ToString().c_str(), ok ? "оповещён" : "не оповещён (нет связи с ntfy)");
        if (ok) relay = FindRelay(code);  // the host allows our address on its relay when it gets the message
        if (relay.host) LOGI("У хоста есть ретранслятор: %s", nat::Endpoint{relay.host, relay.port}.ToString().c_str());
    } else {
        LOGW("Внешний адрес не определён (STUN недоступен). Подключаемся напрямую");
    }
    if (g_mapped.Valid() && g_mapped.ip == server.host) {
        // the same public address as the host: most NATs cannot loop back, look for it in our network
        g_lanServer = {};
        enet_socket_set_option(client->socket, ENET_SOCKOPT_BROADCAST, 1);
        ENetBuffer disc{sizeof(nat::kDiscoverMagic) - 1, (void*)nat::kDiscoverMagic};
        for (enet_uint16 port : {(enet_uint16)7788, server.port}) {
            ENetAddress bc{ENET_HOST_BROADCAST, port};
            enet_socket_send(client->socket, &bc, &disc, 1);
        }
        ENetEvent ev;
        for (int i = 0; i < 20 && !g_lanServer.host; ++i) enet_host_service(client, &ev, 50);
        if (g_lanServer.host) {
            char ip[64];
            enet_address_get_host_ip(&g_lanServer, ip, sizeof(ip));
            LOGI("Хост найден в локальной сети: %s:%u", ip, g_lanServer.port);
            server = g_lanServer;
            return;
        }
    }
    ENetBuffer punch{sizeof(nat::kPunchMagic) - 1, (void*)nat::kPunchMagic};
    for (int i = 0; i < 3; ++i) enet_socket_send(client->socket, &server, &punch, 1);
}
}  // namespace

bool NetClient::Start(const std::string& host, uint16_t port) {
    Stop();
    if (!net::Init()) {
        LOGE("enet init failed");
        return false;
    }
    m_quit = false;
    m_state = State::Connecting;
    m_thread = std::thread(&NetClient::ThreadMain, this, host, port);
    return true;
}

void NetClient::Stop() {
    if (m_thread.joinable()) {
        m_quit = true;
        m_thread.join();
        net::Shutdown();
    }
    m_state = State::Idle;
    std::lock_guard<std::mutex> a(m_outMutex);
    m_out.clear();
    // what came after the last poll (a refusal's reason, a world chunk) belonged to that connection: it was handled
    // in the next one ("the world arrived damaged")
    std::lock_guard<std::mutex> b(m_inMutex);
    m_in.clear();
}

void NetClient::Send(uint8_t channel, const Writer& w) { Send(channel, w.Data(), w.Size()); }

void NetClient::Send(uint8_t channel, const uint8_t* data, size_t size) {
    if (m_state != State::Connected || size == 0) return;
    std::lock_guard<std::mutex> lock(m_outMutex);
    m_out.push_back(Packet{channel, std::vector<uint8_t>(data, data + size)});
}

void NetClient::Poll(std::vector<Packet>& out) {
    std::lock_guard<std::mutex> lock(m_inMutex);
    if (m_in.empty()) return;
    if (out.empty()) {
        out.swap(m_in);
    } else {
        for (auto& p : m_in) out.push_back(std::move(p));
        m_in.clear();
    }
}

bool NetClient::SignalFailed() const { return g_signalFailed; }
bool NetClient::ViaCode() const { return g_viaCode; }

void NetClient::ThreadMain(std::string host, uint16_t port) {
    ENetHost* client = enet_host_create(nullptr, 1, proto::CH_COUNT, 0, 0);
    if (!client) {
        LOGE("enet_host_create failed");
        m_state = State::Disconnected;
        m_disconnectEvent = true;
        return;
    }
    enet_host_compress_with_range_coder(client);
    client->mtu = net::kMtu;
    ENetAddress addr{}, relay{};
    nat::Endpoint codeEp;
    uint8_t key = 0;
    g_viaCode = nat::ParseInviteCode(host, codeEp, key);
    if (g_viaCode) {
        addr.host = codeEp.ip;
        addr.port = codeEp.port;
        LOGI("Код приглашения: хост %s", codeEp.ToString().c_str());
        PrepareNat(client, addr, relay, host);
    } else if (enet_address_set_host(&addr, host.c_str()) != 0) {
        LOGE("Не удалось определить адрес сервера \"%s\"", host.c_str());
        enet_host_destroy(client);
        m_state = State::Disconnected;
        m_disconnectEvent = true;
        return;
    }
    if (!codeEp.Valid()) addr.port = port;
    // direct (or punched) first; the relay when that does not answer within 4 s
    bool useRelay = relay.host && Cfg().forceRelay;
    uint64_t relayAt = relay.host && !useRelay ? GetTickCount64() + 4000 : 0;
    auto viaRelay = [client] {  // what the connect handshake tells the server (net::kRelayMtu)
        client->mtu = net::kRelayMtu;
        enet_host_bandwidth_limit(client, net::kRelayBandwidth, 0);
    };
    if (useRelay) viaRelay();
    ENetPeer* peer = enet_host_connect(client, useRelay ? &relay : &addr, proto::CH_COUNT, 0);
    if (!peer) {
        enet_host_destroy(client);
        m_state = State::Disconnected;
        m_disconnectEvent = true;
        return;
    }
    // a lossy line (2-6 %) dropped the connection now and then: 15 s before a silent peer counts as gone, 20 s at most
    enet_peer_timeout(peer, 0, 15000, 20000);
    LOGI("Подключение к %s:%u...", host.c_str(), port);

    std::vector<Packet> outLocal;
    bool connected = false;
    // the connection just before it ended, for the log: ENet resets the peer before it reports the end
    enet_uint32 lastLoss = 0, lastInTransit = 0, lastHeard = 0;
    while (!m_quit) {
        // flush outgoing
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            outLocal.swap(m_out);
        }
        if (connected) {
            for (auto& p : outLocal) {
                ENetPacket* pkt = net::MakePacket(p.data.data(), p.data.size(), p.channel);
                if (enet_peer_send(peer, p.channel, pkt) != 0) enet_packet_destroy(pkt);
                m_bytesOut += p.data.size();
            }
        }
        outLocal.clear();

        if (!connected && relayAt && (GetTickCount64() >= relayAt || peer->state == ENET_PEER_STATE_DISCONNECTED)) {
            LOGI("Напрямую сервер не отвечает. Подключаемся через ретранслятор");
            enet_peer_reset(peer);
            viaRelay();
            peer = enet_host_connect(client, &relay, proto::CH_COUNT, 0);
            relayAt = 0;
            if (!peer) break;
            enet_peer_timeout(peer, 0, 15000, 20000);
        }

        ENetEvent ev;
        int rc = enet_host_service(client, &ev, 2);
        while (rc > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    connected = true;
                    m_state = State::Connected;
                    m_connectEvent = true;
                    LOGI("Соединение с сервером установлено (MTU %u)", peer->mtu);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (!connected && relayAt) {
                        relayAt = 1;  // the direct attempt gave up: switch to the relay above
                        break;
                    }
                    LOGW("Соединение с сервером %s", connected ? "потеряно" : "не установлено");
                    if (connected)
                        LOGI("Перед разрывом: пинг %u мс, потери %.1f%%, без подтверждения %u байт, сервер молчал %u мс, код %u",
                             m_rtt.load(), lastLoss * 100.0 / (double)ENET_PEER_PACKET_LOSS_SCALE, lastInTransit,
                             enet_time_get() - lastHeard, ev.data);
                    connected = false;
                    m_state = State::Disconnected;
                    m_disconnectEvent = true;
                    m_quit = true;
                    break;
                case ENET_EVENT_TYPE_RECEIVE: {
                    m_bytesIn += ev.packet->dataLength;
                    Packet p{ev.channelID, std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength)};
                    enet_packet_destroy(ev.packet);
                    std::lock_guard<std::mutex> lock(m_inMutex);
                    // a game that stands (a long load, a hang, alt-tab without run_in_background) piled up every state
                    // and applied them all at once, stale: past a few thousand only the reliable ones are kept
                    if (m_in.size() > 4000 && (p.channel == proto::CH_STATE || p.channel == proto::CH_VOICE)) break;
                    m_in.push_back(std::move(p));
                    break;
                }
                default:
                    break;
            }
            if (m_quit) break;
            rc = enet_host_check_events(client, &ev);
        }
        if (connected) {
            m_rtt = peer->roundTripTime;
            lastLoss = peer->packetLoss;
            lastInTransit = peer->reliableDataInTransit;
            lastHeard = peer->lastReceiveTime;
        }
    }

    if (connected) {
        // what the game queued last goes first (the partner's profile on leaving): enet_peer_disconnect drops the
        // queues, enet_peer_disconnect_later sends them and then disconnects
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            outLocal.swap(m_out);
        }
        for (auto& p : outLocal) {
            ENetPacket* pkt = net::MakePacket(p.data.data(), p.data.size(), p.channel);
            if (enet_peer_send(peer, p.channel, pkt) != 0) enet_packet_destroy(pkt);
        }
        outLocal.clear();
        // graceful quit: data = 1 tells the server it was not a timeout
        enet_peer_disconnect_later(peer, 1);
        ENetEvent ev;
        uint64_t deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline && enet_host_service(client, &ev, 50) >= 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
        enet_peer_reset(peer);
    }
    enet_host_destroy(client);
    if (m_state != State::Disconnected) {
        m_state = State::Disconnected;
    }
}

}  // namespace coop
