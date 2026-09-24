// A v1 FILE GOES IN AND A FILE THIS ENGINE WILL LOAD COMES OUT. End to end, through MigrateScene — the one
// entry point Tools/SceneMigrator calls on every .desce it is pointed at.
//
// WHY THIS SUITE EXISTS SEPARATELY FROM THE NINE BESIDE IT. Each of those tests ONE step: the sky, the
// units, the tonemapper, the four cloud steps, the terrain material, the material path. Every one of them
// is green on a tree that never travels further than its own step. What none of them says is that a file
// entering the chain at the BOTTOM comes out at the top — and that is the only claim that matters now,
// because the engine no longer migrates anything: an old file either survives this function in one run or
// it does not open at all. The chain is also not independent — v3->v4 writes the "Species" key v4->v5
// reads, and v4->v5 writes the "CloudType" key v5->v6 renames — so "each step works" and "the sequence
// works" are genuinely different statements.
//
// §4.4, literally: in the old property tree, out the new one. No files, no GPU, no globals — the assets
// root is passed as a parameter, which is why the v7->v8 step can be driven here with a root of this
// suite's own choosing instead of whatever content root a process happens to have open.

#include <SceneMigration.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Core/AssetHandle.hpp>

// For TonemapOperator::Reinhard, which the v1 -> v2 step pins and this suite reads back. The engine owns
// the enum; naming its value here rather than the integer 0 is what keeps the assertion true if the
// enumerators are ever reordered.
#include <Engine/Core/SceneSettings.hpp>
// For the shipped presets' names and the path they are composed into — the same header the v4 -> v5 step
// reads them from, so this suite cannot hold a second spelling of the library that disagrees with it.
#include <Engine/Assets/CloudTypeData.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <string>

using Desert::Migration::kSceneVersion;
using Desert::Migration::kUnitVersion;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;

namespace
{
    const std::filesystem::path kAssetsRoot = "Resources/Assets";

    /// Reads a component payload back off an entity, or fails the assertion that it is there at all.
    rfl::Generic::Object Payload( const Desert::Assets::EntityData& entity, const char* component )
    {
        const auto found = entity.Components.get( component );
        EXPECT_TRUE( found.has_value() ) << component << " is not on this entity";
        if ( !found.has_value() )
            return {};
        const auto object = found.value().to_object();
        EXPECT_TRUE( object.has_value() ) << component << " is not an object";
        return object.value_or( rfl::Generic::Object{} );
    }

    bool HasKey( const rfl::Generic::Object& object, const std::string& key )
    {
        for ( auto it = object.begin(); it != object.end(); ++it )
            if ( it->first == key )
                return true;
        return false;
    }

    std::string StringAt( const rfl::Generic::Object& object, const char* key )
    {
        const auto found = object.get( key );
        if ( !found.has_value() )
            return "<absent>";
        return found.value().to_string().value_or( "<not a string>" );
    }

    double NumberAt( const rfl::Generic::Object& object, const char* key )
    {
        const auto found = object.get( key );
        if ( !found.has_value() )
            return -1.0;
        if ( const auto d = found.value().to_double(); d.has_value() )
            return d.value();
        if ( const auto i = found.value().to_int64(); i.has_value() )
            return static_cast<double>( i.value() );
        return -1.0;
    }

