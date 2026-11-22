#pragma once

// THE COOK HASH (NET-ARCH §8, the NET-1 handshake): the identity of one cook of one world, so a client and a
// server agree they stream the same cells. FNV-1a 64 of the index file's bytes - the index names every cell file
// with its size and checksum (WorldCells::IndexFile), so any re-cooked cell changes it. DEPENDENCY-FREE ON
// PURPOSE: the network layer and its tools (DesertNetProbe) compute it from a cooked world directory without
// linking the cook (CookedWorld.hpp reads the index through the VFS and states the same hash).

#include <cstdint>
#include <span>
#include <string_view>

namespace Desert::Core
{
    namespace WorldCells
    {
        // The cooked world's index file, inside the world directory (CookedWorldDirectory).
        inline constexpr std::string_view kIndexFileName = "World.dwindex";
    } // namespace WorldCells

    [[nodiscard]] constexpr std::uint64_t CookHashOf( std::span<const unsigned char> indexBytes )
    {
        std::uint64_t hash = 14695981039346656037ull;
        for ( const unsigned char byte : indexBytes )
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }
} // namespace Desert::Core
