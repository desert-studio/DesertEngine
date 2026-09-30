#pragma once

#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <entt/entt.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Desert::ECS
{
    struct LevelSequenceComponent;

    /**
     * @brief The LevelSequence host of the Timeline seam (Evaluator.hpp `ITimelineHost`): the entities of ONE
     * registry. UE: the level sequence player's object binding resolution + property track setters.
     *
     * Registry-only on purpose — no Scene, no AssetManager — so the suite drives it without the renderer.
     * The ECS system (System/LevelSequenceSystem.hpp) owns the Scene half: Play state and the view target.
     *
     * RESOLVE: an Entity binding's locator is the entity UUID as decimal text; a binding override of the
     * component, when present, wins over the locator. Anything else (a Bone or Widget binding, a locator
     * that is not a number, a UUID no entity of this registry carries) is not present: the Evaluator
     * reports its Label in `ApplyReport::Unresolved`.
     *
     * APPLY, by the track's Property (the names Sequencer's Transform track uses):
     *   "Transform"   BoneTransform → TransformComponent Translation / Rotation (Euler) / Scale
     *   "Translation" | "Location" vec3, "Rotation" quat, "Scale" vec3 → that one member
     *   "Visible"     bool → VisibilityComponent::Visible
     * Any other property, or a value of the wrong kind, is REFUSED by name into `Refusals()` — never skipped
     * in silence.
     */
    class LevelSequenceEntityHost final : public Animation::Timeline::ITimelineHost
    {
    public:
        LevelSequenceEntityHost( entt::registry& registry, const LevelSequenceComponent& component );

        [[nodiscard]] std::optional<Animation::Timeline::ResolvedBinding>
             Resolve( const Animation::Timeline::Binding& binding ) override;
        void Apply( const Animation::Timeline::ResolvedBinding& target, std::string_view property,
                    const Animation::Timeline::EvaluatedValue& value ) override;
        void Fire( const Animation::Timeline::FiredEvent& event ) override;
        void SetCamera( const std::optional<Animation::Timeline::ResolvedBinding>& camera ) override;
        void PlayAnimation( const Animation::Timeline::ResolvedBinding& target,
                            const Animation::Timeline::AnimationSample& sample ) override;

        /// The camera entity the Camera Cut in force names; nullopt = no cut in force this step.
        [[nodiscard]] const std::optional<entt::entity>& CameraCut() const
        {
            return m_CameraCut;
        }
        [[nodiscard]] const std::vector<std::string>& Refusals() const
        {
            return m_Refusals;
        }
        [[nodiscard]] const std::vector<std::string>& FiredEvents() const
        {
            return m_Fired;
        }

    private:
        entt::registry&               m_Registry;
        const LevelSequenceComponent& m_Component;
        std::optional<entt::entity>   m_CameraCut;
        std::vector<std::string>      m_Refusals;
        std::vector<std::string>      m_Fired;
    };

    /// One actor's playback state: the transport and the evaluator over the asset's sequence.
    struct LevelSequencePlayback
    {
        explicit LevelSequencePlayback( const Animation::Timeline::Sequence& sequence );

        Animation::Timeline::Player         Player;
        Animation::Timeline::Evaluator      Evaluator;
        Animation::Timeline::EvaluatedFrame Frame;
    };

    struct LevelSequenceStep
    {
        Animation::Timeline::ApplyReport Report;
        /// The Camera Cut in force after the step; nullopt = the sequence names no camera now.
        std::optional<entt::entity> CameraCut;
        std::vector<std::string>    Refusals;
        std::vector<std::string>    FiredEvents;
    };

    /// Evaluate @p step of @p playback's sequence and apply it to @p registry's entities.
    [[nodiscard]] LevelSequenceStep StepLevelSequence( entt::registry&                      registry,
                                                       const LevelSequenceComponent&        component,
                                                       LevelSequencePlayback&               playback,
                                                       const Animation::Timeline::TimeStep& step );

    /**
     * @brief What an actor remembers between steps for the Scene half: which errors it already reported
     * and the view target the first Camera Cut took over. Kept here, not in the system, so the suite checks
     * "reported once" and "the previous camera comes back" without a Scene.
     */
    struct LevelSequenceActorState
    {
        std::set<std::string> Reported;
        entt::entity          TargetBeforeCut = entt::null;
        bool                  CutInForce      = false;
    };

    /// The unresolved bindings and refused tracks of @p step that @p state has not reported yet, each in the
    /// words the log carries; they are recorded as reported.
    [[nodiscard]] std::vector<std::string> TakeNewLevelSequenceErrors( LevelSequenceActorState& state,
                                                                       const LevelSequenceStep& step );

    /// The view target after @p step, given the scene's @p current one: the cut's camera while a cut is in
    /// force; when the cut ends, the target that was current before it began. nullopt = leave it as it is.
    [[nodiscard]] std::optional<entt::entity>
    LevelSequenceViewTarget( LevelSequenceActorState& state, const LevelSequenceStep& step, entt::entity current );

    /// The bindings of @p sequence that @p component may still override, in the sequence's order (UE: the
    /// "+" of ALevelSequenceActor's Binding Overrides lists the object bindings): its Entity bindings with no
    /// override yet. Only an Entity binding is resolved against this scene (`LevelSequenceEntityHost::Resolve`),
    /// so a Bone, Widget or Sequence binding has nothing an override could re-point.
    [[nodiscard]] std::vector<const Animation::Timeline::Binding*>
    OverridableBindings( const Animation::Timeline::Sequence& sequence, const LevelSequenceComponent& component );
} // namespace Desert::ECS
