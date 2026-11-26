#include <Engine/Core/SceneUpgrade/Steps/WP13EntityHLODLayer.hpp>

#include <string>

namespace Desert::Core
{
    StepOutcome RunEntityHLODLayer( UpgradeContext& /*context*/ )
    {
        // Additive: the block is read with the declared defaults and the next save states them.
        StepOutcome outcome;
        outcome.Summary = "HLOD layer per entity: additive, nothing restated";
        return outcome;
    }
} // namespace Desert::Core
