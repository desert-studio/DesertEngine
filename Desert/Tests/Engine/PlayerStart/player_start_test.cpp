// SPAWN1: the two rules Play is made of — which PlayerStart the pawn stands on, and which camera the
// player looks through (UE: GameModeBase::FindPlayerStart and APlayerCameraManager's view target).
#include <Engine/Core/PlayerStart.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Desert::Core;

namespace
{
    std::vector<PlayerStartCandidate> Starts( std::initializer_list<std::pair<const char*, const char*>> s )
    {
        std::vector<PlayerStartCandidate> out;
        for ( const auto& [name, tag] : s )
            out.push_back( { name, tag } );
        return out;
    }
} // namespace

TEST( PlayerStartRule, NoStartIsAnErrorNotTheOrigin )
{
    const auto r = ChoosePlayerStart( {}, "" );
    ASSERT_FALSE( r );
    EXPECT_NE( r.GetError().find( "no PlayerStart" ), std::string::npos ) << r.GetError();
    EXPECT_NE( r.GetError().find( "origin" ), std::string::npos ) << r.GetError();
}

TEST( PlayerStartRule, TheOnlyStartIsUsedWhateverItsTag )
{
    const auto starts = Starts( { { "Door", "door" } } );
    const auto r      = ChoosePlayerStart( starts, "" );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue(), 0u );
}

TEST( PlayerStartRule, WithoutATagTheOneUntaggedStartWins )
{
    const auto starts = Starts( { { "Door", "door" }, { "Main", "" }, { "Cave", "cave" } } );
    const auto r      = ChoosePlayerStart( starts, "" );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue(), 1u );
}

TEST( PlayerStartRule, TwoUntaggedStartsAreAnErrorNamingBoth )
{
    const auto starts = Starts( { { "StartA", "" }, { "Door", "door" }, { "StartB", "" } } );
    const auto r      = ChoosePlayerStart( starts, "" );
    ASSERT_FALSE( r );
    EXPECT_NE( r.GetError().find( "'StartA'" ), std::string::npos ) << r.GetError();
    EXPECT_NE( r.GetError().find( "'StartB'" ), std::string::npos ) << r.GetError();
    EXPECT_EQ( r.GetError().find( "'Door'" ), std::string::npos ) << r.GetError();
}

TEST( PlayerStartRule, AllTaggedAndNoneAskedForIsAnError )
{
    const auto starts = Starts( { { "Door", "door" }, { "Cave", "cave" } } );
    EXPECT_FALSE( ChoosePlayerStart( starts, "" ) );
}

TEST( PlayerStartRule, ARequestedTagPicksItsStart )
{
    const auto starts = Starts( { { "Main", "" }, { "Cave", "cave" } } );
    const auto r      = ChoosePlayerStart( starts, "cave" );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue(), 1u );
}

TEST( PlayerStartRule, ARequestedTagThatNamesNoneOrTwoIsAnError )
{
    const auto starts = Starts( { { "Main", "" }, { "CaveA", "cave" }, { "CaveB", "cave" } } );
    EXPECT_FALSE( ChoosePlayerStart( starts, "door" ) );
    const auto two = ChoosePlayerStart( starts, "cave" );
    ASSERT_FALSE( two );
    EXPECT_NE( two.GetError().find( "'CaveA'" ), std::string::npos ) << two.GetError();
    EXPECT_NE( two.GetError().find( "'CaveB'" ), std::string::npos ) << two.GetError();
}

TEST( ViewTargetRule, ThePawnsCameraComesFirst )
{
    const std::vector<std::string> autoCams{ "SceneCam" };
    const auto                     r = ChooseViewTarget(
         { .Level = "L", .PawnSpawned = true, .PawnHasCamera = true, .AutoActivateCameras = autoCams } );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue().Kind, ViewTargetKind::PawnCamera );
}

TEST( ViewTargetRule, ThenTheOneAutoActivateCamera )
{
    const std::vector<std::string> autoCams{ "SceneCam" };
    const auto r = ChooseViewTarget( { .Level = "L", .PawnSpawned = true, .AutoActivateCameras = autoCams } );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue().Kind, ViewTargetKind::AutoActivateCamera );
    EXPECT_EQ( r.GetValue().AutoIndex, 0u );
}

TEST( ViewTargetRule, TwoAutoActivateCamerasAreAnErrorNamingBoth )
{
    const std::vector<std::string> autoCams{ "CamA", "CamB" };
    const auto r = ChooseViewTarget( { .Level = "L", .AutoActivateCameras = autoCams, .PlayFromHere = true } );
    ASSERT_FALSE( r );
    EXPECT_NE( r.GetError().find( "'CamA'" ), std::string::npos ) << r.GetError();
    EXPECT_NE( r.GetError().find( "'CamB'" ), std::string::npos ) << r.GetError();
}

TEST( ViewTargetRule, TheEditorCameraOnlyInPlayFromHere )
{
    const auto here = ChooseViewTarget( { .Level = "L", .PlayFromHere = true } );
    ASSERT_TRUE( here ) << here.GetError();
    EXPECT_EQ( here.GetValue().Kind, ViewTargetKind::EditorCamera );

    const auto play = ChooseViewTarget( { .Level = "L", .PawnSpawned = true } );
    ASSERT_FALSE( play );
    EXPECT_NE( play.GetError().find( "Play from Here" ), std::string::npos ) << play.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

TEST( PlayRequestArgs, NoFlagIsTheDefaultStart )
{
    const std::vector<std::string> args{ "--scene", "Scenes/Starter.desce" };
    const auto                     request = Desert::Core::PlayRequestFromArgs( args );
    ASSERT_TRUE( request ) << request.GetError();
    EXPECT_TRUE( request.GetValue().PlayerStartTag.empty() );
}

TEST( PlayRequestArgs, TheFlagNamesTheTag )
{
    const std::vector<std::string> args{ "--scene", "Scenes/Arena.desce", "--player-start", "Red" };
    const auto                     request = Desert::Core::PlayRequestFromArgs( args );
    ASSERT_TRUE( request ) << request.GetError();
    EXPECT_EQ( request.GetValue().PlayerStartTag, "Red" );
    EXPECT_FALSE( request.GetValue().SpawnAt.has_value() );
}

TEST( PlayRequestArgs, AFlagWithoutATagIsRefused )
{
    for ( const std::vector<std::string>& args :
          { std::vector<std::string>{ "--player-start" }, std::vector<std::string>{ "--player-start", "" },
            std::vector<std::string>{ "--player-start", "--scene", "S.desce" },
            std::vector<std::string>{ "--player-start", "A", "--player-start", "B" } } )
    {
        const auto request = Desert::Core::PlayRequestFromArgs( args );
        ASSERT_FALSE( request ) << "accepted " << args.size() << " args";
        EXPECT_NE( request.GetError().find( "--player-start" ), std::string::npos ) << request.GetError();
    }
}
