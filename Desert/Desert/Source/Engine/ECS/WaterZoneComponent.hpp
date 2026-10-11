#pragma once

// A WATER ZONE (UE: AWaterZone + its UWaterMeshComponent). The zone is the region whose water surface is drawn:
// a quadtree of square tiles over ZoneExtent, centred on the entity, each tile an instanced grid whose density
// and LOD follow the camera (Engine/Water/WaterQuadTree.hpp, Graphic/Systems/Scene/Water/WaterRenderer.hpp).
// The water itself is the water bodies inside it (WaterBodyComponent): a tile is drawn where a body's footprint
// covers it, at that body's still level (its entity Y).

#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec2.hpp>

#include <cstdint>

namespace Desert::ECS
{
    struct WaterZoneData
    {
        REFLECT()

        PROPERTY( DisplayName( "Zone Extent" ), Category( "Water Mesh" ), Length,
                  Tooltip( "The footprint of the zone around the entity, X by Z (UE ZoneExtent). Water bodies are "
                           "drawn inside it and nowhere else." ) )
        glm::vec2 ZoneExtent{ 51200.0f, 51200.0f };

        PROPERTY( DisplayName( "Tile Size" ), Category( "Water Mesh" ), Length, Range( 100.0f, 100000.0f ),
                  Tooltip( "The side of one leaf tile of the water quadtree, the finest LOD (UE TileSize)." ) )
        float TileSize = 2400.0f;

        PROPERTY( DisplayName( "Tessellation Factor" ), Category( "Water Mesh" ), Range( 1.0f, 12.0f ),
                  Tooltip( "A leaf tile is a grid of 2^factor quads per side (UE TessellationFactor). Coarser "
                           "LODs reuse the grid over larger tiles." ) )
        int32_t TessellationFactor = 6;

        PROPERTY(
             DisplayName( "LOD Scale" ), Category( "Water Mesh" ), Range( 0.5f, 100.0f ),
             Tooltip( "How far each LOD reaches, in leaf tiles: LOD L ends 2^(L+1) x LOD Scale x Tile Size from "
                      "the camera (UE LODScale, at least 0.5)." ) )
        float LODScale = 1.0f;
    };

    struct WaterZoneComponent
    {
        WaterZoneData Data;
    };
} // namespace Desert::ECS
