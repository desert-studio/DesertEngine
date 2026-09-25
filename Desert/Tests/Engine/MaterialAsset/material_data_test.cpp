#include <gtest/gtest.h>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>

// Same serialization environment as SurfaceMaterialAsset.cpp: glm/UUID adapters + json backend.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

using Desert::Assets::MaterialAssetRef;
using Desert::Assets::MaterialData;
using Desert::Assets::MaterialShaderParam;
using Desert::Assets::PBRSurfaceParams;

// ─── The unified protocol: MaterialData is the ONLY material storage ─────────────────────

TEST( MaterialData, DefaultsAreStandardPBR )
{
    // The data states no shader; what that resolves to is the asset's answer
    // (SurfaceMaterialAsset::GetShaderName, pinned by the AssetMissingFile suite).
    MaterialData m;
    EXPECT_FALSE( m.Shader.has_value() );
}

TEST( MaterialData, ParamAndTextureAccessors )
{
    MaterialData m;
    EXPECT_EQ( m.FindParam( "AlbedoColor" ), nullptr );
    EXPECT_FLOAT_EQ( m.GetFloat( "RoughnessFactor", 0.5f ), 0.5f ); // fallback

    m.SetParam( "RoughnessFactor", glm::vec4( 0.25f, 0, 0, 0 ) );
    EXPECT_FLOAT_EQ( m.GetFloat( "RoughnessFactor" ), 0.25f );
    m.SetParam( "RoughnessFactor", glm::vec4( 0.75f, 0, 0, 0 ) ); // overwrite, no duplicate
    EXPECT_FLOAT_EQ( m.GetFloat( "RoughnessFactor" ), 0.75f );
    ASSERT_EQ( m.Params.size(), 1u );

    const Common::Content::AssetGuid a{ 0x0102030405060708ull, 0x1112131415161718ull };
    const Common::Content::AssetGuid b{ 0x2122232425262728ull, 0x3132333435363738ull };
    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), 0ull );
    m.SetTexture( "u_AlbedoTexture", a, "assets:Textures/A.detex" );
    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), static_cast<uint64_t>( Common::Content::HandleForGuid( a ) ) );
    m.SetTexture( "u_AlbedoTexture", b, "assets:Textures/B.detex" );
    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), static_cast<uint64_t>( Common::Content::HandleForGuid( b ) ) );
    ASSERT_EQ( m.Textures.size(), 1u );
    m.SetTexture( "u_AlbedoTexture", {}, "ignored" ); // an authored empty slot: no GUID, no locator
    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), 0ull );
    EXPECT_TRUE( m.Textures[0].Path.empty() );
}

TEST( MaterialData, JsonRoundTrip )
{
    MaterialData m;
    m.SetShader( Common::Content::AssetGuid{ 0x51ull, 0x52ull }, "engine:Shaders/Programs/Unlit.shader" );
    m.SetParam( "Color", glm::vec4( 0.1f, 0.2f, 0.3f, 1.0f ) );
    const Common::Content::AssetGuid tex{ 0xa1a2a3a4a5a6a7a8ull, 0xb1b2b3b4b5b6b7b8ull };
    m.SetTexture( "u_AlbedoTex", tex, "assets:Textures/T.detex" );
    const Common::Content::AssetGuid parent{ 0x1122334455667788ull, 0x99aabbccddeeff00ull };
    m.SetParent( parent );

    const std::string json = rfl::json::write( m );
    auto              back = rfl::json::read<MaterialData>( json );
    ASSERT_TRUE( back ) << "round-trip parse failed";

    const MaterialData& r = back.value();
    EXPECT_EQ( r.Shader, m.Shader );
    EXPECT_EQ( r.ShaderGuid(), ( Common::Content::AssetGuid{ 0x51ull, 0x52ull } ) );
    EXPECT_FLOAT_EQ( r.GetParam( "Color" ).y, 0.2f );
    EXPECT_EQ( r.GetTexture( "u_AlbedoTex" ), static_cast<uint64_t>( Common::Content::HandleForGuid( tex ) ) );
    EXPECT_EQ( r.ParentGuid(), parent );
}

