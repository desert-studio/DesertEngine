// The v7 -> v8 material-path migration, and the property it exists to make permanent.
//
// `MakeAssetResolver::ToPath`'s MaterialAsset branch wrote the asset's filepath VERBATIM. With a project
// open every content root is absolute (Constants::Path::SetProjectRoot), so a scene re-saved in the
// editor took whoever saved it home directory into the repository. 42 of the 50 shipped scenes carried
// one, naming 22 distinct `/Users/<somebody>/.../Materials/*.demat` files, and not one of those strings
// names anything on any other machine. The three cloud asset classes went relative for exactly this
// reason; this is the fourth.
//
// Four things are asserted:
//
//   1. The pure function rewrites the four places a scene can name a material, and nothing else.
//   2. It survives the shapes a hand-edited or truncated file can have — missing keys, wrong types,
//      an empty slot, a path outside the project.
//   3. It is idempotent and gated on its OWN version integer, not on the head.
//   4. NO shipped `.desce` contains an absolute path at all. That is the class this defect belongs to,
//      and the only assertion of the four that catches it coming back through some other door.

#include <SceneMigration.hpp>
#include <assets_sandbox.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionMaterialPath;
using Desert::Migration::kSceneVersionTerrainMaterial;
using Desert::Migration::MaterialPathMigrationReport;
using Desert::Migration::MigrateMaterialPathV7ToV8;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;

namespace
{
    // The root every test below measures against. Spelled the way the editor spells it (relative, with a
    // trailing separator) precisely because that is the shape that has to work: the migration must reduce
    // an ABSOLUTE stored path against a RELATIVE root, which std::filesystem::relative cannot do without
    // touching the disk.
    const std::filesystem::path kAssetsRoot = "Resources/Assets/";

    rfl::Generic::Array Paths( std::initializer_list<const char*> values )
    {
        rfl::Generic::Array out;
        for ( const char* v : values )
            out.push_back( rfl::Generic( std::string( v ) ) );
        return out;
    }

    EntityData MeshEntity( const char* tag, const char* component, rfl::Generic::Array paths )
    {
        EntityData e;
        e.Tag = tag;

        rfl::Generic::Object payload;
        payload["MaterialPaths"] = std::move( paths );
        payload["Primitive"]     = std::string( "Cube" );

        e.Components[component] = rfl::Generic( std::move( payload ) );
        return e;
    }

    EntityData TerrainEntity( const char* tag, const char* material )
    {
        EntityData e;
        e.Tag = tag;

        rfl::Generic::Object payload;
        payload["Size"]     = 30000.0;
        payload["Material"] = std::string( material );

        e.Components["Terrain"] = rfl::Generic( std::move( payload ) );
        return e;
    }

    // The value a component holds under `key`, as a vector of strings. Fails the calling test rather than
    // returning something plausible when the shape is not what the migration should have produced.
    std::vector<std::string> SlotsOf( const EntityData& e, const char* component, const char* key )
    {
        std::vector<std::string> out;
        const auto               payload = e.Components.get( component );
        if ( !payload.has_value() )
            return out;
        const auto fields = payload.value().to_object();
        if ( !fields.has_value() )
            return out;
        const auto named = fields.value().get( key );
        if ( !named.has_value() )
            return out;
        if ( const auto text = named.value().to_string(); text.has_value() )
        {
            out.push_back( text.value() );
            return out;
        }
        if ( const auto rows = named.value().to_array(); rows.has_value() )
            for ( const auto& row : rows.value() )
                out.push_back( row.to_string().value_or( "<not a string>" ) );
        return out;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy, written periodically and never
    // migrated. A recursive sweep descends into it and would then pass or fail on whatever the last
    // editor session left behind — green in CI, red on any machine that has ever run the editor. The
    // terrain-material suite learned this the expensive way and its filter is copied here.
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }
} // namespace

// ── The pure function ──────────────────────────────────────────────────────────────────────────────

