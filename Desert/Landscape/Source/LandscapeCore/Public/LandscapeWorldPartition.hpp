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

#include <string_view>

namespace Desert::LandscapeCore
{
    // The landscape's component keys on disk (ComponentRegistry keys). A tile's footprint is a RECTANGLE from its
    // coordinate and its root's frame; the root is read, never joined. A spline's footprint is the box around its
    // band (World::Landscape::LandscapeSplinesBounds): its points are world cm.
    inline constexpr std::string_view kLandscapeRootComponent     = "Landscape";
    inline constexpr std::string_view kLandscapeTileComponent     = "LandscapeTile";
    inline constexpr std::string_view kLandscapeSplineComponent   = "LandscapeSpline";
    inline constexpr std::string_view kLandscapeMaterialComponent = "LandscapeMaterial";

    // Registers the landscape's partition rows and descriptor hooks; a second call does nothing.
    void StartupModule();
} // namespace Desert::LandscapeCore
