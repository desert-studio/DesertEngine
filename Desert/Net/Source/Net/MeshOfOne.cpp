#include "MeshOfOne.hpp"

#include <algorithm>
#include <cmath>
#include <format>

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

    Gateway::Gateway( INetTransport& transport, IReplicationStore& store, HelloPolicy policy, NetLogSink log )
         : m_Transport( transport ), m_Store( store ), m_Policy( policy ), m_Log( std::move( log ) )
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
            m_Log( std::format( "connection {}: refused - {}", connection, RefuseReasonText( verdict.Reason ) ) );
            m_Transport.Send( connection, EncodeRefuse( verdict.Reason ), ENetLane::Control );
            m_Transport.Close( connection, RefuseCloseCode( verdict.Reason ), RefuseReasonText( verdict.Reason ),
                               true );
            m_Peers.erase( connection );
            return;
        }
        peer.Admitted              = true;
        peer.Player                = m_NextPlayer++;
        m_Connections[peer.Player] = connection;
        m_Transport.Send( connection, EncodeWelcome( m_Policy.CookHash ), ENetLane::Control );
        m_Log( std::format( "connection {}: admitted as player {} (account {})", connection, peer.Player,
                            verdict.Token.Account ) );
        // UE's initial replication of the actors already relevant: the pawns of the players already in.
        for ( const PawnState& pawn : m_Store.All() )
            m_Transport.Send( connection, EncodeSpawn( { pawn, false, 0 } ), ENetLane::Rpc );
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
        if ( PeekReplicationMessage( data ) != EReplicationMessage::Input )
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
            m_Transport.Send( connection, EncodeSpawn( { pawn, id == owner, player } ), ENetLane::Rpc );
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
    }

    std::size_t Gateway::Players() const
    {
        return m_Connections.size();
    }
} // namespace Desert::Net
