// EVERY WIDGET THE MODELING PANEL DRAWS IS REACHABLE WITHOUT A MOUSE.
//
// Three tasks in a row (M8, M16b, M17) shipped a Modeling tool that could not be photographed: CubeGrid, Create
// Shape's activation, "Keep both halves" and Trim's cutter existed only as widgets, and the agent that has to
// verify a change has the command palette and the control channel but no cursor. So each widget of
// Editor/Source/Editor/Panels/Modeling/ModelingPanel.cpp is a row of the register below, saying how it is
// reached instead:
//   - Palette: an entry in EditorLayer::BuildPaletteCommands; every token must appear in EditorLayer.cpp;
//   - Set:     a row of Core::kModelingStateRows, the control channel's `modeling` subject (a dragged value);
//   - Exempt:  not an action, with the reason.
//
// The panel and EditorLayer are compiled by no suite, so both are READ AS TEXT; the subject's table is a header
// and is compiled in. A widget added to the panel without a row, a row whose widget is gone, and a palette entry
// removed from EditorLayer.cpp are each red here.

#include <Editor/Core/Selection/ModelingStateProperties.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Editor::Core::kModelingStateRows;
    using Desert::Editor::Core::ModelingState;
    using Desert::Editor::Core::SetModelingStateProperty;

    enum class Reach
    {
        Palette,
        Set,
        Exempt,
    };

    struct WidgetRow
    {
        const char*              Kind;  // the ImGui call
        const char*              Label; // its first argument, whitespace collapsed
        Reach                    How;
        std::vector<std::string> Tokens; // Palette: substrings of EditorLayer.cpp; Set: subject row names;
                                         // Exempt: the reason
    };

    // clang-format off
    const std::vector<WidgetRow> kRegister = {
        // The category rail: a view of the panel, not an action. It follows the active tool (ModelingPanel::
        // m_ShownTool), so choosing a tool from the palette shows that tool's properties.
        { "Button", "label", Reach::Exempt, { "the category rail follows the active tool" } },
        { "Button", "ICON_MDI_VECTOR_SQUARE \" PolyEdit\"", Reach::Palette, { "\"PolyEdit tool\"" } },
        { "Button", "ICON_MDI_VECTOR_SELECTION \" Select Elements\"", Reach::Palette, { "\"Select Elements tool\"" } },
        { "Button", "ICON_MDI_GRID \" CubeGrid\"", Reach::Palette, { "\"CubeGrid tool\"" } },
        { "Button", "MS::ShapeName( shape )", Reach::Palette, { "\"Create shape tool: \"" } },
        // CubeGrid
        { "Button", "\"Accept and Start New\"", Reach::Palette, { "{ \"Accept and Start New\", &MS::ReqAccept }" } },
        { "Button", "\"Reset Grid from Actor\"", Reach::Palette, { "{ \"Reset Grid from Actor\", &MS::ReqResetFromActor }" } },
        { "DragFloat3", "\"Grid Frame Origin\"", Reach::Set, { "CubeGrid.GridFrameOrigin" } },
        { "Checkbox", "\"Show Gizmo\"", Reach::Palette, { "\"Show Gizmo\"" } },
        { "SliderInt", "\"Grid Power\"", Reach::Palette, { "\"Grid Power \"" } },
        { "Button", "ms.CornerMode ? \"Corner Mode: ON (Z)\" : \"Corner Mode: OFF (Z)\"", Reach::Palette,
          { "{ \"Corner Mode (Z)\", &MS::ReqCornerMode }", "\"Corner posts: U+ edge\"", "ReqCornerPosts = posts" } },
        { "Combo", "\"Snap Size\"", Reach::Palette, { "\"Snap Size 1/\"" } },
        { "Checkbox", "\"Hit Unrelated Geometry\"", Reach::Palette, { "\"Hit Unrelated Geometry\"" } },
        { "Checkbox", "\"Generate Collision\"", Reach::Palette, { "\"Generate Collision\"" } },
        { "DragFloat", "\"Current Block Size\"", Reach::Set, { "CubeGrid.CurrentBlockSize" } },
        { "SmallButton", "\"/2##bs\"", Reach::Palette, { "\"Block Size /2\"" } },
        { "SmallButton", "\"x2##bs\"", Reach::Palette, { "\"Block Size x2\"" } },
        { "SliderInt", "\"Blocks / Step\"", Reach::Set, { "CubeGrid.BlocksPerStep" } },
        // Two panel buttons read "Clear": CubeGrid's and the element selection's.
        { "Button", "\"Clear\"", Reach::Palette, { "{ \"Clear\", &MS::ReqClear }", "SelectionOp::Clear" } },
        // Create Shape: `cm( label, ... )` and `count( label, ... )` draw every dimension.
        { "DragFloat", "label", Reach::Set,
          { "CreateShape.Width", "CreateShape.Depth", "CreateShape.Height", "CreateShape.StepDepth",
            "CreateShape.StepHeight" } },
        { "SliderInt", "label", Reach::Set,
          { "CreateShape.Subdivisions", "CreateShape.Slices", "CreateShape.Stacks", "CreateShape.Steps" } },
        { "BeginCombo", "\"Polygroups\"", Reach::Palette, { "\"Create shape polygroups: \"" } },
        { "BeginCombo", "\"Pivot\"", Reach::Palette, { "\"Create shape pivot: \"" } },
        { "Checkbox", "\"Place on Scene\"", Reach::Palette, { "\"Create shape: Place on Scene\"" } },
        { "Combo", "\"##OutputType\"", Reach::Palette, { "\"Output type: Static Mesh\"", "\"Output type: Dynamic Mesh\"" } },
        { "InputTextWithHint", "\"##OutputFolder\"", Reach::Exempt, { "text: `set` carries numbers" } },
        { "InputTextWithHint", "\"##OutputName\"", Reach::Exempt, { "text: `set` carries numbers" } },
        // Select Elements
        { "RadioButton", "Geometry::ToString( mode )", Reach::Palette, { "\"Mesh selection mode: \"" } },
        { "Button", "\"Grow\"", Reach::Palette, { "SelectionOp::Grow" } },
        { "Button", "\"Shrink\"", Reach::Palette, { "SelectionOp::Shrink" } },
        { "Button", "\"Connected\"", Reach::Palette, { "SelectionOp::SelectConnected" } },
        { "Button", "\"All\"", Reach::Palette, { "SelectionOp::SelectAll" } },
        { "DragFloat", "\"##ElementOpDistance\"", Reach::Set, { "Element.Distance" } },
        { "Button", "Core::ToString( row[0] )", Reach::Palette,
          { "\"Mesh operation: \"", "Core::MeshOperation::Extrude", "Core::MeshOperation::PushPull",
            "Core::MeshOperation::Inset", "Core::MeshOperation::Offset", "Core::MeshOperation::Delete",
            "Core::MeshOperation::InsertEdgeLoop" } },
        { "Button", "Core::ToString( row[1] )", Reach::Palette,
          { "Core::MeshOperation::Outset", "Core::MeshOperation::Bevel" } },
        { "SliderFloat", "\"##ElementLoopPosition\"", Reach::Set, { "Element.LoopPosition" } },
        { "DragFloat", "\"##ElementWeldTolerance\"", Reach::Set, { "Element.WeldTolerance" } },
        { "Button", "Core::ToString( MO::Clean )", Reach::Palette, { "Core::MeshOperation::Clean" } },
        { "SliderInt", "\"##ElementSubdivideLevels\"", Reach::Set, { "Element.SubdivideLevels" } },
        { "Checkbox", "\"Smooth (Loop)\"", Reach::Palette, { "\"Subdivide: Smooth (Loop)\"" } },
        { "Button", "Core::ToString( MO::Subdivide )", Reach::Palette, { "Core::MeshOperation::Subdivide" } },
        { "Combo", "\"##ElementMirrorAxis\"", Reach::Palette, { "\"Mirror axis: \"" } },
        { "Checkbox", "\"World\"", Reach::Palette, { "\"Mirror: World\"" } },
        { "Checkbox", "\"Keep -\"", Reach::Palette, { "\"Mirror: Keep -\"" } },
        { "Checkbox", "\"Cut the far half first\"", Reach::Palette, { "\"Mirror: Cut the far half first\"" } },
        { "Button", "Core::ToString( MO::Mirror )", Reach::Palette, { "Core::MeshOperation::Mirror" } },
        { "Combo", "\"##ElementPlaneCutAxis\"", Reach::Palette, { "\"Plane Cut axis: \"" } },
        { "DragFloat", "\"##ElementPlaneCutOffset\"", Reach::Set, { "PlaneCut.Offset" } },
        { "Checkbox", "\"World##PlaneCut\"", Reach::Palette, { "\"Plane Cut: World\"" } },
        { "Checkbox", "\"Keep -##PlaneCut\"", Reach::Palette, { "\"Plane Cut: Keep -\"" } },
        { "Checkbox", "\"Fill##PlaneCut\"", Reach::Palette, { "\"Plane Cut: Fill\"" } },
        { "Checkbox", "\"Keep both halves (new entity)\"", Reach::Palette, { "\"Plane Cut: Keep both halves\"" } },
        { "Button", "Core::ToString( MO::PlaneCut )", Reach::Palette, { "Core::MeshOperation::PlaneCut" } },
        { "Button", "\"Pick Cutter\"", Reach::Palette,
          { "\"Trim: Pick Cutter from the selection\"", "\"Trim: cutter \"" } },
        { "Checkbox", "\"Keep only the inside\"", Reach::Palette, { "\"Trim: Keep only the inside\"" } },
        { "Button", "Core::ToString( MO::Trim )", Reach::Palette, { "Core::MeshOperation::Trim" } },
        // XForm
        { "Combo", "\"##XformPivot\"", Reach::Palette, { "\"XForm pivot: Bounds Center\"", "\"XForm pivot: World Point\"" } },
        { "DragFloat3", "\"##XformPivotPoint\"", Reach::Set, { "XForm.PivotWorldPoint" } },
        { "Button", "Core::ToString( XO::EditPivot )", Reach::Palette, { "\"XForm: \"", "Core::XformOperation::EditPivot" } },
        { "Checkbox", "\"Rotation##Bake\"", Reach::Palette, { "\"Bake: Rotation\"" } },
        { "Checkbox", "\"Scale##Bake\"", Reach::Palette, { "\"Bake: Scale\"" } },
        { "Checkbox", "\"Location##Bake\"", Reach::Palette, { "\"Bake: Location\"" } },
        { "Button", "Core::ToString( XO::BakeTransform )", Reach::Palette, { "Core::XformOperation::BakeTransform" } },
        { "Button", "Core::ToString( XO::Merge )", Reach::Palette, { "Core::XformOperation::Merge" } },
        { "Button", "Core::ToString( XO::Split )", Reach::Palette, { "Core::XformOperation::Split" } },
        { "Checkbox", "\"Split by polygroups\"", Reach::Palette, { "\"Split by polygroups\"" } },
        { "Combo", "\"##XformPatternShape\"", Reach::Palette, { "\"Pattern shape: Circle\"" } },
        { "Combo", "\"##XformPatternAxis\"", Reach::Palette, { "\"Pattern axis: \"" } },
        { "DragInt", "\"##XformPatternCount\"", Reach::Set, { "Pattern.Count" } },
        { "DragFloat", "\"##XformPatternRadius\"", Reach::Set, { "Pattern.Radius" } },
        { "DragFloat", "\"##XformPatternSpacing\"", Reach::Set, { "Pattern.Spacing" } },
        { "Combo", "\"##XformPatternAxisB\"", Reach::Palette, { "\"Pattern axis B: \"" } },
        { "DragInt", "\"##XformPatternCountB\"", Reach::Set, { "Pattern.CountB" } },
        { "DragFloat", "\"##XformPatternSpacingB\"", Reach::Set, { "Pattern.SpacingB" } },
        { "DragFloat", "\"##XformPatternSweep\"", Reach::Set, { "Pattern.SweepDegrees" } },
        { "Checkbox", "\"Orient\"", Reach::Palette, { "\"Pattern: Orient\"" } },
        { "Checkbox", "\"Separate entities##Pattern\"", Reach::Palette, { "\"Pattern: Separate entities\"" } },
        { "Button", "Core::ToString( XO::Pattern )", Reach::Palette, { "Core::XformOperation::Pattern" } },
    };
    // clang-format on

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string Collapse( const std::string& text )
    {
        std::string out;
        bool        space = false;
        for ( const char c : text )
        {
            if ( c == ' ' || c == '\n' || c == '\t' )
            {
                space = true;
                continue;
            }
            if ( space && !out.empty() )
                out += ' ';
            space = false;
            out += c;
        }
        return out;
    }

    // Every `ImGui::<Widget>( <first argument>` in the panel, keyed "Kind|argument". The first argument ends at
    // the first comma or closing parenthesis that is not inside a nested call.
    std::set<std::string> PanelWidgets( const std::string& source )
    {
        static const std::regex kCall(
             R"(ImGui::(Button|SmallButton|Checkbox|Combo|BeginCombo|SliderInt|SliderFloat|DragFloat3|DragFloat|DragInt|RadioButton|InputTextWithHint|InputText|Selectable|MenuItem|ColorEdit3|ColorEdit4|InputFloat|InputInt)\s*\()" );
        std::set<std::string> widgets;
        for ( auto it = std::sregex_iterator( source.begin(), source.end(), kCall ); it != std::sregex_iterator();
              ++it )
        {
            std::size_t i     = static_cast<std::size_t>( it->position() + it->length() );
            int         depth = 0;
            std::string arg;
            for ( ; i < source.size(); ++i )
            {
                const char c = source[i];
                if ( depth == 0 && ( c == ',' || c == ')' ) )
                    break;
                depth += ( c == '(' ) - ( c == ')' );
                arg += c;
            }
            widgets.insert( std::string( ( *it )[1] ) + "|" + Collapse( arg ) );
        }
        return widgets;
    }

    constexpr const char* kPanel             = "Editor/Source/Editor/Panels/Modeling/ModelingPanel.cpp";
    constexpr const char* kLayer             = "Editor/Source/EditorLayer.cpp";
    const std::string     kSelectableInCombo = "Selectable|Geometry::ToString( mode )";
    const std::string     kSelectablePivot   = "Selectable|Geometry::ToString( pivot )";

    bool IsSubjectRow( const std::string& name )
    {
        for ( const auto& row : kModelingStateRows )
            if ( name == row.Name )
                return true;
        return false;
    }
} // namespace

