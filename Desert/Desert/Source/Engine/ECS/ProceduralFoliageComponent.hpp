#pragma once

// A PROCEDURAL FOLIAGE VOLUME (UE: AProceduralFoliageVolume + UProceduralFoliageComponent + its
// UProceduralFoliageSpawner). The box is the entity's TransformComponent translation +- Extent; what grows in it
// is the simulation of World::Foliage::Procedural::ProceduralFoliageSpawner over the listed `.defoliage` types,
// each type's FoliageProcedural block being its UE FoliageType procedural section. Resimulate (the Details button,
// Editor FoliagePaintTool::ResimulateProcedural) traces the desired instances onto the ground and files them into
// FO-6 foliage fields that carry a ProceduralFoliageFieldComponent naming this volume, so a resimulation rewrites
// ITS fields and nothing the brush painted.

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <Common/Core/UUID.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace Desert::ECS
{
    struct ProceduralFoliageData
    {
        REFLECT()

        PROPERTY( DisplayName( "Extent" ), Category( "Procedural Foliage" ), Summary,
                  Tooltip( "Half size of the volume box, cm, around the entity's position (UE: the volume's "
                           "brush bounds). Rotation and scale of the transform are not part of the box." ) )
        glm::vec3 Extent{ 5000.0f, 5000.0f, 5000.0f };

        PROPERTY( DisplayName( "Tile Size" ), Category( "Spawner" ), Range( 100.0f, 100000.0f ),
                  Tooltip( "Side of one simulated tile, cm (UE ProceduralFoliageSpawner TileSize). The tiles are "
                           "anchored to the world, so two volumes with the same spawner meet seamlessly." ) )
        float TileSize = 10000.0f;

        PROPERTY(
             DisplayName( "Minimum Quad Tree Size" ), Category( "Spawner" ), Range( 1.0f, 10000.0f ),
             Tooltip( "The smallest quadtree cell of the simulation's broadphase, cm (UE MinimumQuadTreeSize)." ) )
        float MinimumQuadTreeSize = 100.0f;

        PROPERTY( DisplayName( "Num Unique Tiles" ), Category( "Spawner" ), Range( 1, 100 ),
                  Tooltip( "How many different tiles are simulated; the world repeats them in a hashed pattern "
                           "(UE NumUniqueTiles)." ) )
        int32_t NumUniqueTiles = 10;

        PROPERTY( DisplayName( "Random Seed" ), Category( "Spawner" ),
                  Tooltip( "The seed every tile's simulation is drawn from (UE RandomSeed)." ) )
        int32_t RandomSeed = 42;

        PROPERTY( DisplayName( "Tile Overlap" ), Category( "Procedural Foliage" ), Range( 0.0f, 10000.0f ),
                  Tooltip( "How far, cm, a tile looks into its neighbours when it is placed (UE TileOverlap); 0 "
                           "leaves the tiles independent." ) )
        float TileOverlap = 0.0f;

        PROPERTY( DisplayName( "Allow Landscape" ), Category( "Procedural Foliage" ),
                  Tooltip( "Instances may land on a landscape (UE bAllowLandscape)." ) )
        bool AllowLandscape = true;

        PROPERTY( DisplayName( "Allow Static Mesh" ), Category( "Procedural Foliage" ),
                  Tooltip( "Instances may land on static meshes (UE bAllowStaticMesh)." ) )
        bool AllowStaticMesh = true;

        // Drawn by the component's own Details entry (a list with a drop target), not by the generic builder.
        PROPERTY( DisplayName( "Foliage Types" ), Category( "Spawner" ), Asset<FoliageTypeAsset>, Hidden,
                  Tooltip( "The .defoliage types the spawner simulates (UE FoliageTypes)." ) )
        std::vector<Assets::AssetHandle> FoliageTypes;
    };

    struct ProceduralFoliageComponent
    {
        COMPONENT( Key( "ProceduralFoliage" ), Block( Data ), Run( SkyAndAtmosphere ) )
        ProceduralFoliageData Data;
    };

    // On a foliage field the volume `Owner` generated (UE: instances carrying the component's ProceduralGuid).
    // The brush does not paint into such a field and a resimulation of another volume does not touch it.
    struct ProceduralFoliageFieldComponent
    {
        Common::UUID Owner = Common::UUID::Null();
    };
} // namespace Desert::ECS
