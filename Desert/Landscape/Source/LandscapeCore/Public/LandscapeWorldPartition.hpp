#pragma once

// THE LANDSCAPE'S WORLD-PARTITION DESCRIPTOR (UE: FLandscapeActorDesc, the Landscape module's
// FWorldPartitionActorDesc). The partition rules (Engine/Core/Serialize/WorldPartitionRules.hpp) name no
// landscape: this module registers, at StartupModule, the DescriptorHooks that read a landscape root's tiling, a
// tile's coordinate and root, and a spline's band, and answer a tile's rectangle (its root's frame,
// LandscapeLayout.hpp) and a spline's box. Every executable that plans or describes a world calls it (Engine's
// StartupEngineModules, DesertServer, WorldGen).

namespace Desert::LandscapeCore
{
    // Registers the landscape descriptor hooks; a second call does nothing.
    void StartupModule();
} // namespace Desert::LandscapeCore
