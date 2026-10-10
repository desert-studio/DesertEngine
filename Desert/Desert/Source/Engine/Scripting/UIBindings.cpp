#include "Internal/ScriptRuntime.hpp"

#include <UI/UIDataStore.hpp>
#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>

#include <format>
#include <string>

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
        UI::UIDataStore& Store()
        {
            return UI::UIDataStore::Get();
        }

        // A Lua table of fields -> one record. Numbers, bools and strings map to themselves; a table of three
        // numbers is a colour, the same shape ui.set's ( key, r, g, b ) form writes. Anything else is refused
        // by name rather than dropped, so a typo in a record is an error at the write and not a blank cell.
        Common::ResultStr<UI::UIDataStore> RecordFromTable( lua_State* L, int index )
        {
            luaL_checktype( L, index, LUA_TTABLE );
            UI::UIDataStore record;
            lua_pushnil( L );
            while ( lua_next( L, index ) != 0 )
            {
                // key at -2, value at -1; lua_next needs the key left on the stack.
                if ( lua_type( L, -2 ) != LUA_TSTRING )
                {
                    lua_pop( L, 2 );
                    return Common::MakeError<UI::UIDataStore>(
                         std::string( "ui.list record: field names must be strings" ) );
                }
                const std::string name = lua_tostring( L, -2 );
                switch ( lua_type( L, -1 ) )
                {
                    case LUA_TNUMBER:
                        record.Set( name, static_cast<double>( lua_tonumber( L, -1 ) ) );
                        break;
                    case LUA_TBOOLEAN:
                        record.Set( name, lua_toboolean( L, -1 ) != 0 );
                        break;
                    case LUA_TSTRING:
                        record.Set( name, std::string( lua_tostring( L, -1 ) ) );
                        break;
                    case LUA_TTABLE:
                    {
                        const int size = lua_objlen( L, -1 );
                        if ( size != 3 )
                        {
                            lua_pop( L, 2 );
                            return Common::MakeError<UI::UIDataStore>(
                                 std::format( "ui.list record: field '{}' is a table of {} values; only {{ r, g, "
                                              "b }} is a value",
                                              name, size ) );
                        }
                        glm::vec3 colour{};
                        for ( int i = 0; i < 3; ++i )
                        {
                            lua_rawgeti( L, -1, i + 1 );
                            colour[i] = static_cast<float>( lua_tonumber( L, -1 ) );
                            lua_pop( L, 1 );
                        }
                        record.Set( name, colour );
                        break;
                    }
                    default:
                        lua_pop( L, 2 );
                        return Common::MakeError<UI::UIDataStore>( std::format(
                             "ui.list record: field '{}' has a type a UI binding cannot show", name ) );
                }
                lua_pop( L, 1 );
            }
            return Common::MakeSuccess( std::move( record ) );
        }

        // Lua's idiom for a failed call that is the caller's to handle: true, or false plus the reason.
        int Outcome( lua_State* L, const Common::BoolResultStr& r )
        {
            lua_pushboolean( L, r.IsSuccess() ? 1 : 0 );
            lua_pushstring( L, r.IsSuccess() ? "" : r.GetError().c_str() );
            return 2;
        }

        // set( key, value ) — number / string / bool, or ( key, r, g, b ) for a colour.
        int DataSet( lua_State* L )
        {
            const std::string key = luaL_checkstring( L, 1 );
            if ( lua_gettop( L ) >= 4 )
            {
                Store().Set( key, glm::vec3( static_cast<float>( luaL_checknumber( L, 2 ) ),
                                             static_cast<float>( luaL_checknumber( L, 3 ) ),
                                             static_cast<float>( luaL_checknumber( L, 4 ) ) ) );
                return 0;
            }
            switch ( lua_type( L, 2 ) )
            {
                case LUA_TNUMBER:
                    Store().Set( key, static_cast<double>( lua_tonumber( L, 2 ) ) );
                    return 0;
                case LUA_TBOOLEAN:
                    Store().Set( key, lua_toboolean( L, 2 ) != 0 );
                    return 0;
                case LUA_TSTRING:
                    Store().Set( key, std::string( lua_tostring( L, 2 ) ) );
                    return 0;
                default:
                    luaL_error( L, "ui.set('%s'): a value is a number, a boolean, a string or r, g, b; got %s",
                                key.c_str(), luaL_typename( L, 2 ) );
            }
            return 0;
        }

        // get( key ) — returns the value in its natural Lua type, or nil when unset.
        int DataGet( lua_State* L )
        {
            const std::string      key   = luaL_checkstring( L, 1 );
            const UI::UIDataStore& store = Store();
            if ( const auto b = store.Bool( key ); b && !store.Number( key ) )
                lua_pushboolean( L, *b ? 1 : 0 );
            else if ( const auto n = store.Number( key ) )
                lua_pushnumber( L, *n );
            else if ( const auto t = store.Text( key ) )
                lua_pushstring( L, t->c_str() );
            else
                lua_pushnil( L );
            return 1;
        }

        int DataHas( lua_State* L )
        {
            lua_pushboolean( L, Store().Has( luaL_checkstring( L, 1 ) ) ? 1 : 0 );
            return 1;
        }

        int Clear( lua_State* L )
        {
            if ( lua_isnoneornil( L, 1 ) )
                Store().Clear();
            else
                Store().Erase( luaL_checkstring( L, 1 ) );
            return 0;
        }

        // send( msg ) — put a message on the same queue the canvas uses, so a script can drive another
        // script's OnUIMessage (or its own) without a second channel.
        int Send( lua_State* L )
        {
            UI::UIMessageQueue::Get().Push( luaL_checkstring( L, 1 ) );
            return 0;
        }

        // Collections (UIL1). Every mutation returns ( ok, error ): an index outside the collection is the
        // script's bug and it is told so, with the numbers, instead of the write vanishing. Indices are 1-based.
        int Index( lua_State* L, int at )
        {
            return luaL_checkinteger( L, at ) - 1;
        }

        int ListAdd( lua_State* L )
        {
            const std::string key    = luaL_checkstring( L, 1 );
            auto              record = RecordFromTable( L, 2 );
            if ( !record.IsSuccess() )
                return Outcome( L, Common::MakeError<bool>( record.GetError() ) );
            Store().Collection( key ).Add( record.ExtractValue() );
            return Outcome( L, Common::MakeSuccess( true ) );
        }
        int ListInsert( lua_State* L )
        {
            const std::string key    = luaL_checkstring( L, 1 );
            const int         index  = Index( L, 2 );
            auto              record = RecordFromTable( L, 3 );
            if ( !record.IsSuccess() )
                return Outcome( L, Common::MakeError<bool>( record.GetError() ) );
            return Outcome( L, Store().Collection( key ).Insert( index, record.ExtractValue() ) );
        }
        int ListRemove( lua_State* L )
        {
            const std::string key = luaL_checkstring( L, 1 );
            return Outcome( L, Store().Collection( key ).Remove( Index( L, 2 ) ) );
        }
        int ListSet( lua_State* L )
        {
            const std::string key   = luaL_checkstring( L, 1 );
            const int         index = Index( L, 2 );
            const std::string field = luaL_checkstring( L, 3 );
            UI::UICollection& list  = Store().Collection( key );
            switch ( lua_type( L, 4 ) )
            {
                case LUA_TNUMBER:
                    return Outcome( L,
                                    list.SetField( index, field, static_cast<double>( lua_tonumber( L, 4 ) ) ) );
                case LUA_TBOOLEAN:
                    return Outcome( L, list.SetField( index, field, lua_toboolean( L, 4 ) != 0 ) );
                case LUA_TSTRING:
                    return Outcome( L, list.SetField( index, field, std::string( lua_tostring( L, 4 ) ) ) );
                default:
                    return Outcome( L, Common::MakeError<bool>( std::format(
                                            "ui.list_set('{}', {}, '{}'): a value is a number, a boolean or a "
                                            "string; got {}",
                                            key, index + 1, field, luaL_typename( L, 4 ) ) ) );
            }
        }
        int ListClear( lua_State* L )
        {
            Store().Collection( luaL_checkstring( L, 1 ) ).Clear();
            return 0;
        }
        int ListCount( lua_State* L )
        {
            const UI::UICollection* c = Store().FindCollection( luaL_checkstring( L, 1 ) );
            lua_pushinteger( L, c != nullptr ? static_cast<int>( c->Size() ) : 0 );
            return 1;
        }

        // toast( overlay, text ) — queue a notification on the named Toast overlay. It is not `ui.set`,
        // because a notification is an EVENT and a data-store key is a STATE: raising the same message
        // twice must queue it twice, and writing the same key twice writes it once.
        int Toast( lua_State* L )
        {
            UI::UIOverlayRequests::Get().Raise( luaL_checkstring( L, 1 ), luaL_checkstring( L, 2 ) );
            return 0;
        }
    } // namespace

    void RegisterUIBindings( lua_State* L )
    {
        constexpr luaL_Reg kUI[] = { { "set", &DataSet },
                                     { "get", &DataGet },
                                     { "has", &DataHas },
                                     { "clear", &Clear },
                                     { "send", &Send },
                                     { "list_add", &ListAdd },
                                     { "list_insert", &ListInsert },
                                     { "list_remove", &ListRemove },
                                     { "list_set", &ListSet },
                                     { "list_clear", &ListClear },
                                     { "list_count", &ListCount },
                                     { "toast", &Toast },
                                     { nullptr, nullptr } };
        luaL_register( L, "ui", kUI );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
