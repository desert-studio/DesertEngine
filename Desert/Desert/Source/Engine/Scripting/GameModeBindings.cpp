#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/GameMode.hpp>
#include <Engine/Core/Scene.hpp>

namespace Desert::Scripting
{
    // gameMode - the played world's game rules (Core/GameMode.hpp, UE AGameModeBase) for scripts:
    //   gameMode.kill( entity )  -> true, or false + the reason: the player's pawn dies; RestartPlayer follows
    //                               after the level's Respawn Delay. Hooks: OnPawnDied(pawn) and
    //                               OnPlayerRestarted(pawn) on every script (ScriptEngine::DeliverGameModeEvents).
    //   gameMode.pawn()          -> the possessed pawn, or nil (dead, or a level without one)
    //   gameMode.controller()    -> the level's Player Controller entity, or nil
    //   gameMode.respawnIn()     -> seconds until the restart, or nil when none is pending
    namespace
    {
        void PushEntityOrNil( lua_State* L, ScriptEngine::Impl& host, entt::entity entity )
        {
            if ( host.Scene == nullptr || entity == entt::null )
            {
                lua_pushnil( L );
                return;
            }
            LuauBinder::PushEntity( L, host.Registry(), entity );
        }

        int Kill( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            if ( host.Scene == nullptr )
            {
                lua_pushboolean( L, 0 );
                lua_pushstring( L, "no world is being played" );
                return 2;
            }
            const entt::entity pawn   = host.CheckEntity( L, 1 );
            const auto         killed = host.Scene->GetGameMode().Kill( *host.Scene, pawn );
            lua_pushboolean( L, killed ? 1 : 0 );
            if ( killed )
                return 1;
            lua_pushstring( L, killed.GetError().c_str() );
            return 2;
        }

        int Pawn( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            PushEntityOrNil( L, host, host.Scene != nullptr ? host.Scene->GetPlayerPawn() : entt::null );
            return 1;
        }

        int Controller( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            PushEntityOrNil( L, host, host.Scene != nullptr ? host.Scene->GetPlayerController() : entt::null );
            return 1;
        }

        int RespawnIn( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            if ( host.Scene == nullptr )
            {
                lua_pushnil( L );
                return 1;
            }
            const auto remaining = host.Scene->GetGameMode().RespawnRemaining();
            if ( remaining )
                lua_pushnumber( L, *remaining );
            else
                lua_pushnil( L );
            return 1;
        }
    } // namespace

    void RegisterGameModeBindings( lua_State* L )
    {
        constexpr luaL_Reg kGameMode[] = { { "kill", &Kill },
                                           { "pawn", &Pawn },
                                           { "controller", &Controller },
                                           { "respawnIn", &RespawnIn },
                                           { nullptr, nullptr } };
        luaL_register( L, "gameMode", kGameMode );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
