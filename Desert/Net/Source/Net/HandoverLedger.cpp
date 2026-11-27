#include "Net/HandoverLedger.hpp"

#include <format>
#include <utility>

namespace Desert::Net::Handover
{
    namespace
    {
        void SendToSim( Effects& effects, std::uint32_t sim, std::vector<std::byte> message )
        {
            effects.Sends.push_back( Send{ EMeshRole::Sim, sim, std::move( message ) } );
        }

        std::string NameOf( const OfferKey& key )
        {
            return std::format( "offer (sim {}, #{})", key.From, key.Serial );
        }

        // The verdict's word in a store log line ("accept from sim 2 refused - ...").
        std::string_view VerbOf( EMessage type )
        {
            switch ( type )
            {
                case EMessage::Accept:
                    return "accept";
                case EMessage::Refuse:
                    return "refuse";
                case EMessage::Withdraw:
                    return "withdraw";
                default:
                    return MessageName( type );
            }
        }
    } // namespace

    Ledger::Ledger( std::function<bool( std::uint32_t sim )> simLinked ) : m_SimLinked( std::move( simLinked ) )
    {
    }

    std::optional<Authority> Ledger::Of( NetId id ) const
    {
        if ( const auto record = m_Records.find( id ); record != m_Records.end() )
            return record->second;
        return std::nullopt;
    }

    bool Ledger::AdmitWrite( NetId id, std::uint32_t sim )
    {
        const auto [record, created] = m_Records.emplace( id, Authority{ sim, 1 } );
        if ( !created && record->second.Sim != sim )
            return false;
        // THE ADOPTION IS PROVEN by the new holder's first admitted write: no vote of it can be in doubt any more,
        // so the commit it would have been answered with is forgotten (2PC: the coordinator keeps a decision until
        // the participant acknowledges it).
        m_CommitRows.erase( id );
        return true;
    }

    bool Ledger::Remove( NetId id, std::uint32_t sim, Effects& effects )
    {
        const auto record = m_Records.find( id );
        if ( record != m_Records.end() && record->second.Sim != sim )
            return false;
        if ( const auto open = m_OfferOfPawn.find( id ); open != m_OfferOfPawn.end() )
        {
            const auto offer = m_Offers.find( open->second );
            effects.Log.push_back(
                 std::format( "store: {} of pawn net id {} to sim {} ended - the pawn was removed "
                              "by sim {}; both sides told",
                              NameOf( offer->first ), id, offer->second.Header.To, sim ) );
            Refuse( effects, offer->second, true, true,
                    Refusal{ ERefusal::PawnRemoved,
                             std::format( "sim {} removed pawn net id {} (its player left) with {} open", sim, id,
                                          NameOf( offer->first ) ) } );
            m_Offers.erase( offer );
            m_OfferOfPawn.erase( open );
        }
        if ( record != m_Records.end() )
            m_Records.erase( record );
        m_CommitRows.erase( id );
        return true;
    }

    Effects Ledger::OnSimMessage( std::uint32_t sim, std::uint8_t type, std::span<const std::byte> body )
    {
        const auto message = static_cast<EMessage>( type );
        if ( message == EMessage::Offer )
            return Opened( sim, body );
        if ( IsSimMessage( message ) )
            return Verdicted( sim, message, body );
        // A message only the store sends (Commit, Committed, Refused), sent BY a sim: answered, never dropped
        // (MeshLink.cpp hands the ledger only IsHandoverMessage types).
        Effects       effects;
        const Refusal why{ ERefusal::WrongDirection,
                           std::format( "sim {} sent a {} ({} bytes), which only the store sends", sim,
                                        MessageName( message ), body.size() ) };
        effects.Log.push_back( std::format( "store: {} from sim {} refused - {}", MessageName( message ), sim,
                                            Handover::Describe( why ) ) );
        RefuseSpeaker( effects, sim, OfferKey{ sim, kNoSerial }, 0, why );
        return effects;
    }

