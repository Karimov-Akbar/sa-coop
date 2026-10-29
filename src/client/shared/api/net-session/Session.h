#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/buffer/Buffer.h"
#include "shared/api/net-client/NetClient.h"

namespace coop {

struct PeerInfo {
    uint8_t id = 0;
    std::string name;
    bool isHost = false;
    uint16_t ping = 0;
};

// Network session of the local client: connection lifecycle, handshake, player list and
// routing of game messages to the registered handlers.
class Session {
public:
    enum class State { Offline, Connecting, Online, Failed };
    using Handler = void (*)(uint8_t from, Reader& r);

    void Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password,
                 bool wantsHost);
    void Disconnect(const std::string& reason = "");
    void Update();  // once per frame, game thread

    State GetState() const { return m_state; }
    bool Online() const { return m_state == State::Online; }
    bool IsHost() const { return Online() && m_hostId == m_myId; }
    uint8_t MyId() const { return m_myId; }
    uint8_t HostId() const { return m_hostId; }
    const std::string& FailReason() const { return m_failReason; }
    const std::string& ServerName() const { return m_serverName; }
    bool FriendlyFire() const { return Online() && m_friendlyFire; }  // the host let the players hurt each other
    bool PartnerTeleport() const { return m_partnerTeleport; }  // the host let its partners use /tp and /tphere
    const std::map<uint8_t, PeerInfo>& Peers() const { return m_peers; }
    const PeerInfo* Peer(uint8_t id) const;
    std::string PeerName(uint8_t id) const;
    uint32_t Rtt() const { return m_net.Rtt(); }
    NetClient& Net() { return m_net; }

    void Send(uint8_t channel, const Writer& w) { m_net.Send(channel, w); }
    void SendChat(const std::string& text);

    void SetHandler(uint8_t msgId, Handler h) { m_handlers[msgId] = h; }

    // Callbacks for the game flow
    std::function<void()> onOnline;                         // handshake completed
    std::function<void(const std::string&)> onOffline;      // connection closed (reason)
    std::function<void(const std::vector<uint8_t>&, const std::string&)> onWorld;  // world save received
    std::function<void()> onWorldNone;                      // server has no world yet
    std::function<void()> onWorldNewGame;                   // the host's prologue is over: start a new game, join it
    std::function<void()> onWorldWait;                      // the host plays a new game's prologue: wait
    std::function<void()> onWorldWanted;                    // host: a partner needs a fresh snapshot
    std::function<void(const std::vector<uint8_t>&)> onProfile;
    std::function<void(uint8_t id)> onPeerJoin;
    std::function<void(uint8_t id)> onPeerLeave;
    std::function<void(uint8_t newHost)> onHostChanged;
    std::function<void(const std::string&)> onInvite;      // the server learned its invite code
    const std::string& InviteCode() const { return m_inviteCode; }

    // World transfer progress (0..1) while downloading, -1 when idle.
    float WorldProgress() const { return m_worldExpected ? (float)m_worldRecv / (float)m_worldExpected : -1.0f; }

private:
    void HandleService(uint8_t id, Reader& r);

    NetClient m_net;
    State m_state = State::Offline;
    std::string m_failReason;
    std::string m_nick, m_password;
    bool m_wantsHost = false;
    uint8_t m_myId = 255, m_hostId = 255;
    std::string m_serverName;
    bool m_friendlyFire = false;
    bool m_partnerTeleport = true;
    std::string m_inviteCode;
    std::map<uint8_t, PeerInfo> m_peers;
    Handler m_handlers[256] = {};
    std::vector<NetClient::Packet> m_inbox;
    // world download
    std::vector<uint8_t> m_world;
    uint32_t m_worldExpected = 0, m_worldRecv = 0, m_worldCrc = 0;
    int m_worldRetries = 0;  // damaged worlds asked for again in a row
    std::string m_worldLabel;
};

Session& Net();

}  // namespace coop
