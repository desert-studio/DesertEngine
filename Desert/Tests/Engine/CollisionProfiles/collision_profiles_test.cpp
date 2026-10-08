// PHYS-A1: collision channels and profiles (UE ECollisionChannel / FCollisionResponseTemplate) drive Jolt.
//
//   1. The register: every refusal of CollisionProfiles::Build names its row; a missing file is an error naming
//      the path (there is no built-in register); the project's committed Config/CollisionProfiles.json builds.
//   2. The pair rule: the lesser of the two bodies' responses to each other's channel (UE), and Ignore when either
//      profile takes no part in physics.
//   3. Jolt obeys it: a PhysicsActor ball rests on a BlockAll floor, falls through an IgnoreAll floor (the pair
//      never reaches the narrow phase) and through a Trigger floor (Overlap = a sensor contact: found, not solved,
//      no impulse reported); a ray does not find a NoCollision / PhysicsOnly body; a Pawn capsule stands on a
//      BlockAll floor and falls through a Trigger floor.
//
// Mutations this suite must turn red (the test that should fail is named on each line; run them before trusting it):
//   * CollisionProfiles::PhysicsResponse — std::min → std::max            (PairResponseIsTheLesserOfTheTwo)
//   * PhysicsWorld.cpp ContactRecorder::Respond — drop `settings.mIsSensor = true`  (ABallPassesThroughATrigger)
//   * PhysicsWorld::CastRay — drop the QueryableLayerFilter argument     (ARayDoesNotFindBodiesOutsideQueries)

#include <Engine/Physics/CollisionProfiles.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include "../../TestSupport/repo_root.hpp"
#include "../PhysicsFixture.hpp"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Desert;
using Physics::CollisionEnabled;
using Physics::CollisionResponse;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // Build must refuse @p config with an error containing @p words.
    void ExpectRefused( const Physics::CollisionProfilesConfig& config, std::string_view words )
    {
        const auto built = Physics::CollisionProfiles::Build( config );
        ASSERT_FALSE( built.IsSuccess() ) << "accepted; expected a refusal mentioning '" << words << "'";
        EXPECT_NE( built.GetError().find( words ), std::string::npos ) << built.GetError();
    }

    Physics::CollisionProfileId Id( const Physics::CollisionProfiles& profiles, std::string_view name )
    {
        auto id = profiles.Resolve( name );
        EXPECT_TRUE( id.IsSuccess() ) << id.GetError();
        return id.IsSuccess() ? id.GetValue() : Physics::kNoProfile;
    }

    struct SimWorld
    {
        Physics::PhysicsWorld Physics;
        SimWorld()
        {
            EXPECT_TRUE( Physics.Init( 981.0f, TestSupport::PhysicsTestProfiles() ) );
        }
        ~SimWorld()
        {
            Physics.Shutdown();
        }
        SimWorld( const SimWorld& )            = delete;
        SimWorld& operator=( const SimWorld& ) = delete;

        // A static 10 m × 1 m × 10 m slab whose top face is y = 0.
        Physics::BodyHandle Floor( std::string_view profile, glm::vec3 at = glm::vec3( 0.0f ) )
        {
            Physics::BodyDesc desc;
            desc.Shape       = Physics::ShapeType::Box;
            desc.Type        = Physics::BodyType::Static;
            desc.HalfExtents = { 500.0f, 50.0f, 500.0f };
            desc.Position    = at + glm::vec3( 0.0f, -50.0f, 0.0f );
            desc.Profile     = TestSupport::ProfileId( Physics, profile );
            const auto body  = Physics.CreateBody( desc );
            EXPECT_TRUE( body.IsSuccess() ) << body.GetError();
            return body.IsSuccess() ? body.GetValue() : Physics::kInvalidBody;
        }

        // A PhysicsActor ball of radius 25 cm dropped from 2 m.
        Physics::BodyHandle Ball()
        {
            Physics::BodyDesc desc;
            desc.Shape       = Physics::ShapeType::Sphere;
            desc.Radius      = 25.0f;
            desc.Restitution = 0.0f;
            desc.Position    = { 0.0f, 200.0f, 0.0f };
            desc.Profile     = TestSupport::ProfileId( Physics, "PhysicsActor" );
            const auto body  = Physics.CreateBody( desc );
            EXPECT_TRUE( body.IsSuccess() ) << body.GetError();
            return body.IsSuccess() ? body.GetValue() : Physics::kInvalidBody;
        }

        void Run( float seconds )
        {
            for ( int i = 0; i < static_cast<int>( seconds / kStep ); ++i )
                Physics.Step( kStep );
        }
    };

    float BallHeightAfterFallingOnto( std::string_view floorProfile )
    {
        SimWorld world;
        world.Floor( floorProfile );
        const Physics::BodyHandle ball = world.Ball();
        world.Run( 3.0f );
        return world.Physics.GetPosition( ball ).y;
    }
} // namespace