    Effects Ledger::Opened( std::uint32_t sim, std::span<const std::byte> body )
    {
        Effects                effects;
        Offer                  offer;
        const DecodedOffer     read       = DecodeOffer( body, offer.Header, offer.Pawn );
        OfferHeader&           h          = offer.Header;
        const NetId            id         = offer.Pawn.State.Id;
        const auto             record     = m_Records.find( id );
        const auto             openOfPawn = m_OfferOfPawn.find( id );
        const auto             openOfKey  = m_Offers.find( h.Key );
        std::optional<Refusal> refusal;
        // Rule 1, in this order; each of the first four is a defect of the record or of its sender, refused under
        // (sim, #0): the header's key could name an offer the speaker - or the sim the header names - holds open,
        // and a Refused under it would resume that pawn while its offer may still commit (two owners).
        if ( !read )
            refusal =
                 Refusal{ ERefusal::Malformed, std::format( "the offer record from sim {} did not decode - {}",
                                                            sim, DescribeCut( read ) ) };
        else if ( h.Key.From != sim || h.Key.Serial == kNoSerial || h.To == sim || h.To == 0 )
            refusal = Refusal{ ERefusal::Malformed,
                               std::format( "the offer (sim {}, #{}) names receiver sim {} but came from sim {}",
                                            h.Key.From, h.Key.Serial, h.To, sim ) };
        else if ( openOfPawn != m_OfferOfPawn.end() )
            // The sender holds the pawn while its offer is open, so a second offer is a sender defect, not data.
            refusal = Refusal{ ERefusal::AlreadyOffered, std::format( "pawn net id {} already has {} open", id,
                                                                      NameOf( openOfPawn->second ) ) };
        else if ( openOfKey != m_Offers.end() )
            refusal = Refusal{ ERefusal::SerialReused,
                               std::format( "{} is open for pawn net id {}, not {}", NameOf( h.Key ),
                                            openOfKey->second.Pawn.State.Id, id ) };
        if ( refusal )
            h.Key = OfferKey{ sim, kNoSerial };
        else if ( record == m_Records.end() )
            refusal = Refusal{ ERefusal::NoRecord,
                               std::format( "pawn net id {} has no authority record yet (its first write has not "
                                            "arrived)",
                                            id ) };
        else if ( record->second.Sim != sim )
            refusal = Refusal{ ERefusal::NotHolder, std::format( "the record is (sim {}, epoch {})",
                                                                 record->second.Sim, record->second.Epoch ) };
        else if ( record->second.Epoch != h.Epoch )
            refusal = Refusal{ ERefusal::StaleEpoch,
                               std::format( "the offer names epoch {}, the record is (sim {}, epoch {})", h.Epoch,
                                            record->second.Sim, record->second.Epoch ) };
        else if ( !m_SimLinked( h.To ) )
            refusal = Refusal{ ERefusal::ReceiverUnlinked, std::format( "no mesh link to sim {}", h.To ) };

        if ( refusal )
        {
            effects.Log.push_back( std::format( "store: {} of pawn net id {} to sim {} at step {} refused - {}",
                                                NameOf( h.Key ), id, h.To, h.Step,
                                                Handover::Describe( *refusal ) ) );
            Refuse( effects, offer, true, false, *refusal );
            return effects;
        }

        // The receiver gets the sender's bytes as they came: nothing is re-encoded, so nothing can fail here.
        std::vector<std::byte> forward;
        forward.reserve( body.size() + 1 );
        forward.push_back( static_cast<std::byte>( EMessage::Offer ) );
        forward.insert( forward.end(), body.begin(), body.end() );
        SendToSim( effects, h.To, std::move( forward ) );
        effects.Log.push_back( std::format( "store: {} of pawn net id {} (player {} of gateway {}) at step {} "
                                            "forwarded to sim {} - record (sim {}, epoch {}), {} unacked input(s)",
                                            NameOf( h.Key ), id, offer.Pawn.Player, h.Gateway, h.Step, h.To, sim,
                                            h.Epoch, offer.Pawn.Pending.size() ) );
        // A commit row of this pawn (a vote of its last move still in doubt) stays: it is the pawn's, not the
        // key's, and the new offer's own Accept replaces it.
        const OfferKey key = h.Key;
        m_OfferOfPawn.emplace( id, key );
        m_Offers.emplace( key, std::move( offer ) );
        return effects;
    }

