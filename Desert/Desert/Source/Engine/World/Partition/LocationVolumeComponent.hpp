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
    // A PLACE OF A PARTITIONED WORLD THE EDITOR LOADS BY NAME - UE's ALocationVolume (WP-LOADER-ADAPTERS): its box
    // (the +-Extent box placed by the entity's world transform, seen from above as an X/Z rectangle) is a loader of
    // the World Partition window (Core::EditorRegions): Load holds every composite whose footprint meets it, and it
    // is listed there whether loaded or not. Always loaded in the editor (UE: bIsSpatiallyLoaded = false), so the list never misses one;
    // an editor-only entity - the cook leaves it out (WorldCells), as UE never cooks an editor-only actor.
    struct LocationVolumeData
    {
        REFLECT()

        PROPERTY( DisplayName( "Extent" ), Category( "Location Volume" ), Summary,
                  Tooltip( "Half size of the volume box, cm, around the entity, placed by its transform (rotation "
                           "and scale included); Load holds what meets the box's X/Z rectangle." ) )
        glm::vec3 Extent{ 15000.0f, 15000.0f, 15000.0f };

        PROPERTY( DisplayName( "Auto Load" ), Category( "Location Volume" ),
                  Tooltip( "Loaded when the world is opened in the editor, unless this user unloaded it (UE "
                           "ALocationVolume::bIsAutoLoad)." ) )
        bool AutoLoad = false;
    };

    struct LocationVolumeComponent
    {
        COMPONENT( Key( "LocationVolume" ), Block( Data ), Run( ActorsAndUI ) )
        LocationVolumeData Data;
    };
} // namespace Desert::ECS
