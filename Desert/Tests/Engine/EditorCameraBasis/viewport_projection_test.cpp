#include <Engine/Core/Projection.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>

using namespace Desert::Core;

namespace
{
    glm::mat4 EditorPerspective( float fovXDegrees, float width, float height )
    {
        const float aspect = width / height;
        return MakePerspective( VerticalFovKeepingHorizontal( glm::radians( fovXDegrees ), aspect ), aspect,
                                kDefaultNearPlane, kDefaultFarPlane );
    }
} // namespace

TEST( ViewportProjection, NarrowingTheViewportKeepsTheWidthOfSceneItShows )
{
    const glm::mat4 wide   = EditorPerspective( kEditorViewportFovXDegrees, 1920.0f, 1080.0f );
    const glm::mat4 narrow = EditorPerspective( kEditorViewportFovXDegrees, 900.0f, 1080.0f );

    EXPECT_NEAR( wide[0][0], narrow[0][0], 1e-5f );
    EXPECT_LT( narrow[1][1], wide[1][1] );
}

TEST( ViewportProjection, TheHorizontalFovIsTheAuthoredOne )
{
    const glm::mat4 p = EditorPerspective( 90.0f, 1600.0f, 900.0f );
    EXPECT_NEAR( p[0][0], 1.0f / glm::tan( glm::radians( 45.0f ) ), 1e-5f );
}

TEST( ViewportProjection, ASquareViewportHasEqualFovOnBothAxes )
{
    EXPECT_NEAR( VerticalFovKeepingHorizontal( glm::half_pi<float>(), 1.0f ), glm::half_pi<float>(), 1e-6f );
    EXPECT_LT( VerticalFovKeepingHorizontal( glm::half_pi<float>(), 16.0f / 9.0f ), glm::half_pi<float>() );
}
