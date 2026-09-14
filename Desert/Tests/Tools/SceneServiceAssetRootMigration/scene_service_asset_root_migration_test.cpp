// The v16 -> v17 service-asset migration: a reference to a FONT, a VECTOR ICON or a VIDEO stops being
// the path its service's registry happened to hold and becomes the root-tagged key every other content
// reference in a `.desce` already is.
//
// WHAT WAS WRONG, AND WHY NOBODY HAD SEEN IT. These three are not AssetManager assets: FontService,
// IconService and VideoService each own a handle<->path registry keyed on AssetHandle::FromCookedPath,
// so a scene could not store a handle (nothing would resolve it before the service has scanned) and
// stored the path instead. A packaged game remaps the assets root to <package>/Assets/, so a path
// written from the development tree names a directory that does not exist there — the same sentence I9
// removed from the script slot one merge earlier. It survived because every value this repository ships
// names the ENGINE trees Resources/Fonts and Resources/Icons, which the packager stores under their own
// dev-time relative paths and which SetProjectRoot never remaps. Measured before this step: 5 FontPath,
// 8 Icon, 0 Font, 0 Video, and all 13 engine-rooted. Both scan roots also accept the PROJECT'S OWN
// assets tree (Runtime/Services/ServiceScanRoots.hpp), so a `.ttf` or `.svg` dropped in from there took
// the broken route.
//
// THE RELATION is asserted end to end where I9's is, and by the same instrument:
// Desert/Tests/Editor/PackagedContent packs a real archive, mounts it in a bare directory whose assets
// root deliberately differs from the dev one, and reads the file back through the stored reference on
// both sides — for a font and for an icon, each with the pre-migration spelling as its negative control.
//
// What is asserted HERE is the conversion, which is a pure function over the parsed tree:
//
//   1. Both roots are recognised and the LONGEST match wins — which is load-bearing rather than tidy,
//      because in the sandbox layout `Resources/Assets/` is nested inside `Resources/`.
//   2. The key is character for character the one AssetHandle::StableKeyForPath mints for the same file.
//   3. All FOUR sites move, including the one that arrives by the other route (`Text.FontPath`, whose
//      JSON key is renamed with its value because it is no longer a path).
//   4. The shapes a hand-edited file has, and idempotence, and the version gate.
//   5. Corpus: no shipped scene states an old key, and every reference in the tree is tagged.

#include <SceneMigration.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    // The sandbox assets root, which is what the editor resolves against with no project open and what
    // every fixture below measures against. Spelled through the census, not as a literal.
    std::filesystem::path SandboxAssetsRoot()
    {
        Common::Constants::Path::ResetToSandbox();
        return Common::Constants::Path::ASSETS_PATH;
    }

    // One entity carrying `component` with `key` set to `value`, plus a neighbouring field so the test
    // can prove the rest of the payload survives the rename untouched.
    Assets::EntityData EntityWith( const char* component, const char* key, const rfl::Generic& value,
                                   const char* tag = "Label" )
    {
        rfl::Generic::Object payload;
        payload["Color"] = rfl::Generic( rfl::Generic::Array{ rfl::Generic( 1.0 ) } );
        payload[key]     = value;
        payload["Size"]  = rfl::Generic( 1.0 );

        Assets::EntityData entity;
        entity.Tag                   = tag;
        entity.Components[component] = rfl::Generic( payload );
        return entity;
    }

    Assets::EntityData EntityWith( const char* component, const char* key, const std::string& value,
                                   const char* tag = "Label" )
    {
        return EntityWith( component, key, rfl::Generic( value ), tag );
    }

    rfl::Generic::Object PayloadOf( const Assets::EntityData& entity, const char* component )
    {
        const auto found = entity.Components.get( component );
        if ( !found.has_value() )
            return {};
        return found.value().to_object().value_or( rfl::Generic::Object{} );
    }

    std::optional<std::string> StringAt( const rfl::Generic::Object& object, const std::string& key )
    {
        const auto found = object.get( key );
        if ( !found.has_value() )
            return std::nullopt;
        return found.value().to_string().value_or( std::string() );
    }

    bool HasKey( const rfl::Generic::Object& object, const std::string& key )
    {
        for ( const auto& [k, v] : object )
            if ( k == key )
                return true;
        return false;
    }

    std::vector<std::string> KeysOf( const rfl::Generic::Object& object )
    {
        std::vector<std::string> keys;
        for ( const auto& [k, v] : object )
            keys.push_back( k );
        return keys;
    }

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

    // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy and is never migrated.
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }

    Core::SceneSerialized SceneAt( int sceneVersion, std::vector<Assets::EntityData> entities )
    {
        Core::SceneSerialized scene;
        scene.SceneName    = "Fixture";
        scene.SceneVersion = sceneVersion;
        scene.UnitVersion  = Core::kUnitVersion;
        scene.Entities     = std::move( entities );
        return scene;
    }
} // namespace

