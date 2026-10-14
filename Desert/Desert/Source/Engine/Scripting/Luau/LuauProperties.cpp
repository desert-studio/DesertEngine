#include <Engine/Scripting/Luau/LuauProperties.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Engine/Scripting/Luau/LuauBinder.hpp>

#include <VM/include/lua.h>
#include <VM/include/lualib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <new>
#include <string_view>
#include <utility>

namespace Desert::Scripting::LuauProperties
{
    namespace
    {
        /// What a Property.* constructor returns: the declaration and WHEN it was made. A Luau table does not
        /// keep the order its constructor listed the entries in, and Details must (a script's author orders
        /// the fields the way they want to read them), so every declaration takes the next number of one
        /// process-wide counter: within one top level the numbers rise in source order.
        struct Declaration
        {
            ScriptPropertyDecl Decl;
            std::uint64_t      Order = 0;
        };

        std::atomic<std::uint64_t> s_NextOrder{ 1 };

        constexpr const char* kOptions =
             "tooltip, category, displayName, min, max, units, editCondition, advanced";

        /// The options table at `index` (none / nil = no options) into `meta`. An unknown option is an error:
        /// a misspelt `tooltp` would otherwise be a tooltip that silently never shows.
        void ReadOptions( lua_State* L, int index, const char* ctor, Reflection::PropertyMetadata& meta,
                          bool rangeAllowed )
        {
            if ( lua_isnoneornil( L, index ) )
                return;
            if ( !lua_istable( L, index ) )
                luaL_errorL( L, "Property.%s: the options must be a table, got %s", ctor,
                             luaL_typename( L, index ) );
            index       = lua_absindex( L, index );
            bool hasMin = false;
            bool hasMax = false;
            lua_pushnil( L );
            while ( lua_next( L, index ) != 0 )
            {
                if ( lua_type( L, -2 ) != LUA_TSTRING )
                    luaL_errorL( L, "Property.%s: an option has no name", ctor );
                const std::string_view key  = lua_tostring( L, -2 );
                const auto             text = [&]( std::string& into )
                {
                    if ( lua_type( L, -1 ) != LUA_TSTRING )
                        luaL_errorL( L, "Property.%s: option '%s' must be a string", ctor, key.data() );
                    into = lua_tostring( L, -1 );
                };
                const auto number = [&]() -> float
                {
                    if ( lua_type( L, -1 ) != LUA_TNUMBER )
                        luaL_errorL( L, "Property.%s: option '%s' must be a number", ctor, key.data() );
                    return static_cast<float>( lua_tonumber( L, -1 ) );
                };
                if ( key == "tooltip" )
                    text( meta.Tooltip );
                else if ( key == "category" )
                    text( meta.Category );
                else if ( key == "displayName" )
                    text( meta.DisplayName );
                else if ( key == "units" )
                    text( meta.Units );
                else if ( key == "editCondition" )
                    text( meta.EditCondition );
                else if ( key == "advanced" )
                {
                    if ( lua_type( L, -1 ) != LUA_TBOOLEAN )
                        luaL_errorL( L, "Property.%s: option 'advanced' must be a boolean", ctor );
                    meta.Advanced = lua_toboolean( L, -1 ) != 0;
                }
                else if ( key == "min" )
                {
                    meta.RangeMin = number();
                    hasMin        = true;
                }
                else if ( key == "max" )
                {
                    meta.RangeMax = number();
                    hasMax        = true;
                }
                else
                    luaL_errorL( L, "Property.%s: unknown option '%s' (known: %s)", ctor, key.data(), kOptions );
                lua_pop( L, 1 );
            }
            if ( !hasMin && !hasMax )
                return;
            if ( !rangeAllowed )
                luaL_errorL( L, "Property.%s takes no min / max", ctor );
            if ( !hasMin || !hasMax )
                luaL_errorL( L, "Property.%s: give both min and max", ctor );
            if ( meta.RangeMin > meta.RangeMax )
                luaL_errorL( L, "Property.%s: min %g is above max %g", ctor, static_cast<double>( meta.RangeMin ),
                             static_cast<double>( meta.RangeMax ) );
            meta.HasRange = true;
        }