    /// A SCENE AS IT WAS ACTUALLY WRITTEN AT v1: the sky has already moved into its own payload (that is
    /// what v1 MEANS), the units were never stamped, and every later step still has work waiting for it.
    ///
    /// Written out by hand rather than serialised from today's structs, for the reason the fixture exists:
    /// serialising a current component would write today's keys and prove nothing. What a v1 file has is
    /// the old keys and the ABSENCE of the new ones.
    SceneSerialized SceneAtV1()
    {
        SceneSerialized scene;
        scene.SceneName    = "Old";
        scene.SceneVersion = 1;
        // UnitVersion deliberately ABSENT: a v1 file predates the stamp entirely, and absent-is-zero is the
        // rule the whole units axis turns on.

        // A cloud layer as v1 carried it: four bake settings (dropped at v3), the authored shell and the
        // scalar type with its variance (dropped/translated at v4).
        {
            Desert::Assets::EntityData clouds;
            clouds.Tag = "Sky";

            rfl::Generic::Object payload;
            payload["Enabled"]                   = true;
            payload["WeatherSeed"]               = static_cast<int64_t>( 7 );
            payload["WeatherOctaves"]            = static_cast<int64_t>( 4 );
            payload["DetailSeed"]                = static_cast<int64_t>( 11 );
            payload["DetailOctaves"]             = static_cast<int64_t>( 3 );
            payload["LayerBottomAltitude"]       = 2200.0;
            payload["LayerThickness"]            = 3600.0;
            payload["CloudTypeVariance"]         = 0.25;
            payload["CloudType"]                 = 0.6; // the component's own former default
            payload["Coverage"]                  = 0.5;
            clouds.Components["VolumetricCloud"] = rfl::Generic( std::move( payload ) );

            scene.Entities.push_back( std::move( clouds ) );
        }

        // A terrain with the inline Material component that v7 removes, and an absolute material path that
        // v8 makes relative. Both on one entity on purpose: the two steps are ordered against each other
        // (v6->v7 deletes the payload v7->v8 must not waste time rewriting), and one entity is where that
        // ordering can actually go wrong.
        {
            Desert::Assets::EntityData terrain;
            terrain.Tag         = "Ground";
            terrain.Translation = glm::vec3( 1.0f, 2.0f, 3.0f ); // metres
            terrain.Scale       = glm::vec3( 1.0f, 1.0f, 1.0f ); // metres

            rfl::Generic::Object terrainPayload;
            terrainPayload["Size"]        = 100.0; // metres
            terrainPayload["HeightScale"] = 20.0;  // metres
            terrainPayload["Material"] =
                 std::string( "/Users/somebody/Proj/Editor/Resources/Assets/Materials/Ground.demat" );
            terrain.Components["Terrain"] = rfl::Generic( std::move( terrainPayload ) );

            rfl::Generic::Object row;
            row["Name"] = std::string( "GrassTexture" );
            rfl::Generic::Array textures;
            textures.push_back( rfl::Generic( row ) );

            rfl::Generic::Object material;
            material["Textures"]           = std::move( textures );
            terrain.Components["Material"] = rfl::Generic( std::move( material ) );

            scene.Entities.push_back( std::move( terrain ) );
        }

        // A mesh whose material slots are absolute paths — the v8 case, in its list form.
        {
            Desert::Assets::EntityData mesh;
            mesh.Tag = "Rock";

            rfl::Generic::Array slots;
            slots.push_back( rfl::Generic(
                 std::string( "/Users/somebody/Proj/Editor/Resources/Assets/Materials/Rock.demat" ) ) );

            rfl::Generic::Object payload;
            payload["MaterialPaths"]      = std::move( slots );
            mesh.Components["StaticMesh"] = rfl::Generic( std::move( payload ) );

            scene.Entities.push_back( std::move( mesh ) );
        }

        return scene;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// THE CLAIM
// ---------------------------------------------------------------------------------------------------

// THE STAMP. Whatever else happened, a tree that has been through this function is one the engine's gate
// accepts — which since the migrations left the runtime is the difference between a file that opens and a
// file that does not.
TEST( SceneMigratorEndToEnd, AV1SceneComesOutStampedAtBothHeads )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    EXPECT_TRUE( report.Changed() );
    ASSERT_TRUE( scene.Header.has_value() );
    ASSERT_TRUE( scene.Header.has_value() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion );
}

// EVERY STEP FROM v1 TO THE HEAD RAN, and is reported as having run. A chain that stamps the head while
// silently skipping a step in the middle produces a file the gate accepts and the renderer misreads, which
// is worse than a refusal.
TEST( SceneMigratorEndToEnd, EveryStepAboveV1IsReportedAsHavingRun )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    EXPECT_FALSE( report.SkyRaised ) << "the fixture is already at v1, so the sky step must not re-run";
    EXPECT_TRUE( report.UnitsRaised );
    EXPECT_TRUE( report.TonemapperRaised );
    EXPECT_TRUE( report.CloudNoiseRaised );
    EXPECT_TRUE( report.CloudSpeciesRaised );
    EXPECT_TRUE( report.CloudTypeRaised );
    EXPECT_TRUE( report.CloudSetRaised );
    EXPECT_TRUE( report.TerrainMaterialRaised );
    EXPECT_TRUE( report.MaterialPathRaised );
    EXPECT_TRUE( report.CloudMaterialRaised );
}

