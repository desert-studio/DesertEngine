#include "GameMode.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <utility>

namespace Desert::Core
{
    namespace
    {
        std::string NameOf( const entt::registry& registry, entt::entity entity )
        {
            const auto* tag = registry.try_get<ECS::TagComponent>( entity );
            return tag != nullptr ? tag->Tag : std::string( "<unnamed>" );
        }
    } // namespace

    void GameMode::Begin( std::string playerStartTag )
    {
        Reset();
        m_PlayerStartTag = std::move( playerStartTag );
    }

    void GameMode::Reset()
    {
        m_PlayerStartTag.clear();
        m_RespawnIn.reset();
        m_DeathDelivered = false;
        m_DeadPawn       = entt::null;
        m_Events.clear();
    }

    Common::BoolResultStr GameMode::Kill( Scene& scene, const entt::entity pawn )
    {
        auto& reg = scene.GetRegistry();
        if ( pawn == entt::null || !reg.valid( pawn ) )
            return Common::MakeError( "level '" + scene.GetSceneName() + "': Kill was given no living entity" );
        if ( pawn != scene.GetPlayerPawn() )
            return Common::MakeError( "level '" + scene.GetSceneName() + "': '" + NameOf( reg, pawn ) +
                                      "' is not the player's pawn; only the player's pawn dies and restarts" );
        if ( m_RespawnIn.has_value() )
            return Common::MakeError( "level '" + scene.GetSceneName() + "': the player is already dead" );

        // UE: the controller unpossesses the pawn. The body stays in the world (and in view) until the restart.
        scene.SetPlayerPawn( entt::null );
        m_DeadPawn       = pawn;
        m_RespawnIn      = scene.GetSettings().RespawnDelay;
        m_DeathDelivered = false;
        m_Events.push_back( { GameModeEventKind::PawnDied, pawn } );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr GameMode::Tick( Scene& scene, const float deltaSeconds, const PawnSpawner& spawn )
    {
        if ( !m_RespawnIn.has_value() || !m_DeathDelivered )
            return BOOLSUCCESS;
        *m_RespawnIn -= deltaSeconds;
        if ( *m_RespawnIn > 0.0f )
            return BOOLSUCCESS;
        return RestartPlayer( scene, spawn );
    }

    Common::BoolResultStr GameMode::RestartPlayer( Scene& scene, const PawnSpawner& spawn )
    {
        const std::string level = "level '" + scene.GetSceneName() + "'";
        if ( scene.GetSettings().DefaultPawn == 0 )
            return Common::MakeError( level + " names no Default Pawn: there is no player to restart" );

        m_RespawnIn.reset();
        m_DeathDelivered = false;
        auto& reg        = scene.GetRegistry();
        if ( m_DeadPawn != entt::null && reg.valid( m_DeadPawn ) )
            scene.DestroyEntity( ECS::Entity( m_DeadPawn, reg ) );
        m_DeadPawn = entt::null;

        const auto at = PlayerStartTransform( scene, m_PlayerStartTag );
        if ( !at )
            return Common::MakeError( level + ": the player cannot restart: " + at.GetError() );
        const auto pawn = spawn( scene, at.GetValue() );
        if ( !pawn )
            return Common::MakeError( level + ": the Default Pawn could not be respawned: " + pawn.GetError() );

        scene.SetPlayerPawn( pawn.GetValue() );
        if ( const auto view = scene.ResolveViewTarget(); !view )
            return Common::MakeError( level + ": the restarted player has no view: " + view.GetError() );
        m_Events.push_back( { GameModeEventKind::PlayerRestarted, pawn.GetValue() } );
        return BOOLSUCCESS;
    }

    std::vector<GameModeEvent> GameMode::TakeEvents()
    {
        for ( const auto& event : m_Events )
            if ( event.Kind == GameModeEventKind::PawnDied )
                m_DeathDelivered = true;
        return std::exchange( m_Events, {} );
    }
} // namespace Desert::Core