TEST( MaterialData, PBRJsonRoundTripKeepsShaderAbsent )
{
    MaterialData m;
    m.SetParam( "AlbedoColor", glm::vec4( 1, 1, 1, 1 ) );
    const std::string json = rfl::json::write( m );
    ASSERT_EQ( json.find( "\"Shader\"" ), std::string::npos ); // nullopt omitted -> stays standard PBR
    auto back = rfl::json::read<MaterialData>( json );
    ASSERT_TRUE( back );
    EXPECT_FALSE( back.value().Shader.has_value() );
}

// ─── PBRSurfaceParams: the optimized backend's typed VIEW of the canon ───────────────────

TEST( PBRSurfaceParams, TypedViewToCanonAndBack )
{
    PBRSurfaceParams p;
    p.AlbedoColor     = glm::vec4( 0.2f, 0.4f, 0.6f, 1.0f );
    p.RoughnessFactor = 0.33f;
    p.GlassTint       = glm::vec4( 0.9f, 0.8f, 0.7f, 0.5f );
    p.AlbedoTexture   = Desert::Assets::AssetHandle( 777ull );
    p.UVTiling        = glm::vec2( 3.0f, 5.0f );

    const MaterialData canon = p.ToMaterialData();
    EXPECT_FLOAT_EQ( canon.GetParam( "AlbedoColor" ).y, 0.4f );
    EXPECT_FLOAT_EQ( canon.GetFloat( "RoughnessFactor" ), 0.33f );
    EXPECT_FLOAT_EQ( canon.GetParam( "GlassTint" ).w, 0.5f );
    EXPECT_FLOAT_EQ( canon.GetParam( "UVTiling" ).y, 5.0f );
    // The typed view's handle is a fold, not an identity: it is NOT written back (MATL 3 names by GUID).
    EXPECT_TRUE( canon.Textures.empty() );

    const PBRSurfaceParams back = PBRSurfaceParams::FromMaterialData( canon );
    EXPECT_FLOAT_EQ( back.AlbedoColor.y, 0.4f );
    EXPECT_FLOAT_EQ( back.RoughnessFactor, 0.33f );
    EXPECT_FLOAT_EQ( back.GlassTint.w, 0.5f );
    ASSERT_TRUE( back.UVTiling.has_value() );
    EXPECT_FLOAT_EQ( back.UVTiling->y, 5.0f );
    EXPECT_EQ( static_cast<uint64_t>( back.AlbedoTexture ), 0ull );
}

TEST( PBRSurfaceParams, FromCanonUsesDefaultsForMissingParams )
{
    // A minimal canon (e.g. a hand-written .demat) -> the typed view falls back to schema defaults.
    MaterialData m;
    m.SetParam( "AlbedoColor", glm::vec4( 0.1f, 0.2f, 0.3f, 1.0f ) );

    const PBRSurfaceParams p = PBRSurfaceParams::FromMaterialData( m );
    EXPECT_FLOAT_EQ( p.AlbedoColor.z, 0.3f );
    EXPECT_FLOAT_EQ( p.RoughnessFactor, 0.5f ); // default
    EXPECT_FLOAT_EQ( p.IOR, 1.5f );             // default
    EXPECT_EQ( static_cast<uint64_t>( p.NormalTexture ), 0ull );
}

// ─── Migration: pre-protocol .demat layouts parse as the legacy reader shape ─────────────

TEST( Migration, LegacyTypedJsonIsNotValidCanon )
{
    // A pre-protocol file (typed fields, no Params key) must NOT parse as MaterialData —
    // that's the discriminator SurfaceMaterialAsset::Load relies on to route into migration.
    const char* legacyJson =
         R"({"AlbedoColor":[1,1,1,1],"MetallicFactor":0.5,"RoughnessFactor":0.5,"AOStrength":1.0,)"
         R"("EmissiveColor":[0,0,0,1],"EmissiveIntensity":1.0,"AlphaCutoff":0.0,"Transmission":0.0,)"
         R"("IOR":1.5,"GlassTint":[1,1,1,1],"AlbedoTexture":0,"NormalTexture":0,"MetallicTexture":0,)"
         R"("RoughnessTexture":0,"AOTexture":0,"EmissiveTexture":0,"OpacityTexture":0})";

    auto asCanon = rfl::json::read<MaterialData>( legacyJson );
    EXPECT_FALSE( asCanon.has_value() );
}

