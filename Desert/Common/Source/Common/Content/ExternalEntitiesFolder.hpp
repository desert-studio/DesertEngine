#pragma once

// WHERE A PARTITIONED SCENE KEEPS ITS ENTITY FILES (SCNE v35, WP16) AND ITS BUILT HLODS - the one statement of
// the layout rule of every folder named after a scene file.
//
// It lives in Common and not beside the scene writer (Engine/Core/Serialize/ExternalEntities.hpp) because two
// layers need it: the writer and the loader place and find the pieces by it, and the content browser's move
// (Common/Content/AssetMove.hpp) must carry the folder along when the scene it belongs to is renamed or moved.
// The folder is a function of the scene's PATH, so a scene that moves without it would load with every entity
// file "missing" - and a stale folder left behind would be adopted by the next scene given the old name.

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace Common::Content
{
    inline constexpr std::string_view kExternalEntitiesFolder = "__ExternalEntities__";

    // `<scene dir>/__ExternalEntities__/<scene file stem>` - every entity file of the scene at `scenePath`.
    [[nodiscard]] inline std::filesystem::path
    ExternalEntitiesDirectoryOf( const std::filesystem::path& scenePath )
    {
        return scenePath.parent_path() / kExternalEntitiesFolder / scenePath.stem();
    }

    // `<scene dir>/<scene file stem>_HLOD` - the world's built HLODs (WorldCells::BuiltHLODDirectory, WP-FAR-6B):
    // the index `HLODs.dhlod`, one record file per cell and the cell materials and textures baked for them.
    inline constexpr std::string_view kBuiltHLODFolderSuffix = "_HLOD";

    [[nodiscard]] inline std::filesystem::path BuiltHLODDirectoryOf( const std::filesystem::path& scenePath )
    {
        return scenePath.parent_path() / ( scenePath.stem().string() + std::string( kBuiltHLODFolderSuffix ) );
    }

    // THE FOLDERS A SCENE FILE TAKES WITH IT (WP-ASSET-OPS; UE's external packages of a map: Duplicate and Save
    // As of a World Partition map copy its One File Per Actor packages, Delete deletes them). Both are a function
    // of the scene's PATH, so a copy, a delete or a move of the scene file alone would leave the new name without
    // them and the old one with folders the next scene given that name would adopt:
    //   - the entity folder (ExternalEntitiesDirectoryOf), which also holds the descriptor index
    //     (DescriptorIndex::PathOf) - the scene without it loads with every listed entity file missing;
    //   - the built HLODs (BuiltHLODDirectoryOf) - the world without them streams with every HLOD stale.
    // The one list the content browser's Duplicate / Paste / Delete (Editor AssetFileOps) and the trash
    // (AssetTrash) walk; a folder that is not on disk is simply not there to carry.
    [[nodiscard]] inline std::array<std::filesystem::path, 2>
    SceneCompanionDirectoriesOf( const std::filesystem::path& scenePath )
    {
        return { ExternalEntitiesDirectoryOf( scenePath ), BuiltHLODDirectoryOf( scenePath ) };
    }
} // namespace Common::Content