        /// A vector, or a table of three numbers {x, y, z}; false for anything else.
        bool ReadVec( lua_State* L, int index, std::array<double, 3>& out )
        {
            if ( lua_isvector( L, index ) )
            {
                const float* v = lua_tovector( L, index );
                out            = { v[0], v[1], v[2] };
                return true;
            }
            if ( !lua_istable( L, index ) || lua_objlen( L, index ) != 3 )
                return false;
            for ( int i = 0; i < 3; ++i )
            {
                lua_rawgeti( L, index, i + 1 );
                const bool isNumber                = lua_type( L, -1 ) == LUA_TNUMBER;
                out[static_cast<std::size_t>( i )] = isNumber ? lua_tonumber( L, -1 ) : 0.0;
                lua_pop( L, 1 );
                if ( !isNumber )
                    return false;
            }
            return true;
        }

        void CheckInRange( lua_State* L, const char* ctor, const ScriptPropertyDecl& decl, double value )
        {
            if ( decl.Meta.HasRange && ( value < decl.Meta.RangeMin || value > decl.Meta.RangeMax ) )
                luaL_errorL( L, "Property.%s: the default %g is outside min %g .. max %g", ctor, value,
                             static_cast<double>( decl.Meta.RangeMin ),
                             static_cast<double>( decl.Meta.RangeMax ) );
        }

        /// The value at `index` as one value of the kind `decl` declares (an Array's item, a default).
        ScriptValue ReadValueOf( lua_State* L, int index, const ScriptPropertyDecl& decl, const char* ctor )
        {
            ScriptValue value;
            value.Type     = decl.Default.Type;
            const int type = lua_type( L, index );
            switch ( decl.Default.Type )
            {
                case PropertyType::Number:
                case PropertyType::Int:
                    if ( type != LUA_TNUMBER )
                        luaL_errorL( L, "Property.%s: expected a number, got %s", ctor,
                                     luaL_typename( L, index ) );
                    value.Number = lua_tonumber( L, index );
                    if ( decl.Default.Type == PropertyType::Int && std::floor( value.Number ) != value.Number )
                        luaL_errorL( L, "Property.%s: %g is not a whole number", ctor, value.Number );
                    CheckInRange( L, ctor, decl, value.Number );
                    return value;
                case PropertyType::Bool:
                    if ( type != LUA_TBOOLEAN )
                        luaL_errorL( L, "Property.%s: expected a boolean, got %s", ctor,
                                     luaL_typename( L, index ) );
                    value.Bool = lua_toboolean( L, index ) != 0;
                    return value;
                case PropertyType::String:
                case PropertyType::Enum:
                    if ( type != LUA_TSTRING )
                        luaL_errorL( L, "Property.%s: expected a string, got %s", ctor,
                                     luaL_typename( L, index ) );
                    value.Str = lua_tostring( L, index );
                    if ( decl.Default.Type == PropertyType::Enum &&
                         std::find( decl.EnumOptions.begin(), decl.EnumOptions.end(), value.Str ) ==
                              decl.EnumOptions.end() )
                        luaL_errorL( L, "Property.%s: '%s' is not one of the enumerators", ctor,
                                     value.Str.c_str() );
                    return value;
                case PropertyType::Color:
                case PropertyType::Vector:
                    if ( !ReadVec( L, index, value.Vec ) )
                        luaL_errorL( L, "Property.%s: expected a vector or {x, y, z}, got %s", ctor,
                                     luaL_typename( L, index ) );
                    return value;
                case PropertyType::Asset:
                case PropertyType::Entity:
                    luaL_errorL( L, "Property.%s: a reference has no default; it is set in Details", ctor );
                case PropertyType::Array:
                    break;
            }
            luaL_errorL( L, "Property.%s: an Array cannot hold an Array", ctor );
        }