    Effects Ledger::Verdicted( std::uint32_t sim, EMessage type, std::span<const std::byte> body )
    {
        Effects       effects;
        Verdict       verdict;
        Refusal       receiverWhy;
        const Decoded read  = DecodeVerdict( body, verdict, type == EMessage::Refuse ? &receiverWhy : nullptr );
        const auto    offer = read ? m_Offers.find( verdict.Key ) : m_Offers.end();
        // Every refusal of a vote goes to the speaker only, and moves nothing. Under (sim, #0) - a key that names
        // no offer the speaker holds - unless the vote is about an offer that is not open and names its speaker
        // truly: then under the vote's own key, so a receiver in doubt about an offer that ended without moving
        // the record to it drops its copy.
        std::optional<Refusal> refusal;
        OfferKey               under{ sim, kNoSerial };
        if ( !read )
            refusal = Refusal{ ERefusal::Malformed, std::format( "the {} record from sim {} did not decode - {}",
                                                                 VerbOf( type ), sim, DescribeCut( read ) ) };
        else if ( offer == m_Offers.end() )
        {
            // The answer to a vote about an offer that is not open is the outcome of the pawn it names.
            if ( AnsweredFromRow( effects, sim, type, verdict ) )
                return effects;
            refusal = Refusal{ ERefusal::OfferUnknown,
                               std::format( "{} is not open at the store (withdrawn, refused or committed before)",
                                            NameOf( verdict.Key ) ) };
            if ( verdict.Sim == sim )
                under = verdict.Key;
        }
        else
        {
            const OfferHeader& open   = offer->second.Header;
            const NetId        openId = offer->second.Pawn.State.Id;
            // Accept and Refuse come from the offer's receiver, Withdraw from its sender.
            const std::uint32_t role = type == EMessage::Withdraw ? open.Key.From : open.To;
            // An Accept of ANOTHER pawn under an open key: a receiver in doubt about the pawn a restarted sender's
            // earlier offer under this key committed to it - answered with that pawn's outcome, if it has one.
            if ( type == EMessage::Accept && verdict.Id != openId &&
                 AnsweredFromRow( effects, sim, type, verdict ) )
                return effects;
            if ( verdict.Sim != sim || role != sim )
                refusal = Refusal{ ERefusal::OfferUnknown,
                                   std::format( "sim {} (naming sim {}) is not the {} of {}", sim, verdict.Sim,
                                                type == EMessage::Withdraw ? "sender" : "receiver",
                                                NameOf( verdict.Key ) ) };
            // A Refuse moves nothing and may name no pawn (an Offer whose pawn record did not decode); an Accept
            // or a Withdraw moves or ends the offer, so it must be about the pawn the offer is of.
            else if ( type != EMessage::Refuse && verdict.Id != openId )
                refusal =
                     Refusal{ ERefusal::PawnMismatch, std::format( "{} is of pawn net id {}, the vote names {}",
                                                                   NameOf( verdict.Key ), openId, verdict.Id ) };
        }
        if ( refusal )
        {
            effects.Log.push_back( std::format( "store: {} from sim {} refused - {}", VerbOf( type ), sim,
                                                Handover::Describe( *refusal ) ) );
            RefuseSpeaker( effects, sim, under, read ? verdict.Step : 0, *refusal );
            return effects;
        }

        Offer ended = std::move( offer->second );
        m_Offers.erase( offer );
        m_OfferOfPawn.erase( ended.Pawn.State.Id );
        const OfferHeader& h  = ended.Header;
        const NetId        id = ended.Pawn.State.Id;
        switch ( type )
        {
            case EMessage::Accept:
                Commit( effects, std::move( ended ), verdict.Step );
                break;
            case EMessage::Refuse:
                effects.Log.push_back( std::format(
                     "store: {} of pawn net id {} refused by sim {} at its step {} - "
                     "{}; sim {} keeps the pawn",
                     NameOf( h.Key ), id, sim, verdict.Step, Handover::Describe( receiverWhy ), h.Key.From ) );
                Refuse( effects, ended, true, false, receiverWhy );
                break;
            default: // Withdraw
                effects.Log.push_back( std::format( "store: {} of pawn net id {} withdrawn by sim {} at step {} - "
                                                    "the record stays (sim {}, epoch {}); sim {} drops its copy, "
                                                    "the sender resumes",
                                                    NameOf( h.Key ), id, sim, verdict.Step, h.Key.From, h.Epoch,
                                                    h.To ) );
                Refuse( effects, ended, true, true,
                        Refusal{ ERefusal::Withdrawn, std::format( "sim {} withdrew {} at its step {}", sim,
                                                                   NameOf( h.Key ), verdict.Step ) } );
                break;
        }
        return effects;
    }

