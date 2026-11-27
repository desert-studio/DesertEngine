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
        // A read that stopped at byte @p at: the first byte of a field that did not fit, or of a value past its
        // range.
        Decoded CutAt( std::size_t at, std::size_t size )
        {
            return Decoded{ false, at, size };
        }
        // Every field read: Ok when nothing is left over, otherwise the byte the record ended at.
        Decoded End( const Wire::Reader& r, std::size_t size )
        {
            return Decoded{ r.Done(), r.At(), size };
        }

        // The tail of a Verdict or Decision body: the refusal when @p refusal asks for one, then nothing left
        // over. A code past kLastRefusal is a malformed record, not a guess: the read stops AT the code's byte.
        Decoded ReadTail( Wire::Reader& r, std::size_t size, Refusal* refusal )
        {
            if ( refusal )
            {
                std::uint8_t      code = 0;
                const std::size_t at   = r.At();
                if ( !r.U8( code ) || code > static_cast<std::uint8_t>( kLastRefusal ) )
                    return CutAt( at, size );
                refusal->Code = static_cast<ERefusal>( code );
                if ( !r.Str( refusal->Detail ) )
                    return CutAt( r.At(), size );
            }
            return End( r, size );
        }

        std::vector<std::byte> EncodeVerdictAs( EMessage type, const Verdict& verdict, const Refusal* refusal )
        {
            Wire::Writer w( Code( type ) );
            WriteKey( w, verdict.Key );
            w.U32( verdict.Id );
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
        return "UnknownHandoverMessage"; // a byte past IsHandoverMessage, named for the log line that refuses it
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
            case ERefusal::AlreadyOffered:
                return "AlreadyOffered";
            case ERefusal::SerialReused:
                return "SerialReused";
            case ERefusal::PawnMismatch:
                return "PawnMismatch";
            case ERefusal::WrongDirection:
                return "WrongDirection";
        }
        return "UnknownRefusal"; // never read off the wire: ReadTail stops at a code past kLastRefusal
    }

    std::string Describe( const Refusal& refusal )
    {
        return std::format( "{}: {}", RefusalName( refusal.Code ), refusal.Detail );
    }

    // Decoded carries no bit telling a record cut short from a whole record with bytes behind it (both stop at an
    // At below or at Size): the byte is named either way, as the place the record stopped being readable.
    std::string DescribeCut( const Decoded& read )
    {
        return std::format( "cut at byte {} of {}", read.At, read.Size );
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

    DecodedOffer DecodeOffer( std::span<const std::byte> body, OfferHeader& header,
                              PawnSimulation::HandoverPawn& pawn )
    {
        // A field that does not fit leaves the reader at its first byte, so r.At() before a read is where the
        // read stops whether the field did not fit or its value is past its range.
        Wire::Reader      r( body );
        const std::size_t size       = body.size();
        bool              headerRead = false;
        const auto        stop = [&]( std::size_t at ) { return DecodedOffer{ CutAt( at, size ), headerRead }; };
        if ( !ReadKey( r, header.Key ) || !r.U32( header.Epoch ) || !r.U32( header.To ) || !r.U32( header.Step ) ||
             !r.U32( header.Gateway ) )
            return stop( r.At() );
        headerRead = true; // the key to answer under is known from here on

        std::uint8_t  hasProcessed = 0;
        std::uint32_t processed = 0, pending = 0;
        if ( !r.U32( pawn.Player ) || !r.U64( pawn.Account ) || !r.U32( pawn.Index ) || !r.Pawn( pawn.State ) )
            return stop( r.At() );
        if ( const std::size_t at = r.At(); !r.U8( hasProcessed ) || hasProcessed > 1 )
            return stop( at );
        if ( !r.U32( processed ) || !r.Move( pawn.Move ) )
            return stop( r.At() );
        std::uint8_t hasPlacement = 0;
        if ( const std::size_t at = r.At(); !r.U8( hasPlacement ) || hasPlacement > 1 )
            return stop( at );
        pawn.Placement.reset();
        if ( hasPlacement )
        {
            using EWhy = PawnSimulation::PendingPlacement::EWhy;
            PawnSimulation::PendingPlacement placement;
            std::uint8_t                     why = 0;
            if ( !r.Vec( placement.Feet ) )
                return stop( r.At() );
            // A reason past the last one is a malformed record, not a guess.
            if ( const std::size_t at = r.At(); !r.U8( why ) || why > static_cast<std::uint8_t>( EWhy::Teleport ) )
                return stop( at );
            if ( !r.U32( placement.Waited ) )
                return stop( r.At() );
            placement.Why  = static_cast<EWhy>( why );
            pawn.Placement = placement;
        }
        if ( const std::size_t at = r.At(); !r.U32( pending ) || pending > InputRing::kCapacity )
            return stop( at );
        pawn.LastProcessed.reset();
        if ( hasProcessed )
            pawn.LastProcessed = processed;
        pawn.Pending.resize( pending );
        for ( InputStep& input : pawn.Pending )
            if ( !r.InputStepFull( input ) )
                return stop( r.At() );
        return DecodedOffer{ End( r, size ), headerRead };
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

    Decoded DecodeVerdict( std::span<const std::byte> body, Verdict& verdict, Refusal* refusal )
    {
        Wire::Reader r( body );
        if ( !ReadKey( r, verdict.Key ) || !r.U32( verdict.Id ) || !r.U32( verdict.Sim ) ||
             !r.U32( verdict.Step ) )
            return CutAt( r.At(), body.size() );
        return ReadTail( r, body.size(), refusal );
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

    Decoded DecodeDecision( std::span<const std::byte> body, Decision& decision, Refusal* refusal )
    {
        Wire::Reader r( body );
        if ( !ReadKey( r, decision.Key ) || !r.U32( decision.Id ) || !r.U32( decision.Epoch ) ||
             !r.U32( decision.Step ) )
            return CutAt( r.At(), body.size() );
        return ReadTail( r, body.size(), refusal );
    }
} // namespace Desert::Net::Handover
