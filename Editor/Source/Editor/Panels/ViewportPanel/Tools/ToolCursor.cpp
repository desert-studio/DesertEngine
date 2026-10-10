#include "ToolCursor.hpp"

#include <ImGui/imgui.h>

namespace Desert::Editor::Tools
{
    void ApplyToolCursor( ToolCursor cursor, const glm::vec2& viewportPos, const glm::vec2& viewportSize,
                          bool viewportHovered )
    {
        if ( cursor == ToolCursor::None || !viewportHovered )
            return;
        const ImVec2 mouse = ::ImGui::GetMousePos();
        if ( mouse.x < viewportPos.x || mouse.y < viewportPos.y || mouse.x >= viewportPos.x + viewportSize.x ||
             mouse.y >= viewportPos.y + viewportSize.y )
            return;

        switch ( cursor )
        {
            case ToolCursor::None:
                return;
            case ToolCursor::Default:
                ::ImGui::SetMouseCursor( ImGuiMouseCursor_Arrow );
                return;
            case ToolCursor::Select:
                ::ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
                return;
            case ToolCursor::Drag:
                ::ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeAll );
                return;
            case ToolCursor::Unavailable:
                ::ImGui::SetMouseCursor( ImGuiMouseCursor_NotAllowed );
                return;
            case ToolCursor::Place:
            {
                // UE's placement crosshair: a dark outline under a light cross, so it reads on sky and on
                // white geometry alike, with a gap at the centre that leaves the target point visible.
                ::ImGui::SetMouseCursor( ImGuiMouseCursor_None );
                ImDrawList*     dl   = ::ImGui::GetForegroundDrawList();
                constexpr float kArm = 10.0f, kGap = 3.0f;
                const ImU32     dark = IM_COL32( 0, 0, 0, 200 ), light = IM_COL32( 255, 255, 255, 255 );
                const auto      arms = [&]( ImU32 col, float width )
                {
                    dl->AddLine( ImVec2( mouse.x - kArm, mouse.y ), ImVec2( mouse.x - kGap, mouse.y ), col,
                                 width );
                    dl->AddLine( ImVec2( mouse.x + kGap, mouse.y ), ImVec2( mouse.x + kArm, mouse.y ), col,
                                 width );
                    dl->AddLine( ImVec2( mouse.x, mouse.y - kArm ), ImVec2( mouse.x, mouse.y - kGap ), col,
                                 width );
                    dl->AddLine( ImVec2( mouse.x, mouse.y + kGap ), ImVec2( mouse.x, mouse.y + kArm ), col,
                                 width );
                };
                arms( dark, 3.0f );
                arms( light, 1.0f );
                return;
            }
        }
    }
} // namespace Desert::Editor::Tools
