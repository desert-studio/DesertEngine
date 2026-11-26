#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    // World Partition's chain (Assets::kWorldPartitionSchemaTag), above its floor v12: additive only.
    // 13 - HLOD LAYER PER ENTITY (WP-HLOD-LAYER-COMPONENT): a record that states no HLODLayer component is built
    //      by its partition's layer, as it was.
    inline constexpr int kWorldPartitionVersionEntityHLODLayer = 13;

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunEntityHLODLayer( UpgradeContext& context );
} // namespace Desert::Core
