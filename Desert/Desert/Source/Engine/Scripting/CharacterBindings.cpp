#include "Internal/ScriptRuntime.hpp"

namespace Desert::Scripting
{
    namespace
    {
        ECS::CharacterControllerComponent* Character( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            return ref.Registry->try_get<ECS::CharacterControllerComponent>( ref.Entity );
        }

        // forward = W/S axis, right = D/A axis (each -1..1), speed in m/s.
        int Move( lua_State* L )
        {
            if ( auto* cc = Character( L ) )
            {
                cc->MoveInput    = { static_cast<float>( luaL_checknumber( L, 3 ) ),
                                     static_cast<float>( luaL_checknumber( L, 2 ) ) };
                cc->DesiredSpeed = static_cast<float>( luaL_checknumber( L, 4 ) );
            }
            return 0;
        }
        int Jump( lua_State* L )
        {
            if ( auto* cc = Character( L ) )
            {
                cc->JumpRequested = true;
                cc->JumpStrength  = static_cast<float>( luaL_checknumber( L, 2 ) );
            }
            return 0;
        }
        int IsOnGround( lua_State* L )
        {
            const auto* cc = Character( L );
            lua_pushboolean( L, static_cast<int>( cc != nullptr && cc->OnGround ) );
            return 1;
        }
        // Swimming (buoyancy): toggled when the body crosses the water surface; `vertical` is the up/down intent.
        int SetSwimming( lua_State* L )
        {
            if ( auto* cc = Character( L ) )
                cc->Swimming = lua_toboolean( L, 2 ) != 0;
            return 0;
        }
        int Swim( lua_State* L )
        {
            if ( auto* cc = Character( L ) )
                cc->SwimVertical = static_cast<float>( luaL_checknumber( L, 2 ) );
            return 0;
        }
        int IsSwimming( lua_State* L )
        {
            const auto* cc = Character( L );
            lua_pushboolean( L, static_cast<int>( cc != nullptr && cc->Swimming ) );
            return 1;
        }
        // Yaw turns the whole entity (body + child camera follow through the hierarchy).
        int AddYaw( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            if ( auto* t = ref.Registry->try_get<ECS::TransformComponent>( ref.Entity ) )
                t->Rotation.y += static_cast<float>( luaL_checknumber( L, 2 ) );
            return 0;
        }
        // Pitch tilts the child camera only (look up/down), clamped to avoid flipping over.
        int AddCameraPitch( lua_State* L )
        {
            const LuauEntityRef ref     = LuauBinder::CheckEntity( L, 1 );
            const auto          radians = static_cast<float>( luaL_checknumber( L, 2 ) );
            auto&               reg     = *ref.Registry;
            const auto*         rel     = reg.try_get<ECS::RelationshipComponent>( ref.Entity );
            if ( rel == nullptr )
                return 0;
            for ( entt::entity const child : rel->Children )
            {
                if ( reg.has<ECS::CameraComponent>( child ) && reg.has<ECS::TransformComponent>( child ) )
                {
                    auto& rot = reg.get<ECS::TransformComponent>( child ).Rotation;
                    rot.x     = glm::clamp( rot.x + radians, glm::radians( -85.0f ), glm::radians( 85.0f ) );
                    break;
                }
            }
            return 0;
        }
    } // namespace

    // Gameplay verbs of a character entity; each is a no-op without a CharacterControllerComponent.
    void RegisterCharacterBindings( lua_State* L )
    {
        for ( const luaL_Reg& method :
              { luaL_Reg{ "move", &Move }, luaL_Reg{ "jump", &Jump }, luaL_Reg{ "isOnGround", &IsOnGround },
                luaL_Reg{ "setSwimming", &SetSwimming }, luaL_Reg{ "swim", &Swim },
                luaL_Reg{ "isSwimming", &IsSwimming }, luaL_Reg{ "addYaw", &AddYaw },
                luaL_Reg{ "addCameraPitch", &AddCameraPitch } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
