// Destroying an entity: O(1) in the size of the world, and it takes its Jolt body with it.
//
// Two defects WorldPartition streaming would have hit on its first unload (WP6):
//   * Scene::DestroyEntity was O(N) in the WORLD — linear search, erase from the middle, then a pass over the
//     whole UUID map to shift indices — so unloading a cell of k entities cost O(k * N);
//   * nothing ever called PhysicsWorld::RemoveBody / RemoveCharacter, so a destroyed entity's collider
//     stayed in the simulation until Stop.
//
// The suite drives the real units — SceneEntityIndex + DestroyEntityTree (what Scene::DestroyEntity is), and
// PhysicsBodyLifetime against a real Jolt PhysicsWorld — on a bare registry, because Scene itself drags the
// renderer in. The timing test asserts a RELATION (cost per destroy barely moves when the world grows 33x),
// not milliseconds, so it holds under a sanitizer or a loaded machine.

#include <Engine/Core/SceneEntityIndex.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/PhysicsBodyLifetime.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
    using namespace Desert;

    entt::entity Make( entt::registry& reg, Core::SceneEntityIndex& index, const Common::UUID& id = Common::UUID::Generate() )
    {
        // The same four components Scene::CreateEntityWithUUID gives every entity.
        const entt::entity e = reg.create();
        reg.emplace<ECS::TagComponent>( e );
        reg.emplace<ECS::UUIDComponent>( e, id );
        reg.emplace<ECS::TransformComponent>( e );
        reg.emplace<ECS::RelationshipComponent>( e );
        index.Add( id, e, reg );
        return e;
    }

    void Attach( entt::registry& reg, entt::entity parent, entt::entity child )
    {
        reg.get<ECS::RelationshipComponent>( parent ).Children.push_back( child );
        reg.get<ECS::RelationshipComponent>( child ).Parent = parent;
    }

    // Every entity the index lists is alive, is found by its own UUID, and is found as ITSELF.
    void ExpectIndexConsistent( const entt::registry& reg, const Core::SceneEntityIndex& index )
    {
        for ( const ECS::Entity& e : index.All() )
        {
            ASSERT_TRUE( reg.valid( e.GetHandle() ) );
            const auto&        id    = reg.get<ECS::UUIDComponent>( e.GetHandle() ).UUID;
            const ECS::Entity* found = index.Find( id );
            ASSERT_NE( found, nullptr );
            EXPECT_EQ( found->GetHandle(), e.GetHandle() );
        }
    }
} // namespace

TEST( EntityDestroy, RemovingFromTheMiddleKeepsEveryOtherLookupRight )
{
    entt::registry         reg;
    Core::SceneEntityIndex index;
    std::vector<entt::entity> all;
    for ( int i = 0; i < 200; ++i )
        all.push_back( Make( reg, index ) );

    std::mt19937 rng( 7 );
    std::shuffle( all.begin(), all.end(), rng );
    for ( int i = 0; i < 150; ++i )
    {
        const auto id = reg.get<ECS::UUIDComponent>( all[i] ).UUID;
        Core::DestroyEntityTree( reg, index, all[i] );
        EXPECT_EQ( index.Find( id ), nullptr );
        ExpectIndexConsistent( reg, index );
    }
    EXPECT_EQ( index.Size(), 50u );
    EXPECT_EQ( reg.alive(), 50u );
}

TEST( EntityDestroy, ADuplicatedUuidDoesNotOrphanTheNewerEntity )
{
    entt::registry         reg;
    Core::SceneEntityIndex index;
    const Common::UUID     shared = Common::UUID::Generate();
    const entt::entity     older  = Make( reg, index, shared );
    const entt::entity     newer  = Make( reg, index, shared );

    EXPECT_EQ( index.Find( shared )->GetHandle(), newer );
    Core::DestroyEntityTree( reg, index, older );
    ASSERT_NE( index.Find( shared ), nullptr ) << "removing the older twin dropped the newer one's lookup";
    EXPECT_EQ( index.Find( shared )->GetHandle(), newer );
    EXPECT_EQ( index.Size(), 1u );
}