        int Push( lua_State* L, ScriptPropertyDecl decl )
        {
            void* memory = lua_newuserdatatagged( L, sizeof( Declaration ), kPropertyTag );
            new ( memory ) Declaration{ std::move( decl ), s_NextOrder.fetch_add( 1 ) };
            return 1;
        }

        ScriptPropertyDecl Make( PropertyType type )
        {
            ScriptPropertyDecl decl;
            decl.Default.Type = type;
            return decl;
        }

        /// Property.Float / Int / Bool / String ( default, options ).
        template <PropertyType Type>
        int Scalar( lua_State* L )
        {
            constexpr bool     numeric = Type == PropertyType::Number || Type == PropertyType::Int;
            const char*        ctor    = Type == PropertyType::Number ? "Float"
                                         : Type == PropertyType::Int  ? "Int"
                                         : Type == PropertyType::Bool ? "Bool"
                                                                      : "String";
            ScriptPropertyDecl decl    = Make( Type );
            ReadOptions( L, 2, ctor, decl.Meta, numeric );
            if ( !lua_isnoneornil( L, 1 ) )
                static_cast<ScriptValue&>( decl.Default ) = ReadValueOf( L, 1, decl, ctor );
            else if ( numeric && decl.Meta.HasRange )
                decl.Default.Number = std::clamp( 0.0, static_cast<double>( decl.Meta.RangeMin ),
                                                  static_cast<double>( decl.Meta.RangeMax ) );
            return Push( L, std::move( decl ) );
        }

        /// Property.Color ( default = white, options ) / Property.Vector ( default = zero, options ).
        template <PropertyType Type>
        int Vector( lua_State* L )
        {
            const char*        ctor = Type == PropertyType::Color ? "Color" : "Vector";
            ScriptPropertyDecl decl = Make( Type );
            decl.Meta.IsColor       = Type == PropertyType::Color;
            ReadOptions( L, 2, ctor, decl.Meta, Type == PropertyType::Vector );
            if ( Type == PropertyType::Color )
                decl.Default.Vec = { 1.0, 1.0, 1.0 };
            if ( !lua_isnoneornil( L, 1 ) )
                static_cast<ScriptValue&>( decl.Default ) = ReadValueOf( L, 1, decl, ctor );
            return Push( L, std::move( decl ) );
        }

        /// Property.Asset ( kind, options ): kind is a content kind's name ("Prefab", "StaticMesh", "Texture").
        int Asset( lua_State* L )
        {
            ScriptPropertyDecl decl  = Make( PropertyType::Asset );
            const char*        kind  = luaL_checkstring( L, 1 );
            bool               known = false;
            std::string        names;
            for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
            {
                const std::string_view name =
                     Common::Content::KindName( static_cast<Common::Content::ContentKind>( i ) );
                known = known || name == kind;
                names += std::format( "{}{}", names.empty() ? "" : ", ", name );
            }
            if ( !known )
                luaL_errorL( L, "Property.Asset: '%s' is no kind of asset (kinds: %s)", kind, names.c_str() );
            decl.Meta.IsAsset   = true;
            decl.Meta.AssetType = kind;
            ReadOptions( L, 2, "Asset", decl.Meta, false );
            return Push( L, std::move( decl ) );
        }

        /// Property.Entity ( options ).
        int Entity( lua_State* L )
        {
            ScriptPropertyDecl decl = Make( PropertyType::Entity );
            ReadOptions( L, 1, "Entity", decl.Meta, false );
            return Push( L, std::move( decl ) );
        }

        /// Property.Enum ( { "A", "B", ... }, default = the first, options ).
        int Enum( lua_State* L )
        {
            ScriptPropertyDecl decl = Make( PropertyType::Enum );
            luaL_checktype( L, 1, LUA_TTABLE );
            const int count = lua_objlen( L, 1 );
            for ( int i = 1; i <= count; ++i )
            {
                lua_rawgeti( L, 1, i );
                if ( lua_type( L, -1 ) != LUA_TSTRING )
                    luaL_errorL( L, "Property.Enum: enumerator %d is not a string", i );
                decl.EnumOptions.emplace_back( lua_tostring( L, -1 ) );
                lua_pop( L, 1 );
            }
            if ( decl.EnumOptions.empty() )
                luaL_errorL( L, "Property.Enum: no enumerators" );
            ReadOptions( L, 3, "Enum", decl.Meta, false );
            decl.Default.Str = decl.EnumOptions.front();
            if ( !lua_isnoneornil( L, 2 ) )
                static_cast<ScriptValue&>( decl.Default ) = ReadValueOf( L, 2, decl, "Enum" );
            return Push( L, std::move( decl ) );
        }

