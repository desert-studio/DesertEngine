#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/Common.hpp>

namespace Desert::ECS
{
    // One binding of the sequence re-pointed at another entity of THIS scene (UE: a binding override on
    // ALevelSequenceActor). The `.dseq` stays the same file for every actor that plays it.
    struct LevelSequenceBindingOverride
    {
        Animation::Timeline::BindingGuid Binding;
        Common::UUID                     Entity = Common::UUID::Null();
    };

    // A LEVEL SEQUENCE ACTOR (UE: ALevelSequenceActor + FMovieSceneSequencePlaybackSettings). Plays the
    // `.dseq` named by `Sequence` in Play (ECS/System/LevelSequenceSystem.hpp); its Entity bindings name
    // entities of this scene by UUID. Saved as {SequenceGuid, SequencePath, Loop, AutoPlay, PlayRate,
    // BindingOverrides} (ComponentRegistry.cpp); a sequence the project does not have refuses the load with both.
    struct LevelSequenceComponent
    {
        Assets::AssetHandle                       Sequence;
        Animation::Timeline::LoopMode             Loop     = Animation::Timeline::LoopMode::Once;
        bool                                      AutoPlay = true;
        // Sequence seconds per game second (UE: FMovieSceneSequencePlaybackSettings::PlayRate). Read every
        // frame by ApplyLevelSequencePlaySettings, so a change while playing takes effect on the next step; a
        // negative rate plays backwards, 0 holds the current frame.
        double                                    PlayRate = 1.0;
        std::vector<LevelSequenceBindingOverride> BindingOverrides;
    };
} // namespace Desert::ECS
