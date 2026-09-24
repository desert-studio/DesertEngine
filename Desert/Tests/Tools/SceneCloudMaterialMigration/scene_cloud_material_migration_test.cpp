// The cloud-material migration, scene v11 -> v12, and the relations it exists to protect.
//
// O1: the cloud LOOK is a material. Thirty-three keys leave the "VolumetricCloud" payload for a `.demat`
// on the Volume-domain cloud shader — values verbatim, asset paths as the same path-derived handles the
// runtime mints — and the payload gains "Material" naming the file. Three properties carry the step:
//
//   * THE SKY MUST NOT MOVE. A stated value arrives in the material exactly as stated; an ABSENT one
//     stays absent, because the schema's defaults are the old component defaults digit for digit
//     (Desert/Tests/Engine/CloudMaterialSchema pins that), so "the default" keeps meaning the same sky.
//   * NOTHING IS DROPPED SILENTLY. A value of the wrong shape leaves the payload — the runtime knows
//     nothing about the old format — but its NAME comes back in the report, out loud (§1.4).
//   * IT IS DETERMINISTIC, file bytes included: the MaterialId is derived from the file's own relative
//     path, so two runs produce byte-identical scenes AND byte-identical materials, and the repository
//     diff of the migration shows only real change.
//
// Everything below runs on the parsed tree. No GPU, no filesystem — the material FILES are part of the
// return value, and the tool is what writes them.

#include <SceneMigration.hpp>

#include <Engine/Assets/MaterialData.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using Desert::Migration::CloudMaterialMigrationReport;
using Desert::Migration::kSceneVersion;
using Desert::Migration::kSceneVersionCloudMaterial;
using Desert::Migration::kSceneVersionSSRUnits;
using Desert::Migration::MigrateCloudMaterialV11ToV12;
using Desert::Migration::MigrateScene;
using Desert::Migration::SceneSerialized;

namespace
{
    // A v11 cloud payload as Clouds_Protocol spells it: every look key stated, a representative set of
    // the keys that STAY, one authored type and an empty second slot.
    rfl::Generic::Object FullPayloadV11()
    {
        rfl::Generic::Object o;
        o["Enabled"]                          = true;
        o["CloudType1"]                       = std::string( "Clouds/Types/Cumulus_Congestus.decloudtype" );
        o["CloudType2"]                       = std::string( "" );
        o["CloudType3"]                       = std::string( "" );
        o["CloudType4"]                       = std::string( "" );
        o["PlanetRadius"]                     = 6360.0;
        o["MaxViewDistance"]                  = 6000000.0;
        o["Coverage"]                         = 0.762;
        o["CoverageContrast"]                 = 1.0;
        o["WeatherTileSize"]                  = 1200000.0;
        o["Seed"]                             = static_cast<int64_t>( 7 );
        o["PlacementDensity"]                 = 1.75;
        o["PlacementScatter"]                 = 1.0;
        o["PlacementSizeVariety"]             = 0.75;
        o["PatchTileSize"]                    = 2100000.0;
        o["PatchStrength"]                    = 0.6;
        o["CloudLayout"]                      = std::string( "Clouds/Layouts/Layout_Stripe.dclayout" );
        o["LayoutPatternStrength"]            = 1.0;
        o["LayoutMaskStrength"]               = 1.0;
        o["LayoutRepeats"]                    = static_cast<int64_t>( 2 );
        o["LayoutRotation"]                   = static_cast<int64_t>( 1 );
        o["LayoutOffset"]                     = std::vector<rfl::Generic>{ 100000.0, -200000.0 };
        o["DetailTileSize"]                   = 100000.0;
        o["DetailStrength"]                   = 0.65;
        o["DensityScale"]                     = 1.0;
        o["ExtinctionScale"]                  = 8.0;
        o["ScatteringAlbedo"]                 = 0.98;
        o["PhaseG"]                           = 0.8;
        o["PhaseGBackward"]                   = 0.1667;
        o["PhaseBlend"]                       = 0.575;
        o["AmbientOcclusionStrength"]         = 1.0;
        o["MultiScatterOctaves"]              = static_cast<int64_t>( 3 );
        o["MultiScatterContribution"]         = 0.667;
        o["MultiScatterOcclusion"]            = 0.25;
        o["MultiScatterEccentricity"]         = 0.18;
        o["AmbientScale"]                     = std::vector<rfl::Generic>{ 1.0, 0.9, 0.8 };
        o["SkyOcclusionVolume"]               = true;
        o["PerSampleAtmosphereTransmittance"] = false;
        o["MaxSteps"]                         = static_cast<int64_t>( 256 );
        o["WindSpeed"]                        = 3000.0;
        return o;
    }

