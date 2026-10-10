#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/System/CharacterMovement.hpp>
#include <Engine/ECS/System/SpringArm.hpp>
#include <Engine/ECS/System/PhysicsBodyLifetime.hpp>
#include <Engine/ECS/System/KinematicBodies.hpp>
#include <Engine/ECS/System/DestructibleLifetime.hpp>
#include <Engine/ECS/System/WaterBodyGather.hpp>
#include <Engine/ECS/System/LandscapeCollision.hpp>
#include <Engine/ECS/System/ColliderMesh.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/PhysicsEvents.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Physics/CollisionProfiles.hpp>
#include <Common/Core/Constants.hpp>
#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/Water/WaterBodyQuery.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>

#include <Common/Core/Logger.hpp>

#include <Common/Core/KeyCodes.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <format>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

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

        void Update( entt::registry&         registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ts ) override
        {
            using SceneState = Core::Scene::SceneState;

            const SceneState state   = m_Scene ? m_Scene->GetState() : SceneState::Edit;
            const bool playing = m_Scene != nullptr && m_Scene->TicksGameplay(); // Play, or a paused frame skip
            const bool       active  = ( state == SceneState::Play || state == SceneState::Paused );

            if ( !active )
            {
                // Back to Edit (or never played): drop the live simulation. The scene is re-deserialized
                // from the play snapshot on Stop, so the entities' RuntimeBody handles reset themselves.
                if ( m_World )
                {
                    m_Lifetime.reset(); // stop releasing into a world that is about to stop existing
                    m_RefusedColliders.clear();
                    m_BodyEntities.clear();
                    if ( auto* queue = registry.try_ctx<PhysicsEventQueue>() )
                        queue->Events.clear();
                    m_RefusedCharacters.clear();
                    m_Landscape.reset();
                    m_Destructibles.reset();
                    m_Destruction.reset();
                    m_Water.reset();
                    m_WaterGather.Reset();
                    m_World->Shutdown();
                    m_World.reset();
                }
                m_ProfilesRefused = false; // the next Play reads the register again
                return;
            }

            auto bodies = registry.view<TransformComponent, RigidBodyComponent, ColliderComponent>();

            if ( !m_World )
            {
                if ( m_ProfilesRefused )
                    return; // said once this Play; the register is read again on the next one
                // The project's channels and profiles (UE DefaultEngine.ini [CollisionProfile]). No register,
                // no simulation: there is no built-in answer to which bodies collide.
                auto profiles = Physics::CollisionProfiles::Read( Common::Constants::Path::PROJECT_CONFIG_PATH /
                                                                  Physics::kCollisionProfilesFileName );
                if ( !profiles )
                {
                    m_ProfilesRefused = true;
                    LOG_ERROR( "[Physics] the scene does not simulate: {}", profiles.GetError() );
                    return;
                }
                m_World          = std::make_unique<Physics::PhysicsWorld>();
                m_AppliedGravity = m_Scene ? m_Scene->GetSettings().Gravity : Core::SceneSettings{}.Gravity;
                m_World->Init( m_AppliedGravity, profiles.ExtractValue() );
                m_Lifetime  = std::make_unique<PhysicsBodyLifetime>( *m_World );
                m_Landscape = std::make_unique<LandscapeCollision>( *m_World );
                m_Destruction   = std::make_unique<Destruction::DestructionWorld>( *m_World );
                m_Destructibles = std::make_unique<DestructibleLifetime>( *m_Destruction );
                // The wave clock starts with the Play, at zero (UE: the water subsystem's time of a new world).
                m_Water = std::make_unique<Water::WaterSubsystem>();
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
            m_Destructibles->Attach( registry );
            m_Destructibles->Sync( registry, []( const Assets::AssetHandle& fracture )
                                   { return Runtime::ResourceRegistry::GetFractureService()->Get( fracture ); } );
            // Refused tiles get no body; LandscapeECSSystem reports them (it applies the same test).
            m_Landscape->Sync( DrawableLandscapeTiles( registry ).Tiles );

            // Create a Jolt body for any physics entity that doesn't have one yet (uses its authored pose).
            for ( auto entity : bodies )
            {
                auto& rb = bodies.get<RigidBodyComponent>( entity );
                if ( rb.RuntimeBody != Physics::kInvalidBody || m_RefusedColliders.contains( entity ) )
                    continue;

                const auto& transform = bodies.get<TransformComponent>( entity );
                const auto& collider  = bodies.get<ColliderComponent>( entity );

                Physics::BodyDesc desc;
                desc.Shape       = collider.Data.Shape;
                desc.HalfExtents = collider.Data.HalfExtents;
                desc.Radius      = collider.Data.Radius;
                desc.HalfHeight  = collider.Data.HalfHeight;
                desc.Axis        = collider.Data.Axis;
                desc.Center      = collider.Data.Center;
                desc.Type        = rb.Data.Type;
                desc.Mass        = rb.Data.Mass;
                desc.Friction    = rb.Data.Friction;
                desc.Restitution = rb.Data.Restitution;
                auto profile     = m_World->GetCollisionProfiles().Resolve( rb.Data.CollisionProfile );
                if ( !profile )
                {
                    RefuseCollider( entity, profile.GetError() );
                    continue;
                }
                desc.Profile = profile.GetValue();

                // The WORLD pose, so a collider on a CHILD entity is created where it actually is.
                const EntityWorldPose pose       = ComputeEntityWorldPose( registry, entity );
                const glm::vec3       worldScale = pose.Scale;
                desc.Position                    = pose.Position;
                desc.Rotation                    = pose.Rotation;

                std::optional<ColliderMesh> colliderMesh;
                if ( desc.Shape == Physics::ShapeType::Mesh || desc.Shape == Physics::ShapeType::ConvexHull )
                {
                    auto gathered = GatherColliderMesh( registry, entity, worldScale );
                    if ( !gathered.IsSuccess() )
                    {
                        RefuseCollider( entity, gathered.GetError() );
                        continue;
                    }
                    if ( !gathered.GetValue() )
                        continue; // the mesh asset is still loading: try again next frame
                    colliderMesh     = *gathered.GetValue();
                    desc.MeshPoints  = colliderMesh->Points;
                    desc.MeshIndices = colliderMesh->Indices;
                }

                auto created = m_World->CreateBody( desc );
                if ( !created.IsSuccess() )
                {
                    RefuseCollider( entity, created.GetError() );
                    continue;
                }
                rb.RuntimeBody = created.GetValue();
            }

            // Create a Jolt CharacterVirtual for any character entity that doesn't have one (authored pose).
            auto characters = registry.view<TransformComponent, CharacterControllerComponent>();
            for ( auto entity : characters )
            {
                auto& cc = characters.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter != Physics::kInvalidCharacter || m_RefusedCharacters.contains( entity ) )
                    continue;
                const auto& transform = characters.get<TransformComponent>( entity );

                if ( const auto created =
                          CharacterMovement::CreateCharacter( cc, *m_World, transform.Translation ); // capsule centre
                     !created )
                {
                    m_RefusedCharacters.insert( entity );
                    LOG_ERROR( "[Physics] entity {} has no character: {}", static_cast<uint32_t>( entity ),
                               created.GetError() );
                    continue;
                }
            }

            if ( !playing )
                return; // Paused (and not stepping): bodies exist but time is frozen.

            // The events of this frame's steps are readable until the next frame's physics.
            m_Destruction->ClearEvents();
            // The water the step's bodies are in: this frame's bodies over this frame's ground, gathered every
            // frame so a moved, added or removed body is the water at once. The landscape tiles' heights are
            // borrowed from the registry for the step's queries only. The water's clock advances by the FIXED
            // step, once per substep, in the pre-step callback below.
            m_Water->SetWorld( m_WaterGather.Gather(
                                    registry, []( const Assets::AssetHandle& waves )
                                    { return Runtime::ResourceRegistry::GetWaterWavesService()->Get( waves ); } ),
                               m_Scene->GatherRaycastLandscape().Tiles );

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

            // ---- Character controllers: the controller SCRIPT (e.g. player_controller.lua) sets the move
            // INTENT each frame; the engine resolves it against the camera and moves the capsule on every FIXED
            // step, so a character moves at the world's rate whatever the frame rate. Cursor capture, mouse
            // delta and the WASD/sprint/look POLICY live in the script (via ScriptSystem) — engine = mechanism,
            // script = behavior. Unsubscribed after Step: the callback holds this frame's registry. ----
            m_World->SetPreStepCallback(
                 [&]( float dt )
                 {
                     m_Water->Advance( dt );
                     StepCharacters( registry, camFwd, camRight, dt );
                 } );
            // A kinematic body follows its entity (UE: the component leads, the body travels to it over the
            // step's fixed steps); only a dynamic body's pose is written back below.
            DriveKinematicBodies( registry, *m_World );
            m_World->Step( ts.GetSeconds() );
            m_World->SetPreStepCallback( {} );
            PublishEvents( registry );
            // The steps' breaks are destruction's own news, published by its owner into its own queue.
            m_Destructibles->PublishEvents( registry );

            // Write the pose to DRAW back into the transform for moving bodies: interpolated between the last
            // two fixed steps, so motion is smooth at any frame rate.
            for ( auto entity : bodies )
            {
                auto& rb = bodies.get<RigidBodyComponent>( entity );
                if ( rb.RuntimeBody == Physics::kInvalidBody || rb.Data.Type != Physics::BodyType::Dynamic )
                    continue;

                auto& transform       = bodies.get<TransformComponent>( entity );
                transform.Translation = m_World->GetInterpolatedPosition( rb.RuntimeBody );
                transform.Rotation    = glm::eulerAngles( m_World->GetInterpolatedRotation( rb.RuntimeBody ) );
            }

            for ( auto entity : characters )
            {
                const auto& cc = characters.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter == Physics::kInvalidCharacter )
                    continue;
                characters.get<TransformComponent>( entity ).Translation =
                     m_World->GetInterpolatedCharacterPosition( cc.RuntimeCharacter );
            }

            // Spring arms after the characters moved (and after the scripts pitched them, ScriptSystem runs
            // first): the camera is placed against this frame's pawn and this frame's world.
            SpringArm::UpdateAll( registry, m_World.get(), ts.GetSeconds() );
        }

        // nullopt = the asset is still loading; an error = there is nothing to build from. Public: the editor's
        // Mesh To Collision tool fits its simple shapes to the same points a hull collider is cooked from.
        static Common::ResultStr<std::optional<ColliderMesh>>
        GatherColliderMesh( entt::registry& registry, entt::entity entity, const glm::vec3& scale )
        {
            using Result = std::optional<ColliderMesh>;
            if ( !registry.has<StaticMeshComponent>( entity ) )
                return Common::MakeError<Result>(
                     "the collider builds from its entity's StaticMesh, and there is none" );
            const auto& mesh = registry.get<StaticMeshComponent>( entity );

            switch ( PickColliderMeshSource( mesh ) )
            {
                case ColliderMeshSource::RuntimeMesh:
                    return Common::MakeSuccess( Result( BuildColliderMesh(
                         mesh.RuntimeMesh->GetVertices(), mesh.RuntimeMesh->GetIndices(), scale ) ) );
                case ColliderMeshSource::Primitive:
                {
                    // PickColliderMeshSource only returns Primitive when mesh.Primitive.has_value(); re-checked
                    // here so the access below is never through an empty optional (tidy can't see across the
                    // call).
                    if ( !mesh.Primitive.has_value() )
                        return Common::MakeError<Result>(
                             "primitive collider mesh source picked without a primitive" );
                    const DynamicMesh* shared = Geometry::PrimitiveMeshFactory::GetShared( *mesh.Primitive );
                    if ( shared == nullptr )
                        return Common::MakeError<Result>( std::format( "primitive {} has no shared mesh",
                                                                       static_cast<int>( *mesh.Primitive ) ) );
                    return Common::MakeSuccess(
                         Result( BuildColliderMesh( shared->GetVertices(), shared->GetIndices(), scale ) ) );
                }
                case ColliderMeshSource::Asset:
                {
                    const auto* service = Runtime::ResourceRegistry::GetMeshService();
                    if ( service == nullptr )
                        return Common::MakeError<Result>( "no mesh service to read the collider's mesh from" );
                    const Assets::MeshAsset* asset = service->GetAsset( mesh.MeshHandle );
                    if ( asset == nullptr )
                        return Common::MakeSuccess( Result{} );
                    const auto* staticAsset = dynamic_cast<const Assets::StaticMeshAsset*>( asset );
                    if ( staticAsset == nullptr )
                        return Common::MakeError<Result>(
                             "the StaticMesh's asset is skinned: a skinned mesh has no rest "
                             "collision, use a Box/Sphere/Capsule" );
                    return Common::MakeSuccess( Result(
                         BuildColliderMesh( staticAsset->GetVertices(), staticAsset->GetIndices(), scale ) ) );
                }
                case ColliderMeshSource::None:
                    break;
            }
            return Common::MakeError<Result>( "the entity's StaticMesh has no mesh assigned" );
        }

        /// The scene's destruction world while Play runs, null in Edit: what a field entity fires into
        /// (ECS::FireDestructionField), from the Sequencer or from gameplay.
        /// The scene's physics world while Play runs, null in Edit: what moves a body the game teleports
        /// (Core::LoadGameFromSlot restoring a pawn) so the next step does not put it back.
        [[nodiscard]] Physics::PhysicsWorld* GetPhysicsWorld() const
        {
            return m_World.get();
        }

        [[nodiscard]] Destruction::DestructionWorld* GetDestructionWorld() const
        {
            return m_Destruction.get();
        }

        /// The scene's water while Play runs, null in Edit: what buoyancy, swimming and scripts query
        /// (UE UWaterSubsystem / QueryWaterInfoClosestToWorldLocation).
        [[nodiscard]] const Water::WaterSubsystem* GetWater() const
        {
            return m_Water.get();
        }

    private:
        // This frame's steps' contact events, named by entity, into the registry's PhysicsEventQueue (see
        // PhysicsEvents.hpp). The body → entity map keeps an entity whose body went this frame until its
        // EndOverlap is named.
        void PublishEvents( entt::registry& registry )
        {
            auto& queue = registry.ctx_or_set<PhysicsEventQueue>();
            queue.Events.clear();
            ++queue.Publication;
            for ( auto entity : registry.view<RigidBodyComponent>() )
            {
                const auto& rb = registry.get<RigidBodyComponent>( entity );
                if ( rb.RuntimeBody != Physics::kInvalidBody )
                    m_BodyEntities[rb.RuntimeBody] = entity;
            }
            // A character is its inner body to the others (PhysicsWorld::GetCharacterBody): the pawn that walks
            // into a trigger is named by its entity.
            for ( auto entity : registry.view<CharacterControllerComponent>() )
            {
                const auto& cc = registry.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter == Physics::kInvalidCharacter )
                    continue;
                if ( const Physics::BodyHandle body = m_World->GetCharacterBody( cc.RuntimeCharacter );
                     body != Physics::kInvalidBody )
                    m_BodyEntities[body] = entity;
            }
            NameContactEvents( m_World->GetContactEvents(), m_BodyEntities, queue.Events );

            std::erase_if( m_BodyEntities,
                           [&]( const auto& entry )
                           {
                               if ( !registry.valid( entry.second ) )
                                   return true;
                               if ( const auto* rb = registry.try_get<RigidBodyComponent>( entry.second );
                                    rb != nullptr && rb->RuntimeBody == entry.first )
                                   return false;
                               const auto* cc = registry.try_get<CharacterControllerComponent>( entry.second );
                               return cc == nullptr || cc->RuntimeCharacter == Physics::kInvalidCharacter ||
                                      m_World->GetCharacterBody( cc->RuntimeCharacter ) != entry.first;
                           } );
        }

        // One fixed step of every character controller: the script's intent resolved against the camera basis,
        // gravity/jump/swim integrated over @p dt (the fixed step), the capsule moved through Jolt.
        void StepCharacters( entt::registry& registry, const glm::vec3& camFwd, const glm::vec3& camRight,
                             float dt )
        {
            auto characters = registry.view<TransformComponent, CharacterControllerComponent>();
            for ( auto entity : characters )
            {
                auto& cc = characters.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter == Physics::kInvalidCharacter )
                    continue;

                // World move intent from the script's camera-relative one (y = forward, x = right); the model
                // (walking / falling / crouch / swimming, UE CharacterMovementComponent) is CharacterMovement::Step,
                // run on every FIXED step.
                const glm::vec3 wish = camFwd * cc.MoveInput.y + camRight * cc.MoveInput.x;
                CharacterMovement::Step( cc, *m_World, wish, dt );

                // NOTE: physics only PRODUCES state (cc.Velocity / cc.OnGround). Mapping that state to a
                // locomotion clip is behaviour and lives in LocomotionSystem (runs after this), not here.
            }
        }

        // Said once per entity per Play: a refused collider would otherwise be retried, and logged, every frame.
        void RefuseCollider( entt::entity entity, const std::string& reason )
        {
            m_RefusedColliders.insert( entity );
            LOG_ERROR( "[Physics] entity {} has no body: {}", static_cast<uint32_t>( entity ), reason );
        }

        Core::Scene*                           m_Scene = nullptr;
        std::unique_ptr<Physics::PhysicsWorld> m_World;
        // Declared AFTER m_World so it is destroyed first: it releases into the world, never the other way.
        std::unique_ptr<PhysicsBodyLifetime> m_Lifetime;
        // Same rule: the landscape's heightfield bodies live in m_World.
        std::unique_ptr<LandscapeCollision> m_Landscape;
        // Same rule: the scene's destructibles are bodies in m_World, advanced by its fixed step.
        std::unique_ptr<Destruction::DestructionWorld> m_Destruction;
        // Same rule, one level down: the destructible entities' objects live in m_Destruction.
        std::unique_ptr<DestructibleLifetime> m_Destructibles;
        // The water bodies, ground and wave clock of this Play; null in Edit.
        std::unique_ptr<Water::WaterSubsystem> m_Water;
        // Refuses a body whose wave set cannot be read, once per entity per Play.
        WaterBodyGather m_WaterGather;
        // Last value handed to the world, so a change in SceneSettings can be noticed without asking Jolt.
        float m_AppliedGravity = 0.0f;
        // Entities whose collider was refused during this Play; cleared with the world.
        std::unordered_set<entt::entity> m_RefusedColliders;
        std::unordered_map<Physics::BodyHandle, entt::entity> m_BodyEntities; // see PublishEvents
        std::unordered_set<entt::entity> m_RefusedCharacters;
        bool                             m_ProfilesRefused = false;
    };
} // namespace Desert::ECS