// ---- 1. The register ------------------------------------------------------------------------------------------

TEST( CollisionProfiles, TheTestRegisterAndTheProjectRegisterBuild )
{
    const auto built = Physics::CollisionProfiles::Build( TestSupport::PhysicsTestProfilesConfig() );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();

    const std::filesystem::path project =
         TestSupport::RepoRoot() / "Projects" / "Desert" / "Config" / Physics::kCollisionProfilesFileName;
    const auto read = Physics::CollisionProfiles::Read( project );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    for ( const std::string_view engine :
          { Physics::EngineProfiles::kBlockAll, Physics::EngineProfiles::kBlockAllDynamic,
            Physics::EngineProfiles::kPhysicsActor, Physics::EngineProfiles::kPawn,
            Physics::EngineProfiles::kDestructible } )
        EXPECT_TRUE( read.GetValue().Resolve( engine ).IsSuccess() ) << engine;
}

TEST( CollisionProfiles, AMissingRegisterIsAnErrorNamingThePath )
{
    const std::filesystem::path missing =
         std::filesystem::temp_directory_path() / "desert-no-such-project" / "Config" / "CollisionProfiles.json";
    const auto read = Physics::CollisionProfiles::Read( missing );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( missing.string() ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "does not exist" ), std::string::npos ) << read.GetError();
}

TEST( CollisionProfiles, BuildRefusesEachBrokenRowByName )
{
    const Physics::CollisionProfilesConfig good = TestSupport::PhysicsTestProfilesConfig();

    auto noChannels = good;
    noChannels.Channels.clear();
    ExpectRefused( noChannels, "0 channels" );

    auto repeatedChannel = good;
    repeatedChannel.Channels.push_back( { "Pawn", CollisionResponse::Block } );
    ExpectRefused( repeatedChannel, "collision channel 'Pawn' is empty or repeated" );

    auto noDefault = good;
    noDefault.Channels[1].DefaultResponse.reset();
    ExpectRefused( noDefault, "collision channel 'WorldDynamic' states no DefaultResponse" );

    auto repeatedProfile = good;
    repeatedProfile.Profiles.push_back( { "Trigger", CollisionEnabled::QueryOnly, "WorldStatic", {} } );
    ExpectRefused( repeatedProfile, "collision profile 'Trigger' is empty or repeated" );

    auto noEnabled = good;
    noEnabled.Profiles[0].Enabled.reset();
    ExpectRefused( noEnabled, "collision profile 'BlockAll' states no Enabled" );

    auto unknownObjectChannel                      = good;
    unknownObjectChannel.Profiles[0].ObjectChannel = "Water";
    ExpectRefused( unknownObjectChannel, "ObjectChannel 'Water' is no channel" );

    auto unknownResponseChannel = good;
    unknownResponseChannel.Profiles[0].Responses.push_back( { "Water", CollisionResponse::Ignore } );
    ExpectRefused( unknownResponseChannel, "response to 'Water' names no channel" );

    auto noResponse = good;
    noResponse.Profiles[0].Responses.push_back( { "Pawn", std::nullopt } );
    ExpectRefused( noResponse, "response to 'Pawn' names no channel or states no Response" );

    for ( const std::string_view engine :
          { Physics::EngineProfiles::kBlockAll, Physics::EngineProfiles::kBlockAllDynamic,
            Physics::EngineProfiles::kPhysicsActor, Physics::EngineProfiles::kPawn,
            Physics::EngineProfiles::kDestructible } )
    {
        auto withoutEngineProfile = good;
        std::erase_if( withoutEngineProfile.Profiles,
                       [&]( const Physics::CollisionProfileConfig& row ) { return row.Name == engine; } );
        ExpectRefused( withoutEngineProfile, "the engine profile '" + std::string( engine ) + "' is missing" );
    }
}

TEST( CollisionProfiles, ResolveRefusesAnUnknownNameListingTheProfiles )
{
    const Physics::CollisionProfiles profiles = TestSupport::PhysicsTestProfiles();
    const auto                       unknown  = profiles.Resolve( "Water" );
    ASSERT_FALSE( unknown.IsSuccess() );
    EXPECT_NE( unknown.GetError().find( "'Water'" ), std::string::npos ) << unknown.GetError();
    EXPECT_NE( unknown.GetError().find( "PhysicsActor" ), std::string::npos ) << unknown.GetError();
}

// ---- 2. The pair rule -----------------------------------------------------------------------------------------

