// The terrain-material migration, scene v6 -> v7, and the relations it exists to protect.
//
// The terrain's material stopped being an ECS::MaterialComponent authored inside the Details panel and
// became a `.demat` that TerrainData names by handle, like every other material in the engine
// (Docs/MaterialEditor/PLAN_STAGE3_ASSET_DOCUMENTS.md, M3). So the "Material" payload a terrain entity used
// to carry has to leave the FILE as well as the code — DC 4.1, the old path goes with the change that
// replaces it, and DC 4.5, the scenes in this repository are converted by the same task.
//
// THIS IS THE ONE MIGRATION IN THE ENGINE THAT DROPS RATHER THAN MOVES, and that is what most of this
// suite is about. The new home for a terrain material's values is a FILE, the migration is pure and cannot
// write one, and there is no other place in a scene tree for them that is not the inline authoring the task
// exists to delete. So three properties have to hold, and each has tests below:
//
//   * IT MUST DROP ONLY THE TERRAIN'S COPY. A "Material" on an entity with no terrain is the runtime/Lua
//     `setMaterialParam` channel and legacy-scene compatibility, which is all MaterialComponent claims to
//     be and all it now is. Taking that one too would break every scene that scripts a material, and
//     MAT_ProbeOverride.desce is exactly such a scene.
//   * IT MUST NAME WHAT IT DROPPED. A count is not enough: "3 values dropped" does not tell anybody the
//     grass texture was one of them, and DC 1.4 forbids a quiet substitution. Every row's name goes back
//     to the loader, INCLUDING the rows it could not read, which are the ones a reader most needs told
//     about.
//   * IT MUST BE HARMLESS HERE, AND STAY HARMLESS. Not one `.desce` in this repository has a terrain
//     entity carrying an inline material, so the conversion moved no values at all. That is a fact about
//     the tree and not an argument, so the sweep at the bottom of this file asserts it — if somebody adds
//     such a scene, this suite goes red before the drop can silently eat their work.
//
// Everything below runs on the parsed tree. No GPU, no scene graph, no asset manager — the migration is a
// pure function and this is what that buys.

#include <SceneMigration.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionCloudSet;
using Desert::Migration::kSceneVersionTerrainMaterial;
using Desert::Migration::MigrateScene;
using Desert::Migration::MigrateTerrainMaterialV6ToV7;
using Desert::Migration::SceneSerialized;
using Desert::Migration::TerrainMaterialMigrationReport;

namespace
{
    // One "Params"/"Textures" row as ComponentRegistry writes it: a name and a value.
    rfl::Generic ParamRow( const std::string& name, double x )
    {
        rfl::Generic::Object row;
        row["Name"] = name;
        row["Value"] =
             rfl::Generic::Array{ rfl::Generic( x ), rfl::Generic( x ), rfl::Generic( x ), rfl::Generic( 1.0 ) };
        return rfl::Generic( std::move( row ) );
    }

    rfl::Generic TextureRow( const std::string& name, std::int64_t handle )
    {
        rfl::Generic::Object row;
        row["Name"]          = name;
        row["TextureHandle"] = handle;
        return rfl::Generic( std::move( row ) );
    }

    // The v6 shape of an authored terrain material: the Terrain shader, a tint and a tiling, and the three
    // splat layers. This is what DrawTerrainMaterialWidget wrote before it was deleted.
    rfl::Generic::Object TerrainMaterialPayloadV6()
    {
        rfl::Generic::Object mat;
        mat["ShaderName"] = std::string( "Terrain" );
        mat["Params"]     = rfl::Generic::Array{ ParamRow( "Tint", 0.5 ), ParamRow( "DetailTiling", 8.0 ) };
        mat["Textures"]   = rfl::Generic::Array{ TextureRow( "u_GrassTex", 111 ), TextureRow( "u_RockTex", 222 ),
                                               TextureRow( "u_SnowTex", 333 ) };
        return mat;
    }

    rfl::Generic::Object TerrainPayload()
    {
        rfl::Generic::Object terrain;
        terrain["Size"]        = 30000.0;
        terrain["Resolution"]  = 64;
        terrain["HeightScale"] = 400.0;
        return terrain;
    }

