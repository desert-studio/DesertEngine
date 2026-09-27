// THE FOLIAGE PALETTE (FO-UI1): what its rows state, what the brush preview promises, what the search keeps, the
// two files it writes — and a census that every control of the Foliage panel is reachable without a mouse.
//
// The census follows ModelingPaletteCensus: each widget of Editor/Source/Editor/Panels/Foliage/FoliagePanel.cpp
// is a row of the register below, either reached through a palette entry in EditorLayer::BuildPaletteCommands
// (every token must appear in EditorLayer.cpp) or exempt with the reason. The panel and EditorLayer are compiled
// by no suite, so both are READ AS TEXT. A widget added without a row, a row whose widget is gone, and a palette
// entry removed from EditorLayer.cpp are each red here.

#include <Editor/Panels/Collections/CollectionFoliageTypes.hpp>
#include <Editor/Panels/Foliage/FoliagePalette.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Editor::Foliage;
    using Desert::Assets::Serialization::FoliageFloatInterval;
    using Desert::Assets::Serialization::FoliageTypeData;

    std::vector<glm::mat4> Row( int count, float spacing )
    {
        std::vector<glm::mat4> out;
        for ( int i = 0; i < count; ++i )
            out.push_back(
                 glm::translate( glm::mat4( 1.0f ), glm::vec3( static_cast<float>( i ) * spacing, 0, 0 ) ) );
        return out;
    }

    std::string TextOf( const std::filesystem::path& file )
    {
        const auto text = Common::Utils::FileSystem::ReadFileContent( file );
        return text ? text.GetValue() : std::string();
    }

    class FoliagePaletteFiles : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_Root = std::filesystem::temp_directory_path() / ( "desert_foui1_" + std::to_string( stamp ) );
            std::filesystem::create_directories( m_Root );
        }
        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
        }
        std::filesystem::path m_Root;
    };
} // namespace

// ----- cost -----------------------------------------------------------------------------------------------------

TEST( FoliagePalette, CostCountsEveryInstanceAndTheTrianglesOfThoseInCullRange )
{
    // Ten instances 100 cm apart from the view; a cull distance fading 0..0 means never culled.
    const auto instances = Row( 10, 100.0f );
    const auto never     = MeasureFoliageTypeCost( instances, FoliageFloatInterval{ 0.0f, 0.0f }, {}, 12u, false );
    EXPECT_EQ( never.Instances, 10u );
    EXPECT_EQ( never.InCullRange, 10u );
    EXPECT_EQ( never.Triangles, 120u );

    // Gone from 450 cm with no fade band: instances at 0..400 stay, 500..900 go.
    const auto culled =
         MeasureFoliageTypeCost( instances, FoliageFloatInterval{ 450.0f, 450.0f }, {}, 12u, false );
    EXPECT_EQ( culled.Instances, 10u );
    EXPECT_EQ( culled.InCullRange, 5u );
    EXPECT_EQ( culled.Triangles, 60u );
    EXPECT_EQ( culled.TrianglesPerInstance, 12u );
}

TEST( FoliagePalette, AHiddenTypeCostsNothingButKeepsItsCount )
{
    const auto cost = MeasureFoliageTypeCost( Row( 7, 10.0f ), FoliageFloatInterval{}, {}, 100u, true );
    EXPECT_EQ( cost.Instances, 7u );
    EXPECT_EQ( cost.InCullRange, 0u );
    EXPECT_EQ( cost.Triangles, 0u );
}

// ----- footprint preview ----------------------------------------------------------------------------------------

