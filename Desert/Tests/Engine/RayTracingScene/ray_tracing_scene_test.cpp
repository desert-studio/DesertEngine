// RT-SCENE: the TLAS instance list built from the primitives MeshRenderer queued (count, transforms, masks, rows,
// the skipped ones counted by reason) and the gate that keeps every TLAS cost away from a frame whose resolved
// methods trace no hardware rays.

#include <Engine/Graphic/RenderMethods.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <vector>

using namespace Desert::Graphic;
using namespace Desert::Graphic::RayTracing;
using Common::Scalability::GIMethod;
using Common::Scalability::ReflectionMethod;
using Common::Scalability::ShadowMethod;

namespace
{
    // Distinct non-null BLAS keys; the list never dereferences them.
    int kRock  = 1;
    int kCrate = 2;
} // namespace

// Red if the gate ignores a method, or answers true for the raster methods (a TLAS built every frame for nothing).
TEST( RayTracingScene, NoRayTracedMethodMeansNoTlas )
{
    RenderMethods methods; // Off / Off / CascadedMaps
    methods.GlobalIllumination.Effective = GIMethod::ScreenSpace;
    methods.Reflections.Effective        = ReflectionMethod::ScreenSpace;
    EXPECT_FALSE( RayTracingSceneRequired( methods ) );

    // A REQUESTED hardware method that was walked down (device, ceiling) does not count: only the effective one.
    methods.Shadows.Requested = ShadowMethod::HardwareTraced;
    methods.Shadows.Effective = ShadowMethod::CascadedMaps;
    EXPECT_FALSE( RayTracingSceneRequired( methods ) );
}

TEST( RayTracingScene, EachHardwareTracedMethodAsksForTheTlas )
{
    {
        RenderMethods methods;
        methods.Shadows.Effective = ShadowMethod::HardwareTraced;
        EXPECT_TRUE( RayTracingSceneRequired( methods ) );
    }
    {
        RenderMethods methods;
        methods.Reflections.Effective = ReflectionMethod::HardwareTraced;
        EXPECT_TRUE( RayTracingSceneRequired( methods ) );
    }
    {
        RenderMethods methods;
        methods.GlobalIllumination.Effective = GIMethod::PathTraced;
        EXPECT_TRUE( RayTracingSceneRequired( methods ) );
    }
}

// Red if a skipped primitive enters the TLAS (a skinned mesh in bind pose, a mesh with no BLAS, a hidden submesh
// casting a shadow) or if a skip is not counted under its reason.
TEST( RayTracingScene, InstanceListKeepsTracablePrimitivesAndCountsTheRest )
{
    std::vector<RayTracingPrimitive> primitives( 6 );
    primitives[0].Geometry        = &kRock;
    primitives[0].GpuSceneRow     = 4;
    primitives[1].Geometry        = nullptr; // not resident
    primitives[2].Geometry        = &kCrate;
    primitives[2].Skinned         = true;
    primitives[3].Geometry        = &kCrate;
    primitives[3].GpuSceneRow     = 9;
    primitives[3].CastShadows     = false;
    primitives[4].Geometry        = &kRock;
    primitives[4].HiddenSubmeshes = 0b10;
    primitives[5].Geometry        = &kRock;
    primitives[5].GpuSceneRow     = kMaxGpuSceneRow + 1;

    const RayTracingInstanceList list = BuildRayTracingInstances( primitives );

    ASSERT_EQ( list.Instances.size(), 2u );
    EXPECT_EQ( list.Instances[0].Geometry, &kRock );
    EXPECT_EQ( list.Instances[0].CustomIndex, 4u );
    EXPECT_EQ( list.Instances[0].Mask, kMaskOpaque | kMaskShadowCaster );
    EXPECT_EQ( list.Instances[1].Geometry, &kCrate );
    EXPECT_EQ( list.Instances[1].CustomIndex, 9u );
    EXPECT_EQ( list.Instances[1].Mask, kMaskOpaque ); // CastShadows off: invisible to shadow rays

    EXPECT_EQ( list.SkippedFor( RayTracingSkip::NoGeometry ), 1u );
    EXPECT_EQ( list.SkippedFor( RayTracingSkip::Skinned ), 1u );
    EXPECT_EQ( list.SkippedFor( RayTracingSkip::HiddenSubmeshes ), 1u );
    EXPECT_EQ( list.SkippedFor( RayTracingSkip::RowOutOfRange ), 1u );
}

// Red if the 3x4 is written column-major (glm's order): the translation would land in the bottom row, which a
// VkTransformMatrixKHR does not have, and every instance would sit at the origin, sheared.
TEST( RayTracingScene, TransformIsTheRowMajorTopThreeRowsInCentimetres )
{
    RayTracingPrimitive primitive;
    primitive.Geometry = &kRock;
    primitive.World    = glm::translate( glm::mat4( 1.0f ), glm::vec3( 150.0f, -20.0f, 3000.0f ) ) *
                      glm::scale( glm::mat4( 1.0f ), glm::vec3( 2.0f, 3.0f, 4.0f ) );

    const RayTracingInstanceList list = BuildRayTracingInstances( std::span( &primitive, 1 ) );
    ASSERT_EQ( list.Instances.size(), 1u );
    const auto& t = list.Instances[0].Transform;
    EXPECT_FLOAT_EQ( t[0], 2.0f );
    EXPECT_FLOAT_EQ( t[5], 3.0f );
    EXPECT_FLOAT_EQ( t[10], 4.0f );
    EXPECT_FLOAT_EQ( t[3], 150.0f );
    EXPECT_FLOAT_EQ( t[7], -20.0f );
    EXPECT_FLOAT_EQ( t[11], 3000.0f );
    EXPECT_FLOAT_EQ( t[1], 0.0f );
    EXPECT_FLOAT_EQ( t[4], 0.0f );
}
