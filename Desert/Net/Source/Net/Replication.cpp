#include "Replication.hpp"
#include "InputWire.hpp"
#include "WireCodec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <type_traits>

namespace Desert::Net
{
    namespace
    {
        using Wire::Reader;
        using Wire::Writer;

        // Snapshot pawn count bound: a malformed count must not make the decoder reserve gigabytes.
        constexpr std::uint32_t kMaxSnapshotPawns = 4096;

        // Shortest-arc interpolation of a yaw in radians.
        float LerpYaw( float a, float b, float t )
        {
            constexpr float kPi   = 3.14159265358979f;
            float           delta = std::fmod( b - a, 2.f * kPi );
            if ( delta > kPi )
                delta -= 2.f * kPi;
            else if ( delta < -kPi )
                delta += 2.f * kPi;
            return a + delta * t;
        }
    } // namespace

    std::vector<std::byte> EncodeInput( const InputMessage& message )
    {
        Writer out( EReplicationMessage::Input );
        out.U32( message.Sequence );
        // Every step the message holds, as one run (InputWire.hpp); a run the codec refuses (not consecutive, too
        // long, too many axes) sends nothing - the empty buffer is the refusal, the caller reports it.
        std::vector<std::byte> bytes = out.Take();
        if ( !AppendInputRun( bytes, message.Steps ) )
            return {};
        return bytes;
    }

    std::vector<std::byte> EncodeSnapshot( const SnapshotMessage& message )
    {
        Writer out( EReplicationMessage::Snapshot );
        out.U32( message.ServerStep );
        out.U8( message.HasAck ? 1 : 0 );
        out.U32( message.AckInputStep );
        if ( message.HasAck )
        {
            out.Vec( message.OwnState.Position );
            out.Vec( message.OwnState.Velocity );
            out.F32( message.OwnState.Yaw );
            out.U8( MoveFlagsOf( message.OwnState ) );
            out.U8( message.InputBuffered );
        }
        out.U32( static_cast<std::uint32_t>( message.Pawns.size() ) );
        for ( const SnapshotEntry& entry : message.Pawns )
        {
            out.Pawn( entry.Pawn );
            out.U8( entry.PeriodSteps );
        }
        return out.Take();
    }

    std::vector<std::byte> EncodeSpawn( const SpawnMessage& message )
    {
        Writer out( EReplicationMessage::Spawn );
        out.Pawn( message.Pawn );
        out.U8( message.Owned ? 1 : 0 );
        out.U32( message.Player );
        return out.Take();
    }

    std::vector<std::byte> EncodeDestroy( const DestroyMessage& message )
    {
        Writer out( EReplicationMessage::Destroy );
        out.U32( message.Id );
        out.U8( static_cast<std::uint8_t>( message.Reason ) );
        return out.Take();
    }

    std::optional<EReplicationMessage> PeekReplicationMessage( std::span<const std::byte> data )
    {
        if ( data.empty() )
            return std::nullopt;
        return static_cast<EReplicationMessage>( data[0] );
    }

    std::optional<InputMessage> DecodeInput( std::span<const std::byte> data )
    {
        // The message head (type, sequence), then the run to the end of the message (InputWire.hpp).
        constexpr std::size_t kHeadBytes = 1 + sizeof( std::uint32_t );
        if ( data.size() < kHeadBytes )
            return std::nullopt;
        Reader       in( data.first( kHeadBytes ), EReplicationMessage::Input );
        InputMessage message;
        if ( !in.U32( message.Sequence ) || !in.Done() )
            return std::nullopt;
        std::span<const std::byte> run = data.subspan( kHeadBytes );
        if ( !ReadInputRun( run, message.Steps ) || !run.empty() )
            return std::nullopt;
        return message;
    }

