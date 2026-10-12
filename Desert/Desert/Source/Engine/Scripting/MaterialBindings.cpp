#include "Internal/ScriptRuntime.hpp"

#include <Engine/Graphic/Materials/MaterialInstance.hpp>

namespace Desert::Scripting
{
    namespace
    {
        void UpsertParam( std::vector<ECS::MaterialParamOverride>& params, const std::string& name,
                          const glm::vec4& v )
        {
            for ( auto& p : params )
                if ( p.Name == name )
                {
                    p.Value = v;
                    return;
                }
            params.push_back( { name, v } );
        }

        // The Shader Override component, when the entity has one naming a shader: its Params are that draw.
        ECS::MaterialComponent* ShaderOverrideOf( entt::registry& reg, entt::entity entity )
        {
            auto* mc = reg.try_get<ECS::MaterialComponent>( entity );
            return mc != nullptr && !mc->ShaderName.empty() ? mc : nullptr;
        }

        // Two routes, one per thing the entity draws with (UE: UMeshComponent::SetScalar/VectorParameterValue-
        // OnMaterials, MeshComponent.cpp:549-628, whose MID is created on demand):
        //   * a Shader Override (MaterialComponent naming a shader) - the component's Params are that draw;
        //   * a static mesh's slot 0 - the write joins the mesh's ScriptSlotParams and, when the slot-0 instance
        //     exists, lands on it now. Before the instance is built (OnCreate runs first) the list alone holds it,
        //     and MeshECSSystem applies the list to every slot-0 instance it builds.
        // An entity with neither has no material to write, and the script is told so.
        int SetMaterialParam( lua_State* L )
        {
            const LuauEntityRef ref  = LuauBinder::CheckEntity( L, 1 );
            const std::string   name = luaL_checkstring( L, 2 );
            // w defaults to 1 so `self:setMaterialParam("AlbedoColor", r, g, b)` reads as a colour.
            const glm::vec4 v( static_cast<float>( luaL_checknumber( L, 3 ) ),
                               static_cast<float>( luaL_optnumber( L, 4, 0.0 ) ),
                               static_cast<float>( luaL_optnumber( L, 5, 0.0 ) ),
                               static_cast<float>( luaL_optnumber( L, 6, 1.0 ) ) );
            auto&           reg = *ref.Registry;

            if ( auto* mc = ShaderOverrideOf( reg, ref.Entity ) )
            {
                UpsertParam( mc->Params, name, v );
                return 0;
            }
            auto* smc = reg.try_get<ECS::StaticMeshComponent>( ref.Entity );
            if ( smc == nullptr )
                luaL_error( L,
                            "setMaterialParam('%s'): the entity has no static mesh and no Shader Override - "
                            "there is no material to write",
                            name.c_str() );
            UpsertParam( smc->ScriptSlotParams, name, v );
            if ( !smc->RuntimeMaterialInstances.empty() && smc->RuntimeMaterialInstances[0] )
                smc->RuntimeMaterialInstances[0]->SetParamFromVec4( name, v );
            return 0;
        }

        int PushVec4( lua_State* L, const glm::vec4& v )
        {
            for ( int i = 0; i < 4; ++i )
                lua_pushnumber( L, v[i] );
            return 4;
        }

        // The value a read sees is the one the write route above keeps: the override's Params, else the script's
        // slot-0 list, else the slot-0 instance's own override (a material-instance asset's authored value).
        int GetMaterialParam( lua_State* L )
        {
            const LuauEntityRef ref  = LuauBinder::CheckEntity( L, 1 );
            const std::string   name = luaL_checkstring( L, 2 );
            auto&               reg  = *ref.Registry;

            if ( const auto* mc = ShaderOverrideOf( reg, ref.Entity ) )
            {
                for ( const auto& p : mc->Params )
                    if ( p.Name == name )
                        return PushVec4( L, p.Value );
                return PushVec4( L, glm::vec4( 0.0f ) );
            }
            if ( const auto* smc = reg.try_get<ECS::StaticMeshComponent>( ref.Entity ) )
            {
                for ( const auto& p : smc->ScriptSlotParams )
                    if ( p.Name == name )
                        return PushVec4( L, p.Value );
                if ( !smc->RuntimeMaterialInstances.empty() && smc->RuntimeMaterialInstances[0] )
                    if ( const auto authored = smc->RuntimeMaterialInstances[0]->GetOverrideAsVec4( name ) )
                        return PushVec4( L, *authored );
            }
            return PushVec4( L, glm::vec4( 0.0f ) );
        }

        // Undo every script-set param. On a static mesh the slot instances are DROPPED and rebuilt by
        // MeshECSSystem next frame from the authored slots: resetting the live instance's overrides would also
        // wipe a material-instance asset's own overrides, which the instance carries the same way.
        int ClearMaterialParams( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            if ( auto* mc = ShaderOverrideOf( *ref.Registry, ref.Entity ) )
            {
                mc->Params.clear();
                mc->Textures.clear();
            }
            if ( auto* smc = ref.Registry->try_get<ECS::StaticMeshComponent>( ref.Entity );
                 smc != nullptr && !smc->ScriptSlotParams.empty() )
            {
                smc->ScriptSlotParams.clear();
                smc->RuntimeMaterialInstances.clear();
            }
            return 0;
        }

        // A surface shader by name; "" drops the Shader Override and the mesh draws its lit slot materials again
        // (the override's Params belonged to that shader and go with it).
        int SetShader( lua_State* L )
        {
            const LuauEntityRef ref  = LuauBinder::CheckEntity( L, 1 );
            const std::string   name = luaL_checkstring( L, 2 );
            if ( name.empty() )
                ref.Registry->remove<ECS::MaterialComponent>( ref.Entity );
            else
                ref.Registry->get_or_emplace<ECS::MaterialComponent>( ref.Entity ).ShaderName = name;
            return 0;
        }

        int GetShader( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            const auto*         mc  = ref.Registry->try_get<ECS::MaterialComponent>( ref.Entity );
            lua_pushstring( L, mc != nullptr ? mc->ShaderName.c_str() : "" );
            return 1;
        }
    } // namespace

    // Material API — the unified material protocol (params by shader-schema name).
    void RegisterMaterialBindings( lua_State* L )
    {
        for ( const luaL_Reg& method :
              { luaL_Reg{ "setMaterialParam", &SetMaterialParam },
                luaL_Reg{ "getMaterialParam", &GetMaterialParam },
                luaL_Reg{ "clearMaterialParams", &ClearMaterialParams }, luaL_Reg{ "setShader", &SetShader },
                luaL_Reg{ "getShader", &GetShader } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
