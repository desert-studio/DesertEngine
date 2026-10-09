#include <Engine/Scripting/Luau/LuauBinder.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Reflection/FieldValue.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <new>
#include <vector>

#include <VM/include/lualib.h>

namespace Desert::Scripting::LuauBinder
{
    namespace
    {
        using Reflection::FieldType;
        using Reflection::FunctionInfo;
        using Reflection::TypeInfo;
        using Reflection::Value;

        /// The userdata tag of a bound object (Luau tags 1..LUA_UTAG_LIMIT-1 are the embedder's).
        constexpr int kObjectTag = 1;

        /// Registry key of the method cache: FunctionInfo* (light userdata) -> its closure, built on first use.
        constexpr const char* kMethodCache = "Desert.LuauMethods";

        LuauBinding* CheckObject( lua_State* L, int index )
        {
            auto* object = static_cast<LuauBinding*>( lua_touserdatatagged( L, index, kObjectTag ) );
            if ( object == nullptr )
                luaL_errorL( L, "expected an engine object, got %s", lua_typename( L, lua_type( L, index ) ) );
            return object;
        }

        void* Instance( lua_State* L, const LuauBinding& object )
        {
            void* instance = object.Resolve ? object.Resolve() : nullptr;
            if ( instance == nullptr )
                luaL_errorL( L, "the %s bound as '%s' is gone", object.Type->Name.c_str(), object.Name.c_str() );
            return instance;
        }

        const Reflection::FieldInfo* FindField( const TypeInfo& type, const char* name )
        {
            for ( const Reflection::FieldInfo& field : type.Fields )
                if ( field.Name == name )
                    return &field;
            return nullptr;
        }

        /// Calls `function` with the arguments from stack slot `first` on; pushes its results.
        int Invoke( lua_State* L, const FunctionInfo& function, void* self, int first )
        {
            const int given = lua_gettop( L ) - first + 1;
            if ( given != static_cast<int>( function.Params.size() ) )
                luaL_errorL( L, "%s.%s takes %d argument(s), got %d", function.Owner.c_str(),
                             function.Name.c_str(), static_cast<int>( function.Params.size() ), given );

            std::vector<Value> args;
            args.reserve( function.Params.size() );
            for ( std::size_t i = 0; i < function.Params.size(); ++i )
            {
                std::string                why;
                const std::optional<Value> value =
                     ToValue( L, first + static_cast<int>( i ), function.Params[i].Type, why );
                if ( !value )
                    luaL_errorL( L, "%s.%s argument %d ('%s'): %s", function.Owner.c_str(), function.Name.c_str(),
                                 static_cast<int>( i ) + 1, function.Params[i].Name.c_str(), why.c_str() );
                args.push_back( *value );
            }

            std::vector<Value> rets( function.Returns.size() );
            if ( Common::BoolResultStr called = function.Invoke( self, args.data(), args.size(), rets.data() );
                 !called.IsSuccess() )
                luaL_errorL( L, "%s", called.GetError().c_str() );
            for ( const Value& ret : rets )
                PushValue( L, ret );
            return static_cast<int>( rets.size() );
        }

        int CallMethod( lua_State* L )
        {
            const auto* function =
                 static_cast<const FunctionInfo*>( lua_tolightuserdata( L, lua_upvalueindex( 1 ) ) );
            auto* object = static_cast<LuauBinding*>( lua_touserdatatagged( L, 1, kObjectTag ) );
            if ( object == nullptr || object->Type->Name != function->Owner )
                luaL_errorL( L, "%s.%s is a method of %s: call it as obj:%s(...)", function->Owner.c_str(),
                             function->Name.c_str(), function->Owner.c_str(), function->Name.c_str() );
            return Invoke( L, *function, Instance( L, *object ), 2 );
        }

        int CallStatic( lua_State* L )
        {
            const auto* function =
                 static_cast<const FunctionInfo*>( lua_tolightuserdata( L, lua_upvalueindex( 1 ) ) );
            return Invoke( L, *function, nullptr, 1 );
        }

        void PushMethod( lua_State* L, const FunctionInfo& function )
        {
            lua_rawgetfield( L, LUA_REGISTRYINDEX, kMethodCache );
            lua_pushlightuserdata( L, const_cast<FunctionInfo*>( &function ) );
            lua_rawget( L, -2 );
            if ( lua_isnil( L, -1 ) )
            {
                lua_pop( L, 1 );
                lua_pushlightuserdata( L, const_cast<FunctionInfo*>( &function ) );
                lua_pushcclosure( L, CallMethod, function.Name.c_str(), 1 );
                lua_pushlightuserdata( L, const_cast<FunctionInfo*>( &function ) );
                lua_pushvalue( L, -2 );
                lua_rawset( L, -4 );
            }
            lua_remove( L, -2 ); // the cache table
        }

