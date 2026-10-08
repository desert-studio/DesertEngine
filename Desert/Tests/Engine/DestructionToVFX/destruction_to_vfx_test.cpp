// DST-05: the destruction events (DestructionWorld, headless Jolt) written into the scene's VFX data channels
// through the project's own channel assets (Content/VFX/Destruction*.dfxch): N breaks -> N entries with their
// fields, collisions above the object's minimum impulse only, removals, and the per-destructible flags.

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Serialization/VFXDataChannel.hpp>
#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/ECS/System/DestructionVFXEvents.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/VFX/VFXDataChannel.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace Desert;
using namespace Desert::Destruction;
namespace S = Desert::Assets::Serialization;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

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

    // N 100 cm cubes in a row along X under one root; a threshold of zero breaks every one off at once.
    std::shared_ptr<const FractureData> CubeRow( int count, float threshold )
    {
        auto         data = std::make_shared<FractureData>();
        FractureNode root;
        root.Volume       = 1.0e6 * count;
        root.CenterOfMass = glm::dvec3( -50.0 + 50.0 * ( count - 1 ), 50.0, 50.0 );
        data->Nodes.push_back( root );
        for ( int i = 0; i < count; ++i )
            data->Nodes.push_back(
                 BoxLeaf( { -100.0f + 100.0f * i, 0.0f, 0.0f }, { 100.0f * i, 100.0f, 100.0f }, threshold ) );
        return data;
    }

    struct Fixture
    {
        Physics::PhysicsWorld             physics;
        std::unique_ptr<DestructionWorld> destruction;
        VFX::VFXDataChannels              channels;

        Fixture()
        {
            physics.Init( 0.0f );
            destruction = std::make_unique<DestructionWorld>( physics );
            // The project's own channel assets: the writer and the shipped layouts must agree.
            for ( const std::string_view name : { ECS::kDestructionBreakChannel, ECS::kDestructionCollisionChannel,
                                                  ECS::kDestructionRemovedChannel } )
            {
                const auto path = TestSupport::RepositoryRoot() / "Projects" / "Desert" / "Content" / "VFX" /
                                  ( std::string( name ) + S::kVFXDataChannelExtension );
                auto loaded = S::LoadVFXDataChannelFile( path );
                EXPECT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
                if ( loaded.IsSuccess() )
                    EXPECT_TRUE( channels.Register( std::string( name ), loaded.GetValue() ).IsSuccess() );
            }
        }

        // A 100 kg ball at x = -150 flying at +X into the first cube's face at x = -100.
        void Throw( float speed )
        {
            Physics::BodyDesc ball;
            ball.Shape    = Physics::ShapeType::Sphere;
            ball.Radius   = 20.0f;
            ball.Mass     = 100.0f;
            ball.Position = { -150.0f, 50.0f, 50.0f };
            auto created  = physics.CreateBody( ball );
            ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
            physics.SetLinearVelocity( created.GetValue(), { speed, 0.0f, 0.0f } );
        }

        void Run( int steps )
        {
            for ( int i = 0; i < steps; ++i )
                physics.Step( kStep );
        }

        ECS::DestructionVFXReport Publish( const ECS::DestructionEventSources& sources )
        {
            ECS::DestructionVFXReport report;
            auto written = ECS::WriteDestructionEventsToVFX( destruction->GetEvents(), sources, channels, report );
            EXPECT_TRUE( written.IsSuccess() ) << written.GetError();
            return report;
        }

        [[nodiscard]] std::size_t Entries( std::string_view channel ) const
        {
            const VFX::VFXDataChannel* found = channels.Find( channel );
            return found == nullptr ? 0 : found->EntryCount();
        }

        // Component @p c of field @p field of entry @p entry.
        [[nodiscard]] float Field( std::string_view channel, std::size_t entry, std::string_view field,
                                   int c = 0 ) const
        {
            const VFX::VFXDataChannel* found = channels.Find( channel );
            const int                  index = found->Layout().Find( field );
            EXPECT_GE( index, 0 ) << channel << " has no field " << field;
            return found->Entry( entry )[found->Layout().Offsets[index] + c];
        }
    };

    ECS::DestructionEventSources AllOn( DestructibleHandle object, int32_t entity )
    {
        return { { object, ECS::DestructionEventSource{ entity, true, true, true } } };
    }

    int Count( const DestructionWorld& world, DestructionEventKind kind )
    {
        int n = 0;
        for ( const DestructionEvent& e : world.GetEvents() )
            n += e.Kind == kind ? 1 : 0;
        return n;
    }
} // namespace

