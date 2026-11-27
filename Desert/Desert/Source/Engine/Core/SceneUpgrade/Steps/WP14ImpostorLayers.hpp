#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    // 14 - IMPOSTOR HLOD LAYERS: a v13 world states no Impostor layer.
    inline constexpr int kWorldPartitionVersionImpostorLayers = 14;

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunImpostorLayers( UpgradeContext& context );
} // namespace Desert::Core
