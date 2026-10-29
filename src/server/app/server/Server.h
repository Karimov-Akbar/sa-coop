#pragma once
#include <enet/enet.h>

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/buffer/Buffer.h"
#include "features/internet/Internet.h"

namespace coop::server {

struct Config {
    std::string name = "SA:COOP server";
    std::string password;
    std::string motd = "Добро пожаловать! Чтобы увидеть список команд, напишите /help.";
    bool friendlyFire = false;  // the players can hurt each other
    bool partnerTeleport = true;  // the partners may use /tp and /tphere
    uint16_t port = 7788;
    int maxPlayers = 2;
    std::wstring dataDir;    // profiles/, world/
    std::wstring worldFile;  // optional initial world save (passed by the launcher)
    bool newGame = false;    // the host starts a new game: the stored world belongs to an older one
    std::string hostName;    // nickname that gets the host role (empty = first player)
    bool internet = true;    // UPnP + STUN + invite code + hole punching
    std::string turn;        // optional TURN relay for friends behind any NAT (see TurnRelay)
};

struct Player {
    bool used = false;
    bool joined = false;  // completed the handshake
    ENetPeer* peer = nullptr;
    uint8_t id = 0;
    std::string name;
    bool isHost = false;
    uint8_t status = proto::STATUS_MENU;
    uint64_t connectTime = 0;
    bool follower = false;  // joined the host after a new game's prologue, in a new game of its own: wants the first save
    // plays a world that may not be the host's game now (the stored one, the host loaded another save): wants the next
    bool staleWorld = false;
    // world upload in progress (host only)
    std::vector<uint8_t> upload;
    uint32_t uploadCrc = 0;
    std::string uploadLabel;
};

class Server {
public:
    bool Start(const Config& cfg);
    void Run();  // blocks until Stop()
    void Stop() { m_running = false; }

    // Thread-safe: console commands are queued and executed by the network thread.
    void QueueConsoleCommand(const std::string& line);

private:
    void Service();
    void OnConnect(ENetPeer* peer);
    void OnDisconnect(ENetPeer* peer, bool timeout);
    void OnPacket(ENetPeer* peer, uint8_t channel, const uint8_t* data, size_t size, uint32_t flags);

    void HandleHello(Player& p, Reader& r);
    void HandleChat(Player& p, const std::string& text);
    void HandleCommand(Player* p, const std::string& text);  // p == nullptr for the console
    void HandleWorldBegin(Player& p, Reader& r);
    void HandleWorldChunk(Player& p, Reader& r);
    void HandleWorldEnd(Player& p);
    void SendWorld(Player& p);
    void RequestWorld(Player& p);
    void FlushWorldRequests(bool fresh);  // fresh: the host's upload of now, else the stored world
    void HostStartedNewGame();
    void FollowersJoin();
    void TellToWait(Player& p);
    void SendProfile(Player& p);
    void SaveProfile(Player& p, const std::vector<uint8_t>& data);
    void WriteProfiles();      // the profile writer's thread
    void WaitProfileWrites();  // returns once what is queued is on the disk
    void ResetProfiles();

    void Relay(Player& from, uint8_t channel, const uint8_t* data, size_t size, uint32_t flags);
    // to == nullptr: everyone. The server's own texts come in both languages (en empty: the same in English).
    void SendChat(Player* to, uint8_t fromId, uint32_t rgba, const std::string& text, const std::string& en = {});
    void Broadcast(uint8_t channel, const Writer& w, int exceptId = -1);
    void Kick(Player& p, const std::string& reason, const std::string& reasonEn = {});
    void Leave(Player& p, proto::LeaveReason why, const std::string& reason = {});  // frees the slot, tells the others
    void EnsureHost();
    void SendPings();

    Player* FindByPeer(ENetPeer* peer);
    Player* FindById(int id);
    Player* FindByName(const std::string& name);
    int JoinedCount() const;

    std::wstring ProfilePath(const std::string& name) const;
    std::wstring WorldPath() const { return m_cfg.dataDir + L"world\\world.b"; }
    bool LoadWorld(const std::wstring& path, const std::string& label);

    void SendInvite(Player* to);  // to == nullptr: everyone

    Config m_cfg;
    ENetHost* m_host = nullptr;
    Internet m_internet;
    std::array<Player, proto::kMaxPlayers> m_players{};
    volatile bool m_running = false;
    std::vector<uint8_t> m_world;  // current world save
    std::string m_worldLabel;
    uint64_t m_lastPings = 0;
    std::vector<uint8_t> m_worldWaiters;   // player ids waiting for the world save
    uint64_t m_worldWantedTime = 0;        // when the host was asked for a fresh snapshot (0 = not asked)
    bool m_prologue = false;               // the host plays a new game's prologue: partners wait for its end

    std::mutex m_cmdMutex;
    std::vector<std::string> m_cmdQueue;

    std::mutex m_writeMutex;
    std::map<std::wstring, std::vector<uint8_t>> m_profileWrites;  // the latest for each file, to be written
    bool m_writing = false;                                         // the writer's thread runs
};

}  // namespace coop::server