// ── The two roots ─────────────────────────────────────────────────────────────────────────────────

TEST( SceneServiceAssetRootMigration, AnEngineResourceGetsTheEngineTag )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/cart.svg" ) ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Refs, 1 );
    EXPECT_TRUE( report.UnrootedNames.empty() );
    EXPECT_EQ( StringAt( PayloadOf( entities[0], "UIIcon" ), "Icon" ).value_or( "<none>" ),
               "engine:Icons/cart.svg" );
}

// THE LOAD-BEARING CASE. In the sandbox layout `Resources/Assets/` is NESTED INSIDE `Resources/`, so a
// project asset matches the ENGINE root too — one component deep instead of two. Longest match wins, and
// a step that took the first match would tag every project font as an engine resource: it would resolve
// in the dev tree (where both roots are relative to the same directory) and name nothing in a package.
TEST( SceneServiceAssetRootMigration, AProjectAssetBeatsTheEngineRootItIsNestedIn )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "UIText", "Font", std::string( "Resources/Assets/Fonts/Custom.ttf" ) ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Refs, 1 );
    EXPECT_EQ( StringAt( PayloadOf( entities[0], "UIText" ), "Font" ).value_or( "<none>" ),
               "assets:Fonts/Custom.ttf" );
}

// The agreement that makes the step correct rather than merely a rewrite: what this pure, lexical
// function writes must be, character for character, what the runtime's own key-minting produces for the
// same file. They cannot be the same code — StableKeyForPath calls fs::absolute and reads the live
// project root, and a migration may do neither (DC §4.4) — so the agreement is asserted.
TEST( SceneServiceAssetRootMigration, TheKeyIsTheOneStableKeyForPathWouldMint )
{
    const std::filesystem::path assets = SandboxAssetsRoot();

    struct Case
    {
        const char*           Component;
        const char*           Key;
        std::filesystem::path File;
    };
    const Case cases[] = {
         { "UIIcon", "Icon", Common::Constants::Path::ICONS_PATH / "gear.svg" },
         { "UIText", "Font", Common::Constants::Path::FONTS_PATH / "Roboto-Regular.ttf" },
         { "UIPanel", "Video", assets / "Videos" / "Intro.mpg" },
    };

    for ( const Case& c : cases )
    {
        std::vector<Assets::EntityData> entities{ EntityWith( c.Component, c.Key, c.File.generic_string() ) };
        Migration::MigrateServiceAssetRootV16ToV17( entities, assets );

        EXPECT_EQ( StringAt( PayloadOf( entities[0], c.Component ), c.Key ).value_or( "<none>" ),
                   Common::AssetHandle::StableKeyForPath( c.File ) )
             << c.Component << "." << c.Key;
    }
}

// The editor writes paths from `Editor/` and this tool is run from the repository root, so the same root
// is spelled two ways and neither contains the other's full sequence. One file, one key.
TEST( SceneServiceAssetRootMigration, BothSpellingsOfOneRootGiveOneKey )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/gear.svg" ), "FromTheEditor" ),
         EntityWith( "UIIcon", "Icon", std::string( "Editor/Resources/Icons/gear.svg" ), "FromTheTool" ),
    };

    Migration::MigrateServiceAssetRootV16ToV17( entities, "Editor/Resources/Assets" );

    EXPECT_EQ( StringAt( PayloadOf( entities[0], "UIIcon" ), "Icon" ).value_or( "<a>" ), "engine:Icons/gear.svg" );
    EXPECT_EQ( StringAt( PayloadOf( entities[1], "UIIcon" ), "Icon" ).value_or( "<b>" ), "engine:Icons/gear.svg" );
}

// ── All four sites, including the one that arrives by the other route ─────────────────────────────

