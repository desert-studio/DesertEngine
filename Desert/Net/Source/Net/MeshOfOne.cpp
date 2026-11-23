#include "MeshOfOne.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>

namespace Desert::Net
{
    // ---- store -------------------------------------------------------------------------------------------------

    NetId InProcessReplicationStore::AllocateNetId()
    {
        return m_Next++;
    }

    void InProcessReplicationStore::Write( const PawnState& state )
    {
        m_States[state.Id] = state;
    }

    void InProcessReplicationStore::Remove( NetId id )
    {
        m_States.erase( id );
    }

    std::uint32_t InProcessReplicationStore::WriteStable( StableNetId id, std::string_view component,
                                                          std::string_view field, const ReplicatedValue& value )
    {
        std::vector<FieldUpdate>& fields = m_Stable[id];
        const auto                it     = std::find_if( fields.begin(), fields.end(), [&]( const FieldUpdate& f )
                                                         { return f.Component == component && f.Field == field; } );
        if ( it != fields.end() )
        {
            if ( it->Value == value )
                return it->Version;
            it->Value   = value;
            it->Version = ++m_StableVersion;
            return it->Version;
        }
        fields.push_back( { std::string( component ), std::string( field ), ++m_StableVersion, value } );
        return fields.back().Version;
    }

    std::vector<PawnState> InProcessReplicationStore::All() const
    {
        std::vector<PawnState> all;
        all.reserve( m_States.size() );
        for ( const auto& [id, state] : m_States )
            all.push_back( state );
        return all;
    }

    // ---- simulation --------------------------------------------------------------------------------------------

    PawnSimulation::PawnSimulation( IReplicationStore& store, IGatewayLink& gateway, Settings settings,
                                    NetLogSink log )
         : m_Store( store ), m_Gateway( gateway ), m_Settings( std::move( settings ) ), m_Log( std::move( log ) )
    {
        if ( m_Settings.SpawnPoints.empty() )
            m_Settings.SpawnPoints.push_back( {} );
    }

    void PawnSimulation::PlayerJoined( PlayerId player, std::uint64_t account )
    {
        Pawn pawn;
        pawn.Account        = account;
        pawn.Index          = m_Joined++;
        pawn.State.Id       = m_Store.AllocateNetId();
        pawn.State.Position = m_Settings.SpawnPoints[pawn.Index % m_Settings.SpawnPoints.size()];
        m_Store.Write( pawn.State );
        m_Log( std::format( "player {} (account {}): pawn net id {} spawned at ({:.0f}, {:.0f}, {:.0f}) cm",
                            player, account, pawn.State.Id, pawn.State.Position.X, pawn.State.Position.Y,
                            pawn.State.Position.Z ) );
        const PawnState     spawned = pawn.State;
        const std::uint32_t index   = pawn.Index;
        m_Pawns.emplace( player, std::move( pawn ) );
        m_Gateway.PawnSpawned( player, index, spawned );
    }

    void PawnSimulation::PlayerLeft( PlayerId player )
    {
        const auto it = m_Pawns.find( player );
        if ( it == m_Pawns.end() )
            return;
        const NetId id = it->second.State.Id;
        m_Log( std::format( "player {}: left, pawn net id {} destroyed", player, id ) );
        m_Pawns.erase( it );
        m_Store.Remove( id );
        m_Gateway.PawnDestroyed( id );
    }

    void PawnSimulation::PlayerInput( PlayerId player, const InputMessage& input )
    {
        const auto it = m_Pawns.find( player );
        if ( it == m_Pawns.end() )
            return;
        Pawn& pawn = it->second;
        for ( const InputStep& step : input.Steps )
        {
            // Already processed (a redundant copy) or already queued: the first arrival wins.
            if ( pawn.LastProcessed && !StepAfter( step.Step, *pawn.LastProcessed ) )
                continue;
            pawn.Pending.emplace( step.Step, step );
        }
        // A client that floods steps far ahead is held to the replay window, not buffered without bound.
        while ( pawn.Pending.size() > InputRing::kCapacity )
            pawn.Pending.erase( std::prev( pawn.Pending.end() ) );
    }

