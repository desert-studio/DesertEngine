#pragma once

// THE LANDSCAPE'S WORLD-PARTITION DESCRIPTOR (UE: FLandscapeActorDesc, the Landscape module's
// FWorldPartitionActorDesc). The partition rules (Engine/Core/Serialize/WorldPartitionRules.hpp) name no
// landscape: this module registers, at StartupModule, everything they read of its components - the loading row of
// each (the root and its material global, a tile and a spline spatial), the references a tile and a spline make to
// their root (observation), the DescriptorHooks that read a root's tiling, a tile's coordinate and a spline's band
// and answer a tile's rectangle (its root's frame, LandscapeLayout.hpp) and a spline's box, and what the HLOD and
// the activation cost make of a tile and a spline. Every executable that plans, describes or cooks a world calls
// it (Engine's StartupEngineModules, DesertServer, WorldGen, WorldCook); a process that does not reports every
// landscape record as an Issue and places none (WorldPartitionPlan::UnregisteredRecords) - WorldCook and WorldGen
// run so with `--disable-module LandscapeCore` (Tools/Shared/ToolModules.hpp).

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <LandscapeCore/Public/LandscapeLayout.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::LandscapeCore
{
    // The landscape's component keys on disk (ComponentRegistry keys). A tile's footprint is a RECTANGLE from its
    // coordinate and its root's frame; the root is read, never joined. A spline's footprint is the box around its
    // band (World::Landscape::LandscapeSplinesBounds): its points are world cm.
    inline constexpr std::string_view kLandscapeRootComponent     = "Landscape";
    inline constexpr std::string_view kLandscapeTileComponent     = "LandscapeTile";
    inline constexpr std::string_view kLandscapeSplineComponent   = "LandscapeSpline";
    inline constexpr std::string_view kLandscapeMaterialComponent = "LandscapeMaterial";

    // WHAT A LANDSCAPE TILE'S HLOD IS BUILT FROM (UE ULandscapeHLODBuilder::ComputeHLODHash: the proxy's transform
    // and size, the landscape material, the heights): its root's frame (the root's composed world position and
    // tiling), its coordinate, its height file, and the root's look - the LandscapeMaterial's material and the
    // root's layer infos the bake draws it with. Read by the landscape HLOD builder (Core's ReadLandscapeHLODTile)
    // and by the tile's saved-HLOD inputs alike, so the checksum folds exactly what the builder reads.
    struct LandscapeTileRead
    {
        std::size_t                      RootIndex = 0; // into the records
        World::Landscape::LandscapeRoot Root;
        std::int32_t                     TileX = 0;
        std::int32_t                     TileZ = 0;
        std::string                      HeightFile;
        std::string                      MaterialKey; // the LandscapeMaterial's GUID text, else its path; "" = none
        std::vector<std::string>         LayerKeys;   // the root's Layers, each GUID text else path
    };

    // @p records[index] read as a landscape tile; @p world is every record's composed world matrix, indexed like
    // @p records. Refused by name: no readable LandscapeTile block, no root, a root not in @p records or whose
    // frame ValidateLandscapeRoot refuses, a field that does not read.
    [[nodiscard]] Common::ResultStr<LandscapeTileRead> ReadLandscapeTileFrame( std::span<const Assets::EntityData> records,
                                                                               std::span<const glm::mat4> world,
                                                                               std::size_t                index );

    // Registers the landscape's partition rows and descriptor hooks; a second call does nothing.
    void StartupModule();
} // namespace Desert::LandscapeCore