    Desert::Assets::EntityData TerrainEntity( bool withMaterial )
    {
        Desert::Assets::EntityData e;
        e.Tag                   = "Terrain";
        e.Components["Terrain"] = rfl::Generic( TerrainPayload() );
        if ( withMaterial )
            e.Components["Material"] = rfl::Generic( TerrainMaterialPayloadV6() );
        return e;
    }

    // The repository root, found by walking up from wherever the binary was started — the same approach
    // SettingConsumers and SceneTonemapMigration use, so the suite does not have to be run from one exact
    // directory.
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

    const Desert::Reflection::FieldInfo* FindField( const char* type, const char* field )
    {
        const auto* info = Desert::Reflection::ReflectionRegistry::Get().Find( type );
        if ( !info )
            return nullptr;
        for ( const auto& f : info->Fields )
            if ( f.Name == field )
                return &f;
        return nullptr;
    }
} // namespace

// ── The pure function ──────────────────────────────────────────────────────────────────────────────

TEST( SceneTerrainMaterialMigration, TheTerrainsInlineMaterialIsRemovedAndEverythingItHeldIsNamed )
{
    std::vector<Desert::Assets::EntityData> entities{ TerrainEntity( /*withMaterial*/ true ) };

    const TerrainMaterialMigrationReport report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Params, 2 );
    EXPECT_EQ( report.Textures, 3 );

    // The names, in the order they were found. This is the payload of the log the loader prints, and it is
    // the whole reason a drop is allowed to be a migration at all.
    const std::vector<std::string> expected{ "Tint", "DetailTiling", "u_GrassTex", "u_RockTex", "u_SnowTex" };
    EXPECT_EQ( report.DroppedNames, expected );

    // Gone from the tree, and the terrain itself untouched.
    EXPECT_FALSE( entities.front().Components.get( "Material" ).has_value() );
    ASSERT_TRUE( entities.front().Components.get( "Terrain" ).has_value() );
    const auto terrain = entities.front().Components.get( "Terrain" ).value().to_object();
    ASSERT_TRUE( terrain.has_value() );
    ASSERT_TRUE( terrain.value().get( "HeightScale" ).has_value() );
    EXPECT_DOUBLE_EQ( terrain.value().get( "HeightScale" ).value().to_double().value(), 400.0 );
}

// THE ONE THAT WOULD HAVE BROKEN MAT_ProbeOverride.desce.
//
// A "Material" without a "Terrain" beside it is a mesh entity's runtime override — the channel scripts
// write through and the channel legacy scenes load through. It is not authoring, it was never the terrain's,
// and it stays exactly where it is. A migration keyed on the component alone would have deleted it.
TEST( SceneTerrainMaterialMigration, AMaterialOnAnEntityWithNoTerrainIsLeftAlone )
{
    Desert::Assets::EntityData mesh;
    mesh.Tag                      = "Override_BlendMax";
    mesh.Components["StaticMesh"] = rfl::Generic( rfl::Generic::Object{} );
    rfl::Generic::Object override;
    override["ShaderName"]      = std::string( "MatProbe" );
    override["Params"]          = rfl::Generic::Array{ ParamRow( "Blend", 1.0 ) };
    mesh.Components["Material"] = rfl::Generic( std::move( override ) );

    std::vector<Desert::Assets::EntityData> entities{ mesh };
    const TerrainMaterialMigrationReport    report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Params, 0 );
    EXPECT_TRUE( report.DroppedNames.empty() );

    ASSERT_TRUE( entities.front().Components.get( "Material" ).has_value() );
    const auto kept = entities.front().Components.get( "Material" ).value().to_object();
    ASSERT_TRUE( kept.has_value() );
    ASSERT_TRUE( kept.value().get( "ShaderName" ).has_value() );
    EXPECT_EQ( kept.value().get( "ShaderName" ).value().to_string().value(), "MatProbe" );
}

TEST( SceneTerrainMaterialMigration, ATerrainWithNoMaterialIsLeftByteIdenticalAndReportsZero )
{
    std::vector<Desert::Assets::EntityData> entities{ TerrainEntity( /*withMaterial*/ false ) };
    const std::string                       before = rfl::json::write( entities );

    const TerrainMaterialMigrationReport report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_TRUE( report.DroppedNames.empty() );
    EXPECT_EQ( rfl::json::write( entities ), before );
}

