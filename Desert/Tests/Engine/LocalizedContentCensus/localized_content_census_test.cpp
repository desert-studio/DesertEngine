// THE GATE THIS TASK WAS ASKED TO LEAVE BEHIND: it goes red when a new UNTRANSLATABLE literal appears in
// shipped content, and it goes red when a key nothing can resolve appears in a scene.
//
// IS IT POSSIBLE WITHOUT FALSE POSITIVES? For AUTHORED CONTENT, yes, and this suite is the proof. Three
// properties make it exact rather than heuristic:
//
//   1. The SITES are enumerable. A reader-facing authored string in a `.desce` can sit in exactly four
//      places (UIText.Text, UIInputField.Placeholder, each item of UIDropdown.Options, Text.Text). Every
//      other string field in a UI component is an identifier — a data-store key, a message name, a drag
//      payload, a screen name — and the census never looks at one, so it cannot fire on one.
//   2. A KEY IS RECOGNISABLE BY SIGHT. The leading '#' is the whole rule (Ю15 decision 2), so no table,
//      no heuristic and no name convention is needed to tell a key from a literal.
//   3. The DECISIONS are registered, not the strings. A row below is a decision somebody took about a
//      scene or about one string, with the reason attached. That is what keeps the register at 12 rows
//      for 55 literals, and what makes a NEW literal in an already-translated scene stand out.
//
// FOR C++ IT IS NOT POSSIBLE, AND THAT IS MEASURED RATHER THAN ASSERTED. In Editor/Source there are 977
// ImGui call sites whose first argument is a string literal, and 182 of those literals carry an ImGui
// `##` identity suffix — so the same literal is half prose and half identifier, and ImGui's widget IDs
// (and therefore the saved dock layout) are DERIVED from it. On top of that the editor is developer
// chrome, not a shipped surface: there is no consumer for a translated Details panel. A gate over those
// 977 sites would be a list of exceptions longer than the list of hits. So it is refused, here, in
// writing — see the report.

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <gtest/gtest.h>

#include <Engine/Localization/LocalizedText.hpp>
#include <Engine/Localization/StringTable.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Desert::Localization;

namespace
{
    /**
     * @brief One DECISION about content that is deliberately not translated.
     *
     * `Scene` is the file's stem. `Text` empty means "every literal in this scene is deliberate"; a
     * non-empty `Text` narrows the row to that one string, which is what an otherwise-translated scene
     * needs. `Why` is the sentence a reader gets when they wonder why their new label is not in the table.
     *
     * A ROW IS PINNED, NOT A COUNT. A gate that asserted "there are 55 literals" is satisfied by editing
     * the number, which is the failure this project has a name for; every row here has to match something
     * real, and the suite says so when one stops matching.
     */
    struct LiteralRule
    {
        const char* Scene;
        const char* Text;
        const char* Why;
    };