// ─── MATL 3: every asset slot by header GUID + a path locator ────────────────────────────

namespace
{
    const Common::Content::AssetGuid kTexGuid{ 0x0a0b0c0d0e0f1011ull, 0x1213141516171819ull };
    const Common::Content::AssetGuid kTypeGuid{ 0x2021222324252627ull, 0x28292a2b2c2d2e2full };
    const Common::Content::AssetGuid kLayoutGuid{ 0x3031323334353637ull, 0x38393a3b3c3d3e3full };

    uint64_t Fold( const Common::Content::AssetGuid& g )
    {
        return static_cast<uint64_t>( Common::Content::HandleForGuid( g ) );
    }
} // namespace

TEST( MaterialFormatV3, AWrittenMaterialReadsBackWithEverySlotByGuidAndEveryGuidADependency )
{
    MaterialData m;
    const Common::Content::AssetGuid kShaderGuid{ 0xc1c2c3c4ull, 0xd1d2d3d4ull };
    constexpr const char*            kShaderPath = "engine:Shaders/Programs/Clouds/CloudRaymarch.shader";
    m.SetShader( kShaderGuid, kShaderPath );
    m.SetTexture( "u_AlbedoTexture", kTexGuid, "assets:Textures/T.detex" );
    m.SetCloudAsset( "CloudType1", kTypeGuid, "assets:Clouds/Types/Cu.decloudtype" );
    m.SetCloudAsset( "LayoutPattern", kLayoutGuid, "assets:Clouds/Layouts/L.dclayout" );
    m.SetCloudAsset( "LayoutMask", kLayoutGuid, "assets:Clouds/Layouts/L.dclayout" );
    m.SetCloudAsset( "Medium", kShaderGuid, kShaderPath );

    const auto text = Desert::Assets::WriteMaterialJson( m );
    ASSERT_TRUE( text ) << text.GetError();
    EXPECT_EQ( text.GetValue().find( "TextureHandle" ), std::string::npos ) << "no path-derived number is written";

    const auto back = Desert::Assets::ParseMaterialJson( "v3.demat", text.GetValue() );
    ASSERT_TRUE( back ) << back.GetError();
    const MaterialData& r = back.GetValue();
    EXPECT_EQ( r.GetTexture( "u_AlbedoTexture" ), Fold( kTexGuid ) );
    EXPECT_EQ( r.GetCloudAsset( "CloudType1" ), Fold( kTypeGuid ) );
    EXPECT_EQ( r.GetCloudAsset( "LayoutMask" ), Fold( kLayoutGuid ) );
    EXPECT_EQ( r.GetTexture( "CloudType1" ), 0ull ) << "cloud slots live in their own list, not in Textures";
    ASSERT_EQ( r.Textures.size(), 1u );
    EXPECT_EQ( r.Textures[0].Path, "assets:Textures/T.detex" );
    // The Medium shader BY GUID: the handle a ShaderAsset registers under (HandleForGuid of its header GUID),
    // not the path-derived number MATL 3 folded its path into, which no loaded shader answers to since T7j.
    EXPECT_EQ( r.GetCloudAsset( "Medium" ), Fold( kShaderGuid ) );
    EXPECT_NE( r.GetCloudAsset( "Medium" ), static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( kShaderPath ) ) );
    EXPECT_EQ( r.ShaderGuid(), kShaderGuid );
    // Four distinct GUIDs (shader first after no parent), the shared layout and the Medium shader stated once.
    EXPECT_EQ( Desert::Assets::StatedVersion( r.Header, Desert::Assets::kMaterialSchemaTag ),
               static_cast<int>( Desert::Assets::kMaterialSchemaVersion ) );
    ASSERT_TRUE( r.Header.has_value() );
    ASSERT_EQ( r.Header->Dependencies.size(), 4u );
    EXPECT_EQ( r.Header->Dependencies.front(), Common::Content::AssetGuidToText( kShaderGuid ) );
}

