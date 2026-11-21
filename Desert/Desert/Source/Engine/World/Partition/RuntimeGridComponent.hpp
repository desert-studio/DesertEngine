#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // WHICH RUNTIME GRID OF A PARTITIONED WORLD THIS ENTITY STREAMS ON - UE's AActor::RuntimeGrid (WP22). Absent
    // = the world's first grid, UE's main grid, so there is no present-and-empty second spelling of "main": an
    // empty or unknown name is refused by the planner with the entity's path (Rules::PlanWorldPartition). A
    // composite streams on its anchor's grid; the grid sets the cell size and the loading range it uses.
    struct RuntimeGridData
    {
        REFLECT()

        PROPERTY( DisplayName( "Runtime Grid" ), Category( "World Partition" ),
                  Tooltip( "The name of the World Partition grid this entity streams on; the grid's cell size "
                           "and loading range apply to it." ) )
        std::string Grid;
    };

    struct RuntimeGridComponent
    {
        COMPONENT( Key( "RuntimeGrid" ), Block( Data ), Run( ActorsAndUI ) )
        RuntimeGridData Data;
    };
} // namespace Desert::ECS
