#pragma once

#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Animation
{
    class AnimationClip;
    class Skeleton;
}

namespace Desert::ECS
{
    struct LevelSequenceComponent;
    struct LevelSequencePlayback;

    /**
     * @brief Where the host finds the clip an Animation section names (UE: the section's UAnimSequence
     * reference, loaded by the asset system). Returns nullptr when no such clip is resident; the host then
     * refuses the section by name. The ECS system and the editor preview inject one over AssetManager
     * (ByGuid → AnimationAsset::GetClip); the suite injects its own clips. The clip must outlive the step.
     */
    using LevelSequenceClipSource =
         std::function<const Animation::AnimationClip*( const Common::Content::AssetGuid& clip )>;

    /**
     * @brief Where a Subsequence section's sequence is found (UE: the sub section's UMovieSceneSequence
     * reference). `Find` returns the loaded sequence (nullptr = not loaded: the section is refused by name) and
     * must outlive the step; `Name` is what a refusal calls it. LevelSequenceSubsequences (System/
     * LevelSequenceSystem.hpp) is the one over AssetManager; the suite injects its own sequences.
     */
    struct LevelSequenceSubsequenceSource
    {
        std::function<const Animation::Timeline::Sequence*( const Common::Content::AssetGuid& sequence )> Find;
        std::function<std::string( const Common::Content::AssetGuid& sequence )>                          Name;
    };

    /**
     * @brief Refuses a sequence that plays itself through its Subsequence sections, directly (A -> A) or
     * through others (A -> B -> A), naming the loop: "subsequence cycle: A -> B -> A". A section whose
     * sequence @p source does not have is not followed (the step refuses it by name when it plays).
     * LevelSequenceSystem checks this before an actor plays; the step refuses a looping section as well.
     */
    [[nodiscard]] Common::BoolResultStr CheckSubsequenceCycles( const Common::Content::AssetGuid&     root,
                                                                const LevelSequenceSubsequenceSource& source );

    /// The Property prefix of a Material Parameter track: "Material.<slot>.<Parameter>".
    inline constexpr std::string_view kLevelSequenceMaterialPropertyPrefix = "Material.";

    /// What a Material Parameter track's Property names: one parameter of one material slot of its actor.
    struct LevelSequenceMaterialParameter
    {
        uint32_t    Slot = 0;
        std::string Name;
        bool        operator==( const LevelSequenceMaterialParameter& ) const = default;
    };

    /// "Material.<slot>.<Parameter>" → (slot, parameter); nullopt for any other property (a slot that is not a
    /// decimal number, an empty parameter name).
    [[nodiscard]] std::optional<LevelSequenceMaterialParameter>
    ParseLevelSequenceMaterialProperty( std::string_view property );
    /// The Property of the Material Parameter track for @p parameter — the inverse of the parse above.
    [[nodiscard]] std::string LevelSequenceMaterialProperty( const LevelSequenceMaterialParameter& parameter );

    /**
     * @brief Where the host reaches an entity's per-slot material parameter overrides (UE: the dynamic material
     * instance MovieSceneComponentMaterialTrack creates on the component — the ACTOR's instance, never the
     * material asset). The overrides live on the entity's runtime material instance of that slot (the one the
     * Lit and slot draws bind, and the one Lua's SetMaterialParam writes); `LevelSequenceMaterialSlotOverrides()`
     * (System/LevelSequenceSystem.hpp) is that one implementation, which the ECS system and the
     * editor preview inject. The suite injects its own map: this header stays free of the renderer.
     *
     * `Get` is the slot's OWN override (nullopt = not overridden: the material's value shows); `Set` writes one
     * (a value) or drops it (nullopt) and is false when the entity has no such slot of its own material — a
     * mesh with no authored material shares the engine default instance, which is not the actor's to write.
     */
    struct LevelSequenceMaterialSlots
    {
        std::function<std::optional<glm::vec4>( entt::registry& registry, entt::entity entity,
                                                const LevelSequenceMaterialParameter& parameter )>
             Get;
        std::function<bool( entt::registry& registry, entt::entity entity,
                            const LevelSequenceMaterialParameter& parameter,
                            const std::optional<glm::vec4>&       value )>
             Set;

