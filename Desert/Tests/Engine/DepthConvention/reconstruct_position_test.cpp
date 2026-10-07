// GBUF1 — the world position of a G-buffer pixel comes from its depth, not from a world-position target.
//
// These tests run Editor/Resources/Shaders/Common/ReconstructPosition.glslh compiled as C++
// (ReconstructPositionReference.hpp): world -> clip -> (uv, device depth) the way the rasteriser and the
// negative-height viewport store it -> ReconstructWorldPosition -> world, for the projections the engine renders
// depth with: reversed-Z perspective and reversed-Z orthographic (Core/Projection.hpp, the scene), and standard-Z
// orthographic (the shadow cascades).
//
// Mutations that turn these red: drop the divide by w (perspective cases), keep NDC y = 2*uv.y - 1 (no flip),
// remap depth to 2*d - 1 (GL range), or pass the forward instead of the inverse matrix.

#include <Engine/Core/Projection.hpp>

#include "ReconstructPositionReference.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace ReconstructPositionTest
{
    // What the rasteriser stores for a world point: the screen uv of its pixel (NDC y = +1 is the top row,
    // FullscreenTriangle / ScreenUV) and the device depth z/w.
    struct Stored
    {
        glm::vec2 Uv;
        float     Depth;
    };

    Stored Rasterise( const glm::mat4& viewProjection, const glm::vec3& world )
    {
        const glm::vec4 clip = viewProjection * glm::vec4( world, 1.0f );
        const glm::vec3 ndc  = glm::vec3( clip ) / clip.w;
        return { glm::vec2( ( ndc.x + 1.0f ) * 0.5f, ( 1.0f - ndc.y ) * 0.5f ), ndc.z };
    }

    glm::mat4 ViewAt( const glm::vec3& eye, const glm::vec3& target )
    {
        return glm::lookAtRH( eye, target, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    }

    // Points in front of a camera at (300, 200, 900) cm looking at the origin, from 1 m to 400 m away.
    std::vector<glm::vec3> Samples()
    {
        return { { 0.0f, 0.0f, 0.0f },     { 150.0f, -40.0f, 20.0f },   { -250.0f, 80.0f, -300.0f },
                 { 30.0f, 10.0f, 700.0f }, { 2000.0f, 0.0f, -9000.0f }, { -6000.0f, 500.0f, -38000.0f } };
    }

    // The relative tolerance of a float32 round trip through a reversed-Z depth: a few ulps of the distance.
    void ExpectRoundTrip( const glm::mat4& viewProjection, const glm::vec3& eye, float relativeTolerance )
    {
        const glm::mat4 inverse = glm::inverse( viewProjection );
        for ( const glm::vec3& world : Samples() )
        {
            const Stored    stored = Rasterise( viewProjection, world );
            const glm::vec3 back   = Desert::Tests::ReconstructPositionRef::ReconstructWorldPosition(
                 stored.Uv, stored.Depth, inverse );
            const float distance = glm::length( world - eye );
            EXPECT_LE( glm::length( back - world ), relativeTolerance * distance + 0.01f )
                 << "world (" << world.x << ", " << world.y << ", " << world.z << ") came back (" << back.x << ", "
                 << back.y << ", " << back.z << ")";
        }
    }

    const glm::vec3 kEye{ 300.0f, 200.0f, 900.0f };
} // namespace ReconstructPositionTest

using namespace ReconstructPositionTest;

TEST( DepthConvention, ReconstructionRoundTripsReversedZPerspective )
{
    const glm::mat4 projection = Desert::Core::MakePerspective(
         glm::radians( 70.0f ), 16.0f / 9.0f, Desert::Core::kDefaultNearPlane, Desert::Core::kDefaultFarPlane );
    ExpectRoundTrip( projection * ViewAt( kEye, glm::vec3( 0.0f ) ), kEye, 1.0e-4f );
}

TEST( DepthConvention, ReconstructionRoundTripsReversedZOrthographic )
{
    const glm::mat4 projection = Desert::Core::MakeOrthographic(
         -8000.0f, 8000.0f, -4500.0f, 4500.0f, Desert::Core::kDefaultNearPlane, Desert::Core::kDefaultFarPlane );
    ExpectRoundTrip( projection * ViewAt( kEye, glm::vec3( 0.0f ) ), kEye, 1.0e-4f );
}

TEST( DepthConvention, ReconstructionRoundTripsStandardZOrthographic )
{
    // The shadow cascades' convention (standard-Z ortho): the same text, because the depth range is the matrix's.
    const glm::mat4 projection = glm::orthoRH_ZO( -8000.0f, 8000.0f, -4500.0f, 4500.0f, 10.0f, 50000.0f );
    ExpectRoundTrip( projection * ViewAt( kEye, glm::vec3( 0.0f ) ), kEye, 1.0e-4f );
}

TEST( DepthConvention, ReconstructionPutsTheTopLeftTexelAtTheTopLeftOfTheView )
{
    // The negative-height viewport: uv (0,0) is NDC (-1, +1). A point reconstructed at uv (0,0) on the near plane
    // lies up and to the left of the view axis.
    const glm::mat4 view       = ViewAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ) );
    const glm::mat4 projection = Desert::Core::MakePerspective( glm::radians( 90.0f ), 1.0f, 10.0f, 1000.0f );
    const glm::vec3 topLeft    = Desert::Tests::ReconstructPositionRef::ReconstructWorldPosition(
         glm::vec2( 0.0f, 0.0f ), Desert::Core::kDepthNear, glm::inverse( projection * view ) );
    EXPECT_NEAR( topLeft.x, -10.0f, 1.0e-3f );
    EXPECT_NEAR( topLeft.y, 10.0f, 1.0e-3f );
    EXPECT_NEAR( topLeft.z, -10.0f, 1.0e-3f );
}
