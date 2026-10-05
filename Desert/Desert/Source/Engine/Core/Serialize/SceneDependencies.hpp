#pragma once

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Core
{
    // THE ONE RULE FOR A SCENE'S Header.Dependencies (SCENE-DEPS), read off the stored records so the save and
    // SceneMigrator cannot gather two different lists: the save calls it on the records it is about to write,
    // the migrator on the records it read, both with the scene's assets root.
    //
    // A dependency is every asset GUID the records state - a {Guid, Path} reference, MeshGuid, MaterialGuids,
    // a hosted block's own header Dependencies (a UI sequence's sounds; the header's Guid is its identity, not
    // a reference) - and every string that names a file under an ancestor of `assetsRoot` whose header states
    // a GUID (a path-only slot). A prefab instance depends on the prefab ITSELF (UE: a hard reference to the
    // package), never on what its overrides name. Sorted, unique.
    struct SceneDependencyGather
    {
        std::vector<std::string> Guids;
        std::vector<std::string> Refused; // a prefab link whose file states no GUID
    };

    [[nodiscard]] SceneDependencyGather GatherSceneDependencies( const std::vector<Assets::EntityData>& entities,
                                                                 const Common::Json::Value*             settings,
                                                                 const std::filesystem::path& assetsRoot );

    // The GUID the header of the file `statedPath` names (looked up under `assetsRoot` and its ancestors), or
    // nothing when no such file exists or it states no GUID.
    [[nodiscard]] std::optional<std::string> StatedPathGuid( const std::string&           statedPath,
                                                             const std::filesystem::path& assetsRoot );
} // namespace Desert::Core