TEST( EntityDestroy, ASubtreeGoesWholeAndOnlyTheSurvivingParentIsEdited )
{
    entt::registry         reg;
    Core::SceneEntityIndex index;
    const entt::entity     keep   = Make( reg, index );
    const entt::entity     folder = Make( reg, index );
    const entt::entity     other  = Make( reg, index );
    Attach( reg, keep, folder );
    Attach( reg, keep, other );
    for ( int i = 0; i < 20; ++i )
    {
        const entt::entity child = Make( reg, index );
        Attach( reg, folder, child );
        Attach( reg, child, Make( reg, index ) );
    }

    Core::DestroyEntityTree( reg, index, folder );

    EXPECT_EQ( reg.alive(), 2u );
    EXPECT_EQ( index.Size(), 2u );
    const auto& siblings = reg.get<ECS::RelationshipComponent>( keep ).Children;
    ASSERT_EQ( siblings.size(), 1u );
    EXPECT_EQ( siblings.front(), other ) << "the surviving sibling's order entry was disturbed";
    ExpectIndexConsistent( reg, index );
}

// ---- Jolt ------------------------------------------------------------------------------------------------

namespace
{
    entt::entity MakeBody( entt::registry& reg, Core::SceneEntityIndex& index, Physics::PhysicsWorld& world,
                           float x )
    {
        const entt::entity e = Make( reg, index );
        reg.emplace<ECS::ColliderComponent>( e );
        auto&             rb = reg.emplace<ECS::RigidBodyComponent>( e );
        Physics::BodyDesc desc;
        desc.Position  = { x, 0.0f, 0.0f };
        rb.RuntimeBody = world.CreateBody( desc );
        return e;
    }

    entt::entity MakeCharacter( entt::registry& reg, Core::SceneEntityIndex& index, Physics::PhysicsWorld& world )
    {
        const entt::entity e  = Make( reg, index );
        auto&              cc = reg.emplace<ECS::CharacterControllerComponent>( e );
        cc.RuntimeCharacter   = world.CreateCharacter( {} );
        return e;
    }
} // namespace

TEST( EntityDestroy, DestroyingTheEntityGivesItsJoltBodyAndCharacterBack )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    entt::registry              reg;
    Core::SceneEntityIndex      index;
    ECS::PhysicsBodyLifetime    lifetime( world );
    lifetime.Attach( reg );

    const uint32_t bodiesBefore = world.GetBodyCount();
    std::vector<entt::entity> doomed;
    for ( int i = 0; i < 64; ++i )
        doomed.push_back( MakeBody( reg, index, world, 300.0f * static_cast<float>( i ) ) );
    doomed.push_back( MakeCharacter( reg, index, world ) );
    doomed.push_back( MakeCharacter( reg, index, world ) );
    ASSERT_EQ( world.GetBodyCount(), bodiesBefore + 64u );
    ASSERT_EQ( world.GetCharacterCount(), 2u );
    world.Step( 1.0f / 30.0f ); // the bodies have been simulated, as they would be in Play

    for ( entt::entity e : doomed )
        Core::DestroyEntityTree( reg, index, e );

    EXPECT_EQ( world.GetBodyCount(), bodiesBefore ) << "a destroyed entity left its Jolt body in the world";
    EXPECT_EQ( world.GetCharacterCount(), 0u ) << "a destroyed entity left its CharacterVirtual behind";

    // A released character slot is reused, so a streamed world does not grow the slot table forever.
    const entt::entity again = MakeCharacter( reg, index, world );
    EXPECT_EQ( reg.get<ECS::CharacterControllerComponent>( again ).RuntimeCharacter, 0u );

    lifetime.Detach();
    world.Shutdown();
}

