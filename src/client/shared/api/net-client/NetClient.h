#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/lib/buffer/Buffer.h"

namespace coop {

// ENet client running in its own thread. The game thread enqueues outgoing packets and polls
// incoming ones once per frame, so network latency never stalls rendering.
class NetClient {
public:
    enum class State { Idle, Connecting, Connected, Disconnected };

    struct Packet {
        uint8_t channel;
        std::vector<uint8_t> data;
    };

    ~NetClient() { Stop(); }

    bool Start(const std::string& host, uint16_t port);
    void Stop();  // graceful disconnect

    void Send(uint8_t channel, const Writer& w);
    void Send(uint8_t channel, const uint8_t* data, size_t size);
    // Moves all received packets into `out`. Connection events are reported through State().
    void Poll(std::vector<Packet>& out);

    State GetState() const { return m_state.load(); }
    uint32_t Rtt() const { return m_rtt.load(); }
    // Set when a Connected -> Disconnected transition happened (cleared by the caller).
    bool TakeDisconnectEvent() { return m_disconnectEvent.exchange(false); }
    bool TakeConnectEvent() { return m_connectEvent.exchange(false); }
    uint64_t BytesIn() const { return m_bytesIn.load(); }
    uint64_t BytesOut() const { return m_bytesOut.load(); }
    // the last connect by an invite code could not reach its channel (ntfy.sh): the host never heard of us
    bool SignalFailed() const;
    bool ViaCode() const;  // the last connection went by an invite code

private:
    void ThreadMain(std::string host, uint16_t port);

    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    std::atomic<State> m_state{State::Idle};
    std::atomic<uint32_t> m_rtt{0};
    std::atomic<bool> m_disconnectEvent{false};
    std::atomic<bool> m_connectEvent{false};
    std::atomic<uint64_t> m_bytesIn{0}, m_bytesOut{0};

    std::mutex m_outMutex;
    std::vector<Packet> m_out;
    std::mutex m_inMutex;
    std::vector<Packet> m_in;
};

}  // namespace coop
