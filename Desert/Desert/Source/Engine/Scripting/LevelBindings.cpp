#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/LevelTravel.hpp>

namespace Desert::Scripting
{
    namespace
    {
        // level.open([name]) -> true | false, why. Queues the travel (Core::OpenLevel); no name = the default
        // level.
        int Open( lua_State* L )
        {
            const auto opened = Core::OpenLevel( luaL_optstring( L, 1, "" ) );
            lua_pushboolean( L, opened ? 1 : 0 );
            if ( opened )
                return 1;
            lua_pushstring( L, opened.GetError().c_str() );
            return 2;
        }
    } // namespace

    void RegisterLevelBindings( lua_State* L )
    {
        constexpr luaL_Reg kLevel[] = { { "open", &Open }, { nullptr, nullptr } };
        luaL_register( L, "level", kLevel );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
