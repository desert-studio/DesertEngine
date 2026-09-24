#include "Internal/ScriptRuntime.hpp"

#include <Engine/Reflection/ReflectionRegistry.hpp>

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
    // Adding a PROPERTY field to a component makes it scriptable automatically; adding a NEW
    // reflected component = ONE line in kReflectedComponents below.

    namespace
    {
        struct ReflectedComponentEntry
        {
            const char* Name;     // Lua-facing component name
            const char* TypeName; // reflection registry type of the Data block
            bool ( *Has )( entt::registry&, entt::entity );
            void* ( *Data )( entt::registry&, entt::entity );
            void ( *Add )( entt::registry&, entt::entity );    // no-op if already present
            void ( *Remove )( entt::registry&, entt::entity ); // no-op if absent
        };

        template <class TComp>
        constexpr ReflectedComponentEntry MakeEntry( const char* name, const char* typeName )
        {
            return { name, typeName,
                     []( entt::registry& r, entt::entity e ) { return r.has<TComp>( e ); },
                     []( entt::registry& r, entt::entity e ) -> void*
                     { return &r.get<TComp>( e ).Data; },
                     []( entt::registry& r, entt::entity e )
                     {
                         if ( !r.has<TComp>( e ) )
                             r.emplace<TComp>( e );
                     },
                     []( entt::registry& r, entt::entity e )
                     {
                         if ( r.has<TComp>( e ) )
                             r.remove<TComp>( e );
                     } };
        }

        // The single registration list (mirrors the reflected-component serializers).
        constexpr ReflectedComponentEntry kReflectedComponents[] = {
             MakeEntry<ECS::CameraComponent>( "Camera", "CameraData" ),
             MakeEntry<ECS::DirectionLightComponent>( "DirectionLight", "DirectionalLightData" ),
             MakeEntry<ECS::PointLightComponent>( "PointLight", "PointLightData" ),
             MakeEntry<ECS::SpotLightComponent>( "SpotLight", "SpotLightData" ),
             MakeEntry<ECS::LandscapeMaterialComponent>( "LandscapeMaterial", "LandscapeMaterialData" ),
             MakeEntry<ECS::ColliderComponent>( "Collider", "ColliderData" ),
             MakeEntry<ECS::RigidBodyComponent>( "RigidBody", "RigidBodyData" ),
             MakeEntry<ECS::CharacterControllerComponent>( "CharacterController", "CharacterControllerData" ),
             MakeEntry<ECS::SkyAtmosphereComponent>( "SkyAtmosphere", "SkyAtmosphereData" ),
             MakeEntry<ECS::ExponentialHeightFogComponent>( "ExponentialHeightFog", "ExponentialHeightFogData" ),
             MakeEntry<ECS::VolumetricCloudComponent>( "VolumetricCloud", "VolumetricCloudData" ),
             MakeEntry<ECS::HeroCloudComponent>( "HeroCloud", "HeroCloudData" ),
        };

        const ReflectedComponentEntry* FindEntry( const std::string& name )
        {
            for ( const auto& e : kReflectedComponents )
                if ( name == e.Name )
                    return &e;
            return nullptr;
        }

        // Lua-side view of one reflected component instance on one entity.
        struct ComponentProxy
        {
            entt::entity                   handle = entt::null;
            Core::Scene*                   scene  = nullptr;
            const ReflectedComponentEntry* entry  = nullptr;
            const Reflection::TypeInfo*    type   = nullptr;

            bool Valid() const
            {
                return scene && entry && type && handle != entt::null &&
                       scene->GetRegistry().valid( handle ) &&
                       entry->Has( scene->GetRegistry(), handle );
            }

            void* DataPtr() const
            {
                return entry->Data( scene->GetRegistry(), handle );
            }

            const Reflection::FieldInfo* FindField( const std::string& name ) const
            {
                for ( const auto& f : type->Fields )
                    if ( f.Name == name )
                        return &f;
                return nullptr;
            }

            sol::object Index( sol::this_state ts, const std::string& fieldName ) const
            {
                if ( !Valid() )
                    return sol::lua_nil;
                const auto* f = FindField( fieldName );
                if ( !f )
                    return sol::lua_nil;

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
                    case FT::Enum: return sol::make_object( lua, *reinterpret_cast<const int32_t*>( p ) );
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
                    default:
                        return sol::lua_nil; // Struct/AssetHandle/containers: not exposed (yet)
                }
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

                char* p = static_cast<char*>( DataPtr() ) + f->Offset;
                using FT = Reflection::FieldType;
                switch ( f->Type )
                {
                    case FT::Bool:
                        if ( value.is<bool>() )
                            *reinterpret_cast<bool*>( p ) = value.as<bool>();
                        break;
                    case FT::Int:
                    case FT::Enum:
                        if ( value.is<double>() )
                            *reinterpret_cast<int32_t*>( p ) = static_cast<int32_t>( value.as<double>() );
                        break;
                    case FT::UInt:
                        if ( value.is<double>() )
                            *reinterpret_cast<uint32_t*>( p ) = static_cast<uint32_t>( value.as<double>() );
                        break;
                    case FT::Float:
                        if ( value.is<double>() )
                            *reinterpret_cast<float*>( p ) = static_cast<float>( value.as<double>() );
                        break;
                    case FT::Double:
                        if ( value.is<double>() )
                            *reinterpret_cast<double*>( p ) = value.as<double>();
                        break;
                    case FT::String:
                        if ( value.is<std::string>() )
                            *reinterpret_cast<std::string*>( p ) = value.as<std::string>();
                        break;
                    case FT::Vec2:
                        if ( value.is<sol::table>() )
                        {
                            auto  t = value.as<sol::table>();
                            auto* v = reinterpret_cast<glm::vec2*>( p );
                            v->x    = VecComp( t, "x", "r", 1, v->x );
                            v->y    = VecComp( t, "y", "g", 2, v->y );
                        }
                        break;
                    case FT::Vec3:
                        if ( value.is<sol::table>() )
                        {
                            auto  t = value.as<sol::table>();
                            auto* v = reinterpret_cast<glm::vec3*>( p );
                            v->x    = VecComp( t, "x", "r", 1, v->x );
                            v->y    = VecComp( t, "y", "g", 2, v->y );
                            v->z    = VecComp( t, "z", "b", 3, v->z );
                        }
                        break;
                    case FT::Vec4:
                        if ( value.is<sol::table>() )
                        {
                            auto  t = value.as<sol::table>();
                            auto* v = reinterpret_cast<glm::vec4*>( p );
                            v->x    = VecComp( t, "x", "r", 1, v->x );
                            v->y    = VecComp( t, "y", "g", 2, v->y );
                            v->z    = VecComp( t, "z", "b", 3, v->z );
                            v->w    = VecComp( t, "w", "a", 4, v->w );
                        }
                        break;
                    default:
                        break;
                }
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
            const auto* entry = FindEntry( name );
            if ( !entry || !entry->Has( self.Reg(), self.handle ) )
                return sol::lua_nil;
            const auto* type = Reflection::ReflectionRegistry::Get().Find( entry->TypeName );
            if ( !type )
                return sol::lua_nil;
            return sol::make_object( lua, ComponentProxy{ self.handle, self.scene, entry, type } );
        };

        entity["hasComponent"] = []( ScriptEntity& self, const std::string& name )
        {
            if ( !self.Valid() )
                return false;
            const auto* entry = FindEntry( name );
            return entry && entry->Has( self.Reg(), self.handle );
        };

        // self:addComponent("PointLight") -> proxy over the (new or existing) component with
        // default-constructed data — configure it through the proxy fields right after.
        entity["addComponent"] = []( ScriptEntity& self, const std::string& name,
                                     sol::this_state ts ) -> sol::object
        {
            sol::state_view lua( ts );
            if ( !self.Valid() )
                return sol::lua_nil;
            const auto* entry = FindEntry( name );
            if ( !entry )
                return sol::lua_nil;
            const auto* type = Reflection::ReflectionRegistry::Get().Find( entry->TypeName );
            if ( !type )
                return sol::lua_nil;
            entry->Add( self.Reg(), self.handle );
            return sol::make_object( lua, ComponentProxy{ self.handle, self.scene, entry, type } );
        };

        entity["removeComponent"] = []( ScriptEntity& self, const std::string& name )
        {
            if ( !self.Valid() )
                return;
            if ( const auto* entry = FindEntry( name ) )
                entry->Remove( self.Reg(), self.handle );
        };
    }
} // namespace Desert::Scripting
