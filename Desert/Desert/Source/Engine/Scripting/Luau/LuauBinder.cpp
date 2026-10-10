#include <Engine/Scripting/Luau/LuauBinder.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/Reflection/FieldValue.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <format>
#include <new>
#include <vector>

#include <VM/include/lualib.h>

#include <entt/entt.hpp>

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

        /// The userdata tag of a container field's list view (ContainerView).
        constexpr int kContainerTag = 2;

        /// The light-userdata tag of an asset handle: its 64-bit id rides in the pointer bits, because a Luau
        /// number is a double and would round an id above 2^53. Compared by ==, passed back to a handle field.
        constexpr int kAssetHandleTag = 1;
        static_assert( sizeof( void* ) == sizeof( std::uint64_t ), "an asset handle rides in a pointer" );

        /// A std::vector field seen as a 1-based list: the owner it lives in (resolved on every access) and
        /// the field's codegen'd element accessors.
        struct ContainerView
        {
            LuauBinding                  Owner;
            const Reflection::FieldInfo* Field = nullptr;
        };

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
                luaL_errorL( L, "the %s bound as '%s' is gone",
                             object.Type != nullptr ? object.Type->Name.c_str() : "entity", object.Name.c_str() );
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
            if ( object == nullptr || object->Type == nullptr || object->Type->Name != function->Owner )
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

        void Touched( const LuauBinding& object )
        {
            if ( object.Changed )
                object.Changed();
        }

        void PushHandle( lua_State* L, std::uint64_t id )
        {
            lua_pushlightuserdatatagged( L, reinterpret_cast<void*>( static_cast<std::uintptr_t>( id ) ),
                                         kAssetHandleTag );
        }

        /// A value of category `type` (a field's or a container element's) in its Luau form.
        void PushOf( lua_State* L, FieldType type, const Value& value )
        {
            if ( type == FieldType::AssetHandle && value.Type() == FieldType::UInt )
                PushHandle( L, *value.Get<std::uint64_t>() );
            else
                PushValue( L, value );
        }

        /// A Struct field as an object of its own: same owner, resolved at the owner's address + Offset.
        LuauBinding Nested( const LuauBinding& owner, const Reflection::FieldInfo& field )
        {
            LuauBinding nested;
            nested.Name    = std::format( "{}.{}", owner.Name, field.Name );
            nested.Type    = field.StructType;
            nested.Resolve = [resolve = owner.Resolve, offset = field.Offset]() -> void*
            {
                void* base = resolve ? resolve() : nullptr;
                return base != nullptr ? static_cast<char*>( base ) + offset : nullptr;
            };
            nested.Changed = owner.Changed;
            return nested;
        }

        void PushField( lua_State* L, const LuauBinding& object, const Reflection::FieldInfo& field )
        {
            const char* owner = object.Type->Name.c_str();
            if ( field.IsContainer )
            {
                if ( field.ContainerGet == nullptr )
                    luaL_errorL( L, "%s.%s is a container without element accessors", owner, field.Name.c_str() );
                Instance( L, object ); // a gone owner is an error now, not at the first element
                void* memory = lua_newuserdatataggedwithmetatable( L, sizeof( ContainerView ), kContainerTag );
                new ( memory ) ContainerView{ object, &field };
                return;
            }
            if ( field.Type == FieldType::Struct )
            {
                if ( field.StructType == nullptr )
                    luaL_errorL( L, "%s.%s is a struct with no reflected type", owner, field.Name.c_str() );
                Instance( L, object );
                PushObject( L, Nested( object, field ) );
                return;
            }
            Common::ResultStr<Value> value = Reflection::ReadField( field, Instance( L, object ) );
            if ( !value.IsSuccess() )
                luaL_errorL( L, "%s", value.GetError().c_str() );
            PushOf( L, field.Type, value.GetValue() );
        }

        /// Replaces the whole vector with the sequence t[1..#t] at `table`. All or nothing: every element is
        /// converted first, and a range refusal after the resize puts the old elements back.
        bool AssignAll( lua_State* L, int table, const Reflection::FieldInfo& field, void* at, std::string& why )
        {
            const int          count = lua_objlen( L, table );
            std::vector<Value> incoming;
            incoming.reserve( static_cast<std::size_t>( count ) );
            for ( int i = 1; i <= count; ++i )
            {
                lua_rawgeti( L, table, i );
                std::optional<Value> element = ToValue( L, -1, field.ElementType, why );
                lua_pop( L, 1 );
                if ( !element )
                {
                    why = std::format( "element {}: {}", i, why );
                    return false;
                }
                incoming.push_back( std::move( *element ) );
            }

            const std::size_t  previousCount = field.ContainerSize( at );
            std::vector<Value> previous;
            previous.reserve( previousCount );
            for ( std::size_t i = 0; i < previousCount; ++i )
                previous.push_back( field.ContainerGet( at, i ) );

            field.ContainerResize( at, incoming.size() );
            for ( std::size_t i = 0; i < incoming.size(); ++i )
            {
                if ( field.ContainerSet( at, i, incoming[i] ) )
                    continue;
                field.ContainerResize( at, previousCount );
                for ( std::size_t j = 0; j < previousCount; ++j )
                    (void)field.ContainerSet( at, j, previous[j] );
                why = std::format( "element {} does not fit the element type", i + 1 );
                return false;
            }
            return true;
        }

        ContainerView& CheckView( lua_State* L, int index )
        {
            auto* view = static_cast<ContainerView*>( lua_touserdatatagged( L, index, kContainerTag ) );
            if ( view == nullptr )
                luaL_errorL( L, "expected a container field, got %s", lua_typename( L, lua_type( L, index ) ) );
            return *view;
        }

        void* ViewField( lua_State* L, const ContainerView& view )
        {
            return static_cast<char*>( Instance( L, view.Owner ) ) + view.Field->Offset;
        }

        int ViewIndex( lua_State* L )
        {
            const ContainerView& view  = CheckView( L, 1 );
            const int            index = luaL_checkinteger( L, 2 );
            void*                at    = ViewField( L, view );
            const std::size_t    count = view.Field->ContainerSize( at );
            if ( index < 1 || static_cast<std::size_t>( index ) > count )
            {
                lua_pushnil( L );
                return 1;
            }
            PushOf( L, view.Field->ElementType,
                    view.Field->ContainerGet( at, static_cast<std::size_t>( index - 1 ) ) );
            return 1;
        }

        /// v[i] = x writes an element; v[#v + 1] = x appends; v[#v] = nil removes the last one. Nothing else
        /// resizes, so a typo in an index is an error, not a hole.
        int ViewNewIndex( lua_State* L )
        {
            const ContainerView&         view  = CheckView( L, 1 );
            const Reflection::FieldInfo& field = *view.Field;
            const char*                  owner = view.Owner.Type->Name.c_str();
            const int                    index = luaL_checkinteger( L, 2 );
            if ( field.Meta.ReadOnly )
                luaL_errorL( L, "%s.%s is read-only", owner, field.Name.c_str() );

            void*             at    = ViewField( L, view );
            const std::size_t count = field.ContainerSize( at );
            if ( lua_isnil( L, 3 ) )
            {
                if ( count == 0 || static_cast<std::size_t>( index ) != count )
                    luaL_errorL( L, "%s.%s: only the last element (%d) can be removed", owner, field.Name.c_str(),
                                 static_cast<int>( count ) );
                field.ContainerResize( at, count - 1 );
                Touched( view.Owner );
                return 0;
            }
            if ( index < 1 || static_cast<std::size_t>( index ) > count + 1 )
                luaL_errorL( L, "%s.%s: index %d is outside 1..%d", owner, field.Name.c_str(), index,
                             static_cast<int>( count + 1 ) );

            std::string                why;
            const std::optional<Value> value = ToValue( L, 3, field.ElementType, why );
            if ( !value )
                luaL_errorL( L, "%s.%s[%d]: %s", owner, field.Name.c_str(), index, why.c_str() );
            const bool grows = static_cast<std::size_t>( index ) == count + 1;
            if ( grows )
                field.ContainerResize( at, count + 1 );
            if ( !field.ContainerSet( at, static_cast<std::size_t>( index - 1 ), *value ) )
            {
                if ( grows )
                    field.ContainerResize( at, count );
                luaL_errorL( L, "%s.%s[%d]: the value does not fit the element type", owner, field.Name.c_str(),
                             index );
            }
            Touched( view.Owner );
            return 0;
        }

        int ViewLength( lua_State* L )
        {
            const ContainerView& view = CheckView( L, 1 );
            lua_pushinteger( L, static_cast<int>( view.Field->ContainerSize( ViewField( L, view ) ) ) );
            return 1;
        }

        /// The component row an entity method names by `key` (argument 2), on a live entity — or a Luau error:
        /// an unknown key is an error (a typo must not read as "absent"), as is a gone entity.
        const ECS::ReflectedComponent& EntityRow( lua_State* L, const char* method, LuauEntityRef& ref )
        {
            const LuauBinding& object = *CheckObject( L, 1 );
            const char*        key    = luaL_checkstring( L, 2 );
            if ( object.Type != nullptr || !object.Entity )
                luaL_errorL( L, "%s() is a method of an entity: call it as entity:%s(\"%s\")", method, method,
                             key );

            const ECS::ReflectedComponent* row = ECS::FindReflectedComponent( key );
            if ( row == nullptr )
                luaL_errorL( L, "no component is reachable by the key '%s'", key );
            if ( row->Type() == nullptr )
                luaL_errorL( L, "the component '%s' has no reflected type", key );

            ref = object.Entity();
            if ( ref.Registry == nullptr || !ref.Registry->valid( ref.Entity ) )
                luaL_errorL( L, "the entity bound as '%s' is gone", object.Name.c_str() );
            return *row;
        }

        /// entity:component(key) — the component by its record key (ECS::ReflectedComponents), nil when the
        /// entity has none.
        int EntityComponent( lua_State* L )
        {
            LuauEntityRef                  ref;
            const ECS::ReflectedComponent& row    = EntityRow( L, "component", ref );
            const LuauBinding&             object = *CheckObject( L, 1 );
            if ( !row.Has( *ref.Registry, ref.Entity ) )
            {
                lua_pushnil( L );
                return 1;
            }
            PushObject( L, ComponentBinding( std::format( "{}:{}", object.Name, row.Name ), object.Entity, row ) );
            return 1;
        }

        /// entity:has(key) — whether the entity carries the component.
        int EntityHas( lua_State* L )
        {
            LuauEntityRef                  ref;
            const ECS::ReflectedComponent& row = EntityRow( L, "has", ref );
            lua_pushboolean( L, row.Has( *ref.Registry, ref.Entity ) ? 1 : 0 );
            return 1;
        }

        /// entity:add(key) — adds the component default-constructed (kept when present) and returns it.
        int EntityAdd( lua_State* L )
        {
            LuauEntityRef                  ref;
            const ECS::ReflectedComponent& row = EntityRow( L, "add", ref );
            row.Add( *ref.Registry, ref.Entity );
            return EntityComponent( L );
        }

        /// entity:remove(key) — removes the component (no-op when absent).
        int EntityRemove( lua_State* L )
        {
            LuauEntityRef                  ref;
            const ECS::ReflectedComponent& row = EntityRow( L, "remove", ref );
            row.Remove( *ref.Registry, ref.Entity );
            return 0;
        }

        int Index( lua_State* L )
        {
            const LuauBinding& object = *CheckObject( L, 1 );
            const char*        key    = luaL_checkstring( L, 2 );

            if ( object.Type == nullptr )
            {
                for ( const auto& [name, method] :
                      { std::pair{ "component", &EntityComponent }, std::pair{ "has", &EntityHas },
                        std::pair{ "add", &EntityAdd }, std::pair{ "remove", &EntityRemove } } )
                {
                    if ( std::strcmp( key, name ) == 0 )
                    {
                        lua_pushcfunction( L, method, name );
                        return 1;
                    }
                }
                // The engine's own entity methods (destroy, call, move, ...) — installed by the host module.
                lua_getfield( L, LUA_REGISTRYINDEX, kEntityMethods );
                if ( lua_istable( L, -1 ) )
                {
                    lua_getfield( L, -1, key );
                    if ( lua_isfunction( L, -1 ) )
                        return 1;
                }
                luaL_errorL( L, "an entity has no field or method '%s' (its data is entity:component(key))", key );
            }
            if ( const Reflection::FieldInfo* field = FindField( *object.Type, key ) )
            {
                PushField( L, object, *field );
                return 1;
            }
            if ( const FunctionInfo* function = object.Type->FindBoundFunction( key );
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
            if ( object.Type == nullptr )
                luaL_errorL( L, "an entity has no field '%s' (its data is entity:component(key))", key );
            const char* owner = object.Type->Name.c_str();

            const Reflection::FieldInfo* field = FindField( *object.Type, key );
            if ( field == nullptr )
                luaL_errorL( L, "%s has no field '%s'", owner, key );
            if ( field->Type == FieldType::Struct && !field->IsContainer )
                luaL_errorL( L, "%s.%s is a struct: write its fields (obj.%s.Field = ...)", owner, key, key );

            std::string why;
            if ( field->IsContainer )
            {
                if ( field->ContainerSet == nullptr )
                    luaL_errorL( L, "%s.%s is a container without element accessors", owner, key );
                if ( field->Meta.ReadOnly )
                    luaL_errorL( L, "%s.%s is read-only", owner, key );
                luaL_checktype( L, 3, LUA_TTABLE );
                void* at = static_cast<char*>( Instance( L, object ) ) + field->Offset;
                if ( !AssignAll( L, 3, *field, at, why ) )
                    luaL_errorL( L, "%s.%s: %s", owner, key, why.c_str() );
                Touched( object );
                return 0;
            }

            const std::optional<Value> value = ToValue( L, 3, field->Type, why );
            if ( !value )
                luaL_errorL( L, "%s.%s: %s", owner, key, why.c_str() );
            if ( Common::BoolResultStr written = Reflection::WriteField( *field, Instance( L, object ), *value );
                 !written.IsSuccess() )
                luaL_errorL( L, "%s.%s: %s", owner, key, written.GetError().c_str() );
            Touched( object );
            return 0;
        }

        int ToString( lua_State* L )
        {
            const LuauBinding& object = *CheckObject( L, 1 );
            lua_pushstring(
                 L, std::format( "{} '{}'", object.Type != nullptr ? object.Type->Name : "entity", object.Name )
                         .c_str() );
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

        /// Pops the function on top of the stack into the entity methods table as `name`.
        void SetEntityMethodValue( lua_State* L, const char* name )
        {
            lua_getfield( L, LUA_REGISTRYINDEX, kEntityMethods );
            if ( !lua_istable( L, -1 ) )
            {
                lua_pop( L, 1 );
                lua_newtable( L );
                lua_pushvalue( L, -1 );
                lua_setfield( L, LUA_REGISTRYINDEX, kEntityMethods );
            }
            lua_insert( L, -2 );
            lua_setfield( L, -2, name );
            lua_pop( L, 1 );
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
                        lua_getglobal( L, type.BoundName().c_str() );
                        const bool taken = !lua_isnil( L, -1 );
                        lua_pop( L, 1 );
                        if ( taken )
                        {
                            LOG_ERROR( "Luau: reflected type '{}' is not bound: the global '{}' is a library",
                                       type.Name, type.BoundName() );
                            break;
                        }
                        lua_newtable( L );
                        any = true;
                    }
                    lua_pushlightuserdata( L, const_cast<FunctionInfo*>( &function ) );
                    lua_pushcclosure( L, CallStatic, function.BoundName().c_str(), 1 );
                    if ( function.Meta.ScriptMethod )
                    {
                        // UE's ScriptMethod: the static's first parameter is the entity, so `entity:name(...)`
                        // passes the object itself as that argument.
                        if ( function.Params.empty() || function.Params[0].Type != FieldType::Entity )
                        {
                            LOG_ERROR( "Luau: {}.{} is a ScriptMethod but its first parameter is not an entity",
                                       type.Name, function.Name );
                        }
                        else
                        {
                            lua_pushvalue( L, -1 );
                            SetEntityMethodValue( L, function.BoundName().c_str() );
                        }
                    }
                    lua_setfield( L, -2, function.BoundName().c_str() );
                }
                if ( any )
                    lua_setglobal( L, type.BoundName().c_str() );
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

        lua_setuserdatadtor( L, kContainerTag, []( lua_State*, void* view )
                             { static_cast<ContainerView*>( view )->~ContainerView(); } );
        lua_newtable( L );
        lua_pushcfunction( L, ViewIndex, "__index" );
        lua_setfield( L, -2, "__index" );
        lua_pushcfunction( L, ViewNewIndex, "__newindex" );
        lua_setfield( L, -2, "__newindex" );
        lua_pushcfunction( L, ViewLength, "__len" );
        lua_setfield( L, -2, "__len" );
        lua_pushstring( L, "EngineList" );
        lua_setfield( L, -2, "__type" );
        lua_pushstring( L, "the metatable of an engine list is locked" );
        lua_setfield( L, -2, "__metatable" );
        lua_setreadonly( L, -1, 1 );
        lua_setuserdatametatable( L, kContainerTag );

        lua_newtable( L );
        lua_setfield( L, LUA_REGISTRYINDEX, kMethodCache );

        BindStaticFunctions( L );
    }

    void SetEntityMethod( lua_State* L, const char* name, lua_CFunction method )
    {
        lua_pushcfunction( L, method, name );
        SetEntityMethodValue( L, name );
    }

    std::optional<LuauEntityRef> ToEntity( lua_State* L, int index )
    {
        auto* object = static_cast<LuauBinding*>( lua_touserdatatagged( L, index, kObjectTag ) );
        if ( object == nullptr || object->Type != nullptr || !object->Entity )
            return std::nullopt;
        return object->Entity();
    }

    LuauEntityRef CheckEntity( lua_State* L, int index )
    {
        std::optional<LuauEntityRef> ref = ToEntity( L, index );
        if ( !ref )
            luaL_typeerrorL( L, index, "entity" );
        if ( ref->Registry == nullptr || !ref->Registry->valid( ref->Entity ) )
            luaL_errorL( L, "argument #%d: the entity is gone", index );
        return *ref;
    }

    void PushEntity( lua_State* L, entt::registry& registry, entt::entity entity )
    {
        if ( entity == entt::null || !registry.valid( entity ) )
        {
            lua_pushnil( L );
            return;
        }
        entt::registry* owner = &registry;
        PushObject( L, EntityBinding( std::format( "entity {}", static_cast<std::uint32_t>( entity ) ),
                                      [owner, entity]() { return LuauEntityRef{ owner, entity }; } ) );
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
            case FieldType::Entity:
            {
                const Value::EntityRef& ref = *value.Get<Value::EntityRef>();
                if ( ref.World == nullptr )
                    break;
                PushEntity( L, *static_cast<entt::registry*>( ref.World ), static_cast<entt::entity>( ref.Id ) );
                return;
            }
            case FieldType::Unknown:
            case FieldType::Struct:
            case FieldType::AssetHandle:
            case FieldType::Any:
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
            case FieldType::AssetHandle:
                // The handle a read returned (kAssetHandleTag); its id travels as the UInt WriteField takes.
                if ( type != LUA_TLIGHTUSERDATA || lua_lightuserdatatag( L, index ) != kAssetHandleTag )
                    return got( "an asset handle" );
                return Value::UInt( static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>(
                     lua_tolightuserdatatagged( L, index, kAssetHandleTag ) ) ) );
            case FieldType::Entity:
            {
                const std::optional<LuauEntityRef> ref = ToEntity( L, index );
                if ( !ref )
                    return got( "an entity" );
                if ( ref->Registry == nullptr || !ref->Registry->valid( ref->Entity ) )
                {
                    why = "the entity is gone";
                    return std::nullopt;
                }
                return Value::Entity( { ref->Registry, static_cast<std::uint32_t>( ref->Entity ) } );
            }
            case FieldType::Any:
                // The language's own kind decides: a number is a Double, a vector a Vec3, an entity an Entity.
                switch ( type )
                {
                    case LUA_TBOOLEAN:
                        return ToValue( L, index, FieldType::Bool, why );
                    case LUA_TNUMBER:
                        return Value::Double( lua_tonumber( L, index ) );
                    case LUA_TSTRING:
                        return ToValue( L, index, FieldType::String, why );
                    case LUA_TVECTOR:
                        return ToValue( L, index, FieldType::Vec3, why );
                    case LUA_TUSERDATA:
                        return ToValue( L, index, FieldType::Entity, why );
                    case LUA_TNIL:
                        return Value();
                    default:
                        return got( "a boolean, number, string, vector or entity" );
                }
            case FieldType::Unknown:
            case FieldType::Struct:
                break;
        }
        why = std::format( "a {} has no script form", Reflection::FieldTypeName( expected ) );
        return std::nullopt;
    }
} // namespace Desert::Scripting::LuauBinder

