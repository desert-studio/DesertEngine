// Ported from UE 5.8 Engine/Source/Runtime/Engine/Public/Allocator2D.h / Private/Allocator2D.cpp (FAllocator2D),
// adapted as Allocator2D.hpp states.
#include <Engine/Geometry/Allocator2D.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>

namespace Desert::Geometry
{
    Allocator2D::Allocator2D( const uint32_t width, const uint32_t height )
         : m_Width( width ), m_Height( height ), m_Pitch( ( width + 63u ) / 64u ),
           m_Bits( static_cast<std::size_t>( ( width + 63u ) / 64u ) * height, 0u )
    {
    }

    void Allocator2D::Clear()
    {
        std::fill( m_Bits.begin(), m_Bits.end(), 0u );
    }

    bool Allocator2D::Test( const uint32_t x, const uint32_t y ) const
    {
        if ( x >= m_Width || y >= m_Height )
            return false;
        return ( ( m_Bits[static_cast<std::size_t>( y ) * m_Pitch + ( x >> 6u )] >> ( x & 63u ) ) & 1u ) != 0u;
    }

    void Allocator2D::SetBit( const uint32_t x, const uint32_t y )
    {
        if ( x >= m_Width || y >= m_Height )
            return;
        m_Bits[static_cast<std::size_t>( y ) * m_Pitch + ( x >> 6u )] |= uint64_t{ 1 } << ( x & 63u );
    }

    uint32_t Allocator2D::CountSet() const
    {
        uint32_t count = 0;
        for ( const uint64_t word : m_Bits )
            count += static_cast<uint32_t>( std::popcount( word ) );
        return count;
    }

    uint64_t Allocator2D::RowBitsAt( const uint32_t y, const uint32_t x ) const
    {
        const uint32_t  word  = x >> 6u;
        const uint32_t  shift = x & 63u;
        const uint64_t* row   = m_Bits.data() + static_cast<std::size_t>( y ) * m_Pitch;
        const uint64_t  low   = word < m_Pitch ? row[word] : 0u;
        if ( shift == 0u )
            return low;
        const uint64_t high = word + 1u < m_Pitch ? row[word + 1u] : 0u;
        return ( low >> shift ) | ( high << ( 64u - shift ) );
    }

    bool Allocator2D::Fits( const Rect& rect, const Allocator2D& other ) const
    {
        if ( uint64_t{ rect.X } + other.m_Width > m_Width || uint64_t{ rect.Y } + other.m_Height > m_Height )
            return false;
        for ( uint32_t y = 0; y < other.m_Height; ++y )
            for ( uint32_t word = 0; word < other.m_Pitch; ++word )
            {
                const uint64_t theirs = other.m_Bits[static_cast<std::size_t>( y ) * other.m_Pitch + word];
                if ( theirs != 0u && ( RowBitsAt( rect.Y + y, rect.X + word * 64u ) & theirs ) != 0u )
                    return false;
            }
        return true;
    }

    bool Allocator2D::Find( Rect& rect, const Allocator2D& other ) const
    {
        if ( other.m_Width > m_Width || other.m_Height > m_Height )
            return false;
        for ( uint32_t y = 0; y + other.m_Height <= m_Height; ++y )
            for ( uint32_t x = 0; x + other.m_Width <= m_Width; ++x )
            {
                const Rect at{ x, y, other.m_Width, other.m_Height };
                if ( Fits( at, other ) )
                {
                    rect = at;
                    return true;
                }
            }
        return false;
    }

    void Allocator2D::Alloc( const int32_t x, const int32_t y, const Allocator2D& other )
    {
        for ( uint32_t oy = 0; oy < other.m_Height; ++oy )
        {
            const int64_t ty = int64_t{ y } + oy;
            if ( ty < 0 || ty >= int64_t{ m_Height } )
                continue;
            for ( uint32_t ox = 0; ox < other.m_Width; ++ox )
            {
                const int64_t tx = int64_t{ x } + ox;
                if ( tx < 0 || tx >= int64_t{ m_Width } || !other.Test( ox, oy ) )
                    continue;
                SetBit( static_cast<uint32_t>( tx ), static_cast<uint32_t>( ty ) );
            }
        }
    }

    Allocator2D Allocator2D::Dilated() const
    {
        Allocator2D out( m_Width + 2u, m_Height + 2u );
        for ( uint32_t y = 0; y < m_Height; ++y )
            for ( uint32_t x = 0; x < m_Width; ++x )
            {
                if ( !Test( x, y ) )
                    continue;
                for ( uint32_t dy = 0; dy < 3u; ++dy )
                    for ( uint32_t dx = 0; dx < 3u; ++dx )
                        out.SetBit( x + dx, y + dy );
            }
        return out;
    }
} // namespace Desert::Geometry
