#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/System/PhysicsBodyLifetime.hpp>
#include <Engine/ECS/System/LandscapeCollision.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/KeyCodes.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>

namespace Desert::ECS
{
    // Drives the Jolt PhysicsWorld from ECS data. Lifecycle is gated on the scene's play state:
    //   - Edit:   no simulation. Any live world is torn down so authored transforms stay freely
    //             editable (you can move/select physics bodies in the viewport).
    //   - Play:   bodies are (lazily) created from the authored transform of every entity that has a
    //             RigidBody + Collider, the simulation steps, and the resulting pose is written back
    //             into TransformComponent (so the mesh renderer draws bodies where physics puts them).
    //   - Paused: bodies persist but the simulation is frozen (no step, no writeback).
    //
    // The editor snapshots the scene on Play and restores it on Stop, so the world is rebuilt fresh
    // each Play (RuntimeBody handles come back as kInvalidBody after the snapshot is reloaded).
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT. An invisible wall you still collide with is a
    // DIFFERENT FEATURE from an invisible wall: skipping hidden bodies would let one tick in the outliner
    // silently change the simulation, and bodies are built from authored transforms the moment Play starts.
    // Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class PhysicsECSSystem final : public System
    {
    public:
        explicit PhysicsECSSystem( Core::Scene* scene ) : m_Scene( scene )
        {
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ts ) override
        {
            using SceneState = Core::Scene::SceneState;

            const SceneState state   = m_Scene ? m_Scene->GetState() : SceneState::Edit;
            const bool       playing = ( state == SceneState::Play );
            const bool       active  = ( state == SceneState::Play || state == SceneState::Paused );

            if ( !active )
            {
                // Back to Edit (or never played): drop the live simulation. The scene is re-deserialized
                // from the play snapshot on Stop, so the entities' RuntimeBody handles reset themselves.
                if ( m_World )
                {
                    m_Lifetime.reset(); // stop releasing into a world that is about to stop existing
                    m_Landscape.reset();
                    m_World->Shutdown();
                    m_World.reset();
                }
                return;
            }

            auto bodies = registry.view<TransformComponent, RigidBodyComponent, ColliderComponent>();

            if ( !m_World )
            {
                m_World = std::make_unique<Physics::PhysicsWorld>();
                m_AppliedGravity = m_Scene ? m_Scene->GetSettings().Gravity : Core::SceneSettings{}.Gravity;
                m_World->Init( m_AppliedGravity );
                m_Lifetime  = std::make_unique<PhysicsBodyLifetime>( *m_World );
                m_Landscape = std::make_unique<LandscapeCollision>( *m_World );
            }
            else if ( m_Scene && m_Scene->GetSettings().Gravity != m_AppliedGravity )
            {
                // The setting is live: dragging it in Details changes the fall of everything already
                // simulating, rather than waiting for the next Play. Compared rather than assigned every
                // frame so Jolt is not told the same number sixty times a second.
                m_AppliedGravity = m_Scene->GetSettings().Gravity;
                m_World->SetGravity( m_AppliedGravity );
            }

            // Every body created below is given back by the listener when its entity (or its collider) goes —
            // see PhysicsBodyLifetime.hpp. Re-armed each frame because a reloaded scene may be a new registry.
            m_Lifetime->Attach( registry );
            m_Landscape->Attach( registry );
            // Refused tiles get no body; LandscapeECSSystem reports them (it applies the same test).
            m_Landscape->Sync( DrawableLandscapeTiles( registry ).Tiles );

            // Create a Jolt body for any physics entity that doesn't have one yet (uses its authored pose).
            for ( auto entity : bodies )
            {
                auto& rb = bodies.get<RigidBodyComponent>( entity );
                if ( rb.RuntimeBody != Physics::kInvalidBody )
                    continue;

                const auto& transform = bodies.get<TransformComponent>( entity );
                const auto& collider  = bodies.get<ColliderComponent>( entity );

                Physics::BodyDesc desc;
                desc.Shape       = collider.Data.Shape;
                desc.HalfExtents = collider.Data.HalfExtents;
                desc.Radius      = collider.Data.Radius;
                desc.HalfHeight  = collider.Data.HalfHeight;
                desc.Type        = rb.Data.Type;
                desc.Mass        = rb.Data.Mass;
                desc.Friction    = rb.Data.Friction;
                desc.Restitution = rb.Data.Restitution;

                // Use the WORLD pose (walk parents) so a collider on a CHILD entity (e.g. a wall inside a
                // "House" prefab root) is created where it actually is, not at its local offset.
                glm::mat4    world = transform.GetTransform();
                entt::entity cur   = entity;
                while ( registry.has<RelationshipComponent>( cur ) )
                {
                    const auto& rel = registry.get<RelationshipComponent>( cur );
                    if ( rel.Parent == entt::null )
                        break;
                    cur = rel.Parent;
                    if ( registry.has<TransformComponent>( cur ) )
                        world = registry.get<TransformComponent>( cur ).GetTransform() * world;
                }
                desc.Position = glm::vec3( world[3] );
                glm::mat3 basis( world ); // strip scale so quat_cast gives a clean rotation
                if ( glm::length( basis[0] ) > 1e-6f ) basis[0] = glm::normalize( basis[0] );
                if ( glm::length( basis[1] ) > 1e-6f ) basis[1] = glm::normalize( basis[1] );
                if ( glm::length( basis[2] ) > 1e-6f ) basis[2] = glm::normalize( basis[2] );
                desc.Rotation = glm::quat_cast( basis );

                rb.RuntimeBody = m_World->CreateBody( desc );
            }

            // Create a Jolt CharacterVirtual for any character entity that doesn't have one (authored pose).
            auto characters = registry.view<TransformComponent, CharacterControllerComponent>();
            for ( auto entity : characters )
            {
                auto& cc = characters.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter != Physics::kInvalidCharacter )
                    continue;
                const auto& transform = characters.get<TransformComponent>( entity );

                Physics::CharacterDesc desc;
                desc.Radius      = cc.Data.Radius;
                desc.HalfHeight  = glm::max( ( cc.Data.Height - 2.0f * cc.Data.Radius ) * 0.5f, 0.01f );
                desc.Position    = transform.Translation; // capsule center
                desc.MaxSlopeDeg = cc.Data.MaxSlopeDeg;
                cc.RuntimeCharacter   = m_World->CreateCharacter( desc );
                cc.VerticalVelocity   = 0.0f;
            }

            if ( !playing )
                return; // Paused: bodies exist but time is frozen.

            m_World->Step( ts.GetSeconds() );

            // Write the simulated pose back into the transform for moving bodies.
            for ( auto entity : bodies )
            {
                auto& rb = bodies.get<RigidBodyComponent>( entity );
                if ( rb.RuntimeBody == Physics::kInvalidBody || rb.Data.Type == Physics::BodyType::Static )
                    continue;

                auto& transform       = bodies.get<TransformComponent>( entity );
                transform.Translation = m_World->GetPosition( rb.RuntimeBody );
                transform.Rotation    = glm::eulerAngles( m_World->GetRotation( rb.RuntimeBody ) );
            }

            // ---- Character controllers: the controller SCRIPT (e.g. player_controller.lua) sets the move
            // INTENT each frame; the engine only resolves it against the camera and steps Jolt. Cursor
            // capture, mouse delta and the WASD/sprint/look POLICY now live in the script (via ScriptSystem) —
            // engine = mechanism, script = behavior. ----
            const float dt = ts.GetSeconds();

            // Movement basis from the active camera (flattened to the ground plane). The script's MoveInput is
            // in local forward/right axes; resolving it here keeps "forward" following wherever the camera looks.
            glm::vec3 camFwd( 0.0f, 0.0f, -1.0f );
            glm::vec3 camRight( 1.0f, 0.0f, 0.0f );
            if ( auto cam = m_Scene->GetMainCamera().lock() )
            {
                const glm::mat4 c2w = glm::inverse( cam->GetViewMatrix() );
                camFwd              = -glm::vec3( c2w[2] );
                camRight            = glm::vec3( c2w[0] );
            }
            camFwd.y   = 0.0f;
            camRight.y = 0.0f;
            if ( glm::length( camFwd ) > 1e-4f )
                camFwd = glm::normalize( camFwd );
            if ( glm::length( camRight ) > 1e-4f )
                camRight = glm::normalize( camRight );

            for ( auto entity : characters )
            {
                auto& cc = characters.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter == Physics::kInvalidCharacter )
                    continue;

                // World move direction from the script's local intent (y = forward, x = strafe/right).
                glm::vec3 wish = camFwd * cc.MoveInput.y + camRight * cc.MoveInput.x;
                if ( glm::length( wish ) > 1e-4f )
                    wish = glm::normalize( wish );

                const bool onGround = m_World->IsCharacterOnGround( cc.RuntimeCharacter );
                cc.OnGround         = onGround; // exposed to scripts via self:isOnGround()
                // Gravity is authored per-character (CharacterControllerData::Gravity), not a baked constant —
                // default ~2x real so the jump arc is SNAPPY (real 9.81 feels floaty / cartoonish).
                const float kGravity = cc.Data.Gravity;

                glm::vec3 horiz;
                if ( cc.Swimming )
                {
                    // BUOYANCY: no hard gravity. Vertical follows the swim intent (up/down); when neutral the
                    // body drifts gently up toward the surface. Full 3D control, but slower than on land.
                    const float targetV = cc.SwimVertical * cc.DesiredSpeed;
                    cc.VerticalVelocity = glm::mix( cc.VerticalVelocity, targetV, 0.15f );
                    if ( glm::abs( cc.SwimVertical ) < 0.01f )
                        cc.VerticalVelocity += 2.5f * dt; // gentle rise (float up)
                    cc.JumpRequested = false;

                    horiz          = wish * ( cc.DesiredSpeed * 0.6f ); // water drag
                    cc.AirVelocity = { horiz.x, horiz.z };
                }
                else
                {
                    if ( onGround )
                        cc.VerticalVelocity = cc.JumpRequested ? cc.JumpStrength : -1.0f; // jump (script) or stick
                    else
                        cc.VerticalVelocity -= kGravity * dt;
                    cc.JumpRequested = false; // one-shot, consumed

                    // NO AIR CONTROL: on the ground the script's input steers (and we remember that horizontal
                    // velocity); in the air the takeoff velocity is locked, so a jump goes one direction and
                    // there's no bunny-hop strafing.
                    if ( onGround )
                    {
                        horiz          = wish * cc.DesiredSpeed;
                        cc.AirVelocity = { horiz.x, horiz.z }; // capture for the moment we leave the ground
                    }
                    else
                    {
                        horiz = { cc.AirVelocity.x, 0.0f, cc.AirVelocity.y };
                    }
                }
                const glm::vec3 vel( horiz.x, cc.VerticalVelocity, horiz.z );
                m_World->UpdateCharacter( cc.RuntimeCharacter, vel, dt );

                cc.CurrentSpeed = glm::length( glm::vec2( horiz.x, horiz.z ) ); // 0 when idle, drives anim

                auto& transform       = characters.get<TransformComponent>( entity );
                transform.Translation = m_World->GetCharacterPosition( cc.RuntimeCharacter );

                // NOTE: physics only PRODUCES state (cc.CurrentSpeed / cc.OnGround). Mapping that state to a
                // locomotion clip is behaviour and lives in LocomotionSystem (runs after this), not here.
            }
        }

    private:
        Core::Scene*                           m_Scene = nullptr;
        std::unique_ptr<Physics::PhysicsWorld> m_World;
        // Declared AFTER m_World so it is destroyed first: it releases into the world, never the other way.
        std::unique_ptr<PhysicsBodyLifetime> m_Lifetime;
        // Same rule: the landscape's heightfield bodies live in m_World.
        std::unique_ptr<LandscapeCollision> m_Landscape;
        // Last value handed to the world, so a change in SceneSettings can be noticed without asking Jolt.
        float m_AppliedGravity = 0.0f;
    };
} // namespace Desert::ECS
