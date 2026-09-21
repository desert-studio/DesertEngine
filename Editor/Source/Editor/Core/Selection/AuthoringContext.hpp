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
// ── WHY THERE IS NO `Mode::Control` HERE ──────────────────────────────────────────────────────────
//
// §9.3's sketch lists four modes and a `SelectedControl`. Control rigs have their own live selection
// (`Editor/Core/Selection/ControlRigEditMode.hpp`) and nothing in this tree would read a `Control`
// value from here today, so adding one now would ship a knob that moves nothing — contract §3. The
// viewport's four-way mode switcher (§14.2) is the change that gives `Control` a reader, and it is the
// change that should add it, together with dissolving `ControlRigEditMode` the same way this dissolves
// `SkeletonEditMode`. What §14.2 gets from here is listed at the bottom of this header.

#include <Editor/Core/EditorSubject.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

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
        }
        return "Object";
    }

    // ONE RIG BEING AUTHORED, AND HOW.
    //
    // A value type: the owner keeps one of these as a member (that is where the state LIVES), and the
    // host below holds a copy of whichever is live. Copying rather than pointing is deliberate — a
    // published pointer into a document's member is a dangling pointer the frame that document closes,
    // and a closed document is the ordinary case here, not the exotic one.
    struct AuthoringContext
    {
        Common::UUID            Entity;                     // whose rig; null = none
        AuthoringMode           Mode = AuthoringMode::Object;
        std::optional<uint32_t> SelectedBone;               // index into Skeleton::GetBones()
        bool                    ShowBoneNames = false;      // label every bone, not just the selected one

        // Leaving bone authoring drops what only made sense inside it. The old type did this inside
        // `SetActive(false)` and it is kept because the alternative is a stale selected bone index
        // surviving into Object mode, where nothing clears it and the next entry starts on a bone the
        // user did not pick.
        void LeaveBoneAuthoring() noexcept
        {
            Mode = AuthoringMode::Object;
            SelectedBone.reset();
        }

        NO_DISCARD bool ShowsBones() const noexcept
        {
            return Mode == AuthoringMode::Skeleton || Mode == AuthoringMode::Pose;
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

            m_Holder  = AuthoringOwner{};
            m_Context.reset();
            return Common::MakeSuccess<bool>( true );
        }

        // ── WRITING ───────────────────────────────────────────────────────────────────────────────
        //
        // Every mutation goes through one gate, and the gate is the reason this type exists. A refusal
        // is a VALUE and not a log line: the call sites that can be refused are UI events, and an event
        // that is dropped without the caller being able to see it is the shape §1.4 of the contract
        // forbids.
        NO_DISCARD Common::BoolResultStr SetMode( const AuthoringOwner& owner, AuthoringMode mode )
        {
            return Write( owner, "set mode",
                          [mode]( AuthoringContext& context )
                          {
                              if ( mode == AuthoringMode::Object )
                                  context.LeaveBoneAuthoring();
                              else
                                  context.Mode = mode;
                          } );
        }

        NO_DISCARD Common::BoolResultStr SetSelectedBone( const AuthoringOwner&   owner,
                                                          std::optional<uint32_t> bone )
        {
            return Write( owner, "select bone", [bone]( AuthoringContext& context ) { context.SelectedBone = bone; } );
        }

        NO_DISCARD Common::BoolResultStr SetShowBoneNames( const AuthoringOwner& owner, bool show )
        {
            return Write( owner, "show bone names",
                          [show]( AuthoringContext& context ) { context.ShowBoneNames = show; } );
        }

    private:
        // One refusal, spelled once. Two shapes of failure and they are DIFFERENT facts: nothing is
        // published at all (the editor is in plain Object mode and no surface has claimed anything), or
        // somebody who is not the holder tried to write. A single "failed" would tell the caller which
        // of the two to fix in neither case.
        template <typename Mutation>
        NO_DISCARD Common::BoolResultStr Write( const AuthoringOwner& owner, const char* what, Mutation&& mutate )
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

    // ── WHAT THE §14.2 MODE SWITCHER GETS FROM HERE, ALREADY DONE ─────────────────────────────────
    //
    //   * a MODE rather than two booleans, so "Object / Skeleton / Pose" is one value with one writer
    //     and the switcher is a `SetMode` call rather than two toggles that can disagree;
    //   * an OWNER, so "the document in focus declares the mode" (§14.2) is enforced and not merely
    //     intended — the switcher does not have to police who writes it;
    //   * the entity the mode is about, so switching viewports or documents carries the mode with the
    //     character instead of leaking it onto the next one.
    //
    // What it still has to do, and why it is not done here: `Mode::Control` needs a reader (see the top
    // of this header), and untying the bind-pose preview from `ShowsBones()` — today
    // ViewportPanel::DrawViewportToolbar sets the preview for BOTH authoring modes, which is 07 §1.3's
    // defect and stays exactly as it was so that this change is a move of ownership and nothing else.
} // namespace Desert::Editor::Core
