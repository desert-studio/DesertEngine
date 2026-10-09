#include "Internal/ScriptRuntime.hpp"

#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <cstring>

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
    // component's on_update signal (ReflectedComponent::NotifyChanged). Containers are not exposed yet.

    namespace
    {
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
                if ( !f || f->IsContainer )
                    return sol::lua_nil; // a std::vector field is not a scalar at Offset: never reinterpret it

                sol::state_view lua( ts );
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
                if ( !f || f->Meta.ReadOnly || f->IsContainer )
                    return;
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