TEST( SceneMaterialPathMigration, AnAbsolutePathBecomesOneRelativeToTheAssetsRoot )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity(
         "Floor", "StaticMesh",
         Paths( { "/Users/somebody/Proj/Editor/Resources/Assets/Materials/M_CheckerFloor.demat" } ) ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Paths, 1 );
    EXPECT_TRUE( report.OutsideNames.empty() );
    EXPECT_EQ( SlotsOf( entities[0], "StaticMesh", "MaterialPaths" ),
               std::vector<std::string>{ "Materials/M_CheckerFloor.demat" } );
}

// The three mesh components and the terrain, in one tree, because "it works for StaticMesh" is exactly
// how the other three get forgotten — and a scene names materials from all four.
TEST( SceneMaterialPathMigration, AllFourPlacesASceneCanNameAMaterialAreRewritten )
{
    const char* absolute = "/Users/somebody/Proj/Editor/Resources/Assets/Materials/M.demat";

    std::vector<EntityData> entities;
    entities.push_back( MeshEntity( "A", "StaticMesh", Paths( { absolute } ) ) );
    entities.push_back( MeshEntity( "B", "InstancedStaticMesh", Paths( { absolute } ) ) );
    entities.push_back( MeshEntity( "C", "SkinnedMesh", Paths( { absolute } ) ) );
    entities.push_back( TerrainEntity( "D", absolute ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Entities, 4 );
    EXPECT_EQ( report.Paths, 4 );
    EXPECT_EQ( SlotsOf( entities[0], "StaticMesh", "MaterialPaths" )[0], "Materials/M.demat" );
    EXPECT_EQ( SlotsOf( entities[1], "InstancedStaticMesh", "MaterialPaths" )[0], "Materials/M.demat" );
    EXPECT_EQ( SlotsOf( entities[2], "SkinnedMesh", "MaterialPaths" )[0], "Materials/M.demat" );
    EXPECT_EQ( SlotsOf( entities[3], "Terrain", "Material" )[0], "Materials/M.demat" );
}

// A mesh has as many slots as it has submeshes, and only some of them were ever absolute — the file also
// carries "" for a slot whose handle resolved to nothing. Each slot is decided on its own.
TEST( SceneMaterialPathMigration, EachSlotOfAMultiSlotMeshIsDecidedSeparately )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity( "Mesh", "StaticMesh",
                                    Paths( { "/Users/somebody/Proj/Editor/Resources/Assets/Materials/A.demat", "",
                                             "Materials/B.demat", "Resources/Assets/Materials/C.demat" } ) ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Paths, 2 ) << "the empty slot and the already-relative one are not rewrites";

    const std::vector<std::string> expected{ "Materials/A.demat", "", "Materials/B.demat", "Materials/C.demat" };
    EXPECT_EQ( SlotsOf( entities[0], "StaticMesh", "MaterialPaths" ), expected );
}

// The working-directory-relative form the hand-written probe scenes carried. It is not "already correct":
// FromPath joins the assets root to a relative path, so `Resources/Assets/Materials/X.demat` would resolve
// to `Resources/Assets/Resources/Assets/Materials/X.demat`. One convention, and this is what converts to it.
TEST( SceneMaterialPathMigration, AWorkingDirectoryRelativePathIsReducedToTheAssetsRootForm )
{
    std::vector<EntityData> entities;
    entities.push_back( TerrainEntity( "Terrain", "Resources/Assets/Materials/M_TerrainProbe.demat" ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Paths, 1 );
    EXPECT_EQ( SlotsOf( entities[0], "Terrain", "Material" )[0], "Materials/M_TerrainProbe.demat" );
}

// A file genuinely outside the project has no project-relative form to be given, so it is left exactly as
// it is — and NAMED, because that scene still does not open elsewhere and a count would not say which
// slot to re-point (DC 1.4).
TEST( SceneMaterialPathMigration, APathOutsideTheAssetsRootIsLeftAloneAndNamed )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity( "Prop", "StaticMesh", Paths( { "/opt/shared/library/Steel.demat" } ) ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Entities, 0 ) << "nothing was rewritten, so no entity was touched";
    EXPECT_EQ( report.Paths, 0 );
    ASSERT_EQ( report.OutsideNames.size(), 1u );
    EXPECT_NE( report.OutsideNames[0].find( "/opt/shared/library/Steel.demat" ), std::string::npos )
         << "the message must carry the path itself: " << report.OutsideNames[0];
    EXPECT_NE( report.OutsideNames[0].find( "Prop" ), std::string::npos )
         << "and the entity it is on: " << report.OutsideNames[0];
    EXPECT_EQ( SlotsOf( entities[0], "StaticMesh", "MaterialPaths" )[0], "/opt/shared/library/Steel.demat" );
}

