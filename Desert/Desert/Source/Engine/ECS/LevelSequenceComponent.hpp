#pragma once

#include <Common/Core/Entt.hpp>

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
    // BindingOverrides} (Core/Serialize/ActorComponentSerializers.cpp); a sequence the project does not have
    // refuses the load with both.
    //
    // REFLECTED (UE: LevelSequenceAsset and PlaybackSettings are UPROPERTYs), so the Details panel, the control
    // channel's `set` and undo reach the same fields through one property path. The file form stays the hand
    // serializer's (a {Guid, Path} reference, not a bare handle), so the struct carries no COMPONENT marker.
    // BindingOverrides is not a PROPERTY: a vector of {BindingGuid, entity UUID} pairs is no field kind the
    // reflection has; Details draws it below the reflected rows.
    struct LevelSequenceComponent
    {
        REFLECT()

        // A soft reference: the content registry's handle of the `.dseq` (the Details picker and `set ...
        // assets:<path>` both write it). LevelSequenceSystem creates the asset from its registry row when Play
        // starts, and the serializer saves the row's GUID and key.
        PROPERTY( DisplayName( "Sequence" ), Category( "Level Sequence" ), Summary, Asset<LevelSequenceAsset>,
                  Tooltip( "UE LevelSequenceAsset: the .dseq this actor plays in Play." ) )
        Assets::AssetHandle Sequence;

        PROPERTY( DisplayName( "Loop" ), Category( "Playback" ),
                  Tooltip( "UE PlaybackSettings.LoopCount: Once holds the last frame, Loop restarts, PingPong "
                           "reverses at each end." ) )
        Animation::Timeline::LoopMode Loop = Animation::Timeline::LoopMode::Once;

        PROPERTY( DisplayName( "Auto Play" ), Category( "Playback" ),
                  Tooltip( "UE PlaybackSettings.bAutoPlay: start playing when Play begins." ) )
        bool AutoPlay = true;

        // Sequence seconds per game second (UE: FMovieSceneSequencePlaybackSettings::PlayRate). Read every
        // frame by ApplyLevelSequencePlaySettings, so a change while playing takes effect on the next step; a
        // negative rate plays backwards, 0 holds the current frame.
        PROPERTY( DisplayName( "Play Rate" ), Category( "Playback" ),
                  Tooltip( "UE PlaybackSettings.PlayRate: sequence seconds per game second; negative plays "
                           "backwards, 0 holds the frame." ) )
        double PlayRate = 1.0;

        std::vector<LevelSequenceBindingOverride> BindingOverrides;
    };
} // namespace Desert::ECS
