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
#include <vector>

namespace Desert::Net
{
    using NetId                              = std::uint32_t;
    inline constexpr NetId  kInvalidNetId    = 0;
    inline constexpr double kNetStepHz       = 60.0; // the server's fixed step (§2); the net frame number
    inline constexpr int    kInputRedundancy = 3;    // input steps per Input message (§2)
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
    };

    struct NetVec3
    {
        float X = 0.f, Y = 0.f, Z = 0.f;
    };

    // One fixed step of a player's input: the world-space direction the player asks to move in (length 0..1,
    // already turned by its view on the client) and the yaw it faces, in degrees.
    struct InputStep
    {
        std::uint32_t Step = 0; // the client's fixed-step number
        NetVec3       Move;
        float         Yaw = 0.f;
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

    struct SnapshotMessage
    {
        std::uint32_t          ServerStep   = 0;
        std::uint32_t          AckInputStep = 0; // the last of this connection's input steps the server processed
        bool                   HasAck       = false;
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
    std::vector<std::byte> EncodeDestroy( NetId id );

    // The message type of a session message, or nullopt for an empty message.
    std::optional<EReplicationMessage> PeekReplicationMessage( std::span<const std::byte> data );
    // Each decode returns nullopt for a malformed message (wrong type, truncated, trailing bytes, too many steps).
    std::optional<InputMessage>    DecodeInput( std::span<const std::byte> data );
    std::optional<SnapshotMessage> DecodeSnapshot( std::span<const std::byte> data );
    std::optional<SpawnMessage>    DecodeSpawn( std::span<const std::byte> data );
    std::optional<NetId>           DecodeDestroy( std::span<const std::byte> data );

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

    // Wrap-safe "a is after b" for 32-bit step numbers.
    constexpr bool StepAfter( std::uint32_t a, std::uint32_t b )
    {
        return static_cast<std::int32_t>( a - b ) > 0;
    }
} // namespace Desert::Net