    std::optional<SnapshotMessage> DecodeSnapshot( std::span<const std::byte> data )
    {
        Reader          in( data, EReplicationMessage::Snapshot );
        SnapshotMessage message;
        std::uint8_t    hasAck = 0;
        std::uint32_t   count  = 0;
        if ( !in.U32( message.ServerStep ) || !in.U8( hasAck ) || !in.U32( message.AckInputStep ) || hasAck > 1 )
            return std::nullopt;
        message.HasAck = hasAck == 1;
        if ( message.HasAck )
        {
            std::uint8_t grounded = 0;
            if ( !in.Vec( message.OwnState.Position ) || !in.Vec( message.OwnState.Velocity ) ||
                 !in.F32( message.OwnState.Yaw ) || !in.U8( grounded ) || !in.U8( message.InputBuffered ) )
                return std::nullopt;
            SetMoveFlags( message.OwnState, grounded );
        }
        if ( !in.U32( count ) || count > kMaxSnapshotPawns )
            return std::nullopt;
        message.Pawns.resize( count );
        for ( SnapshotEntry& entry : message.Pawns )
            if ( !in.Pawn( entry.Pawn ) || !in.U8( entry.PeriodSteps ) || entry.PeriodSteps == 0 )
                return std::nullopt; // a period of 0 steps is no period: the entry is malformed
        if ( !in.Done() )
            return std::nullopt;
        return message;
    }

    std::optional<SpawnMessage> DecodeSpawn( std::span<const std::byte> data )
    {
        Reader       in( data, EReplicationMessage::Spawn );
        SpawnMessage message;
        std::uint8_t owned = 0;
        if ( !in.Pawn( message.Pawn ) || !in.U8( owned ) || owned > 1 || !in.U32( message.Player ) || !in.Done() ||
             message.Pawn.Id == kInvalidNetId )
            return std::nullopt;
        message.Owned = owned == 1;
        return message;
    }

    std::optional<DestroyMessage> DecodeDestroy( std::span<const std::byte> data )
    {
        Reader         in( data, EReplicationMessage::Destroy );
        DestroyMessage message;
        std::uint8_t   reason = 0;
        if ( !in.U32( message.Id ) || !in.U8( reason ) || !in.Done() || message.Id == kInvalidNetId ||
             reason > static_cast<std::uint8_t>( EDestroyReason::NotRelevant ) )
            return std::nullopt;
        message.Reason = static_cast<EDestroyReason>( reason );
        return message;
    }

    namespace
    {
        // A residency report or a delta lists at most this many entries (a world has a few thousand cells).
        constexpr std::uint32_t kMaxListEntries = 65536;

        void WriteUnits( Writer& out, const std::vector<std::uint32_t>& units )
        {
            out.U32( static_cast<std::uint32_t>( units.size() ) );
            for ( const std::uint32_t unit : units )
                out.U32( unit );
        }
        bool ReadUnits( Reader& in, std::vector<std::uint32_t>& units )
        {
            std::uint32_t count = 0;
            if ( !in.U32( count ) || count > kMaxListEntries )
                return false;
            units.resize( count );
            for ( std::uint32_t& unit : units )
                if ( !in.U32( unit ) )
                    return false;
            return true;
        }
    } // namespace

    std::vector<std::byte> EncodeCellResidency( const CellResidencyMessage& message )
    {
        Writer out( EReplicationMessage::CellResidency );
        WriteUnits( out, message.Loaded );
        WriteUnits( out, message.Unloaded );
        return out.Take();
    }

    std::optional<CellResidencyMessage> DecodeCellResidency( std::span<const std::byte> data )
    {
        Reader               in( data, EReplicationMessage::CellResidency );
        CellResidencyMessage message;
        if ( !in.Ok() || !ReadUnits( in, message.Loaded ) || !ReadUnits( in, message.Unloaded ) || !in.Done() )
            return std::nullopt;
        return message;
    }

