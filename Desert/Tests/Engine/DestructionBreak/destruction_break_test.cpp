// DST-03: a two-piece fracture on headless Jolt. Below its threshold a hit leaves it whole; above, it breaks;
// the pieces keep the velocity the whole had; an anchored piece stays where it was; a broken piece that sleeps
// is removed. A break is destruction's own event: named by the destructible entity in DestructionEventQueue
// (UE UGeometryCollectionComponent::OnChaosBreakEvent), never in the physics contact queue.
//
// Mutations this suite must turn red:
//   * DestructionEvents.hpp NameBreakEvents — drop the `!= Break` filter          (RemovedAndUnownedNameNothing)
//   * DestructibleLifetime.cpp PublishEvents — drop `queue.Breaks.clear()`        (BreakIsNamedByItsEntity)

#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/DestructionEvents.hpp>
#include <Engine/ECS/System/DestructibleLifetime.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include "../PhysicsFixture.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <vector>

using namespace Desert;
using namespace Desert::Destruction;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // An axis-aligned box [lo, hi] as a leaf: eight corners, six faces counter-clockwise from outside.
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

    // Two 100 cm cubes side by side along X, under a root: node 1 at x in [-100, 0], node 2 at [0, 100].
    std::shared_ptr<const FractureData> TwoCubes( float threshold )
    {
        auto         data = std::make_shared<FractureData>();
        FractureNode root;
        root.Volume       = 2.0e6;
        root.CenterOfMass = glm::dvec3( 0.0, 50.0, 50.0 );
        data->Nodes.push_back( root );
        data->Nodes.push_back( BoxLeaf( { -100.0f, 0.0f, 0.0f }, { 0.0f, 100.0f, 100.0f }, threshold ) );
        data->Nodes.push_back( BoxLeaf( { 0.0f, 0.0f, 0.0f }, { 100.0f, 100.0f, 100.0f }, threshold ) );
        return data;
    }

    struct Fixture
    {
        Physics::PhysicsWorld             physics;
        std::unique_ptr<DestructionWorld> destruction;

        explicit Fixture( float gravity )
        {
            EXPECT_TRUE( physics.Init( gravity, TestSupport::PhysicsTestProfiles() ) );
            destruction = std::make_unique<DestructionWorld>( physics );
        }

        // A 100 kg ball at x = -150 flying at +X into node 1's face at x = -100.
        void Throw( float speed )
        {
            Physics::BodyDesc ball;
            ball.Shape    = Physics::ShapeType::Sphere;
            ball.Radius   = 20.0f;
            ball.Mass     = 100.0f;
            ball.Position = { -150.0f, 50.0f, 50.0f };
            ball.Profile  = TestSupport::ProfileId( physics, "PhysicsActor" );
            auto created  = physics.CreateBody( ball );
            ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
            physics.SetLinearVelocity( created.GetValue(), { speed, 0.0f, 0.0f } );
        }

        void Run( int steps )
        {
            for ( int i = 0; i < steps; ++i )
                physics.Step( kStep );
        }
    };
} // namespace

TEST( DestructionBreak, BelowThresholdStaysWhole )
{
    Fixture f( 0.0f );
    auto    added = f.destruction->Add( TwoCubes( 1.0e12f ), DestructibleDesc{} );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    f.Throw( 1000.0f );
    f.Run( 30 );
    EXPECT_EQ( f.destruction->GetBodyCount( added.GetValue() ), 1u );
    EXPECT_TRUE( f.destruction->GetEvents().empty() );
    EXPECT_EQ( f.destruction->GetNodeBody( added.GetValue(), 1 ),
               f.destruction->GetNodeBody( added.GetValue(), 2 ) );
}

TEST( DestructionBreak, AboveThresholdBreaks )
{
    Fixture f( 0.0f );
    auto    added = f.destruction->Add( TwoCubes( 1.0e4f ), DestructibleDesc{} );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    f.Run( 5 );
    ASSERT_EQ( f.destruction->GetBodyCount( added.GetValue() ), 1u ) << "nothing touched it yet";
    f.Throw( 1000.0f );
    f.Run( 30 );
    EXPECT_EQ( f.destruction->GetBodyCount( added.GetValue() ), 2u );
    EXPECT_NE( f.destruction->GetNodeBody( added.GetValue(), 1 ),
               f.destruction->GetNodeBody( added.GetValue(), 2 ) );
    ASSERT_FALSE( f.destruction->GetEvents().empty() );
    EXPECT_EQ( f.destruction->GetEvents().front().Kind, DestructionEventKind::Break );
    EXPECT_EQ( f.destruction->GetEvents().front().Node, 1 ) << "the hit piece breaks off";
}

