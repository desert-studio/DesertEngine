// DesertNetProbe: the transport and the connect handshake on loopback, without the engine.
//   --server --port P --cook-hash H --clients K --messages M [--lag-ms L --loss PCT]
//   --client --connect IP:P --cook-hash H --name N --messages M [--lag-ms L --loss PCT]
// Each accepted client and the server send each other M numbered reliable messages; each side checks that all M
// arrived in order, the server returns its count to the client (Summary), and both log the measured round trip.
// --lag-ms is this process's one-way send lag: two processes at 75 make a 150 ms round trip.
// Exit codes: 0 done, 1 failure (timeout, loss, disorder, bad arguments), 2 refused by the server.

#include "Net/NetHandshake.hpp"
#include "Net/NetTransport.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <thread>

using namespace Desert::Net;

namespace
{
    enum class ESessionMessage : std::uint8_t
    {
        Name = kFirstSessionMessage, // client -> server: the client's display name
        Data,                        // u32 sequence number + filler, both directions
        Summary,                     // server -> client: u32 messages received from that client, u32 out of order
    };
    // About one UDP packet per message (MTU ~1200), so the simulator's loss hits individual messages and the
    // reliable lane must retransmit them; small messages would coalesce into a few dozen packets.
    constexpr std::size_t kDataSize = 1100;
    constexpr auto        kTimeout  = std::chrono::seconds( 120 );

    const auto g_Start = std::chrono::steady_clock::now();