TEST( SceneTerrainMaterialMigration, RunningItTwiceChangesNothingTheSecondTime )
{
    std::vector<Desert::Assets::EntityData> entities{ TerrainEntity( /*withMaterial*/ true ) };

    EXPECT_EQ( MigrateTerrainMaterialV6ToV7( entities ).Entities, 1 );
    const std::string once = rfl::json::write( entities );

    const TerrainMaterialMigrationReport again = MigrateTerrainMaterialV6ToV7( entities );
    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( rfl::json::write( entities ), once );
}

// EVERY OTHER COMPONENT SURVIVES, IN ORDER. The payload map has no erase, so the migration rebuilds it —
// and a rebuild is exactly where a component quietly goes missing or the file's key order churns.
TEST( SceneTerrainMaterialMigration, TheOtherComponentsSurviveTheRebuildInTheirOriginalOrder )
{
    Desert::Assets::EntityData e;
    e.Tag                      = "Terrain";
    e.Components["Terrain"]    = rfl::Generic( TerrainPayload() );
    e.Components["Material"]   = rfl::Generic( TerrainMaterialPayloadV6() );
    e.Components["Visibility"] = rfl::Generic( rfl::Generic::Object{} );
    e.Components["RigidBody"]  = rfl::Generic( rfl::Generic::Object{} );

    std::vector<Desert::Assets::EntityData> entities{ e };
    ASSERT_EQ( MigrateTerrainMaterialV6ToV7( entities ).Entities, 1 );

    std::vector<std::string> keys;
    for ( const auto& [key, value] : entities.front().Components )
        keys.push_back( key );

    const std::vector<std::string> expected{ "Terrain", "Visibility", "RigidBody" };
    EXPECT_EQ( keys, expected );
}

// ── Missing and malformed fields ───────────────────────────────────────────────────────────────────

// A material with a shader and nothing else. Nothing to name, and that is a legitimate answer: the shader
// name is deliberately not reported, because the new model has exactly one Terrain-domain program and a
// terrain material is created on it. There is nothing there for anybody to re-author.
TEST( SceneTerrainMaterialMigration, AMaterialWithNoValuesIsStillRemovedAndNamesNothing )
{
    Desert::Assets::EntityData e = TerrainEntity( /*withMaterial*/ false );
    rfl::Generic::Object       bare;
    bare["ShaderName"]       = std::string( "Terrain" );
    e.Components["Material"] = rfl::Generic( std::move( bare ) );

    std::vector<Desert::Assets::EntityData> entities{ e };
    const TerrainMaterialMigrationReport    report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Params, 0 );
    EXPECT_EQ( report.Textures, 0 );
    EXPECT_TRUE( report.DroppedNames.empty() );
    EXPECT_FALSE( entities.front().Components.get( "Material" ).has_value() );
}

// A hand-edit, or a file from a format nobody remembers. It STILL counts as a removal: the entity carried
// something under that key, and reporting "nothing was there" because it could not be parsed is precisely
// the quiet substitution DC 1.4 forbids.
TEST( SceneTerrainMaterialMigration, AMaterialPayloadThatIsNotAnObjectIsStillRemovedAndStillCounted )
{
    Desert::Assets::EntityData e = TerrainEntity( /*withMaterial*/ false );
    e.Components["Material"]     = rfl::Generic( std::string( "Terrain" ) );

    std::vector<Desert::Assets::EntityData> entities{ e };
    const TerrainMaterialMigrationReport    report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Params, 0 );
    EXPECT_EQ( report.Textures, 0 );
    EXPECT_FALSE( entities.front().Components.get( "Material" ).has_value() );
}

