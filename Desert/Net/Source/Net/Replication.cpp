#include "Replication.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

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
} // namespace Desert::Net
