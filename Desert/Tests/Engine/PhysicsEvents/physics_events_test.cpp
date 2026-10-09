// PHYS-A2: the physics event queue (UE OnComponentHit / Begin/EndOverlap, ChaosEventRelay's collect-then-drain).
//
//   1. Zero at zero: bodies that never touch report nothing.
//   2. One Begin per End: a ball falling through a Trigger reports one BeginOverlap, then one EndOverlap; a body
//      removed while overlapping still ends its overlap.
//   3. A Hit is a contact BEGUN: a ball landing on a floor reports Hit(s) on landing and none while resting; a
//      profile that does not generate hit events gets none.
//   4. Named by entity: each side that asked gets its own event, Normal pointing from Self to Other.
//
// Mutations this suite must turn red:
//   * PhysicsWorld.cpp ImpulseListener::OnContactRemoved — drop the Events.push_back   (OneBeginThenOneEnd...)
//   * PhysicsWorld.cpp RecordAdded — `OverlapCounts[bodies]++ == 0u` → `true`           (OneBeginThenOneEnd...)
//   * PhysicsWorld.cpp RecordAdded — `event.Notify1 = Profiles->GeneratesHitEvents(..)` → true
//   (NoHitWithoutTheFlag)
//   * PhysicsEvents.hpp NameContactEvents — drop the `-` on the second side's Normal     (EachSideIsNamed...)

#include <Engine/ECS/PhysicsEvents.hpp>
#include <Engine/Physics/CollisionProfiles.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include "../PhysicsFixture.hpp"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Desert;
using Physics::ContactEventKind;

namespace
{
    constexpr float kStep = Physics::kFixedStepSeconds;

    Physics::CollisionProfiles EventProfiles()
    {
        Physics::CollisionProfilesConfig config = TestSupport::PhysicsTestProfilesConfig();
        for ( Physics::CollisionProfileConfig& row : config.Profiles )
        {
            if ( row.Name == "PhysicsActor" )
                row.GeneratesHitEvents = row.GeneratesOverlapEvents = true;
            if ( row.Name == "Trigger" || row.Name == "BlockAllDynamic" )
                row.GeneratesOverlapEvents = true;
        }
        auto built = Physics::CollisionProfiles::Build( config );
        EXPECT_TRUE( built.IsSuccess() ) << built.GetError();
        return built.ExtractValue();
    }

    struct EventWorld
    {
        Physics::PhysicsWorld              World;
        std::vector<Physics::ContactEvent> Events;

        EventWorld()
        {
            World.Init( 981.0f, EventProfiles() );
        }
        ~EventWorld()
        {
            World.Shutdown();
        }
        EventWorld( const EventWorld& )            = delete;
        EventWorld& operator=( const EventWorld& ) = delete;

        Physics::BodyHandle Box( std::string_view profile, glm::vec3 position, glm::vec3 half )
        {
            Physics::BodyDesc desc;
            desc.Type        = Physics::BodyType::Static;
            desc.HalfExtents = half;
            desc.Position    = position;
            desc.Profile     = TestSupport::ProfileId( World, profile );
            auto body        = World.CreateBody( desc );
            EXPECT_TRUE( body.IsSuccess() ) << body.GetError();
            return body.IsSuccess() ? body.GetValue() : Physics::kInvalidBody;
        }
        Physics::BodyHandle Ball( std::string_view profile, glm::vec3 position )
        {
            Physics::BodyDesc desc;
            desc.Shape       = Physics::ShapeType::Sphere;
            desc.Radius      = 25.0f;
            desc.Mass        = 10.0f;
            desc.Restitution = 0.0f;
            desc.Position    = position;
            desc.Profile     = TestSupport::ProfileId( World, profile );
            auto body        = World.CreateBody( desc );
            EXPECT_TRUE( body.IsSuccess() ) << body.GetError();
            return body.IsSuccess() ? body.GetValue() : Physics::kInvalidBody;
        }
        void Run( int steps )
        {
            for ( int i = 0; i < steps; ++i )
            {
                World.Step( kStep );
                const auto events = World.GetContactEvents();
                Events.insert( Events.end(), events.begin(), events.end() );
            }
        }
        [[nodiscard]] int Count( ContactEventKind kind ) const
        {
            int n = 0;
            for ( const Physics::ContactEvent& e : Events )
                n += e.Kind == kind ? 1 : 0;
            return n;
        }
    };
} // namespace

TEST( PhysicsEvents, ZeroEventsWhenNothingTouches )
{
    EventWorld world;
    world.Box( "BlockAll", { 0.0f, -1000.0f, 0.0f }, { 50.0f, 10.0f, 50.0f } );
    world.Ball( "PhysicsActor", { 500.0f, 0.0f, 0.0f } );
    world.Box( "Trigger", { -500.0f, 0.0f, 0.0f }, { 50.0f, 50.0f, 50.0f } );
    world.Run( 30 ); // the ball falls about 12 cm in 30 steps: far from everything
    EXPECT_TRUE( world.Events.empty() ) << world.Events.size() << " events with nothing touching";
}

