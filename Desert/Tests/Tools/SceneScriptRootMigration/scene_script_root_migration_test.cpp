// The v15 -> v16 script-reference migration: a script slot stops naming its `.lua` by the path the
// editor happened to be standing in and names it by the ROOT-TAGGED KEY every other content reference
// in a scene already uses.
//
// WHAT WAS ACTUALLY WRONG. A packaged game remaps the assets root to <package>/Assets/, so
// `Resources/Assets/Scripts/Examples/MoveAlongX.lua` — the spelling three shipped scenes carried — named
// a directory that does not exist in a package. I8 measured it on a mounted archive: the stored spelling
// gave Exists=0, the same file addressed through the scripts root gave Exists=1. Every other reference in
// a `.desce` is an AssetHandle hashed from `<tag>:<path relative to that root>` and was already immune;
// a script was the one kind of content that referred to itself in a form the move breaks.
//
// THE RELATION this suite is about is asserted end to end somewhere else, and deliberately so:
// Desert/Tests/Editor/PackagedContent (`AScriptReferenceResolvesToTheSameFileLooseAndPackaged`) packs a
// real archive, mounts it in a bare directory and reads the file through the stored reference on both
// sides. It also carries the NEGATIVE control — the pre-migration rooted spelling, which resolves loose
// and fails packaged — because a silence only proves something once the noise is shown.
//
// What is asserted HERE is the conversion itself, which is a pure function over the parsed tree:
//
//   1. The rooted spelling becomes exactly the key Common::AssetHandle::StableKeyForPath would mint, and
//      the JSON field is renamed with the value (`Path` -> `ScriptKey`) because it is no longer a path.
//   2. The root is read out of the STORED PATH, so the relative and absolute spellings of one file —
//      which are what the editor and this tool respectively produce — give ONE key.
//   3. The shapes a hand-edited file has: an empty slot, a value that is not a string, a slot object
//      that is not an object, a payload that is not an object, a `.lua` outside any `Scripts/` folder.
//   4. It is idempotent, it is gated on its own version integer through MigrateScene, and a scene it
//      does not touch comes out byte-identical.
//   5. Corpus: no shipped scene still states the old `Path` key, and every `ScriptKey` in the tree is
//      tagged. That block is what makes the migration a fact about the repository rather than a function
//      nobody ran.

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
    // One entity carrying a "Script" payload whose slots state the given `Path` values, plus a
    // neighbouring key in each slot so the test can prove the rest of the slot survives untouched.
    Assets::EntityData ScriptedWith( const std::vector<rfl::Generic>& paths, const char* tag = "Player" )
    {
        rfl::Generic::Array slots;
        for ( const rfl::Generic& path : paths )
        {
            rfl::Generic::Object slot;
            slot["Path"]  = path;
            slot["Props"] = rfl::Generic( rfl::Generic::Array{} );
            slots.push_back( rfl::Generic( slot ) );
        }

        rfl::Generic::Object payload;
        payload["Scripts"] = rfl::Generic( slots );

        Assets::EntityData entity;
        entity.Tag                  = tag;
        entity.Components["Script"] = rfl::Generic( payload );
        return entity;
    }

    Assets::EntityData ScriptedWith( const std::vector<std::string>& paths, const char* tag = "Player" )
    {
        std::vector<rfl::Generic> generic;
        for ( const std::string& p : paths )
            generic.push_back( rfl::Generic( p ) );
        return ScriptedWith( generic, tag );
    }

    rfl::Generic::Array SlotsOf( const Assets::EntityData& entity )
    {
        const auto payload = entity.Components.get( "Script" );
        if ( !payload.has_value() )
            return {};
        const auto fields = payload.value().to_object();
        if ( !fields.has_value() )
            return {};
        const auto slots = fields.value().get( "Scripts" );
        if ( !slots.has_value() )
            return {};
        return slots.value().to_array().value_or( rfl::Generic::Array{} );
    }

    std::vector<std::string> KeysOfSlot( const rfl::Generic& slot )
    {
        std::vector<std::string> keys;
        const auto               fields = slot.to_object();
        if ( !fields.has_value() )
            return keys;
        for ( const auto& [key, value] : fields.value() )
            keys.push_back( key );
        return keys;
    }

    // The string a slot now states under `ScriptKey`, or nullopt if it states none.
    std::optional<std::string> ScriptKeyOf( const Assets::EntityData& entity, std::size_t slotIndex = 0 )
    {
        const rfl::Generic::Array slots = SlotsOf( entity );
        if ( slotIndex >= slots.size() )
            return std::nullopt;
        const auto fields = slots[slotIndex].to_object();
        if ( !fields.has_value() )
            return std::nullopt;
        const auto found = fields.value().get( "ScriptKey" );
        if ( !found.has_value() )
            return std::nullopt;
        return found.value().to_string().value_or( std::string() );
    }

    bool SlotHasKey( const Assets::EntityData& entity, const std::string& key, std::size_t slotIndex = 0 )
    {
        const rfl::Generic::Array slots = SlotsOf( entity );
        if ( slotIndex >= slots.size() )
            return false;
        for ( const std::string& k : KeysOfSlot( slots[slotIndex] ) )
            if ( k == key )
                return true;
        return false;
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

    // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy and is never migrated. Sweeping
    // it would make this suite pass or fail on whatever the last editor session left behind.
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }

    Desert::Migration::SceneSerialized SceneAt( int sceneVersion, std::vector<Assets::EntityData> entities )
    {
        Desert::Migration::SceneSerialized scene;
        scene.SceneName    = "Fixture";
        scene.SceneVersion = sceneVersion;
        scene.UnitVersion  = Core::kUnitVersion;
        scene.Entities     = std::move( entities );
        return scene;
    }
} // namespace

