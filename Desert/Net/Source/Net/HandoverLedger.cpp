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
        return created || record->second.Sim == sim;
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
        if ( const auto last = m_CommitOfPawn.find( id ); last != m_CommitOfPawn.end() )
        {
            m_CommitOf.erase( last->second );
            m_CommitOfPawn.erase( last );
        }
        return true;
    }

    Effects Ledger::OnSimMessage( std::uint32_t sim, std::uint8_t type, std::span<const std::byte> body )
    {
        const auto message = static_cast<EMessage>( type );
        switch ( message )
        {
            case EMessage::Offer:
                return Opened( sim, body );
            case EMessage::Accept:
            case EMessage::Refuse:
            case EMessage::Withdraw:
                return Verdicted( sim, message, body );
            case EMessage::Commit:
            case EMessage::Committed:
            case EMessage::Refused:
                break;
        }
        // A store-to-sim message (or a type past the range) sent BY a sim: no offer to answer for.
        Effects effects;
        effects.Log.push_back( std::format( "store: handover message 0x{:02x} ({}) from sim {} is not one a sim "
                                            "sends ({} bytes); ignored",
                                            type, IsHandoverMessage( type ) ? MessageName( message ) : "unknown",
                                            sim, body.size() ) );
        return effects;
    }

    Effects Ledger::Opened( std::uint32_t sim, std::span<const std::byte> body )
    {
        Effects                effects;
        Offer                  offer;
        const bool             decoded = DecodeOffer( body, offer.Header, offer.Pawn );
        const OfferHeader&     h       = offer.Header;
        const NetId            id      = offer.Pawn.State.Id;
        std::optional<Refusal> refusal;
        const auto             record     = m_Records.find( id );
        const auto             openOfPawn = m_OfferOfPawn.find( id );
        if ( !decoded )
        {
            // The refusal goes back to the sim that spoke, under serial 0 (no sender ever issues it): a partly
            // read serial could name an offer that sim still has open.
            offer.Header.Key = OfferKey{ sim, 0 };
            refusal               = Refusal{
                 ERefusal::Malformed,
                 std::format( "the offer record from sim {} did not decode ({} bytes)", sim, body.size() ) };
        }
        else if ( h.Key.From != sim || h.To == sim || h.To == 0 )
        {
            refusal =
                 Refusal{ ERefusal::Malformed,
                          std::format( "the offer names sender sim {} and receiver sim {} but came from sim {}",
                                       h.Key.From, h.To, sim ) };
            // The refusal goes to the sim that spoke, under serial 0: the header's own key would send it to the
            // sim the header names, where it could match and resume a live offer - two owners.
            offer.Header.Key = OfferKey{ sim, 0 };
        }
        else if ( m_Offers.contains( h.Key ) || openOfPawn != m_OfferOfPawn.end() )
        {
            // The sender holds the pawn while its offer is open, so a second offer is a sender defect, not data.
            refusal = Refusal{
                 ERefusal::Malformed,
                 std::format( "{} names pawn net id {}, which already has {} open", NameOf( h.Key ), id,
                              NameOf( openOfPawn != m_OfferOfPawn.end() ? openOfPawn->second : h.Key ) ) };
            // Refused under the offer's own key would match the sender's live offer and resume the pawn while
            // the open one may still commit: two owners. Serial 0 matches no offer a sender holds.
            offer.Header.Key.Serial = 0;
        }
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
        const OfferKey key = h.Key;
        m_OfferOfPawn.emplace( id, key );
        m_Offers.emplace( key, std::move( offer ) );
        return effects;
    }

    Effects Ledger::Verdicted( std::uint32_t sim, EMessage type, std::span<const std::byte> body )
    {
        Effects    effects;
        Verdict    verdict;
        Refusal    receiverWhy;
        const bool decoded = DecodeVerdict( body, verdict, type == EMessage::Refuse ? &receiverWhy : nullptr );
        const auto offer   = decoded ? m_Offers.find( verdict.Key ) : m_Offers.end();
        // Accept and Refuse come from the offer's receiver, Withdraw from its sender.
        const auto speakerOf = [type]( const Offer& o )
        { return type == EMessage::Withdraw ? o.Header.Key.From : o.Header.To; };

        if ( decoded && offer == m_Offers.end() && type == EMessage::Accept && verdict.Sim == sim )
        {
            // THE VOTE OF A RECEIVER IN DOUBT: the offer that last moved the record to this very sim is answered
            // with its Commit again; nothing moves, nothing is counted.
            const auto committed = m_CommitOf.find( verdict.Key );
            const auto record =
                 committed != m_CommitOf.end() ? m_Records.find( committed->second.Id ) : m_Records.end();
            if ( record != m_Records.end() && record->second.Sim == sim )
            {
                const Decision decision{ verdict.Key, committed->second.Id, record->second.Epoch,
                                         committed->second.Step };
                SendToSim( effects, sim, EncodeCommit( decision ) );
                effects.Log.push_back( std::format(
                     "store: accept from sim {} at its step {} of {}, which committed pawn net id {} to it (sim "
                     "{}, epoch {}) before - Commit sent again, nothing moves",
                     sim, verdict.Step, NameOf( verdict.Key ), decision.Id, sim, decision.Epoch ) );
                return effects;
            }
        }

        std::optional<Refusal> refusal;
        if ( !decoded )
            refusal =
                 Refusal{ ERefusal::Malformed, std::format( "the {} record from sim {} did not decode ({} bytes)",
                                                            VerbOf( type ), sim, body.size() ) };
        else if ( offer == m_Offers.end() )
            refusal = Refusal{ ERefusal::OfferUnknown,
                               std::format( "{} is not open at the store (withdrawn, refused or committed before)",
                                            NameOf( verdict.Key ) ) };
        else if ( verdict.Sim != sim || speakerOf( offer->second ) != sim )
            refusal = Refusal{ ERefusal::OfferUnknown,
                               std::format( "sim {} (naming sim {}) is not the {} of {}", sim, verdict.Sim,
                                            type == EMessage::Withdraw ? "sender" : "receiver",
                                            NameOf( verdict.Key ) ) };
        if ( refusal )
        {
            // Answered to the speaker only: the offer it names (if any) is not touched.
            effects.Log.push_back( std::format( "store: {} from sim {} refused - {}", VerbOf( type ), sim,
                                                Handover::Describe( *refusal ) ) );
            SendToSim( effects, sim,
                       EncodeRefused( Decision{ verdict.Key, kInvalidNetId, 0, verdict.Step }, *refusal ) );
            ++m_RefusedCount[refusal->Code];
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
                                                    "the record stays (sim {}, epoch {}); sim {} drops its copy, the sender resumes",
                                                    NameOf( h.Key ), id, sim, verdict.Step, h.Key.From, h.Epoch,
                                                    h.To ) );
                Refuse( effects, ended, true, true,
                        Refusal{ ERefusal::Withdrawn, std::format( "sim {} withdrew {} at its step {}", sim,
                                                                   NameOf( h.Key ), verdict.Step ) } );
                break;
        }
        return effects;
    }

    Effects Ledger::OnSimLost( std::uint32_t sim )
    {
        // Every stored offer is Open: an Accept commits within the call that carries it, so a sender lost after
        // its receiver accepted finds the record already moved (the "Accepted + sender lost = commit" rule).
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
        std::string   reasons;
        for ( const auto& [code, count] : m_RefusedCount )
        {
            if ( code == ERefusal::Withdrawn )
            {
                withdrawn += count; // a sender's own decision, not a refusal: reported on its own
                continue;
            }
            refused += count;
            reasons += std::format( "{}{} {}", reasons.empty() ? "" : ", ", RefusalName( code ), count );
        }
        return std::format( "records {}, open offers {}; committed {}, refused {}{}; withdrawn {}",
                            m_Records.size(), m_Offers.size(), m_CommittedCount, refused,
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

    void Ledger::Commit( Effects& effects, Offer offer, std::uint32_t atStep )
    {
        offer.Phase               = EPhase::Accepted;
        const OfferHeader&  h     = offer.Header;
        const NetId         id    = offer.Pawn.State.Id;
        const std::uint32_t epoch = h.Epoch + 1;
        // THE ONE PLACE A RECORD MOVES: the receiver accepted an offer that was still open.
        m_Records[id] = Authority{ h.To, epoch };
        if ( const auto last = m_CommitOfPawn.find( id ); last != m_CommitOfPawn.end() )
            m_CommitOf.erase( last->second );
        m_CommitOf[h.Key]  = CommittedOffer{ id, h.Step };
        m_CommitOfPawn[id] = h.Key;
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
