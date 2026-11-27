#pragma once

// THE CONFIRMED HANDOVER ON THE MESH (NET-HANDOVER-CONFIRM; NET-ARCH §2M.4 as amended by this feature).
//
// A pawn changes owner by a two-phase commit whose coordinator is the store (the state of record) and whose
// participants are the two sims:
//
//   sender  --Offer------>  store  --Offer------>  receiver     the sender HOLDS the pawn (not stepped, inputs
//                                                               queue) and keeps its authority
//   receiver --Accept----> store                                or --Refuse(reason)--> store
//   store   --Commit-----> receiver                             the record moves (sim, epoch) -> (receiver, epoch+1)
//   store   --Committed--> sender                               only now the sender releases the pawn
//   store   --AuthorityChanged (MeshLink.cpp EMesh)--> gateways
//   store   --Refused(reason)--> sender | receiver              every other end of an offer
//   sender  --Withdraw---> store                                after kOfferDeadlineSteps without an answer
//
// Nothing is dropped without a reason: every way an offer ends is an ERefusal carried on the wire to the side that
// still holds something, and counted in that role's Describe(). SpatialOS's AuthorityLossImminent -> ack ->
// Authoritative and the classic two-phase commit are the pattern; C++ Core Guidelines: errors are values.
//
// Wire: a message is its type byte then little-endian fields (WireCodec.hpp). The Encode* functions return the
// WHOLE message (type byte first); the Decode* functions take the BODY the mesh handler hands out (the type byte
// already consumed), as every MeshLink.cpp decoder does. ONE NUMBERING: a mesh connection carries the session
// codes (NetHandshake.hpp, < kFirstSessionMessage), the mesh's own EMesh codes (MeshLink.cpp, from
// kFirstSessionMessage + 40) and these, from kFirstHandoverMessage; MeshLink.cpp static_asserts that its last
// EMesh code stays below kFirstHandoverMessage, so no handover byte can be read as a mesh message or the reverse.

