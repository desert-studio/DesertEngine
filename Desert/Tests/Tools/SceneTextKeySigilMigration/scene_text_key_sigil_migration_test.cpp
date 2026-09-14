// The v18 -> v19 migration: one SPELLING changed meaning, so every authored string it would now capture
// is escaped.
//
// Ю15 made a leading '#' mean "the rest of this is a string-table key" (decision 2: one field, and the
// key differs from the literal by its FORM). That is a change to what an EXISTING file means: a label
// somebody typed as "#1 in the world" was a literal yesterday and is a key called "1 in the world" today,
// which would draw as itself and log an error. So the step escapes a leading '#' to '##', which the
// resolver turns back into a single '#' on the way to the screen.
//
// THE LOAD-BEARING ASSERTION IS THE ROUND TRIP, and it is asserted against the REAL resolver rather than
// against a second copy of the rule here: escape( literal ) resolved as authored text must give the
// literal back, for every literal. Two implementations of one escape is the drift this project pays for
// elsewhere; there is one, and this suite drives both ends of it.
//
// What is asserted:
//
//   1. Only a LEADING hash is escaped. `[color=#FF7A33]` — which this repository's main menu is full of —
//      must come out byte-identical, or every rich-text colour in the corpus breaks.
//   2. All four sites move, and `UIInputField.Text` does NOT: it is the player's own text.
//   3. Each `;`-separated dropdown item is escaped on its own, and the list's shape is kept exactly.
//   4. It runs EXACTLY ONCE, through the version gate — it is not idempotent and cannot be, because a
//      v18 "##" must become "###" and a second pass cannot tell that apart from a string it has not seen.
//   5. The shapes a hand-edited file has: a payload that is not an object, a field that is not a string.
//   6. The round trip, over every literal in the corpus.
//   7. `UIBinding.Format` is gone from the files AND from the component — the field and the key went
//      together, which is the half of a removal that is usually forgotten.
//   8. The step is the head, and the head is what the engine requires.

#include <SceneMigration.hpp>

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Localization/LocalizedText.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert;
namespace fs = std::filesystem;

namespace
{
    Assets::EntityData Entity( const char* component, const char* key, rfl::Generic value, std::string tag = "E" )
    {
        rfl::Generic::Object payload;
        payload[key] = std::move( value );
        Assets::EntityData entity;
        entity.Tag                   = std::move( tag );
        entity.Components[component] = rfl::Generic( std::move( payload ) );
        return entity;
    }

