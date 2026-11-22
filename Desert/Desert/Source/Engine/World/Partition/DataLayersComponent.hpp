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
    // WHICH DATA LAYERS OF A PARTITIONED WORLD THIS ENTITY BELONGS TO - UE's AActor::DataLayerAssets
    // (WP-DATA-LAYERS). Names of the world's layers (Core::WorldPartitionSerialized::DataLayers); absent = no layer,
    // so there is no present-and-empty second spelling of "none" the editor would keep writing. A composite
    // belongs to its ANCHOR's layers (a parent never leaves without its children); a member that states other
    // layers, or a name the world has no layer of, is an Issue of the world's plan with the entity's path
    // (Rules::PlanWorldPartition). What the layers decide is what the editor holds (Core::EditorRegions).
    struct DataLayersData
    {
        REFLECT()

        // Hidden: Details draws it as a pick of THIS world's layer names (ComponentEditorRegistrations.cpp,
        // MakeDataLayersEntry), as UE's Details lists an actor's Data Layers by asset - a typed list would invite
        // a name no layer has.
        PROPERTY( DisplayName( "Data Layers" ), Category( "World Partition" ),
                  Tooltip( "The World Partition data layers this entity belongs to, by name." ), Hidden )
        std::vector<std::string> Layers;
    };

    struct DataLayersComponent
    {
        COMPONENT( Key( "DataLayers" ), Block( Data ), Run( ActorsAndUI ) )
        DataLayersData Data;
    };
} // namespace Desert::ECS