    std::vector<std::byte> EncodeStableDestroy( const StableDestroyMessage& message )
    {
        Writer out( EReplicationMessage::StableDestroy );
        out.U32( message.Unit );
        out.U32( static_cast<std::uint32_t>( message.Ids.size() ) );
        for ( const StableNetId id : message.Ids )
            out.U64( id );
        return out.Take();
    }

    std::optional<StableDestroyMessage> DecodeStableDestroy( std::span<const std::byte> data )
    {
        Reader               in( data, EReplicationMessage::StableDestroy );
        StableDestroyMessage message;
        std::uint32_t        count = 0;
        // No ids is the bare residency acknowledgement of the unit (nothing destroyed there), not a malformed
        // list.
        if ( !in.Ok() || !in.U32( message.Unit ) || !in.U32( count ) || count > kMaxListEntries )
            return std::nullopt;
        message.Ids.resize( count );
        for ( StableNetId& id : message.Ids )
            if ( !in.U64( id ) )
                return std::nullopt;
        if ( !in.Done() )
            return std::nullopt;
        return message;
    }

    void InputRing::Push( const InputStep& step )
    {
        m_Steps.push_back( step );
        while ( m_Steps.size() > kCapacity )
            m_Steps.pop_front();
    }

    void InputRing::DropUpTo( std::uint32_t ackStep )
    {
        while ( !m_Steps.empty() && !StepAfter( m_Steps.front().Step, ackStep ) )
            m_Steps.pop_front();
    }

    std::vector<InputStep> InputRing::Unacked() const
    {
        return { m_Steps.begin(), m_Steps.end() };
    }

    void InterpolationBuffer::Push( double serverSeconds, const PawnState& state )
    {
        // A late (reordered) snapshot older than the newest one is dropped: the State lane is unordered.
        if ( !m_Samples.empty() && serverSeconds <= m_Samples.back().Time )
            return;
        m_Samples.push_back( { serverSeconds, state } );
        while ( m_Samples.size() > 2 && m_Samples.front().Time < serverSeconds - 1.0 )
            m_Samples.pop_front();
    }

    PawnState InterpolationBuffer::Sample( double serverSeconds, double extrapolationCap ) const
    {
        if ( m_Samples.empty() )
            return {};
        if ( serverSeconds <= m_Samples.front().Time )
            return m_Samples.front().State;
        if ( serverSeconds >= m_Samples.back().Time )
        {
            // Past the newest snapshot (a late or lost one): on along the newest velocity, capped, then held still.
            const PawnState& newest = m_Samples.back().State;
            const double     ahead  = serverSeconds - m_Samples.back().Time;
            const float      dt     = static_cast<float>( std::min( ahead, extrapolationCap ) );
            PawnState        out    = newest;
            out.Position.X += newest.Velocity.X * dt;
            out.Position.Y += newest.Velocity.Y * dt;
            out.Position.Z += newest.Velocity.Z * dt;
            if ( ahead > extrapolationCap )
                out.Velocity = {}; // held: the locomotion stands rather than walking on the spot
            return out;
        }
        const auto     after = std::find_if( m_Samples.begin(), m_Samples.end(),
                                             [&]( const Sample_& s ) { return s.Time > serverSeconds; } );
        const Sample_& a     = *( after - 1 );
        const Sample_& b     = *after;
        if ( a.State.TeleportCount != b.State.TeleportCount )
            return a.State; // a cut at b's time, never a slide across the teleport
        const double h  = b.Time - a.Time;
        const float  s  = static_cast<float>( ( serverSeconds - a.Time ) / h );
        const float  hs = static_cast<float>( h );
        // Cubic Hermite basis and its derivative (d/ds; divided by h for d/dt).
        const float s2 = s * s, s3 = s2 * s;
        const float h00 = 2.f * s3 - 3.f * s2 + 1.f, h10 = s3 - 2.f * s2 + s, h01 = -2.f * s3 + 3.f * s2,
                    h11 = s3 - s2;
        const float d00 = 6.f * s2 - 6.f * s, d10 = 3.f * s2 - 4.f * s + 1.f, d01 = -6.f * s2 + 6.f * s,
                    d11 = 3.f * s2 - 2.f * s;
        const auto position = [&]( float p0, float v0, float p1, float v1 )
        { return h00 * p0 + h10 * hs * v0 + h01 * p1 + h11 * hs * v1; };
        const auto velocity = [&]( float p0, float v0, float p1, float v1 )
        { return ( d00 * p0 + d10 * hs * v0 + d01 * p1 + d11 * hs * v1 ) / hs; };
        const NetVec3 &pa = a.State.Position, &pb = b.State.Position, &va = a.State.Velocity, &vb = b.State.Velocity;
        PawnState      out;
        out.Id            = b.State.Id;
        out.Position      = { position( pa.X, va.X, pb.X, vb.X ), position( pa.Y, va.Y, pb.Y, vb.Y ),
                              position( pa.Z, va.Z, pb.Z, vb.Z ) };
        out.Velocity      = { velocity( pa.X, va.X, pb.X, vb.X ), velocity( pa.Y, va.Y, pb.Y, vb.Y ),
                              velocity( pa.Z, va.Z, pb.Z, vb.Z ) };
        out.Yaw           = LerpYaw( a.State.Yaw, b.State.Yaw, s );
        out.Movement      = s < 0.5f ? a.State.Movement : b.State.Movement;
        out.TeleportCount = b.State.TeleportCount;
        return out;
    }

