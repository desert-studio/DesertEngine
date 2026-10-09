#pragma once

/**
 * THE LEVEL SEQUENCE DOCUMENT'S ENGINE HALF (ANIM-LSEQ): the edits the Sequencer makes to a `.dseq` and the
 * preview it poses the scene with. UE: FSequencer's "+ Track → Actor" (AddPossessable), the Transform track's
 * key, the Camera Cut track, and the "Restore State" the editor applies when the sequence is closed.
 *
 * Registry-only, like LevelSequencePlayback.hpp: the suite reaches every line here without a Scene or a GPU,
 * and the editor panel is left with drawing. The preview resolves and applies through the SAME host the
 * LevelSequenceSystem plays with (`LevelSequenceEntityHost`), so what the document shows at a tick is what
 * the placed component plays at that tick — one resolver, never a second one for the editor.
 */

#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <entt/entt.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::ECS
{
    /// The property name of an entity's Transform track — the one `LevelSequenceEntityHost::Apply` reads.
    inline constexpr const char* kLevelSequenceTransformProperty = "Transform";
    /// The property name of the sequence-level Camera Cut track.
    inline constexpr const char* kLevelSequenceCameraCutProperty = "CameraCut";
    /// The Property of the Animation track an actor binding carries (UE: the Skeletal Animation track).
    inline constexpr const char* kLevelSequenceAnimationProperty = "Animation";
    /// The Property of an actor's Visibility track — the bool `LevelSequenceEntityHost::Apply` writes into
    /// VisibilityComponent (UE: the actor's "Visibility" bool property track, bHiddenInGame inverted).
    inline constexpr const char* kLevelSequenceVisibilityProperty = "Visible";
    /// The Property of an Event track, on an actor binding or on the master Sequence binding (UE: the Event
    /// Track); its keys are named markers playback fires once when a step crosses them.
    inline constexpr const char* kLevelSequenceEventProperty = "Events";

    /**
     * @brief "+ Track → Actor" (UE: a Possessable): the Entity binding naming @p entity, created when missing.
     *
     * The GUID is DERIVED from the entity UUID (`BindingGuid::ForObject`), so binding one entity twice is the
     * same binding and not a second row — the find-or-create the bone tracks already follow. Revision++ when
     * the binding is new. Refuses the null UUID.
     */
    [[nodiscard]] Common::ResultStr<Animation::Timeline::BindingGuid>
    AddEntityBinding( Animation::Timeline::Sequence& sequence, const Common::UUID& entity,
                      const std::string& label );

    /// The entity pose a Transform key stores: TransformComponent's Euler rotation as the channel's quaternion.
    [[nodiscard]] Animation::BoneTransform EntityPose( const TransformComponent& transform );

    /**
     * @brief Upsert @p pose at @p tick on @p binding's Transform track (track and section created as needed,
     * `TrackEditing::ChannelForKey`'s gap rule). Refuses a binding that is not an Entity binding of this
     * sequence, and a non-finite pose. Revision++ on a structural change.
     */
    [[nodiscard]] Common::BoolResultStr SetEntityTransformKey( Animation::Timeline::Sequence&          sequence,
                                                               const Animation::Timeline::BindingGuid& binding,
                                                               Animation::FrameNumber                  tick,
                                                               const Animation::BoneTransform&         pose );

    /// The ticks of @p binding's Transform keys, ascending, one per keyed pose (a pose key writes every lane,
    /// so the ticks are the union over Location / Rotation / Scale). Empty for no Transform track.
    [[nodiscard]] std::vector<Animation::FrameNumber>
    EntityTransformKeyTicks( const Animation::Timeline::Sequence&    sequence,
                             const Animation::Timeline::BindingGuid& binding );

    /**
     * @brief Moves every pose key of @p binding's Transform track on a tick of @p from by @p delta ticks —
     * every lane of each, as one edit (a dragged selection on the dope sheet). ALL OR NOTHING: a key that
     * would leave the playback range or land on a key that is not itself moving refuses the whole move and
     * leaves the sequence as it was. Revision++ when anything moved.
     */
    [[nodiscard]] Common::BoolResultStr MoveEntityTransformKeys( Animation::Timeline::Sequence&          sequence,
                                                                 const Animation::Timeline::BindingGuid& binding,
                                                                 const std::vector<Animation::FrameNumber>& from,
                                                                 int32_t delta );

    /// Deletes the pose keys of @p binding on @p ticks (every lane). All or nothing: a tick with no key refuses
    /// and leaves the sequence as it was. Revision++.
    [[nodiscard]] Common::BoolResultStr
    RemoveEntityTransformKeys( Animation::Timeline::Sequence&             sequence,
                               const Animation::Timeline::BindingGuid&    binding,
                               const std::vector<Animation::FrameNumber>& ticks );

    /**
     * @brief The curve editor's per-key shape (UE: the key's interpolation and tangent mode in the curve editor's
     * right-click menu): every lane of @p binding's pose keys on @p ticks gets @p interp and @p mode, and the
     * channel's auto tangents are refreshed. The Rotation lanes are a quaternion (Constant or Linear by the
     * RotationChannel invariant), so Cubic sets them Linear — the slerp UE's Rotation lanes also use. A nullopt
     * @p interp or @p mode leaves that part of each key as it is (UE: picking the interpolation keeps the
     * tangents); both nullopt is refused. All or nothing: a tick with no pose key refuses and leaves the sequence
     * as it was. Revision++.
     */
    [[nodiscard]] Common::BoolResultStr SetEntityTransformKeyShape(
         Animation::Timeline::Sequence& sequence, const Animation::Timeline::BindingGuid& binding,
         const std::vector<Animation::FrameNumber>& ticks, std::optional<Animation::KeyInterp> interp,
         std::optional<Animation::TangentMode> mode );

    /// A pose key on the dope sheet: its binding and tick (the Sequencer's key selection).
    struct TransformKeyRef
    {
        Animation::Timeline::BindingGuid Binding;
        Animation::FrameNumber           Tick;
    };

    /// What a set of pose keys share (UE: the curve editor's key menu shows the selection's value, "Multiple
    /// Values" when the keys differ). A nullopt field: the keys differ on it, or none of them exists.
    struct TransformKeyShape
    {
        std::optional<Animation::KeyInterp>   Interp;
        std::optional<Animation::TangentMode> Mode;
    };

    /**
     * @brief The shape of the pose keys @p keys name, read from @p sequence on every call (the selection's current
     * value, never a remembered pick). Interp is read from the Location and Scale lanes: the Rotation lanes hold
     * the slerp `SetEntityTransformKeyShape` writes for Cubic, so reading them would make every Cubic key look
     * mixed. The tangent mode is read from every lane. A ref with no pose key adds nothing.
     */
    [[nodiscard]] TransformKeyShape SelectedEntityTransformKeyShape( const Animation::Timeline::Sequence& sequence,
                                                                     const std::vector<TransformKeyRef>&  keys );

    /**
     * @brief An easing preset on the segment of @p binding's @p part (Position or Scale) that ENDS at the pose key
     * on @p endTick (UE: section easing, authored here as keys — `Timeline::ApplyEasingPreset` on each of the
     * part's three lanes, at the sequence's display rate). Refuses Rotation (a quaternion has no per-lane
     * tangents), a tick with no key, and the first key (no segment ends there). All or nothing. Revision++.
     */
    [[nodiscard]] Common::BoolResultStr
    ApplyEntityTransformEasing( Animation::Timeline::Sequence&          sequence,
                                const Animation::Timeline::BindingGuid& binding, Animation::TrackChannel part,
                                Animation::FrameNumber endTick, Animation::Timeline::EasingPreset preset );

    /**
     * @brief Auto Key for a level sequence (UE's Auto Key with the Sequencer open): ONE pose key per bound actor
     * a GESTURE moved, written when the gesture ends — the rule `Animation::ControlKeyer` applies to a bone,
     * here for actors.
     *
     * `Observe` is called once per frame with the editor's one gizmo bit (`held`). On the rising edge it
     * remembers the live Transform of every Entity binding the registry resolves; on the falling edge it keys,
     * at @p tick, every one whose Transform differs from what it remembered. Every other frame it writes
     * nothing. `Releasing` answers whether this frame's `Observe` is the falling edge, so the caller can open
     * its undo step around exactly the frame that writes.
     */
    class LevelSequenceAutoKey
    {
    public:
        [[nodiscard]] bool Releasing( bool held ) const
        {
            return m_Held && !held;
        }
        /// Keys written this call (0 off the falling edge), or the first refusal.
        [[nodiscard]] Common::ResultStr<uint32_t> Observe( entt::registry&                registry,
                                                           Animation::Timeline::Sequence& sequence,
                                                           Animation::FrameNumber tick, bool held );
        /// Forget a gesture in flight (REC switched off mid-drag): its release keys nothing.
        void Reset();

    private:
        bool                                                                               m_Held = false;
        std::vector<std::pair<Animation::Timeline::BindingGuid, Animation::BoneTransform>> m_Before;
    };

    /**
     * @brief A Camera Cut section over [@p start, @p end] looking through @p camera (an Entity binding), on the
     * sequence-level Camera Cut track (its master binding and track created when missing). Whatever `Validate`
     * refuses — an overlap with another cut on row 0, a camera that is not an Entity binding — leaves the
     * sequence exactly as it was.
     */
    [[nodiscard]] Common::BoolResultStr AddCameraCut( Animation::Timeline::Sequence&          sequence,
                                                      const Animation::Timeline::BindingGuid& camera,
                                                      Animation::FrameNumber start, Animation::FrameNumber end );

    /**
     * @brief An Animation section playing @p clip over [@p start, @p end] on the Animation track of the Entity
     * binding @p binding (the track created when missing; a new section goes on the first row free over the
     * range). UE: "+ Track ▸ Animation ▸ <clip>" on an actor with a skeletal mesh. Refuses a binding that is
     * missing or not an Entity; whatever `Validate` refuses leaves the sequence exactly as it was.
     */
    [[nodiscard]] Common::BoolResultStr AddAnimationSection( Animation::Timeline::Sequence&          sequence,
                                                             const Animation::Timeline::BindingGuid& binding,
                                                             const Common::Content::AssetGuid&       clip,
                                                             Animation::FrameNumber                  start,
                                                             Animation::FrameNumber end, bool loop );

    /**
     * @brief "+ Track ▸ Visibility" on an actor (UE: the Visibility property track of any actor binding): a Bool
     * track "Visible" on @p binding with ONE section over the playback range, keyed at its start with the
     * actor's @p current visibility. The start key is what makes a later key a CHANGE: a bool channel holds its
     * first key before it (UE's FMovieSceneBoolChannel), so a lone "hidden at N" key would hide the actor from
     * the first frame. Refuses a binding that is not an Entity binding of this sequence and a binding that
     * already has the track; whatever `Validate` refuses leaves the sequence as it was. Revision++.
     */
    [[nodiscard]] Common::BoolResultStr AddVisibilityTrack( Animation::Timeline::Sequence&          sequence,
                                                            const Animation::Timeline::BindingGuid& binding,
                                                            bool                                    current );

    /// Whether @p binding has a Visibility track — the Sequencer offers "+ Track ▸ Visibility" only where not.
    [[nodiscard]] bool HasVisibilityTrack( const Animation::Timeline::Sequence&    sequence,
                                           const Animation::Timeline::BindingGuid& binding );

    /**
     * @brief Upsert a Constant key @p visible at @p tick on @p binding's Visibility track — on the highest-row
     * section whose range holds @p tick, else on the first section. Refuses a binding with no Visibility track
     * (add the track first); the sequence is left as it was on any refusal. Revision++ (the preview re-poses).
     */
    [[nodiscard]] Common::BoolResultStr SetVisibilityKey( Animation::Timeline::Sequence&          sequence,
                                                          const Animation::Timeline::BindingGuid& binding,
                                                          Animation::FrameNumber tick, bool visible );

    /// What @p binding's Visibility track says at @p tick (the bool channel's own Evaluate, on the section a key
    /// at that tick would land on); nullopt for no track.
    [[nodiscard]] std::optional<bool> VisibilityAt( const Animation::Timeline::Sequence&    sequence,
                                                    const Animation::Timeline::BindingGuid& binding,
                                                    Animation::FrameNumber                  tick );

    /// One key of an actor's Visibility track, as the dope sheet draws it.
    struct VisibilityKey
    {
        Animation::FrameNumber Tick;
        bool                   Visible = true;
    };

    /// Every key of @p binding's Visibility track, ascending by tick (every section). Empty for no track.
    [[nodiscard]] std::vector<VisibilityKey> VisibilityKeys( const Animation::Timeline::Sequence&    sequence,
                                                             const Animation::Timeline::BindingGuid& binding );

    /**
     * @brief "+ Track ▸ Material Parameter" on an actor (UE: MovieSceneComponentMaterialTrack — a Scalar or Vector
     * parameter of one material slot of the actor's mesh): a @p kind (Float = scalar, Vector = rgb) track
     * "Material.<slot>.<Parameter>" on @p binding with ONE section over the playback range, keyed at its start
     * with @p current (the parameter's value on the actor now: .x for a scalar, .xyz for a vector) — the same
     * start-key rule as Visibility, so a later key is a change and not a jump from the channel default. The
     * track drives the ACTOR's slot instance; the material asset is never written. Refuses a binding that is not
     * an Entity binding of this sequence, a kind other than Float / Vector, an empty parameter name and a
     * parameter that already has a track; the sequence is left as it was on any refusal. Revision++.
     */
    [[nodiscard]] Common::BoolResultStr AddMaterialParameterTrack( Animation::Timeline::Sequence& sequence,
                                                                   const Animation::Timeline::BindingGuid& binding,
                                                                   const LevelSequenceMaterialParameter& parameter,
                                                                   Animation::Timeline::TrackKind        kind,
                                                                   const glm::vec4&                      current );

    /// Whether @p binding has a Material Parameter track for @p parameter (either kind).
    [[nodiscard]] bool HasMaterialParameterTrack( const Animation::Timeline::Sequence&    sequence,
                                                  const Animation::Timeline::BindingGuid& binding,
                                                  const LevelSequenceMaterialParameter&   parameter );

    /**
     * @brief Upsert a key @p value (.x for a scalar track, .xyz for a vector one; Linear, like every new key) at
     * @p tick on @p binding's Material Parameter track for @p parameter — on the highest-row section whose range
     * holds @p tick, else on the first section. Refuses a parameter with no track (add it first); the sequence
     * is left as it was on any refusal. Revision++ (the preview re-poses).
     */
    [[nodiscard]] Common::BoolResultStr SetMaterialParameterKey( Animation::Timeline::Sequence&          sequence,
                                                                 const Animation::Timeline::BindingGuid& binding,
                                                                 const LevelSequenceMaterialParameter&   parameter,
                                                                 Animation::FrameNumber                  tick,
                                                                 const glm::vec4&                        value );

    /// What @p binding's Material Parameter track for @p parameter says at @p tick (.x for a scalar, .xyz for a
    /// vector; read from the section a key at @p tick would go to) — the value the Sequencer's row shows at the
    /// playhead. nullopt for no such track.
    [[nodiscard]] std::optional<glm::vec4> MaterialParameterAt( const Animation::Timeline::Sequence&    sequence,
                                                                const Animation::Timeline::BindingGuid& binding,
                                                                const LevelSequenceMaterialParameter&   parameter,
                                                                Animation::FrameNumber                  tick );

    /// Every key tick of that track, ascending and once each (every section; a vector's X / Y / Z keys merged),
    /// as the dope sheet draws them. Empty for no track.
    [[nodiscard]] std::vector<Animation::FrameNumber>
    MaterialParameterKeyTicks( const Animation::Timeline::Sequence&    sequence,
                               const Animation::Timeline::BindingGuid& binding,
                               const LevelSequenceMaterialParameter&   parameter );

    /// Every Material Parameter track of @p binding with its kind, in track order — the rows under the actor.
    [[nodiscard]] std::vector<std::pair<LevelSequenceMaterialParameter, Animation::Timeline::TrackKind>>
    MaterialParameterTracks( const Animation::Timeline::Sequence&    sequence,
                             const Animation::Timeline::BindingGuid& binding );

    // ── Event track (UE: the Sequencer Event Track — on an actor binding or on the sequence itself) ────────
    //
    // ONE Event track per binding, ONE section over the playback range when added. A key is a named marker
    // (`EventKey`, Duration 0) that playback fires once when a step crosses its tick (`CollectFired` →
    // `LevelSequenceEntityHost::Fire` → `LevelSequenceStep::FiredEvents`). Keys are addressed by their INDEX in
    // `EventKeys` order (several events may share a tick, so a tick does not name one). Every edit is made on
    // a copy kept only when `Validate` passes — a refusal leaves the sequence as it was — and bumps Revision.

    /// The binding a sequence-level track lives on (the master Sequence binding Camera Cuts share).
    [[nodiscard]] Animation::Timeline::BindingGuid LevelSequenceMasterBinding();

    /**
     * @brief "+ Track ▸ Event" on @p binding: an Entity binding of this sequence, or
     * `LevelSequenceMasterBinding()` (created when missing). Refuses any other binding and a binding that already
     * has an Event track.
     */
    [[nodiscard]] Common::BoolResultStr AddEventTrack( Animation::Timeline::Sequence&          sequence,
                                                       const Animation::Timeline::BindingGuid& binding );

    /// Whether @p binding has an Event track — the Sequencer offers "+ Track ▸ Event" only where not.
    [[nodiscard]] bool HasEventTrack( const Animation::Timeline::Sequence&    sequence,
                                      const Animation::Timeline::BindingGuid& binding );

    /// One key of an Event track, as the Sequencer row draws it.
    struct LevelEventKey
    {
        Animation::FrameNumber                          Tick;
        std::string                                     Name;
        std::optional<Animation::Timeline::EventAction> Action;
    };

    /// Every key of @p binding's Event track, section by section, each section's keys by tick. Empty for no track.
    [[nodiscard]] std::vector<LevelEventKey> EventKeys( const Animation::Timeline::Sequence&    sequence,
                                                        const Animation::Timeline::BindingGuid& binding );

    /**
     * @brief An event @p name at @p tick (after any event already on that tick) on the highest-row section of
     * @p binding's Event track whose range holds @p tick, else on its first section. Refuses an empty name and
     * a binding with no Event track. Returns the new key's index in `EventKeys`.
     */
    [[nodiscard]] Common::ResultStr<size_t> AddEventKey( Animation::Timeline::Sequence&          sequence,
                                                         const Animation::Timeline::BindingGuid& binding,
                                                         Animation::FrameNumber tick, std::string name );

    /// Renames the event at @p index (UE: the key's event name in the row / Details). Refuses an empty name.
    [[nodiscard]] Common::BoolResultStr RenameEventKey( Animation::Timeline::Sequence&          sequence,
                                                        const Animation::Timeline::BindingGuid& binding,
                                                        size_t index, std::string name );

    /// Moves the event at @p index by @p delta ticks (the channel stays sorted). Returns its new index.
    [[nodiscard]] Common::ResultStr<size_t> MoveEventKey( Animation::Timeline::Sequence&          sequence,
                                                          const Animation::Timeline::BindingGuid& binding,
                                                          size_t index, int32_t delta );

    /// Removes the event at @p index.
    [[nodiscard]] Common::BoolResultStr RemoveEventKey( Animation::Timeline::Sequence&          sequence,
                                                        const Animation::Timeline::BindingGuid& binding,
                                                        size_t                                  index );

    /// What the event at @p index does when it fires (UE: the event key's endpoint): @p action, or nullopt for a
    /// named marker only. `Validate` refuses a PlaySound / CallScript without its Target; the sequence is then
    /// left as it was.
    [[nodiscard]] Common::BoolResultStr
    SetEventKeyAction( Animation::Timeline::Sequence& sequence, const Animation::Timeline::BindingGuid& binding,
                       size_t index, std::optional<Animation::Timeline::EventAction> action );

    /// The Property of the sequence-level Subsequence track (UE: the Subsequences track).
    inline constexpr const char* kLevelSequenceSubsequenceProperty = "Subsequence";

    /// One section of the Subsequence track, as the Sequencer row draws it; `Index` is its place in the track.
    struct LevelSubsequenceSection
    {
        size_t                                         Index = 0;
        Animation::FrameNumber                         Start;
        Animation::FrameNumber                         End;
        int32_t                                        Row = 0;
        Animation::Timeline::SubsequenceSectionContent Content;
    };

    /// Every section of the Subsequence track, in track order. Empty for no track.
    [[nodiscard]] std::vector<LevelSubsequenceSection>
    SubsequenceSections( const Animation::Timeline::Sequence& sequence );

    /**
     * @brief "+ Track ▸ Subsequence ▸ <.dseq>": a section playing @p sub over [@p start, @p end] (offset 0, scale
     * 1) on the master binding's Subsequence track (binding and track created when missing). An overlapping
     * section stacks on the next free row, as UE's sub sections do. Refuses @p sub == @p self (the sequence would
     * play itself), the null GUID, and a section through which @p self reaches itself over @p reachable (the
     * sequences the others play: `CheckSubsequenceCycles` on the edited sequence, refused by name). Returns the
     * new section's index. All or nothing; Revision++.
     */
    [[nodiscard]] Common::ResultStr<size_t>
    AddSubsequenceSection( Animation::Timeline::Sequence& sequence, const Common::Content::AssetGuid& self,
                           const Common::Content::AssetGuid& sub, Animation::FrameNumber start,
                           Animation::FrameNumber end, const LevelSequenceSubsequenceSource& reachable = {} );

    /// Replaces section @p index of the Subsequence track: its range, sequence, start offset (sub ticks) and time
    /// scale (UE: the sub section's properties). `Validate` refuses a zero/negative scale and a null GUID; @p self
    /// is refused as the sequence, and so is a sequence through which @p self reaches itself over @p reachable
    /// (as `AddSubsequenceSection`). All or nothing; Revision++.
    [[nodiscard]] Common::BoolResultStr
    SetSubsequenceSection( Animation::Timeline::Sequence& sequence, const Common::Content::AssetGuid& self,
                           size_t index, Animation::FrameNumber start, Animation::FrameNumber end,
                           const Animation::Timeline::SubsequenceSectionContent& content,
                           const LevelSequenceSubsequenceSource&                 reachable = {} );

    /// Removes section @p index of the Subsequence track (the track stays, empty). Revision++.
    [[nodiscard]] Common::BoolResultStr RemoveSubsequenceSection( Animation::Timeline::Sequence& sequence,
                                                                  size_t                         index );

    /// The playback range, inclusive, in ticks (UE: the sequence's green / red playback range handles). Keys
    /// outside it stay: a player loops and clamps on the range, not on the keys. `Validate` refuses an @p end
    /// before @p start; the sequence is then left as it was. Revision++.
    [[nodiscard]] Common::BoolResultStr SetPlaybackRange( Animation::Timeline::Sequence& sequence,
                                                          Animation::FrameNumber         start,
                                                          Animation::FrameNumber         end );

    /**
     * @brief The Sequencer's preview of a level sequence over a scene's registry (UE: the editor's sequence
     * player with "Restore State" on close).
     *
     * `Scrub` poses the registry at a tick through `LevelSequenceEntityHost`, having first RECORDED the state of
     * every entity it is about to write (its Transform, whether it had a VisibilityComponent and its value, and
     * its Animator's clip, playhead, loop and the component's Playing — an Animation section repoints them —
     * and the slot override of every material parameter a track drives, or that it had none).
     * `Restore` writes every recorded state back — the component the preview added is removed again — and
     * forgets it. An entity destroyed while previewed is skipped. The destructor does NOT restore: it has no
     * registry, and a registry that died first has nothing to give back to.
     */
    class LevelSequencePreview
    {
    public:
        /// @p subsequences finds the sequences the Subsequence sections play (their actors are recorded and
        /// restored like the root's); @p asset is the document's own GUID, the root of the cycle check.
        LevelSequenceStep Scrub( entt::registry& registry, const Animation::Timeline::Sequence& sequence,
                                 Animation::FrameNumber tick, const LevelSequenceClipSource& clips = {},
                                 const LevelSequenceMaterialSlots&     materials    = {},
                                 const LevelSequenceSubsequenceSource& subsequences = {},
                                 const Common::Content::AssetGuid&     asset        = {} );
        void              Restore( entt::registry& registry );

        [[nodiscard]] bool Active() const
        {
            return !m_Saved.empty() || !m_SavedMaterials.empty();
        }

    private:
        struct Saved
        {
            entt::entity                    Entity = entt::null;
            TransformComponent              Transform;
            bool                            HadTransform  = false;
            bool                            HadVisibility = false;
            bool                            Visible       = true;
            bool                            HadAnimator   = false;
            const Animation::AnimationClip* Clip          = nullptr;
            Animation::FrameTime            Tick;
            bool                            Loop    = true;
            bool                            Playing = true;
        };
        std::vector<Saved> m_Saved;
        /// A material parameter override as it was before the first scrub wrote it (nullopt = none: dropped).
        struct SavedMaterialParameter
        {
            entt::entity                   Entity = entt::null;
            LevelSequenceMaterialParameter Parameter;
            std::optional<glm::vec4>       Override;
        };
        std::vector<SavedMaterialParameter> m_SavedMaterials;
        /// The slot access the scrubs wrote through — `Restore` gives the overrides back through the same.
        LevelSequenceMaterialSlots m_Materials;
        /// No overrides: the document is about the ASSET, not about one placed actor's re-pointing of it.
        LevelSequenceComponent m_NoOverrides;
    };
} // namespace Desert::ECS