// THE MIRROR OF THE TEST ABOVE, AND THE TWO ONLY MEAN SOMETHING TOGETHER.
//
// "Is this path rooted" was asked with std::filesystem::path::is_absolute(), which answers in the grammar
// of the HOST. So the POSIX path above is not absolute on Windows and a Windows path is not absolute on
// POSIX, and in each case the migration silently left a path it could not rewrite instead of naming it.
// The test above went red on Windows CI while macOS stayed green, which is the whole shape of the defect:
// a scene travels between developers, and the Mac-authored one opened on Windows is exactly the case the
// migration exists for.
//
// One test on each platform's own spelling would still have passed on the platform that wrote it. This
// pair cannot: whichever host runs them, one of the two is foreign, so the predicate has to be about the
// STRING rather than about the machine reading it.
TEST( SceneMaterialPathMigration, AWindowsRootedPathIsAlsoLeftAloneAndNamed )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity( "Prop", "StaticMesh", Paths( { "C:/shared/library/Steel.demat" } ) ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Paths, 0 );
    ASSERT_EQ( report.OutsideNames.size(), 1u )
         << "a drive-lettered path is rooted somewhere and is not under our assets root, whatever OS is "
            "reading it -- leaving it unnamed is the silent substitution DC 1.4 forbids";
    EXPECT_NE( report.OutsideNames[0].find( "C:/shared/library/Steel.demat" ), std::string::npos );
    EXPECT_EQ( SlotsOf( entities[0], "StaticMesh", "MaterialPaths" )[0], "C:/shared/library/Steel.demat" );
}

// And a backslash-spelled one, because that is what Windows actually writes.
TEST( SceneMaterialPathMigration, ABackslashRootedPathIsAlsoLeftAloneAndNamed )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity( "Prop", "StaticMesh", Paths( { "D:\\build\\Steel.demat" } ) ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Paths, 0 );
    ASSERT_EQ( report.OutsideNames.size(), 1u );
    EXPECT_NE( report.OutsideNames[0].find( "D:\\build\\Steel.demat" ), std::string::npos );
}

// ── Missing and malformed fields ───────────────────────────────────────────────────────────────────

TEST( SceneMaterialPathMigration, AnEntityThatNamesNoMaterialIsUntouched )
{
    std::vector<EntityData> entities;

    EntityData bare;
    bare.Tag = "Sun";
    rfl::Generic::Object light;
    light["Intensity"]                = 1.0;
    bare.Components["DirectionLight"] = rfl::Generic( std::move( light ) );
    entities.push_back( std::move( bare ) );

    // A StaticMesh with no MaterialPaths key at all — a primitive with no material slot.
    EntityData primitive;
    primitive.Tag = "Cube";
    rfl::Generic::Object mesh;
    mesh["Primitive"]                  = std::string( "Cube" );
    primitive.Components["StaticMesh"] = rfl::Generic( std::move( mesh ) );
    entities.push_back( std::move( primitive ) );

    const std::string before = rfl::json::write( entities );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Paths, 0 );
    EXPECT_TRUE( report.OutsideNames.empty() );
    EXPECT_EQ( rfl::json::write( entities ), before ) << "the tree must be byte-identical";
}

