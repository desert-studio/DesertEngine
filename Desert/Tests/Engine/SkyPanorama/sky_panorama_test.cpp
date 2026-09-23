// THE HDR SKY'S LOOKUP, AND THE RULE THAT BOTH BAKE PROGRAMS APPLY IT.
//
// An HDR skybox becomes THREE images from ONE panorama: PanoramaToCubemap writes the radiance cube the
// background is drawn from and the GGX prefilter convolves, and DiffuseIrradiance integrates the same
// panorama into the cube every lit surface reads. The sky's authored look — rotation, intensity, tint —
// is applied at that step and nowhere else, so the picture and the light it casts cannot disagree.
//
// That is a property of TWO FILES AGREEING, and no frame can state it: a rotation that reached the
// radiance cube and missed the irradiance cube would turn the visible sky while leaving the ambient
// where it was, which looks like a lighting opinion and not like a defect. It is exactly the shape the
// engine shipped for `Intensity`'s whole life (applied in the skybox fragment shader, so a sky authored
// at 5x lit the world at 1x), and the census at the bottom of this file is what makes it unspellable.

#include "SkyPanoramaReference.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm> // std::clamp -- libc++ pulls it in transitively, MSVC does not (Windows Debug, 2026-09-23)
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Tests::SkyPanoramaRef::ApplySkyGain;
using Desert::Tests::SkyPanoramaRef::PanoramaSampleUV;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    glm::vec2 CosSin( float degrees )
    {
        const float yaw = glm::radians( degrees );
        return glm::vec2( std::cos( yaw ), std::sin( yaw ) );
    }

    // A world direction rotated about +Y by `degrees`, built with glm rather than with the header's own
    // arithmetic — a test that rotates with the code under test can only ever agree with itself.
    glm::vec3 RotateAboutUp( const glm::vec3& d, float degrees )
    {
        const glm::mat4 m = glm::rotate( glm::mat4( 1.0f ), glm::radians( degrees ), glm::vec3( 0, 1, 0 ) );
        return glm::vec3( m * glm::vec4( d, 0.0f ) );
    }

    const std::vector<glm::vec3>& Directions()
    {
        static const std::vector<glm::vec3> all = {
             glm::normalize( glm::vec3( 1, 0, 0 ) ),
             glm::normalize( glm::vec3( -1, 0, 0 ) ),
             glm::normalize( glm::vec3( 0, 0, 1 ) ),
             glm::normalize( glm::vec3( 0, 0, -1 ) ),
             glm::normalize( glm::vec3( 1, 1, 1 ) ),
             glm::normalize( glm::vec3( -0.3f, 0.8f, 0.5f ) ),
             glm::normalize( glm::vec3( 0.6f, -0.4f, -0.7f ) ),
        };
        return all;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Resources/Shaders/Common/SkyPanorama.glslh" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Comment lines removed before anything is searched for. A census that fires on prose is a census
    // that gets switched off — it has happened twice here, once taking a real finding down with it.
    std::string CodeOnly( const std::string& source )
    {
        std::istringstream in( source );
        std::ostringstream out;
        std::string        line;
        while ( std::getline( in, line ) )
        {
            const auto first = line.find_first_not_of( " \t" );
            if ( first != std::string::npos && line.compare( first, 2, "//" ) == 0 )
                continue;
            out << line << '\n';
        }
        return out.str();
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The numbers
// ---------------------------------------------------------------------------------------------------

TEST( SkyPanoramaLookup, IdentityYawIsTheTextbookEquirectMapping )
{
    // The mapping the two shaders held their own copies of before this header existed, written out once
    // here so the move is a refactor with a witness rather than a rewrite anyone has to take on trust.
    for ( const glm::vec3& d : Directions() )
    {
        const float phi   = std::atan2( d.z, d.x );
        const float theta = std::acos( std::clamp( d.y, -1.0f, 1.0f ) );

        const glm::vec2 uv = PanoramaSampleUV( d, CosSin( 0.0f ) );
        EXPECT_NEAR( uv.x, phi / ( 2.0f * kPi ) + 0.5f, 1e-6f );
        EXPECT_NEAR( uv.y, theta / kPi, 1e-6f );
    }
}

TEST( SkyPanoramaLookup, TheImageTurnsWITHTheAngleAndNotAgainstIt )
{
    // THE RELATION, not a value: a feature the unrotated sky showed in direction `d` must be found in
    // direction `d rotated by +yaw` once the sky is rotated by +yaw. An author who types 90 expects the
    // sun that was in the east to end up in the north, and a sign error here gives them 270 with a
    // frame that looks perfectly plausible.
    for ( const float yaw : { 15.0f, 90.0f, 180.0f, 270.0f, 359.0f } )
    {
        for ( const glm::vec3& d : Directions() )
        {
            const glm::vec2 before = PanoramaSampleUV( d, CosSin( 0.0f ) );
            const glm::vec2 after  = PanoramaSampleUV( RotateAboutUp( d, yaw ), CosSin( yaw ) );

            // u wraps: 0.0 and 1.0 are the same texel column, so compare the wrapped distance.
            const float du = std::fabs( after.x - before.x );
            EXPECT_NEAR( std::min( du, 1.0f - du ), 0.0f, 1e-5f ) << "yaw " << yaw;
            EXPECT_NEAR( after.y, before.y, 1e-5f ) << "yaw " << yaw;
        }
    }
}

TEST( SkyPanoramaLookup, YawTouchesTheAZIMUTHONLY )
{
    // The negative half of the same statement, and the one a frame CAN be fooled about: the elevation of
    // a texel must be untouched by a yaw. A rotation built as a general matrix — or about the wrong axis
    // — would tilt the horizon, and on an azimuthally uniform sky nobody would see it.
    for ( const float yaw : { 37.0f, 123.0f, 300.0f } )
    {
        for ( const glm::vec3& d : Directions() )
            EXPECT_NEAR( PanoramaSampleUV( d, CosSin( yaw ) ).y, PanoramaSampleUV( d, CosSin( 0.0f ) ).y, 1e-6f )
                 << "yaw " << yaw;
    }
}

TEST( SkyPanoramaLookup, APoleIsUnMOVEDByAnyYaw )
{
    // Straight up and straight down are fixed points of a rotation about up. This is the frame control's
    // own statement in numbers: the probe panorama's zenith is one flat colour, so a 180-degree yaw must
    // leave the zenith frame byte-identical while the horizon changes completely.
    for ( const float yaw : { 5.0f, 90.0f, 180.0f, 271.0f } )
    {
        for ( const glm::vec3& pole : { glm::vec3( 0, 1, 0 ), glm::vec3( 0, -1, 0 ) } )
            EXPECT_NEAR( PanoramaSampleUV( pole, CosSin( yaw ) ).y, PanoramaSampleUV( pole, CosSin( 0.0f ) ).y,
                         1e-6f );
    }
}

TEST( SkyPanoramaLookup, AnOverlongDirectionDoesNotProduceNaN )
{
    // acos of anything past 1 is NaN, and ONE NaN texel poisons every mip the prefilter builds from the
    // cube. The clamp is why the header takes a direction rather than insisting on a unit vector.
    for ( const float scale : { 1.0f + 1e-6f, 1.5f, 8.0f } )
    {
        const glm::vec2 uv = PanoramaSampleUV( glm::vec3( 0, scale, 0 ), CosSin( 0.0f ) );
        EXPECT_FALSE( std::isnan( uv.x ) );
        EXPECT_FALSE( std::isnan( uv.y ) );
        EXPECT_NEAR( uv.y, 0.0f, 1e-6f );
    }
}

TEST( SkyPanoramaGain, ScalesEachChannelIndependently )
{
    const glm::vec3 radiance( 0.25f, 0.5f, 1.0f );
    EXPECT_EQ( ApplySkyGain( radiance, glm::vec3( 1.0f ) ), radiance );

    const glm::vec3 tinted = ApplySkyGain( radiance, glm::vec3( 2.0f, 0.5f, 0.0f ) );
    EXPECT_NEAR( tinted.r, 0.5f, 1e-6f );
    EXPECT_NEAR( tinted.g, 0.25f, 1e-6f );
    EXPECT_NEAR( tinted.b, 0.0f, 1e-6f );
}

// ---------------------------------------------------------------------------------------------------
// The census: BOTH bake programs apply the look, and NEITHER carries its own mapping
// ---------------------------------------------------------------------------------------------------

TEST( SkyPanoramaCensus, BothBakeProgramsReadThePanoramaThroughTheSharedText )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

    // WHAT THIS FORBIDS, said out loud so the next reader knows what it is guarding: a compute program
    // that reads the sky panorama and spells the direction->UV mapping, the yaw, or the gain itself.
    // Any of the three gives the engine two descriptions of one sky.
    struct Program
    {
        const char* Path;
        const char* Why;
    };
    const Program programs[] = {
         { "Editor/Resources/Shaders/Programs/Compute/PanoramaToCubemap.shader",
           "writes the radiance cube the background is drawn from and the prefilter convolves" },
         { "Editor/Resources/Shaders/Programs/Compute/DiffuseIrradiance.shader",
           "integrates the same panorama into the cube every lit surface reads" },
    };

    for ( const Program& program : programs )
    {
        const std::string source = ReadFile( root + program.Path );
        ASSERT_FALSE( source.empty() ) << program.Path << " could not be read";
        const std::string code = CodeOnly( source );

        EXPECT_NE( code.find( "#include <Common/SkyPanorama.glslh>" ), std::string::npos )
             << program.Path << " (" << program.Why << ") does not include the shared lookup";
        EXPECT_NE( code.find( "PanoramaSampleUV(" ), std::string::npos )
             << program.Path << " (" << program.Why << ") does not call PanoramaSampleUV";
        EXPECT_NE( code.find( "ApplySkyGain(" ), std::string::npos )
             << program.Path << " (" << program.Why
             << ") does not apply the sky's gain — its cube would ignore intensity and tint while the "
                "other one honours them";
        EXPECT_NE( code.find( "skyLook.YawCosSin" ), std::string::npos )
             << program.Path << " declares no yaw to apply";

        // The mapping itself, in either program, is the second copy this header exists to remove.
        EXPECT_EQ( code.find( "atan(" ), std::string::npos )
             << program.Path << " spells its own direction->UV mapping again";
    }
}

TEST( SkyPanoramaCensus, TheSkyboxProgramHasNoBrightnessOfItsOwn )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // THE DEFECT THIS WHOLE ARRANGEMENT REPLACES. `SkyboxParamsUB` carried SkyboxComponent::Intensity
    // into the fullscreen sky pass — and only there. Re-adding a multiplier to this program would make
    // the backdrop brighter than the cube that lights the scene, which is the state the owner reported
    // and the one nothing in a frame distinguishes from a deliberate look.
    const std::string code =
         CodeOnly( ReadFile( root + "Editor/Resources/Shaders/Programs/Skybox/Skybox.shader" ) );
    ASSERT_FALSE( code.empty() );

    EXPECT_EQ( code.find( "SkyboxParamsUB" ), std::string::npos )
         << "the skybox program has a brightness uniform again; the sky's intensity belongs in the cube";
    EXPECT_EQ( code.find( "u_SkyboxParams" ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
