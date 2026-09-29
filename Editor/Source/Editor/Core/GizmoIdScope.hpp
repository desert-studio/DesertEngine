#pragma once

#include <imgui.h>

#include <ImGuizmo.h>

namespace Desert::Editor::Core
{
    /**
     * @brief Gives one ImGuizmo call site its own ID for the rest of the scope.
     *
     * ImGuizmo is ONE global context. Without an ID every Manipulate in a frame shares ID -1, so a drag
     * started on the Animation window's bone gizmo is also "the gizmo being used" for the level viewport's
     * Manipulate drawn later in the same frame, and that one applies the same mouse delta to its own
     * selection (and the reverse). With distinct IDs only the gizmo that took the press follows the drag.
     */
    class GizmoIdScope
    {
    public:
        explicit GizmoIdScope( const char* site )
        {
            ImGuizmo::PushID( site );
        }
        ~GizmoIdScope()
        {
            ImGuizmo::PopID();
        }
        GizmoIdScope( const GizmoIdScope& )            = delete;
        GizmoIdScope& operator=( const GizmoIdScope& ) = delete;
    };
} // namespace Desert::Editor::Core
