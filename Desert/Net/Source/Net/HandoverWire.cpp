#include "Net/HandoverWire.hpp"
#include "Net/WireCodec.hpp"

#include <format>

namespace Desert::Net::Handover
{
    namespace
    {
        constexpr std::uint8_t Code( EMessage type )
        {
            return static_cast<std::uint8_t>( type );
        }

        void WriteKey( Wire::Writer& w, const OfferKey& key )
        {
            w.U32( key.From );
            w.U32( key.Serial );
        }
        bool ReadKey( Wire::Reader& r, OfferKey& key )
        {
            return r.U32( key.From ) && r.U32( key.Serial );
        }

        void WriteRefusal( Wire::Writer& w, const Refusal& refusal )
        {
            w.U8( static_cast<std::uint8_t>( refusal.Code ) );
            w.Str( refusal.Detail ); // at most 255 bytes, never empty (the Reader refuses an empty one)
        }
        bool ReadRefusal( Wire::Reader& r, Refusal& refusal )
        {
            std::uint8_t code = 0;
            if ( !r.U8( code ) || code > static_cast<std::uint8_t>( ERefusal::WireStep ) )
                return false; // a reason past the last one is a malformed record, not a guess
            refusal.Code = static_cast<ERefusal>( code );
            return r.Str( refusal.Detail );
        }

        std::vector<std::byte> EncodeVerdictAs( EMessage type, const Verdict& verdict, const Refusal* refusal )
        {
            Wire::Writer w( Code( type ) );
            WriteKey( w, verdict.Key );
            w.U32( verdict.Sim );
            w.U32( verdict.Step );
            if ( refusal )
                WriteRefusal( w, *refusal );
            return w.Take();
        }

        std::vector<std::byte> EncodeDecisionAs( EMessage type, const Decision& decision, const Refusal* refusal )
        {
            Wire::Writer w( Code( type ) );
            WriteKey( w, decision.Key );
            w.U32( decision.Id );
            w.U32( decision.Epoch );
            w.U32( decision.Step );
            if ( refusal )
                WriteRefusal( w, *refusal );
            return w.Take();
        }
    } // namespace

    std::string_view MessageName( EMessage type )
    {
        switch ( type )
        {
            case EMessage::Offer:
                return "Offer";
            case EMessage::Accept:
                return "Accept";
            case EMessage::Refuse:
                return "Refuse";
            case EMessage::Commit:
                return "Commit";
            case EMessage::Committed:
                return "Committed";
            case EMessage::Withdraw:
                return "Withdraw";
            case EMessage::Refused:
                return "Refused";
        }
        return "UnknownHandoverMessage";
    }

    std::string_view RefusalName( ERefusal code )
    {
        switch ( code )
        {
            case ERefusal::NotHolder:
                return "NotHolder";
            case ERefusal::StaleEpoch:
                return "StaleEpoch";
            case ERefusal::NoRecord:
                return "NoRecord";
            case ERefusal::ReceiverUnlinked:
                return "ReceiverUnlinked";
            case ERefusal::ReceiverLost:
                return "ReceiverLost";
            case ERefusal::SenderLost:
                return "SenderLost";
            case ERefusal::Withdrawn:
                return "Withdrawn";
            case ERefusal::OfferUnknown:
                return "OfferUnknown";
            case ERefusal::PawnRemoved:
                return "PawnRemoved";
            case ERefusal::ReceiverSilent:
                return "ReceiverSilent";
            case ERefusal::RegionNotMine:
                return "RegionNotMine";
            case ERefusal::NoCell:
                return "NoCell";
            case ERefusal::CatchUpTooLarge:
                return "CatchUpTooLarge";
            case ERefusal::AlreadyHeld:
                return "AlreadyHeld";
            case ERefusal::Malformed:
                return "Malformed";
            case ERefusal::WireStep:
                return "WireStep";
        }
        return "UnknownRefusal";
    }

    std::string Describe( const Refusal& refusal )
    {
        return std::format( "{}: {}", RefusalName( refusal.Code ), refusal.Detail );
    }