// A hand-edit, a truncation or a file from a future schema can put anything under these keys. None of it
// may be silently replaced by a default: the value is left exactly as it is and warned about.
TEST( SceneMaterialPathMigration, MalformedPayloadsAreLeftExactlyAsTheyAre )
{
    std::vector<EntityData> entities;

    // The whole component payload is a number, not an object.
    EntityData notAnObject;
    notAnObject.Tag                      = "Broken1";
    notAnObject.Components["StaticMesh"] = rfl::Generic( 42 );
    entities.push_back( std::move( notAnObject ) );

    // MaterialPaths is a string where an array belongs.
    EntityData notAnArray;
    notAnArray.Tag = "Broken2";
    rfl::Generic::Object scalarPaths;
    scalarPaths["MaterialPaths"]        = std::string( "/Users/x/Proj/Resources/Assets/Materials/M.demat" );
    notAnArray.Components["StaticMesh"] = rfl::Generic( std::move( scalarPaths ) );
    entities.push_back( std::move( notAnArray ) );

    // A slot that is a number rather than a path.
    rfl::Generic::Array mixed;
    mixed.push_back( rfl::Generic( 7 ) );
    mixed.push_back( rfl::Generic( std::string( "/Users/x/Proj/Resources/Assets/Materials/M.demat" ) ) );
    entities.push_back( MeshEntity( "Broken3", "StaticMesh", std::move( mixed ) ) );

    // Terrain.Material is a bool.
    EntityData terrainBool;
    terrainBool.Tag = "Broken4";
    rfl::Generic::Object terrain;
    terrain["Material"]               = true;
    terrainBool.Components["Terrain"] = rfl::Generic( std::move( terrain ) );
    entities.push_back( std::move( terrainBool ) );

    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    // Only the one usable string inside Broken3 moved. Nothing was invented for the rest.
    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Paths, 1 );

    EXPECT_EQ( entities[0].Components.get( "StaticMesh" ).value().to_int().value_or( 0 ), 42 );
    EXPECT_EQ( SlotsOf( entities[1], "StaticMesh", "MaterialPaths" )[0],
               "/Users/x/Proj/Resources/Assets/Materials/M.demat" );
    EXPECT_EQ( SlotsOf( entities[2], "StaticMesh", "MaterialPaths" )[1], "Materials/M.demat" );
    EXPECT_TRUE( entities[3]
                      .Components.get( "Terrain" )
                      .value()
                      .to_object()
                      .value()
                      .get( "Material" )
                      .value()
                      .to_bool()
                      .value_or( false ) );
}

TEST( SceneMaterialPathMigration, AnEmptyTreeIsAcceptedAndReportsNothing )
{
    std::vector<EntityData>           entities;
    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Paths, 0 );
    EXPECT_TRUE( report.OutsideNames.empty() );
}

// A root with no usable components cannot decide anything, so it decides nothing rather than stripping a
// leading component off every path it sees.
TEST( SceneMaterialPathMigration, AnEmptyAssetsRootRewritesNothing )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity(
         "A", "StaticMesh", Paths( { "/Users/somebody/Proj/Editor/Resources/Assets/Materials/M.demat" } ) ) );

    const std::string                 before = rfl::json::write( entities );
    const MaterialPathMigrationReport report = MigrateMaterialPathV7ToV8( entities, "" );

    EXPECT_EQ( report.Paths, 0 );
    EXPECT_EQ( rfl::json::write( entities ), before );
}

TEST( SceneMaterialPathMigration, RunningItTwiceChangesNothingTheSecondTime )
{
    std::vector<EntityData> entities;
    entities.push_back( MeshEntity(
         "A", "StaticMesh", Paths( { "/Users/somebody/Proj/Editor/Resources/Assets/Materials/M.demat" } ) ) );
    entities.push_back( TerrainEntity( "T", "Resources/Assets/Materials/T.demat" ) );

    EXPECT_EQ( MigrateMaterialPathV7ToV8( entities, kAssetsRoot ).Paths, 2 );
    const std::string once = rfl::json::write( entities );

    const MaterialPathMigrationReport again = MigrateMaterialPathV7ToV8( entities, kAssetsRoot );
    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( again.Paths, 0 );
    EXPECT_EQ( rfl::json::write( entities ), once ) << "the tree must be byte-identical the second time";
}