TEST( ModelingPaletteCensus, TheSourcesAreFound )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "run from inside the repository";
    ASSERT_FALSE( ReadFile( RepoRoot() + kPanel ).empty() ) << kPanel;
    ASSERT_FALSE( ReadFile( RepoRoot() + kLayer ).empty() ) << kLayer;
    // A parser that finds nothing would pass everything below; the panel draws well over fifty widgets.
    EXPECT_GT( PanelWidgets( ReadFile( RepoRoot() + kPanel ) ).size(), 50u );
}

TEST( ModelingPaletteCensus, EveryPanelWidgetHasARow )
{
    std::set<std::string> registered;
    for ( const WidgetRow& row : kRegister )
        registered.insert( std::string( row.Kind ) + "|" + row.Label );

    for ( const std::string& widget : PanelWidgets( ReadFile( RepoRoot() + kPanel ) ) )
    {
        // The combos' own entries are the combo's values, reached through the combo's row.
        if ( widget == kSelectableInCombo || widget == kSelectablePivot )
            continue;
        EXPECT_TRUE( registered.count( widget ) )
             << "ModelingPanel draws '" << widget
             << "' and nothing reaches it without a mouse. Add a palette entry in "
                "EditorLayer::BuildPaletteCommands "
                "(or a row of Core::kModelingStateRows for a dragged value) and a row here.";
    }
}