    // ---- stable objects (NET-2)
    // ---------------------------------------------------------------------------------

    namespace
    {
        constexpr float         kSmallestThreeRange = 0.70710678f; // 1/sqrt(2): the three smaller components
        constexpr std::uint32_t kRotationBits       = 10;
        constexpr std::uint32_t kRotationMax        = ( 1u << kRotationBits ) - 2; // odd level count: 0 is exact (511)
        constexpr std::uint32_t kMaxObjectsPerState = 1024;
        constexpr std::uint8_t  kMaxFieldsPerObject = 64; // one change-mask bit per field (the gateway's baseline)

        enum class EValueKind : std::uint8_t
        {
            Bool      = 0,
            Int       = 1,
            Float     = 2,
            Transform = 3,
        };

        std::uint32_t QuantizeUnit( float v )
        {
            const float t = std::clamp( ( v + kSmallestThreeRange ) / ( 2.f * kSmallestThreeRange ), 0.f, 1.f );
            return static_cast<std::uint32_t>( std::lround( t * static_cast<float>( kRotationMax ) ) );
        }
        float DequantizeUnit( std::uint32_t q )
        {
            return static_cast<float>( q ) / static_cast<float>( kRotationMax ) * 2.f * kSmallestThreeRange -
                   kSmallestThreeRange;
        }
    } // namespace

    QuantizedTransform QuantizeTransform( const NetTransform& transform )
    {
        QuantizedTransform out;
        const float        position[3] = { transform.Position.X, transform.Position.Y, transform.Position.Z };
        for ( int i = 0; i < 3; ++i )
            out.Millimetres[i] = static_cast<std::int32_t>( std::clamp(
                 std::lround( static_cast<double>( position[i] ) * 10.0 ), -2147483647L, 2147483647L ) );
        std::array<float, 4> q   = transform.Rotation;
        const float          len = std::sqrt( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );
        for ( float& c : q )
            c = len > 0.f ? c / len : 0.f;
        if ( len <= 0.f )
            q[3] = 1.f;
        std::uint32_t largest = 0;
        for ( std::uint32_t i = 1; i < 4; ++i )
            if ( std::fabs( q[i] ) > std::fabs( q[largest] ) )
                largest = i;
        const float sign =
             q[largest] < 0.f ? -1.f : 1.f; // q and -q are one rotation: the largest is sent positive
        std::uint32_t packed = largest << ( 3 * kRotationBits );
        int           slot   = 2;
        for ( std::uint32_t i = 0; i < 4; ++i )
            if ( i != largest )
                packed |= QuantizeUnit( q[i] * sign ) << ( kRotationBits * static_cast<std::uint32_t>( slot-- ) );
        out.Rotation = packed;
        return out;
    }

