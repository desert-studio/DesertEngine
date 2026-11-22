#pragma once

// The prefab reader a cook and the world cells are handed (PrefabExpansion.hpp, WorldCells.hpp), in its own header
// so WorldCells.hpp names it without the scene format the expansion's result holds (BUILD-MOD-3).

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <string_view>

namespace Desert::Core::WorldCells
{
    // Reads the prefab a record states (EntityData::PrefabPath spelling). The tool and the packager read the file
    // (Assets::ResolvePrefabPath + Assets::ParseLoadablePrefab); a suite answers from a table.
    using PrefabReader = std::function<Common::ResultStr<Assets::PrefabData>( std::string_view statedPath )>;
} // namespace Desert::Core::WorldCells