    Desert::Assets::EntityData EntityWith( rfl::Generic::Object payload )
    {
        Desert::Assets::EntityData entity;
        entity.Tag                           = "Sky";
        entity.Components["VolumetricCloud"] = rfl::Generic( std::move( payload ) );
        return entity;
    }

    rfl::Generic::Object CloudPayloadOf( const Desert::Assets::EntityData& entity )
    {
        const auto payload = entity.Components.get( "VolumetricCloud" );
        EXPECT_TRUE( payload.has_value() );
        const auto object = payload.value().to_object();
        EXPECT_TRUE( object.has_value() );
        return object.value_or( rfl::Generic::Object{} );
    }

    bool Has( const rfl::Generic::Object& o, const std::string& key )
    {
        for ( const auto& [k, v] : o )
            if ( k == key )
                return true;
        return false;
    }

    Desert::Assets::MaterialData MaterialOf( const CloudMaterialMigrationReport& report, size_t index = 0 )
    {
        EXPECT_GT( report.Materials.size(), index );
        if ( report.Materials.size() <= index )
            return {};
        const auto parsed = rfl::json::read<Desert::Assets::MaterialData>( report.Materials[index].Json );
        EXPECT_TRUE( parsed ) << report.Materials[index].Json;
        return parsed ? parsed.value() : Desert::Assets::MaterialData{};
    }
} // namespace

