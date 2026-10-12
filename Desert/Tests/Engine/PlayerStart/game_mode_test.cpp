// GP3: the game rules of a played world (UE AGameModeBase) — which start the player restarts at, the pawn made
// there, possession (view + input), death and the restart after the level's delay, and a level without a pawn.
// Device-free: the pawn is built by a spawner standing in for the prefab (Core::SpawnPawnPrefabAt in the hosts),
// and time is the deltas the test passes.
#include <Engine/Core/GameMode.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert;
using namespace Desert::Core;

namespace
{
    entt::entity Start( Scene& scene, const char* name, const char* tag, glm::vec3 at )
    {
        ECS::Entity& e                                        = scene.CreateEntityWithUUID( Common::UUID(), name );
        e.GetComponent<ECS::TransformComponent>().Translation = at;
        e.AddComponent<ECS::PlayerStartComponent>().Data.Tag  = tag;
        return e.GetHandle();
    }

    // The "prefab": a body with a controller and its own camera, placed at the start's translation.
    PawnSpawner PawnPrefab( int& spawned )
    {
        return [&spawned]( Scene& scene, const glm::mat4& at ) -> Common::ResultStr<entt::entity>
        {
            ++spawned;
            ECS::Entity& e = scene.CreateEntityWithUUID( Common::UUID(), "Pawn" );
            e.GetComponent<ECS::TransformComponent>().Translation = glm::vec3( at[3] );
            e.AddComponent<ECS::CharacterControllerComponent>();
            e.AddComponent<ECS::CameraComponent>();
            return Common::MakeSuccess( e.GetHandle() );
        };
    }

    void NamePawn( Scene& scene, float delay )
    {
        scene.GetSettings().DefaultPawn  = Assets::AssetHandle( 7 ); // any set handle: the spawner is the prefab
        scene.GetSettings().RespawnDelay = delay;
    }

    std::string ReadSource( const char* relative )
    {
        const std::ifstream in( Desert::TestSupport::RepositoryRoot() / relative );
        EXPECT_TRUE( in ) << "could not open " << relative;
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

TEST( GameMode, StartSelectionIsTheLevelRuleOverTheScenesPlayerStarts )
{
    Scene scene( "GP3_Starts", nullptr );
    Start( scene, "Main", "", { 100.0f, 0.0f, 0.0f } );
    Start( scene, "Door", "Door", { 0.0f, 0.0f, 500.0f } );

    const auto untagged = PlayerStartTransform( scene, "" );
    ASSERT_TRUE( untagged ) << untagged.GetError();
    EXPECT_EQ( glm::vec3( untagged.GetValue()[3] ), glm::vec3( 100.0f, 0.0f, 0.0f ) );
    const auto door = PlayerStartTransform( scene, "Door" );
    ASSERT_TRUE( door ) << door.GetError();
    EXPECT_EQ( glm::vec3( door.GetValue()[3] ), glm::vec3( 0.0f, 0.0f, 500.0f ) );
    EXPECT_FALSE( PlayerStartTransform( scene, "Window" ) ) << "a tag no start carries must be refused";
}

TEST( GameMode, RestartSpawnsThePrefabAtTheStartAndPossessesItsCamera )
{
    Scene scene( "GP3_Restart", nullptr );
    NamePawn( scene, 2.0f );
    Start( scene, "Main", "", { 300.0f, 50.0f, -20.0f } );
    int spawned = 0;

    const auto restarted = scene.GetGameMode().RestartPlayer( scene, PawnPrefab( spawned ) );
    ASSERT_TRUE( restarted ) << restarted.GetError();
    const entt::entity pawn = scene.GetPlayerPawn();
    ASSERT_TRUE( pawn != entt::null );
    auto& reg = scene.GetRegistry();
    EXPECT_EQ( reg.get<ECS::TransformComponent>( pawn ).Translation, glm::vec3( 300.0f, 50.0f, -20.0f ) );
    EXPECT_TRUE( ( reg.has<ECS::CharacterControllerComponent, ECS::CameraComponent>( pawn ) ) );
    const auto view = scene.ResolveViewTarget();
    ASSERT_TRUE( view ) << view.GetError();
    EXPECT_EQ( view.GetValue(), pawn ) << "possession must move the view to the pawn's camera";
    const auto events = scene.GetGameMode().TakeEvents();
    ASSERT_EQ( events.size(), 1u );
    EXPECT_EQ( events[0].Kind, GameModeEventKind::PlayerRestarted );
    EXPECT_EQ( events[0].Pawn, pawn ) << "the restart must hand the new pawn to input possession and the hooks";
}

TEST( GameMode, KillUnpossessesAndRestartsAfterTheDelayAtTheStart )
{
    Scene scene( "GP3_Respawn", nullptr );
    NamePawn( scene, 2.0f );
    Start( scene, "Main", "", { 0.0f, 0.0f, 900.0f } );
    int   spawned = 0;
    auto  spawn   = PawnPrefab( spawned );
    auto& mode    = scene.GetGameMode();
    auto& reg     = scene.GetRegistry();
    mode.Begin( "" );
    ASSERT_TRUE( mode.RestartPlayer( scene, spawn ) );
    (void)mode.TakeEvents();
    const entt::entity first                              = scene.GetPlayerPawn();
    reg.get<ECS::TransformComponent>( first ).Translation = glm::vec3( 5000.0f, 0.0f, 0.0f ); // walked away

    ASSERT_TRUE( mode.Kill( scene, first ) );
    EXPECT_TRUE( scene.GetPlayerPawn() == entt::null ) << "a dead pawn is no longer possessed";
    EXPECT_TRUE( reg.valid( first ) )
         << "the body stays until the restart (the hook sees it, the view stays on it)";

    ASSERT_TRUE( mode.Tick( scene, 10.0f, spawn ) );
    EXPECT_EQ( spawned, 1 ) << "the clock must not run before OnPawnDied was delivered";
    const auto died = mode.TakeEvents();
    ASSERT_EQ( died.size(), 1u );
    EXPECT_EQ( died[0].Kind, GameModeEventKind::PawnDied );
    EXPECT_EQ( died[0].Pawn, first );

    ASSERT_TRUE( mode.Tick( scene, 1.5f, spawn ) );
    EXPECT_EQ( spawned, 1 ) << "restarted before the 2 s Respawn Delay ran out";
    ASSERT_TRUE( mode.RespawnRemaining().has_value() );
    EXPECT_NEAR( *mode.RespawnRemaining(), 0.5f, 1e-5f );

    ASSERT_TRUE( mode.Tick( scene, 0.6f, spawn ) );
    EXPECT_EQ( spawned, 2 );
    EXPECT_FALSE( reg.valid( first ) ) << "the dead body must be destroyed by the restart";
    const entt::entity second = scene.GetPlayerPawn();
    ASSERT_TRUE( second != entt::null );
    EXPECT_EQ( reg.get<ECS::TransformComponent>( second ).Translation, glm::vec3( 0.0f, 0.0f, 900.0f ) );
    EXPECT_FALSE( mode.RespawnRemaining().has_value() );
    const auto back = mode.TakeEvents();
    ASSERT_EQ( back.size(), 1u );
    EXPECT_EQ( back[0].Kind, GameModeEventKind::PlayerRestarted );
    EXPECT_EQ( back[0].Pawn, second );
}

TEST( GameMode, OnlyThePlayersPawnDiesAndOnlyOnce )
{
    Scene scene( "GP3_Kill", nullptr );
    NamePawn( scene, 1.0f );
    const entt::entity bystander = Start( scene, "Main", "", { 0.0f, 0.0f, 0.0f } );
    int                spawned   = 0;
    ASSERT_TRUE( scene.GetGameMode().RestartPlayer( scene, PawnPrefab( spawned ) ) );
    const auto refused = scene.GetGameMode().Kill( scene, bystander );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "Main" ), std::string::npos ) << "the refusal must name the entity";
    const entt::entity pawn = scene.GetPlayerPawn();
    ASSERT_TRUE( scene.GetGameMode().Kill( scene, pawn ) );
    EXPECT_FALSE( scene.GetGameMode().Kill( scene, pawn ) ) << "a dead player cannot die again";
}

