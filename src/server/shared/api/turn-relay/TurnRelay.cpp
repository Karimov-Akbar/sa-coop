#include "shared/api/turn-relay/TurnRelay.h"

#include <bcrypt.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <random>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop::server {

namespace {
constexpr uint32_t kMagic = 0x2112A442;
enum : uint16_t { ALLOCATE = 0x003, REFRESH = 0x004, SEND_IND = 0x016, DATA_IND = 0x017, CREATE_PERMISSION = 0x008 };
enum : uint16_t {
    A_USERNAME = 0x0006,
    A_INTEGRITY = 0x0008,
    A_ERROR = 0x0009,
    A_LIFETIME = 0x000D,
    A_PEER = 0x0012,
    A_DATA = 0x0013,
    A_REALM = 0x0014,
    A_NONCE = 0x0015,
    A_RELAYED = 0x0016,
    A_TRANSPORT = 0x0019,
};
using Bytes = std::vector<uint8_t>;

void BigBuffers(SOCKET s) {  // a burst of the server's datagrams overflowed the default buffers and was dropped
    int size = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(size));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&size, sizeof(size));
}

Bytes Digest(LPCWSTR alg, const Bytes* hmacKey, const void* data, size_t len) {
    Bytes out;
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    DWORD size = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, hmacKey ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) == 0 &&
        BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&size, sizeof(size), &got, 0) == 0 &&
        BCryptCreateHash(a, &h, nullptr, 0, hmacKey ? (PUCHAR)hmacKey->data() : nullptr,
                         hmacKey ? (ULONG)hmacKey->size() : 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0) == 0) {
        out.resize(size);
        if (BCryptFinishHash(h, out.data(), size, 0) != 0) out.clear();
    }
    if (h) BCryptDestroyHash(h);
    if (a) BCryptCloseAlgorithmProvider(a, 0);
    return out;
}

Bytes Attr(uint16_t type, const void* v, size_t n) {
    Bytes a(4 + n + (4 - n % 4) % 4, 0);
    a[0] = (uint8_t)(type >> 8), a[1] = (uint8_t)type, a[2] = (uint8_t)(n >> 8), a[3] = (uint8_t)n;
    memcpy(a.data() + 4, v, n);
    return a;
}
Bytes AttrStr(uint16_t type, const std::string& s) { return Attr(type, s.data(), s.size()); }
Bytes AttrU32(uint16_t type, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return Attr(type, b, 4);
}
Bytes XorAddr(uint16_t type, const sockaddr_in& a) {
    uint16_t port = ntohs(a.sin_port) ^ (uint16_t)(kMagic >> 16);
    uint32_t ip = ntohl(a.sin_addr.s_addr) ^ kMagic;
    uint8_t v[8] = {0, 1, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip};
    return Attr(type, v, 8);
}
bool ParseXorAddr(const Bytes& v, sockaddr_in& out) {
    if (v.size() < 8 || v[1] != 1) return false;
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons((uint16_t)(((v[2] << 8) | v[3]) ^ (kMagic >> 16)));
    out.sin_addr.s_addr = htonl(((uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7]) ^ kMagic);
    return true;
}

struct Parsed {
    uint16_t type = 0;
    uint8_t tx[12] = {};
    std::map<uint16_t, Bytes> attrs;
};
bool Parse(const uint8_t* d, size_t n, Parsed& p) {
    if (n < 20 || (d[0] & 0xC0) || ((uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]) != kMagic) return false;
    size_t len = (size_t)(d[2] << 8 | d[3]);
    if (20 + len > n) return false;
    p.type = (uint16_t)(d[0] << 8 | d[1]);
    memcpy(p.tx, d + 8, 12);
    for (size_t i = 20; i + 4 <= 20 + len;) {
        uint16_t t = (uint16_t)(d[i] << 8 | d[i + 1]), l = (uint16_t)(d[i + 2] << 8 | d[i + 3]);
        if (i + 4 + l > 20 + len) return false;
        p.attrs[t] = Bytes(d + i + 4, d + i + 4 + l);
        i += 4 + l + (4 - l % 4) % 4;
    }
    return true;
}
int ErrorCode(const Parsed& p) {
    auto it = p.attrs.find(A_ERROR);
    return it == p.attrs.end() || it->second.size() < 4 ? 0 : (it->second[2] & 7) * 100 + it->second[3];
}