TEST( FoliagePalette, TheFootprintPromisesWhatTheBrushWouldTopUp )
{
    // UE default density 100 per 1000x1000 cm; a 500 cm brush holds pi * 0.25 * 100 = 78.5.
    const auto empty = PreviewFoliageFootprint( 100.0f, 500.0f, 1.0f, {}, {} );
    EXPECT_NEAR( empty.Desired, Desert::Editor::Tools::FoliageBrushDesiredCount( 100.0f, 500.0f, 1.0f ), 1e-4f );
    EXPECT_NEAR( empty.Desired, 78.54f, 0.01f );
    EXPECT_EQ( empty.Existing, 0u );
    EXPECT_NEAR( empty.Expected, empty.Desired, 1e-4f );

    // Twenty instances inside the sphere (0..475 cm) and twenty outside it: only the inside ones count.
    auto inside  = Row( 20, 25.0f );
    auto outside = Row( 20, 25.0f );
    for ( auto& m : outside )
        m[3].x += 600.0f;
    inside.insert( inside.end(), outside.begin(), outside.end() );
    const auto some = PreviewFoliageFootprint( 100.0f, 500.0f, 1.0f, inside, {} );
    EXPECT_EQ( some.Existing, 20u );
    EXPECT_NEAR( some.Expected, some.Desired - 20.0f, 1e-3f );

    // A full brush promises nothing, never a negative count.
    const auto full = PreviewFoliageFootprint( 100.0f, 500.0f, 0.1f, inside, {} );
    EXPECT_EQ( full.Expected, 0.0f );
}

// ----- search ---------------------------------------------------------------------------------------------------

TEST( FoliagePalette, SearchKeepsRowsHoldingEveryWordIgnoringCase )
{
    EXPECT_TRUE( PaletteNameMatches( "SM_Fern_Large", "" ) );
    EXPECT_TRUE( PaletteNameMatches( "SM_Fern_Large", "fern" ) );
    EXPECT_TRUE( PaletteNameMatches( "SM_Fern_Large", "  LARGE   fern " ) );
    EXPECT_FALSE( PaletteNameMatches( "SM_Fern_Large", "fern small" ) );
    EXPECT_FALSE( PaletteNameMatches( "Grass", "fern" ) );
}

// ----- the files the palette writes -----------------------------------------------------------------------------

TEST_F( FoliagePaletteFiles, SaveAsAssetWritesACopyUnderANewIdentity )
{
    FoliageTypeData data;
    data.Mesh           = { "0123456789abcdef0123456789abcdef", "Meshes/Fern.demesh" };
    data.Density        = 321.0f;
    const auto original = m_Root / "Fern.defoliage";
    ASSERT_TRUE( Desert::Assets::Serialization::SaveFoliageTypeFile( original, data ) );
    const auto parsed = Desert::Assets::Serialization::ParseFoliageType(
         TextOf( original ) );
    ASSERT_TRUE( parsed );

    const auto copy = SaveFoliageTypeCopy( original, parsed.GetValue() );
    ASSERT_TRUE( copy ) << copy.GetError();
    EXPECT_EQ( copy.GetValue().filename(), "Fern_Copy.defoliage" );
    const auto second = SaveFoliageTypeCopy( original, parsed.GetValue() );
    ASSERT_TRUE( second );
    EXPECT_EQ( second.GetValue().filename(), "Fern_Copy_1.defoliage" );

    const auto read = Desert::Assets::Serialization::ParseFoliageType(
         TextOf( copy.GetValue() ) );
    ASSERT_TRUE( read );
    EXPECT_NE( read.GetValue().Header->Guid, parsed.GetValue().Header->Guid ) << "a copy stating the original's "
                                                                                 "GUID is one asset on two paths";
    FoliageTypeData a = read.GetValue(), b = parsed.GetValue();
    a.Header.reset();
    b.Header.reset();
    EXPECT_EQ( a, b ) << "the copy carries every number of the original";

    EXPECT_FALSE( SaveFoliageTypeCopy( m_Root / "Fern.demesh", data ) );
}

