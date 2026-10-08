// DST-03b: a destructible entity is an object of the scene's DestructionWorld while it exists — added at its
// world pose by DestructibleLifetime::Sync, released by on_destroy — and the component's per-level damage
// thresholds replace the bake's.

#include <Engine/ECS/System/DestructibleLifetime.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <memory>

using namespace Desert;
using namespace Desert::Destruction;

namespace
{
    constexpr float           kStep = 1.0f / 60.0f;
    const Assets::AssetHandle kFractureHandle( uint64_t{ 7 } );

    FractureNode BoxLeaf( const glm::vec3& lo, const glm::vec3& hi, float threshold )
    {
        FractureNode leaf;
        leaf.Parent          = 0;
        leaf.Level           = 1;
        leaf.DamageThreshold = threshold;
        const glm::vec3 size = hi - lo;
        leaf.Volume          = static_cast<double>( size.x ) * size.y * size.z;
        leaf.CenterOfMass    = glm::dvec3( ( lo + hi ) * 0.5f );
        for ( int i = 0; i < 8; ++i )
            leaf.HullVertices.emplace_back( ( i & 1 ) != 0 ? hi.x : lo.x, ( i & 2 ) != 0 ? hi.y : lo.y,
                                            ( i & 4 ) != 0 ? hi.z : lo.z );
        leaf.HullFaces = { { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 },
                           { 2, 6, 7, 3 }, { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };
        return leaf;
    }

    // Two touching cubes under a root; the bake's threshold is out of reach.
    std::shared_ptr<const FractureData> TwoCubes()
    {
        auto         data = std::make_shared<FractureData>();
        FractureNode root;
        root.Volume       = 2.0e6;
        root.CenterOfMass = glm::dvec3( 0.0, 50.0, 50.0 );
        data->Nodes.push_back( root );
        data->Nodes.push_back( BoxLeaf( { -100.0f, 0.0f, 0.0f }, { 0.0f, 100.0f, 100.0f }, 1.0e30f ) );
        data->Nodes.push_back( BoxLeaf( { 0.0f, 0.0f, 0.0f }, { 100.0f, 100.0f, 100.0f }, 1.0e30f ) );
        return data;
    }

    struct Fixture
    {
        Physics::PhysicsWorld                      physics;
        std::unique_ptr<DestructionWorld>          destruction;
        std::unique_ptr<ECS::DestructibleLifetime> lifetime;
        entt::registry                             registry;
        std::shared_ptr<const FractureData>        fracture = TwoCubes();
        bool                                       loaded   = true;

        Fixture()
        {
            physics.Init( 0.0f );
            destruction = std::make_unique<DestructionWorld>( physics );
            lifetime    = std::make_unique<ECS::DestructibleLifetime>( *destruction );
            lifetime->Attach( registry );
        }
        ~Fixture()
        {
            lifetime.reset();
            destruction.reset();
            physics.Shutdown();
        }

        void Sync()
        {
            lifetime->Sync(
                 registry,
                 [this](
                      const Assets::AssetHandle& handle ) -> Common::ResultStr<std::shared_ptr<const FractureData>>
                 {
                     if ( handle != kFractureHandle )
                         return Common::MakeError<std::shared_ptr<const FractureData>>( "no such fracture" );
                     return Common::MakeSuccess( loaded ? fracture : std::shared_ptr<const FractureData>{} );
                 } );
        }

        entt::entity Spawn( const glm::vec3& at, Assets::AssetHandle handle = kFractureHandle )
        {
            const entt::entity entity                                            = registry.create();
            registry.emplace<ECS::TransformComponent>( entity ).Translation      = at;
            registry.emplace<ECS::DestructibleComponent>( entity ).Data.Fracture = handle;
            return entity;
        }

        uint32_t Object( entt::entity entity )
        {
            return registry.get<ECS::DestructibleComponent>( entity ).RuntimeObject;
        }
    };
} // namespace

TEST( DestructibleComponent, SyncAddsTheObjectAtTheEntityPose )
{
    Fixture    f;
    const auto entity = f.Spawn( { 300.0f, 20.0f, -40.0f } );
    f.Sync();
    ASSERT_NE( f.Object( entity ), kInvalidDestructible );
    EXPECT_EQ( f.destruction->GetBodyCount( f.Object( entity ) ), 1u );
    const auto body = f.destruction->GetNodeBody( f.Object( entity ), 1 );
    ASSERT_NE( body, Physics::kInvalidBody );
    // The root body is placed at the entity: its origin or its centre of mass, both inside the object.
    const glm::vec3 position = f.physics.GetPosition( body );
    EXPECT_LT( glm::length( position - glm::vec3( 300.0f, 20.0f, -40.0f ) ), 150.0f );
}

TEST( DestructibleComponent, PendingFractureIsAskedAgain )
{
    Fixture f;
    f.loaded          = false;
    const auto entity = f.Spawn( { 0.0f, 0.0f, 0.0f } );
    f.Sync();
    EXPECT_EQ( f.Object( entity ), kInvalidDestructible );
    f.loaded = true;
    f.Sync();
    EXPECT_NE( f.Object( entity ), kInvalidDestructible );
}

TEST( DestructibleComponent, ComponentThresholdReplacesTheBake )
{
    Fixture    f;
    const auto baked    = f.Spawn( { 0.0f, 0.0f, 0.0f } );
    const auto override = f.Spawn( { 1000.0f, 0.0f, 0.0f } );
    // Level 1 breaks at the first step (a threshold of zero or less, PBDRigidClustering.cpp:1730).
    f.registry.get<ECS::DestructibleComponent>( override ).Data.DamageThreshold = { 1.0e30f, 0.0f };
    f.Sync();
    f.physics.Step( kStep );
    EXPECT_EQ( f.destruction->GetBodyCount( f.Object( baked ) ), 1u );
    EXPECT_EQ( f.destruction->GetBodyCount( f.Object( override ) ), 2u );
}

TEST( DestructibleComponent, DestroyingTheEntityRemovesItsBodies )
{
    Fixture    f;
    const auto entity = f.Spawn( { 0.0f, 0.0f, 0.0f } );
    f.Sync();
    const uint32_t object = f.Object( entity );
    ASSERT_NE( object, kInvalidDestructible );
    f.registry.destroy( entity );
    EXPECT_EQ( f.destruction->GetBodyCount( object ), 0u );
    EXPECT_EQ( f.destruction->GetNodeBody( object, 1 ), Physics::kInvalidBody );
}

TEST( DestructibleComponent, RemovingTheComponentRemovesItsBodies )
{
    Fixture    f;
    const auto entity = f.Spawn( { 0.0f, 0.0f, 0.0f } );
    f.Sync();
    const uint32_t object = f.Object( entity );
    f.registry.remove<ECS::DestructibleComponent>( entity );
    EXPECT_EQ( f.destruction->GetBodyCount( object ), 0u );
}

TEST( DestructibleComponent, RefusalsAreNamedAndNotRetried )
{
    Fixture    f;
    const auto empty   = f.Spawn( { 0.0f, 0.0f, 0.0f }, Assets::AssetHandle::Null() );
    const auto missing = f.Spawn( { 0.0f, 0.0f, 0.0f }, Assets::AssetHandle( uint64_t{ 99 } ) );
    const auto scaled  = f.Spawn( { 0.0f, 0.0f, 0.0f } );
    f.registry.get<ECS::TransformComponent>( scaled ).Scale = glm::vec3( 2.0f );
    f.Sync();
    f.Sync();
    EXPECT_EQ( f.Object( empty ), kInvalidDestructible );
    EXPECT_EQ( f.Object( missing ), kInvalidDestructible );
    EXPECT_EQ( f.Object( scaled ), kInvalidDestructible );
}
