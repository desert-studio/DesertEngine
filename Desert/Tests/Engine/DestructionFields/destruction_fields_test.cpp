// DST-04: fields fired once at a two-piece fracture on headless Jolt (zero gravity). A radial impulse inside its
// radius breaks the piece it reaches and pushes it, outside it does nothing; a strain field breaks without a
// contact; a kill removes the body it reads above zero at; an anchored piece stays while the rest flies.

#include <Engine/Destruction/DestructionField.hpp>
#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <memory>

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
            leaf.HullVertices.emplace_back( i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z );
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
        DestructibleHandle                object = kInvalidDestructible;

        explicit Fixture( float threshold )
        {
            physics.Init( 0.0f );
            destruction = std::make_unique<DestructionWorld>( physics );
            auto added  = destruction->Add( TwoCubes( threshold ), DestructibleDesc{} );
            EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
            object = added.IsSuccess() ? added.GetValue() : kInvalidDestructible;
        }

        void Run( int steps )
        {
            for ( int i = 0; i < steps; ++i )
                physics.Step( kStep );
        }

        Physics::BodyHandle Body( int32_t node ) const
        {
            return destruction->GetNodeBody( object, node );
        }
    };

    // RadialImpulse as the component builds it: a linear falloff times the direction away from the centre.
    FieldCommand RadialImpulse( const glm::vec3& centre, float magnitude, float radius )
    {
        SumVector impulse;
        impulse.Scalar = RadialFalloff{ .Magnitude = magnitude, .Radius = radius, .Position = centre };
        impulse.Left   = RadialVector{ .Magnitude = 1.0f, .Position = centre };
        return FieldCommand{ .Type = FieldPhysicsType::Impulse, .Vector = impulse };
    }

    FieldCommand Strain( float magnitude )
    {
        return FieldCommand{ .Type   = FieldPhysicsType::ExternalStrain,
                             .Scalar = RadialFalloff{ .Magnitude = magnitude,
                                                      .Radius    = 1000.0f,
                                                      .Falloff   = FieldFalloff::None } };
    }
} // namespace

TEST( DestructionFields, FalloffNodesFollowUe )
{
    const RadialFalloff linear{ .Magnitude = 10.0f, .Radius = 100.0f };
    EXPECT_FLOAT_EQ( linear.Evaluate( { 50.0f, 0.0f, 0.0f } ), 5.0f );
    EXPECT_FLOAT_EQ( linear.Evaluate( { 150.0f, 0.0f, 0.0f } ), 0.0f ) << "outside reads Default";
    const BoxFalloff box{ .Magnitude = 2.0f, .Falloff = FieldFalloff::None };
    EXPECT_FLOAT_EQ( box.Evaluate( { 49.0f, 0.0f, 0.0f } ), 2.0f );
    EXPECT_FLOAT_EQ( box.Evaluate( { 51.0f, 0.0f, 0.0f } ), 0.0f ) << "the unit box is 100 cm";
    const RadialIntMask mask{ .Radius = 10.0f };
    EXPECT_EQ( mask.Evaluate( { 5.0f, 0.0f, 0.0f } ), 1 );
    EXPECT_EQ( mask.Evaluate( { 15.0f, 0.0f, 0.0f } ), 0 );
}

TEST( DestructionFields, ImpulseInsideRadiusBreaksAndPushes )
{
    Fixture f( 1.0e4f );
    // Node 1's centre is 10 cm from the centre, node 2's 90 cm: only node 1 is inside the 50 cm radius. The centre
    // sits on node 1's inner side so the push is -X, away from node 2: a centre on the outer side drives node 1
    // into the face it shares with node 2 and the contact hands node 2 the momentum (76 cm/s after one step).
    EXPECT_GT( f.destruction->ApplyField( RadialImpulse( { -40.0f, 50.0f, 50.0f }, 1.0e6f, 50.0f ) ), 0u );
    EXPECT_EQ( f.destruction->GetBodyCount( f.object ), 2u );
    ASSERT_FALSE( f.destruction->GetEvents().empty() );
    EXPECT_EQ( f.destruction->GetEvents().front().Node, 1 );
    f.Run( 1 );
    EXPECT_LT( f.physics.GetLinearVelocity( f.Body( 1 ) ).x, -50.0f ) << "pushed away from the centre";
    EXPECT_LT( glm::length( f.physics.GetLinearVelocity( f.Body( 2 ) ) ), 1.0f ) << "outside the radius";
}

