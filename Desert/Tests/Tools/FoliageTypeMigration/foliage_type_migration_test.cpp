// FO-3: FOLT 1 -> 2; FO-5: FOLT 2 -> 3 (CullDistance joins at UE's never-culled default); FO-7: FOLT 3 -> 4
// (Wind joins, still); FO-6: FOLT 4 -> 5 (IncludeInHLOD); FO-8: FOLT 5 -> 6 (Kind Mesh, Prefab joins). A v1
// `.defoliage` stated Density per brush dab; v2 states it per 1000x1000 cm (UE). The step converts through the v1
// brush's default radius, so one reference dab places the same count under both, keeps every other number and the
// GUID, and the engine reads the result while refusing v1.

#include <SceneMigration.hpp>

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace
{
    using namespace Desert;

    // The shape FO-1 wrote (the corpus file U13_Authored_Components_Probe_Ground.defoliage, mesh added).
    const std::string kV1 = R"({
    "Header": {
        "Kind": "FoliageType",
        "Guid": "40d85d14a33a791506ec8583ba5ecbb8",
        "Versions": { "FOLT": 1 },
        "Dependencies": [ "11112222333344445555666677778888" ]
    },
    "Mesh": { "Guid": "11112222333344445555666677778888", "Path": "Cooked/Meshes/Grass.stmesh" },
    "Density": 23.0,
    "ScaleX": { "Min": 0.11, "Max": 4.75 },
    "ZOffset": { "Min": -12.5, "Max": 37.25 },
    "AlignToNormal": false,
    "RandomYaw": false,
    "RandomPitchAngle": 18.5,
    "GroundSlopeAngle": { "Min": 7.0, "Max": 62.5 }
})";

    std::string WithVersion( std::string text, int version )
    {
        const std::string from = "\"FOLT\": 1";
        text.replace( text.find( from ), from.size(), "\"FOLT\": " + std::to_string( version ) );
        return text;
    }
} // namespace

TEST( FoliageTypeMigration, TheEngineReadsVersionSixOnlyAndRefusesVersionOne )
{
    const auto v1 = Assets::Serialization::ParseFoliageType( kV1 );
    ASSERT_FALSE( v1 );
    EXPECT_NE( v1.GetError().find( "FOLT" ), std::string::npos ) << v1.GetError();
}

namespace
{
    // v2 -> v3 -> v4 -> v5 -> v6: the engine reads the last generation only.
    Common::ResultStr<std::string> RaiseV2ToEngine( const std::string& v2 )
    {
        const auto toV3 = Migration::MigrateFoliageTypeV2ToV3( v2 );
        if ( !toV3 )
            return toV3;
        const auto toV4 = Migration::MigrateFoliageTypeV3ToV4( toV3.GetValue() );
        if ( !toV4 )
            return toV4;
        const auto toV5 = Migration::MigrateFoliageTypeV4ToV5( toV4.GetValue() );
        if ( !toV5 )
            return toV5;
        return Migration::MigrateFoliageTypeV5ToV6( toV5.GetValue() );
    }
} // namespace

