#pragma once

// INetTransport: the pipe under the engine's networking (Docs/contracts/NET-ARCH.md §1) -- connections, reliable
// and unreliable messages, the lag/loss simulator. The UE split is NetDriver (ours: replication, ids, relevancy,
// above this interface) over a socket subsystem (this). Nothing above sees the transport library's types, so the
// implementation (GameNetworkingSockets today, CreateGnsTransport) can be replaced without touching replication.

#include <chrono>
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
    using ConnectionId                               = std::uint32_t;
    inline constexpr ConnectionId kInvalidConnection = 0;

    // THE LANES of a connection (UE: a channel per traffic kind; GNS: lanes with a priority and a weight). Each
    // lane is a separate send queue, so a large transfer on Bulk does not hold back an RPC queued after it (the
    // head-of-line blocking of one reliable stream); reliable messages keep their order within their lane only.
    //   Control - the handshake, kicks, session control: reliable, sent before every other lane.
    //   Rpc     - remote calls: reliable, shares the rest of the bandwidth by weight (Rpc 50 : State 40 : Bulk
    //   10). Bulk    - large transfers (a character, a level's worth of state): reliable, the smallest weight.
    //   State   - replicated state snapshots: unreliable (a lost snapshot is superseded by the next).
    enum class ENetLane : std::uint8_t
    {
        Control,
        Rpc,
        Bulk,
        State,
    };
    inline constexpr int kNetLaneCount = 4;
    constexpr bool       IsReliableLane( ENetLane lane )
    {
        return lane != ENetLane::State;
    }
    constexpr std::string_view NetLaneName( ENetLane lane )
    {
        switch ( lane )
        {
            case ENetLane::Control:
                return "Control";
            case ENetLane::Rpc:
                return "Rpc";
            case ENetLane::Bulk:
                return "Bulk";
            case ENetLane::State:
                return "State";
        }
        return "unknown lane";
    }

    enum class ESendResult : std::uint8_t
    {
        Sent,
        QueueFull,    // the connection's send queue is full (congestion control): send again after a Poll
        NoConnection, // the connection is closed or was never valid
    };

    // How a connection ended. Codes below kAppCloseCodeMin are the transport's own; the protocol above uses
    // kAppCloseCodeMin + its reason (NetHandshake.hpp ERefuseReason).
    inline constexpr int kAppCloseCodeMin = 1000;
    struct DisconnectInfo
    {
        int         Code = 0;
        std::string Reason;
        bool        ClosedByPeer = false; // false: a problem detected locally (timeout, unreachable)
    };

    struct ConnectionStats
    {
        int   PingMs               = -1;   // smoothed round trip; -1 while unknown
        float QualityLocal         = -1.f; // fraction of packets delivered to us, 0..1; -1 while unknown
        float QualityRemote        = -1.f;
        float OutBytesPerSecond    = 0.f;
        float InBytesPerSecond     = 0.f;
        int   PendingReliableBytes = 0;
    };

    // The network condition simulator, applied to every packet THIS process sends (so two processes with
    // SendLagMs 75 see a 150 ms round trip). All zero = off. Development only: a shipping server never sets it.
    struct NetSimulator
    {
        int   SendLagMs           = 0;
        float SendLossPercent     = 0.f;
        float SendJitterAverageMs = 0.f;
    };

    // Who is on the other end of a connection, as the transport verified it: the identity its certificate names
    // ("str:mesh-sim-1") and whether a CA this process trusts signed that certificate (MeshCertificates.hpp).
    struct PeerIdentity
    {
        std::string Identity;
        bool        Authenticated = false;
    };

    // The clock message receive times are on (INetTransportEvents::OnMessage).
    using NetClock = std::chrono::steady_clock;

    class INetTransportEvents
    {
    public:
        virtual ~INetTransportEvents() = default;
        // The connection is up: a client's Connect completed, or a listener accepted a peer.
        virtual void OnConnected( ConnectionId connection, std::string_view address ) = 0;
        // The connection is gone (peer closed it, or it timed out). Not called for connections closed by Close().
        virtual void OnClosed( ConnectionId connection, const DisconnectInfo& info ) = 0;
        // One whole message, as sent, on the lane it was sent on (the transport fragments and reassembles). The
        // span is valid during the call. A message on a lane this protocol does not have is dropped by the
        // transport. `received` is when the transport took the message off the wire (its receive thread), not when
        // Poll handed it over: a consumer timing arrivals (the interpolation buffer's jitter) must not count its
        // own frame stalls as network jitter.
        virtual void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                                NetClock::time_point received ) = 0;
    };

    class INetTransport
    {
    public:
        virtual ~INetTransport() = default;
        // Accept connections on every local address at `port`. False + `error` when the port cannot be bound.
        virtual bool Listen( std::uint16_t port, std::string& error ) = 0;
        // Start connecting to "ip:port". kInvalidConnection + `error` when the address does not parse.
        virtual ConnectionId Connect( std::string_view address, std::string& error )                          = 0;
        // Queues one message on `lane`. The send queue is shared by the lanes (QueueFull counts every lane's
        // bytes), so a bulk sender keeps its share of it bounded (ConnectionStats::PendingReliableBytes).
        virtual ESendResult Send( ConnectionId connection, std::span<const std::byte> data, ENetLane lane ) = 0;
        // Deliver every pending connection change and message to `events`, on the calling thread.
        virtual void Poll( INetTransportEvents& events ) = 0;
        // Close with a code and reason the peer receives in OnClosed; `flush` first delivers what is queued
        // reliably.
        virtual void Close( ConnectionId connection, int code, std::string_view reason, bool flush ) = 0;
        virtual ConnectionStats GetStats( ConnectionId connection ) const                            = 0;
        // The transport's own multi-line report of a connection (packets sent/lost, queues), for logs only.
        virtual std::string DescribeConnection( ConnectionId connection ) const = 0;
        // False + `error` naming the setting the transport refused.
        virtual bool SetSimulator( const NetSimulator& simulator, std::string& error ) = 0;
        // The send rate every connection's congestion control stays within, bytes per second (UE: the net
        // driver's MaxClientRate / MaxInternetClientRate). min == max fixes the rate. False + `error` when
        // refused.
        virtual bool SetSendRate( int minBytesPerSecond, int maxBytesPerSecond, std::string& error ) = 0;
        // The verified identity of the peer of `connection` (empty, unauthenticated when it is not connected).
        [[nodiscard]] virtual PeerIdentity Peer( ConnectionId connection ) const = 0;
        // Present the mesh certificate of `identity` from the mesh CA directory and trust that CA (a mesh role
        // process, NET-ARCH §2M.2). False + `error` when the directory has no such certificate.
        virtual bool UseMeshCertificate( const std::string& caDirectory, std::string_view identity,
                                         std::string& error ) = 0;
    };

    // Receives the transport library's own diagnostics, one line each.
    using NetLogSink = std::function<void( std::string_view line )>;

    // The GameNetworkingSockets transport. One per process (the library is process-global). nullptr when the
    // library fails to initialise; the reason went to `log`.
    std::unique_ptr<INetTransport> CreateGnsTransport( NetLogSink log );
} // namespace Desert::Net
