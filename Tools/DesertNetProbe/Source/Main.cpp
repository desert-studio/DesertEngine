// DesertNetProbe: the transport, its lanes and the connect handshake on loopback, without the engine.
//   --issue-token --secret-file F --account A --server-id S --expires-in SECONDS     (prints the token as hex)
//   --server --port P --cooked DIR --server-id S --secret-file F --clients K --refusals R
//            --bulk-mb B --rpcs N [--lag-ms L --loss PCT --rate-kbps KB]
//   --client --connect IP:P --cooked DIR --token HEX --name N [--lag-ms L --loss PCT --rate-kbps KB]
// The cook hash is Core::CookHashOf of DIR's index (a cooked world directory, WorldCells::kIndexFileName), never a
// literal. The issuer stands in for the login backend (NET-ARCH §8): the server checks the token offline.
// After the handshake the server sends each admitted client B MB on the Bulk lane and, spread over that transfer,
// N stamped messages on the Rpc lane and a snapshot every 50 ms on the State lane; the client measures each RPC's
// one-way latency (both processes read the same machine's steady clock) and reports p50/p95/max back on Rpc.
// --lag-ms is this process's one-way send lag: two processes at 75 make a 150 ms round trip.
// Exit codes: 0 done, 1 failure (timeout, loss, disorder, p95 >= 400 ms, bad arguments), 2 refused by the server.

#include "Net/ConnectToken.hpp"
#include "Net/NetHandshake.hpp"
#include "Net/NetTransport.hpp"

#include <Engine/Core/Serialize/CookHash.hpp>

#include <Net/ReplicationClient.hpp>

#include <cmath>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <memory>

using namespace Desert::Net;

namespace
{
    enum class ESessionMessage : std::uint8_t
    {
        Name = kFirstSessionMessage, // client -> server (Control): the client's display name
        Bulk,                        // server -> client (Bulk): u32 chunk index + payload
        Rpc,                         // server -> client (Rpc): u32 index, i64 steady-clock send time (ns)
        State,                       // server -> client (State): u32 snapshot index
        Report, // client -> server (Rpc): u32 rpcs, u32 bulk ok, u32 p50/p95/max ms, u32 states
    };
    constexpr std::size_t kBulkChunk       = 64 * 1024;
    constexpr int         kBulkQueueBudget = 256 * 1024; // pending reliable bytes a bulk sender leaves room above
    constexpr auto        kStateInterval   = std::chrono::milliseconds( 50 );
    constexpr double      kRpcP95LimitMs   = 400.0;
    constexpr auto        kTimeout         = std::chrono::seconds( 180 );
    constexpr std::size_t kSessionHeader   = 1 + 4;

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