TEST( DestructionToVFX, NBreaksBecomeNEntriesWithTheirFields )
{
    Fixture f;
    auto    added = f.destruction->Add( CubeRow( 3, 0.0f ), DestructibleDesc{} );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    const glm::vec3 velocity( 300.0f, 0.0f, -200.0f );
    f.physics.SetLinearVelocity( f.destruction->GetNodeBody( added.GetValue(), 0 ), velocity );
    f.Run( 1 );
    const int breaks = Count( *f.destruction, DestructionEventKind::Break );
    ASSERT_EQ( breaks, 3 ) << "every cube breaks off on its own";

    const auto report = f.Publish( AllOn( added.GetValue(), 7 ) );
    EXPECT_EQ( report.Breaks, 3u );
    ASSERT_EQ( f.Entries( ECS::kDestructionBreakChannel ), 3u );
    std::size_t entry = 0;
    for ( const DestructionEvent& e : f.destruction->GetEvents() )
    {
        if ( e.Kind != DestructionEventKind::Break )
            continue;
        const auto ch = ECS::kDestructionBreakChannel;
        EXPECT_NEAR( e.Position.x, -50.0f + 100.0f * ( e.Node - 1 ), 2.0f ) << "the group's centre of mass";
        for ( int c = 0; c < 3; ++c )
        {
            EXPECT_FLOAT_EQ( f.Field( ch, entry, "Position", c ), e.Position[c] );
            EXPECT_FLOAT_EQ( f.Field( ch, entry, "Velocity", c ), e.Velocity[c] );
        }
        EXPECT_NEAR( f.Field( ch, entry, "Velocity", 0 ), velocity.x, 1.0f ) << "the group keeps moving";
        EXPECT_NEAR( f.Field( ch, entry, "Speed" ), glm::length( e.Velocity ), 1e-3f );
        EXPECT_NEAR( f.Field( ch, entry, "MassKg" ), 1.0e6f * 0.0024f, 0.5f ) << "volume x density";
        EXPECT_EQ( f.Field( ch, entry, "PieceCount" ), 1.0f );
        EXPECT_EQ( f.Field( ch, entry, "SourceEntity" ), 7.0f );
        ++entry;
    }
    EXPECT_EQ( f.Entries( ECS::kDestructionCollisionChannel ), 0u );
}

TEST( DestructionToVFX, OnlyCollisionsAtOrAboveTheMinimumImpulseAreRecorded )
{
    for ( const float minimum : { 1.0f, 1.0e12f } )
    {
        Fixture          f;
        DestructibleDesc desc;
        desc.Settings.CollisionEvents          = true;
        desc.Settings.CollisionEventMinImpulse = minimum;
        auto added                             = f.destruction->Add( CubeRow( 2, 1.0e12f ), desc ); // never breaks
        ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
        f.Throw( 1000.0f );
        f.Run( 30 );
        const int collisions = Count( *f.destruction, DestructionEventKind::Collision );
        if ( minimum > 1.0e9f )
        {
            EXPECT_EQ( collisions, 0 ) << "no contact reaches the minimum";
            continue;
        }
        ASSERT_GT( collisions, 0 ) << "the ball's hit is a collision event";
        const auto report = f.Publish( AllOn( added.GetValue(), 3 ) );
        EXPECT_EQ( report.Collisions, static_cast<uint32_t>( collisions ) );
        const auto ch = ECS::kDestructionCollisionChannel;
        for ( std::size_t i = 0; i < f.Entries( ch ); ++i )
        {
            EXPECT_GE( f.Field( ch, i, "Impulse" ), minimum );
            EXPECT_LT( f.Field( ch, i, "Normal", 0 ), -0.9f ) << "out of the cube's -X face, towards the ball";
            EXPECT_NEAR( f.Field( ch, i, "Position", 0 ), -100.0f, 5.0f ) << "on the face that was hit";
            EXPECT_NEAR( f.Field( ch, i, "MassKg" ), 2.0e6f * 0.0024f, 1.0f ) << "the whole body's mass";
            EXPECT_EQ( f.Field( ch, i, "SourceEntity" ), 3.0f );
        }
    }

    Fixture f; // the object does not ask for collisions: none recorded, however hard the hit
    auto    added = f.destruction->Add( CubeRow( 2, 1.0e12f ), DestructibleDesc{} );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    f.Throw( 1000.0f );
    f.Run( 30 );
    EXPECT_EQ( Count( *f.destruction, DestructionEventKind::Collision ), 0 );

    DestructibleDesc negative;
    negative.Settings.CollisionEventMinImpulse = -1.0f;
    EXPECT_FALSE( f.destruction->Add( CubeRow( 2, 1.0e12f ), negative ).IsSuccess() );
}

