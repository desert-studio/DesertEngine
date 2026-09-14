// The v17 -> v18 grass-generation migration, and the one relation that makes it more than a delete.
//
// Г25 removed the PROCEDURAL grass generator: a compute pass that culled a seeded grid of clumps and an
// indirect draw that grew hashed blade geometry from it. Grass is a mesh ASSET from here on, scattered by
// the Foliage paint tool, so the six `TerrainData` fields that steered the generator have nothing left to
// steer and are gone from the component, the reflection table, Details and the file.
//
// FIVE ARE DROPPED AND ONE IS CARRIED, and the carry is what this suite is really about.
// `EnableGrass` was doing TWO jobs. It switched the generator on, and — through `LayerModes.w` — it also
// gated the terrain's grass SPLAT LAYER in Terrain.shader, which is a ground TEXTURE and survives the cut.
// The layer's own authored mode, `GrassMode`, was reflected, serialized, drawn in Details and read by
// NOTHING: the shader never looked at `LayerModes.x`. So the only place a scene's opinion about its grass
// ground actually lived was the generator's switch, and deleting that switch without carrying it would
// have given every terrain that said "no grass" a green lawn.
//
// That is the defect shape this project keeps paying for — both ends of a chain correct, a middle link
// dropping the property — and it is why the assertions below are about the RELATION between what the
// migration writes and what the enum means, not about either alone.
//
// What is asserted:
//
//   1. The pure function removes all six generator keys and leaves every other key in place and in order.
//   2. The carry: EnableGrass=true -> GrassMode=Auto, false -> GrassMode=Off, and both are REPORTED.
//   3. A payload that already states GrassMode keeps what it states.
//   4. A payload that states NEITHER key gets no GrassMode at all, so the component default applies.
//   5. The shapes a hand-edited file has: EnableGrass of the wrong type, a Terrain payload that is not an
//      object, entities with no Terrain at all.
//   6. Idempotence — a second run over the same tree removes nothing and reports zero.
//   7. THE RELATION: the integers this migration writes for Auto and Off are the reflection registry's own
//      enumerators for TerrainLayerMode. Reorder the enum and this suite reddens instead of 86 scenes
//      quietly changing meaning.
//   8. The step is the head, and the head is what the engine requires.
//   9. The corpus: no shipped scene carries a generator key, and TerrainData no longer declares one —
//      the field and the file went together, which is the half of a removal that is usually forgotten.

#include <SceneMigration.hpp>

// Core::kSceneVersion -- the head this step is checked to sit at or below.
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    // The six keys the generator owned. Spelled here INDEPENDENTLY of the migration's own list: a test
    // that imported the list under test would agree with it by construction and assert nothing.
    constexpr const char* kGeneratorKeys[] = {
         "EnableGrass", "GrassDensity", "GrassHeight", "GrassBladesPerClump", "GrassWidth", "GrassBrightness",
    };

    // One entity carrying a "Terrain" payload built from the given fields.
    Assets::EntityData TerrainEntity( rfl::Generic::Object terrain, std::string tag = "Terrain" )
    {
        Assets::EntityData entity;
        entity.Tag                   = std::move( tag );
        entity.Components["Terrain"] = rfl::Generic( std::move( terrain ) );
        return entity;
    }

    // A v17 terrain payload: the surviving keys on both sides of the six, so a test can prove the rest of
    // the block survives untouched AND in order.
    rfl::Generic::Object V17Terrain( rfl::Generic enableGrass )
    {
        rfl::Generic::Object o;
        o["Size"]                = 30000.0;
        o["Resolution"]          = 64;
        o["EnableGrass"]         = std::move( enableGrass );
        o["GrassDensity"]        = 512;
        o["GrassHeight"]         = 60.0;
        o["GrassBladesPerClump"] = 6;
        o["GrassWidth"]          = 1.0;
        o["GrassBrightness"]     = 1.0;
        o["Material"]            = std::string( "Materials/M_Terrain.demat" );
        return o;
    }

    std::optional<rfl::Generic::Object> TerrainOf( const Assets::EntityData& entity )
    {
        const auto payload = entity.Components.get( "Terrain" );
        if ( !payload.has_value() )
            return std::nullopt;
        const auto fields = payload.value().to_object();
        if ( !fields.has_value() )
            return std::nullopt;
        return fields.value();
    }

    std::vector<std::string> KeysOf( const rfl::Generic::Object& o )
    {
        std::vector<std::string> keys;
        for ( const auto& [key, value] : o )
            keys.push_back( key );
        return keys;
    }

    std::optional<int64_t> ModeOf( const rfl::Generic::Object& o )
    {
        const auto mode = o.get( "GrassMode" );
        if ( !mode.has_value() )
            return std::nullopt;
        return mode.value().to_int64().value_or( -1 );
    }

    // The enumerator value the reflection table states for one name of TerrainLayerMode, which is what a
    // `.desce` stores (ReflectionSerializer writes FieldType::Enum as its integer).
    std::optional<int64_t> ReflectedLayerMode( const char* enumerator )
    {
        const auto* type = Reflection::ReflectionRegistry::Get().Find( "TerrainData" );
        if ( type == nullptr )
            return std::nullopt;
        for ( const auto& field : type->Fields )
        {
            if ( field.TypeName != "TerrainLayerMode" )
                continue;
            for ( const auto& value : field.EnumValues )
                if ( value.Name == enumerator )
                    return static_cast<int64_t>( value.Value );
        }
        return std::nullopt;
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

    // `Scenes/Autosave/` is the editor's gitignored crash-recovery copy, never migrated. A recursive sweep
    // would otherwise pass or fail on whatever the last editor session left behind.
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }
} // namespace

