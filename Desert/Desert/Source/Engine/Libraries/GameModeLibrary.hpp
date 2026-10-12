#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/ECS/EntityValue.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

namespace Desert::Libraries
{
    /// The played world's game rules (Core/GameMode.hpp, UE AGameModeBase) for gameplay code. The hooks it causes
    /// are OnPawnDied(pawn) and OnPlayerRestarted(pawn) on every script (ScriptEngine::DeliverGameModeEvents).
    struct GameModeLibrary
    {
        REFLECT( ScriptName( "gameMode" ) )

        FUNCTION( ScriptCallable, ScriptName( "kill" ),
                  Tooltip( "The player's pawn dies; RestartPlayer follows after the level's Respawn Delay. Ok, or "
                           "false and the reason." ) )
        static Common::BoolResultStr Kill( ECS::Entity pawn );

        FUNCTION( ScriptCallable, ScriptName( "pawn" ),
                  Tooltip( "The possessed pawn, or nil (dead, or a level without one)." ) )
        static ECS::Entity Pawn();

        FUNCTION( ScriptCallable, ScriptName( "controller" ),
                  Tooltip( "The level's Player Controller entity, or nil." ) )
        static ECS::Entity Controller();

        FUNCTION( ScriptCallable, ScriptName( "respawnIn" ),
                  Tooltip( "Seconds until the restart, or nil when none is pending." ) )
        static Reflection::Value RespawnIn();
    };
} // namespace Desert::Libraries