    NetTransform DequantizeTransform( const QuantizedTransform& quantized )
    {
        NetTransform out;
        out.Position                = { static_cast<float>( quantized.Millimetres[0] ) / 10.f,
                                        static_cast<float>( quantized.Millimetres[1] ) / 10.f,
                                        static_cast<float>( quantized.Millimetres[2] ) / 10.f };
        const std::uint32_t largest = ( quantized.Rotation >> ( 3 * kRotationBits ) ) & 3u;
        float               sum     = 0.f;
        int                 slot    = 2;
        for ( std::uint32_t i = 0; i < 4; ++i )
        {
            if ( i == largest )
                continue;
            out.Rotation[i] = DequantizeUnit(
                 ( quantized.Rotation >> ( kRotationBits * static_cast<std::uint32_t>( slot-- ) ) ) &
                 kRotationMax );
            sum += out.Rotation[i] * out.Rotation[i];
        }
        out.Rotation[largest] = std::sqrt( std::max( 0.f, 1.f - sum ) );
        return out;
    }

    std::string ReplicatedValueText( const ReplicatedValue& value )
    {
        return std::visit(
             []( const auto& v ) -> std::string
             {
                 using T = std::decay_t<decltype( v )>;
                 if constexpr ( std::is_same_v<T, bool> )
                     return v ? "true" : "false";
                 else if constexpr ( std::is_same_v<T, QuantizedTransform> )
                 {
                     const NetTransform t = DequantizeTransform( v );
                     return std::format( "pos ({:.1f}, {:.1f}, {:.1f}) cm rot ({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                                         t.Position.X, t.Position.Y, t.Position.Z, t.Rotation[0], t.Rotation[1],
                                         t.Rotation[2], t.Rotation[3] );
                 }
                 else
                     return std::format( "{}", v );
             },
             value );
    }

    std::vector<std::byte> EncodeObjectState( const ObjectStateMessage& message )
    {
        Writer out( EReplicationMessage::ObjectState );
        out.U32( message.Sequence );
        out.U32( static_cast<std::uint32_t>( message.Objects.size() ) );
        for ( const ObjectUpdate& object : message.Objects )
        {
            out.U64( object.Id );
            out.U8( static_cast<std::uint8_t>( object.Fields.size() ) );
            for ( const FieldUpdate& field : object.Fields )
            {
                out.Str( field.Component );
                out.Str( field.Field );
                out.U32( field.Version );
                out.U8( static_cast<std::uint8_t>( field.Value.index() ) );
                std::visit(
                     [&out]( const auto& v )
                     {
                         using T = std::decay_t<decltype( v )>;
                         if constexpr ( std::is_same_v<T, bool> )
                             out.U8( v ? 1 : 0 );
                         else if constexpr ( std::is_same_v<T, std::int32_t> )
                             out.U32( static_cast<std::uint32_t>( v ) );
                         else if constexpr ( std::is_same_v<T, float> )
                             out.F32( v );
                         else
                         {
                             for ( const std::int32_t mm : v.Millimetres )
                                 out.U32( static_cast<std::uint32_t>( mm ) );
                             out.U32( v.Rotation );
                         }
                     },
                     field.Value );
            }
        }
        return out.Take();
    }