    std::optional<std::uint32_t> PawnSimulation::LastProcessedInput( PlayerId player ) const
    {
        const auto it = m_Pawns.find( player );
        return it == m_Pawns.end() ? std::nullopt : it->second.LastProcessed;
    }

    void PawnSimulation::Step( double dtSeconds )
    {
        for ( auto& [player, pawn] : m_Pawns )
        {
            if ( !pawn.Pending.empty() )
            {
                const auto next    = pawn.Pending.begin();
                pawn.LastApplied   = next->second;
                pawn.LastProcessed = next->first;
                pawn.Pending.erase( next );
            }
            NetVec3     move   = pawn.LastApplied.Move;
            const float length = std::sqrt( move.X * move.X + move.Y * move.Y + move.Z * move.Z );
            if ( length > 1.f )
                move = { move.X / length, move.Y / length, move.Z / length };
            const float distance = m_Settings.WalkSpeed * static_cast<float>( dtSeconds );
            pawn.State.Position.X += move.X * distance;
            pawn.State.Position.Y += move.Y * distance;
            pawn.State.Position.Z += move.Z * distance;
            pawn.State.Yaw = pawn.LastApplied.Yaw;
            m_Store.Write( pawn.State );
        }
    }

    std::vector<PawnSimulation::Source> PawnSimulation::Sources() const
    {
        std::vector<Source> sources;
        for ( const auto& [player, pawn] : m_Pawns )
            sources.push_back( { player, pawn.Account, pawn.State.Position } );
        return sources;
    }

    // ---- gateway -----------------------------------------------------------------------------------------------

    Gateway::Gateway( INetTransport& transport, IReplicationStore& store, HelloPolicy policy,
                      std::uint64_t pawnPrefab, NetLogSink log )
         : m_Transport( transport ), m_Store( store ), m_Policy( policy ), m_PawnPrefab( pawnPrefab ),
           m_Log( std::move( log ) )
    {
    }

    void Gateway::OnConnected( ConnectionId connection, std::string_view address )
    {
        m_Peers[connection] = {};
        m_Log( std::format( "connection {} from {}: awaiting its Hello", connection, address ) );
    }

    void Gateway::OnClosed( ConnectionId connection, const DisconnectInfo& info )
    {
        const auto it = m_Peers.find( connection );
        if ( it == m_Peers.end() )
            return;
        m_Log( std::format( "connection {} closed ({}{})", connection, info.Code,
                            info.Reason.empty() ? std::string() : ": " + info.Reason ) );
        if ( it->second.Admitted )
        {
            m_Connections.erase( it->second.Player );
            if ( m_Sim != nullptr )
                m_Sim->PlayerLeft( it->second.Player );
        }
        m_Peers.erase( it );
    }

    void Gateway::Admit( ConnectionId connection, Peer& peer, std::span<const std::byte> hello )
    {
        HelloPolicy policy         = m_Policy;
        policy.NowUnixSeconds      = UnixSecondsNow();
        const HelloVerdict verdict = EvaluateHello( hello, policy );
        if ( !verdict.Accepted )
        {
            // UE PreLogin's ErrorMessage: the reason is logged here and sent to the client, which shows it.
            m_Log( std::format(
                 "connection {}: refused - {} (client protocol {}, cook hash {:016x}, token {} bytes, "
                 "account {}, server id {}, expiry {}; server protocol {}, cook hash {:016x}, id {}, "
                 "now {})",
                 connection, RefuseReasonText( verdict.Reason ), verdict.Hello.ProtocolVersion,
                 verdict.Hello.CookHash, verdict.Hello.Token.size(), verdict.Token.Account, verdict.Token.Server,
                 verdict.Token.ExpiresUnixSeconds, kProtocolVersion, policy.CookHash, policy.ServerId,
                 policy.NowUnixSeconds ) );
            m_Transport.Send( connection, EncodeRefuse( verdict.Reason ), ENetLane::Control );
            m_Transport.Close( connection, RefuseCloseCode( verdict.Reason ), RefuseReasonText( verdict.Reason ),
                               true );
            m_Peers.erase( connection );
            return;
        }
        peer.Admitted              = true;
        peer.Player                = m_NextPlayer++;
        m_Connections[peer.Player] = connection;
        m_Transport.Send( connection, EncodeWelcome( m_Policy.CookHash, m_PawnPrefab ), ENetLane::Control );
        m_Log( std::format( "connection {}: admitted as player {} (account {}), pawn prefab {:016x}", connection,
                            peer.Player, verdict.Token.Account, m_PawnPrefab ) );
        // UE's initial replication of the actors already relevant: the pawns of the players already in.
        for ( const PawnState& pawn : m_Store.All() )
        {
            m_Transport.Send( connection, EncodeSpawn( { pawn, false, 0 } ), ENetLane::Rpc );
            ++m_Stats.Spawns;
        }
        // Stable objects are NOT spawned: the new connection's baselines are empty, so the next state send carries
        // every changed field of every stable object in full (§3.4).
        if ( m_Sim != nullptr )
            m_Sim->PlayerJoined( peer.Player, verdict.Token.Account );
    }