TEST( SceneTerrainMaterialMigration, AParamsFieldThatIsNotAnArrayIsRemovedWithTheRestAndNamesNothing )
{
    Desert::Assets::EntityData e = TerrainEntity( /*withMaterial*/ false );
    rfl::Generic::Object       broken;
    broken["ShaderName"]     = std::string( "Terrain" );
    broken["Params"]         = 7;
    broken["Textures"]       = rfl::Generic::Array{ TextureRow( "u_SnowTex", 9 ) };
    e.Components["Material"] = rfl::Generic( std::move( broken ) );

    std::vector<Desert::Assets::EntityData> entities{ e };
    const TerrainMaterialMigrationReport    report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Params, 0 );
    // The readable half is still read: a malformed Params must not cost the Textures beside it their names.
    EXPECT_EQ( report.Textures, 1 );
    ASSERT_EQ( report.DroppedNames.size(), 1u );
    EXPECT_EQ( report.DroppedNames.front(), "u_SnowTex" );
}

// A row with a value and no name is the worst case for a REPORT — there is nothing to call it — so it is
// counted and reported under a placeholder rather than skipped. A dropped value that appears in no count
// and no list is a value nobody knows they lost.
TEST( SceneTerrainMaterialMigration, ARowWithNoNameIsCountedAndReportedRatherThanSkipped )
{
    Desert::Assets::EntityData e = TerrainEntity( /*withMaterial*/ false );
    rfl::Generic::Object       anon;
    rfl::Generic::Object       nameless;
    nameless["Value"] = rfl::Generic::Array{ rfl::Generic( 1.0 ) };
    anon["Params"]    = rfl::Generic::Array{ rfl::Generic( std::move( nameless ) ), ParamRow( "Tint", 0.25 ) };
    e.Components["Material"] = rfl::Generic( std::move( anon ) );

    std::vector<Desert::Assets::EntityData> entities{ e };
    const TerrainMaterialMigrationReport    report = MigrateTerrainMaterialV6ToV7( entities );

    EXPECT_EQ( report.Params, 2 );
    const std::vector<std::string> expected{ "<unnamed>", "Tint" };
    EXPECT_EQ( report.DroppedNames, expected );
}

// ── The version gate ───────────────────────────────────────────────────────────────────────────────

TEST( SceneTerrainMaterialMigration, MigrateSceneRunsItAndStampsTheFileSoItNeverRunsAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Terrain_Grass";
    scene.SceneVersion = kSceneVersionCloudSet;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.Entities.push_back( TerrainEntity( /*withMaterial*/ true ) );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.TerrainMaterialRaised );
    EXPECT_EQ( report.TerrainMaterial.Entities, 1 );
    EXPECT_EQ( report.TerrainMaterial.Params, 2 );
    EXPECT_EQ( report.TerrainMaterial.Textures, 3 );
    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    // GE, not EQ. This used to pin the head to this step's own number, which was true only while the
    // terrain step WAS the head — the v7 -> v8 material-path step moved it, and the assertion failed
    // without anything about the terrain migration having changed. The tonemap suite wrote GE here for
    // exactly this reason and recorded why; this is the same lesson arriving a second time.
    EXPECT_GE( kSceneVersion, kSceneVersionTerrainMaterial );

    // Second pass over the stamped tree: nothing left to do.
    const auto again = MigrateScene( scene );
    EXPECT_FALSE( again.Changed() );
}

// SHELF LIFE, ASSERTED. The step raises v6 to v7 and nothing else: a file already at v7 does not enter it,
// which is what lets the function be deleted once no v6 file remains rather than living forever as a
// "supports the old format" branch (DC 4.6).
TEST( SceneTerrainMaterialMigration, AFileAlreadyAtV7IsNotRunThroughTheStepAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Already";
    scene.SceneVersion = kSceneVersionTerrainMaterial;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    // A terrain entity that (impossibly, for a v7 file) still carries one. The gate must not look at it.
    scene.Entities.push_back( TerrainEntity( /*withMaterial*/ true ) );

    const auto report = MigrateScene( scene );

    EXPECT_FALSE( report.TerrainMaterialRaised );
    EXPECT_EQ( report.TerrainMaterial.Entities, 0 );
    EXPECT_TRUE( scene.Entities.front().Components.get( "Material" ).has_value() );
}