TEST( DestructionToVFX, RemovedPiecesBecomeRemovedEntries )
{
    Fixture          f;
    DestructibleDesc desc;
    desc.Settings.MaxSleepTime = { 0.1f, 0.1f };
    auto added                 = f.destruction->Add( CubeRow( 2, 0.0f ), desc );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    f.Run( 30 );
    ASSERT_EQ( Count( *f.destruction, DestructionEventKind::Removed ), 2 );
    const auto report = f.Publish( AllOn( added.GetValue(), 11 ) );
    EXPECT_EQ( report.Removals, 2u );
    const auto ch = ECS::kDestructionRemovedChannel;
    ASSERT_EQ( f.Entries( ch ), 2u );
    for ( std::size_t i = 0; i < 2; ++i )
    {
        EXPECT_NEAR( f.Field( ch, i, "MassKg" ), 1.0e6f * 0.0024f, 0.5f );
        EXPECT_EQ( f.Field( ch, i, "PieceCount" ), 1.0f );
        EXPECT_EQ( f.Field( ch, i, "SourceEntity" ), 11.0f );
        EXPECT_NEAR( f.Field( ch, i, "Position", 1 ), 50.0f, 1.0f ) << "where the piece lay";
    }
}

TEST( DestructionToVFX, EachDestructiblePublishesOnlyTheKindsItsFlagsName )
{
    Fixture          f;
    DestructibleDesc desc;
    desc.Settings.MaxSleepTime  = { 0.1f, 0.1f };
    auto             breaksOnly = f.destruction->Add( CubeRow( 2, 0.0f ), desc );
    DestructibleDesc apart      = desc;
    apart.Position              = { 0.0f, 0.0f, 1000.0f }; // 10 m away: the two never touch
    auto removalsOnly           = f.destruction->Add( CubeRow( 3, 0.0f ), apart );
    ASSERT_TRUE( breaksOnly.IsSuccess() && removalsOnly.IsSuccess() );
    f.Run( 30 ); // both break at once, then sleep and are removed
    ECS::DestructionEventSources sources;
    sources[breaksOnly.GetValue()]   = ECS::DestructionEventSource{ 1, true, false, false };
    sources[removalsOnly.GetValue()] = ECS::DestructionEventSource{ 2, false, false, true };
    const auto report                = f.Publish( sources );
    EXPECT_EQ( report.Breaks, 2u ) << "the two cubes of the first object, not the three of the second";
    EXPECT_EQ( report.Removals, 3u ) << "the three cubes of the second object, not the two of the first";
    for ( std::size_t i = 0; i < f.Entries( ECS::kDestructionBreakChannel ); ++i )
        EXPECT_EQ( f.Field( ECS::kDestructionBreakChannel, i, "SourceEntity" ), 1.0f );
    for ( std::size_t i = 0; i < f.Entries( ECS::kDestructionRemovedChannel ); ++i )
        EXPECT_EQ( f.Field( ECS::kDestructionRemovedChannel, i, "SourceEntity" ), 2.0f );
}

TEST( DestructionToVFX, NothingIsWrittenOrRegisteredWhenTheFlagsAreOff )
{
    Fixture          f;
    DestructibleDesc desc;
    desc.Settings.MaxSleepTime = { 0.1f, 0.1f };
    auto added                 = f.destruction->Add( CubeRow( 2, 0.0f ), desc );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    f.Run( 30 );
    ASSERT_FALSE( f.destruction->GetEvents().empty() );

    ECS::DestructionEventSources off;
    off[added.GetValue()] = ECS::DestructionEventSource{ 5, false, false, false };
    const auto report     = f.Publish( off );
    EXPECT_EQ( report.Breaks + report.Collisions + report.Removals, 0u );
    EXPECT_EQ( f.Entries( ECS::kDestructionBreakChannel ), 0u );
    EXPECT_EQ( f.Entries( ECS::kDestructionRemovedChannel ), 0u );
    EXPECT_EQ( f.Publish( {} ).Breaks, 0u ) << "an object no entity names (gone) publishes nothing";

    // Flags off: the channels are not even asked for, so a scene without the assets is not refused.
    VFX::VFXDataChannels empty;
    Assets::AssetManager assets;
    EXPECT_TRUE( ECS::UseDestructionChannels( off, empty, assets ).IsSuccess() );
    EXPECT_EQ( empty.Find( ECS::kDestructionBreakChannel ), nullptr );
    // Flags on, channel missing: refused by name, not dropped silently.
    ECS::DestructionVFXReport report2;
    auto written = ECS::WriteDestructionEventsToVFX( f.destruction->GetEvents(), AllOn( added.GetValue(), 5 ),
                                                     empty, report2 );
    ASSERT_FALSE( written.IsSuccess() );
    EXPECT_NE( written.GetError().find( "DestructionBreak" ), std::string::npos ) << written.GetError();
}
