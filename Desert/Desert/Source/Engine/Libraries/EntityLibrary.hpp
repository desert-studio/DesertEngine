#pragma once

#include <Engine/ECS/EntityValue.hpp>
#include <Engine/Libraries/DeformationLibrary.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <limits>
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

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "attachTo" ),
                  Tooltip( "Socket-attaches to a skeleton socket (or a bone name) of target." ) )
        static void AttachTo( ECS::Entity entity, ECS::Entity target, const std::string& socket );

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

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "forward" ),
                  Tooltip( "World forward (-Z) of the rotation." ) )
        static glm::vec3 Forward( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "right" ) )
        static glm::vec3 Right( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "distanceTo" ),
                  Tooltip( "-1 when either has no transform." ) )
        static float DistanceTo( ECS::Entity entity, ECS::Entity other );
    };

    /// Gameplay verbs of a character; each is a no-op without a CharacterControllerComponent.
    struct CharacterLibrary
    {
        REFLECT( ScriptName( "Character" ) )

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "move" ),
                  Tooltip( "UE AddMovementInput: camera-relative forward and right (-1..1); the speed is the "
                           "controller's (Max Walk Speed / Crouched / Swim)." ) )
        static void Move( ECS::Entity entity, float forward, float right );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "jump" ),
                  Tooltip( "UE Jump: Jump Z Velocity from the ground, not while crouched." ) )
        static void Jump( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "crouch" ),
                  Tooltip( "UE Crouch / UnCrouch: the wish; standing up waits for head room." ) )
        static void Crouch( ECS::Entity entity, bool crouch );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isCrouched" ) )
        static bool IsCrouched( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isOnGround" ) )
        static bool IsOnGround( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setSwimming" ) )
        static void SetSwimming( ECS::Entity entity, bool swimming );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "swim" ),
                  Tooltip( "The up/down intent while swimming." ) )
        static void Swim( ECS::Entity entity, float vertical );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isSwimming" ) )
        static bool IsSwimming( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "addYaw" ),
                  Tooltip( "Turns the whole entity (radians)." ) )
        static void AddYaw( ECS::Entity entity, float radians );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "addCameraPitch" ),
                  Tooltip( "Tilts the child camera, clamped to +-85 degrees." ) )
        static void AddCameraPitch( ECS::Entity entity, float radians );
    };

    /// The verbs of a projectile entity (UE UProjectileMovementComponent's Velocity / SetVelocityInLocalSpace and
    /// AActor::SetInstigator). The movement itself is ECS/System/ProjectileMovement.cpp; each verb is a no-op on
    /// an entity without a ProjectileComponent.
    struct ProjectileLibrary
    {
        REFLECT( ScriptName( "Projectile" ) )

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "launch" ),
                  Tooltip( "Flies (again) at that world velocity, cm/s, ignoring the instigator's body - what a "
                           "weapon script calls on the projectile World.spawn placed." ) )
        static void Launch( ECS::Entity entity, const glm::vec3& velocity, ECS::Entity instigator = {} );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "projectileVelocity" ),
                  Tooltip( "cm/s; zero once it stopped." ) )
        static glm::vec3 ProjectileVelocity( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "hasStopped" ),
                  Tooltip( "UE HasStoppedSimulation: a blocking hit ended its flight." ) )
        static bool HasStopped( ECS::Entity entity );
    };

    /// The unified material protocol: parameter overrides on the entity's Shader Override, else on its static
    /// mesh's slot 0 (UE SetVectorParameterValueOnMaterials, its MID made on demand).
    struct MaterialLibrary
    {
        REFLECT( ScriptName( "Material" ) )

        FUNCTION(
             ScriptCallable, ScriptMethod, ScriptName( "setMaterialParam" ),
             Tooltip( "value = {x, y, z, w}; fails on an entity with no static mesh and no Shader Override." ) )
        static void SetMaterialParam( ECS::Entity entity, const std::string& name, const glm::vec4& value );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getMaterialParam" ) )
        static glm::vec4 GetMaterialParam( ECS::Entity entity, const std::string& name );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "clearMaterialParams" ) )
        static void ClearMaterialParams( ECS::Entity entity );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setShader" ),
                  Tooltip( "A surface shader by name; \"\" drops the Shader Override (back to the lit slots)." ) )
        static void SetShader( ECS::Entity entity, const std::string& shader );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "getShader" ) )
        static std::string GetShader( ECS::Entity entity );
    };

    /// What a trace through the world found — a port of UE's FHitResult (Engine/HitResult.h) to what
    /// Scene::Raycast measures: whether something blocked the trace, where and with which surface normal, how far
    /// along it and the entity hit. A miss leaves bBlockingHit false and Entity nil.
    struct HitResult
    {
        REFLECT( ScriptStruct )

        PROPERTY()
        bool bBlockingHit = false;

        PROPERTY()
        float Distance = 0.0f; // from the trace's origin to Location, world units (cm)

        PROPERTY()
        glm::vec3 Location = glm::vec3( 0.0f );

        PROPERTY()
        glm::vec3 Normal = glm::vec3( 0.0f, 1.0f, 0.0f );

        PROPERTY()
        ECS::Entity Entity;
    };

    /// A ray in the world: where it starts and the unit direction it travels (UE's DeprojectScreenPositionToWorld
    /// pair WorldLocation / WorldDirection).
    struct WorldRay
    {
        REFLECT( ScriptStruct )

        PROPERTY()
        glm::vec3 Origin = glm::vec3( 0.0f );

        PROPERTY()
        glm::vec3 Direction = glm::vec3( 0.0f, 0.0f, -1.0f );
    };

    /// The world of the running script (Core::WorldContext): find, spawn, trace.
    struct WorldLibrary
    {
        REFLECT( ScriptName( "World" ) )

        FUNCTION( ScriptCallable, ScriptName( "find" ), Tooltip( "The first entity tagged `name`, or nil." ) )
        static ECS::Entity Find( const std::string& name );

        FUNCTION( ScriptCallable, ScriptName( "spawn" ), Tooltip( "Places a prefab; nil (logged) on failure." ) )
        static ECS::Entity Spawn( const std::string& prefab, const glm::vec3& position );

        FUNCTION( ScriptCallable, ScriptName( "spawnMarker" ),
                  Tooltip( "A debug sphere drawn with the DebugColor template." ) )
        static ECS::Entity SpawnMarker( const glm::vec3& position, float scale, const glm::vec3& color );

        FUNCTION(
             ScriptCallable, ScriptName( "raycast" ),
             Tooltip( "The nearest static-mesh or landscape hit along a ray, within maxDistance (unbounded by "
                      "default)." ) )
        static HitResult Raycast( const glm::vec3& origin, const glm::vec3& direction,
                                  float maxDistance = std::numeric_limits<float>::max() );

        FUNCTION(
             ScriptCallable, ScriptName( "cameraRay" ),
             Tooltip( "The active camera's eye ray (looking down -Z from the origin when there is no camera)." ) )
        static WorldRay CameraRay();

        /// The script source of the world's DeformationWorld (Engine/World/Deformation). Mechanism only: what the
        /// contact looks like is the named stamp asset's; what the surface does with it is the physical material's
        /// (DEFORM-RESPONSE). Defined in DeformationLibrary.cpp.
        FUNCTION( ScriptCallable, ScriptName( "deform" ),
                  Tooltip( "Presses a DeformStamp into the world's deformation field at a point: "
                           "World.deform{ Stamp = name, At = vector(x, y, z), Params = { Radius = cm, ... } }. "
                           "Ok, or false and the reason." ) )
        static Common::BoolResultStr Deform( const DeformCall& call );
    };
} // namespace Desert::Libraries