// ── The pure function ──────────────────────────────────────────────────────────────────────────────

TEST( SceneGrassGenerationMigration, AllSixGeneratorKeysLeaveAndNothingElseDoes )
{
    std::vector<Assets::EntityData> entities{ TerrainEntity( V17Terrain( true ) ) };

    const auto report = Migration::MigrateGrassGenerationV17ToV18( entities );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.KeysRemoved, 6 );

    const auto terrain = TerrainOf( entities[0] );
    ASSERT_TRUE( terrain.has_value() );

    for ( const char* key : kGeneratorKeys )
        EXPECT_FALSE( terrain->get( key ).has_value() ) << key << " survived the migration";

    // The surviving keys keep their ORDER as well as their values. The saver preserves key order, so a
    // migration that reshuffles a block makes every converted file a needless diff — and a needless diff
    // is how a real one stops being read.
    const std::vector<std::string> expected = { "Size", "Resolution", "Material", "GrassMode" };
    EXPECT_EQ( KeysOf( *terrain ), expected );

    EXPECT_EQ( terrain->get( "Size" ).value().to_double().value_or( 0.0 ), 30000.0 );
    EXPECT_EQ( terrain->get( "Material" ).value().to_string().value_or( "" ),
               std::string( "Materials/M_Terrain.demat" ) );
}

TEST( SceneGrassGenerationMigration, EveryRemovedKeyIsNamedWithItsValue )
{
    std::vector<Assets::EntityData> entities{ TerrainEntity( V17Terrain( true ) ) };

    const auto report = Migration::MigrateGrassGenerationV17ToV18( entities );

    ASSERT_EQ( report.RemovedNames.size(), 6u );
    // Named and not counted, because these were AUTHORED numbers: this report is the only place they are
    // ever said again (DC 1.4).
    EXPECT_NE( std::find( report.RemovedNames.begin(), report.RemovedNames.end(),
                          std::string( "Terrain.GrassDensity=512" ) ),
               report.RemovedNames.end() );
    EXPECT_NE( std::find( report.RemovedNames.begin(), report.RemovedNames.end(),
                          std::string( "Terrain.EnableGrass=true" ) ),
               report.RemovedNames.end() );
}

// ── The carry ──────────────────────────────────────────────────────────────────────────────────────