        /// Property.Array ( Property.<Kind>(...), { default items } = {}, options ).
        int Array( lua_State* L )
        {
            const auto* element = static_cast<const Declaration*>( lua_touserdatatagged( L, 1, kPropertyTag ) );
            if ( element == nullptr )
                luaL_errorL( L, "Property.Array: the first argument is the item's declaration, e.g. "
                                "Property.Array( Property.Float( 0 ), { 1, 2 } )" );
            if ( element->Decl.Default.Type == PropertyType::Array )
                luaL_errorL( L, "Property.Array: an Array cannot hold an Array" );

            ScriptPropertyDecl decl  = Make( PropertyType::Array );
            decl.ElementType         = element->Decl.Default.Type;
            decl.ElementMeta         = element->Decl.Meta;
            decl.EnumOptions         = element->Decl.EnumOptions;
            decl.Default.ElementType = decl.ElementType;
            ReadOptions( L, 3, "Array", decl.Meta, false );
            if ( !lua_isnoneornil( L, 2 ) )
            {
                luaL_checktype( L, 2, LUA_TTABLE );
                const int count = lua_objlen( L, 2 );
                for ( int i = 1; i <= count; ++i )
                {
                    lua_rawgeti( L, 2, i );
                    decl.Default.Items.push_back( ReadValueOf( L, lua_gettop( L ), element->Decl, "Array" ) );
                    lua_pop( L, 1 );
                }
            }
            return Push( L, std::move( decl ) );
        }

        constexpr const char* kPropertiesLineKey = "__propertiesline";

        /// The globals' __newindex (WatchPropertiesLine): `Properties` notes the line assigning it; every global
        /// is then set as it would be without the hook.
        int RecordPropertiesLine( lua_State* L )
        {
            if ( lua_type( L, 2 ) == LUA_TSTRING && std::string_view( lua_tostring( L, 2 ) ) == "Properties" )
            {
                lua_Debug ar{};
                if ( lua_getinfo( L, 1, "l", &ar ) != 0 && lua_getmetatable( L, 1 ) != 0 )
                {
                    lua_pushinteger( L, ar.currentline );
                    lua_setfield( L, -2, kPropertiesLineKey );
                    lua_pop( L, 1 );
                }
            }
            lua_settop( L, 3 );
            lua_rawset( L, 1 );
            return 0;
        }

        /// `<script>:<line>: message` (ScriptErrorLine's form) when the `Properties` line is known.
        std::string Located( lua_State* thread, std::string_view script, const std::string& message )
        {
            int line = 0;
            if ( lua_getmetatable( thread, LUA_GLOBALSINDEX ) != 0 )
            {
                lua_getfield( thread, -1, kPropertiesLineKey );
                line = lua_isnumber( thread, -1 ) ? lua_tointeger( thread, -1 ) : 0;
                lua_pop( thread, 2 );
            }
            return line > 0 ? std::format( "{}:{}: {}", script, line, message )
                            : std::format( "{}: {}", script, message );
        }

        const luaL_Reg kModule[] = {
             { "Float", &Scalar<PropertyType::Number> },
             { "Int", &Scalar<PropertyType::Int> },
             { "Bool", &Scalar<PropertyType::Bool> },
             { "String", &Scalar<PropertyType::String> },
             { "Color", &Vector<PropertyType::Color> },
             { "Vector", &Vector<PropertyType::Vector> },
             { "Asset", &Asset },
             { "Entity", &Entity },
             { "Enum", &Enum },
             { "Array", &Array },
             { nullptr, nullptr },
        };
    } // namespace

