#pragma once

#include <Common/Core/SimulationStep.hpp>

// PAWN REPLICATION WIRE + CLIENT-SIDE BUFFERS (Docs/contracts/NET-ARCH.md §2, §3.1, §5).
//
// After the handshake (NetHandshake.hpp) a session speaks four messages:
//   Input    client -> server, State lane (unreliable): every fixed step of input the server has not acked yet
//            (all of them, up to kMaxUnackedInputSteps) as one consecutive run from the first step's number, under
//            a sequence id (the run's layout and quantization: InputWire.hpp) - a lost packet loses no input: its
//            steps ride in every later one until the ack (§2; UE ServerMove resent until ClientAckGoodMove,
//            Overwatch-style full resend).
//   Snapshot server -> client, State lane (unreliable): the server step, the last input step the server has
//            PROCESSED for this connection (the ack the client's prediction replays from, NET-4 §5.1), and the
//            transform of every replicated pawn relevant to the connection that is DUE this step (WP-FAR-15: near
//            pawns every snapshot, far ones every N-th, each entry stamped with its period; Relevancy.hpp).
//   Spawn    server -> client, Rpc lane (reliable, ordered): a pawn entered the connection's view; `Owned` marks
//            the connection's own pawn (UE: the PlayerController's possessed pawn, autonomous proxy).
//   Destroy  server -> client, Rpc lane: the pawn left (its player disconnected, or it is no longer relevant).
// A Snapshot entry for a net id the client has not been told to Spawn yet is skipped (lanes are not ordered
// with each other); the next snapshot after the Spawn carries it again.
//
// The client keeps (a) an InputRing of the inputs it sent, dropped up to the server's ack - the replay buffer of
// NET-4's move replay, unused for correction until then - and (b) one InterpolationBuffer per remote pawn,
// sampled a fixed delay behind the estimated server clock (§5.3, UE's simulated-proxy smoothing / Source's
// cl_interp). Engine-free: positions are world centimetres as plain floats.

