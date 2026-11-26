#include <Engine/Core/SceneUpgrade/Steps/WP14ImpostorLayers.hpp>

#include <string>

namespace Desert::Core
{
    StepOutcome RunImpostorLayers( UpgradeContext& /*context*/ )
    {
        // Additive: a v13 world states no Impostor layer.
        StepOutcome outcome;
        outcome.Summary = "impostor HLOD layers: additive, nothing restated";
        return outcome;
    }
} // namespace Desert::Core
