// M22a: collision from a mesh, UE's Collision Complexity in Jolt terms.
//
//   1. Mesh (complex as simple): the triangles themselves, a MeshShape — a ball dropped on a floor made of
//      them comes to rest on it, and a ray meets the very triangle it was aimed at.
//   2. ConvexHull (simple): the hull of the points, a ConvexHullShape — a dynamic cube falls onto the Mesh
//      floor and rests on its face; a dense point cloud is simplified by Jolt, not refused.
//   3. Refusals by name: a Mesh on a dynamic body, no points, a broken index list.
//   4. Shapes are cooked once per content: equal data shares one shape, different data does not.

#include <Engine/Physics/PhysicsWorld.hpp>

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    constexpr float kGravity = 981.0f;
    constexpr float kStep    = 1.0f / 60.0f;

    struct TriangleData
    {
        std::vector<glm::vec3> Points;
        std::vector<uint32_t>  Indices;
    };

    // A flat floor at y = 0, n × n quads of 500 cm, wound so every face normal points up (+y).
    TriangleData Floor( int n )
    {
        TriangleData floor;
        const float  half = 0.5f * static_cast<float>( n ) * 500.0f;
        for ( int z = 0; z <= n; ++z )
            for ( int x = 0; x <= n; ++x )
                floor.Points.emplace_back( static_cast<float>( x ) * 500.0f - half, 0.0f,
                                           static_cast<float>( z ) * 500.0f - half );
        const auto at = [n]( int x, int z ) { return static_cast<uint32_t>( z * ( n + 1 ) + x ); };
        for ( int z = 0; z < n; ++z )
            for ( int x = 0; x < n; ++x )
                floor.Indices.insert( floor.Indices.end(),
                                      { at( x, z ), at( x, z + 1 ), at( x + 1, z ), at( x + 1, z ), at( x, z + 1 ),
                                        at( x + 1, z + 1 ) } );
        return floor;
    }

    std::vector<glm::vec3> CubeCorners( float half )
    {
        std::vector<glm::vec3> corners;
        for ( int i = 0; i < 8; ++i )
            corners.emplace_back( ( i & 1 ) ? half : -half, ( i & 2 ) ? half : -half, ( i & 4 ) ? half : -half );
        return corners;
    }

    Physics::BodyDesc MeshBody( const TriangleData& data, Physics::BodyType type )
    {
        Physics::BodyDesc desc;
        desc.Shape       = Physics::ShapeType::Mesh;
        desc.Type        = type;
        desc.MeshPoints  = data.Points;
        desc.MeshIndices = data.Indices;
        return desc;
    }

    struct World
    {
        Physics::PhysicsWorld Physics;
        World()
        {
            EXPECT_TRUE( Physics.Init( kGravity ) );
        }
        ~World()
        {
            Physics.Shutdown();
        }

        void Run( float seconds )
        {
            for ( int i = 0; i < static_cast<int>( seconds / kStep ); ++i )
                Physics.Step( kStep );
        }
    };

    bool Mentions( const std::string& error, const char* word )
    {
        return error.find( word ) != std::string::npos;
    }
} // namespace

TEST( MeshCollision, BallComesToRestOnAMeshFloor )
{
    World              world;
    const TriangleData floor = Floor( 4 );
    ASSERT_TRUE( world.Physics.CreateBody( MeshBody( floor, Physics::BodyType::Static ) ).IsSuccess() );

    Physics::BodyDesc ball;
    ball.Shape       = Physics::ShapeType::Sphere;
    ball.Radius      = 25.0f;
    ball.Restitution = 0.0f;
    ball.Position    = { 130.0f, 200.0f, -70.0f };
    const auto body  = world.Physics.CreateBody( ball );
    ASSERT_TRUE( body.IsSuccess() ) << body.GetError();

    world.Run( 3.0f );
    const glm::vec3 settled = world.Physics.GetPosition( body.GetValue() );
    world.Run( 0.5f );
    const glm::vec3 later = world.Physics.GetPosition( body.GetValue() );

    // On the surface — not through it (y << 25) and not held above it (y >> 25) — and still.
    EXPECT_NEAR( settled.y, 25.0f, 1.0f );
    EXPECT_NEAR( later.y, settled.y, 0.1f );
}

TEST( MeshCollision, ConvexHullCubeFallsAndRestsOnItsFace )
{
    World              world;
    const TriangleData floor = Floor( 4 );
    ASSERT_TRUE( world.Physics.CreateBody( MeshBody( floor, Physics::BodyType::Static ) ).IsSuccess() );

    const std::vector<glm::vec3> corners = CubeCorners( 50.0f );
    Physics::BodyDesc            cube;
    cube.Shape       = Physics::ShapeType::ConvexHull;
    cube.MeshPoints  = corners;
    cube.Mass        = 10.0f;
    cube.Restitution = 0.0f;
    cube.Position    = { -200.0f, 300.0f, 100.0f };
    const auto body  = world.Physics.CreateBody( cube );
    ASSERT_TRUE( body.IsSuccess() ) << body.GetError();

    world.Run( 4.0f );
    const glm::vec3 rest = world.Physics.GetPosition( body.GetValue() );
    EXPECT_NEAR( rest.y, 50.0f, 1.5f ); // half the cube above the floor: lying on a face, not sunk
    EXPECT_NEAR( rest.x, -200.0f, 5.0f );
    EXPECT_NEAR( rest.z, 100.0f, 5.0f );
}