TEST( FoliageTypeMigration, DensityPerDabBecomesUEAreaDensity )
{
    // 23 per dab of a 300 cm brush: 23 / (pi * 300^2) * 1000^2 = 81.35 per 1000x1000 cm.
    EXPECT_NEAR( Migration::FoliageDensityFromPerDab( 23.0f ), 81.345f, 1e-2f );

    const auto toV2 = Migration::MigrateFoliageTypeV1ToV2( kV1 );
    ASSERT_TRUE( toV2 ) << toV2.GetError();
    const auto raised = RaiseV2ToEngine( toV2.GetValue() );
    ASSERT_TRUE( raised ) << raised.GetError();
    const auto parsed = Assets::Serialization::ParseFoliageType( raised.GetValue() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const auto& data = parsed.GetValue();

    // One reference dab places the same count under both generations: v2's density over the v1 disk.
    const float disk = 3.14159265f * 300.0f * 300.0f;
    EXPECT_NEAR( data.Density * disk / ( 1000.0f * 1000.0f ), 23.0f, 1e-3f );

    // Everything else crosses as it was; the new fields take UE's defaults; the identity is kept.
    EXPECT_EQ( data.Header->Guid, "40d85d14a33a791506ec8583ba5ecbb8" );
    EXPECT_EQ( data.Header->Versions.at( "FOLT" ), 6u );
    // FOLT 6 (FO-8): every raised type draws a mesh, as every v5 type did.
    EXPECT_EQ( data.Kind, Assets::Serialization::FoliageTypeKind::Mesh );
    EXPECT_TRUE( data.Prefab.Guid.empty() );
    EXPECT_EQ( data.Mesh.Guid, "11112222333344445555666677778888" );
    EXPECT_EQ( data.Mesh.Path, "Cooked/Meshes/Grass.stmesh" );
    EXPECT_FLOAT_EQ( data.ScaleX.Min, 0.11f );
    EXPECT_FLOAT_EQ( data.ScaleX.Max, 4.75f );
    EXPECT_FLOAT_EQ( data.ZOffset.Min, -12.5f );
    EXPECT_FLOAT_EQ( data.ZOffset.Max, 37.25f );
    EXPECT_FALSE( data.AlignToNormal );
    EXPECT_FALSE( data.RandomYaw );
    EXPECT_FLOAT_EQ( data.RandomPitchAngle, 18.5f );
    EXPECT_FLOAT_EQ( data.GroundSlopeAngle.Min, 7.0f );
    EXPECT_FLOAT_EQ( data.GroundSlopeAngle.Max, 62.5f );
    EXPECT_TRUE( data.LandscapeLayers.empty() );
    EXPECT_FLOAT_EQ( data.MinimumLayerWeight, 0.0f );
    EXPECT_FLOAT_EQ( data.Height.Min, -262144.0f );
    EXPECT_FLOAT_EQ( data.Height.Max, 262144.0f );
    EXPECT_FLOAT_EQ( data.CullDistance.Min, 0.0f );
    EXPECT_FLOAT_EQ( data.CullDistance.Max, 0.0f );
    // FOLT 4 (FO-7): a raised type stands still, as every v3 field drew.
    EXPECT_EQ( data.Wind, Assets::Serialization::FoliageWind{} );
    EXPECT_FLOAT_EQ( data.Wind.Strength, 0.0f );
}

TEST( FoliageTypeMigration, OnlyVersionOneIsRaised )
{
    const auto two = Migration::MigrateFoliageTypeV1ToV2( WithVersion( kV1, 2 ) );
    ASSERT_FALSE( two );
    EXPECT_NE( two.GetError().find( "FOLT 2" ), std::string::npos ) << two.GetError();

    const auto zero = Migration::MigrateFoliageTypeV1ToV2( WithVersion( kV1, 0 ) );
    EXPECT_FALSE( zero );
}

TEST( FoliageTypeMigration, OnlyVersionTwoIsRaisedToThree )
{
    const auto toV2 = Migration::MigrateFoliageTypeV1ToV2( kV1 );
    ASSERT_TRUE( toV2 ) << toV2.GetError();
    // The v1 step writes v2 and not the engine's generation: the engine refuses it until the next step.
    EXPECT_FALSE( Assets::Serialization::ParseFoliageType( toV2.GetValue() ) );

    const auto one = Migration::MigrateFoliageTypeV2ToV3( kV1 );
    ASSERT_FALSE( one );
    EXPECT_NE( one.GetError().find( "FOLT 1" ), std::string::npos ) << one.GetError();

    const auto toV3 = Migration::MigrateFoliageTypeV2ToV3( toV2.GetValue() );
    ASSERT_TRUE( toV3 ) << toV3.GetError();
    const auto three = Migration::MigrateFoliageTypeV2ToV3( toV3.GetValue() );
    ASSERT_FALSE( three );
    EXPECT_NE( three.GetError().find( "FOLT 3" ), std::string::npos ) << three.GetError();
}

TEST( FoliageTypeMigration, OnlyVersionThreeIsRaisedToFourAndKeepsItsCullDistance )
{
    const auto toV2 = Migration::MigrateFoliageTypeV1ToV2( kV1 );
    ASSERT_TRUE( toV2 ) << toV2.GetError();
    const auto toV3 = Migration::MigrateFoliageTypeV2ToV3( toV2.GetValue() );
    ASSERT_TRUE( toV3 ) << toV3.GetError();
    // The v2 step writes v3, which the engine (v4) refuses until the last step.
    EXPECT_FALSE( Assets::Serialization::ParseFoliageType( toV3.GetValue() ) );

    // A v3 file as FO-5 wrote it, with a CullDistance the step must carry.
    const std::string v3 = R"({
    "Header": {
        "Kind": "FoliageType",
        "Guid": "40d85d14a33a791506ec8583ba5ecbb8",
        "Versions": { "FOLT": 3 },
        "Dependencies": [ "11112222333344445555666677778888" ]
    },
    "Mesh": { "Guid": "11112222333344445555666677778888", "Path": "Cooked/Meshes/Grass.stmesh" },
    "Density": 300.0,
    "ScaleX": { "Min": 0.3, "Max": 0.5 },
    "ZOffset": { "Min": 0.0, "Max": 0.0 },
    "AlignToNormal": true,
    "RandomYaw": true,
    "RandomPitchAngle": 0.0,
    "GroundSlopeAngle": { "Min": 0.0, "Max": 90.0 },
    "Height": { "Min": -262144.0, "Max": 262144.0 },
    "LandscapeLayers": [],
    "MinimumLayerWeight": 0.0,
    "CullDistance": { "Min": 1500.0, "Max": 4000.0 }
})";

    const auto two = Migration::MigrateFoliageTypeV3ToV4( toV2.GetValue() );
    ASSERT_FALSE( two );
    EXPECT_NE( two.GetError().find( "FOLT 2" ), std::string::npos ) << two.GetError();

    const auto toV4 = Migration::MigrateFoliageTypeV3ToV4( v3 );
    ASSERT_TRUE( toV4 ) << toV4.GetError();
    const auto toV5 = Migration::MigrateFoliageTypeV4ToV5( toV4.GetValue() );
    ASSERT_TRUE( toV5 ) << toV5.GetError();
    // v5 text is no longer the engine's generation: the engine refuses it by number.
    const auto v5Refused = Assets::Serialization::ParseFoliageType( toV5.GetValue() );
    ASSERT_FALSE( v5Refused );
    EXPECT_NE( v5Refused.GetError().find( "5" ), std::string::npos ) << v5Refused.GetError();
    const auto toV6 = Migration::MigrateFoliageTypeV5ToV6( toV5.GetValue() );
    ASSERT_TRUE( toV6 ) << toV6.GetError();
    const auto parsed = Assets::Serialization::ParseFoliageType( toV6.GetValue() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Header->Versions.at( "FOLT" ), 6u );
    EXPECT_TRUE( parsed.GetValue().IncludeInHLOD ) << "FOLT 4 -> 5 must keep UE's default: in the HLOD";
    const auto five = Migration::MigrateFoliageTypeV4ToV5( toV5.GetValue() );
    const auto six  = Migration::MigrateFoliageTypeV5ToV6( toV6.GetValue() );
    ASSERT_FALSE( six );
    EXPECT_NE( six.GetError().find( "FOLT 6" ), std::string::npos ) << six.GetError();
    ASSERT_FALSE( five );
    EXPECT_NE( five.GetError().find( "FOLT 5" ), std::string::npos ) << five.GetError();
    EXPECT_EQ( parsed.GetValue().Header->Guid, "40d85d14a33a791506ec8583ba5ecbb8" );
    EXPECT_FLOAT_EQ( parsed.GetValue().CullDistance.Min, 1500.0f );
    EXPECT_FLOAT_EQ( parsed.GetValue().CullDistance.Max, 4000.0f );
    EXPECT_FLOAT_EQ( parsed.GetValue().Wind.Strength, 0.0f );

    const auto four = Migration::MigrateFoliageTypeV3ToV4( toV4.GetValue() );
    ASSERT_FALSE( four );
    EXPECT_NE( four.GetError().find( "FOLT 4" ), std::string::npos ) << four.GetError();
}

