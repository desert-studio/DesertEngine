// INetTransport over Valve GameNetworkingSockets (NET-ARCH §1). GNS keeps its own socket thread; this file only
// translates: connection handles are the ConnectionIds (both are non-zero 32-bit, 0 = invalid), status-change
// callbacks become OnConnected/OnClosed, poll-group messages become OnMessage, ENetLane is the GNS lane index
// (ConfigureLanes sets every connection's lanes the moment it exists, on both ends).

#include "Net/NetTransport.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>

namespace Desert::Net
{
    namespace
    {
        // GNS reports through process-global C callbacks without a user pointer; there is one transport per
        // process (CreateGnsTransport refuses a second), so these name it.
        NetLogSink           g_LogSink;
        INetTransportEvents* g_PollEvents = nullptr;
        HSteamNetPollGroup   g_PollGroup  = k_HSteamNetPollGroup_Invalid;
        std::atomic<bool>    g_Alive{ false };

        void GnsDebugOutput( ESteamNetworkingSocketsDebugOutputType type, const char* message )
        {
            if ( g_LogSink )
            {
                g_LogSink( std::format( "[GNS {}] {}", static_cast<int>( type ), message ) );
            }
        }

        // The sending side's lanes (GNS lanes are configured per direction, by the sender): index = ENetLane.
        // Lower priority value = sent first; weights share bandwidth between lanes of one priority.
        constexpr std::array<int, kNetLaneCount>    kLanePriorities = { 0, 1, 1, 1 };
        constexpr std::array<uint16, kNetLaneCount> kLaneWeights    = { 1, 50, 10, 40 };
        static_assert( static_cast<int>( ENetLane::Control ) == 0 && static_cast<int>( ENetLane::Rpc ) == 1 &&
                       static_cast<int>( ENetLane::Bulk ) == 2 && static_cast<int>( ENetLane::State ) == 3 );

        bool ConfigureLanes( ISteamNetworkingSockets* sockets, HSteamNetConnection connection )
        {
            return sockets->ConfigureConnectionLanes( connection, kNetLaneCount, kLanePriorities.data(),
                                                      kLaneWeights.data() ) == k_EResultOK;
        }

        std::string AddressText( const SteamNetworkingIPAddr& address )
        {
            char text[SteamNetworkingIPAddr::k_cchMaxString];
            address.ToString( text, sizeof( text ), true );
            return text;
        }

        class GnsTransport final : public INetTransport
        {
        public:
            GnsTransport() : m_Sockets( SteamNetworkingSockets() ), m_PollGroup( m_Sockets->CreatePollGroup() )
            {
                g_PollGroup = m_PollGroup;
            }

            ~GnsTransport() override
            {
                if ( m_ListenSocket != k_HSteamListenSocket_Invalid )
                {
                    m_Sockets->CloseListenSocket( m_ListenSocket );
                }
                m_Sockets->DestroyPollGroup( m_PollGroup );
                g_PollGroup = k_HSteamNetPollGroup_Invalid;
                GameNetworkingSockets_Kill();
                g_LogSink = nullptr;
                g_Alive   = false;
            }

            bool Listen( std::uint16_t port, std::string& error ) override
            {
                if ( m_ListenSocket != k_HSteamListenSocket_Invalid )
                {
                    error = "already listening";
                    return false;
                }
                SteamNetworkingIPAddr local;
                local.Clear();
                local.m_port                              = port;
                const SteamNetworkingConfigValue_t option = StatusCallbackOption();
                m_ListenSocket                            = m_Sockets->CreateListenSocketIP( local, 1, &option );
                if ( m_ListenSocket == k_HSteamListenSocket_Invalid )
                {
                    error = std::format( "cannot listen on UDP port {}", port );
                    return false;
                }
                return true;
            }

            ConnectionId Connect( std::string_view address, std::string& error ) override
            {
                SteamNetworkingIPAddr remote;
                remote.Clear();
                const std::string text( address );
                if ( !remote.ParseString( text.c_str() ) || remote.m_port == 0 )
                {
                    error = std::format( "'{}' is not an ip:port address", address );
                    return kInvalidConnection;
                }
                const SteamNetworkingConfigValue_t option = StatusCallbackOption();
                const HSteamNetConnection connection      = m_Sockets->ConnectByIPAddress( remote, 1, &option );
                if ( connection == k_HSteamNetConnection_Invalid )
                {
                    error = std::format( "cannot start a connection to {}", address );
                    return kInvalidConnection;
                }
                if ( !ConfigureLanes( m_Sockets, connection ) )
                {
                    m_Sockets->CloseConnection( connection, 0, "lane configuration refused", false );
                    error =
                         std::format( "connection to {}: the transport refused the lane configuration", address );
                    return kInvalidConnection;
                }
                m_Sockets->SetConnectionPollGroup( connection, m_PollGroup );
                return connection;
            }

