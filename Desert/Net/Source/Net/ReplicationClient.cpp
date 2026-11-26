#include "ReplicationClient.hpp"

#include <Net/InputWire.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
        RollInputWindow( LocalSeconds() );
    }

    void ReplicationClient::RollInputWindow( double local )
    {
        const double elapsed = local - m_InputWindowStart;
        if ( elapsed < 1.0 )
            return;
        // Per second of the window as it ran: a frame stall that stretched it past one second is not counted as
        // one second's worth.
        m_InputBytesPerSecond =
             static_cast<std::size_t>( std::lround( static_cast<double>( m_InputWindowBytes ) / elapsed ) );
        m_InputMessagesPerSecond =
             static_cast<std::size_t>( std::lround( static_cast<double>( m_InputWindowMessages ) / elapsed ) );
        m_InputWindowStart    = local;
        m_InputWindowBytes    = 0;
        m_InputWindowMessages = 0;
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
        m_SnapshotInterval = SnapshotInterval();
    }

    std::optional<double> ReplicationClient::SnapshotInterval() const
    {
        // Server times, not arrival times: the server's steps are exact, the arrivals carry the jitter.
        std::vector<double> server;
        server.reserve( m_Arrivals.size() );
        for ( const auto& [at, offset] : m_Arrivals )
            server.push_back( at - offset );
        std::sort( server.begin(), server.end() );
        std::vector<double> intervals;
        intervals.reserve( server.size() );
        for ( std::size_t i = 1; i < server.size(); ++i )
            if ( const double interval = server[i] - server[i - 1]; interval > 0.5 / kNetStepHz )
                intervals.push_back( interval );
        if ( intervals.empty() )
            return std::nullopt;
        const auto middle = intervals.begin() + static_cast<std::ptrdiff_t>( intervals.size() / 2 );
        std::nth_element( intervals.begin(), middle, intervals.end() );
        return *middle;
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
        // snapshot floor; the delay grows quickly (a late snapshot is a visible hold) and shrinks slowly. The
        // interval is the cadence measured (the project's Net settings set it on the server); until two snapshots
        // have come the delay stays where it starts, at the floor.
        const std::optional<double>& interval = m_SnapshotInterval;
        const double                 target =
             interval ? std::clamp( 2.0 * *interval + m_Jitter, kInterpolationDelaySeconds,
                                    kMaxInterpolationDelaySeconds )
                      : m_Delay;
        m_Delay += std::clamp( target - m_Delay, -0.05 * dt, 0.5 * dt );
        // WP-FAR-15: a pawn sent every N-th step is drawn two of ITS intervals behind (Relevancy.hpp
        // PawnInterpolationDelay), the part beyond the connection's delay sliding at the same rates. While the
        // newest sample stands still the extra part is held where it is (UE's per-actor interpolation buffer keeps
        // its delay through an idle actor): a dormant pawn is sent at its band's period, so its target does not
        // move, and sliding it toward zero would make the pawn hitch while the delay grows back when it walks.
        constexpr double kStandingCmPerS2 = 1.0; // below 1 cm/s
        for ( auto& [id, pawn] : m_Pawns )
        {
            const NetVec3 velocity = pawn.History.Newest().Velocity;
            if ( velocity.X * velocity.X + velocity.Y * velocity.Y + velocity.Z * velocity.Z < kStandingCmPerS2 )
                continue;
            const double extra =
                 std::max( PawnInterpolationDelay( pawn.PeriodSteps, m_Jitter, m_Delay ) - m_Delay, 0.0 );
            pawn.ExtraDelay += std::clamp( extra - pawn.ExtraDelay, -0.05 * dt, 0.5 * dt );
        }
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

    std::optional<InputStep> ReplicationClient::RecordInput( InputStep input )
    {
        if ( m_State != EState::InSession )
            return std::nullopt;
        // The one place a step gets its number is the one place it is snapped: the ring, the prediction and the
        // server all step the lattice value (NET-INPUT-PACK D7).
        input      = OnWireLattice( input );
        input.Step = m_NextStep++;
        m_Inputs.Push( input );
        return input;
    }

    void ReplicationClient::FlushInputs()
    {
        if ( m_State != EState::InSession )
            return;
        InputMessage message;
        message.Steps = m_Inputs.Unacked(); // each unacked step again, until the ack drops it (UE ServerMove)
        if ( message.Steps.empty() )
            return;
        // A refused run burns its sequence id: the server reads sequence gaps as lost packets.
        message.Sequence                   = m_NextSequence++;
        const std::vector<std::byte> bytes = EncodeInput( message );
        if ( bytes.empty() )
        {
            if ( m_RefusedFrom == message.Steps.front().Step )
                return;
            m_RefusedFrom = message.Steps.front().Step;
            m_Log( std::format( "input run refused: steps not consecutive or over the wire's limits ({} step(s), "
                                "{}..{}); nothing sent",
                                message.Steps.size(), message.Steps.front().Step, message.Steps.back().Step ) );
            return;
        }
        RollInputWindow( LocalSeconds() );
        m_InputWindowBytes += bytes.size();
        ++m_InputWindowMessages;
        if ( bytes.size() > m_LargestInput.Bytes )
            m_LargestInput = { bytes.size(), message.Steps.size() };
        if ( message.Steps.size() > m_LongestInput.Steps ||
             ( message.Steps.size() == m_LongestInput.Steps && bytes.size() > m_LongestInput.Bytes ) )
            m_LongestInput = { bytes.size(), message.Steps.size() };
        m_Transport.Send( m_Connection, bytes, ENetLane::State );
    }

    std::optional<InputStep> ReplicationClient::SendInput( InputStep input )
    {
        const std::optional<InputStep> step = RecordInput( std::move( input ) );
        FlushInputs();
        return step;
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
                                     ? answer.ReasonText
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
                        m_Log( std::format( "relevancy: pawn net id {} (player {}) left this client's view - the "
                                            "server finds it past the view shape's exit margin",
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
                        m_OwnAuthority =
                             OwnAuthority{ snapshot->AckInputStep, snapshot->OwnState, snapshot->InputBuffered };
                    }
                    const double time = snapshot->ServerStep / kNetStepHz;
                    MeasureArrival( time, received );
                    for ( const SnapshotEntry& entry : snapshot->Pawns )
                        if ( auto it = m_Pawns.find( entry.Pawn.Id ); it != m_Pawns.end() )
                        {
                            it->second.History.Push( time, entry.Pawn );
                            it->second.PeriodSteps = entry.PeriodSteps;
                        }
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
        const RemotePawn& pawn = it->second;
        return pawn.History.Sample( RenderServerSeconds() - pawn.ExtraDelay, PawnExtrapolationCap( pawn.PeriodSteps ) );
    }

    std::optional<double> ReplicationClient::PawnDelay( NetId id ) const
    {
        const auto it = m_Pawns.find( id );
        if ( it == m_Pawns.end() )
            return std::nullopt;
        return m_Delay + it->second.ExtraDelay;
    }

    void ReplicationClient::ReportView( const ViewReport& view )
    {
        if ( m_State != EState::InSession )
            return;
        const double local = LocalSeconds();
        // The yaw's change is taken around the circle (359 -> 1 is two degrees); a circle has no axis, so its
        // yaw never counts as a change.
        const auto changed = [&]( const ViewReport& sent )
        {
            const bool  circle = view.SectorAngleDeg >= 360.0f && sent.SectorAngleDeg >= 360.0f;
            const float yaw    = std::abs( std::remainder( view.FacingYawDeg - sent.FacingYawDeg, 360.0f ) );
            return ( !circle && yaw >= kViewAngleStepDeg ) ||
                   std::abs( view.SectorAngleDeg - sent.SectorAngleDeg ) >= kViewAngleStepDeg ||
                   std::abs( view.RangeCm - sent.RangeCm ) >= kViewRangeStepCm;
        };
        if ( m_SentView )
        {
            const double since = local - m_ViewSentAt;
            if ( since < kViewMinIntervalSeconds || ( !changed( *m_SentView ) && since < kViewResendSeconds ) )
                return;
        }
        m_Transport.Send( m_Connection, EncodeViewReport( view ), ENetLane::State );
        m_SentView   = view;
        m_ViewSentAt = local;
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