TEST( FoliageTypeMigration, VersionFiveIsRaisedToSixAsAMeshTypeKeepingEveryValue )
{
    // A v5 file whose type was taken out of the HLOD and sways: FOLT 5 -> 6 keeps both, adds Kind Mesh.
    std::string v5   = R"({
    "Header": {
        "Kind": "FoliageType",
        "Versions": { "FOLT": 5 },
        "Guid": "40d85d14a33a791506ec8583ba5ecbb8",
        "Dependencies": [ "11112222333344445555666677778888" ]
    },
    "Mesh": { "Guid": "11112222333344445555666677778888", "Path": "Cooked/Meshes/Grass.stmesh" },
    "Density": 64.0,
    "ScaleX": { "Min": 0.9, "Max": 1.1 },
    "ZOffset": { "Min": 0.0, "Max": 0.0 },
    "AlignToNormal": true,
    "RandomYaw": true,
    "RandomPitchAngle": 0.0,
    "GroundSlopeAngle": { "Min": 0.0, "Max": 90.0 },
    "Height": { "Min": -262144.0, "Max": 262144.0 },
    "LandscapeLayers": [],
    "MinimumLayerWeight": 0.0,
    "CullDistance": { "Min": 1500.0, "Max": 4000.0 },
    "Wind": { "Strength": 12.0, "Speed": 0.5, "Height": 100.0, "DirectionDegrees": 30.0 },
    "IncludeInHLOD": false
})";
    const auto  toV6 = Migration::MigrateFoliageTypeV5ToV6( v5 );
    ASSERT_TRUE( toV6 ) << toV6.GetError();
    const auto parsed = Assets::Serialization::ParseFoliageType( toV6.GetValue() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const auto& data = parsed.GetValue();
    EXPECT_EQ( data.Header->Versions.at( "FOLT" ), 6u );
    EXPECT_EQ( data.Header->Guid, "40d85d14a33a791506ec8583ba5ecbb8" );
    EXPECT_EQ( data.Kind, Assets::Serialization::FoliageTypeKind::Mesh );
    EXPECT_TRUE( data.Prefab.Guid.empty() && data.Prefab.Path.empty() );
    EXPECT_EQ( data.Mesh.Path, "Cooked/Meshes/Grass.stmesh" );
    EXPECT_FLOAT_EQ( data.Density, 64.0f );
    EXPECT_FLOAT_EQ( data.CullDistance.Max, 4000.0f );
    EXPECT_FLOAT_EQ( data.Wind.Strength, 12.0f );
    EXPECT_FALSE( data.IncludeInHLOD ) << "FOLT 5 -> 6 must keep the file's IncludeInHLOD, not reset it";
    // A fixed point: the raised text is exactly what the engine writes for it.
    EXPECT_EQ( Assets::Serialization::WriteFoliageType( data ), toV6.GetValue() );
}