TEST( SceneCloudMaterialMigration, EveryStatedLookKeyMovesVerbatimAndTheRestStay )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWith( FullPayloadV11() ) };

    const CloudMaterialMigrationReport report = MigrateCloudMaterialV11ToV12( entities, "Clouds_Protocol" );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.ValuesMoved, 28 );
    // Five asset keys were present, ONE of them non-empty pairs: the authored type and the layout.
    // THREE, NOT TWO: the type slot, plus the ONE layout slot that O-4 splits into two inputs on the way
    // out. The v11 file states `CloudLayout`; what lands in the material is `LayoutPattern` and
    // `LayoutMask`, both naming that painting, because a file produced here naming a slot the shader no
    // longer declares would be born needing a second pass.
    EXPECT_EQ( report.AssetsMoved, 3 );
    EXPECT_EQ( report.Rejected, 0 );
    EXPECT_EQ( report.Defaulted, 0 ) << "every moved key was stated, so nothing may report as defaulted";

    const rfl::Generic::Object payload = CloudPayloadOf( entities.front() );

    // The look is GONE from the payload...
    for ( const char* moved : { "Coverage", "CloudType1", "CloudLayout", "AmbientScale", "DetailStrength",
                                "MultiScatterOctaves", "LayoutOffset" } )
        EXPECT_FALSE( Has( payload, moved ) ) << moved;
    // ...the survivors are untouched...
    for ( const char* stays : { "Enabled", "PlanetRadius", "MaxViewDistance", "SkyOcclusionVolume",
                                "PerSampleAtmosphereTransmittance", "MaxSteps", "WindSpeed" } )
        EXPECT_TRUE( Has( payload, stays ) ) << stays;
    // ...and the seam is in place.
    ASSERT_TRUE( Has( payload, "Material" ) );
    ASSERT_EQ( report.Materials.size(), 1u );
    EXPECT_EQ( report.Materials[0].RelativePath, "Materials/M_Clouds_Protocol_Clouds.demat" );

    const Desert::Assets::MaterialData material = MaterialOf( report );
    EXPECT_EQ( material.EffectiveShaderName(), "CloudRaymarch" );
    EXPECT_EQ( material.Params.size(), 28u );
    EXPECT_FLOAT_EQ( material.GetFloat( "Coverage" ), 0.762f );
    EXPECT_EQ( static_cast<int32_t>( material.GetFloat( "Seed" ) ), 7 );
    EXPECT_EQ( material.GetParam( "LayoutOffset" ), glm::vec4( 100000.0f, -200000.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( material.GetParam( "AmbientScale" ), glm::vec4( 1.0f, 0.9f, 0.8f, 0.0f ) );

    // The asset handles are the SAME path-derived FNV the runtime mints for these files — the relation
    // that keeps the material resolving to the artist's type on every machine.
    EXPECT_EQ( material.GetTexture( "CloudType1" ), static_cast<uint64_t>( Common::AssetHandle::FromKey(
                                                         "assets:Clouds/Types/Cumulus_Congestus.decloudtype" ) ) );
    const uint64_t painting =
         static_cast<uint64_t>( Common::AssetHandle::FromKey( "assets:Clouds/Layouts/Layout_Stripe.dclayout" ) );
    EXPECT_EQ( material.GetTexture( "LayoutPattern" ), painting );
    EXPECT_EQ( material.GetTexture( "LayoutMask" ), painting );
    EXPECT_EQ( material.GetTexture( "CloudLayout" ), 0u )
         << "the pre-O-4 slot name survived into the material, so the shader will drop it and the sky "
            "loses its painting";
    // Empty slots produced NO entry: absent and empty spell the same null handle.
    EXPECT_EQ( material.GetTexture( "CloudType2" ), 0u );

    // AND THE ALBEDO COMES OUT AS A COLOUR, from ONE run. The scene field it was taken from is a float, so
    // this function writes (0.98, 0, 0, 0) — the shape a `.demat` authored before the albedo became a
    // colour carries, and the shape the shader reads as pure red. It is raised here rather than left to
    // the material pass so that a v11 scene needs the tool once and not twice.
    EXPECT_EQ( material.GetParam( "ScatteringAlbedo" ), glm::vec4( 0.98f, 0.98f, 0.98f, 0.0f ) )
         << "a v11 raise produced a SCALAR albedo, so the sky this scene renders after one run of the "
            "migrator is red";
}

// D-37 (teamlead, 2026-09-06): an empty Material slot was rejected as a second source of truth for
// the look — the earlier behaviour this test's name still describes ("left byte-identical, names no
// material") gave the question "where did this look come from" two different answers depending on the
// scene. A defaults-only payload now gets pointed at the SHARED default file instead, so every migrated
// layer names some material without exception.
TEST( SceneCloudMaterialMigration, ADefaultsOnlyPayloadIsPointedAtTheSharedDefaultMaterial )
{
    rfl::Generic::Object payload;
    payload["Enabled"]      = true;
    payload["PlanetRadius"] = 6360.0;
    payload["MaxSteps"]     = static_cast<int64_t>( 256 );

    std::vector<Desert::Assets::EntityData> entities{ EntityWith( payload ) };

    const CloudMaterialMigrationReport report = MigrateCloudMaterialV11ToV12( entities, "Quiet" );

    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.DefaultsAssigned, 1 );
    EXPECT_EQ( report.Defaulted, 28 + 5 ) << "every one of the thirty-three moved keys was absent";
    // NOT a bespoke file: the shared default is checked into the repository once, never regenerated.
    EXPECT_TRUE( report.Materials.empty() );

    const rfl::Generic::Object out = CloudPayloadOf( entities.front() );
    ASSERT_TRUE( Has( out, "Material" ) )
         << "a scene whose sky was the defaults must still name SOME material (D-37)";
    const auto material = out.get( "Material" );
    ASSERT_TRUE( material.has_value() );
    EXPECT_EQ( material.value().to_string().value_or( "" ), "Materials/M_CloudDefault.demat" );
    // The survivors are untouched, exactly as the bespoke path leaves them.
    EXPECT_TRUE( Has( out, "Enabled" ) );
    EXPECT_TRUE( Has( out, "PlanetRadius" ) );
}

