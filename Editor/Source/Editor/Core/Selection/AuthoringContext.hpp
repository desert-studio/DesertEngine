#pragma once

// ── WHO OWNS "WHICH BONE IS SELECTED, AND WHAT ARE WE DOING TO IT" ────────────────────────────────
//
// This replaces `Editor::Core::SkeletonEditMode`, which was four `static inline` values on the process
// (`s_Active`, `s_SelectedBone`, `s_ShowAllNames`, `s_PoseMode`) with public setters, read and written
// from fifteen places in five files. Nobody owned it and everybody could write it, and that is not a
// style complaint — it has a named, reproducible defect:
//
//   The editor deliberately supports SEVERAL Sequencer documents at once, one per character
//   (SequencerPanel.hpp: "Two characters could not be compared side by side" is listed as what making
//   it a document fixed). But every open Sequencer wrote `SetPoseMode( IsActive() && editClip )`
//   UNCONDITIONALLY, every frame, from its own copy of that line. Two of them therefore fought over
//   one process-wide bit: whichever drew last decided whether the OTHER one's bone gizmo edited the
//   rig's bind pose or the animator's pose buffer. Opening a second Sequencer and authoring in both
//   was impossible by construction, not by omission.
//
// Docs/Animation/07_panels_design.md §9.3 closes fork C as C1 — the state belongs to the DOCUMENT —
// and names the mechanism: one `AuthoringContext`, published by whoever the user is working in.
//
// ── THE RULE, IN ONE SENTENCE ─────────────────────────────────────────────────────────────────────
//
// The context is PUBLISHED by exactly one owner at a time, and ONLY THAT OWNER MAY WRITE IT. A write
// from anybody else is refused with a named reason and changes nothing.
//
// That is the whole difference from the four statics, and it is the part a test can see: the old
// `SetPoseMode` was callable by anyone and always succeeded, so no test could distinguish "the right
// panel wrote it" from "some other panel wrote it". Now the second case has an answer.
//
// ── WHY THE CONTEXT CARRIES THE ENTITY, AND WHAT "ADOPTION" IS ────────────────────────────────────
//
// A context is about ONE rig. Two Sequencers over two characters are two contexts that must never be
// merged, and that is exactly what `Entity` makes checkable.
//
// But a single character is authored through several surfaces at once — the bone tree in Details, the
// bone gizmo in the viewport, the track lanes in the Sequencer — and the user expects the selected
// bone and the pose mode to SURVIVE moving between them. So `Focus()` adopts: when the outgoing
// context is about the same entity, the incoming owner takes it over rather than resetting it. With
// one character on screen that makes the behaviour identical to the single global it replaces; with
// two, the contexts stay apart because the entities differ.
//
// ── `Mode::Control` IS HERE NOW, AND WHAT MADE IT ADMISSIBLE ──────────────────────────────────────
//
// It was deliberately absent until the viewport's four-way switcher existed, because a mode nothing
// reads is a knob that moves nothing (contract §3). §14.2 is that switcher, so the readers now exist
// and are named rather than assumed:
//
//   * `LightGizmoRenderer::Render` draws the control shapes and runs the drag for `Control` alone;
//   * `LightGizmoRenderer::Render` / `ViewportPanel` draw the bone overlay and hand the gizmo to the
//     bone for `ShowsBones()` — Skeleton and Pose;
//   * `GizmoController::RenderBone` writes the ANIMATOR's pose for `Pose` and the rig's bind
//     transform otherwise;
//   * `ViewportPanel::DrawViewportToolbar` asks the bind-pose preview for `PreviewsBindPose()` —
//     Skeleton ONLY, which is 07 §1.3 and the reason that predicate has a name of its own.
//
// The same change dissolved `Editor::Core::ControlRigEditMode` — three more `static inline` values on
// the process (`s_Active`, `s_Selected`, `s_Rotate`) written by the panel and the viewport overlay —
// into `Mode::Control`, `SelectedControl` and `ControlRotate` below. It had the identical defect for
// the identical reason: two characters on screen shared one selected-control index, which is an index
// into ONE hierarchy.