// THE CLOUD CHAIN, WHICH IS THE PART THAT CANNOT BE PROVED ONE STEP AT A TIME. A v1 scalar `CloudType` of
// 0.6 has to become the SPECIES it names (v4), then the PATH of the shipped `.decloudtype` that species
// ships as (v5), then the FIRST SLOT of the set (v6). Three steps, each reading a key the one before it
// wrote, and a break anywhere in the middle leaves a layer with no cloud in it.
TEST( SceneMigratorEndToEnd, TheScalarCloudTypeBecomesAPathInTheFirstSlotOfTheSet )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    const rfl::Generic::Object clouds = Payload( scene.Entities[0], "VolumetricCloud" );

    // It left the payload entirely — the chain is four long since O1, and the last link lifts the slot
    // into the cloud MATERIAL the payload now names.
    EXPECT_FALSE( HasKey( clouds, "CloudType" ) ) << "the single-type key survived the rename to a set";
    EXPECT_FALSE( HasKey( clouds, "CloudType1" ) ) << "the slot survived the move into the material";
    ASSERT_TRUE( HasKey( clouds, "Material" ) );
    ASSERT_EQ( report.CloudMaterial.Materials.size(), 1u );

    // And it is the CONGESTUS that the old scalar 0.6 named — that is the claim, and it is what a break
    // anywhere in the chain would change: the quarters could have landed on stratus, or the species integer
    // could have failed to become a path at all.
    //
    // Spelled through the same constant the migration composes from rather than as a literal. Not to make
    // the assertion easier: the stem on disk is `Cumulus_Congestus`, this suite first asserted
    // `CumulusCongestus`, and the test failed against code that was correct. A hand-copied name is a second
    // statement of the library's spelling and it was wrong the first time it was written. What is NOT
    // tautological here is the index — which of the four presets the scalar chose — and that is the part
    // this test exists for.
    const auto material = rfl::json::read<Desert::Assets::MaterialData>( report.CloudMaterial.Materials[0].Json );
    ASSERT_TRUE( material );
    EXPECT_EQ( material.value().GetTexture( "CloudType1" ),
               static_cast<uint64_t>( Common::AssetHandle::FromKey(
                    "assets:" +
                    Desert::Assets::CloudTypeAssetRelativePath( Desert::Assets::kCloudTypeCumulusCongestus ) ) ) );

    // The other three slots are ABSENT — from the payload and from the material alike, which is how both
    // formats spell "the empty handle".
    EXPECT_FALSE( HasKey( clouds, "CloudType2" ) );
    EXPECT_EQ( material.value().GetTexture( "CloudType2" ), 0u );
    EXPECT_EQ( material.value().GetTexture( "CloudType3" ), 0u );
    EXPECT_EQ( material.value().GetTexture( "CloudType4" ), 0u );
}

