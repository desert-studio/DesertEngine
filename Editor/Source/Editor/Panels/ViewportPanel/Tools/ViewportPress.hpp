#pragma once

#include <imgui_internal.h>

namespace Desert::Editor::Tools
{
    // Whether a widget, not the viewport itself, holds the mouse this frame - the one test every viewport
    // stroke (foliage paint, landscape sculpt, landscape paint) gates its press on. Call it inside the viewport
    // window's Begin/End.
    //
    // A press on the scene image (a plain Image, no item) is a click on the window's void, so ImGui makes the
    // window's own MoveId the active id from the next frame (UpdateMouseMovingWindowEndFrame) and holds it until
    // release: that id is the viewport holding its own press, not a widget claiming it. Counting it as "an item
    // is active" (ImGui::IsAnyItemActive) ended every stroke after its first frame, so a drag applied one dab
    // where it was pressed and nothing along the way.
    [[nodiscard]] inline bool WidgetClaimsMouse()
    {
        const ImGuiID activeId = ImGui::GetActiveID();
        return activeId != 0 && activeId != ImGui::GetCurrentWindow()->MoveId;
    }
} // namespace Desert::Editor::Tools
