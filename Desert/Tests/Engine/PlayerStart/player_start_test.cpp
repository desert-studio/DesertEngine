// SPAWN1: the two rules Play is made of — which PlayerStart the pawn stands on, and which camera the
// player looks through (UE: GameModeBase::FindPlayerStart and APlayerCameraManager's view target).
#include <Engine/Core/PawnBodyRules.hpp>
#include <Engine/Core/PlayerStart.hpp>

#include <Common/Json/Document.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
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
    const auto here = ChooseViewTarget( { .Level = "L", .AutoActivateCameras = {}, .PlayFromHere = true } );
    ASSERT_TRUE( here ) << here.GetError();
    EXPECT_EQ( here.GetValue().Kind, ViewTargetKind::EditorCamera );

    const auto play = ChooseViewTarget( { .Level = "L", .PawnSpawned = true, .AutoActivateCameras = {} } );
    ASSERT_FALSE( play );
    EXPECT_NE( play.GetError().find( "Play from Here" ), std::string::npos ) << play.GetError();
}

// Owner 2026-09-28: "the camera is both the spawn and the view? that is what we must separate". A level
// with no Default Pawn spawns nothing, and the view rule alone picks what the player looks through.
TEST( ViewTargetRule, NoPawnStillLooksThroughTheAutoActivateCamera )
{
    const std::vector<std::string> autoCams{ "SceneCam" };
    const auto r = ChooseViewTarget( { .Level = "L", .PawnSpawned = false, .AutoActivateCameras = autoCams } );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue().Kind, ViewTargetKind::AutoActivateCamera );

    const auto none = ChooseViewTarget( { .Level = "L", .PawnSpawned = false, .AutoActivateCameras = {} } );
    EXPECT_FALSE( none ) << "no pawn and no camera must refuse Play, not fall back to the editor camera";
}

// SPAWN is PlayerStart (or Play from Here's explicit editor viewpoint, UE's exception) and nothing else: the
// code that places the pawn never reads a camera, so no camera can move or place the player.
TEST( SpawnIsNotACamera, SpawnDefaultPawnNeverReadsACameraComponent )
{
    const std::filesystem::path file =
         Desert::TestSupport::RepositoryRoot() / "Desert/Desert/Source/Engine/Core/PlayerStart.cpp";
    const std::ifstream in( file );
    ASSERT_TRUE( in ) << "could not open " << file;
    std::stringstream text;
    text << in.rdbuf();
    const std::string src = text.str();
    ASSERT_NE( src.find( "SpawnDefaultPawn" ), std::string::npos );
    EXPECT_EQ( src.find( "CameraComponent" ), std::string::npos )
         << "PlayerStart.cpp reads a CameraComponent: a camera is the VIEW, never the spawn";
    EXPECT_NE( src.find( "PlayerStartComponent" ), std::string::npos );
}

