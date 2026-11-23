#include "Replication.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <type_traits>

namespace Desert::Net
{
    namespace
    {
        // Little-endian on the wire, whatever the host.
        class Writer
        {
        public:
            explicit Writer( EReplicationMessage type )
            {
                U8( static_cast<std::uint8_t>( type ) );
            }
            void U8( std::uint8_t value )
            {
                m_Out.push_back( static_cast<std::byte>( value ) );
            }
            void U32( std::uint32_t value )
            {
                for ( int i = 0; i < 4; ++i )
                    m_Out.push_back( static_cast<std::byte>( ( value >> ( 8 * i ) ) & 0xFFu ) );
            }
            void F32( float value )
            {
                U32( std::bit_cast<std::uint32_t>( value ) );
            }
            void U64( std::uint64_t value )
            {
                U32( static_cast<std::uint32_t>( value & 0xFFFFFFFFu ) );
                U32( static_cast<std::uint32_t>( value >> 32 ) );
            }
            void Str( std::string_view text ) // u8 length: keys and field names are short
            {
                U8( static_cast<std::uint8_t>( std::min<std::size_t>( text.size(), 255 ) ) );
                for ( std::size_t i = 0; i < text.size() && i < 255; ++i )
                    m_Out.push_back( static_cast<std::byte>( text[i] ) );
            }
            [[nodiscard]] std::size_t Size() const
            {
                return m_Out.size();
            }
            void Vec( const NetVec3& v )
            {
                F32( v.X );
                F32( v.Y );
                F32( v.Z );
            }
            void Pawn( const PawnState& pawn )
            {
                U32( pawn.Id );
                Vec( pawn.Position );
                F32( pawn.Yaw );
            }
            std::vector<std::byte> Take()
            {
                return std::move( m_Out );
            }

        private:
            std::vector<std::byte> m_Out;
        };

        class Reader
        {
        public:
            Reader( std::span<const std::byte> data, EReplicationMessage type ) : m_Data( data )
            {
                std::uint8_t first = 0;
                m_Ok               = U8( first ) && first == static_cast<std::uint8_t>( type );
            }
            bool U8( std::uint8_t& value )
            {
                if ( m_At + 1 > m_Data.size() )
                    return m_Ok = false;
                value = static_cast<std::uint8_t>( m_Data[m_At++] );
                return true;
            }
            bool U32( std::uint32_t& value )
            {
                if ( m_At + 4 > m_Data.size() )
                    return m_Ok = false;
                value = 0;
                for ( int i = 0; i < 4; ++i )
                    value |= static_cast<std::uint32_t>( m_Data[m_At + i] ) << ( 8 * i );
                m_At += 4;
                return true;
            }
            bool F32( float& value )
            {
                std::uint32_t bits = 0;
                if ( !U32( bits ) )
                    return false;
                value       = std::bit_cast<float>( bits );
                return m_Ok = m_Ok && std::isfinite( value );
            }
            bool Vec( NetVec3& v )
            {
                return F32( v.X ) && F32( v.Y ) && F32( v.Z );
            }
            bool U64( std::uint64_t& value )
            {
                std::uint32_t low = 0, high = 0;
                if ( !U32( low ) || !U32( high ) )
                    return false;
                value = ( static_cast<std::uint64_t>( high ) << 32 ) | low;
                return true;
            }
            bool Str( std::string& text )
            {
                std::uint8_t length = 0;
                if ( !U8( length ) || m_At + length > m_Data.size() )
                    return m_Ok = false;
                text.assign( reinterpret_cast<const char*>( m_Data.data() + m_At ), length );
                m_At += length;
                return !text.empty() || ( m_Ok = false );
            }
            bool Pawn( PawnState& pawn )
            {
                return U32( pawn.Id ) && Vec( pawn.Position ) && F32( pawn.Yaw );
            }
            // Whole message read, nothing left over.
            [[nodiscard]] bool Done() const
            {
                return m_Ok && m_At == m_Data.size();
            }
            [[nodiscard]] bool Ok() const
            {
                return m_Ok;
            }

        private:
            std::span<const std::byte> m_Data;
            std::size_t                m_At = 0;
            bool                       m_Ok = true;
        };

        // Snapshot pawn count bound: a malformed count must not make the decoder reserve gigabytes.
        constexpr std::uint32_t kMaxSnapshotPawns = 4096;

        float LerpYaw( float a, float b, float t )
        {
            float delta = std::fmod( b - a, 360.f );
            if ( delta > 180.f )
                delta -= 360.f;
            else if ( delta < -180.f )
                delta += 360.f;
            return a + delta * t;
        }
    } // namespace