// A v0 file arrives at the HEAD in one pass — which is what this test is named for, and which is not
// the same claim as "arrives at v7". Every step is gated on its OWN constant precisely so that a file
// entering the chain at any point comes out at the end of it, wherever the end currently is.
TEST( SceneTerrainMaterialMigration, AFileFromBeforeEveryStepStillComesOutAtTheHead )
{
    SceneSerialized scene;
    scene.SceneName = "Ancient";
    // No SceneVersion and no UnitVersion at all: the oldest shape a .desce can have.
    scene.Entities.push_back( TerrainEntity( /*withMaterial*/ true ) );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.TerrainMaterialRaised );
    EXPECT_EQ( report.TerrainMaterial.Entities, 1 );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    EXPECT_FALSE( scene.Entities.front().Components.get( "Material" ).has_value() );
}

// ── The relation: the migration and the component agree where a terrain material lives ─────────────

// The migration takes the material OFF the entity because the terrain's own block named it — and since v23
// that block's material lives on the landscape root's LandscapeMaterialData. If that field were
// renamed, retyped, or removed, the migration would be deleting values with nowhere for their replacement
// to go — and nothing else in the build would notice, because a migration compiles perfectly well against
// a component that no longer has the field it made room for.
TEST( SceneTerrainMaterialMigration, TheLandscapeNamesItsMaterialAsAMaterialAssetHandle )
{
    const auto* field = FindField( "LandscapeMaterialData", "Material" );
    ASSERT_NE( field, nullptr )
         << "LandscapeMaterialData has no 'Material' field — the v6 -> v7 migration removes the "
            "terrain's inline material on the promise that this field replaced it.";

    EXPECT_EQ( field->Type, Desert::Reflection::FieldType::AssetHandle );
    EXPECT_TRUE( field->Meta.IsAsset );
    // "MaterialAsset" and not "SurfaceMaterialAsset": it is the string the scene serializer's asset
    // resolver branches on, so a different spelling would round-trip the handle as a raw number and the
    // reference would not survive a rename or a machine.
    EXPECT_EQ( field->Meta.AssetType, "MaterialAsset" );

    // Hidden from the auto-built Details on purpose — the Landscape Material entry draws the row itself, with an
    // Edit button that opens the Material Editor window. If this ever stops being Hidden the panel grows a SECOND
    // material control beside the first, which is the exact duplication M3 removed.
    EXPECT_TRUE( field->Meta.Hidden );
}

// ONE HANDLE, NOT A SLOT VECTOR — the decision this task was given, pinned so it is not quietly reopened.
// Terrain.shader is a single program of domain Terrain whose three splat layers are TEXTURE PARAMETERS of
// it. A vector here would promise one material per layer and nothing downstream could consume one.
TEST( SceneTerrainMaterialMigration, TheTerrainHasOneMaterialFieldAndNoSlotList )
{
    const auto* info = Desert::Reflection::ReflectionRegistry::Get().Find( "LandscapeMaterialData" );
    ASSERT_NE( info, nullptr );

    const auto assetFields =
         std::count_if( info->Fields.begin(), info->Fields.end(), []( const Desert::Reflection::FieldInfo& f )
                        { return f.Type == Desert::Reflection::FieldType::AssetHandle; } );
    EXPECT_EQ( assetFields, 1 );

    EXPECT_EQ( FindField( "LandscapeMaterialData", "MaterialSlots" ), nullptr )
         << "the terrain grew a slot LIST. Its three splat layers are texture parameters of one "
            "Terrain-domain program, not three materials — see the decision recorded in "
            "Docs/MaterialEditor/PLAN_STAGE3_ASSET_DOCUMENTS.md, M3.";
}