TEST( DestructionFields, ImpulseOutsideRadiusDoesNothing )
{
    Fixture f( 1.0e4f );
    EXPECT_EQ( f.destruction->ApplyField( RadialImpulse( { -500.0f, 50.0f, 50.0f }, 1.0e6f, 150.0f ) ), 0u );
    f.Run( 5 );
    EXPECT_EQ( f.destruction->GetBodyCount( f.object ), 1u );
    EXPECT_TRUE( f.destruction->GetEvents().empty() );
    EXPECT_LT( glm::length( f.physics.GetLinearVelocity( f.Body( 1 ) ) ), 1.0e-3f );
}

TEST( DestructionFields, StrainBreaksWithoutContact )
{
    Fixture below( 1.0e4f );
    below.destruction->ApplyField( Strain( 5.0e3f ) );
    EXPECT_EQ( below.destruction->GetBodyCount( below.object ), 1u ) << "below the internal strain";

    Fixture above( 1.0e4f );
    above.destruction->ApplyField( Strain( 2.0e4f ) );
    EXPECT_EQ( above.destruction->GetBodyCount( above.object ), 2u );
    EXPECT_NE( above.Body( 1 ), above.Body( 2 ) );
    above.Run( 2 );
    EXPECT_EQ( above.destruction->GetBodyCount( above.object ), 2u ) << "the strain was one instant, not kept";
}

TEST( DestructionFields, KillRemovesTheBodyItReaches )
{
    Fixture f( 1.0e4f );
    f.destruction->ApplyField( Strain( 2.0e4f ) );
    f.destruction->ClearEvents();
    const FieldCommand kill{ .Type   = FieldPhysicsType::Kill,
                             .Scalar = RadialIntMask{ .Radius = 10.0f, .Position = { 50.0f, 50.0f, 50.0f } } };
    EXPECT_EQ( f.destruction->ApplyField( kill ), 1u );
    EXPECT_EQ( f.Body( 2 ), Physics::kInvalidBody );
    EXPECT_NE( f.Body( 1 ), Physics::kInvalidBody );
    ASSERT_EQ( f.destruction->GetEvents().size(), 1u );
    EXPECT_EQ( f.destruction->GetEvents().front().Kind, DestructionEventKind::Removed );
    EXPECT_EQ( f.destruction->GetEvents().front().Node, 2 );
}

TEST( DestructionFields, AnchorHoldsWhileTheRestFlies )
{
    Fixture f( 1.0e4f );
    // The unit box around node 1's centre: node 1 is anchored, node 2 is not.
    const FieldCommand anchor{
         .Type   = FieldPhysicsType::Anchor,
         .Scalar = BoxFalloff{ .Transform = glm::translate( glm::mat4( 1.0f ), { -50.0f, 50.0f, 50.0f } ),
                               .Falloff   = FieldFalloff::None } };
    EXPECT_EQ( f.destruction->ApplyField( anchor ), 1u );
    f.destruction->ApplyField( FieldCommand{ .Type   = FieldPhysicsType::Impulse,
                                             .Vector = UniformVector{ 1.0e6f, { 1.0f, 0.0f, 0.0f } } } );
    ASSERT_EQ( f.destruction->GetBodyCount( f.object ), 2u );
    const glm::vec3 held = f.physics.GetPosition( f.Body( 1 ) );
    f.Run( 30 );
    EXPECT_LT( glm::length( f.physics.GetPosition( f.Body( 1 ) ) - held ), 1.0e-3f ) << "anchored";
    EXPECT_GT( f.physics.GetLinearVelocity( f.Body( 2 ) ).x, 50.0f ) << "the free piece flies";
}