TEST( EntityDestroy, AParentsSubtreeAndARemovedColliderAlsoGiveTheirBodiesBack )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    entt::registry           reg;
    Core::SceneEntityIndex   index;
    ECS::PhysicsBodyLifetime lifetime( world );
    lifetime.Attach( reg );

    const uint32_t     before = world.GetBodyCount();
    const entt::entity house  = Make( reg, index );
    for ( int i = 0; i < 8; ++i )
        Attach( reg, house, MakeBody( reg, index, world, 200.0f * static_cast<float>( i ) ) );
    const entt::entity loose = MakeBody( reg, index, world, -500.0f );
    ASSERT_EQ( world.GetBodyCount(), before + 9u );

    Core::DestroyEntityTree( reg, index, house );
    EXPECT_EQ( world.GetBodyCount(), before + 1u ) << "the children of a destroyed parent kept their bodies";

    // The system's view needs RigidBody AND Collider; losing the collider must not strand the body.
    reg.remove<ECS::ColliderComponent>( loose );
    EXPECT_EQ( world.GetBodyCount(), before );
    EXPECT_EQ( reg.get<ECS::RigidBodyComponent>( loose ).RuntimeBody, Physics::kInvalidBody )
         << "the handle must reset so a collider added back rebuilds the body";

    lifetime.Detach();
    world.Shutdown();
}

// Negative control: the SAME scenario with no listener attached leaks, so the count above is an instrument
// that can see the defect it guards against.
TEST( EntityDestroy, WithoutTheListenerTheBodiesLeak )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    entt::registry         reg;
    Core::SceneEntityIndex index;

    const uint32_t before = world.GetBodyCount();
    const entt::entity e  = MakeBody( reg, index, world, 0.0f );
    Core::DestroyEntityTree( reg, index, e );
    EXPECT_EQ( world.GetBodyCount(), before + 1u );
    world.Shutdown();
}

// ---- cost ------------------------------------------------------------------------------------------------

namespace
{
    // Destroys 1 000 random entities out of a flat world of @p worldSize and answers nanoseconds per destroy.
    // Flat, because that is the shape of World_Grid8km: Tools/WorldGen/Source/WorldBuild.cpp emits 32 x 32
    // cells of one ground tile + 48 buildings plus sun, sky and camera — 50 179 entities, none parented.
    double NanosecondsPerDestroy( std::size_t worldSize )
    {
        entt::registry            reg;
        Core::SceneEntityIndex    index;
        std::vector<entt::entity> all;
        all.reserve( worldSize );
        for ( std::size_t i = 0; i < worldSize; ++i )
        {
            const entt::entity e = Make( reg, index );
            reg.emplace<ECS::StaticMeshComponent>( e );
            all.push_back( e );
        }
        std::mt19937 rng( 11 );
        std::shuffle( all.begin(), all.end(), rng );

        constexpr std::size_t kDestroyed = 1000;
        const auto            start      = std::chrono::steady_clock::now();
        for ( std::size_t i = 0; i < kDestroyed; ++i )
            Core::DestroyEntityTree( reg, index, all[i] );
        const auto stop = std::chrono::steady_clock::now();

        EXPECT_EQ( index.Size(), worldSize - kDestroyed );
        return std::chrono::duration<double, std::nano>( stop - start ).count() / kDestroyed;
    }
} // namespace

TEST( EntityDestroy, CostPerDestroyDoesNotGrowWithTheWorld )
{
    // Best of three per size: the question is the algorithm's shape, and the minimum is the run least
    // disturbed by whatever else the machine was doing.
    double small = 1e300;
    double large = 1e300;
    for ( int run = 0; run < 3; ++run )
    {
        small = std::min( small, NanosecondsPerDestroy( 1500 ) );
        large = std::min( large, NanosecondsPerDestroy( 50179 ) );
    }
    std::printf( "[   COST   ] destroy 1000: world 1500 -> %.0f ns each (%.3f ms total); world 50179 -> %.0f ns "
                 "each (%.3f ms total); ratio %.2f\n",
                 small, small * 1000.0 / 1e6, large, large * 1000.0 / 1e6, large / small );

    // Measured on the WP6 machine (M-series, best of three): the pre-WP6 algorithm — linear find, erase from
    // the middle, shift every stored index — gave ratios of 59-70 in Release and ~150 in Debug; the swap-remove
    // gives 6.5-7 in Release and 1.2-1.6 in Debug. What is left in Release is not the algorithm but the cache:
    // at 184 ns a destroy in the small world runs out of L2, and the 50 179-entity world's hash tables and
    // component pools do not fit, so every lookup is a miss. The bound sits between the two populations with
    // a factor of two either side.
    EXPECT_LT( large / small, 15.0 ) << "destroy cost grows with the size of the world again";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