    const std::vector<LiteralRule>& LiteralRegister()
    {
        static const std::vector<LiteralRule> rules = {
             // --- R2: proper nouns. A name is not translated, it is spelled. -------------------------
             { "MainMenu", "DESERT[color=#FF7A33]RP[/color]", "the product's own name, with its brand colour" },
             { "MainMenu", "Nico_Bellic",
               "a player's character name; gameplay overwrites it through the "
               "player.name binding" },
             { "MainMenu", "Los Santos RP", "a server's name, as its operator spelled it" },
             { "MainMenu", "DesertRP - build v0.9.1", "a product name and a build number" },
             { "Desert_Sandbox", "Desert Engine", "the engine's own name, in world text" },
             { "Starter", "Desert Engine", "the engine's own name, in world text" },
             { "RDG_DeferredSSRGI", "Desert Engine", "the engine's own name, in world text (RDG3 bench scene)" },

             // --- R3: test fixtures. ------------------------------------------------------------------
             // These scenes exist to be photographed and compared. Their labels are the NEGATIVE CONTROL
             // of the relation this whole subsystem is judged on — "a language change must not move a
             // literal" — so translating them would delete the control that proves the feature works.
             // UI_ElementProbe is the one exception and it is deliberate: its title IS keyed, so that one
             // frame of one scene carries both directions of the relation.
             { "UI_ElementProbe", "", "probe fixture: every label but the title is the negative control" },
             { "UI_SpriteSlots", "", "probe fixture (sprite slots)" },
             { "UI_TwoCanvases", "", "probe fixture (two canvases)" },
             { "UI_MaterialProbe", "", "probe fixture (UI materials)" },
             { "UI_TransformProbe", "", "probe fixture (UI transforms)" },
             { "UI_VisibilityStack_Visible", "", "probe fixture (visibility stack)" },
             { "UI_VisibilityStack_Hidden", "", "probe fixture (visibility stack)" },
             { "UI_VisibilityStack_Collapsed", "", "probe fixture (visibility stack)" },
             { "MAT_ProbeTextRows", "", "probe fixture: RED/GREEN/BLUE name the colours being measured" },

             // Arrived in the same merge as this census, from two tasks that could not have known about
             // it. Same rule as the rows above: these scenes exist to be photographed, and their labels
             // are the negative control that proves a language change does NOT move a literal. Keying
             // them would delete the control.
             { "UI_OverlayProbe", "", "probe fixture (tooltips, menus, modals, toasts)" },
             { "UI_ThemeProbe_Dark", "", "probe fixture (theme resolution, dark)" },
             { "UI_ThemeProbe_Light", "", "probe fixture (theme resolution, light)" },
             { "UI_ThemeProbe_None", "", "probe fixture (no theme: every slot falls back to authored)" },
             { "UI_ThemeProbe_A11y", "", "probe fixture (font scale and high contrast)" },

             // Ю16's three, on the same terms. UI_RenderTextureProbe and its _Hidden twin are one A/B:
             // every label in them must be byte-identical between the two shots, because the pixels that
             // are ALLOWED to differ are exactly the three render-texture rects. Keying any of them would
             // put a translation between the two frames and destroy the control.
             { "UI_RenderTextureProbe", "", "probe fixture (render-texture element: live, and two refusals)" },
             { "UI_RenderTextureProbe_Hidden", "",
               "probe fixture: the same scene with the elements hidden — "
               "the negative control of the one above" },
             { "UI_RenderTextureBudget", "", "probe fixture (six elements against six renderer slots)" },

             // Ю17's four, same terms. The list probes are an A/B whose whole content is ROW NUMBERS:
             // "Row 0000" is the coordinate that says WHICH rows the window covered, and the two shots
             // are compared on exactly that. A translation between them would delete the measurement.
             // The slot pair's titles name the CONTAINER under test, which is what the log lines beside
             // them are matched against.
             { "UI_ListViewProbe", "", "probe fixture: the labels ARE the row indices being measured" },
             { "UI_ListViewProbe_Scrolled", "",
               "probe fixture: the same list scrolled — the negative "
               "control of the one above" },
             { "UI_ListViewSlots", "", "probe fixture (40 render-texture rows in a virtualized list)" },
             // UIL1's bound lists: every row label is a template overwritten by its UIBinding, and the
             // title names the probe for the frame it is shot in.
             { "UI_ListProbe", "", "probe fixture (UIListView bound to a UIDataStore collection)" },
             { "UI_ListViewSlots_ScrollView", "",
               "probe fixture: the same 40 rows in a scroll view — the "
               "renderer-slot negative control" },

             // Ю18's pair. These two are the only scenes in the project that put a canvas over LIT
             // GEOMETRY, and their labels are what a human reads off the frame to see WHICH property is
             // being witnessed; the witness itself keys off a panel's colour and not off any text.
             { "UI_OverScene", "", "probe fixture: canvas over lit geometry, the A of the A->B->A witness" },
             { "UI_OverScene_Hidden", "",
               "probe fixture: the same scene with the canvas not Visible — "
               "the negative control of the one above" },
        };
        return rules;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 8; ++up )
        {
            std::ifstream probe( prefix + "Desert/Common/Source/Common/Core/Constants.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const fs::path& path )
    {
        std::ifstream     in( path, std::ios::binary );
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /// One authored, reader-facing string, and where it came from.
    struct Authored
    {
        std::string Scene;
        std::string Entity;
        std::string Site; // "UIText.Text"
        std::string Text;
    };

    // The four sites, and the set is the argument: see the file comment. `UIInputField.Text` is absent on
    // purpose — it is what a player typed and nothing resolves it.
    struct Site
    {
        const char* Component;
        const char* Key;
        bool        SemicolonList;
    };
    const Site kSites[] = {
         { "UIText", "Text", false },
         { "UIInputField", "Placeholder", false },
         { "UIDropdown", "Options", true },
         { "Text", "Text", false },
    };

    void CollectFromFile( const fs::path& path, std::vector<Authored>& out )
    {
        const auto tree = Common::Json::Parse( ReadFile( path ) );
        ASSERT_TRUE( tree ) << path.string() << " does not parse as JSON";
        const Common::Json::Node root = Common::Json::Root( tree.GetValue() );
        ASSERT_EQ( root.GetKind(), Common::Json::Kind::Object ) << path.string() << " is not a JSON object";

        const auto entities = root.Find( "Entities" );
        if ( !entities.has_value() )
            return;

        entities->ForEachElement(
             [&]( std::size_t, const Common::Json::Node& entity )
             {
                 if ( entity.GetKind() != Common::Json::Kind::Object )
                     return;
                 std::string tag = "Entity";
                 if ( const auto named = entity.Find( "Tag" ); named.has_value() )
                 {
                     if ( const auto text = named->AsString() )
                         tag = text.GetValue();
                 }

                 for ( const Site& site : kSites )
                 {
                     const auto payload = entity.Find( site.Component );
                     if ( !payload.has_value() )
                         continue;
                     const auto named = payload->Find( site.Key );
                     if ( !named.has_value() )
                         continue;
                     const auto textResult = named->AsString();
                     if ( !textResult || textResult.GetValue().empty() )
                         continue;
                     const std::string& text = textResult.GetValue();

                     const std::string where = std::string( site.Component ) + "." + site.Key;
                     if ( !site.SemicolonList )
                     {
                         out.push_back( { path.stem().string(), tag, where, text } );
                         continue;
                     }
                     std::string item;
                     for ( const char c : text + ";" )
                     {
                         if ( c != ';' )
                         {
                             item += c;
                             continue;
                         }
                         if ( !item.empty() )
                             out.push_back( { path.stem().string(), tag, where, item } );
                         item.clear();
                     }
                 }
             } );
    }

    std::vector<Authored> ShippedAuthoredStrings( const std::string& root )
    {
        std::vector<Authored> out;
        for ( const char* tree : { "Editor/Resources/Assets/Scenes", "Editor/Resources/Assets/Prefabs" } )
        {
            const fs::path dir = fs::path( root ) / tree;
            if ( !fs::exists( dir ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                const std::string ext = entry.path().extension().string();
                if ( ext != ".desce" && ext != ".deprefab" )
                    continue;
                CollectFromFile( entry.path(), out );
            }
        }
        return out;
    }

    std::map<std::string, StringTableData> ShippedTables( const std::string& root )
    {
        std::map<std::string, StringTableData> tables;
        const fs::path                         dir = fs::path( root ) / "Editor/Resources/Assets/Localization";
        if ( !fs::exists( dir ) )
            return tables;
        for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != kStringTableExtension )
                continue;
            auto parsed = ParseStringTable( ReadFile( entry.path() ) );
            EXPECT_TRUE( parsed ) << entry.path().string() << ": " << ( parsed ? "" : parsed.GetError() );
            // Keyed `<language>/<table>`: a table is a file per language (STRT 3), and the language is its
            // directory - one this build knows, or the file is refused like the engine refuses it.
            const auto language = StringTableLanguageOf( entry.path() );
            EXPECT_TRUE( language ) << ( language ? "" : language.GetError() );
            if ( parsed && language )
                tables.emplace( std::string( language.GetValue()->Tag ) + "/" + entry.path().stem().string(),
                                parsed.ExtractValue() );
        }
        return tables;
    }
} // namespace

TEST( LocalizedContentCensus, TheShippedTablesAreReadableAndTheSourceLanguageIsComplete )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the repository root was not found from the working directory";

    const auto tables = ShippedTables( root );
    ASSERT_FALSE( tables.empty() ) << "no .destrings under Editor/Resources/Assets/Localization";

    for ( const auto& [name, table] : tables )
    {
        EXPECT_FALSE( table.Entries.empty() ) << name << " has no rows";
        const std::string stem   = name.substr( name.find( '/' ) + 1 );
        const auto        source = tables.find( "en/" + stem );
        ASSERT_NE( source, tables.end() ) << name << " has no source-language file en/" << stem;
        for ( const StringTableEntry& entry : table.Entries )
        {
            // EVERY key must resolve in the SOURCE language. A key that does not is a key that shows as
            // itself on the default screen of the default build, which is the one state nobody would ship
            // on purpose and the one a partial translation produces by accident.
            const auto& rows = source->second.Entries;
            EXPECT_TRUE( std::any_of( rows.begin(), rows.end(),
                                      [&entry]( const StringTableEntry& row ) { return row.Key == entry.Key; } ) )
                 << name << ": key '" << entry.Key << "' has no row in en/" << stem;
            // A translator cannot see the screen. A source row without a note is a row somebody will guess
            // at; the note lives in the source language's file, where translation starts.
            if ( name == source->first )
                EXPECT_TRUE( entry.Comment.has_value() && !entry.Comment->empty() )
                     << name << ": key '" << entry.Key << "' has no Comment for whoever translates it";
        }
    }
}

TEST( LocalizedContentCensus, EveryKeyASceneNamesExists )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const auto            tables = ShippedTables( root );
    std::set<std::string> keys;
    for ( const auto& [name, table] : tables )
        for ( const StringTableEntry& entry : table.Entries )
            keys.insert( entry.Key );

