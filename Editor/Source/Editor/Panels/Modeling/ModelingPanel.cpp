#include "ModelingPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Common/Core/Units.hpp>
#include <ImGui/imgui.h>

#include <algorithm>

namespace Desert::Editor
{
    ModelingPanel::ModelingPanel( const std::shared_ptr<Desert::Core::Scene>& scene )
         : IPanel( "Modeling", /*showPanel=*/false ), m_Scene( scene ) // contextual: Modeling mode opens it
    {
    }

    // The Modeling palette only exists while the viewport is in Modeling mode (enter it via the mode
    // dropdown), mirroring UE5. Now expressed through the shared contextual contract, so it obeys the
    // same pin rule as every other tool panel instead of forcing its own visibility every frame.
    bool ModelingPanel::IsRelevant() const
    {
        return Core::ViewportMode::Get() == Core::EditorMode::Modeling;
    }

    void ModelingPanel::OnUIRender()
    {
        using MS  = Core::ModelingState;
        auto& ms  = MS::Get();
        // The editor's ONE accent colour. A panel that mixes its own blue in is how a UI ends up looking
        // assembled from parts.
        const ImVec4 sel = ThemeManager::GetSelectedColor();

        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 6.0f ) );

        // --- Left category rail: only categories that have a working tool are listed ---
        // UE's rail also has Select / XForm / Deform / Mesh / Voxel / Bake. They are absent, not greyed
        // out, until each has a tool behind it: a category that opens onto "not implemented" is a
        // button that does nothing (owner's decision: hide empty tabs until their tools exist).
        struct Cat
        {
            const char* Icon;
            const char* Name;
        };
        const Cat cats[] = { { ICON_MDI_SHAPE_PLUS, "Create" }, { ICON_MDI_VECTOR_SQUARE, "Model" } };

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 6.0f, 8.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 8.0f ) );
        ImGui::BeginChild( "##modeling_cats", ImVec2( 80.0f, 0.0f ), true );
        for ( int i = 0; i < static_cast<int>( IM_ARRAYSIZE( cats ) ); ++i )
        {
            const bool active = i == m_Category;
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            char label[64];
            std::snprintf( label, sizeof( label ), "%s\n%s", cats[i].Icon, cats[i].Name );
            if ( ImGui::Button( label, ImVec2( -1.0f, 46.0f ) ) )
                m_Category = i;
            if ( active )
                ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );

        ImGui::SameLine();

        // --- Right content: tool grid + tool properties ---
        ImGui::BeginChild( "##modeling_content", ImVec2( 0.0f, 0.0f ), false );

        // Model category: PolyEdit — face select + push/pull on the selected mesh.
        if ( m_Category == 1 )
        {
            const bool active = ms.ActiveTool == MS::Tool::PolyEdit;
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            if ( ImGui::Button( ICON_MDI_VECTOR_SQUARE "  PolyEdit", ImVec2( -1.0f, 30.0f ) ) )
            {
                ms.ActiveTool = MS::Tool::PolyEdit;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( active )
                ImGui::PopStyleColor();
            ImGui::Separator();
            if ( active )
            {
                ImGui::TextUnformatted( "PolyEdit" );
                ImGui::Spacing();
                ImGui::TextDisabled( "Select a mesh (e.g. a CubeGrid blockout)" );
                ImGui::TextDisabled( "in Select mode first, then:" );
                ImGui::TextDisabled( "LMB a face -> highlights green" );
                ImGui::TextDisabled( "LMB-drag the face -> push / pull" );
            }
            else
            {
                ImGui::TextDisabled( "Pick PolyEdit to edit a mesh's faces." );
            }
            ImGui::EndChild();
            ImGui::PopStyleVar( 2 );
            return;
        }

        // Create category: its one working tool. Box / Sphere / Cylinder / Cone / Stairs are not listed
        // as disabled placeholders — they are being rebuilt on EditMesh and appear when they work.
        {
            const bool active = ms.ActiveTool == MS::Tool::CubeGrid;
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            if ( ImGui::Button( ICON_MDI_GRID "  CubeGrid", ImVec2( -1.0f, 30.0f ) ) )
            {
                ms.ActiveTool = MS::Tool::CubeGrid;
                Core::ViewportMode::Set( Core::EditorMode::Modeling ); // selecting a tool enters Modeling mode
            }
            if ( active )
                ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // --- Tool Properties (CubeGrid) ---
        if ( ms.ActiveTool == MS::Tool::CubeGrid )
        {
            ImGui::TextUnformatted( "CubeGrid" );
            ImGui::Spacing();
            // --- Asset Actions / Grid Reinitialization: the two top sections of UE's Cube Grid Tool ---
            if ( Utils::ImGuiUtilities::SectionHeader( "Asset Actions" ) )
            {
                if ( ImGui::Button( "Accept and Start New", ImVec2( -1.0f, 0.0f ) ) )
                    ms.ReqAccept = true;
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Keep what you built as a mesh and start a fresh grid.\n"
                                       "The grid frame and Block Size carry over." );
            }
            if ( Utils::ImGuiUtilities::SectionHeader( "Grid Reinitialization" ) )
            {
                if ( ImGui::Button( "Reset Grid from Actor", ImVec2( -1.0f, 0.0f ) ) )
                    ms.ReqResetFromActor = true;
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Put the grid origin on the SELECTED object's origin, so every block\n"
                                       "size stays flush with its corners instead of tiling from (0,0,0)." );
            }
            if ( Utils::ImGuiUtilities::SectionHeader( "Options" ) )
            {
                // Moving the frame commits the current piece (cells are lattice indices) and re-tiles from
                // the new origin — already-built geometry keeps the frame it was made in and never moves.
                ImGui::SetNextItemWidth( -1.0f );
                ImGui::DragFloat3( "Grid Frame Origin", &ms.GridOrigin.x, 1.0f, 0.0f, 0.0f, "%.0f" );
                ImGui::Checkbox( "Show Gizmo", &ms.ShowGizmo );

                // Grid Power: block size = 1 m >> power (Power 2 = 25 cm), like UE's slider. Typing a free
                // Current Block Size below still wins — the power just snaps to the nearest step.
                int power = 0;
                for ( float sz = MS::BaseBlockSize; power < MS::MaxGridPower && sz > ms.CellSize + 0.01f; ++power )
                    sz *= 0.5f;
                ImGui::SetNextItemWidth( 120.0f );
                if ( ImGui::SliderInt( "Grid Power", &power, 0, MS::MaxGridPower ) )
                    ms.CellSize =
                         std::max( MS::MinCellSize, MS::BaseBlockSize / static_cast<float>( 1 << power ) );
            }
            if ( Utils::ImGuiUtilities::SectionHeader( "Corner Mode" ) )
            {
                // Ramps / roofs / wedges: pick the selection's corner posts and raise or lower them.
                if ( ms.CornerMode )
                    ImGui::PushStyleColor( ImGuiCol_Button, sel );
                if ( ImGui::Button( ms.CornerMode ? "Corner Mode: ON  (Z)" : "Corner Mode: OFF  (Z)",
                                    ImVec2( -1.0f, 0.0f ) ) )
                    ms.ReqCornerMode = true;
                if ( ms.CornerMode )
                    ImGui::PopStyleColor();

                // Snap Size — how far one E/Q press moves a post, as a fraction of the block height.
                const char* const snaps[] = { "1/2 block", "1/4 block", "1/10 block" };
                const int         divs[]  = { 2, 4, 10 };
                int               cur     = 0;
                for ( int i = 0; i < 3; ++i )
                    if ( divs[i] == ms.CornerSnapDiv )
                        cur = i;
                ImGui::SetNextItemWidth( -1.0f );
                if ( ImGui::Combo( "Snap Size", &cur, snaps, 3 ) )
                    ms.CornerSnapDiv = divs[cur];

                ImGui::TextDisabled( "Select a rectangle, press Z, click the" );
                ImGui::TextDisabled( "corner posts (Shift adds), then E / Q." );
            }
            if ( Utils::ImGuiUtilities::SectionHeader( "Block Selection" ) )
            {
                ImGui::Checkbox( "Hit Unrelated Geometry", &ms.HitUnrelated );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Target other objects in the scene too, so you can start a grid on\n"
                                       "top of an existing mesh (bounding-box level)." );
            }
            // UE titles this section "Output Type"; ours has no type choice yet (Accept makes one kind of
            // mesh), so the section is named for what it does hold.
            if ( Utils::ImGuiUtilities::SectionHeader( "Collision" ) )
            {
                ImGui::Checkbox( "Generate Collision", &ms.GenerateCollision );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Accept also adds a BOX collider around the piece and a static body,\n"
                                       "so you can walk into it right away.\n"
                                       "It is the bounding box, not a triangle mesh: a concave blockout is\n"
                                       "solid inside until the physics layer grows a mesh shape." );
            }
            if ( Utils::ImGuiUtilities::SectionHeader( "Grid" ) )
            {
                const float kMinBlock = MS::MinCellSize;
                // Block Size (grid step) in world units = CENTIMETRES, like UE: 100 is a one-metre block.
                // Already-drawn blocks never change; a coarser step just stamps bigger, a step finer than the
                // base subdivides the base. The value snaps to a base multiple after editing (the tool shows
                // the effective size).
                ImGui::SetNextItemWidth( 96.0f );
                if ( ImGui::DragFloat( "Current Block Size", &ms.CellSize, 1.0f, kMinBlock, 100000.0f,
                                       "%.0f cm" ) )
                    ms.CellSize = std::max( kMinBlock, ms.CellSize );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "/2##bs" ) )
                    ms.CellSize = std::max( ms.CellSize * 0.5f, kMinBlock );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "x2##bs" ) )
                    ms.CellSize = std::min( ms.CellSize * 2.0f, 100000.0f );
                // Blocks Per Step: how many cells one Push/Pull moves (UE multiplier).
                ImGui::SetNextItemWidth( 120.0f );
                if ( ImGui::SliderInt( "Blocks / Step", &ms.BlocksPerStep, 1, 32 ) )
                    ms.BlocksPerStep = std::max( 1, ms.BlocksPerStep );
                ImGui::Text( "Cells: %d", ms.Cubes );
                if ( ImGui::Button( "Clear" ) )
                    ms.ReqClear = true;
            }
            ImGui::Separator();
            // Shortcut Info — same block UE shows at the bottom of the Cube Grid Tool panel.
            if ( Utils::ImGuiUtilities::SectionHeader( "Shortcut Info" ) )
            {
                const struct
                {
                    const char* Action;
                    const char* Keys;
                } shortcuts[] = {
                     { "Select blocks", "LMB drag on the surface" },
                     { "Push / Pull", "E / Q" },
                     { "Corner Mode", "Z (then E / Q on posts)" },
                     { "Resize Grid", "Ctrl + E / Q" },
                     { "Shift work-plane", "Ctrl + Mouse Wheel" },
                     { "Snap grid to surface", "Ctrl + MMB" },
                     { "Clear selection", "Esc" },
                     { "Fly the camera", "RMB + WASD / Q / E" },
                };
                ImGui::Columns( 2, "##cg_keys", false );
                ImGui::SetColumnWidth( 0, 130.0f );
                for ( const auto& s : shortcuts )
                {
                    ImGui::TextDisabled( "%s", s.Action );
                    ImGui::NextColumn();
                    ImGui::TextUnformatted( s.Keys );
                    ImGui::NextColumn();
                }
                ImGui::Columns( 1 );
            }
            ImGui::TextDisabled( "Accept / Cancel: bottom of the viewport" );
        }
        else
        {
            ImGui::TextDisabled( "Pick a tool above (CubeGrid) to begin." );
        }
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );
    }
} // namespace Desert::Editor