TEST( ModelingPaletteCensus, EveryRowStillHasItsWidget )
{
    const std::set<std::string> widgets = PanelWidgets( ReadFile( RepoRoot() + kPanel ) );
    for ( const WidgetRow& row : kRegister )
        EXPECT_TRUE( widgets.count( std::string( row.Kind ) + "|" + row.Label ) )
             << "the register names " << row.Kind << "( " << row.Label
             << " ) and the panel no longer draws it; remove the row with the widget";
}

TEST( ModelingPaletteCensus, EveryPaletteRowIsInTheRegistry )
{
    const std::string layer = ReadFile( RepoRoot() + kLayer );
    for ( const WidgetRow& row : kRegister )
    {
        if ( row.How != Reach::Palette )
            continue;
        for ( const std::string& token : row.Tokens )
            EXPECT_NE( layer.find( token ), std::string::npos )
                 << row.Kind << "( " << row.Label << " ) is reached through the palette entry built from '"
                 << token << "', and EditorLayer.cpp no longer contains it";
    }
}

TEST( ModelingPaletteCensus, EverySetRowIsASubjectRowAndEverySubjectRowIsAWidget )
{
    std::set<std::string> named;
    for ( const WidgetRow& row : kRegister )
    {
        if ( row.How != Reach::Set )
            continue;
        for ( const std::string& name : row.Tokens )
        {
            EXPECT_TRUE( IsSubjectRow( name ) ) << name << " is not a row of Core::kModelingStateRows";
            named.insert( name );
        }
    }
    for ( const auto& row : kModelingStateRows )
        EXPECT_TRUE( named.count( row.Name ) ) << row.Name << " stands for no widget of the panel";
}