TEST( SceneGrassGenerationMigration, GrassOnBecomesTheAutoGroundLayer )
{
    std::vector<Assets::EntityData> entities{ TerrainEntity( V17Terrain( true ) ) };

    const auto report  = Migration::MigrateGrassGenerationV17ToV18( entities );
    const auto terrain = TerrainOf( entities[0] );
    ASSERT_TRUE( terrain.has_value() );

    ASSERT_TRUE( ModeOf( *terrain ).has_value() ) << "a terrain that had grass came back with no GrassMode";
    EXPECT_EQ( *ModeOf( *terrain ), ReflectedLayerMode( "Auto" ).value_or( -1 ) );
    ASSERT_EQ( report.CarriedNames.size(), 1u );
    EXPECT_EQ( report.CarriedNames[0], std::string( "Terrain.GrassMode=Auto (was EnableGrass=true)" ) );
}

TEST( SceneGrassGenerationMigration, GrassOffBecomesTheOffGroundLayer )
{
    // THE ONE THAT MATTERS. Without the carry this terrain comes back with GrassMode at its C++ default
    // (Auto) and grows a green lawn the scene never asked for — on 3 of the 4 terrains in this repository.
    std::vector<Assets::EntityData> entities{ TerrainEntity( V17Terrain( false ) ) };

    const auto report  = Migration::MigrateGrassGenerationV17ToV18( entities );
    const auto terrain = TerrainOf( entities[0] );
    ASSERT_TRUE( terrain.has_value() );

    ASSERT_TRUE( ModeOf( *terrain ).has_value() );
    EXPECT_EQ( *ModeOf( *terrain ), ReflectedLayerMode( "Off" ).value_or( -1 ) );
    ASSERT_EQ( report.CarriedNames.size(), 1u );
    EXPECT_EQ( report.CarriedNames[0], std::string( "Terrain.GrassMode=Off (was EnableGrass=false)" ) );
}

TEST( SceneGrassGenerationMigration, AnAuthoredGrassModeWinsOverTheCarry )
{
    // The authored value beats one derived from a switch that is going away.
    rfl::Generic::Object terrain = V17Terrain( true );
    terrain["GrassMode"]         = ReflectedLayerMode( "Manual" ).value_or( 1 );

    std::vector<Assets::EntityData> entities{ TerrainEntity( std::move( terrain ) ) };
    const auto                      report = Migration::MigrateGrassGenerationV17ToV18( entities );

    const auto out = TerrainOf( entities[0] );
    ASSERT_TRUE( out.has_value() );
    EXPECT_EQ( *ModeOf( *out ), ReflectedLayerMode( "Manual" ).value_or( 1 ) );
    EXPECT_TRUE( report.CarriedNames.empty() ) << "the migration overwrote a mode the file stated";
}

TEST( SceneGrassGenerationMigration, ATerrainThatStatedNeitherKeyGetsNoMode )
{
    // A file that never expressed an opinion still has none afterwards, so TerrainData's own default
    // applies. Writing one here would be the migration deciding an authored value on the author's behalf.
    rfl::Generic::Object bare;
    bare["Size"]       = 5000.0;
    bare["Resolution"] = 32;

    std::vector<Assets::EntityData> entities{ TerrainEntity( bare ) };
    const auto                      report = Migration::MigrateGrassGenerationV17ToV18( entities );

    const auto out = TerrainOf( entities[0] );
    ASSERT_TRUE( out.has_value() );
    EXPECT_FALSE( ModeOf( *out ).has_value() );
    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.KeysRemoved, 0 );
    EXPECT_TRUE( report.CarriedNames.empty() );
}

// ── The shapes a hand-edited file has ──────────────────────────────────────────────────────────────

TEST( SceneGrassGenerationMigration, AnEnableGrassOfTheWrongTypeReadsAsOff )
{
    // "Off" is the only reading that cannot invent grass a scene never asked for. The key is still
    // removed — the field it named does not exist any more, so there is no type for it to be right for.
    std::vector<Assets::EntityData> entities{ TerrainEntity( V17Terrain( std::string( "yes" ) ) ) };

    const auto report  = Migration::MigrateGrassGenerationV17ToV18( entities );
    const auto terrain = TerrainOf( entities[0] );
    ASSERT_TRUE( terrain.has_value() );

    EXPECT_FALSE( terrain->get( "EnableGrass" ).has_value() );
    ASSERT_TRUE( ModeOf( *terrain ).has_value() );
    EXPECT_EQ( *ModeOf( *terrain ), ReflectedLayerMode( "Off" ).value_or( -1 ) );
    ASSERT_EQ( report.CarriedNames.size(), 1u );
    EXPECT_NE( report.CarriedNames[0].find( "Off" ), std::string::npos );
}

