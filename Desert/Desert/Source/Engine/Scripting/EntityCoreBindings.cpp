#include "Internal/ScriptRuntime.hpp"

#include <glm/gtc/quaternion.hpp>

namespace Desert::Scripting
{
    glm::vec3 CheckVec3( lua_State* L, int first )
    {
        if ( const float* v = lua_tovector( L, first ) )
            return { v[0], v[1], v[2] };
        return { static_cast<float>( luaL_checknumber( L, first ) ), static_cast<float>( luaL_checknumber( L, first + 1 ) ),
                 static_cast<float>( luaL_checknumber( L, first + 2 ) ) };
    }

    namespace
    {
        int Push3( lua_State* L, const glm::vec3& v )
        {
            lua_pushnumber( L, v.x );
            lua_pushnumber( L, v.y );
            lua_pushnumber( L, v.z );
            return 3;
        }

        ECS::TransformComponent* Transform( lua_State* L, int index = 1 )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, index );
            return ref.Registry->try_get<ECS::TransformComponent>( ref.Entity );
        }

        // ── identity / lifetime ──
        int Valid( lua_State* L )
        {
            const std::optional<LuauEntityRef> ref = LuauBinder::ToEntity( L, 1 );
            lua_pushboolean( L, ref && ref->Registry != nullptr && ref->Registry->valid( ref->Entity ) );
            return 1;
        }

        int Name( lua_State* L )
        {
            const LuauEntityRef ref = LuauBinder::CheckEntity( L, 1 );
            const auto*         tag = ref.Registry->try_get<ECS::TagComponent>( ref.Entity );
            lua_pushstring( L, tag != nullptr ? tag->Tag.c_str() : "" );
            return 1;
        }

        // Removes this entity (and its subtree) from the scene; its scripts are released once the call returns.
        int Destroy( lua_State* L )
        {
            const std::optional<LuauEntityRef> ref = LuauBinder::ToEntity( L, 1 );
            ScriptEngine::Impl&                host = ScriptEngine::Impl::Of( L );
            if ( !ref || ref->Registry == nullptr || !ref->Registry->valid( ref->Entity ) || host.Scene == nullptr )
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
                if ( Common::BoolResultStr r = host.Runtime->CallFrom( slot, function, L, 3, count ); !r.IsSuccess() )
                    LOG_ERROR( "[Lua] {} error: {}", function, r.GetError() );
            }
            return 0;
        }

        // Socket-attach this entity to a bone of `target` (UE-style): weapon:attachTo(player, "hand_r").
        int AttachTo( lua_State* L )
        {
            const LuauEntityRef self   = LuauBinder::CheckEntity( L, 1 );
            const LuauEntityRef target = LuauBinder::CheckEntity( L, 2 );
            const char*         socket = luaL_checkstring( L, 3 ); // a skeleton socket, or a bone name
            Common::UUID        targetId;
            if ( const auto* id = target.Registry->try_get<ECS::UUIDComponent>( target.Entity ) )
                targetId = id->UUID;
            auto& attachment      = self.Registry->get_or_emplace<ECS::SocketAttachmentComponent>( self.Entity );
            attachment.Target     = targetId;
            attachment.SocketName = socket;
            return 0;
        }

        int Detach( lua_State* L )
        {
            const LuauEntityRef self = LuauBinder::CheckEntity( L, 1 );
            self.Registry->remove<ECS::SocketAttachmentComponent>( self.Entity );
            return 0;
        }

        // ── transform (numbers; component("Transform") is the same data as vectors) ──
        int GetPosition( lua_State* L )
        {
            const ECS::TransformComponent* t = Transform( L );
            return Push3( L, t != nullptr ? t->Translation : glm::vec3( 0.0f ) );
        }
        int SetPosition( lua_State* L )
        {
            if ( ECS::TransformComponent* t = Transform( L ) )
                t->Translation = CheckVec3( L, 2 );
            return 0;
        }
        int Translate( lua_State* L )
        {
            if ( ECS::TransformComponent* t = Transform( L ) )
                t->Translation += CheckVec3( L, 2 );
            return 0;
        }
        int GetRotation( lua_State* L ) // euler radians
        {
            const ECS::TransformComponent* t = Transform( L );
            return Push3( L, t != nullptr ? t->Rotation : glm::vec3( 0.0f ) );
        }
        int SetRotation( lua_State* L )
        {
            if ( ECS::TransformComponent* t = Transform( L ) )
                t->Rotation = CheckVec3( L, 2 );
            return 0;
        }
        int GetScale( lua_State* L )
        {
            const ECS::TransformComponent* t = Transform( L );
            return Push3( L, t != nullptr ? t->Scale : glm::vec3( 1.0f ) );
        }
        int SetScale( lua_State* L )
        {
            if ( ECS::TransformComponent* t = Transform( L ) )
                t->Scale = CheckVec3( L, 2 );
            return 0;
        }

        // World-space directions from the euler rotation (the transform matrix's quat convention): forward = -Z.
        int Forward( lua_State* L )
        {
            const ECS::TransformComponent* t = Transform( L );
            return Push3( L, t != nullptr ? glm::quat( t->Rotation ) * glm::vec3( 0.0f, 0.0f, -1.0f )
                                          : glm::vec3( 0.0f, 0.0f, -1.0f ) );
        }
        int Right( lua_State* L )
        {
            const ECS::TransformComponent* t = Transform( L );
            return Push3( L, t != nullptr ? glm::quat( t->Rotation ) * glm::vec3( 1.0f, 0.0f, 0.0f )
                                          : glm::vec3( 1.0f, 0.0f, 0.0f ) );
        }
        int DistanceTo( lua_State* L )
        {
            const ECS::TransformComponent* a = Transform( L, 1 );
            const ECS::TransformComponent* b = Transform( L, 2 );
            lua_pushnumber( L, ( a != nullptr && b != nullptr ) ? glm::distance( a->Translation, b->Translation ) : -1.0 );
            return 1;
        }
    } // namespace

    void RegisterEntityCoreBindings( lua_State* L )
    {
        for ( const luaL_Reg& method : { luaL_Reg{ "valid", &Valid }, luaL_Reg{ "name", &Name },
                                         luaL_Reg{ "destroy", &Destroy }, luaL_Reg{ "call", &Call },
                                         luaL_Reg{ "attachTo", &AttachTo }, luaL_Reg{ "detach", &Detach },
                                         luaL_Reg{ "getPosition", &GetPosition }, luaL_Reg{ "setPosition", &SetPosition },
                                         luaL_Reg{ "translate", &Translate }, luaL_Reg{ "getRotation", &GetRotation },
                                         luaL_Reg{ "setRotation", &SetRotation }, luaL_Reg{ "getScale", &GetScale },
                                         luaL_Reg{ "setScale", &SetScale }, luaL_Reg{ "forward", &Forward },
                                         luaL_Reg{ "right", &Right }, luaL_Reg{ "distanceTo", &DistanceTo } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