// header + attributes, MESSAGE-INTEGRITY over everything before it (length already counting it)
Bytes Build(uint16_t type, const uint8_t tx[12], const std::vector<Bytes>& attrs, const Bytes* key) {
    Bytes m(20, 0);
    m[0] = (uint8_t)(type >> 8), m[1] = (uint8_t)type;
    m[4] = 0x21, m[5] = 0x12, m[6] = 0xA4, m[7] = 0x42;
    memcpy(m.data() + 8, tx, 12);
    for (const Bytes& a : attrs) m.insert(m.end(), a.begin(), a.end());
    auto setLen = [&](size_t n) { m[2] = (uint8_t)(n >> 8), m[3] = (uint8_t)n; };
    if (key) {
        setLen(m.size() - 20 + 24);
        Bytes mac = Digest(BCRYPT_SHA1_ALGORITHM, key, m.data(), m.size());
        Bytes a = Attr(A_INTEGRITY, mac.data(), mac.size());
        m.insert(m.end(), a.begin(), a.end());
    }
    setLen(m.size() - 20);
    return m;
}

void NewTx(uint8_t tx[12]) {
    static std::mt19937 rng{std::random_device{}()};
    for (int i = 0; i < 12; ++i) tx[i] = (uint8_t)rng();
}

// value of "key":"..." inside one JSON object
std::string JsonStr(const std::string& obj, const std::string& key) {
    size_t k = obj.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    size_t q = obj.find('"', obj.find(':', k) + 1);
    size_t e = q == std::string::npos ? q : obj.find('"', q + 1);
    return e == std::string::npos ? std::string() : obj.substr(q + 1, e - q - 1);
}
}  // namespace

void TurnRelay::Start(const std::string& spec, uint16_t gamePort) {
    m_spec = spec;
    m_gamePort = gamePort;
    m_quit = false;
    m_thread = std::thread(&TurnRelay::Thread, this);
}

void TurnRelay::Stop() {
    m_quit = true;
    if (m_thread.joinable()) m_thread.join();
}

void TurnRelay::AllowPeer(uint32_t ipNet) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_allowed.insert(ipNet);
}

nat::Endpoint TurnRelay::Relayed() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_relayed;
}