            ESendResult Send( ConnectionId connection, std::span<const std::byte> data, ENetLane lane ) override
            {
                SteamNetworkingMessage_t* message =
                     SteamNetworkingUtils()->AllocateMessage( static_cast<int>( data.size() ) );
                if ( !data.empty() )
                {
                    std::memcpy( message->m_pData, data.data(), data.size() );
                }
                message->m_conn = connection;
                message->m_nFlags =
                     IsReliableLane( lane ) ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
                message->m_idxLane = static_cast<uint16>( lane );
                int64 result       = 0;
                // SendMessages takes ownership of the message whatever the outcome.
                m_Sockets->SendMessages( 1, &message, &result );
                if ( result > 0 )
                {
                    return ESendResult::Sent;
                }
                return -result == k_EResultLimitExceeded ? ESendResult::QueueFull : ESendResult::NoConnection;
            }

            void Poll( INetTransportEvents& events ) override
            {
                // Messages first: a connection's last messages (a Refuse before the close) must be delivered
                // before the status change that closes the connection and drops whatever it still holds.
                SteamNetworkingMessage_t* messages[64];
                for ( ;; )
                {
                    const int count = m_Sockets->ReceiveMessagesOnPollGroup( m_PollGroup, messages, 64 );
                    for ( int i = 0; i < count; ++i )
                    {
                        // The peer chooses its own lane count: a lane we do not have is not our protocol.
                        if ( messages[i]->m_idxLane < kNetLaneCount )
                        {
                            events.OnMessage( messages[i]->m_conn, static_cast<ENetLane>( messages[i]->m_idxLane ),
                                              std::span( static_cast<const std::byte*>( messages[i]->m_pData ),
                                                         static_cast<std::size_t>( messages[i]->m_cbSize ) ) );
                        }
                        else if ( g_LogSink )
                        {
                            g_LogSink( std::format( "connection {}: message on unknown lane {} dropped",
                                                    messages[i]->m_conn, messages[i]->m_idxLane ) );
                        }
                        messages[i]->Release();
                    }
                    if ( count < 64 )
                    {
                        break;
                    }
                }
                g_PollEvents = &events;
                m_Sockets->RunCallbacks();
                g_PollEvents = nullptr;
            }

            void Close( ConnectionId connection, int code, std::string_view reason, bool flush ) override
            {
                const std::string text( reason );
                m_Sockets->CloseConnection( connection, code, text.c_str(), flush );
            }

            ConnectionStats GetStats( ConnectionId connection ) const override
            {
                ConnectionStats                    stats;
                SteamNetConnectionRealTimeStatus_t status;
                if ( m_Sockets->GetConnectionRealTimeStatus( connection, &status, 0, nullptr ) != k_EResultOK )
                {
                    return stats;
                }
                stats.PingMs               = status.m_nPing;
                stats.QualityLocal         = status.m_flConnectionQualityLocal;
                stats.QualityRemote        = status.m_flConnectionQualityRemote;
                stats.OutBytesPerSecond    = status.m_flOutBytesPerSec;
                stats.InBytesPerSecond     = status.m_flInBytesPerSec;
                stats.PendingReliableBytes = status.m_cbPendingReliable;
                return stats;
            }

            std::string DescribeConnection( ConnectionId connection ) const override
            {
                char text[4096];
                if ( m_Sockets->GetDetailedConnectionStatus( connection, text, sizeof( text ) ) < 0 )
                {
                    return std::format( "connection {} unknown to the transport", connection );
                }
                return text;
            }