    std::int64_t SteadyNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch() )
             .count();
    }

    // ---------------------------------------------------------------- bytes

    void PutLE( std::vector<std::byte>& out, std::uint64_t value, int bytes )
    {
        for ( int i = 0; i < bytes; ++i )
            out.push_back( static_cast<std::byte>( ( value >> ( 8 * i ) ) & 0xFFu ) );
    }

    std::uint64_t GetLE( std::span<const std::byte> data, std::size_t offset, int bytes )
    {
        std::uint64_t value = 0;
        for ( int i = 0; i < bytes; ++i )
            value |= static_cast<std::uint64_t>( data[offset + i] ) << ( 8 * i );
        return value;
    }

    std::string ToHex( std::span<const std::byte> bytes )
    {
        std::string text;
        for ( const std::byte b : bytes )
            text += std::format( "{:02x}", static_cast<unsigned>( b ) );
        return text;
    }

    std::optional<std::vector<std::byte>> FromHex( std::string_view text )
    {
        if ( text.size() % 2 != 0 )
            return std::nullopt;
        std::vector<std::byte> bytes;
        for ( std::size_t i = 0; i < text.size(); i += 2 )
        {
            unsigned value = 0;
            for ( std::size_t k = i; k < i + 2; ++k )
            {
                const char c = text[k];
                value <<= 4;
                if ( c >= '0' && c <= '9' )
                    value |= static_cast<unsigned>( c - '0' );
                else if ( c >= 'a' && c <= 'f' )
                    value |= static_cast<unsigned>( c - 'a' + 10 );
                else if ( c >= 'A' && c <= 'F' )
                    value |= static_cast<unsigned>( c - 'A' + 10 );
                else
                    return std::nullopt;
            }
            bytes.push_back( static_cast<std::byte>( value ) );
        }
        return bytes;
    }

    // The server secret: a text file holding the key as hex (the login backend holds the same one).
    std::optional<std::vector<std::byte>> ReadSecret( const std::string& path )
    {
        std::ifstream file( path );
        std::string   text;
        if ( !file || !( file >> text ) )
        {
            Log( "secret file '{}' cannot be read", path );
            return std::nullopt;
        }
        auto bytes = FromHex( text );
        if ( !bytes || bytes->size() < kMinTokenSecretSize )
        {
            Log( "secret file '{}' must hold at least {} bytes as hex", path, kMinTokenSecretSize );
            return std::nullopt;
        }
        return bytes;
    }

    // The cook hash of a cooked world directory: CookHashOf its index file's bytes, as the server's CookedWorld
    // states it (ReadCookedWorld). A directory without an index is an error, not a zero hash.
    std::optional<std::uint64_t> ReadCookHash( const std::string& directory )
    {
        const std::filesystem::path index =
             std::filesystem::path( directory ) / std::string( Desert::Core::WorldCells::kIndexFileName );
        std::ifstream file( index, std::ios::binary );
        if ( !file )
        {
            Log( "cooked world '{}' has no {}", directory, Desert::Core::WorldCells::kIndexFileName );
            return std::nullopt;
        }
        const std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( file ) ),
                                                std::istreambuf_iterator<char>() );
        return Desert::Core::CookHashOf( bytes );
    }

    // ---------------------------------------------------------------- options

    struct Options
    {
        bool          IssueToken = false;
        bool          Server     = false;
        bool          Client     = false;
        std::uint16_t Port       = 27020;
        std::string   Connect;
        std::string   Name = "client";
        std::string   Cooked;
        std::string   SecretFile;
        std::string   TokenHex;
        std::uint64_t Account   = 0;
        std::uint64_t ServerId  = 0;
        bool          HasServer = false;
        std::uint64_t PawnPrefab = 0; // --server: the pawn its Welcome names (an AssetHandle; no game runs here)
        std::int64_t  ExpiresIn = 0;
        bool          HasExpiry = false;
        int           Clients   = 1;
        int           Refusals  = 0;
        std::uint64_t BulkBytes = 8ull << 20;
        std::uint32_t Rpcs      = 200;
        int           RateKBps  = 0;
        NetSimulator  Simulator;
        std::uint64_t CookHash = 0; // read from Cooked
    };

    std::optional<Options> ParseOptions( int argc, char** argv )
    {
        Options options;
        for ( int i = 1; i < argc; ++i )
        {
            const std::string arg      = argv[i];
            const bool        hasValue = i + 1 < argc;
            if ( arg == "--issue-token" )
                options.IssueToken = true;
            else if ( arg == "--server" )
                options.Server = true;
            else if ( arg == "--client" )
                options.Client = true;
            else if ( arg == "--port" && hasValue )
                options.Port = static_cast<std::uint16_t>( std::strtoul( argv[++i], nullptr, 10 ) );
            else if ( arg == "--connect" && hasValue )
                options.Connect = argv[++i];
            else if ( arg == "--name" && hasValue )
                options.Name = argv[++i];
            else if ( arg == "--cooked" && hasValue )
                options.Cooked = argv[++i];
            else if ( arg == "--secret-file" && hasValue )
                options.SecretFile = argv[++i];
            else if ( arg == "--token" && hasValue )
                options.TokenHex = argv[++i];
            else if ( arg == "--account" && hasValue )
                options.Account = std::strtoull( argv[++i], nullptr, 0 );
            else if ( arg == "--pawn-prefab" && hasValue )
                options.PawnPrefab = std::strtoull( argv[++i], nullptr, 0 );
            else if ( arg == "--server-id" && hasValue )
            {
                options.ServerId  = std::strtoull( argv[++i], nullptr, 0 );
                options.HasServer = true;
            }
            else if ( arg == "--expires-in" && hasValue )
            {
                options.ExpiresIn = std::strtoll( argv[++i], nullptr, 10 );
                options.HasExpiry = true;
            }
            else if ( arg == "--clients" && hasValue )
                options.Clients = std::atoi( argv[++i] );
            else if ( arg == "--refusals" && hasValue )
                options.Refusals = std::atoi( argv[++i] );
            else if ( arg == "--bulk-mb" && hasValue )
                options.BulkBytes = std::strtoull( argv[++i], nullptr, 10 ) << 20;
            else if ( arg == "--rpcs" && hasValue )
                options.Rpcs = static_cast<std::uint32_t>( std::strtoul( argv[++i], nullptr, 10 ) );
            else if ( arg == "--rate-kbps" && hasValue )
                options.RateKBps = std::atoi( argv[++i] );
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
        const int  modes = int( options.IssueToken ) + int( options.Server ) + int( options.Client );
        const bool ok =
             modes == 1 &&
             ( !options.IssueToken ||
               ( !options.SecretFile.empty() && options.HasServer && options.HasExpiry ) ) &&
             ( !options.Server || ( !options.Cooked.empty() && !options.SecretFile.empty() && options.HasServer &&
                                    options.PawnPrefab != 0 && options.Rpcs > 0 && options.BulkBytes > 0 ) ) &&
             ( !options.Client ||
               ( !options.Cooked.empty() && !options.Connect.empty() && !options.TokenHex.empty() ) );
        if ( !ok )
        {
            Log( "usage: --issue-token --secret-file F --account A --server-id S --expires-in SECONDS | "
                 "--server --port P --cooked DIR --server-id S --secret-file F --pawn-prefab H --clients K "
                 "--refusals R --bulk-mb "
                 "B "
                 "--rpcs N [--lag-ms L --loss PCT --rate-kbps KB] | "
                 "--client --connect IP:P --cooked DIR --token HEX --name N [--lag-ms L --loss PCT --rate-kbps "
                 "KB]" );
            return std::nullopt;
        }
        return options;
    }

    // ---------------------------------------------------------------- server

    class ProbeServer final : public INetTransportEvents
    {
    public:
        ProbeServer( INetTransport& transport, const Options& options, std::span<const std::byte> secret )
             : m_Transport( transport ), m_Options( options ), m_Secret( secret )
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
            if ( it->second.Accepted && !it->second.Reported )
            {
                Log( "client {} left before its report", it->second.Name );
                m_Failed = true;
            }
            Log( "connection {} ({}) closed: code {} '{}'", connection,
                 it->second.Name.empty() ? it->second.Address : it->second.Name, info.Code, info.Reason );
            m_Peers.erase( it );
        }

        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data ) override
        {
            Peer& peer = m_Peers[connection];
            if ( data.empty() )
            {
                Log( "connection {}: empty message on {}", connection, NetLaneName( lane ) );
                m_Failed = true;
                return;
            }
            if ( !peer.Accepted )
            {
                Admit( connection, peer, lane, data );
                return;
            }
            switch ( static_cast<ESessionMessage>( data[0] ) )
            {
                case ESessionMessage::Name:
                    peer.Name.assign( reinterpret_cast<const char*>( data.data() + 1 ), data.size() - 1 );
                    Log( "connection {} is client {} (account {}): sending {} bytes on Bulk, {} messages on Rpc, "
                         "snapshots on State",
                         connection, peer.Name, peer.Account, m_Options.BulkBytes, m_Options.Rpcs );
                    peer.Streaming = true;
                    break;
                case ESessionMessage::Report:
                {
                    if ( lane != ENetLane::Rpc || data.size() != 1 + 6 * 4 )
                    {
                        Log( "client {}: malformed report", peer.Name );
                        m_Failed = true;
                        break;
                    }
                    const auto field  = [&]( int i ) { return GetLE( data, 1 + 4 * i, 4 ); };
                    const bool passed = field( 0 ) == m_Options.Rpcs && field( 1 ) == 1 &&
                                        static_cast<double>( field( 3 ) ) < kRpcP95LimitMs;
                    Log( "client {} report: rpc {}/{} p50 {} ms p95 {} ms max {} ms, bulk {}, {} state snapshots "
                         "received -> {}; transport status:\n{}",
                         peer.Name, field( 0 ), m_Options.Rpcs, field( 2 ), field( 3 ), field( 4 ),
                         field( 1 ) == 1 ? "complete and in order" : "BROKEN", field( 5 ),
                         passed ? "PASS" : "FAIL", m_Transport.DescribeConnection( connection ) );
                    if ( !passed )
                        m_Failed = true;
                    peer.Reported = true;
                    ++m_Completed;
                    break;
                }
                default:
                    Log( "connection {}: unknown message type {} on {}", connection, static_cast<int>( data[0] ),
                         NetLaneName( lane ) );
                    m_Failed = true;
                    break;
            }
        }

        // Bulk keeps the shared send queue below kBulkQueueBudget so the Rpc and State lanes always find room;
        // RPC k goes out once k/N of the bulk is queued, so every RPC travels while the bulk lane is busy.
        void PumpSends()
        {
            const auto now = std::chrono::steady_clock::now();
            for ( auto& [connection, peer] : m_Peers )
            {
                if ( !peer.Streaming )
                    continue;
                while ( peer.BulkQueued < m_Options.BulkBytes &&
                        m_Transport.GetStats( connection ).PendingReliableBytes < kBulkQueueBudget )
                {
                    const std::size_t size = static_cast<std::size_t>(
                         std::min<std::uint64_t>( kBulkChunk, m_Options.BulkBytes - peer.BulkQueued ) );
                    std::vector<std::byte> chunk;
                    chunk.reserve( kSessionHeader + size );
                    PutLE( chunk, static_cast<std::uint8_t>( ESessionMessage::Bulk ), 1 );
                    PutLE( chunk, peer.BulkChunks, 4 );
                    chunk.resize( kSessionHeader + size, static_cast<std::byte>( peer.BulkChunks & 0xFFu ) );
                    if ( m_Transport.Send( connection, chunk, ENetLane::Bulk ) != ESendResult::Sent )
                        break;
                    peer.BulkQueued += size;
                    ++peer.BulkChunks;
                }
                while ( peer.RpcsSent < m_Options.Rpcs &&
                        peer.BulkQueued * m_Options.Rpcs >= std::uint64_t( peer.RpcsSent ) * m_Options.BulkBytes )
                {
                    std::vector<std::byte> rpc;
                    PutLE( rpc, static_cast<std::uint8_t>( ESessionMessage::Rpc ), 1 );
                    PutLE( rpc, peer.RpcsSent, 4 );
                    PutLE( rpc, static_cast<std::uint64_t>( SteadyNowNs() ), 8 );
                    if ( m_Transport.Send( connection, rpc, ENetLane::Rpc ) != ESendResult::Sent )
                    {
                        Log( "client {}: rpc {} refused by the send queue", peer.Name, peer.RpcsSent );
                        m_Failed = true;
                        break;
                    }
                    ++peer.RpcsSent;
                }
                if ( now - peer.LastState >= kStateInterval )
                {
                    std::vector<std::byte> state;
                    PutLE( state, static_cast<std::uint8_t>( ESessionMessage::State ), 1 );
                    PutLE( state, peer.StatesSent++, 4 );
                    m_Transport.Send( connection, state, ENetLane::State );
                    peer.LastState = now;
                }
            }
        }

        // Done once every expected client reported, every expected refusal happened, and all have left.
        bool Done() const
        {
            return m_Completed >= m_Options.Clients && m_Refused >= m_Options.Refusals && m_Peers.empty();
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
            std::string                           Address;
            std::string                           Name;
            std::uint64_t                         Account    = 0;
            bool                                  Accepted   = false;
            bool                                  Streaming  = false;
            bool                                  Reported   = false;
            std::uint64_t                         BulkQueued = 0;
            std::uint32_t                         BulkChunks = 0;
            std::uint32_t                         RpcsSent   = 0;
            std::uint32_t                         StatesSent = 0;
            std::chrono::steady_clock::time_point LastState;
        };

        void Admit( ConnectionId connection, Peer& peer, ENetLane lane, std::span<const std::byte> data )
        {
            const HelloPolicy  policy{ m_Options.CookHash, m_Options.ServerId, m_Secret, UnixSecondsNow() };
            const HelloVerdict verdict =
                 lane == ENetLane::Control ? EvaluateHello( data, policy ) : HelloVerdict{};
            if ( !verdict.Accepted )
            {
                Log( "refused connection {} from {}: {} (client protocol {}, cook hash {:016x}, token {} bytes, "
                     "account {}, expiry {}; server protocol {}, cook hash {:016x}, id {}, now {})",
                     connection, peer.Address, RefuseReasonText( verdict.Reason ), verdict.Hello.ProtocolVersion,
                     verdict.Hello.CookHash, verdict.Hello.Token.size(), verdict.Token.Account,
                     verdict.Token.ExpiresUnixSeconds, kProtocolVersion, m_Options.CookHash, m_Options.ServerId,
                     policy.NowUnixSeconds );
                ++m_Refused;
                m_Transport.Send( connection, EncodeRefuse( verdict.Reason ), ENetLane::Control );
                m_Transport.Close( connection, RefuseCloseCode( verdict.Reason ),
                                   RefuseReasonText( verdict.Reason ), true );
                m_Peers.erase( connection );
                return;
            }
            peer.Accepted = true;
            peer.Account  = verdict.Token.Account;
            Log( "accepted connection {} from {}: protocol {}, cook hash {:016x}, account {} (token valid for {} "
                 "s)",
                 connection, peer.Address, verdict.Hello.ProtocolVersion, verdict.Hello.CookHash,
                 verdict.Token.Account, verdict.Token.ExpiresUnixSeconds - policy.NowUnixSeconds );
            m_Transport.Send( connection, EncodeWelcome( m_Options.CookHash, m_Options.PawnPrefab ),
                              ENetLane::Control );
        }

        INetTransport&               m_Transport;
        const Options&               m_Options;
        std::span<const std::byte>   m_Secret;
        std::map<ConnectionId, Peer> m_Peers;
        int                          m_Completed = 0;
        int                          m_Refused   = 0;
        bool                         m_Failed    = false;
    };

    // ---------------------------------------------------------------- client

    class ProbeClient final : public INetTransportEvents
    {
    public:
        ProbeClient( INetTransport& transport, const Options& options, std::vector<std::byte> token )
             : m_Transport( transport ), m_Options( options ), m_Token( std::move( token ) )
        {
        }

        void OnConnected( ConnectionId connection, std::string_view address ) override
        {
            Log( "connected to {} (connection {}): sending hello, protocol {}, cook hash {:016x}, token {} bytes",
                 address, connection, kProtocolVersion, m_Options.CookHash, m_Token.size() );
            m_Transport.Send( connection, EncodeHello( { kProtocolVersion, m_Options.CookHash, m_Token } ),
                              ENetLane::Control );
        }

        void OnClosed( ConnectionId, const DisconnectInfo& info ) override
        {
            Log( "connection closed by {}: code {} '{}'", info.ClosedByPeer ? "server" : "transport", info.Code,
                 info.Reason );
            if ( !m_Refused && !m_Reported )
                m_Result = 1;
            m_Closed = true;
        }

        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data ) override
        {
            if ( data.empty() )
            {
                Log( "empty message on {}", NetLaneName( lane ) );
                m_Result = 1;
                return;
            }
            if ( !m_Welcomed )
            {
                Answer( connection, DecodeAnswer( data ) );
                return;
            }
            switch ( static_cast<ESessionMessage>( data[0] ) )
            {
                case ESessionMessage::Bulk:
                {
                    // In order within the lane, and each chunk's payload is its index's low byte.
                    const auto index = static_cast<std::uint32_t>( GetLE( data, 1, 4 ) );
                    const bool fill  = std::all_of( data.begin() + kSessionHeader, data.end(), [&]( std::byte b )
                                                    { return b == static_cast<std::byte>( index & 0xFFu ); } );
                    if ( lane != ENetLane::Bulk || index != m_BulkChunks || !fill )
                        m_BulkBroken = true;
                    ++m_BulkChunks;
                    m_BulkBytes += data.size() - kSessionHeader;
                    if ( m_BulkBytes == m_Options.BulkBytes )
                        Log( "bulk {}/{} bytes {} on lane Bulk ({} chunks)", m_BulkBytes, m_Options.BulkBytes,
                             m_BulkBroken ? "BROKEN" : "in order", m_BulkChunks );
                    break;
                }
                case ESessionMessage::Rpc:
                {
                    const auto   index     = static_cast<std::uint32_t>( GetLE( data, 1, 4 ) );
                    const auto   sentNs    = static_cast<std::int64_t>( GetLE( data, 5, 8 ) );
                    const double latencyMs = static_cast<double>( SteadyNowNs() - sentNs ) / 1e6;
                    if ( lane != ENetLane::Rpc || index != m_Latencies.size() )
                        m_RpcDisorder = true;
                    m_Latencies.push_back( latencyMs );
                    m_RpcsDuringBulk += m_BulkBytes < m_Options.BulkBytes ? 1 : 0;
                    break;
                }
                case ESessionMessage::State:
                    m_States += lane == ENetLane::State ? 1 : 0;
                    break;
                default:
                    Log( "unknown message type {} on {}", static_cast<int>( data[0] ), NetLaneName( lane ) );
                    m_Result = 1;
                    break;
            }
            if ( !m_Reported && m_BulkBytes >= m_Options.BulkBytes && m_Latencies.size() >= m_Options.Rpcs )
                Report( connection );
        }

        void PumpSends()
        {
        }
        bool Finished() const
        {
            return m_Reported;
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
        void Answer( ConnectionId connection, const HandshakeAnswer& answer )
        {
            if ( answer.Kind == HandshakeAnswer::EKind::Welcome )
            {
                m_Welcomed = true;
                Log( "accepted by server (server cook hash {:016x}, pawn prefab {:016x})", answer.ServerCookHash,
                     answer.PawnPrefab );
                std::vector<std::byte> name( 1 + m_Options.Name.size() );
                name[0] = static_cast<std::byte>( ESessionMessage::Name );
                std::memcpy( name.data() + 1, m_Options.Name.data(), m_Options.Name.size() );
                m_Transport.Send( connection, name, ENetLane::Control );
            }
            else if ( answer.Kind == HandshakeAnswer::EKind::Refuse )
            {
                Log( "refused by server: {}", RefuseReasonText( answer.Reason ) );
                m_Refused = true;
                m_Result  = 2;
                // The server closes too; closing here ends the attempt now instead of at the transport timeout.
                m_Transport.Close( connection, RefuseCloseCode( answer.Reason ), RefuseReasonText( answer.Reason ),
                                   false );
                m_Closed = true;
            }
            else
            {
                Log( "malformed handshake answer" );
                m_Result = 1;
                m_Closed = true;
            }
        }

        void Report( ConnectionId connection )
        {
            std::vector<double> sorted = m_Latencies;
            std::sort( sorted.begin(), sorted.end() );
            const auto at = [&]( double q )
            { return sorted[std::min( sorted.size() - 1, static_cast<std::size_t>( q * sorted.size() ) )]; };
            const double p50 = at( 0.50 ), p95 = at( 0.95 ), max = sorted.back();
            const bool   bulkOk = !m_BulkBroken && m_BulkBytes == m_Options.BulkBytes;
            const bool   passed = bulkOk && !m_RpcDisorder && p95 < kRpcP95LimitMs;
            Log( "rpc {}/{} on lane Rpc {}: latency p50 {:.1f} ms, p95 {:.1f} ms, max {:.1f} ms ({} of them while "
                 "the bulk lane was still filling); {} state snapshots on lane State; RTT {} ms -> {}",
                 m_Latencies.size(), m_Options.Rpcs, m_RpcDisorder ? "OUT OF ORDER" : "in order", p50, p95, max,
                 m_RpcsDuringBulk, m_States, m_Transport.GetStats( connection ).PingMs,
                 passed ? "PASS (p95 < 400 ms)" : "FAIL" );
            std::vector<std::byte> report;
            PutLE( report, static_cast<std::uint8_t>( ESessionMessage::Report ), 1 );
            PutLE( report, m_Latencies.size(), 4 );
            PutLE( report, bulkOk ? 1 : 0, 4 );
            PutLE( report, static_cast<std::uint64_t>( p50 ), 4 );
            PutLE( report, static_cast<std::uint64_t>( p95 ), 4 );
            PutLE( report, static_cast<std::uint64_t>( max ), 4 );
            PutLE( report, m_States, 4 );
            m_Transport.Send( connection, report, ENetLane::Rpc );
            // Flush the report, then leave: the server's Done waits for every client to go.
            m_Transport.Close( connection, kAppCloseCodeMin, "done", true );
            m_Reported = true;
            if ( !passed )
                m_Result = 1;
        }

        INetTransport&         m_Transport;
        const Options&         m_Options;
        std::vector<std::byte> m_Token;
        std::vector<double>    m_Latencies;
        std::uint64_t          m_BulkBytes      = 0;
        std::uint32_t          m_BulkChunks     = 0;
        std::uint32_t          m_States         = 0;
        std::uint32_t          m_RpcsDuringBulk = 0;
        bool                   m_BulkBroken     = false;
        bool                   m_RpcDisorder    = false;
        bool                   m_Welcomed       = false;
        bool                   m_Refused        = false;
        bool                   m_Reported       = false;
        bool                   m_Closed         = false;
        int                    m_Result         = 0;
    };

    template <typename Probe>
    void Pump( INetTransport& transport, Probe& probe )
    {
        transport.Poll( probe );
        probe.PumpSends();
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    int IssueToken( const Options& options )
    {
        const auto secret = ReadSecret( options.SecretFile );
        if ( !secret )
            return 1;
        const ConnectToken token{
             options.Account, options.ServerId,
             static_cast<std::uint64_t>( static_cast<std::int64_t>( UnixSecondsNow() ) + options.ExpiresIn ) };
        std::string            error;
        std::vector<std::byte> bytes = IssueConnectToken( token, *secret, error );
        if ( bytes.empty() )
        {
            Log( "issue token: {}", error );
            return 1;
        }
        std::printf( "%s\n", ToHex( bytes ).c_str() );
        return 0;
    }

    // A HEADLESS PLAYER (NET-1 scenario net1_two_clients; NET-ARCH §9 NET-10's bot client): the same
    // ReplicationClient the engine's net driver runs, input sent at the 60 Hz client step.
    //   --bot --connect IP:PORT --cooked DIR --token HEX --name N --seconds S [--walk-cm D --dir x,y,z]
    //         [--lag-ms L --loss P]
    // Walks its own pawn D cm along dir (server-authoritative: it stops when the SERVER's position of its pawn has
    // moved D cm), then idles; logs its own pawn and every remote pawn (interpolated) twice a second and a final
    // line per pawn with its displacement from where this client first saw it.
    int RunBot( int argc, char** argv )
    {
        std::string connect, cooked, tokenHex, name = "bot";
        double      seconds = 10.0, walkCm = 0.0;
        NetVec3     dir{ 1.f, 0.f, 0.f };
        NetSimulator simulator;
        for ( int i = 1; i < argc; ++i )
        {
            const std::string arg      = argv[i];
            const bool        hasValue = i + 1 < argc;
            if ( arg == "--bot" )
                continue;
            if ( !hasValue )
            {
                Log( "'{}' needs a value", arg );
                return 1;
            }
            const std::string value = argv[++i];
            if ( arg == "--connect" )
                connect = value;
            else if ( arg == "--cooked" )
                cooked = value;
            else if ( arg == "--token" )
                tokenHex = value;
            else if ( arg == "--name" )
                name = value;
            else if ( arg == "--seconds" )
                seconds = std::atof( value.c_str() );
            else if ( arg == "--walk-cm" )
                walkCm = std::atof( value.c_str() );
            else if ( arg == "--lag-ms" )
                simulator.SendLagMs = std::atoi( value.c_str() );
            else if ( arg == "--loss" )
                simulator.SendLossPercent = static_cast<float>( std::atof( value.c_str() ) );
            else if ( arg == "--dir" )
            {
                if ( std::sscanf( value.c_str(), "%f,%f,%f", &dir.X, &dir.Y, &dir.Z ) != 3 )
                {
                    Log( "--dir '{}' is not x,y,z", value );
                    return 1;
                }
            }
            else
            {
                Log( "unknown argument '{}'", arg );
                return 1;
            }
        }
        const auto token = FromHex( tokenHex );
        if ( connect.empty() || cooked.empty() || !token || seconds <= 0.0 )
        {
            Log( "usage: --bot --connect IP:PORT --cooked DIR --token HEX --name N --seconds S [--walk-cm D --dir "
                 "x,y,z] [--lag-ms L --loss P]" );
            return 1;
        }
        const std::optional<std::uint64_t> cookHash = ReadCookHash( cooked );
        if ( !cookHash )
            return 1;
        const std::string tag = name;
        const NetLogSink  log = [tag]( std::string_view line ) { Log( "[{}] {}", tag, line ); };
        std::unique_ptr<INetTransport> transport = CreateGnsTransport( log );
        std::string                    error;
        if ( !transport || !transport->SetSimulator( simulator, error ) )
        {
            Log( "[{}] transport: {}", name, error );
            return 1;
        }
        ReplicationClient client( *transport, { kProtocolVersion, *cookHash, *token }, log );
        if ( !client.Connect( connect, error ) )
        {
            Log( "[{}] connect {}: {}", name, connect, error );
            return 1;
        }

        const auto distance = []( const NetVec3& a, const NetVec3& b )
        { return std::sqrt( ( a.X - b.X ) * ( a.X - b.X ) + ( a.Y - b.Y ) * ( a.Y - b.Y ) + ( a.Z - b.Z ) * ( a.Z - b.Z ) ); };
        std::map<NetId, NetVec3> firstSeen;
        bool                     walking = walkCm > 0.0;
        const auto               start   = std::chrono::steady_clock::now();
        const auto               period  = std::chrono::duration<double>( 1.0 / kNetStepHz );
        std::uint64_t            step    = 0;
        double                   nextLog = 0.5;
        for ( ;; ++step )
        {
            std::this_thread::sleep_until( start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                        period * static_cast<double>( step ) ) );
            const double now = std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
            if ( now >= seconds )
                break;
            client.Poll();
            if ( client.State() == ReplicationClient::EState::Closed )
            {
                Log( "[{}] session closed: {}", name, client.CloseReason() );
                return 2;
            }
            for ( const auto& [id, pawn] : client.Pawns() )
                if ( !firstSeen.contains( id ) )
                    if ( const auto sample = client.Sample( id ) )
                        firstSeen[id] = sample->Position;
            // The SERVER's newest position of the own pawn (the snapshot, not the interpolated one) ends the walk.
            if ( walking && client.OwnPawn() != kInvalidNetId && firstSeen.contains( client.OwnPawn() ) )
            {
                const auto& own = client.Pawns().at( client.OwnPawn() );
                const PawnState newest = own.History.Sample( 1e12 );
                if ( distance( newest.Position, firstSeen[client.OwnPawn()] ) >= walkCm )
                {
                    walking = false;
                    Log( "[{}] walked {:.0f} cm (server position): stopping at step {}", name, walkCm, step );
                }
            }
            client.SendInput( walking ? dir : NetVec3{}, 0.f );
            if ( now >= nextLog )
            {
                nextLog += 0.5;
                std::string line;
                for ( const auto& [id, pawn] : client.Pawns() )
                    if ( const auto sample = client.Sample( id ) )
                        line += std::format( " {}{}=({:.0f},{:.0f},{:.0f})", id, pawn.Owned ? "*" : "",
                                             sample->Position.X, sample->Position.Y, sample->Position.Z );
                Log( "[{}] t={:.1f}s pawns{} ; unacked inputs {}, ack step {}", name, now,
                     line.empty() ? std::string( " none" ) : line, client.PendingInputs(),
                     client.LastAck() ? std::to_string( *client.LastAck() ) : std::string( "none" ) );
            }
        }
        int result = client.State() == ReplicationClient::EState::InSession ? 0 : 2;
        for ( const auto& [id, pawn] : client.Pawns() )
        {
            const auto sample = client.Sample( id );
            if ( !sample || !firstSeen.contains( id ) )
                continue;
            Log( "[{}] FINAL {} pawn net id {} (player {}) at ({:.0f}, {:.0f}, {:.0f}) cm, moved {:.0f} cm from where "
                 "this client first saw it",
                 name, pawn.Owned ? "own" : "remote", id, pawn.Player, sample->Position.X, sample->Position.Y,
                 sample->Position.Z, distance( sample->Position, firstSeen[id] ) );
        }
        Log( "[{}] {} pawn(s) replicated; exit {}", name, client.Pawns().size(), result );
        return result;
    }
} // namespace