namespace
{
    std::string ReadSource( const char* relative )
    {
        const std::filesystem::path file = Desert::TestSupport::RepositoryRoot() / relative;
        const std::ifstream         in( file );
        EXPECT_TRUE( in ) << "could not open " << file;
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

// WP24: the player's pawn is where the world must exist (UE: the player controller is a streaming source).
TEST( PawnIsAStreamingSource, SpawnDefaultPawnGivesThePawnASourceUnlessItsPrefabHasOne )
{
    const std::string src   = ReadSource( "Desert/Desert/Source/Engine/Core/PlayerStart.cpp" );
    const auto        guard = src.find( "if ( !pawn.HasComponent<ECS::StreamingSourceComponent>() )" );
    ASSERT_NE( guard, std::string::npos ) << "a prefab's own source (range, priority, Enabled off) must be kept";
    const auto add = src.find( "pawn.AddComponent<ECS::StreamingSourceComponent>()", guard );
    ASSERT_NE( add, std::string::npos ) << "the spawned pawn is not made a streaming source";
    EXPECT_LT( add, src.find( "scene.SetPlayerPawn( pawn.GetHandle() )" ) )
         << "the pawn must be a source before Play hands it on";
}

// UI-PAWN2: the PlayerStart's capsule is the pawn Play WOULD spawn (UE: APlayerStart draws the DefaultPawnClass
// CDO's capsule), read from the level's Default Pawn prefab through the reflection of CharacterControllerData -
// never a default-constructed struct standing in for a pawn the level did not name.
TEST( PlayerStartCapsule, TheGizmoDrawsTheLevelsDefaultPawnNotTheStructsDefaults )
{
    const std::string engine = ReadSource( "Desert/Desert/Source/Engine/Core/PlayerStart.cpp" );
    const auto        fn     = engine.find( "DefaultPawnCapsule(" );
    ASSERT_NE( fn, std::string::npos ) << "PlayerStart.cpp no longer answers the Default Pawn's capsule";
    EXPECT_NE( engine.find( "scene.GetSettings().DefaultPawn", fn ), std::string::npos )
         << "the capsule must come from the level's Default Pawn";
    EXPECT_NE( engine.find( "PawnControllerBlock(", fn ), std::string::npos )
         << "the capsule must be the prefab's CharacterController block, nested prefabs included";
    EXPECT_NE( ReadSource( "Desert/Desert/Source/Engine/Core/PlayerStartRules.cpp" )
                    .find( "Components.get( \"CharacterController\" )" ),
               std::string::npos )
         << "PawnControllerBlock must read the records' CharacterController block";
    EXPECT_NE( engine.find( "DeserializeReflected(", fn ), std::string::npos )
         << "the block must be read by CharacterControllerData's reflection, its one home";

    const std::string gizmo = ReadSource( "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp" );
    EXPECT_NE( gizmo.find( "::Desert::Core::DefaultPawnCapsule(" ), std::string::npos )
         << "the PlayerStart gizmo does not ask the level for its pawn";
    EXPECT_EQ( gizmo.find( "CharacterControllerData pawn{}" ), std::string::npos )
         << "the PlayerStart gizmo draws the struct's defaults instead of the level's pawn";
    EXPECT_NE( gizmo.find( "role  = GizmoIcon::PlayerStart;" ), std::string::npos )
         << "a PlayerStart must wear its own icon (UE S_Player), not the generic spawn-point pin";
}

// UI-FIX2b: the pawn's body is the CDO's (UE), nested prefabs included: a pawn prefab that nests the prefab
// holding its CharacterController (PrefabPath) drew no capsule.
namespace
{
    using Desert::Assets::EntityData;

    Common::Json::Value Controller( double radius, std::optional<double> height = {} )
    {
        Common::Json::Object object;
        object["Radius"] = Common::Json::Value( radius );
        if ( height )
            object["Height"] = Common::Json::Value( *height );
        return { object };
    }

    std::string FieldOf( const Common::Json::Value& block, const std::string& field )
    {
        const auto value = Common::Json::Root( block ).Find( field );
        return value ? Common::Json::Write( value->Raw() ) : "<absent>";
    }

    struct Library
    {
        std::vector<EntityData> Body; // "Body.deprefab": its root holds the controller
        NestedPrefabRecords     Nested =
             [this]( const std::string& path ) -> Common::ResultStr<const std::vector<EntityData>*>
        {
            const std::vector<EntityData>* body = &Body;
            if ( path == "Body.deprefab" )
                return Common::MakeSuccess( body );
            return Common::MakeFormattedError<const std::vector<EntityData>*>( "'{}' is not a prefab", path );
        };
    };
} // namespace

TEST( PawnBody, TheControllerOfANestedPrefabIsThePawns )
{
    Library lib;
    lib.Body.emplace_back().id                        = Common::UUID( 2 );
    lib.Body.back().Components["CharacterController"] = Controller( 80.0, 300.0 );

    std::vector<EntityData> pawn( 2 );
    pawn[0].id         = Common::UUID( 1 ); // the root: a mesh, no controller
    pawn[1].id         = Common::UUID( 3 );
    pawn[1].PrefabPath = "Body.deprefab";

    const auto block = PawnControllerBlock( pawn, lib.Nested );
    ASSERT_TRUE( block ) << block.GetError();
    const auto& found = block.GetValue();
    if ( !found.has_value() )
        FAIL() << "the nested prefab's controller was not found";
    EXPECT_EQ( FieldOf( *found, "Radius" ), FieldOf( Controller( 80.0 ), "Radius" ) );
}

TEST( PawnBody, AnOverrideOnTheNestingRecordIsMergedOntoTheNestedBlock )
{
    Library lib;
    lib.Body.emplace_back().id                        = Common::UUID( 2 );
    lib.Body.back().Components["CharacterController"] = Controller( 80.0, 300.0 );

    std::vector<EntityData> pawn( 1 );
    pawn[0].id         = Common::UUID( 3 );
    pawn[0].PrefabPath = "Body.deprefab";
    Desert::Assets::PrefabOverrideData over;
    over.Path                              = { Common::UUID( 2 ) };
    over.Components["CharacterController"] = Controller( 45.0 );
    pawn[0].PrefabOverrides                = std::vector{ over };

    const auto block = PawnControllerBlock( pawn, lib.Nested );
    ASSERT_TRUE( block ) << block.GetError();
    const auto& merged = block.GetValue();
    if ( !merged.has_value() )
        FAIL() << "the nested block was not found";
    const Common::Json::Value& mergedBlock = *merged;
    EXPECT_EQ( FieldOf( mergedBlock, "Radius" ), FieldOf( Controller( 45.0 ), "Radius" ) )
         << "the instance's override was not applied";
    EXPECT_EQ( FieldOf( mergedBlock, "Height" ), FieldOf( Controller( 0.0, 300.0 ), "Height" ) )
         << "the merge reset a field it did not name";
}

TEST( PawnBody, AMissingOrSelfNestedPrefabIsAnErrorNamingThePath )
{
    Library                 lib;
    std::vector<EntityData> pawn( 1 );
    pawn[0].PrefabPath = "Gone.deprefab";
    const auto missing = PawnControllerBlock( pawn, lib.Nested );
    ASSERT_FALSE( missing );
    EXPECT_NE( missing.GetError().find( "Gone.deprefab" ), std::string::npos );

    lib.Body.emplace_back().PrefabPath = "Body.deprefab";
    pawn[0].PrefabPath                 = "Body.deprefab";
    const auto cycle                   = PawnControllerBlock( pawn, lib.Nested );
    ASSERT_FALSE( cycle );
    EXPECT_NE( cycle.GetError().find( "nests itself" ), std::string::npos );
}

TEST( PawnBody, NoControllerAnywhereIsASpectatorNotAnError )
{
    const Library                 lib;
    const std::vector<EntityData> pawn( 1 );
    const auto                    block = PawnControllerBlock( pawn, lib.Nested );
    ASSERT_TRUE( block ) << block.GetError();
    EXPECT_FALSE( block.GetValue().has_value() );
}

// WP24: in Play the residency follows the sources, never the view.
TEST( PawnIsAStreamingSource, NoCameraDrivesResidencyInPlay )
{
    const std::string src = ReadSource( "Desert/Desert/Source/Engine/Core/WorldStreamer.cpp" );
    EXPECT_EQ( src.find( "GetActiveCamera" ), std::string::npos )
         << "WorldStreamer reads the active camera: the view would stream the world again";
    EXPECT_EQ( src.find( "CameraComponent" ), std::string::npos );
    EXPECT_NE( src.find( "registry.view<ECS::StreamingSourceComponent>()" ), std::string::npos );
    EXPECT_NE( src.find( "if ( !data.Enabled )" ), std::string::npos ) << "a disabled source must not stream";
    EXPECT_NE( src.find( "Play has no streaming source" ), std::string::npos )
         << "a Play with no source must say so by name, not fall back";
}

// WP24: both the editor and the game start streaming only after BeginPlay spawned the pawn it streams around.
TEST( PawnIsAStreamingSource, StreamingBeginsAfterThePawnIsSpawned )
{
    for ( const char* file : { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" } )
    {
        const std::string src   = ReadSource( file );
        std::size_t       at    = 0;
        int               pairs = 0;
        while ( ( at = src.find( "BeginPlay( *", at ) ) != std::string::npos )
        {
            const auto streamer = src.find( "WorldStreamer::Begin", at );
            ASSERT_NE( streamer, std::string::npos ) << file;
            const auto nextPlay = src.find( "BeginPlay( *", at + 1 );
            EXPECT_TRUE( nextPlay == std::string::npos || streamer < nextPlay )
                 << file << ": a BeginPlay is not followed by its streamer's begin";
            ++pairs;
            at = streamer;
        }
        EXPECT_GE( pairs, 1 ) << file;
        const auto firstStreamer = src.find( "WorldStreamer::Begin" );
        EXPECT_GT( firstStreamer, src.find( "BeginPlay( *" ) ) << file << ": streaming begins before Play";
    }
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
