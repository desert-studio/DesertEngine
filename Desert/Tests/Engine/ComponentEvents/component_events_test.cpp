// SCR-API-3: the facts of physics and destruction reach a component's EVENT subscribers (UE OnComponentHit /
// OnComponentBeginOverlap / OnComponentEndOverlap / OnChaosBreakEvent), language-agnostic, through Values.
//
//   1. Contact: a C++ listener on a ball's RigidBodyData::OnHit is called when the ball lands on a floor, with the
//      floor as `other`, the contact point, the normal towards the floor and a positive impulse.
//   2. Zero at zero: with nobody subscribed nothing is called; a listener on another entity or event is not
//      called; a batch is delivered once (a paused frame does not repeat it).
//   3. Unbinding and the dead: an unsubscribed listener is not called; an entity destroyed takes its listeners.
//   4. Break: DestructibleData::OnBreak carries the node, the position and the velocity.
//   5. Refusals: an unknown type or event, an empty listener, a payload of another signature.
//
// Mutations this suite must turn red:
//   * ComponentEventSystem.cpp DeliverContacts — pass fact.Point as the normal            (ALandingBallHearsItsHit)
//   * ComponentEventSystem.cpp DeliverComponentEvents — drop `contacts->Publication != cursor.Physics`
//     (ABatchIsDeliveredOnce)
//   * ComponentEvents.cpp Broadcast — drop the kind check                                  (AWrongPayloadIsRefused)
//   * ComponentEvents.cpp Prune — return false from the predicate                          (ADestroyedEntityTakes...)

#include <Engine/ECS/ComponentEvents.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/ECS/DestructionEvents.hpp>
#include <Engine/ECS/PhysicsEvents.hpp>
#include <Engine/ECS/System/ComponentEventSystem.hpp>
#include <Engine/Physics/CollisionProfiles.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include "../PhysicsFixture.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace Desert;
using Reflection::Value;

namespace
{
    struct Heard
    {
        std::vector<std::vector<Value>> Calls;
        ECS::ComponentEvents::Listener  Listener()
        {
            return [this]( std::span<const Value> payload ) { Calls.emplace_back( payload.begin(), payload.end() ); };
        }
    };

    glm::vec3 Vec3( const Value& v )
    {
        const auto* f = v.Get<Value::Float3>();
        return f != nullptr ? glm::vec3( ( *f )[0], ( *f )[1], ( *f )[2] ) : glm::vec3( NAN );
    }

    // A floor and a ball as entities, the ball's profile generating hit events; every step's contacts are
    // published into the registry's PhysicsEventQueue and delivered, as PhysicsECSSystem + ComponentEventSystem do.
    struct ContactScene
    {
        entt::registry                                        Registry;
        Physics::PhysicsWorld                                 World;
        std::unordered_map<Physics::BodyHandle, entt::entity> Bodies;
        ECS::ComponentEventCursor                             Cursor;
        entt::entity                                          Floor = Registry.create();
        entt::entity                                          Ball  = Registry.create();

        ContactScene()
        {
            Physics::CollisionProfilesConfig config = TestSupport::PhysicsTestProfilesConfig();
            for ( Physics::CollisionProfileConfig& row : config.Profiles )
                if ( row.Name == "PhysicsActor" )
                    row.GeneratesHitEvents = true;
            auto profiles = Physics::CollisionProfiles::Build( config );
            EXPECT_TRUE( profiles.IsSuccess() ) << profiles.GetError();
            World.Init( 981.0f, profiles.ExtractValue() );

            Physics::BodyDesc floor;
            floor.Type        = Physics::BodyType::Static;
            floor.HalfExtents = { 500.0f, 10.0f, 500.0f };
            floor.Profile     = TestSupport::ProfileId( World, "BlockAll" );
            Physics::BodyDesc ball;
            ball.Shape       = Physics::ShapeType::Sphere;
            ball.Radius      = 25.0f;
            ball.Mass        = 10.0f;
            ball.Restitution = 0.0f;
            ball.Position    = { 0.0f, 100.0f, 0.0f };
            ball.Profile     = TestSupport::ProfileId( World, "PhysicsActor" );
            auto floorBody   = World.CreateBody( floor );
            auto ballBody    = World.CreateBody( ball );
            EXPECT_TRUE( floorBody.IsSuccess() && ballBody.IsSuccess() );
            Bodies[floorBody.GetValue()] = Floor;
            Bodies[ballBody.GetValue()]  = Ball;
        }
        ~ContactScene()
        {
            World.Shutdown();
        }
        ContactScene( const ContactScene& )            = delete;
        ContactScene& operator=( const ContactScene& ) = delete;

