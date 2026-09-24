#pragma once

#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief Where a landscape tile's heights live on disk, and the read and write of that file.
     *
     * Apart from LandscapeLayout.hpp because this half touches the file system (Common's FileSystem: disk
     * first, then the mounted paks) and the tiling half must not — WorldPartition places tiles with the
     * tiling functions and is a pure function of the parsed records.
     */

    /// The extension of a tile's height file. The container's magic is DLHT; the extension says the same.
    inline constexpr const char* kLandscapeTileExtension = ".dlht";

    /**
     * @brief Where a scene saved at @p scenePath keeps the heights of the tile whose entity id is @p tileId:
     *        `<scene dir>/<scene stem>_Landscape/<tileId>.dlht`.
     *
     * DERIVED FROM THE SCENE, NOT CARRIED OVER. A scene saved under a new name writes its tiles under the
     * new name; if the path were kept from the last load, "Save As" would write the new scene's heights over
     * the old scene's files and the two levels would share one terrain without anyone deciding so.
     *
     * By the entity id and not the tile coordinate: the id is what the scene already uses as a tile's
     * identity, and it cannot collide between two landscapes in one scene the way (0, 0) would.
     *
     * Inline, because it is a pure function of two values: the scene migrator names the files of the tiles
     * it bakes with it (Tools/SceneMigrator, v22 -> v23) and must not pull the disk-and-pak half below in.
     */
    inline std::filesystem::path LandscapeTileBlobPath( const std::filesystem::path& scenePath, uint64_t tileId )
    {
        const std::string directory = scenePath.stem().string() + "_Landscape";
        const std::string file      = std::to_string( tileId ) + kLandscapeTileExtension;
        return scenePath.parent_path() / directory / file;
    }

    /// Encodes @p tile and writes it to @p path (write-then-rename, creating the directory). The error names
    /// the file.
    Common::BoolResultStr WriteLandscapeTileFile( const std::filesystem::path& path,
                                                  const LandscapeTileData&     tile );

    /// Reads and decodes @p path — from disk, else from the mounted paks. The error names the file and, for a
    /// blob that is there but wrong, the reason DecodeLandscapeTile gave.
    Common::ResultStr<LandscapeTileData> ReadLandscapeTileFile( const std::filesystem::path& path );
} // namespace Desert::World::Landscape
