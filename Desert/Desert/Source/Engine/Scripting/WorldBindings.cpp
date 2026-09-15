#include "Internal/ScriptRuntime.hpp"

namespace Desert::Scripting
{
    // World table: scene queries (find/spawn/raycast) + water controls.
    void RegisterWorldBindings( ScriptEngine::Impl& implRef )
    {
        auto& lua  = implRef.Lua;
        auto* impl = &implRef;
        (void)lua; (void)impl;

        // The `World` table: scene-wide queries the engine performs on the script's behalf.
        sol::table world = lua.create_named_table( "World" );

        // Find the first entity whose name (TagComponent) matches. Returns an Entity (check :valid()).
        world["find"] = [impl]( const std::string& name ) -> ScriptEntity
        {
            auto& reg  = impl->Scene->GetRegistry();
            auto  view = reg.view<ECS::TagComponent>();
            for ( auto e : view )
            {
                if ( view.get<ECS::TagComponent>( e ).Tag == name )
                    return impl->MakeEntity( e );
            }
            return impl->MakeEntity( entt::null );
        };

        // Cast a ray through the scene's static meshes. Returns a table:
        //   { hit=bool, entity=Entity, x,y,z (point), nx,ny,nz (normal), dist=number }
        world["raycast"] = [impl]( float ox, float oy, float oz, float dx, float dy, float dz,
                                   sol::optional<float> maxDist ) -> sol::table
        {
            sol::table       result = impl->Lua.create_table();
            Common::Math::Ray ray( glm::vec3( ox, oy, oz ), glm::vec3( dx, dy, dz ) );
            Core::RaycastHit  hit;
            const bool        ok = impl->Scene->Raycast( ray, hit );
            const bool inRange   = ok && ( !maxDist || hit.Distance <= *maxDist );
            result["hit"]        = inRange;
            if ( inRange )
            {
                entt::entity h = entt::null;
                if ( auto found = impl->Scene->FindEntityByID( hit.Entity ) )
                    h = found->get().GetHandle();
                result["entity"] = impl->MakeEntity( h );
                result["x"]      = hit.Point.x;
                result["y"]      = hit.Point.y;
                result["z"]      = hit.Point.z;
                result["nx"]     = hit.Normal.x;
                result["ny"]     = hit.Normal.y;
                result["nz"]     = hit.Normal.z;
                result["dist"]   = hit.Distance;
            }
            return result;
        };

        // The active camera's eye + forward (origin, dir) — the natural ray for "what am I looking at".
        world["cameraRay"] = [impl]() -> std::tuple<float, float, float, float, float, float>
        {
            if ( auto cam = impl->Scene->GetActiveCamera() )
            {
                // Derive eye + forward from the view matrix (works for any Camera subtype): the inverse view's
                // translation is the eye, and its -Z column is the world-space forward direction.
                const glm::mat4 inv = glm::inverse( cam->GetViewMatrix() );
                const glm::vec3 o   = glm::vec3( inv[3] );
                const glm::vec3 d   = -glm::normalize( glm::vec3( inv[2] ) );
                return { o.x, o.y, o.z, d.x, d.y, d.z };
            }
            return { 0, 0, 0, 0, 0, -1 };
        };

        // Instantiate a prefab at a world position. Returns the new root Entity (check :valid()).
        world["spawn"] = [impl]( const std::string& prefabPath, float x, float y,
                                 float z ) -> ScriptEntity
        {
            if ( !impl->Assets )
            {
                LOG_ERROR( "[Lua] World.spawn: no AssetManager bound" );
                return impl->MakeEntity( entt::null );
            }
            auto prefab = impl->Assets->FindByPath<Assets::PrefabAsset>( prefabPath );
            if ( !prefab )
                prefab = impl->Assets->CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High,
                                                                         prefabPath );
            if ( !prefab )
            {
                LOG_ERROR( "[Lua] World.spawn: prefab not found '{}'", prefabPath );
                return impl->MakeEntity( entt::null );
            }
            const glm::vec3 pos( x, y, z );
            // At the scene root: a script spawns into the world, and a UI prefab has no canvas to name
            // from here. That case is REFUSED BY NAME now rather than spawning an invisible tree -- the
            // script gets a null entity and the log says which prefab and why.
            const auto placed = prefab->Instantiate( impl->Scene, *impl->Assets, {}, &pos );
            if ( !placed )
            {
                LOG_ERROR( "[Lua] World.spawn: {}", placed.GetError() );
                return impl->MakeEntity( entt::null );
            }
            return impl->MakeEntity( placed.GetValue() ? placed.GetValue().GetHandle() : entt::null );
        };

        // Spawn a small solid-colour marker sphere at a world position (e.g. a bullet-impact "red spot").
        // Uses the data-driven Unlit shader so the colour shows regardless of scene lighting. Returns the
        // new Entity (call :destroy() to remove it, e.g. after a lifetime).
        world["spawnMarker"] = [impl]( float x, float y, float z, float scale, float r, float g,
                                       float b ) -> ScriptEntity
        {
            ECS::Entity e = impl->Scene->CreateNewEntity( "Marker" );
            e.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Sphere;

            auto& t       = e.GetComponent<ECS::TransformComponent>();
            t.Translation = glm::vec3( x, y, z );
            t.Scale       = glm::vec3( scale <= 0.0f ? 0.15f : scale );

            auto& mc      = e.AddComponent<ECS::MaterialComponent>();
            mc.ShaderName = "Unlit";
            mc.Params.push_back( ECS::MaterialParamOverride{ "Color", glm::vec4( r, g, b, 1.0f ) } );

            return impl->MakeEntity( e.GetHandle() );
        };

        // Generic per-scene variable store (a "blackboard"): World.set(key, value) / World.get(key) /
        // World.has(key). Gameplay defines its OWN dynamic variables — water level, quest flags, timers,
        // team scores, anything — with NO engine hardcode. This replaced the hardcoded water bindings
        // (World.waterLevel/waterEnabled/spawnWater): water is now just a user-defined variable, e.g.
        //   World.set("waterLevel", 5.0)      -- set it up
        //   if body.y < World.get("waterLevel") then ... end   -- swim logic reads it
        // Values are any Lua value (numbers, strings, bools, tables, entities); the store lives in the VM.
        impl->Lua["__world_vars"] = impl->Lua.create_table();
        world["set"]              = [impl]( const std::string& key, sol::object value )
        { impl->Lua["__world_vars"][key] = value; };
        world["get"] = [impl]( const std::string& key ) -> sol::object
        { return impl->Lua["__world_vars"][key]; };
        world["has"] = [impl]( const std::string& key ) -> bool
        {
            sol::object v = impl->Lua["__world_vars"][key];
            return v.valid() && v.get_type() != sol::type::lua_nil;
        };
    }
} // namespace Desert::Scripting
