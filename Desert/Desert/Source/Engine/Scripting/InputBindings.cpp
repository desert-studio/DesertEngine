#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/Input.hpp>
#include <Engine/Input/EnhancedInputSubsystem.hpp>

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

        // ---- Enhanced Input (GP1b): the local player's actions, by their file names (IA_Jump), as UE's
        // FInputActionValue / ETriggerEvent. An action no added context ever mapped is a Lua error naming it
        // (a typo must not read as "not pressed" forever).
        Common::Content::AssetGuid CheckAction( lua_State* L, const ScriptEngine::Impl& host )
        {
            const char* name = luaL_checkstring( L, 1 );
            const auto  guid = host.PlayerInput.ActionNamed( name );
            if ( !guid )
                luaL_error( L, "Input: no mapping context the player added maps an action named '%s'", name );
            return *guid;
        }

        int ActionValue( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            const glm::vec3     v    = host.PlayerInput.Subsystem().GetActionValue( CheckAction( L, host ) );
            lua_pushnumber( L, v.x );
            lua_pushnumber( L, v.y );
            lua_pushnumber( L, v.z );
            return 3;
        }

        template <bool Input::TriggerEvents::* Event>
        int ActionEvent( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            lua_pushboolean( L, host.PlayerInput.Subsystem().GetTriggerEvents( CheckAction( L, host ) ).*Event );
            return 1;
        }

        int ActionSeconds( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            lua_pushnumber( L, host.PlayerInput.Subsystem().GetTriggeredSeconds( CheckAction( L, host ) ) );
            return 1;
        }

        // UE AddMappingContext / RemoveMappingContext, the context by file name (IMC_Vehicle) or content path.
        int AddContext( lua_State* L )
        {
            ScriptEngine::Impl& host     = ScriptEngine::Impl::Of( L );
            const std::string   name     = luaL_checkstring( L, 1 );
            const int           priority = luaL_checkinteger( L, 2 );
            if ( host.Assets == nullptr )
                luaL_error( L, "Input.addContext: this world has no asset manager" );
            if ( auto added = host.PlayerInput.AddContext( *host.Assets, name, priority ); !added )
                luaL_error( L, "Input.addContext('%s'): %s", name.c_str(), added.GetError().c_str() );
            return 0;
        }

        int RemoveContext( lua_State* L )
        {
            lua_pushboolean( L, ScriptEngine::Impl::Of( L ).PlayerInput.RemoveContext( luaL_checkstring( L, 1 ) ) );
            return 1;
        }

        // UE MapPlayerKey: the player's own key for one mapping, saved to this user's input.json.
        int RebindKey( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            if ( auto rebound = host.PlayerInput.RebindKey( luaL_checkstring( L, 1 ), luaL_checkstring( L, 2 ),
                                                            luaL_checkstring( L, 3 ), luaL_checkstring( L, 4 ) );
                 !rebound )
                luaL_error( L, "Input.rebindKey: %s", rebound.GetError().c_str() );
            return 0;
        }
    } // namespace

    // The `Input` table: keyboard state + per-frame mouse delta + cursor control + the local player's Enhanced
    // Input actions and mapping contexts.
    void RegisterInputBindings( lua_State* L )
    {
        constexpr luaL_Reg kInput[] = { { "isKeyDown", &IsKeyDown },     { "wasPressed", &WasPressed },
                                        { "mouseDelta", &MouseDelta },   { "lockCursor", &LockCursor },
                                        { "showCursor", &ShowCursor },   { "isMouseDown", &IsMouseDown },
                                        { "actionValue", &ActionValue },
                                        { "actionTriggered", &ActionEvent<&Input::TriggerEvents::Triggered> },
                                        { "actionStarted", &ActionEvent<&Input::TriggerEvents::Started> },
                                        { "actionOngoing", &ActionEvent<&Input::TriggerEvents::Ongoing> },
                                        { "actionCompleted", &ActionEvent<&Input::TriggerEvents::Completed> },
                                        { "actionCanceled", &ActionEvent<&Input::TriggerEvents::Canceled> },
                                        { "actionSeconds", &ActionSeconds },
                                        { "addContext", &AddContext },   { "removeContext", &RemoveContext },
                                        { "rebindKey", &RebindKey },
                                        { nullptr, nullptr } };
        luaL_register( L, "Input", kInput );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