TEST( CollisionProfiles, PairResponseIsTheLesserOfTheTwo )
{
    // A answers Block to B's channel, B answers Overlap to A's: the pair overlaps, whichever way it is asked.
    Physics::CollisionProfilesConfig config = TestSupport::PhysicsTestProfilesConfig();
    config.Profiles.push_back( { "A",
                                 CollisionEnabled::QueryAndPhysics,
                                 "WorldStatic",
                                 { { "WorldDynamic", CollisionResponse::Block } } } );
    config.Profiles.push_back( { "B",
                                 CollisionEnabled::QueryAndPhysics,
                                 "WorldDynamic",
                                 { { "WorldStatic", CollisionResponse::Overlap } } } );
    config.Profiles.push_back( { "C",
                                 CollisionEnabled::QueryAndPhysics,
                                 "WorldDynamic",
                                 { { "WorldStatic", CollisionResponse::Ignore } } } );
    const auto built = Physics::CollisionProfiles::Build( config );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    const Physics::CollisionProfiles& profiles = built.GetValue();

    const auto a = Id( profiles, "A" );
    const auto b = Id( profiles, "B" );
    const auto c = Id( profiles, "C" );
    EXPECT_EQ( profiles.PhysicsResponse( a, b ), CollisionResponse::Overlap );
    EXPECT_EQ( profiles.PhysicsResponse( b, a ), CollisionResponse::Overlap );
    EXPECT_EQ( profiles.PhysicsResponse( a, c ), CollisionResponse::Ignore );
    EXPECT_EQ( profiles.PhysicsResponse( c, a ), CollisionResponse::Ignore );
    EXPECT_EQ( profiles.PhysicsResponse( a, a ), CollisionResponse::Block );

    // The shipped-shape profiles against a simulated body.
    const auto actor = Id( profiles, "PhysicsActor" );
    EXPECT_EQ( profiles.PhysicsResponse( actor, Id( profiles, "BlockAll" ) ), CollisionResponse::Block );
    EXPECT_EQ( profiles.PhysicsResponse( actor, Id( profiles, "Trigger" ) ), CollisionResponse::Overlap );
    EXPECT_EQ( profiles.PhysicsResponse( actor, Id( profiles, "IgnoreAll" ) ), CollisionResponse::Ignore );
}

TEST( CollisionProfiles, AProfileOutsidePhysicsIgnoresEverything )
{
    const Physics::CollisionProfiles profiles = TestSupport::PhysicsTestProfiles();
    const auto                       blockAll = Id( profiles, "BlockAll" );
    for ( const std::string_view outside : { "NoCollision", "QueryOnly" } )
    {
        EXPECT_EQ( profiles.PhysicsResponse( Id( profiles, outside ), blockAll ), CollisionResponse::Ignore )
             << outside;
        EXPECT_EQ( profiles.PhysicsResponse( blockAll, Id( profiles, outside ) ), CollisionResponse::Ignore )
             << outside;
    }
    EXPECT_EQ( profiles.PhysicsResponse( Id( profiles, "PhysicsOnly" ), blockAll ), CollisionResponse::Block );

    EXPECT_TRUE( profiles.IsQueryable( blockAll ) );
    EXPECT_TRUE( profiles.IsQueryable( Id( profiles, "QueryOnly" ) ) );
    EXPECT_FALSE( profiles.IsQueryable( Id( profiles, "PhysicsOnly" ) ) );
    EXPECT_FALSE( profiles.IsQueryable( Id( profiles, "NoCollision" ) ) );
}

// ---- 3. Jolt obeys the register -------------------------------------------------------------------------------

TEST( CollisionProfiles, ABodyWithoutAProfileIsRefused )
{
    SimWorld          world;
    Physics::BodyDesc desc; // Profile = kNoProfile
    const auto        body = world.Physics.CreateBody( desc );
    ASSERT_FALSE( body.IsSuccess() );
    EXPECT_NE( body.GetError().find( "carries no collision profile" ), std::string::npos ) << body.GetError();

    const auto character = world.Physics.CreateCharacter( {} );
    ASSERT_FALSE( character.IsSuccess() );
    EXPECT_NE( character.GetError().find( "carries no collision profile" ), std::string::npos )
         << character.GetError();
    EXPECT_EQ( world.Physics.GetBodyCount(), 0u );
    EXPECT_EQ( world.Physics.GetCharacterCount(), 0u );
}

TEST( CollisionProfiles, ABallRestsOnABlockingFloor )
{
    EXPECT_NEAR( BallHeightAfterFallingOnto( "BlockAll" ), 25.0f, 2.0f );
}

TEST( CollisionProfiles, ABallFallsThroughAnIgnoringFloor )
{
    EXPECT_LT( BallHeightAfterFallingOnto( "IgnoreAll" ), -200.0f );
}

