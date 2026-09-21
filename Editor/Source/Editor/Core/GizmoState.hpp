#pragma once

namespace Desert::Editor::Core
{
    // Editor-global transform-gizmo mode, and the gizmo's view of the snap settings.
    //
    // Shared static (same pattern as SelectionManager) so BOTH the viewport (keyboard W/E/R + the
    // GizmoController that renders the gizmo) and the main toolbar buttons reach ONE place without
    // cross-panel plumbing.
    //
    // THIS CITED `SkeletonEditMode` AS THE OTHER EXAMPLE OF THE PATTERN, and that class no longer exists:
    // it was four process-wide values with public setters that five files wrote, and the defect that
    // follows is not hypothetical — two Sequencer documents authoring two characters overwrote each
    // other's pose mode every frame. It is now Core::AuthoringContext, published by one owner at a time,
    // and a write from anybody else is refused. The reference is removed rather than repointed because
    // the two are no longer the same pattern, and a comment naming a model is read as an endorsement of
    // copying it.
    //
    // WHY THIS ONE DID NOT GO THE SAME WAY: there is exactly one gizmo operation for the whole editor and
    // nothing about it is per-document — W/E/R in any viewport means the same thing everywhere, and no
    // two windows can want different answers. The authoring context is the opposite: it is about ONE rig,
    // and the editor deliberately has several open at once.
    //
    // THIS CLASS STORES ONLY SESSION STATE — the operation and the transform space — and that is the point
    // rather than an accident. Both are "where you are in a task"; neither is written to disk, so neither
    // can become a second copy of something a file already owns.
    // It used to hold the four snap values in private statics as well, which made them a SECOND copy of
    // four fields editor.json already owned — and the two copies had different writers. The toolbar's
    // magnet popup and the viewport's snap popup wrote HERE; the Preferences window wrote THERE;
    // EditorPreferences::Save() pushed THERE -> HERE as its first statement. Two consequences, and the
    // second is the expensive one:
    //
    //   * nothing that wrote this class ever reached the file, so a chosen step did not survive a restart;
    //   * any UNRELATED save — the Perf HUD toggle, an MSAA pick, a star in the Details panel — ran that
    //     push and silently reverted the step the user had just set, mid-session, with no message.
    //
    // So the snap values are read out of EditorPreferences below and stored only there. This class is a
    // seam, not a store: it exists so a gizmo can ask "what step am I on" without including the
    // preferences header, and so a discrete toolbar choice persists without every button remembering to.
    // A setter here is for a DISCRETE choice; a continuous control (a DragFloat) must edit the owning
    // field directly and save when the edit settles — see the note in GizmoState.cpp.
    class GizmoState
    {
    public:
        // Values match ImGuizmo::OPERATION so the GizmoController cast stays free.
        enum class Operation
        {
            None      = -1,
            Translate = 7,
            Rotate    = 120,
            Scale     = 896,
        };

        // Values match ImGuizmo::MODE (LOCAL = 0, WORLD = 1) so the GizmoController cast stays free, the
        // same bargain Operation makes above.
        enum class Space
        {
            Local = 0,
            World = 1,
        };

        // Deliberately NOT persisted: which handle you last dragged is where you are in a task, not a
        // preference, and every session starts in Select the way the editor's other modal state does.
        static Operation Get()
        {
            return s_Operation;
        }

        static void Set( Operation op )
        {
            s_Operation = op;
        }

        // The transform space the USER chose. Not persisted either, and for the same reason as Operation:
        // it is a position in a task, not a setting. UE keeps it per viewport and resets it per session.
        static Space GetSpace()
        {
            return s_Space;
        }

        static void SetSpace( Space space )
        {
            s_Space = space;
        }

        // ── WHY THIS IS NOT JUST `GetSpace()` ────────────────────────────────────────────────────────
        //
        // ImGuizmo DISCARDS the mode argument for scaling. ImGuizmo.cpp:2653 reads
        //
        //     ComputeContext( view, projection, matrix, ( operation & SCALE ) ? LOCAL : mode );
        //
        // so a Scale drag is ALWAYS along the object's own axes no matter what this class stores. That is
        // not a bug to route around — it is the only sane behaviour (a world-axis scale of a rotated
        // object is a shear, which no TRS triple can hold), and UE forces Local for scale for the same
        // reason.
        //
        // The hazard is the UI, not the maths: a World/Local toggle that still reads "World" while the
        // scale handles demonstrably work in Local is a control reporting a state the frame does not
        // have. So the toolbar, the gizmo and any test all ask THIS function what space is in force, and
        // the toggle is disabled (showing Local) while Scale is the active operation. One value, one
        // reader, and the button cannot claim something ImGuizmo will not do.
        static Space EffectiveSpace( Operation op )
        {
            return SpaceIsForced( op ) ? Space::Local : s_Space;
        }

        static Space EffectiveSpace()
        {
            return EffectiveSpace( Get() );
        }

        // True when the operation dictates the space and the user's choice cannot apply.
        static bool SpaceIsForced( Operation op )
        {
            return op == Operation::Scale;
        }

        // ── IS A MANIPULATOR BEING HELD RIGHT NOW (A28) ──────────────────────────────────────────────
        //
        // THE BOUNDARY OF AN INTERACTION, AND IT LIVES HERE FOR THIS CLASS'S OWN STATED REASON: there is
        // one pointer for the whole editor, so "a gizmo is being dragged" is one answer everywhere and
        // nothing about it is per-document. It is the criterion the header note above uses to decide what
        // belongs in `GizmoState` and what belongs in `AuthoringContext`.
        //
        // IT IS NOT BEHIND THE AUTHORING-CONTEXT GATE, and that is the decision rather than an omission.
        // Putting it there would make it writable only by whoever currently holds the context — and while
        // an animator drags a bone in the viewport with a Sequencer window focused, the holder is the
        // SEQUENCER. The viewport's write would be refused, the bit would never rise, and the deferral it
        // exists for would be silently off in exactly the case it was built for.
        //
        // Written by `GizmoController::RenderBone` (the pose branch) and read by `SequencerPanel`, which
        // hands it to `Animation::ControlKeyer::Observe`. Neither of those two files is compiled by any
        // test suite, which is why the RULE is in the keyer and only the BIT is here.
        static bool PoseInteraction()
        {
            return s_PoseInteraction;
        }

        static void SetPoseInteraction( bool held )
        {
            s_PoseInteraction = held;
        }

        // Snap increments, owned by EditorPreferences (~/.desertengine/editor.json). Snapping is active
        // when the persistent toggle is ON, or while Ctrl is held — and Ctrl INVERTS the toggle (so with
        // snap-always on, Ctrl gives a temporary free drag).
        //
        // Each setter writes the owning field and persists it, but only when the value actually changes.
        static float TranslateSnap();
        static float RotateSnapDegrees();
        static float ScaleSnap();
        static void  SetTranslateSnap( float v );
        static void  SetRotateSnapDegrees( float v );
        static void  SetScaleSnap( float v );

        static bool PersistentSnap();
        static void SetPersistentSnap( bool on );

        // The effective "snap now?" answer given the current Ctrl state.
        static bool SnapActive( bool ctrlHeld )
        {
            return PersistentSnap() != ctrlHeld; // XOR: Ctrl temporarily inverts the toggle
        }

    private:
        inline static Operation s_Operation = Operation::None;
        inline static Space     s_Space     = Space::World; // UE's default, and the behaviour before this existed
        inline static bool      s_PoseInteraction = false;
    };
} // namespace Desert::Editor::Core