TEST( SceneGrassGenerationMigration, ATerrainPayloadThatIsNotAnObjectIsLeftAlone )
{
    Assets::EntityData entity;
    entity.Tag                   = "Broken";
    entity.Components["Terrain"] = rfl::Generic( std::string( "not an object" ) );

    std::vector<Assets::EntityData> entities{ entity };
    const auto                      report = Migration::MigrateGrassGenerationV17ToV18( entities );

    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( entities[0].Components.get( "Terrain" ).value().to_string().value_or( "" ),
               std::string( "not an object" ) );
}

TEST( SceneGrassGenerationMigration, EntitiesWithoutATerrainAreUntouched )
{
    Assets::EntityData light;
    light.Tag = "Sun";
    rfl::Generic::Object payload;
    payload["Intensity"]               = 1.0;
    light.Components["DirectionLight"] = rfl::Generic( payload );

    std::vector<Assets::EntityData> entities{ light, TerrainEntity( V17Terrain( true ) ) };
    const auto                      report = Migration::MigrateGrassGenerationV17ToV18( entities );

    EXPECT_EQ( report.Entities, 1 );
    const auto stillThere = entities[0].Components.get( "DirectionLight" );
    ASSERT_TRUE( stillThere.has_value() );
    EXPECT_TRUE( stillThere.value().to_object().has_value() );
}

TEST( SceneGrassGenerationMigration, ASecondRunChangesNothing )
{
    std::vector<Assets::EntityData> entities{ TerrainEntity( V17Terrain( false ) ) };

    Migration::MigrateGrassGenerationV17ToV18( entities );
    const auto before = KeysOf( *TerrainOf( entities[0] ) );
    const auto mode   = ModeOf( *TerrainOf( entities[0] ) );

    const auto again = Migration::MigrateGrassGenerationV17ToV18( entities );

    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( again.KeysRemoved, 0 );
    EXPECT_TRUE( again.CarriedNames.empty() );
    EXPECT_EQ( KeysOf( *TerrainOf( entities[0] ) ), before );
    EXPECT_EQ( ModeOf( *TerrainOf( entities[0] ) ), mode );
}

// ── The relation, and the version ──────────────────────────────────────────────────────────────────

TEST( SceneGrassGenerationMigration, TheCarriedIntegersAreTheEnumsOwnValues )
{
    // The migration states Auto=0 and Off=2 as literals, because it is a pure function and may not reach
    // for the reflection registry (a global). That is the right call and it is also exactly how a
    // migration comes to mean something different from the enum it writes for. So the two are compared
    // HERE: reorder TerrainLayerMode and this reddens, instead of 86 scenes changing meaning quietly.
    ASSERT_TRUE( ReflectedLayerMode( "Auto" ).has_value() ) << "TerrainData no longer reflects a "
                                                               "TerrainLayerMode field";
    EXPECT_EQ( *ReflectedLayerMode( "Auto" ), 0 );
    EXPECT_EQ( *ReflectedLayerMode( "Off" ), 2 );

    // And the round trip, through the function rather than through the constants.
    std::vector<Assets::EntityData> on{ TerrainEntity( V17Terrain( true ) ) };
    std::vector<Assets::EntityData> off{ TerrainEntity( V17Terrain( false ) ) };
    Migration::MigrateGrassGenerationV17ToV18( on );
    Migration::MigrateGrassGenerationV17ToV18( off );
    EXPECT_EQ( ModeOf( *TerrainOf( on[0] ) ), ReflectedLayerMode( "Auto" ) );
    EXPECT_EQ( ModeOf( *TerrainOf( off[0] ) ), ReflectedLayerMode( "Off" ) );
}