    void Gateway::OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data )
    {
        const auto it = m_Peers.find( connection );
        if ( it == m_Peers.end() )
            return;
        Peer& peer = it->second;
        if ( !peer.Admitted )
        {
            if ( lane == ENetLane::Control )
                Admit( connection, peer, data );
            return;
        }
        const std::optional<EReplicationMessage> type = PeekReplicationMessage( data );
        if ( type == EReplicationMessage::StateAck )
        {
            if ( const std::optional<StateAckMessage> ack = DecodeStateAck( data ) )
                OnStateAck( peer, *ack, m_ServerStep );
            else
                m_Log( std::format( "connection {}: malformed StateAck dropped", connection ) );
            return;
        }
        if ( type != EReplicationMessage::Input )
            return;
        const std::optional<InputMessage> input = DecodeInput( data );
        if ( !input )
        {
            m_Log( std::format( "connection {}: malformed Input dropped", connection ) );
            return;
        }
        if ( m_Sim != nullptr )
            m_Sim->PlayerInput( peer.Player, *input );
    }

    void Gateway::PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn )
    {
        for ( const auto& [id, connection] : m_Connections )
        {
            m_Transport.Send( connection, EncodeSpawn( { pawn, id == owner, player } ), ENetLane::Rpc );
            ++m_Stats.Spawns;
        }
    }

    void Gateway::PawnDestroyed( NetId id )
    {
        for ( const auto& [player, connection] : m_Connections )
            m_Transport.Send( connection, EncodeDestroy( id ), ENetLane::Rpc );
    }

    void Gateway::SendSnapshots( std::uint32_t serverStep )
    {
        SnapshotMessage snapshot;
        snapshot.ServerStep = serverStep;
        snapshot.Pawns      = m_Store.All();
        for ( const auto& [player, connection] : m_Connections )
        {
            const std::optional<std::uint32_t> ack =
                 m_Sim != nullptr ? m_Sim->LastProcessedInput( player ) : std::nullopt;
            snapshot.HasAck       = ack.has_value();
            snapshot.AckInputStep = ack.value_or( 0 );
            // State lane: unreliable; a full send queue drops this snapshot, the next one supersedes it.
            m_Transport.Send( connection, EncodeSnapshot( snapshot ), ENetLane::State );
        }
        m_ServerStep = serverStep;
        for ( auto& [connection, peer] : m_Peers )
            if ( peer.Admitted )
            {
                ExpireLost( peer, 0, serverStep, false );
                SendStableState( connection, peer, serverStep );
            }
    }

    namespace
    {
        // A packet neither acked nor reported lost after this long is lost (a client that stopped acking).
        constexpr std::uint32_t kStateTimeoutSteps = 60;
        // Objects per ObjectState packet: well under the transport's MTU for a few fields each.
        constexpr std::size_t kObjectsPerStatePacket = 24;
    } // namespace

    void Gateway::SendStableState( ConnectionId connection, Peer& peer, std::uint32_t serverStep )
    {
        ObjectStateMessage message;
        SentPacket         sent;
        sent.Step        = serverStep;
        const auto flush = [&]
        {
            if ( message.Objects.empty() )
                return;
            message.Sequence = peer.NextStateSequence++;
            m_Transport.Send( connection, EncodeObjectState( message ), ENetLane::State );
            ++m_Stats.StablePackets;
            peer.InFlight[message.Sequence] = std::move( sent );
            message.Objects.clear();
            sent      = {};
            sent.Step = serverStep;
        };
        for ( const auto& [id, fields] : m_Store.StableObjects() )
        {
            ObjectBaseline& baseline = peer.Baselines[id];
            baseline.Acked.resize( fields.size(), 0 );
            baseline.InFlight.resize( fields.size(), 0 );
            const bool   fresh = std::all_of( baseline.Acked.begin(), baseline.Acked.end(),
                                              []( std::uint32_t v ) { return v == 0; } );
            ObjectUpdate update{ id, {} };
            for ( std::size_t i = 0; i < fields.size(); ++i )
            {
                const FieldUpdate& field = fields[i];
                if ( field.Version <= baseline.Acked[i] || baseline.InFlight[i] == field.Version )
                    continue;
                update.Fields.push_back( field );
                baseline.InFlight[i] = field.Version;
                sent.Fields.push_back( { id, i, field.Version } );
                m_Log( std::format( "player {}: stable {} sent as STATE{} - {}.{} v{} = {}", peer.Player, id,
                                    fresh ? " (full baseline)" : "", field.Component, field.Field, field.Version,
                                    ReplicatedValueText( field.Value ) ) );
            }
            if ( update.Fields.empty() )
                continue;
            m_Stats.StableFields += update.Fields.size();
            message.Objects.push_back( std::move( update ) );
            if ( message.Objects.size() >= kObjectsPerStatePacket )
                flush();
        }
        flush();
    }

    void Gateway::OnStateAck( Peer& peer, const StateAckMessage& ack, std::uint32_t serverStep )
    {
        for ( auto it = peer.InFlight.begin(); it != peer.InFlight.end(); )
        {
            if ( !StateAckWindow::Acks( ack, it->first ) )
            {
                ++it;
                continue;
            }
            for ( const SentField& field : it->second.Fields )
            {
                ObjectBaseline& baseline = peer.Baselines[field.Id];
                if ( field.Index < baseline.Acked.size() )
                {
                    baseline.Acked[field.Index] = std::max( baseline.Acked[field.Index], field.Version );
                    if ( baseline.InFlight[field.Index] == field.Version )
                        baseline.InFlight[field.Index] = 0;
                }
                ++m_Stats.Acked;
            }
            it = peer.InFlight.erase( it );
        }
        ExpireLost( peer, ack.Newest, serverStep, true );
    }

    // A packet is lost when an ack of a newer one does not ack it, or when nothing acked it for
    // kStateTimeoutSteps: its in-flight marks clear, so the next send carries exactly its fields again if they are
    // still newer than the acked baseline (Iris; never the whole object).
    void Gateway::ExpireLost( Peer& peer, std::uint32_t newestAcked, std::uint32_t serverStep, bool byAck )
    {
        for ( auto it = peer.InFlight.begin(); it != peer.InFlight.end(); )
        {
            // Acks run in sequence order (Iris packet notification): an unacked packet older than the newest acked
            // one was lost - the ack window repeats every ack 32 times, so it is not a lost ack.
            const bool skipped  = byAck && StepAfter( newestAcked, it->first );
            const bool timedOut = serverStep - it->second.Step > kStateTimeoutSteps;
            if ( !skipped && !timedOut )
            {
                ++it;
                continue;
            }
            for ( const SentField& field : it->second.Fields )
            {
                ObjectBaseline& baseline = peer.Baselines[field.Id];
                if ( field.Index < baseline.InFlight.size() && baseline.InFlight[field.Index] == field.Version )
                {
                    baseline.InFlight[field.Index] = 0;
                    ++m_Stats.Resent;
                }
            }
            it = peer.InFlight.erase( it );
        }
    }

    std::size_t Gateway::Players() const
    {
        return m_Connections.size();
    }
} // namespace Desert::Net
