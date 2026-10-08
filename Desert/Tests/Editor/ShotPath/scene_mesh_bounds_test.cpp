#include <gtest/gtest.h>

#include <Editor/LevelEditor/SceneMeshBounds.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>

#include <memory>

using namespace Desert;
using namespace Desert::Editor;

namespace
{
    // The primitive geometry with no device: PrimitiveMeshFactory::Create is what GetShared builds before it
    // uploads, so the submeshes are the ones the renderer's shared mesh holds.
    const std::vector<Submesh>* CpuPrimitiveSubmeshes( const Geometry::PrimitiveType type )
    {
        static std::shared_ptr<DynamicMesh> s_Cube =
             Geometry::PrimitiveMeshFactory::Create( Geometry::PrimitiveType::Cube );
        return type == Geometry::PrimitiveType::Cube && s_Cube ? &s_Cube->GetSubmeshes() : nullptr;
    }

    entt::entity PlacePrimitive( entt::registry& registry, const glm::vec3& at, const glm::vec3& scale,
                                 const Geometry::PrimitiveType type )
    {
        const entt::entity entity = registry.create();
        auto&              xf     = registry.emplace<ECS::TransformComponent>( entity );
        xf.Translation            = at;
        xf.Scale                  = scale;
        registry.emplace<ECS::StaticMeshComponent>( entity ).Primitive = type;
        return entity;
    }
} // namespace

// A PRIMITIVE has neither a RuntimeMesh nor a handle; it is drawn from the shared primitive mesh, and the
// frame must measure it from that mesh — leaving it out framed Clouds_Showcase's cubes as "no measurable
// mesh". Two cubes: the box is the union of both, through each one's transform, and nothing is "missing".
TEST( SceneMeshBounds, APrimitiveIsMeasuredFromThePrimitiveMesh )
{
    entt::registry registry;
    PlacePrimitive( registry, glm::vec3( 1000.0f, 0.0f, 0.0f ), glm::vec3( 2.0f ), Geometry::PrimitiveType::Cube );
    PlacePrimitive( registry, glm::vec3( -500.0f, 300.0f, 0.0f ), glm::vec3( 1.0f ),
                    Geometry::PrimitiveType::Cube );

    const auto* cube = CpuPrimitiveSubmeshes( Geometry::PrimitiveType::Cube );
    ASSERT_NE( cube, nullptr );
    const auto local = Geometry::LocalBounds( *cube );
    ASSERT_FALSE( Geometry::IsEmpty( local ) );

    const SceneMeshBounds bounds = MeasureSceneMeshes( registry, &CpuPrimitiveSubmeshes );
    EXPECT_EQ( bounds.Meshes, 2 );
    EXPECT_EQ( bounds.Missing, 0 );
    EXPECT_FLOAT_EQ( bounds.Box.Max.x, 1000.0f + 2.0f * local.Max.x );
    EXPECT_FLOAT_EQ( bounds.Box.Min.x, -500.0f + local.Min.x );
    EXPECT_FLOAT_EQ( bounds.Box.Max.y, 300.0f + local.Max.y );
    EXPECT_FLOAT_EQ( bounds.Box.Min.y, 2.0f * local.Min.y );
}

// A primitive type the factory does not build is not drawn either: neither measured nor "missing".
TEST( SceneMeshBounds, APrimitiveWithNoGeometryIsNeitherMeasuredNorMissing )
{
    entt::registry registry;
    PlacePrimitive( registry, glm::vec3( 0.0f ), glm::vec3( 1.0f ), Geometry::PrimitiveType::Sphere );

    const SceneMeshBounds bounds = MeasureSceneMeshes( registry, &CpuPrimitiveSubmeshes );
    EXPECT_EQ( bounds.Meshes, 0 );
    EXPECT_EQ( bounds.Missing, 0 );
    EXPECT_TRUE( Geometry::IsEmpty( bounds.Box ) );
}