// THE HEAD ASSERTION TRAVELS WITH THE NEWEST STEP, and it left here when Г26 added v19. It was
// `EXPECT_EQ( kSceneVersionGrassGeneration, Core::kSceneVersion )`, which is only true while THIS is
// the last step; holding it here would redden this suite the day the next step lands, for a reason
// that has nothing to do with grass. It now lives in Desert/Tests/Tools/SceneRetiredKeysMigration,
// beside the step that is currently the head. What stays is this step's own generation and its place
// above its predecessor -- facts about the grass retirement, which do not move.
// THE HEAD ASSERTION TRAVELS WITH THE NEWEST STEP, and it left here twice -- Г26 moved it out when it
// added the wind retirement, and Ю15 moved it out independently when it added the text-key sigil. Both
// arrived at the same conclusion without seeing the other, which is the strongest kind of agreement:
// "the last step and the engine's required generation are one number" is a claim about the CHAIN, not
// about grass, and a copy of it in every earlier suite turns one version bump into N red suites for one
// reason. That teaches whoever bumps next to edit numbers rather than read them. It lives with whichever
// suite owns the head -- SceneTextKeySigilMigration today.
//
// What IS this suite's to assert: its own step's number, and that the step sits after the one it was
// added behind and at or below the head. Those are facts about the grass retirement and do not move.
TEST( SceneGrassGenerationMigration, TheStepSitsWhereItWasAddedInTheChain )
{
    EXPECT_EQ( Migration::kSceneVersionGrassGeneration, 18 );
    EXPECT_GT( Migration::kSceneVersionGrassGeneration, Migration::kSceneVersionServiceAssetRoot );
    EXPECT_LE( Migration::kSceneVersionGrassGeneration, Core::kSceneVersion )
         << "a step cannot sit above the head the loader requires";
}

TEST( SceneGrassGenerationMigration, TheChainRunsTheStepAndStampsTheHead )
{
    Core::SceneSerialized scene;
    scene.SceneName    = "GrassProbe";
    scene.SceneVersion = Migration::kSceneVersionServiceAssetRoot;
    scene.UnitVersion  = Core::kUnitVersion;
    scene.Entities.push_back( TerrainEntity( V17Terrain( true ) ) );

    const auto report = Migration::MigrateScene( scene );

    EXPECT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.GrassGenerationRaised );
    EXPECT_EQ( report.GrassGeneration.KeysRemoved, 6 );
    EXPECT_EQ( scene.SceneVersion.value_or( 0 ), Core::kSceneVersion );

    // And a file already at the head does not run it again.
    const auto second = Migration::MigrateScene( scene );
    EXPECT_FALSE( second.GrassGenerationRaised );
}

// ── The corpus ─────────────────────────────────────────────────────────────────────────────────────

TEST( SceneGrassGenerationMigration, NoShippedSceneCarriesAGeneratorKey )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

    const std::filesystem::path scenes = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string();

    int checked = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        std::ifstream     in( entry.path(), std::ios::binary );
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        ++checked;

        for ( const char* key : kGeneratorKeys )
        {
            EXPECT_EQ( text.find( std::string( "\"" ) + key + "\"" ), std::string::npos )
                 << entry.path().filename().string() << " still states " << key
                 << " — run Tools/SceneMigrator over Editor/Resources/Assets and commit the result (DC 4.5).";
        }
    }
    EXPECT_GT( checked, 0 ) << "the sweep found no scenes at all, so it proved nothing";
}

TEST( SceneGrassGenerationMigration, TerrainDataNoLongerDeclaresAGeneratorField )
{
    // The half of a removal that is usually forgotten: the files were cleaned AND the struct was. If this
    // passes while the one above fails, the corpus was not re-run; if it fails on its own, somebody put
    // the generator back and the migration is now deleting a live field.
    const auto* type = Reflection::ReflectionRegistry::Get().Find( "TerrainData" );
    ASSERT_NE( type, nullptr );

    for ( const auto& field : type->Fields )
        for ( const char* key : kGeneratorKeys )
            EXPECT_NE( field.Name, std::string( key ) )
                 << "TerrainData still declares " << key << ", which the v17 -> v18 migration deletes from "
                 << "every file — the field and the file must go together";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
