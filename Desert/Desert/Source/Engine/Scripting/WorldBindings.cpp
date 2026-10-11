#include "Internal/ScriptRuntime.hpp"

#include <Common/Core/Math/Ray.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

namespace Desert::Scripting
{
    namespace
    {
        void PushEntity( lua_State* L, ScriptEngine::Impl& host, entt::entity entity )
        {
            if ( host.Scene == nullptr )
            {
                lua_pushnil( L );
                return;
            }
            LuauBinder::PushEntity( L, host.Registry(), entity );
        }

        ScriptEngine::Impl& Host( lua_State* L, const char* function )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            if ( host.Scene == nullptr )
                luaL_errorL( L, "World.%s: no scene is bound to the script engine", function );
            return host;
        }

        // The first entity whose Tag is `name` (nil when none).
        int Find( lua_State* L )
        {
            const std::string   name = luaL_checkstring( L, 1 );
            ScriptEngine::Impl& host = Host( L, "find" );
            auto                view = host.Registry().view<ECS::TagComponent>();
            for ( auto e : view )
                if ( view.get<ECS::TagComponent>( e ).Tag == name )
                {
                    PushEntity( L, host, e );
                    return 1;
                }
            lua_pushnil( L );
            return 1;
        }

        // World.raycast(ox,oy,oz, dx,dy,dz [, maxDist]) -> { hit, entity, x,y,z, nx,ny,nz, dist }.
        int Raycast( lua_State* L )
        {
            ScriptEngine::Impl&     host      = Host( L, "raycast" );
            const glm::vec3         origin    = CheckVec3( L, 1 );
            const glm::vec3         direction = CheckVec3( L, 4 );
            const bool              bounded   = !lua_isnoneornil( L, 7 );
            const double            maxDist   = luaL_optnumber( L, 7, 0.0 );
            Common::Math::Ray const ray( origin, direction );
            Core::RaycastHit        hit;
            const bool inRange = host.Scene->Raycast( ray, hit ) && ( !bounded || hit.Distance <= maxDist );

            lua_newtable( L );
            lua_pushboolean( L, static_cast<int>( inRange ) );
            lua_setfield( L, -2, "hit" );
            if ( !inRange )
                return 1;
            entt::entity h = entt::null;
            if ( auto found = host.Scene->FindEntityByID( hit.Entity ) )
                h = found->get().GetHandle();
            PushEntity( L, host, h );
            lua_setfield( L, -2, "entity" );
            for ( const auto& [name, value] :
                  { std::pair{ "x", hit.Point.x }, std::pair{ "y", hit.Point.y }, std::pair{ "z", hit.Point.z },
                    std::pair{ "nx", hit.Normal.x }, std::pair{ "ny", hit.Normal.y },
                    std::pair{ "nz", hit.Normal.z }, std::pair{ "dist", hit.Distance } } )
            {
                lua_pushnumber( L, value );
                lua_setfield( L, -2, name );
            }
            return 1;
        }

        // The active camera's eye ray: ox,oy,oz, dx,dy,dz.
        int CameraRay( lua_State* L )
        {
            ScriptEngine::Impl const& host = Host( L, "cameraRay" );
            glm::vec3                 o( 0.0f );
            glm::vec3                 d( 0.0f, 0.0f, -1.0f );
            if ( auto cam = host.Scene->GetActiveCamera() )
            {
                const glm::mat4 inv = glm::inverse( cam->GetViewMatrix() );
                o                   = glm::vec3( inv[3] );
                d                   = -glm::normalize( glm::vec3( inv[2] ) );
            }
            for ( float const v : { o.x, o.y, o.z, d.x, d.y, d.z } )
                lua_pushnumber( L, v );
            return 6;
        }