    template <typename... Args>
    void Log( std::format_string<Args...> format, Args&&... args )
    {
        const auto ms =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - g_Start );
        std::printf( "[%8.3f] %s\n", static_cast<double>( ms.count() ) / 1000.0,
                     std::format( format, std::forward<Args>( args )... ).c_str() );
        std::fflush( stdout );
    }

    struct Options
    {
        bool          Server = false;
        bool          Client = false;
        std::uint16_t Port   = 27020;
        std::string   Connect;
        std::string   Name        = "client";
        std::uint64_t CookHash    = 0;
        bool          HasCookHash = false;
        int           Clients     = 1;
        std::uint32_t Messages    = 1000;
        NetSimulator  Simulator;
    };

    std::optional<Options> ParseOptions( int argc, char** argv )
    {
        Options options;
        for ( int i = 1; i < argc; ++i )
        {
            const std::string arg      = argv[i];
            const bool        hasValue = i + 1 < argc;
            if ( arg == "--server" )
                options.Server = true;
            else if ( arg == "--client" )
                options.Client = true;
            else if ( arg == "--port" && hasValue )
                options.Port = static_cast<std::uint16_t>( std::strtoul( argv[++i], nullptr, 10 ) );
            else if ( arg == "--connect" && hasValue )
                options.Connect = argv[++i];
            else if ( arg == "--name" && hasValue )
                options.Name = argv[++i];
            else if ( arg == "--cook-hash" && hasValue )
            {
                options.CookHash    = std::strtoull( argv[++i], nullptr, 0 );
                options.HasCookHash = true;
            }
            else if ( arg == "--clients" && hasValue )
                options.Clients = std::atoi( argv[++i] );
            else if ( arg == "--messages" && hasValue )
                options.Messages = static_cast<std::uint32_t>( std::strtoul( argv[++i], nullptr, 10 ) );
            else if ( arg == "--lag-ms" && hasValue )
                options.Simulator.SendLagMs = std::atoi( argv[++i] );
            else if ( arg == "--loss" && hasValue )
                options.Simulator.SendLossPercent = std::strtof( argv[++i], nullptr );
            else
            {
                Log( "unknown or incomplete argument '{}'", arg );
                return std::nullopt;
            }
        }
        if ( options.Server == options.Client || !options.HasCookHash ||
             ( options.Client && options.Connect.empty() ) )
        {
            Log( "usage: --server --port P --cook-hash H --clients K --messages M [--lag-ms L --loss PCT] | "
                 "--client --connect IP:P --cook-hash H --name N --messages M [--lag-ms L --loss PCT]" );
            return std::nullopt;
        }
        return options;
    }

    std::vector<std::byte> DataMessage( std::uint32_t sequence )
    {
        std::vector<std::byte> out( kDataSize, std::byte{ 0x5A } );
        out[0] = static_cast<std::byte>( ESessionMessage::Data );
        for ( int i = 0; i < 4; ++i )
            out[1 + i] = static_cast<std::byte>( ( sequence >> ( 8 * i ) ) & 0xFFu );
        return out;
    }

    std::uint32_t ReadU32( std::span<const std::byte> data, std::size_t offset )
    {
        std::uint32_t value = 0;
        for ( int i = 0; i < 4; ++i )
            value |= static_cast<std::uint32_t>( data[offset + i] ) << ( 8 * i );
        return value;
    }

    // Sends numbered messages to one peer as fast as the transport's send queue accepts them (congestion control
    // pushes back with QueueFull; the rest goes after the next Poll).
    struct SequenceSender
    {
        ConnectionId  Connection = kInvalidConnection;
        std::uint32_t Next       = 0;
        std::uint32_t Total      = 0;

        void Pump( INetTransport& transport )
        {
            while ( Connection != kInvalidConnection && Next < Total &&
                    transport.Send( Connection, DataMessage( Next ), ESendMode::Reliable ) == ESendResult::Sent )
                ++Next;
        }
    };

    // Counts numbered messages from one peer: in order means each one is the previous + 1.
    struct SequenceCheck
    {
        std::uint32_t Received   = 0;
        std::uint32_t OutOfOrder = 0;
        std::uint32_t Next       = 0;

        void Accept( std::uint32_t sequence )
        {
            if ( sequence != Next )
                ++OutOfOrder;
            Next = sequence + 1;
            ++Received;
        }
    };

    // ---------------------------------------------------------------- server

    class ProbeServer final : public INetTransportEvents
    {
    public:
        ProbeServer( INetTransport& transport, const Options& options )
             : m_Transport( transport ), m_Options( options )
        {
        }

        void OnConnected( ConnectionId connection, std::string_view address ) override
        {
            m_Peers[connection].Address = std::string( address );
            Log( "connection {} from {}: waiting for hello", connection, address );
        }

        void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override
        {
            const auto it = m_Peers.find( connection );
            if ( it == m_Peers.end() )
                return;
            const Peer& peer = it->second;
            if ( peer.Accepted && peer.From.Received < m_Options.Messages )
            {
                Log( "client {} left after {}/{} messages: {} lost", peer.Name, peer.From.Received,
                     m_Options.Messages, m_Options.Messages - peer.From.Received );
                m_Failed = true;
            }
            Log( "connection {} ({}) closed: code {} '{}'", connection,
                 peer.Name.empty() ? peer.Address : peer.Name, info.Code, info.Reason );
            m_Peers.erase( it );
        }

        void OnMessage( ConnectionId connection, std::span<const std::byte> data ) override
        {
            Peer& peer = m_Peers[connection];
            if ( data.empty() )
            {
                Log( "connection {}: empty message", connection );
                m_Failed = true;
                return;
            }
            if ( !peer.Accepted )
            {
                const HelloVerdict verdict = EvaluateHello( data, m_Options.CookHash );
                if ( !verdict.Accepted )
                {
                    Log( "refused connection {} from {}: {} (client protocol {}, cook hash {:#018x}; server "
                         "protocol {}, "
                         "cook hash {:#018x})",
                         connection, peer.Address, RefuseReasonText( verdict.Reason ),
                         verdict.Hello.ProtocolVersion, verdict.Hello.CookHash, kProtocolVersion,
                         m_Options.CookHash );
                    ++m_Refused;
                    m_Transport.Send( connection, EncodeRefuse( verdict.Reason ), ESendMode::Reliable );
                    m_Transport.Close( connection, RefuseCloseCode( verdict.Reason ),
                                       RefuseReasonText( verdict.Reason ), true );
                    m_Peers.erase( connection );
                    return;
                }
                peer.Accepted = true;
                Log( "accepted connection {} from {}: protocol {}, cook hash {:#018x}", connection, peer.Address,
                     verdict.Hello.ProtocolVersion, verdict.Hello.CookHash );
                m_Transport.Send( connection, EncodeWelcome( m_Options.CookHash ), ESendMode::Reliable );
                peer.To = { connection, 0, m_Options.Messages };
                peer.To.Pump( m_Transport );
                return;
            }
            switch ( static_cast<ESessionMessage>( data[0] ) )
            {
                case ESessionMessage::Name:
                    peer.Name.assign( reinterpret_cast<const char*>( data.data() + 1 ), data.size() - 1 );
                    Log( "connection {} is client {}", connection, peer.Name );
                    break;
                case ESessionMessage::Data:
                    peer.From.Accept( ReadU32( data, 1 ) );
                    if ( peer.From.Received == m_Options.Messages )
                    {
                        const ConnectionStats stats = m_Transport.GetStats( connection );
                        Log( "client {}: received {}/{} in order, {} lost, {} out of order; RTT {} ms; transport "
                             "status:\n{}",
                             peer.Name, peer.From.Received, m_Options.Messages,
                             m_Options.Messages - peer.From.Received, peer.From.OutOfOrder, stats.PingMs,
                             m_Transport.DescribeConnection( connection ) );
                        if ( peer.From.OutOfOrder != 0 )
                            m_Failed = true;
                        std::vector<std::byte> summary( 9 );
                        summary[0] = static_cast<std::byte>( ESessionMessage::Summary );
                        for ( int i = 0; i < 4; ++i )
                        {
                            summary[1 + i] = static_cast<std::byte>( ( peer.From.Received >> ( 8 * i ) ) & 0xFFu );
                            summary[5 + i] =
                                 static_cast<std::byte>( ( peer.From.OutOfOrder >> ( 8 * i ) ) & 0xFFu );
                        }
                        m_Transport.Send( connection, summary, ESendMode::Reliable );
                        ++m_Completed;
                    }
                    break;
                default:
                    Log( "connection {}: unknown message type {}", connection, static_cast<int>( data[0] ) );
                    m_Failed = true;
                    break;
            }
        }

        void PumpSends()
        {
            for ( auto& [connection, peer] : m_Peers )
                peer.To.Pump( m_Transport );
        }

        // Done once every expected client completed its exchange and left.
        bool Done() const
        {
            return m_Completed >= m_Options.Clients && m_Peers.empty();
        }
        bool Failed() const
        {
            return m_Failed;
        }
        int Completed() const
        {
            return m_Completed;
        }
        int Refused() const
        {
            return m_Refused;
        }

    private:
        struct Peer
        {
            std::string    Address;
            std::string    Name;
            bool           Accepted = false;
            SequenceCheck  From;
            SequenceSender To;
        };

        INetTransport&               m_Transport;
        const Options&               m_Options;
        std::map<ConnectionId, Peer> m_Peers;
        int                          m_Completed = 0;
        int                          m_Refused   = 0;
        bool                         m_Failed    = false;
    };

    // ---------------------------------------------------------------- client

    class ProbeClient final : public INetTransportEvents
    {
    public:
        ProbeClient( INetTransport& transport, const Options& options )
             : m_Transport( transport ), m_Options( options )
        {
        }

        void OnConnected( ConnectionId connection, std::string_view address ) override
        {
            Log( "connected to {} (connection {}): sending hello, protocol {}, cook hash {:#018x}", address,
                 connection, kProtocolVersion, m_Options.CookHash );
            m_Transport.Send( connection, EncodeHello( { kProtocolVersion, m_Options.CookHash } ),
                              ESendMode::Reliable );
        }

        void OnClosed( ConnectionId, const DisconnectInfo& info ) override
        {
            Log( "connection closed by {}: code {} '{}'", info.ClosedByPeer ? "server" : "transport", info.Code,
                 info.Reason );
            if ( !m_Refused && !Finished() )
                m_Result = 1;
            m_Closed = true;
        }

        void OnMessage( ConnectionId connection, std::span<const std::byte> data ) override
        {
            if ( data.empty() )
            {
                Log( "empty message" );
                m_Result = 1;
                return;
            }
            if ( !m_Welcomed )
            {
                const HandshakeAnswer answer = DecodeAnswer( data );
                if ( answer.Kind == HandshakeAnswer::EKind::Welcome )
                {
                    m_Welcomed = true;
                    Log( "accepted by server (server cook hash {:#018x}); sending {} reliable messages",
                         answer.ServerCookHash, m_Options.Messages );
                    std::vector<std::byte> name( 1 + m_Options.Name.size() );
                    name[0] = static_cast<std::byte>( ESessionMessage::Name );
                    std::memcpy( name.data() + 1, m_Options.Name.data(), m_Options.Name.size() );
                    m_Transport.Send( connection, name, ESendMode::Reliable );
                    m_To = { connection, 0, m_Options.Messages };
                    m_To.Pump( m_Transport );
                }
                else if ( answer.Kind == HandshakeAnswer::EKind::Refuse )
                {
                    Log( "refused by server: {}", RefuseReasonText( answer.Reason ) );
                    m_Refused = true;
                    m_Result  = 2;
                    // The server closes too; closing here ends the attempt now instead of at the transport
                    // timeout.
                    m_Transport.Close( connection, RefuseCloseCode( answer.Reason ),
                                       RefuseReasonText( answer.Reason ), false );
                    m_Closed = true;
                }
                else
                {
                    Log( "malformed handshake answer ({} bytes)", data.size() );
                    m_Result = 1;
                    m_Closed = true;
                }
                return;
            }
            switch ( static_cast<ESessionMessage>( data[0] ) )
            {
                case ESessionMessage::Data:
                    m_From.Accept( ReadU32( data, 1 ) );
                    if ( m_From.Received == m_Options.Messages )
                    {
                        Log( "received {}/{} in order, {} lost, {} out of order", m_From.Received,
                             m_Options.Messages, m_Options.Messages - m_From.Received, m_From.OutOfOrder );
                        if ( m_From.OutOfOrder != 0 )
                            m_Result = 1;
                    }
                    break;
                case ESessionMessage::Summary:
                {
                    const std::uint32_t received   = ReadU32( data, 1 );
                    const std::uint32_t outOfOrder = ReadU32( data, 5 );
                    m_Summary                      = true;
                    Log( "server summary: received {}/{} of ours in order, {} lost, {} out of order", received,
                         m_Options.Messages, m_Options.Messages - received, outOfOrder );
                    if ( received != m_Options.Messages || outOfOrder != 0 )
                        m_Result = 1;
                    break;
                }
                default:
                    Log( "unknown message type {}", static_cast<int>( data[0] ) );
                    m_Result = 1;
                    break;
            }
        }

        void PumpSends()
        {
            m_To.Pump( m_Transport );
        }
        bool Finished() const
        {
            return m_Summary && m_From.Received >= m_Options.Messages;
        }
        bool Closed() const
        {
            return m_Closed;
        }
        int Result() const
        {
            return m_Result;
        }

    private:
        INetTransport& m_Transport;
        const Options& m_Options;
        SequenceCheck  m_From;
        SequenceSender m_To;
        bool           m_Welcomed = false;
        bool           m_Refused  = false;
        bool           m_Summary  = false;
        bool           m_Closed   = false;
        int            m_Result   = 0;
    };

    template <typename Probe>
    void Pump( INetTransport& transport, Probe& probe )
    {
        transport.Poll( probe );
        probe.PumpSends();
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
} // namespace