bool TurnRelay::Credentials() {
    std::string port = "3478";
    if (m_spec.rfind("metered:", 0) == 0) {
        // free metered.ca account: the REST API hands out TURN credentials for the app
        size_t c = m_spec.find(':', 8);
        if (c == std::string::npos) return false;
        std::string app = m_spec.substr(8, c - 8), key = m_spec.substr(c + 1);
        nat::HttpResponse r = nat::HttpRequest("GET", "https://" + app + ".metered.live/api/v1/turn/credentials?apiKey=" + key);
        if (r.status != 200) {
            LOGW("TURN: metered.ca не выдал учётные данные (HTTP %d)", r.status);
            return false;
        }
        for (size_t at = 0; (at = r.body.find('{', at)) != std::string::npos; ++at) {
            std::string obj = r.body.substr(at, r.body.find('}', at) - at);
            std::string url = JsonStr(obj, "urls");
            if (url.rfind("turn:", 0) != 0 || url.find("transport=tcp") != std::string::npos) continue;
            std::string hostPort = url.substr(5, url.find('?') - 5);
            size_t colon = hostPort.rfind(':');
            m_host = hostPort.substr(0, colon);
            if (colon != std::string::npos) port = hostPort.substr(colon + 1);
            m_user = JsonStr(obj, "username");
            m_password = JsonStr(obj, "credential");
            break;
        }
    } else if (m_spec.rfind("turn:", 0) == 0) {
        std::vector<std::string> p = Split(m_spec.substr(5), ':');
        if (p.size() < 4) return false;
        m_host = p[0], port = p[1], m_user = p[2];
        m_password = m_spec.substr(5 + p[0].size() + p[1].size() + p[2].size() + 3);  // may contain ':'
    }
    if (m_host.empty() || m_user.empty()) {
        LOGW("TURN: непонятная настройка turn = %s", m_spec.c_str());
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(m_host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        LOGW("TURN: сервер %s не найден", m_host.c_str());
        return false;
    }
    m_server = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    return true;
}

bool TurnRelay::Request(uint16_t method, const std::vector<Bytes>& attrs, Bytes* response) {
    for (int attempt = 0; attempt < 3 && !m_quit; ++attempt) {
        uint8_t tx[12];
        NewTx(tx);
        std::vector<Bytes> all = attrs;
        bool auth = !m_key.empty();
        if (auth) {
            all.push_back(AttrStr(A_USERNAME, m_user));
            all.push_back(AttrStr(A_REALM, m_realm));
            all.push_back(AttrStr(A_NONCE, m_nonce));
        }
        Bytes msg = Build(method, tx, all, auth ? &m_key : nullptr);
        sendto(m_sock, (const char*)msg.data(), (int)msg.size(), 0, (sockaddr*)&m_server, sizeof(m_server));
        uint64_t deadline = NowMs() + 2000;
        while (NowMs() < deadline && !m_quit) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(m_sock, &fds);
            timeval tv{0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            uint8_t buf[2048];
            int n = recv(m_sock, (char*)buf, sizeof(buf), 0);
            Parsed p;
            if (n <= 0 || !Parse(buf, (size_t)n, p)) continue;
            if (memcmp(p.tx, tx, 12)) {
                OnTurnPacket(buf, (size_t)n);  // relayed data keeps flowing while we wait
                continue;
            }
            if ((p.type & 0x0110) == 0x0100) {  // success response
                if (response) response->assign(buf, buf + n);
                return true;
            }
            int code = ErrorCode(p);
            if ((code == 401 || code == 438) && p.attrs.count(A_NONCE)) {  // (new) challenge: answer it
                m_nonce.assign(p.attrs[A_NONCE].begin(), p.attrs[A_NONCE].end());
                if (p.attrs.count(A_REALM)) m_realm.assign(p.attrs[A_REALM].begin(), p.attrs[A_REALM].end());
                std::string k = m_user + ":" + m_realm + ":" + m_password;
                m_key = Digest(BCRYPT_MD5_ALGORITHM, nullptr, k.data(), k.size());
                if (code == 401 && auth) {
                    LOGW("TURN: неверные учётные данные");
                    return false;
                }
                break;  // resend with credentials
            }
            LOGW("TURN: запрос %04X отклонён, ошибка %d", method, code);
            return false;
        }
    }
    return false;
}

void TurnRelay::SendIndication(const sockaddr_in& peer, const uint8_t* data, size_t len) {
    uint8_t tx[12];
    NewTx(tx);
    Bytes msg = Build(SEND_IND, tx, {XorAddr(A_PEER, peer), Attr(A_DATA, data, len)}, nullptr);
    sendto(m_sock, (const char*)msg.data(), (int)msg.size(), 0, (sockaddr*)&m_server, sizeof(m_server));
}

void TurnRelay::OnTurnPacket(const uint8_t* data, size_t len) {
    Parsed p;
    sockaddr_in from;
    if (!Parse(data, len, p) || p.type != DATA_IND || !p.attrs.count(A_DATA) || !ParseXorAddr(p.attrs[A_PEER], from)) return;
    uint64_t key = (uint64_t)ntohl(from.sin_addr.s_addr) << 16 | ntohs(from.sin_port);
    Peer& peer = m_peers[key];
    if (peer.local == INVALID_SOCKET) {
        // a new friend: its own socket towards the game port, ENet sees it as 127.0.0.1:<port>
        peer.local = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BigBuffers(peer.local);
        sockaddr_in game{};
        game.sin_family = AF_INET;
        game.sin_port = htons(m_gamePort);
        game.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(peer.local, (sockaddr*)&game, sizeof(game));
        peer.addr = from;
        peer.reportMs = NowMs();
        char ip[32];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        LOGI("TURN: игрок %s:%u подключается через ретранслятор", ip, ntohs(from.sin_port));
    }
    peer.lastMs = NowMs();
    const Bytes& d = p.attrs[A_DATA];
    send(peer.local, (const char*)d.data(), (int)d.size(), 0);
    ++peer.fromCount;
    peer.fromBytes += d.size();
}

void TurnRelay::RefreshPermissions() {
    std::set<uint32_t> want;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        want = m_allowed;
    }
    if (want.empty()) return;
    std::vector<Bytes> attrs;
    for (uint32_t ip : want) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ip;
        attrs.push_back(XorAddr(A_PEER, a));
    }
    if (Request(CREATE_PERMISSION, attrs, nullptr)) m_permitted = want;
}