// ── The conversion ────────────────────────────────────────────────────────────────────────────────

TEST( SceneScriptRootMigration, TheRootedSpellingBecomesATaggedKey )
{
    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<std::string>{ "Resources/Assets/Scripts/Examples/MoveAlongX.lua" } ) };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Slots, 1 );
    EXPECT_EQ( report.Empty, 0 );
    EXPECT_TRUE( report.UnrootedNames.empty() );

    EXPECT_EQ( ScriptKeyOf( entities[0] ).value_or( "<none>" ), "assets:Scripts/Examples/MoveAlongX.lua" );
    EXPECT_FALSE( SlotHasKey( entities[0], "Path" ) ) << "the old field must be gone, not shadowed";
    EXPECT_TRUE( SlotHasKey( entities[0], "Props" ) ) << "the rest of the slot must survive the rename";
}

// THE AGREEMENT THAT MAKES THE MIGRATION CORRECT rather than merely a rename: what this pure, lexical
// function writes has to be, character for character, what the runtime's own key-minting produces for
// the same file. They are two implementations of one quantity — the classic pair this project asserts
// rather than hopes for — and they cannot be the same code, because StableKeyForPath calls
// fs::absolute and reads the live project root while a migration may do neither (DC §4.4).
TEST( SceneScriptRootMigration, TheKeyIsTheOneStableKeyForPathWouldMint )
{
    Common::Constants::Path::ResetToSandbox();

    const std::filesystem::path file = Common::Constants::Path::SCRIPT_PATH / "Examples" / "MoveAlongX.lua";

    std::vector<Assets::EntityData> entities{ ScriptedWith( std::vector<std::string>{ file.generic_string() } ) };
    Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( ScriptKeyOf( entities[0] ).value_or( "<none>" ), Common::AssetHandle::StableKeyForPath( file ) );
}