    std::optional<ObjectStateMessage> DecodeObjectState( std::span<const std::byte> data )
    {
        Reader             in( data, EReplicationMessage::ObjectState );
        ObjectStateMessage message;
        std::uint32_t      objects = 0;
        if ( !in.Ok() || !in.U32( message.Sequence ) || !in.U32( objects ) || objects > kMaxObjectsPerState )
            return std::nullopt;
        message.Objects.resize( objects );
        for ( ObjectUpdate& object : message.Objects )
        {
            std::uint8_t fields = 0;
            if ( !in.U64( object.Id ) || !in.U8( fields ) || fields > kMaxFieldsPerObject )
                return std::nullopt;
            object.Fields.resize( fields );
            for ( FieldUpdate& field : object.Fields )
            {
                std::uint8_t kind = 0;
                if ( !in.Str( field.Component ) || !in.Str( field.Field ) || !in.U32( field.Version ) ||
                     !in.U8( kind ) )
                    return std::nullopt;
                switch ( static_cast<EValueKind>( kind ) )
                {
                    case EValueKind::Bool:
                    {
                        std::uint8_t b = 0;
                        if ( !in.U8( b ) || b > 1 )
                            return std::nullopt;
                        field.Value = b == 1;
                        break;
                    }
                    case EValueKind::Int:
                    {
                        std::uint32_t i = 0;
                        if ( !in.U32( i ) )
                            return std::nullopt;
                        field.Value = static_cast<std::int32_t>( i );
                        break;
                    }
                    case EValueKind::Float:
                    {
                        float f = 0.f;
                        if ( !in.F32( f ) )
                            return std::nullopt;
                        field.Value = f;
                        break;
                    }
                    case EValueKind::Transform:
                    {
                        QuantizedTransform t;
                        for ( std::int32_t& mm : t.Millimetres )
                        {
                            std::uint32_t raw = 0;
                            if ( !in.U32( raw ) )
                                return std::nullopt;
                            mm = static_cast<std::int32_t>( raw );
                        }
                        if ( !in.U32( t.Rotation ) )
                            return std::nullopt;
                        field.Value = t;
                        break;
                    }
                    default:
                        return std::nullopt;
                }
            }
        }
        if ( !in.Done() )
            return std::nullopt;
        return message;
    }

    std::vector<std::byte> EncodeStateAck( const StateAckMessage& message )
    {
        Writer out( EReplicationMessage::StateAck );
        out.U32( message.Newest );
        out.U32( message.Previous );
        return out.Take();
    }

    std::optional<StateAckMessage> DecodeStateAck( std::span<const std::byte> data )
    {
        Reader          in( data, EReplicationMessage::StateAck );
        StateAckMessage message;
        if ( !in.Ok() || !in.U32( message.Newest ) || !in.U32( message.Previous ) || !in.Done() )
            return std::nullopt;
        return message;
    }

    void StateAckWindow::Received( std::uint32_t sequence )
    {
        if ( !m_Any )
        {
            m_Any    = true;
            m_Newest = sequence;
            return;
        }
        if ( StepAfter( sequence, m_Newest ) )
        {
            const std::uint32_t shift = sequence - m_Newest;
            // The old newest moves to bit shift - 1; whatever falls past bit 31 leaves the window.
            if ( shift > 32 )
                m_Previous = 0;
            else if ( shift == 32 )
                m_Previous = 1u << 31;
            else
                m_Previous = ( m_Previous << shift ) | ( 1u << ( shift - 1 ) );
            m_Newest = sequence;
            return;
        }
        const std::uint32_t behind = m_Newest - sequence; // 0 = a duplicate of the newest
        if ( behind >= 1 && behind <= 32 )
            m_Previous |= 1u << ( behind - 1 );
    }

    bool StateAckWindow::Acks( const StateAckMessage& ack, std::uint32_t s )
    {
        if ( s == ack.Newest )
            return true;
        const std::uint32_t behind = ack.Newest - s;
        return StepAfter( ack.Newest, s ) && behind <= 32 && ( ack.Previous & ( 1u << ( behind - 1 ) ) ) != 0;
    }
} // namespace Desert::Net