TEST_F( FoliagePaletteFiles, APresetIsACollectionWhoseRecordsResolveBackToItsTypes )
{
    const auto      assets = m_Root / "Assets";
    const auto      types  = assets / "Foliage";
    FoliageTypeData fern, grass;
    fern.Mesh            = { "0123456789abcdef0123456789abcdef", "Meshes/Fern.demesh" };
    grass.Mesh           = { "fedcba9876543210fedcba9876543210", "Meshes/Grass.demesh" };
    grass.Density        = 400.0f;
    const auto fernFile  = Desert::Assets::Serialization::FindOrCreateFoliageTypeFile( types, fern, "Fern" );
    const auto grassFile = Desert::Assets::Serialization::FindOrCreateFoliageTypeFile( types, grass, "Grass" );
    ASSERT_TRUE( fernFile && grassFile );

    const std::vector<PalettePresetEntry> entries = {
         { "Fern", "Assets/Meshes/Fern.fbx", { fernFile.GetValue().Guid, "Foliage/Fern.defoliage" } },
         { "Grass", "Assets/Meshes/Grass.fbx", { grassFile.GetValue().Guid, "Foliage/Grass.defoliage" } },
    };
    const auto collections = m_Root / "Collections";
    const auto saved       = SavePalettePreset( collections, "Forest", entries );
    ASSERT_TRUE( saved ) << saved.GetError();
    EXPECT_EQ( saved.GetValue(), collections / "Forest" / "collection.json" );

    // Applying = the collection dropped on the palette: each record resolves to its own file, no mesh lookup.
    auto manifest = Desert::Editor::ReadCollectionManifest(
         TextOf( saved.GetValue() ) );
    ASSERT_TRUE( manifest );
    auto       m        = manifest.GetValue();
    const auto resolved = Desert::Editor::ResolveCollectionFoliageTypes(
         m, types, assets,
         []( const Desert::Editor::CollectionManifestItem& item )
         {
             return Common::MakeFormattedError<Desert::Assets::AssetGuidRef>(
                  "no mesh lookup expected for '{}'", item.Name );
         } );
    ASSERT_TRUE( resolved ) << resolved.GetError();
    ASSERT_EQ( resolved.GetValue().Types.size(), 2u );
    EXPECT_EQ( resolved.GetValue().Types[0].Guid, fernFile.GetValue().Guid );
    EXPECT_EQ( resolved.GetValue().Types[1].Guid, grassFile.GetValue().Guid );
    EXPECT_FALSE( resolved.GetValue().ManifestChanged );

    EXPECT_FALSE( SavePalettePreset( collections, "Forest", entries ) ) << "a preset overwrote a collection";
    EXPECT_FALSE( SavePalettePreset( collections, "../Escape", entries ) );
    EXPECT_FALSE( SavePalettePreset( collections, "Meadow", {} ) );
}

// ----- census: every panel widget is reachable without a mouse --------------------------------------------------

namespace
{
    enum class Reach
    {
        Palette,
        Exempt,
    };

    struct WidgetRow
    {
        const char*              Kind;
        const char*              Label; // the first argument, whitespace collapsed
        Reach                    How;
        std::vector<std::string> Tokens; // Palette: substrings of EditorLayer.cpp; Exempt: the reason
    };