// THE TAG THIS STEP WRITES IS ONE THE READER RECOGNISES, asserted rather than assumed. The migration
// composes its prefix from AssetHandle::AssetsTag(), which finds the assets row by that root's own
// address; if the row ever stopped being there the lookup would return an empty tag, the step would
// write a bare ":Scripts/x.lua", and IsProjectRelativeKey — the function the rest of the engine asks
// "does this name a place inside the project" — would say no. That is a silent wrong answer with a
// successful-looking migration in front of it (§1.4), so it gets its own line here.
TEST( SceneScriptRootMigration, TheTagTheStepWritesIsOneTheReaderKnows )
{
    ASSERT_FALSE( Common::AssetHandle::AssetsTag().empty() )
         << "the assets row vanished from AssetHandle::ContentRoots(); every key this step writes would "
            "be untagged";

    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<std::string>{ "Resources/Assets/Scripts/Examples/MoveAlongX.lua" } ) };
    Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_TRUE( Common::AssetHandle::IsProjectRelativeKey( ScriptKeyOf( entities[0] ).value_or( "" ) ) );
}

// The editor writes `Resources/Assets/Scripts/...` (its working directory is `Editor/`), the migrator is
// run from the repository root and a hand-edited file may carry an absolute path. All three name one
// file and must produce ONE key — which is why the root is derived from the stored path rather than
// from a root the function is handed. Handing it `Editor/Resources/Assets` would find no match in the
// first spelling at all.
TEST( SceneScriptRootMigration, EverySpellingOfOneFileGivesOneKey )
{
    std::vector<Assets::EntityData> entities{ ScriptedWith( std::vector<std::string>{
         "Resources/Assets/Scripts/Examples/MoveAlongX.lua",
         "Editor/Resources/Assets/Scripts/Examples/MoveAlongX.lua",
         "/Users/somebody/Proj/Editor/Resources/Assets/Scripts/Examples/MoveAlongX.lua",
         "./Resources/Assets/Scripts/Examples/MoveAlongX.lua",
    } ) };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Slots, 4 );
    for ( std::size_t i = 0; i < 4; ++i )
        EXPECT_EQ( ScriptKeyOf( entities[0], i ).value_or( "<none>" ), "assets:Scripts/Examples/MoveAlongX.lua" )
             << "slot " << i;
}

// A nested scripts folder must not swallow the outer one: the LAST occurrence wins, which is the same
// rule Constants::Path::RootForContentPath states for every other content row.
TEST( SceneScriptRootMigration, TheLastScriptsFolderIsTheRoot )
{
    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<std::string>{ "Scripts/Proj/Resources/Assets/Scripts/AI/Brain.lua" } ) };

    Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( ScriptKeyOf( entities[0] ).value_or( "<none>" ), "assets:Scripts/AI/Brain.lua" );
}

// ── The shapes a hand-edited file has ─────────────────────────────────────────────────────────────

// An empty slot names nothing and must stay naming nothing. A bare "assets:" would make every unfilled
// slot in the editor start trying to load the assets root itself on Play — ScriptSystem decides whether
// a slot runs at all by testing the reference for emptiness.
TEST( SceneScriptRootMigration, AnEmptySlotStaysEmptyRatherThanBecomingABareTag )
{
    std::vector<Assets::EntityData> entities{ ScriptedWith( std::vector<std::string>{ "" } ) };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Slots, 1 );
    EXPECT_EQ( report.Empty, 1 );
    EXPECT_EQ( ScriptKeyOf( entities[0] ).value_or( "<none>" ), "" );
    EXPECT_FALSE( SlotHasKey( entities[0], "Path" ) );
}