TEST( DestructionBreak, PiecesInheritVelocity )
{
    Fixture f( 0.0f );
    auto    added = f.destruction->Add( TwoCubes( 0.0f ), DestructibleDesc{} ); // zero strain: breaks at once
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    const glm::vec3 velocity( 300.0f, 0.0f, -200.0f );
    f.physics.SetLinearVelocity( f.destruction->GetNodeBody( added.GetValue(), 0 ), velocity );
    f.Run( 1 );
    ASSERT_EQ( f.destruction->GetBodyCount( added.GetValue() ), 2u );
    for ( const int32_t node : { 1, 2 } )
    {
        const glm::vec3 v = f.physics.GetLinearVelocity( f.destruction->GetNodeBody( added.GetValue(), node ) );
        EXPECT_NEAR( v.x, velocity.x, 1.0f ) << "node " << node;
        EXPECT_NEAR( v.z, velocity.z, 1.0f ) << "node " << node;
    }
}

TEST( DestructionBreak, AnchorHolds )
{
    Fixture          f( 981.0f );
    DestructibleDesc desc;
    desc.AnchoredNodes = { 1 };
    auto whole         = f.destruction->Add( TwoCubes( 1.0e12f ), desc );
    ASSERT_TRUE( whole.IsSuccess() ) << whole.GetError();
    const Physics::BodyHandle wholeBody = f.destruction->GetNodeBody( whole.GetValue(), 0 );
    f.Run( 30 );
    EXPECT_NEAR( f.physics.GetPosition( wholeBody ).y, 0.0f, 1.0e-3f ) << "an anchored whole does not fall";

    desc.Position = { 1000.0f, 0.0f, 0.0f };
    auto broken   = f.destruction->Add( TwoCubes( 0.0f ), desc );
    ASSERT_TRUE( broken.IsSuccess() ) << broken.GetError();
    f.Run( 30 );
    ASSERT_EQ( f.destruction->GetBodyCount( broken.GetValue() ), 2u );
    const Physics::BodyHandle anchored = f.destruction->GetNodeBody( broken.GetValue(), 1 );
    const Physics::BodyHandle loose    = f.destruction->GetNodeBody( broken.GetValue(), 2 );
    EXPECT_NEAR( f.physics.GetPosition( anchored ).y, 0.0f, 1.0e-3f ) << "the anchored piece stays";
    EXPECT_LT( f.physics.GetPosition( loose ).y, -50.0f ) << "the other piece falls";
}

TEST( DestructionBreak, RemoveOnSleep )
{
    Fixture          f( 0.0f );
    DestructibleDesc desc;
    desc.Settings.MaxSleepTime = { 0.1f, 0.1f };
    auto added                 = f.destruction->Add( TwoCubes( 0.0f ), desc );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    f.Run( 30 ); // still pieces: slower than the slow-moving speed, asleep from the first step
    EXPECT_EQ( f.destruction->GetBodyCount( added.GetValue() ), 0u );
    EXPECT_EQ( f.destruction->GetNodeBody( added.GetValue(), 1 ), Physics::kInvalidBody );
    int removed = 0;
    for ( const DestructionEvent& event : f.destruction->GetEvents() )
        removed += event.Kind == DestructionEventKind::Removed ? 1 : 0;
    EXPECT_EQ( removed, 2 );
}

TEST( DestructionBreak, BreakIsNamedByItsEntity )
{
    Fixture f( 0.0f );
    auto    added = f.destruction->Add( TwoCubes( 1.0e4f ), DestructibleDesc{} );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();

    entt::registry registry;
    const auto     entity       = registry.create();
    auto&          destructible = registry.emplace<ECS::DestructibleComponent>( entity );
    destructible.RuntimeObject  = added.GetValue();
    ECS::DestructibleLifetime owner( *f.destruction );

    f.Run( 5 );
    owner.PublishEvents( registry );
    EXPECT_TRUE( registry.ctx<ECS::DestructionEventQueue>().Breaks.empty() ) << "zero at zero: nothing broke";

    f.Throw( 1000.0f );
    f.Run( 30 );
    owner.PublishEvents( registry );
    const auto& breaks = registry.ctx<ECS::DestructionEventQueue>().Breaks;
    ASSERT_FALSE( breaks.empty() );
    EXPECT_EQ( breaks.front().Self, entity );
    EXPECT_EQ( breaks.front().Node, 1 ) << "the hit piece breaks off";

    // Published again with nothing new: the queue is the frame's, not an accumulation.
    f.destruction->ClearEvents();
    owner.PublishEvents( registry );
    EXPECT_TRUE( registry.ctx<ECS::DestructionEventQueue>().Breaks.empty() );
}

TEST( DestructionBreak, RemovedAndUnownedNameNothing )
{
    const DestructibleHandle object = 7;
    const DestructionEvent   removed{ DestructionEventKind::Removed, object, 1 };
    const DestructionEvent   unowned{ DestructionEventKind::Break, object + 1, 2 };
    const DestructionEvent   events[] = { removed, unowned };

    entt::registry                                             registry;
    const std::unordered_map<DestructibleHandle, entt::entity> objects = { { object, registry.create() } };
    std::vector<ECS::DestructionBreakEvent>                    out;
    ECS::NameBreakEvents( events, objects, out );
    EXPECT_TRUE( out.empty() ) << "Removed is the simulation's bookkeeping; an object with no entity names no one";
}