        int Index( lua_State* L )
        {
            const LuauBinding& object = *CheckObject( L, 1 );
            const char*        key    = luaL_checkstring( L, 2 );

            if ( const Reflection::FieldInfo* field = FindField( *object.Type, key ) )
            {
                Common::ResultStr<Value> value = Reflection::ReadField( *field, Instance( L, object ) );
                if ( !value.IsSuccess() )
                    luaL_errorL( L, "%s", value.GetError().c_str() );
                PushValue( L, value.GetValue() );
                return 1;
            }
            if ( const FunctionInfo* function = object.Type->FindFunction( key );
                 function != nullptr && function->Meta.ScriptCallable && !function->IsStatic )
            {
                PushMethod( L, *function );
                return 1;
            }
            luaL_errorL( L, "%s has no field or script-callable method '%s'", object.Type->Name.c_str(), key );
        }

        int NewIndex( lua_State* L )
        {
            const LuauBinding& object = *CheckObject( L, 1 );
            const char*        key    = luaL_checkstring( L, 2 );

            const Reflection::FieldInfo* field = FindField( *object.Type, key );
            if ( field == nullptr )
                luaL_errorL( L, "%s has no field '%s'", object.Type->Name.c_str(), key );

            std::string                why;
            const std::optional<Value> value = ToValue( L, 3, field->Type, why );
            if ( !value )
                luaL_errorL( L, "%s.%s: %s", object.Type->Name.c_str(), key, why.c_str() );
            if ( Common::BoolResultStr written = Reflection::WriteField( *field, Instance( L, object ), *value );
                 !written.IsSuccess() )
                luaL_errorL( L, "%s.%s: %s", object.Type->Name.c_str(), key, written.GetError().c_str() );
            return 0;
        }

        int ToString( lua_State* L )
        {
            const LuauBinding& object = *CheckObject( L, 1 );
            lua_pushstring( L, std::format( "{} '{}'", object.Type->Name, object.Name ).c_str() );
            return 1;
        }

        std::optional<double> Integral( lua_State* L, int index, double low, double high, std::string& why )
        {
            if ( lua_type( L, index ) != LUA_TNUMBER )
            {
                why = std::format( "expected an integer, got {}", lua_typename( L, lua_type( L, index ) ) );
                return std::nullopt;
            }
            const double number = lua_tonumber( L, index );
            if ( std::trunc( number ) != number || number < low || number >= high )
            {
                why = std::format( "{} is not an integer in [{}, {})", number, low, high );
                return std::nullopt;
            }
            return number;
        }

        bool Float4Field( lua_State* L, int table, const char* name, float& out )
        {
            lua_rawgetfield( L, table, name );
            const bool isNumber = lua_type( L, -1 ) == LUA_TNUMBER;
            if ( isNumber )
                out = static_cast<float>( lua_tonumber( L, -1 ) );
            lua_pop( L, 1 );
            return isNumber;
        }

        void BindStaticFunctions( lua_State* L )
        {
            for ( const auto& [name, type] : Reflection::ReflectionRegistry::Get().All() )
            {
                bool any = false;
                for ( const FunctionInfo& function : type.Functions )
                {
                    if ( !function.IsStatic || !function.Meta.ScriptCallable )
                        continue;
                    if ( !any )
                    {
                        lua_getglobal( L, type.Name.c_str() );
                        const bool taken = !lua_isnil( L, -1 );
                        lua_pop( L, 1 );
                        if ( taken )
                        {
                            LOG_ERROR( "Luau: reflected type '{}' is not bound: the global '{}' is a library",
                                       type.Name, type.Name );
                            break;
                        }
                        lua_newtable( L );
                        any = true;
                    }
                    lua_pushlightuserdata( L, const_cast<FunctionInfo*>( &function ) );
                    lua_pushcclosure( L, CallStatic, function.Name.c_str(), 1 );
                    lua_setfield( L, -2, function.Name.c_str() );
                }
                if ( any )
                    lua_setglobal( L, type.Name.c_str() );
            }
        }
    } // namespace

    void Install( lua_State* L )
    {
        lua_setuserdatadtor( L, kObjectTag, []( lua_State*, void* object )
                             { static_cast<LuauBinding*>( object )->~LuauBinding(); } );

        lua_newtable( L );
        lua_pushcfunction( L, Index, "__index" );
        lua_setfield( L, -2, "__index" );
        lua_pushcfunction( L, NewIndex, "__newindex" );
        lua_setfield( L, -2, "__newindex" );
        lua_pushcfunction( L, ToString, "__tostring" );
        lua_setfield( L, -2, "__tostring" );
        lua_pushstring( L, "EngineObject" );
        lua_setfield( L, -2, "__type" );
        lua_pushstring( L, "the metatable of an engine object is locked" );
        lua_setfield( L, -2, "__metatable" );
        lua_setreadonly( L, -1, 1 );
        lua_setuserdatametatable( L, kObjectTag );

        lua_newtable( L );
        lua_setfield( L, LUA_REGISTRYINDEX, kMethodCache );

        BindStaticFunctions( L );
    }

    void PushObject( lua_State* L, const LuauBinding& binding )
    {
        void* memory = lua_newuserdatataggedwithmetatable( L, sizeof( LuauBinding ), kObjectTag );
        new ( memory ) LuauBinding( binding );
    }

