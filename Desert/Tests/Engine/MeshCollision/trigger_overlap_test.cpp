// GP4: trigger volumes and overlap events, UE's bGenerateOverlapEvents / OnComponentBeginOverlap /
// OnComponentEndOverlap in Jolt terms (a sensor body, its contacts queued on the physics threads and turned
// into Begin/End on the stepping thread). Device-free: a PhysicsWorld and nothing else.
//
//   1. A body falling through a trigger begins exactly once and ends exactly once, in that order.
//   2. A body resting inside for many steps (asleep, even) repeats nothing.
//   3. The filter is honoured: a trigger that ignores dynamic bodies reports none, and still reports a
//      kinematic one.
//   4. Events reach the subscriber on the thread that called Step, never on a Jolt worker.
//   5. A body removed while inside ends its overlap.

#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace Desert;

namespace
{
    constexpr float kGravity = 981.0f;
    constexpr float kStep    = 1.0f / 60.0f;

    struct Recorded
    {
        Physics::OverlapEvent Event;
        std::thread::id       Thread;
    };

    Physics::BodyHandle MakeTrigger( Physics::PhysicsWorld& world, const Physics::OverlapFilter& filter )
    {
        Physics::BodyDesc desc;
        desc.Shape       = Physics::ShapeType::Box;
        desc.HalfExtents = { 100.0f, 100.0f, 100.0f };
        desc.Type        = Physics::BodyType::Static;
        desc.IsTrigger   = true;
        desc.Overlaps    = filter;
        auto created     = world.CreateBody( desc );
        EXPECT_TRUE( created.IsSuccess() );
        return created.IsSuccess() ? created.GetValue() : Physics::kInvalidBody;
    }

    Physics::BodyHandle MakeBall( Physics::PhysicsWorld& world, const glm::vec3& position, Physics::BodyType type )
    {
        Physics::BodyDesc desc;
        desc.Shape    = Physics::ShapeType::Sphere;
        desc.Radius   = 10.0f;
        desc.Type     = type;
        desc.Position = position;
        auto created  = world.CreateBody( desc );
        EXPECT_TRUE( created.IsSuccess() );
        return created.IsSuccess() ? created.GetValue() : Physics::kInvalidBody;
    }

    std::vector<Recorded>& Record( Physics::PhysicsWorld& world, std::vector<Recorded>& into )
    {
        world.SubscribeOverlaps( [&into]( const Physics::OverlapEvent& event )
                                 { into.push_back( { event, std::this_thread::get_id() } ); } );
        return into;
    }

    void StepFor( Physics::PhysicsWorld& world, int steps )
    {
        for ( int i = 0; i < steps; ++i )
            world.Step( kStep );
    }
} // namespace

TEST( TriggerOverlap, BodyPassingThroughBeginsOnceAndEndsOnce )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( kGravity ) );
    std::vector<Recorded> events;
    Record( world, events );

    const auto trigger = MakeTrigger( world, {} );
    const auto ball    = MakeBall( world, { 0.0f, 300.0f, 0.0f }, Physics::BodyType::Dynamic );
    StepFor( world, 120 ); // 2 s: 300 cm above the box, it falls through all of it and out below

    ASSERT_LT( world.GetPosition( ball ).y, -110.0f ) << "the ball must have left the trigger: it pushes nothing";
    ASSERT_EQ( events.size(), 2u );
    EXPECT_EQ( events[0].Event.Phase, Physics::OverlapPhase::Begin );
    EXPECT_EQ( events[1].Event.Phase, Physics::OverlapPhase::End );
    for ( const Recorded& recorded : events )
    {
        EXPECT_EQ( recorded.Event.Trigger, trigger );
        EXPECT_EQ( recorded.Event.Other, ball );
    }
}

TEST( TriggerOverlap, BodyRestingInsideDoesNotRepeat )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 0.0f ) ); // nothing falls: the ball stays inside and goes to sleep
    std::vector<Recorded> events;
    Record( world, events );

    MakeTrigger( world, {} );
    MakeBall( world, { 0.0f, 0.0f, 0.0f }, Physics::BodyType::Dynamic );
    StepFor( world, 600 ); // 10 s, long past Jolt's sleep time

    ASSERT_EQ( events.size(), 1u );
    EXPECT_EQ( events[0].Event.Phase, Physics::OverlapPhase::Begin );
}

TEST( TriggerOverlap, FilterExcludesAKindOfBody )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( kGravity ) );
    std::vector<Recorded> events;
    Record( world, events );

    Physics::OverlapFilter noDynamic;
    noDynamic.Dynamic   = false;
    noDynamic.Kinematic = true;
    const auto trigger  = MakeTrigger( world, noDynamic );
    MakeBall( world, { 0.0f, 300.0f, 0.0f }, Physics::BodyType::Dynamic );
    StepFor( world, 120 );
    EXPECT_TRUE( events.empty() ) << "a dynamic body passed through a trigger that ignores dynamic bodies";

    const auto platform = MakeBall( world, { 0.0f, 0.0f, 0.0f }, Physics::BodyType::Kinematic );
    StepFor( world, 10 );
    ASSERT_EQ( events.size(), 1u ) << "the same trigger must still see the kind it admits";
    EXPECT_EQ( events[0].Event.Phase, Physics::OverlapPhase::Begin );
    EXPECT_EQ( events[0].Event.Trigger, trigger );
    EXPECT_EQ( events[0].Event.Other, platform );
}

TEST( TriggerOverlap, EventsArriveOnTheSteppingThread )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( kGravity ) );
    std::vector<Recorded> events;
    Record( world, events );

    MakeTrigger( world, {} );
    MakeBall( world, { 0.0f, 300.0f, 0.0f }, Physics::BodyType::Dynamic );
    EXPECT_TRUE( events.empty() ) << "creating bodies must not deliver anything";

    const std::thread::id stepping = std::this_thread::get_id();
    StepFor( world, 120 );
    ASSERT_EQ( events.size(), 2u );
    for ( const Recorded& recorded : events )
        EXPECT_EQ( recorded.Thread, stepping ) << "an overlap was delivered on a physics worker thread";
}

TEST( TriggerOverlap, RemovedBodyInsideEnds )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 0.0f ) );
    std::vector<Recorded> events;
    Record( world, events );

    const auto trigger = MakeTrigger( world, {} );
    const auto ball    = MakeBall( world, { 0.0f, 0.0f, 0.0f }, Physics::BodyType::Dynamic );
    StepFor( world, 5 );
    ASSERT_EQ( events.size(), 1u );

    world.RemoveBody( ball );
    EXPECT_EQ( events.size(), 1u ) << "the End is delivered after a step, not from inside RemoveBody";
    StepFor( world, 5 );
    ASSERT_EQ( events.size(), 2u );
    EXPECT_EQ( events[1].Event.Phase, Physics::OverlapPhase::End );
    EXPECT_EQ( events[1].Event.Trigger, trigger );
    EXPECT_EQ( events[1].Event.Other, ball );
}