// A `set` lands in the field the widget writes, and a value the widget cannot hold is refused, not clamped.
TEST( ModelingPaletteCensus, ASetWritesTheWidgetsFieldAndRefusesWhatTheWidgetCannotHold )
{
    ModelingState state;
    ASSERT_TRUE( SetModelingStateProperty( state, "PlaneCut.Offset", { 12.5f } ) );
    EXPECT_FLOAT_EQ( state.ElementPlaneCutOffset, 12.5f );
    ASSERT_TRUE( SetModelingStateProperty( state, "Pattern.Count", { 6.0f } ) );
    EXPECT_EQ( state.XformPattern.Count, 6 );
    ASSERT_TRUE( SetModelingStateProperty( state, "CubeGrid.GridFrameOrigin", { 1.0f, 2.0f, 3.0f } ) );
    EXPECT_FLOAT_EQ( state.GridOrigin.z, 3.0f );

    EXPECT_FALSE( SetModelingStateProperty( state, "Pattern.Count", { 2.5f } ) );
    EXPECT_FALSE( SetModelingStateProperty( state, "Element.LoopPosition", { 1.5f } ) );
    EXPECT_FALSE( SetModelingStateProperty( state, "CubeGrid.GridFrameOrigin", { 1.0f } ) );
    const auto unknown = SetModelingStateProperty( state, "PlaneCut.Ofset", { 1.0f } );
    ASSERT_FALSE( unknown );
    EXPECT_NE( unknown.GetError().find( "PlaneCut.Offset" ), std::string::npos ) << "the refusal lists the rows";
    EXPECT_FLOAT_EQ( state.ElementPlaneCutOffset, 12.5f ) << "a refused write changed nothing";
}

// Grid Power and the block-size steps are one function each for the slider, the buttons, the shortcuts and the
// palette; they must invert each other.
TEST( ModelingPaletteCensus, GridPowerRoundTripsThroughTheBlockSize )
{
    ModelingState state;
    for ( int power = 0; power <= ModelingState::MaxGridPower; ++power )
    {
        state.SetGridPower( power );
        EXPECT_EQ( state.GridPower(), power );
    }
    state.SetGridPower( 2 );
    EXPECT_FLOAT_EQ( state.CellSize, 25.0f );
    state.DoubleBlockSize();
    EXPECT_EQ( state.GridPower(), 1 );
    state.CellSize = ModelingState::MinCellSize;
    state.HalveBlockSize();
    EXPECT_FLOAT_EQ( state.CellSize, ModelingState::MinCellSize );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