            bool SetSimulator( const NetSimulator& simulator, std::string& error ) override
            {
                ISteamNetworkingUtils* utils = SteamNetworkingUtils();
                if ( !utils->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketLag_Send,
                                                        simulator.SendLagMs ) )
                {
                    error = "FakePacketLag_Send refused";
                    return false;
                }
                if ( !utils->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketLoss_Send,
                                                        simulator.SendLossPercent ) )
                {
                    error = "FakePacketLoss_Send refused";
                    return false;
                }
                if ( !utils->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg,
                                                        simulator.SendJitterAverageMs ) )
                {
                    error = "FakePacketJitter_Send_Avg refused";
                    return false;
                }
                return true;
            }

            bool SetSendRate( int minBytesPerSecond, int maxBytesPerSecond, std::string& error ) override
            {
                if ( minBytesPerSecond <= 0 || maxBytesPerSecond < minBytesPerSecond )
                {
                    error = std::format( "send rate {}..{} B/s is not a range", minBytesPerSecond,
                                         maxBytesPerSecond );
                    return false;
                }
                ISteamNetworkingUtils* utils = SteamNetworkingUtils();
                if ( !utils->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_SendRateMin,
                                                        minBytesPerSecond ) ||
                     !utils->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_SendRateMax, maxBytesPerSecond ) )
                {
                    error = std::format( "send rate {}..{} B/s refused", minBytesPerSecond, maxBytesPerSecond );
                    return false;
                }
                return true;
            }

        private:
            static SteamNetworkingConfigValue_t StatusCallbackOption()
            {
                SteamNetworkingConfigValue_t option;
                option.SetPtr( k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                               reinterpret_cast<void*>( &OnStatusChanged ) );
                return option;
            }

            static void OnStatusChanged( SteamNetConnectionStatusChangedCallback_t* change );

            ISteamNetworkingSockets* m_Sockets;
            HSteamNetPollGroup       m_PollGroup;
            HSteamListenSocket       m_ListenSocket = k_HSteamListenSocket_Invalid;
        };

        void GnsTransport::OnStatusChanged( SteamNetConnectionStatusChangedCallback_t* change )
        {
            ISteamNetworkingSockets*  sockets    = SteamNetworkingSockets();
            const HSteamNetConnection connection = change->m_hConn;
            switch ( change->m_info.m_eState )
            {
                case k_ESteamNetworkingConnectionState_Connecting:
                    // A peer knocking on our listen socket (a client's own outgoing connection also passes through
                    // Connecting, with no listen socket): accept it into the poll group.
                    if ( change->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid )
                    {
                        if ( sockets->AcceptConnection( connection ) != k_EResultOK ||
                             !ConfigureLanes( sockets, connection ) )
                        {
                            sockets->CloseConnection( connection, 0, "accept failed", false );
                            return;
                        }
                        sockets->SetConnectionPollGroup( connection, g_PollGroup );
                    }
                    break;
                case k_ESteamNetworkingConnectionState_Connected:
                    if ( g_PollEvents )
                    {
                        g_PollEvents->OnConnected( connection, AddressText( change->m_info.m_addrRemote ) );
                    }
                    break;
                case k_ESteamNetworkingConnectionState_ClosedByPeer:
                case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                    // Reported whether or not the connection was ever up: a failed Connect (Connecting -> Closed)
                    // must tell the caller why it ended.
                    if ( g_PollEvents )
                    {
                        DisconnectInfo info;
                        info.Code   = change->m_info.m_eEndReason;
                        info.Reason = change->m_info.m_szEndDebug;
                        info.ClosedByPeer =
                             change->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer;
                        g_PollEvents->OnClosed( connection, info );
                    }
                    sockets->CloseConnection( connection, 0, nullptr, false );
                    break;
                default:
                    break;
            }
        }
    } // namespace

    std::unique_ptr<INetTransport> CreateGnsTransport( NetLogSink log )
    {
        if ( g_Alive.exchange( true ) )
        {
            if ( log )
            {
                log( "CreateGnsTransport: a transport already exists in this process (GNS is process-global)" );
            }
            return nullptr;
        }
        g_LogSink = std::move( log );
        SteamNetworkingErrMsg error;
        if ( !GameNetworkingSockets_Init( nullptr, error ) )
        {
            if ( g_LogSink )
            {
                g_LogSink( std::format( "GameNetworkingSockets_Init failed: {}", error ) );
            }
            g_LogSink = nullptr;
            g_Alive   = false;
            return nullptr;
        }
        SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Msg,
                                                        &GnsDebugOutput );
        return std::make_unique<GnsTransport>();
    }
} // namespace Desert::Net