// A level without a Default Pawn keeps today's Play: no pawn, the view is the level's AutoActivateForPlayer
// camera, and nobody can die or restart.
TEST( GameMode, ALevelWithoutAPawnPlaysThroughItsCameraAndNeverRestarts )
{
    Scene        scene( "GP3_CameraOnly", nullptr );
    ECS::Entity& camera = scene.CreateEntityWithUUID( Common::UUID(), "Shot" );
    camera.AddComponent<ECS::CameraComponent>().Data.AutoActivateForPlayer = true;
    const entt::entity cameraHandle                                        = camera.GetHandle();
    int                spawned                                             = 0;

    EXPECT_FALSE( scene.GetGameMode().RestartPlayer( scene, PawnPrefab( spawned ) ) );
    EXPECT_EQ( spawned, 0 );
    EXPECT_FALSE( scene.GetGameMode().Kill( scene, cameraHandle ) );
    EXPECT_TRUE( scene.GetPlayerPawn() == entt::null );
    const auto view = scene.ResolveViewTarget();
    ASSERT_TRUE( view ) << view.GetError();
    EXPECT_EQ( view.GetValue(), cameraHandle );
}

// The wiring the device-free tests above cannot run: the hosts tick the rules, the restart uses the prefab
// spawn, and the events move the player's input contexts and reach the Lua hooks before the scripts run.
TEST( GameMode, HostsTickItAndItsEventsMoveInputAndReachLua )
{
    for ( const char* host :
          { "Runtime/Source/RuntimeLayer.cpp", "Editor/Source/Editor/LevelEditor/SceneWorkspace.cpp" } )
    {
        const std::string src     = ReadSource( host );
        const auto        scripts = src.find( "AddSystem<ECS::ScriptSystem>" );
        const auto        mode    = src.find( "AddSystem<ECS::GameModeSystem>" );
        ASSERT_NE( mode, std::string::npos ) << host << " does not tick the GameMode";
        EXPECT_LT( scripts, mode ) << host << ": the GameMode must tick after the scripts that kill";
    }
    EXPECT_NE( ReadSource( "Desert/Desert/Source/Engine/ECS/System/GameModeSystem.hpp" )
                    .find( "Core::SpawnPawnPrefabAt(" ),
               std::string::npos );
    const std::string engine = ReadSource( "Desert/Desert/Source/Engine/Scripting/ScriptEngine.cpp" );
    EXPECT_NE( engine.find( "PlayerInput.UnpossessPawn()" ), std::string::npos );
    EXPECT_NE( engine.find( "PlayerInput.PossessPawn( registry" ), std::string::npos );
    EXPECT_NE( engine.find( "\"OnPawnDied\" : \"OnPlayerRestarted\"" ), std::string::npos );
    const std::string system  = ReadSource( "Desert/Desert/Source/Engine/ECS/System/ScriptSystem.hpp" );
    const auto        tick    = system.find( "m_Engine.TickPlayerInput(" );
    const auto        deliver = system.find( "m_Engine.DeliverGameModeEvents(" );
    ASSERT_NE( deliver, std::string::npos );
    EXPECT_LT( tick, deliver );
    EXPECT_LT( deliver, system.find( "m_Engine.CallUpdate(" ) ) << "hooks must run before this frame's OnUpdate";
}
