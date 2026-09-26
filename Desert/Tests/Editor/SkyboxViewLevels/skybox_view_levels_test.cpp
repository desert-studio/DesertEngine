// The skybox viewer's picker (UE TextureCube editor pattern): which cube and which mip each entry reads, and
// that the 2D view's unwrap is the exact inverse of the mapping the bake read the panorama with.

#include <Editor/Panels/SkyboxViewer/SkyboxViewLevels.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <Common/Core/GlslAsCpp.hpp>

namespace
{
    using vec2 = glm::vec2;
    using vec3 = glm::vec3;
    using glm::acos;
    using glm::atan;
    using glm::clamp;
    using glm::cos;
    using glm::sin;

    DESERT_GLSL_AS_CPP_BEGIN
#include <Common/SkyPanorama.glslh>
    DESERT_GLSL_AS_CPP_END

    namespace SV = Desert::Editor::SkyboxView;

    constexpr uint32_t kBakedMips = 9u; // EnvironmentBake: a 256 face with a nine-level GGX chain
} // namespace

TEST( SkyboxViewLevels, TheListIsRadianceThenTheChainThenDiffuse )
{
    ASSERT_EQ( SV::LevelCount( kBakedMips ), 10 );

    const auto first = SV::ResolveLevel( 0, kBakedMips );
    ASSERT_TRUE( first.has_value() );
    EXPECT_EQ( *first, ( SV::Level{ SV::Cube::Radiance, 0.0f } ) );

    for ( int mip = 1; mip < static_cast<int>( kBakedMips ); ++mip )
    {
        const auto level = SV::ResolveLevel( mip, kBakedMips );
        ASSERT_TRUE( level.has_value() ) << mip;
        EXPECT_EQ( level->Source, SV::Cube::Prefiltered ) << mip;
        EXPECT_FLOAT_EQ( level->Lod, static_cast<float>( mip ) ) << "entry k reads prefiltered mip k";
    }

    const auto last = SV::ResolveLevel( SV::DiffuseLevel( kBakedMips ), kBakedMips );
    ASSERT_TRUE( last.has_value() );
    EXPECT_EQ( *last, ( SV::Level{ SV::Cube::Irradiance, 0.0f } ) );
}

TEST( SkyboxViewLevels, AnIndexOutsideTheListIsRefusedNotClamped )
{
    EXPECT_FALSE( SV::ResolveLevel( -1, kBakedMips ).has_value() );
    EXPECT_FALSE( SV::ResolveLevel( SV::LevelCount( kBakedMips ), kBakedMips ).has_value() );
}

TEST( SkyboxViewLevels, WithoutAChainRadianceAndDiffuseAreStillListed )
{
    ASSERT_EQ( SV::LevelCount( 0u ), 2 );
    EXPECT_EQ( SV::ResolveLevel( 0, 0u )->Source, SV::Cube::Radiance );
    EXPECT_EQ( SV::ResolveLevel( 1, 0u )->Source, SV::Cube::Irradiance );
}

TEST( SkyboxViewLevels, MipRoughnessInvertsTheRuleLitSurfacesReadTheChainWith )
{
    // AmbientIBL.glslh: lod = roughness * textureQueryLevels. The label must name the roughness that reads
    // exactly this mip, or the picker lies about which surface it previews.
    for ( int mip = 0; mip < static_cast<int>( kBakedMips ); ++mip )
        EXPECT_FLOAT_EQ( SV::MipRoughness( mip, kBakedMips ) * static_cast<float>( kBakedMips ),
                         static_cast<float>( mip ) );
    EXPECT_EQ( SV::LevelLabel( 0, kBakedMips ), "Mip 0 (Radiance)" );
    EXPECT_EQ( SV::LevelLabel( 3, kBakedMips ), "Mip 3 (roughness 0.33)" );
    EXPECT_EQ( SV::LevelLabel( 9, kBakedMips ), "Diffuse (Irradiance)" );
}

TEST( SkyboxViewLevels, ExposureIsEVAndDoublesPerStep )
{
    EXPECT_FLOAT_EQ( SV::ExposureFromEV( 0.0f ), 1.0f );
    EXPECT_FLOAT_EQ( SV::ExposureFromEV( 1.0f ), 2.0f );
    EXPECT_FLOAT_EQ( SV::ExposureFromEV( -2.0f ), 0.25f );
}

TEST( SkyboxViewLevels, TheUnwrapIsTheInverseOfTheBakesPanoramaLookup )
{
    // Interior texels only: the poles and the seam are many-to-one in any equirect layout.
    for ( int y = 1; y < 16; ++y )
        for ( int x = 1; x < 32; ++x )
        {
            const glm::vec2 uv( ( static_cast<float>( x ) + 0.25f ) / 32.0f,
                                ( static_cast<float>( y ) + 0.25f ) / 16.0f );
            const glm::vec3 d = PanoramaDirection( uv );
            EXPECT_NEAR( glm::length( d ), 1.0f, 1e-5f );
            const glm::vec2 back = PanoramaSampleUV( d );
            EXPECT_NEAR( back.x, uv.x, 1e-4f ) << x << "," << y;
            EXPECT_NEAR( back.y, uv.y, 1e-4f ) << x << "," << y;
        }
    // Up is the top row, as in the file.
    EXPECT_GT( PanoramaDirection( glm::vec2( 0.5f, 0.01f ) ).y, 0.99f );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
