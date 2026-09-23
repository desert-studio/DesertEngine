// THE HDR SKY'S LOOKUP, AND WHERE ITS AUTHORED LOOK IS APPLIED.
//
// An HDR skybox becomes THREE images from ONE panorama: PanoramaToCubemap writes the radiance cube the
// background is drawn from and the GGX prefilter convolves, and DiffuseIrradiance integrates the same
// panorama into the cube every lit surface reads. Those cubes are the FILE as authored. The sky's look —
// rotation, intensity, tint — is applied where they are SAMPLED (Common/SkyLook.glslh), so a slider drag
// is a uniform write per frame and not a device-idling rebake per value.
//
// Two properties of FILES AGREEING, which no frame can state:
//   * every program that reads an environment cube applies the look — one that did not would show the
//     unturned sky in its reflections under a turned backdrop, which looks like a lighting opinion. It
//     is exactly the shape the engine shipped for `Intensity`'s whole life (applied in the skybox pass
//     alone, so a sky authored at 5x lit the world at 1x);
//   * no bake program applies it — one that did would apply it TWICE and bring the per-value rebake
//     back through the cache key.
// The census at the bottom of this file makes both unspellable.

#include "SkyPanoramaReference.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm> // std::clamp -- libc++ pulls it in transitively, MSVC does not (Windows Debug, 2026-09-23)
#include <cmath>
#include <filesystem>
#include <regex>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Tests::SkyPanoramaRef::ApplySkyGain;
using Desert::Tests::SkyPanoramaRef::PanoramaSampleUV;
using Desert::Tests::SkyPanoramaRef::SkyLookDirection;

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

TEST( SkyPanoramaLookup, IsTheTextbookEquirectMapping )
{
    // The mapping the two shaders held their own copies of before this header existed, written out once
    // here so the move is a refactor with a witness rather than a rewrite anyone has to take on trust.
    for ( const glm::vec3& d : Directions() )
    {
        const float phi   = std::atan2( d.z, d.x );
        const float theta = std::acos( std::clamp( d.y, -1.0f, 1.0f ) );

        const glm::vec2 uv = PanoramaSampleUV( d );
        EXPECT_NEAR( uv.x, phi / ( 2.0f * kPi ) + 0.5f, 1e-6f );
        EXPECT_NEAR( uv.y, theta / kPi, 1e-6f );
    }
}

TEST( SkyLookLookup, IdentityYawReadsTheCubeWhereItWasBaked )
{
    for ( const glm::vec3& d : Directions() )
    {
        const glm::vec3 r = SkyLookDirection( d, CosSin( 0.0f ) );
        EXPECT_NEAR( glm::length( r - d ), 0.0f, 1e-6f );
    }
}

TEST( SkyLookLookup, TheImageTurnsWITHTheAngleAndNotAgainstIt )
{
    // THE RELATION, not a value: a feature the unrotated sky showed in direction `d` must be found in
    // direction `d rotated by +yaw` once the sky is rotated by +yaw. An author who types 90 expects the
    // sun that was in the east to end up in the north, and a sign error here gives them 270 with a
    // frame that looks perfectly plausible. The cube stores the unrotated sky, so "found" means: the
    // turned direction, read through the look, lands on the texel `d` has in the cube.
    for ( const float yaw : { 15.0f, 90.0f, 180.0f, 270.0f, 359.0f } )
    {
        for ( const glm::vec3& d : Directions() )
        {
            const glm::vec3 read = SkyLookDirection( RotateAboutUp( d, yaw ), CosSin( yaw ) );
            EXPECT_NEAR( glm::length( read - d ), 0.0f, 1e-5f ) << "yaw " << yaw;
        }
    }
}