int main( int argc, char** argv )
{
    const std::optional<Options> options = ParseOptions( argc, argv );
    if ( !options )
        return 1;
    std::unique_ptr<INetTransport> transport =
         CreateGnsTransport( []( std::string_view line ) { Log( "{}", line ); } );
    if ( !transport )
        return 1;
    std::string error;
    if ( !transport->SetSimulator( options->Simulator, error ) )
    {
        Log( "simulator: {}", error );
        return 1;
    }
    Log( "simulator: send lag {} ms, send loss {}%", options->Simulator.SendLagMs,
         options->Simulator.SendLossPercent );
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;

    if ( options->Server )
    {
        if ( !transport->Listen( options->Port, error ) )
        {
            Log( "{}", error );
            return 1;
        }
        Log( "server listening on UDP {}: protocol {}, cook hash {:#018x}, expecting {} clients x {} messages",
             options->Port, kProtocolVersion, options->CookHash, options->Clients, options->Messages );
        ProbeServer server( *transport, *options );
        while ( !server.Done() && std::chrono::steady_clock::now() < deadline )
            Pump( *transport, server );
        if ( !server.Done() )
        {
            Log( "timed out: {}/{} clients completed", server.Completed(), options->Clients );
            return 1;
        }
        Log( "server done: {} clients completed, {} refused, {}", server.Completed(), server.Refused(),
             server.Failed() ? "FAILED" : "all messages in order, none lost" );
        return server.Failed() ? 1 : 0;
    }

    const ConnectionId connection = transport->Connect( options->Connect, error );
    if ( connection == kInvalidConnection )
    {
        Log( "{}", error );
        return 1;
    }
    Log( "client {} connecting to {}", options->Name, options->Connect );
    ProbeClient client( *transport, *options );
    while ( !client.Finished() && !client.Closed() && std::chrono::steady_clock::now() < deadline )
        Pump( *transport, client );
    if ( client.Finished() )
    {
        const ConnectionStats stats = transport->GetStats( connection );
        Log( "client {} done: RTT {} ms; transport status:\n{}", options->Name, stats.PingMs,
             transport->DescribeConnection( connection ) );
        transport->Close( connection, kAppCloseCodeMin, "client done", true );
        // Linger lets the close (and any unacknowledged reliable data) reach the server.
        for ( int i = 0; i < 500; ++i )
            Pump( *transport, client );
        return client.Result();
    }
    if ( !client.Closed() )
    {
        Log( "timed out" );
        return 1;
    }
    return client.Result();
}
