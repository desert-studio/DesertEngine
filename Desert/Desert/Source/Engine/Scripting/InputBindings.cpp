#include "Internal/ScriptRuntime.hpp"

namespace Desert::Scripting
{
    // Input table: keyboard/mouse state, edge presses, cursor control.
    void RegisterInputBindings( ScriptEngine::Impl& implRef )
    {
        auto& lua  = implRef.Lua;
        auto* impl = &implRef;
        (void)lua; (void)impl;

        // The `Input` table: keyboard state + per-frame mouse delta + cursor control.
        sol::table input    = lua.create_named_table( "Input" );
        input["isKeyDown"]  = []( const std::string& name )
        {
            auto k = KeyFromName( name );
            return k.has_value() && Input::Keyboard::IsKeyPressed( *k );
        };
        // Fires once, on the frame the key goes down (edge). Needs NewInputFrame() once per frame.
        input["wasPressed"] = [impl]( const std::string& name )
        {
            auto k = KeyFromName( name );
            if ( !k.has_value() )
                return false;
            auto it = impl->KeyEdge.find( static_cast<int>( *k ) );
            return it != impl->KeyEdge.end() && it->second;
        };
        input["mouseDelta"]  = [impl]() { return std::make_tuple( impl->MouseDx, impl->MouseDy ); };
        input["lockCursor"]  = [impl]() { impl->CursorLockRequest = true; };  // capture (gameplay look)
        input["showCursor"]  = [impl]() { impl->CursorLockRequest = false; }; // free (click UI)
        // Raw mouse-button held state ("left"/"right"/"middle"). Edge-detect in-script if you need one-shot.
        input["isMouseDown"] = []( const std::string& name )
        {
            Common::MouseButton b = Common::MouseButton::Left;
            if ( name == "right" )
                b = Common::MouseButton::Right;
            else if ( name == "middle" )
                b = Common::MouseButton::Middle;
            return Input::Mouse::Get().IsMouseButtonPressed( b );
        };

        // ---- Enhanced Input (GP1b): the local player's actions, by their file names (IA_Jump), as UE's
        // FInputActionValue / ETriggerEvent. An action no added context ever mapped is a Lua error naming it
        // (a typo must not read as "not pressed" forever).
        const auto action = [impl]( const std::string& name )
        {
            const auto guid = impl->PlayerInput.ActionNamed( name );
            if ( !guid )
                throw sol::error( "Input: no mapping context the player added maps an action named '" + name +
                                  "'" );
            return *guid;
        };
        input["actionValue"] = [impl, action]( const std::string& name )
        {
            const glm::vec3 v = impl->PlayerInput.Subsystem().GetActionValue( action( name ) );
            return std::make_tuple( v.x, v.y, v.z );
        };
        input["actionTriggered"] = [impl, action]( const std::string& name )
        { return impl->PlayerInput.Subsystem().GetTriggerEvents( action( name ) ).Triggered; };
        input["actionStarted"] = [impl, action]( const std::string& name )
        { return impl->PlayerInput.Subsystem().GetTriggerEvents( action( name ) ).Started; };
        input["actionOngoing"] = [impl, action]( const std::string& name )
        { return impl->PlayerInput.Subsystem().GetTriggerEvents( action( name ) ).Ongoing; };
        input["actionCompleted"] = [impl, action]( const std::string& name )
        { return impl->PlayerInput.Subsystem().GetTriggerEvents( action( name ) ).Completed; };
        input["actionCanceled"] = [impl, action]( const std::string& name )
        { return impl->PlayerInput.Subsystem().GetTriggerEvents( action( name ) ).Canceled; };
        input["actionSeconds"] = [impl, action]( const std::string& name )
        { return impl->PlayerInput.Subsystem().GetTriggeredSeconds( action( name ) ); };
        // UE AddMappingContext / RemoveMappingContext, the context by file name (IMC_Vehicle) or content path.
        input["addContext"] = [impl]( const std::string& name, const int priority )
        {
            if ( impl->Assets == nullptr )
                throw sol::error( "Input.addContext: this world has no asset manager" );
            if ( auto added = impl->PlayerInput.AddContext( *impl->Assets, name, priority ); !added )
                throw sol::error( "Input.addContext('" + name + "'): " + added.GetError() );
        };
        input["removeContext"] = [impl]( const std::string& name )
        { return impl->PlayerInput.RemoveContext( name ); };
        // UE MapPlayerKey: the player's own key for one mapping, saved to this user's input.json.
        input["rebindKey"] = [impl]( const std::string& context, const std::string& actionName,
                                     const std::string& defaultKey, const std::string& key )
        {
            if ( auto rebound = impl->PlayerInput.RebindKey( context, actionName, defaultKey, key ); !rebound )
                throw sol::error( "Input.rebindKey: " + rebound.GetError() );
        };
    }
} // namespace Desert::Scripting