    void Install( lua_State* L )
    {
        lua_setuserdatadtor( L, kPropertyTag, []( lua_State*, void* declaration )
                             { static_cast<Declaration*>( declaration )->~Declaration(); } );
        lua_newtable( L );
        for ( const luaL_Reg* entry = kModule; entry->name != nullptr; ++entry )
        {
            lua_pushcfunction( L, entry->func, entry->name );
            lua_setfield( L, -2, entry->name );
        }
        lua_setglobal( L, "Property" );
    }

    void WatchPropertiesLine( lua_State* thread )
    {
        // luaL_sandboxthread gave the thread fresh globals reading through a READ-ONLY metatable to the shared
        // ones; this one reads through the same way and also notes where `Properties` is first assigned.
        lua_getmetatable( thread, LUA_GLOBALSINDEX );
        lua_newtable( thread );
        lua_getfield( thread, -2, "__index" );
        lua_setfield( thread, -2, "__index" );
        lua_pushcfunction( thread, &RecordPropertiesLine, "Properties line" );
        lua_setfield( thread, -2, "__newindex" );
        lua_setmetatable( thread, LUA_GLOBALSINDEX );
        lua_pop( thread, 1 );
    }

    Common::ResultStr<std::vector<ScriptPropertyDecl>> Settle( lua_State* thread, std::string_view script )
    {
        using Decls = std::vector<ScriptPropertyDecl>;
        lua_getglobal( thread, "Properties" );
        if ( lua_isnil( thread, -1 ) )
        {
            lua_pop( thread, 1 );
            return Common::MakeSuccess( Decls{} );
        }
        if ( !lua_istable( thread, -1 ) )
        {
            const std::string type = luaL_typename( thread, -1 );
            lua_pop( thread, 1 );
            return Common::MakeError<Decls>(
                 Located( thread, script, std::format( "'Properties' is a {}, not a table", type ) ) );
        }

        std::vector<std::pair<std::uint64_t, ScriptPropertyDecl>> declared;
        lua_pushnil( thread );
        while ( lua_next( thread, -2 ) != 0 )
        {
            if ( lua_type( thread, -2 ) != LUA_TSTRING )
            {
                lua_pop( thread, 3 );
                return Common::MakeError<Decls>( std::string( "'Properties' holds an entry without a name" ) );
            }
            const std::string name = lua_tostring( thread, -2 );
            if ( const auto* d =
                      static_cast<const Declaration*>( lua_touserdatatagged( thread, -1, kPropertyTag ) ) )
            {
                ScriptPropertyDecl decl = d->Decl;
                decl.Name               = name;
                decl.Default.Name       = name;
                declared.emplace_back( d->Order, std::move( decl ) );
            }
            else
            {
                const int         kind = lua_type( thread, -1 );
                const std::string want =
                     kind == LUA_TBOOLEAN  ? std::format( "Property.Bool( {} )", lua_toboolean( thread, -1 ) != 0 )
                     : kind == LUA_TNUMBER ? std::format( "Property.Float( {} ) or "
                                                          "Property.Int( {} )",
                                                          lua_tonumber( thread, -1 ), lua_tonumber( thread, -1 ) )
                     : kind == LUA_TSTRING ? std::format( "Property.String( \"{}\" )", lua_tostring( thread, -1 ) )
                                           : std::string( "Property.Float(...), Property.Entity(...), ..." );
                const std::string type = luaL_typename( thread, -1 );
                lua_pop( thread, 3 );
                return Common::MakeError<Decls>( Located( thread, script,
                                                          std::format( "Properties.{} is a plain {}: declare it "
                                                                       "as {}",
                                                                       name, type, want ) ) );
            }
            lua_pop( thread, 1 );
        }

        // Declarations in source order (a Luau table keeps none; the constructors counted it).
        std::sort( declared.begin(), declared.end(),
                   []( const auto& a, const auto& b ) { return a.first < b.first; } );
        Decls decls;
        decls.reserve( declared.size() );
        for ( auto& [order, decl] : declared )
            decls.push_back( std::move( decl ) );

        // EditCondition names a Bool of THIS script ("!Name" inverts it): Details greys the row by it, so a name
        // that is not one would be a condition that never reads anything.
        for ( const ScriptPropertyDecl& decl : decls )
        {
            if ( decl.Meta.EditCondition.empty() )
                continue;
            const std::string_view gate = std::string_view( decl.Meta.EditCondition ).starts_with( '!' )
                                               ? std::string_view( decl.Meta.EditCondition ).substr( 1 )
                                               : std::string_view( decl.Meta.EditCondition );
            const bool isBool = std::any_of( decls.begin(), decls.end(), [&]( const ScriptPropertyDecl& d )
                                             { return d.Name == gate && d.Default.Type == PropertyType::Bool; } );
            if ( !isBool )
            {
                lua_pop( thread, 1 );
                return Common::MakeError<Decls>( Located(
                     thread, script,
                     std::format(
                          "Properties.{}: editCondition names '{}', which is no Bool property of this script",
                          decl.Name, gate ) ) );
            }
        }

        // The script reads values, never declarations: every entry becomes its default (an unset reference nil).
        for ( const ScriptPropertyDecl& decl : decls )
        {
            PushProperty( thread, decl.Default, World{} );
            lua_setfield( thread, -2, decl.Name.c_str() );
        }
        lua_pop( thread, 1 );
        return Common::MakeSuccess( std::move( decls ) );
    }