#include <Editor/Core/EditorSubject.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Desert::Editor::Core
{
    // WHAT THE USER IS DOING TO THE SELECTED RIG.
    //
    // `Skeleton` and `Pose` are two states the old type had fused into `IsActive()` plus a separate
    // `PoseMode()` bit, which is why "show the skeleton" and "pose the skeleton" could never be told
    // apart by a reader that had only one of the two (07 §1.3). One enum, so they cannot disagree.
    enum class AuthoringMode : uint8_t
    {
        Object,   // ordinary scene editing: no bone overlay, the gizmo moves the entity
        Skeleton, // editing the RIG: bone overlay + bone gizmo writing LocalBindTransform
        Pose,     // authoring a CLIP: bone overlay + bone gizmo writing the animator's pose buffer
        Control,  // posing through the CONTROL RIG: control shapes drawn and grabbed, no bone overlay
    };

    NO_DISCARD inline const char* AuthoringModeName( AuthoringMode mode ) noexcept
    {
        switch ( mode )
        {
            case AuthoringMode::Object:
                return "Object";
            case AuthoringMode::Skeleton:
                return "Skeleton";
            case AuthoringMode::Pose:
                return "Pose";
            case AuthoringMode::Control:
                return "Control";
        }
        return "Object";
    }

    // EVERY MODE, ONCE. The viewport's §14.2 switcher draws its segments from this and the control
    // channel's palette entries are generated from it, so a fifth mode appears on both surfaces by being
    // added to the enum — which is the opposite of what a hand-written strip does, and the opposite of
    // what the two booleans did (there was no list of the states they could be in at all).
    inline constexpr std::array<AuthoringMode, 4> kAuthoringModes = {
         AuthoringMode::Object,
         AuthoringMode::Skeleton,
         AuthoringMode::Pose,
         AuthoringMode::Control,
    };

    // ONE RIG BEING AUTHORED, AND HOW.
    //
    // A value type: the owner keeps one of these as a member (that is where the state LIVES), and the
    // host below holds a copy of whichever is live. Copying rather than pointing is deliberate — a
    // published pointer into a document's member is a dangling pointer the frame that document closes,
    // and a closed document is the ordinary case here, not the exotic one.
    struct AuthoringContext
    {
        Common::UUID            Entity; // whose rig; null = none
        AuthoringMode           Mode = AuthoringMode::Object;
        std::optional<uint32_t> SelectedBone;          // index into Skeleton::GetBones()
        std::optional<uint32_t> SelectedControl;       // index into the live ControlHierarchy
        bool                    ShowBoneNames = false; // label every bone, not just the selected one

        // WHAT THE CONTROL MANIPULATOR DOES WITH A GRAB. One bit because `ManipulatorMode` has exactly
        // two values — scale is deliberately absent from it (ControlManipulator.hpp) — and it lives here
        // rather than in `GizmoState` because it is about ONE rig: GizmoState's W/E/R is one answer for
        // the whole editor by construction, and this is per character like everything else in this type.
        bool ControlRotate = false;

        // THE ONE WRITER OF `Mode`, and it drops the selections the new mode cannot mean.
        //
        // An index only means something inside the mode that produced it: a bone index is an index into
        // `Skeleton::GetBones()`, a control index into the live `ControlHierarchy`, and carrying either
        // into a mode that does not draw it leaves a stale highlight the user cannot see to clear. The
        // rule is DERIVED from the two predicates below rather than written out per mode, so a fifth mode
        // cannot be added with the clearing half forgotten — which is exactly how the bone index used to
        // survive into Object mode before `SetActive(false)` was taught to reset it.
        void EnterMode( AuthoringMode mode ) noexcept
        {
            Mode = mode;
            if ( !ShowsBones() )
                SelectedBone.reset();
            if ( !ShowsControls() )
                SelectedControl.reset();
        }

        NO_DISCARD bool ShowsBones() const noexcept
        {
            return Mode == AuthoringMode::Skeleton || Mode == AuthoringMode::Pose;
        }

        NO_DISCARD bool ShowsControls() const noexcept
        {
            return Mode == AuthoringMode::Control;
        }

        // ── 07 §1.3, AND THE REASON IT IS A NAMED PREDICATE ───────────────────────────────────────
        //
        // While the RIG is being edited the mesh must be drawn in its bind pose, or a clip playing on the
        // entity overwrites every bone edit before it can be seen. While a CLIP is being authored the
        // opposite is true: the animator's pose buffer IS the thing under the cursor, and forcing bind
        // pose hides the very edit the user is making. The viewport asked `ShowsBones()` for both and so
        // got the rig answer in both — 07 §1.3's defect, and it survived the move to this type on purpose
        // so that that change stayed a move of ownership.
        //
        // Named here rather than spelled at the call site because `ViewportPanel.cpp` is compiled by no
        // test suite (scripts/CI/UnreachedSources.sh): as a predicate the DECISION is reachable and its
        // truth table is asserted, which an `if` inside the toolbar could never be.
        NO_DISCARD bool PreviewsBindPose() const noexcept
        {
            return Mode == AuthoringMode::Skeleton;
        }
    };

    // WHO IS ALLOWED TO WRITE. Three kinds because three kinds of thing author bones today, and they
    // are named rather than counted so a refusal can say who was refused and who holds it.
    class AuthoringOwner final
    {
    public:
        enum class Kind : uint8_t
        {
            None,      // nobody holds the context
            Document,  // an ISubjectDocument — a Sequencer over one character
            SceneView, // one viewport; several exist and they are told apart by SceneViewIdentity's id
            Panel,     // a singleton tool panel — the Details bone tree
        };

        AuthoringOwner() = default;

        NO_DISCARD static AuthoringOwner ForDocument( const SubjectId& subject )
        {
            AuthoringOwner owner;
            owner.m_Kind    = Kind::Document;
            owner.m_Subject = subject;
            return owner;
        }

        NO_DISCARD static AuthoringOwner ForSceneView( uint64_t sceneViewId )
        {
            AuthoringOwner owner;
            owner.m_Kind = Kind::SceneView;
            owner.m_Id   = sceneViewId;
            return owner;
        }

        // @p panel is a string literal owned by the caller's translation unit (a panel name), stored by
        // value so the owner stays a plain value type.
        NO_DISCARD static AuthoringOwner ForPanel( std::string panel )
        {
            AuthoringOwner owner;
            owner.m_Kind  = Kind::Panel;
            owner.m_Panel = std::move( panel );
            return owner;
        }

        NO_DISCARD bool IsNone() const noexcept
        {
            return m_Kind == Kind::None;
        }

        NO_DISCARD friend bool operator==( const AuthoringOwner& lhs, const AuthoringOwner& rhs ) noexcept
        {
            if ( lhs.m_Kind != rhs.m_Kind )
                return false;
            switch ( lhs.m_Kind )
            {
                case Kind::None:
                    return true;
                case Kind::Document:
                    return lhs.m_Subject == rhs.m_Subject;
                case Kind::SceneView:
                    return lhs.m_Id == rhs.m_Id;
                case Kind::Panel:
                    return lhs.m_Panel == rhs.m_Panel;
            }
            return false;
        }

        // For the refusal message. A refusal that cannot name who was refused is a log line nobody can
        // act on, which this editor has paid for before.
        NO_DISCARD std::string Describe() const
        {
            switch ( m_Kind )
            {
                case Kind::None:
                    return "nobody";
                case Kind::Document:
                    return "document " + m_Subject.ToString();
                case Kind::SceneView:
                    return "scene view " + std::to_string( m_Id );
                case Kind::Panel:
                    return "panel " + m_Panel;
            }
            return "nobody";
        }

    private:
        Kind        m_Kind = Kind::None;
        SubjectId   m_Subject;
        uint64_t    m_Id = 0;
        std::string m_Panel;
    };

    // ── THE ELECTION ──────────────────────────────────────────────────────────────────────────────
    //
    // An INSTANCE and not a pile of statics, for the reason this file exists at all. The editor has one
    // (see `ActiveAuthoringContext()` below); a test builds its own and two tests cannot leak state into
    // each other — which the four statics could not offer, and which is why none of them was ever
    // asserted by anything.
    class AuthoringContextHost final
    {
    public:
        // WHAT IS LIVE, or nothing. `nullopt` is a real state and not a failure: it is what the editor
        // is in before anybody has entered bone authoring, and after the last owner released. Every
        // reader below answers from the neutral value in that state rather than from a stale one.
        NO_DISCARD const std::optional<AuthoringContext>& Published() const noexcept
        {
            return m_Context;
        }

        NO_DISCARD const AuthoringOwner& Holder() const noexcept
        {
            return m_Holder;
        }

        NO_DISCARD AuthoringMode Mode() const noexcept
        {
            return m_Context ? m_Context->Mode : AuthoringMode::Object;
        }

        // "Draw the bone overlay / give the gizmo to the bone" — what `SkeletonEditMode::IsActive()`
        // answered. Both authoring modes show bones; only the mode says which pose is written.
        NO_DISCARD bool ShowsBones() const noexcept
        {
            return m_Context && m_Context->ShowsBones();
        }

        // What `SkeletonEditMode::PoseMode()` answered.
        NO_DISCARD bool IsPoseAuthoring() const noexcept
        {
            return Mode() == AuthoringMode::Pose;
        }

        // "Draw the control shapes and let one be grabbed" — what `ControlRigEditMode::IsActive()`
        // answered, and the reader that made `Mode::Control` admissible.
        NO_DISCARD bool ShowsControls() const noexcept
        {
            return m_Context && m_Context->ShowsControls();
        }

        // 07 §1.3. Skeleton only — see AuthoringContext::PreviewsBindPose.
        NO_DISCARD bool PreviewsBindPose() const noexcept
        {
            return m_Context && m_Context->PreviewsBindPose();
        }

        NO_DISCARD std::optional<uint32_t> SelectedControl() const noexcept
        {
            return m_Context ? m_Context->SelectedControl : std::nullopt;
        }

        // What `ControlRigEditMode::RotateMode()` answered.
        NO_DISCARD bool ControlRotate() const noexcept
        {
            return m_Context && m_Context->ControlRotate;
        }

        NO_DISCARD std::optional<uint32_t> SelectedBone() const noexcept
        {
            return m_Context ? m_Context->SelectedBone : std::nullopt;
        }

        // The old readers are written in `int` with -1 for none. Kept as one named conversion rather
        // than a `value_or( -1 )` repeated at five call sites, each of which could pick a different
        // sentinel.
        NO_DISCARD int SelectedBoneIndex() const noexcept
        {
            const auto bone = SelectedBone();
            return bone ? static_cast<int>( *bone ) : -1;
        }

        NO_DISCARD bool ShowBoneNames() const noexcept
        {
            return m_Context && m_Context->ShowBoneNames;
        }

        NO_DISCARD Common::UUID Entity() const noexcept
        {
            return m_Context ? m_Context->Entity : Common::UUID::Null();
        }

        // ── TAKING THE CONTEXT ────────────────────────────────────────────────────────────────────
        //
        // @p mine is the owner's OWN storage — the state lives there, this only publishes a copy — and
        // it is taken by reference because adoption writes back into it: after this call the owner's
        // member and the published context agree, which is what lets the owner read its own member for
        // the rest of the frame.
        //
        // ADOPTION IS BY ENTITY. See the header note; the one-line version is that several surfaces
        // author one character and the selected bone must survive moving between them, while two
        // characters must never share one.
        const AuthoringContext& Focus( const AuthoringOwner& owner, AuthoringContext& mine )
        {
            if ( m_Context && m_Context->Entity == mine.Entity && !mine.Entity.IsNull() )
                mine = *m_Context;

            m_Holder  = owner;
            m_Context = mine;
            return *m_Context;
        }

        // Gives the context up. Refused for anyone who is not the holder: releasing on behalf of
        // somebody else is how a panel that closed at the wrong moment would silently take bone
        // authoring away from the panel the user is actually working in.
        NO_DISCARD Common::BoolResultStr Release( const AuthoringOwner& owner )
        {
            if ( m_Holder.IsNone() )
                return Common::MakeError<bool>( "release refused: nothing is published" );
            if ( !( m_Holder == owner ) )
                return Common::MakeError<bool>( "release refused: " + owner.Describe() + " does not hold the " +
                                                "authoring context (" + m_Holder.Describe() + " does)" );

            m_Holder = AuthoringOwner{};
            m_Context.reset();
            return Common::MakeSuccess<bool>( true );
        }

        // ── WRITING ───────────────────────────────────────────────────────────────────────────────
        //
        // Every mutation goes through one gate, and the gate is the reason this type exists. A refusal
        // is a VALUE and not a log line: the call sites that can be refused are UI events, and an event
        // that is dropped without the caller being able to see it is the shape §1.4 of the contract
        // forbids.
        // @p mine is the owner's own storage again, and it is not a convenience: THE OWNER'S COPY IS THE
        // DURABLE ONE and the host's is the publication. A mutation that reached only the publication
        // would be lost the moment another character's document took the context, because the owner
        // republishes from `mine` when it comes back — a Sequencer would return to its window and find
        // its pose mode and its selected bone gone. Naming both here is what makes them unable to drift;
        // the alternative (the host keeping a pointer into a document's member) is a dangling pointer the
        // frame that document closes.
        NO_DISCARD Common::BoolResultStr SetMode( const AuthoringOwner& owner, AuthoringContext& mine,
                                                  AuthoringMode mode )
        {
            return Write( owner, mine, "set mode",
                          [mode]( AuthoringContext& context ) { context.EnterMode( mode ); } );
        }

        NO_DISCARD Common::BoolResultStr SetSelectedBone( const AuthoringOwner& owner, AuthoringContext& mine,
                                                          std::optional<uint32_t> bone )
        {
            return Write( owner, mine, "select bone",
                          [bone]( AuthoringContext& context ) { context.SelectedBone = bone; } );
        }

        NO_DISCARD Common::BoolResultStr SetShowBoneNames( const AuthoringOwner& owner, AuthoringContext& mine,
                                                           bool show )
        {
            return Write( owner, mine, "show bone names",
                          [show]( AuthoringContext& context ) { context.ShowBoneNames = show; } );
        }

        // THE CONTROL SIDE OF THE SAME GATE. `ControlRigEditMode::SetSelected` was a public static any
        // file could call and none could be refused; the two surfaces that call this — the Control Rig
        // panel and the viewport's overlay — now have to say they are the one the user is working in.
        NO_DISCARD Common::BoolResultStr SetSelectedControl( const AuthoringOwner& owner, AuthoringContext& mine,
                                                             std::optional<uint32_t> control )
        {
            return Write( owner, mine, "select control",
                          [control]( AuthoringContext& context ) { context.SelectedControl = control; } );
        }

        NO_DISCARD Common::BoolResultStr SetControlRotate( const AuthoringOwner& owner, AuthoringContext& mine,
                                                           bool rotate )
        {
            return Write( owner, mine, "set control manipulator mode",
                          [rotate]( AuthoringContext& context ) { context.ControlRotate = rotate; } );
        }

    private:
        // One refusal, spelled once. Two shapes of failure and they are DIFFERENT facts: nothing is
        // published at all (the editor is in plain Object mode and no surface has claimed anything), or
        // somebody who is not the holder tried to write. A single "failed" would tell the caller which
        // of the two to fix in neither case.
        template <typename Mutation>
        NO_DISCARD Common::BoolResultStr Write( const AuthoringOwner& owner, AuthoringContext& mine,
                                                const char* what, Mutation&& mutate )
        {
            if ( !m_Context )
                return Common::MakeError<bool>( std::string( "cannot " ) + what +
                                                ": no authoring context is published (" + owner.Describe() +
                                                " must Focus() first)" );
            if ( !( m_Holder == owner ) )
                return Common::MakeError<bool>( std::string( "cannot " ) + what + ": " + owner.Describe() +
                                                " does not hold the authoring context (" + m_Holder.Describe() +
                                                " does)" );

            mutate( *m_Context );
            mine = *m_Context; // the owner's durable copy and the publication, in one step
            return Common::MakeSuccess<bool>( true );
        }

        std::optional<AuthoringContext> m_Context;
        AuthoringOwner                  m_Holder;
    };

    // THE EDITOR'S ONE INSTANCE. §9.3: "ОДИН на редактор".
    //
    // A function-local rather than a namespace-scope object so that its construction is ordered by first
    // use, and — the part that matters here — so that nothing can read or write it without naming it.
    // Tests never touch this one; they build their own `AuthoringContextHost`, which is the property the
    // four statics could not have.
    inline AuthoringContextHost& ActiveAuthoringContext()
    {
        static AuthoringContextHost s_Host;
        return s_Host;
    }

    // ── WHAT THE §14.2 MODE SWITCHER TOOK FROM HERE, AND WHAT IT ADDED ───────────────────────────
    //
    // Taken, and it is why the switcher is small:
    //
    //   * a MODE rather than two booleans, so "Object / Skeleton / Pose / Control" is one value with one
    //     writer and the switcher is a `SetMode` call rather than toggles that can disagree;
    //   * an OWNER, so "the document in focus declares the mode" (§14.2) is enforced and not merely
    //     intended — the switcher does not have to police who writes it;
    //   * the entity the mode is about, so switching viewports or documents carries the mode with the
    //     character instead of leaking it onto the next one.
    //
    // Added by it, both because a reader arrived for them: `Mode::Control` with `SelectedControl` and
    // `ControlRotate` (see the top of this header), and `PreviewsBindPose()` — which is 07 §1.3's fix and
    // the one behaviour the previous change deliberately left broken so that it stayed a move of
    // ownership and nothing else.
} // namespace Desert::Editor::Core
