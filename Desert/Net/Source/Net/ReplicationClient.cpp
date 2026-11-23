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

    void ReplicationClient::OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data )
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
                if ( const auto destroy = DecodeDestroy( data ) )
                {
                    const auto it = m_Pawns.find( destroy->Id );
                    if ( it == m_Pawns.end() )
                        break;
                    const std::uint32_t player = it->second.Player;
                    m_Pawns.erase( it );
                    if ( m_Own == destroy->Id )
                        m_Own = kInvalidNetId;
                    m_Destroyed.push_back( destroy->Id );
                    if ( destroy->Reason == EDestroyReason::NotRelevant )
                        m_Log( std::format( "relevancy: pawn net id {} (player {}) removed from relevancy - the "
                                            "server sees no cell of it resident here",
                                            destroy->Id, player ) );
                    else
                        m_Log( std::format( "destroy: pawn net id {}", destroy->Id ) );
                }
                break;
            case EReplicationMessage::StableDestroy:
                if ( const auto delta = DecodeStableDestroy( data ) )
                {
                    std::string ids;
                    for ( const StableNetId id : delta->Ids )
                        ids += std::format( "{}{}", ids.empty() ? "" : ", ", id );
                    m_Log( std::format( "world delta: cell unit {} - stable {} destroyed on the server",
                                        delta->Unit, ids ) );
                    m_StableDestroyed.insert( m_StableDestroyed.end(), delta->Ids.begin(), delta->Ids.end() );
                }
                else
                    m_Log( "malformed StableDestroy dropped" );
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
            case EReplicationMessage::ObjectState:
                if ( const auto state = DecodeObjectState( data ) )
                {
                    m_StateAcks.Received( state->Sequence );
                    // Acked at once: the server's baseline moves as soon as possible, and the ack is repeated in
                    // the next 32 acks' window, so a lost ack costs nothing.
                    m_Transport.Send( connection, EncodeStateAck( m_StateAcks.Message() ), ENetLane::State );
                    for ( const ObjectUpdate& object : state->Objects )
                        for ( const FieldUpdate& field : object.Fields )
                        {
                            // Unreliable packets may arrive out of order: a field takes only a newer version.
                            std::uint32_t& known =
                                 m_StableVersions[{ object.Id, field.Component + "." + field.Field }];
                            if ( field.Version <= known )
                                continue;
                            known = field.Version;
                            m_StableChanges.push_back( { object.Id, field } );
                            m_Log( std::format( "state: stable {} {}.{} v{} = {}", object.Id, field.Component,
                                                field.Field, field.Version, ReplicatedValueText( field.Value ) ) );
                        }
                }
                else
                    m_Log( "malformed ObjectState dropped" );
                break;
            default:
                break;
        }
    }

    std::vector<ReplicationClient::StableChange> ReplicationClient::TakeStableChanges()
    {
        return std::exchange( m_StableChanges, {} );
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

    std::vector<StableNetId> ReplicationClient::TakeStableDestroyed()
    {
        return std::exchange( m_StableDestroyed, {} );
    }

    void ReplicationClient::ReportResidency( const CellResidencyMessage& change, std::span<const StableNetId> gone )
    {
        if ( m_State != EState::InSession || ( change.Loaded.empty() && change.Unloaded.empty() ) )
            return;
        // The records of an unloaded cell are gone here with it: the server's full baseline of them, when the cell
        // returns, carries the same versions this client already took, so they must be taken again.
        for ( const StableNetId id : gone )
            for ( auto it = m_StableVersions.lower_bound( { id, std::string() } );
                  it != m_StableVersions.end() && it->first.first == id; )
                it = m_StableVersions.erase( it );
        m_Transport.Send( m_Connection, EncodeCellResidency( change ), ENetLane::Rpc );
    }
} // namespace Desert::Net
