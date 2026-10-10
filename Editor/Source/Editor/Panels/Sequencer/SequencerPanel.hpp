#pragma once

#include "../IPanel.hpp"
#include "LevelMaterialProperties.hpp"

#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/Commands/PoseEditTransaction.hpp>
#include <Editor/Core/Commands/SequenceEdit.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>

#include <Engine/Animation/Rig/ControlKeyer.hpp>

#include <Common/Core/UUID.hpp>

#include <Engine/ECS/Entity.hpp>

#include <glm/glm.hpp>

#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
    class LevelSequenceAsset;
    class AnimationAsset;
} // namespace Desert::Assets
namespace Desert::Animation
{
    class AnimationLibrary;
    class Animator;
    class Skeleton;
    // `class` and not `struct`: the definition in Engine/Animation/AnimationClip.hpp uses class, and the Microsoft
    // C++ ABI encodes the class-key into the decorated name, so the mismatch is a Windows-only link error waiting
    // for the day this forward declaration is the one a caller sees first.
    class AnimationClip;
} // namespace Desert::Animation

namespace Desert::Editor
{
    // ── ONE THING'S TIMELINE: A DOCUMENT, NOT A TOOL ──────────────────────────────────────────────────
    //
    // A scrubbable time ruler with a live playhead over ONE animated entity: for a rig, a clip picker,
    // a transport and per-bone position/rotation/scale keyframe lanes authored by posing; for a UI
    // element, its Offset / Size / Opacity / Color lanes.
    //
    // IT USED TO BE A SINGLETON THAT DREW WHATEVER WAS SELECTED, and it had FIVE empty states to show
    // for it — no scene, nothing selected, selection not in the scene, selection is neither a rig nor a
    // UI element, skinned mesh not resolved. Four of the five exist only because a window about
    // "whatever is selected" has to have an opinion about every way that can be nothing. Two characters
    // could not be compared side by side, and its Details button (SequencerPanel::RequestOpen) could
    // only say "reveal that window" because there was nothing else for it to say.
    //
    // ── WHY TWO SUBJECT TYPES AND NOT ONE ─────────────────────────────────────────────────────────────
    //
    // This window has always been two timelines behind one name, and making it a document is what forced
    // that to be said out loud: SubjectEditorRegistry is keyed on the subject TYPE, so a window has to
    // name the kind of thing it is about, and these two are not the same kind.
    //
    //   SKELETAL — the subject is SkinnedMeshComponent, the RIG. This timeline keys BONE POSES, and the
    //              bones are that component's skeleton; the clips it picks from are the ones the library
    //              holds for that skeleton's signature.
    //
    //              NOT AnimationComponent, and that is not an accident of what was free: AnimGraphPanel
    //              is registered under AnimationComponent and edits the STATES AND TRANSITIONS that live
    //              in it, and it never touches a bone. Two editors under one key is refused by the
    //              registry (by name, at startup) precisely so that this question gets answered instead
    //              of one window silently opening in place of the other.
    //
    //              The AnimationComponent is still READ — the current clip, Playing, Loop, Speed and the
    //              Animator all live there — the way the cloud layout document reads the scene's cloud
    //              layer: an input, never a second subject. IsSubjectAlive requires both components,
    //              because a rig with no AnimationComponent has no clip to key and no animator to pose.
    //
    //   UI       — the subject is UIAnimComponent, the CLIP ITSELF. There is no asset behind it; the
    //              keys live in the component and are serialized with the scene.
    //
    // One class rather than two files because the two share the window, the zoom and the ruler idiom;
    // the MODE is fixed at construction by whichever registration built it, not sniffed from the subject
    // — a window that had to inspect its own identity to know what it draws would be one `if` away from
    // drawing the wrong half.
    class SequencerPanel final : public ISubjectDocument
    {
    public:
        // Which timeline this window is. Fixed for its whole life, like the subject it goes with.
        enum class Timeline
        {
            Skeletal,
            UI,
            /// A LEVEL SEQUENCE (`.dseq`) asset: the subject is the ASSET (its handle), the scene it poses is
            /// the one the document was opened over — UE's Sequencer over a ULevelSequence.
            Level
        };

        // The two subject types this editor is registered under — one literal each, read by the
        // registration and by the Details button, because two that must agree is the shape that drifts.
        static constexpr const char* kSkeletalComponentTypeName = "SkinnedMeshComponent";
        static constexpr const char* kUIComponentTypeName       = "UIAnimComponent";

