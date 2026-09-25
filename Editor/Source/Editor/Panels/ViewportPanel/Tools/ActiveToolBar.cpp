#include "ActiveToolBar.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor::Tools
{
    ActiveToolLabel LabelOf( Core::ModelingState::Tool tool )
    {
        using T = Core::ModelingState::Tool;
        switch ( tool )
        {
            case T::None:
                return {};
            // UE's Cube Grid tool is cancellable: Cancel throws the in-progress blockout away.
            case T::CubeGrid:
                return { ICON_MDI_GRID, "CubeGrid", true };
            // Ours applies every PolyEdit operation as its own undo step, so there is no pending edit a
            // Cancel could drop (UE's EditMeshPolygons keeps one); Complete is the honest button.
            case T::PolyEdit:
                return { ICON_MDI_VECTOR_SQUARE, "PolyEdit", false };
            case T::ElementSelect:
                return { ICON_MDI_VECTOR_SELECTION, "Select Elements", false };
            // UE's Add Primitive tools have no Cancel either: each click has already placed its shape.
            case T::CreateShape:
                return { ICON_MDI_PLUS_BOX_OUTLINE, "Create Shape", false };
        }
        return {};
    }

    void DrawActiveToolBar( const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        auto&                 ms    = Core::ModelingState::Get();
        const ActiveToolLabel label = LabelOf( ms.ActiveTool );
        if ( label.Name == nullptr )
            return;

        ::ImGui::SetNextWindowPos(
             ImVec2( viewportPos.x + viewportSize.x * 0.5f, viewportPos.y + viewportSize.y - 12.0f ),
             ImGuiCond_Always, ImVec2( 0.5f, 1.0f ) );
        ::ImGui::SetNextWindowBgAlpha( 0.92f );
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12.0f, 8.0f ) );
        ::ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 10.0f, 6.0f ) );
        ::ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
        if ( ::ImGui::Begin( "##active_tool_bar", nullptr,
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing ) )
        {
            ::ImGui::AlignTextToFramePadding();
            ::ImGui::Text( "%s  %s", label.Icon, label.Name );
            ::ImGui::SameLine( 0.0f, 16.0f );
            ::ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.55f, 0.30f, 1.0f ) );
            ::ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.68f, 0.38f, 1.0f ) );
            const bool finish =
                 ::ImGui::Button( label.HasCancel ? ICON_MDI_CHECK "  Accept" : ICON_MDI_CHECK "  Complete" );
            ::ImGui::PopStyleColor( 2 );
            bool cancel = false;
            if ( label.HasCancel )
            {
                ::ImGui::SameLine();
                ::ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.22f, 0.22f, 1.0f ) );
                ::ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.70f, 0.28f, 0.28f, 1.0f ) );
                cancel = ::ImGui::Button( ICON_MDI_CLOSE "  Cancel" );
                ::ImGui::PopStyleColor( 2 );
            }
            if ( finish && label.HasCancel )
                ms.ReqAccept = true;
            if ( cancel )
                ms.ReqCancel = true;
            if ( finish || cancel )
                ms.ActiveTool = Core::ModelingState::Tool::None;
        }
        ::ImGui::End();
        ::ImGui::PopStyleVar( 3 );
    }
} // namespace Desert::Editor::Tools