        [[nodiscard]] explicit operator bool() const
        {
            return Get && Set;
        }
    };

    /**
     * @brief The LevelSequence host of the Timeline seam (Evaluator.hpp `ITimelineHost`): the entities of ONE
     * registry. UE: the level sequence player's object binding resolution + property track setters.
     *
     * Registry-only on purpose — no Scene, no AssetManager — so the suite drives it without the renderer.
     * The ECS system (System/LevelSequenceSystem.hpp) owns the Scene half: Play state and the view target.
     *
     * RESOLVE: an Entity binding's locator is the entity UUID as decimal text; a binding override of the
     * component, when present, wins over the locator. A Bone binding resolves to its PARENT's entity (the
     * actor whose skeleton has the bone) while a sequence is being applied (`Begin`). Anything else (a Widget
     * binding, a locator that is not a number, a UUID no entity of this registry carries) is not present: the
     * Evaluator reports its Label in `ApplyReport::Unresolved`.
     *
     * APPLY, by the track's Property (the names Sequencer's Transform track uses):
     *   "Transform"   BoneTransform → TransformComponent Translation / Rotation (Euler) / Scale
     *   "Translation" | "Location" vec3, "Rotation" quat, "Scale" vec3 → that one member
     *   "Visible"     bool → VisibilityComponent::Visible
     *   "Material.<slot>.<Parameter>"  float → the slot's scalar override (vec4(x, 0, 0, 0), unpacked by the
     *                 parameter's own type); vec3 → the slot's vector override as (rgb, the override's own alpha,
     *                 1 when none) — through the injected `LevelSequenceMaterialSlots`; no source, no such slot,
     *                 or a value of another kind is refused by name
     * Any other property, or a value of the wrong kind, is REFUSED by name into `Refusals()` — never skipped
     * in silence.
     *
     * ANIMATION (UE: the Skeletal Animation track on an actor binding): the section's clip, found through the
     * injected `LevelSequenceClipSource`, poses the bound entity's `AnimationComponent::Animator` at the
     * section's clip time — the SAME path the Sequencer's Skeletal timeline scrubs through: `Playing` off
     * (the sequence owns the playhead), `Play(clip, loop)` when another clip is current, `SetTick(time)`,
     * which wraps a looping clip and clamps a finished one. An entity with no Animator, a clip the source
     * does not have, or a section blended below full weight / additively is REFUSED by name.
     *
     * BONES (UE: FK Control Rig on a skeletal mesh binding): the Bone bindings under one actor binding are
     * bound to the actor's skeleton ONCE per (Sequence::Revision, skeleton) — `LevelSequenceBoneCache` in the
     * playback — then sampled (`SampleBoneLayer`) and handed to the actor's Animator as ONE layer under the
     * playback's `Writer` (`Animator::SetSequencerLayer`), folded over its animated pose. ONE LAYER PER ACTOR
     * PER STEP (UE: FMovieSceneBlendingAccumulator blends every section writing one mesh into one pose): the
     * samples of every Bone binding reaching an actor — the master's and its subsequences', two entity
     * bindings of one actor — are gathered over the whole step and set once (`CommitBoneLayers`), in the
     * order they were sampled, so a later sample folds over an earlier one by its weight instead of replacing
     * the layer. An actor without an Animator, a bone its skeleton lacks (once per bone table), or an Animator
     * another writer layers is REFUSED by name. A layer the previous step set and this one did not is cleared
     * (`StepLevelSequence`).
     *
     * EVENTS (UE: the Event track's endpoints): every crossed key's name is recorded (`FiredEvents`); a key's
     * Action then runs once, on an Instant key or where a ranged key begins:
     *   PlaySound          the clip path goes to `Sounds()`; LevelSequenceSystem plays it one-shot (this host
     *                      stays registry-only, so the suite needs no audio device)
     *   ActivateParticles  the bound entity's Particle Emitter is enabled and restarted
     *   CallScript         {function, key name} queued on the bound entity's ScriptComponent; ScriptSystem calls
     *                      the function on every started slot with the key's name
     * A particle or script action with no bound entity, or one without that component, is REFUSED by name.
     */
    class LevelSequenceEntityHost final : public Animation::Timeline::ITimelineHost
    {
    public:
        LevelSequenceEntityHost( entt::registry& registry, const LevelSequenceComponent& component,
                                 LevelSequenceClipSource clips = {}, LevelSequenceMaterialSlots materials = {} );