TEST( MaterialFormatV3, AVersion2FileIsRefusedByNameAndPointsAtTheMigrator )
{
    const std::string v2     = R"({"Header":{"Kind":"Material","Guid":"3cac456286293463b516718906b23e28",)"
                               R"("Versions":{"MATL":2},"Dependencies":[]},"Params":[],)"
                               R"("Textures":[{"Name":"CloudType1","TextureHandle":14207433254880240939}]})";
    const auto        parsed = Desert::Assets::ParseMaterialJson( "M_Old.demat", v2 );
    ASSERT_FALSE( parsed );
    EXPECT_NE( parsed.GetError().find( "M_Old.demat" ), std::string::npos ) << parsed.GetError();
    EXPECT_NE( parsed.GetError().find( "schema v2" ), std::string::npos ) << parsed.GetError();
    EXPECT_NE( parsed.GetError().find( "SceneMigrator" ), std::string::npos ) << parsed.GetError();
}

TEST( MaterialFormatV3, ASlotGuidTheHeaderDoesNotStateOrAPathWithoutAGuidIsRefused )
{
    const std::string head = R"({"Header":{"Kind":"Material","Guid":"3cac456286293463b516718906b23e28",)"
                             R"("Versions":{"MATL":4},"Dependencies":[)";
    const std::string slot = R"("Params":[],"Textures":[],)"
                             R"("CloudAssets":[{"Name":"CloudType1","Guid":"45d579b03cc0d0a8df2e4cb025d6bea5",)"
                             R"("Path":"assets:Clouds/Types/Cu.decloudtype"}]})";
    EXPECT_TRUE(
         Desert::Assets::ParseMaterialJson( "ok", head + R"("45d579b03cc0d0a8df2e4cb025d6bea5"]},)" + slot ) );
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson( "undeclared", head + "]}," + slot ) );
    // The shader by GUID (MATL 4): an empty GUID, and a GUID the header does not state, are refused by name.
    const std::string shader = R"("Shader":{"Guid":"45d579b03cc0d0a8df2e4cb025d6bea5","Path":"engine:Shaders/S.shader"},)"
                               R"("Params":[],"Textures":[],"CloudAssets":[]})";
    EXPECT_TRUE( Desert::Assets::ParseMaterialJson( "shader", head + R"("45d579b03cc0d0a8df2e4cb025d6bea5"]},)" + shader ) );
    const auto undeclared = Desert::Assets::ParseMaterialJson( "M_Undeclared.demat", head + "]}," + shader );
    ASSERT_FALSE( undeclared );
    EXPECT_NE( undeclared.GetError().find( "M_Undeclared.demat" ), std::string::npos ) << undeclared.GetError();
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson(
         "noguid", head + R"(]},"Shader":{"Guid":"","Path":"engine:Shaders/S.shader"},"Params":[],"Textures":[],"CloudAssets":[]})" ) );
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson(
         "pathonly", head + R"(]},"Params":[],"CloudAssets":[],)"
                            R"("Textures":[{"Name":"u_AlbedoTexture","Guid":"","Path":"assets:T.detex"}]})" ) );
}

// The relation, not either end: a type registers under HandleForGuid of its header GUID (CloudTypeAsset's
// constructor), and the cloud slot that names that GUID hands ApplyCloudAssetRef (through the flattened
// overrides MaterialService::ResolveOverrides builds with ForEachSlotHandle) exactly that number.
TEST( MaterialFormatV3, ACloudSlotNamedByGuidFindsTheTypeRegisteredUnderThatGuid )
{
    MaterialData m;
    m.SetCloudAsset( "CloudType1", kTypeGuid, "assets:Clouds/Types/Cu.decloudtype" );
    const auto text = Desert::Assets::WriteMaterialJson( m );
    ASSERT_TRUE( text ) << text.GetError();
    const auto back = Desert::Assets::ParseMaterialJson( "c.demat", text.GetValue() );
    ASSERT_TRUE( back ) << back.GetError();

    const uint64_t registeredUnder = Fold( kTypeGuid ); // what CloudTypeService keys the type by
    uint64_t       handed          = 0;
    back.GetValue().ForEachSlotHandle(
         [&]( const std::string& name, uint64_t handle )
         {
             if ( name == "CloudType1" )
                 handed = handle;
         } );
    EXPECT_EQ( handed, registeredUnder );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}