// THE FIELD NAMES THE FILE THAT READS IT — SettingConsumers' rule, applied to the one field this task
// added.
//
// SettingConsumers itself enumerates the sky, fog and cloud components only, so it does NOT fire on a new
// terrain field; the brief for this task expected it to, and it does not. The rule behind it is still the
// contract's (DC 1.3, a parameter must be wired end to end), so the claim is made here instead, about the
// field this suite is about: a terrain material that nothing reads would render nothing and look exactly
// like a terrain material that renders the shader's defaults.
TEST( SceneTerrainMaterialMigration, TheTerrainCollectorActuallyReadsTheMaterialField )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::string path = root + "Desert/Desert/Source/Engine/ECS/System/LandscapeECSSystem.cpp";
    std::ifstream     in( path, std::ios::binary );
    ASSERT_TRUE( in ) << path << " is missing — it is the file the terrain's material field is read in";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string source = ss.str();

    EXPECT_NE( source.find( "look.Material" ), std::string::npos )
         << "LandscapeECSSystem no longer mentions look.Material. Either the read moved (point this test "
            "at its new home) or it was deleted, and the field became a slot that moves nothing.";
    EXPECT_NE( source.find( "ResolveOverrides" ), std::string::npos )
         << "LandscapeECSSystem no longer resolves the material's values. Reading the handle and not its "
            "contents is the same dead setting one step later.";
}

// ── The repository, swept ──────────────────────────────────────────────────────────────────────────

// A scene the REPOSITORY ships, as opposed to one this machine happens to have on disk.
//
// `Scenes/Autosave/` holds `<name>_autosave.desce`, written periodically by the editor for crash
// recovery (EditorLayer.cpp:662-685) and gitignored precisely so a stale copy on an old schema is not a
// file in the repository. A `recursive_directory_iterator` over `Scenes/` descends into it anyway, so
// the sweep below picked up whatever the last editor session left behind — and those files are never
// migrated, because migration is for content we ship.
//
// That is invisible in CI, where a fresh checkout has no `Autosave/` at all, and it fails on every
// machine that has ever run the editor. Which is exactly what happened: green in the author's
// worktree, two failures on the integrator's, on a stale `Clouds_Demo_autosave.desce` still stamped
// SceneVersion 1. An environment-dependent gate is worse than no gate, because it teaches people that
// a red suite means nothing.
static bool IsShippedScene( const std::filesystem::path& p )
{
    for ( const auto& part : p )
        if ( part == "Autosave" )
            return false;
    return true;
}

// WHY A SWEEP AND NOT A LIST. DC 4.5 says every scene in the repository is converted by the task that
// changes the format, and this step DROPS what it finds — so "which scenes are affected" is not a question
// to answer once at review time. Every `.desce` is opened, and two things are asserted about each: it is
// stamped at the head, and no terrain entity in it carries an inline material. The second is what makes
// the first safe.
TEST( SceneTerrainMaterialMigration, NoSceneInTheRepositoryStillCarriesATerrainsInlineMaterial )
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

        auto parsed = rfl::json::read<SceneSerialized>( ss.str() );
        ASSERT_TRUE( parsed ) << entry.path().string() << " does not parse as a scene";
        ++seen;

        EXPECT_EQ( Desert::Assets::StatedVersion( parsed.value().Header, Desert::Assets::kSceneSchemaTag ),
                   kSceneVersion )
             << entry.path().filename().string()
             << " is not stamped at the current schema version. Run Tools/SceneMigrator over "
                "Editor/Resources/Assets/Scenes and commit the result (DC 4.5).";

        for ( const auto& e : parsed.value().Entities )
        {
            if ( !e.Components.get( "Terrain" ).has_value() )
                continue;
            EXPECT_FALSE( e.Components.get( "Material" ).has_value() )
                 << entry.path().filename().string() << ", entity '" << e.Tag.value_or( "Entity" )
                 << "': a terrain entity carries an inline Material component. The v6 -> v7 migration "
                    "DELETES that component and its values are not read anywhere else — author them on a "
                    "`.demat` and put it in Terrain > Material instead.";
        }
    }

    EXPECT_GT( seen, 0 ) << "no .desce files were found — the sweep asserted nothing";
}

// The same sweep's second half, stated as its own claim: running the migration over every shipped scene
// changes none of them. That is the difference between "the files were converted" and "the files happen to
// parse", and it is the check Tools/SceneMigrator --check makes in CI's place.
TEST( SceneTerrainMaterialMigration, EveryShippedSceneIsAlreadyMigratedAndMigrateSceneAgreesItHasNothingToDo )
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
        EXPECT_FALSE( MigrateScene( scene ).Changed() )
             << entry.path().filename().string()
             << " still has something to migrate. Run Tools/SceneMigrator and commit the result.";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