    void PushValue( lua_State* L, const Value& value )
    {
        switch ( value.Type() )
        {
            case FieldType::Bool:
                lua_pushboolean( L, *value.Get<bool>() ? 1 : 0 );
                return;
            case FieldType::Int:
                lua_pushnumber( L, static_cast<double>( *value.Get<std::int64_t>() ) );
                return;
            case FieldType::UInt:
                lua_pushnumber( L, static_cast<double>( *value.Get<std::uint64_t>() ) );
                return;
            case FieldType::Enum:
                lua_pushnumber( L, static_cast<double>( value.Get<Value::EnumBits>()->Bits ) );
                return;
            case FieldType::Float:
                lua_pushnumber( L, *value.Get<float>() );
                return;
            case FieldType::Double:
                lua_pushnumber( L, *value.Get<double>() );
                return;
            case FieldType::String:
            {
                const std::string& text = *value.Get<std::string>();
                lua_pushlstring( L, text.data(), text.size() );
                return;
            }
            case FieldType::Vec2:
            {
                const Value::Float2& v = *value.Get<Value::Float2>();
                lua_pushvector( L, v[0], v[1], 0.0f );
                return;
            }
            case FieldType::Vec3:
            {
                const Value::Float3& v = *value.Get<Value::Float3>();
                lua_pushvector( L, v[0], v[1], v[2] );
                return;
            }
            case FieldType::Vec4:
            {
                const Value::Float4& v = *value.Get<Value::Float4>();
                lua_createtable( L, 0, 4 );
                const char* names[] = { "x", "y", "z", "w" };
                for ( int i = 0; i < 4; ++i )
                {
                    lua_pushnumber( L, v[i] );
                    lua_setfield( L, -2, names[i] );
                }
                return;
            }
            case FieldType::Unknown:
            case FieldType::Struct:
            case FieldType::AssetHandle:
                break;
        }
        lua_pushnil( L );
    }

    std::optional<Value> ToValue( lua_State* L, int index, FieldType expected, std::string& why )
    {
        const int type = lua_type( L, index );
        auto      got  = [&]( const char* wanted )
        {
            why = std::format( "expected {}, got {}", wanted, lua_typename( L, type ) );
            return std::nullopt;
        };

        switch ( expected )
        {
            case FieldType::Bool:
                if ( type != LUA_TBOOLEAN )
                    return got( "a boolean" );
                return Value::Bool( lua_toboolean( L, index ) != 0 );
            case FieldType::Int:
            case FieldType::Enum:
            {
                const std::optional<double> number =
                     Integral( L, index, -9223372036854775808.0, 9223372036854775808.0, why );
                if ( !number )
                    return std::nullopt;
                const auto bits = static_cast<std::int64_t>( *number );
                return expected == FieldType::Int ? Value::Int( bits ) : Value::Enum( bits );
            }
            case FieldType::UInt:
            {
                const std::optional<double> number = Integral( L, index, 0.0, 18446744073709551616.0, why );
                if ( !number )
                    return std::nullopt;
                return Value::UInt( static_cast<std::uint64_t>( *number ) );
            }
            case FieldType::Float:
                if ( type != LUA_TNUMBER )
                    return got( "a number" );
                return Value::Float( static_cast<float>( lua_tonumber( L, index ) ) );
            case FieldType::Double:
                if ( type != LUA_TNUMBER )
                    return got( "a number" );
                return Value::Double( lua_tonumber( L, index ) );
            case FieldType::String:
            {
                if ( type != LUA_TSTRING )
                    return got( "a string" );
                std::size_t length = 0;
                const char* text   = lua_tolstring( L, index, &length );
                return Value::String( std::string( text, length ) );
            }
            case FieldType::Vec2:
            case FieldType::Vec3:
            {
                if ( type != LUA_TVECTOR )
                    return got( "a vector" );
                const float* v = lua_tovector( L, index );
                if ( expected == FieldType::Vec2 )
                    return Value::Vec2( { v[0], v[1] } );
                return Value::Vec3( { v[0], v[1], v[2] } );
            }
            case FieldType::Vec4:
            {
                if ( type != LUA_TTABLE )
                    return got( "a table {x, y, z, w}" );
                const int     table = lua_absindex( L, index );
                Value::Float4 v{};
                if ( !Float4Field( L, table, "x", v[0] ) || !Float4Field( L, table, "y", v[1] ) ||
                     !Float4Field( L, table, "z", v[2] ) || !Float4Field( L, table, "w", v[3] ) )
                {
                    why = "expected a table with numbers x, y, z, w";
                    return std::nullopt;
                }
                return Value::Vec4( v );
            }
            case FieldType::Unknown:
            case FieldType::Struct:
            case FieldType::AssetHandle:
                break;
        }
        why = std::format( "a {} has no script form", Reflection::FieldTypeName( expected ) );
        return std::nullopt;
    }
} // namespace Desert::Scripting::LuauBinder
