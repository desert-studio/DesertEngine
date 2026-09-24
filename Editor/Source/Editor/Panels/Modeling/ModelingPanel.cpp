#include "ModelingPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/MeshXformOperations.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Common/Core/Units.hpp>
#include <Common/Core/Logger.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <array>

namespace Desert::Editor
{
    namespace
    {
        const char* ShapeIcon( Core::ModelingState::Shape shape )
        {
            using S = Core::ModelingState::Shape;
            switch ( shape )
            {
                case S::Box:
                    return ICON_MDI_CUBE_OUTLINE;
                case S::Sphere:
                    return ICON_MDI_SPHERE;
                case S::Cylinder:
                    return ICON_MDI_CYLINDER;
                case S::Cone:
                    return ICON_MDI_CONE;
                case S::Capsule:
                    return ICON_MDI_PILL;
                case S::Pyramid:
                    return ICON_MDI_TRIANGLE_OUTLINE;
                case S::Stairs:
                    return ICON_MDI_STAIRS;
            }
            return ICON_MDI_SHAPE_PLUS;
        }

        const char* ElementModeIcon( Geometry::ElementMode mode )
        {
            switch ( mode )
            {
                case Geometry::ElementMode::Vertex:
                    return ICON_MDI_VECTOR_POINT;
                case Geometry::ElementMode::Edge:
                    return ICON_MDI_VECTOR_LINE;
                case Geometry::ElementMode::Triangle:
                    return ICON_MDI_VECTOR_TRIANGLE;
                case Geometry::ElementMode::PolyGroup:
                    return ICON_MDI_VECTOR_POLYGON;
            }
            return ICON_MDI_VECTOR_SELECTION;
        }

        // UE 5.8 ModelingToolsEditorModeToolkit.cpp:1717-1737 (PaletteNames_Standard), with the Selection
        // palette first as UE inserts it while mesh selection is on (:1756): only the palettes something of
        // ours lives in, in UE's order.
        enum class Palette
        {
            Selection,
            Shapes,
            Create,
            PolyModel,
            TriModel,
            Transform,
        };
        struct PaletteEntry
        {
            const char* Icon;
            const char* Name;
        };
        constexpr std::array<PaletteEntry, 6> kPalettes = { { { ICON_MDI_VECTOR_SELECTION, "Selection" },
                                                              { ICON_MDI_SHAPE_PLUS, "Shapes" },
                                                              { ICON_MDI_PLUS_BOX_OUTLINE, "Create" },
                                                              { ICON_MDI_VECTOR_SQUARE, "PolyModel" },
                                                              { ICON_MDI_VECTOR_TRIANGLE, "TriModel" },
                                                              { ICON_MDI_ARROW_ALL, "Transform" } } };
        static_assert( kPalettes.size() == static_cast<size_t>( Palette::Transform ) + 1,
                       "one rail entry per palette" );
    } // namespace

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
        using MS = Core::ModelingState;
        auto& ms = MS::Get();

