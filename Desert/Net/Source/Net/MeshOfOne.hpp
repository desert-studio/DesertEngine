#pragma once

// THE MESH OF ONE (Docs/contracts/NET-ARCH.md §2M.2): the gateway, simulation server and replication store roles
// of DesertServer, run as in-process modules behind the interfaces NET-MESH-1 splits into processes without a
// rewrite:
//   IReplicationStore  latest state of every replicated entity (the store role; partitioned by cell from MESH-1)
//   ISimLink           gateway -> sim: a player joined / left, its input (routed by the authority map later)
//   IGatewayLink       sim -> gateway: a pawn spawned / was destroyed (the gateway tells the relevant clients)
// The gateway owns the client connections and their handshake, sends Spawn/Destroy on the Rpc lane and a
// Snapshot per connection on the State lane at the project's snapshot cadence, each pawn in it at its own send
// period by distance (Relevancy.hpp, WP-FAR-15), read from the store - it never asks the sim for state, so moving
// the store out of process changes only the link. UE: one APlayerController per connection, the pawn spawned by
// the server for it (GameMode::RestartPlayer) and replicated to every relevant connection, autonomous proxy for
// its owner, simulated proxy for the others.

#include <Net/NetTransport.hpp>
#include <Net/MovePrediction.hpp>
#include <Net/Relevancy.hpp>
#include <Net/Replication.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
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

        // STABLE OBJECTS (§3.2-§3.4): the replicated fields of cooked records, by record id. A write that changes
        // a value bumps that field's version (the push model's dirty bit; the store's counter is session-wide and
        // never reused, so a version also orders writes); an equal value is no change and keeps the version.
        // Returns the field's version after the write.
        virtual std::uint32_t WriteStable( StableNetId id, std::string_view component, std::string_view field,
                                           const ReplicatedValue& value ) = 0;
        // Every stable object that differs from its record: its fields in first-write order (a field's index is
        // its change-mask bit, stable for the session).
        [[nodiscard]] virtual const std::map<StableNetId, std::vector<FieldUpdate>>& StableObjects() const = 0;

        // THE WORLD DELTA TABLE's destroyed part (§3.3, UE DestroyedStartupOrDormantActors): a stable id destroyed
        // on the server stays destroyed for the session whatever streams in or out, and is replayed to every
        // client that loads its cell. Its changed fields are dropped with it. False when it already was.
        virtual bool                                           DestroyStable( StableNetId id ) = 0;
        [[nodiscard]] virtual const std::set<StableNetId>& DestroyedStable() const       = 0;
    };

    // THE RELEVANCY GRID (§3.5): the cooked World Partition grid, the same one residency runs on, so relevancy and
    // residency are one geometry (Iris keeps a NetObjectGridFilter grid of its own; we reuse the WP one). Unit
    // indices are the cooked index's, which client and server share (the cook hash is checked at connect).
    struct NetRelevancyGrid
    {
        struct Cell
        {
            std::string              Name; // the index's unit name, for logs
            float                    MinX = 0.f, MinZ = 0.f, MaxX = 0.f, MaxZ = 0.f; // its square, world cm
            std::vector<StableNetId> Records;
        };
        std::map<std::uint32_t, Cell>         Cells;        // every spatially loaded cell unit, by unit index
        std::map<StableNetId, std::uint32_t> UnitOfRecord; // cell records only: an always-loaded one has none
        // The main grid's loading range (cm; the first partition's, what a record naming none streams on): the
        // circle a connection sees pawns within never falls below what its client streams anyway
        // (Relevancy.hpp ResolveViewShape). 0 when the world has no grid. Pawns are relevant by POINT in the
        // connection's view shape, not by cell (WP-FAR-15; UE NetCullDistance) - the cells are for stable objects.
        float MainGridLoadingRangeCm = 0.f;
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
        // The ack of §5.1: the last of @p player's input steps the sim has processed, the own pawn's movement
        // state after it, and the input steps still queued (the client's clock sync); nullopt before the first.
        struct AckedMove
        {
            std::uint32_t Step = 0;
            PawnMoveState State;
            std::uint8_t  Buffered = 0;
        };
        [[nodiscard]] virtual std::optional<AckedMove> LastProcessedInput( PlayerId player ) const = 0;
    };

    class InProcessReplicationStore final : public IReplicationStore
    {
    public:
        NetId                  AllocateNetId() override;
        void                   Write( const PawnState& state ) override;
        void                   Remove( NetId id ) override;
        std::vector<PawnState> All() const override;
        std::uint32_t          WriteStable( StableNetId id, std::string_view component, std::string_view field,
                                            const ReplicatedValue& value ) override;
        [[nodiscard]] const std::map<StableNetId, std::vector<FieldUpdate>>& StableObjects() const override
        {
            return m_Stable;
        }
        bool                                       DestroyStable( StableNetId id ) override;
        [[nodiscard]] const std::set<StableNetId>& DestroyedStable() const override
        {
            return m_Destroyed;
        }

    private:
        NetId                                           m_Next = 1;
        std::map<NetId, PawnState>                      m_States;
        std::map<StableNetId, std::vector<FieldUpdate>> m_Stable;
        std::uint32_t                                   m_StableVersion = 0;
        std::set<StableNetId>                           m_Destroyed;
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
            std::vector<NetVec3> SpawnPoints; // world cm; the n-th player takes point n % size
        };
        // @p mover: THE GAME'S movement (Engine/Gameplay/CharacterMoveStep.hpp, the same step the client predicts
        // with, over the server's collision world of the streamed cells). Net queues and orders the input steps
        // and hands each to it; what an input means is the game's.
        PawnSimulation( IReplicationStore& store, IGatewayLink& gateway, IPawnMover& mover, Settings settings,
                        NetLogSink log );

        void PlayerJoined( PlayerId player, std::uint64_t account ) override;
        void PlayerLeft( PlayerId player ) override;
        void PlayerInput( PlayerId player, const InputMessage& input ) override;
        [[nodiscard]] std::optional<AckedMove> LastProcessedInput( PlayerId player ) const override;

        // Server authority moves @p player's pawn to @p position (UE TeleportTo); false when it has no pawn.
        bool Teleport( PlayerId player, const NetVec3& position );
        // One fixed step of every pawn (its queued input steps); writes the moved states to the store.
        void Step();
        // The pawns' positions: the server's streaming sources (NET-ARCH §4.1), with the player they belong to.
        struct Source
        {
            PlayerId      Player  = 0;
            std::uint64_t Account = 0;
            NetId         Id      = kInvalidNetId;
            NetVec3       Position;
        };
        [[nodiscard]] std::vector<Source> Sources() const;
        [[nodiscard]] bool                HasPawn( PlayerId player ) const
        {
            return m_Pawns.contains( player );
        }

        // THE HANDOVER of a pawn to another simulation (NET-ARCH §2M.4): everything the next owner needs to go on
        // at the next step - the state, the inputs not applied yet and the input that repeats when none arrives.
        struct HandoverPawn
        {
            PlayerId                     Player  = 0;
            std::uint64_t                Account = 0;
            std::uint32_t                Index   = 0;
            PawnState                    State;
            std::vector<InputStep>       Pending; // by client step
            std::optional<std::uint32_t> LastProcessed;
            PawnMoveState                Move; // the CharacterMovement state the next step starts from
        };
        // Freezes @p player's pawn and hands it out: it is no longer stepped or written here (not destroyed - its
        // net id lives on in the next owner). nullopt when this simulation has no pawn for @p player.
        [[nodiscard]] std::optional<HandoverPawn> Release( PlayerId player );
        // Takes a pawn handed over by another simulation and runs @p catchUpSteps steps of it, the mesh steps
        // that passed here between the release and now; writes its state.
        void Adopt( HandoverPawn pawn, std::uint32_t catchUpSteps );

    private:
        struct Pawn
        {
            std::uint64_t                      Account = 0;
            std::uint32_t                      Index   = 0;
            PawnState                          State;
            PawnMoveState                      Move;    // the movement state State is the replicated part of
            std::map<std::uint32_t, InputStep> Pending; // by client step (wrap: a session is < 2^31 steps)
            std::optional<std::uint32_t>       LastProcessed;
            // The newest first step of any Input message: the client resends from its oldest unacked step, so a
            // step before this floor is one the client no longer has (its ring overflowed) - the only hole given
            // up.
            std::optional<std::uint32_t> ResendFloor;
            std::uint32_t                LastActions = 0; // the last applied step's action set (for the log)
            // The queue as the last Step found it, BEFORE it consumed: the clock sync's measure (one waiting step
            // each server step = the client exactly on time; after consumption a punctual client reads 0).
            std::uint8_t QueuedAtStep = 0;
        };
        void StepPawn( Pawn& pawn ) const;

        IReplicationStore&       m_Store;
        IGatewayLink&            m_Gateway;
        IPawnMover&              m_Mover;
        Settings                 m_Settings;
        NetLogSink               m_Log;
        std::map<PlayerId, Pawn> m_Pawns;
        std::uint32_t            m_Joined = 0;
    };

    // What a gateway admits (NET-AUTH-SPLIT): the served cook, the connect tokens of its world (the issuer public
    // keys it trusts + its audience + the replay cache), and the game's PreLogin, which decides on the claims.
    struct GatewayAdmission
    {
        std::uint64_t        CookHash = 0;
        ConnectTokenVerifier Tokens;
        IPreLogin&           Game; // the server's game layer; outlives the gateway
    };

    // THE GATEWAY ROLE: client connections, the connect handshake (token + cook hash), routing input to the sim,
    // Spawn/Destroy and Snapshots out of the store, for each connection's relevant set (§3.5): its own pawn, the
    // pawns standing in its view shape (Relevancy.hpp: the cull circle + the client's reported sector, left only
    // past the exit margin), and the stable objects of the cells the client reports resident (+ always-loaded).
    // Each relevant pawn goes at its send period by distance, a dormant one at the keep-alive period, the due ones
    // by priority within the connection's byte budget (§3.6; UE Replication Graph, Iris prioritisation).
    class Gateway final : public INetTransportEvents, public IGatewayLink
    {
    public:
        // @p pawnPrefab: the server GameMode's pawn (UE DefaultPawnClass), named to every admitted client in the
        // Welcome; never 0.
        // @p relevancy: validated by the caller (ValidateRelevancySettings; the server refuses to start
        // otherwise).
        Gateway( INetTransport& transport, IReplicationStore& store, GatewayAdmission admission,
                 std::uint64_t pawnPrefab, NetRelevancyGrid grid, RelevancySettings relevancy, NetLogSink log );
        void Bind( ISimLink& sim )
        {
            m_Sim = &sim;
        }

        void OnConnected( ConnectionId connection, std::string_view address ) override;
        void OnClosed( ConnectionId connection, const DisconnectInfo& info ) override;
        void OnMessage( ConnectionId connection, ENetLane lane, std::span<const std::byte> data,
                        NetClock::time_point received ) override;

        void PawnSpawned( PlayerId owner, std::uint32_t player, const PawnState& pawn ) override;
        void PawnDestroyed( NetId id ) override;

        // Call EVERY server step: on the snapshot cadence's steps (Bands.front().EverySteps) it updates every
        // connection's relevant set and sends each its snapshot of the pawns due; on the others it does nothing.
        void                      SendSnapshots( std::uint32_t serverStep );
        [[nodiscard]] std::size_t Players() const;

        // What the gateway has sent, for the server's stat line: dynamic objects go as Spawn (reliable lane),
        // stable objects only ever as state (State lane, against each connection's acked baseline).
        struct Stats
        {
            std::uint64_t Spawns        = 0; // Spawn messages (dynamic pawns), all connections
            std::uint64_t StablePackets = 0; // ObjectState packets
            std::uint64_t StableFields  = 0; // field updates in them (a resend counts again)
            std::uint64_t Resent        = 0; // field updates sent again because their packet was lost
            std::uint64_t Acked         = 0; // field updates the clients acked
            std::uint64_t Irrelevant    = 0; // pawns removed from a connection's relevant set (Destroy NotRelevant)
            std::uint64_t DeltaReplays  = 0; // destroyed stable ids replayed to a client loading their cell
            std::uint64_t SnapshotBytes = 0; // remote pawn entry bytes written into snapshots, all connections
            std::uint64_t Deferred = 0; // due entries the byte budget pushed to a later snapshot, all connections
            // Per admitted connection, as of its last snapshot (UE `stat net` per connection).
            struct Connection
            {
                PlayerId      Player             = 0;
                std::uint64_t PawnBytesPerSecond = 0;    // remote pawn entry bytes over the last whole second
                std::vector<std::uint32_t> ByBand;       // non-dormant remote pawns per band, Bands' order
                std::uint32_t              Dormant  = 0; // remote pawns at the dormant period
                std::uint32_t              Deferred = 0; // due entries the budget deferred in the last snapshot
            };
            std::vector<Connection> Connections; // by player
        };
        // The totals and every connection's numbers now (built on call: the stat line asks every few seconds).
        [[nodiscard]] Stats                    ReplicationStats() const;
        [[nodiscard]] const RelevancySettings& Relevancy() const
        {
            return m_Relevancy;
        }

    private:
        // PER CONNECTION x STABLE OBJECT: the version each field was last acked at (the baseline) and the version
        // in flight (0 = none), by the field's index in the store's object.
        struct ObjectBaseline
        {
            std::vector<std::uint32_t> Acked, InFlight;
        };
        struct SentField
        {
            StableNetId   Id      = 0;
            std::size_t   Index   = 0;
            std::uint32_t Version = 0;
        };
        struct SentPacket
        {
            std::uint32_t          Step = 0; // the server step it went out on
            std::vector<SentField> Fields;
        };
        struct SentPawn
        {
            std::uint32_t LastSentStep = 0;
            PawnState     LastSent;
            bool          Dormant = false; // LastSent equalled the store at the last decision (logged on change)
        };
        struct Peer
        {
            bool                                  Admitted = false;
            PlayerId                              Player   = 0;
            std::string                           Address; // where it connected from (the game's PreLogin sees it)
            std::map<StableNetId, ObjectBaseline> Baselines;
            std::map<std::uint32_t, SentPacket>   InFlight; // by ObjectState sequence
            std::uint32_t                         NextStateSequence = 1;
            std::set<std::uint32_t>               Resident;     // cell units the client reports resident
            std::set<NetId>                       Relevant;     // pawns this client has (Spawn sent, no Destroy)
            std::set<StableNetId>                 DeltaSent;    // destroyed stable ids replayed to it
            std::set<std::uint32_t>               AwaitingAck;  // units reported resident, not yet acknowledged
            // WP-FAR-15: the newest view shape the client reported (nullopt = never: the cull circle alone).
            std::optional<ViewReport> View;
            // Per relevant remote pawn: what this connection last received of it (UE
            // FConnectionReplicationActorInfo). Absent = never sent since it became relevant: due at once.
            std::map<NetId, SentPawn> Sent;
            // Remote pawn entry bytes sent, and the count at the start of the current one-second window.
            std::uint64_t BytesSent = 0, BytesAtWindow = 0;
            std::uint32_t WindowStep = 0;
            // As of the last snapshot (the stat line).
            std::uint64_t              BytesPerSecond = 0;
            std::vector<std::uint32_t> ByBand;
            std::uint32_t              Dormant = 0, Deferred = 0;
        };
        struct PawnInfo
        {
            PlayerId      Owner  = 0;
            std::uint32_t Player = 0;
        };
        [[nodiscard]] bool StableRelevant( const Peer& peer, StableNetId id ) const;
        // The resident cell of @p peer the position stands in, or nullptr.
        void OnCellResidency( ConnectionId connection, Peer& peer, const CellResidencyMessage& message );
        // The relevant set by point in the view shape, with the exit margin; returns the viewer (own pawn)
        // position.
        std::optional<NetVec3> UpdateRelevancy( ConnectionId connection, Peer& peer,
                                                const std::vector<PawnState>& pawns );
        // The snapshot of one connection: the pawns due on this step by period, the budget cut, the stats.
        void SendSnapshot( ConnectionId connection, Peer& peer, const std::vector<PawnState>& pawns,
                           std::uint32_t serverStep );
        void ReplayWorldDelta( ConnectionId connection, Peer& peer );
        void SendStableState( ConnectionId connection, Peer& peer, std::uint32_t serverStep );
        void OnStateAck( Peer& peer, const StateAckMessage& ack, std::uint32_t serverStep );
        void ExpireLost( Peer& peer, std::uint32_t newestAcked, std::uint32_t serverStep, bool byAck );
        void Admit( ConnectionId connection, Peer& peer, std::span<const std::byte> hello );

        INetTransport&                   m_Transport;
        IReplicationStore&               m_Store;
        GatewayAdmission                 m_Admission;
        std::uint64_t                    m_PawnPrefab = 0;
        NetRelevancyGrid                 m_Grid;
        RelevancySettings                m_Relevancy;
        std::size_t                      m_EntryBytes = 0; // one snapshot entry on the wire (measured at start)
        NetLogSink                       m_Log;
        std::map<NetId, PawnInfo>        m_PawnInfo;
        ISimLink*                        m_Sim = nullptr;
        std::map<ConnectionId, Peer>     m_Peers;
        std::map<PlayerId, ConnectionId> m_Connections;
        PlayerId                         m_NextPlayer = 1;
        std::uint32_t                    m_ServerStep = 0;
        Stats                            m_Stats;
    };
} // namespace Desert::Net
