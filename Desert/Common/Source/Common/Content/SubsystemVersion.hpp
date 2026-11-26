#pragma once

#include <cstdint>

namespace Common::Content
{
    // Four ASCII characters packed so the FIRST character is the lowest byte, i.e. the tag reads
    // correctly in a hex dump of the little-endian file.
    constexpr uint32_t FourCC( const char ( &text )[5] )
    {
        return static_cast<uint32_t>( static_cast<unsigned char>( text[0] ) ) |
               ( static_cast<uint32_t>( static_cast<unsigned char>( text[1] ) ) << 8 ) |
               ( static_cast<uint32_t>( static_cast<unsigned char>( text[2] ) ) << 16 ) |
               ( static_cast<uint32_t>( static_cast<unsigned char>( text[3] ) ) << 24 );
    }

    // One subsystem's serialization version inside an asset: a mesh envelope carries the mesh codec's
    // version AND the material-slot layout's AND the bounds', each owned by a different subsystem.
    struct SubsystemVersion
    {
        uint32_t Tag     = 0;
        uint32_t Version = 0;

        bool operator==( const SubsystemVersion& ) const = default;
    };
} // namespace Common::Content
