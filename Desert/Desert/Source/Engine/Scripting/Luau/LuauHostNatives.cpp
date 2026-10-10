#include <Engine/Scripting/Luau/LuauHost.hpp>

#include <Common/Core/Math/Ray.hpp>
#include <Engine/Core/Camera.hpp>

#include <algorithm>

// THE LANGUAGE HOST'S OWN NATIVES — what is about Luau itself or about the script host, not about the engine:
// print-style logging of any values, closures kept for later (Timer.after), Luau values shared between scripts
// (World.set/get/has), an entity object's liveness, destroying an entity whose scripts this host runs, calling
// into another entity's scripts. Everything the ENGINE offers reaches Luau through reflection
// (Engine/Libraries, LuauBinder) — never through a file here. raycast/cameraRay stay until the public layer has
// a struct result (REMAINDER of SCR-PORT).

namespace Desert::Scripting
{
    glm::vec3 CheckVec3( lua_State* L, int first )
    {
        if ( const float* v = lua_tovector( L, first ) )
            return { v[0], v[1], v[2] };
        return { static_cast<float>( luaL_checknumber( L, first ) ),
                 static_cast<float>( luaL_checknumber( L, first + 1 ) ),
                 static_cast<float>( luaL_checknumber( L, first + 2 ) ) };
    }

    namespace
    {
        // Every argument as text (tostring), tab-separated — what print() would show.
        int Log( lua_State* L )
        {
            std::string text;
            for ( int i = 1, n = lua_gettop( L ); i <= n; ++i )
            {
                std::size_t length = 0;
                const char* piece  = luaL_tolstring( L, i, &length );
                if ( i > 1 )
                    text += '\t';
                text.append( piece, length );
                lua_pop( L, 1 );
            }
            LOG_INFO( "[Lua] {}", text );
            return 0;
        }

        // Timer.after(seconds, fn): runs fn once after `seconds` of game time (Play only — ticked by
        // ScriptSystem). Owned by the (entity, slot) that scheduled it: a reload / destroy cancels it. A callback
        // may call Timer.after again to re-arm itself.
        int After( lua_State* L )
        {
            const auto seconds = static_cast<float>( luaL_checknumber( L, 1 ) );
            luaL_checktype( L, 2, LUA_TFUNCTION );
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            host.Timers.push_back( { host.CurrentOwner, std::max( seconds, 0.0f ), lua_ref( L, 2 ) } );
            return 0;
        }

        // World.set/get/has: Luau values shared by every script (one table for the VM's lifetime).
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
            lua_pushboolean( L, !lua_isnil( L, -1 ) );
            return 1;
        }

