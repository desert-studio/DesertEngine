#include "Internal/ScriptRuntime.hpp"

#include <Engine/Graphic/Materials/MaterialInstance.hpp>

namespace Desert::Scripting
{
    namespace
    {
        void UpsertComponentParam( ECS::MaterialComponent& mc, const std::string& name, const glm::vec4& v )
        {
            for ( auto& p : mc.Params )
                if ( p.Name == name )
                {
                    p.Value = v;
                    return;
                }
            mc.Params.push_back( { name, v } );
        }

        // Lit path: writes go STRAIGHT to the slot-0 runtime MaterialInstance (live override; the authored slots
        // stay the source of truth). Custom Shader Override path: the MaterialComponent params ARE the draw state.
        // Before the first instance build the write is stashed on the component; MeshECSSystem applies it once.
        int SetMaterialParam( lua_State* L )
        {
            const LuauEntityRef ref  = LuauBinder::CheckEntity( L, 1 );
            const std::string   name = luaL_checkstring( L, 2 );
            // w defaults to 1 so `self:setMaterialParam("AlbedoColor", r, g, b)` reads as a colour.
            const glm::vec4 v( static_cast<float>( luaL_checknumber( L, 3 ) ), static_cast<float>( luaL_optnumber( L, 4, 0.0 ) ),
                               static_cast<float>( luaL_optnumber( L, 5, 0.0 ) ), static_cast<float>( luaL_optnumber( L, 6, 1.0 ) ) );
            auto& reg = *ref.Registry;

            if ( auto* mc = reg.try_get<ECS::MaterialComponent>( ref.Entity ); mc != nullptr && !mc->ShaderName.empty() )
            {
                UpsertComponentParam( *mc, name, v );
                return 0;
            }
            if ( auto* smc = reg.try_get<ECS::StaticMeshComponent>( ref.Entity );
                 smc != nullptr && !smc->RuntimeMaterialInstances.empty() && smc->RuntimeMaterialInstances[0] )
            {
                smc->RuntimeMaterialInstances[0]->SetParamFromVec4( name, v );
                return 0;
            }
            UpsertComponentParam( reg.get_or_emplace<ECS::MaterialComponent>( ref.Entity ), name, v );
            return 0;
        }

        int PushVec4( lua_State* L, const glm::vec4& v )
        {
            for ( int i = 0; i < 4; ++i )
                lua_pushnumber( L, v[i] );
            return 4;
        }

        int GetMaterialParam( lua_State* L )
        {
            const LuauEntityRef ref  = LuauBinder::CheckEntity( L, 1 );
            const std::string   name = luaL_checkstring( L, 2 );
            auto&               reg  = *ref.Registry;

            // Component channel first: custom-shader state and not-yet-consumed seeds live here.
            if ( const auto* mc = reg.try_get<ECS::MaterialComponent>( ref.Entity ) )
                for ( const auto& p : mc->Params )
                    if ( p.Name == name )
                        return PushVec4( L, p.Value );

            // Lit path: the live override on the slot-0 instance.
            if ( const auto* smc = reg.try_get<ECS::StaticMeshComponent>( ref.Entity );
                 smc != nullptr && !smc->RuntimeMaterialInstances.empty() && smc->RuntimeMaterialInstances[0] )
            {
                for ( const auto& [pname, prop] : smc->RuntimeMaterialInstances[0]->GetPropertySet().GetProperties() )
                {
                    if ( pname != name || !prop.bIsOverridden )
                        continue;
                    if ( const auto* f = std::get_if<float>( &prop.Value ) )
                        return PushVec4( L, { *f, 0.0f, 0.0f, 0.0f } );
                    if ( const auto* v2 = std::get_if<glm::vec2>( &prop.Value ) )
                        return PushVec4( L, { v2->x, v2->y, 0.0f, 0.0f } );
                    if ( const auto* v3 = std::get_if<glm::vec3>( &prop.Value ) )
                        return PushVec4( L, { v3->x, v3->y, v3->z, 1.0f } );
                    if ( const auto* v4 = std::get_if<glm::vec4>( &prop.Value ) )
                        return PushVec4( L, *v4 );
                }
            }
            return PushVec4( L, glm::vec4( 0.0f ) );
        }

        // Undo every script-set param: the component channel AND the slot-0 instance overrides.
        int ClearMaterialParams( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            if ( auto* mc = ref.Registry->try_get<ECS::MaterialComponent>( ref.Entity ) )
            {
                mc->Params.clear();
                mc->Textures.clear();
            }
            if ( auto* smc = ref.Registry->try_get<ECS::StaticMeshComponent>( ref.Entity );
                 smc != nullptr && !smc->RuntimeMaterialInstances.empty() && smc->RuntimeMaterialInstances[0] )
                smc->RuntimeMaterialInstances[0]->ResetOverrides();
            return 0;
        }

        // A surface shader by name ("" -> back to the lit slot materials); the editor's Shader Override routing.
        int SetShader( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            ref.Registry->get_or_emplace<ECS::MaterialComponent>( ref.Entity ).ShaderName = luaL_checkstring( L, 2 );
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
        for ( const luaL_Reg& method : { luaL_Reg{ "setMaterialParam", &SetMaterialParam },
                                         luaL_Reg{ "getMaterialParam", &GetMaterialParam },
                                         luaL_Reg{ "clearMaterialParams", &ClearMaterialParams },
                                         luaL_Reg{ "setShader", &SetShader }, luaL_Reg{ "getShader", &GetShader } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
