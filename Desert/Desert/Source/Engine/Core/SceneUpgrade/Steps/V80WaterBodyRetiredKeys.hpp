#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  80 - THE WATERBODY BLOCK'S v2 / v3 UPGRADE (MIG-BISTRO; the block step ComponentSchema.lock asks for). Two
    //       WaterBody keys left the block with nothing that holds their value now, so they are dropped:
    //       - TargetWaveMaskDepth (block v2, WATER-RIPPLES-FFT): UE's hand-set damping depth is replaced by the
    //         physical depth transform (Water::TransformBand / WaterDepthTransform.glslh: shoaling by the
    //         dispersion relation omega^2 = g k tanh(k h), capped by the breaker index 0.78 h). What the depth
    //         does to a wave is derived from the depth and the spectrum alone; no number replaces this one.
    //       - FallFoam (block v3, WATER-FALLS): the fall's foam is the plunge boil in the foam field (FallPlunge
    //         -> WaterFoamUpdate), fed by the baked sheet, not an authored asset.
    //       (MigrateWaterBodyRetiredKeysV79ToV80). The record and every prefab override.
    inline constexpr int kSceneVersionWaterBodyRetiredKeys = 80;

    // What MigrateWaterBodyRetiredKeysV79ToV80 did to one file.
    struct WaterBodyRetiredKeysReport
    {
        std::size_t Dropped = 0; // TargetWaveMaskDepth / FallFoam keys removed
    };

    // PURE (no filesystem, no globals): edits the records in place.
    WaterBodyRetiredKeysReport MigrateWaterBodyRetiredKeysV79ToV80( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunWaterBodyRetiredKeys( UpgradeContext& context );
} // namespace Desert::Core