        ScriptEngine::Impl& SceneHost( lua_State* L, const char* function )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            if ( host.Scene == nullptr )
                luaL_errorL( L, "World.%s: no scene is bound to the script engine", function );
            return host;
        }

        // World.raycast(ox,oy,oz, dx,dy,dz [, maxDist]) -> { hit, entity, x,y,z, nx,ny,nz, dist }.
        int Raycast( lua_State* L )
        {
            ScriptEngine::Impl& host      = SceneHost( L, "raycast" );
            const glm::vec3     origin    = CheckVec3( L, 1 );
            const int           next      = lua_isvector( L, 1 ) ? 2 : 4;
            const glm::vec3     direction = CheckVec3( L, next );
            const int           limit     = lua_isvector( L, next ) ? next + 1 : next + 3;
            const bool          bounded   = !lua_isnoneornil( L, limit );
            const double        maxDist   = luaL_optnumber( L, limit, 0.0 );
            Common::Math::Ray   ray( origin, direction );
            Core::RaycastHit    hit;
            const bool inRange = host.Scene->Raycast( ray, hit ) && ( !bounded || hit.Distance <= maxDist );
            lua_newtable( L );
            lua_pushboolean( L, inRange );
            lua_setfield( L, -2, "hit" );
            if ( !inRange )
                return 1;
            entt::entity h = entt::null;
            if ( auto found = host.Scene->FindEntityByID( hit.Entity ) )
                h = found->get().GetHandle();
            LuauBinder::PushEntity( L, host.Registry(), h );
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
            ScriptEngine::Impl& host = SceneHost( L, "cameraRay" );
            glm::vec3           o( 0.0f );
            glm::vec3           d( 0.0f, 0.0f, -1.0f );
            if ( auto cam = host.Scene->GetActiveCamera() )
            {
                const glm::mat4 inv = glm::inverse( cam->GetViewMatrix() );
                o                   = glm::vec3( inv[3] );
                d                   = -glm::normalize( glm::vec3( inv[2] ) );
            }
            for ( float v : { o.x, o.y, o.z, d.x, d.y, d.z } )
                lua_pushnumber( L, v );
            return 6;
        }

        int Valid( lua_State* L )
        {
            const std::optional<LuauEntityRef> ref = LuauBinder::ToEntity( L, 1 );
            lua_pushboolean( L, ref && ref->Registry != nullptr && ref->Registry->valid( ref->Entity ) );
            return 1;
        }

        // Removes this entity (and its subtree) from the scene; its scripts are released once the call returns.
        int Destroy( lua_State* L )
        {
            const std::optional<LuauEntityRef> ref  = LuauBinder::ToEntity( L, 1 );
            ScriptEngine::Impl&                host = ScriptEngine::Impl::Of( L );
            if ( !ref || ref->Registry == nullptr || !ref->Registry->valid( ref->Entity ) ||
                 host.Scene == nullptr )
                return 0;
            host.PendingRelease.push_back( static_cast<uint32_t>( ref->Entity ) );
            host.Scene->DestroyEntity( ECS::Entity{ ref->Entity, *ref->Registry } );
            return 0;
        }

        // Cross-entity message: target:call(fn, ...) runs `fn` with the arguments in EVERY script slot of the
        // target that defines it (an entity may run many behaviours). The mechanism behind OnInteract.
        int Call( lua_State* L )
        {
            const LuauEntityRef ref      = LuauBinder::CheckEntity( L, 1 );
            const char*         function = luaL_checkstring( L, 2 );
            ScriptEngine::Impl& host     = ScriptEngine::Impl::Of( L );
            auto                found    = host.Slots.find( static_cast<uint32_t>( ref.Entity ) );
            if ( found == host.Slots.end() )
                return 0;
            const std::vector<LuauSlot> slots = found->second; // the callee may change the map
            const int                   count = lua_gettop( L ) - 2;
            for ( LuauSlot slot : slots )
            {
                if ( slot == 0 || !host.Runtime->Defines( slot, function ) )
                    continue;
                if ( Common::BoolResultStr r = host.Runtime->CallFrom( slot, function, L, 3, count );
                     !r.IsSuccess() )
                    LOG_ERROR( "[Lua] {} error: {}", function, r.GetError() );
            }
            return 0;
        }

        /// The global table `name` (the reflected library's, when one is bound under it), created when absent.
        void OpenGlobalTable( lua_State* L, const char* name )
        {
            lua_getglobal( L, name );
            if ( lua_istable( L, -1 ) )
                return;
            lua_pop( L, 1 );
            lua_newtable( L );
            lua_pushvalue( L, -1 );
            lua_setglobal( L, name );
        }
    } // namespace

    void RegisterHostNatives( lua_State* L )
    {
        lua_pushcfunction( L, &Log, "log" );
        lua_setglobal( L, "log" );

        OpenGlobalTable( L, "Timer" );
        lua_pushcfunction( L, &After, "after" );
        lua_setfield( L, -2, "after" );
        lua_pop( L, 1 );

        OpenGlobalTable( L, "World" );
        for ( const luaL_Reg& entry :
              { luaL_Reg{ "set", &WorldVarSet }, luaL_Reg{ "get", &WorldVarGet }, luaL_Reg{ "has", &WorldVarHas },
                luaL_Reg{ "raycast", &Raycast }, luaL_Reg{ "cameraRay", &CameraRay } } )
        {
            lua_pushcfunction( L, entry.func, entry.name );
            lua_setfield( L, -2, entry.name );
        }
        lua_pop( L, 1 );

        for ( const luaL_Reg& method :
              { luaL_Reg{ "valid", &Valid }, luaL_Reg{ "destroy", &Destroy }, luaL_Reg{ "call", &Call } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