TEST( CollisionProfiles, ABallPassesThroughATriggerAndNoImpulseIsReported )
{
    EXPECT_LT( BallHeightAfterFallingOnto( "Trigger" ), -200.0f );

    // The same through a body that reports its contacts: a Trigger contact is a sensor contact, never solved and
    // never measured; the BlockAll floor under it is (the positive control).
    SimWorld world;
    world.Floor( "Trigger", { 0.0f, 100.0f, 0.0f } );
    world.Floor( "BlockAll", { 0.0f, -300.0f, 0.0f } );
    std::array<glm::vec3, 8> corners{};
    for ( int i = 0; i < 8; ++i )
        corners[i] = { ( i & 1 ) != 0 ? 25.0f : -25.0f, ( i & 2 ) != 0 ? 25.0f : -25.0f,
                       ( i & 4 ) != 0 ? 25.0f : -25.0f };
    const std::array<std::span<const glm::vec3>, 1> parts = { std::span<const glm::vec3>( corners ) };
    Physics::CompoundBodyDesc                       cube;
    cube.Parts                 = parts;
    cube.Mass                  = 10.0f;
    cube.Restitution           = 0.0f;
    cube.Position              = { 0.0f, 300.0f, 0.0f };
    cube.ReportContactImpulses = true;
    cube.Profile               = TestSupport::ProfileId( world.Physics, "PhysicsActor" );
    const auto body            = world.Physics.CreateCompoundBody( cube );
    ASSERT_TRUE( body.IsSuccess() ) << body.GetError();

    bool reportedAboveTheBlockingFloor = false;
    bool reportedOnTheBlockingFloor    = false;
    for ( int i = 0; i < 240; ++i )
    {
        world.Physics.Step( kStep );
        const float y = world.Physics.GetPosition( body.GetValue() ).y;
        for ( const Physics::ContactImpulse& contact : world.Physics.GetStepContactImpulses() )
        {
            if ( contact.Impulse <= 0.0f )
                continue;
            ( y > -250.0f ? reportedAboveTheBlockingFloor : reportedOnTheBlockingFloor ) = true;
        }
    }
    EXPECT_FALSE( reportedAboveTheBlockingFloor ) << "the Trigger contact was solved or measured";
    EXPECT_TRUE( reportedOnTheBlockingFloor ) << "the BlockAll floor reported nothing: the instrument is blind";
    EXPECT_NEAR( world.Physics.GetPosition( body.GetValue() ).y, -300.0f + 25.0f, 2.0f );
}

TEST( CollisionProfiles, ARayDoesNotFindBodiesOutsideQueries )
{
    // One body per profile, each straight below its own ray.
    const std::array<std::pair<std::string_view, bool>, 4> rows = { {
         { "BlockAll", true },
         { "QueryOnly", true },
         { "PhysicsOnly", false },
         { "NoCollision", false },
    } };
    SimWorld                                               world;
    for ( std::size_t i = 0; i < rows.size(); ++i )
    {
        const glm::vec3           at( 2000.0f * static_cast<float>( i ), 0.0f, 0.0f );
        const Physics::BodyHandle floor = world.Floor( rows[i].first, at );
        const auto                hit =
             world.Physics.CastRay( at + glm::vec3( 0.0f, 500.0f, 0.0f ), { 0.0f, -1.0f, 0.0f }, 1000.0f );
        EXPECT_EQ( hit.has_value(), rows[i].second ) << rows[i].first;
        if ( hit && rows[i].second )
        {
            EXPECT_EQ( hit->Body, floor ) << rows[i].first;
            EXPECT_NEAR( hit->Distance, 500.0f, 0.5f ) << rows[i].first;
        }
    }
}

TEST( CollisionProfiles, APawnStandsOnABlockingFloorAndFallsThroughATrigger )
{
    for ( const auto& [floorProfile, blocks] :
          std::array<std::pair<std::string_view, bool>, 2>{ { { "BlockAll", true }, { "Trigger", false } } } )
    {
        SimWorld world;
        world.Floor( floorProfile );
        Physics::CharacterDesc desc;
        desc.Radius     = 30.0f;
        desc.HalfHeight = 60.0f;
        desc.Position   = { 0.0f, 200.0f, 0.0f };
        desc.Profile    = TestSupport::ProfileId( world.Physics, "Pawn" );
        const auto pawn = world.Physics.CreateCharacter( desc );
        ASSERT_TRUE( pawn.IsSuccess() ) << pawn.GetError();
        for ( int i = 0; i < 120; ++i )
        {
            world.Physics.Step( kStep );
            world.Physics.UpdateCharacter( pawn.GetValue(), { 0.0f, -500.0f, 0.0f }, kStep );
        }
        const float y = world.Physics.GetCharacterPosition( pawn.GetValue() ).y;
        if ( blocks )
            EXPECT_NEAR( y, 90.0f, 5.0f ) << "the capsule (half height 90 cm) should stand on the floor's top";
        else
            EXPECT_LT( y, -200.0f ) << "the capsule stood on a Trigger: an Overlap pair blocked it";
    }
}