        ECS::ComponentEvents& Events()
        {
            return Registry.ctx_or_set<ECS::ComponentEvents>();
        }
        std::size_t Run( int steps )
        {
            std::size_t calls = 0;
            for ( int i = 0; i < steps; ++i )
            {
                World.Step( Physics::kFixedStepSeconds );
                auto& queue = Registry.ctx_or_set<ECS::PhysicsEventQueue>();
                queue.Events.clear();
                ++queue.Publication;
                ECS::NameContactEvents( World.GetContactEvents(), Bodies, queue.Events );
                calls += ECS::DeliverComponentEvents( Registry, Cursor );
            }
            return calls;
        }
    };
} // namespace

TEST( ComponentEvents, ALandingBallHearsItsHit )
{
    ContactScene scene;
    Heard        heard;
    auto bound = scene.Events().Subscribe( scene.Ball, "RigidBodyData", "OnHit", heard.Listener() );
    ASSERT_TRUE( bound.IsSuccess() ) << bound.GetError();

    EXPECT_GE( scene.Run( 120 ), 1u );
    ASSERT_GE( heard.Calls.size(), 1u ) << "the landing was not delivered to the ball's OnHit";
    const std::vector<Value>& hit = heard.Calls.front();
    ASSERT_EQ( hit.size(), 4u ) << "OnHit( other, point, normal, impulse )";
    ASSERT_NE( hit[0].Get<std::uint64_t>(), nullptr );
    EXPECT_EQ( *hit[0].Get<std::uint64_t>(),
               static_cast<std::uint64_t>( static_cast<std::underlying_type_t<entt::entity>>( scene.Floor ) ) );
    EXPECT_NEAR( Vec3( hit[1] ).y, 10.0f, 2.0f ) << "the point is on the floor's top face";
    EXPECT_LT( Vec3( hit[2] ).y, -0.9f ) << "the normal points from the ball towards the floor";
    ASSERT_NE( hit[3].Get<float>(), nullptr );
    EXPECT_GT( *hit[3].Get<float>(), 0.0f );
}

TEST( ComponentEvents, NobodyListeningIsNothingCalled )
{
    ContactScene scene;
    EXPECT_EQ( scene.Run( 120 ), 0u ) << "no table: no call";

    ContactScene other;
    Heard        heard;
    ASSERT_TRUE( other.Events().Subscribe( other.Floor, "RigidBodyData", "OnHit", heard.Listener() ).IsSuccess() );
    ASSERT_TRUE( other.Events().Subscribe( other.Ball, "RigidBodyData", "OnBeginOverlap", heard.Listener() ).IsSuccess() );
    EXPECT_EQ( other.Run( 120 ), 0u );
    EXPECT_TRUE( heard.Calls.empty() ) << "the floor's profile reports no hit; the ball overlaps nothing";
}

TEST( ComponentEvents, ABatchIsDeliveredOnce )
{
    ContactScene scene;
    Heard        heard;
    ASSERT_TRUE( scene.Events().Subscribe( scene.Ball, "RigidBodyData", "OnHit", heard.Listener() ).IsSuccess() );
    scene.Run( 120 );
    const std::size_t landed = heard.Calls.size();
    ASSERT_GE( landed, 1u );
    // A paused frame: the queue is not republished, the system runs again.
    EXPECT_EQ( ECS::DeliverComponentEvents( scene.Registry, scene.Cursor ), 0u );
    EXPECT_EQ( heard.Calls.size(), landed );
}

TEST( ComponentEvents, AnUnboundListenerIsNotCalled )
{
    ContactScene scene;
    Heard        heard;
    auto         bound = scene.Events().Subscribe( scene.Ball, "RigidBodyData", "OnHit", heard.Listener() );
    ASSERT_TRUE( bound.IsSuccess() );
    EXPECT_TRUE( scene.Events().Unsubscribe( bound.GetValue() ) );
    EXPECT_FALSE( scene.Events().Unsubscribe( bound.GetValue() ) ) << "twice is not bound";
    EXPECT_TRUE( scene.Events().Empty() );
    EXPECT_EQ( scene.Run( 120 ), 0u );
    EXPECT_TRUE( heard.Calls.empty() );
}

