#include "ReplicationClient.hpp"

#include <algorithm>
#include <cmath>
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
        AdvanceRenderClock();
    }

    double ReplicationClient::LocalSeconds() const
    {
        return std::chrono::duration<double>( Clock::now() - m_Epoch ).count();
    }

    void ReplicationClient::MeasureArrival( double serverSeconds, NetClock::time_point received )
    {
        // The transport's receive time, not this call's: Poll runs once per frame, and a frame stall is not
        // network jitter (stamping at Poll counted every editor hitch into the p95 and pinned the delay at its
        // cap).
        const double local = std::chrono::duration<double>( received - m_Epoch ).count();
        m_Arrivals.emplace_back( local, local - serverSeconds );
        while ( m_Arrivals.size() > 8 && m_Arrivals.front().first < local - kJitterWindowSeconds )
            m_Arrivals.pop_front();
        std::vector<double> offsets;
        offsets.reserve( m_Arrivals.size() );
        for ( const auto& [at, offset] : m_Arrivals )
            offsets.push_back( offset );
        std::sort( offsets.begin(), offsets.end() );
        m_FastestOffset = offsets.front();
        m_Jitter = offsets[static_cast<std::size_t>( 0.95 * static_cast<double>( offsets.size() - 1 ) )] -
                   m_FastestOffset;
    }

    void ReplicationClient::AdvanceRenderClock()
    {
        const double local = LocalSeconds();
        // The real elapsed time, however long the frame was: the render clock is local time minus the delay, so a
        // stall advances it by the stall (clamping dt made every stall a snap).
        const double dt    = std::max( local - m_LastAdvance, 0.0 );
        m_LastAdvance      = local;
        if ( m_Arrivals.empty() )
            return;
        // Two snapshot intervals (the pair the pawn is drawn between) plus the jitter, never below the one-lost-
        // snapshot floor; the delay grows quickly (a late snapshot is a visible hold) and shrinks slowly.
        constexpr double interval = kSnapshotEverySteps / kNetStepHz;
        const double     target =
             std::clamp( 2.0 * interval + m_Jitter, kInterpolationDelaySeconds, kMaxInterpolationDelaySeconds );
        m_Delay += std::clamp( target - m_Delay, -0.05 * dt, 0.5 * dt );
        const double wanted = local - m_FastestOffset - m_Delay;
        // The error is measured against where the clock lands at local rate (render + dt), not where it was: the
        // old form counted the frame's own dt as error, ran the clock 10 % fast every frame and drifted it seconds
        // ahead until it snapped. The correction closes the error smoothly, never faster than kClockDilation of
        // dt.
        const double predicted = m_RenderSeconds ? *m_RenderSeconds + dt : wanted;
        const double error     = wanted - predicted;
        if ( !m_RenderSeconds || std::abs( error ) > kClockSnapSeconds )
        {
            m_ClockSnaps += m_RenderSeconds ? 1 : 0;
            m_RenderSeconds = wanted;
        }
        else
            m_RenderSeconds = predicted + std::clamp( error * std::min( 1.0, 4.0 * dt ), -kClockDilation * dt,
                                                      kClockDilation * dt );
        if ( local >= m_NextDelayLog )
        {
            m_NextDelayLog = local + 1.0;
            m_Log( std::format(
                 "interpolation buffer: delay {:.0f} ms (target {:.0f}), arrival jitter p95 {:.0f} ms over "
                 "{} snapshots, render clock {:+.1f} ms off before correction ({} snap(s))",
                 m_Delay * 1e3, target * 1e3, m_Jitter * 1e3, m_Arrivals.size(), -error * 1e3, m_ClockSnaps ) );
        }
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

    void ReplicationClient::OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                                       NetClock::time_point received )
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
                                            "server finds its finest cell out of this client's loading range",
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
                    if ( !delta->Ids.empty() )
                        m_Log( std::format( "world delta: cell unit {} - stable {} destroyed on the server",
                                            delta->Unit, ids ) );
                    m_AcknowledgedUnits.push_back( delta->Unit );
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
                    }
                    if ( snapshot->HasAck && ( !m_LastAck || StepAfter( snapshot->AckInputStep, *m_LastAck ) ) )
                    {
                        m_LastAck = snapshot->AckInputStep;
                        m_Inputs.DropUpTo( snapshot->AckInputStep );
                    }
                    const double time = snapshot->ServerStep / kNetStepHz;
                    MeasureArrival( time, received );
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
        return m_RenderSeconds.value_or( 0.0 );
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

    std::vector<std::uint32_t> ReplicationClient::TakeAcknowledgedUnits()
    {
        return std::exchange( m_AcknowledgedUnits, {} );
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
