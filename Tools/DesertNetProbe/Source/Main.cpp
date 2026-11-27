// DesertNetProbe: the transport, its lanes and the connect handshake on loopback, without the engine.
//   --gen-issuer-keys --issuer I --key-id K --out-private F --out-public F     (a new Ed25519 issuer key pair)
//   --mint-token --issuer-key PRIVATE_F --audience W --claims TEXT --expires-in SECONDS  (prints the token as hex;
//            a negative expiry mints an already expired token)
//   --server --port P --cooked DIR --audience W --issuer-key PUBLIC_F --clients K --refusals R
//            --bulk-mb B --rpcs N [--lag-ms L --loss PCT --rate-kbps KB]
//   --client --connect IP:P --cooked DIR --token HEX --name N [--join-options TEXT --lag-ms L --loss PCT
//            --rate-kbps KB]
// The cook hash is Core::CookHashOf of DIR's index (a cooked world directory, WorldCells::kIndexFileName), never a
// literal. The issuer stands in for the login backend (NET-ARCH §8): it alone holds the private key; servers check
// the token offline with the public one. This probe's server applies no game rule: it logs the claims as text.
// After the handshake the server sends each admitted client B MB on the Bulk lane and, spread over that transfer,
// N stamped messages on the Rpc lane and a snapshot every 50 ms on the State lane; the client measures each RPC's
// one-way latency (both processes read the same machine's steady clock) and reports p50/p95/max back on Rpc.
// --lag-ms is this process's one-way send lag: two processes at 75 make a 150 ms round trip.
// Exit codes: 0 done, 1 failure (timeout, loss, disorder, p95 >= 400 ms, bad arguments), 2 refused by the server.
//   --mesh-peer --mesh-ca DIR --store IP:P --as-sim N [--store-index I] [--seconds S]
//            a HOSTILE MESH PEER (NET-HANDOVER-SCN-DUP-OFFER): links to the store with sim N's CA-signed
//            certificate beside the real sim N, holds no pawn and speaks the handover wire (Net/HandoverWire.hpp)
//            from console lines on stdin, as a defective or hostile sender would; the store must refuse each under
//            the key (sim N, #0), which names no offer any sender holds:
//              dup <serial> <to> <net id> <epoch> <step>    an Offer under the key (sim N, #serial), one the
//                                                           store holds open
//              undecodable <serial> <to>                    an Offer whose header (key (sim N, #serial)) decodes
//                                                           and whose pawn record is cut short
//            Several commands may share one console line, separated by ';' (sent in the same poll).
//            Every handover message the store sends this link is logged ("probe: received ..."): a refusal that
//            reached the probe instead of the real sim N is visible. Exit 0 after S seconds (default 900), 1 when
//            the link cannot be made or is lost.

#include "Net/ConnectToken.hpp"
#include "Net/HandoverWire.hpp"
#include "Net/MeshLink.hpp"
#include "Net/NetHandshake.hpp"
#include "Net/NetTransport.hpp"