    for ( const Authored& authored : ShippedAuthoredStrings( root ) )
    {
        if ( !IsKeyReference( authored.Text ) )
            continue;
        const std::string key( KeyOf( authored.Text ) );
        EXPECT_NE( keys.count( key ), 0u )
             << authored.Scene << " > " << authored.Entity << " > " << authored.Site << " names key '" << key
             << "', which no shipped string table defines — that label would draw its own key on screen";
    }
}

TEST( LocalizedContentCensus, EveryUntranslatedLiteralIsARegisteredDECISION )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<size_t> used;
    size_t           literals = 0;

    for ( const Authored& authored : ShippedAuthoredStrings( root ) )
    {
        if ( IsKeyReference( authored.Text ) )
            continue;
        ++literals;

        // The narrow row (this scene, this exact string) wins over the whole-scene row, so an
        // otherwise-translated scene cannot be waved through by a blanket entry.
        bool covered = false;
        for ( size_t i = 0; i < LiteralRegister().size(); ++i )
        {
            const LiteralRule& rule = LiteralRegister()[i];
            if ( authored.Scene != rule.Scene )
                continue;
            if ( *rule.Text != '\0' && LiteralOf( authored.Text ) != rule.Text )
                continue;
            used.insert( i );
            covered = true;
            break;
        }

        EXPECT_TRUE( covered ) << "UNTRANSLATABLE LITERAL WITH NO DECISION BEHIND IT:\n  " << authored.Scene
                               << " > " << authored.Entity << " > " << authored.Site << " = \"" << authored.Text
                               << "\"\n"
                               << "Either give it a key ('#some.key', with a row in a .destrings under "
                                  "Editor/Resources/Assets/Localization), or add a row to LiteralRegister() "
                                  "in this file saying why it stays a literal.";
    }

    EXPECT_GT( literals, 0u ) << "the census found no literals at all, which means it found no content";

    // A ROW THAT MATCHES NOTHING IS A STALE DECISION. It is not harmless: it is a hole the next literal
    // with that name falls through, and it is how a register stops describing the tree it guards.
    for ( size_t i = 0; i < LiteralRegister().size(); ++i )
    {
        const LiteralRule& rule = LiteralRegister()[i];
        EXPECT_NE( used.count( i ), 0u )
             << "stale register row: " << rule.Scene << " / \"" << rule.Text << "\" matches nothing in the "
             << "content any more — delete it rather than leaving a hole";
    }
}