void TurnRelay::Thread() {
    if (!Credentials()) return;
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BigBuffers(m_sock);
    Bytes resp;
    if (!Request(ALLOCATE, {Attr(A_TRANSPORT, "\x11\0\0\0", 4)}, &resp)) {
        LOGW("TURN: не удалось получить адрес ретранслятора у %s", m_host.c_str());
        closesocket(m_sock);
        return;
    }
    Parsed p;
    sockaddr_in relayed{};
    Parse(resp.data(), resp.size(), p);
    if (p.attrs.count(A_LIFETIME) && p.attrs[A_LIFETIME].size() == 4) {
        const Bytes& l = p.attrs[A_LIFETIME];
        m_lifetime = (uint32_t)l[0] << 24 | l[1] << 16 | l[2] << 8 | l[3];
    }
    if (!ParseXorAddr(p.attrs[A_RELAYED], relayed)) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_relayed.ip = relayed.sin_addr.s_addr;
        m_relayed.port = ntohs(relayed.sin_port);
        LOGI("TURN: адрес ретранслятора %s (сервер %s)", m_relayed.ToString().c_str(), m_host.c_str());
    }

    uint64_t refreshAt = NowMs() + std::max<uint32_t>(m_lifetime, 120) * 500, permissionsAt = NowMs() + 240000;
    while (!m_quit) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_sock, &fds);
        for (auto& [k, peer] : m_peers) FD_SET(peer.local, &fds);
        timeval tv{0, 100000};
        if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
            uint8_t buf[4096];
            if (FD_ISSET(m_sock, &fds)) {
                int n = recv(m_sock, (char*)buf, sizeof(buf), 0);
                if (n > 0) OnTurnPacket(buf, (size_t)n);
            }
            for (auto& [k, peer] : m_peers) {
                if (!FD_ISSET(peer.local, &fds)) continue;
                int n = recv(peer.local, (char*)buf, sizeof(buf), 0);
                if (n <= 0) continue;
                SendIndication(peer.addr, buf, (size_t)n);  // game -> friend
                ++peer.toCount;
                peer.toBytes += (uint64_t)n;
                peer.toMax = std::max(peer.toMax, (uint32_t)n);
            }
        }
        uint64_t now = NowMs();
        for (auto& [k, peer] : m_peers) {
            if (now - peer.reportMs < 30000 || !(peer.toCount || peer.fromCount)) continue;
            char ip[32];
            inet_ntop(AF_INET, &peer.addr.sin_addr, ip, sizeof(ip));
            LOGI("TURN: %s:%u за %u с: другу %u пакетов (%llu КБ, до %u байт), от друга %u (%llu КБ)", ip,
                 ntohs(peer.addr.sin_port), (unsigned)((now - peer.reportMs) / 1000), peer.toCount, peer.toBytes / 1024,
                 peer.toMax, peer.fromCount, peer.fromBytes / 1024);
            peer.toCount = peer.fromCount = peer.toMax = 0;
            peer.toBytes = peer.fromBytes = 0;
            peer.reportMs = now;
        }
        bool newPeers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            newPeers = m_allowed != m_permitted;
        }
        if (newPeers || now >= permissionsAt) {  // permissions live 300 s
            RefreshPermissions();
            permissionsAt = now + 240000;
        }
        if (now >= refreshAt) {
            Request(REFRESH, {AttrU32(A_LIFETIME, m_lifetime)}, nullptr);
            refreshAt = now + std::max<uint32_t>(m_lifetime, 120) * 500;
        }
        // Blocking TURN requests can dispatch packets and advance peer.lastMs.
        // Refresh the clock before checking inactivity to avoid unsigned underflow.
        now = NowMs();
        for (auto it = m_peers.begin(); it != m_peers.end();) {  // friends that left
            if (now >= it->second.lastMs && now - it->second.lastMs > 120000) {
                closesocket(it->second.local);
                it = m_peers.erase(it);
            } else {
                ++it;
            }
        }
    }
    Request(REFRESH, {AttrU32(A_LIFETIME, 0)}, nullptr);  // give the allocation back
    for (auto& [k, peer] : m_peers) closesocket(peer.local);
    m_peers.clear();
    closesocket(m_sock);
}

}  // namespace coop::server