#include <Engine/Core/Serialize/CookHash.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>

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
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <memory>
#include <mutex>
#include <sstream>

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

    std::optional<IssuerKeyRecord> ReadKey( const std::string& path )
    {
        std::string                    error;
        std::optional<IssuerKeyRecord> key = ReadIssuerKeyFile( path, error );
        if ( !key )
            Log( "{}", error );
        return key;
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

    // ---------------------------------------------------------------- the bot's cells

    // THE BOT'S STREAMING (NET-3): the cells a client would have resident around its own pawn - the cooked world's
    // plan (WorldCells::PlanFromIndex, the plan the cook made and the client's streamer runs) queried with one
    // streaming source at the pawn's server position (QueryStreamingCells, the query the client's residency
    // steps). A headless player has no reads to wait on, so a wanted cell is resident at once; what changes is
    // reported to the server (CellResidencyMessage) as the editor client's ClientNetDriver::ReportResidency does,
    // with the unloaded cells' record ids leaving the relevant set.
    class BotCells
    {
    public:
        static std::optional<BotCells> Open( const std::string& directory )
        {
            namespace Core = Desert::Core;
            const std::filesystem::path path =
                 std::filesystem::path( directory ) / std::string( Core::WorldCells::kIndexFileName );
            std::ifstream file( path, std::ios::binary );
            if ( !file )
            {
                Log( "cooked world '{}' has no {}", directory, Core::WorldCells::kIndexFileName );
                return std::nullopt;
            }
            const std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( file ) ),
                                                    std::istreambuf_iterator<char>() );
            auto index = Core::WorldCells::ReadWorldIndex( path.generic_string(), bytes );
            if ( !index )
            {
                Log( "cooked world '{}': {}", directory, index.GetError() );
                return std::nullopt;
            }
            auto planned = Core::WorldCells::PlanFromIndex( index.GetValue() );
            if ( !planned )
            {
                Log( "cooked world '{}': {}", directory, planned.GetError() );
                return std::nullopt;
            }
            BotCells cells;
            cells.m_Partition = index.GetValue().WorldPartition;
            cells.m_World     = planned.ExtractValue();
            return cells;
        }

        // The report for the pawn at @p at (cm), or nullopt when the resident cells did not change. @p said
        // receives the line to log; an error of the query is said and changes nothing.
        std::optional<std::pair<CellResidencyMessage, std::vector<StableNetId>>> Update( const NetVec3& at,
                                                                                         std::string&   said )
        {
            namespace Rules = Desert::Core::Rules;
            Rules::StreamingSource source;
            source.Position = { at.X, at.Y, at.Z };
            auto wish       = Rules::QueryStreamingCells( m_World.Plan, m_Partition, std::span( &source, 1 ) );
            if ( !wish )
            {
                said = "streaming query: " + wish.GetError();
                return std::nullopt;
            }
            const Rules::WorldPartitionPlan& plan = m_World.Plan;
            std::set<std::uint32_t>          now;
            for ( const Rules::WantedCell& cell : wish.GetValue().Cells )
                now.insert( static_cast<std::uint32_t>( plan.AlwaysLoaded.size() + cell.Cell ) );
            if ( now == m_Resident )
                return std::nullopt;
            CellResidencyMessage     change;
            std::vector<StableNetId> gone;
            std::string              loaded, unloaded;
            for ( const std::uint32_t unit : now )
                if ( !m_Resident.contains( unit ) )
                {
                    change.Loaded.push_back( unit );
                    loaded += std::format( "{}{}", loaded.empty() ? "" : ", ",
                                           Rules::DescribeResidencyUnit( plan, unit ) );
                }
            for ( const std::uint32_t unit : m_Resident )
                if ( !now.contains( unit ) )
                {
                    change.Unloaded.push_back( unit );
                    unloaded += std::format( "{}{}", unloaded.empty() ? "" : ", ",
                                             Rules::DescribeResidencyUnit( plan, unit ) );
                    for ( const std::size_t record : Rules::ResidencyUnitMembers( plan, unit ) )
                        gone.push_back( m_World.RecordIds.at( record ) );
                }
            said = std::format(
                 "residency report at ({:.0f}, {:.0f}, {:.0f}) cm: {} cell(s) resident; {} loaded{}{}; "
                 "{} unloaded{}{}",
                 at.X, at.Y, at.Z, now.size(), change.Loaded.size(), loaded.empty() ? "" : ": ", loaded,
                 change.Unloaded.size(), unloaded.empty() ? "" : ": ", unloaded );
            m_Resident = std::move( now );
            return std::make_pair( std::move( change ), std::move( gone ) );
        }

    private:
        Desert::Core::WorldPartitionSerialized m_Partition;
        Desert::Core::WorldCells::IndexedWorld m_World;
        std::set<std::uint32_t>                m_Resident;
    };

    // ---------------------------------------------------------------- options

    struct Options
    {
        bool          GenKeys    = false;
        bool          MintToken  = false;
        bool          Server     = false;
        bool          Client     = false;
        std::uint16_t Port       = 27020;
        std::string   Connect;
        std::string   Name = "client";
        std::string   Cooked;
        std::string   IssuerKeyFile; // --mint-token: the private key; --server: the public key
        std::string   TokenHex;
        std::string   Claims;      // --mint-token: the game's claims, signed as they are
        std::string   JoinOptions; // --client: the game's join options sent in the Hello
        std::uint64_t Issuer = 0;
        std::uint32_t KeyId  = 0;
        std::string   OutPrivate, OutPublic;
        std::uint64_t Audience    = 0;
        bool          HasAudience = false;
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
            if ( arg == "--gen-issuer-keys" )
                options.GenKeys = true;
            else if ( arg == "--mint-token" )
                options.MintToken = true;
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
            else if ( arg == "--issuer-key" && hasValue )
                options.IssuerKeyFile = argv[++i];
            else if ( arg == "--claims" && hasValue )
                options.Claims = argv[++i];
            else if ( arg == "--join-options" && hasValue )
                options.JoinOptions = argv[++i];
            else if ( arg == "--issuer" && hasValue )
                options.Issuer = std::strtoull( argv[++i], nullptr, 0 );
            else if ( arg == "--key-id" && hasValue )
                options.KeyId = static_cast<std::uint32_t>( std::strtoul( argv[++i], nullptr, 0 ) );
            else if ( arg == "--out-private" && hasValue )
                options.OutPrivate = argv[++i];
            else if ( arg == "--out-public" && hasValue )
                options.OutPublic = argv[++i];
            else if ( arg == "--token" && hasValue )
                options.TokenHex = argv[++i];
            else if ( arg == "--pawn-prefab" && hasValue )
                options.PawnPrefab = std::strtoull( argv[++i], nullptr, 0 );
            else if ( arg == "--audience" && hasValue )
            {
                options.Audience    = std::strtoull( argv[++i], nullptr, 0 );
                options.HasAudience = true;
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
        const int modes =
             int( options.GenKeys ) + int( options.MintToken ) + int( options.Server ) + int( options.Client );
        const bool ok = modes == 1 &&
                        ( !options.GenKeys || ( !options.OutPrivate.empty() && !options.OutPublic.empty() ) ) &&
                        ( !options.MintToken ||
                          ( !options.IssuerKeyFile.empty() && options.HasAudience && options.HasExpiry ) ) &&
                        ( !options.Server ||
                          ( !options.Cooked.empty() && !options.IssuerKeyFile.empty() && options.HasAudience &&
                            options.PawnPrefab != 0 && options.Rpcs > 0 && options.BulkBytes > 0 ) ) &&
                        ( !options.Client ||
                          ( !options.Cooked.empty() && !options.Connect.empty() && !options.TokenHex.empty() ) );
        if ( !ok )
        {
            Log( "usage: --gen-issuer-keys --issuer I --key-id K --out-private F --out-public F | "
                 "--mint-token --issuer-key PRIVATE_F --audience W --claims TEXT --expires-in SECONDS | "
                 "--server --port P --cooked DIR --audience W --issuer-key PUBLIC_F --pawn-prefab H --clients K "
                 "--refusals R --bulk-mb "
                 "B "
                 "--rpcs N [--lag-ms L --loss PCT --rate-kbps KB] | "
                 "--client --connect IP:P --cooked DIR --token HEX --name N [--join-options TEXT --lag-ms L "
                 "--loss "
                 "PCT --rate-kbps "
                 "KB]" );
            return std::nullopt;
        }
        return options;
    }

    // ---------------------------------------------------------------- server

    class ProbeServer final : public INetTransportEvents
    {
    public:
        ProbeServer( INetTransport& transport, const Options& options, ConnectTokenVerifier tokens )
             : m_Transport( transport ), m_Options( options ), m_Tokens( std::move( tokens ) )
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

        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                        NetClock::time_point ) override
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
                    Log( "connection {} is client {} (claims '{}'): sending {} bytes on Bulk, {} messages on Rpc, "
                         "snapshots on State",
                         connection, peer.Name, peer.Claims, m_Options.BulkBytes, m_Options.Rpcs );
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
            std::string                           Claims; // logged as text: this probe applies no game rule
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
            const HelloPolicy  policy{ m_Options.CookHash, UnixSecondsNow() };
            const HelloVerdict verdict =
                 lane == ENetLane::Control ? EvaluateHello( data, policy, m_Tokens ) : HelloVerdict{};
            if ( !verdict.Accepted )
            {
                Log( "refused connection {} from {}: {} (client protocol {}, cook hash {:016x}, token {} bytes, "
                     "issuer {}, audience {}, expiry {}; server protocol {}, cook hash {:016x}, audience {}, now "
                     "{})",
                     connection, peer.Address, RefuseReasonText( verdict.Reason ), verdict.Hello.ProtocolVersion,
                     verdict.Hello.CookHash, verdict.Hello.Token.size(), verdict.Token.Issuer,
                     verdict.Token.Audience, verdict.Token.ExpiresUnixSeconds, kProtocolVersion,
                     m_Options.CookHash, m_Options.Audience, policy.NowUnixSeconds );
                ++m_Refused;
                m_Transport.Send( connection, EncodeRefuse( verdict.Reason, RefuseReasonText( verdict.Reason ) ),
                                  ENetLane::Control );
                m_Transport.Close( connection, RefuseCloseCode( verdict.Reason ),
                                   RefuseReasonText( verdict.Reason ), true );
                m_Peers.erase( connection );
                return;
            }
            peer.Accepted = true;
            peer.Claims.assign( reinterpret_cast<const char*>( verdict.Token.Claims.data() ),
                                verdict.Token.Claims.size() );
            Log( "accepted connection {} from {}: protocol {}, cook hash {:016x}, claims '{}', join options '{}' "
                 "(token valid for {} s)",
                 connection, peer.Address, verdict.Hello.ProtocolVersion, verdict.Hello.CookHash, peer.Claims,
                 verdict.Hello.JoinOptions, verdict.Token.ExpiresUnixSeconds - policy.NowUnixSeconds );
            m_Transport.Send( connection, EncodeWelcome( m_Options.CookHash, m_Options.PawnPrefab ),
                              ENetLane::Control );
        }

        INetTransport&               m_Transport;
        const Options&               m_Options;
        ConnectTokenVerifier         m_Tokens;
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
            m_Transport.Send(
                 connection,
                 EncodeHello( { kProtocolVersion, m_Options.CookHash, m_Token, m_Options.JoinOptions } ),
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

        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                        NetClock::time_point ) override
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
                Log( "refused by server: {}", answer.ReasonText );
                m_Refused = true;
                m_Result  = 2;
                // The server closes too; closing here ends the attempt now instead of at the transport timeout.
                m_Transport.Close( connection, RefuseCloseCode( answer.Reason ), answer.ReasonText, false );
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

    int GenerateKeys( const Options& options )
    {
        IssuerKeyRecord privateKey, publicKey;
        std::string     error;
        if ( !GenerateIssuerKeys( options.Issuer, options.KeyId, privateKey, publicKey, error ) ||
             !WriteIssuerKeyFile( options.OutPrivate, privateKey, error ) ||
             !WriteIssuerKeyFile( options.OutPublic, publicKey, error ) )
        {
            Log( "issuer keys: {}", error );
            return 1;
        }
        Log( "issuer {} key {}: private key '{}' (the issuer only), public key '{}' (the servers)", options.Issuer,
             options.KeyId, options.OutPrivate, options.OutPublic );
        return 0;
    }

    int MintToken( const Options& options )
    {
        const std::optional<IssuerKeyRecord> key = ReadKey( options.IssuerKeyFile );
        if ( !key )
            return 1;
        ConnectToken token;
        token.Audience           = options.Audience;
        token.IssuedUnixSeconds  = UnixSecondsNow();
        token.ExpiresUnixSeconds = static_cast<std::uint64_t>(
             static_cast<std::int64_t>( token.IssuedUnixSeconds ) + options.ExpiresIn );
        token.Claims.assign( reinterpret_cast<const std::byte*>( options.Claims.data() ),
                             reinterpret_cast<const std::byte*>( options.Claims.data() ) + options.Claims.size() );
        std::string            error;
        std::vector<std::byte> bytes = MintConnectToken( token, *key, error );
        if ( bytes.empty() )
        {
            Log( "mint token: {}", error );
            return 1;
        }
        std::printf( "%s\n", ToHex( bytes ).c_str() );
        return 0;
    }

    // A HEADLESS PLAYER (NET-1 scenario net1_two_clients; NET-ARCH §9 NET-10's bot client): the same
    // ReplicationClient the engine's net driver runs, input sent at the 60 Hz client step.
    //   --bot --connect IP:PORT --cooked DIR --token HEX --name N --seconds S [--walk-cm D --dir x,y,z
    //         --turn-deg-s R] [--waypoints x,y,z;x,y,z...] [--lag-ms L --loss P]
    //         [--hold-actions MASK --hold-from S --hold-until S]
    // --hold-actions holds the pawn's declared action bits MASK (Gameplay::PawnActions: bit i = the prefab's i-th
    // declared action - IA_Jump 1, IA_Sprint 2, IA_Crouch 4 for a CharacterController with the default names)
    // from --hold-from to --hold-until seconds (PAWN-MANNEQUIN: a proxy that crouches in front of an observer).
    // --waypoints walks a SHUTTLE instead (NET-5's remote walk in front of an observer): the bot steers its pawn
    // straight at the current point from the SERVER's newest position of it (closed loop, so a slope or a push
    // cannot carry it off the path), turns back at each end of the list and walks until --seconds; only x and z
    // steer (the server's character keeps the pawn on the ground), and |dir| is the speed fraction.
    // --turn-deg-s turns the walk direction about +Y at R degrees per second (a curve: radius = speed / R), and
    // the pawn faces where it walks (NET-5's remote walk). |dir| below 1 walks slower (the server scales the
    // move). Walks its own pawn D cm along dir (server-authoritative: it stops when the SERVER's position of its
    // pawn has moved D cm), then idles; logs its own pawn and every remote pawn (interpolated) twice a second and
    // a final line per pawn with its displacement from where this client first saw it.
    // The action bits the bot holds and when (--hold-actions / --hold-from / --hold-until).
    struct HeldActions
    {
        std::uint32_t Mask  = 0;
        float         From  = 0.0F;
        float         Until = 0.0F;

        [[nodiscard]] std::uint32_t At( double seconds ) const
        {
            return seconds >= From && seconds < Until ? Mask : 0u;
        }
    };

    int RunBot( int argc, char** argv )
    {
        std::string connect, cooked, tokenHex, name = "bot", joinOptions;
        // --protocol: a client of another protocol version (NET-AUTH-SPLIT N-A02: the server refuses it first).
        std::uint32_t        protocol = kProtocolVersion;
        double      seconds = 10.0, walkCm = 0.0, turnDegPerSecond = 0.0;
        NetVec3     dir{ 1.f, 0.f, 0.f };
        std::vector<NetVec3> waypoints;
        HeldActions          held;
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
            else if ( arg == "--join-options" )
                joinOptions = value;
            else if ( arg == "--protocol" )
                protocol = static_cast<std::uint32_t>( std::strtoul( value.c_str(), nullptr, 10 ) );
            else if ( arg == "--name" )
                name = value;
            else if ( arg == "--seconds" )
                seconds = std::atof( value.c_str() );
            else if ( arg == "--walk-cm" )
                walkCm = std::atof( value.c_str() );
            else if ( arg == "--hold-actions" )
                held.Mask = static_cast<std::uint32_t>( std::strtoul( value.c_str(), nullptr, 10 ) );
            else if ( arg == "--hold-from" )
                held.From = static_cast<float>( std::atof( value.c_str() ) );
            else if ( arg == "--hold-until" )
                held.Until = static_cast<float>( std::atof( value.c_str() ) );
            else if ( arg == "--turn-deg-s" )
                turnDegPerSecond = std::atof( value.c_str() );
            else if ( arg == "--lag-ms" )
                simulator.SendLagMs = std::atoi( value.c_str() );
            else if ( arg == "--loss" )
                simulator.SendLossPercent = static_cast<float>( std::atof( value.c_str() ) );
            else if ( arg == "--waypoints" )
            {
                for ( std::size_t begin = 0; begin <= value.size(); )
                {
                    const std::size_t end = std::min( value.find( ';', begin ), value.size() );
                    NetVec3           point{};
                    if ( std::sscanf( value.substr( begin, end - begin ).c_str(), "%f,%f,%f", &point.X, &point.Y,
                                      &point.Z ) != 3 )
                    {
                        Log( "--waypoints '{}': '{}' is not x,y,z", value, value.substr( begin, end - begin ) );
                        return 1;
                    }
                    waypoints.push_back( point );
                    begin = end + 1;
                }
                if ( waypoints.size() < 2 )
                {
                    Log( "--waypoints '{}' needs at least two points", value );
                    return 1;
                }
            }
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
        if ( held.Mask != 0u && !( held.Until > held.From ) )
        {
            Log( "--hold-actions {} needs --hold-until ({}) above --hold-from ({})", held.Mask, held.Until,
                 held.From );
            return 1;
        }
        const auto token = FromHex( tokenHex );
        if ( connect.empty() || cooked.empty() || !token || seconds <= 0.0 )
        {
            Log( "usage: --bot --connect IP:PORT --cooked DIR --token HEX --name N --seconds S [--walk-cm D --dir "
                 "x,y,z --turn-deg-s R] [--waypoints x,y,z;x,y,z...] [--lag-ms L --loss P] [--join-options TEXT] "
                 "[--protocol V] [--hold-actions MASK --hold-from S --hold-until S]" );
            return 1;
        }
        const std::optional<std::uint64_t> cookHash = ReadCookHash( cooked );
        if ( !cookHash )
            return 1;
        std::optional<BotCells> cells = BotCells::Open( cooked );
        if ( !cells )
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
        ReplicationClient client( *transport, { protocol, *cookHash, *token, joinOptions }, log );
        if ( !client.Connect( connect, error ) )
        {
            Log( "[{}] connect {}: {}", name, connect, error );
            return 1;
        }

        const auto distance = []( const NetVec3& a, const NetVec3& b )
        { return std::sqrt( ( a.X - b.X ) * ( a.X - b.X ) + ( a.Y - b.Y ) * ( a.Y - b.Y ) + ( a.Z - b.Z ) * ( a.Z - b.Z ) ); };
        std::map<NetId, NetVec3> firstSeen;
        bool                     walking      = walkCm > 0.0 || !waypoints.empty();
        const float              shuttleSpeed = std::sqrt( dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z );
        std::size_t              target       = 0;
        int                      heading      = 1; // +1 walks up the list, -1 back down it
        // Within this horizontal distance of the point the bot turns: the server's position it steers from is
        // ~one link delay old, so a tighter radius would overshoot and circle the point.
        constexpr float          kArriveCm = 40.f;
        const auto               start   = std::chrono::steady_clock::now();
        const auto               period  = std::chrono::duration<double>( 1.0 / kNetStepHz );
        std::uint64_t            step    = 0;
        double                   nextLog = 0.5;
        // The previous step's held actions: a step that overruns its slot still logs the boundary it crossed.
        std::uint32_t lastActions = 0u;
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
            if ( walking && waypoints.empty() && client.OwnPawn() != kInvalidNetId &&
                 firstSeen.contains( client.OwnPawn() ) )
            {
                const auto& own = client.Pawns().at( client.OwnPawn() );
                const PawnState newest = own.History.Newest();
                if ( distance( newest.Position, firstSeen[client.OwnPawn()] ) >= walkCm )
                {
                    walking = false;
                    Log( "[{}] walked {:.0f} cm (server position): stopping at step {}", name, walkCm, step );
                }
            }
            if ( !waypoints.empty() )
            {
                dir = {};
                if ( client.OwnPawn() != kInvalidNetId && client.Pawns().contains( client.OwnPawn() ) )
                {
                    const PawnState newest = client.Pawns().at( client.OwnPawn() ).History.Newest();
                    float           dx     = waypoints[target].X - newest.Position.X;
                    float           dz     = waypoints[target].Z - newest.Position.Z;
                    if ( std::sqrt( dx * dx + dz * dz ) < kArriveCm )
                    {
                        const auto next = static_cast<std::ptrdiff_t>( target ) + heading;
                        if ( next < 0 || next >= static_cast<std::ptrdiff_t>( waypoints.size() ) )
                            heading = -heading;
                        target = static_cast<std::size_t>( static_cast<std::ptrdiff_t>( target ) + heading );
                        Log( "[{}] shuttle turn at ({:.0f}, {:.0f}, {:.0f}) for ({:.0f}, {:.0f}), step {}", name,
                             newest.Position.X, newest.Position.Y, newest.Position.Z, waypoints[target].X,
                             waypoints[target].Z, step );
                        dx = waypoints[target].X - newest.Position.X;
                        dz = waypoints[target].Z - newest.Position.Z;
                    }
                    if ( const float length = std::sqrt( dx * dx + dz * dz ); length > 0.f )
                        dir = { dx / length * shuttleSpeed, 0.f, dz / length * shuttleSpeed };
                }
            }
            else if ( walking && turnDegPerSecond != 0.0 )
            {
                const double turn = turnDegPerSecond * 3.14159265358979 / 180.0 / kNetStepHz;
                const float  c = static_cast<float>( std::cos( turn ) ), s = static_cast<float>( std::sin( turn ) );
                dir = { dir.X * c + dir.Z * s, dir.Y, -dir.X * s + dir.Z * c };
            }
            // Facing the walk (the editor's convention: yaw 0 faces -Z).
            const bool  moving = walking && ( dir.X != 0.f || dir.Z != 0.f );
            const float yaw    = moving ? std::atan2( -dir.X, -dir.Z ) : 0.f;
            const std::uint32_t actions = held.At( now );
            if ( actions != lastActions )
                Log( "[{}] actions {} at {:.2f} s (step {})", name, actions, now, step );
            lastActions = actions;
            client.SendInput( InputStep{ .Move = walking ? dir : NetVec3{}, .Yaw = yaw, .Actions = actions } );
            // The cells around the SERVER's position of the own pawn, reported when they change.
            if ( client.OwnPawn() != kInvalidNetId && client.Pawns().contains( client.OwnPawn() ) )
            {
                const PawnState newest = client.Pawns().at( client.OwnPawn() ).History.Newest();
                std::string     said;
                if ( auto report = cells->Update( newest.Position, said ) )
                    client.ReportResidency( report->first, report->second );
                if ( !said.empty() )
                    Log( "[{}] {}", name, said );
            }
            if ( now >= nextLog )
            {
                nextLog += 0.5;
                std::string line;
                // Each pawn's sampled position, then the velocity of its newest received snapshot (the replicated
                // value, UE FRepMovement's LinearVelocity, not the interpolation's tangent).
                for ( const auto& [id, pawn] : client.Pawns() )
                    if ( const auto sample = client.Sample( id ) )
                    {
                        const NetVec3 velocity = pawn.History.Newest().Velocity;
                        line += std::format( " {}{}=({:.0f},{:.0f},{:.0f}) v ({:.0f}, {:.0f}, {:.0f}) cm/s", id,
                                             pawn.Owned ? "*" : "", sample->Position.X, sample->Position.Y,
                                             sample->Position.Z, velocity.X, velocity.Y, velocity.Z );
                    }
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
            const NetVec3 velocity = pawn.History.Newest().Velocity;
            Log( "[{}] FINAL {} pawn net id {} (player {}) at ({:.0f}, {:.0f}, {:.0f}) cm, moved {:.0f} cm from where "
                 "this client first saw it, v ({:.0f}, {:.0f}, {:.0f}) cm/s",
                 name, pawn.Owned ? "own" : "remote", id, pawn.Player, sample->Position.X, sample->Position.Y,
                 sample->Position.Z, distance( sample->Position, firstSeen[id] ), velocity.X, velocity.Y, velocity.Z );
        }
        Log( "[{}] {} pawn(s) replicated; exit {}", name, client.Pawns().size(), result );
        return result;
    }
    // THE HOSTILE MESH PEER (--mesh-peer): see the header. Its own console thread, as DesertServer's
    // ServerConsole.
    class PeerConsole
    {
    public:
        void Start()
        {
            std::thread(
                 [this]
                 {
                     std::string line;
                     while ( std::getline( std::cin, line ) )
                     {
                         const std::scoped_lock lock( m_Mutex );
                         m_Lines.push_back( line );
                     }
                 } )
                 .detach();
        }
        std::vector<std::string> Take()
        {
            const std::scoped_lock lock( m_Mutex );
            return std::exchange( m_Lines, {} );
        }

    private:
        std::mutex               m_Mutex;
        std::vector<std::string> m_Lines;
    };

    // The Offer a defective sender would send: the header names the key, the pawn record carries only its net id
    // (the store reads no further than the key and the pawn's id before it refuses).
    std::vector<std::byte> HostileOffer( std::uint32_t sim, Handover::OfferSerial serial, std::uint32_t to,
                                         NetId id, std::uint32_t epoch, std::uint32_t step )
    {
        PawnSimulation::HandoverPawn pawn;
        pawn.State.Id = id;
        return Handover::EncodeOffer( { { sim, serial }, epoch, to, step, 1 }, pawn );
    }

    int RunMeshPeer( int argc, char** argv )
    {
        std::string   caDirectory, store;
        std::uint32_t sim = 0, storeIndex = 1;
        double        seconds = 900.0;
        for ( int i = 1; i < argc; ++i )
        {
            const std::string arg = argv[i];
            if ( arg == "--mesh-peer" )
                continue;
            if ( i + 1 >= argc )
            {
                Log( "--mesh-peer: {} needs a value", arg );
                return 1;
            }
            const std::string value = argv[++i];
            if ( arg == "--mesh-ca" )
                caDirectory = value;
            else if ( arg == "--store" )
                store = value;
            else if ( arg == "--as-sim" )
                sim = static_cast<std::uint32_t>( std::strtoul( value.c_str(), nullptr, 10 ) );
            else if ( arg == "--store-index" )
                storeIndex = static_cast<std::uint32_t>( std::strtoul( value.c_str(), nullptr, 10 ) );
            else if ( arg == "--seconds" )
                seconds = std::strtod( value.c_str(), nullptr );
            else
            {
                Log( "--mesh-peer: unknown argument '{}'", arg );
                return 1;
            }
        }
        if ( caDirectory.empty() || store.empty() || sim == 0 || storeIndex == 0 || seconds <= 0.0 )
        {
            Log( "--mesh-peer takes --mesh-ca DIR --store IP:P --as-sim N (N >= 1) [--store-index I] [--seconds "
                 "S]" );
            return 1;
        }
        const NetLogSink               netLog    = []( std::string_view line ) { Log( "[Net] {}", line ); };
        std::unique_ptr<INetTransport> transport = CreateGnsTransport( netLog );
        const std::string              identity  = MeshIdentity( EMeshRole::Sim, sim );
        std::string                    error;
        if ( !transport || !transport->UseMeshCertificate( caDirectory, identity, error ) )
        {
            Log( "probe: mesh certificate '{}': {}", identity, transport ? error : std::string( "no transport" ) );
            return 1;
        }
        MeshEndpoint endpoint( *transport, EMeshRole::Sim, sim, 0, netLog );
        bool         lost = false;
        endpoint.SetHandler(
             { [&]( EMeshRole role, std::uint32_t index, ConnectionId connection, std::uint16_t )
               {
                   Log( "probe: mesh link to {} {} up (connection {}) as sim {} - a hostile peer, it holds no "
                        "pawn",
                        MeshRoleName( role ), index, connection, sim );
               },
               [&]( EMeshRole role, std::uint32_t index, ConnectionId )
               {
                   Log( "probe: mesh link to {} {} lost", MeshRoleName( role ), index );
                   lost = true;
               },
               [&]( EMeshRole, std::uint32_t, ConnectionId, std::uint8_t type, std::span<const std::byte> body )
               {
                   if ( !Handover::IsHandoverMessage( type ) )
                       return;
                   const auto         message = static_cast<Handover::EMessage>( type );
                   Handover::Decision decision;
                   Handover::Refusal  refusal;
                   const bool         refused = message == Handover::EMessage::Refused;
                   if ( ( message == Handover::EMessage::Commit || message == Handover::EMessage::Committed ||
                          refused ) &&
                        Handover::DecodeDecision( body, decision, refused ? &refusal : nullptr ) )
                       Log( "probe: received {} of offer (sim {}, #{}) - pawn net id {}{}",
                            Handover::MessageName( message ), decision.Key.From, decision.Key.Serial, decision.Id,
                            refused ? " - " + Handover::Describe( refusal ) : std::string() );
                   else
                       Log( "probe: received {} ({} bytes)", Handover::MessageName( message ), body.size() );
               } } );
        if ( !endpoint.ConnectTo( EMeshRole::Store, storeIndex, store, error ) )
        {
            Log( "probe: --store '{}': {}", store, error );
            return 1;
        }
        PeerConsole console;
        console.Start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>( seconds );
        while ( !lost && std::chrono::steady_clock::now() < deadline )
        {
            transport->Poll( endpoint );
            // One console line may carry several commands separated by ';': they leave in one poll, so a
            // scenario fires both into one open offer without a runner round trip between them.
            std::vector<std::string> commands;
            for ( const std::string& line : console.Take() )
            {
                std::istringstream parts( line );
                for ( std::string command; std::getline( parts, command, ';' ); )
                    commands.push_back( command );
            }
            for ( const std::string& line : commands )
            {
                std::istringstream words( line );
                std::string        verb;
                words >> verb;
                const std::optional<ConnectionId> link = endpoint.PeerConnection( EMeshRole::Store, storeIndex );
                if ( !link )
                {
                    Log( "probe: '{}' not sent - no mesh link to store {} yet", line, storeIndex );
                    continue;
                }
                Handover::OfferSerial serial = 0;
                std::uint32_t         to = 0, epoch = 0, step = 0;
                NetId                 id = kInvalidNetId;
                if ( verb == "dup" && words >> serial >> to >> id >> epoch >> step )
                {
                    const std::vector<std::byte> offer = HostileOffer( sim, serial, to, id, epoch, step );
                    endpoint.Send( *link, offer );
                    Log( "probe: sent a duplicate Offer under the open key (sim {}, #{}): pawn net id {} to sim "
                         "{}, "
                         "epoch {}, step {} ({} bytes)",
                         sim, serial, id, to, epoch, step, offer.size() );
                }
                else if ( verb == "undecodable" && words >> serial >> to )
                {
                    // The type byte and the whole header (key, epoch, to, step, gateway: 6 x u32); the pawn record
                    // that follows is cut off, so the key is readable and the record is not.
                    constexpr std::size_t  kHeaderBytes = 1 + 6 * sizeof( std::uint32_t );
                    std::vector<std::byte> offer        = HostileOffer( sim, serial, to, kInvalidNetId, 1, 0 );
                    offer.resize( std::min( offer.size(), kHeaderBytes ) );
                    endpoint.Send( *link, offer );
                    Log( "probe: sent an undecodable Offer: header key (sim {}, #{}) to sim {}, pawn record cut "
                         "off "
                         "({} bytes)",
                         sim, serial, to, offer.size() );
                }
                else
                    Log( "probe: unknown console line '{}' (dup <serial> <to> <net id> <epoch> <step> | "
                         "undecodable <serial> <to>)",
                         line );
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        Log( "probe: {} after {:.0f} s", lost ? "mesh link lost" : "done", seconds );
        return lost ? 1 : 0;
    }
} // namespace

int main( int argc, char** argv )
{
    for ( int i = 1; i < argc; ++i )
        if ( std::strcmp( argv[i], "--mesh-peer" ) == 0 )
            return RunMeshPeer( argc, argv );
    for ( int i = 1; i < argc; ++i )
        if ( std::strcmp( argv[i], "--bot" ) == 0 )
            return RunBot( argc, argv );
    std::optional<Options> options = ParseOptions( argc, argv );
    if ( !options )
        return 1;
    if ( options->GenKeys )
        return GenerateKeys( *options );
    if ( options->MintToken )
        return MintToken( *options );

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
        const std::optional<IssuerKeyRecord> issuerKey = ReadKey( options->IssuerKeyFile );
        if ( !issuerKey )
            return 1;
        if ( !transport->Listen( options->Port, error ) )
        {
            Log( "{}", error );
            return 1;
        }
        Log( "server {} listening on UDP {}: protocol {}, cooked world '{}' cook hash {:016x}; expecting {} "
             "client(s) and {} refusal(s)",
             options->Audience, options->Port, kProtocolVersion, options->Cooked, options->CookHash,
             options->Clients, options->Refusals );
        ProbeServer server( *transport, *options, ConnectTokenVerifier( { *issuerKey }, options->Audience ) );
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
