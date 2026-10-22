#include "ModelingPanel.hpp"

#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/ColliderFit.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/MeshBakeNormalTool.hpp>
#include <Editor/Core/Selection/MeshBooleanTool.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/MeshXformOperations.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>
#include <Editor/Widgets/MeshLODRows.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Editor/Panels/ViewportPanel/Tools/SculptTool.hpp>
#include <Common/Core/Units.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#include <Common/Core/Logger.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <initializer_list>
#include <type_traits>

namespace Desert::Editor
{
    namespace
    {
        // What the last Bake wrote, shown under the Bake palette's button.
        Core::BakeNormalMapOutcome& LastBake()
        {
            static Core::BakeNormalMapOutcome s_Last;
            return s_Last;
        }

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
                case S::Torus:
                    return ICON_MDI_RING;
                case S::Arrow:
                    return ICON_MDI_ARROW_UP_BOLD;
                case S::Disc:
                    return ICON_MDI_CIRCLE_OUTLINE;
                case S::Rectangle:
                    return ICON_MDI_RECTANGLE_OUTLINE;
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

        // UE 5.8 ModelingToolsEditorModeToolkit.cpp:1717-1737 (PaletteNames_Standard; LODs last, as there), with
        // the Selection palette first as UE inserts it while mesh selection is on (:1756): only the palettes
        // something of ours lives in, in UE's order.
        enum class Palette
        {
            Selection,
            Shapes,
            Create,
            PolyModel,
            TriModel,
            Deform,
            Transform,
            MeshOps,
            VoxOps,
            Attributes,
            UVs,
            Bake,
            Collision,
            LODs,
        };
        struct PaletteEntry
        {
            const char* Icon;
            const char* Name;
        };
        constexpr std::array<PaletteEntry, 14> kPalettes = { { { ICON_MDI_VECTOR_SELECTION, "Selection" },
                                                               { ICON_MDI_SHAPE_PLUS, "Shapes" },
                                                               { ICON_MDI_PLUS_BOX_OUTLINE, "Create" },
                                                               { ICON_MDI_VECTOR_SQUARE, "PolyModel" },
                                                               { ICON_MDI_VECTOR_TRIANGLE, "TriModel" },
                                                               { ICON_MDI_BLUR, "Deform" },
                                                               { ICON_MDI_ARROW_ALL, "Transform" },
                                                               { ICON_MDI_WRENCH, "Mesh" },
                                                               { ICON_MDI_CUBE_SCAN, "VoxOps" },
                                                               { ICON_MDI_PALETTE_SWATCH, "Attributes" },
                                                               { ICON_MDI_CHECKERBOARD, "UVs" },
                                                               { ICON_MDI_TEXTURE_BOX, "Bake" },
                                                               { ICON_MDI_SHIELD_OUTLINE, "Collision" },
                                                               { ICON_MDI_LAYERS_TRIPLE, "LODs" } } };
        static_assert( kPalettes.size() == static_cast<size_t>( Palette::LODs ) + 1,
                       "one rail entry per palette" );
    } // namespace

