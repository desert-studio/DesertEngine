#include "Internal/ScriptRuntime.hpp"

namespace Desert::Scripting
{
    namespace
    {
        // Every argument as text (tostring), tab-separated — what print() would show.
        std::string Message( lua_State* L )
        {
            std::string text;
            for ( int i = 1, n = lua_gettop( L ); i <= n; ++i )
            {
                std::size_t length = 0;
                const char* piece  = luaL_tolstring( L, i, &length );
                if ( i > 1 )
                    text += '\t';
                text.append( piece, length );
                lua_pop( L, 1 );
            }
            return text;
        }

        int Info( lua_State* L )
        {
            LOG_INFO( "[Lua] {}", Message( L ) );
            return 0;
        }
        int Warn( lua_State* L )
        {
            LOG_WARN( "[Lua] {}", Message( L ) );
            return 0;
        }
        int Error( lua_State* L )
        {
            LOG_ERROR( "[Lua] {}", Message( L ) );
            return 0;
        }
    } // namespace

    // Logging into the engine Logs panel: log(), Log.info/warn/error (the panel filters by level).
    void RegisterLogBindings( lua_State* L )
    {
        lua_pushcfunction( L, &Info, "log" );
        lua_setglobal( L, "log" );
        constexpr luaL_Reg kLog[] = { { "info", &Info }, { "warn", &Warn }, { "error", &Error }, { nullptr, nullptr } };
        luaL_register( L, "Log", kLog );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