        [[nodiscard]] std::optional<Animation::Timeline::ResolvedBinding>
             Resolve( const Animation::Timeline::Binding& binding ) override;
        void Apply( const Animation::Timeline::ResolvedBinding& target, std::string_view property,
                    const Animation::Timeline::EvaluatedValue& value ) override;
        void Fire( const Animation::Timeline::FiredEvent&                     event,
                   const std::optional<Animation::Timeline::ResolvedBinding>& target ) override;
        void SetCamera( const std::optional<Animation::Timeline::ResolvedBinding>& camera ) override;
        void PlayAnimation( const Animation::Timeline::ResolvedBinding& target,
                            const Animation::Timeline::AnimationSample& sample ) override;
        /// A VFX track's state: the bound entity's VFXComponent::SequencerDrive (activate / age / deactivate).
        void DriveEffect( const Animation::Timeline::ResolvedBinding& target,
                          const Animation::Timeline::VFXSample&       sample ) override;
        /// The bones under @p parentGuid at @p at, gathered into the parent entity's layer of this step (class
        /// comment); `CommitBoneLayers` sets it.
        void PoseBones( const Animation::Timeline::ResolvedBinding& parent,
                        Animation::Timeline::BindingGuid parentGuid, Animation::FrameTime at ) override;

        /// The playback whose sequence is applied next: Bone bindings resolve against its bindings and bind
        /// through its bone cache. The layer is written under @p writer (the root playback's).
        void Begin( LevelSequencePlayback& playback, std::string_view writer );
        /// Sets each actor's gathered layer once on its Animator, under the writer of the first playback that
        /// reached it. Called once, after the master and every subsequence were applied.
        void CommitBoneLayers();
        /// Every entity whose Animator this host layered, once each.
        [[nodiscard]] const std::vector<entt::entity>& Layered() const
        {
            return m_Layered;
        }

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
        /// The clip paths of the PlaySound actions fired, in firing order.
        [[nodiscard]] const std::vector<std::string>& Sounds() const
        {
            return m_Sounds;
        }
        /// A refusal found outside the seam's calls (a Subsequence section the step cannot play).
        void Refuse( std::string message )
        {
            m_Refusals.push_back( std::move( message ) );
        }

    private:
        entt::registry&               m_Registry;
        const LevelSequenceComponent& m_Component;
        LevelSequenceClipSource       m_Clips;
        LevelSequenceMaterialSlots    m_Materials;
        std::optional<entt::entity>   m_CameraCut;
        std::vector<std::string>      m_Refusals;
        std::vector<std::string>      m_Fired;
        std::vector<std::string>      m_Sounds;
        LevelSequencePlayback*        m_Playback = nullptr;
        std::string                   m_Writer;
        std::vector<entt::entity>     m_Layered;
        /// One actor's layer of this step, gathered over every PoseBones that reached it.
        struct PendingLayer
        {
            entt::entity                                      Entity = entt::null;
            std::string                                       Actor;
            std::string                                       Writer;
            std::vector<Animation::Timeline::BoneLayerSample> Samples;
        };
        std::vector<PendingLayer> m_Pending;
    };

    /// The bone table of ONE actor binding of a level sequence against ONE skeleton (Evaluator.hpp
    /// `BindBones` with the actor as parent). Rebuilt when the table's Sequence::Revision or the skeleton
    /// (its identity or bone count — an Animator re-made for another mesh) changes.
    struct LevelSequenceBoneCache
    {
        Animation::Timeline::BindingGuid      Parent;
        const Animation::Skeleton*            Skeleton  = nullptr;
        size_t                                BoneCount = 0;
        Animation::Timeline::BoneBindingTable Table;
        /// The table's missing bones were refused already: once per table, not on every step of Play.
        bool MissingReported = false;
    };

    /// One actor's playback state: the transport and the evaluator over the asset's sequence.
    struct LevelSequencePlayback
    {
        explicit LevelSequencePlayback( const Animation::Timeline::Sequence& sequence );

