#pragma once

// THE CONFIRMED HANDOVER ON THE MESH (NET-HANDOVER-CONFIRM; NET-ARCH §2M.4 as amended by this feature and by
// NET-HANDOVER-REFUSE).
//
// A pawn changes owner by a two-phase commit whose coordinator is the store (the state of record) and whose
// participants are the two sims:
//
// clang-format off
//   sender  --Offer------>  store  --Offer------>  receiver     the sender HOLDS the pawn (not stepped, inputs
//                                                               queue) and keeps its authority
//   receiver --Accept----> store                                or --Refuse(reason)--> store
//   store   --Commit-----> receiver                             the record moves (sim, epoch) -> (receiver, epoch+1)
//   store   --Committed--> sender                               only now the sender releases the pawn
//   store   --AuthorityChanged (MeshLink.cpp EMesh)--> gateways
//   store   --Refused(reason)--> sender | receiver              every other end of an offer
//   sender  --Withdraw---> store                                after kOfferDeadline without an answer
// clang-format on
//
// Nothing is dropped without a reason: every way an offer ends is an ERefusal carried on the wire to the side that
// still holds something, and counted in that role's Describe(). SpatialOS's AuthorityLossImminent -> ack ->
// Authoritative and the classic two-phase commit are the pattern; C++ Core Guidelines: errors are values.
//
// A VOTE NAMES ITS SUBJECT (NET-HANDOVER-REFUSE): Accept, Refuse and Withdraw carry the pawn's NetId beside the
// offer key. An offer key alone is not an identity on a mesh whose sims restart (a restarted sender numbers its
// offers from 1 again) or are impersonated (DesertNetProbe --mesh-peer): the store moves a record only on a vote
// whose key AND pawn are the open offer's, and answers a vote it does not hold open with the OUTCOME of the pawn
// it names - the Commit again to the receiver the record names, the Committed again to the sender the record
// left, Refused(OfferUnknown) otherwise. The participant-asks-the-coordinator rule of the two-phase commit, in
// both directions.
//
// Wire: a message is its type byte then little-endian fields (WireCodec.hpp). The Encode* functions return the
// WHOLE message (type byte first); the Decode* functions take the BODY the mesh handler hands out (the type byte
// already consumed), as every MeshLink.cpp decoder does, and return WHERE a read ended (Decoded): a refusal of a
// record that did not decode names the byte, never just "malformed". ONE NUMBERING: a mesh connection carries the
// session codes (NetHandshake.hpp, < kFirstSessionMessage), the mesh's own EMesh codes (MeshLink.cpp, from
// kFirstSessionMessage + 40) and these, from kFirstHandoverMessage; MeshLink.cpp static_asserts that its last
// EMesh code stays below kFirstHandoverMessage, so no handover byte can be read as a mesh message or the reverse.
// The wire is not serialized data: every role is built from one tree, so a field added here needs no version.

#include <Net/MeshOfOne.hpp>
#include <Net/NetHandshake.hpp>
#include <Net/Replication.hpp>

