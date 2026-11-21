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

#include <Engine/Animation/Timeline/AudioVoices.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

namespace Desert::ECS
{
    // A multi-key UI animation: a Timeline::Sequence hosted as UIAnimation (Timeline/Hosts.hpp) — UE's
    // UWidgetAnimation, the same MovieScene core a LevelSequence plays; widgets own no key model of their own.
    // Its Widget bindings name elements by entity UUID, so one clip may drive several elements. Authored on the
    // Sequencer, serialized by hand (ComponentRegistry) as the TMLN text block.
    struct UIAnimData
    {
        Animation::Timeline::Sequence Sequence = []
        {
            Animation::Timeline::Sequence hosted; // every other field keeps the Sequence's own default
            hosted.Host = Animation::Timeline::SequenceHost::UIAnimation;
            return hosted;
        }();
        Animation::Timeline::LoopMode Loop     = Animation::Timeline::LoopMode::Once;
        bool                          AutoPlay = true;

        // Where playback is. RUNTIME only — never serialized, so scrubbing in the editor cannot dirty the
        // scene. Created lazily from Sequence.TickRate/Start/End by the one view that drives scene animation
        // (UI/Ecs/UIAnimationPlayback.hpp); whoever edits the range resets it so the next frame re-creates it.
        std::optional<Animation::Timeline::Player> Playback;

        // The clip's Audio sections sounding at the playhead, as the driving view's LAST frame evaluated them
        // (UIAnimationPlayback.hpp; empty while paused, stopped or scrubbed backwards). RUNTIME only, never
        // serialized: AudioECSSystem consumes and clears it every frame and turns it into voices.
        std::vector<Animation::Timeline::SoundingVoice> Sounding;
    };
    struct UIAnimComponent
    {
        UIAnimData Data;
    };
} // namespace Desert::ECS