// The keys with nowhere to go are gone, and the ones that still mean something are untouched.
TEST( SceneMigratorEndToEnd, TheRetiredCloudKeysAreGoneAndTheSurvivingOnesAreNot )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    const rfl::Generic::Object clouds = Payload( scene.Entities[0], "VolumetricCloud" );

    for ( const char* dropped : { "WeatherSeed", "WeatherOctaves", "DetailSeed", "DetailOctaves",
                                  "LayerBottomAltitude", "LayerThickness", "CloudTypeVariance", "Species" } )
        EXPECT_FALSE( HasKey( clouds, dropped ) ) << dropped << " survived the chain";

    EXPECT_TRUE( HasKey( clouds, "Enabled" ) );
    // Coverage SURVIVES the chain, but as the material's parameter: v11 -> v12 lifts every look field
    // into the `.demat`, values verbatim.
    EXPECT_FALSE( HasKey( clouds, "Coverage" ) ) << "a look field survived on the payload";
    ASSERT_EQ( report.CloudMaterial.Materials.size(), 1u );
    {
        const auto material =
             rfl::json::read<Desert::Assets::MaterialData>( report.CloudMaterial.Materials[0].Json );
        ASSERT_TRUE( material );
        EXPECT_DOUBLE_EQ( material.value().GetFloat( "Coverage" ), 0.5 );
    }
}

// THE TERRAIN'S INLINE MATERIAL IS GONE AND WHAT IT HELD IS NAMED. This step DROPS values, so the report
// has to carry enough for somebody to re-author them — a count alone would say "3 values dropped" and
// never mention that the grass texture was one of them (§1.4).
TEST( SceneMigratorEndToEnd, TheTerrainsInlineMaterialIsRemovedAndItsContentsAreNamed )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    EXPECT_FALSE( scene.Entities[1].Components.get( "Material" ).has_value() );
    EXPECT_EQ( report.TerrainMaterial.Entities, 1 );
    EXPECT_EQ( report.TerrainMaterial.Textures, 1 );
    ASSERT_EQ( report.TerrainMaterial.DroppedNames.size(), 1u );
    EXPECT_EQ( report.TerrainMaterial.DroppedNames[0], "GrassTexture" );
}

// The material paths lost the home directory they were carrying, in BOTH shapes a scene can name one:
// `Terrain.Material` (a single string) and `StaticMesh.MaterialPaths` (a list).
TEST( SceneMigratorEndToEnd, BothShapesOfMaterialPathComeOutRelativeToTheAssetsRoot )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    EXPECT_EQ( StringAt( Payload( scene.Entities[1], "Terrain" ), "Material" ), "Materials/Ground.demat" );

    const rfl::Generic::Object mesh  = Payload( scene.Entities[2], "StaticMesh" );
    const auto                 slots = mesh.get( "MaterialPaths" ).value().to_array();
    ASSERT_TRUE( slots.has_value() );
    ASSERT_EQ( slots.value().size(), 1u );
    EXPECT_EQ( slots.value()[0].to_string().value_or( "" ), "Materials/Rock.demat" );

    EXPECT_EQ( report.MaterialPath.Paths, 2 );
    EXPECT_TRUE( report.MaterialPath.OutsideNames.empty() );
}

// The lengths were authored in metres and come out in centimetres — once. The transform and the component
// fields are separate code paths and both are checked, because "the units migration ran" is not the same
// statement as "it reached this field".
TEST( SceneMigratorEndToEnd, TheMetresEraLengthsAreScaledExactlyOnce )
{
    SceneSerialized scene = SceneAtV1();
    MigrateScene( scene, kAssetsRoot );

    ASSERT_TRUE( scene.Entities[1].Translation.has_value() );
    EXPECT_FLOAT_EQ( scene.Entities[1].Translation->x, 100.0f );
    EXPECT_FLOAT_EQ( scene.Entities[1].Translation->z, 300.0f );

    const rfl::Generic::Object terrain = Payload( scene.Entities[1], "Terrain" );
    EXPECT_DOUBLE_EQ( NumberAt( terrain, "Size" ), 10000.0 );
    EXPECT_DOUBLE_EQ( NumberAt( terrain, "HeightScale" ), 2000.0 );
}