    std::vector<std::byte> EncodeInput( const InputMessage& message )
    {
        Writer out( EReplicationMessage::Input );
        out.U32( message.Sequence );
        const std::size_t count = std::min<std::size_t>( message.Steps.size(), kInputRedundancy );
        out.U8( static_cast<std::uint8_t>( count ) );
        for ( std::size_t i = message.Steps.size() - count; i < message.Steps.size(); ++i )
        {
            out.U32( message.Steps[i].Step );
            out.Vec( message.Steps[i].Move );
            out.F32( message.Steps[i].Yaw );
        }
        return out.Take();
    }

    std::vector<std::byte> EncodeSnapshot( const SnapshotMessage& message )
    {
        Writer out( EReplicationMessage::Snapshot );
        out.U32( message.ServerStep );
        out.U8( message.HasAck ? 1 : 0 );
        out.U32( message.AckInputStep );
        out.U32( static_cast<std::uint32_t>( message.Pawns.size() ) );
        for ( const PawnState& pawn : message.Pawns )
            out.Pawn( pawn );
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

    std::vector<std::byte> EncodeDestroy( NetId id )
    {
        Writer out( EReplicationMessage::Destroy );
        out.U32( id );
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
        Reader       in( data, EReplicationMessage::Input );
        InputMessage message;
        std::uint8_t count = 0;
        if ( !in.U32( message.Sequence ) || !in.U8( count ) || count == 0 || count > kInputRedundancy )
            return std::nullopt;
        message.Steps.resize( count );
        for ( InputStep& step : message.Steps )
            if ( !in.U32( step.Step ) || !in.Vec( step.Move ) || !in.F32( step.Yaw ) )
                return std::nullopt;
        if ( !in.Done() )
            return std::nullopt;
        return message;
    }

    std::optional<SnapshotMessage> DecodeSnapshot( std::span<const std::byte> data )
    {
        Reader          in( data, EReplicationMessage::Snapshot );
        SnapshotMessage message;
        std::uint8_t    hasAck = 0;
        std::uint32_t   count  = 0;
        if ( !in.U32( message.ServerStep ) || !in.U8( hasAck ) || !in.U32( message.AckInputStep ) ||
             !in.U32( count ) || hasAck > 1 || count > kMaxSnapshotPawns )
            return std::nullopt;
        message.HasAck = hasAck == 1;
        message.Pawns.resize( count );
        for ( PawnState& pawn : message.Pawns )
            if ( !in.Pawn( pawn ) )
                return std::nullopt;
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

    std::optional<NetId> DecodeDestroy( std::span<const std::byte> data )
    {
        Reader in( data, EReplicationMessage::Destroy );
        NetId  id = kInvalidNetId;
        if ( !in.U32( id ) || !in.Done() || id == kInvalidNetId )
            return std::nullopt;
        return id;
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

    std::vector<InputStep> InputRing::Latest( std::size_t count ) const
    {
        const std::size_t n = std::min( count, m_Steps.size() );
        return { m_Steps.end() - static_cast<std::ptrdiff_t>( n ), m_Steps.end() };
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

    PawnState InterpolationBuffer::Sample( double serverSeconds ) const
    {
        if ( m_Samples.empty() )
            return {};
        if ( serverSeconds <= m_Samples.front().Time )
            return m_Samples.front().State;
        if ( serverSeconds >= m_Samples.back().Time )
            return m_Samples.back().State;
        const auto     after = std::find_if( m_Samples.begin(), m_Samples.end(),
                                             [&]( const Sample_& s ) { return s.Time > serverSeconds; } );
        const Sample_& a     = *( after - 1 );
        const Sample_& b     = *after;
        const float    t     = static_cast<float>( ( serverSeconds - a.Time ) / ( b.Time - a.Time ) );
        PawnState      out;
        out.Id         = b.State.Id;
        out.Position.X = a.State.Position.X + ( b.State.Position.X - a.State.Position.X ) * t;
        out.Position.Y = a.State.Position.Y + ( b.State.Position.Y - a.State.Position.Y ) * t;
        out.Position.Z = a.State.Position.Z + ( b.State.Position.Z - a.State.Position.Z ) * t;
        out.Yaw        = LerpYaw( a.State.Yaw, b.State.Yaw, t );
        return out;
    }

    // ---- stable objects (NET-2)
    // ---------------------------------------------------------------------------------

    namespace
    {
        constexpr float         kSmallestThreeRange = 0.70710678f; // 1/sqrt(2): the three smaller components
        constexpr std::uint32_t kRotationBits       = 10;
        constexpr std::uint32_t kRotationMax        = ( 1u << kRotationBits ) - 1;
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
