#include "Internal/ScriptRuntime.hpp"

// The verbs of a projectile entity (UE UProjectileMovementComponent's Velocity / SetVelocityInLocalSpace and
// AActor::SetInstigator). The movement itself is ECS/System/ProjectileMovement.cpp; each verb is a no-op on an
// entity without a ProjectileComponent.
namespace Desert::Scripting
{
    namespace
    {
        ECS::ProjectileComponent* Projectile( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            return ref.Registry->try_get<ECS::ProjectileComponent>( ref.Entity );
        }

        // projectile:launch(vx, vy, vz [, instigator]): flies (again) at that world velocity, cm/s, ignoring the
        // instigator's body — what a weapon script calls on the projectile World.spawn placed.
        int Launch( lua_State* L )
        {
            ECS::ProjectileComponent* projectile = Projectile( L );
            if ( projectile == nullptr )
                return 0;
            projectile->Data.Velocity = CheckVec3( L, 2 );
            projectile->Stopped       = false;
            if ( lua_gettop( L ) >= 5 && !lua_isnil( L, 5 ) )
                projectile->Instigator = LuauBinder::CheckEntity( L, 5 ).Entity;
            return 0;
        }

        // projectile:projectileVelocity() -> vx, vy, vz (cm/s; 0 once it stopped).
        int ProjectileVelocity( lua_State* L )
        {
            const ECS::ProjectileComponent* projectile = Projectile( L );
            const glm::vec3 v = projectile != nullptr ? projectile->Data.Velocity : glm::vec3( 0.0f );
            lua_pushnumber( L, v.x );
            lua_pushnumber( L, v.y );
            lua_pushnumber( L, v.z );
            return 3;
        }

        // projectile:hasStopped() -> true once a blocking hit ended its flight (UE HasStoppedSimulation).
        int HasStopped( lua_State* L )
        {
            const ECS::ProjectileComponent* projectile = Projectile( L );
            lua_pushboolean( L, static_cast<int>( projectile != nullptr && projectile->Stopped ) );
            return 1;
        }
    } // namespace

    void RegisterProjectileBindings( lua_State* L )
    {
        for ( const luaL_Reg& method :
              { luaL_Reg{ "launch", &Launch }, luaL_Reg{ "projectileVelocity", &ProjectileVelocity },
                luaL_Reg{ "hasStopped", &HasStopped } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