#include <Net/MeshOfOne.hpp>
#include <Net/NetHandshake.hpp>
#include <Net/Replication.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Net::Handover
{
    // The first handover type byte: past the mesh's EMesh range (MeshLink.cpp, kFirstSessionMessage + 40 ..
    // + 62 today), with room for the mesh to grow.
    inline constexpr std::uint8_t kFirstHandoverMessage = kFirstSessionMessage + 80;
    enum class EMessage : std::uint8_t
    {
        Offer     = kFirstHandoverMessage, // OfferHeader + HandoverPawn sender -> store -> receiver
        Accept,    // Verdict (no refusal)                 receiver -> store
        Refuse,    // Verdict + Refusal                    receiver -> store
        Commit,    // Decision                             store -> receiver
        Committed, // Decision                             store -> sender
        Withdraw,  // Verdict (no refusal)                 sender -> store
        Refused,   // Decision + Refusal                   store -> sender | receiver
    };
    [[nodiscard]] constexpr bool IsHandoverMessage( std::uint8_t type )
    {
        return type >= static_cast<std::uint8_t>( EMessage::Offer ) &&
               type <= static_cast<std::uint8_t>( EMessage::Refused );
    }
    [[nodiscard]] std::string_view MessageName( EMessage type );

    // The sender waits this many mesh steps (60 Hz) for Committed or Refused before it withdraws: 0.5 s, more
    // than ten store round trips inside one datacentre and well under the client's input ring
    // (Replication.hpp kMaxUnackedInputSteps = 128 steps), so a held pawn loses no input.
    inline constexpr std::uint32_t kOfferDeadlineSteps = 30;
    // After a refusal or a withdrawal the sender keeps the pawn and does not offer it again for this many steps
    // (2 s); a new authority map from the coordinator ends the backoff early.
    inline constexpr std::uint32_t kRetryBackoffSteps = 120;
    // The receiver refuses an offer frozen more than this many steps behind its own step (2 s): catching up
    // further would be a whole second simulated in one step.
    inline constexpr std::uint32_t kMaxCatchUpSteps = 120;

    // Why an offer did not end in a commit. The side that reads it still holds the pawn (the sender) or holds a
    // copy it must drop (the receiver). Detail carries the numbers (sims, steps, epochs) for the log.
    enum class ERefusal : std::uint8_t
    {
        NotHolder,        // store: the record names another sim as the holder
        StaleEpoch,       // store: the sender's epoch is not the record's
        NoRecord,         // store: the pawn has no authority record yet (its first write has not arrived)
        ReceiverUnlinked, // store: no mesh link to the receiver sim
        ReceiverLost,     // store: the receiver left the mesh while the offer was open
        SenderLost,       // store -> receiver: the sender left the mesh before the receiver accepted
        Withdrawn,        // store -> receiver: the sender withdrew before the Accept arrived
        OfferUnknown,     // store: an Accept / Refuse / Withdraw for an offer it does not hold
        PawnRemoved,      // store: the pawn was removed (player left) while the offer was open
        ReceiverSilent,   // sender: no Committed / Refused within kOfferDeadlineSteps - it withdrew
        RegionNotMine,    // receiver: its authority map does not give it the region at the pawn's position
        NoCell,           // receiver: no cell of the world grid at the pawn's position
        CatchUpTooLarge,  // receiver: the offer's step is more than kMaxCatchUpSteps behind its own
        AlreadyHeld,      // receiver: it already holds a pawn (owned or incoming) for that player
        Malformed,        // the record did not decode
        WireStep,         // a pending input step the wire does not carry (InputWire.hpp)
    };
    [[nodiscard]] std::string_view RefusalName( ERefusal code );

    struct Refusal
    {
        ERefusal    Code = ERefusal::Malformed;
        std::string Detail; // never empty: the numbers the operator needs ("sim 2 answered nothing for 30 steps")
    };
    // "<name>: <detail>" for a log line.
    [[nodiscard]] std::string Describe( const Refusal& refusal );

    // An offer is named by the sender and its serial: unique across the mesh, and a stale Accept of an earlier
    // offer of the same pawn never matches a later one.
    using OfferSerial = std::uint32_t;
    struct OfferKey
    {
        std::uint32_t From   = 0; // the sender sim
        OfferSerial   Serial = 0;
        [[nodiscard]] auto operator<=>( const OfferKey& ) const = default;
    };

    // Who hands which pawn to whom, at which mesh step, under which epoch; the gateway the player is on.
    struct OfferHeader
    {
        OfferKey      Key;
        std::uint32_t Epoch   = 0; // the sender's epoch of the pawn (the record must agree)
        std::uint32_t To      = 0; // the receiver sim
        std::uint32_t Step    = 0; // the mesh step the pawn is frozen at
        std::uint32_t Gateway = 0;
    };
    // Accept / Refuse (receiver -> store) and Withdraw (sender -> store): who speaks about which offer, at its step.
    struct Verdict
    {
        OfferKey      Key;
        std::uint32_t Sim  = 0; // the speaker
        std::uint32_t Step = 0; // the speaker's mesh step
    };
    // Commit / Committed / Refused (store -> sims): the store's decision on an offer.
    struct Decision
    {
        OfferKey      Key;
        NetId         Id    = kInvalidNetId;
        std::uint32_t Epoch = 0; // the new epoch on Commit / Committed; the record's epoch on Refused
        std::uint32_t Step  = 0; // the offer's frozen step
    };

    // Empty when a pending input step is not one the wire carries (the caller refuses with ERefusal::WireStep
    // BEFORE it holds anything).
    [[nodiscard]] std::vector<std::byte> EncodeOffer( const OfferHeader& header,
                                                      const PawnSimulation::HandoverPawn& pawn );
    [[nodiscard]] bool DecodeOffer( std::span<const std::byte> body, OfferHeader& header,
                                    PawnSimulation::HandoverPawn& pawn );

    [[nodiscard]] std::vector<std::byte> EncodeAccept( const Verdict& verdict );
    [[nodiscard]] std::vector<std::byte> EncodeRefuse( const Verdict& verdict, const Refusal& refusal );
    [[nodiscard]] std::vector<std::byte> EncodeWithdraw( const Verdict& verdict );
    // Accept and Withdraw bodies; a Refuse body when @p refusal is given.
    [[nodiscard]] bool DecodeVerdict( std::span<const std::byte> body, Verdict& verdict,
                                      Refusal* refusal = nullptr );

    [[nodiscard]] std::vector<std::byte> EncodeCommit( const Decision& decision );
    [[nodiscard]] std::vector<std::byte> EncodeCommitted( const Decision& decision );
    [[nodiscard]] std::vector<std::byte> EncodeRefused( const Decision& decision, const Refusal& refusal );
    // Commit and Committed bodies; a Refused body when @p refusal is given.
    [[nodiscard]] bool DecodeDecision( std::span<const std::byte> body, Decision& decision,
                                       Refusal* refusal = nullptr );
} // namespace Desert::Net::Handover