// `Text.FontPath` reaches the file through a hand-written serializer while the other three are reflected
// AssetHandle slots. A step that knew only the reflected ones would leave every world-space label behind
// — both ends individually right, the relation between them missing, which is this project's most
// repeated defect. The JSON key is renamed WITH the value because the value is no longer a path.
TEST( SceneServiceAssetRootMigration, TheWorldSpaceLabelsFontPathIsRenamedWithItsValue )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "Text", "FontPath", std::string( "Resources/Fonts/Roboto-Regular.ttf" ) ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Refs, 1 );

    const rfl::Generic::Object payload = PayloadOf( entities[0], "Text" );
    EXPECT_FALSE( HasKey( payload, "FontPath" ) ) << "the old key must be gone, not shadowed";
    EXPECT_EQ( StringAt( payload, "Font" ).value_or( "<none>" ), "engine:Fonts/Roboto-Regular.ttf" );
    EXPECT_TRUE( HasKey( payload, "Color" ) ) << "the rest of the payload must survive the rename";
    EXPECT_TRUE( HasKey( payload, "Size" ) );
}

TEST( SceneServiceAssetRootMigration, EverySiteMovesInOnePass )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "Text", "FontPath", std::string( "Resources/Fonts/A.ttf" ), "WorldLabel" ),
         EntityWith( "UIText", "Font", std::string( "Resources/Fonts/B.ttf" ), "UILabel" ),
         EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/c.svg" ), "Glyph" ),
         EntityWith( "UIPanel", "Video", std::string( "Resources/Assets/Videos/d.mpg" ), "Screen" ),
    };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 4 );
    EXPECT_EQ( report.Refs, 4 );
    EXPECT_EQ( StringAt( PayloadOf( entities[0], "Text" ), "Font" ).value_or( "" ), "engine:Fonts/A.ttf" );
    EXPECT_EQ( StringAt( PayloadOf( entities[1], "UIText" ), "Font" ).value_or( "" ), "engine:Fonts/B.ttf" );
    EXPECT_EQ( StringAt( PayloadOf( entities[2], "UIIcon" ), "Icon" ).value_or( "" ), "engine:Icons/c.svg" );
    EXPECT_EQ( StringAt( PayloadOf( entities[3], "UIPanel" ), "Video" ).value_or( "" ), "assets:Videos/d.mpg" );
}

// One entity can carry several of the four at once (a panel with a video and a text child is two
// entities, but a hand-built file may put more than one payload on one). It is counted once.
TEST( SceneServiceAssetRootMigration, AnEntityCarryingTwoSitesIsCountedOnce )
{
    Assets::EntityData   entity = EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/a.svg" ), "Both" );
    rfl::Generic::Object panel;
    panel["Video"]               = rfl::Generic( std::string( "Resources/Assets/Videos/v.mpg" ) );
    entity.Components["UIPanel"] = rfl::Generic( panel );

    std::vector<Assets::EntityData> entities{ entity };
    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Refs, 2 );
}

// ── The shapes a hand-edited file has ─────────────────────────────────────────────────────────────

// An empty slot names nothing and must stay naming nothing. A bare "engine:" would make every unfilled
// font slot try to register the resource root itself — the read side turns any non-empty string into a
// registration attempt. At a site whose key does not change there is nothing to do at all, so the tree
// is left byte-identical; at the RENAMED site the key still moves and the empty value travels with it.
TEST( SceneServiceAssetRootMigration, AnEmptySlotStaysEmptyRatherThanBecomingABareTag )
{
    std::vector<Assets::EntityData> entities{ EntityWith( "UIText", "Font", std::string( "" ) ),
                                              EntityWith( "Text", "FontPath", std::string( "" ), "World" ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 1 ) << "only the renamed site had anything to do";
    EXPECT_EQ( report.Refs, 0 ) << "nothing was tagged - an empty slot is not a reference";
    EXPECT_EQ( report.Empty, 1 );
    EXPECT_EQ( StringAt( PayloadOf( entities[0], "UIText" ), "Font" ).value_or( "<none>" ), "" );
    EXPECT_FALSE( HasKey( PayloadOf( entities[1], "Text" ), "FontPath" ) );
    EXPECT_EQ( StringAt( PayloadOf( entities[1], "Text" ), "Font" ).value_or( "<none>" ), "" );
}

// A value that is ALREADY a key is what an earlier pass left, and it must not be re-spelled or even
// rewritten: this is the whole of the step's idempotence on the three sites whose key name is unchanged.
TEST( SceneServiceAssetRootMigration, AnAlreadyTaggedValueIsNotTouchedAtAll )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "UIIcon", "Icon", std::string( "engine:Icons/gear.svg" ) ) };
    const std::string before = rfl::json::write( entities );

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Refs, 0 );
    EXPECT_TRUE( report.UnrootedNames.empty() );
    EXPECT_EQ( rfl::json::write( entities ), before );
}

