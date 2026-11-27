#pragma once

// THE AUTHORITY LEDGER of the store (NET-HANDOVER-CONFIRM, NET-HANDOVER-REFUSE; NET-ARCH §2M.4): the ONE owner of
// every pawn's authority record (sim, epoch) and of every handover offer in flight. MeshStore (MeshLink.cpp) asks
// it before it accepts a sim's write or remove, hands it every handover message a sim sends and every sim it
// loses, and performs the Effects it returns. The ledger does no I/O and keeps no clock: a pure function of its
// maps and the message, so the store's wire code and this decision code are two units, and the decision can be
// read in one file.
//
// AN OFFER'S STATE IS THE MAP IT LIES IN: in m_Offers it is open (forwarded, no vote yet); its commit row in
// m_CommitRows (keyed by the PAWN) says it committed and the new holder has not written yet; in neither it ended.
// Nothing else names a phase.
//
// The rules it enforces (the two-phase commit of HandoverWire.hpp):
//   0. a write is admitted only from the record's holder (the first write creates the record at epoch 1);
//   1. an Offer is checked in this order and refused to the sender under (sender, #0) - a key no sender issues, so
//      the refusal can resume nothing the sender holds open - when it did not decode (Malformed, the byte named),
//      names another sender or no receiver (Malformed), names a pawn that already has an offer open
//      (AlreadyOffered) or a key open for another pawn (SerialReused); then under its own key when the record
//      refuses it (NoRecord, NotHolder, StaleEpoch) or the receiver is not linked; otherwise it is forwarded;
//   2. a vote NAMES ITS PAWN (Verdict::Id). On an open offer: a speaker in the wrong role is refused OfferUnknown,
//      an Accept or Withdraw naming another pawn PawnMismatch - both under (speaker, #0), the offer untouched (an
//      Accept naming another pawn is first answered from that pawn's commit row, below, when it has one); an
//      Accept commits (the ONE place a record moves: Commit to the receiver, Committed to the sender,
//      Effects::Committed for the store's wire), a Refuse or Withdraw ends the offer with Refused to the side that
//      still holds something. On an offer not open, the vote is answered with the OUTCOME of the pawn it names: an
//      Accept from the sim the record names, under the key of the pawn's commit row, gets that Commit again; a
//      Withdraw from the sender the record no longer names gets Committed again; anything else Refused
//      (OfferUnknown) under its key, so a receiver in doubt drops its copy. Nothing moves on an answer. A vote
//      that did not decode is refused Malformed under (speaker, #0);
//   3. a message only the store sends (Commit, Committed, Refused) coming FROM a sim is refused WrongDirection
//      under (speaker, #0);
//   - a lost receiver, a lost sender, a removed pawn each end the open offer with a Refused to the side that still
//     holds something; a sender lost AFTER the receiver accepted is a commit already (an Accept commits within the
//     call that carries it);
//   - a commit row lives until the new holder's first admitted write proves the adoption (2PC: the coordinator
//     keeps a decision until the participant acknowledges it), or the record is removed. A later offer reusing the
//     key for another pawn does not touch it: the row is found by the pawn a vote names, then its key compared.

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
        struct Offer
        {
            OfferHeader                  Header;
            PawnSimulation::HandoverPawn Pawn;
        };
        // The offer that last moved a pawn's record (its key, the step it was frozen at), kept until the new
        // holder's first admitted write proves the adoption, so a vote in doubt is answered with its outcome.
        struct CommitRow
        {
            OfferKey      Key;
            std::uint32_t Step = 0;
        };
        [[nodiscard]] Effects Opened( std::uint32_t sim, std::span<const std::byte> body );
        [[nodiscard]] Effects Verdicted( std::uint32_t sim, EMessage type, std::span<const std::byte> body );
        // A vote matched to no open offer: Commit / Committed again when the commit row of the pawn it names
        // says so (rule 2); false when nothing answers it but Refused(OfferUnknown).
        [[nodiscard]] bool AnsweredFromRow( Effects& effects, std::uint32_t sim, EMessage type,
                                            const Verdict& verdict );
        void Refuse( Effects& effects, const Offer& offer, bool toSender, bool toReceiver, const Refusal& why );
        // Refused to the sim that spoke, under @p key (a key it holds nothing under unless it is the offer's own),
        // counted; the log line is the caller's.
        void RefuseSpeaker( Effects& effects, std::uint32_t sim, const OfferKey& key, std::uint32_t step,
                            const Refusal& why );
        void Commit( Effects& effects, Offer offer, std::uint32_t atStep );

        std::function<bool( std::uint32_t )> m_SimLinked;
        std::map<NetId, Authority>           m_Records;
        std::map<OfferKey, Offer>            m_Offers;
        std::map<NetId, OfferKey>            m_OfferOfPawn; // one open offer per pawn
        std::map<NetId, CommitRow>           m_CommitRows;  // committed, not yet adopted: one row per pawn
        std::uint64_t                        m_CommittedCount = 0;
        std::map<ERefusal, std::uint64_t>    m_RefusedCount;
    };
} // namespace Desert::Net::Handover
