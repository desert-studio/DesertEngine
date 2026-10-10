#pragma once

#include <Engine/ECS/EntityValue.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>

namespace Desert::Libraries
{
    /// An entity's identity, attachment and transform — methods of the entity in a script (ScriptMethod:
    /// `self:getPosition()`). Rotation is euler radians; the same data is `self:component("Transform")`.
    struct EntityLibrary
    {
        REFLECT( ScriptName( "Entity" ) )

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "name" ) )
        static std::string Name( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "attachTo" ), Tooltip( "Socket-attaches to a bone of target." ) )
        static void AttachTo( ECS::Entity entity, ECS::Entity target, const std::string& bone );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "detach" ) )
        static void Detach( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getPosition" ) )
        static glm::vec3 GetPosition( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setPosition" ) )
        static void SetPosition( ECS::Entity entity, const glm::vec3& position );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "translate" ) )
        static void Translate( ECS::Entity entity, const glm::vec3& offset );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getRotation" ) )
        static glm::vec3 GetRotation( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setRotation" ) )
        static void SetRotation( ECS::Entity entity, const glm::vec3& euler );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getScale" ) )
        static glm::vec3 GetScale( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setScale" ) )
        static void SetScale( ECS::Entity entity, const glm::vec3& scale );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "forward" ), Tooltip( "World forward (-Z) of the rotation." ) )
        static glm::vec3 Forward( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "right" ) )
        static glm::vec3 Right( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "distanceTo" ), Tooltip( "-1 when either has no transform." ) )
        static float DistanceTo( ECS::Entity entity, ECS::Entity other );
    };

    /// Gameplay verbs of a character; each is a no-op without a CharacterControllerComponent.
    struct CharacterLibrary
    {
        REFLECT( ScriptName( "Character" ) )

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "move" ), Tooltip( "forward = W/S axis, right = D/A axis (-1..1), speed." ) )
        static void Move( ECS::Entity entity, float forward, float right, float speed );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "jump" ) )
        static void Jump( ECS::Entity entity, float strength );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isOnGround" ) )
        static bool IsOnGround( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setSwimming" ) )
        static void SetSwimming( ECS::Entity entity, bool swimming );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "swim" ), Tooltip( "The up/down intent while swimming." ) )
        static void Swim( ECS::Entity entity, float vertical );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isSwimming" ) )
        static bool IsSwimming( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "addYaw" ), Tooltip( "Turns the whole entity (radians)." ) )
        static void AddYaw( ECS::Entity entity, float radians );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "addCameraPitch" ), Tooltip( "Tilts the child camera, clamped to +-85 degrees." ) )
        static void AddCameraPitch( ECS::Entity entity, float radians );
    };

    /// The unified material protocol: parameter overrides on the entity's material.
    struct MaterialLibrary
    {
        REFLECT( ScriptName( "Material" ) )

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setMaterialParam" ), Tooltip( "value = {x, y, z, w}." ) )
        static void SetMaterialParam( ECS::Entity entity, const std::string& name, const glm::vec4& value );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getMaterialParam" ) )
        static glm::vec4 GetMaterialParam( ECS::Entity entity, const std::string& name );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "clearMaterialParams" ) )
        static void ClearMaterialParams( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setShader" ) )
        static void SetShader( ECS::Entity entity, const std::string& shader );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getShader" ) )
        static std::string GetShader( ECS::Entity entity );
    };

    /// The world of the running script (Core::WorldContext): find, spawn.
    struct WorldLibrary
    {
        REFLECT( ScriptName( "World" ) )

        FUNCTION( ScriptCallable, ScriptName( "find" ), Tooltip( "The first entity tagged `name`, or nil." ) )
        static ECS::Entity Find( const std::string& name );

        FUNCTION( ScriptCallable, ScriptName( "spawn" ), Tooltip( "Places a prefab; nil (logged) on failure." ) )
        static ECS::Entity Spawn( const std::string& prefab, const glm::vec3& position );

        FUNCTION( ScriptCallable, ScriptName( "spawnMarker" ), Tooltip( "A debug sphere drawn with the DebugColor template." ) )
        static ECS::Entity SpawnMarker( const glm::vec3& position, float scale, const glm::vec3& color );
    };
} // namespace Desert::Libraries
