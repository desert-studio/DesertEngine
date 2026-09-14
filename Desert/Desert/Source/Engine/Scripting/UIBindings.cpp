#include "Internal/ScriptRuntime.hpp"

#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIOverlay.hpp>

namespace Desert::Scripting
{
    // The UI <-> gameplay bridge, from the script side.
    //
    //   ui.set( "player.hp", 87 )        -- any element bound to that key follows on the next frame
    //   ui.get( "player.hp" )            -- read it back (number / string / bool)
    //   ui.send( "quest.completed" )     -- raise a UI message yourself (same channel as a button)
    //   ui.toast( "Toasts", "Saved" )    -- queue a notification on a Toast overlay canvas
    //
    // and, in the other direction, a script that defines OnUIMessage( msg ) hears every button action,
    // pointer event and drop the canvas produced. Scripts never look elements up by name: they write
    // DATA and the bindings do the rest, so renaming or restyling a widget can't break gameplay code.
    void RegisterUIBindings( ScriptEngine::Impl& implRef )
    {
        auto& lua = implRef.Lua;

        sol::table ui = lua.create_named_table( "ui" );

        // set( key, value ) — number / string / bool, or ( key, r, g, b ) for a colour.
        ui.set_function( "set", sol::overload( []( const std::string& key, double value )
                                               { UI::UIDataStore::Get().Set( key, value ); },
                                               []( const std::string& key, bool value )
                                               { UI::UIDataStore::Get().Set( key, value ); },
                                               []( const std::string& key, const std::string& value )
                                               { UI::UIDataStore::Get().Set( key, value ); },
                                               []( const std::string& key, float r, float g, float b )
                                               { UI::UIDataStore::Get().Set( key, glm::vec3( r, g, b ) ); } ) );

        // get( key ) — returns the value in its natural Lua type, or nil when unset.
        ui.set_function( "get",
                         [&lua]( const std::string& key ) -> sol::object
                         {
                             const UI::UIDataStore& store = UI::UIDataStore::Get();
                             if ( const auto b = store.Bool( key ); b && !store.Number( key ) )
                                 return sol::make_object( lua, *b );
                             if ( const auto n = store.Number( key ) )
                                 return sol::make_object( lua, *n );
                             if ( const auto t = store.Text( key ) )
                                 return sol::make_object( lua, *t );
                             return sol::lua_nil;
                         } );

        ui.set_function( "has", []( const std::string& key ) { return UI::UIDataStore::Get().Has( key ); } );
        ui.set_function( "clear",
                         []( sol::optional<std::string> key )
                         {
                             if ( key )
                                 UI::UIDataStore::Get().Erase( *key );
                             else
                                 UI::UIDataStore::Get().Clear();
                         } );

        // send( msg ) — put a message on the same queue the canvas uses, so a script can drive another
        // script's OnUIMessage (or its own) without a second channel.
        ui.set_function( "send", []( const std::string& message ) { UI::UIMessageQueue::Get().Push( message ); } );

        // toast( overlay, text ) — queue a notification on the named Toast overlay. It is not `ui.set`,
        // because a notification is an EVENT and a data-store key is a STATE: raising the same message
        // twice must queue it twice, and writing the same key twice writes it once.
        ui.set_function( "toast", []( const std::string& overlay, const std::string& text )
                         { UI::UIOverlayRequests::Get().Raise( overlay, text ); } );
    }
} // namespace Desert::Scripting