TEST( ComponentEvents, ADestroyedEntityTakesItsListeners )
{
    ContactScene scene;
    Heard        heard;
    const entt::entity gone = scene.Registry.create();
    ASSERT_TRUE( scene.Events().Subscribe( gone, "RigidBodyData", "OnHit", heard.Listener() ).IsSuccess() );
    ASSERT_TRUE( scene.Events().Subscribe( scene.Ball, "RigidBodyData", "OnHit", heard.Listener() ).IsSuccess() );
    scene.Registry.destroy( gone );
    scene.Run( 120 );
    const auto* hit = Reflection::ReflectionRegistry::Get().Find( "RigidBodyData" )->FindEvent( "OnHit" );
    ASSERT_NE( hit, nullptr );
    EXPECT_FALSE( scene.Events().IsBound( gone, *hit ) ) << "the dead entity's listener was kept";
    EXPECT_TRUE( scene.Events().IsBound( scene.Ball, *hit ) );
}

TEST( ComponentEvents, ABreakCarriesItsPiece )
{
    entt::registry            registry;
    ECS::ComponentEventCursor cursor;
    const entt::entity        wall = registry.create();
    Heard                     heard;
    ASSERT_TRUE( registry.ctx_or_set<ECS::ComponentEvents>()
                      .Subscribe( wall, "DestructibleData", "OnBreak", heard.Listener() )
                      .IsSuccess() );
    auto& queue = registry.ctx_or_set<ECS::DestructionEventQueue>();
    queue.Breaks.push_back( { wall, 5, { 1.0f, 2.0f, 3.0f }, { 0.0f, -100.0f, 0.0f } } );
    ++queue.Publication;
    EXPECT_EQ( ECS::DeliverComponentEvents( registry, cursor ), 1u );
    ASSERT_EQ( heard.Calls.size(), 1u );
    ASSERT_EQ( heard.Calls[0].size(), 3u );
    ASSERT_NE( heard.Calls[0][0].Get<std::int64_t>(), nullptr );
    EXPECT_EQ( *heard.Calls[0][0].Get<std::int64_t>(), 5 );
    EXPECT_EQ( Vec3( heard.Calls[0][1] ), glm::vec3( 1.0f, 2.0f, 3.0f ) );
    EXPECT_EQ( Vec3( heard.Calls[0][2] ), glm::vec3( 0.0f, -100.0f, 0.0f ) );
}

TEST( ComponentEvents, AWrongPayloadIsRefused )
{
    ECS::ComponentEvents events;
    const entt::entity   self = static_cast<entt::entity>( 3u );
    EXPECT_FALSE( events.Subscribe( self, "NoSuchType", "OnHit", []( std::span<const Value> ) {} ).IsSuccess() );
    EXPECT_FALSE( events.Subscribe( self, "RigidBodyData", "OnNoSuchEvent", []( std::span<const Value> ) {} ).IsSuccess() );
    EXPECT_FALSE( events.Subscribe( self, "RigidBodyData", "OnHit", {} ).IsSuccess() );

    Heard heard;
    ASSERT_TRUE( events.Subscribe( self, "RigidBodyData", "OnEndOverlap", heard.Listener() ).IsSuccess() );
    const auto* end = Reflection::ReflectionRegistry::Get().Find( "RigidBodyData" )->FindEvent( "OnEndOverlap" );
    ASSERT_NE( end, nullptr );
    const std::array<Value, 1> wrongKind  = { Value::Float( 1.0f ) };
    const std::array<Value, 2> wrongCount = { Value::UInt( 1u ), Value::UInt( 2u ) };
    EXPECT_FALSE( events.Broadcast( self, *end, wrongKind ).IsSuccess() );
    EXPECT_FALSE( events.Broadcast( self, *end, wrongCount ).IsSuccess() );
    EXPECT_TRUE( heard.Calls.empty() ) << "a refused payload calls nobody";
    const auto right = Reflection::EventPayload<ECS::RigidBodyData::OnEndOverlap>( static_cast<entt::entity>( 4u ) );
    auto       called = events.Broadcast( self, *end, right );
    ASSERT_TRUE( called.IsSuccess() ) << called.GetError();
    EXPECT_EQ( called.GetValue(), 1u );
}