#include <Net/NetHandshake.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Desert::Net
{
    using NetId                              = std::uint32_t;
    inline constexpr NetId  kInvalidNetId    = 0;
    inline constexpr double kNetStepHz       = Common::kSimulationStepHz; // the server's fixed step (§2); the net frame number
    // The unacked input steps the client keeps and resends (§2): the InputRing's capacity, 2.1 s at 60 Hz, and so
    // the most one Input message carries. The server moves a pawn by every step in order and waits for a missing
    // one, so every Input message carries EVERY unacked step - never only the newest few: a slow client frame
    // records a burst of steps (20-30 at 3 fps), and a newest-32 window pushed the older steps of the burst
    // before it out after one send, so one lost packet lost them for good (NET-4 run 1125-061130: steps
    // 742..754). An unacked step older than this is a lost session's; the server gives it up when a message
    // no longer starts at or before it (MeshOfOne.cpp PawnSimulation::StepPawn).
    inline constexpr std::size_t kMaxUnackedInputSteps = 128;
    // One input step's length - the move both the server and the predicting client simulate (the game's
    // Net::IPawnMover, MovePrediction.hpp).
    inline constexpr float kNetStepSeconds = static_cast<float>( 1.0 / kNetStepHz );
    // WP-FAR-15: the snapshot cadence and every pawn's send period are the project's Net settings
    // (Relevancy.hpp RelevancySettings::Bands, Project::NetSettings); each snapshot entry says the period it went
    // at (SnapshotEntry::PeriodSteps). There is no fixed snapshot step in code.
    // How far behind the server clock remote pawns are drawn at least: three 30 Hz snapshot intervals, so one lost
    // snapshot still interpolates between two received ones (UE / Source default 100 ms at 30 Hz). A pawn sent at
    // a longer period is drawn farther behind (Relevancy.hpp PawnInterpolationDelay).
    inline constexpr double kInterpolationDelaySeconds = 0.1;
    // NET-5 (§5.3): the adaptive delay (ReplicationClient) - two snapshot intervals plus the measured arrival
    // jitter, never below kInterpolationDelaySeconds nor above kMaxInterpolationDelaySeconds; past the newest
    // snapshot a remote pawn is extrapolated along its velocity for at most kMaxExtrapolationSeconds, then held.
    inline constexpr double kMaxInterpolationDelaySeconds = 0.35;
    inline constexpr double kMaxExtrapolationSeconds      = 0.25;

    enum class EReplicationMessage : std::uint8_t
    {
        Input    = kFirstSessionMessage + 16,
        Snapshot = kFirstSessionMessage + 17,
        Spawn    = kFirstSessionMessage + 18,
        Destroy  = kFirstSessionMessage + 19,
        // NET-2 (§3.2-§3.4): the replicated state of stable objects, and the client's ack of it.
        ObjectState = kFirstSessionMessage + 20,
        StateAck    = kFirstSessionMessage + 21,
        // NET-3 (§3.3, §4.3): client -> server, the WP cell units that became resident / unloaded on the client
        // (UE ServerUpdateLevelVisibility); server -> client, the world delta table's destroyed stable ids of the
        // cells the client has resident (UE DestroyedStartupOrDormantActors, replayed when the level streams in).
        CellResidency = kFirstSessionMessage + 22,
        StableDestroy = kFirstSessionMessage + 23,
        // WP-FAR-15 (§3.5, §3.6), WP-STREAM-SOURCE-VIEW: client -> server, State lane, the view point and its
        // shape (the player's eye and its view streaming source's facing, sector angle, range) - the server's
        // relevancy, send periods and per-player residency are measured from that eye, bounded by the own pawn
        // (Relevancy.hpp ViewReport, ResolveViewPosition). Sent when the eye or the shape changes and once a
        // second.
        ViewReport = kFirstSessionMessage + 24,
    };

    struct NetVec3
    {
        float X = 0.f, Y = 0.f, Z = 0.f;
    };

    // One fixed step of a player's input. Net numbers it, carries it, acks it and resends it; it never reads what
    // it says. Move and Yaw are the pawn's movement intent (UE ServerMove's Acceleration + View; Mover's
    // MoveInput + OrientationIntent): the world-space direction the player asks to move in (length 0..1, already
    // turned by its view on the client) and the yaw it faces, in RADIANS about +Y (0 faces -Z; PawnState::Yaw).
    // On the wire every field is quantized (InputWire.hpp OnWireLattice): a sent step is on that lattice.
    // Everything else is the PAWN'S DECLARED ACTION SET, opaque here (Mover 2.0: the game declares the input
    // struct the net layer carries): bit i of Actions is the pawn's i-th declared digital action, Axes[j] its j-th
    // declared analog one - the game's movement (Engine/Gameplay/PawnActions.hpp) names them and maps them to
    // what they do. There is no sprint or jump here: a pawn that declares a new action sends it without a change
    // to this file.
    inline constexpr std::size_t kMaxInputActions = 32; // the bits of InputStep::Actions
    inline constexpr std::size_t kMaxInputAxes    = 8;

    struct InputStep
    {
        std::uint32_t      Step = 0; // the client's fixed-step number
        NetVec3            Move;
        float              Yaw     = 0.f;
        std::uint32_t      Actions = 0;
        std::vector<float> Axes; // at most kMaxInputAxes
    };

    struct InputMessage
    {
        std::uint32_t          Sequence = 0;
        std::vector<InputStep>
             Steps; // oldest first, every unacked step (consecutive), at most kMaxUnackedInputSteps
    };

    // The movement mode bits of a pawn's state (UE: the simulated proxy's replicated MovementMode).
    inline constexpr std::uint8_t kPawnGrounded = 1u << 0;

    struct PawnState
    {
        NetId   Id = kInvalidNetId;
        NetVec3 Position;
        float   Yaw = 0.f; // radians about +Y (0 faces -Z), the pawn root's facing
        // NET-5 (§5.3): the movement's velocity in cm/s - the Hermite tangent of the remote pawn's path and the
        // source of its locomotion variables (speed, direction), as UE's simulated proxy replicates Velocity.
        NetVec3      Velocity;
        std::uint8_t Movement = 0; // kPawn* bits
        // Bumped by every teleport (wraps): two samples with different counts are never interpolated between
        // (UE's replicated teleport flag), so a teleport is a cut, not a 1.5 km slide.
        std::uint8_t TeleportCount = 0;
    };

    // The own character's whole movement state (Net::IPawnMover): what the server sends back for the acked input
    // step so the predicting client can compare and replay (UE ClientAdjustPosition's location + velocity + base +
    // movement mode). Raw floats on the wire - the client compares them with its own, bit-exact path.
    struct PawnMoveState
    {
        NetVec3 Position; // the capsule's feet
        NetVec3 Velocity;
        float   Yaw      = 0.f;
        bool    Grounded = true;
        // The game's movement mode bits beyond Grounded (Gameplay: crouched, swimming, jump held), opaque here:
        // carried and compared, never read - at most kMaxMoveModeBits of them.
        std::uint8_t Mode = 0;
    };
    inline constexpr unsigned kMaxMoveModeBits = 7;
    // The move state's one flag byte on the wire: Grounded in bit 0, the game's Mode bits above it.
    [[nodiscard]] inline std::uint8_t MoveFlagsOf( const PawnMoveState& state )
    {
        return static_cast<std::uint8_t>( ( state.Grounded ? 1u : 0u ) | ( unsigned( state.Mode ) << 1 ) );
    }
    inline void SetMoveFlags( PawnMoveState& state, std::uint8_t flags )
    {
        state.Grounded = ( flags & 1u ) != 0;
        state.Mode     = static_cast<std::uint8_t>( flags >> 1 );
    }

    // One pawn in a snapshot and the period, in server steps, the gateway sends it to this connection at now
    // (Relevancy.hpp SendPeriodSteps / DormantEverySteps): the client's interpolation delay for the pawn follows
    // it (two of its intervals + jitter), so a 6 Hz pawn never runs out of samples and extrapolates into a hold.
    struct SnapshotEntry
    {
        PawnState    Pawn;
        std::uint8_t PeriodSteps = 2; // >= 1; one byte on the wire (a period is at most 255 steps = 4.25 s)
    };

    struct SnapshotMessage
    {
        std::uint32_t          ServerStep   = 0;
        std::uint32_t          AckInputStep = 0; // the last of this connection's input steps the server processed
        bool                   HasAck       = false;
        // With the ack: the own pawn's state after AckInputStep, and how many of this connection's input steps
        // wait in the server's queue - the client's clock sync dilates its step rate to keep it at a small target
        // (Overwatch / Rocket League input buffer feedback).
        PawnMoveState              OwnState;
        std::uint8_t               InputBuffered = 0;
        // The pawns DUE for this connection on this step (WP-FAR-15): a near pawn is in every snapshot, a far one
        // in every N-th, a dormant one in every DormantEverySteps-th; the budget cut defers the least urgent to the
        // next snapshot. A pawn absent from a snapshot is not gone - Destroy says that; it was not due.
        std::vector<SnapshotEntry> Pawns;
    };

    struct SpawnMessage
    {
        PawnState     Pawn;
        bool          Owned  = false;
        std::uint32_t Player = 0; // the server's player index (its PlayerController), for names and logs
    };

    // Empty when the steps are not a run the wire carries (InputWire.hpp AppendInputRun: not consecutive, none, more
    // than kMaxUnackedInputSteps, too many axes) - the caller reports it and sends nothing.
    std::vector<std::byte> EncodeInput( const InputMessage& message );
    std::vector<std::byte> EncodeSnapshot( const SnapshotMessage& message );
    std::vector<std::byte> EncodeSpawn( const SpawnMessage& message );
    // Why a dynamic object leaves a client (UE: actor destroyed vs. its channel closed for relevancy).
    enum class EDestroyReason : std::uint8_t
    {
        Destroyed   = 0, // the object is gone on the server
        NotRelevant = 1, // it left this connection's relevant set (§3.5); it comes back as a Spawn when relevant
    };
    struct DestroyMessage
    {
        NetId          Id     = kInvalidNetId;
        EDestroyReason Reason = EDestroyReason::Destroyed;
    };
    std::vector<std::byte> EncodeDestroy( const DestroyMessage& message );

    // The message type of a session message, or nullopt for an empty message.
    std::optional<EReplicationMessage> PeekReplicationMessage( std::span<const std::byte> data );
    // Each decode returns nullopt for a malformed message (wrong type, truncated, trailing bytes, too many steps).
    std::optional<InputMessage>    DecodeInput( std::span<const std::byte> data );
    std::optional<SnapshotMessage> DecodeSnapshot( std::span<const std::byte> data );
    std::optional<SpawnMessage>    DecodeSpawn( std::span<const std::byte> data );
    std::optional<DestroyMessage>  DecodeDestroy( std::span<const std::byte> data );

    // THE CLIENT'S SENT INPUTS (NET-4's replay buffer): every step's input is pushed; the server's ack drops the
    // ones it processed; Unacked() - all of the rest - is what the next Input message carries.
    class InputRing
    {
    public:
        static constexpr std::size_t kCapacity = kMaxUnackedInputSteps; // an ack older than that: a lost session

        void Push( const InputStep& step );
        // Drops every input with Step <= @p ackStep (wrap-safe step comparison).
        void DropUpTo( std::uint32_t ackStep );
        // Every input not acked yet, oldest first: what each Input message carries.
        [[nodiscard]] std::vector<InputStep> Unacked() const;
        [[nodiscard]] std::size_t            Size() const
        {
            return m_Steps.size();
        }

    private:
        std::deque<InputStep> m_Steps;
    };

    // ONE REMOTE PAWN'S TRANSFORM HISTORY, by server time (§5.3): sampled between the two snapshots around the
    // render time as a cubic Hermite of position with the replicated velocities as tangents (C1 across snapshots:
    // no velocity kinks at the samples, which linear interpolation shows as a stutter at 30 Hz), shortest-arc yaw,
    // and the Hermite's derivative as the sampled velocity. Past the newest sample it extrapolates along the
    // newest velocity for at most kMaxExtrapolationSeconds, then holds (UE SmoothClientPosition's capped
    // extrapolation); before the first it holds it. Across a teleport (two samples with different TeleportCount)
    // the path runs on along the earlier sample's velocity by the same capped rule until the later sample's time,
    // then cuts to it - never a slide, never a hold with a moving velocity (the locomotion speed is the drawn
    // speed throughout).
    class InterpolationBuffer
    {
    public:
        void                    Push( double serverSeconds, const PawnState& state );
        // @p extrapolationCap: how far past the newest sample the path is extrapolated before it holds - a pawn
        // received every N steps gets a longer cap (WP-FAR-15, Relevancy.hpp PawnExtrapolationCap).
        [[nodiscard]] PawnState Sample( double serverSeconds,
                                        double extrapolationCap = kMaxExtrapolationSeconds ) const;
        // The newest snapshot's state as the server sent it (never extrapolated); {} when empty.
        [[nodiscard]] PawnState Newest() const
        {
            return m_Samples.empty() ? PawnState{} : m_Samples.back().State;
        }
        [[nodiscard]] bool      Empty() const
        {
            return m_Samples.empty();
        }

    private:
        struct Sample_
        {
            double    Time = 0.0;
            PawnState State;
        };
        std::deque<Sample_> m_Samples; // ascending time, the last ~1 s
    };

    // ---- STABLE OBJECTS: STATE, NEVER SPAWNED (NET-ARCH §3.2-§3.4) ---------------------------------------------
    //
    // A stable object is a record of the cooked world (a cell entity or an always-loaded one). Its net id IS the
    // record id the cooked index lists and the entity carries as its UUID, so every machine with the same cook
    // (the handshake checks the cook hash) already has the entity: the server never spawns it, it sends only the
    // fields that differ from the authored record. Each such field has a change VERSION in the server's store
    // (a write bumps it: the push model's dirty bit, Iris); per connection the gateway keeps the version each
    // field was last ACKED at - the acked baseline - and sends a field while its version is newer than that
    // baseline and not already in flight. A lost packet clears its in-flight marks, so exactly its fields go again
    // (Iris), never a whole snapshot; a new connection has an empty baseline and so receives the full state.
    using StableNetId = std::uint64_t;

    // The component key of the dedicated transform fragment (§3.1): position + rotation of the record's entity.
    inline constexpr std::string_view kTransformFragment = "Transform";

    struct NetTransform
    {
        NetVec3              Position;                       // world cm
        std::array<float, 4> Rotation{ 0.f, 0.f, 0.f, 1.f }; // unit quaternion x, y, z, w
    };

    // §3.1's quantization: position in whole millimetres (int32: +-21 km, the world's extent), rotation
    // smallest-three - the index of the largest quaternion component in 2 bits, the other three in 10 bits each
    // over [-1/sqrt2, 1/sqrt2] (about 0.08 degree).
    struct QuantizedTransform
    {
        std::array<std::int32_t, 3> Millimetres{};
        std::uint32_t               Rotation = 0;

        friend bool operator==( const QuantizedTransform&, const QuantizedTransform& ) = default;
    };
    [[nodiscard]] QuantizedTransform QuantizeTransform( const NetTransform& transform );
    [[nodiscard]] NetTransform       DequantizeTransform( const QuantizedTransform& quantized );

    // A replicated field's value: the quantizers of this pass (bool, int, float as themselves; the transform).
    using ReplicatedValue = std::variant<bool, std::int32_t, float, QuantizedTransform>;
    [[nodiscard]] std::string ReplicatedValueText( const ReplicatedValue& value );

    // One field of a stable object: its reflection row key ("DirectionLight") and field name ("Intensity"), or
    // kTransformFragment twice for the transform; the store's version of the value.
    struct FieldUpdate
    {
        std::string     Component;
        std::string     Field;
        std::uint32_t   Version = 0;
        ReplicatedValue Value;
    };

    struct ObjectUpdate
    {
        StableNetId              Id = 0;
        std::vector<FieldUpdate> Fields;
    };

    // Server -> client, State lane (unreliable). Sequence numbers the packet for the client's ack.
    struct ObjectStateMessage
    {
        std::uint32_t             Sequence = 0;
        std::vector<ObjectUpdate> Objects;
    };

    // Client -> server, State lane: the newest ObjectState sequence received and, in bit i of Previous, whether
    // Newest - 1 - i was received too - every ack is repeated 32 times, so a lost ack loses nothing.
    struct StateAckMessage
    {
        std::uint32_t Newest   = 0;
        std::uint32_t Previous = 0;
    };

    // CELL RESIDENCY (§4.3): residency unit indices of the cooked index both ends share (the cook hash is checked
    // at connect, so a unit index names the same cell on both). Reliable lane, so only changes are sent.
    struct CellResidencyMessage
    {
        std::vector<std::uint32_t> Loaded, Unloaded;
    };
    // THE WORLD DELTA TABLE's destroyed part for one cell unit (§3.3): the client destroys those records of the
    // cell and skips them when its streamer makes the cell again. Reliable lane. It is also the server's answer to
    // a residency report of the cell (UE ClientAckUpdateLevelVisibility), with no ids when nothing was destroyed:
    // the client reveals the cell only once it has it.
    struct StableDestroyMessage
    {
        std::uint32_t            Unit = 0;
        std::vector<StableNetId> Ids;
    };
    std::vector<std::byte>              EncodeCellResidency( const CellResidencyMessage& message );
    std::vector<std::byte>              EncodeStableDestroy( const StableDestroyMessage& message );
    std::optional<CellResidencyMessage> DecodeCellResidency( std::span<const std::byte> data );
    std::optional<StableDestroyMessage> DecodeStableDestroy( std::span<const std::byte> data );

    std::vector<std::byte>            EncodeObjectState( const ObjectStateMessage& message );
    std::vector<std::byte>            EncodeStateAck( const StateAckMessage& message );
    std::optional<ObjectStateMessage> DecodeObjectState( std::span<const std::byte> data );
    std::optional<StateAckMessage>    DecodeStateAck( std::span<const std::byte> data );

    // THE CLIENT'S ACK WINDOW over ObjectState sequences.
    class StateAckWindow
    {
    public:
        void                          Received( std::uint32_t sequence );
        [[nodiscard]] StateAckMessage Message() const
        {
            return { m_Newest, m_Previous };
        }
        // Whether sequence @p s is acked by @p ack (it is Newest, or one of the 32 before it with its bit set).
        [[nodiscard]] static bool Acks( const StateAckMessage& ack, std::uint32_t s );

    private:
        std::uint32_t m_Newest = 0, m_Previous = 0;
        bool          m_Any = false;
    };

    // Wrap-safe "a is after b" for 32-bit step numbers.
    constexpr bool StepAfter( std::uint32_t a, std::uint32_t b )
    {
        return static_cast<std::int32_t>( a - b ) > 0;
    }
} // namespace Desert::Net
