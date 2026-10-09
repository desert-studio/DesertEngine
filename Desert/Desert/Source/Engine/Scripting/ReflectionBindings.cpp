#include "Internal/ScriptRuntime.hpp"

#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/Value.hpp>

#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace Desert::Scripting
{
    // ── AUTO-GENERATED component access from reflection ────────────────────────────────────
    //
    // Zero per-field binding code: any PROPERTY()-reflected component data block is readable and
    // writable from Lua through a proxy driven by the ReflectionRegistry (the same metadata that
    // powers the Details editor and serialization):
    //
    //     local light = self:component("PointLight")
    //     if light then
    //         light.Intensity = light.Intensity * 0.5
    //         light.Radius    = 10
    //         light.Color     = { x = 1, y = 0.5, z = 0.2 }   -- vec fields <-> tables
    //     end
    //
    //         light.Shadow.Bias = 0.01                       -- a Struct field is a nested proxy
    //
    // Adding a PROPERTY field to a component makes it scriptable automatically; a component is reachable by
    // name exactly when its block is in the serializer's list (ECS/ReflectedComponents.hpp), under its record
    // key. An asset-handle field reads and writes as its integer handle. A write that lands ends in the
    // component's on_update signal (ReflectedComponent::NotifyChanged).
    //
    // A std::vector field reads as a list view over the live vector (FieldInfo::ContainerGet/Set, codegen'd):
    //
    //     local d = self:component("Destructible")
    //     local n = #d.DamageThreshold                     -- length
    //     d.DamageThreshold[1] = 500                       -- element write (1-based, as Lua)
    //     d.DamageThreshold[n + 1] = 900                   -- one past the end appends
    //     d.DamageThreshold[#d.DamageThreshold] = nil      -- nil at the last index removes it
    //     d.AnchoredNodes = { 0, 4, 7 }                    -- a whole table replaces the vector
    //
    // An element of the wrong kind, an index outside 1..n+1, a fractional or out-of-range integer: nothing is
    // written, and a table with one such element writes none of them.

    namespace
    {
        // One container element, Value -> Lua. An asset handle is its 64-bit id's bits, as a scalar handle field.
        sol::object ElementToLua( sol::state_view lua, Reflection::FieldType elementType,
                                  const Reflection::Value& v )
        {
            using FT = Reflection::FieldType;
            switch ( v.Type() )
            {
                case FT::Bool:
                    return sol::make_object( lua, *v.Get<bool>() );
                case FT::Int:
                    return sol::make_object( lua, *v.Get<std::int64_t>() );
                case FT::UInt:
                    if ( elementType == FT::AssetHandle )
                        return sol::make_object( lua, std::bit_cast<std::int64_t>( *v.Get<std::uint64_t>() ) );
                    return sol::make_object( lua, *v.Get<std::uint64_t>() );
                case FT::Float:
                    return sol::make_object( lua, *v.Get<float>() );
                case FT::Double:
                    return sol::make_object( lua, *v.Get<double>() );
                case FT::String:
                    return sol::make_object( lua, *v.Get<std::string>() );
                default:
                    return sol::lua_nil;
            }
        }

        // Lua -> one container element of `elementType`, or nothing when the Lua value is not of that kind. An
        // integer element takes a whole number only (3.5 is refused, not truncated); its range is checked by
        // ContainerSet against the element's C++ type.
        std::optional<Reflection::Value> ElementFromLua( Reflection::FieldType elementType, const sol::object& o )
        {
            using FT    = Reflection::FieldType;
            using Value = Reflection::Value;
            switch ( elementType )
            {
                case FT::Bool:
                    if ( o.is<bool>() )
                        return Value::Bool( o.as<bool>() );
                    return std::nullopt;
                case FT::Int:
                case FT::UInt:
                {
                    if ( o.get_type() != sol::type::number )
                        return std::nullopt;
                    const double d = o.as<double>();
                    if ( std::trunc( d ) != d )
                        return std::nullopt;
                    if ( elementType == FT::Int )
                        return Value::Int( static_cast<std::int64_t>( d ) );
                    if ( d < 0.0 )
                        return std::nullopt;
                    return Value::UInt( static_cast<std::uint64_t>( d ) );
                }
                case FT::Float:
                    if ( o.get_type() == sol::type::number )
                        return Value::Float( static_cast<float>( o.as<double>() ) );
                    return std::nullopt;
                case FT::Double:
                    if ( o.get_type() == sol::type::number )
                        return Value::Double( o.as<double>() );
                    return std::nullopt;
                case FT::String:
                    if ( o.get_type() == sol::type::string )
                        return Value::String( o.as<std::string>() );
                    return std::nullopt;
                case FT::AssetHandle:
                    if ( o.get_type() == sol::type::number )
                        return Value::UInt(
                             std::bit_cast<std::uint64_t>( o.as<std::int64_t>() ) ); // the bits a read returned
                    return std::nullopt;
                default:
                    return std::nullopt;
            }
        }

        // Replaces the whole vector with a Lua sequence (t[1..#t]). All or nothing: every element is converted
        // first; a range refusal by ContainerSet after the resize puts the old elements back.
        bool AssignContainer( const Reflection::FieldInfo& f, void* field, const sol::table& t )
        {
            std::vector<Reflection::Value> incoming;
            const std::size_t              count = t.size();
            incoming.reserve( count );
            for ( std::size_t i = 1; i <= count; ++i )
            {
                auto v = ElementFromLua( f.ElementType, t.get<sol::object>( i ) );
                if ( !v )
                    return false;
                incoming.push_back( std::move( *v ) );
            }
            std::vector<Reflection::Value> previous;
            const std::size_t              oldCount = f.ContainerSize( field );
            previous.reserve( oldCount );
            for ( std::size_t i = 0; i < oldCount; ++i )
                previous.push_back( f.ContainerGet( field, i ) );

            f.ContainerResize( field, count );
            for ( std::size_t i = 0; i < count; ++i )
            {
                if ( f.ContainerSet( field, i, incoming[i] ) )
                    continue;
                f.ContainerResize( field, oldCount );
                for ( std::size_t k = 0; k < oldCount; ++k )
                    (void)f.ContainerSet( field, k, previous[k] ); // values it held: cannot be refused
                return false;
            }
            return true;
        }

        // Lua-side view of one std::vector field of one component on one entity: the live vector, never a copy,
        // so a write through it is a write to the component (and fires its on_update).
        struct ContainerProxy
        {
            entt::entity                   handle    = entt::null;
            Core::Scene*                   scene     = nullptr;
            const ECS::ReflectedComponent* component = nullptr;
            const Reflection::FieldInfo*   field     = nullptr;
            std::size_t                    offset    = 0; // the vector's offset within the component's data block

            bool Valid() const
            {
                return scene && component && field && handle != entt::null &&
                       scene->GetRegistry().valid( handle ) && component->Has( scene->GetRegistry(), handle );
            }

            void* FieldPtr() const
            {
                return static_cast<char*>( component->Data( scene->GetRegistry(), handle ) ) + offset;
            }

            std::size_t Length() const
            {
                return Valid() ? field->ContainerSize( FieldPtr() ) : 0;
            }

            // A Lua index (1-based, whole) as a 0-based one; nothing for any other key.
            static std::optional<std::size_t> ToIndex( const sol::object& key )
            {
                if ( key.get_type() != sol::type::number )
                    return std::nullopt;
                const double d = key.as<double>();
                if ( d < 1.0 || std::trunc( d ) != d )
                    return std::nullopt;
                return static_cast<std::size_t>( d ) - 1;
            }

            sol::object Index( sol::this_state ts, const sol::object& key ) const
            {
                const auto index = ToIndex( key );
                if ( !Valid() || !index )
                    return sol::lua_nil;
                return ElementToLua( sol::state_view( ts ), field->ElementType,
                                     field->ContainerGet( FieldPtr(), *index ) );
            }

            void NewIndex( const sol::object& key, const sol::object& value )
            {
                const auto index = ToIndex( key );
                if ( !Valid() || !index || field->Meta.ReadOnly )
                    return;
                void*             p     = FieldPtr();
                const std::size_t count = field->ContainerSize( p );
                bool              wrote = false;
                if ( value.get_type() == sol::type::lua_nil )
                {
                    if ( count > 0 && *index == count - 1 ) // nil at the last index removes it
                    {
                        field->ContainerResize( p, count - 1 );
                        wrote = true;
                    }
                }
                else if ( auto v = ElementFromLua( field->ElementType, value ); v && *index <= count )
                {
                    if ( *index == count ) // one past the end appends
                    {
                        field->ContainerResize( p, count + 1 );
                        wrote = field->ContainerSet( p, *index, *v );
                        if ( !wrote )
                            field->ContainerResize( p, count );
                    }
                    else
                        wrote = field->ContainerSet( p, *index, *v );
                }
                if ( wrote )
                    component->NotifyChanged( scene->GetRegistry(), handle );
            }
        };

        // Lua-side view of one reflected struct inside one component on one entity: the component's data block
        // itself (Base 0), or a Struct field nested in it (Base = its offset in the block). The component row
        // comes from ECS::ReflectedComponents, the one component<->type table (the serializer's list).
        struct ComponentProxy
        {
            entt::entity                   handle    = entt::null;
            Core::Scene*                   scene     = nullptr;
            const ECS::ReflectedComponent* component = nullptr;
            const Reflection::TypeInfo*    type      = nullptr; // the struct this proxy views
            std::size_t                    base      = 0;       // its offset within the component's data block

            bool Valid() const
            {
                return scene && component && type && handle != entt::null &&
                       scene->GetRegistry().valid( handle ) && component->Has( scene->GetRegistry(), handle );
            }

            void* DataPtr() const
            {
                return static_cast<char*>( component->Data( scene->GetRegistry(), handle ) ) + base;
            }

            const Reflection::FieldInfo* FindField( const std::string& name ) const
            {
                for ( const auto& f : type->Fields )
                    if ( f.Name == name )
                        return &f;
                return nullptr;
            }

            // An integer field of `size` bytes (enums are stored at their underlying width).
            static int64_t ReadInt( const char* p, std::size_t size )
            {
                switch ( size )
                {
                    case 1:
                    {
                        int8_t v;
                        std::memcpy( &v, p, 1 );
                        return v;
                    }
                    case 2:
                    {
                        int16_t v;
                        std::memcpy( &v, p, 2 );
                        return v;
                    }
                    case 8:
                    {
                        int64_t v;
                        std::memcpy( &v, p, 8 );
                        return v;
                    }
                    default:
                    {
                        int32_t v;
                        std::memcpy( &v, p, 4 );
                        return v;
                    }
                }
            }

            static void WriteInt( char* p, std::size_t size, int64_t value )
            {
                switch ( size )
                {
                    case 1:
                    {
                        const auto v = static_cast<int8_t>( value );
                        std::memcpy( p, &v, 1 );
                        break;
                    }
                    case 2:
                    {
                        const auto v = static_cast<int16_t>( value );
                        std::memcpy( p, &v, 2 );
                        break;
                    }
                    case 8:
                    {
                        std::memcpy( p, &value, 8 );
                        break;
                    }
                    default:
                    {
                        const auto v = static_cast<int32_t>( value );
                        std::memcpy( p, &v, 4 );
                        break;
                    }
                }
            }

            sol::object Index( sol::this_state ts, const std::string& fieldName ) const
            {
                if ( !Valid() )
                    return sol::lua_nil;
                const auto* f = FindField( fieldName );
                if ( !f )
                    return sol::lua_nil;

                sol::state_view lua( ts );
                if ( f->IsContainer ) // a std::vector is not a scalar at Offset: a list view over it, never
                                      // reinterpreted
                    return sol::make_object( lua,
                                             ContainerProxy{ handle, scene, component, f, base + f->Offset } );
                const char*     p = static_cast<const char*>( DataPtr() ) + f->Offset;
                using FT          = Reflection::FieldType;
                switch ( f->Type )
                {
                    case FT::Bool:   return sol::make_object( lua, *reinterpret_cast<const bool*>( p ) );
                    case FT::Int:    return sol::make_object( lua, *reinterpret_cast<const int32_t*>( p ) );
                    case FT::UInt:   return sol::make_object( lua, *reinterpret_cast<const uint32_t*>( p ) );
                    case FT::Float:  return sol::make_object( lua, *reinterpret_cast<const float*>( p ) );
                    case FT::Double: return sol::make_object( lua, *reinterpret_cast<const double*>( p ) );
                    case FT::String:
                        return sol::make_object( lua, *reinterpret_cast<const std::string*>( p ) );
                    case FT::Enum:
                        return sol::make_object( lua, ReadInt( p, f->Size ) );
                    case FT::AssetHandle:
                    {
                        int64_t handle = 0; // the u64 handle's bits: Lua integers are 64-bit, nothing is lost
                        std::memcpy( &handle, p, sizeof( handle ) );
                        return sol::make_object( lua, handle );
                    }
                    case FT::Struct:
                        if ( f->StructType == nullptr )
                            return sol::lua_nil;
                        return sol::make_object(
                             lua, ComponentProxy{ handle, scene, component, f->StructType, base + f->Offset } );
                    case FT::Vec2:
                    {
                        const auto* v = reinterpret_cast<const glm::vec2*>( p );
                        return sol::make_object( lua, lua.create_table_with( "x", v->x, "y", v->y ) );
                    }
                    case FT::Vec3:
                    {
                        const auto* v = reinterpret_cast<const glm::vec3*>( p );
                        return sol::make_object(
                             lua, lua.create_table_with( "x", v->x, "y", v->y, "z", v->z ) );
                    }
                    case FT::Vec4:
                    {
                        const auto* v = reinterpret_cast<const glm::vec4*>( p );
                        return sol::make_object(
                             lua, lua.create_table_with( "x", v->x, "y", v->y, "z", v->z, "w", v->w ) );
                    }
                    case FT::Unknown:
                        return sol::lua_nil;
                }
                return sol::lua_nil;
            }

            // Reads a vector component out of a Lua table accepting {x=..}, {r=..} or [1..4].
            static float VecComp( const sol::table& t, const char* xyzw, const char* rgba, int idx,
                                  float current )
            {
                if ( auto v = t.get<sol::optional<float>>( xyzw ) )
                    return *v;
                if ( auto v = t.get<sol::optional<float>>( rgba ) )
                    return *v;
                if ( auto v = t.get<sol::optional<float>>( idx ) )
                    return *v;
                return current;
            }

            void NewIndex( const std::string& fieldName, const sol::object& value )
            {
                if ( !Valid() )
                    return;
                const auto* f = FindField( fieldName );
                if ( !f || f->Meta.ReadOnly )
                    return;
                if ( f->IsContainer )
                {
                    if ( value.get_type() == sol::type::table &&
                         AssignContainer( *f, static_cast<char*>( DataPtr() ) + f->Offset,
                                          value.as<sol::table>() ) )
                        component->NotifyChanged( scene->GetRegistry(), handle );
                    return;
                }
                if ( Write( *f, static_cast<char*>( DataPtr() ) + f->Offset, value ) )
                    component->NotifyChanged( scene->GetRegistry(), handle );
            }

            // True when `value` was of the field's kind and landed; a mismatch writes nothing.
            static bool Write( const Reflection::FieldInfo& f, char* p, const sol::object& value )
            {
                using FT = Reflection::FieldType;
                switch ( f.Type )
                {
                    case FT::Bool:
                        if ( !value.is<bool>() )
                            return false;
                        *reinterpret_cast<bool*>( p ) = value.as<bool>();
                        return true;
                    case FT::Int:
                        if ( !value.is<double>() )
                            return false;
                        *reinterpret_cast<int32_t*>( p ) = static_cast<int32_t>( value.as<double>() );
                        return true;
                    case FT::Enum:
                        if ( !value.is<double>() )
                            return false;
                        WriteInt( p, f.Size, static_cast<int64_t>( value.as<double>() ) );
                        return true;
                    case FT::UInt:
                        if ( !value.is<double>() )
                            return false;
                        *reinterpret_cast<uint32_t*>( p ) = static_cast<uint32_t>( value.as<double>() );
                        return true;
                    case FT::Float:
                        if ( !value.is<double>() )
                            return false;
                        *reinterpret_cast<float*>( p ) = static_cast<float>( value.as<double>() );
                        return true;
                    case FT::Double:
                        if ( !value.is<double>() )
                            return false;
                        *reinterpret_cast<double*>( p ) = value.as<double>();
                        return true;
                    case FT::String:
                        if ( !value.is<std::string>() )
                            return false;
                        *reinterpret_cast<std::string*>( p ) = value.as<std::string>();
                        return true;
                    case FT::AssetHandle:
                    {
                        if ( !value.is<int64_t>() )
                            return false;
                        const int64_t handle = value.as<int64_t>(); // the bits a read returned
                        std::memcpy( p, &handle, sizeof( handle ) );
                        return true;
                    }
                    case FT::Vec2:
                    {
                        if ( !value.is<sol::table>() )
                            return false;
                        auto  t = value.as<sol::table>();
                        auto* v = reinterpret_cast<glm::vec2*>( p );
                        v->x    = VecComp( t, "x", "r", 1, v->x );
                        v->y    = VecComp( t, "y", "g", 2, v->y );
                        return true;
                    }
                    case FT::Vec3:
                    {
                        if ( !value.is<sol::table>() )
                            return false;
                        auto  t = value.as<sol::table>();
                        auto* v = reinterpret_cast<glm::vec3*>( p );
                        v->x    = VecComp( t, "x", "r", 1, v->x );
                        v->y    = VecComp( t, "y", "g", 2, v->y );
                        v->z    = VecComp( t, "z", "b", 3, v->z );
                        return true;
                    }
                    case FT::Vec4:
                    {
                        if ( !value.is<sol::table>() )
                            return false;
                        auto  t = value.as<sol::table>();
                        auto* v = reinterpret_cast<glm::vec4*>( p );
                        v->x    = VecComp( t, "x", "r", 1, v->x );
                        v->y    = VecComp( t, "y", "g", 2, v->y );
                        v->z    = VecComp( t, "z", "b", 3, v->z );
                        v->w    = VecComp( t, "w", "a", 4, v->w );
                        return true;
                    }
                    case FT::Struct: // written field by field through the nested proxy
                    case FT::Unknown:
                        return false;
                }
                return false;
            }
        };
    } // namespace

    void RegisterReflectionBindings( ScriptEngine::Impl& impl )
    {
        auto& lua = impl.Lua;

        lua.new_usertype<ComponentProxy>( "ComponentProxy", "valid", &ComponentProxy::Valid,
                                          sol::meta_function::index, &ComponentProxy::Index,
                                          sol::meta_function::new_index, &ComponentProxy::NewIndex );
        lua.new_usertype<ContainerProxy>( "ContainerProxy", sol::meta_function::index, &ContainerProxy::Index,
                                          sol::meta_function::new_index, &ContainerProxy::NewIndex,
                                          sol::meta_function::length, &ContainerProxy::Length );

        sol::table entity = lua["Entity"];

        // self:component("PointLight") -> proxy (or nil when absent/unknown).
        entity["component"] = []( ScriptEntity& self, const std::string& name,
                                  sol::this_state ts ) -> sol::object
        {
            sol::state_view lua( ts );
            if ( !self.Valid() )
                return sol::lua_nil;
            const auto* row = ECS::FindReflectedComponent( name );
            if ( !row || !row->Has( self.Reg(), self.handle ) )
                return sol::lua_nil;
            const auto* type = row->Type();
            if ( !type )
                return sol::lua_nil;
            return sol::make_object( lua, ComponentProxy{ self.handle, self.scene, row, type, 0 } );
        };

        entity["hasComponent"] = []( ScriptEntity& self, const std::string& name )
        {
            if ( !self.Valid() )
                return false;
            const auto* row = ECS::FindReflectedComponent( name );
            return row && row->Has( self.Reg(), self.handle );
        };

        // self:addComponent("PointLight") -> proxy over the (new or existing) component with
        // default-constructed data — configure it through the proxy fields right after.
        entity["addComponent"] = []( ScriptEntity& self, const std::string& name,
                                     sol::this_state ts ) -> sol::object
        {
            sol::state_view lua( ts );
            if ( !self.Valid() )
                return sol::lua_nil;
            const auto* row = ECS::FindReflectedComponent( name );
            if ( !row )
                return sol::lua_nil;
            const auto* type = row->Type();
            if ( !type )
                return sol::lua_nil;
            row->Add( self.Reg(), self.handle );
            return sol::make_object( lua, ComponentProxy{ self.handle, self.scene, row, type, 0 } );
        };

        entity["removeComponent"] = []( ScriptEntity& self, const std::string& name )
        {
            if ( !self.Valid() )
                return;
            if ( const auto* row = ECS::FindReflectedComponent( name ) )
                row->Remove( self.Reg(), self.handle );
        };
    }
} // namespace Desert::Scripting
