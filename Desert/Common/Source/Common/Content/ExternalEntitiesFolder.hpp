#pragma once

// WHERE A PARTITIONED SCENE KEEPS ITS ENTITY FILES (SCNE v35, WP16) - the one statement of the layout rule.
//
// It lives in Common and not beside the scene writer (Engine/Core/Serialize/ExternalEntities.hpp) because two
// layers need it: the writer and the loader place and find the pieces by it, and the content browser's move
// (Common/Content/AssetMove.hpp) must carry the folder along when the scene it belongs to is renamed or moved.
// The folder is a function of the scene's PATH, so a scene that moves without it would load with every entity
// file "missing" - and a stale folder left behind would be adopted by the next scene given the old name.

#include <filesystem>
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
} // namespace Common::Content
