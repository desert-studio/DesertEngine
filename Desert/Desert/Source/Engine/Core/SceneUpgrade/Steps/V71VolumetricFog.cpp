#include <Engine/Core/SceneUpgrade/Steps/V71VolumetricFog.hpp>

#include <string>

namespace Desert::Core
{
    StepOutcome RunVolumetricFog( UpgradeContext& /*context*/ )
    {
        // Additive: the records are read with the declared defaults and the next save states them.
        StepOutcome outcome;
        outcome.Summary = "fog and lights state the Volumetric Fog fields at their defaults";
        return outcome;
    }
} // namespace Desert::Core
