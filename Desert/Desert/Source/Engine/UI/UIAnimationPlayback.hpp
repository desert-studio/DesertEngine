#pragma once

/**
 * UI ANIMATION PLAYBACK: THE UIAnimation HOST OF THE TIMELINE (Timeline/Hosts.hpp, Evaluator.hpp).
 *
 * A UI clip is a `Timeline::Sequence` — UE's UWidgetAnimation, the one MovieScene core — so the canvas does
 * not interpolate keys of its own. Once per view frame, before any canvas is walked, every `UIAnimComponent`
 * is stepped by its `Player` and evaluated, and its Widget bindings (locator = element entity UUID) are
 * resolved against the scene; the results land in a per-view `UIClipFrame`, which the walk folds into each
 * element's tween sample. A pre-pass rather than a per-element lookup because a clip on one element may drive
 * another element that is walked first.
 *
 * The player lives in the component (scene state, scrubbed by the Sequencer) and only the view that drives
 * scene animation advances it; every other view evaluates the same playhead without moving it.
 */

#include <Engine/Animation/Timeline/Evaluator.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Desert::UI
{
    /// What the clips of the scene add to one element this frame — the same three quantities a tween drives.
    struct UIClipSample
    {
        glm::vec2 Offset = glm::vec2( 0.0F );
        glm::vec2 Size   = glm::vec2( 0.0F );
        glm::vec4 Tint   = glm::vec4( 1.0F );
    };

    /// One view's clip results for the frame. Cleared and refilled by `PlayUIAnimations`.
    struct UIClipFrame
    {
        std::unordered_map<entt::entity, UIClipSample> Samples;

        /// Labels / properties already reported, so a clip bound to a deleted element warns once, not per frame.
        std::unordered_set<std::string> Warned;

        /// Reused between frames (the evaluator clears it and keeps its capacity).
        Animation::Timeline::EvaluatedFrame Scratch;

        void Reset()
        {
            Samples.clear();
            Warned.clear();
        }
    };

    /**
     * @brief Step (when @p advance) and evaluate every UI clip of @p reg, writing each driven element's sample.
     *
     * A clip without a player gets one from its sequence's TickRate/Start/End, its LoopMode, and — when
     * AutoPlay — started. A Widget binding whose element is not in the scene, and a property the UI host does
     * not know, are reported by name once and skipped.
     */
    void PlayUIAnimations( entt::registry& reg, float dtSeconds, bool advance, UIClipFrame& frame );
} // namespace Desert::UI
