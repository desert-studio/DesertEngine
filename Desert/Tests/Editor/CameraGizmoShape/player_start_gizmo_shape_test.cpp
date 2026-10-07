// UI-PAWN — the PlayerStart gizmo's shape (UE's APlayerStart: capsule + ArrowComponent).
//
// The old gizmo was a 2D screen line: constant in pixels, no body. These are relations the world-space
// shape must hold and the old one could not: the arrow's tip is exactly Length along the facing (a
// world distance, so it scales with the camera), the capsule is exactly the pawn's size, upright
// whatever the start's pitch, and the arrow is a CLOSED solid (every face outward).

#include <gtest/gtest.h>

#include <Editor/Panels/ViewportPanel/Tools/PlayerStartGizmoMath.hpp>

#include <algorithm>
#include <cmath>

using namespace Desert::Editor::Tools;

namespace
{
    constexpr float kEps = 1e-3f;
}

TEST( PlayerStartGizmoShape, ArrowTipIsLengthAlongTheFacing )
{
    const glm::vec3 origin( 100.0f, 50.0f, -30.0f );
    const glm::vec3 facing      = glm::normalize( glm::vec3( 1.0f, 0.0f, -1.0f ) );
    float           farDistance = -1.0f;
    for ( const GizmoTriangle& t : BuildArrowMesh( origin, facing ) )
        for ( const glm::vec3& p : t.P )
            farDistance = std::max( farDistance, glm::dot( p - origin, facing ) );
    EXPECT_NEAR( farDistance, kPlayerStartArrowLength, kEps );
}

TEST( PlayerStartGizmoShape, ArrowIsAClosedOutwardSolid )
{
    const glm::vec3 origin( 0.0f );
    const glm::vec3 facing( 0.0f, 0.0f, -1.0f );
    const auto      mesh = BuildArrowMesh( origin, facing );
    ASSERT_FALSE( mesh.empty() );
    // Every face's normal points away from the arrow's axis or along it at an end: never inward.
    for ( const GizmoTriangle& t : mesh )
    {
        const glm::vec3 mid    = ( t.P[0] + t.P[1] + t.P[2] ) / 3.0f;
        const float     along  = glm::dot( mid - origin, facing );
        const glm::vec3 radial = ( mid - origin ) - facing * along;
        const float     out    = glm::dot( t.Normal, radial ) + std::abs( glm::dot( t.Normal, facing ) );
        EXPECT_GT( out, -kEps );
        EXPECT_NEAR( glm::length( t.Normal ), 1.0f, kEps );
    }
}

TEST( PlayerStartGizmoShape, CapsuleIsThePawnsSizeAndCentred )
{
    const glm::vec3 centre( 10.0f, 92.0f, 20.0f );
    const float     radius = 40.0f;  // UE's default pawn: 40 cm,
    const float     height = 184.0f; // half-height 92
    float           top    = -1e9f;
    float           bottom = 1e9f;
    float           widest = 0.0f;
    for ( const GizmoSegment& s : BuildPawnCapsuleWire( centre, glm::vec3( 0.0f, 0.0f, -1.0f ), radius, height ) )
        for ( const glm::vec3& p : { s.A, s.B } )
        {
            top    = std::max( top, p.y );
            bottom = std::min( bottom, p.y );
            widest = std::max( widest, glm::length( glm::vec2( p.x - centre.x, p.z - centre.z ) ) );
        }
    EXPECT_NEAR( top - bottom, height, kEps );
    EXPECT_NEAR( ( top + bottom ) * 0.5f, centre.y, kEps );
    EXPECT_NEAR( widest, radius, kEps );
}

TEST( PlayerStartGizmoShape, CapsuleStaysUprightWhateverThePitch )
{
    const auto level   = BuildPawnCapsuleWire( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), 30.0f, 180.0f );
    const auto pitched = BuildPawnCapsuleWire( glm::vec3( 0.0f ),
                                               glm::normalize( glm::vec3( 0.0f, -1.0f, -1.0f ) ), 30.0f, 180.0f );
    ASSERT_EQ( level.size(), pitched.size() );
    for ( size_t i = 0; i < level.size(); ++i )
    {
        EXPECT_NEAR( glm::length( level[i].A - pitched[i].A ), 0.0f, kEps );
        EXPECT_NEAR( glm::length( level[i].B - pitched[i].B ), 0.0f, kEps );
    }
}
