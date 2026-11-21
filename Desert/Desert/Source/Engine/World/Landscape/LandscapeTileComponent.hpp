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
    // ONE TILE OF A LANDSCAPE (UE: ALandscapeStreamingProxy of one component).
    //
    // `Landscape` names the root BY ID AND NOT AS A PARENT, and that is the whole reason this is a field:
    // a parent link is containment, and WorldPartition keeps a composite whole in one cell — every tile of
    // a landscape would be one composite and one cell would grow to hold the entire terrain. As an
    // OBSERVATION (WorldPartitionRules.hpp, kEntityReferences) each tile is its own composite, placed by its
    // own rectangle; a tile whose root is not loaded simply has no frame yet.
    //
    // `HeightFile` is where the heights live — a DLHT blob beside the scene, never inside the .desce: a
    // 64x64 tile is 8 KiB of samples and JSON would carry it as text, eleven times over. The path is
    // written as the scene file spells its other files (relative to the working directory), and it is
    // RE-DERIVED on every save from the destination (LandscapeTileFiles.hpp, LandscapeTileBlobPath), so "Save As"
    // never writes one scene's heights into another's files.
    //
    // `Heights` is the loaded tile — the single source of truth for this tile's terrain (landscape
    // analysis A2). Runtime state: it is not in the scene block, it is what the block's file decodes to.
    // HELD BY VALUE, because the component is its one owner (PointerOwnership's Q1): a pointer here would
    // answer a sharing question nobody has asked yet. A consumer that must keep the tile across frames
    // (the GPU copy, LS-4) cannot keep an address into an entt pool anyway, and decides its own form then.
    //
    // ONE WRITER after construction: ECS::SetLandscapeTileHeights / ECS::EditLandscapeTile
    // (LandscapeRootOf.hpp), both a registry.patch, so the shared tile table (DrawableLandscapeTiles) is
    // rebuilt on exactly the changes that can move or refuse a tile. Sample values edited in place report
    // through the tile's dirty rectangles instead.
    struct LandscapeTileComponent
    {
        Common::UUID Landscape = Common::UUID::Null(); // the root entity; null = no frame
        int32_t      TileX     = 0;                    // which tile of the root's grid, either side of it
        int32_t      TileZ     = 0;
        std::string  HeightFile;

        std::optional<World::Landscape::LandscapeTileData> Heights;
    };
} // namespace Desert::ECS