#include <chrono>
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
    // Accept, Refuse, Withdraw: the messages a sim sends the store; the others only the store sends.
    [[nodiscard]] constexpr bool IsSimMessage( EMessage type )
    {
        return type == EMessage::Accept || type == EMessage::Refuse || type == EMessage::Withdraw;
    }
    [[nodiscard]] std::string_view MessageName( EMessage type );

    // The sender waits this long for Committed or Refused before it withdraws: 0.5 s, more than ten store round
    // trips inside one datacentre and well under the client's input ring (Replication.hpp kMaxUnackedInputSteps =
    // 128 steps at 60 Hz), so a held pawn loses no input. WALL TIME on NetClock, not mesh steps, as a two-phase-
    // commit participant's timeout is: after a stall the sim runs the ticks that queued in one burst, and a
    // deadline counted in steps would expire in a few milliseconds and withdraw an offer whose answer is already
    // queued.
    inline constexpr std::chrono::milliseconds kOfferDeadline{ 500 };
    // A receiver that accepted and heard no decision (Commit / Refused) for this long is IN DOUBT and re-sends
    // its Accept; the store answers with the offer's outcome (HandoverLedger.hpp). Twice the sender's deadline,
    // so the sender's own Withdraw has been decided by then; then again as often while no answer comes. Wall
    // time on NetClock, for the same reason as kOfferDeadline.
    inline constexpr std::chrono::milliseconds kInDoubtWait = 2 * kOfferDeadline;
    // After a refusal or a withdrawal the sender keeps the pawn and does not offer it again for this many steps
    // (2 s); a new authority map from the coordinator ends the backoff early.
    inline constexpr std::uint32_t kRetryBackoffSteps = 120;
    // The receiver refuses an offer frozen more than this many steps behind its own step (2 s): catching up
    // further would be a whole second simulated in one step.
    inline constexpr std::uint32_t kMaxCatchUpSteps = 120;

    // Why an offer did not end in a commit. The side that reads it still holds the pawn (the sender) or holds a
    // copy it must drop (the receiver). Detail carries the numbers (sims, steps, epochs, bytes) for the log.
    // Malformed is ONLY a record that did not decode; a record that decoded and breaks a rule has that rule's
    // code, so the store's count tells a corrupt wire from a defective sender.
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
        ReceiverSilent,   // sender: no Committed / Refused within kOfferDeadline - it withdrew
        RegionNotMine,    // receiver: its authority map does not give it the region at the pawn's position
        NoCell,           // receiver: no cell of the world grid at the pawn's position
        CatchUpTooLarge,  // receiver: the offer's step is more than kMaxCatchUpSteps behind its own
        AlreadyHeld,      // receiver: it already holds a pawn (owned or incoming) for that player
        Malformed,        // the record did not decode (Detail names the byte: "cut at byte N of M")
        WireStep,         // a pending input step the wire does not carry (InputWire.hpp)
        AlreadyOffered,   // store: the pawn already has an offer open (a second Offer of a held pawn)
        SerialReused,     // store: the offer key is open for ANOTHER pawn (the sender reuses a serial)
        PawnMismatch,     // store: the vote names a pawn other than the open offer's
        WrongDirection,   // store: a sim sent a message only the store sends (Commit, Committed, Refused)
    };
    // The last code the wire accepts; ReadRefusal refuses a byte past it as malformed, never guesses.
    inline constexpr ERefusal kLastRefusal = ERefusal::WrongDirection;
    [[nodiscard]] std::string_view RefusalName( ERefusal code );

    struct Refusal
    {
        ERefusal    Code = ERefusal::Malformed;
        std::string Detail; // never empty: the numbers the operator needs ("sim 2 answered nothing for 30 steps")
    };
    // "<name>: <detail>" for a log line.
    [[nodiscard]] std::string Describe( const Refusal& refusal );

    // An offer is named by the sender and its serial: unique across the mesh while the sender lives, and a stale
    // Accept of an earlier offer of the same pawn never matches a later one. Serial 0 is never issued: a refusal
    // of a message the store could not match to an open offer of the speaker goes back under (speaker, #0), so it
    // can match nothing the speaker holds.
    using OfferSerial = std::uint32_t;
    inline constexpr OfferSerial kNoSerial = 0;
    struct OfferKey
    {
        std::uint32_t From   = 0; // the sender sim
        OfferSerial   Serial = kNoSerial;
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
    // Accept / Refuse (receiver -> store) and Withdraw (sender -> store): who speaks about which offer OF WHICH
    // PAWN, at its step. Id is the pawn the speaker holds (incoming or held) under that key; kInvalidNetId only
    // on a Refuse(Malformed) of an Offer whose pawn record did not decode past the header - the store matches a
    // Refuse by key and speaker alone (it moves nothing), an Accept or Withdraw by key, speaker AND pawn.
    struct Verdict
    {
        OfferKey      Key;
        NetId         Id   = kInvalidNetId;
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

    // Where a read ended. Ok: the whole body read, nothing left over. Otherwise At is the byte the read stopped at
    // (a field that did not fit, a value past its range, or Size when bytes were left over).
    struct Decoded
    {
        bool        Ok   = false;
        std::size_t At   = 0;
        std::size_t Size = 0;
        [[nodiscard]] explicit operator bool() const
        {
            return Ok;
        }
    };
    // An Offer read: Header is true when OfferHeader (key, epoch, to, step, gateway: 24 bytes) was read even if
    // the pawn record after it was not - the receiver then has the key to Refuse under.
    struct DecodedOffer : Decoded
    {
        bool Header = false;
    };
    // "cut at byte N of M" / "N byte(s) left over after M read" for a Refusal's Detail; never called on an Ok read.
    [[nodiscard]] std::string DescribeCut( const Decoded& read );

    // Empty when a pending input step is not one the wire carries (the caller refuses with ERefusal::WireStep
    // BEFORE it holds anything).
    [[nodiscard]] std::vector<std::byte> EncodeOffer( const OfferHeader& header,
                                                      const PawnSimulation::HandoverPawn& pawn );
    [[nodiscard]] DecodedOffer DecodeOffer( std::span<const std::byte> body, OfferHeader& header,
                                            PawnSimulation::HandoverPawn& pawn );

    [[nodiscard]] std::vector<std::byte> EncodeAccept( const Verdict& verdict );
    [[nodiscard]] std::vector<std::byte> EncodeRefuse( const Verdict& verdict, const Refusal& refusal );
    [[nodiscard]] std::vector<std::byte> EncodeWithdraw( const Verdict& verdict );
    // Accept and Withdraw bodies; a Refuse body when @p refusal is given.
    [[nodiscard]] Decoded DecodeVerdict( std::span<const std::byte> body, Verdict& verdict,
                                         Refusal* refusal = nullptr );

    [[nodiscard]] std::vector<std::byte> EncodeCommit( const Decision& decision );
    [[nodiscard]] std::vector<std::byte> EncodeCommitted( const Decision& decision );
    [[nodiscard]] std::vector<std::byte> EncodeRefused( const Decision& decision, const Refusal& refusal );
    // Commit and Committed bodies; a Refused body when @p refusal is given.
    [[nodiscard]] Decoded DecodeDecision( std::span<const std::byte> body, Decision& decision,
                                          Refusal* refusal = nullptr );
} // namespace Desert::Net::Handover