TEST( SceneCloudMaterialMigration, TheSharedDefaultAssignmentIsIdempotent )
{
    rfl::Generic::Object payload;
    payload["Enabled"] = true;

    std::vector<Desert::Assets::EntityData> entities{ EntityWith( payload ) };
    MigrateCloudMaterialV11ToV12( entities, "Quiet" );

    const std::string before = rfl::json::write( entities.front().Components );
    const auto        again  = MigrateCloudMaterialV11ToV12( entities, "Quiet" );

    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( again.DefaultsAssigned, 0 );
    EXPECT_TRUE( again.Materials.empty() );
    EXPECT_EQ( rfl::json::write( entities.front().Components ), before )
         << "a second pass over an already-defaulted payload must not touch it";
}

TEST( SceneCloudMaterialMigration, ItIsDeterministicToTheByte )
{
    std::vector<Desert::Assets::EntityData> a{ EntityWith( FullPayloadV11() ) };
    std::vector<Desert::Assets::EntityData> b{ EntityWith( FullPayloadV11() ) };

    const auto ra = MigrateCloudMaterialV11ToV12( a, "Clouds_Protocol" );
    const auto rb = MigrateCloudMaterialV11ToV12( b, "Clouds_Protocol" );

    ASSERT_EQ( ra.Materials.size(), 1u );
    ASSERT_EQ( rb.Materials.size(), 1u );
    EXPECT_EQ( ra.Materials[0].Json, rb.Materials[0].Json )
         << "two runs disagree, so the MaterialId is drawn rather than derived and every migration "
            "run dirties the repository";

    // And the derived identity is stable across runs by construction: FNV of the file's own path.
    const Desert::Assets::MaterialData material = MaterialOf( ra );
    ASSERT_TRUE( material.MaterialId.has_value() );
    EXPECT_EQ( static_cast<uint64_t>( *material.MaterialId ),
               static_cast<uint64_t>(
                    Common::AssetHandle::FromKey( "cloudmat:Materials/M_Clouds_Protocol_Clouds.demat" ) ) );
}

TEST( SceneCloudMaterialMigration, ItIsIdempotent )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWith( FullPayloadV11() ) };
    MigrateCloudMaterialV11ToV12( entities, "Clouds" );

    // Second pass over the migrated tree: nothing moved, nothing produced, tree untouched.
    const std::string before = rfl::json::write( entities.front().Components );
    const auto        again  = MigrateCloudMaterialV11ToV12( entities, "Clouds" );

    EXPECT_EQ( again.Entities, 0 );
    EXPECT_TRUE( again.Materials.empty() );
    EXPECT_EQ( rfl::json::write( entities.front().Components ), before );
}

TEST( SceneCloudMaterialMigration, AValueOfTheWrongShapeIsNamedRemovedAndNotGuessedAt )
{
    rfl::Generic::Object payload = FullPayloadV11();
    payload["Coverage"]          = std::string( "mostly cloudy" );              // not a number
    payload["AmbientScale"]      = std::vector<rfl::Generic>{ 1.0 };            // wrong arity
    payload["CloudType1"]        = static_cast<int64_t>( 3 );                   // not a path
    payload["CloudLayout"]       = std::string( "/Users/somebody/x.dclayout" ); // not project-relative

    std::vector<Desert::Assets::EntityData> entities{ EntityWith( payload ) };
    const auto                              report = MigrateCloudMaterialV11ToV12( entities, "Broken" );

    EXPECT_EQ( report.Rejected, 4 );
    for ( const char* name : { "Coverage", "AmbientScale", "CloudType1", "CloudLayout" } )
        EXPECT_NE( std::find( report.RejectedNames.begin(), report.RejectedNames.end(), name ),
                   report.RejectedNames.end() )
             << name << " was rejected without being named — a value somebody authored vanished silently";

    // The bad keys still LEFT the payload (the runtime knows nothing about the old format), and did NOT
    // become material entries (a guess about intent).
    const rfl::Generic::Object out = CloudPayloadOf( entities.front() );
    EXPECT_FALSE( Has( out, "Coverage" ) );
    const Desert::Assets::MaterialData material = MaterialOf( report );
    EXPECT_EQ( material.FindParam( "Coverage" ), nullptr );
    EXPECT_EQ( material.GetTexture( "CloudType1" ), 0u );
    EXPECT_EQ( material.GetTexture( "LayoutPattern" ), 0u );
    EXPECT_EQ( material.GetTexture( "LayoutMask" ), 0u );

    // The good neighbours still moved.
    EXPECT_FLOAT_EQ( material.GetFloat( "CoverageContrast" ), 1.0f );
}