// The root arriving absolute must give the same answer as the root arriving relative — that is what lets
// the editor (working directory `Editor/`) and Tools/SceneMigrator (working directory the repository
// root) produce the same file from the same input. Two spellings of one rule that must agree.
TEST( SceneMaterialPathMigration, TheAnswerDoesNotDependOnHowTheRootIsSpelled )
{
    const char* absolute = "/Users/somebody/Proj/Editor/Resources/Assets/Materials/M.demat";

    std::vector<EntityData> viaRelative;
    viaRelative.push_back( MeshEntity( "A", "StaticMesh", Paths( { absolute } ) ) );
    MigrateMaterialPathV7ToV8( viaRelative, "Resources/Assets/" );

    std::vector<EntityData> viaAbsolute;
    viaAbsolute.push_back( MeshEntity( "A", "StaticMesh", Paths( { absolute } ) ) );
    MigrateMaterialPathV7ToV8( viaAbsolute, "/Users/somebody/Proj/Editor/Resources/Assets" );

    EXPECT_EQ( rfl::json::write( viaRelative ), rfl::json::write( viaAbsolute ) );
    EXPECT_EQ( SlotsOf( viaRelative[0], "StaticMesh", "MaterialPaths" )[0], "Materials/M.demat" );
}

// ── The version gate ───────────────────────────────────────────────────────────────────────────────

TEST( SceneMaterialPathMigration, MigrateSceneRunsTheStepForAV7FileAndStampsItAtTheHead )
{
    const Desert::TestSupport::AssetsSandbox sandbox( "SceneMaterialPathMigration", { "Materials/M.demat" } );
    SceneSerialized scene;
    scene.SceneName    = "V7";
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.SceneVersion = kSceneVersionTerrainMaterial;
    scene.Entities.push_back( MeshEntity(
         "A", "StaticMesh", Paths( { "/Users/somebody/Proj/Editor/Resources/Assets/Materials/M.demat" } ) ) );

    const auto report = MigrateScene( scene, kAssetsRoot );

    EXPECT_TRUE( report.MaterialPathRaised );
    EXPECT_EQ( report.MaterialPath.Paths, 1 );
    EXPECT_FALSE( report.TerrainMaterialRaised ) << "a v7 file is already past the terrain step";
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
}

TEST( SceneMaterialPathMigration, AFileAlreadyAtTheHeadIsNotMigratedAgain )
{
    SceneSerialized scene;
    scene.SceneName   = "AtTheHead";
    scene.UnitVersion = Desert::Migration::kUnitVersion;
    // kSceneVersion, NOT kSceneVersionMaterialPath. This test stamped step 8's own integer while step 8
    // happened to be the newest, so the two were the same number and the difference was invisible. Adding
    // step 9 made a file stamped 8 exactly what this test says it is not - one with a migration still owed -
    // and `Changed()` went true. What the test means is "at the HEAD", so it has to say the head.
    scene.SceneVersion = kSceneVersion;
    scene.Entities.push_back( MeshEntity( "A", "StaticMesh", Paths( { "Materials/M.demat" } ) ) );

    const auto report = MigrateScene( scene, kAssetsRoot );

    EXPECT_FALSE( report.MaterialPathRaised );
    EXPECT_FALSE( report.Changed() );
}

// Each step is gated on ITS OWN constant. Gating them all on kSceneVersion would send every v7 file back
// through the terrain migration the moment the head moved — the mistake the tonemap suite records having
// been made once already.
TEST( SceneMaterialPathMigration, TheStepHasItsOwnVersionIntegerAboveTheOneBeforeIt )
{
    EXPECT_GT( kSceneVersionMaterialPath, kSceneVersionTerrainMaterial );

    // It used to also assert `kSceneVersion == kSceneVersionMaterialPath`. That was true only while this
    // was the NEWEST step, which is not a property of the step - it expired the moment step 9 landed. Being
    // at or below the head is the durable statement; the newest step asserts equality in its own suite, and
    // SceneMigration.hpp's static_assert is what actually pins the pair.
    EXPECT_LE( kSceneVersionMaterialPath, kSceneVersion );
}

