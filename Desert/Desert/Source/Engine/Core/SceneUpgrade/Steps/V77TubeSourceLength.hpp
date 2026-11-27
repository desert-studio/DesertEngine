#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  77 - TUBE LIGHTS (NG-AREA-LIGHTS; UE ULocalLightComponent::SourceLength). Every PointLight / SpotLight
    //       record that does not state SourceLength states 0 - a sphere, the meaning every older light had
    //       (MigrateTubeSourceLengthV76ToV77). A prefab override is a delta and is left alone.
    inline constexpr int kSceneVersionTubeSourceLength = 77;

    // PURE (no filesystem, no globals): edits the records in place.
    // Returns how many PointLight / SpotLight records it stated SourceLength 0 in.
    std::size_t MigrateTubeSourceLengthV76ToV77( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunTubeSourceLength( UpgradeContext& context );
} // namespace Desert::Core