    ModelingPanel::ModelingPanel( const std::shared_ptr<Desert::Core::Scene>&   scene,
                                  const std::shared_ptr<Assets::AssetManager>& assets )
         : IPanel( "Modeling", /*showPanel=*/false ), m_Scene( scene ),
           m_Assets( assets ) // contextual: Modeling mode opens it
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
            else if ( ms.ActiveTool == MS::Tool::Sculpt )
                m_Category = static_cast<int>( Palette::Deform );
            else if ( ms.ActiveTool == MS::Tool::ElementSelect )
                m_Category = static_cast<int>( Palette::Selection );
            else if ( ms.ActiveTool == MS::Tool::AutoUV || ms.ActiveTool == MS::Tool::UVProject ||
                      ms.ActiveTool == MS::Tool::UVUnwrap || ms.ActiveTool == MS::Tool::UVLayout ||
                      ms.ActiveTool == MS::Tool::UVTransform )
                m_Category = static_cast<int>( Palette::UVs );
            else if ( ms.ActiveTool == MS::Tool::Smooth || ms.ActiveTool == MS::Tool::Offset ||
                      ms.ActiveTool == MS::Tool::Warp || ms.ActiveTool == MS::Tool::Lattice ||
                      ms.ActiveTool == MS::Tool::VertexSculpt )
                m_Category = static_cast<int>( Palette::Deform );
            else if ( ms.ActiveTool == MS::Tool::Normals || ms.ActiveTool == MS::Tool::Tangents ||
                      ms.ActiveTool == MS::Tool::EditMaterials || ms.ActiveTool == MS::Tool::PaintGroups )
                m_Category = static_cast<int>( Palette::Attributes );
            else if ( ms.ActiveTool == MS::Tool::HoleFill || ms.ActiveTool == MS::Tool::Jacket ||
                      ms.ActiveTool == MS::Tool::Project || ms.ActiveTool == MS::Tool::WeldEdges )
                m_Category = static_cast<int>( Palette::MeshOps );
            else if ( ms.ActiveTool == MS::Tool::VoxWrap || ms.ActiveTool == MS::Tool::VoxBoolean ||
                      ms.ActiveTool == MS::Tool::VoxMorph || ms.ActiveTool == MS::Tool::VoxBlend )
                m_Category = static_cast<int>( Palette::VoxOps );
        }

        if ( ms.ReqLODManager )
        {
            m_Category = static_cast<int>( *ms.ReqLODManager ? Palette::LODs : Palette::Selection );
            ms.ReqLODManager.reset();
        }
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 6.0f ) );

        // --- Left category rail: only the palettes something of ours lives in ---
        // UE's other palettes (Volumes) are absent, not
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

        // The LOD preview belongs to the LODs tab: on any other tab the view returns to Auto (and the renderer's
        // lease, no longer renewed, lapses on its own).
        if ( m_Category != static_cast<int>( Palette::LODs ) )
            ms.LODPreviewLevel = -1;

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
            case Palette::Deform:
                DrawDeformPalette();
                break;
            case Palette::Transform:
                DrawTransformPalette();
                break;
            case Palette::MeshOps:
                DrawMeshOpsPalette();
                break;
            case Palette::VoxOps:
                DrawVoxOpsPalette();
                break;
            case Palette::Attributes:
                DrawAttributesPalette();
                break;
            case Palette::UVs:
                DrawUVsPalette();
                break;
            case Palette::Bake:
                DrawBakePalette();
                break;
            case Palette::Collision:
                DrawCollisionPalette();
                break;
            case Palette::LODs:
                DrawLODsPalette();
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

    void ModelingPanel::RunBoolean( Core::BooleanTool tool )
    {
        if ( !m_Scene )
        {
            LOG_WARN( "{0}: the Modeling panel has no scene", Core::ToString( tool ) );
            return;
        }
        if ( const auto done = Core::ApplyBooleanTool( *m_Scene, tool, Core::BooleanArgsFromModelingState() );
             !done )
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

    // UE's Create palette: of its tools we have Boolean, Merge (UE's Combine Meshes) and Pattern, all operations
    // on the scene selection that run on one click; in UE each is a tool with its own Accept.
    void ModelingPanel::DrawCreatePalette()
    {
        auto&       ms       = Core::ModelingState::Get();
        const float half     = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        const char* axes[]   = { "X", "Y", "Z" };
        using XO             = Core::XformOperation;
        const auto transform = [this]( XO op ) { Transform( op ); };
        if ( Utils::ImGuiUtilities::SectionHeader( "Boolean" ) )
        {
            static constexpr std::array<const char*, 4> kOps = { "Difference A - B", "Difference B - A",
                                                                 "Intersect", "Union" };
            int                                         op   = static_cast<int>( ms.Boolean.Operation );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::Combo( "##BooleanOperation", &op, kOps.data(), static_cast<int>( kOps.size() ) ) )
                ms.Boolean.Operation = static_cast<Core::CsgOperation>( op );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Of the two selected entities, A is the first selected and B the second.\n"
                                   "Both must be closed solids." );
            DrawBooleanOutput();
            if ( ImGui::Button( Core::ToString( Core::BooleanTool::Boolean ), ImVec2( -1.0f, 0.0f ) ) )
                RunBoolean( Core::BooleanTool::Boolean );
        }
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
            // UE's Subdivide tool settings (SubdividePolyTool.h:44-62); the Boundary is greyed out for Bilinear,
            // as UE's EditCondition does.
            Geometry::SubdivideSettings& subdivide = ms.ElementSubdivide;
            ImGui::SetNextItemWidth( half );
            ImGui::SliderInt( "##ElementSubdivideLevels", &subdivide.Level, 1, Geometry::kMaxSubdivisionLevel,
                              "Levels %d" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( half );
            int                                         scheme   = static_cast<int>( subdivide.Scheme );
            static constexpr std::array<const char*, 3> kSchemes = { "Bilinear", "Catmull-Clark", "Loop" };
            if ( ImGui::Combo( "##ElementSubdivideScheme", &scheme, kSchemes.data(),
                               static_cast<int>( kSchemes.size() ) ) )
                subdivide.Scheme = static_cast<Geometry::SubdivisionScheme>( scheme );
            ImGui::SetNextItemWidth( half );
            ImGui::BeginDisabled( subdivide.Scheme == Geometry::SubdivisionScheme::Bilinear );
            int                                         boundary    = static_cast<int>( subdivide.Boundary );
            static constexpr std::array<const char*, 2> kBoundaries = { "Smooth Corners", "Sharp Corners" };
            if ( ImGui::Combo( "##ElementSubdivideBoundary", &boundary, kBoundaries.data(),
                               static_cast<int>( kBoundaries.size() ) ) )
                subdivide.Boundary = static_cast<Geometry::SubdivisionBoundaryScheme>( boundary );
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::SetNextItemWidth( half );
            int                                         normals  = static_cast<int>( subdivide.Normals );
            static constexpr std::array<const char*, 2> kNormals = { "Normals: Interpolated",
                                                                     "Normals: Generated" };
            if ( ImGui::Combo( "##ElementSubdivideNormals", &normals, kNormals.data(),
                               static_cast<int>( kNormals.size() ) ) )
                subdivide.Normals = static_cast<Geometry::SubdivisionOutputNormals>( normals );
            ImGui::Checkbox( "New PolyGroups", &subdivide.NewPolyGroups );
            if ( ImGui::Button( Core::ToString( MO::Subdivide ), ImVec2( -1.0f, 0.0f ) ) )
                operate( MO::Subdivide );
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Simplify" ) )
        {
            const float half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
            using MO         = Core::MeshOperation;
            // UE's Simplify tool settings (SimplifyMeshTool.h): the target is a percentage of the triangles or a
            // vertex count; polygroup borders are held Fixed (UE's FullyConstrained) unless unticked.
            Geometry::SimplifySettings&                 simplify = ms.ElementSimplify;
            int                                         target   = static_cast<int>( simplify.Target );
            static constexpr std::array<const char*, 2> kTargets = { "Percentage", "Vertex Count" };
            ImGui::SetNextItemWidth( half );
            if ( ImGui::Combo( "##ElementSimplifyTarget", &target, kTargets.data(),
                               static_cast<int>( kTargets.size() ) ) )
                simplify.Target = static_cast<Geometry::SimplifyTarget>( target );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( half );
            if ( simplify.Target == Geometry::SimplifyTarget::Percentage )
                ImGui::SliderFloat( "##ElementSimplifyPercentage", &simplify.Percentage, 1.0f, 100.0f, "%.0f %%" );
            else
                ImGui::DragInt( "##ElementSimplifyVertexCount", &simplify.VertexCount, 1.0f, 3, 1000000,
                                "%d vertices" );
            ImGui::Checkbox( "Preserve PolyGroups", &simplify.PreserveGroupBoundaries );
            ImGui::Checkbox( "Preserve Sharp Edges", &simplify.PreserveSharpEdges );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "If enabled, UV and normal seams (hard edges) are kept: they never collapse\n"
                                   "and their vertices never move." );
            if ( ImGui::Button( Core::ToString( MO::Simplify ), ImVec2( -1.0f, 0.0f ) ) )
                Operate( MO::Simplify );
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
    // its own Accept; here each runs on one click - Mirror and Plane Cut on the Select Elements mesh, Trim on
    // the scene selection's two entities.
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

        // Trim (UE's Trim tool): of the scene selection's two entities, one is cut by the other's closed surface
        // (any shape, dents included); the cut stays open.
        auto&                                       boolean  = ms.Boolean;
        static constexpr std::array<const char*, 2> kTrimmed = { "Trim A (first selected)", "Trim B (second)" };
        int                                         trimmed  = static_cast<int>( boolean.Trimmed );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::Combo( "##TrimWhich", &trimmed, kTrimmed.data(), static_cast<int>( kTrimmed.size() ) ) )
            boolean.Trimmed = static_cast<Core::TrimTarget>( trimmed );
        static constexpr std::array<const char*, 2> kSides = { "Remove Inside", "Remove Outside" };
        int                                         side   = static_cast<int>( boolean.Side );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::Combo( "##TrimSide", &side, kSides.data(), static_cast<int>( kSides.size() ) ) )
            boolean.Side = static_cast<Core::TrimSide>( side );
        DrawBooleanOutput();
        if ( ImGui::Button( Core::ToString( Core::BooleanTool::Trim ), ImVec2( -1.0f, 0.0f ) ) )
            RunBoolean( Core::BooleanTool::Trim );
    }

    // UE's Transform palette: Edit Pivot, Bake Transform and Split. In UE each is a tool with its own Accept;
    // here each is an operation on the scene selection's whole entities that runs on one click.
    // UE's Deform palette, its Sculpt tool (UMeshVertexSculptTool): the brush, its size, strength and falloff.
    // The stroke itself is the viewport's (SculptTool); Accept writes the result into the .stmesh. Drawn at the top
    // of the Deform palette, above its preview/Accept tools.
    void ModelingPanel::DrawSculptTools()
    {
        using MS          = Core::ModelingState;
        auto&      ms     = MS::Get();
        const bool active = ms.ActiveTool == MS::Tool::Sculpt;
        // Sculpt (UE UMeshVertexSculptTool) and Dynamic Sculpt (UE UDynamicMeshSculptTool): one viewport tool,
        // the second remeshing the brush's region before every stamp.
        for ( const bool dynamic : { false, true } )
        {
            const bool lit = active && ms.SculptDynamic == dynamic;
            if ( lit )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            if ( ImGui::Button( dynamic ? ICON_MDI_BRUSH "  Dynamic Sculpt" : ICON_MDI_BRUSH "  Sculpt",
                                ImVec2( -1.0f, 30.0f ) ) )
            {
                ms.ActiveTool    = MS::Tool::Sculpt;
                ms.SculptDynamic = dynamic;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( lit )
                ImGui::PopStyleColor();
        }
        // UE's Remesh tool (URemeshMeshTool): the whole selected mesh to a uniform edge length, one undo step.
        if ( Utils::ImGuiUtilities::SectionHeader( "Remesh" ) )
        {
            auto& r = ms.Remesh;
            ImGui::DragFloat( "Target Edge Length", &r.TargetEdgeLength, 0.1f, 0.01f, 1.0e4f, "%.2f cm",
                              ImGuiSliderFlags_AlwaysClamp );
            ImGui::SliderFloat( "Smoothing", &r.SmoothingStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
            ImGui::SliderInt( "Iterations", &r.Iterations, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp );
            ImGui::Checkbox( "Flips##Remesh", &r.Flips );
            ImGui::SameLine();
            ImGui::Checkbox( "Splits##Remesh", &r.Splits );
            ImGui::SameLine();
            ImGui::Checkbox( "Collapses##Remesh", &r.Collapses );
            if ( ImGui::Button( "Remesh", ImVec2( -1.0f, 0.0f ) ) && m_Scene )
                if ( auto refused = Tools::RemeshSelected( *m_Scene, r ) )
                    LOG_WARN( "[Remesh] {}", *refused );
        }
        if ( !active )
            return;
        if ( ms.SculptDynamic && Utils::ImGuiUtilities::SectionHeader( "Dynamic Topology" ) )
        {
            auto& t = ms.DynamicSculptTopology;
            ImGui::DragFloat( "Edge Length", &t.TargetEdgeLength, 0.05f, 0.01f, 1.0e4f, "%.2f cm",
                              ImGuiSliderFlags_AlwaysClamp );
            ImGui::Checkbox( "Refine", &t.Splits );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Split edges under the brush longer than 4/3 of the edge length." );
            ImGui::SameLine();
            ImGui::Checkbox( "Collapse", &t.Collapses );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Collapse edges under the brush shorter than 2/3 of the edge length." );
        }
        auto& s = ms.Sculpt;
        if ( Utils::ImGuiUtilities::SectionHeader( "Brush" ) )
        {
            constexpr std::array kBrushes = { Geometry::SculptBrushType::Standard, Geometry::SculptBrushType::Smooth };
            if ( ImGui::BeginCombo( "Brush Type", Geometry::ToString( s.Brush ) ) )
            {
                for ( const auto brush : kBrushes )
                    if ( ImGui::Selectable( Geometry::ToString( brush ), brush == s.Brush ) )
                        s.Brush = brush;
                ImGui::EndCombo();
            }
            ImGui::DragFloat( "Brush Size", &s.Radius, 0.5f, 0.1f, 1.0e5f, "%.1f cm", ImGuiSliderFlags_AlwaysClamp );
            ImGui::SliderFloat( "Strength", &s.Strength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
            ImGui::SliderFloat( "Falloff", &s.Falloff, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
            ImGui::SliderFloat( "Spacing", &s.Spacing, 0.01f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Distance between stamps along a stroke, as a share of the brush size." );
            ImGui::Checkbox( "Invert", &s.Invert );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Standard pulls the surface in instead of pushing it out (Ctrl held flips it)." );
        }
        ImGui::Separator();
        ImGui::TextDisabled( "Select a static mesh, then LMB-drag over it." );
        ImGui::TextDisabled( "Shift = Smooth, Ctrl = Invert. One stroke = one undo." );
        ImGui::TextDisabled( "Accept writes the result into the .stmesh." );
    }

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

    // UE's Deform palette: Smooth and Offset (UE's USmoothMeshTool / UOffsetMeshTool, Iterative type). Each is a
    // tool on the selected mesh (MeshProcessingTool): the viewport previews it with the settings below, recomputed on
    // every change, and the tool bar's Accept (one undo step) / Cancel (the mesh as it was) end it. The fields are
    // UE's UIterativeSmoothProperties / UIterativeOffsetProperties with their ranges.
    void ModelingPanel::DrawDeformPalette()
    {
        DrawSculptTools();
        using MS          = Core::ModelingState;
        auto&        ms   = MS::Get();
        const ImVec4 sel  = ThemeManager::GetSelectedColor();
        const float  half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        struct DeformToolEntry
        {
            const char* Icon;
            const char* Name;
            MS::Tool    Tool;
        };
        constexpr std::array<DeformToolEntry, 5> kMeshProcessingTools = {
             { { ICON_MDI_BLUR, "Smooth", MS::Tool::Smooth },
               { ICON_MDI_ARROW_EXPAND_ALL, "Offset", MS::Tool::Offset },
               { ICON_MDI_ROTATE_ORBIT, "Warp", MS::Tool::Warp },
               { ICON_MDI_GRID, "Lattice", MS::Tool::Lattice },
               { ICON_MDI_BRUSH, "VSculpt", MS::Tool::VertexSculpt } } };
        for ( size_t i = 0; i < kMeshProcessingTools.size(); ++i )
        {
            const bool on = ms.ActiveTool == kMeshProcessingTools[i].Tool;
            if ( on )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            if ( i % 2 == 1 )
                ImGui::SameLine();
            char deformToolLabel[64];
            std::snprintf( deformToolLabel, sizeof( deformToolLabel ), "%s  %s", kMeshProcessingTools[i].Icon,
                           kMeshProcessingTools[i].Name );
            if ( ImGui::Button( deformToolLabel, ImVec2( half, 26.0f ) ) )
            {
                ms.ActiveTool = kMeshProcessingTools[i].Tool;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( on )
                ImGui::PopStyleColor();
        }
        ImGui::Separator();

        const auto row = []( const char* label )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( label );
            ImGui::SameLine( 150.0f );
            ImGui::SetNextItemWidth( -1.0f );
        };
        constexpr const char* kWeights[] = { "Uniform", "Cotan" };
        switch ( ms.ActiveTool )
        {
            case MS::Tool::Smooth:
            {
                auto&                 s          = ms.DeformSmooth;
                int                   smoothType = static_cast<int>( s.Type );
                int                   weight     = static_cast<int>( s.WeightType );
                constexpr const char* kTypes[]   = { "Iterative", "Implicit", "Diffusion" };
                row( "Smoothing Type" );
                if ( ImGui::Combo( "##SmoothType", &smoothType, kTypes, 3 ) )
                    s.Type = static_cast<Geometry::SmoothType>( smoothType );
                row( "Weight Type" );
                if ( ImGui::Combo( "##SmoothWeight", &weight, kWeights, 2 ) )
                    s.WeightType = static_cast<Geometry::SmoothWeightType>( weight );
                if ( s.Type == Geometry::SmoothType::Implicit )
                {
                    row( "Smoothness" );
                    ImGui::SliderFloat( "##SmoothSmoothness", &s.Smoothness, 0.0f, 1.0f, "%.2f",
                                        ImGuiSliderFlags_AlwaysClamp );
                    row( "Volume Correction" ); // UE UI range 0..1, clamp 0..10
                    ImGui::SliderFloat( "##SmoothVolumeCorrection", &s.VolumeCorrection, 0.0f, 1.0f, "%.2f cm" );
                    s.VolumeCorrection = std::clamp( s.VolumeCorrection, 0.0f, 10.0f );
                }
                else
                {
                    row( "Smoothing Per Step" );
                    ImGui::SliderFloat( "##SmoothPerStep", &s.SmoothingPerStep, 0.0f, 1.0f, "%.2f",
                                        ImGuiSliderFlags_AlwaysClamp );
                    row( "Steps" );
                    ImGui::SliderInt( "##SmoothSteps", &s.Steps, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp );
                }
                ImGui::Checkbox( "Smooth Boundary##Smooth", &s.SmoothBoundary );
                ImGui::Checkbox( "Pin UV Seams##Smooth", &s.PinUVSeams );
                break;
            }
            case MS::Tool::Offset:
            {
                auto&                 s          = ms.DeformOffset;
                int                   offsetType = static_cast<int>( s.Type );
                int                   weight     = static_cast<int>( s.WeightType );
                constexpr const char* kTypes[]   = { "Iterative", "Implicit" };
                row( "Offset Type" );
                if ( ImGui::Combo( "##OffsetType", &offsetType, kTypes, 2 ) )
                    s.Type = static_cast<Geometry::OffsetType>( offsetType );
                row( "Distance" );
                ImGui::DragFloat( "##OffsetDistance", &s.Distance, 0.5f, -10000.0f, 10000.0f, "%.1f cm",
                                  ImGuiSliderFlags_AlwaysClamp );
                if ( s.Type == Geometry::OffsetType::Implicit )
                {
                    row( "Smoothness" ); // UE UI range 0..1, clamp 0..100
                    ImGui::SliderFloat( "##OffsetSmoothness", &s.Smoothness, 0.0f, 1.0f, "%.2f" );
                    s.Smoothness = std::clamp( s.Smoothness, 0.0f, 100.0f );
                }
                else
                {
                    row( "Steps" );
                    ImGui::SliderInt( "##OffsetSteps", &s.Steps, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp );
                    row( "Smoothing Per Step" );
                    ImGui::SliderFloat( "##OffsetPerStep", &s.SmoothingPerStep, 0.0f, 1.0f, "%.2f",
                                        ImGuiSliderFlags_AlwaysClamp );
                }
                row( "Weight Type" );
                if ( ImGui::Combo( "##OffsetWeight", &weight, kWeights, 2 ) )
                    s.WeightType = static_cast<Geometry::SmoothWeightType>( weight );
                if ( s.Type == Geometry::OffsetType::Iterative ) // UE: Implicit always holds the boundary
                    ImGui::Checkbox( "Offset Boundary##Offset", &s.OffsetBoundary );
                ImGui::Checkbox( "Pin UV Seams##Offset", &s.PinUVSeams );
                break;
            }
            case MS::Tool::Warp:
            {
                // UE UMeshSpaceDeformerToolProperties, its ranges and EditConditions.
                auto&                 s      = ms.DeformWarp;
                int                   op     = static_cast<int>( s.Operation );
                constexpr const char* kOps[] = { "Bend", "Twist", "Flare" };
                row( "Operation Type" );
                if ( ImGui::Combo( "##WarpOperation", &op, kOps, 3 ) )
                    s.Operation = static_cast<Geometry::WarpOperation>( op );
                row( "Upper Bound" );
                ImGui::DragFloat( "##WarpUpper", &s.UpperBound, 0.5f, 0.0f, 100000.0f, "%.1f cm",
                                  ImGuiSliderFlags_AlwaysClamp );
                row( "Lower Bound" );
                ImGui::DragFloat( "##WarpLower", &s.LowerBound, 0.5f, -100000.0f, 0.0f, "%.1f cm",
                                  ImGuiSliderFlags_AlwaysClamp );
                if ( s.Operation == Geometry::WarpOperation::Bend )
                {
                    row( "Bend Degrees" );
                    ImGui::SliderFloat( "##WarpBend", &s.BendDegrees, -360.0f, 360.0f, "%.1f" );
                    s.BendDegrees = std::clamp( s.BendDegrees, -3600.0f, 3600.0f );
                }
                else if ( s.Operation == Geometry::WarpOperation::Twist )
                {
                    row( "Twist Degrees" );
                    ImGui::SliderFloat( "##WarpTwist", &s.TwistDegrees, -360.0f, 360.0f, "%.1f" );
                    s.TwistDegrees = std::clamp( s.TwistDegrees, -3600.0f, 3600.0f );
                }
                else
                {
                    int                   profile    = static_cast<int>( s.Profile );
                    constexpr const char* kProfile[] = { "Sin Mode", "Sin Squared", "Triangle" };
                    row( "Flare Profile Type" );
                    if ( ImGui::Combo( "##WarpProfile", &profile, kProfile, 3 ) )
                        s.Profile = static_cast<Geometry::FlareProfile>( profile );
                    row( "Flare Percent Y" );
                    ImGui::SliderFloat( "##WarpFlareY", &s.FlarePercentY, -100.0f, 200.0f, "%.0f %%" );
                    s.FlarePercentY = std::clamp( s.FlarePercentY, -1000.0f, 2000.0f );
                    ImGui::Checkbox( "Lock X and Y Flaring##Warp", &s.LockXAndYFlaring );
                    if ( !s.LockXAndYFlaring )
                    {
                        row( "Flare Percent X" );
                        ImGui::SliderFloat( "##WarpFlareX", &s.FlarePercentX, -100.0f, 200.0f, "%.0f %%" );
                        s.FlarePercentX = std::clamp( s.FlarePercentX, -1000.0f, 2000.0f );
                    }
                }
                if ( s.Operation != Geometry::WarpOperation::Flare )
                    ImGui::Checkbox( "Lock Bottom##Warp", &s.LockBottom );
                ImGui::TextDisabled( "The viewport gizmo is the deformer's frame: Z is the axis." );
                break;
            }
            case MS::Tool::Lattice:
            {
                // UE ULatticeDeformerToolProperties: resolution (a change rebuilds the lattice) and interpolation.
                auto& s = ms.DeformLattice;
                row( "X Axis Resolution" );
                ImGui::SliderInt( "##LatticeX", &s.XResolution, Geometry::kLatticeMinResolution,
                                  Geometry::kLatticeMaxResolution, "%d", ImGuiSliderFlags_AlwaysClamp );
                row( "Y Axis Resolution" );
                ImGui::SliderInt( "##LatticeY", &s.YResolution, Geometry::kLatticeMinResolution,
                                  Geometry::kLatticeMaxResolution, "%d", ImGuiSliderFlags_AlwaysClamp );
                row( "Z Axis Resolution" );
                ImGui::SliderInt( "##LatticeZ", &s.ZResolution, Geometry::kLatticeMinResolution,
                                  Geometry::kLatticeMaxResolution, "%d", ImGuiSliderFlags_AlwaysClamp );
                int                   interpolation = static_cast<int>( s.Interpolation );
                constexpr const char* kInterp[]     = { "Linear", "Cubic" };
                row( "Interpolation Type" );
                if ( ImGui::Combo( "##LatticeInterp", &interpolation, kInterp, 2 ) )
                    s.Interpolation = static_cast<Geometry::LatticeInterpolation>( interpolation );
                row( "Soft Deformation" );
                ImGui::Checkbox( "##LatticeSoft", &s.SoftDeformation );
                ImGui::Text( "%zu of %zu points selected", ms.LatticeSelection.size(), ms.LatticePoints.size() );
                ImGui::TextDisabled( "Click a point (Shift adds, Ctrl toggles); the gizmo moves the selection." );
                break;
            }
            case MS::Tool::VertexSculpt:
            {
                // UE UMeshVertexSculptTool: brush type + USculptBrushProperties (size, strength, falloff).
                auto&                 s        = ms.DeformSculpt;
                int                   brush    = static_cast<int>( s.Brush );
                int                   falloff  = static_cast<int>( s.FalloffType );
                constexpr const char* kFall[]  = { "Smooth", "Linear", "Inverse", "Round" };
                row( "Brush" );
                if ( ImGui::Combo(
                          "##SculptBrush", &brush,
                          []( void*, int i, const char** out )
                          {
                              *out = Geometry::ToString( Geometry::kSculptBrushes[i] );
                              return true;
                          },
                          nullptr, static_cast<int>( std::size( Geometry::kSculptBrushes ) ) ) )
                    s.Brush = Geometry::kSculptBrushes[brush];
                row( "Brush Radius" );
                ImGui::DragFloat( "##SculptRadius", &s.Radius, 0.5f, Geometry::kSculptMinRadius,
                                  Geometry::kSculptMaxRadius, "%.1f cm", ImGuiSliderFlags_AlwaysClamp );
                row( "Strength" );
                ImGui::SliderFloat( "##SculptStrength", &s.Strength, 0.0f, 1.0f, "%.2f",
                                    ImGuiSliderFlags_AlwaysClamp );
                row( "Falloff" );
                ImGui::SliderFloat( "##SculptFalloff", &s.FalloffAmount, 0.0f, 1.0f, "%.2f",
                                    ImGuiSliderFlags_AlwaysClamp );
                row( "Falloff Type" );
                if ( ImGui::Combo( "##SculptFalloffType", &falloff, kFall, 4 ) )
                    s.FalloffType = static_cast<Geometry::SculptFalloff>( falloff );
                ImGui::TextDisabled( "Drag on the mesh; each stroke is one undo step." );
                break;
            }
            default:
                ImGui::TextDisabled( "Pick a Deform tool above; it works on the selected mesh." );
                return;
        }
        ImGui::Separator();
        ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
        ImGui::TextDisabled( "Accept / Cancel: bottom of the viewport" );
    }

    // UE's VoxOps palette. VoxWrap = UE UVoxelSolidifyMeshesTool (UVoxelProperties +
    // UVoxelSolidifyMeshesToolProperties, their defaults and ranges): previewed on the selected mesh by
    // MeshProcessingTool's session, recomputed on every change, committed by the bar's Accept as one undo step.
    void ModelingPanel::DrawVoxOpsPalette()
    {
        using MS          = Core::ModelingState;
        auto&        ms   = MS::Get();
        const ImVec4 sel  = ThemeManager::GetSelectedColor();
        const float  half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        const bool   on   = ms.ActiveTool == MS::Tool::VoxWrap || ms.ActiveTool == MS::Tool::VoxBoolean ||
                        ms.ActiveTool == MS::Tool::VoxMorph || ms.ActiveTool == MS::Tool::VoxBlend;
        const auto   toolButton = [&]( MS::Tool tool, const char* icon, const char* name )
        {
            const bool active = ms.ActiveTool == tool;
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            char label[64];
            std::snprintf( label, sizeof( label ), "%s  %s", icon, name );
            if ( ImGui::Button( label, ImVec2( half, 26.0f ) ) )
            {
                ms.ActiveTool = tool;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( active )
                ImGui::PopStyleColor();
        };
        toolButton( MS::Tool::VoxWrap, ICON_MDI_CUBE_SCAN, "VoxWrap" );
        ImGui::SameLine();
        toolButton( MS::Tool::VoxBoolean, ICON_MDI_CUBE_OUTLINE, "VoxBoolean" );
        toolButton( MS::Tool::VoxMorph, ICON_MDI_CUBE_SEND, "VoxMorph" );
        ImGui::SameLine();
        toolButton( MS::Tool::VoxBlend, ICON_MDI_VECTOR_UNION, "VoxBlend" );
        ImGui::Separator();
        if ( !on )
        {
            ImGui::TextDisabled(
                 "Pick a VoxOps tool above; VoxWrap works on the selected mesh, VoxMorph on the selected mesh, "
                 "VoxBoolean and VoxBlend on the two selected meshes (A first, then B)." );
            return;
        }

        const auto row = []( const char* label )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( label );
            ImGui::SameLine( 150.0f );
            ImGui::SetNextItemWidth( -1.0f );
        };
        if ( ms.ActiveTool == MS::Tool::VoxBoolean )
        {
            // UE UVoxelCSGMeshesToolProperties::Operation + UVoxelProperties + HandleSourcesProperties.
            auto&                                       b    = ms.VoxBoolean;
            static constexpr std::array<const char*, 4> kOps = { "Difference A - B", "Difference B - A",
                                                                 "Intersect", "Union" };
            int                                         op   = static_cast<int>( b.Operation );
            row( "Operation" );
            if ( ImGui::Combo( "##VoxBooleanOp", &op, kOps.data(), static_cast<int>( kOps.size() ) ) )
                b.Operation = static_cast<Geometry::VoxBooleanOperation>( op );
            row( "Voxel Count" ); // UE UI range 8..1024
            ImGui::SliderInt( "##VoxBooleanVoxels", &b.VoxelCount, 8, 1024, "%d", ImGuiSliderFlags_AlwaysClamp );
            row( "Surface Search Steps" );
            ImGui::SliderInt( "##VoxBooleanSearch", &b.SurfaceSearchSteps, 0, 10, "%d",
                              ImGuiSliderFlags_AlwaysClamp );
            static constexpr std::array<const char*, 3> kInputs = { "Delete Inputs", "Hide Inputs",
                                                                    "Keep Inputs" };
            int                                         inputs  = static_cast<int>( ms.VoxBooleanInputs );
            row( "Input Handling" );
            if ( ImGui::Combo( "##VoxBooleanInputs", &inputs, kInputs.data(),
                               static_cast<int>( kInputs.size() ) ) )
                ms.VoxBooleanInputs = static_cast<Core::BooleanInputs>( inputs );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            ImGui::TextDisabled( "Accept creates the result as a new entity (one undo step)." );
            return;
        }
        if ( ms.ActiveTool == MS::Tool::VoxBlend )
        {
            // UE UVoxelBlendMeshesToolProperties (Operation, BlendFalloff, BlendPower) + UVoxelProperties +
            // HandleSourcesProperties.
            auto&                                       bl   = ms.VoxBlend;
            static constexpr std::array<const char*, 2> kOps = { "Union", "Subtract" };
            int                                         op   = static_cast<int>( bl.Operation );
            row( "Operation" );
            if ( ImGui::Combo( "##VoxBlendOp", &op, kOps.data(), static_cast<int>( kOps.size() ) ) )
                bl.Operation = static_cast<Geometry::VoxBlendOperation>( op );
            row( "Blend Falloff" ); // UE ClampMin 0.001
            ImGui::DragFloat( "##VoxBlendFalloff", &bl.BlendFalloff, 0.1f, 0.001f, 1000.0f, "%.1f cm",
                              ImGuiSliderFlags_AlwaysClamp );
            row( "Blend Power" );
            ImGui::DragFloat( "##VoxBlendPower", &bl.BlendPower, 0.01f, 0.1f, 10.0f, "%.2f",
                              ImGuiSliderFlags_AlwaysClamp );
            row( "Voxel Count" );
            ImGui::SliderInt( "##VoxBlendVoxels", &bl.VoxelCount, 8, 1024, "%d", ImGuiSliderFlags_AlwaysClamp );
            row( "Surface Search Steps" );
            ImGui::SliderInt( "##VoxBlendSearch", &bl.SurfaceSearchSteps, 0, 10, "%d",
                              ImGuiSliderFlags_AlwaysClamp );
            static constexpr std::array<const char*, 3> kInputs = { "Delete Inputs", "Hide Inputs",
                                                                    "Keep Inputs" };
            int                                         inputs  = static_cast<int>( ms.VoxBlendInputs );
            row( "Input Handling" );
            if ( ImGui::Combo( "##VoxBlendInputs", &inputs, kInputs.data(), static_cast<int>( kInputs.size() ) ) )
                ms.VoxBlendInputs = static_cast<Core::BooleanInputs>( inputs );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            ImGui::TextDisabled( "Accept creates the result as a new entity (one undo step)." );
            return;
        }
        if ( ms.ActiveTool == MS::Tool::VoxMorph )
        {
            // UE UVoxelMorphologyMeshesToolProperties (Operation, Distance) + UVoxelProperties +
            // HandleSourcesProperties.
            auto&                                       m    = ms.VoxMorph;
            static constexpr std::array<const char*, 4> kOps = { "Dilate", "Contract", "Close", "Open" };
            int                                         op   = static_cast<int>( m.Operation );
            row( "Operation" );
            if ( ImGui::Combo( "##VoxMorphOp", &op, kOps.data(), static_cast<int>( kOps.size() ) ) )
                m.Operation = static_cast<Geometry::VoxMorphOperation>( op );
            row( "Distance" ); // UE ClampMin 0.001
            ImGui::DragFloat( "##VoxMorphDistance", &m.Distance, 0.1f, 0.001f, 1000.0f, "%.1f cm",
                              ImGuiSliderFlags_AlwaysClamp );
            row( "Voxel Count" );
            ImGui::SliderInt( "##VoxMorphVoxels", &m.VoxelCount, 8, 1024, "%d", ImGuiSliderFlags_AlwaysClamp );
            row( "Surface Search Steps" );
            ImGui::SliderInt( "##VoxMorphSearch", &m.SurfaceSearchSteps, 0, 10, "%d",
                              ImGuiSliderFlags_AlwaysClamp );
            static constexpr std::array<const char*, 3> kInputs = { "Delete Inputs", "Hide Inputs",
                                                                    "Keep Inputs" };
            int                                         inputs  = static_cast<int>( ms.VoxMorphInputs );
            row( "Input Handling" );
            if ( ImGui::Combo( "##VoxMorphInputs", &inputs, kInputs.data(), static_cast<int>( kInputs.size() ) ) )
                ms.VoxMorphInputs = static_cast<Core::BooleanInputs>( inputs );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            ImGui::TextDisabled( "Accept creates the result as a new entity (one undo step)." );
            return;
        }
        auto& s = ms.VoxWrap;
        row( "Winding Threshold" ); // UE UI range -10..10
        ImGui::SliderFloat( "##VoxWrapWinding", &s.WindingThreshold, -10.0f, 10.0f, "%.2f",
                            ImGuiSliderFlags_AlwaysClamp );
        row( "Voxel Count" ); // UE UI range 8..1024
        ImGui::SliderInt( "##VoxWrapVoxels", &s.VoxelCount, 8, 1024, "%d", ImGuiSliderFlags_AlwaysClamp );
        row( "Extend Bounds" );
        ImGui::DragFloat( "##VoxWrapExtend", &s.ExtendBounds, 0.1f, 0.0f, 1000.0f, "%.1f cm",
                          ImGuiSliderFlags_AlwaysClamp );
        row( "Surface Search Steps" ); // UE UI range 0..10
        ImGui::SliderInt( "##VoxWrapSearch", &s.SurfaceSearchSteps, 0, 10, "%d", ImGuiSliderFlags_AlwaysClamp );
        ImGui::Checkbox( "Solid At Boundaries##VoxWrap", &s.SolidAtBoundaries );
        ImGui::Checkbox( "Apply Thicken Shells##VoxWrap", &s.ThickenShells );
        if ( s.ThickenShells )
        {
            row( "Thicken Shells" );
            ImGui::DragFloat( "##VoxWrapThickness", &s.ShellThickness, 0.1f, 0.0f, 1000.0f, "%.1f cm",
                              ImGuiSliderFlags_AlwaysClamp );
        }
        ImGui::Separator();
        ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
        ImGui::TextDisabled( "Accept / Cancel: bottom of the viewport" );
    }

    // UE's UVs palette: AutoUV, Project, Unwrap (UE's Recompute UVs) and Layout. Each is a tool on the selected
    // mesh (UVTool): the viewport previews it with the settings below and the tool bar's Accept / Cancel end it.
    // UE's Attributes palette. Normals = UE UEditNormalsTool (UEditNormalsProperties, its defaults and ranges): a
    // tool on the selected mesh previewed by MeshProcessingTool's session, recomputed on every change, committed by the
    // bar's Accept as one undo step.
    // UE's MeshOps palette ("Mesh" on the rail). Jacket = UE URemoveOccludedTrianglesTool: the same session, the
    // preview showing the mesh with its hidden triangles removed. Hole Fill = UE UHoleFillTool (UHoleFillToolProperties,
    // USmoothHoleFillProperties, its Select All / Clear actions): a tool on the selected mesh previewed by
    // MeshProcessingTool's session, the viewport drawing the holes (selected ones highlighted, a click on a
    // hole's edge toggles it), committed by the bar's Accept as one undo step.
    void ModelingPanel::DrawMeshOpsPalette()
    {
        using MS         = Core::ModelingState;
        auto&       ms   = MS::Get();
        const float full = ImGui::GetContentRegionAvail().x;
        const auto  toolButton = [&]( MS::Tool tool, const char* icon, const char* name )
        {
            const bool on = ms.ActiveTool == tool;
            if ( on )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            char label[64];
            std::snprintf( label, sizeof( label ), "%s  %s", icon, name );
            if ( ImGui::Button( label, ImVec2( full, 26.0f ) ) )
            {
                ms.ActiveTool = tool;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( on )
                ImGui::PopStyleColor();
        };
        toolButton( MS::Tool::HoleFill, ICON_MDI_VECTOR_SQUARE_CLOSE, "Hole Fill" );
        toolButton( MS::Tool::Jacket, ICON_MDI_LAYERS_MINUS, "Jacket" );
        toolButton( MS::Tool::Project, ICON_MDI_VECTOR_COMBINE, "Project" );
        toolButton( MS::Tool::WeldEdges, ICON_MDI_VECTOR_LINK, "Weld Edges" );
        // The label above its control: the palette column is too narrow for UE's label-beside-value rows.
        const auto row = []( const char* label )
        {
            ImGui::TextUnformatted( label );
            ImGui::SetNextItemWidth( -1.0f );
        };
        if ( ms.ActiveTool == MS::Tool::WeldEdges )
        {
            // UE UWeldMeshEdgesToolProperties: Tolerance, Only Unique Pairs; the open edges before and after
            // (UE InitialEdges / RemainingEdges) are in the status. The open edges left are drawn in red.
            ImGui::Separator();
            Geometry::WeldEdgesSettings& w = ms.MeshWeldEdges;
            row( "Tolerance (cm)" );
            ImGui::DragFloat( "##WeldTolerance", &w.Tolerance, 0.0001f, 1e-8f, 1000.0f, "%.6f",
                              ImGuiSliderFlags_Logarithmic );
            ImGui::Checkbox( "Only Unique Pairs", &w.OnlyUniquePairs );
            ImGui::Checkbox( "Weld Attributes", &w.WeldAttributes );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            return;
        }
        if ( ms.ActiveTool == MS::Tool::Project )
        {
            // UE UProjectToTargetTool (URemeshMeshToolProperties): the first selected mesh is remeshed and
            // projected onto the second; Target Triangle Count or Target Edge Length, Smoothing Strength, Remesh
            // Iterations, the edge edits, Projection, Discard Attributes.
            ImGui::Separator();
            Geometry::ProjectSettings& p = ms.MeshProject;
            ImGui::Checkbox( "Use Target Edge Length", &p.UseTargetEdgeLength );
            if ( p.UseTargetEdgeLength )
            {
                row( "Target Edge Length (cm)" );
                ImGui::DragFloat( "##ProjectEdge", &p.TargetEdgeLength, 0.1f, 0.01f, 10000.0f, "%.2f" );
            }
            else
            {
                row( "Target Triangle Count" );
                ImGui::DragInt( "##ProjectTris", &p.TargetTriangleCount, 10.0f, 1, 1000000 );
            }
            row( "Smoothing Strength" );
            ImGui::SliderFloat( "##ProjectSmooth", &p.SmoothingStrength, 0.0f, 1.0f, "%.2f" );
            row( "Remesh Iterations" );
            ImGui::SliderInt( "##ProjectIter", &p.RemeshIterations, 0, 100 );
            ImGui::Checkbox( "Flips", &p.Flips );
            ImGui::Checkbox( "Splits", &p.Splits );
            ImGui::Checkbox( "Collapses", &p.Collapses );
            ImGui::Checkbox( "Projection", &p.Projection );
            ImGui::Checkbox( "Discard Attributes", &p.DiscardAttributes );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            return;
        }
        if ( ms.ActiveTool == MS::Tool::Jacket )
        {
            // UE URemoveOccludedTrianglesToolProperties: Occlusion Mode, Triangle Sampling, Winding Iso Value,
            // Add Random Rays (Raycast only), Add Triangle Samples; the status line counts the removed triangles.
            ImGui::Separator();
            Geometry::JacketSettings& j = ms.MeshJacket;
            row( "Occlusion Mode" );
            int mode = static_cast<int>( j.OcclusionMode );
            if ( ImGui::Combo( "##JacketMode", &mode, "Winding Number\0Raycast Occlusion Samples\0" ) )
                j.OcclusionMode = static_cast<Geometry::JacketOcclusionMode>( mode );
            row( "Triangle Sampling" );
            int sampling = static_cast<int>( j.TriangleSampling );
            if ( ImGui::Combo( "##JacketSampling", &sampling, "Vertices\0Vertices and Centroids\0" ) )
                j.TriangleSampling = static_cast<Geometry::JacketTriangleSampling>( sampling );
            if ( j.OcclusionMode == Geometry::JacketOcclusionMode::WindingNumber )
            {
                row( "Winding Iso Value" );
                ImGui::SliderFloat( "##JacketIso", &j.WindingIsoValue, 0.0f, 1.0f, "%.2f" );
            }
            else
            {
                row( "Add Random Rays" );
                ImGui::SliderInt( "##JacketRays", &j.AddRandomRays, 0, 1000 );
            }
            row( "Add Triangle Samples" );
            ImGui::SliderInt( "##JacketSamples", &j.AddTriangleSamples, 0, 1000 );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            return;
        }
        if ( ms.ActiveTool != MS::Tool::HoleFill )
            return;
        ImGui::Separator();
        Geometry::HoleFillSettings& h = ms.MeshHoleFill;
        row( "Fill Type" );
        int type = static_cast<int>( h.Type );
        if ( ImGui::Combo( "##HoleFillType", &type, "Trivial\0Minimal\0Smooth\0" ) )
            h.Type = static_cast<Geometry::HoleFillType>( type );
        if ( h.Type == Geometry::HoleFillType::Smooth )
        {
            row( "Fill Density" );
            ImGui::SliderFloat( "##HoleFillDensity", &h.FillDensity, 0.1f, 4.0f, "%.2f" );
            row( "Interior Smoothness" );
            ImGui::SliderFloat( "##HoleFillSmoothness", &h.InteriorSmoothness, 0.0f, 1.0f, "%.2f" );
        }
        ImGui::Text( "Holes: %d selected of %d", static_cast<int>( ms.HoleFillSelected.size() ),
                     ms.HoleFillLoopCount );
        if ( ImGui::Button( "Select All##HoleFill", ImVec2( full * 0.5f - 3.0f, 0.0f ) ) )
            ms.HoleFillReqSelectAll = true;
        ImGui::SameLine();
        if ( ImGui::Button( "Clear##HoleFill", ImVec2( -1.0f, 0.0f ) ) )
            ms.HoleFillReqClear = true;
        ImGui::Separator();
        ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
    }

    void ModelingPanel::DrawAttributesPalette()
    {
        using MS         = Core::ModelingState;
        auto&       ms   = MS::Get();
        const float full = ImGui::GetContentRegionAvail().x;
        // UE's Attributes palette buttons: Normals (UEditNormalsTool), Tangents (UMeshTangentsTool).
        for ( const auto& [tool, name] :
              { std::pair{ MS::Tool::Normals, "Normals" }, std::pair{ MS::Tool::Tangents, "Tangents" },
                std::pair{ MS::Tool::EditMaterials, "Edit Materials" },
                std::pair{ MS::Tool::PaintGroups, "Paint PolyGroups" } } )
        {
            const bool on = ms.ActiveTool == tool;
            if ( on )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            char label[64];
            std::snprintf( label, sizeof( label ), "%s  %s", ICON_MDI_ARROW_TOP_RIGHT_THICK, name );
            if ( ImGui::Button( label, ImVec2( full, 26.0f ) ) )
            {
                ms.ActiveTool = tool;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( on )
                ImGui::PopStyleColor();
        }
        const auto row = []( const char* label )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( label );
            ImGui::SameLine( 150.0f );
            ImGui::SetNextItemWidth( -1.0f );
        };
        if ( ms.ActiveTool == MS::Tool::PaintGroups )
        {
            // UE UMeshGroupPaintTool: the brush / fill choice, the group painted, New Group, the radius.
            ImGui::Separator();
            ImGui::TextWrapped( "Paint the current group onto the mesh with the left button; Accept keeps it as "
                                "one undo step." );
            row( "Mode" );
            int mode = static_cast<int>( ms.PaintGroupsMode );
            if ( ImGui::Combo( "##PaintGroupsMode", &mode, "Brush\0Fill Connected\0" ) )
                ms.PaintGroupsMode = static_cast<MS::PaintGroupsModeKind>( mode );
            row( "Current Group" );
            if ( ImGui::InputInt( "##PaintGroupsCurrent", &ms.PaintGroupsCurrent ) )
                ms.PaintGroupsCurrent = std::max( ms.PaintGroupsCurrent, 0 );
            if ( ImGui::Button( "New Group##PaintGroups", ImVec2( full, 0.0f ) ) )
                ms.PaintGroupsReqNewGroup = true;
            row( "Brush Radius" );
            ImGui::SliderFloat( "##PaintGroupsRadius", &ms.PaintGroupsRadius, 1.0f, 500.0f, "%.0f cm",
                                ImGuiSliderFlags_Logarithmic );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.PaintGroupsStatus.c_str() );
            return;
        }
        if ( ms.ActiveTool == MS::Tool::EditMaterials )
        {
            // UE UEditMeshMaterialsTool: the material list (active slot), Assign / Add / Remove, the status.
            ImGui::Separator();
            ImGui::TextWrapped( "Select triangles or polygroups in the viewport (Shift adds, Ctrl removes)." );
            for ( int i = 0; i < static_cast<int>( ms.EditMaterialsSlotNames.size() ); ++i )
            {
                char slotLabel[160];
                std::snprintf( slotLabel, sizeof( slotLabel ), "Slot %d: %s##EditMaterialsSlot", i,
                               ms.EditMaterialsSlotNames[i].c_str() );
                if ( ImGui::RadioButton( slotLabel, ms.EditMaterialsActiveSlot == i ) )
                    ms.EditMaterialsActiveSlot = i;
            }
            if ( ImGui::Button( "Assign Active Material##EditMaterials", ImVec2( full, 0.0f ) ) )
                ms.EditMaterialsReqAssign = true;
            if ( ImGui::Button( "Add Slot##EditMaterials" ) )
                ms.EditMaterialsReqAddSlot = true;
            ImGui::SameLine();
            if ( ImGui::Button( "Remove Last Slot##EditMaterials" ) )
                ms.EditMaterialsReqRemoveSlot = true;
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.EditMaterialsStatus.c_str() );
            return;
        }
        if ( ms.ActiveTool == MS::Tool::Tangents )
        {
            // UE UMeshTangentsToolProperties: Calculation Method; Display (lines, preview only).
            ImGui::Separator();
            Geometry::TangentsSettings&  t          = ms.AttribTangents;
            static constexpr const char* kMethods[] = { "Fast MikkT Space", "MikkT Space", "Per Triangle" };
            int                          method     = static_cast<int>( t.CalculationMethod );
            row( "Calculation Method" );
            if ( ImGui::Combo( "##TangentsMethod", &method, kMethods, 3 ) )
                t.CalculationMethod = static_cast<Geometry::TangentsMethod>( method );
            ImGui::Checkbox( "Show Tangents##Tangents", &t.ShowTangents );
            ImGui::Checkbox( "Show Normals##Tangents", &t.ShowNormals );
            row( "Line Length" );
            ImGui::SliderFloat( "##TangentsLength", &t.LineLength, 0.01f, 25.0f, "%.2f cm" );
            row( "Line Thickness" );
            ImGui::SliderFloat( "##TangentsThickness", &t.LineThickness, 0.0f, 25.0f, "%.1f" );
            ImGui::Separator();
            ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
            return;
        }
        if ( ms.ActiveTool != MS::Tool::Normals )
            return;
        ImGui::Separator();
        Geometry::NormalsSettings& n = ms.AttribNormals;
        ImGui::Checkbox( "Recompute Normals##Normals", &n.RecomputeNormals );
        static constexpr const char* kMethods[] = { "Area Weighted", "Angle Weighted", "Area Angle Weighting" };
        int                          method     = static_cast<int>( n.CalculationMethod );
        row( "Normal Calculation" );
        if ( ImGui::Combo( "##NormalsMethod", &method, kMethods, 3 ) )
            n.CalculationMethod = static_cast<Geometry::NormalCalculationMethod>( method );
        static constexpr const char* kSplits[] = { "Use Existing Topology", "Face Normal Threshold",
                                                   "Face Group ID", "Per Triangle", "Per Vertex" };
        int                          split     = static_cast<int>( n.SplitMethod );
        row( "Split Normals" );
        if ( ImGui::Combo( "##NormalsSplit", &split, kSplits, 5 ) )
            n.SplitMethod = static_cast<Geometry::SplitNormalMethod>( split );
        if ( n.SplitMethod == Geometry::SplitNormalMethod::FaceNormalThreshold )
        {
            row( "Sharp Edge Angle" );
            ImGui::SliderFloat( "##NormalsThreshold", &n.SharpEdgeAngleThreshold, 0.0f, 180.0f, "%.1f deg",
                                ImGuiSliderFlags_AlwaysClamp );
            ImGui::Checkbox( "Allow Sharp Vertices##Normals", &n.AllowSharpVertices );
        }
        ImGui::Checkbox( "Fix Inconsistent Normals##Normals", &n.FixInconsistentNormals );
        ImGui::Checkbox( "Invert Normals##Normals", &n.InvertNormals );
        ImGui::Separator();
        ImGui::TextWrapped( "%s", ms.DeformStatus.c_str() );
    }

    void ModelingPanel::DrawUVsPalette()
    {
        using MS          = Core::ModelingState;
        auto&        ms   = MS::Get();
        const ImVec4 sel  = ThemeManager::GetSelectedColor();
        const float  half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        struct UVToolEntry
        {
            const char* Icon;
            const char* Name;
            MS::Tool    Tool;
        };
        constexpr std::array<UVToolEntry, 5> kUVTools = {
             { { ICON_MDI_AUTO_FIX, "AutoUV", MS::Tool::AutoUV },
               { ICON_MDI_PROJECTOR, "Project", MS::Tool::UVProject },
               { ICON_MDI_CONTENT_CUT, "Unwrap", MS::Tool::UVUnwrap },
               { ICON_MDI_VIEW_GRID_OUTLINE, "Layout", MS::Tool::UVLayout },
               { ICON_MDI_CURSOR_MOVE, "Transform", MS::Tool::UVTransform } } };
        for ( size_t i = 0; i < kUVTools.size(); ++i )
        {
            const bool on = ms.ActiveTool == kUVTools[i].Tool;
            if ( on )
                ImGui::PushStyleColor( ImGuiCol_Button, sel );
            if ( i % 2 == 1 )
                ImGui::SameLine();
            char uvToolLabel[64];
            std::snprintf( uvToolLabel, sizeof( uvToolLabel ), "%s  %s", kUVTools[i].Icon, kUVTools[i].Name );
            if ( ImGui::Button( uvToolLabel, ImVec2( half, 26.0f ) ) )
            {
                ms.ActiveTool = kUVTools[i].Tool;
                Core::ViewportMode::Set( Core::EditorMode::Modeling );
            }
            if ( on )
                ImGui::PopStyleColor();
        }
        ImGui::Separator();

        const auto row = []( const char* label )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( label );
            ImGui::SameLine( 150.0f );
            ImGui::SetNextItemWidth( -1.0f );
        };
        switch ( ms.ActiveTool )
        {
            case MS::Tool::AutoUV:
            {
                auto& s = ms.UVAuto;
                row( "Max Deviation" );
                ImGui::DragFloat( "##AutoUVDeviation", &s.MaxNormalDeviationDeg, 0.5f, 1.0f, 180.0f, "%.1f deg",
                                  ImGuiSliderFlags_AlwaysClamp );
                ImGui::Checkbox( "Respect PolyGroups##AutoUV", &s.RespectPolygroups );
                ImGui::Checkbox( "Auto Align##AutoUV", &s.AutoAlign );
                ImGui::Checkbox( "Auto Pack##AutoUV", &s.AutoPack );
                row( "Resolution" );
                ImGui::DragInt( "##AutoUVResolution", &s.PackingResolution, 8.0f, 16, 4096, "%d",
                                ImGuiSliderFlags_AlwaysClamp );
                break;
            }
            case MS::Tool::UVProject:
            {
                auto&                 s        = ms.UVProject;
                constexpr const char* kTypes[] = { "Plane", "Box", "Cylinder" };
                int                   type     = static_cast<int>( s.Type );
                row( "Projection" );
                if ( ImGui::Combo( "##UVProjectType", &type, kTypes, 3 ) )
                    s.Type = static_cast<Geometry::UVProjectionType>( type );
                ImGui::Checkbox( "Fit to Bounds##UVProject", &s.FitToBounds );
                if ( !s.FitToBounds )
                {
                    row( "Dimensions" );
                    ImGui::DragFloat3( "##UVProjectDims", &s.Dimensions.x, 1.0f, 0.001f, 1.0e6f, "%.1f cm",
                                       ImGuiSliderFlags_AlwaysClamp );
                }
                row( "Center Offset" );
                ImGui::DragFloat3( "##UVProjectOffset", &s.CenterOffset.x, 1.0f, -1.0e6f, 1.0e6f, "%.1f cm" );
                row( "Rotation" );
                ImGui::DragFloat3( "##UVProjectRotation", &s.RotationDeg.x, 0.5f, -360.0f, 360.0f, "%.1f deg",
                                   ImGuiSliderFlags_AlwaysClamp );
                if ( s.Type == Geometry::UVProjectionType::Cylinder )
                {
                    row( "Split Angle" );
                    ImGui::DragFloat( "##UVProjectSplit", &s.CylinderSplitAngle, 0.5f, 0.0f, 90.0f, "%.1f deg",
                                      ImGuiSliderFlags_AlwaysClamp );
                }
                ImGui::Separator();
                ImGui::TextDisabled( "UV Transform" );
                row( "UV Rotation" );
                ImGui::DragFloat( "##UVProjectUVRotation", &s.UVRotationDeg, 0.5f, -360.0f, 360.0f, "%.1f deg",
                                  ImGuiSliderFlags_AlwaysClamp );
                row( "Scale U" );
                ImGui::DragFloat( "##UVProjectScaleU", &s.UVScale.x, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Scale V" );
                ImGui::DragFloat( "##UVProjectScaleV", &s.UVScale.y, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Translate U" );
                ImGui::DragFloat( "##UVProjectTranslateU", &s.UVTranslate.x, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Translate V" );
                ImGui::DragFloat( "##UVProjectTranslateV", &s.UVTranslate.y, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                break;
            }
            case MS::Tool::UVUnwrap:
            {
                auto&                 s          = ms.UVUnwrap;
                constexpr const char* kSources[] = { "PolyGroups", "Existing UVs" };
                constexpr const char* kMethods[] = { "ExpMap", "Spectral Conformal" };
                int                   source     = static_cast<int>( s.Source );
                int                   method     = static_cast<int>( s.Method );
                row( "Islands" );
                if ( ImGui::Combo( "##UVUnwrapIslands", &source, kSources, 2 ) )
                    s.Source = static_cast<Geometry::UVIslandSource>( source );
                row( "Method" );
                if ( ImGui::Combo( "##UVUnwrapMethod", &method, kMethods, 2 ) )
                    s.Method = static_cast<Geometry::UVUnwrapMethod>( method );
                ImGui::Checkbox( "Auto Rotate##UVUnwrap", &s.AutoRotate );
                ImGui::Checkbox( "Normalize Areas##UVUnwrap", &s.NormalizeAreas );
                ImGui::Checkbox( "Pack##UVUnwrap", &s.Pack );
                row( "Resolution" );
                ImGui::DragInt( "##UVUnwrapResolution", &s.PackingResolution, 8.0f, 16, 4096, "%d",
                                ImGuiSliderFlags_AlwaysClamp );
                break;
            }
            case MS::Tool::UVLayout:
            {
                auto&                 s        = ms.UVLayout;
                constexpr const char* kTypes[] = { "Repack", "Transform" };
                int                   type     = static_cast<int>( s.Type );
                row( "Layout Type" );
                if ( ImGui::Combo( "##UVLayoutType", &type, kTypes, 2 ) )
                    s.Type = static_cast<Geometry::UVLayoutType>( type );
                if ( s.Type == Geometry::UVLayoutType::Repack )
                {
                    row( "Resolution" );
                    ImGui::DragInt( "##UVLayoutResolution", &s.TextureResolution, 8.0f, 16, 4096, "%d",
                                    ImGuiSliderFlags_AlwaysClamp );
                    ImGui::Checkbox( "Preserve Scale##UVLayout", &s.PreserveScale );
                    ImGui::Checkbox( "Preserve Rotation##UVLayout", &s.PreserveRotation );
                    ImGui::Checkbox( "Allow Flips##UVLayout", &s.AllowFlips );
                }
                row( "Scale" );
                ImGui::DragFloat( "##UVLayoutScale", &s.Scale, 0.01f, 0.001f, 1000.0f, "%.3f",
                                  ImGuiSliderFlags_AlwaysClamp );
                row( "Translate U" );
                ImGui::DragFloat( "##UVLayoutTranslateU", &s.Translation.x, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Translate V" );
                ImGui::DragFloat( "##UVLayoutTranslateV", &s.Translation.y, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                break;
            }
            case MS::Tool::UVTransform:
            {
                // UE's UV Editor Transform tool: scale, then rotate, about the pivot, then translate; on the
                // selected triangles (Select Elements) or, with none, on every island.
                auto&                 s         = ms.UVTransform;
                constexpr const char* kPivots[] = { "Origin", "Island Center", "Selection Center", "Manual" };
                int                   pivot     = static_cast<int>( s.Pivot );
                row( "Scale U" );
                ImGui::DragFloat( "##UVTransformScaleU", &s.Scale.x, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Scale V" );
                ImGui::DragFloat( "##UVTransformScaleV", &s.Scale.y, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Rotation" );
                ImGui::DragFloat( "##UVTransformRotation", &s.RotationDeg, 0.5f, -360.0f, 360.0f, "%.1f deg",
                                  ImGuiSliderFlags_AlwaysClamp );
                row( "Translate U" );
                ImGui::DragFloat( "##UVTransformTranslateU", &s.Translation.x, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Translate V" );
                ImGui::DragFloat( "##UVTransformTranslateV", &s.Translation.y, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                row( "Pivot" );
                if ( ImGui::Combo( "##UVTransformPivot", &pivot, kPivots, 4 ) )
                    s.Pivot = static_cast<Geometry::UVTransformPivot>( pivot );
                if ( s.Pivot == Geometry::UVTransformPivot::Manual )
                {
                    row( "Pivot U" );
                    ImGui::DragFloat( "##UVTransformPivotU", &s.ManualPivot.x, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                    row( "Pivot V" );
                    ImGui::DragFloat( "##UVTransformPivotV", &s.ManualPivot.y, 0.01f, -1000.0f, 1000.0f, "%.3f" );
                }
                break;
            }
            default:
                ImGui::TextDisabled( "Pick a UV tool above; it works on the selected mesh." );
                return;
        }
        ImGui::Separator();
        ImGui::Checkbox( "Checker Preview##UV", &ms.UVShowChecker );
        if ( ms.UVShowChecker )
        {
            row( "Checker Density" );
            ImGui::DragFloat( "##UVCheckerDensity", &ms.UVCheckerDensity, 0.1f, 0.1f, 1000.0f, "%.1f",
                              ImGuiSliderFlags_AlwaysClamp );
        }
        ImGui::Separator();
        ImGui::TextWrapped( "%s", ms.UVStatus.c_str() );
        ImGui::TextDisabled( "Accept / Cancel: bottom of the viewport" );
    }

    void ModelingPanel::DrawBakePalette()
    {
        // UE 5.8 Modeling Mode -> Bake -> BakeTx (UBakeMeshAttributeMapsTool), Map Type = Tangent Normal: select
        // the target (low poly, its UVs lay out the map) FIRST, then the detail (high poly).
        auto&      s   = Core::BakeNormalMapSettings().Settings;
        const auto row = []( const char* label )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( label );
            ImGui::SameLine( 150.0f );
            ImGui::SetNextItemWidth( -1.0f );
        };
        // UE EBakeTextureResolution: square powers of two, 16 to 8192; ours from 256 to 4096.
        constexpr std::array<int, 5>         kResolutions     = { 256, 512, 1024, 2048, 4096 };
        constexpr std::array<const char*, 5> kResolutionNames = { "256", "512", "1024", "2048", "4096" };
        int                                  resolution       = 0;
        for ( size_t i = 0; i < kResolutions.size(); ++i )
            if ( kResolutions[i] == s.Resolution )
                resolution = static_cast<int>( i );
        row( "Resolution" );
        if ( ImGui::Combo( "##BakeResolution", &resolution, kResolutionNames.data(),
                           static_cast<int>( kResolutionNames.size() ) ) )
            s.Resolution = kResolutions.at( static_cast<size_t>( resolution ) );
        row( "Projection Distance" );
        ImGui::DragScalar( "##BakeProjectionDistance", ImGuiDataType_Double, &s.ProjectionDistance, 0.1f, nullptr,
                           nullptr, "%.2f cm" );
        s.ProjectionDistance = std::max( s.ProjectionDistance, 0.001 );
        row( "Gutter" );
        ImGui::DragInt( "##BakeGutter", &s.GutterTexels, 0.25f, 0, 64, "%d texels", ImGuiSliderFlags_AlwaysClamp );
        // UE's Bake Mesh Attribute Maps: the normal map always, Ambient Occlusion and Curvature on request.
        row( "Ambient Occlusion" );
        ImGui::Checkbox( "##BakeOcclusion", &s.Occlusion );
        if ( s.Occlusion )
        {
            row( "Occlusion Rays" );
            ImGui::DragInt( "##BakeOcclusionRays", &s.OcclusionRays, 0.25f, 1, 1024, "%d",
                            ImGuiSliderFlags_AlwaysClamp );
        }
        row( "Curvature" );
        ImGui::Checkbox( "##BakeCurvature", &s.Curvature );
        ImGui::TextDisabled( "Select the target (low) first, then the detail (high)." );
        if ( ImGui::Button( "Bake Normal Map", ImVec2( -1.0f, 0.0f ) ) )
        {
            // BakeNormalMap logs its own refusal (the palette and DesertCtl reach it too).
            (void)BakeNormalMap( m_Scene, m_Assets );
        }
        // The maps the last bake wrote (whichever door ran it).
        const Core::BakeNormalMapOutcome& last = LastBake();
        if ( !last.TextureAsset.empty() )
        {
            ImGui::Separator();
            ImGui::TextUnformatted( "Baked Maps" );
            const auto map = [&]( const char* label, const std::filesystem::path& asset )
            {
                if ( asset.empty() )
                    return;
                row( label );
                ImGui::TextUnformatted( asset.filename().generic_string().c_str() );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", asset.generic_string().c_str() );
            };
            map( "Normal", last.TextureAsset );
            map( "Ambient Occlusion", last.OcclusionAsset );
            map( "Curvature", last.CurvatureAsset );
            map( "Material", last.Material );
            ImGui::TextDisabled( "%zu of %zu texels hit the detail", last.HitTexels, last.CoveredTexels );
        }
    }

    Common::ResultStr<bool> ModelingPanel::BakeNormalMap( const std::shared_ptr<Desert::Core::Scene>&   scene,
                                                          const std::shared_ptr<Assets::AssetManager>& assets )
    {
        if ( !scene || !assets )
            return Common::MakeFormattedError<bool>( "no {} to bake in", !scene ? "scene" : "asset manager" );
        const auto baked = Core::ApplyBakeNormalMap( *scene, *assets, Core::BakeNormalMapSettings() );
        if ( !baked.IsSuccess() )
        {
            // The refusal is the user's answer, whichever door ran the bake (button, palette, DesertCtl): it goes
            // to the log, not only to the caller.
            LOG_WARN( "Bake Normal Map refused: {}", baked.GetError() );
            return Common::MakeFormattedError<bool>( "{}", baked.GetError() );
        }
        const auto& out = baked.GetValue();
        LastBake()      = out;
        LOG_INFO( "Bake Normal Map: {} -> {} ({} of {} texels hit)", out.TextureAsset.generic_string(),
                  out.Material.generic_string(), out.HitTexels, out.CoveredTexels );
        return Common::MakeSuccess( true );
    }

    void ModelingPanel::DrawCollisionPalette()
    {
        std::array<const char*, kCollisionShapes.size()> names{};
        for ( size_t i = 0; i < kCollisionShapes.size(); ++i )
            names[i] = kCollisionShapes[i].Name;
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::Combo( "##CollisionShape", &m_CollisionShape, names.data(), static_cast<int>( names.size() ) );
        if ( ImGui::Button( "Mesh To Collision", ImVec2( -1.0f, 0.0f ) ) )
        {
            const auto done =
                 MeshToCollision( m_Scene, kCollisionShapes.at( static_cast<size_t>( m_CollisionShape ) ) );
            if ( !done.IsSuccess() )
                LOG_WARN( "Mesh To Collision: {}", done.GetError() );
        }

        // UE's Mesh To Volume (the Volumes tools): the mesh becomes a volume of the chosen type.
        ImGui::Separator();
        std::array<const char*, kVolumeTypes.size()> volumeNames{};
        for ( size_t i = 0; i < kVolumeTypes.size(); ++i )
            volumeNames[i] = kVolumeTypes[i].Name;
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::Combo( "##VolumeType", &m_VolumeType, volumeNames.data(), static_cast<int>( volumeNames.size() ) );
        if ( ImGui::Button( "Mesh To Volume", ImVec2( -1.0f, 0.0f ) ) )
        {
            const auto done = MeshToVolume( m_Scene, kVolumeTypes.at( static_cast<size_t>( m_VolumeType ) ) );
            if ( !done.IsSuccess() )
                LOG_WARN( "Mesh To Volume: {}", done.GetError() );
        }
    }

    Common::ResultStr<bool> ModelingPanel::MeshToVolume( const std::shared_ptr<Desert::Core::Scene>& scene,
                                                         const VolumeTypeEntry&                      type )
    {
        const auto& ids = Core::SelectionManager::GetSelection();
        if ( !scene || ids.size() != 1 )
            return Common::MakeFormattedError<bool>( "select one entity with a static mesh ({} selected)",
                                                     ids.size() );
        const Common::UUID id  = ids.front();
        const auto         ref = scene->FindEntityByID( id );
        if ( !ref )
            return Common::MakeFormattedError<bool>( "entity {} is not in the scene",
                                                     static_cast<uint64_t>( id ) );
        const ECS::Entity entity = ref->get();
        if ( entity.HasComponent<ECS::VolumeComponent>() )
            return Common::MakeFormattedError<bool>( "entity {} is already a volume", static_cast<uint64_t>( id ) );

        // The mesh's points at the entity's WORLD scale, body-local: the volume takes the source's world pose
        // (it is created at the scene root, the source may be a child) and carries the scale in its points, as
        // UE's tool bakes the actor's scale into the new brush.
        const auto world    = ECS::Rules::DecomposeTransform( entity.GetWorldTransform() );
        auto       gathered = ECS::PhysicsECSSystem::GatherColliderMesh( *entity.GetRegistry(), entity.GetHandle(),
                                                                         world.Scale );
        if ( !gathered.IsSuccess() )
            return Common::MakeFormattedError<bool>( "entity {}: {}", static_cast<uint64_t>( id ),
                                                     gathered.GetError() );
        if ( !gathered.GetValue().has_value() )
            return Common::MakeFormattedError<bool>( "entity {}: its mesh is still loading",
                                                     static_cast<uint64_t>( id ) );
        // UE's AVolume brush is the CONVEX HULL of the mesh (UMeshToVolumeTool, ConvexHull), not its triangles:
        // the outline drawn is the region Jolt simulates.
        auto hull = Physics::BuildConvexHull( gathered.GetValue()->Points );
        if ( !hull.IsSuccess() )
            return Common::MakeFormattedError<bool>( "entity {}: {}", static_cast<uint64_t>( id ), hull.GetError() );
        Physics::ConvexHullGeometry shape = std::move( hull.GetValue() );

        const std::string name = std::string( type.Name ) + "Volume";
        const auto replaced    = Commands::ReplaceWithNewEntity(
             id, name,
             [&]( ECS::Entity& volume )
             {
                 auto& transform       = volume.GetComponent<ECS::TransformComponent>();
                 transform.Translation = world.Translation;
                 transform.Rotation    = world.Rotation;
                 transform.Scale       = glm::vec3( 1.0f );

                 ECS::VolumeComponent component;
                 component.Data.Type        = type.Type;
                 ECS::SetVolumeCorners( component.Data, shape.Points );
                 component.Data.HullIndices = std::move( shape.Indices );
                 volume.AddComponent<ECS::VolumeComponent>( std::move( component ) );

                 ECS::RigidBodyComponent body;
                 body.Data.Type             = Physics::BodyType::Static;
                 body.Data.CollisionProfile = ECS::VolumeCollisionProfile( type.Type );
                 volume.AddComponent<ECS::RigidBodyComponent>( std::move( body ) );

                 ECS::ColliderComponent collider;
                 collider.Data.Shape = Physics::ShapeType::ConvexHull;
                 volume.AddComponent<ECS::ColliderComponent>( std::move( collider ) );
             } );
        if ( !replaced.IsSuccess() )
            return Common::MakeError<bool>( replaced.GetError() );
        LOG_INFO( "[Modeling] Mesh To Volume on entity {}: {} volume {}", static_cast<uint64_t>( id ), type.Name,
                  static_cast<uint64_t>( replaced.GetValue() ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool> ModelingPanel::MeshToCollision( const std::shared_ptr<Desert::Core::Scene>& scene,
                                                            const CollisionShapeEntry&                  shape )
    {
        const auto& ids = Core::SelectionManager::GetSelection();
        if ( !scene || ids.size() != 1 )
            return Common::MakeFormattedError<bool>( "select one entity with a static mesh ({} selected)",
                                                     ids.size() );
        const Common::UUID id  = ids.front();
        const auto         ref = scene->FindEntityByID( id );
        if ( !ref )
            return Common::MakeFormattedError<bool>( "entity {} is not in the scene",
                                                     static_cast<uint64_t>( id ) );
        const ECS::Entity entity = ref->get();

        const auto fit = Core::FitEntityCollider( entity, shape.Shape );
        if ( !fit.IsSuccess() )
            return Common::MakeFormattedError<bool>( "entity {}: {}", static_cast<uint64_t>( id ),
                                                     fit.GetError() );
        const ECS::ColliderData collider = fit.GetValue();
        Commands::MutateEntityUndoable( id,
                                        [&]
                                        {
                                            auto live = scene->FindEntityByID( id );
                                            if ( live )
                                                live->get().AddComponent<ECS::ColliderComponent>(
                                                     ECS::ColliderComponent{ collider } );
                                        } );
        LOG_INFO( "[Modeling] Mesh To Collision on entity {}: {}", static_cast<uint64_t>( id ), shape.Name );
        return Common::MakeSuccess( true );
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
                ImGui::SetTooltip( "Put the grid frame on the SELECTED object's origin and orientation, so every\n"
                                   "block size stays flush with its corners and faces instead of tiling from\n"
                                   "(0,0,0) along the world axes. Ctrl+MMB puts the pivot on the nearest corner\n"
                                   "of the face under the cursor." );
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Options" ) )
        {
            // Moving the frame commits the current piece (cells are lattice indices) and re-tiles from
            // the new origin and axes — already-built geometry keeps the frame it was made in and never moves.
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat3( "Grid Frame Origin", &ms.GridOrigin.x, 1.0f, 0.0f, 0.0f, "%.0f" );
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat3( "Grid Frame Orientation", &ms.GridRotation.x, 0.5f, -180.0f, 180.0f, "%.1f deg" );
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

            // UE's Crosswise Diagonal: which way a non-planar sloped quad is split.
            ImGui::Checkbox( "Crosswise Diagonal (X)", &ms.CornerCrosswise );

            ImGui::TextDisabled( "Select a rectangle on any face, press Z," );
            ImGui::TextDisabled( "sweep over posts to flip them, then E / Q." );
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Block Selection" ) )
        {
            ImGui::Checkbox( "Hit Unrelated Geometry", &ms.HitUnrelated );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Target other objects in the scene too, so you can start a grid on\n"
                                   "top of an existing mesh (bounding-box level)." );
        }
        // Quick Materials (UE's Material property on the Cube Grid Tool): what Push/Pull gives the faces it
        // creates and Shift+B paints onto the selection. Picked from the project's materials.
        if ( Utils::ImGuiUtilities::SectionHeader( "Material" ) )
        {
            std::string current = "Engine default";
            const auto  rows    = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Material );
            for ( const auto& row : rows )
                if ( row.Handle == ms.QuickMaterial )
                    current = ::Desert::Editor::PickerDisplayName( row );
            ImGui::SetNextItemWidth( 200.0f );
            if ( ImGui::BeginCombo( "Quick Material", current.c_str() ) )
            {
                if ( ImGui::Selectable( "Engine default", ms.QuickMaterial == Common::AssetHandle{} ) )
                    ms.QuickMaterial = Common::AssetHandle{};
                for ( const auto& row : rows )
                {
                    const std::string name = ::Desert::Editor::PickerDisplayName( row );
                    if ( ImGui::Selectable( ( name + "##" + row.Key ).c_str(), row.Handle == ms.QuickMaterial ) )
                        ms.QuickMaterial = row.Handle;
                }
                ImGui::EndCombo();
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Push / Pull give every face they create this material;\n"
                                   "Shift+B paints it onto the selected faces without changing the shape." );
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Collision" ) )
        {
            ImGui::Checkbox( "Generate Collision", &ms.GenerateCollision );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Accept also gives the piece a Mesh collider - its own triangles - and a\n"
                                   "static body, so you can walk into it right away: through a doorway,\n"
                                   "into a room. Static only: a triangle mesh has no mass." );
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
                 { "Push / Pull", "E / Q, or Ctrl + LMB drag" },
                 { "Slide selection", "Shift + E / Q" },
                 { "Paint Quick Material", "Shift + B" },
                 { "Corner Mode", "Z (sweep posts, E / Q, X diagonal)" },
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
        const auto count =
             []( const char* label, int* value, int uiLowest, int uiHighest, int clampLowest, int clampHighest )
        {
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::SliderInt( label, value, uiLowest, uiHighest ) )
                *value = std::clamp( *value, clampLowest, clampHighest );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Drag %d..%d; Ctrl+click to type %d..%d.", uiLowest, uiHighest, clampLowest,
                                   clampHighest );
        };

        ImGui::TextUnformatted( MS::ShapeName( s.Kind ) );
        ImGui::Spacing();
        if ( Utils::ImGuiUtilities::SectionHeader( "Shape" ) )
        {
            // Only the chosen shape's own property set, with UE's names, UI ranges and clamps
            // (AddPrimitiveTool.h): every field shown here moves the shape.
            using Shape      = MS::Shape;
            const auto combo = []( const char* label, auto& current,
                                   std::initializer_list<std::decay_t<decltype( current )>> values )
            {
                ImGui::SetNextItemWidth( 110.0f );
                if ( ImGui::BeginCombo( label, Geometry::ToString( current ) ) )
                {
                    for ( const auto type : values )
                        if ( ImGui::Selectable( Geometry::ToString( type ), type == current ) )
                            current = type;
                    ImGui::EndCombo();
                }
            };
            switch ( s.Kind )
            {
                case Shape::Box:
                    cm( "Width", &s.Box.Width, "Width of the box (X)." );
                    cm( "Depth", &s.Box.Depth, "Depth of the box (Z)." );
                    cm( "Height", &s.Box.Height, "Height of the box (Y)." );
                    count( "Width Subdivisions", &s.Box.WidthSubdivisions, 1, 100, 1, 500 );
                    count( "Depth Subdivisions", &s.Box.DepthSubdivisions, 1, 100, 1, 500 );
                    count( "Height Subdivisions", &s.Box.HeightSubdivisions, 1, 100, 1, 500 );
                    break;
                case Shape::Sphere:
                    cm( "Radius", &s.Sphere.Radius, "Radius of the sphere." );
                    combo( "Subdivision Type", s.Sphere.SubdivisionType,
                           { Geometry::SphereType::LatLong, Geometry::SphereType::Box } );
                    if ( s.Sphere.SubdivisionType == Geometry::SphereType::Box )
                        count( "Subdivisions", &s.Sphere.Subdivisions, 1, 100, 1, 500 );
                    else
                    {
                        count( "Horizontal Slices", &s.Sphere.HorizontalSlices, 3, 100, 4, 500 );
                        count( "Vertical Slices", &s.Sphere.VerticalSlices, 3, 100, 4, 500 );
                    }
                    break;
                case Shape::Cylinder:
                    cm( "Radius", &s.Cylinder.Radius, "Radius of the cylinder." );
                    cm( "Height", &s.Cylinder.Height, "Height of the cylinder." );
                    count( "Radial Slices", &s.Cylinder.RadialSlices, 3, 128, 3, 500 );
                    count( "Height Subdivisions", &s.Cylinder.HeightSubdivisions, 1, 100, 1, 500 );
                    break;
                case Shape::Cone:
                    cm( "Radius", &s.Cone.Radius, "Radius of the cone's base." );
                    cm( "Height", &s.Cone.Height, "Height of the cone." );
                    count( "Radial Slices", &s.Cone.RadialSlices, 3, 128, 3, 500 );
                    count( "Height Subdivisions", &s.Cone.HeightSubdivisions, 1, 100, 1, 500 );
                    break;
                case Shape::Capsule:
                    cm( "Radius", &s.Capsule.Radius, "Radius of the capsule." );
                    cm( "Cylinder Length", &s.Capsule.CylinderLength,
                        "Length of the middle; the capsule is this plus two radii." );
                    count( "Hemisphere Slices", &s.Capsule.HemisphereSlices, 2, 100, 2, 500 );
                    count( "Cylinder Slices", &s.Capsule.CylinderSlices, 3, 100, 3, 500 );
                    count( "Cylinder Subdivisions", &s.Capsule.CylinderSubdivisions, 0, 100, 0, 500 );
                    break;
                case Shape::Pyramid:
                    cm( "Width", &s.Pyramid.Width, "X extent (not a UE shape: this engine's own)." );
                    cm( "Depth", &s.Pyramid.Depth, "Z extent." );
                    cm( "Height", &s.Pyramid.Height, "Y extent." );
                    break;
                case Shape::Stairs:
                {
                    auto&      st = s.Stairs;
                    const bool curved =
                         st.Type == Geometry::StairsType::Curved || st.Type == Geometry::StairsType::Spiral;
                    combo( "Stairs Type", st.Type,
                           { Geometry::StairsType::Linear, Geometry::StairsType::Floating,
                             Geometry::StairsType::Curved, Geometry::StairsType::Spiral } );
                    count( "Num Steps", &st.Steps, 2, 100, 2, 1000000 );
                    cm( "Step Width", &st.StepWidth, curved ? "Outer radius minus inner radius." : "X extent." );
                    cm( "Step Height", &st.StepHeight, "Riser height." );
                    if ( !curved )
                        cm( "Step Depth", &st.StepDepth, "Tread depth, along +Z." );
                    if ( curved )
                    {
                        cm( "Inner Radius", &st.InnerRadius, "From the axis to the inner wall." );
                        // UE's ranges: a curve turns up to 360 degrees, a spiral as many turns as it likes.
                        const bool  spiral = st.Type == Geometry::StairsType::Spiral;
                        const float span   = spiral ? 720.0f : 360.0f;
                        ImGui::SetNextItemWidth( 110.0f );
                        if ( ImGui::DragFloat( "Curve Angle", &st.CurveAngle, 1.0f, -span, span, "%.1f deg" ) )
                            st.CurveAngle = std::clamp( st.CurveAngle, spiral ? -360000.0f : -360.0f,
                                                        spiral ? 360000.0f : 360.0f );
                        if ( ImGui::IsItemHovered() )
                            ImGui::SetTooltip( "Degrees the whole flight turns; negative turns the other way." );
                    }
                    break;
                }
                case Shape::Torus:
                    cm( "Major Radius", &s.Torus.MajorRadius, "From the axis to the middle of the tube." );
                    cm( "Minor Radius", &s.Torus.MinorRadius, "Radius of the tube." );
                    count( "Major Slices", &s.Torus.MajorSlices, 3, 128, 3, 500 );
                    count( "Minor Slices", &s.Torus.MinorSlices, 3, 128, 3, 500 );
                    break;
                case Shape::Arrow:
                    cm( "Shaft Radius", &s.Arrow.ShaftRadius, "Radius of the shaft." );
                    cm( "Shaft Height", &s.Arrow.ShaftHeight, "Height of the shaft." );
                    cm( "Head Radius", &s.Arrow.HeadRadius, "Radius of the head's base." );
                    cm( "Head Height", &s.Arrow.HeadHeight, "Height of the head." );
                    count( "Radial Slices", &s.Arrow.RadialSlices, 3, 100, 3, 500 );
                    count( "Height Subdivisions", &s.Arrow.HeightSubdivisions, 1, 100, 1, 500 );
                    break;
                case Shape::Disc:
                    combo( "Disc Type", s.Disc.Type,
                           { Geometry::DiscType::Disc, Geometry::DiscType::PuncturedDisc } );
                    cm( "Radius", &s.Disc.Radius, "Radius of the disc." );
                    count( "Radial Slices", &s.Disc.RadialSlices, 3, 128, 3, 500 );
                    count( "Radial Subdivisions", &s.Disc.RadialSubdivisions, 1, 100, 1, 500 );
                    if ( s.Disc.Type == Geometry::DiscType::PuncturedDisc )
                        cm( "Hole Radius", &s.Disc.HoleRadius, "Radius of the hole; kept inside the rim." );
                    break;
                case Shape::Rectangle:
                    combo( "Rectangle Type", s.Rectangle.Type,
                           { Geometry::RectangleType::Rectangle, Geometry::RectangleType::RoundedRectangle } );
                    cm( "Width", &s.Rectangle.Width, "Width of the rectangle (X)." );
                    cm( "Depth", &s.Rectangle.Depth, "Depth of the rectangle (Z)." );
                    count( "Width Subdivisions", &s.Rectangle.WidthSubdivisions, 1, 100, 1, 500 );
                    count( "Depth Subdivisions", &s.Rectangle.DepthSubdivisions, 1, 100, 1, 500 );
                    if ( s.Rectangle.Type == Geometry::RectangleType::RoundedRectangle )
                    {
                        ImGui::Checkbox( "Maintain Dimension", &s.Rectangle.MaintainDimension );
                        if ( ImGui::IsItemHovered() )
                            ImGui::SetTooltip(
                                 "Width and Depth stay the outer size; the corners come out of them." );
                        cm( "Corner Radius", &s.Rectangle.CornerRadius, "Radius of the rounded corners." );
                        count( "Corner Slices", &s.Rectangle.CornerSlices, 3, 128, 3, 500 );
                    }
                    break;
            }
        }
        if ( Utils::ImGuiUtilities::SectionHeader( "Polygroups and Pivot" ) )
        {
            constexpr Geometry::ShapePolygroupMode modes[] = { Geometry::ShapePolygroupMode::PerShape,
                                                               Geometry::ShapePolygroupMode::PerFace,
                                                               Geometry::ShapePolygroupMode::PerQuad };
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
            ImGui::Checkbox( "Align to Normal", &s.AlignToNormal );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "On: the shape's up axis turns to the normal of the surface it lands on.\n"
                                   "Off: it stands upright wherever it lands." );
        }
        DrawOutputType();
        ImGui::Spacing();
        ImGui::TextDisabled( "LMB in the viewport places the shape." );
        ImGui::TextDisabled( "Ctrl+Z removes it again." );
    }

    // UE's "Output Type" section (UCreateMeshObjectTypeProperties), shared by the creating tools.
    void ModelingPanel::DrawBooleanOutput()
    {
        auto&                                       boolean  = Core::ModelingState::Get().Boolean;
        static constexpr std::array<const char*, 2> kWriteTo = { "Write To: New Object", "Write To: Input" };
        int                                         writeTo  = static_cast<int>( boolean.WriteTo );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::Combo( "##BooleanWriteTo", &writeTo, kWriteTo.data(), static_cast<int>( kWriteTo.size() ) ) )
            boolean.WriteTo = static_cast<Core::BooleanWriteTo>( writeTo );
        static constexpr std::array<const char*, 3> kInputs = { "Delete Inputs", "Hide Inputs", "Keep Inputs" };
        int                                         inputs  = static_cast<int>( boolean.Inputs );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::Combo( "##BooleanInputs", &inputs, kInputs.data(), static_cast<int>( kInputs.size() ) ) )
            boolean.Inputs = static_cast<Core::BooleanInputs>( inputs );
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
        // Offset, PushPull, Inset, Outset, Bevel, InsertEdgeLoop, then Retriangulate (our Clean). Fill Hole and
        // Weld Edges sit under UE's Mesh Repair palette; they share this grid instead of a category of two.
        const MO grid[5][2] = { { MO::Delete, MO::Extrude },
                                { MO::Offset, MO::PushPull },
                                { MO::Inset, MO::Outset },
                                { MO::Bevel, MO::InsertEdgeLoop },
                                { MO::FillHole, MO::WeldEdges } };
        for ( const auto& row : grid )
        {
            if ( ImGui::Button( Core::ToString( row[0] ), ImVec2( half, 0.0f ) ) )
                operate( row[0] );
            ImGui::SameLine();
            if ( ImGui::Button( Core::ToString( row[1] ), ImVec2( half, 0.0f ) ) )
                operate( row[1] );
        }
        // UE PolyEdit's Faces > Disconnect.
        if ( ImGui::Button( Core::ToString( MO::Disconnect ), ImVec2( -1.0f, 0.0f ) ) )
            operate( MO::Disconnect );
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
    namespace
    {
        // The LOD Manager's target: the one selected entity's static mesh, with its chain.
        struct LODTarget
        {
            Common::UUID        Id;
            uint32_t            DrawEntity = 0; // the id its draws carry (DrawStaticMeshCommand::Entity)
            ::Desert::Mesh*     Mesh       = nullptr;
            Assets::AssetHandle MeshHandle;
            size_t              Levels = 0;
        };

        Common::ResultStr<LODTarget> SelectedLODTarget( const std::shared_ptr<Desert::Core::Scene>& scene )
        {
            const auto& ids = Core::SelectionManager::GetSelection();
            if ( !scene || ids.size() != 1 )
                return Common::MakeFormattedError<LODTarget>( "select one entity with a static mesh ({} selected)",
                                                              ids.size() );
            const auto ref = scene->FindEntityByID( ids.front() );
            if ( !ref )
                return Common::MakeFormattedError<LODTarget>( "entity {} is not in the scene",
                                                              static_cast<uint64_t>( ids.front() ) );
            ECS::Entity entity = ref->get(); // a handle: the copy names the same entity
            if ( !entity.HasComponent<ECS::StaticMeshComponent>() )
                return Common::MakeFormattedError<LODTarget>( "entity {} has no static mesh",
                                                              static_cast<uint64_t>( ids.front() ) );
            const auto& c = entity.GetComponent<ECS::StaticMeshComponent>();
            LODTarget   t;
            t.Id         = ids.front();
            t.DrawEntity = static_cast<uint32_t>( entity.GetHandle() );
            t.MeshHandle = c.MeshHandle;
            t.Mesh       = c.RuntimeMesh ? static_cast<::Desert::Mesh*>( c.RuntimeMesh.get() )
                                         : Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
            if ( !t.Mesh )
                return Common::MakeFormattedError<LODTarget>( "entity {}: its mesh is not loaded",
                                                              static_cast<uint64_t>( ids.front() ) );
            t.Levels = MeshLODRows::LevelCount( *t.Mesh );
            return Common::MakeSuccess( t );
        }
    } // namespace

    Common::ResultStr<bool> ModelingPanel::PreviewLOD( const std::shared_ptr<Desert::Core::Scene>& scene,
                                                       const int                                   level )
    {
        auto& ms = Core::ModelingState::Get();
        if ( level < 0 )
        {
            ms.LODPreviewLevel = -1;
            return Common::MakeSuccess( true );
        }
        const auto target = SelectedLODTarget( scene );
        if ( !target.IsSuccess() )
            return Common::MakeFormattedError<bool>( "LOD preview: {}", target.GetError() );
        if ( static_cast<size_t>( level ) >= target.GetValue().Levels )
            return Common::MakeFormattedError<bool>( "LOD preview: the mesh has {} level(s), no LOD {}",
                                                     target.GetValue().Levels, level );
        ms.LODPreviewLevel = level;
        return Common::MakeSuccess( true );
    }

    // UE 5.8 LODManagerTool (Modeling -> LODs -> LOD Manager): the selected mesh's levels with their triangle
    // counts and screen sizes, and a preview of one level in the active viewport. UE's preview is a separate
    // actor drawing the chosen level beside the source; ours is the source itself, drawn at that level by this
    // view only (SceneRenderer::SetLODPreview), so it is seen where it stands and no other view changes.
    void ModelingPanel::DrawLODsPalette()
    {
        auto& ms = Core::ModelingState::Get();
        ImGui::TextUnformatted( ICON_MDI_LAYERS_TRIPLE "  LOD Manager" );
        ImGui::Separator();

        const auto target = SelectedLODTarget( m_Scene );
        if ( !target.IsSuccess() )
        {
            ms.LODPreviewLevel = -1;
            ImGui::TextDisabled( "%s", target.GetError().c_str() );
            return;
        }
        const LODTarget& t = target.GetValue();
        if ( t.Levels <= 1 )
        {
            ms.LODPreviewLevel = -1;
            ImGui::TextDisabled( "This mesh has a single level: no LOD chain to manage." );
            return;
        }
        if ( ms.LODPreviewLevel >= static_cast<int>( t.Levels ) )
            ms.LODPreviewLevel = -1;

        // Renew the active view's lease every frame the tab shows a preview; the renderer drops it on its own
        // the frame after this stops (tab left, panel closed, mode changed).
        if ( ms.LODPreviewLevel >= 0 )
            if ( const auto leased = ViewportPanel::PreviewLODInActiveViewport( t.DrawEntity, ms.LODPreviewLevel );
                 !leased.IsSuccess() )
                ImGui::TextDisabled( "%s", leased.GetError().c_str() );

        const auto            live = m_Scene->FindEntityByID( t.Id );
        std::optional<size_t> drawing;
        if ( ms.LODPreviewLevel >= 0 )
            drawing = static_cast<size_t>( ms.LODPreviewLevel );
        else if ( live )
        {
            ECS::Entity e = live->get();
            const auto& c = e.GetComponent<ECS::StaticMeshComponent>();
            drawing       = MeshLODRows::DrawingLevel( m_Scene.get(), e, *t.Mesh, c.ForcedLOD, c.LODBias );
        }
        MeshLODRows::DrawLevels( *t.Mesh, drawing, ms.LODPreviewLevel >= 0 ? "Previewing" : "Drawing" );

        ImGui::Spacing();
        ImGui::TextUnformatted( "Preview" );
        int choice = ms.LODPreviewLevel;
        if ( ImGui::RadioButton( "Auto", choice < 0 ) )
            choice = -1;
        // The buttons wrap to the next line when the palette column is too narrow for all of them.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float       right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        for ( int l = 0; l < static_cast<int>( t.Levels ); ++l )
        {
            const std::string label = std::format( "LOD{}", l );
            const float       width =
                 ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize( label.c_str() ).x;
            if ( ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right )
                ImGui::SameLine();
            if ( ImGui::RadioButton( label.c_str(), choice == l ) )
                choice = l;
        }
        if ( choice != ms.LODPreviewLevel )
            if ( const auto done = PreviewLOD( m_Scene, choice ); !done.IsSuccess() )
                LOG_WARN( "{}", done.GetError() );

        ImGui::Spacing();
        MeshLODRows::DrawScreenSizes( t.MeshHandle, *t.Mesh );
    }
} // namespace Desert::Editor
