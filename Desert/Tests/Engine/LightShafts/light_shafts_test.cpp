// The pure maths of the light-shaft pass: where the sun lands on screen and how the streak source
// fades at the edge — Engine/Graphic/PostProcessing/LightShaftRules.hpp, header-only, no renderer.
//
// The property that matters most is the one that cannot be seen in a single frame: a sun BEHIND the
// camera must contribute nothing, because a radial blur toward a sun behind the camera streaks away
// from where the light comes from and reads as a black hole in the sky.

#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

using Desert::Graphic::ComputeSunScreen;
using Desert::Graphic::LightShaftEdgeFade;
using Desert::Graphic::SunScreen;

namespace
{
    // A real camera, not an identity: the mapping under test is the inverse of the raymarch's own
    // uv->ndc convention, and only a genuine perspective matrix can prove the two agree.
    glm::mat4 MakeViewProjection( const glm::vec3& eye, const glm::vec3& forward )
    {
        const glm::mat4 projection = glm::perspective( glm::radians( 60.0f ), 16.0f / 9.0f, 0.1f, 10000.0f );
        const glm::mat4 view       = glm::lookAt( eye, eye + forward, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        return projection * view;
    }
} // namespace

TEST( LightShaftRules, TheSunAheadLandsAtScreenCentreWithFullFade )
{
    const glm::mat4 vp  = MakeViewProjection( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ) );
    const SunScreen sun = ComputeSunScreen( vp, glm::vec3( 0.0f, 0.0f, -1.0f ) );

    EXPECT_NEAR( sun.Uv.x, 0.5f, 1e-4f );
    EXPECT_NEAR( sun.Uv.y, 0.5f, 1e-4f );
    EXPECT_FLOAT_EQ( sun.Fade, 1.0f );
}

TEST( LightShaftRules, TheSunBehindTheCameraContributesNothing )
{
    const glm::mat4 vp  = MakeViewProjection( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ) );
    const SunScreen sun = ComputeSunScreen( vp, glm::vec3( 0.0f, 0.0f, 1.0f ) );

    EXPECT_FLOAT_EQ( sun.Fade, 0.0f );
}

TEST( LightShaftRules, ScreenUvIsYDown )
{
    // A sun ABOVE the view axis must land in the UPPER half of the image, which in the engine's
    // y-down screen UV is uv.y < 0.5 — the exact inverse of the compute passes' uv->ndc mapping.
    const glm::mat4 vp  = MakeViewProjection( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ) );
    const SunScreen sun = ComputeSunScreen( vp, glm::normalize( glm::vec3( 0.0f, 0.4f, -1.0f ) ) );

    EXPECT_LT( sun.Uv.y, 0.5f );
    EXPECT_NEAR( sun.Uv.x, 0.5f, 1e-4f );
    EXPECT_GT( sun.Fade, 0.0f );
}

TEST( LightShaftRules, EdgeFadeIsOneInsideAndZeroFarOutsideAndMonotone )
{
    EXPECT_FLOAT_EQ( LightShaftEdgeFade( glm::vec2( 0.0f, 0.0f ) ), 1.0f );
    EXPECT_FLOAT_EQ( LightShaftEdgeFade( glm::vec2( 0.99f, 0.5f ) ), 1.0f );
    EXPECT_FLOAT_EQ( LightShaftEdgeFade( glm::vec2( 1.7f, 0.0f ) ), 0.0f );
    EXPECT_FLOAT_EQ( LightShaftEdgeFade( glm::vec2( 0.0f, -1.7f ) ), 0.0f );

    // Fading, never popping: monotone non-increasing as the sun walks off the screen.
    float previous = 2.0f;
    for ( float x = 0.0f; x <= 2.0f; x += 0.05f )
    {
        const float fade = LightShaftEdgeFade( glm::vec2( x, 0.0f ) );
        EXPECT_LE( fade, previous + 1e-6f ) << "ndc.x = " << x;
        previous = fade;
    }
}