TEST( SkyLookLookup, TheRotationIsTheOneTheBakeUsedToApply )
{
    // THE BEFORE/AFTER RELATION. The bake used to write, at cube direction `d`, the panorama texel
    // PanoramaSampleUV-with-yaw(d); the sampler now reads the identity cube at SkyLookDirection(d). Those
    // are the same texel only if SkyLookDirection is the rotation the bake's lookup performed — written
    // out here from the old text (rotation about +Y by -yaw), so a sign flip in either reddens this.
    for ( const float yaw : { 15.0f, 90.0f, 200.0f } )
    {
        const glm::vec2 cs = CosSin( yaw );
        for ( const glm::vec3& d : Directions() )
        {
            const glm::vec3 oldLookup( cs.x * d.x - cs.y * d.z, d.y, cs.x * d.z + cs.y * d.x );
            const glm::vec2 oldUV = PanoramaSampleUV( oldLookup );
            const glm::vec2 newUV = PanoramaSampleUV( SkyLookDirection( d, cs ) );
            EXPECT_NEAR( oldUV.x, newUV.x, 1e-6f ) << "yaw " << yaw;
            EXPECT_NEAR( oldUV.y, newUV.y, 1e-6f ) << "yaw " << yaw;
        }
    }
}

TEST( SkyLookLookup, IsARotationAboutUpAndNothingElse )
{
    // Why applying it at the SAMPLE is exact and not an approximation of applying it at the bake: the
    // irradiance and prefilter convolutions commute with a rigid rotation, and only with one. So the look
    // must preserve every length and angle, and must leave the elevation alone — a general matrix, or a
    // rotation about the wrong axis, would tilt the horizon, and on an azimuthally uniform sky nobody
    // would see it.
    for ( const float yaw : { 37.0f, 123.0f, 300.0f } )
    {
        const glm::vec2 cs = CosSin( yaw );
        for ( const glm::vec3& a : Directions() )
        {
            const glm::vec3 ra = SkyLookDirection( a, cs );
            EXPECT_NEAR( glm::length( ra ), glm::length( a ), 1e-6f );
            EXPECT_NEAR( ra.y, a.y, 1e-6f ) << "yaw " << yaw << " moved the elevation";
            for ( const glm::vec3& b : Directions() )
                EXPECT_NEAR( glm::dot( ra, SkyLookDirection( b, cs ) ), glm::dot( a, b ), 1e-5f );
        }
    }
}

TEST( SkyPanoramaLookup, AnOverlongDirectionDoesNotProduceNaN )
{
    // acos of anything past 1 is NaN, and ONE NaN texel poisons every mip the prefilter builds from the
    // cube. The clamp is why the header takes a direction rather than insisting on a unit vector.
    for ( const float scale : { 1.0f + 1e-6f, 1.5f, 8.0f } )
    {
        const glm::vec2 uv = PanoramaSampleUV( glm::vec3( 0, scale, 0 ) );
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
// The census: the bake reads the panorama bare, and EVERY reader of the cubes applies the look
// ---------------------------------------------------------------------------------------------------

TEST( SkyPanoramaCensus, BothBakeProgramsReadThePanoramaBareThroughTheSharedText )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's working directory";

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
        // The mapping itself, in either program, is the second copy the header exists to remove.
        EXPECT_EQ( code.find( "atan(" ), std::string::npos )
             << program.Path << " spells its own direction->UV mapping again";

        // THE LOOK MAY NOT COME BACK HERE. Applied at the bake it would be applied twice (the readers
        // apply it too), and it would put the per-value rebake the owner complained about back in.
        for ( const char* banned : { "skyLook", "SkyLookUB", "ApplySkyGain(", "SkyLookDirection(" } )
            EXPECT_EQ( code.find( banned ), std::string::npos )
                 << program.Path << " (" << program.Why << ") applies the sky's look ('" << banned
                 << "') — the cubes must be the file as authored";
    }
}

