#pragma once

// THE HANDOVER IN A SIM (NET-HANDOVER-CONFIRM; NET-ARCH §2M.4): both ends of the two-phase commit of
// HandoverWire.hpp as one sim sees them.
//
// As the SENDER, after every mesh step: a pawn standing past the margin inside a neighbour's region, not under a
// backoff, is HELD (PawnSimulation: not stepped, its inputs queue, its state no longer written) and offered to
// the store with its unacked inputs. The sim keeps the authority: it is still the only writer of that pawn until
// the store's Committed. Committed -> the pawn is released, the inputs that queued while it was held and every
// later one go to the new owner through the store (RemoteGatewayLink::ForwardInputs). Refused, or no answer for
// kOfferDeadline (-> Withdraw) -> the pawn resumes with its queued inputs, and is not offered again for
// kRetryBackoffSteps unless the authority map changes. Every outcome is one log line with its ERefusal and one
// count in Stats.
//
// As the RECEIVER: an Offer is checked (its region is this sim's in the map, the world has a cell there, the
// catch-up is at most kMaxCatchUpSteps, no pawn held for that player) and the pawn is held as INCOMING in
// PawnSimulation (not stepped; inputs the gateway already re-routed here queue on it - no input is dropped)
// while Accept goes to the store; Commit -> the incoming pawn becomes owned and catches up to this sim's step;
// Refused -> the copy is dropped with the reason. An Offer that fails a check is answered with Refuse(reason) and
// nothing is held. A receiver IN DOUBT - accepted, and no Commit or Refused for kInDoubtWait (one was lost) - asks
// the coordinator: it re-sends its Accept, and the store answers every vote with the offer's outcome (Commit again
// when the record already names this sim, Refused(OfferUnknown) when the offer ended any other way, the commit
// itself when the offer is still open); it asks again every kInDoubtWait until an answer comes. Dropping the copy
// on a timer alone would leave a committed pawn with no owner. A player that leaves while its pawn is incoming
// waits for the same decision (PawnSimulation::PlayerLeft).
//
// Lives in Server/Source/main.cpp's sim role next to PawnSimulation, RemoteGatewayLink and the store replica;
// the mesh handler routes the store's handover messages (IsHandoverMessage) and the coordinator's map here.

#include <Net/HandoverWire.hpp>
#include <Net/MeshLink.hpp>
#include <Net/MeshOfOne.hpp>
#include <Net/NetTransport.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace Desert::Net
{
    class SimHandover
    {
    public:
        struct Stats
        {
            std::uint64_t Offered       = 0; // offers sent
            std::uint64_t Committed     = 0; // offers that moved the record (this sim as sender)
            std::uint64_t Adopted       = 0; // incoming pawns committed to this sim
            std::uint64_t RefusedByStore    = 0;
            std::uint64_t RefusedByReceiver = 0; // as sender: the receiver said no
            std::uint64_t Withdrawn     = 0; // deadline passed, offer withdrawn
            std::uint64_t IncomingRefused = 0; // as receiver: this sim said no to an offer
            std::uint64_t IncomingDropped = 0; // as receiver: an incoming copy dropped on Refused
            std::uint64_t InDoubtAsks = 0; // as receiver: Accepts re-sent after kInDoubtWait without a decision
        };

        SimHandover( MeshEndpoint& endpoint, std::uint32_t sim, NetLogSink log );

        // The coordinator's authority map; true when @p type was one. A new map ends every backoff.
        bool OnCoordinatorMessage( std::uint8_t type, std::span<const std::byte> body );
        // The store's handover messages (HandoverWire.hpp IsHandoverMessage) and ForwardedInput; true when @p type
        // was one of them. @p step: the last mesh step this sim ran.
        bool OnStoreMessage( std::uint8_t type, std::span<const std::byte> body, PawnSimulation& sim,
                             RemoteGatewayLink& gateways, RemoteReplicationStore& replica, std::uint32_t step );
        // After mesh step @p step: withdraws offers past their deadline, offers the pawns that crossed into a
        // neighbour's region.
        void AfterStep( std::uint32_t step, PawnSimulation& sim, RemoteGatewayLink& gateways,
                        RemoteReplicationStore& replica );

        [[nodiscard]] const MeshAuthorityMap& Map() const
        {
            return m_Map;
        }
        [[nodiscard]] const Stats& Counts() const
        {
            return m_Stats;
        }
        // "handovers: N offered, N committed, N adopted; refused N by store, N by receiver; N withdrawn; incoming
        // N refused, N dropped; N open as sender, N incoming; N in-doubt ask(s)" for the sim's stat line.
        [[nodiscard]] std::string Describe() const;

    private:
        struct OpenOffer
        {
            Handover::OfferSerial Serial = 0;
            NetId                 Id     = kInvalidNetId;
            std::uint32_t         To     = 0;
            std::uint32_t         Step   = 0; // frozen at (the log)
            NetClock::time_point  When;       // the wall time of the same (the offer deadline is measured on it)
        };
        struct Incoming
        {
            Handover::OfferKey Key;
            NetId              Id   = kInvalidNetId;
            std::uint32_t      From    = 0;
            std::uint32_t      Step    = 0; // frozen at
            std::uint32_t      AskedAt = 0; // this sim's step of the Accept, or of the last in-doubt ask (the log)
            NetClock::time_point AskedWhen; // the wall time of the same (the in-doubt wait is measured on it)
        };

        void Offer( std::uint32_t step, const PawnSimulation::Source& source, std::uint32_t target,
                    std::uint32_t gateway, PawnSimulation& sim, RemoteGatewayLink& gateways );
        void OnOffer( std::span<const std::byte> body, PawnSimulation& sim, std::uint32_t step );
        // As receiver, after a step: an incoming copy with no decision for kInDoubtWait of wall time asks the
        // store again. @p now: NetClock::now() of this step.
        void AskInDoubt( std::uint32_t step, NetClock::time_point now, bool storeLinked );
        void OnDecision( Handover::EMessage type, std::span<const std::byte> body, PawnSimulation& sim,
                         RemoteGatewayLink& gateways, RemoteReplicationStore& replica, std::uint32_t step );
        void Resume( PlayerId player, const Handover::Refusal& why, std::uint32_t step, PawnSimulation& sim );

        MeshEndpoint&                      m_Endpoint;
        std::uint32_t                      m_Sim;
        NetLogSink                         m_Log;
        MeshAuthorityMap                   m_Map;
        std::map<PlayerId, std::uint32_t>  m_Epoch;      // the authority epoch of each pawn this sim holds
        std::map<PlayerId, OpenOffer>      m_Offers;     // as sender: held pawns awaiting the store
        std::map<PlayerId, Incoming>       m_Incoming;   // as receiver: accepted, awaiting Commit
        std::map<PlayerId, std::uint32_t>  m_BackoffEnd; // as sender: no new offer before this step
        Handover::OfferSerial              m_NextSerial = 1;
        Stats                              m_Stats;
    };
} // namespace Desert::Net