    void PushValue( lua_State* L, const ScriptValue& value, const World& world )
    {
        switch ( value.Type )
        {
            case PropertyType::Number:
            case PropertyType::Int:
                lua_pushnumber( L, value.Number );
                return;
            case PropertyType::Bool:
                lua_pushboolean( L, value.Bool ? 1 : 0 );
                return;
            case PropertyType::String:
            case PropertyType::Enum:
                lua_pushlstring( L, value.Str.data(), value.Str.size() );
                return;
            case PropertyType::Color:
            case PropertyType::Vector:
                lua_pushvector( L, static_cast<float>( value.Vec[0] ), static_cast<float>( value.Vec[1] ),
                                static_cast<float>( value.Vec[2] ) );
                return;
            case PropertyType::Asset:
                if ( value.Ref == 0 )
                    break;
                LuauBinder::PushAssetHandle( L, value.Ref );
                return;
            case PropertyType::Entity:
                if ( value.Ref == 0 || world.Registry == nullptr || !world.FindEntity )
                    break;
                LuauBinder::PushEntity( L, *world.Registry, world.FindEntity( value.Ref ) );
                return;
            case PropertyType::Array:
                break; // an item is never an Array (Property.Array refuses one)
        }
        lua_pushnil( L );
    }

    void PushProperty( lua_State* L, const ScriptProperty& property, const World& world )
    {
        if ( property.Type != PropertyType::Array )
        {
            PushValue( L, property, world );
            return;
        }
        lua_createtable( L, static_cast<int>( property.Items.size() ), 0 );
        for ( std::size_t i = 0; i < property.Items.size(); ++i )
        {
            PushValue( L, property.Items[i], world );
            lua_rawseti( L, -2, static_cast<int>( i ) + 1 );
        }
    }

    bool ReadScalar( lua_State* L, int index, PropertyType type, ScriptValue& out )
    {
        out.Type = type;
        switch ( type )
        {
            case PropertyType::Number:
            case PropertyType::Int:
                if ( lua_type( L, index ) != LUA_TNUMBER )
                    return false;
                out.Number = lua_tonumber( L, index );
                return true;
            case PropertyType::Bool:
                if ( lua_type( L, index ) != LUA_TBOOLEAN )
                    return false;
                out.Bool = lua_toboolean( L, index ) != 0;
                return true;
            case PropertyType::String:
            case PropertyType::Enum:
                if ( lua_type( L, index ) != LUA_TSTRING )
                    return false;
                out.Str = lua_tostring( L, index );
                return true;
            default:
                return false;
        }
    }
} // namespace Desert::Scripting::LuauProperties
