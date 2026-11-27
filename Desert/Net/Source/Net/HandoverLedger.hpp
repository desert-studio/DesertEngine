#pragma once

// THE AUTHORITY LEDGER of the store (NET-HANDOVER-CONFIRM; NET-ARCH §2M.4): the ONE owner of every pawn's
// authority record (sim, epoch) and of every handover offer in flight. MeshStore (MeshLink.cpp) asks it before
// it accepts a sim's write or remove, hands it every handover message a sim sends and every sim it loses, and
// performs the Effects it returns. The ledger does no I/O and keeps no clock: a pure state machine over values
// (GoF State over an offer: Open -> Accepted -> Committed | Refused), so the store's wire code and this decision
// code are two units, and the decision can be read in one file.
//
// The rules it enforces (the two-phase commit of HandoverWire.hpp):
//   - a write is admitted only from the record's holder; the first write of a pawn creates its record at epoch 1;
//   - one open offer per pawn; an Offer is forwarded to the receiver only when the sender holds the record at
//     the epoch it names and the receiver is linked - otherwise Refused(reason) goes back to the sender at once;
//   - the record moves ONLY on the receiver's Accept of an offer that is still open: Commit to the receiver,
//     Committed to the sender, and Effects::Committed for the store to write the frozen state and notify the
//     gateways (AuthorityChanged, PawnWrite, proxies - the store's wire, not the ledger's);
//   - a Refuse, a Withdraw, a lost receiver, a lost sender, a removed pawn each end the open offer with a Refused
//     to the side that still holds something (the sender keeps its authority; the receiver drops its copy); a
//     sender lost AFTER the receiver accepted is a commit (the offered state is the best state of record);
//   - an Accept / Refuse / Withdraw for an offer the ledger does not hold is answered with Refused(OfferUnknown),
//     so a stale Accept after a withdrawal never moves a record - with ONE exception: an Accept of the offer that
//     last moved a pawn's record, from the sim that record names, is answered with that Commit again (nothing
//     moves). A receiver IN DOUBT (it accepted and heard no decision: a Commit or Refused lost) re-sends its vote
//     (kInDoubtWait); the coordinator's answer to every vote is the offer's outcome - the
//     participant-asks-the-coordinator rule of the two-phase commit.

#include <Net/HandoverWire.hpp>
#include <Net/MeshLink.hpp>
#include <Net/Replication.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Net::Handover
{
    struct Authority
    {
        std::uint32_t Sim   = 0;
        std::uint32_t Epoch = 0;
    };

    // One message the store must send: to one peer of @p Role (Index > 0) or to every peer of it (Index == 0).
    struct Send
    {
        EMeshRole              Role  = EMeshRole::Sim;
        std::uint32_t          Index = 0;
        std::vector<std::byte> Message;
    };
    // A record that moved: the store writes @p Pawn.State as the state of record, streams it to the gateways
    // with AuthorityChanged (To, Epoch, Step, Gateway, Player) and re-streams the proxies around the new owner.
    struct Committed
    {
        NetId                        Id      = kInvalidNetId;
        PlayerId                     Player  = 0;
        std::uint32_t                From    = 0;
        std::uint32_t                To      = 0;
        std::uint32_t                Epoch   = 0; // the new epoch
        std::uint32_t                Step    = 0; // the frozen step
        std::uint32_t                Gateway = 0;
        PawnSimulation::HandoverPawn Pawn;
    };
    struct Effects
    {
        std::vector<Send>        Sends;
        std::optional<Committed> Commit;
        std::vector<std::string> Log; // one line each, already worded for the store's log ("store: ...")
    };

    class Ledger
    {
    public:
        // @p simLinked: whether the store holds a mesh link to that sim right now (MeshEndpoint::PeerConnection).
        explicit Ledger( std::function<bool( std::uint32_t sim )> simLinked );

        // The record of a pawn; nullopt before its first write.
        [[nodiscard]] std::optional<Authority> Of( NetId id ) const;
        // A PawnWrite from @p sim: true when it holds the record (created at epoch 1 when there is none); false
        // when another sim holds it - the store refuses the write (and logs it once per (id, sim)). An admitted
        // write of the new holder forgets the commit that moved the record (its adoption is proven).
        [[nodiscard]] bool AdmitWrite( NetId id, std::uint32_t sim );
        // A PawnRemove from @p sim: the record is erased when @p sim holds it (true); an open offer of that pawn
        // ends with Refused(PawnRemoved) to both sides through @p effects.
        [[nodiscard]] bool Remove( NetId id, std::uint32_t sim, Effects& effects );

        // A handover message (HandoverWire.hpp IsHandoverMessage) from @p sim; the type byte is @p type, the rest
        // is @p body. Every outcome is in the returned Effects.
        [[nodiscard]] Effects OnSimMessage( std::uint32_t sim, std::uint8_t type, std::span<const std::byte> body );
        // A sim left the mesh: its open offers as sender or receiver end (see the file comment).
        [[nodiscard]] Effects OnSimLost( std::uint32_t sim );

        [[nodiscard]] std::size_t OpenOffers() const
        {
            return m_Offers.size();
        }
        // "records N, open offers N, unadopted commits N; committed N, refused N (<name> N, ...); withdrawn N" for
        // the store's stat and log (unadopted: committed, and no write of the new holder yet).
        [[nodiscard]] std::string Describe() const;

    private:
        enum class EPhase : std::uint8_t
        {
            Open,     // forwarded to the receiver, no verdict yet
            Accepted, // the receiver accepted: the record moved, Commit / Committed sent (kept until the store
                      // has performed the effects; erased at the end of the call that produced them)
        };
        struct Offer
        {
            OfferHeader                  Header;
            PawnSimulation::HandoverPawn Pawn;
            EPhase                       Phase = EPhase::Open;
        };
        [[nodiscard]] Effects Opened( std::uint32_t sim, std::span<const std::byte> body );
        [[nodiscard]] Effects Verdicted( std::uint32_t sim, EMessage type, std::span<const std::byte> body );
        void Refuse( Effects& effects, const Offer& offer, bool toSender, bool toReceiver, const Refusal& why );
        void Commit( Effects& effects, Offer offer, std::uint32_t atStep );

        std::function<bool( std::uint32_t )> m_SimLinked;
        std::map<NetId, Authority>           m_Records;
        std::map<OfferKey, Offer>            m_Offers;
        std::map<NetId, OfferKey>            m_OfferOfPawn; // one open offer per pawn
        // The offer that last moved each pawn's record, until the new holder's first admitted write proves the
        // adoption (or the record is removed, or a restarted sender reuses the key for another pawn), so the vote
        // of a receiver in doubt is answered with its Commit again: key -> (pawn, frozen step).
        struct CommittedOffer
        {
            NetId         Id   = kInvalidNetId;
            std::uint32_t Step = 0;
        };
        std::map<OfferKey, CommittedOffer> m_CommitOf;
        std::map<NetId, OfferKey>          m_CommitOfPawn;
        std::uint64_t                        m_CommittedCount = 0;
        std::map<ERefusal, std::uint64_t>    m_RefusedCount;
    };
} // namespace Desert::Net::Handover
