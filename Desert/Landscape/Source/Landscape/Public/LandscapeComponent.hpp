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

#include <Engine/Assets/Common.hpp>
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

namespace Desert::ECS
{
    // THE LANDSCAPE ROOT (UE: ALandscape). Owns the frame every tile is placed by: the entity's own world
    // position is sample (0, 0) of tile (0, 0), and these three say how far apart samples are, how tall a
    // sample step is and how many quads a tile has. It owns NO heights and NO tile list — the tiles are the
    // entities whose LandscapeTileComponent names this entity, and a count here would be a second answer
    // (World/Landscape/LandscapeLayout.hpp says what is deliberately not stored).
    //
    // Rotation and scale of the root entity are not part of the frame: LandscapeFrame has no rotation, as
    // the TES sampling it feeds has none. That is stated here rather than hidden behind a transform the
    // tiles would silently ignore; a landscape that must turn is a new frame field, not a gizmo.
    struct LandscapeComponent
    {
        uint32_t QuadsPerTile = World::Landscape::kLandscapeDefaultTileQuads; // UE section size, 7..255
        float    SpacingCm    = World::Landscape::kLandscapeDefaultSpacingCm; // cm between neighbouring samples
        float    ZScale       = World::Landscape::kLandscapeDefaultZScale;    // cm per local height unit
        // UE target layers, in panel order: each names a `.delayerinfo` (UE: ALandscape's target layer ->
        // ULandscapeLayerInfoObject). Every tile's weight plane is keyed by the asset's LayerName, and the
        // paint stroke reads its Hardness/NoWeightBlend (Runtime::LandscapeLayerInfoService resolves them).
        std::vector<Assets::AssetHandle> Layers;
        // UE edit layers (ALandscape's stack), bottom first: order, name, visibility, lock and alphas. Each
        // layer's data lives on the tiles it touched (LandscapeTileData::EditLayers, in the DLHT blob), and a
        // tile's samples are their merge (MergeLandscapeEditLayers).
        World::Landscape::LandscapeEditLayerStack EditLayers;
        // No splines here: every spline is its own entity (LandscapeSplineComponent below, UE 5.5
        // ALandscapeSplineActor), placed by World Partition by its own band.
    };
} // namespace Desert::ECS
