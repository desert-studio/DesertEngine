// BuildPool platform: the parts of Platform.hpp that need no OS header.
#include "BuildPool/Platform.hpp"

#include "Internal.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace buildpool
{
    std::string ToHex( const Digest& digest )
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string           text;
        text.reserve( digest.size() * 2 );
        for ( const uint8_t byte : digest )
        {
            text.push_back( kDigits[byte >> 4] );
            text.push_back( kDigits[byte & 0x0F] );
        }
        return text;
    }

    uint64_t MonotonicMs() noexcept
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::milliseconds>( now ).count() );
    }

    uint64_t UnixSeconds() noexcept
    {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::seconds>( now ).count() );
    }

    namespace detail
    {
        std::string EndpointHash( std::string_view loweredHome )
        {
            Sha256 hash;
            hash.Update( loweredHome );
            return ToHex( hash.Finish() ).substr( 0, 16 );
        }

        Result<std::vector<std::byte>> Frame( std::span<const std::byte> message )
        {
            if ( message.size() > kMaxMessageBytes )
            {
                return Fail( "message of " + std::to_string( message.size() ) + " bytes is over the 1 MiB limit" );
            }
            const auto             size = static_cast<uint32_t>( message.size() );
            std::vector<std::byte> framed;
            framed.reserve( kFrameHeaderBytes + message.size() );
            for ( size_t i = 0; i < kFrameHeaderBytes; ++i )
            {
                framed.push_back( static_cast<std::byte>( ( size >> ( 8 * i ) ) & 0xFFu ) );
            }
            framed.insert( framed.end(), message.begin(), message.end() );
            return framed;
        }

        void FrameReader::Append( std::span<const std::byte> bytes )
        {
            // Consumed bytes are dropped once they are the larger part of the buffer, so it never grows unbounded.
            if ( m_offset > 0 && m_offset * 2 >= m_bytes.size() )
            {
                m_bytes.erase( m_bytes.begin(), m_bytes.begin() + static_cast<std::ptrdiff_t>( m_offset ) );
                m_offset = 0;
            }
            m_bytes.insert( m_bytes.end(), bytes.begin(), bytes.end() );
        }

        Result<std::optional<std::vector<std::byte>>> FrameReader::Next()
        {
            const size_t available = m_bytes.size() - m_offset;
            if ( available < kFrameHeaderBytes )
            {
                return std::optional<std::vector<std::byte>>();
            }
            uint32_t size = 0;
            for ( size_t i = 0; i < kFrameHeaderBytes; ++i )
            {
                size |= static_cast<uint32_t>( m_bytes[m_offset + i] ) << ( 8 * i );
            }
            if ( size > kMaxMessageBytes )
            {
                return Fail( "a frame announces " + std::to_string( size ) + " bytes, over the 1 MiB limit" );
            }
            if ( available < kFrameHeaderBytes + size )
            {
                return std::optional<std::vector<std::byte>>();
            }
            const auto first = m_bytes.begin() + static_cast<std::ptrdiff_t>( m_offset + kFrameHeaderBytes );
            std::vector<std::byte> message( first, first + size );
            m_offset += kFrameHeaderBytes + size;
            return std::optional<std::vector<std::byte>>( std::move( message ) );
        }
    } // namespace detail
} // namespace buildpool
