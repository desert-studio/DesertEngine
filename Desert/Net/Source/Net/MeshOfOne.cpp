#include "MeshOfOne.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

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

    bool InProcessReplicationStore::DestroyStable( StableNetId id )
    {
        m_Stable.erase( id );
        return m_Destroyed.insert( id ).second;
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

    PawnSimulation::PawnSimulation( IReplicationStore& store, IGatewayLink& gateway, IPawnMover& mover,
                                    Settings settings, NetLogSink log )
         : m_Store( store ), m_Gateway( gateway ), m_Mover( mover ), m_Settings( std::move( settings ) ),
           m_Log( std::move( log ) )
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
        // A spawned character starts falling and finds the floor under it on its first move (UE: the first
        // PerformMovement's FindFloor; a spawn point stands above the ground). The replicated mode says so too:
        // the owning client starts its prediction from this snapshot, and a server that began grounded took a
        // ground step where the client took an air step (NET-4 run 11: 8.5 cm correction at input step 15,
        // server vx +22 = one ground-acceleration step, vy +16 = one gravity step).
        pawn.Move.Position  = pawn.State.Position;
        pawn.Move.Grounded  = false;
        pawn.State.Movement = 0;
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
        if ( !input.Steps.empty() &&
             ( !pawn.ResendFloor || StepAfter( input.Steps.front().Step, *pawn.ResendFloor ) ) )
            pawn.ResendFloor = input.Steps.front().Step;
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

    std::optional<ISimLink::AckedMove> PawnSimulation::LastProcessedInput( PlayerId player ) const
    {
        const auto it = m_Pawns.find( player );
        if ( it == m_Pawns.end() || !it->second.LastProcessed )
            return std::nullopt;
        return AckedMove{ *it->second.LastProcessed, it->second.Move, it->second.QueuedAtStep };
    }

    // The server moves a pawn only by its player's input steps, every one, in step order (UE ServerMove: the
    // server simulates the client's moves, never moves of its own) - so its state after step N is the client's
    // prediction of N. A step that has not arrived holds the pawn, the steps after it wait: every Input message
    // carries EVERY unacked step (Replication.hpp kMaxUnackedInputSteps), so a lost packet's steps come with the
    // next one. Moving on past a hole would leave the server a step's move short of the client - the periodic
    // sprint corrections of NET-4 run 9 (33-183 cm, whole steps of 16.7 cm). Only a step the client no longer
    // sends is given up: a message that starts past it (the client's ring overflowed - never a guess from how
    // far the queue runs ahead, which a slow client frame's burst of steps exceeds while the hole is still
    // being resent). A starved pawn holds; a queue above the sync's target drains two per step.
    void PawnSimulation::StepPawn( Pawn& pawn ) const
    {
        constexpr std::size_t kDrainAbove = 4;
        pawn.QueuedAtStep = static_cast<std::uint8_t>( std::min<std::size_t>( pawn.Pending.size(), 255 ) );
        const int consume = pawn.Pending.size() > kDrainAbove ? 2 : 1;
        for ( int i = 0; i < consume && !pawn.Pending.empty(); ++i )
        {
            const auto next = pawn.Pending.begin();
            if ( pawn.LastProcessed && next->first != *pawn.LastProcessed + 1 )
            {
                const std::uint32_t missing = *pawn.LastProcessed + 1;
                if ( !pawn.ResendFloor || !StepAfter( *pawn.ResendFloor, missing ) )
                    break; // still in flight: every Input message repeats it until the ack
                m_Log( std::format(
                     "pawn net id {}: input steps {}..{} never arrived and the client no longer resends "
                     "them (its unacked ring of {} steps overflowed); moving on (the client is "
                     "corrected)",
                     pawn.State.Id, missing, next->first - 1, kMaxUnackedInputSteps ) );
            }
            // A move is one CLIENT step long, whatever the server's frame took (UE FSavedMove's DeltaTime).
            // The step's action set is the game's to read (IPawnMover); the log names it in the game's words when
            // it changes, and once a second with the speed it gives (cm/s, planar).
            const InputStep& input = next->second;
            pawn.Move              = m_Mover.Step( pawn.Move, input, kNetStepSeconds );
            const float speed      = std::sqrt( pawn.Move.Velocity.X * pawn.Move.Velocity.X +
                                                pawn.Move.Velocity.Z * pawn.Move.Velocity.Z );
            if ( !pawn.LastProcessed || input.Actions != pawn.LastActions || next->first % static_cast<std::uint32_t>( kNetStepHz ) == 0 )
                m_Log( std::format( "pawn net id {}: input step {} actions {} (bits 0x{:x}, axes {}), speed {:.0f} cm/s",
                                    pawn.State.Id, next->first, m_Mover.DescribeActions( input ), input.Actions,
                                    input.Axes.size(), speed ) );
            pawn.LastActions   = input.Actions;
            pawn.LastProcessed = next->first;
            pawn.Pending.erase( next );
        }
        pawn.State.Position = pawn.Move.Position;
        pawn.State.Yaw      = pawn.Move.Yaw;
        pawn.State.Velocity = pawn.Move.Velocity;
        pawn.State.Movement = MoveFlagsOf( pawn.Move ); // Grounded and the game's mode (a proxy's crouch)
    }

    void PawnSimulation::Step()
    {
        for ( auto& [player, pawn] : m_Pawns )
        {
            StepPawn( pawn );
            m_Store.Write( pawn.State );
        }
    }

    std::optional<PawnSimulation::HandoverPawn> PawnSimulation::Release( PlayerId player )
    {
        const auto it = m_Pawns.find( player );
        if ( it == m_Pawns.end() )
            return std::nullopt;
        HandoverPawn out;
        out.Player        = player;
        out.Account       = it->second.Account;
        out.Index         = it->second.Index;
        out.State         = it->second.State;
        out.LastProcessed = it->second.LastProcessed;
        out.Move          = it->second.Move;
        for ( const auto& [step, input] : it->second.Pending )
            out.Pending.push_back( input );
        m_Pawns.erase( it );
        return out;
    }

    void PawnSimulation::Adopt( HandoverPawn handed, std::uint32_t catchUpSteps )
    {
        Pawn pawn;
        pawn.Account       = handed.Account;
        pawn.Index         = handed.Index;
        pawn.State         = handed.State;
        pawn.LastProcessed = handed.LastProcessed;
        pawn.Move          = handed.Move;
        for ( const InputStep& input : handed.Pending )
            pawn.Pending.emplace( input.Step, input );
        for ( std::uint32_t i = 0; i < catchUpSteps; ++i )
            StepPawn( pawn );
        m_Store.Write( pawn.State );
        m_Pawns.insert_or_assign( handed.Player, std::move( pawn ) );
    }

    bool PawnSimulation::Teleport( PlayerId player, const NetVec3& position )
    {
        const auto it = m_Pawns.find( player );
        if ( it == m_Pawns.end() )
            return false;
        it->second.State.Position = position;
        ++it->second.State.TeleportCount;
        // UE TeleportTo on a character: it is at the new place, still, and falls to the floor there (the next
        // move's FindFloor) - a teleport onto the landscape lands on the terrain, not on a plane at its height.
        it->second.Move.Position = position;
        it->second.Move.Velocity = {};
        it->second.Move.Grounded = false;
        it->second.State.Movement = 0; // the replicated mode is the move's: falling
        m_Store.Write( it->second.State );
        m_Log( std::format( "player {}: pawn {} teleported to ({:.0f}, {:.0f}, {:.0f}) cm", player,
                            it->second.State.Id, position.X, position.Y, position.Z ) );
        return true;
    }

    std::vector<PawnSimulation::Source> PawnSimulation::Sources() const
    {
        std::vector<Source> sources;
        for ( const auto& [player, pawn] : m_Pawns )
            sources.push_back( { player, pawn.Account, pawn.State.Id, pawn.State.Position } );
        return sources;
    }

    // ---- gateway -----------------------------------------------------------------------------------------------

    Gateway::Gateway( INetTransport& transport, IReplicationStore& store, GatewayAdmission admission,
                      std::uint64_t pawnPrefab, NetRelevancyGrid grid, NetLogSink log )
         : m_Transport( transport ), m_Store( store ), m_Admission( std::move( admission ) ),
           m_PawnPrefab( pawnPrefab ), m_Grid( std::move( grid ) ), m_Log( std::move( log ) )
    {
    }

    void Gateway::OnConnected( ConnectionId connection, std::string_view address )
    {
        m_Peers[connection]         = {};
        m_Peers[connection].Address = std::string( address );
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
        const HelloPolicy policy{ m_Admission.CookHash, UnixSecondsNow() };
        HelloVerdict      verdict = EvaluateHello( hello, policy, m_Admission.Tokens );
        // Net's checks passed: the game decides on the claims (UE AGameModeBase::PreLogin).
        PreLoginResult login;
        if ( verdict.Accepted )
        {
            login = m_Admission.Game.PreLogin(
                 { verdict.Token.Claims, verdict.Token.Issuer, verdict.Hello.JoinOptions, peer.Address } );
            if ( !login.Refusal.empty() )
            {
                verdict.Accepted = false;
                verdict.Reason   = ERefuseReason::PreLogin;
            }
        }
        if ( !verdict.Accepted )
        {
            // UE PreLogin's ErrorMessage: the reason is logged here and sent to the client, which shows it.
            const std::string text = verdict.Reason == ERefuseReason::PreLogin
                                          ? login.Refusal
                                          : std::string( RefuseReasonText( verdict.Reason ) );
            m_Log( std::format(
                 "connection {}: refused - {} (client protocol {}, cook hash {:016x}, token {} bytes, issuer {} "
                 "key {}, audience {}, expiry {}, claims {} bytes; server protocol {}, cook hash {:016x}, "
                 "audience {}, now {})",
                 connection, text, verdict.Hello.ProtocolVersion, verdict.Hello.CookHash,
                 verdict.Hello.Token.size(), verdict.Token.Issuer, verdict.Token.KeyId, verdict.Token.Audience,
                 verdict.Token.ExpiresUnixSeconds, verdict.Token.Claims.size(), kProtocolVersion, policy.CookHash,
                 m_Admission.Tokens.Audience(), policy.NowUnixSeconds ) );
            m_Transport.Send( connection, EncodeRefuse( verdict.Reason, text ), ENetLane::Control );
            m_Transport.Close( connection, RefuseCloseCode( verdict.Reason ), text, true );
            m_Peers.erase( connection );
            return;
        }
        peer.Admitted              = true;
        peer.Player                = m_NextPlayer++;
        m_Connections[peer.Player] = connection;
        m_Transport.Send( connection, EncodeWelcome( m_Admission.CookHash, m_PawnPrefab ), ENetLane::Control );
        m_Log( std::format( "connection {}: admitted as player {} (account {}, issuer {}), pawn prefab {:016x}",
                            connection, peer.Player, login.Account, verdict.Token.Issuer, m_PawnPrefab ) );
        // Nothing else is relevant yet: the other pawns and the cells' stable objects follow the client's first
        // residency report (UE: actors replicate once the client has made their level visible).
        // Stable objects are NOT spawned: the new connection's baselines are empty, so the next state send carries
        // every changed field of every stable object in full (§3.4).
        if ( m_Sim != nullptr )
            m_Sim->PlayerJoined( peer.Player, login.Account );
    }

    void Gateway::OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                             NetClock::time_point )
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
        if ( type == EReplicationMessage::CellResidency )
        {
            if ( const std::optional<CellResidencyMessage> report = DecodeCellResidency( data ) )
                OnCellResidency( connection, peer, *report );
            else
                m_Log( std::format( "connection {}: malformed CellResidency dropped", connection ) );
            return;
        }
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

    // The own pawn is relevant to its owner from the start (§3.5 rule 1); every other connection gets it when it
    // stands in a cell that connection has resident (UpdateRelevancy).
    void Gateway::PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn )
    {
        m_PawnInfo[pawn.Id] = { owner, player };
        const auto connection = m_Connections.find( owner );
        if ( connection == m_Connections.end() )
            return;
        m_Transport.Send( connection->second, EncodeSpawn( { pawn, true, player } ), ENetLane::Rpc );
        ++m_Stats.Spawns;
        m_Peers[connection->second].Relevant.insert( pawn.Id );
    }

    void Gateway::PawnDestroyed( NetId id )
    {
        m_PawnInfo.erase( id );
        for ( auto& [connection, peer] : m_Peers )
            if ( peer.Relevant.erase( id ) > 0 )
                m_Transport.Send( connection, EncodeDestroy( { id, EDestroyReason::Destroyed } ), ENetLane::Rpc );
    }

    bool Gateway::StableRelevant( const Peer& peer, StableNetId id ) const
    {
        const auto unit = m_Grid.UnitOfRecord.find( id );
        return unit == m_Grid.UnitOfRecord.end() || peer.Resident.contains( unit->second ); // always-loaded: always
    }

    void Gateway::OnCellResidency( ConnectionId connection, Peer& peer, const CellResidencyMessage& message )
    {
        const auto nameOf = [&]( std::uint32_t unit )
        {
            const auto cell = m_Grid.Cells.find( unit );
            return cell != m_Grid.Cells.end() ? cell->second.Name : std::format( "unit {} (not a cell)", unit );
        };
        for ( const std::uint32_t unit : message.Unloaded )
        {
            if ( peer.Resident.erase( unit ) == 0 )
                continue;
            // The client's entities of the cell went with it (§4.4): its baselines and replayed deltas are void,
            // so the cell's return gets the full changed state and the destroyed list again (§4.3).
            std::size_t dropped = 0;
            if ( const auto cell = m_Grid.Cells.find( unit ); cell != m_Grid.Cells.end() )
                for ( const StableNetId id : cell->second.Records )
                {
                    dropped += peer.Baselines.erase( id );
                    peer.DeltaSent.erase( id );
                }
            m_Log( std::format( "player {} (connection {}): cell {} unloaded on the client - its stable objects "
                                "leave the relevant set ({} baseline(s) dropped)",
                                peer.Player, connection, nameOf( unit ), dropped ) );
        }
        for ( const std::uint32_t unit : message.Loaded )
            peer.AwaitingAck.insert( unit ); // even one already resident: the client waits for every report
        for ( const std::uint32_t unit : message.Loaded )
            if ( peer.Resident.insert( unit ).second )
                m_Log( std::format( "player {}: cell {} resident on the client", peer.Player, nameOf( unit ) ) );
    }

    void Gateway::UpdateRelevancy( ConnectionId connection, Peer& peer, const std::vector<PawnState>& pawns )
    {
        // The viewer is the client's own pawn (its streaming source): without one only its own pawn is.
        std::optional<NetVec3> viewer;
        for ( const PawnState& pawn : pawns )
            if ( const auto info = m_PawnInfo.find( pawn.Id );
                 info != m_PawnInfo.end() && info->second.Owner == peer.Player )
                viewer = pawn.Position;
        std::set<NetId> now;
        for ( const PawnState& pawn : pawns )
        {
            const auto info = m_PawnInfo.find( pawn.Id );
            if ( info == m_PawnInfo.end() )
                continue; // a state with no spawn yet
            const bool                 own = info->second.Owner == peer.Player;
            std::optional<std::string> cell;
            if ( !own && viewer && m_Grid.PawnCellInRange )
                cell = m_Grid.PawnCellInRange( *viewer, pawn.Position );
            if ( !own && !cell )
                continue;
            now.insert( pawn.Id );
            if ( peer.Relevant.contains( pawn.Id ) )
                continue;
            m_Transport.Send( connection, EncodeSpawn( { pawn, own, info->second.Player } ), ENetLane::Rpc );
            ++m_Stats.Spawns;
            m_Log( std::format( "player {}: pawn {} (player {}) relevant - its finest cell {} is in the client's "
                                "loading range",
                                peer.Player, pawn.Id, info->second.Player, cell ? *cell : std::string( "own" ) ) );
        }
        for ( const NetId id : peer.Relevant )
        {
            if ( now.contains( id ) )
                continue;
            m_Transport.Send( connection, EncodeDestroy( { id, EDestroyReason::NotRelevant } ), ENetLane::Rpc );
            ++m_Stats.Irrelevant;
            const auto info = m_PawnInfo.find( id );
            m_Log( std::format( "player {}: pawn {} (player {}) removed from relevancy - its finest cell is out "
                                "of the client's loading range",
                                peer.Player, id, info != m_PawnInfo.end() ? info->second.Player : 0 ) );
        }
        peer.Relevant = std::move( now );
    }

    // THE WORLD DELTA REPLAY (§3.3): the destroyed stable ids of every cell the client has resident, once per
    // residency (an unload clears them from DeltaSent, so the next load replays them again).
    void Gateway::ReplayWorldDelta( ConnectionId connection, Peer& peer )
    {
        std::map<std::uint32_t, StableDestroyMessage> byUnit;
        for ( const StableNetId id : m_Store.DestroyedStable() )
        {
            if ( peer.DeltaSent.contains( id ) || !StableRelevant( peer, id ) )
                continue;
            const auto          unit = m_Grid.UnitOfRecord.find( id );
            const std::uint32_t key  = unit != m_Grid.UnitOfRecord.end() ? unit->second : 0xFFFFFFFFu;
            byUnit[key].Unit         = key;
            byUnit[key].Ids.push_back( id );
            peer.DeltaSent.insert( id );
        }
        // Every reported cell is acknowledged (UE ClientAckUpdateLevelVisibility), carrying its delta: the client
        // reveals the cell only then, so nothing the server destroyed is ever seen. An empty list is the bare ack.
        for ( const std::uint32_t unit : std::exchange( peer.AwaitingAck, {} ) )
            byUnit[unit].Unit = unit;
        for ( const auto& [unit, message] : byUnit )
        {
            m_Transport.Send( connection, EncodeStableDestroy( message ), ENetLane::Rpc );
            if ( message.Ids.empty() )
                continue;
            m_Stats.DeltaReplays += message.Ids.size();
            const auto  cell = m_Grid.Cells.find( unit );
            std::string ids;
            for ( const StableNetId id : message.Ids )
                ids += std::format( "{}{}", ids.empty() ? "" : ", ", id );
            m_Log( std::format( "player {}: world delta replayed for cell {} - stable {} destroyed", peer.Player,
                                cell != m_Grid.Cells.end() ? cell->second.Name : std::string( "always-loaded" ),
                                ids ) );
        }
    }

    void Gateway::SendSnapshots( std::uint32_t serverStep )
    {
        const std::vector<PawnState> pawns = m_Store.All();
        for ( const auto& [player, connection] : m_Connections )
        {
            Peer& peer = m_Peers[connection];
            UpdateRelevancy( connection, peer, pawns );
            SnapshotMessage snapshot;
            snapshot.ServerStep = serverStep;
            for ( const PawnState& pawn : pawns )
                if ( peer.Relevant.contains( pawn.Id ) )
                    snapshot.Pawns.push_back( pawn );
            const std::optional<ISimLink::AckedMove> ack =
                 m_Sim != nullptr ? m_Sim->LastProcessedInput( player ) : std::nullopt;
            snapshot.HasAck = ack.has_value();
            if ( ack )
            {
                snapshot.AckInputStep  = ack->Step;
                snapshot.OwnState      = ack->State;
                snapshot.InputBuffered = ack->Buffered;
            }
            // State lane: unreliable; a full send queue drops this snapshot, the next one supersedes it.
            m_Transport.Send( connection, EncodeSnapshot( snapshot ), ENetLane::State );
        }
        m_ServerStep = serverStep;
        for ( auto& [connection, peer] : m_Peers )
            if ( peer.Admitted )
            {
                ExpireLost( peer, 0, serverStep, false );
                ReplayWorldDelta( connection, peer );
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
            if ( !StableRelevant( peer, id ) )
                continue; // its cell is not resident on the client: it has no entity to apply it to
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
