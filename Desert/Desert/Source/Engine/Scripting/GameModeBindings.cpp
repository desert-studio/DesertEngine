#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/GameMode.hpp>
#include <Engine/Core/Scene.hpp>

#include <tuple>

namespace Desert::Scripting
{
    // gameMode - the played world's game rules (Core/GameMode.hpp, UE AGameModeBase) for scripts:
    //   gameMode.kill( entity )  -> true, or false + the reason: the player's pawn dies; RestartPlayer follows
    //                               after the level's Respawn Delay. Hooks: OnPawnDied(pawn) and
    //                               OnPlayerRestarted(pawn) on every script (ScriptEngine::DeliverGameModeEvents).
    //   gameMode.pawn()          -> the possessed pawn, or nil (dead, or a level without one)
    //   gameMode.controller()    -> the level's Player Controller entity, or nil
    //   gameMode.respawnIn()     -> seconds until the restart, or nil when none is pending
    void RegisterGameModeBindings( ScriptEngine::Impl& implRef )
    {
        ScriptEngine::Impl* impl     = &implRef;
        sol::table          gameMode = implRef.Lua.create_named_table( "gameMode" );
        gameMode.set_function( "kill",
                               [impl]( const ScriptEntity& pawn ) -> std::tuple<bool, sol::optional<std::string>>
                               {
                                   if ( impl->Scene == nullptr )
                                       return { false, std::string( "no world is being played" ) };
                                   const auto killed =
                                        impl->Scene->GetGameMode().Kill( *impl->Scene, pawn.handle );
                                   if ( !killed )
                                       return { false, killed.GetError() };
                                   return { true, sol::nullopt };
                               } );
        gameMode.set_function( "pawn",
                               [impl]() -> sol::object
                               {
                                   if ( impl->Scene == nullptr || impl->Scene->GetPlayerPawn() == entt::null )
                                       return sol::make_object( impl->Lua, sol::lua_nil );
                                   return sol::make_object( impl->Lua,
                                                            impl->MakeEntity( impl->Scene->GetPlayerPawn() ) );
                               } );
        gameMode.set_function(
             "controller",
             [impl]() -> sol::object
             {
                 if ( impl->Scene == nullptr || impl->Scene->GetPlayerController() == entt::null )
                     return sol::make_object( impl->Lua, sol::lua_nil );
                 return sol::make_object( impl->Lua, impl->MakeEntity( impl->Scene->GetPlayerController() ) );
             } );
        gameMode.set_function( "respawnIn",
                               [impl]() -> sol::optional<float>
                               {
                                   if ( impl->Scene == nullptr )
                                       return sol::nullopt;
                                   const auto remaining = impl->Scene->GetGameMode().RespawnRemaining();
                                   if ( !remaining )
                                       return sol::nullopt;
                                   return *remaining;
                               } );
    }
} // namespace Desert::Scripting