    // clang-format off
    const std::vector<WidgetRow> kRegister = {
        // The tool bar (UE order).
        { "ToolButton", "const char* label", Reach::Exempt, { "the tool bar's button helper; its calls are the rows below" } },
        { "Button", "label", Reach::Exempt, { "inside ToolButton" } },
        { "ToolButton", "ICON_MDI_CURSOR_DEFAULT_CLICK \" Select\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Select" } },
        { "ToolButton", "ICON_MDI_SELECT_ALL \" All\"", Reach::Palette, { "\"Select all instances of the checked types\"" } },
        { "ToolButton", "ICON_MDI_SELECT_OFF \" Deselect\"", Reach::Palette, { "\"Select no instances\"" } },
        { "ToolButton", "ICON_MDI_LASSO \" Lasso\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Lasso" } },
        { "ToolButton", "ICON_MDI_BRUSH \" Paint\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Paint" } },
        { "ToolButton", "ICON_MDI_REFRESH \" Reapply\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Reapply" } },
        { "ToolButton", "ICON_MDI_SPROUT \" Single\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Single" } },
        { "ToolButton", "ICON_MDI_FORMAT_COLOR_FILL \" Fill\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Fill" } },
        { "ToolButton", "ICON_MDI_ERASER \" Remove\"", Reach::Palette, { "\"Tool: \"", "Core::FoliageTool::Remove" } },
        // Brush Options and Filters.
        { "SliderFloat", "\"##BrushSize\"", Reach::Exempt, { "a value, not an action: commands stroke with the current size" } },
        { "SliderFloat", "\"##PaintDensity\"", Reach::Exempt, { "a value, not an action: commands stroke with the current density" } },
        { "Checkbox", "\"##FilterLandscape\"", Reach::Palette, { "\"Brush filter: landscape on/off\"" } },
        { "Checkbox", "\"##FilterStaticMesh\"", Reach::Palette, { "\"Brush filter: static meshes on/off\"" } },
        { "BeginCombo", "\"##BrushLayers\"", Reach::Palette, { "\"Brush layer filter: off\"" } },
        { "Checkbox", "name.c_str()", Reach::Palette, { "\"Brush layer filter: toggle \"" } },
        { "SliderFloat", "\"##BrushLayerMin\"", Reach::Exempt, { "a value: the brush layer filter's threshold" } },
        // Selection and Reapply.
        { "Button", "ICON_MDI_DELETE \" Delete\"", Reach::Palette, { "\"Delete selected instances\"" } },
        { "DragFloat3", "\"##MoveOffset\"", Reach::Exempt, { "a value: the offset the move command applies" } },
        { "Button", "ICON_MDI_ARROW_ALL \" Move by offset (cm)\"", Reach::Palette, { "\"Move selected instances by the panel offset\"" } },
        { "Checkbox", "names[i]", Reach::Exempt, { "Reapply's switches: values the Reapply stroke reads" } },
        // The palette.
        { "Button", "ICON_MDI_PLUS \" Foliage\"", Reach::Palette, { "\"Add type to the palette: \"", "\"Add mesh to the palette: \"" } },
        { "Selectable", "( ICON_MDI_GRASS \" \" + path.stem().string() ).c_str()", Reach::Palette, { "\"Add type to the palette: \"" } },
        { "Selectable", "( ICON_MDI_CUBE_OUTLINE \" \" + path.stem().string() ).c_str()", Reach::Palette, { "\"Add mesh to the palette: \"" } },
        { "Button", "ICON_MDI_CURSOR_DEFAULT_CLICK \" From Selection\"", Reach::Palette, { "\"Add the selected entity to the palette\"" } },
        { "Button", "grid ? ICON_MDI_VIEW_LIST : ICON_MDI_VIEW_GRID", Reach::Palette, { "\"Palette: grid view\"", "\"Palette: list view\"" } },
        { "InputTextWithHint", "\"##FoliageSearch\"", Reach::Palette, { "\"Palette: clear the search\"" } },
        { "BeginMenu", "ICON_MDI_SWAP_HORIZONTAL \" Replace\"", Reach::Palette, { "\"Replace the edited type with: \"" } },
        { "MenuItem", "path.stem().string().c_str()", Reach::Palette, { "\"Replace the edited type with: \"" } },
        { "MenuItem", "ICON_MDI_SELECT_ALL \" Select all instances\"", Reach::Palette, { "\"Edited type: select all instances\"" } },
        { "MenuItem", "ICON_MDI_CONTENT_SAVE \" Save as asset\"", Reach::Palette, { "\"Edited type: save as asset\"" } },
        { "MenuItem", "ICON_MDI_FOLDER_SEARCH \" Show in Content Browser\"", Reach::Palette, { "\"Edited type: show in Content Browser\"" } },
        { "MenuItem", "ICON_MDI_DELETE \" Remove\"", Reach::Palette, { "\"Edited type: remove from the palette\"" } },
        { "Checkbox", "\"##active\"", Reach::Palette, { "\"Edited type: toggle checked (paint with it)\"", "\"Palette: edit \"" } },
        { "SmallButton", "hidden ? ICON_MDI_EYE_OFF : ICON_MDI_EYE", Reach::Palette, { "\"Edited type: toggle visibility\"" } },
        { "Selectable", "name.c_str()", Reach::Palette, { "\"Palette: edit \"" } },
        { "InvisibleButton", "\"##tile\"", Reach::Palette, { "\"Edited type: toggle checked (paint with it)\"" } },
        { "InputText", "\"##PresetName\"", Reach::Exempt, { "a value: the name \"Palette: save as preset\" writes under" } },
        { "Button", "ICON_MDI_CONTENT_SAVE_ALL \" Save Preset\"", Reach::Palette, { "\"Palette: save as preset\"", "\"Add collection to the palette: \"" } },
        // Details of the edited type (the type's own fields are FoliagePaintTool::DrawTypeSettings, the file's).
        { "Checkbox", "\"##CastShadows\"", Reach::Palette, { "\"Edited type: cast shadows on/off\"" } },
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

    // Every `ImGui::<Widget>( <first argument>` and `ToolButton( <first argument>` in the panel, keyed
    // "Kind|argument". The first argument ends at the first comma or closing parenthesis outside a nested call.
    std::set<std::string> PanelWidgets( const std::string& source )
    {
        static const std::regex kCall(
             R"((?:ImGui::)?\b(Button|SmallButton|Checkbox|BeginCombo|SliderFloat|DragFloat3|InputTextWithHint|InputText|Selectable|MenuItem|BeginMenu|InvisibleButton|ToolButton)\s*\()" );
        std::set<std::string> widgets;
        for ( auto it = std::sregex_iterator( source.begin(), source.end(), kCall ); it != std::sregex_iterator();
              ++it )
        {
            const std::string kind( ( *it )[1] );
            // Only ImGui's own widgets and the panel's ToolButton: `Row::` helpers and the like are not controls.
            const auto at = static_cast<std::size_t>( it->position() );
            if ( kind != "ToolButton" && source.compare( at, 7, "ImGui::" ) != 0 )
                continue;
            std::size_t i     = at + static_cast<std::size_t>( it->length() );
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
            widgets.insert( kind + "|" + Collapse( arg ) );
        }
        return widgets;
    }

    constexpr const char* kPanel = "Editor/Source/Editor/Panels/Foliage/FoliagePanel.cpp";
    constexpr const char* kLayer = "Editor/Source/EditorLayer.cpp";
} // namespace