        // World.spawn(prefabPath, x, y, z) -> the placed root entity (nil and a logged error on failure).
        int Spawn( lua_State* L )
        {
            const std::string   prefabPath = luaL_checkstring( L, 1 );
            const glm::vec3     pos        = CheckVec3( L, 2 );
            ScriptEngine::Impl& host       = Host( L, "spawn" );
            if ( host.Assets == nullptr )
            {
                LOG_ERROR( "[Lua] World.spawn: no AssetManager bound" );
                lua_pushnil( L );
                return 1;
            }
            auto prefab = host.Assets->FindByPath<Assets::PrefabAsset>( prefabPath );
            if ( !prefab )
                prefab = host.Assets->CreateAsset<Assets::PrefabAsset>( prefabPath );
            if ( !prefab )
            {
                LOG_ERROR( "[Lua] World.spawn: prefab not found '{}'", prefabPath );
                lua_pushnil( L );
                return 1;
            }
            const auto placed = prefab->Instantiate( host.Scene, *host.Assets, {}, &pos );
            if ( !placed )
            {
                LOG_ERROR( "[Lua] World.spawn: {}", placed.GetError() );
                lua_pushnil( L );
                return 1;
            }
            PushEntity( L, host, placed.GetValue() ? placed.GetValue().GetHandle() : entt::null );
            return 1;
        }

        // World.spawnMarker(x,y,z, scale, r,g,b) -> a debug sphere drawn with the DebugColor template.
        int SpawnMarker( lua_State* L )
        {
            const glm::vec3     pos   = CheckVec3( L, 1 );
            const auto          scale = static_cast<float>( luaL_checknumber( L, 4 ) );
            const glm::vec3     rgb   = CheckVec3( L, 5 );
            ScriptEngine::Impl& host  = Host( L, "spawnMarker" );

            ECS::Entity const e                                  = host.Scene->CreateNewEntity( "Marker" );
            e.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Sphere;
            auto& t       = e.GetComponent<ECS::TransformComponent>();
            t.Translation                                        = pos;
            t.Scale       = glm::vec3( scale <= 0.0f ? 0.15f : scale );

            const auto debugColor =
                 host.Assets != nullptr
                      ? Assets::FindTemplateByRole( *host.Assets, Common::Content::kDebugColorRole )
                      : Common::MakeError<Common::AssetHandle>( std::string( "no asset manager is bound" ) );
            const auto template_ =
                 debugColor ? host.Assets->FindByHandle<Assets::ShaderAsset>( debugColor.GetValue() ) : nullptr;
            if ( template_ == nullptr )
            {
                LOG_ERROR( "[Script] World.spawnMarker: {} — the marker draws its mesh's own material",
                           debugColor ? std::string( "the DebugColor template is not loaded" )
                                      : debugColor.GetError() );
                PushEntity( L, host, e.GetHandle() );
                return 1;
            }
            auto& mc      = e.AddComponent<ECS::MaterialComponent>();
            mc.ShaderName = template_->GetMetadata().Filepath.stem().string();
            mc.Params.push_back( ECS::MaterialParamOverride{ "Color", glm::vec4( rgb, 1.0f ) } );
            PushEntity( L, host, e.GetHandle() );
            return 1;
        }

        // World.set/get/has: script state shared by every script (one table for the VM's lifetime).
        int WorldVarSet( lua_State* L )
        {
            luaL_checkstring( L, 1 );
            lua_getref( L, ScriptEngine::Impl::Of( L ).WorldVars );
            lua_pushvalue( L, 1 );
            lua_pushvalue( L, 2 );
            lua_rawset( L, -3 );
            return 0;
        }
        int WorldVarGet( lua_State* L )
        {
            luaL_checkstring( L, 1 );
            lua_getref( L, ScriptEngine::Impl::Of( L ).WorldVars );
            lua_pushvalue( L, 1 );
            lua_rawget( L, -2 );
            return 1;
        }
        int WorldVarHas( lua_State* L )
        {
            WorldVarGet( L );
            lua_pushboolean( L, static_cast<int>( !lua_isnil( L, -1 ) ) );
            return 1;
        }
    } // namespace

    void RegisterWorldBindings( lua_State* L )
    {
        constexpr luaL_Reg kWorld[] = {
             { "find", &Find },       { "raycast", &Raycast },         { "cameraRay", &CameraRay },
             { "spawn", &Spawn },     { "spawnMarker", &SpawnMarker }, { "set", &WorldVarSet },
             { "get", &WorldVarGet }, { "has", &WorldVarHas },         { nullptr, nullptr } };
        luaL_register( L, "World", kWorld );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting
