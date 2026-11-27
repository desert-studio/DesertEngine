#include "Net/SimHandover.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace Desert::Net
{
    namespace
    {
        using Handover::EMessage;
        using Handover::ERefusal;
        using Handover::OfferKey;
        using Handover::Refusal;

        // The refusals only a receiver gives (SimHandover::OnOffer); every other one is the store's.
        [[nodiscard]] bool IsReceiverRefusal( ERefusal code )
        {
            return code == ERefusal::RegionNotMine || code == ERefusal::NoCell ||
                   code == ERefusal::CatchUpTooLarge || code == ERefusal::AlreadyHeld;
        }

        [[nodiscard]] std::uint32_t StepsSince( std::uint32_t now, std::uint32_t then )
        {
            return now > then ? now - then : 0;
        }
    } // namespace

    SimHandover::SimHandover( MeshEndpoint& endpoint, std::uint32_t sim, NetLogSink log )
         : m_Endpoint( endpoint ), m_Sim( sim ), m_Log( std::move( log ) )
    {
    }

    bool SimHandover::OnCoordinatorMessage( std::uint8_t type, std::span<const std::byte> body )
    {
        auto map = DecodeMeshAuthorityMap( type, body );
        if ( !map )
            return false;
        m_Map = std::move( *map );
        for ( const MeshRegion& region : m_Map.Regions )
            m_Log( std::format( "sim {}: authority map: {}{}", m_Sim, DescribeMeshRegion( region ),
                                region.Sim == m_Sim ? " (this sim)" : "" ) );
        // A new map can make a refused receiver right (a region reassigned): every pawn may be offered again. A
        // HELD pawn's entry is not a backoff but its withdrawal mark (AfterStep) and stays until the store
        // answers.
        std::size_t ended = 0;
        for ( auto it = m_BackoffEnd.begin(); it != m_BackoffEnd.end(); )
        {
            if ( m_Offers.contains( it->first ) )
            {
                ++it;
                continue;
            }
            it = m_BackoffEnd.erase( it );
            ++ended;
        }
        if ( ended != 0 )
            m_Log( std::format( "sim {}: the new authority map ends the handover backoff of {} pawn(s)", m_Sim,
                                ended ) );
        return true;
    }

    // ------------------------------------------------------------------------------------------------- sender

    void SimHandover::AfterStep( std::uint32_t step, PawnSimulation& sim, RemoteGatewayLink& gateways,
                                 RemoteReplicationStore& )
    {
        // THE DEADLINE: an offer the store has not decided within kOfferDeadlineSteps is withdrawn - ONCE; the
        // pawn stays held until the store answers that Withdraw (the coordinator of the two-phase commit decides:
        // its Committed may already be on the way, and then the authority is the receiver's). The withdrawal is
        // marked by the held pawn's m_BackoffEnd entry (the step it was sent at): a held pawn has no backoff
        // otherwise. Without a link to the store no answer can come and nothing can have committed since it went:
        // the pawn resumes here at once.
        const bool storeLinked = !m_Endpoint.PeersOfRole( EMeshRole::Store ).empty();
        std::vector<std::pair<PlayerId, OpenOffer>> late;
        for ( const auto& [player, offer] : m_Offers )
            if ( StepsSince( step, offer.Step ) >= Handover::kOfferDeadlineSteps )
                late.emplace_back( player, offer );
        for ( const auto& [player, offer] : late )
        {
            const auto withdrawn = m_BackoffEnd.find( player );
            if ( !storeLinked )
            {
                ++m_Stats.Withdrawn;
                Resume( player,
                        Refusal{ ERefusal::ReceiverSilent,
                                 withdrawn != m_BackoffEnd.end()
                                      ? std::format( "withdrawn at step {}, and the link to the store is gone",
                                                     withdrawn->second )
                                      : std::format( "sim {} answered nothing for {} steps, and no link to the "
                                                     "store carries a Withdraw",
                                                     offer.To, StepsSince( step, offer.Step ) ) },
                        step, sim );
                continue;
            }
            if ( withdrawn != m_BackoffEnd.end() )
                continue; // the Withdraw went; the store's answer decides
            m_Endpoint.SendToRole( EMeshRole::Store,
                                   Handover::EncodeWithdraw( { OfferKey{ m_Sim, offer.Serial }, m_Sim, step } ) );
            m_BackoffEnd[player] = step;
            m_Log(
                 std::format( "sim {}: offer #{} of pawn net id {} (player {}) to sim {} - {}; Withdraw sent, the "
                              "pawn stays held until the store answers",
                              m_Sim, offer.Serial, offer.Id, player, offer.To,
                              Handover::Describe( { ERefusal::ReceiverSilent,
                                                    std::format( "sim {} answered nothing for {} steps", offer.To,
                                                                 StepsSince( step, offer.Step ) ) } ) ) );
        }

        AskInDoubt( step, storeLinked );

        for ( const PawnSimulation::Source& source : sim.Sources() )
        {
            if ( m_Offers.contains( source.Player ) || m_Incoming.contains( source.Player ) )
                continue; // held: the store decides first
            if ( const auto backoff = m_BackoffEnd.find( source.Player ); backoff != m_BackoffEnd.end() )
            {
                if ( step < backoff->second )
                    continue;
                m_BackoffEnd.erase( backoff );
            }
            const auto target  = m_Map.HandoverTarget( m_Sim, source.Position.X, source.Position.Z );
            const auto gateway = gateways.GatewayOf( source.Player );
            if ( !target || !gateway )
                continue;
            Offer( step, source, *target, *gateway, sim, gateways );
        }
    }

    void SimHandover::Offer( std::uint32_t step, const PawnSimulation::Source& source, std::uint32_t target,
                             std::uint32_t gateway, PawnSimulation& sim, RemoteGatewayLink& )
    {
        const MeshRegion* region = m_Map.RegionAt( source.Position.X, source.Position.Z );
        auto              pawn   = sim.HandoverOf( source.Player );
        if ( !region || !pawn )
            return;
        const auto                  epochIt = m_Epoch.find( source.Player );
        const std::uint32_t         epoch   = epochIt == m_Epoch.end() ? 1 : epochIt->second;
        const Handover::OfferHeader header{ OfferKey{ m_Sim, m_NextSerial }, epoch, target, step, gateway };
        // THE WIRE FIRST: a record the wire cannot carry is refused before anything is held (the pawn goes on).
        const std::vector<std::byte> message = Handover::EncodeOffer( header, *pawn );
        if ( message.empty() )
        {
            m_BackoffEnd[source.Player] = step + Handover::kRetryBackoffSteps;
            m_Log( std::format(
                 "sim {}: pawn net id {} (player {}) not offered to sim {} - {}; it stays on this sim, "
                 "next offer not before step {}",
                 m_Sim, source.Id, source.Player, target,
                 Handover::Describe(
                      { ERefusal::WireStep, std::format( "{} pending input step(s), one the wire does not "
                                                         "carry (InputWire.hpp)",
                                                         pawn->Pending.size() ) } ),
                 step + Handover::kRetryBackoffSteps ) );
            return;
        }
        if ( const Common::BoolResultStr held = sim.Hold( source.Player ); !held )
        {
            m_Log( std::format( "sim {}: pawn net id {} (player {}) not offered to sim {} - {}", m_Sim, source.Id,
                                source.Player, target, held.GetError() ) );
            return;
        }
        m_Endpoint.SendToRole( EMeshRole::Store, message );
        m_Offers[source.Player] = OpenOffer{ m_NextSerial, source.Id, target, step };
        ++m_Stats.Offered;
        m_Log( std::format(
             "sim {}: pawn net id {} (player {}) at ({:.0f}, {:.0f}) cm is {:.0f} cm inside region "
             "'{}' of sim {} (margin {:.0f}) - held and offered to sim {} (offer #{}, epoch {}, frozen "
             "at step {}, {} unacked input(s)); it stays this sim's until the store commits",
             m_Sim, source.Id, source.Player, source.Position.X, source.Position.Z,
             region->Depth( source.Position.X, source.Position.Z ), region->Name, target, m_Map.HandoverMargin,
             target, m_NextSerial, epoch, step, pawn->Pending.size() ) );
        ++m_NextSerial;
    }

    void SimHandover::Resume( PlayerId player, const Refusal& why, std::uint32_t step, PawnSimulation& sim )
    {
        const auto offer = m_Offers.find( player );
        if ( offer == m_Offers.end() )
            return;
        const OpenOffer open = offer->second;
        m_Offers.erase( offer );
        const std::uint32_t backoffEnd = step + Handover::kRetryBackoffSteps;
        m_BackoffEnd[player]           = backoffEnd;
        const auto queued              = sim.Resume( player );
        if ( !queued )
        {
            // The player left while its pawn was held (PawnSimulation::PlayerLeft removed it): nothing resumes.
            m_Log(
                 std::format( "sim {}: offer #{} of pawn net id {} (player {}) to sim {} ended - {}; the pawn is "
                              "gone from this sim (its player left)",
                              m_Sim, open.Serial, open.Id, player, open.To, Handover::Describe( why ) ) );
            return;
        }
        m_Log( std::format(
             "sim {}: offer #{} of pawn net id {} (player {}) to sim {} ended - {}{}; the pawn resumes "
             "on sim {} with {} queued input(s), next offer not before step {}",
             m_Sim, open.Serial, open.Id, player, open.To, Handover::Describe( why ),
             why.Code == ERefusal::ReceiverSilent || why.Code == ERefusal::Withdrawn ? " - withdrawn" : "", m_Sim,
             *queued, backoffEnd ) );
    }

    // ------------------------------------------------------------------------------------------- store messages

    bool SimHandover::OnStoreMessage( std::uint8_t type, std::span<const std::byte> body, PawnSimulation& sim,
                                      RemoteGatewayLink& gateways, RemoteReplicationStore& replica,
                                      std::uint32_t step )
    {
        if ( const auto forwarded = DecodeMeshForwardedInput( type, body ) )
        {
            if ( !forwarded->Input )
                m_Log( std::format( "sim {}: a forwarded input of player {} is malformed ({} bytes); dropped",
                                    m_Sim, forwarded->Player, body.size() ) );
            else if ( !sim.HasPawn( forwarded->Player ) )
                m_Log(
                     std::format( "sim {}: a forwarded input of player {} dropped - this sim holds no pawn of it",
                                  m_Sim, forwarded->Player ) );
            else
                sim.PlayerInput( forwarded->Player, *forwarded->Input );
            return true;
        }
        if ( !Handover::IsHandoverMessage( type ) )
            return false;
        const auto message = static_cast<EMessage>( type );
        switch ( message )
        {
            case EMessage::Offer:
                OnOffer( body, sim, step );
                break;
            case EMessage::Commit:
            case EMessage::Committed:
            case EMessage::Refused:
                OnDecision( message, body, sim, gateways, replica, step );
                break;
            case EMessage::Accept:
            case EMessage::Refuse:
            case EMessage::Withdraw:
                m_Log( std::format( "sim {}: the store sent a {} message, which only a sim sends; ignored", m_Sim,
                                    Handover::MessageName( message ) ) );
                break;
        }
        return true;
    }

    // ----------------------------------------------------------------------------------------------- receiver

    void SimHandover::OnOffer( std::span<const std::byte> body, PawnSimulation& sim, std::uint32_t step )
    {
        Handover::OfferHeader        header;
        PawnSimulation::HandoverPawn pawn;
        if ( !Handover::DecodeOffer( body, header, pawn ) )
        {
            // No key to answer: the store's deadline on the sender ends the offer (ReceiverSilent).
            m_Log( std::format( "sim {}: an offer from the store is malformed ({} bytes); not answered", m_Sim,
                                body.size() ) );
            return;
        }
        const PlayerId      player = pawn.Player;
        const NetId         id     = pawn.State.Id;
        const NetVec3       at     = pawn.State.Position;
        const std::uint32_t behind = StepsSince( step, header.Step );
        const std::string   name = std::format( "sim {}: offer (sim {}, #{}) of pawn net id {} (player {})", m_Sim,
                                                header.Key.From, header.Key.Serial, id, player );

        // THE RECEIVER'S VOTE: every check its own, each failure a named reason.
        std::optional<Refusal> refusal;
        const MeshRegion*      region = m_Map.RegionAt( at.X, at.Z );
        if ( !region || region->Sim != m_Sim )
            refusal = Refusal{
                 ERefusal::RegionNotMine,
                 region ? std::format( "({:.0f}, {:.0f}) cm is in region '{}' of sim {} in this sim's "
                                       "map",
                                       at.X, at.Z, region->Name, region->Sim )
                        : std::format( "no region of this sim's map holds ({:.0f}, {:.0f}) cm", at.X, at.Z ) };
        else if ( sim.HasPawn( player ) )
            refusal = Refusal{ ERefusal::AlreadyHeld,
                               std::format( "this sim already holds a pawn of player {}", player ) };
        else if ( behind > Handover::kMaxCatchUpSteps )
            refusal =
                 Refusal{ ERefusal::CatchUpTooLarge,
                          std::format( "frozen at step {}, this sim is at step {}: {} steps behind (at most {})",
                                       header.Step, step, behind, Handover::kMaxCatchUpSteps ) };
        else if ( const Common::BoolResultStr cell = sim.CanTake( pawn ); !cell )
            refusal = Refusal{ ERefusal::NoCell, cell.GetError() };

        const Handover::Verdict verdict{ header.Key, m_Sim, step };
        if ( refusal )
        {
            m_Endpoint.SendToRole( EMeshRole::Store, Handover::EncodeRefuse( verdict, *refusal ) );
            ++m_Stats.IncomingRefused;
            m_Log( std::format( "{} refused - {}; nothing held", name, Handover::Describe( *refusal ) ) );
            return;
        }
        const std::size_t pending = pawn.Pending.size();
        sim.Expect( std::move( pawn ), header.Gateway );
        m_Incoming[player] = Incoming{ header.Key, id, header.Key.From, header.Step, step };
        m_Endpoint.SendToRole( EMeshRole::Store, Handover::EncodeAccept( verdict ) );
        m_Log(
             std::format( "{} accepted (catch-up {} steps) - held as incoming with {} unacked input(s) until the "
                          "store commits",
                          name, behind, pending ) );
    }

    void SimHandover::AskInDoubt( std::uint32_t step, bool storeLinked )
    {
        // THE PARTICIPANT IN DOUBT ASKS THE COORDINATOR: the Accept is re-sent under the same key and the store
        // answers with the offer's outcome (HandoverLedger.hpp); OnDecision acts on it as on the first answer.
        // Without a link to the store no answer can come: the copy waits, and asks on the first step the link is
        // back (the store may have committed - dropping it here could leave the pawn with no owner).
        if ( !storeLinked )
            return;
        for ( auto& [player, held] : m_Incoming )
        {
            if ( StepsSince( step, held.AskedAt ) < Handover::kInDoubtSteps )
                continue;
            m_Endpoint.SendToRole( EMeshRole::Store,
                                   Handover::EncodeAccept( Handover::Verdict{ held.Key, m_Sim, step } ) );
            ++m_Stats.InDoubtAsks;
            m_Log( std::format( "sim {}: offer (sim {}, #{}) of pawn net id {} (player {}) accepted at step {}, no "
                                "Commit or Refused for {} steps - in doubt: Accept sent again, the store's answer "
                                "decides",
                                m_Sim, held.Key.From, held.Key.Serial, held.Id, player, held.AskedAt,
                                StepsSince( step, held.AskedAt ) ) );
            held.AskedAt = step;
        }
    }

    // ------------------------------------------------------------------------------------- the store decides

    void SimHandover::OnDecision( EMessage type, std::span<const std::byte> body, PawnSimulation& sim,
                                  RemoteGatewayLink& gateways, RemoteReplicationStore& replica,
                                  std::uint32_t step )
    {
        Handover::Decision decision;
        Refusal            refusal;
        if ( !Handover::DecodeDecision( body, decision, type == EMessage::Refused ? &refusal : nullptr ) )
        {
            m_Log( std::format( "sim {}: a {} from the store is malformed ({} bytes); ignored", m_Sim,
                                Handover::MessageName( type ), body.size() ) );
            return;
        }
        // Matched by the OFFER KEY, never by the net id (a Refused(OfferUnknown) names no pawn).
        const auto offer    = decision.Key.From != m_Sim
                                   ? m_Offers.end()
                                   : std::find_if( m_Offers.begin(), m_Offers.end(), [&]( const auto& entry )
                                                   { return entry.second.Serial == decision.Key.Serial; } );
        const auto incoming = std::find_if( m_Incoming.begin(), m_Incoming.end(), [&]( const auto& entry )
                                            { return entry.second.Key == decision.Key; } );

        if ( type == EMessage::Committed )
        {
            if ( offer == m_Offers.end() )
            {
                m_Log(
                     std::format( "sim {}: Committed of offer (sim {}, #{}) at epoch {}, which this sim does not "
                                  "hold as sender; ignored",
                                  m_Sim, decision.Key.From, decision.Key.Serial, decision.Epoch ) );
                return;
            }
            const PlayerId  player = offer->first;
            const OpenOffer open   = offer->second;
            m_Offers.erase( offer );
            auto pawn = sim.Release( player );
            if ( !pawn )
            {
                m_Log(
                     std::format( "sim {}: offer #{} of pawn net id {} (player {}) committed by the store (epoch "
                                  "{}), but this sim no longer holds the pawn (its player left)",
                                  m_Sim, open.Serial, open.Id, player, decision.Epoch ) );
                return;
            }
            // THE AUTHORITY GOES NOW, and only now: the inputs that queued while it was held, and every later one
            // that still reaches this sim, follow the pawn to its new owner.
            const std::size_t queued    = pawn->Pending.size();
            const std::size_t forwarded = gateways.ForwardInputs( player, open.To, pawn->Pending );
            replica.Forget( open.Id );
            m_Epoch.erase( player );
            m_BackoffEnd.erase( player );
            ++m_Stats.Committed;
            m_Log(
                 std::format( "sim {}: offer #{} of pawn net id {} (player {}) committed by the store (epoch {}): "
                              "released, {} queued input(s) forwarded to sim {} ({} queued; held from step {} to "
                              "step {}, {} step(s))",
                              m_Sim, open.Serial, open.Id, player, decision.Epoch, forwarded, open.To, queued,
                              open.Step, step, StepsSince( step, open.Step ) ) );
            return;
        }

        if ( type == EMessage::Commit )
        {
            if ( incoming == m_Incoming.end() )
            {
                m_Log( std::format(
                     "sim {}: Commit of offer (sim {}, #{}) of pawn net id {} at epoch {}, which this "
                     "sim does not hold as incoming - the record names this sim and no pawn is here",
                     m_Sim, decision.Key.From, decision.Key.Serial, decision.Id, decision.Epoch ) );
                return;
            }
            const PlayerId player = incoming->first;
            const Incoming held   = incoming->second;
            // A decision names its key AND its subject: a Commit under this key for another pawn (a restarted
            // sender reusing its serials) is not this copy's - the copy stays in doubt and asks again.
            if ( decision.Id != held.Id )
            {
                m_Log( std::format( "sim {}: Commit of offer (sim {}, #{}) names pawn net id {} at epoch {}, but "
                                    "the incoming copy under that key is pawn net id {} (player {}) - decision "
                                    "dropped, the copy stays in doubt",
                                    m_Sim, decision.Key.From, decision.Key.Serial, decision.Id, decision.Epoch,
                                    held.Id, player ) );
                return;
            }
            // THE CATCH-UP IS CAPPED as the Offer's check is (kMaxCatchUpSteps): after a long doubt the steps past
            // the cap are not simulated in one step - the pawn goes on from its frozen state, its queued inputs
            // applied by the steps that follow.
            const std::uint32_t behind   = StepsSince( step, held.Step );
            const std::uint32_t catchUp  = std::min( behind, Handover::kMaxCatchUpSteps );
            const bool          wasProxy = replica.IsProxy( held.Id );
            m_Incoming.erase( incoming );
            if ( catchUp != behind )
                m_Log( std::format( "sim {}: offer (sim {}, #{}) of pawn net id {} (player {}) committed {} steps "
                                    "after it froze (step {}, this sim at step {}): catch-up capped at {} steps, "
                                    "{} step(s) not simulated",
                                    m_Sim, held.Key.From, held.Key.Serial, held.Id, player, behind, held.Step,
                                    step, catchUp, behind - catchUp ) );
            const auto gateway = sim.Commit( player, catchUp );
            if ( !gateway )
            {
                m_Log( std::format( "sim {}: offer (sim {}, #{}) of pawn net id {} (player {}) committed, but its "
                                    "player left while it was incoming - removed from the store, not adopted",
                                    m_Sim, held.Key.From, held.Key.Serial, held.Id, player ) );
                return;
            }
            replica.Promote( held.Id );
            m_Epoch[player] = decision.Epoch;
            gateways.Adopted( player, *gateway );
            ++m_Stats.Adopted;
            m_Log(
                 std::format( "sim {}: offer (sim {}, #{}) of pawn net id {} (player {}) committed: incoming pawn "
                              "promoted at epoch {} - {}; frozen at step {}, this sim is at step {}: {} catch-up "
                              "step(s)",
                              m_Sim, held.Key.From, held.Key.Serial, held.Id, player, decision.Epoch,
                              wasProxy ? "its proxy replaced by the authoritative pawn"
                                       : "held no proxy of it (outside the border band)",
                              held.Step, step, catchUp ) );
            return;
        }

        // Refused: the side that still holds something lets go of the offer, never of the pawn's authority.
        if ( offer != m_Offers.end() )
        {
            // The answer to this sim's Withdraw (or a refusal that crossed it) is the store's decision: resume.
            if ( refusal.Code == ERefusal::Withdrawn )
                ++m_Stats.Withdrawn;
            else if ( IsReceiverRefusal( refusal.Code ) )
                ++m_Stats.RefusedByReceiver;
            else
                ++m_Stats.RefusedByStore;
            const PlayerId player = offer->first;
            m_Log( std::format( "sim {}: offer #{} of pawn net id {} (player {}) Refused by the {} - {}", m_Sim,
                                offer->second.Serial, offer->second.Id, player,
                                IsReceiverRefusal( refusal.Code ) ? "receiver" : "store",
                                Handover::Describe( refusal ) ) );
            Resume( player, refusal, step, sim );
            return;
        }
        if ( incoming != m_Incoming.end() )
        {
            const PlayerId player = incoming->first;
            const Incoming held   = incoming->second;
            m_Incoming.erase( incoming );
            sim.Abandon( player );
            ++m_Stats.IncomingDropped;
            m_Log( std::format( "sim {}: offer (sim {}, #{}) of pawn net id {} (player {}) Refused by the store - "
                                "{}; incoming copy dropped",
                                m_Sim, held.Key.From, held.Key.Serial, held.Id, player,
                                Handover::Describe( refusal ) ) );
            return;
        }
        // An answer about an offer this sim holds nothing of any more (it withdrew, or it refused it): no state.
        m_Log( std::format( "sim {}: Refused of offer (sim {}, #{}) - {}; this sim holds nothing of it", m_Sim,
                            decision.Key.From, decision.Key.Serial, Handover::Describe( refusal ) ) );
    }

    std::string SimHandover::Describe() const
    {
        return std::format(
             "handovers: {} offered, {} committed, {} adopted; refused {} by store, {} by receiver; {} "
             "withdrawn; incoming {} refused, {} dropped; {} open as sender, {} incoming; {} in-doubt ask(s)",
             m_Stats.Offered, m_Stats.Committed, m_Stats.Adopted, m_Stats.RefusedByStore,
             m_Stats.RefusedByReceiver, m_Stats.Withdrawn, m_Stats.IncomingRefused, m_Stats.IncomingDropped,
             m_Offers.size(), m_Incoming.size(), m_Stats.InDoubtAsks );
    }
} // namespace Desert::Net
