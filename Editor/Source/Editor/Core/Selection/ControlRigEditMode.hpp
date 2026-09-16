#pragma once

#include <Engine/Animation/Rig/ControlHierarchy.hpp>

#include <cstdint>

namespace Desert::Editor::Core
{
    /**
     * @brief "Control Rig" mode: the viewport draws the selected entity's control shapes and lets them be
     *        grabbed.
     *
     * THE SAME SHAPE AS `SkeletonEditMode` next door, and the same reason for existing: two panels have to
     * agree on one selection. The Control Rig panel lists the controls and the viewport overlay draws and
     * drags them, and neither owns the other — so the selection is a third thing both read, exactly as the
     * selected BONE is.
     *
     * IT IS NOT VIEW-ONLY. `ControlManipulator`'s `ControlDrag` writes the control's local pose through the
     * live `ControlHierarchy` the entity's Animator holds, which is the stage `AnimationECSSystem` built —
     * so a drag reaches the skinning matrices on the SAME frame, through `Animator::Update`'s Rig stage.
     *
     * THE SELECTION IS AN INDEX INTO THE LIVE HIERARCHY, NOT A NAME, and that is deliberate: a name would
     * have to be re-resolved every frame by the overlay, and the one moment the index can go stale — the
     * ECS rebuilding the stage — is the moment `Clear()` is called for, from the one place that can see it.
     */
    class ControlRigEditMode final
    {
    public:
        static bool IsActive()
        {
            return s_Active;
        }
        static void SetActive( bool active )
        {
            s_Active = active;
            if ( !active )
            {
                s_Selected = Animation::ControlHierarchy::INVALID;
            }
        }
        static void Toggle()
        {
            SetActive( !s_Active );
        }

        /// The control the panel and the overlay agree is selected, or `ControlHierarchy::INVALID`.
        static uint32_t GetSelected()
        {
            return s_Selected;
        }
        static void SetSelected( uint32_t control )
        {
            s_Selected = control;
        }

        /// Forgets the selection. Called when the stage the index points into is replaced or goes away —
        /// an index into a hierarchy that no longer exists is the stale-handle defect with a smaller name.
        static void Clear()
        {
            s_Selected = Animation::ControlHierarchy::INVALID;
        }

        /// Rotate instead of translate. One bit, because `ManipulatorMode` has exactly two values and
        /// scale is deliberately absent from the manipulator (see ControlManipulator.hpp).
        static bool RotateMode()
        {
            return s_Rotate;
        }
        static void SetRotateMode( bool rotate )
        {
            s_Rotate = rotate;
        }

    private:
        static inline bool     s_Active   = false;
        static inline uint32_t s_Selected = Animation::ControlHierarchy::INVALID;
        static inline bool     s_Rotate   = false;
    };
} // namespace Desert::Editor::Core