TEST( SceneCloudMaterialMigration, ASecondCloudEntityGetsANumberedSiblingRatherThanAClobber )
{
    std::vector<Desert::Assets::EntityData> entities{ EntityWith( FullPayloadV11() ),
                                                      EntityWith( FullPayloadV11() ) };

    const auto report = MigrateCloudMaterialV11ToV12( entities, "Twins" );

    ASSERT_EQ( report.Materials.size(), 2u );
    EXPECT_EQ( report.Materials[0].RelativePath, "Materials/M_Twins_Clouds.demat" );
    EXPECT_EQ( report.Materials[1].RelativePath, "Materials/M_Twins_Clouds_2.demat" );
    EXPECT_NE( report.Materials[0].Json, report.Materials[1].Json )
         << "two files, one MaterialId — whichever registers second can never resolve";
}

TEST( SceneCloudMaterialMigration, MigrateSceneRunsItLastAndStampsTheFileSoItNeverRunsAgain )
{
    SceneSerialized scene;
    scene.SceneName    = "Clouds";
    scene.SceneVersion = kSceneVersionSSRUnits;
    scene.UnitVersion  = Desert::Migration::kUnitVersion;
    scene.Entities.push_back( EntityWith( FullPayloadV11() ) );

    const auto report = MigrateScene( scene );

    EXPECT_TRUE( report.CloudMaterialRaised );
    EXPECT_EQ( report.CloudMaterial.Entities, 1 );
    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    // A newer step exists now (v12 -> v13, the debug-view removal), so this suite takes the shape the
    // older cloud suites already had: the head is at or past THIS step's version, and the tree comes out
    // stamped at the head rather than at this step's number.
    EXPECT_GE( kSceneVersion, kSceneVersionCloudMaterial );

    // Second pass over the stamped tree: nothing left to do — which is also what keeps the tool from
    // writing a numbered sibling material on every run.
    const auto again = MigrateScene( scene );
    EXPECT_FALSE( again.Changed() );
    EXPECT_TRUE( again.CloudMaterial.Materials.empty() );
}

// ───────────────────────────────────────────────────────────────────────────────────────────────────────
// THE ALBEDO BECAME A COLOUR, AND EVERY `.demat` ON DISK STATES IT AS A SCALAR (O1)
// ───────────────────────────────────────────────────────────────────────────────────────────────────────
//
// WHY THIS IS DATA LOSS AND NOT COSMETICS. A `.demat` stores every parameter as a vec4 with the tail
// zeroed — the format's own convention for a scalar — so `ScatteringAlbedo: [0.98, 0, 0, 0]` read by a
// shader that now wants three components is a medium which scatters red and absorbs green and blue
// outright. Fourteen material files in this repository carry it in that shape. Unraised, they render a
// RED sky, which is not a subtle drift anybody would argue about; it is the whole point of running the
// tool over the corpus in the same change that moves the shader.
//
// CONTENT-DETECTED, because a `.demat` has no version field — the same arrangement, and the same reason,
// as the O-4 layout split beside it.
TEST( SceneCloudMaterialAlbedo, AScalarAlbedoIsBroadcastToANeutralColour )
{
    Desert::Assets::MaterialData material;
    material.ShaderName = "CloudRaymarch";
    material.SetParam( "ScatteringAlbedo", glm::vec4( 0.98f, 0.0f, 0.0f, 0.0f ) );
    material.SetParam( "Coverage", glm::vec4( 0.45f, 0.0f, 0.0f, 0.0f ) );

    const auto report = Desert::Migration::MigrateCloudMaterialAlbedoToColour( material );

    EXPECT_TRUE( report.Changed() );
    EXPECT_EQ( report.Broadcast, 1 );
    EXPECT_EQ( material.GetParam( "ScatteringAlbedo" ), glm::vec4( 0.98f, 0.98f, 0.98f, 0.0f ) );
    // Its neighbours are scalars too and must NOT be touched: the step is about one parameter whose TYPE
    // changed, not about every value that happens to have zeroes after it.
    EXPECT_EQ( material.GetParam( "Coverage" ), glm::vec4( 0.45f, 0.0f, 0.0f, 0.0f ) );
}