// Under neither root: carried across unchanged — PathForStableKey hands an untagged string back verbatim,
// so the slot keeps exactly the behaviour it had — and NAMED, because "exactly the behaviour it had"
// includes not resolving in a packaged game. A guess would be the silent substitution §1.4 forbids.
TEST( SceneServiceAssetRootMigration, AReferenceUnderNeitherRootIsCarriedAndNamed )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "UIIcon", "Icon", std::string( "/Users/somebody/Downloads/free.svg" ), "Stray" ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Refs, 0 );
    EXPECT_EQ( report.Entities, 0 ) << "nothing could be re-spelled, so the tree is left byte-identical";
    ASSERT_EQ( report.UnrootedNames.size(), 1u );
    EXPECT_NE( report.UnrootedNames[0].find( "Stray" ), std::string::npos ) << report.UnrootedNames[0];
    EXPECT_EQ( StringAt( PayloadOf( entities[0], "UIIcon" ), "Icon" ).value_or( "<none>" ),
               "/Users/somebody/Downloads/free.svg" );
}

// The whole path being the root names a DIRECTORY, not a file, and there is no reference to make of it.
TEST( SceneServiceAssetRootMigration, AValueThatIsExactlyARootIsNotAReference )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "UIIcon", "Icon", std::string( "Resources" ), "JustTheRoot" ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Refs, 0 );
    EXPECT_EQ( report.Entities, 0 );
    ASSERT_EQ( report.UnrootedNames.size(), 1u );
    EXPECT_EQ( StringAt( PayloadOf( entities[0], "UIIcon" ), "Icon" ).value_or( "<none>" ), "Resources" );
}

TEST( SceneServiceAssetRootMigration, AValueThatIsNotAStringIsCarriedAndNamed )
{
    std::vector<Assets::EntityData> entities{ EntityWith( "UIText", "Font", rfl::Generic( 7 ), "Broken" ) };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Refs, 0 );
    EXPECT_EQ( report.Entities, 0 ) << "there is no rename to perform, so the tree is left alone";
    ASSERT_EQ( report.UnrootedNames.size(), 1u );
    EXPECT_NE( report.UnrootedNames[0].find( "Broken" ), std::string::npos ) << report.UnrootedNames[0];
    EXPECT_TRUE( HasKey( PayloadOf( entities[0], "UIText" ), "Font" ) );
}

TEST( SceneServiceAssetRootMigration, APayloadThatIsNotAnObjectIsLeftAlone )
{
    Assets::EntityData entity;
    entity.Tag                  = "Odd";
    entity.Components["UIIcon"] = rfl::Generic( std::string( "nonsense" ) );
    std::vector<Assets::EntityData> entities{ entity };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( entities[0].Components.get( "UIIcon" ).value().to_string().value_or( "" ), "nonsense" );
}

// A UI payload that simply never named one of the four is not rewritten at all — which is what keeps a
// second run byte-identical for the ninety per cent of the corpus that has no such reference.
TEST( SceneServiceAssetRootMigration, APayloadWithNoSuchKeyIsUntouched )
{
    Assets::EntityData entity;
    entity.Tag = "Plain";
    rfl::Generic::Object payload;
    payload["Color"]            = rfl::Generic( 1.0 );
    entity.Components["UIIcon"] = rfl::Generic( payload );
    std::vector<Assets::EntityData> entities{ entity };

    const auto report = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( KeysOf( PayloadOf( entities[0], "UIIcon" ) ), std::vector<std::string>{ "Color" } );
}

// ── Idempotence and the version gate ──────────────────────────────────────────────────────────────

TEST( SceneServiceAssetRootMigration, ASecondRunChangesNothing )
{
    std::vector<Assets::EntityData> entities{
         EntityWith( "Text", "FontPath", std::string( "Resources/Fonts/Roboto-Regular.ttf" ) ),
         EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/gear.svg" ), "Glyph" ),
    };

    Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );
    const std::string once = rfl::json::write( entities );

    const auto again = Migration::MigrateServiceAssetRootV16ToV17( entities, SandboxAssetsRoot() );

    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( again.Refs, 0 );
    EXPECT_EQ( rfl::json::write( entities ), once );
}