// A `.lua` that is not under a `Scripts/` folder has no census row to measure against. It is carried
// across unchanged — PathForStableKey hands an untagged string back verbatim, so the slot keeps exactly
// the behaviour it had — and it is NAMED, because "exactly the behaviour it had" includes not resolving
// in a packaged game. A guess would be the silent substitution §1.4 forbids.
TEST( SceneScriptRootMigration, AScriptOutsideAnyScriptsFolderIsCarriedAndNamed )
{
    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<std::string>{ "Resources/Assets/AI/Brain.lua" }, "Wanderer" ) };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Slots, 0 ) << "nothing was root-tagged";
    EXPECT_EQ( report.Entities, 1 ) << "the field was still renamed, so the entity was touched";
    ASSERT_EQ( report.UnrootedNames.size(), 1u );
    EXPECT_NE( report.UnrootedNames[0].find( "Wanderer" ), std::string::npos ) << report.UnrootedNames[0];
    EXPECT_NE( report.UnrootedNames[0].find( "Resources/Assets/AI/Brain.lua" ), std::string::npos )
         << report.UnrootedNames[0];

    EXPECT_EQ( ScriptKeyOf( entities[0] ).value_or( "<none>" ), "Resources/Assets/AI/Brain.lua" );
    EXPECT_FALSE( SlotHasKey( entities[0], "Path" ) );
}

TEST( SceneScriptRootMigration, APathThatIsNotAStringIsCarriedAndNamed )
{
    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<rfl::Generic>{ rfl::Generic( 7 ) }, "Broken" ) };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Slots, 0 );
    ASSERT_EQ( report.UnrootedNames.size(), 1u );
    EXPECT_NE( report.UnrootedNames[0].find( "Broken" ), std::string::npos ) << report.UnrootedNames[0];
    EXPECT_TRUE( SlotHasKey( entities[0], "ScriptKey" ) );
    EXPECT_FALSE( SlotHasKey( entities[0], "Path" ) );
}

TEST( SceneScriptRootMigration, ASlotThatIsNotAnObjectIsLeftAlone )
{
    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<std::string>{ "Resources/Assets/Scripts/A.lua" } ) };

    // Replace the whole slot list with one non-object row, keeping the payload shape.
    rfl::Generic::Object payload;
    payload["Scripts"]               = rfl::Generic( rfl::Generic::Array{ rfl::Generic( "not-an-object" ) } );
    entities[0].Components["Script"] = rfl::Generic( payload );

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Slots, 0 );
    EXPECT_EQ( report.Entities, 0 ) << "nothing moved, so the payload must be left byte-identical";
    ASSERT_EQ( SlotsOf( entities[0] ).size(), 1u );
    EXPECT_EQ( SlotsOf( entities[0] )[0].to_string().value_or( "" ), "not-an-object" );
}

TEST( SceneScriptRootMigration, APayloadThatIsNotAnObjectIsLeftAlone )
{
    Assets::EntityData entity;
    entity.Tag                  = "Odd";
    entity.Components["Script"] = rfl::Generic( std::string( "nonsense" ) );
    std::vector<Assets::EntityData> entities{ entity };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( entities[0].Components.get( "Script" ).value().to_string().value_or( "" ), "nonsense" );
}

TEST( SceneScriptRootMigration, AnEntityWithNoScriptComponentIsUntouched )
{
    Assets::EntityData entity;
    entity.Tag = "Rock";
    std::vector<Assets::EntityData> entities{ entity };

    const auto report = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Slots, 0 );
    EXPECT_FALSE( entities[0].Components.get( "Script" ).has_value() );
}

// ── Idempotence and the version gate ──────────────────────────────────────────────────────────────

TEST( SceneScriptRootMigration, ASecondRunChangesNothing )
{
    std::vector<Assets::EntityData> entities{
         ScriptedWith( std::vector<std::string>{ "Resources/Assets/Scripts/Examples/MoveAlongX.lua" } ) };

    Migration::MigrateScriptRootV15ToV16( entities );
    const std::string once = rfl::json::write( entities );

    const auto again = Migration::MigrateScriptRootV15ToV16( entities );

    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( again.Slots, 0 );
    EXPECT_EQ( rfl::json::write( entities ), once );
}

