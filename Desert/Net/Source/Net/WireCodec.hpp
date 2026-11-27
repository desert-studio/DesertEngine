#pragma once

// The byte codec of the replication messages (Replication.hpp, Relevancy.hpp ViewReport): a message is its type
// byte then little-endian fields. Shared by the .cpp files that encode and decode them (Replication.cpp,
// Relevancy.cpp) and by the mesh handover (HandoverWire.cpp, whose type bytes are not EReplicationMessage codes:
// the raw-type Writer and the body Reader); not part of the Net API - nothing outside Desert/Net/Source/Net
// includes it.

#include <Net/InputWire.hpp>
#include <Net/Replication.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Net::Wire
{
    // Little-endian on the wire, whatever the host.
    class Writer
    {
    public:
        explicit Writer( EReplicationMessage type )
        {
            U8( static_cast<std::uint8_t>( type ) );
        }
        // A message whose type byte belongs to another range (HandoverWire.hpp EMessage).
        explicit Writer( std::uint8_t rawType )
        {
            U8( rawType );
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
            Vec( pawn.Velocity );
            U8( pawn.Movement );
            U8( pawn.TeleportCount );
        }
        void Bytes( std::span<const std::byte> bytes )
        {
            m_Out.insert( m_Out.end(), bytes.begin(), bytes.end() );
        }
        // The CharacterMovement state: Position, Velocity, Yaw, the flag byte (Replication.hpp MoveFlagsOf).
        void Move( const PawnMoveState& move )
        {
            Vec( move.Position );
            Vec( move.Velocity );
            F32( move.Yaw );
            U8( MoveFlagsOf( move ) );
        }
        // An input step with its number and every field (InputWire.hpp AppendInputStepFull). False when the wire
        // does not carry the step (nothing appended).
        [[nodiscard]] bool InputStepFull( const InputStep& step )
        {
            const std::size_t before = m_Out.size();
            AppendInputStepFull( m_Out, step );
            return m_Out.size() != before;
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
        // A message body whose type byte the caller already consumed (the mesh handlers).
        explicit Reader( std::span<const std::byte> body ) : m_Data( body )
        {
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
            return U32( pawn.Id ) && Vec( pawn.Position ) && F32( pawn.Yaw ) && Vec( pawn.Velocity ) &&
                   U8( pawn.Movement ) && U8( pawn.TeleportCount );
        }
        bool Move( PawnMoveState& move )
        {
            std::uint8_t flags = 0;
            if ( !( Vec( move.Position ) && Vec( move.Velocity ) && F32( move.Yaw ) && U8( flags ) ) )
                return false;
            SetMoveFlags( move, flags );
            return true;
        }
        bool InputStepFull( InputStep& step )
        {
            std::span<const std::byte> rest = Rest();
            if ( !ReadInputStepFull( rest, step ) )
                return m_Ok = false;
            m_At = m_Data.size() - rest.size();
            return true;
        }
        // The bytes not read yet.
        [[nodiscard]] std::span<const std::byte> Rest() const
        {
            return m_Data.subspan( m_At );
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
} // namespace Desert::Net::Wire