TEST( LocalizedContentCensus, EveryShippedTranslationHasAReader )
{
    // THE OTHER DIRECTION, and it is the one nothing else would catch: a key with no reader is a string
    // somebody pays to translate into every language for ever and nobody ever sees. A reader is a scene
    // (`#key` in an authored field) or a script (`loc.text( "key" )` / `loc.plural( "key", n )`).
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<std::string> referenced;
    for ( const Authored& authored : ShippedAuthoredStrings( root ) )
    {
        if ( IsKeyReference( authored.Text ) )
            referenced.insert( std::string( KeyOf( authored.Text ) ) );
    }

    const fs::path scripts = fs::path( root ) / "Editor/Resources/Assets/Scripts";
    if ( fs::exists( scripts ) )
    {
        for ( const auto& entry : fs::recursive_directory_iterator( scripts ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".lua" )
                continue;
            const std::string source = ReadFile( entry.path() );
            // Every quoted string in the file. Deliberately generous: this direction must not produce a
            // FALSE RED, and a key that happens to appear in a Lua string is a key somebody can reach.
            for ( size_t at = source.find( '"' ); at != std::string::npos; at = source.find( '"', at + 1 ) )
            {
                const size_t end = source.find( '"', at + 1 );
                if ( end == std::string::npos )
                    break;
                referenced.insert( source.substr( at + 1, end - at - 1 ) );
                at = end;
            }
        }
    }

    for ( const auto& [name, table] : ShippedTables( root ) )
    {
        for ( const StringTableEntry& entry : table.Entries )
        {
            EXPECT_NE( referenced.count( entry.Key ), 0u )
                 << name << ": key '" << entry.Key << "' is translated and nothing reads it — no scene names '#"
                 << entry.Key << "' and no script names it either";
        }
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// A WORLD LABEL IS DRAWN AT Size x ITS TRANSFORM SCALE, IN CENTIMETRES (PKG2c). The metre -> centimetre corpus
// migration of 2026-08-18 (f12f85ccc) multiplied BOTH the Text's Size (0.8 -> 80) and its entity Scale (1 -> 100),
// so Starter's "Desert Engine" was 80 m tall: from any Starter camera one glyph filled the view and read as a
// solid white quad, in the editor and in every package, while every text test was green. Nothing about the text
// path was wrong; the SIZE was, and only the corpus can say so. The bound is generous (a 10 m tall glyph is
// already a building-sized sign) and exists to catch the hundredfold class, not to police style.
TEST( LocalizedContentCensus, EveryWorldLabelIsDrawnAtAHumanScale )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    constexpr double kTallestGlyphCm = 1000.0;

    std::size_t labels = 0;
    for ( const char* tree : { "Editor/Resources/Assets/Scenes", "Editor/Resources/Assets/Prefabs" } )
    {
        const fs::path dir = fs::path( root ) / tree;
        ASSERT_TRUE( fs::exists( dir ) ) << dir.string();
        for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".desce" && ext != ".deprefab" ) ||
                 entry.path().generic_string().find( "/Autosave/" ) != std::string::npos )
                continue;
            const auto parsed = Common::Json::Parse( ReadFile( entry.path() ) );
            ASSERT_TRUE( parsed ) << entry.path().string();
            const auto entities = Common::Json::Root( parsed.GetValue() ).Find( "Entities" );
            if ( !entities.has_value() )
                continue;
            entities->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& entity )
                 {
                     const auto text = entity.Find( "Text" );
                     if ( !text.has_value() || text->GetKind() != Common::Json::Kind::Object )
                         return;
                     const auto size = text->Find( "Size" );
                     if ( !size.has_value() )
                         return;
                     const auto sizeCm = size->AsNumber();
                     if ( !sizeCm )
                         return;
                     double scale = 1.0;
                     if ( const auto s = entity.Find( "Scale" ); s.has_value() )
                         s->ForEachElement(
                              [&]( std::size_t, const Common::Json::Node& axis )
                              {
                                  if ( const auto v = axis.AsNumber() )
                                      scale = std::max( scale, std::abs( v.GetValue() ) );
                              } );
                     ++labels;
                     const double glyphCm = sizeCm.GetValue() * scale;
                     EXPECT_LE( glyphCm, kTallestGlyphCm )
                          << entry.path().filename().string() << ": a world label is " << glyphCm / 100.0
                          << " m tall (Size " << sizeCm.GetValue() << " x Scale " << scale
                          << ") - Size is already centimetres, so the entity Scale almost certainly carries a "
                             "second metre->centimetre factor";
                 } );
        }
    }
    EXPECT_GE( labels, 5u ) << "the corpus world labels were not found; the census reads nothing";
}