TEST( SkyLookCensus, EveryProgramThatReadsAnEnvironmentCubeAppliesTheLook )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // THE REGISTER — every shader text that declares a cube the environment services fill, named, one
    // row each. It is compared with what a scan of the shader tree FINDS, in both directions: a new
    // reader of the cubes that nobody listed reddens this before it can ship an unturned reflection, and
    // a row whose file stopped declaring a cube reddens it too, so the list cannot rot into prose.
    const std::set<std::string> registered = {
         "Common/GraphSurfaceLighting.glslh",         // every lit shader-graph surface
         "Programs/Deferred/DeferredLighting.shader", // the deferred composite's ambient
         "Programs/PBR/SkinnedMeshPBR.shader",
         "Programs/PBR/StaticMeshGlass.shader", // the reflection at the glass's grazing edge
         "Programs/PBR/StaticMeshPBR.shader",
         "Programs/PBR/StaticMeshPBR_Instanced.shader",
         "Programs/Preview/CubemapSphere.shader", // the Details panel's ball beside the sliders
         "Programs/Skybox/Skybox.shader",         // the backdrop
    };

    // The names the engine binds environment cubes under: the IBL pair, the skybox pass's cube, and
    // the preview ball's. A `samplerCube` declared under any of them is a reader of the environment.
    static const std::regex kEnvCube(
         R"(samplerCube\s+(u_EnvSpecularTex|u_EnvIrradianceTex|samplerCubeMap|u_CubeMap)\b)" );

    const std::string     shaders = root + "Editor/Resources/Shaders/";
    std::set<std::string> found;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string ext = entry.path().extension().string();
        if ( ext != ".shader" && ext != ".glslh" )
            continue;
        const std::string code = CodeOnly( ReadFile( entry.path().string() ) );
        if ( !std::regex_search( code, kEnvCube ) )
            continue;

        const std::string rel = std::filesystem::relative( entry.path(), shaders ).generic_string();
        found.insert( rel );

        EXPECT_NE( code.find( "SkyLookUB" ), std::string::npos )
             << rel
             << " reads an environment cube but declares no SkyLookUB — it would show the sky "
                "unturned and at unit intensity while the backdrop is turned and scaled";
        // Through the shared text, directly or via AmbientIBL (which calls both on its two fetches).
        const bool direct = code.find( "SkyLookDirection(" ) != std::string::npos &&
                            code.find( "ApplySkyGain(" ) != std::string::npos;
        const bool viaIbl = code.find( "#include <Mesh/AmbientIBL.glslh>" ) != std::string::npos;
        EXPECT_TRUE( direct || viaIbl ) << rel << " declares the look but never reads through it";
    }

    EXPECT_EQ( found, registered ) << "the set of environment-cube readers changed; update the register "
                                      "above only after the new reader applies the look";

    // AmbientIBL is what `viaIbl` trusts, so it is held to the same rule: both fetches, both calls.
    const std::string ibl = CodeOnly( ReadFile( shaders + "Mesh/AmbientIBL.glslh" ) );
    ASSERT_FALSE( ibl.empty() );
    EXPECT_NE( ibl.find( "texture(u_EnvIrradianceTex, SkyLookDirection(" ), std::string::npos );
    EXPECT_NE( ibl.find( "textureLod(u_EnvSpecularTex, SkyLookDirection(" ), std::string::npos );
    size_t gains = 0;
    for ( size_t at = ibl.find( "ApplySkyGain(" ); at != std::string::npos;
          at        = ibl.find( "ApplySkyGain(", at + 1 ) )
        ++gains;
    EXPECT_EQ( gains, 2u ) << "each of AmbientIBL's two cube fetches must be scaled by the sky's gain";
}

TEST( SkyPanoramaCensus, TheSkyboxProgramHasNoBrightnessOfItsOwn )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // THE DEFECT THE LOOK'S SINGLE ROUTE REPLACES. `SkyboxParamsUB` carried SkyboxComponent::Intensity
    // into the fullscreen sky pass — and only there. The backdrop's brightness now comes through
    // SkyLookUB, the same block and the same call every lit surface uses; a second multiplier here would
    // make the backdrop brighter than the light it casts, which nothing in a frame distinguishes from a
    // deliberate look.
    const std::string code =
         CodeOnly( ReadFile( root + "Editor/Resources/Shaders/Programs/Skybox/Skybox.shader" ) );
    ASSERT_FALSE( code.empty() );

    EXPECT_EQ( code.find( "SkyboxParamsUB" ), std::string::npos )
         << "the skybox program has a brightness uniform of its own again";
    EXPECT_EQ( code.find( "u_SkyboxParams" ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