TEST( PhysicsEvents, OneBeginThenOneEndThroughATrigger )
{
    EventWorld world;
    world.Box( "Trigger", { 0.0f, 0.0f, 0.0f }, { 200.0f, 20.0f, 200.0f } );
    const auto ball = world.Ball( "PhysicsActor", { 0.0f, 100.0f, 0.0f } );
    world.Run( 120 );
    ASSERT_LT( world.World.GetPosition( ball ).y, -100.0f ) << "the ball should have fallen through the trigger";
    EXPECT_EQ( world.Count( ContactEventKind::BeginOverlap ), 1 );
    EXPECT_EQ( world.Count( ContactEventKind::EndOverlap ), 1 );
    EXPECT_EQ( world.Count( ContactEventKind::Hit ), 0 ) << "an overlap is not a hit";
    ASSERT_EQ( world.Events.size(), 2u );
    EXPECT_EQ( world.Events[0].Kind, ContactEventKind::BeginOverlap );
}

TEST( PhysicsEvents, RemovingAnOverlappingBodyEndsItsOverlap )
{
    EventWorld world;
    world.Box( "Trigger", { 0.0f, 0.0f, 0.0f }, { 500.0f, 500.0f, 500.0f } );
    const auto ball = world.Ball( "PhysicsActor", { 0.0f, 0.0f, 0.0f } );
    world.Run( 2 );
    ASSERT_EQ( world.Count( ContactEventKind::BeginOverlap ), 1 );
    world.World.RemoveBody( ball );
    world.Run( 2 );
    EXPECT_EQ( world.Count( ContactEventKind::EndOverlap ), 1 ) << "a removed body must end its overlap";
}

TEST( PhysicsEvents, AHitIsAContactBegunNotAContactKept )
{
    EventWorld world;
    const auto floor = world.Box( "BlockAll", { 0.0f, 0.0f, 0.0f }, { 500.0f, 10.0f, 500.0f } );
    const auto ball  = world.Ball( "PhysicsActor", { 0.0f, 100.0f, 0.0f } );
    world.Run( 120 );
    ASSERT_GE( world.Count( ContactEventKind::Hit ), 1 ) << "landing reported no Hit";
    const Physics::ContactEvent& hit = world.Events.front();
    EXPECT_TRUE( ( hit.Body1 == ball && hit.Notify1 && !hit.Notify2 ) ||
                 ( hit.Body2 == ball && hit.Notify2 && !hit.Notify1 ) )
         << "only the ball's profile generates hit events";
    EXPECT_TRUE( hit.Body1 == floor || hit.Body2 == floor );
    EXPECT_GT( hit.Impulse, 0.0f );
    world.Events.clear();
    world.Run( 60 );
    EXPECT_EQ( world.Count( ContactEventKind::Hit ), 0 ) << "a resting contact is not a new hit";
}

TEST( PhysicsEvents, NoHitWithoutTheFlag )
{
    EventWorld world;
    world.Box( "BlockAll", { 0.0f, 0.0f, 0.0f }, { 500.0f, 10.0f, 500.0f } );
    world.Ball( "BlockAllDynamic", { 0.0f, 100.0f, 0.0f } );
    world.Run( 120 );
    EXPECT_EQ( world.Count( ContactEventKind::Hit ), 0 );
}

TEST( PhysicsEvents, EachSideIsNamedWithTheNormalTowardsTheOther )
{
    const auto                                                  e1     = static_cast<entt::entity>( 7u );
    const auto                                                  e2     = static_cast<entt::entity>( 9u );
    const std::unordered_map<Physics::BodyHandle, entt::entity> bodies = { { 1u, e1 }, { 2u, e2 } };
    Physics::ContactEvent                                       hit;
    hit.Body1                       = 1u;
    hit.Body2                       = 2u;
    hit.Notify1                     = true;
    hit.Notify2                     = true;
    hit.Normal                      = { 0.0f, 1.0f, 0.0f };
    Physics::ContactEvent landscape = hit;
    landscape.Body2                 = 3u; // a body no entity owns: named as null, given no event of its own
    const std::vector<Physics::ContactEvent> events = { hit, landscape };

    std::vector<ECS::PhysicsEvent> named;
    ECS::NameContactEvents( events, bodies, named );
    ASSERT_EQ( named.size(), 3u );
    EXPECT_EQ( named[0].Self, e1 );
    EXPECT_EQ( named[0].Other, e2 );
    EXPECT_EQ( named[0].Normal, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    EXPECT_EQ( named[1].Self, e2 );
    EXPECT_EQ( named[1].Other, e1 );
    EXPECT_EQ( named[1].Normal, glm::vec3( 0.0f, -1.0f, 0.0f ) );
    EXPECT_EQ( named[2].Self, e1 );
    EXPECT_TRUE( named[2].Other == entt::null );
}