namespace Desert::Scripting
{
    LuauBinding ComponentBinding( std::string name, std::function<LuauEntityRef()> entity,
                                  const ECS::ReflectedComponent& row )
    {
        // Every access re-resolves the entity: the binding never holds the registry, the entity or the data.
        auto present = [entity, &row]() -> LuauEntityRef
        {
            LuauEntityRef ref = entity ? entity() : LuauEntityRef{};
            if ( ref.Registry == nullptr || !ref.Registry->valid( ref.Entity ) ||
                 !row.Has( *ref.Registry, ref.Entity ) )
                return {};
            return ref;
        };

        LuauBinding binding;
        binding.Name    = std::move( name );
        binding.Type    = row.Type();
        binding.Resolve = [present, &row]() -> void*
        {
            const LuauEntityRef ref = present();
            return ref.Registry != nullptr ? row.Data( *ref.Registry, ref.Entity ) : nullptr;
        };
        binding.Changed = [present, &row]
        {
            if ( const LuauEntityRef ref = present(); ref.Registry != nullptr )
                row.NotifyChanged( *ref.Registry, ref.Entity );
        };
        return binding;
    }

    LuauBinding EntityBinding( std::string name, std::function<LuauEntityRef()> resolve )
    {
        LuauBinding binding;
        binding.Name   = std::move( name );
        binding.Entity = std::move( resolve );
        return binding;
    }
} // namespace Desert::Scripting