int main( int argc, char** argv )
{
    for ( int i = 1; i < argc; ++i )
        if ( std::strcmp( argv[i], "--bot" ) == 0 )
            return RunBot( argc, argv );
    std::optional<Options> options = ParseOptions( argc, argv );
    if ( !options )
        return 1;
    if ( options->IssueToken )
        return IssueToken( *options );

    const std::optional<std::uint64_t> cookHash = ReadCookHash( options->Cooked );
    if ( !cookHash )
        return 1;
    options->CookHash = *cookHash;

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
    if ( options->RateKBps > 0 &&
         !transport->SetSendRate( options->RateKBps * 1024, options->RateKBps * 1024, error ) )
    {
        Log( "send rate: {}", error );
        return 1;
    }
    Log( "simulator: send lag {} ms, send loss {}%; send rate {}", options->Simulator.SendLagMs,
         options->Simulator.SendLossPercent,
         options->RateKBps > 0 ? std::format( "{} KB/s", options->RateKBps )
                               : std::string( "transport default" ) );
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;

    if ( options->Server )
    {
        const auto secret = ReadSecret( options->SecretFile );
        if ( !secret )
            return 1;
        if ( !transport->Listen( options->Port, error ) )
        {
            Log( "{}", error );
            return 1;
        }
        Log( "server {} listening on UDP {}: protocol {}, cooked world '{}' cook hash {:016x}; expecting {} "
             "client(s) and {} refusal(s)",
             options->ServerId, options->Port, kProtocolVersion, options->Cooked, options->CookHash,
             options->Clients, options->Refusals );
        ProbeServer server( *transport, *options, *secret );
        while ( !server.Done() && std::chrono::steady_clock::now() < deadline )
            Pump( *transport, server );
        if ( !server.Done() )
        {
            Log( "timed out: {}/{} clients completed, {}/{} refused", server.Completed(), options->Clients,
                 server.Refused(), options->Refusals );
            return 1;
        }
        Log( "server done: {} client(s) completed, {} refused, {}", server.Completed(), server.Refused(),
             server.Failed() ? "FAILED" : "all passed" );
        return server.Failed() ? 1 : 0;
    }

    const auto token = FromHex( options->TokenHex );
    if ( !token )
    {
        Log( "--token is not hex" );
        return 1;
    }
    const ConnectionId connection = transport->Connect( options->Connect, error );
    if ( connection == kInvalidConnection )
    {
        Log( "{}", error );
        return 1;
    }
    Log( "client {} connecting to {} with cooked world '{}'", options->Name, options->Connect, options->Cooked );
    ProbeClient client( *transport, *options, *token );
    while ( !client.Finished() && !client.Closed() && std::chrono::steady_clock::now() < deadline )
        Pump( *transport, client );
    // Let the flushed report and the close reach the server before the transport dies with the process.
    for ( int i = 0; client.Finished() && i < 1000; ++i )
        Pump( *transport, client );
    if ( !client.Finished() && !client.Closed() )
    {
        Log( "timed out" );
        return 1;
    }
    Log( "client {} exit {}", options->Name, client.Result() );
    return client.Result();
}