    bool Ledger::AnsweredFromRow( Effects& effects, std::uint32_t sim, EMessage type, const Verdict& verdict )
    {
        if ( verdict.Sim != sim )
            return false;
        const auto record = m_Records.find( verdict.Id );
        if ( record == m_Records.end() )
            return false;
        const auto row              = m_CommitRows.find( verdict.Id );
        const bool rowOfKey         = row != m_CommitRows.end() && row->second.Key == verdict.Key;
        const auto& [holder, epoch] = record->second;
        if ( type == EMessage::Accept && rowOfKey && holder == sim )
        {
            // THE VOTE OF A RECEIVER IN DOUBT (its Commit lost): the offer that moved the record to it is
            // answered with that Commit again.
            SendToSim( effects, sim,
                       EncodeCommit( Decision{ verdict.Key, verdict.Id, epoch, row->second.Step } ) );
            effects.Log.push_back( std::format(
                 "store: accept from sim {} at its step {} of {}, which committed pawn net id {} to it (sim {}, "
                 "epoch {}) before - Commit sent again, nothing moves",
                 sim, verdict.Step, NameOf( verdict.Key ), verdict.Id, sim, epoch ) );
            return true;
        }
        if ( type == EMessage::Withdraw && verdict.Key.From == sim && holder != sim )
        {
            // THE WITHDRAW OF A SENDER IN DOUBT (its Committed lost): the record no longer names it, so the
            // outcome it waits for is Committed - a Refused would resume a pawn whose record moved (a ghost whose
            // writes AdmitWrite refuses while its new holder lives). Without the row (the new holder has written
            // since) the record alone says it: the step is then the speaker's own.
            const std::uint32_t step = rowOfKey ? row->second.Step : verdict.Step;
            SendToSim( effects, sim, EncodeCommitted( Decision{ verdict.Key, verdict.Id, epoch, step } ) );
            effects.Log.push_back( std::format(
                 "store: withdraw from sim {} at its step {} of {}, which {} pawn net id {} away from it (the "
                 "record is sim {}, epoch {}) - Committed sent again, nothing moves",
                 sim, verdict.Step, NameOf( verdict.Key ), rowOfKey ? "committed" : "left with", verdict.Id,
                 holder, epoch ) );
            return true;
        }
        return false;
    }

    Effects Ledger::OnSimLost( std::uint32_t sim )
    {
        // An offer in m_Offers is open: an Accept commits within the call that carries it, so a sender lost after
        // its receiver accepted finds the record already moved (the "accepted + sender lost = commit" rule).
        Effects effects;
        for ( auto offer = m_Offers.begin(); offer != m_Offers.end(); )
        {
            const OfferHeader& h  = offer->second.Header;
            const NetId        id = offer->second.Pawn.State.Id;
            if ( h.Key.From == sim )
            {
                effects.Log.push_back(
                     std::format( "store: {} of pawn net id {} ended - its sender sim {} left the "
                                  "mesh; sim {} drops its copy, the record stays (sim {}, epoch "
                                  "{})",
                                  NameOf( h.Key ), id, sim, h.To, h.Key.From, h.Epoch ) );
                Refuse( effects, offer->second, false, true,
                        Refusal{ ERefusal::SenderLost,
                                 std::format( "sim {} left the mesh before sim {} accepted", sim, h.To ) } );
            }
            else if ( h.To == sim )
            {
                effects.Log.push_back( std::format( "store: {} of pawn net id {} ended - its receiver sim {} left "
                                                    "the mesh; sim {} keeps the pawn",
                                                    NameOf( h.Key ), id, sim, h.Key.From ) );
                Refuse( effects, offer->second, true, false,
                        Refusal{ ERefusal::ReceiverLost,
                                 std::format( "sim {} left the mesh with the offer open", sim ) } );
            }
            else
            {
                ++offer;
                continue;
            }
            m_OfferOfPawn.erase( id );
            offer = m_Offers.erase( offer );
        }
        return effects;
    }

