#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/Input.hpp>

namespace Desert::Scripting
{
    namespace
    {
        int IsKeyDown( lua_State* L )
        {
            const auto key = KeyFromName( luaL_checkstring( L, 1 ) );
            lua_pushboolean( L, key.has_value() && Input::Keyboard::IsKeyPressed( *key ) );
            return 1;
        }

        // Fires once, on the frame the key goes down (edge). Needs NewInputFrame() once per frame.
        int WasPressed( lua_State* L )
        {
            const auto                key  = KeyFromName( luaL_checkstring( L, 1 ) );
            const ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            bool                      edge = false;
            if ( key.has_value() )
            {
                auto it = host.KeyEdge.find( static_cast<int>( *key ) );
                edge    = it != host.KeyEdge.end() && it->second;
            }
            lua_pushboolean( L, edge );
            return 1;
        }

        int MouseDelta( lua_State* L )
        {
            const ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            lua_pushnumber( L, host.MouseDx );
            lua_pushnumber( L, host.MouseDy );
            return 2;
        }

        int LockCursor( lua_State* L ) // capture (gameplay look)
        {
            ScriptEngine::Impl::Of( L ).CursorLockRequest = true;
            return 0;
        }

        int ShowCursor( lua_State* L ) // free (click UI)
        {
            ScriptEngine::Impl::Of( L ).CursorLockRequest = false;
            return 0;
        }

        // Raw mouse-button held state ("left"/"right"/"middle"). Edge-detect in-script for a one-shot.
        int IsMouseDown( lua_State* L )
        {
            const std::string   name   = luaL_optstring( L, 1, "left" );
            Common::MouseButton button = Common::MouseButton::Left;
            if ( name == "right" )
                button = Common::MouseButton::Right;
            else if ( name == "middle" )
                button = Common::MouseButton::Middle;
            lua_pushboolean( L, Input::Mouse::Get().IsMouseButtonPressed( button ) );
            return 1;
        }
    } // namespace

    // The `Input` table: keyboard state + per-frame mouse delta + cursor control.
    void RegisterInputBindings( lua_State* L )
    {
        constexpr luaL_Reg kInput[] = { { "isKeyDown", &IsKeyDown },   { "wasPressed", &WasPressed },
                                        { "mouseDelta", &MouseDelta }, { "lockCursor", &LockCursor },
                                        { "showCursor", &ShowCursor }, { "isMouseDown", &IsMouseDown },
                                        { nullptr, nullptr } };
        luaL_register( L, "Input", kInput );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