TEST( FoliagePaletteCensus, TheSourcesAreFound )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    EXPECT_FALSE( ReadFile( root + kPanel ).empty() ) << kPanel;
    EXPECT_FALSE( ReadFile( root + kLayer ).empty() ) << kLayer;
    EXPECT_GT( PanelWidgets( ReadFile( root + kPanel ) ).size(), 30u ) << "the widget scan found almost nothing";
}

TEST( FoliagePaletteCensus, EveryPanelWidgetHasARow )
{
    std::set<std::string> registered;
    for ( const auto& row : kRegister )
        registered.insert( std::string( row.Kind ) + "|" + row.Label );
    for ( const auto& widget : PanelWidgets( ReadFile( RepoRoot() + kPanel ) ) )
        EXPECT_TRUE( registered.count( widget ) )
             << "the Foliage panel draws " << widget
             << " and the register says nothing about how to reach it without a mouse: add a palette entry in "
                "EditorLayer::BuildPaletteCommands and a row here, or a row exempting it with the reason";
}

TEST( FoliagePaletteCensus, EveryRowStillHasItsWidget )
{
    const auto widgets = PanelWidgets( ReadFile( RepoRoot() + kPanel ) );
    for ( const auto& row : kRegister )
        EXPECT_TRUE( widgets.count( std::string( row.Kind ) + "|" + row.Label ) )
             << "the register names " << row.Kind << "( " << row.Label << " ), which the panel no longer draws";
}

TEST( FoliagePaletteCensus, EveryPaletteRowIsInTheRegistry )
{
    const std::string layer = ReadFile( RepoRoot() + kLayer );
    for ( const auto& row : kRegister )
    {
        if ( row.How != Reach::Palette )
            continue;
        for ( const auto& token : row.Tokens )
            EXPECT_NE( layer.find( token ), std::string::npos )
                 << row.Kind << "( " << row.Label << " ) is reached through the palette entry built from '"
                 << token << "', which EditorLayer.cpp no longer holds";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