        // The rail follows a tool chosen from outside the panel (the palette, the control channel), so the
        // properties of the tool that is active are the ones on screen. A person's own click on the rail
        // still wins until the tool changes again.
        if ( static_cast<int>( ms.ActiveTool ) != m_ShownTool )
        {
            m_ShownTool = static_cast<int>( ms.ActiveTool );
            if ( ms.ActiveTool == MS::Tool::CreateShape )
                m_Category = static_cast<int>( Palette::Shapes );
            else if ( ms.ActiveTool == MS::Tool::CubeGrid || ms.ActiveTool == MS::Tool::PolyEdit )
                m_Category = static_cast<int>( Palette::PolyModel );
            else if ( ms.ActiveTool == MS::Tool::ElementSelect )
                m_Category = static_cast<int>( Palette::Selection );
        }

        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 6.0f ) );

        // --- Left category rail: only the palettes something of ours lives in ---
        // UE's other palettes (Deform, MeshOps, VoxOps, Attributes, UVs, Baking, Volumes, LODs) are absent, not
        // greyed out, until each has a tool behind it: a category that opens onto "not implemented" is a button
        // that does nothing (owner's decision: hide empty tabs until their tools exist).
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 6.0f, 8.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 8.0f ) );
        ImGui::BeginChild( "##modeling_cats", ImVec2( 80.0f, 0.0f ), true );
        const ImVec4 sel = ThemeManager::GetSelectedColor();
        for ( int i = 0; i < static_cast<int>( kPalettes.size() ); ++i )
        {
            const bool active = i == m_Category;
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            char label[64];
            std::snprintf( label, sizeof( label ), "%s\n%s", kPalettes[i].Icon, kPalettes[i].Name );
            if ( ImGui::Button( label, ImVec2( -1.0f, 46.0f ) ) )
                m_Category = i;
            if ( active )
                ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );

        ImGui::SameLine();

        // --- Right content: the palette's tool buttons, then the active tool's properties ---
        ImGui::BeginChild( "##modeling_content", ImVec2( 0.0f, 0.0f ), false );
        switch ( static_cast<Palette>( m_Category ) )
        {
            case Palette::Selection:
                DrawSelectionPalette();
                break;
            case Palette::Shapes:
                DrawShapesPalette();
                break;
            case Palette::Create:
                DrawCreatePalette();
                break;
            case Palette::PolyModel:
                DrawPolyModelPalette();
                break;
            case Palette::TriModel:
                DrawTriModelPalette();
                break;
            case Palette::Transform:
                DrawTransformPalette();
                break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar( 2 );
    }

    void ModelingPanel::Operate( Core::MeshOperation op )
    {
        if ( !m_Scene )
        {
            LOG_WARN( "Mesh {0}: the Modeling panel has no scene", Core::ToString( op ) );
            return;
        }
        if ( const auto done = Core::ApplyMeshOperation( *m_Scene, op, Core::ArgsFromModelingState() ); !done )
            LOG_WARN( "{0}", done.GetError() );
    }

    void ModelingPanel::Transform( Core::XformOperation op )
    {
        if ( !m_Scene )
        {
            LOG_WARN( "{0}: the Modeling panel has no scene", Core::ToString( op ) );
            return;
        }
        if ( const auto done = Core::ApplyXformOperation( *m_Scene, op, Core::XformArgsFromModelingState() );
             !done )
            LOG_WARN( "{0}", done.GetError() );
    }

    void ModelingPanel::DrawSelectionPalette()
    {
        using MS               = Core::ModelingState;
        auto&        ms        = MS::Get();
        const ImVec4 sel       = ThemeManager::GetSelectedColor();
        const bool   selecting = ms.ActiveTool == MS::Tool::ElementSelect;
        if ( selecting )
            ImGui::PushStyleColor( ImGuiCol_Button, sel );
        if ( ImGui::Button( ICON_MDI_VECTOR_SELECTION "  Select Elements", ImVec2( -1.0f, 30.0f ) ) )
        {
            ms.ActiveTool = MS::Tool::ElementSelect;
            Core::ViewportMode::Set( Core::EditorMode::Modeling );
        }
        if ( selecting )
            ImGui::PopStyleColor();
        ImGui::Separator();
        if ( selecting )
            DrawElementSelection();
        else
            ImGui::TextDisabled( "Pick Select Elements to pick a mesh's parts." );
    }

    void ModelingPanel::DrawShapesPalette()
    {
        using MS            = Core::ModelingState;
        auto&        ms     = MS::Get();
        const ImVec4 sel    = ThemeManager::GetSelectedColor();
        const float  half   = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        int          column = 0;
        for ( const MS::Shape shape : MS::kShapes )
        {
            const bool on = ms.ActiveTool == MS::Tool::CreateShape && ms.CreateShape.Kind == shape;
            if ( on )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            if ( column % 2 == 1 )
                ImGui::SameLine();
            // UE's Shapes palette: an icon over each primitive's name (ModelingToolsEditorModeStyle).
            char shapeLabel[64];
            std::snprintf( shapeLabel, sizeof( shapeLabel ), "%s  %s", ShapeIcon( shape ),
                           MS::ShapeName( shape ) );
            if ( ImGui::Button( shapeLabel, ImVec2( half, 26.0f ) ) )
            {
                ms.ActiveTool       = MS::Tool::CreateShape;
                ms.CreateShape.Kind = shape;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( on )
                ImGui::PopStyleColor();
            ++column;
        }
        ImGui::Separator();
        if ( ms.ActiveTool == MS::Tool::CreateShape )
            DrawCreateShape();
        else
            ImGui::TextDisabled( "Pick a shape above to place it." );
    }

    // UE's Create palette: of its tools we have Merge (UE's Combine Meshes) and Pattern, both operations on the
    // scene selection that run on one click; in UE each is a tool with its own Accept.
    void ModelingPanel::DrawCreatePalette()
    {
        auto&       ms       = Core::ModelingState::Get();
        const float half     = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        const char* axes[]   = { "X", "Y", "Z" };
        using XO             = Core::XformOperation;
        const auto transform = [this]( XO op ) { Transform( op ); };
        if ( Utils::ImGuiUtilities::SectionHeader( "Merge" ) )
        {
            if ( ImGui::Button( Core::ToString( XO::Merge ), ImVec2( -1.0f, 0.0f ) ) )
                transform( XO::Merge );
        }
        if ( !Utils::ImGuiUtilities::SectionHeader( "Pattern" ) )
            return;
        Geometry::PatternSettings& pattern  = ms.XformPattern;
        const char*                shapes[] = { "Line", "Grid", "Circle" };
        int                        shape    = static_cast<int>( pattern.Shape );
        ImGui::SetNextItemWidth( half );
        if ( ImGui::Combo( "##XformPatternShape", &shape, shapes, 3 ) )
            pattern.Shape = static_cast<Geometry::PatternShape>( shape );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( half );
        ImGui::Combo( "##XformPatternAxis", &pattern.AxisA, axes, 3 );
        ImGui::SetNextItemWidth( half );
        ImGui::DragInt( "##XformPatternCount", &pattern.Count, 0.1f, 1, Geometry::kMaxPatternCopies, "Count %d" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( half );
        if ( pattern.Shape == Geometry::PatternShape::Circle )
            ImGui::DragFloat( "##XformPatternRadius", &pattern.Radius, 1.0f, 0.0f, 1.0e6f, "Radius %.1f cm" );
        else
            ImGui::DragFloat( "##XformPatternSpacing", &pattern.Spacing, 1.0f, -1.0e6f, 1.0e6f,
                              "Spacing %.1f cm" );
        if ( pattern.Shape == Geometry::PatternShape::Grid )
        {
            ImGui::SetNextItemWidth( half );
            ImGui::Combo( "##XformPatternAxisB", &pattern.AxisB, axes, 3 );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( half );
            ImGui::DragInt( "##XformPatternCountB", &pattern.CountB, 0.1f, 1, Geometry::kMaxPatternCopies,
                            "Count %d" );
            ImGui::SetNextItemWidth( half );
            ImGui::DragFloat( "##XformPatternSpacingB", &pattern.SpacingB, 1.0f, -1.0e6f, 1.0e6f,
                              "Spacing %.1f cm" );
        }
        if ( pattern.Shape == Geometry::PatternShape::Circle )
        {
            ImGui::SetNextItemWidth( half );
            ImGui::DragFloat( "##XformPatternSweep", &pattern.SweepDegrees, 1.0f, -360.0f, 360.0f,
                              "Sweep %.0f deg" );
            ImGui::SameLine();
            ImGui::Checkbox( "Orient", &pattern.OrientToCircle );
        }
        ImGui::Checkbox( "Separate entities##Pattern", &ms.XformPatternSeparate );
        if ( ImGui::Button( Core::ToString( XO::Pattern ), ImVec2( -1.0f, 0.0f ) ) )
            transform( XO::Pattern );

        ImGui::Spacing();
    }

    // UE's PolyModel palette: PolyEdit, CubeGrid and Subdivide (UE's SubdividePoly tool; here an operation on
    // the Select Elements mesh).
    void ModelingPanel::DrawPolyModelPalette()
    {
        using MS            = Core::ModelingState;
        auto&        ms     = MS::Get();
        const ImVec4 sel    = ThemeManager::GetSelectedColor();
        const bool   active = ms.ActiveTool == MS::Tool::PolyEdit;
        if ( active )
            ImGui::PushStyleColor( ImGuiCol_Button, sel );
        if ( ImGui::Button( ICON_MDI_VECTOR_SQUARE "  PolyEdit", ImVec2( -1.0f, 30.0f ) ) )
        {
            ms.ActiveTool = MS::Tool::PolyEdit;
            Core::ViewportMode::Set( Core::EditorMode::Modeling );
        }
        if ( active )
            ImGui::PopStyleColor();
        const bool grid = ms.ActiveTool == MS::Tool::CubeGrid;
        if ( grid )
            ImGui::PushStyleColor( ImGuiCol_Button, sel );
        if ( ImGui::Button( ICON_MDI_GRID "  CubeGrid", ImVec2( -1.0f, 30.0f ) ) )
        {
            ms.ActiveTool = MS::Tool::CubeGrid;
            Core::ViewportMode::Set( Core::EditorMode::Modeling ); // selecting a tool enters Modeling mode
        }
        if ( grid )
            ImGui::PopStyleColor();
        if ( Utils::ImGuiUtilities::SectionHeader( "Subdivide" ) )
        {
            const float half   = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
            using MO           = Core::MeshOperation;
            const auto operate = [this]( MO op ) { Operate( op ); };
            ImGui::SetNextItemWidth( half );
            ImGui::SliderInt( "##ElementSubdivideLevels", &ms.ElementSubdivideLevels, 1,
                              Geometry::kMaxSubdivideLevels, "Levels %d" );
            ImGui::SameLine();
            bool loop = ms.ElementSubdivideScheme == Geometry::SubdivideScheme::Loop;
            if ( ImGui::Checkbox( "Smooth (Loop)", &loop ) )
                ms.ElementSubdivideScheme =
                     loop ? Geometry::SubdivideScheme::Loop : Geometry::SubdivideScheme::Uniform;
            if ( ImGui::Button( Core::ToString( MO::Subdivide ), ImVec2( -1.0f, 0.0f ) ) )
                operate( MO::Subdivide );
        }
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
        else if ( grid )
            DrawCubeGrid();
        else
            ImGui::TextDisabled( "Pick a tool above to begin." );
    }

    // UE's TriModel palette: Mirror, Plane Cut and Trim (UE's Mesh Trim). In UE each is a tool with a gizmo and
    // its own Accept; here each is an operation on the Select Elements mesh that runs on one click.
    void ModelingPanel::DrawTriModelPalette()
    {
        auto&       ms      = Core::ModelingState::Get();
        const float half    = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        using MO            = Core::MeshOperation;
        const auto  operate = [this]( MO op ) { Operate( op ); };
        const char* axes[]  = { "X", "Y", "Z" };
        ImGui::SetNextItemWidth( half );
        ImGui::Combo( "##ElementMirrorAxis", &ms.ElementMirrorAxis, axes, 3 );
        ImGui::SameLine();
        ImGui::Checkbox( "World", &ms.ElementMirrorWorld );
        ImGui::SameLine();
        ImGui::Checkbox( "Keep -", &ms.ElementMirrorKeepNegative );
        bool cut = ms.ElementMirrorMode == Geometry::MirrorMode::CutAndMirror;
        if ( ImGui::Checkbox( "Cut the far half first", &cut ) )
            ms.ElementMirrorMode =
                 cut ? Geometry::MirrorMode::CutAndMirror : Geometry::MirrorMode::AddMirroredCopy;
        if ( ImGui::Button( Core::ToString( MO::Mirror ), ImVec2( -1.0f, 0.0f ) ) )
            operate( MO::Mirror );

        // Plane Cut (UE's Plane Cut tool): an axis plane at an offset; the positive side is kept.
        ImGui::SetNextItemWidth( half );
        ImGui::Combo( "##ElementPlaneCutAxis", &ms.ElementPlaneCutAxis, axes, 3 );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( half );
        ImGui::DragFloat( "##ElementPlaneCutOffset", &ms.ElementPlaneCutOffset, 0.5f, -100000.0f, 100000.0f,
                          "At %.1f cm" );
        ImGui::Checkbox( "World##PlaneCut", &ms.ElementPlaneCutWorld );
        ImGui::SameLine();
        ImGui::Checkbox( "Keep -##PlaneCut", &ms.ElementPlaneCutKeepNegative );
        ImGui::SameLine();
        ImGui::Checkbox( "Fill##PlaneCut", &ms.ElementPlaneCutFill );
        bool both = ms.ElementPlaneCutMode == Geometry::PlaneCutMode::KeepBothHalves;
        if ( ImGui::Checkbox( "Keep both halves (new entity)", &both ) )
            ms.ElementPlaneCutMode =
                 both ? Geometry::PlaneCutMode::KeepBothHalves : Geometry::PlaneCutMode::DiscardNegativeSide;
        if ( ImGui::Button( Core::ToString( MO::PlaneCut ), ImVec2( -1.0f, 0.0f ) ) )
            operate( MO::PlaneCut );

        // Trim (UE's Trim tool): another entity's closed convex mesh cuts this one; the cut stays open.
        if ( ImGui::Button( "Pick Cutter", ImVec2( half, 0.0f ) ) )
        {
            if ( const auto picked = PickTrimCutterFromSelection(); !picked )
                LOG_WARN( "Mesh Trim: {}", picked.GetError() );
        }
        ImGui::SameLine();
        if ( ms.ElementTrimCutter.IsNull() )
            ImGui::TextDisabled( "no cutter" );
        else
            ImGui::Text( "cutter %llu",
                         static_cast<unsigned long long>( static_cast<uint64_t>( ms.ElementTrimCutter ) ) );
        bool outside = ms.ElementTrimSide == Geometry::TrimSide::RemoveOutside;
        if ( ImGui::Checkbox( "Keep only the inside", &outside ) )
            ms.ElementTrimSide = outside ? Geometry::TrimSide::RemoveOutside : Geometry::TrimSide::RemoveInside;
        if ( ImGui::Button( Core::ToString( MO::Trim ), ImVec2( -1.0f, 0.0f ) ) )
            operate( MO::Trim );
    }

    // UE's Transform palette: Edit Pivot, Bake Transform and Split. In UE each is a tool with its own Accept;
    // here each is an operation on the scene selection's whole entities that runs on one click.
    void ModelingPanel::DrawTransformPalette()
    {
        auto&       ms        = Core::ModelingState::Get();
        const float half      = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        using XO              = Core::XformOperation;
        const auto  transform = [this]( XO op ) { Transform( op ); };
        const char* pivots[]  = { "Bounds Center", "Bounds Base", "World Origin", "World Point" };
        int         pivot     = static_cast<int>( ms.XformPivot );
        ImGui::SetNextItemWidth( half );
        if ( ImGui::Combo( "##XformPivot", &pivot, pivots, 4 ) )
            ms.XformPivot = static_cast<Geometry::PivotLocation>( pivot );
        if ( ms.XformPivot == Geometry::PivotLocation::WorldPoint )
            ImGui::DragFloat3( "##XformPivotPoint", &ms.XformPivotWorldPoint.x, 1.0f, -1.0e6f, 1.0e6f, "%.1f cm" );
        ImGui::SameLine();
        if ( ImGui::Button( Core::ToString( XO::EditPivot ), ImVec2( -1.0f, 0.0f ) ) )
            transform( XO::EditPivot );
        ImGui::Checkbox( "Rotation##Bake", &ms.XformBake.Rotation );
        ImGui::SameLine();
        ImGui::Checkbox( "Scale##Bake", &ms.XformBake.Scale );
        ImGui::SameLine();
        ImGui::Checkbox( "Location##Bake", &ms.XformBake.Translation );
        if ( ImGui::Button( Core::ToString( XO::BakeTransform ), ImVec2( -1.0f, 0.0f ) ) )
            transform( XO::BakeTransform );
        if ( ImGui::Button( Core::ToString( XO::Split ), ImVec2( -1.0f, 0.0f ) ) )
            transform( XO::Split );
        bool byGroups = ms.XformSplit == Geometry::SplitMethod::PolyGroups;
        if ( ImGui::Checkbox( "Split by polygroups", &byGroups ) )
            ms.XformSplit =
                 byGroups ? Geometry::SplitMethod::PolyGroups : Geometry::SplitMethod::ConnectedComponents;
    }

    void ModelingPanel::DrawCubeGrid()
    {
        using MS         = Core::ModelingState;
        auto&        ms  = MS::Get();
        const ImVec4 sel = ThemeManager::GetSelectedColor();
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
        DrawOutputType();
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
            int power = ms.GridPower();
            ImGui::SetNextItemWidth( 120.0f );
            if ( ImGui::SliderInt( "Grid Power", &power, 0, MS::MaxGridPower ) )
                ms.SetGridPower( power );
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
            if ( ImGui::DragFloat( "Current Block Size", &ms.CellSize, 1.0f, kMinBlock, 100000.0f, "%.0f cm" ) )
                ms.CellSize = std::max( kMinBlock, ms.CellSize );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "/2##bs" ) )
                ms.HalveBlockSize();
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x2##bs" ) )
                ms.DoubleBlockSize();
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

    void ModelingPanel::DrawCreateShape()
    {
        using MS      = Core::ModelingState;
        auto&      ms = MS::Get();
        auto&      s  = ms.CreateShape;
        // UE's ranges (AddPrimitiveTool.h:124-438): the slider spans the UI range, a typed value may go to
        // the clamp range. Sizes: UI 1..1000 cm, clamp up to 1e6 cm; the low clamp is the generator's own
        // smallest extent (1 mm) rather than UE's 1e-4 cm, so the panel never shows a size the mesh ignores.
        const auto cm = []( const char* label, float* value, const char* tip )
        {
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::DragFloat( label, value, 1.0f, 1.0f, 1000.0f, "%.1f cm" ) )
                *value = std::clamp( *value, Geometry::Detail::kMinExtent, 1000000.0f );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s\nDrag 1..1000 cm; Ctrl+click to type up to 1000000.", tip );
        };
        const auto count = []( const char* label, int* value, int uiLowest, int uiHighest, int clampHighest )
        {
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::SliderInt( label, value, uiLowest, uiHighest ) )
                *value = std::clamp( *value, uiLowest, clampHighest );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Drag %d..%d; Ctrl+click to type up to %d.", uiLowest, uiHighest,
                                   clampHighest );
        };

        ImGui::TextUnformatted( MS::ShapeName( s.Kind ) );
        ImGui::Spacing();
        if ( Utils::ImGuiUtilities::SectionHeader( "Shape" ) )
        {
            // Only the fields the chosen shape reads: a field shown here always moves the shape.
            const bool round = s.Kind == MS::Shape::Sphere || s.Kind == MS::Shape::Cylinder ||
                               s.Kind == MS::Shape::Cone || s.Kind == MS::Shape::Capsule;
            cm( round ? "Diameter" : "Width", &s.Width, round ? "Full width across the axis." : "X extent." );
            if ( s.Kind == MS::Shape::Box || s.Kind == MS::Shape::Pyramid )
                cm( "Depth", &s.Depth, "Z extent." );
            if ( s.Kind != MS::Shape::Sphere && s.Kind != MS::Shape::Stairs )
                cm( "Height", &s.Height,
                    s.Kind == MS::Shape::Capsule ? "End to end, never less than the diameter." : "Y extent." );
            if ( s.Kind == MS::Shape::Box )
                count( "Subdivisions", &s.Subdivisions, 1, 100, 500 );
            if ( round )
                count( "Slices", &s.Slices, 3, 128, 500 );
            if ( s.Kind == MS::Shape::Sphere || s.Kind == MS::Shape::Capsule )
                count( "Stacks", &s.Stacks, 4, 100, 500 );
            if ( s.Kind == MS::Shape::Stairs )
            {
                count( "Steps", &s.Steps, 2, 100, 1000000 );
                cm( "Step Depth", &s.StepDepth, "Tread depth, along +Z." );
                cm( "Step Height", &s.StepHeight, "Riser height." );
            }
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Polygroups and Pivot" ) )
        {
            constexpr Geometry::ShapePolygroupMode modes[] = { Geometry::ShapePolygroupMode::PerFace,
                                                               Geometry::ShapePolygroupMode::PerQuad,
                                                               Geometry::ShapePolygroupMode::Single };
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::BeginCombo( "Polygroups", Geometry::ToString( s.Groups ) ) )
            {
                for ( const auto mode : modes )
                    if ( ImGui::Selectable( Geometry::ToString( mode ), mode == s.Groups ) )
                        s.Groups = mode;
                ImGui::EndCombo();
            }
            constexpr Geometry::ShapePivot pivots[] = { Geometry::ShapePivot::Base, Geometry::ShapePivot::Centre,
                                                        Geometry::ShapePivot::Top };
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::BeginCombo( "Pivot", Geometry::ToString( s.Pivot ) ) )
            {
                for ( const auto pivot : pivots )
                    if ( ImGui::Selectable( Geometry::ToString( pivot ), pivot == s.Pivot ) )
                        s.Pivot = pivot;
                ImGui::EndCombo();
            }
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Placement" ) )
        {
            bool onScene = s.Place == MS::Placement::OnScene;
            if ( ImGui::Checkbox( "Place on Scene", &onScene ) )
                s.Place = onScene ? MS::Placement::OnScene : MS::Placement::Ground;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "On: the click lands on the object under the cursor (its bounding box),\n"
                                   "or on the ground where there is none. Off: always the ground, Y = 0." );
        }
        DrawOutputType();
        ImGui::Spacing();
        ImGui::TextDisabled( "LMB in the viewport places the shape." );
        ImGui::TextDisabled( "Ctrl+Z removes it again." );
    }

    // UE's "Output Type" section (UCreateMeshObjectTypeProperties), shared by the creating tools.
    Common::BoolResultStr ModelingPanel::PickTrimCutterFromSelection()
    {
        // The first selected entity that is not the one being edited.
        Core::ModelingState::Get().ElementTrimCutter = Common::UUID::Null();
        for ( const Common::UUID& id : Core::SelectionManager::GetSelection() )
            if ( id != Core::MeshElementSelection::Get().Entity() )
                return PickTrimCutter( id );
        return Common::MakeError<bool>( "select the cutter entity (besides the edited one) before Pick Cutter" );
    }

    Common::BoolResultStr ModelingPanel::PickTrimCutter( const Common::UUID& cutter )
    {
        if ( cutter == Core::MeshElementSelection::Get().Entity() )
            return Common::MakeError<bool>( "the entity being edited cannot be its own Trim cutter" );
        Core::ModelingState::Get().ElementTrimCutter = cutter;
        return Common::MakeSuccess( true );
    }

    void ModelingPanel::DrawOutputType()
    {
        using MS  = Core::ModelingState;
        auto& out = MS::Get().Output;
        if ( !Utils::ImGuiUtilities::SectionHeader( "Output Type" ) )
            return;
        static constexpr std::array<const char*, 2> kTypes = { "Static Mesh", "Dynamic Mesh" };
        int                                         type   = out.Type == MS::OutputType::StaticMesh ? 0 : 1;
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::Combo( "##OutputType", &type, kTypes.data(), static_cast<int>( kTypes.size() ) ) )
            out.Type = type == 0 ? MS::OutputType::StaticMesh : MS::OutputType::Dynamic;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Static Mesh: Accept writes a new .stmesh asset and the object draws it.\n"
                               "Dynamic Mesh: the mesh stays on the object, editable, saved in the scene." );
        if ( out.Type != MS::OutputType::StaticMesh )
            return;

        // Plain buffers round-tripped through the settings' strings: the panel has no std::string input.
        std::array<char, 128> folder{};
        std::array<char, 128> name{};
        out.Folder.copy( folder.data(), folder.size() - 1 );
        out.Name.copy( name.data(), name.size() - 1 );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::InputTextWithHint( "##OutputFolder", "Asset folder", folder.data(), folder.size() ) )
            out.Folder = folder.data();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Folder inside Cooked/Meshes. A taken name gets _1, _2, ... - never overwritten." );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::InputTextWithHint( "##OutputName", "Asset name (object name)", name.data(), name.size() ) )
            out.Name = name.data();
    }

    void ModelingPanel::DrawElementSelection()
    {
        using Geometry::ElementMode;
        using Op          = Core::MeshElementSelection::Op;
        auto&      state  = Core::MeshElementSelection::Get();
        const auto report = []( const Common::BoolResultStr& result )
        {
            if ( !result.IsSuccess() )
                LOG_WARN( "{0}", result.GetError() );
        };

        ImGui::TextUnformatted( "Select Elements" );
        ImGui::Spacing();
        constexpr ElementMode modes[] = { ElementMode::Vertex, ElementMode::Edge, ElementMode::Triangle,
                                          ElementMode::PolyGroup };
        // UE's element-type buttons. Choosing another type converts the selection (ConvertSelection): the same
        // part of the mesh, named in the new type's elements.
        const float quarter =
             ( ImGui::GetContentRegionAvail().x - 3.0f * ImGui::GetStyle().ItemSpacing.x ) * 0.25f;
        for ( const ElementMode mode : modes )
        {
            if ( mode != modes[0] )
                ImGui::SameLine();
            const bool current = state.Mode() == mode;
            if ( current )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            char modeLabel[48];
            std::snprintf( modeLabel, sizeof( modeLabel ), "%s##mode%d", ElementModeIcon( mode ),
                           static_cast<int>( mode ) );
            if ( ImGui::Button( modeLabel, ImVec2( quarter, 0.0f ) ) )
                report( state.SetMode( mode ) );
            if ( current )
                ImGui::PopStyleColor();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s (converts the selection)", Geometry::ToString( mode ) );
        }
        // UE's TriEdit: Vertex / Edge picks every mesh vertex and edge instead of group corners and edges.
        // Two buttons, as UE's PolyEd / TriSel pair on its Selection palette, instead of a checkbox.
        const bool   triangles = state.Level() == Geometry::TopologyLevel::Triangle;
        const float  levelW    = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        const ImVec4 on        = ThemeManager::GetSelectedColor();
        if ( !triangles )
            ImGui::PushStyleColor( ImGuiCol_Button, on );
        if ( ImGui::Button( ICON_MDI_VECTOR_POLYGON " PolyEdit##level", ImVec2( levelW, 0.0f ) ) )
            state.SetLevel( Geometry::TopologyLevel::Group );
        if ( !triangles )
            ImGui::PopStyleColor();
        ImGui::SameLine();
        if ( triangles )
            ImGui::PushStyleColor( ImGuiCol_Button, on );
        if ( ImGui::Button( ICON_MDI_VECTOR_TRIANGLE " TriEdit##level", ImVec2( levelW, 0.0f ) ) )
            state.SetLevel( Geometry::TopologyLevel::Triangle );
        if ( triangles )
            ImGui::PopStyleColor();
        ImGui::Spacing();
        if ( !state.HasMesh() )
        {
            ImGui::TextDisabled( "Select an entity with an editable mesh" );
            ImGui::TextDisabled( "(e.g. a CubeGrid blockout) first." );
            return;
        }
        ImGui::Text( "Selected: %d %s(s)", state.Selection().Size(), Geometry::ToString( state.Mode() ) );
        if ( state.TotalDropped() > 0 )
            ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ),
                                "Dropped by edits: %d (last: %d gone, %d changed)", state.TotalDropped(),
                                state.LastDropped().Missing, state.LastDropped().Changed );
        if ( !Utils::ImGuiUtilities::SectionHeader( "Selection Edits" ) )
            return;
        const float half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        if ( ImGui::Button( "Grow", ImVec2( half, 0.0f ) ) )
            report( state.Apply( Op::Grow ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Shrink", ImVec2( half, 0.0f ) ) )
            report( state.Apply( Op::Shrink ) );
        if ( ImGui::Button( "Connected", ImVec2( half, 0.0f ) ) )
            report( state.Apply( Op::SelectConnected ) );
        ImGui::SameLine();
        if ( ImGui::Button( "All", ImVec2( half, 0.0f ) ) )
            report( state.Apply( Op::SelectAll ) );
        if ( ImGui::Button( "Invert", ImVec2( half, 0.0f ) ) )
            report( state.Apply( Op::Invert ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Clear", ImVec2( half, 0.0f ) ) )
            report( state.Apply( Op::Clear ) );

        // UE's Selection palette actions: one click = one undo step (mesh + the selection it leaves).
        if ( !Utils::ImGuiUtilities::SectionHeader( "Selection Actions" ) )
            return;
        auto& ms = Core::ModelingState::Get();
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::DragFloat( "##ElementOpDistance", &ms.ElementOpDistance, 0.5f, -10000.0f, 10000.0f,
                          "Distance %.1f cm" );
        using MO           = Core::MeshOperation;
        const auto operate = [this]( MO op ) { Operate( op ); };
        // UE 5.8's Selection palette order (ModelingToolsEditorModeToolkit.cpp:1862-1880): Delete, Extrude,
        // Offset, PushPull, Inset, Outset, Bevel, InsertEdgeLoop, then Retriangulate (our Clean).
        const MO grid[4][2] = { { MO::Delete, MO::Extrude },
                                { MO::Offset, MO::PushPull },
                                { MO::Inset, MO::Outset },
                                { MO::Bevel, MO::InsertEdgeLoop } };
        for ( const auto& row : grid )
        {
            if ( ImGui::Button( Core::ToString( row[0] ), ImVec2( half, 0.0f ) ) )
                operate( row[0] );
            ImGui::SameLine();
            if ( ImGui::Button( Core::ToString( row[1] ), ImVec2( half, 0.0f ) ) )
                operate( row[1] );
        }
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::SliderFloat( "##ElementLoopPosition", &ms.ElementLoopPosition, 0.01f, 0.99f, "Loop at %.2f" );
        ImGui::SetNextItemWidth( half );
        ImGui::DragFloat( "##ElementWeldTolerance", &ms.ElementWeldTolerance, 0.001f, 0.0f, 10.0f,
                          "Weld %.3f cm" );
        ImGui::SameLine();
        if ( ImGui::Button( Core::ToString( MO::Clean ), ImVec2( half, 0.0f ) ) )
            operate( MO::Clean );

        ImGui::TextDisabled( "LMB select, Shift+LMB add, Ctrl+LMB remove" );
        ImGui::TextDisabled( "Del delete, Alt+E extrude, Alt+I inset, Alt+O offset" );
        ImGui::TextDisabled( "Alt+B bevel, Alt+L edge loop, Alt+K knife (two clicks)" );
    }
} // namespace Desert::Editor
