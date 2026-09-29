// EVERY LANDSCAPE PALETTE ENTRY IS NAMED HERE.
//
// The Landscape group of the command palette (EditorLayer::BuildPaletteCommands) is how an agent with no cursor
// drives the Landscape mode: Sculpt / Paint / Manage, the New Landscape form, heightmap import/export, the Paint
// target layers, the Edit Layers stack and every tool control. Each `{ "Landscape", <label>, ... }` entry of
// Editor/Source/EditorLayer.cpp is a row of the register below — its label expression, how many entries are
// built from that expression, and what it reaches. A new entry with no row, a row whose entry is gone, and a
// changed count are each red here, so the palette cannot grow a command nobody has accounted for.
//
// EditorLayer.cpp is compiled by no suite, so it is READ AS TEXT.

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct PaletteRow
    {
        const char* Label; // the label expression, whitespace collapsed
        int         Count; // entries built from this expression
        const char* Reaches;
    };

    // clang-format off
    const std::vector<PaletteRow> kRegister = {
        { "\"Sculpt mode\"", 1, "the Sculpt tab of the mode toolbar" },
        { "\"Manage mode\"", 1, "the Manage tab (New Landscape form)" },
        { "\"New Landscape: noise fill with erosion and hydro erosion\"", 1, "New Landscape form: noise fill preset" },
        { "\"New Landscape: noise fill without erosion\"", 1, "New Landscape form: noise fill preset" },
        { "\"New Landscape: Create\"", 1, "New Landscape form: Create" },
        { "\"New Landscape: Cancel\"", 1, "New Landscape form: Cancel" },
        { "\"Import heightmap as a new landscape: \" + rel", 1, "Manage: import a heightmap file as a new landscape" },
        { "\"Import heightmap into the landscape: \" + rel", 1, "Manage: import a heightmap file into the landscape" },
        { "( selected ? \"Export heightmap of the selected tiles: \" : \"Export heightmap: \" ) + rel", 1,
          "Manage: export the whole landscape / the selected tiles" },
        { "label", 1, "\"Paint mode\" and \"Tool: Paint\": the Paint tab and its one tool" },
        { "\"Create Layer Info\"", 1, "Paint: the \"+\" of the Target Layers list" },
        // Edit Layers (LandscapePanel::DrawEditLayers): the row buttons act on the EDITING layer.
        { "\"Create Edit Layer\"", 1, "Edit Layers: Create Layer" },
        { "\"Editing edit layer: toggle visibility\"", 1, "Edit Layers: the eye of the editing layer" },
        { "\"Editing edit layer: toggle lock\"", 1, "Edit Layers: the lock of the editing layer" },
        { "\"Editing edit layer: delete\"", 1, "Edit Layers: delete the editing layer" },
        { "std::format( \"Editing edit layer: alphas {:.1f}\", alpha )", 1,
          "Edit Layers: the height/weight alpha sliders (0.0, 0.5, 1.0)" },
        { "std::format( \"Edit layer: {}\", layer.Name )", 1, "Edit Layers: select a row (make it editing)" },
        { "\"Target layer: Visibility (holes)\"", 1, "Paint: the Visibility target layer" },
        { "\"Target layer: \" + info->LayerName", 1, "Paint: a target layer row" },
        { "control.Label", 2, "every Core::LandscapeToolControls() row: a stroke request, or a settings step" },
        { "lower ? \"Stroke at the viewport centre, lowering\" : \"Stroke at the viewport centre\"", 1,
          "a brush stroke at the viewport centre (the viewport click)" },
    };
    // clang-format on

    constexpr const char* kLayer = "Editor/Source/EditorLayer.cpp";

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

    // Every `{ "Landscape", <label>` of the source, keyed by the collapsed label expression, with its count. The
    // label ends at the first comma or closing bracket outside a nested bracket and outside a string literal (a
    // label may itself contain a comma: "Stroke at the viewport centre, lowering").
    std::map<std::string, int> PaletteLabels( const std::string& source )
    {
        static const std::regex    kEntry( R"(\{\s*"Landscape"\s*,)" );
        std::map<std::string, int> labels;
        for ( auto it = std::sregex_iterator( source.begin(), source.end(), kEntry ); it != std::sregex_iterator();
              ++it )
        {
            std::size_t i        = static_cast<std::size_t>( it->position() + it->length() );
            int         depth    = 0;
            bool        inString = false;
            std::string label;
            for ( ; i < source.size(); ++i )
            {
                const char c = source[i];
                if ( inString )
                {
                    label += c;
                    if ( c == '\\' && i + 1 < source.size() )
                        label += source[++i];
                    else if ( c == '"' )
                        inString = false;
                    continue;
                }
                if ( depth == 0 && ( c == ',' || c == ')' || c == '}' ) )
                    break;
                if ( c == '"' )
                    inString = true;
                depth += ( c == '(' || c == '[' || c == '{' ) - ( c == ')' || c == ']' || c == '}' );
                label += c;
            }
            ++labels[Collapse( label )];
        }
        return labels;
    }
} // namespace

TEST( LandscapePaletteCensus, TheSourceIsFound )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "run from inside the repository";
    const std::string layer = ReadFile( RepoRoot() + kLayer );
    ASSERT_FALSE( layer.empty() ) << kLayer;
    // A parser that finds nothing would pass everything below; the group builds over twenty entries.
    EXPECT_GE( PaletteLabels( layer ).size(), 20u );
}

TEST( LandscapePaletteCensus, EveryPaletteEntryHasARow )
{
    std::map<std::string, int> registered;
    for ( const PaletteRow& row : kRegister )
        registered[row.Label] = row.Count;

    for ( const auto& [label, count] : PaletteLabels( ReadFile( RepoRoot() + kLayer ) ) )
    {
        const auto row = registered.find( label );
        if ( row == registered.end() )
        {
            ADD_FAILURE() << "EditorLayer.cpp builds the Landscape palette entry '" << label
                          << "' and the census has no row for it. Add a row naming what it reaches.";
            continue;
        }
        EXPECT_EQ( row->second, count )
             << "'" << label << "' builds " << count << " palette entries; the census row says " << row->second;
    }
}

TEST( LandscapePaletteCensus, EveryRowStillHasItsEntry )
{
    const std::map<std::string, int> labels = PaletteLabels( ReadFile( RepoRoot() + kLayer ) );
    for ( const PaletteRow& row : kRegister )
        EXPECT_TRUE( labels.count( row.Label ) )
             << "the census names the Landscape palette entry '" << row.Label
             << "' and EditorLayer.cpp no longer builds it; remove the row with the entry";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
