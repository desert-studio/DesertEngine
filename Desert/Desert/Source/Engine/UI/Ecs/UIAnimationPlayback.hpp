#pragma once

/**
 * UI ANIMATION PLAYBACK: THE UIAnimation HOST OF THE TIMELINE (Timeline/Hosts.hpp, Evaluator.hpp).
 *
 * A UI clip is a `Timeline::Sequence` — UE's UWidgetAnimation, the one MovieScene core — so the canvas does
 * not interpolate keys of its own. Once per view frame, before any canvas is walked, every `UIAnimComponent`
 * is stepped by its `Player` and evaluated, and its Widget bindings (locator = element entity UUID) are
 * resolved against the scene; the results land in the view's own source (UI/UIAnimationSource.hpp), which the
 * walk folds into each element's tween sample. A pre-pass rather than a per-element lookup because a clip on one
 * element may drive another element that is walked first.
 *
 * The player lives in the component (scene state, scrubbed by the Sequencer) and only the view that drives
 * scene animation creates and advances it; every other view evaluates the same playhead without moving it.
 * AutoPlay starts a clip only in a GAME world (Play-in-editor, the packaged game, the movie render): an
 * authored level shows the frame under the playhead, as UE's designer does, and Play starts the run at t = 0.
 */

#include <Engine/UI/UIAnimationSource.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>

#include <Engine/UI/Ecs/EcsUITree.hpp>
#include <entt/entt.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Desert::UI
{
    /**
     * @brief The engine's IUIAnimationSource: every UIAnimComponent of the scene, on the Timeline core.
     *
     * Evaluate steps (when the step advances) and evaluates every UI clip of the scene, writing each driven
     * element's sample. A clip without a player gets one — from the driving view (Advance) only — from its
     * sequence's TickRate/Start/End, its LoopMode, and — when AutoPlay and GameWorld — started; a view that
     * does not drive evaluates such a clip at its Start. A Widget binding whose element is not in the scene,
     * and a property the UI host does not know, are reported by name once and skipped.
     */
    class TimelineUIAnimationSource final : public IUIAnimationSource
    {
    public:
        [[nodiscard]] std::unique_ptr<IUIAnimationSource> Clone() const override
        {
            return std::make_unique<TimelineUIAnimationSource>( *this );
        }

        void Evaluate( const IUITree& scene, const UIAnimationStep& step ) override;

        void Reset() override
        {
            Samples.clear();
            Warned.clear();
        }

        [[nodiscard]] const UIClipSample* Sample( NodeId element ) const override
        {
            const auto it = Samples.find( ToEntity( element ) );
            return it != Samples.end() ? &it->second : nullptr;
        }

        /// This frame's results, one per element a clip drives. Cleared and refilled by Evaluate.
        std::unordered_map<entt::entity, UIClipSample> Samples;

        /// Labels / properties already reported, so a clip bound to a deleted element warns once, not per frame.
        std::unordered_set<std::string> Warned;
        bool                            WarnedForeignTree = false;

        /// Reused between frames (the evaluator clears it and keeps its capacity).
        Animation::Timeline::EvaluatedFrame Scratch;
    };
} // namespace Desert::UI