TEST( SceneScriptRootMigration, MigrateSceneRunsItForAV15FileAndStampsTheHead )
{
    Desert::Migration::SceneSerialized scene = SceneAt(
         Migration::kSceneVersionMachineQuality,
         { ScriptedWith( std::vector<std::string>{ "Resources/Assets/Scripts/Examples/MoveAlongX.lua" } ) } );

    const auto report = Migration::MigrateScene( scene, "Resources/Assets" );

    EXPECT_TRUE( report.ScriptRootRaised );
    EXPECT_EQ( report.ScriptRoot.Slots, 1 );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Core::kSceneVersion );
    EXPECT_EQ( ScriptKeyOf( scene.Entities[0] ).value_or( "<none>" ), "assets:Scripts/Examples/MoveAlongX.lua" );
}

// THE GATE IS ON THE STEP'S OWN NUMBER, and this is the half that proves it fires rather than merely
// being written down. K3 found the mirror of this defect one merge earlier: the retirement pass was
// gated on a step number instead of the head, so rows added later never ran on a corpus already at that
// number and the tool reported "already up to date" over files it had not converted.
TEST( SceneScriptRootMigration, AFileAlreadyAtTheHeadIsNotRunAgain )
{
    Desert::Migration::SceneSerialized scene = SceneAt(
         Core::kSceneVersion,
         { ScriptedWith( std::vector<std::string>{ "Resources/Assets/Scripts/Examples/MoveAlongX.lua" } ) } );

    const auto report = Migration::MigrateScene( scene, "Resources/Assets" );

    EXPECT_FALSE( report.ScriptRootRaised );
    EXPECT_FALSE( report.Changed() );
    EXPECT_EQ( ScriptKeyOf( scene.Entities[0] ).value_or( "<none>" ), "<none>" )
         << "a file at the head must not be re-spelled by this step";
}

// ── This step's own number ────────────────────────────────────────────────────────────────────────

// The HEAD assertion this suite held for one merge has moved on to
// Desert/Tests/Tools/SceneServiceAssetRootMigration, which owns v17 — it travels with the newest step,
// because that is the only suite that can hold it without going red the day the next one lands. What
// stays is this step's own generation and its place above its predecessor: facts about the script
// migration, and they do not move.
TEST( SceneScriptRootMigration, ThisStepSitsAboveItsPredecessor )
{
    EXPECT_EQ( 16, Migration::kSceneVersionScriptRoot );
    EXPECT_GT( Migration::kSceneVersionScriptRoot, Migration::kSceneVersionMachineQuality );
    EXPECT_LE( Migration::kSceneVersionScriptRoot, Core::kSceneVersion )
         << "a step cannot sit above the head the loader requires";
}

// ── The corpus ────────────────────────────────────────────────────────────────────────────────────

TEST( SceneScriptRootMigrationCorpus, NoShippedSceneStillStatesTheOldPathKey )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

    const std::filesystem::path scenes = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string();

    int slots = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        std::ifstream     in( entry.path(), std::ios::binary );
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto parsed = rfl::json::read<Desert::Migration::SceneSerialized>( buffer.str() );
        ASSERT_TRUE( parsed ) << entry.path().string() << " did not parse";

        for ( const auto& entity : parsed.value().Entities )
        {
            for ( const rfl::Generic& slot : SlotsOf( entity ) )
            {
                const auto fields = slot.to_object();
                if ( !fields.has_value() )
                    continue;

                ++slots;
                EXPECT_FALSE( fields.value().get( "Path" ).has_value() )
                     << entry.path().string() << " still states a Script slot Path — run Tools/SceneMigrator";

                const auto key = fields.value().get( "ScriptKey" );
                ASSERT_TRUE( key.has_value() ) << entry.path().string()
                                               << " has a script slot with no "
                                                  "ScriptKey at all";
                const std::string text = key.value().to_string().value_or( std::string() );
                if ( text.empty() )
                    continue; // an unfilled slot names nothing, which is a legitimate value
                EXPECT_TRUE( Common::AssetHandle::IsProjectRelativeKey( text ) )
                     << entry.path().string() << " names the script '" << text
                     << "', which carries no content-root tag and so does not survive packaging";
            }
        }
    }

    EXPECT_GT( slots, 0 ) << "no scene carries a script slot at all - this test would pass vacuously";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