// The tonemapper is PINNED to the operator the file was authored on rather than left to the new default.
// An absent key means "the C++ default", so on the day the default moved every silent file would have been
// re-graded through a curve its author never chose.
TEST( SceneMigratorEndToEnd, TheTonemapperIsPinnedInASettingsBlockCreatedForIt )
{
    SceneSerialized scene  = SceneAtV1();
    const auto      report = MigrateScene( scene, kAssetsRoot );

    EXPECT_TRUE( report.Tonemap.SettingsCreated );
    EXPECT_TRUE( report.Tonemap.OperatorPinned );

    ASSERT_TRUE( scene.Settings.has_value() );
    const auto settings = scene.Settings->to_object();
    ASSERT_TRUE( settings.has_value() );
    EXPECT_EQ( static_cast<int>( NumberAt( settings.value(), "Tonemapper" ) ),
               static_cast<int>( Desert::Core::TonemapOperator::Reinhard ) );
}

// ---------------------------------------------------------------------------------------------------
// IDEMPOTENCE — the property that makes running the tool over a whole repository safe
// ---------------------------------------------------------------------------------------------------

// A SECOND RUN CHANGES NOTHING, and this is asserted on the SERIALISED BYTES rather than field by field,
// because "nothing changed" checked one field at a time is only ever a claim about the fields somebody
// remembered. The units step is the one that could not survive a re-run — it is a blind x100 — which is
// exactly why it is gated on its own integer and why the stamp above has to happen.
TEST( SceneMigratorEndToEnd, RunningTheChainTwiceLeavesTheTreeByteIdentical )
{
    SceneSerialized scene = SceneAtV1();
    MigrateScene( scene, kAssetsRoot );
    const std::string afterFirst = rfl::json::write( scene );

    const auto second = MigrateScene( scene, kAssetsRoot );
    EXPECT_FALSE( second.Changed() ) << "the second run reported work, so the first did not finish";
    EXPECT_EQ( rfl::json::write( scene ), afterFirst );
}

// And a file already at the head is left alone entirely — the case that is true of all 50 scenes in this
// repository, and the reason the tool is safe to point at the whole tree.
TEST( SceneMigratorEndToEnd, ASceneAlreadyAtTheHeadIsNotTouched )
{
    SceneSerialized scene = SceneAtV1();
    MigrateScene( scene, kAssetsRoot );

    SceneSerialized   current = scene;
    const std::string before  = rfl::json::write( current );

    const auto report = MigrateScene( current, kAssetsRoot );
    EXPECT_FALSE( report.Changed() );
    EXPECT_EQ( rfl::json::write( current ), before );
}

// ---------------------------------------------------------------------------------------------------
// MISSING AND MALFORMED — §4.4 asks for both, and neither may take the chain down
// ---------------------------------------------------------------------------------------------------

// AN EMPTY v1 SCENE still comes out stamped. A scene with no entities at version 1 is still a scene at
// version 1, and leaving it unstamped is how a file ends up refused forever by a tool run that "succeeded".
TEST( SceneMigratorEndToEnd, AnEmptyV1SceneIsStillStampedToTheHead )
{
    SceneSerialized scene;
    scene.SceneVersion = 1;

    MigrateScene( scene, kAssetsRoot );

    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion );
    EXPECT_TRUE( scene.Entities.empty() );
}

