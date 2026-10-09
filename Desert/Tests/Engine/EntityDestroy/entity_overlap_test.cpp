// GP4: trigger overlaps at the ENTITY level, and kinematic bodies that follow their entity's transform.
// Device-free: a bare registry, a real PhysicsWorld, EntityOverlapRouter + DriveKinematicBodies (the units
// PhysicsECSSystem runs) and PhysicsBodyLifetime (what removes a destroyed entity's body).
//
//   1. A kinematic box moved ONLY by its TransformComponent enters a trigger: the body follows the transform,
//      and both entities are told Begin, each naming the other (UE fires on both components).
//   2. An entity destroyed while inside a trigger gives the trigger's entity an End naming it.
//   3. A C++ subscriber hears each overlap once, as (trigger entity, other entity).

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/EntityOverlaps.hpp>
#include <Engine/ECS/System/PhysicsBodyLifetime.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace Desert;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    entt::entity MakeTrigger( entt::registry& reg, Physics::PhysicsWorld& world, ECS::EntityOverlapRouter& router )
    {
        Physics::BodyDesc desc;
        desc.Shape       = Physics::ShapeType::Box;
        desc.HalfExtents = { 100.0f, 100.0f, 100.0f };
        desc.Type        = Physics::BodyType::Static;
        desc.IsTrigger   = true;
        auto created     = world.CreateBody( desc );
        EXPECT_TRUE( created.IsSuccess() );
        const entt::entity e = reg.create();
        reg.emplace<ECS::TransformComponent>( e );
        auto& rb       = reg.emplace<ECS::RigidBodyComponent>( e );
        rb.Data.Type   = Physics::BodyType::Static;
        rb.RuntimeBody = created.IsSuccess() ? created.GetValue() : Physics::kInvalidBody;
        router.Track( rb.RuntimeBody, e );
        return e;
    }

    entt::entity MakeKinematicBox( entt::registry& reg, Physics::PhysicsWorld& world,
                                   ECS::EntityOverlapRouter& router, const glm::vec3& at )
    {
        Physics::BodyDesc desc;
        desc.Shape       = Physics::ShapeType::Box;
        desc.HalfExtents = { 20.0f, 20.0f, 20.0f };
        desc.Type        = Physics::BodyType::Kinematic;
        desc.Position    = at;
        auto created     = world.CreateBody( desc );
        EXPECT_TRUE( created.IsSuccess() );
        const entt::entity e                                  = reg.create();
        reg.emplace<ECS::TransformComponent>( e ).Translation = at;
        auto& rb                                              = reg.emplace<ECS::RigidBodyComponent>( e );
        rb.Data.Type                                          = Physics::BodyType::Kinematic;
        rb.RuntimeBody = created.IsSuccess() ? created.GetValue() : Physics::kInvalidBody;
        router.Track( rb.RuntimeBody, e );
        return e;
    }

    // One PhysicsECSSystem frame, without the renderer: drive kinematics, step, deliver.
    void Frame( entt::registry& reg, Physics::PhysicsWorld& world, ECS::EntityOverlapRouter& router )
    {
        ECS::DriveKinematicBodies( reg, world );
        router.Deliver( reg, world.Step( kStep ) );
    }

    std::vector<ECS::OverlapEventsComponent::Event> Pending( const entt::registry& reg, entt::entity e )
    {
        return reg.has<ECS::OverlapEventsComponent>( e ) ? reg.get<ECS::OverlapEventsComponent>( e ).Pending
                                                         : std::vector<ECS::OverlapEventsComponent::Event>{};
    }
} // namespace

// Red if a kinematic body ignores its transform (it never reaches the trigger: no Begin, body x stays -500),
// or if only one of the two entities is told.
TEST( EntityOverlap, KinematicBoxMovedByItsTransformEntersTriggerAndBothEntitiesBegin )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    entt::registry           reg;
    ECS::EntityOverlapRouter router( world );
    const entt::entity       trigger = MakeTrigger( reg, world, router );
    const entt::entity       box     = MakeKinematicBox( reg, world, router, { -500.0f, 0.0f, 0.0f } );

    // 60 frames carry the transform from x = -500 to x = 0 (the trigger's centre).
    for ( int frame = 1; frame <= 60; ++frame )
    {
        reg.get<ECS::TransformComponent>( box ).Translation.x =
             -500.0f + 500.0f * static_cast<float>( frame ) / 60.0f;
        Frame( reg, world, router );
    }

    EXPECT_NEAR( world.GetPosition( reg.get<ECS::RigidBodyComponent>( box ).RuntimeBody ).x, 0.0f, 1.0f )
         << "the kinematic body did not follow its entity's transform";
    const auto onTrigger = Pending( reg, trigger );
    const auto onBox     = Pending( reg, box );
    ASSERT_EQ( onTrigger.size(), 1u );
    EXPECT_TRUE( onTrigger[0].Begin );
    EXPECT_EQ( onTrigger[0].Other, box );
    ASSERT_EQ( onBox.size(), 1u ) << "the body that entered the trigger was not told (UE fires on both)";
    EXPECT_TRUE( onBox[0].Begin );
    EXPECT_EQ( onBox[0].Other, trigger );
}

// Red if destroying an entity inside a trigger leaves the trigger's entity without an End naming it.
TEST( EntityOverlap, DestroyedEntityInsideTriggerGivesTheTriggerAnEnd )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    entt::registry           reg;
    ECS::PhysicsBodyLifetime lifetime( world );
    lifetime.Attach( reg );
    ECS::EntityOverlapRouter router( world );
    const entt::entity       trigger = MakeTrigger( reg, world, router );
    const entt::entity       box     = MakeKinematicBox( reg, world, router, { 0.0f, 0.0f, 0.0f } );

    for ( int frame = 0; frame < 5; ++frame )
        Frame( reg, world, router );
    ASSERT_EQ( Pending( reg, trigger ).size(), 1u );
    router.BeginFrame( reg );

    reg.destroy( box );
    for ( int frame = 0; frame < 5; ++frame )
        Frame( reg, world, router );

    const auto onTrigger = Pending( reg, trigger );
    ASSERT_EQ( onTrigger.size(), 1u ) << "no End (or more than one) after the entity inside was destroyed";
    EXPECT_FALSE( onTrigger[0].Begin );
    EXPECT_EQ( onTrigger[0].Other, box );
    lifetime.Detach();
}

// Red if a subscriber hears an overlap twice (once per entity) or with the entities swapped.
TEST( EntityOverlap, SubscriberHearsEachOverlapOnceAsTriggerAndOther )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    entt::registry                       reg;
    ECS::EntityOverlapRouter             router( world );
    std::vector<ECS::EntityOverlapEvent> heard;
    router.SubscribeOverlaps( [&heard]( const ECS::EntityOverlapEvent& event ) { heard.push_back( event ); } );
    const entt::entity trigger = MakeTrigger( reg, world, router );
    const entt::entity box     = MakeKinematicBox( reg, world, router, { 0.0f, 0.0f, 0.0f } );

    for ( int frame = 0; frame < 5; ++frame )
        Frame( reg, world, router );

    ASSERT_EQ( heard.size(), 1u );
    EXPECT_EQ( heard[0].Phase, Physics::OverlapPhase::Begin );
    EXPECT_EQ( heard[0].Trigger, trigger );
    EXPECT_EQ( heard[0].Other, box );
}
