#pragma once

#include <Common/Core/Entt.hpp>

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
    // WHICH HLOD LAYER BUILDS THIS ENTITY'S PART OF ITS CELL'S HLOD - UE's AActor::HLODLayer (WP-SETTINGS-UI, WP v13).
    // Absent = its partition's first HLOD setup's layer (Core::Rules::GridHLODLayer), so there is no
    // present-and-empty second spelling of "the partition's". A composite is built by its ANCHOR's layer; a name the world has no layer
    // of, or a member that states another layer than its anchor, is an Issue of the world's plan with the entity's
    // path (Rules::PlanWorldPartition), and a grid that builds no HLOD refuses Build HLODs for a member that names
    // one (Rules::CompositeHLODLayer).
    struct HLODLayerData
    {
        REFLECT()

        // Hidden: Details draws it as a pick of THIS world's HLOD layers (WorldPartitionDetails.cpp), as UE's
        // Details picks an actor's HLOD Layer asset - a typed name would invite one no layer has.
        PROPERTY( DisplayName( "HLOD Layer" ), Category( "World Partition" ),
                  Tooltip( "The World Partition HLOD layer that builds this entity's part of its cell's HLOD, "
                           "instead of its grid's layer." ),
                  Hidden )
        std::string Layer;
    };

    struct HLODLayerComponent
    {
        COMPONENT( Key( "HLODLayer" ), Block( Data ), Run( ActorsAndUI ) )
        HLODLayerData Data;
    };
} // namespace Desert::ECS