    std::vector<std::byte> EncodeOffer( const OfferHeader& header, const PawnSimulation::HandoverPawn& pawn )
    {
        Wire::Writer w( Code( EMessage::Offer ) );
        WriteKey( w, header.Key );
        w.U32( header.Epoch );
        w.U32( header.To );
        w.U32( header.Step );
        w.U32( header.Gateway );
        w.U32( pawn.Player );
        w.U64( pawn.Account );
        w.U32( pawn.Index );
        w.Pawn( pawn.State );
        w.U8( pawn.LastProcessed ? 1 : 0 );
        w.U32( pawn.LastProcessed.value_or( 0 ) );
        w.Move( pawn.Move );
        // The pending spawn or teleport is part of the pawn (UE: the actor's whole state migrates): the receiver
        // lands it against its own cells, or holds it while they stream.
        w.U8( pawn.Placement ? 1 : 0 );
        if ( pawn.Placement )
        {
            w.Vec( pawn.Placement->Feet );
            w.U8( static_cast<std::uint8_t>( pawn.Placement->Why ) );
            w.U32( pawn.Placement->Waited );
        }
        // PawnSimulation keeps at most InputRing::kCapacity pending steps (MeshOfOne.cpp PlayerInput), the bound
        // DecodeOffer enforces.
        w.U32( static_cast<std::uint32_t>( pawn.Pending.size() ) );
        for ( const InputStep& input : pawn.Pending )
            if ( !w.InputStepFull( input ) )
                return {}; // ERefusal::WireStep: the caller refuses before it holds anything
        return w.Take();
    }

    bool DecodeOffer( std::span<const std::byte> body, OfferHeader& header, PawnSimulation::HandoverPawn& pawn )
    {
        Wire::Reader  r( body );
        std::uint8_t  hasProcessed = 0;
        std::uint32_t processed = 0, pending = 0;
        if ( !ReadKey( r, header.Key ) || !r.U32( header.Epoch ) || !r.U32( header.To ) || !r.U32( header.Step ) ||
             !r.U32( header.Gateway ) || !r.U32( pawn.Player ) || !r.U64( pawn.Account ) || !r.U32( pawn.Index ) ||
             !r.Pawn( pawn.State ) || !r.U8( hasProcessed ) || hasProcessed > 1 || !r.U32( processed ) ||
             !r.Move( pawn.Move ) )
            return false;
        std::uint8_t hasPlacement = 0;
        if ( !r.U8( hasPlacement ) || hasPlacement > 1 )
            return false;
        pawn.Placement.reset();
        if ( hasPlacement )
        {
            using EWhy = PawnSimulation::PendingPlacement::EWhy;
            PawnSimulation::PendingPlacement placement;
            std::uint8_t                     why = 0;
            if ( !r.Vec( placement.Feet ) || !r.U8( why ) || why > static_cast<std::uint8_t>( EWhy::Teleport ) ||
                 !r.U32( placement.Waited ) )
                return false; // a reason past the last one is a malformed record, not a guess
            placement.Why  = static_cast<EWhy>( why );
            pawn.Placement = placement;
        }
        if ( !r.U32( pending ) || pending > InputRing::kCapacity )
            return false;
        pawn.LastProcessed.reset();
        if ( hasProcessed )
            pawn.LastProcessed = processed;
        pawn.Pending.resize( pending );
        for ( InputStep& input : pawn.Pending )
            if ( !r.InputStepFull( input ) )
                return false;
        return r.Done();
    }

    std::vector<std::byte> EncodeAccept( const Verdict& verdict )
    {
        return EncodeVerdictAs( EMessage::Accept, verdict, nullptr );
    }
    std::vector<std::byte> EncodeRefuse( const Verdict& verdict, const Refusal& refusal )
    {
        return EncodeVerdictAs( EMessage::Refuse, verdict, &refusal );
    }
    std::vector<std::byte> EncodeWithdraw( const Verdict& verdict )
    {
        return EncodeVerdictAs( EMessage::Withdraw, verdict, nullptr );
    }

    bool DecodeVerdict( std::span<const std::byte> body, Verdict& verdict, Refusal* refusal )
    {
        Wire::Reader r( body );
        if ( !ReadKey( r, verdict.Key ) || !r.U32( verdict.Sim ) || !r.U32( verdict.Step ) )
            return false;
        if ( refusal && !ReadRefusal( r, *refusal ) )
            return false;
        return r.Done();
    }

    std::vector<std::byte> EncodeCommit( const Decision& decision )
    {
        return EncodeDecisionAs( EMessage::Commit, decision, nullptr );
    }
    std::vector<std::byte> EncodeCommitted( const Decision& decision )
    {
        return EncodeDecisionAs( EMessage::Committed, decision, nullptr );
    }
    std::vector<std::byte> EncodeRefused( const Decision& decision, const Refusal& refusal )
    {
        return EncodeDecisionAs( EMessage::Refused, decision, &refusal );
    }

    bool DecodeDecision( std::span<const std::byte> body, Decision& decision, Refusal* refusal )
    {
        Wire::Reader r( body );
        if ( !ReadKey( r, decision.Key ) || !r.U32( decision.Id ) || !r.U32( decision.Epoch ) ||
             !r.U32( decision.Step ) )
            return false;
        if ( refusal && !ReadRefusal( r, *refusal ) )
            return false;
        return r.Done();
    }
} // namespace Desert::Net::Handover