TEST( SceneCloudMaterialAlbedo, ItIsIdempotentByShapeRatherThanByAFlag )
{
    Desert::Assets::MaterialData material;
    material.SetParam( "ScatteringAlbedo", glm::vec4( 0.98f, 0.0f, 0.0f, 0.0f ) );

    EXPECT_EQ( Desert::Migration::MigrateCloudMaterialAlbedoToColour( material ).Broadcast, 1 );

    const glm::vec4 afterFirst = material.GetParam( "ScatteringAlbedo" );
    EXPECT_FALSE( Desert::Migration::MigrateCloudMaterialAlbedoToColour( material ).Changed() )
         << "a second run found something to do, so the step is not idempotent and running the tool twice "
            "over a repository would keep rewriting files";
    EXPECT_EQ( material.GetParam( "ScatteringAlbedo" ), afterFirst );
}

TEST( SceneCloudMaterialAlbedo, AnAuthoredColourIsLeftExactlyAlone )
{
    Desert::Assets::MaterialData material;
    material.SetParam( "ScatteringAlbedo", glm::vec4( 0.9f, 0.72f, 0.55f, 0.0f ) );

    EXPECT_FALSE( Desert::Migration::MigrateCloudMaterialAlbedoToColour( material ).Changed() );
    EXPECT_EQ( material.GetParam( "ScatteringAlbedo" ), glm::vec4( 0.9f, 0.72f, 0.55f, 0.0f ) );
}

// THE DEGENERATE INPUT, and it is correct rather than lucky. Black is black in one component and in
// three, so (0,0,0,0) comes out unchanged and reports no change — which is ALSO what makes the shape test
// above safe, because the one value that cannot be told apart before and after is the one where the two
// answers agree.
TEST( SceneCloudMaterialAlbedo, AZeroAlbedoIsAlreadyTheColourItWouldBecome )
{
    Desert::Assets::MaterialData material;
    material.SetParam( "ScatteringAlbedo", glm::vec4( 0.0f, 0.0f, 0.0f, 0.0f ) );

    EXPECT_FALSE( Desert::Migration::MigrateCloudMaterialAlbedoToColour( material ).Changed() );
    EXPECT_EQ( material.GetParam( "ScatteringAlbedo" ), glm::vec4( 0.0f ) );
}

// A material that never stated the parameter keeps not stating it: an absent override means "the schema's
// default", and inventing an entry here would turn every silent material into one that pins a value.
TEST( SceneCloudMaterialAlbedo, AMaterialThatDoesNotStateItIsNotGivenOne )
{
    Desert::Assets::MaterialData material;
    material.SetParam( "Coverage", glm::vec4( 0.45f, 0.0f, 0.0f, 0.0f ) );

    EXPECT_FALSE( Desert::Migration::MigrateCloudMaterialAlbedoToColour( material ).Changed() );
    EXPECT_EQ( material.Params.size(), 1u );
    EXPECT_EQ( material.GetParam( "ScatteringAlbedo" ), glm::vec4( 0.0f ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