// PAYLOADS THAT ARE NOT OBJECTS do not take the chain down, and the entity keeps whatever it had. A
// hand-edited scene can contain anything, and a migration that threw or wrote garbage on one bad key would
// take the other forty-nine scenes of a whole-repository run with it.
TEST( SceneMigratorEndToEnd, MalformedComponentPayloadsAreSurvivedAndLeftAsTheyAre )
{
    SceneSerialized scene;
    scene.SceneVersion = 1;

    Desert::Assets::EntityData broken;
    broken.Tag                           = "Broken";
    broken.Components["VolumetricCloud"] = rfl::Generic( std::string( "not an object" ) );
    broken.Components["Terrain"]         = rfl::Generic( static_cast<int64_t>( 42 ) );
    broken.Components["StaticMesh"]      = rfl::Generic( true );
    scene.Entities.push_back( std::move( broken ) );

    MigrateScene( scene, kAssetsRoot );

    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    EXPECT_EQ( scene.Entities[0].Components.get( "VolumetricCloud" ).value().to_string().value_or( "" ),
               "not an object" );
    EXPECT_EQ( scene.Entities[0].Components.get( "Terrain" ).value().to_int64().value_or( 0 ), 42 );
}

// A KEY OF THE WRONG TYPE inside an otherwise good payload costs that key and nothing else: the layer keeps
// the built-in default and the rest of the scene migrates around it.
TEST( SceneMigratorEndToEnd, AMalformedCloudTypeCostsTheLayerItsTypeAndNothingElse )
{
    SceneSerialized scene;
    scene.SceneVersion = 1;

    Desert::Assets::EntityData clouds;
    clouds.Tag = "Sky";

    rfl::Generic::Object payload;
    payload["Enabled"]                   = true;
    payload["CloudType"]                 = std::string( "cumulus, obviously" ); // not a number
    payload["Coverage"]                  = 0.5;
    clouds.Components["VolumetricCloud"] = rfl::Generic( std::move( payload ) );
    scene.Entities.push_back( std::move( clouds ) );

    MigrateScene( scene, kAssetsRoot );

    const rfl::Generic::Object out = Payload( scene.Entities[0], "VolumetricCloud" );
    EXPECT_FALSE( HasKey( out, "CloudType" ) ) << "the unreadable value was carried forward anyway";
    EXPECT_FALSE( HasKey( out, "CloudType1" ) )
         << "a value nobody could read became a cloud type, which is a guess about intent";
    EXPECT_TRUE( HasKey( out, "Enabled" ) );
    // The good value beside it still arrives — in the material, where the look lives since O1.
    EXPECT_FALSE( HasKey( out, "Coverage" ) );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
}

// A MATERIAL PATH OUTSIDE THE ASSETS ROOT has no project-relative form to have. It is left exactly as it is
// and NAMED, because the scene still will not open on another machine and a count would not say which slot
// to re-point.
TEST( SceneMigratorEndToEnd, APathOutsideTheAssetsRootIsLeftAloneAndNamed )
{
    SceneSerialized scene;
    scene.SceneVersion = 1;
    scene.UnitVersion  = kUnitVersion; // keep the units axis out of this one

    Desert::Assets::EntityData mesh;
    mesh.Tag = "Imported";

    rfl::Generic::Array slots;
    slots.push_back( rfl::Generic( std::string( "/opt/elsewhere/Shared.demat" ) ) );

    rfl::Generic::Object payload;
    payload["MaterialPaths"]      = std::move( slots );
    mesh.Components["StaticMesh"] = rfl::Generic( std::move( payload ) );
    scene.Entities.push_back( std::move( mesh ) );

    const auto report = MigrateScene( scene, kAssetsRoot );

    const rfl::Generic::Object out  = Payload( scene.Entities[0], "StaticMesh" );
    const auto                 kept = out.get( "MaterialPaths" ).value().to_array();
    ASSERT_TRUE( kept.has_value() );
    EXPECT_EQ( kept.value()[0].to_string().value_or( "" ), "/opt/elsewhere/Shared.demat" );

    ASSERT_EQ( report.MaterialPath.OutsideNames.size(), 1u );
    EXPECT_NE( report.MaterialPath.OutsideNames[0].find( "Imported" ), std::string::npos )
         << report.MaterialPath.OutsideNames[0];
    EXPECT_NE( report.MaterialPath.OutsideNames[0].find( "/opt/elsewhere/Shared.demat" ), std::string::npos )
         << report.MaterialPath.OutsideNames[0];
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
