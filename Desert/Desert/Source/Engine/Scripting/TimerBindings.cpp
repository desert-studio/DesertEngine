#include "Internal/ScriptRuntime.hpp"

#include <algorithm>

namespace Desert::Scripting
{
    namespace
    {
        // Timer.after(seconds, fn): runs fn once after `seconds` of game time (Play only — ticked by
        // ScriptSystem). Owned by the (entity, slot) that scheduled it: a reload / destroy cancels it. A callback
        // may call Timer.after again to re-arm itself (a repeating timer is just recursion).
        int After( lua_State* L )
        {
            const auto seconds = static_cast<float>( luaL_checknumber( L, 1 ) );
            luaL_checktype( L, 2, LUA_TFUNCTION );
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            host.Timers.push_back( { host.CurrentOwner, std::max( seconds, 0.0f ), lua_ref( L, 2 ) } );
            return 0;
        }
    } // namespace

    void RegisterTimerBindings( lua_State* L )
    {
        constexpr luaL_Reg kTimer[] = { { "after", &After }, { nullptr, nullptr } };
        luaL_register( L, "Timer", kTimer );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