TEST( SceneServiceAssetRootMigration, MigrateSceneRunsItForAV16FileAndStampsTheHead )
{
    Core::SceneSerialized scene =
         SceneAt( Migration::kSceneVersionScriptRoot,
                  { EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/gear.svg" ) ) } );

    const auto report = Migration::MigrateScene( scene, SandboxAssetsRoot() );

    EXPECT_TRUE( report.ServiceAssetRootRaised );
    EXPECT_EQ( report.ServiceAssetRoot.Refs, 1 );
    EXPECT_EQ( scene.SceneVersion.value_or( 0 ), Core::kSceneVersion );
    EXPECT_EQ( StringAt( PayloadOf( scene.Entities[0], "UIIcon" ), "Icon" ).value_or( "<none>" ),
               "engine:Icons/gear.svg" );
}

// The gate is on this step's own number, and this is the half that proves it FIRES rather than merely
// being written down — the trap K3 caught one programme earlier, where a pass gated on a step number
// never ran on a corpus already stamped at it and the tool reported every file up to date.
TEST( SceneServiceAssetRootMigration, AFileAlreadyAtTheHeadIsNotRunAgain )
{
    Core::SceneSerialized scene = SceneAt(
         Core::kSceneVersion, { EntityWith( "UIIcon", "Icon", std::string( "Resources/Icons/gear.svg" ) ) } );

    const auto report = Migration::MigrateScene( scene, SandboxAssetsRoot() );

    EXPECT_FALSE( report.ServiceAssetRootRaised );
    EXPECT_FALSE( report.Changed() );
    EXPECT_EQ( StringAt( PayloadOf( scene.Entities[0], "UIIcon" ), "Icon" ).value_or( "<none>" ),
               "Resources/Icons/gear.svg" )
         << "a file at the head must not be re-spelled by this step";
}

// ── This step's place in the chain ────────────────────────────────────────────────────────────────

// THE HEAD ASSERTION HAS MOVED ON, and this is what it leaves behind. It was here until Г25 added the
// v17 -> v18 grass-generation step; it now lives in SceneGrassGenerationMigration, which came by it from
// here, as this suite came by it from SceneScriptRootMigration, that one from SceneRetiredKeysMigration
// and that one from SceneDebugViewMigration.
//
// What stays is this step's own identity: its number, and the fact that it sits above the step before
// it. Those are facts about THIS migration and are true for ever; "is the head" was never a fact about
// this step at all, which is precisely why the assertion travels and says so when it goes red.
TEST( SceneServiceAssetRootMigration, ThisStepSitsAboveItsPredecessorAndBelowTheHead )
{
    EXPECT_EQ( 17, Migration::kSceneVersionServiceAssetRoot );
    EXPECT_GT( Migration::kSceneVersionServiceAssetRoot, Migration::kSceneVersionScriptRoot );
    EXPECT_LE( Migration::kSceneVersionServiceAssetRoot, Core::kSceneVersion )
         << "this step is stamped above the generation the engine requires - the tool would write files "
            "the loader refuses";
}

// ── The corpus ────────────────────────────────────────────────────────────────────────────────────

TEST( SceneServiceAssetRootMigrationCorpus, NoShippedSceneStatesAnUntaggedFontIconOrVideo )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

    const std::filesystem::path scenes = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string();

    struct Site
    {
        const char* Component;
        const char* Key;
    };
    static constexpr Site kSites[] = {
         { "Text", "Font" }, { "UIText", "Font" }, { "UIIcon", "Icon" }, { "UIPanel", "Video" } };

    int refs = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        std::ifstream     in( entry.path(), std::ios::binary );
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = rfl::json::read<Core::SceneSerialized>( buffer.str() );
        ASSERT_TRUE( parsed ) << entry.path().string() << " did not parse";

        for ( const auto& entity : parsed.value().Entities )
        {
            // The RENAMED key must be gone from every file, not merely absent from the ones this test
            // remembered to look at.
            const auto text = entity.Components.get( "Text" );
            if ( text.has_value() )
            {
                const auto fields = text.value().to_object();
                if ( fields.has_value() )
                    EXPECT_FALSE( fields.value().get( "FontPath" ).has_value() )
                         << entry.path().string() << " still states Text.FontPath — run Tools/SceneMigrator";
            }

            for ( const Site& site : kSites )
            {
                const auto payload = entity.Components.get( site.Component );
                if ( !payload.has_value() )
                    continue;
                const auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                    continue;
                const auto named = fields.value().get( site.Key );
                if ( !named.has_value() )
                    continue;

                const std::string value = named.value().to_string().value_or( std::string() );
                if ( value.empty() )
                    continue; // an unfilled slot names nothing, which is a legitimate value

                ++refs;
                EXPECT_TRUE( Common::AssetHandle::IsProjectRelativeKey( value ) )
                     << entry.path().string() << " names " << site.Component << "." << site.Key << " as '" << value
                     << "', which carries no content-root tag and so does not survive packaging";
            }
        }
    }

    EXPECT_GT( refs, 0 ) << "no scene names a font, an icon or a video at all - this test would pass "
                            "vacuously";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
