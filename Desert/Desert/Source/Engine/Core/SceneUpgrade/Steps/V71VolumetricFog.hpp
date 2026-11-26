#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  71 - VOLUMETRIC FOG (VOLFOG, UE's Volumetric Fog group and ULightComponent::VolumetricScatteringIntensity).
    //       ExponentialHeightFog and the lights gain fields that are ADDITIVE with defaults that reproduce the old
    //       picture, so the step has no transform: the record is read with the declared defaults and the next
    //       save states them. Scenes and prefabs alike.
    inline constexpr int kSceneVersionVolumetricFog = 71;

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunVolumetricFog( UpgradeContext& context );
} // namespace Desert::Core
