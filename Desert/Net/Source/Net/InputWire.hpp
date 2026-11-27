#pragma once

// THE INPUT STEP ON THE WIRE (NET-INPUT-PACK; NET-ARCH §2). One codec for every place an InputStep crosses a
// connection: the client's Input message (Replication.cpp EncodeInput / DecodeInput), the mesh's PlayerInput /
// ForwardInput (they carry an encoded Input message whole) and the mesh handover's pending list (MeshLink.cpp).
// UE 5.5 is the pattern: FCharacterNetworkMoveData::Serialize - quantized acceleration (FVector_NetQuantize10),
// compressed view rotation (16-bit axes), one flags byte - and FCharacterNetworkMoveDataContainer carrying several
// moves in one unreliable packet. Ours carries EVERY unacked step (Replication.hpp kMaxUnackedInputSteps), so the
// run is coded as one baseline and per-step deltas against the step before it IN THE SAME MESSAGE: a message
// decodes alone, whatever was lost before it (the State lane is unreliable and unordered).
//
// THE LATTICE. Every field is quantized here and nowhere else:
//   Move.X/Y/Z, Axes[j]  -> int8   q = round(clamp(v, -1, 1) * kMoveScale),  v' = q / kMoveScale   (Quake 3 usercmd)
//   Yaw (radians)        -> int16  q = round(wrap(yaw) * kYawScale),           yaw' = q / kYawScale  (UE CompressAxisToShort)
//   Actions              -> uint32 as is (bit i = the pawn's i-th declared action, Replication.hpp)
// The client SNAPS a step to the lattice when it numbers it (ReplicationClient::RecordInput -> OnWireLattice) and
// predicts the snapped step; the server decodes the same q to the same float: both sides step one value, and the
// replay (MovePrediction.cpp) steps it again. A step the game builds and never sends (standalone Play) is never
// snapped. Yaw is RADIANS everywhere (ClientNetDriver.cpp NextInput, LocalPawnInput.cpp, CharacterMoveStep.cpp
// state.Yaw, PawnState::Yaw); the old "degrees" comment in Replication.hpp is a defect this header retires.
//
// RUN LAYOUT (little-endian; after the message's own type / sequence bytes):
//   U32 FirstStep, U8 Count (1..kMaxUnackedInputSteps)
//   step 0, FULL:       I8 MoveX, I8 MoveY, I8 MoveZ, I16 Yaw, U32 Actions, U8 axisCount (<= kMaxInputAxes), I8 x axisCount
//   steps 1..Count-1:   U8 mask (kDelta* bits; the upper bits are zero or the message is malformed), then only
//                       the fields the mask names, in the FULL order (Move 3 B, Yaw 2 B, Actions 4 B, Axes 1+n B)
// Step numbers are not on the wire: the run is CONSECUTIVE (FirstStep + i). The InputRing is consecutive by
// construction (numbered by one counter, trimmed only at the front), so the encoder refuses anything else.
//
// Sizes: a walking pawn at 60 Hz with 15 unacked steps - 10 B run (+10 B message head: type, sequence) against
// 381 B today; a starved client (50 steps, turning every step) - 304 B, under one UDP MTU, against 1256 B.

#include <Net/Replication.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Net
{
    inline constexpr float kMoveScale = 127.f;   // int8 lattice of a -1..1 intent
    inline constexpr float kYawScale  = 32768.f / 3.14159265358979f; // int16 lattice of [-pi, pi)

    // The delta mask of steps 1..Count-1 of a run.
    inline constexpr std::uint8_t kDeltaMove    = 1u << 0;
    inline constexpr std::uint8_t kDeltaYaw     = 1u << 1;
    inline constexpr std::uint8_t kDeltaActions = 1u << 2;
    inline constexpr std::uint8_t kDeltaAxes    = 1u << 3;
    inline constexpr std::uint8_t kDeltaKnown   = kDeltaMove | kDeltaYaw | kDeltaActions | kDeltaAxes;

    // Byte sizes the acceptance and the logs quote; the encoder's output is exactly these.
    inline constexpr std::size_t kRunHeadBytes     = 4 + 1;         // FirstStep, Count
    inline constexpr std::size_t kFullStepBytes    = 3 + 2 + 4 + 1; // without the axes' bytes
    inline constexpr std::size_t kUnchangedStepBytes = 1;           // a delta step whose mask is 0

    // The step as the server will see it: every field quantized and dequantized through the lattice above. Pure;
    // idempotent (OnWireLattice(OnWireLattice(s)) == OnWireLattice(s), field by field, as floats).
    [[nodiscard]] InputStep OnWireLattice( const InputStep& step );

    // Appends the run of @p steps (oldest first, consecutive Step numbers, 1..kMaxUnackedInputSteps of them) to
    // @p out as FirstStep, Count, baseline, deltas. Fields are quantized on the way out (a snapped step changes
    // nothing). Returns false and appends nothing when @p steps is empty, longer than kMaxUnackedInputSteps, not
    // consecutive, or an axis list is longer than kMaxInputAxes - the caller reports it; nothing is sent.
    [[nodiscard]] bool AppendInputRun( std::vector<std::byte>& out, std::span<const InputStep> steps );

    // Reads one run from the front of @p in, advancing it past the run; @p out receives the steps, oldest first,
    // with Step = FirstStep + i and every field on the lattice. False (and @p in / @p out unspecified) when the
    // bytes are not a run: a zero or over-large count, a truncated field, an unknown mask bit, too many axes.
    [[nodiscard]] bool ReadInputRun( std::span<const std::byte>& in, std::vector<InputStep>& out );

    // One step WITH its number, full fields: U32 Step + the FULL layout. For lists that are not consecutive - the
    // mesh handover's pending steps (HandoverWire.cpp EncodeOffer: HandoverPawn::Pending, a map with holes while
    // a step is in flight). Carries Actions and Axes: the handed-over pawn keeps its held sprint / jump / swim axis.
    void               AppendInputStepFull( std::vector<std::byte>& out, const InputStep& step );
    [[nodiscard]] bool ReadInputStepFull( std::span<const std::byte>& in, InputStep& step );

    // What one run costs, for the sender's statistics line (ReplicationClient): the bytes AppendInputRun would
    // append for @p steps, 0 for a run it would refuse. No allocation.
    [[nodiscard]] std::size_t InputRunBytes( std::span<const InputStep> steps );
} // namespace Desert::Net
