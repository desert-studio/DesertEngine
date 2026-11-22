#pragma once

// THE MESH OF ONE (Docs/contracts/NET-ARCH.md §2M.2): the gateway, simulation server and replication store roles
// of DesertServer, run as in-process modules behind the interfaces NET-MESH-1 splits into processes without a
// rewrite:
//   IReplicationStore  latest state of every replicated entity (the store role; partitioned by cell from MESH-1)
//   ISimLink           gateway -> sim: a player joined / left, its input (routed by the authority map later)
//   IGatewayLink       sim -> gateway: a pawn spawned / was destroyed (the gateway tells the relevant clients)
// The gateway owns the client connections and their handshake, sends Spawn/Destroy on the Rpc lane and a
// Snapshot per connection on the State lane every kSnapshotEverySteps steps, read from the store - it never asks
// the sim for state, so moving the store out of process changes only the link.
// UE: one APlayerController per connection, the pawn spawned by the server for it (GameMode::RestartPlayer) and
// replicated to every relevant connection, autonomous proxy for its owner, simulated proxy for the others.

#include <Net/NetTransport.hpp>
#include <Net/Replication.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <string_view>

namespace Desert::Net
{
    using PlayerId = std::uint32_t;

    class IReplicationStore
    {
    public:
        virtual ~IReplicationStore()                                 = default;
        virtual NetId                                AllocateNetId() = 0; // NET-MESH: a leased block (§2M.6)
        virtual void                                 Write( const PawnState& s ) = 0;
        virtual void                                 Remove( NetId id )          = 0;
        [[nodiscard]] virtual std::vector<PawnState> All() const                 = 0; // by net id
    };

    class IGatewayLink
    {
    public:
        virtual ~IGatewayLink()                                                                 = default;
        virtual void PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn ) = 0;
        virtual void PawnDestroyed( NetId id )                                                  = 0;
    };

    class ISimLink
    {
    public:
        virtual ~ISimLink()                                                    = default;
        virtual void PlayerJoined( PlayerId player, std::uint64_t account )    = 0;
        virtual void PlayerLeft( PlayerId player )                             = 0;
        virtual void PlayerInput( PlayerId player, const InputMessage& input ) = 0;
        // The last of @p player's input steps the sim has processed (the ack of §5.1), nullopt before the first.
        [[nodiscard]] virtual std::optional<std::uint32_t> LastProcessedInput( PlayerId player ) const = 0;
    };

    class InProcessReplicationStore final : public IReplicationStore
    {
    public:
        NetId                  AllocateNetId() override;
        void                   Write( const PawnState& state ) override;
        void                   Remove( NetId id ) override;
        std::vector<PawnState> All() const override;

    private:
        NetId                      m_Next = 1;
        std::map<NetId, PawnState> m_States;
    };

    // THE SIMULATION ROLE for player pawns: one pawn per joined player at the next spawn point, moved by its
    // player's input at the fixed step - server authority, no client prediction yet (NET-4). Inputs are applied
    // in client step order; a step whose input has not arrived repeats the last applied input (UE
    // CharacterMovement's "no new move" path), so the pawn does not stutter on one lost packet.
    class PawnSimulation final : public ISimLink
    {
    public:
        struct Settings
        {
            std::vector<NetVec3> SpawnPoints;       // world cm; the n-th player takes point n % size
            float                WalkSpeed = 600.f; // cm/s (UE MaxWalkSpeed default)
        };
        PawnSimulation( IReplicationStore& store, IGatewayLink& gateway, Settings settings, NetLogSink log );

        void PlayerJoined( PlayerId player, std::uint64_t account ) override;
        void PlayerLeft( PlayerId player ) override;
        void PlayerInput( PlayerId player, const InputMessage& input ) override;
        [[nodiscard]] std::optional<std::uint32_t> LastProcessedInput( PlayerId player ) const override;

        // One fixed step of every pawn; writes the moved states to the store.
        void Step( double dtSeconds );
        // The pawns' positions: the server's streaming sources (NET-ARCH §4.1), with the player they belong to.
        struct Source
        {
            PlayerId      Player  = 0;
            std::uint64_t Account = 0;
            NetVec3       Position;
        };
        [[nodiscard]] std::vector<Source> Sources() const;

    private:
        struct Pawn
        {
            std::uint64_t                      Account = 0;
            std::uint32_t                      Index   = 0;
            PawnState                          State;
            std::map<std::uint32_t, InputStep> Pending; // by client step (wrap: a session is < 2^31 steps)
            std::optional<std::uint32_t>       LastProcessed;
            InputStep                          LastApplied;
        };
        IReplicationStore&       m_Store;
        IGatewayLink&            m_Gateway;
        Settings                 m_Settings;
        NetLogSink               m_Log;
        std::map<PlayerId, Pawn> m_Pawns;
        std::uint32_t            m_Joined = 0;
    };

    // THE GATEWAY ROLE: client connections, the connect handshake (token + cook hash), routing input to the sim,
    // Spawn/Destroy and Snapshots out of the store. Relevancy is "every pawn" until the WP-grid relevancy of §3.5.
    class Gateway final : public INetTransportEvents, public IGatewayLink
    {
    public:
        Gateway( INetTransport& transport, IReplicationStore& store, HelloPolicy policy, NetLogSink log );
        void Bind( ISimLink& sim )
        {
            m_Sim = &sim;
        }

        void OnConnected( ConnectionId connection, std::string_view address ) override;
        void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override;
        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data ) override;

        void PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn ) override;
        void PawnDestroyed( NetId id ) override;

        // Sends this step's snapshot to every admitted connection (call every kSnapshotEverySteps steps).
        void                      SendSnapshots( std::uint32_t serverStep );
        [[nodiscard]] std::size_t Players() const;

    private:
        struct Peer
        {
            bool     Admitted = false;
            PlayerId Player   = 0;
        };
        void Admit( ConnectionId connection, Peer& peer, std::span<const std::byte> hello );

        INetTransport&                   m_Transport;
        IReplicationStore&               m_Store;
        HelloPolicy                      m_Policy;
        NetLogSink                       m_Log;
        ISimLink*                        m_Sim = nullptr;
        std::map<ConnectionId, Peer>     m_Peers;
        std::map<PlayerId, ConnectionId> m_Connections;
        PlayerId                         m_NextPlayer = 1;
    };
} // namespace Desert::Net
