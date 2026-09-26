#pragma once

// A PE IMAGE BUILT BYTE BY BYTE FROM THE PE/COFF SPECIFICATION.
//
// One .idata-style section at RVA 0x1000 / file offset 0x200 holding the import descriptors, the
// delay-load descriptors and the name strings. Building the image from the FORMAT rather than from a
// compiler's output is what lets a reader be checked against the specification instead of against
// itself, and it is what lets a test name an import table it could not otherwise arrange -- a release
// CRT, a static CRT, a debug CRT -- without building a binary for each.
//
// SHARED, because there are now two suites that need one and a second copy would be a second PE writer
// to keep in step with the reader: Tests/Common/PeImports checks the reader against it, and
// Tests/Editor/PackagedContent stages a runtime binary with it (PackageGame reads the runtime's import
// table to decide the app-local VC++ redist, Editor/Source/Editor/Packaging/GamePackager.cpp:469, and a
// file that is not a PE -- or one linked against the debug CRT -- is refused there by name).
//
// gtest-free on purpose: it returns bytes and asserts nothing, so each suite keeps its own expectations.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::TestSupport
{
    constexpr std::uint32_t kSectionRva    = 0x1000;
    constexpr std::uint32_t kSectionOffset = 0x200;
    constexpr std::uint32_t kDelayRva      = 0x1100;
    constexpr std::uint32_t kNamesRva      = 0x1200;
    constexpr std::size_t   kFileSize      = 0x600;

    struct PeSpec
    {
        // Defaulted so a case names only the tables it builds.
        std::vector<std::string> Imports{};
        std::vector<std::string> DelayImports{};
        bool                     Pe32 = false;
        // Replaces the first import descriptor's name RVA, to build a corrupt image.
        std::uint32_t BadNameRva = 0;
    };

    inline void Put16( std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v )
    {
        b[at]     = static_cast<std::uint8_t>( v );
        b[at + 1] = static_cast<std::uint8_t>( v >> 8 );
    }

    inline void Put32( std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v )
    {
        for ( int i = 0; i < 4; ++i )
            b[at + i] = static_cast<std::uint8_t>( v >> ( 8 * i ) );
    }

    inline std::size_t At( std::uint32_t rva )
    {
        return kSectionOffset + ( rva - kSectionRva );
    }

    inline std::vector<std::uint8_t> MakePe( const PeSpec& spec )
    {
        std::vector<std::uint8_t> b( kFileSize, 0 );
        b[0] = 'M';
        b[1] = 'Z';
        Put32( b, 0x3C, 0x40 );
        b[0x40]                          = 'P';
        b[0x41]                          = 'E';
        const std::size_t   coff         = 0x44;
        const std::size_t   optional     = coff + 20;
        const std::uint16_t optionalSize = spec.Pe32 ? 0xE0 : 0xF0;
        Put16( b, coff, spec.Pe32 ? 0x14C : 0x8664 );
        Put16( b, coff + 2, 1 );
        Put16( b, coff + 16, optionalSize );
        Put16( b, optional, spec.Pe32 ? 0x10B : 0x20B );
        Put32( b, optional + ( spec.Pe32 ? 92 : 108 ), 16 );
        const std::size_t dirs = optional + ( spec.Pe32 ? 96 : 112 );
        if ( !spec.Imports.empty() )
            Put32( b, dirs + std::size_t{ 1 } * 8, kSectionRva );
        if ( !spec.DelayImports.empty() )
            Put32( b, dirs + std::size_t{ 13 } * 8, kDelayRva );

        const std::size_t section = optional + optionalSize;
        Put32( b, section + 8, 0x400 );
        Put32( b, section + 12, kSectionRva );
        Put32( b, section + 16, 0x400 );
        Put32( b, section + 20, kSectionOffset );

        std::uint32_t nameRva   = kNamesRva;
        auto          writeName = [&]( const std::string& name )
        {
            const std::uint32_t rva = nameRva;
            std::copy( name.begin(), name.end(), b.begin() + static_cast<std::ptrdiff_t>( At( rva ) ) );
            nameRva += static_cast<std::uint32_t>( name.size() + 1 );
            return rva;
        };
        for ( std::size_t i = 0; i < spec.Imports.size(); ++i )
        {
            const std::uint32_t rva = writeName( spec.Imports[i] );
            // A real descriptor also has thunk RVAs; the reader must not need them to find the name.
            Put32( b, At( kSectionRva ) + i * 20 + 12,
                   ( i == 0 && spec.BadNameRva != 0 ) ? spec.BadNameRva : rva );
            Put32( b, At( kSectionRva ) + i * 20 + 16, 0x1300 );
        }
        for ( std::size_t i = 0; i < spec.DelayImports.size(); ++i )
        {
            Put32( b, At( kDelayRva ) + i * 32, 1 ); // attributes: RVA-based
            Put32( b, At( kDelayRva ) + i * 32 + 4, writeName( spec.DelayImports[i] ) );
        }
        return b;
    }
} // namespace Desert::TestSupport