TEST( FoliageTypeMigration, APrefabTypeNamesItsPrefabAndNoMeshSetting )
{
    Assets::Serialization::FoliageTypeData data;
    data.Kind          = Assets::Serialization::FoliageTypeKind::Prefab;
    data.Prefab        = { "99998888777766665555444433332222", "Prefabs/Rock.deprefab" };
    data.IncludeInHLOD = false;
    const auto parsed = Assets::Serialization::ParseFoliageType( Assets::Serialization::WriteFoliageType( data ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_TRUE( parsed.GetValue().IsPrefab() );
    ASSERT_EQ( parsed.GetValue().Header->Dependencies.size(), 1u );
    EXPECT_EQ( parsed.GetValue().Header->Dependencies[0], "99998888777766665555444433332222" );

    // Each instanced-mesh setting on a Prefab type is refused with the reason.
    auto withCull             = data;
    withCull.CullDistance.Max = 100.0f;
    auto withWind             = data;
    withWind.Wind.Strength    = 1.0f;
    auto withHlod             = data;
    withHlod.IncludeInHLOD    = true;
    auto withMesh             = data;
    withMesh.Mesh             = { "11112222333344445555666677778888", "Cooked/Meshes/Grass.stmesh" };
    auto noPrefab             = data;
    noPrefab.Prefab           = {};
    auto meshNamingPrefab     = Assets::Serialization::FoliageTypeData{};
    meshNamingPrefab.Prefab   = data.Prefab;
    for ( const auto* bad : { &withCull, &withWind, &withHlod, &withMesh, &noPrefab, &meshNamingPrefab } )
        EXPECT_FALSE( Assets::Serialization::ValidateFoliageTypeData( *bad ) );
    const auto cull = Assets::Serialization::ValidateFoliageTypeData( withCull );
    ASSERT_FALSE( cull );
    EXPECT_NE( cull.GetError().find( Assets::Serialization::kFoliagePrefabMeshOnlyReason ), std::string::npos )
         << cull.GetError();
}

TEST( FoliageTypeMigration, ARaisedFileIsAFixedPoint )
{
    const auto toV2 = Migration::MigrateFoliageTypeV1ToV2( kV1 );
    ASSERT_TRUE( toV2 );
    const auto raised = RaiseV2ToEngine( toV2.GetValue() );
    ASSERT_TRUE( raised );
    const auto parsed = Assets::Serialization::ParseFoliageType( raised.GetValue() );
    ASSERT_TRUE( parsed );
    EXPECT_EQ( Assets::Serialization::WriteFoliageType( parsed.GetValue() ), raised.GetValue() );
}

TEST( FoliageTypeMigration, LayerReferencesAreHeaderDependenciesAfterTheMesh )
{
    Assets::Serialization::FoliageTypeData data;
    data.Mesh            = { "11112222333344445555666677778888", "Cooked/Meshes/Grass.stmesh" };
    data.LandscapeLayers = { { "aaaabbbbccccddddeeeeffff00001111", "Landscape/Grass.delayerinfo" } };
    const auto parsed = Assets::Serialization::ParseFoliageType( Assets::Serialization::WriteFoliageType( data ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    ASSERT_EQ( parsed.GetValue().Header->Dependencies.size(), 2u );
    EXPECT_EQ( parsed.GetValue().Header->Dependencies[1], "aaaabbbbccccddddeeeeffff00001111" );

    data.LandscapeLayers.push_back( data.LandscapeLayers[0] );
    EXPECT_FALSE( Assets::Serialization::ValidateFoliageTypeData( data ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
