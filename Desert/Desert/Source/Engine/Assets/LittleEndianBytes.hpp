#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Assets
{
    /// Appends @p v little-endian: the byte order of every asset section and DDC settings image written here.
    inline void PutU32( std::vector<std::byte>& out, const uint32_t v )
    {
        for ( int i = 0; i < 4; ++i )
            out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
    }

    /// Appends @p v little-endian.
    inline void PutU64( std::vector<std::byte>& out, const uint64_t v )
    {
        for ( int i = 0; i < 8; ++i )
            out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
    }
} // namespace Desert::Assets