TEST( MeshCollision, ConvexHullOfADenseCloudIsSimplifiedNotRefused )
{
    World                  world;
    std::vector<glm::vec3> cloud; // 40 × 50 points on a sphere: far past the hull's 256-point cap
    for ( int i = 0; i < 40; ++i )
        for ( int j = 0; j < 50; ++j )
        {
            const float theta = 3.14159265f * ( static_cast<float>( i ) + 0.5f ) / 40.0f;
            const float phi   = 6.2831853f * static_cast<float>( j ) / 50.0f;
            cloud.emplace_back( 100.0f * std::sin( theta ) * std::cos( phi ), 100.0f * std::cos( theta ),
                                100.0f * std::sin( theta ) * std::sin( phi ) );
        }
    Physics::BodyDesc desc;
    desc.Shape      = Physics::ShapeType::ConvexHull;
    desc.MeshPoints = cloud;
    const auto body = world.Physics.CreateBody( desc );
    EXPECT_TRUE( body.IsSuccess() ) << body.GetError();
}

TEST( MeshCollision, RayMeetsTheTriangleItIsAimedAt )
{
    World world;
    // One triangle on the plane y = x / 2, over (0,0), (0,1000), (1000,0) in xz; its normal faces up.
    TriangleData slope;
    slope.Points    = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1000.0f }, { 1000.0f, 500.0f, 0.0f } };
    slope.Indices   = { 0u, 1u, 2u };
    const auto body = world.Physics.CreateBody( MeshBody( slope, Physics::BodyType::Static ) );
    ASSERT_TRUE( body.IsSuccess() ) << body.GetError();

    const auto hit = world.Physics.CastRay( { 400.0f, 1000.0f, 200.0f }, { 0.0f, -1.0f, 0.0f }, 5000.0f );
    ASSERT_TRUE( hit.has_value() );
    EXPECT_EQ( hit->Body, body.GetValue() );
    EXPECT_NEAR( hit->Point.y, 200.0f, 0.05f );
    EXPECT_NEAR( hit->Distance, 800.0f, 0.05f );
    const glm::vec3 normal = glm::normalize( glm::vec3( -0.5f, 1.0f, 0.0f ) );
    EXPECT_GT( glm::dot( hit->Normal, normal ), 0.999f );

    // Outside the triangle (x + z > 1000) there is nothing to meet.
    EXPECT_FALSE(
         world.Physics.CastRay( { 800.0f, 1000.0f, 800.0f }, { 0.0f, -1.0f, 0.0f }, 5000.0f ).has_value() );
}

TEST( MeshCollision, RefusalsNameTheReason )
{
    World              world;
    const TriangleData floor = Floor( 1 );

    const auto dynamicMesh = world.Physics.CreateBody( MeshBody( floor, Physics::BodyType::Dynamic ) );
    ASSERT_FALSE( dynamicMesh.IsSuccess() );
    EXPECT_TRUE( Mentions( dynamicMesh.GetError(), "Dynamic" ) ) << dynamicMesh.GetError();

    const auto noPoints = world.Physics.CreateBody( MeshBody( TriangleData{}, Physics::BodyType::Static ) );
    ASSERT_FALSE( noPoints.IsSuccess() );
    EXPECT_TRUE( Mentions( noPoints.GetError(), "no points" ) ) << noPoints.GetError();

    Physics::BodyDesc emptyHull;
    emptyHull.Shape      = Physics::ShapeType::ConvexHull;
    const auto noHullPts = world.Physics.CreateBody( emptyHull );
    ASSERT_FALSE( noHullPts.IsSuccess() );
    EXPECT_TRUE( Mentions( noHullPts.GetError(), "ConvexHull" ) ) << noHullPts.GetError();

    TriangleData ragged = floor;
    ragged.Indices.pop_back();
    const auto notTriangles = world.Physics.CreateBody( MeshBody( ragged, Physics::BodyType::Static ) );
    ASSERT_FALSE( notTriangles.IsSuccess() );
    EXPECT_TRUE( Mentions( notTriangles.GetError(), "multiple of three" ) ) << notTriangles.GetError();

    TriangleData outOfRange   = floor;
    outOfRange.Indices.back() = static_cast<uint32_t>( floor.Points.size() ); // one past the last point
    const auto badIndex       = world.Physics.CreateBody( MeshBody( outOfRange, Physics::BodyType::Static ) );
    ASSERT_FALSE( badIndex.IsSuccess() );
    EXPECT_TRUE( Mentions( badIndex.GetError(), "out of range" ) ) << badIndex.GetError();

    EXPECT_EQ( world.Physics.GetBodyCount(), 0u ); // no refusal left a body behind
}

TEST( MeshCollision, ShapesAreCookedOncePerContent )
{
    World              world;
    const TriangleData floor = Floor( 2 );
    ASSERT_TRUE( world.Physics.CreateBody( MeshBody( floor, Physics::BodyType::Static ) ).IsSuccess() );
    ASSERT_TRUE( world.Physics.CreateBody( MeshBody( floor, Physics::BodyType::Static ) ).IsSuccess() );
    EXPECT_EQ( world.Physics.GetCookedShapeCount(), 1u );

    TriangleData raised = floor;
    for ( glm::vec3& p : raised.Points )
        p.y += 10.0f;
    ASSERT_TRUE( world.Physics.CreateBody( MeshBody( raised, Physics::BodyType::Static ) ).IsSuccess() );
    EXPECT_EQ( world.Physics.GetCookedShapeCount(), 2u );

    // The same points as a hull are a different shape, not the mesh's.
    Physics::BodyDesc hull;
    hull.Shape                           = Physics::ShapeType::ConvexHull;
    hull.Type                            = Physics::BodyType::Static;
    const std::vector<glm::vec3> corners = CubeCorners( 50.0f );
    hull.MeshPoints                      = corners;
    ASSERT_TRUE( world.Physics.CreateBody( hull ).IsSuccess() );
    EXPECT_EQ( world.Physics.GetCookedShapeCount(), 3u );
    EXPECT_EQ( world.Physics.GetBodyCount(), 4u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