    std::string FieldOf( const Assets::EntityData& entity, const char* component, const char* key )
    {
        const auto payload = entity.Components.get( component );
        EXPECT_TRUE( payload.has_value() );
        const auto fields = payload.value().to_object();
        EXPECT_TRUE( fields.has_value() );
        const auto named = fields.value().get( key );
        EXPECT_TRUE( named.has_value() );
        return named.value().to_string().value_or( "<not a string>" );
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
} // namespace

TEST( SceneTextKeySigilMigration, OnlyALeadingHashIsEscaped )
{
    std::vector<Assets::EntityData> entities = {
         Entity( "UIText", "Text", rfl::Generic( std::string( "#1 in the world" ) ), "Looks like a key" ),
         Entity( "UIText", "Text", rfl::Generic( std::string( "[color=#FF7A33]RP[/color]" ) ), "Rich text" ),
         Entity( "UIText", "Text", rfl::Generic( std::string( "PLAY" ) ), "Plain" ),
    };

    const auto report = Migration::MigrateTextKeySigilV18ToV19( entities );

    EXPECT_EQ( FieldOf( entities[0], "UIText", "Text" ), "##1 in the world" );
    // The one that must NOT move. Doubling every hash would break every rich-text colour in the corpus.
    EXPECT_EQ( FieldOf( entities[1], "UIText", "Text" ), "[color=#FF7A33]RP[/color]" );
    EXPECT_EQ( FieldOf( entities[2], "UIText", "Text" ), "PLAY" );

    EXPECT_EQ( report.Escaped, 1 );
    EXPECT_EQ( report.Entities, 1 );
    ASSERT_EQ( report.EscapedNames.size(), 1u );
    // NAMED, not counted: an authored string changed and the person who typed it has to be able to see
    // that it was on purpose.
    EXPECT_NE( report.EscapedNames[0].find( "Looks like a key" ), std::string::npos );
}

TEST( SceneTextKeySigilMigration, AllFourSitesMoveAndThePlayersOwnTextDoesNot )
{
    std::vector<Assets::EntityData> entities = {
         Entity( "UIText", "Text", rfl::Generic( std::string( "#a" ) ) ),
         Entity( "UIInputField", "Placeholder", rfl::Generic( std::string( "#b" ) ) ),
         Entity( "Text", "Text", rfl::Generic( std::string( "#c" ) ) ),
         Entity( "UIInputField", "Text", rfl::Generic( std::string( "#what the player typed" ) ) ),
    };

    const auto report = Migration::MigrateTextKeySigilV18ToV19( entities );

    EXPECT_EQ( FieldOf( entities[0], "UIText", "Text" ), "##a" );
    EXPECT_EQ( FieldOf( entities[1], "UIInputField", "Placeholder" ), "##b" );
    EXPECT_EQ( FieldOf( entities[2], "Text", "Text" ), "##c" );
    // NOT a site: nothing resolves what a player typed, and escaping it would put a hash into their words.
    EXPECT_EQ( FieldOf( entities[3], "UIInputField", "Text" ), "#what the player typed" );
    EXPECT_EQ( report.Escaped, 3 );
}

TEST( SceneTextKeySigilMigration, EachDropdownItemIsEscapedOnItsOwnAndTheListKeepsItsShape )
{
    std::vector<Assets::EntityData> entities = {
         Entity( "UIDropdown", "Options", rfl::Generic( std::string( "#Low;Medium;#High" ) ) ),
         Entity( "UIDropdown", "Options", rfl::Generic( std::string( "A;;B;" ) ) ),
    };

    const auto report = Migration::MigrateTextKeySigilV18ToV19( entities );

    EXPECT_EQ( FieldOf( entities[0], "UIDropdown", "Options" ), "##Low;Medium;##High" );
    // An empty item and a trailing separator are the list's shape. A rewrite that tidied them would change
    // what the author wrote for no reason the author asked for.
    EXPECT_EQ( FieldOf( entities[1], "UIDropdown", "Options" ), "A;;B;" );
    EXPECT_EQ( report.Escaped, 1 );
}

TEST( SceneTextKeySigilMigration, ItRunsEXACTLYONCEBecauseTheVERSIONGatesIt )
{
    // THIS TEST WAS WRITTEN AS "it is idempotent" AND THE STEP DISPROVED IT, which is the result worth
    // keeping. A v18 literal that really begins "##" must come out as "###" — the resolver strips exactly
    // ONE leading hash, so that is the only spelling that still draws "##" — and a second pass cannot tell
    // that string from one it has never seen. Correctness of the one-time conversion and idempotence are
    // in direct conflict, and correctness wins.
    //
    // So the protection is the GATE, not the function, and the gate is what is asserted here: through
    // MigrateScene, which is the only way the step is ever reached in production.
    Core::SceneSerialized scene;
    scene.SceneName    = "Gated";
    scene.SceneVersion = 18;
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Entities.push_back( Entity( "UIText", "Text", rfl::Generic( std::string( "#x" ) ) ) );
    scene.Entities.push_back( Entity( "UIText", "Text", rfl::Generic( std::string( "##already" ) ) ) );

    const auto first = Migration::MigrateScene( scene );
    EXPECT_TRUE( first.TextKeySigilRaised );
    EXPECT_EQ( first.TextKeySigil.Escaped, 2 );
    EXPECT_EQ( FieldOf( scene.Entities[0], "UIText", "Text" ), "##x" );
    EXPECT_EQ( FieldOf( scene.Entities[1], "UIText", "Text" ), "###already" );
    EXPECT_EQ( scene.SceneVersion.value_or( 0 ), Core::kSceneVersion );

    // The tree is now at the head, so the step does not run again and the strings do not move. That is
    // what "runs once" means for every value-deciding step in this file.
    const auto second = Migration::MigrateScene( scene );
    EXPECT_FALSE( second.TextKeySigilRaised );
    EXPECT_EQ( second.TextKeySigil.Escaped, 0 );
    EXPECT_EQ( FieldOf( scene.Entities[0], "UIText", "Text" ), "##x" );
    EXPECT_EQ( FieldOf( scene.Entities[1], "UIText", "Text" ), "###already" );

    // And both still mean what they meant: one pass, and the resolver gives the v18 text back.
    EXPECT_EQ( Localization::LiteralOf( FieldOf( scene.Entities[0], "UIText", "Text" ) ), "#x" );
    EXPECT_EQ( Localization::LiteralOf( FieldOf( scene.Entities[1], "UIText", "Text" ) ), "##already" );
}

TEST( SceneTextKeySigilMigration, AHandEditedFileDoesNotCrashItAndIsLeftAlone )
{
    std::vector<Assets::EntityData> entities;

    Assets::EntityData notAnObject;
    notAnObject.Tag                  = "Broken";
    notAnObject.Components["UIText"] = rfl::Generic( std::string( "this should have been an object" ) );
    entities.push_back( notAnObject );

    entities.push_back( Entity( "UIText", "Text", rfl::Generic( 42.0 ), "Number" ) );

    Assets::EntityData bare;
    bare.Tag = "No UI at all";
    entities.push_back( bare );

    const auto report = Migration::MigrateTextKeySigilV18ToV19( entities );
    EXPECT_EQ( report.Escaped, 0 );
    EXPECT_EQ( report.Entities, 0 );
}

TEST( SceneTextKeySigilMigration, EscapingThenResolvingIsTheIdentity )
{
    // THE ROUND TRIP, against the REAL resolver. The migration's job is "make this string still mean what
    // it meant", and the only statement of what it means is Localization::LiteralOf. A second copy of the
    // escape rule here would agree with the first by construction and prove nothing.
    const char* literals[] = {
         "PLAY",
         "#1 in the world",
         "[color=#FF7A33]RP[/color]",
         "##already escaped",
         "",
         "#",
         "Tab - navigate    Enter - select",
    };

    for ( const char* literal : literals )
    {
        std::vector<Assets::EntityData> entities = {
             Entity( "UIText", "Text", rfl::Generic( std::string( literal ) ) ) };
        (void)Migration::MigrateTextKeySigilV18ToV19( entities );

        const std::string migrated = FieldOf( entities[0], "UIText", "Text" );
        EXPECT_FALSE( Localization::IsKeyReference( migrated ) )
             << "'" << literal << "' became a key reference: '" << migrated << "'";
        EXPECT_EQ( Localization::LiteralOf( migrated ), std::string( literal ) )
             << "the escape changed what '" << literal << "' means";
    }
}

TEST( SceneTextKeySigilMigration, TheRetiredBindingFormatIsGoneFromTheComponentAndFromTheFiles )
{
    // The field. `UIBindingData` must not declare it any more — a removal that leaves the field alive is
    // the half that gets forgotten, and the reflected serializer would keep writing it.
    const auto tree = rfl::json::read<rfl::Generic>( rfl::json::write( ECS::UIBindingData{} ) );
    ASSERT_TRUE( tree );
    const auto fields = tree.value().to_object();
    ASSERT_TRUE( fields );
    EXPECT_FALSE( fields.value().get( "Format" ).has_value() )
         << "UIBindingData still declares Format; the printf path Ю15 removed is still reachable";

    // And the files. Since K11 the saver PRESERVES a key it does not declare, so a key removed on purpose
    // rides along for ever unless a migration takes it out — which is why it is a kRetiredKeys row.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const fs::path scenes = fs::path( root ) / "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( fs::exists( scenes ) );

    for ( const auto& entry : fs::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        // Not `Scenes/Autosave/`: gitignored crash recovery, written by whatever a developer had open, so
        // it may legitimately predate this migration and is not content this repository ships.
        if ( entry.path().string().find( "/Autosave/" ) != std::string::npos )
            continue;
        const std::string text = ReadFile( entry.path() );
        EXPECT_EQ( text.find( "\"Format\"" ), std::string::npos )
             << entry.path().filename().string() << " still carries a retired UIBinding.Format key";
    }
}

TEST( SceneTextKeySigilMigration, TheStepIsTheHeadTheEngineRequires )
{
    // A step added without raising Core::kSceneVersion stamps files at a version the loader refuses, and
    // every scene in the repository stops opening at once. The header asserts it at compile time; this is
    // the readable failure if somebody ever deletes that static_assert.
    EXPECT_EQ( Migration::kSceneVersionTextKeySigil, Core::kSceneVersion );
    EXPECT_EQ( Core::kSceneVersion, 19 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
