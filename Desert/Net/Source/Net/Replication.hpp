#pragma once

// PAWN REPLICATION WIRE + CLIENT-SIDE BUFFERS (Docs/contracts/NET-ARCH.md §2, §3.1, §5).
//
// After the handshake (NetHandshake.hpp) a session speaks four messages:
//   Input    client -> server, State lane (unreliable): the last kInputRedundancy fixed steps of input, each with
//            the client's fixed-step number, under a sequence id - one lost packet loses no input (§2).
//   Snapshot server -> client, State lane (unreliable): the server step, the last input step the server has
//            PROCESSED for this connection (the ack the client's prediction replays from, NET-4 §5.1), and the
//            transform of every replicated pawn relevant to the connection.
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
    inline constexpr double kNetStepHz       = 60.0; // the server's fixed step (§2); the net frame number
    inline constexpr int    kInputRedundancy = 3;    // input steps per Input message (§2)
    // One input step's length - the move both the server and the predicting client simulate (PawnMovement.hpp).
    inline constexpr float kNetStepSeconds = static_cast<float>( 1.0 / kNetStepHz );
    // Snapshots go out every second step: 30 Hz per connection (§2). A project setting once §3.6 lands.
    inline constexpr int kSnapshotEverySteps = 2;
    // How far behind the server clock remote pawns are drawn: three snapshot intervals, so one lost snapshot
    // still interpolates between two received ones (UE / Source default 100 ms at 30 Hz).
    inline constexpr double kInterpolationDelaySeconds = 0.1;

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
        CellResidency  = kFirstSessionMessage + 22,
        StableDestroy  = kFirstSessionMessage + 23,
    };

    struct NetVec3
    {
        float X = 0.f, Y = 0.f, Z = 0.f;
    };

    // One fixed step of a player's input: the world-space direction the player asks to move in (length 0..1,
    // already turned by its view on the client) and the yaw it faces, in degrees.
    // InputStep::Buttons (UE FSavedMove_Character compressed flags): held / pressed this step.
    inline constexpr std::uint8_t kInputSprint = 1u << 0;
    inline constexpr std::uint8_t kInputJump   = 1u << 1; // pressed: a jump starts on this step when grounded

    struct InputStep
    {
        std::uint32_t Step = 0; // the client's fixed-step number
        NetVec3       Move;
        float         Yaw     = 0.f;
        std::uint8_t  Buttons = 0;
    };

    struct InputMessage
    {
        std::uint32_t          Sequence = 0;
        std::vector<InputStep> Steps; // oldest first, at most kInputRedundancy
    };

    struct PawnState
    {
        NetId   Id = kInvalidNetId;
        NetVec3 Position;
        float   Yaw = 0.f;
    };

    // The own character's whole movement state (PawnMovement.hpp): what the server sends back for the acked input
    // step so the predicting client can compare and replay (UE ClientAdjustPosition's location + velocity + base +
    // movement mode). Raw floats on the wire - the client compares them with its own, bit-exact path.
    struct PawnMoveState
    {
        NetVec3 Position;
        NetVec3 Velocity;
        float   Yaw      = 0.f;
        float   FloorY   = 0.f; // the floor plane the pawn stands on (PawnMovement.hpp)
        bool    Grounded = true;
    };

    struct SnapshotMessage
    {
        std::uint32_t          ServerStep   = 0;
        std::uint32_t          AckInputStep = 0; // the last of this connection's input steps the server processed
        bool                   HasAck       = false;
        // With the ack: the own pawn's state after AckInputStep, and how many of this connection's input steps
        // wait in the server's queue - the client's clock sync dilates its step rate to keep it at a small target
        // (Overwatch / Rocket League input buffer feedback).
        PawnMoveState          OwnState;
        std::uint8_t           InputBuffered = 0;
        std::vector<PawnState> Pawns;
    };

    struct SpawnMessage
    {
        PawnState     Pawn;
        bool          Owned  = false;
        std::uint32_t Player = 0; // the server's player index (its PlayerController), for names and logs
    };

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
    // ones it processed; Latest(n) is what the next Input message carries.
    class InputRing
    {
    public:
        static constexpr std::size_t kCapacity = 128; // 2.1 s at 60 Hz: an ack older than that is a lost session

        void Push( const InputStep& step );
        // Drops every input with Step <= @p ackStep (wrap-safe step comparison).
        void DropUpTo( std::uint32_t ackStep );
        // The newest @p count inputs, oldest first.
        [[nodiscard]] std::vector<InputStep> Latest( std::size_t count ) const;
        [[nodiscard]] std::size_t            Size() const
        {
            return m_Steps.size();
        }

    private:
        std::deque<InputStep> m_Steps;
    };

    // ONE REMOTE PAWN'S TRANSFORM HISTORY, by server time: sampled between the two snapshots around the render
    // time (linear position, shortest-arc yaw); before the first or after the last it holds the end sample - no
    // extrapolation, which would be prediction (§5.3).
    class InterpolationBuffer
    {
    public:
        void                    Push( double serverSeconds, const PawnState& state );
        [[nodiscard]] PawnState Sample( double serverSeconds ) const;
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
