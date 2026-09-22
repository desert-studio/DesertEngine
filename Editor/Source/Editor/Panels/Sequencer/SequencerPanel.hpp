#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/Commands/PoseEditTransaction.hpp>

#include <Engine/Animation/Rig/ControlKeyer.hpp>

#include <Common/Core/UUID.hpp>

#include <Engine/ECS/Entity.hpp>

#include <glm/glm.hpp>

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
}
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
            UI
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

        ImVec2 GetDefaultSize() const override
        {
            return ImVec2( 980.0f, 320.0f );
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

        // A TIMELINE COSTS NO RENDERER SLOT. Everything it draws is ImGui geometry over components the
        // scene already holds; there is no Scene of its own, no SceneRenderer and no offscreen target, so
        // it is not pending demand for one of the six and closing it would free nothing. Answering the
        // base class's conservative `true` would have it refuse a sixth window over a slot it was never
        // going to take.
        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsRendererSlot() const override
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

        // Creates a NEW empty clip for the given skeleton (a track per bone, no keys yet), registers it as an
        // in-memory AnimationAsset so it shows in the picker, and returns its name (empty on failure).
        std::string CreateEmptyClip( const Animation::Skeleton& skeleton );

        // Writes the clip to Cooked/Meshes/_<name>.anim (rfl::json, same format the importer cooks) so an
        // in-editor-authored clip PERSISTS and is rediscovered by the AssetPreloader next session.
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

        // UI mode: the subject is this element's UIAnimComponent. Draws the clip's property lanes
        // (Offset / Size / Opacity / Color) with draggable keys and a scrubbable playhead.
        void DrawUITracks( ECS::Entity& entity );

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
        PoseEditTransaction m_ClipEdit;
        // The last-seen posed transform of the selected bone, which is how this window decides a gizmo drag
        // moved something. It stays here because it is about THIS document's bone selection.
        int       m_RecordBone = -1;
        glm::mat4 m_RecordLast = glm::mat4( 1.0f );

        // Keyframe-editor selection (m_SelChannel: 0 = Position, 1 = Rotation, 2 = Scale).
        int   m_SelTrack   = -1;
        int   m_SelChannel = -1;
        int   m_SelKey     = -1;
        float m_DragTime   = 0.0f; // time being written while dragging a key (for re-selection after re-sort)

        // UI-clip editing state (which lane/key is selected in UI mode).
        int m_UITrack = -1;
        int m_UIKey   = -1;

        // Layer-preview authoring state (transient — previews on the live Animator).
        int   m_LayerClip         = -1;
        float m_LayerWeight       = 1.0f;
        bool  m_LayerAdditive     = false;
        char  m_LayerMaskBone[64] = {};
    };
} // namespace Desert::Editor
