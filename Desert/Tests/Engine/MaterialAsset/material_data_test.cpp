#include <gtest/gtest.h>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>

// Same serialization environment as SurfaceMaterialAsset.cpp: glm/UUID adapters + json backend.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

using Desert::Assets::MaterialData;
using Desert::Assets::MaterialShaderParam;
using Desert::Assets::MaterialShaderTexture;
using Desert::Assets::PBRSurfaceParams;

// ─── The unified protocol: MaterialData is the ONLY material storage ─────────────────────

TEST( MaterialData, DefaultsAreStandardPBR )
{
    MaterialData m;
    EXPECT_EQ( m.EffectiveShaderName(), "StaticMeshPBR" );
    EXPECT_FALSE( m.UsesCustomShader() );

    m.ShaderName = "";
    EXPECT_FALSE( m.UsesCustomShader() );

    m.ShaderName = "SkinnedMeshPBR";
    EXPECT_FALSE( m.UsesCustomShader() );

    m.ShaderName = "Unlit";
    EXPECT_TRUE( m.UsesCustomShader() );
    EXPECT_EQ( m.EffectiveShaderName(), "Unlit" );
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

    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), 0ull );
    m.SetTexture( "u_AlbedoTexture", 777ull );
    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), 777ull );
    m.SetTexture( "u_AlbedoTexture", 888ull );
    EXPECT_EQ( m.GetTexture( "u_AlbedoTexture" ), 888ull );
    ASSERT_EQ( m.Textures.size(), 1u );
}

TEST( MaterialData, JsonRoundTrip )
{
    MaterialData m;
    m.ShaderName = "Unlit";
    m.SetParam( "Color", glm::vec4( 0.1f, 0.2f, 0.3f, 1.0f ) );
    m.SetTexture( "u_AlbedoTex", 12345ull );
    const Common::Content::AssetGuid parent{ 0x1122334455667788ull, 0x99aabbccddeeff00ull };
    m.SetParent( parent );

    const std::string json = rfl::json::write( m );
    auto              back = rfl::json::read<MaterialData>( json );
    ASSERT_TRUE( back ) << "round-trip parse failed";

    const MaterialData& r = back.value();
    EXPECT_TRUE( r.UsesCustomShader() );
    EXPECT_FLOAT_EQ( r.GetParam( "Color" ).y, 0.2f );
    EXPECT_EQ( r.GetTexture( "u_AlbedoTex" ), 12345ull );
    EXPECT_EQ( r.ParentGuid(), parent );
}

TEST( MaterialData, PBRJsonRoundTripKeepsShaderAbsent )
{
    MaterialData m;
    m.SetParam( "AlbedoColor", glm::vec4( 1, 1, 1, 1 ) );
    const std::string json = rfl::json::write( m );
    ASSERT_EQ( json.find( "ShaderName" ), std::string::npos ); // nullopt omitted -> stays standard PBR
    auto back = rfl::json::read<MaterialData>( json );
    ASSERT_TRUE( back );
    EXPECT_FALSE( back.value().UsesCustomShader() );
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
    EXPECT_EQ( canon.GetTexture( "u_AlbedoTexture" ), 777ull );

    const PBRSurfaceParams back = PBRSurfaceParams::FromMaterialData( canon );
    EXPECT_FLOAT_EQ( back.AlbedoColor.y, 0.4f );
    EXPECT_FLOAT_EQ( back.RoughnessFactor, 0.33f );
    EXPECT_FLOAT_EQ( back.GlassTint.w, 0.5f );
    ASSERT_TRUE( back.UVTiling.has_value() );
    EXPECT_FLOAT_EQ( back.UVTiling->y, 5.0f );
    EXPECT_EQ( static_cast<uint64_t>( back.AlbedoTexture ), 777ull );
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

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}