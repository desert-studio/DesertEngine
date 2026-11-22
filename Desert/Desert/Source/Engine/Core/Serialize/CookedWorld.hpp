#pragma once

// A GAME'S WORLD AS ITS COOK LEFT IT, read up to the point where it can be streamed: the client's WorldStreamer
// and the dedicated server (DesertServer, NET-ARCH §6) both start from this, so both read one cook the same way.
// No scene, no entity, no renderer: render-free, which is why it lives here and not in WorldStreamer.

#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Desert::Core
{
    // The on-demand REGION chunk of every cell file the packager divided off the base (file -> chunk, WP23).
    using RegionChunks = std::shared_ptr<const std::map<std::string, std::string>>;

    // A cooked file's bytes through the VFS, so a pak and a loose directory read the same way. Safe on JobSystem
    // workers (a pak read opens its own stream, PakFile.cpp). A file the packager shipped in an on-demand region
    // chunk mounts that chunk first - once, thread-safe; with no chunks registered (editor, loose cooked files,
    // the server) the mount does nothing.
    [[nodiscard]] WorldCells::FileReader CookedFileReader( std::string directory, RegionChunks regions );

    // THE COOK HASH (NET-ARCH §8, the NET-1 handshake): the identity of one cook of one world, so a client and a
    // server agree they stream the same cells. FNV-1a 64 of the index file's bytes - the index names every cell
    // file with its size and checksum (WorldCells::IndexFile), so any re-cooked cell changes it.
    [[nodiscard]] std::uint64_t CookHashOf( std::span<const unsigned char> indexBytes );

    // A game's world as its cook left it: the index, the plan residency needs, and the always-loaded part as
    // scene JSON - the scene-wide part and the always-loaded records - which the ordinary loader
    // (SceneSerializer::DeserializeFromJson) takes. What the runtime loads instead of the .desce.
    struct CookedWorldStart
    {
        std::string                             Directory; // WorldCells::CookedWorldDirectory of the scene
        std::shared_ptr<WorldCells::WorldIndex> Index;
        WorldCells::IndexedWorld                Indexed;
        std::string                             AlwaysLoadedJson;
        // WP23: the on-demand REGION chunk of every cell file the packager divided off the base (file -> chunk,
        // from WorldCells::ChunkLayoutOf); empty when the world states no ChunkRegionSize. Files of the world's
        // Shared chunk are absent: it is mounted at start.
        RegionChunks  RegionChunkOfFile;
        std::uint64_t CookHash = 0; // CookHashOf the index
    };

    // Whether @p scenePath has a cooked world beside it (through the VFS: a pak or the disk) and, when it has,
    // that world read up to its always-loaded part. nullopt - and nothing read - when there is no index there;
    // an index that is there but cannot be read, or whose files do not match it, is an error naming the file.
    [[nodiscard]] Common::ResultStr<std::optional<CookedWorldStart>>
    ReadCookedWorld( const std::string& scenePath );
} // namespace Desert::Core
