#include "Internal/ScriptRuntime.hpp"

#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIOverlay.hpp>

#include <string>
#include <tuple>

namespace Desert::Scripting
{
    // The UI <-> gameplay bridge, from the script side.
    //
    //   ui.set( "player.hp", 87 )        -- any element bound to that key follows on the next frame
    //   ui.get( "player.hp" )            -- read it back (number / string / bool)
    //   ui.send( "quest.completed" )     -- raise a UI message yourself (same channel as a button)
    //   ui.toast( "Toasts", "Saved" )    -- queue a notification on a Toast overlay canvas
    //   ui.list_add( "chat", { text = "hi", from = "Ann" } )  -- append a record to a collection a
    //                                    UIListView is bound to; list_insert / list_remove / list_set /
    //                                    list_clear / list_count complete it. Indices are 1-based, as in Lua.
    //
    // and, in the other direction, a script that defines OnUIMessage( msg ) hears every button action,
    // pointer event and drop the canvas produced. Scripts never look elements up by name: they write
    // DATA and the bindings do the rest, so renaming or restyling a widget can't break gameplay code.
    namespace
    {
        // A Lua table of fields -> one record. Numbers, bools and strings map to themselves; a table of three
        // numbers is a colour, the same shape ui.set's ( key, r, g, b ) form writes. Anything else is refused
        // by name rather than dropped, so a typo in a record is an error at the write and not a blank cell.
        Common::ResultStr<UI::UIDataStore> RecordFromTable( const sol::table& fields )
        {
            UI::UIDataStore record;
            for ( const auto& [k, v] : fields )
            {
                if ( !k.is<std::string>() )
                    return Common::MakeError<UI::UIDataStore>(
                         std::string( "ui.list record: field names must be strings" ) );
                const std::string name = k.as<std::string>();
                switch ( v.get_type() )
                {
                    case sol::type::number:
                        record.Set( name, v.as<double>() );
                        break;
                    case sol::type::boolean:
                        record.Set( name, v.as<bool>() );
                        break;
                    case sol::type::string:
                        record.Set( name, v.as<std::string>() );
                        break;
                    case sol::type::table:
                    {
                        const sol::table t = v.as<sol::table>();
                        if ( t.size() != 3 )
                            return Common::MakeError<UI::UIDataStore>(
                                 "ui.list record: field '" + name + "' is a table of " +
                                 std::to_string( t.size() ) + " values; only { r, g, b } is a value" );
                        record.Set( name, glm::vec3( t.get<float>( 1 ), t.get<float>( 2 ), t.get<float>( 3 ) ) );
                        break;
                    }
                    default:
                        return Common::MakeError<UI::UIDataStore>( "ui.list record: field '" + name +
                                                                   "' has a type a UI binding cannot show" );
                }
            }
            return Common::MakeSuccess( std::move( record ) );
        }

        // Lua's idiom for a failed call that is the caller's to handle: true, or false plus the reason.
        std::tuple<bool, std::string> Outcome( const Common::BoolResultStr& r )
        {
            return { r.IsSuccess(), r.IsSuccess() ? std::string() : r.GetError() };
        }
    } // namespace

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
        // Collections (UIL1). Every mutation returns ( ok, error ): an index outside the collection is the
        // script's bug and it is told so, with the numbers, instead of the write vanishing.
        ui.set_function( "list_add",
                         []( const std::string& key, const sol::table& fields ) -> std::tuple<bool, std::string>
                         {
                             auto record = RecordFromTable( fields );
                             if ( !record.IsSuccess() )
                                 return { false, record.GetError() };
                             UI::UIDataStore::Get().Collection( key ).Add( record.ExtractValue() );
                             return { true, std::string() };
                         } );
        ui.set_function(
             "list_insert",
             []( const std::string& key, int index, const sol::table& fields ) -> std::tuple<bool, std::string>
             {
                 auto record = RecordFromTable( fields );
                 if ( !record.IsSuccess() )
                     return { false, record.GetError() };
                 return Outcome(
                      UI::UIDataStore::Get().Collection( key ).Insert( index - 1, record.ExtractValue() ) );
             } );
        ui.set_function( "list_remove", []( const std::string& key, int index )
                         { return Outcome( UI::UIDataStore::Get().Collection( key ).Remove( index - 1 ) ); } );
        ui.set_function(
             "list_set",
             sol::overload(
                  []( const std::string& key, int index, const std::string& field, double value ) {
                      return Outcome(
                           UI::UIDataStore::Get().Collection( key ).SetField( index - 1, field, value ) );
                  },
                  []( const std::string& key, int index, const std::string& field, bool value ) {
                      return Outcome(
                           UI::UIDataStore::Get().Collection( key ).SetField( index - 1, field, value ) );
                  },
                  []( const std::string& key, int index, const std::string& field, const std::string& value ) {
                      return Outcome(
                           UI::UIDataStore::Get().Collection( key ).SetField( index - 1, field, value ) );
                  } ) );
        ui.set_function( "list_clear",
                         []( const std::string& key ) { UI::UIDataStore::Get().Collection( key ).Clear(); } );
        ui.set_function( "list_count",
                         []( const std::string& key )
                         {
                             const UI::UICollection* c = UI::UIDataStore::Get().FindCollection( key );
                             return c != nullptr ? c->Size() : 0;
                         } );

        ui.set_function( "toast", []( const std::string& overlay, const std::string& text )
                         { UI::UIOverlayRequests::Get().Raise( overlay, text ); } );
    }
} // namespace Desert::Scripting