        Animation::Timeline::Player         Player;
        Animation::Timeline::Evaluator      Evaluator;
        Animation::Timeline::EvaluatedFrame Frame;
        /// The sequence's own asset: the root of the subsequence cycle check (null = not an asset).
        Common::Content::AssetGuid Asset;
        /// One child per Subsequence section, by (track, section) index: its own evaluator and resolved
        /// bindings, re-made when the source hands a different sequence.
        std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<LevelSequencePlayback>> Children;
        /// The name the bone layers of this playback carry on an Animator (`Animator::SetSequencerLayer`): the
        /// Level Sequence actor's name and entity in Play (LevelSequenceSystem), `kLevelSequencePreviewWriter`
        /// in the editor preview. Empty = this playback poses no bone (its bone tracks are refused by name).
        std::string Writer;
        /// One table per actor binding with bone tracks, a handful per sequence: searched linearly.
        std::vector<LevelSequenceBoneCache> BoneTables;
        /// The entities whose Animator the last step layered — the step clears the ones it no longer
        /// reaches; Stop / an actor retired clears them all (LevelSequenceSystem).
        std::vector<entt::entity> Layered;

        [[nodiscard]] const std::vector<entt::entity>& LayeredEntities() const
        {
            return Layered;
        }
    };

    /// The bone-layer writer of the editor's preview (LevelSequencePreview), one for every scrubbed document.
    inline constexpr std::string_view kLevelSequencePreviewWriter = "<preview>";

    /// Clears @p writer's bone layer from every Animator in @p entities (those still alive) and empties it.
    void ClearLevelSequenceBoneLayers( entt::registry& registry, std::vector<entt::entity>& entities,
                                       std::string_view writer );

    struct LevelSequenceStep
    {
        Animation::Timeline::ApplyReport Report;
        /// The Camera Cut in force after the step; nullopt = the sequence names no camera now.
        std::optional<entt::entity> CameraCut;
        std::vector<std::string>    Refusals;
        std::vector<std::string>    FiredEvents;
        /// The clip paths of the PlaySound actions fired this step, in firing order.
        std::vector<std::string> Sounds;
    };

    /**
     * @brief Evaluate @p step of @p playback's sequence and apply it to @p registry's entities.
     *
     * SUBSEQUENCES (UE: the Subsequences track): each Subsequence section the step passes through plays its
     * sequence through the SAME host, at `MapSubsequenceTime` of the step's ends: on the leg that ends the step
     * its values, animations, camera cut and events; on any other leg (a wrap's first half, a section the step
     * left) its events only, so a key inside a subsequence fires once per forward crossing. Nested sections
     * recurse. A section whose sequence @p subsequences does not have, or that would play a sequence already
     * being played (a cycle), is refused by name.
     */
    [[nodiscard]] LevelSequenceStep
    StepLevelSequence( entt::registry& registry, const LevelSequenceComponent& component,
                       LevelSequencePlayback& playback, const Animation::Timeline::TimeStep& step,
                       const LevelSequenceClipSource& clips = {}, const LevelSequenceMaterialSlots& materials = {},
                       const LevelSequenceSubsequenceSource& subsequences = {} );

    /// The component's Playback Settings (Loop, PlayRate) onto @p player. Called before every advance, so an
    /// edit made while the sequence plays (Details, a script) is the next step's setting.
    void ApplyLevelSequencePlaySettings( Animation::Timeline::Player&  player,
                                         const LevelSequenceComponent& component );

    /**
     * @brief ONE game-time tick of a level sequence actor — what LevelSequenceSystem runs per actor in Play and
     * in the packaged runtime: the play settings, the transport advanced by @p seconds of game time (× the
     * component's PlayRate, wrapped or clamped by its Loop), then that step evaluated and applied.
     */
    [[nodiscard]] LevelSequenceStep
    AdvanceLevelSequence( entt::registry& registry, const LevelSequenceComponent& component,
                          LevelSequencePlayback& playback, double seconds,
                          const LevelSequenceClipSource&        clips        = {},
                          const LevelSequenceMaterialSlots&     materials    = {},
                          const LevelSequenceSubsequenceSource& subsequences = {} );

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
