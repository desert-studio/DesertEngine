// The skybox viewer's picker (UE TextureCube editor pattern): which cube and which mip each entry reads, and
// that the 2D view's unwrap is the exact inverse of the mapping the bake read the panorama with.

#include <Editor/Panels/SkyboxViewer/SkyboxViewLevels.hpp>

#include <gtest/gtest.h>

#include <string>

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

namespace
{
    // Runs the action the palette would list under `label`, as the document does: over one ViewState.
    bool RunViewAction( SV::ViewState& state, const std::string& label, uint32_t mips )
    {
        for ( const auto& action : SV::ViewActions( mips ) )
            if ( action.Label == label )
            {
                action.Apply( state );
                return true;
            }
        return false;
    }
} // namespace

TEST( SkyboxViewLevels, EveryLevelIsAnActionAndTheDiffuseActionPicksIrradiance )
{
    constexpr uint32_t kMips = 8u; // the chain the environment cache bakes (1..7 listed, then Diffuse)
    SV::ViewState      state;
    int                levelActions = 0;
    for ( const auto& action : SV::ViewActions( kMips ) )
        levelActions += action.Label.rfind( "Level: ", 0 ) == 0 ? 1 : 0;
    EXPECT_EQ( levelActions, SV::LevelCount( kMips ) );

    ASSERT_TRUE( RunViewAction( state, "Level: Mip 5", kMips ) );
    EXPECT_EQ( state.Level, 5 );
    EXPECT_EQ( SV::ResolveLevel( state.Level, kMips ), ( SV::Level{ SV::Cube::Prefiltered, 5.0f } ) );

    ASSERT_TRUE( RunViewAction( state, "Level: Diffuse", kMips ) );
    EXPECT_EQ( state.Level, SV::DiffuseLevel( kMips ) );
    EXPECT_EQ( SV::ResolveLevel( state.Level, kMips ), ( SV::Level{ SV::Cube::Irradiance, 0.0f } ) );

    ASSERT_TRUE( RunViewAction( state, "Level: Mip 0", kMips ) );
    EXPECT_EQ( SV::ResolveLevel( state.Level, kMips ), ( SV::Level{ SV::Cube::Radiance, 0.0f } ) );

    // No action names a level outside the list.
    EXPECT_FALSE( RunViewAction( state, "Level: Mip " + std::to_string( kMips + 1u ), kMips ) );
}

TEST( SkyboxViewLevels, ProjectionAndEVActionsMoveOnlyTheirField )
{
    SV::ViewState state;
    ASSERT_TRUE( RunViewAction( state, "View: 2D", 8u ) );
    EXPECT_EQ( state.View, SV::Projection::LongLat2D );
    ASSERT_TRUE( RunViewAction( state, "EV +1", 8u ) );
    ASSERT_TRUE( RunViewAction( state, "EV +1", 8u ) );
    EXPECT_FLOAT_EQ( state.ExposureEV, 2.0f );
    EXPECT_FLOAT_EQ( SV::ExposureFromEV( state.ExposureEV ), 4.0f );
    EXPECT_EQ( state.Level, 0 );
    for ( int i = 0; i < 30; ++i )
        ASSERT_TRUE( RunViewAction( state, "EV +1", 8u ) );
    EXPECT_FLOAT_EQ( state.ExposureEV, SV::kMaxEV ); // the slider's range, not beyond it
    ASSERT_TRUE( RunViewAction( state, "EV 0", 8u ) );
    EXPECT_FLOAT_EQ( state.ExposureEV, 0.0f );
    ASSERT_TRUE( RunViewAction( state, "View: 3D", 8u ) );
    EXPECT_EQ( state.View, SV::Projection::Sphere3D );
}

TEST( SkyboxViewLevels, EverySampledFieldChangesTheFingerprintSoTheGateRendersAgain )
{
    const SV::ViewState base;
    SV::ViewState       turned = base;
    ASSERT_TRUE( RunViewAction( turned, "Rotate +90 deg", 8u ) );
    EXPECT_FLOAT_EQ( turned.RotationDegrees, 90.0f );
    EXPECT_NE( SV::Fingerprint( turned ), SV::Fingerprint( base ) );

    SV::ViewState level = base;
    level.Level         = 1;
    EXPECT_NE( SV::Fingerprint( level ), SV::Fingerprint( base ) );

    SV::ViewState unwrapped = base;
    unwrapped.View          = SV::Projection::LongLat2D;
    EXPECT_NE( SV::Fingerprint( unwrapped ), SV::Fingerprint( base ) );

    // Rotation wraps into the slider's range: 90 + 90 + 90 = 270 = -90.
    ASSERT_TRUE( RunViewAction( turned, "Rotate +90 deg", 8u ) );
    ASSERT_TRUE( RunViewAction( turned, "Rotate +90 deg", 8u ) );
    EXPECT_NEAR( turned.RotationDegrees, -90.0f, 1e-4f );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
