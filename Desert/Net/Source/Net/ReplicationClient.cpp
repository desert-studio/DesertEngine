#include "ReplicationClient.hpp"

#include <format>
#include <utility>
#include <string_view>

namespace Desert::Net
{
    ReplicationClient::ReplicationClient( INetTransport& transport, HelloMessage hello, NetLogSink log )
         : m_Transport( transport ), m_Hello( std::move( hello ) ), m_Log( std::move( log ) )
    {
    }

    bool ReplicationClient::Connect( std::string_view address, std::string& error )
    {
        m_Connection = m_Transport.Connect( address, error );
        return m_Connection != kInvalidConnection;
    }

    void ReplicationClient::Poll()
    {
        m_Transport.Poll( *this );
    }

    void ReplicationClient::SendInput( const NetVec3& move, float yaw )
    {
        if ( m_State != EState::InSession )
            return;
        m_Inputs.Push( { m_NextStep++, move, yaw } );
        InputMessage message;
        message.Sequence = m_NextSequence++;
        message.Steps    = m_Inputs.Latest( kInputRedundancy );
        m_Transport.Send( m_Connection, EncodeInput( message ), ENetLane::State );
    }

    void ReplicationClient::OnConnected( ConnectionId connection, std::string_view address )
    {
        m_Log( std::format( "connected to {}: sending Hello", address ) );
        m_Transport.Send( connection, EncodeHello( m_Hello ), ENetLane::Control );
    }

    void ReplicationClient::OnClosed( ConnectionId, const DisconnectInfo& info )
    {
        m_State       = EState::Closed;
        m_CloseReason = info.Reason;
        m_Log( std::format( "connection closed ({}{})", info.Code,
                            info.Reason.empty() ? std::string() : ": " + info.Reason ) );
    }

    void ReplicationClient::OnMessage( ConnectionId, ENetLane lane, std::span<const std::byte> data )
    {
        if ( m_State == EState::Connecting )
        {
            if ( lane != ENetLane::Control )
                return;
            const HandshakeAnswer answer = DecodeAnswer( data );
            if ( answer.Kind == HandshakeAnswer::EKind::Welcome )
            {
                m_State      = EState::InSession;
                m_PawnPrefab = answer.PawnPrefab;
                m_Log( std::format( "welcomed: server cook hash {:016x}, pawn prefab {:016x}",
                                    answer.ServerCookHash, answer.PawnPrefab ) );
            }
            else
            {
                m_State       = EState::Closed;
                m_CloseReason = answer.Kind == HandshakeAnswer::EKind::Refuse
                                     ? std::string( RefuseReasonText( answer.Reason ) )
                                     : std::string( "malformed handshake answer" );
                m_Log( std::format( "refused: {}", m_CloseReason ) );
            }
            return;
        }
        switch ( PeekReplicationMessage( data ).value_or( EReplicationMessage{} ) )
        {
            case EReplicationMessage::Spawn:
                if ( const auto spawn = DecodeSpawn( data ) )
                {
                    RemotePawn& pawn = m_Pawns[spawn->Pawn.Id];
                    pawn.Id          = spawn->Pawn.Id;
                    pawn.Player      = spawn->Player;
                    pawn.Owned       = spawn->Owned;
                    pawn.History     = {};
                    // The spawn transform is the first sample, at the newest server time known.
                    pawn.History.Push( m_NewestServerStep ? *m_NewestServerStep / kNetStepHz : 0.0, spawn->Pawn );
                    if ( spawn->Owned )
                        m_Own = spawn->Pawn.Id;
                    m_Spawned.push_back( spawn->Pawn.Id );
                    m_Log( std::format( "spawn: pawn net id {}{} at ({:.0f}, {:.0f}, {:.0f}) cm", spawn->Pawn.Id,
                                        spawn->Owned ? " (own)" : "", spawn->Pawn.Position.X,
                                        spawn->Pawn.Position.Y, spawn->Pawn.Position.Z ) );
                }
                break;
            case EReplicationMessage::Destroy:
                if ( const auto id = DecodeDestroy( data ); id && m_Pawns.erase( *id ) > 0 )
                {
                    if ( m_Own == *id )
                        m_Own = kInvalidNetId;
                    m_Destroyed.push_back( *id );
                    m_Log( std::format( "destroy: pawn net id {}", *id ) );
                }
                break;
            case EReplicationMessage::Snapshot:
                if ( const auto snapshot = DecodeSnapshot( data ) )
                {
                    if ( !m_NewestServerStep || StepAfter( snapshot->ServerStep, *m_NewestServerStep ) )
                    {
                        m_NewestServerStep = snapshot->ServerStep;
                        m_NewestArrival    = Clock::now();
                    }
                    if ( snapshot->HasAck && ( !m_LastAck || StepAfter( snapshot->AckInputStep, *m_LastAck ) ) )
                    {
                        m_LastAck = snapshot->AckInputStep;
                        m_Inputs.DropUpTo( snapshot->AckInputStep );
                    }
                    const double time = snapshot->ServerStep / kNetStepHz;
                    for ( const PawnState& state : snapshot->Pawns )
                        if ( auto it = m_Pawns.find( state.Id ); it != m_Pawns.end() )
                            it->second.History.Push( time, state );
                }
                break;
            default:
                break;
        }
    }

    double ReplicationClient::RenderServerSeconds() const
    {
        if ( !m_NewestServerStep )
            return 0.0;
        const double sinceArrival = std::chrono::duration<double>( Clock::now() - m_NewestArrival ).count();
        return *m_NewestServerStep / kNetStepHz + sinceArrival - kInterpolationDelaySeconds;
    }

    std::optional<PawnState> ReplicationClient::Sample( NetId id ) const
    {
        const auto it = m_Pawns.find( id );
        if ( it == m_Pawns.end() || it->second.History.Empty() )
            return std::nullopt;
        return it->second.History.Sample( RenderServerSeconds() );
    }

    std::vector<NetId> ReplicationClient::TakeSpawned()
    {
        return std::exchange( m_Spawned, {} );
    }

    std::vector<NetId> ReplicationClient::TakeDestroyed()
    {
        return std::exchange( m_Destroyed, {} );
    }
} // namespace Desert::Net