    std::string Ledger::Describe() const
    {
        std::uint64_t refused = 0, withdrawn = 0;
        // By name, so a line a scenario reads does not reorder when a code is added to ERefusal.
        std::map<std::string_view, std::uint64_t> byName;
        for ( const auto& [code, count] : m_RefusedCount )
        {
            if ( code == ERefusal::Withdrawn )
            {
                withdrawn += count; // a sender's own decision, not a refusal: reported on its own
                continue;
            }
            refused += count;
            byName[RefusalName( code )] += count;
        }
        std::string reasons;
        for ( const auto& [name, count] : byName )
            reasons += std::format( "{}{} {}", reasons.empty() ? "" : ", ", name, count );
        return std::format(
             "records {}, open offers {}, unadopted commits {}; committed {}, refused {}{}; withdrawn {}",
             m_Records.size(), m_Offers.size(), m_CommitRows.size(), m_CommittedCount, refused,
             reasons.empty() ? std::string() : std::format( " ({})", reasons ), withdrawn );
    }

    void Ledger::Refuse( Effects& effects, const Offer& offer, bool toSender, bool toReceiver, const Refusal& why )
    {
        const NetId                  id     = offer.Pawn.State.Id;
        const auto                   record = m_Records.find( id );
        const Decision               decision{ offer.Header.Key, id,
                                 record != m_Records.end() ? record->second.Epoch : offer.Header.Epoch,
                                 offer.Header.Step };
        const std::vector<std::byte> message = EncodeRefused( decision, why );
        if ( toSender )
            SendToSim( effects, offer.Header.Key.From, message );
        if ( toReceiver )
            SendToSim( effects, offer.Header.To, message );
        ++m_RefusedCount[why.Code];
    }

    void Ledger::RefuseSpeaker( Effects& effects, std::uint32_t sim, const OfferKey& key, std::uint32_t step,
                                const Refusal& why )
    {
        SendToSim( effects, sim, EncodeRefused( Decision{ key, kInvalidNetId, 0, step }, why ) );
        ++m_RefusedCount[why.Code];
    }

    void Ledger::Commit( Effects& effects, Offer offer, std::uint32_t atStep )
    {
        const OfferHeader&  h     = offer.Header;
        const NetId         id    = offer.Pawn.State.Id;
        const std::uint32_t epoch = h.Epoch + 1;
        // THE ONE PLACE A RECORD MOVES: the receiver accepted an offer that was still open.
        m_Records[id] = Authority{ h.To, epoch };
        m_CommitRows[id] = CommitRow{ h.Key, h.Step };
        const Decision decision{ h.Key, id, epoch, h.Step };
        SendToSim( effects, h.To, EncodeCommit( decision ) );
        SendToSim( effects, h.Key.From, EncodeCommitted( decision ) );
        const NetVec3& at = offer.Pawn.State.Position;
        effects.Log.push_back( std::format( "store: {} of pawn net id {} (player {} of gateway {}) committed (sim "
                                            "{}, epoch {}) -> (sim {}, epoch "
                                            "{}) at step {} - accepted at sim {}'s step {}, full state at "
                                            "({:.0f}, {:.0f}, {:.0f}) cm + {} unacked "
                                            "input(s)",
                                            NameOf( h.Key ), id, offer.Pawn.Player, h.Gateway, h.Key.From, h.Epoch,
                                            h.To, epoch, h.Step, h.To, atStep, at.X, at.Y, at.Z,
                                            offer.Pawn.Pending.size() ) );
        ++m_CommittedCount;
        effects.Commit = Committed{ id,        offer.Pawn.Player,      h.Key.From, h.To, epoch, h.Step,
                                    h.Gateway, std::move( offer.Pawn ) };
    }
} // namespace Desert::Net::Handover
