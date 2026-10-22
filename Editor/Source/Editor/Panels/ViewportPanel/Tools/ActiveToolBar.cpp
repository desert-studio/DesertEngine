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
            // UE's UV tools preview until Accept; Cancel puts the mesh back (UVTool).
            case T::AutoUV:
                return { ICON_MDI_AUTO_FIX, "AutoUV", true };
            case T::UVProject:
                return { ICON_MDI_PROJECTOR, "UV Project", true };
            case T::UVUnwrap:
                return { ICON_MDI_CONTENT_CUT, "UV Unwrap", true };
            case T::UVLayout:
                return { ICON_MDI_VIEW_GRID_OUTLINE, "UV Layout", true };
            case T::UVTransform:
                return { ICON_MDI_CURSOR_MOVE, "UV Transform", true };
            // UE's Deform tools preview until Accept; Cancel puts the mesh back (MeshProcessingTool).
            case T::Smooth:
                return { ICON_MDI_BLUR, "Smooth", true };
            case T::Offset:
                return { ICON_MDI_ARROW_EXPAND_ALL, "Offset", true };
            case T::Warp:
                return { ICON_MDI_ROTATE_ORBIT, "Warp", true };
            case T::Lattice:
                return { ICON_MDI_GRID, "Lattice", true };
            // UE's VSculpt keeps its strokes until Accept; ours commits each stroke as its own undo step, so
            // there is nothing for Cancel to drop - Complete is the honest button (as PolyEdit).
            case T::VertexSculpt:
                return { ICON_MDI_BRUSH, "Vertex Sculpt", false };
            // UE's Normals tool previews until Accept; Cancel puts the mesh back (MeshProcessingTool).
            case T::Normals:
                return { ICON_MDI_ARROW_TOP_RIGHT_THICK, "Normals", true };
            case T::Tangents:
                return { ICON_MDI_ARROW_TOP_RIGHT_THICK, "Tangents", true };
            // UE's Voxel Solidify (VoxWrap) previews until Accept; Cancel puts the mesh back (MeshProcessingTool).
            case T::VoxWrap:
                return { ICON_MDI_CUBE_OUTLINE, "VoxWrap", true };
            // UE's Voxel Boolean previews until Accept, which creates the result entity (MeshProcessingTool).
            case T::VoxBoolean:
                return { ICON_MDI_CUBE_OUTLINE, "VoxBoolean", true };
            // UE's Voxel Morphology previews until Accept, which creates the result entity (MeshProcessingTool).
            case T::VoxMorph:
                return { ICON_MDI_CUBE_SEND, "VoxMorph", true };
            // UE's Voxel Blend previews until Accept, which creates the result entity (MeshProcessingTool).
            case T::VoxBlend:
                return { ICON_MDI_VECTOR_UNION, "VoxBlend", true };
            // UE's Edit Materials previews until Accept; Cancel puts mesh and slots back (EditMaterialsTool).
            case T::EditMaterials:
                return { ICON_MDI_BRUSH, "Edit Materials", true };
            // UE's group paint keeps its strokes until Accept; Cancel drops them (PaintGroupsTool).
            case T::PaintGroups:
                return { ICON_MDI_BRUSH, "Paint PolyGroups", true };
            // UE's Hole Fill previews until Accept; Cancel puts the mesh back (MeshProcessingTool).
            case T::HoleFill:
                return { ICON_MDI_VECTOR_SQUARE_CLOSE, "Hole Fill", true };
            // UE's Jacket previews the removal until Accept; Cancel puts the mesh back (MeshProcessingTool).
            case T::Jacket:
                return { ICON_MDI_LAYERS_MINUS, "Jacket", true };
            // UE's Project previews the remeshed, projected source until Accept; Cancel puts it back.
            case T::Project:
                return { ICON_MDI_VECTOR_COMBINE, "Project", true };
            // UE's Weld Edges previews the merge (the open edges left in red) until Accept; Cancel undoes it.
            case T::WeldEdges:
                return { ICON_MDI_VECTOR_LINK, "Weld Edges", true };
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
            PressToolBar( ms, label.HasCancel, finish, cancel );
        }
        ::ImGui::End();
        ::ImGui::PopStyleVar( 3 );
    }
} // namespace Desert::Editor::Tools