        [[nodiscard]] static SubjectTypeKey SkeletalSubjectType()
        {
            return ComponentSubjectType( kSkeletalComponentTypeName );
        }

        [[nodiscard]] static SubjectTypeKey UISubjectType()
        {
            return ComponentSubjectType( kUIComponentTypeName );
        }

        [[nodiscard]] static SubjectId SkeletalSubjectFor( const Common::UUID& entity )
        {
            return ComponentSubject( entity, kSkeletalComponentTypeName );
        }

        [[nodiscard]] static SubjectId UISubjectFor( const Common::UUID& entity )
        {
            return ComponentSubject( entity, kUIComponentTypeName );
        }

        SequencerPanel( const SubjectId& subject, const std::string& displayName, Timeline timeline,
                        const std::shared_ptr<::Desert::Core::Scene>& scene, Animation::AnimationLibrary* library,
                        Assets::AssetManager* assetManager );

        // Out of line only to give the authoring context back; see the definition.
        ~SequencerPanel() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 980.0f, 320.0f };
        }
        [[nodiscard]] bool IsLevelTimeline() const override
        {
            return true;
        }
        void OnUIRender() override;

        // The entity, in the scene this document was opened over, still carrying what this timeline is
        // about: for a rig BOTH an AnimationComponent and a SkinnedMeshComponent (see the class note —
        // one without the other is not something this window can key), for UI its UIAnimComponent.
        [[nodiscard]] bool IsSubjectAlive() const override;

        // The rig timeline's own view mode, offered to the command palette and therefore to the control
        // channel — see ISubjectDocument::Actions for why a button was not enough. UI mode has none: its
        // lanes are not bone channels and there is no curve view over them.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

        /// Level timeline: one property "<actor>.<slot>.<parameter>" per Material Parameter track, valued at the
        /// playhead (LevelMaterialProperties.hpp). A `set` keys it there — the row field's setter, one undo step.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

        // A TIMELINE COSTS NO RENDERER SLOT. Everything it draws is ImGui geometry over components the
        // scene already holds; there is no Scene of its own, no SceneRenderer and no offscreen target, so
        // it is not pending demand for one of the six and closing it would free nothing. Answering the
        // base class's conservative `true` would have it refuse a sixth window over a slot it was never
        // going to take.
        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        // DELIBERATELY A NO-OP, as in AnimGraphPanel and ParticleEditorPanel, which say why at length: the
        // subject is an entity UUID and UUIDs belong to one registry, so following the active-scene fanout
        // would point this window at a scene where the id names nothing — or names somebody else's entity.
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& /*scene*/ ) override
        {
        }

    private:
        // The entity this window is about, or nullopt when it is gone. ONE resolution, used by the draw
        // and by the liveness answer, so the two cannot disagree.
        //
        // CONST because that is what Scene::FindEntityByID hands back, and ECS::Entity is a HANDLE: the
        // constness is of the handle, not of the components behind it, so GetComponent still returns
        // something writable. Copying it to a non-const handle where one is wanted is what the call sites
        // do, and it is what the previous selection-driven code did for the same reason.
        [[nodiscard]] std::optional<std::reference_wrapper<const ECS::Entity>> ResolveEntity() const;

        // Per-bone P/R/S keyframe lanes, aligned under the ruler (contentX0 = window content left, gutter =
        // label column, laneW = time area width). Handles select/drag/add/delete + a selected-key inspector.
        void DrawClipTracks( Animation::AnimationClip* clip, Animation::Animator* animator, float contentX0,
                             float gutter, float laneW, float duration );

        // The rig timeline for @p entity — the clip picker, the transport and the bone lanes.
        void DrawSkeletalTimeline( ECS::Entity& entity );

        // ── LEVEL SEQUENCE (LevelSequenceTimeline.cpp) ──────────────────────────────────────────────────
        // The asset, resolved from the subject handle per call (never stored: the manager owns it).
        [[nodiscard]] std::shared_ptr<Assets::LevelSequenceAsset> ResolveLevelAsset() const;
        [[nodiscard]] SequenceOwner                               LevelOwner() const;
        void                                                      DrawLevelTimeline();
        [[nodiscard]] std::vector<DocumentAction>                 LevelActions();
        /// "+ Track → Actor": @p entity of the scene bound as a possessable (find-or-create).
        void AddLevelActor( const Common::UUID& entity, const std::string& label );
        /// Keys @p binding's entity's live Transform at the playhead (UE: "Key Transform" on the track row).
        void KeyLevelTransform( const Animation::Timeline::BindingGuid& binding );
        void AddLevelCameraCut( const Animation::Timeline::BindingGuid& camera );
        /// "+ Track ▸ Visibility" on any actor: the Bool "Visible" track, keyed at the range start with the
        /// actor's current visibility (ECS::AddVisibilityTrack), one undo step.
        void AddLevelVisibilityTrack( const Animation::Timeline::BindingGuid& binding );
        /// A Visibility key @p visible at the playhead on @p binding's track, one undo step.
        void KeyLevelVisibility( const Animation::Timeline::BindingGuid& binding, bool visible );
        /// One parameter "+ Track ▸ Material Parameter ▸ <slot>" offers: a Float / Float3 / Float4 of the slot
        /// shader's schema (UE: the scalar and vector parameters of the component's material), with what the
        /// actor's slot instance holds for it now (its own override, else its parent's, else the schema default).
        struct LevelMaterialParameterChoice
        {
            ECS::LevelSequenceMaterialParameter Parameter;
            Animation::Timeline::TrackKind      Kind = Animation::Timeline::TrackKind::Float;
            std::string                         Label;
            glm::vec4                           Current{ 0.0F };
            bool                                Color = false;
            std::optional<float>                Min;
            std::optional<float>                Max;
        };
        /// One material slot of the actor's mesh with the parameters its shader declares.
        struct LevelMaterialSlotChoice
        {
            uint32_t                                  Slot = 0;
            std::string                               Label;
            std::vector<LevelMaterialParameterChoice> Parameters;
        };
        /// The slots of @p binding's entity's Static / Skinned mesh that have their own runtime instance (the
        /// ones a Material Parameter track can drive). Empty when the binding names no such entity.
        [[nodiscard]] std::vector<LevelMaterialSlotChoice>
        LevelMaterialSlots( const Animation::Timeline::BindingGuid& binding ) const;
        /// "+ Track ▸ Material Parameter ▸ <slot> ▸ <parameter>": the track keyed at the range start with the
        /// actor's current value (ECS::AddMaterialParameterTrack), one undo step.
        void AddLevelMaterialParameterTrack( const Animation::Timeline::BindingGuid&    binding,
                                             const ECS::LevelSequenceMaterialParameter& parameter );
        /// A Material Parameter key @p value at the playhead on @p binding's track, one undo step.
        /// THE one setter of a Material Parameter value: the row's field, the palette's "Key Material Parameter"
        /// and the control channel's `set` (SetEditableProperty) all key through it (LevelMaterialEdit::Key).
        [[nodiscard]] Common::BoolResultStr
        KeyLevelMaterialParameter( const Animation::Timeline::BindingGuid&    binding,
                                   const ECS::LevelSequenceMaterialParameter& parameter, const glm::vec4& value );
        /// What the actors' slot shaders declare for every parameter the menu offers (label, clamp, colour).
        [[nodiscard]] std::vector<LevelMaterialEdit::Schema> LevelMaterialSchema() const;
        /// The clips that play on @p binding's entity (SkinnedMesh + Animation): the AnimationLibrary's clips
        /// for the mesh's skeleton (UE: "+ Track → Animation" lists the assets compatible with the skeleton).
        /// Empty when the binding names no such entity.
        [[nodiscard]] std::vector<std::shared_ptr<Assets::AnimationAsset>>
        LevelAnimationClips( const Animation::Timeline::BindingGuid& binding ) const;
        /// "+ Track → Animation <clip>": an Animation section of @p clip from the playhead for the clip's
        /// length, one undo step.
        void AddLevelAnimation( const Animation::Timeline::BindingGuid&        binding,
                                const std::shared_ptr<Assets::AnimationAsset>& clip );
        void SaveLevelSequence();
        void SetLevelTimePercent( int percent );
        /// Poses the scene at m_LevelTick when the tick or the sequence's Revision moved since the last pose.
        void PreviewLevelIfChanged( const Animation::Timeline::Sequence& sequence );

        // ── Level Sequence: transport, keys, Auto Key, curves (ANIM-FIX2) ──
        /// The transport row: play/pause, stop, to start / to end, Loop. The playhead is the PLAYER's, and
        /// `m_LevelTick` is read off it every frame — the one clock the preview poses the scene at.
        void DrawLevelTransport( const Animation::Timeline::Sequence& sequence );
        /// The header's track filters, Selected and Keyed (UE: Filters ▸ Selected / Keyed) — toggles of
        /// EditorPreferences::SequencerFilterSelected / SequencerFilterKeyed; the rule is Sequencer/TrackFilter.hpp.
        void DrawLevelTrackFilters();
        /// The player, (re)built when the sequence's range is not the one it was built for.
        Animation::Timeline::Player& LevelPlayer( const Animation::Timeline::Sequence& sequence );
        void                         JumpLevel( const Animation::Timeline::Sequence& sequence, int32_t tick );
        /// The pose-key lane of one binding: click selects (Shift adds), a drag on the empty lane draws a
        /// marquee, a drag on a selected key retimes the selection on the display grid — one undo step on
        /// release (`ECS::MoveEntityTransformKeys`).
        void DrawLevelKeyLane( Animation::Timeline::Sequence&          sequence,
                               const Animation::Timeline::BindingGuid& binding, float laneX0, float laneW,
                               float rowY, float rowH );
        /// Delete: every selected key, one undo step (`ECS::RemoveEntityTransformKeys`).
        void DeleteSelectedLevelKeys();
        /// "+ Track ▸ Event" on an actor or on the sequence (`ECS::LevelSequenceMasterBinding`), one undo step.
        void AddLevelEventTrack( const Animation::Timeline::BindingGuid& binding );
        /// An event named "Event" at the playhead on @p binding's Event track, selected for renaming; one undo
        /// step.
        void AddLevelEventKey( const Animation::Timeline::BindingGuid& binding );
        /// The Event track row of @p binding (UE: the Event Track): "+ Key" at the playhead, each key a marker
        /// with its name; click selects, a drag retimes it on the display grid (one undo step on release), the
        /// selected key's name is edited in the row (one undo step per committed edit), Delete removes it.
        void DrawLevelEventRow( Animation::Timeline::Sequence&          sequence,
                                const Animation::Timeline::BindingGuid& binding, const char* label,
                                float contentX0, float laneX0, float laneW );
        /// Delete: the selected event key, one undo step (`ECS::RemoveEventKey`).
        void DeleteSelectedLevelEvent();
        void SetLevelRecord( bool on );
        /// Per frame: the gizmo bit into `m_LevelAutoKey`; the release writes its keys inside one undo step.
        void UpdateLevelAutoKey( Animation::Timeline::Sequence& sequence );
        /// The curve view of a Transform track — the selected key's binding, else the first keyed one.
        void DrawLevelCurve( Animation::Timeline::Sequence& sequence, float contentX0, float gutter, float laneW );

        struct LevelKeyRef
        {
            Animation::Timeline::BindingGuid Binding;
            Animation::FrameNumber           Tick;
        };
        std::vector<LevelKeyRef> m_LevelSelKeys;
        /// The selected event key: (binding, index in `ECS::EventKeys`). Events share ticks, so a tick names none.
        struct LevelEventRef
        {
            Animation::Timeline::BindingGuid Binding;
            size_t                           Index = 0;
        };
        std::optional<LevelEventRef> m_LevelSelEvent;
        bool                         m_LevelEventDrag      = false;
        float                        m_LevelEventDragX0    = 0.0f;
        int32_t                      m_LevelEventDragDelta = 0;  ///< ticks, on the display grid
        char                         m_LevelEventName[128] = {}; ///< the row's name field for the selected event
        bool                         m_LevelEventNameEditing =
             false; ///< the field holds a typed, uncommitted name (else it mirrors the key)
        std::optional<Animation::Timeline::Player> m_LevelPlayer;
        Animation::Timeline::LoopMode              m_LevelLoop = Animation::Timeline::LoopMode::Loop;
        Animation::FrameNumber                     m_LevelPlayerStart{ INT32_MIN };
        Animation::FrameNumber                     m_LevelPlayerEnd{ INT32_MIN };
        bool                                       m_LevelKeyDrag   = false; ///< a selected key is held
        float                                      m_LevelDragX0    = 0.0f;
        int32_t                                    m_LevelDragDelta = 0; ///< ticks, on the display grid
        bool                                       m_LevelMarquee   = false;
        glm::vec2                                  m_LevelMarqueeFrom{ 0.0f };
        bool                                       m_LevelRecord    = false;
        bool                                       m_LevelCurveView = false;
        int                                        m_LevelCurvePart = 0; ///< 0 Location, 2 Scale
        ECS::LevelSequenceAutoKey                  m_LevelAutoKey;

        ECS::LevelSequencePreview m_LevelPreview;
        Animation::FrameNumber    m_LevelTick;
        int32_t                   m_LevelTickShown = INT32_MIN;
        /// The value a Material Parameter row's field shows while it is being dragged (row id → value): keyed
        /// once, on release, so a drag is one key and one undo step (UE: one transaction per committed edit).
        std::optional<std::pair<std::string, glm::vec4>> m_LevelMaterialDraft;
        uint32_t                                         m_LevelRevisionShown = UINT32_MAX;
        SequenceEditTransaction                          m_LevelEdit;

        // Creates a NEW empty clip for the given skeleton (a track per bone, no keys yet), registers it as an
        // in-memory AnimationAsset so it shows in the picker, and returns its name (empty on failure).
        std::string CreateEmptyClip( const Animation::Skeleton&             skeleton,
                                     const Animation::MeshSkeletonIdentity& mesh );

        // Writes the clip to Cooked/Meshes/_<name>.anim (rfl::json, same format the importer cooks) so an
        // in-editor-authored clip PERSISTS and is indexed from its registry row next session.
        //
        // Returns the written path, or the REASON it was not written. It used to return a bare
        // std::string with "" for failure, and the only caller discarded it — so the refusal had
        // nowhere to arrive and the person who pressed Save saw the same nothing either way (Д31-D's
        // worst row). The write itself, and its verdict, are
        // Assets::Serialization::SaveClipToFile's.
        [[nodiscard]] Common::ResultStr<std::string> SaveClipToDisk( const Animation::AnimationClip& clip );

        // WHAT A KEY IS ABOUT, assembled per frame and never stored. The keyer refuses an incomplete one,
        // so building it in one place is what stops the "Key" button and Record mode from disagreeing
        // about which clip, which tick and which pose buffer a key is written from — which is exactly how
        // the two "add a key at the playhead" paths came to mean different things (§936).
        [[nodiscard]] Animation::ControlKeyTarget KeyTargetFor( Animation::AnimationClip*  clip,
                                                                const Animation::Animator& animator ) const;

        // THE SAME KEYS, DRAWN AS CURVES (T4.3). Replaces the lane area rather than sitting beside it: a
        // dope sheet and a curve view are two readings of one channel, and showing both at once costs the
        // vertical space that is the only thing a curve needs.
        //
        // Position and scale only. A rotation key is a quaternion with no tangents, and its four components
        // are not four curves an animator can read — see TrackEditing::LiftChannel for why Euler channels
        // would be a format change rather than a view.
        void DrawCurveView( Animation::AnimationClip* clip, Animation::Animator* animator, float contentX0,
                            float gutter, float laneW, float duration );

        // ONE CURVE EDITOR FOR EVERY TIMELINE THAT OWNS TRANSFORM KEYS (UE's Sequencer has one curve editor for
        // a rig and a level sequence alike). `CurvePlot` is everything that differs between the owners — which
        // channel is shown, what the playhead is, how a key is selected, retimed and taken back — and
        // `DrawTransformCurve` is the one drawing and the one drag that both the skeletal `DrawCurveView` and
        // the level sequence's `DrawLevelCurve` hand theirs to.
        struct CurvePlot
        {
            Animation::Timeline::Sequence*                Sequence = nullptr;
            Animation::Timeline::TransformChannel*        Shown    = nullptr;
            Animation::TrackChannel                       Channel  = Animation::TrackChannel::Position;
            int                                           FitTrack = -1; ///< what a value-range refit is keyed on
            int                                           FitChannel = -1;
            std::string                                   Label;
            float                                         ContentX0       = 0.0f;
            float                                         Gutter          = 0.0f;
            float                                         LaneW           = 1.0f;
            float                                         DurationSeconds = 1.0f;
            Animation::FrameNumber                        DurationTicks;
            double                                        PlayheadSeconds = 0.0;
            std::optional<Animation::FrameNumber>         SelectedTick;
            std::function<void( Animation::FrameNumber )> Select;
            std::function<void()>                         BeginEdit;
            std::function<void()>                         EndEdit;
            std::function<bool( Animation::FrameNumber, Animation::FrameNumber )> Retime;
            std::function<void()>                                                 AfterEdit;
        };
        void DrawTransformCurve( const CurvePlot& plot );

        // ── SECTIONS (A32) ────────────────────────────────────────────────────────────────────────
        //
        // THE LANE IS A BAR PER SECTION, not a row per section, and that is what a section is: a RANGE
        // over the same time axis the ruler and the key lanes use. Stacking them as rows would have made
        // an overlap — the one case where the list order decides anything (`AnimationClip::SectionFor`
        // takes the LAST match) — the one case you cannot see.
        //
        // The weight channel is drawn INSIDE the bar rather than as a fourth lane below the keys: a fade
        // is a property of the section it fades, and a curve twenty pixels away from the bar it belongs
        // to is the arrangement that made `Additive Layers` above unreadable.
        void DrawSectionLane( Animation::AnimationClip* clip, Animation::Animator* animator, float contentX0,
                              float gutter, float laneW, float duration );

        // Everything about the selected section that is not a range: its name, its blend type, the tracks
        // it speaks for and its weight keys. Below the lanes, because it is about ONE section while the
        // lane is about all of them.
        void DrawSectionInspector( Animation::AnimationClip* clip, Animation::Animator* animator );

        // WHAT A SECTION COMMAND ACTS ON, resolved the same way the draw resolves it. `Actions()` is
        // handed no entity — the palette calls it on a document, not on a selection — so a command that
        // reached for the clip its own way would be a second answer to "which clip is open", and the two
        // would part company the first time the picker changed one of them.
        struct SectionTarget
        {
            Animation::Animator*      Animator = nullptr;
            Animation::AnimationClip* Clip     = nullptr;
            /// The track the dope sheet selected — whose sections the lane shows (sections belong to a
            /// track, as in UE); null when none is selected.
            Animation::Timeline::Track* Track = nullptr;
        };
        [[nodiscard]] std::optional<SectionTarget> ResolveSectionTarget() const;

        // One palette command's body: open an undo step, run @p edit, report a refusal to the animator.
        // ONE PLACE, because the alternative is thirteen copies of the same four lines and thirteen
        // chances for one of them to forget the transaction — which is how half an editor ends up outside
        // the undo stack (see PoseEditTransaction.hpp for the last time that happened here).
        void RunSectionEdit( const char*                                                           what,
                             const std::function<Common::BoolResultStr( SectionTarget&, size_t )>& edit );

        // Selecting a section, in one place: the rename buffer is refilled from whichever section this
        // points at, so every path that changes the selection has to go through the line that invalidates
        // it. Assigning m_SelSection directly is how the field came to show the previous section's name.
        void SelectSection( int index );
        /// Select the key on @p tick of lane @p lane of track @p track (an index into the sequence's tracks),
        /// keeping m_SelKey the key's index among the part's sorted ticks.
        void SelectKey( int track, int lane, Animation::FrameNumber tick,
                        const Animation::Timeline::Sequence& sequence );

        // The two edits a button and a palette command BOTH offer, written once. "Add" needs the playhead
        // and the clip's length; "reorder" needs the selection and the rule that the list order is the
        // priority order — and a second copy of either in `Actions()` is a second answer that drifts.
        void AddSectionAtPlayhead();
        void ReorderSelectedSection( int delta );

        // UI mode: the subject is this element's UIAnimComponent. Draws the clip's property lanes
        // (Offset / Size / Opacity / Color) with draggable keys and a scrubbable playhead.
        void DrawUITracks( ECS::Entity& entity );

        // The UI timeline's own palette actions, and the clip they act on. Split out of Actions() the
        // way DrawUITracks is split out of OnUIRender: two timelines with nothing in common but a window.
        std::vector<DocumentAction>    UIActions();
        [[nodiscard]] ECS::UIAnimData* ResolveUIClip();
        // "Key this lane at the playhead", shared by the lane's + button and by the palette action.
        void AddUIKeyAtPlayhead( ECS::UIAnimData& clip, int lane );

        // The two edges of a HELD widget in the UI timeline (a Duration drag, a key's value field), in one
        // place. See the definition for why it closes on IsItemDeactivated and not on the AfterEdit form.
        void BracketUIClipEditFromItem( ECS::UIAnimData& clip );
        // The Loop checkbox, which has already written the field by the time it answers true: the value is
        // put back for the length of one transaction so the entry's "before" is the state that was there.
        void RecordUIClipToggle( ECS::UIAnimData& clip, Animation::Timeline::LoopMode loop );
        // Close the open UI-clip transaction and say so if it refuses. A refusal here is a real defect (an
        // end with no begin) and the one thing a silent close would hide.
        void EndUIClipEdit();

        // WEAK, not shared — see AnimGraphPanel for the argument. A closed scene is one of the ways this
        // document's subject dies, and a strong reference would hide that and leak the level with it.
        std::weak_ptr<::Desert::Core::Scene> m_Scene;
        Animation::AnimationLibrary*         m_Library      = nullptr;
        Assets::AssetManager*                m_AssetManager = nullptr;

        const Timeline m_Timeline = Timeline::Skeletal;

        // ── WHAT THIS DOCUMENT AUTHORS, AND ITS RIGHT TO SAY SO ───────────────────────────────────
        //
        // Fork C of Docs/Animation/07_panels_design.md, closed as C1: the pose mode and the selected bone
        // belong to the DOCUMENT. They used to be `static inline` values on the process, so every open
        // Sequencer wrote the same bit every frame and the one the user was not looking at won half the
        // time. `m_Authoring` is this window's own copy — keyed on ITS subject — and it reaches the editor
        // only through Core::ActiveAuthoringContext(), which accepts a write from the holder alone.
        Core::AuthoringContext     m_Authoring;
        const Core::AuthoringOwner m_AuthoringOwner;

        // Publish this document's context while it is the window the user is working in.
        void TakeAuthoringContextIfFocused();

        // Clicking a bone's track selects that bone on the skeleton. Takes the context first, because the
        // click IS the user choosing this window.
        void SelectBoneFromTrack( uint32_t bone );

        // Curve view (T4.3). The value window is view state and is REFITTED when the selection changes,
        // not every frame: a box that rescales itself while a key is dragged moves the key under the mouse.
        bool      m_CurveView       = false;
        glm::vec2 m_CurveRange      = glm::vec2( -1.0f, 1.0f );
        bool      m_CurveFitPending = true;
        int       m_CurveFitTrack   = -1; // what the current fit was computed for
        int       m_CurveFitChannel = -1;

        // What the mouse has hold of in the curve view: which component's key, and whether the key itself
        // (0) or one of its tangent handles (1 = arrive, 2 = leave).
        int m_CurveDragKey       = -1;
        int m_CurveDragComponent = -1;
        int m_CurveDragHandle    = 0;

        // KEYING LIVES IN THE ENGINE NOW. `m_Record` is gone as a separate bool: the Record button reads and
        // writes `m_Keyer`'s `AutoChangeMode`, so a lit REC light and a keyer that is actually recording
        // cannot disagree — two bools for one fact is how they used to. The deferral to the end of a drag,
        // the auto-key modes and the refusals are all `Animation::ControlKeyer`, which a test suite can
        // reach and this file cannot (scripts/CI/UnreachedSources.sh).
        //
        // ONE KEYER PER WINDOW, not one per editor: an interaction is about the character this document is
        // over, and two Sequencers authoring two characters must not share a pending list.
        Animation::ControlKeyer m_Keyer;
        // THE OTHER HALF OF REPORT 05 §971, and it is driven by the SAME edge as the keyer above so that
        // the two cannot disagree about what one interaction is. Before it, this file did not mention
        // `CommandHistory` once: posing a bone and keying it were the only edits in the editor that could
        // not be taken back. ONE PER WINDOW, for the keyer's reason — an interaction is about the
        // character this document is over.
        SequenceEditTransaction m_ClipEdit;

        // ── THE CONTROL RIG'S TRACKS (ANV2b) ───────────────────────────────────────────────────────────
        // A keyer OF ITS OWN for control gestures: `ControlKeyer::Observe` keeps last frame's pointer bit,
        // so the bone keyer above observed for a control too would see two edges per frame. Its MODES are
        // not its own — they are copied from `m_Keyer` every frame, so the one Auto Key toggle drives both.
        Animation::ControlKeyer m_ControlKeyer;
        ControlAutoKey          m_ControlAutoKey;
        // The tick whose keys were last written onto the controls. Re-applied only when the playhead
        // moves, so a control the user is holding is never overwritten by the clip under it.
        int32_t m_ControlTickShown = INT32_MIN;
        // The selected summary-row key: which control's row, at which tick. Delete removes it.
        uint32_t m_ControlKeyRow  = Animation::ControlHierarchy::INVALID;
        int32_t  m_ControlKeyTick = 0;

        /// The key target for the control rig: `KeyTargetFor` plus the live hierarchy (null without a rig).
        [[nodiscard]] Animation::ControlKeyTarget ControlTargetFor( Animation::AnimationClip* clip,
                                                                    Animation::Animator&      animator ) const;
        /// Per frame: auto-key the held control, S / Delete, and the clip's pose onto the controls when the
        /// playhead moved. Called after `m_ClipEdit.Observe`, so a bone gesture's transaction closed first.
        void UpdateControlRig( Animation::AnimationClip* clip, Animation::Animator* animator );
        /// The tree "entity -> Control Rig -> controls" on the left, one summary key row per control.
        void DrawControlRigTracks( const ECS::Entity& entity, Animation::AnimationClip* clip,
                                   Animation::Animator* animator, float contentX0, float gutter, float laneW,
                                   float duration );
        /// The selected control, when the live authoring context is about THIS window's entity.
        [[nodiscard]] std::optional<uint32_t> SelectedControlHere() const;
        void                                  SelectControlFromTrack( uint32_t control );
        /// "Key (S)": every selected control keyed at the playhead as one undo entry. Refusals are logged.
        void KeySelectedControls();
        void SetAutoKey( bool on );
        void SetTimePercent( int percent );
        // THE UI TIMELINE'S OWN TRANSACTION, and it is a second type rather than a second use of the one
        // above because the subjects have nothing in common: this one edits a UIAnimComponent ON AN ENTITY
        // -- no animator, no authoring pose, no BoneTrack -- so neither half of a ClipPoseCommand is about
        // it. See UIClipEdit.hpp. ONE PER WINDOW, for the reason the keyer is: an interaction is about the
        // element this document is over.
        SequenceEditTransaction m_UIClipEdit; // opened with OwnerOf( UIAnimData* )
        // The last-seen posed transform of the selected bone, which is how this window decides a gizmo drag
        // moved something. It stays here because it is about THIS document's bone selection.
        int       m_RecordBone = -1;
        glm::mat4 m_RecordLast = glm::mat4( 1.0f );

        // Keyframe-editor selection (m_SelChannel: 0 = Position, 1 = Rotation, 2 = Scale).
        int m_SelTrack   = -1;
        int m_SelChannel = -1;
        int m_SelKey     = -1;
        Animation::FrameNumber
              m_SelKeyTick;      ///< the selected key's tick; m_SelKey is its index in the part's ticks
        float m_DragTime = 0.0f; // time being written while dragging a key (for re-selection after re-sort)

        // UI-clip editing state (which lane/key is selected in UI mode).
        int m_UITrack = -1;
        int m_UIKey   = -1;

        // ── SECTION AUTHORING STATE ───────────────────────────────────────────────────────────────
        //
        // Per DOCUMENT, like the bone selection above and for the same reason: two Sequencers over two
        // characters are two animators pointing at two sections, and a `static inline` here would make
        // them fight exactly as the pose mode used to.
        int m_SelSection = -1;
        // The rename field's buffer and WHICH section filled it. Two fields because "the buffer holds
        // section 2's name" is a fact the buffer itself cannot state, and the version without it showed
        // the previously selected section's name until the first keystroke.
        char m_SectionName[64] = {};
        int  m_SectionNameFor  = -1;
        // The value the "Key weight" button writes. A field rather than a literal so the button and the
        // slider beside it are one control: a button that always wrote 1 would make a fade-out
        // unauthorable from the inspector.
        float m_SectionWeight = 1.0f;
        // What the mouse has hold of in the lane: which section, and whether the body (0), the start
        // edge (1) or the end edge (2). The anchor tick is advanced only by a move that was ACCEPTED, so
        // a section pushed against the end of the clip stops there and comes back on the way in.
        int                    m_SectionDrag     = -1;
        int                    m_SectionDragEdge = 0;
        Animation::FrameNumber m_SectionDragTick;

        // Layer-preview authoring state (transient — previews on the live Animator).
    };

    // The `.dseq` path opener (LevelSequenceTimeline.cpp): find-or-create the LevelSequenceAsset, load it, then
    // open it through the one handle route, Core::RequestOpenAsset. Any other extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestLevelSequenceDocument( Assets::AssetManager* assets, const std::string& path,
                                  const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
