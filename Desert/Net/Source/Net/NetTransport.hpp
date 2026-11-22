#pragma once

// INetTransport: the pipe under the engine's networking (Docs/contracts/NET-ARCH.md §1) -- connections, reliable
// and unreliable messages, the lag/loss simulator. The UE split is NetDriver (ours: replication, ids, relevancy,
// above this interface) over a socket subsystem (this). Nothing above sees the transport library's types, so the
// implementation (GameNetworkingSockets today, CreateGnsTransport) can be replaced without touching replication.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Net
{
    // One connection as the transport names it, unique within the process for the transport's lifetime.
    using ConnectionId = std::uint32_t;
    inline constexpr ConnectionId kInvalidConnection = 0;

    enum class ESendMode : std::uint8_t
    {
        Reliable,   // ordered, retransmitted until acknowledged
        Unreliable, // may be lost; never retransmitted
    };

    // How a connection ended. Codes below kAppCloseCodeMin are the transport's own; the protocol above uses
    // kAppCloseCodeMin + its reason (NetHandshake.hpp ERefuseReason).
    inline constexpr int kAppCloseCodeMin = 1000;
    struct DisconnectInfo
    {
        int Code = 0;
        std::string Reason;
        bool ClosedByPeer = false; // false: a problem detected locally (timeout, unreachable)
    };

    struct ConnectionStats
    {
        int PingMs = -1;           // smoothed round trip; -1 while unknown
        float QualityLocal = -1.f; // fraction of packets delivered to us, 0..1; -1 while unknown
        float QualityRemote = -1.f;
        float OutBytesPerSecond = 0.f;
        float InBytesPerSecond = 0.f;
        int PendingReliableBytes = 0;
    };

    // The network condition simulator, applied to every packet THIS process sends (so two processes with
    // SendLagMs 75 see a 150 ms round trip). All zero = off. Development only: a shipping server never sets it.
    struct NetSimulator
    {
        int SendLagMs = 0;
        float SendLossPercent = 0.f;
        float SendJitterAverageMs = 0.f;
    };

    class INetTransportEvents
    {
    public:
        virtual ~INetTransportEvents() = default;
        // The connection is up: a client's Connect completed, or a listener accepted a peer.
        virtual void OnConnected(ConnectionId connection, std::string_view address) = 0;
        // The connection is gone (peer closed it, or it timed out). Not called for connections closed by Close().
        virtual void OnClosed(ConnectionId connection, const DisconnectInfo& info) = 0;
        // One whole message, as sent (the transport fragments and reassembles). The span is valid during the call.
        virtual void OnMessage(ConnectionId connection, std::span<const std::byte> data) = 0;
    };

    class INetTransport
    {
    public:
        virtual ~INetTransport() = default;
        // Accept connections on every local address at `port`. False + `error` when the port cannot be bound.
        virtual bool Listen(std::uint16_t port, std::string& error) = 0;
        // Start connecting to "ip:port". kInvalidConnection + `error` when the address does not parse.
        virtual ConnectionId Connect(std::string_view address, std::string& error) = 0;
        virtual bool Send(ConnectionId connection, std::span<const std::byte> data, ESendMode mode) = 0;
        // Deliver every pending connection change and message to `events`, on the calling thread.
        virtual void Poll(INetTransportEvents& events) = 0;
        // Close with a code and reason the peer receives in OnClosed; `flush` first delivers what is queued reliably.
        virtual void Close(ConnectionId connection, int code, std::string_view reason, bool flush) = 0;
        virtual ConnectionStats GetStats(ConnectionId connection) const = 0;
        // False + `error` naming the setting the transport refused.
        virtual bool SetSimulator(const NetSimulator& simulator, std::string& error) = 0;
    };

    // Receives the transport library's own diagnostics, one line each.
    using NetLogSink = std::function<void(std::string_view line)>;

    // The GameNetworkingSockets transport. One per process (the library is process-global). nullptr when the
    // library fails to initialise; the reason went to `log`.
    std::unique_ptr<INetTransport> CreateGnsTransport(NetLogSink log);
} // namespace Desert::Net