// ── The repository, swept ──────────────────────────────────────────────────────────────────────────

// THE assertion of this suite. DC 4.5 says the scenes in the repository are converted by the task that
// changes the format; this is what keeps them converted. It looks for an absolute path of ANY kind rather
// than for material paths specifically, because the defect is the class, not the instance: the cloud
// branches had to be fixed for it once already, and a fifth asset type that forgets will be caught here
// rather than by the next person who clones the repository.
TEST( SceneMaterialPathMigration, NoShippedSceneContainsAnAbsolutePath )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::filesystem::path dir = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( dir ) ) << dir.string() << " is missing";

    int seen = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( dir ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        std::ifstream in( entry.path(), std::ios::binary );
        ASSERT_TRUE( in ) << entry.path().string() << " could not be read";
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        ++seen;

        auto parsed = rfl::json::read<SceneSerialized>( text );
        ASSERT_TRUE( parsed ) << entry.path().string() << " does not parse as a scene";

        EXPECT_EQ( Desert::Assets::StatedVersion( parsed.value().Header, Desert::Assets::kSceneSchemaTag ),
                   kSceneVersion )
             << entry.path().filename().string()
             << " is not stamped at the current schema version. Run Tools/SceneMigrator over "
                "Editor/Resources/Assets/Scenes and commit the result (DC 4.5).";

        // EVERY quoted string in the file, whatever it is nested in. Checked as TEXT, and deliberately
        // not by walking the parsed tree: the first version of this scan looked for `"key":"value"` and
        // so never saw an element of an ARRAY — which is the shape `MaterialPaths` has, i.e. it missed
        // the exact defect it was written for, and said so only when the defect was put back to check.
        // A JSON key is a string too, but no key is an absolute path, so scanning all of them is free.
        for ( size_t at = 0; at < text.size(); ++at )
        {
            if ( text[at] != '"' )
                continue;

            std::string value;
            size_t      cursor = at + 1;
            for ( ; cursor < text.size() && text[cursor] != '"'; ++cursor )
            {
                if ( text[cursor] == '\\' && cursor + 1 < text.size() )
                    ++cursor; // an escaped quote is part of the string, not its end
                value += text[cursor];
            }
            at = cursor; // resume after the closing quote, so a string's contents are never rescanned

            EXPECT_FALSE( !value.empty() && std::filesystem::path( value ).is_absolute() )
                 << entry.path().filename().string() << " carries the ABSOLUTE path '" << value
                 << "'. A scene names project content by a path RELATIVE to the assets root, because an "
                    "absolute one is a machine's home directory and resolves nowhere else. Re-save the "
                    "scene (or run Tools/SceneMigrator) and commit the result.";
        }
    }

    EXPECT_GT( seen, 0 ) << "no .desce files were found — the sweep asserted nothing";
}

// The companion to the sweep above: not only is the head stamped, MigrateScene agrees there is nothing
// left to do. A file can be stamped at v8 and still carry v7 content if somebody edits the integer.
TEST( SceneMaterialPathMigration, MigrateSceneFindsNothingToDoInAnyShippedScene )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::filesystem::path dir = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( dir ) ) << dir.string() << " is missing";

    for ( const auto& entry : std::filesystem::recursive_directory_iterator( dir ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        std::ifstream      in( entry.path(), std::ios::binary );
        std::ostringstream ss;
        ss << in.rdbuf();

        auto parsed = rfl::json::read<SceneSerialized>( ss.str() );
        ASSERT_TRUE( parsed ) << entry.path().string() << " does not parse as a scene";

        SceneSerialized scene = parsed.value();
        EXPECT_FALSE( MigrateScene( scene, kAssetsRoot ).Changed() )
             << entry.path().filename().string()
             << " still has a migration to run. Run Tools/SceneMigrator and commit the result.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
