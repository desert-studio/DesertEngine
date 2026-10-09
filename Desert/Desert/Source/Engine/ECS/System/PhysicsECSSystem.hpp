#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/System/CharacterMovement.hpp>
#include <Engine/ECS/System/SpringArm.hpp>
#include <Engine/ECS/System/PhysicsBodyLifetime.hpp>
#include <Engine/ECS/System/EntityOverlaps.hpp>
#include <Engine/ECS/System/DestructibleLifetime.hpp>
#include <Engine/ECS/System/RagdollLifetime.hpp>
#include <Engine/ECS/System/LandscapeCollision.hpp>
#include <Engine/ECS/System/ColliderMesh.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>

#include <Common/Core/Logger.hpp>

#include <Common/Core/KeyCodes.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <format>
#include <memory>
#include <optional>
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
                    m_Ragdolls.reset();
                    m_Overlaps.reset(); // unsubscribes from the world
                    m_RefusedColliders.clear();
                    m_Landscape.reset();
                    m_Destructibles.reset();
                    m_Destruction.reset();
                    m_World->Shutdown();
                    m_World.reset();
                }
                return;
            }

            auto bodies = registry.view<TransformComponent, RigidBodyComponent, ColliderComponent>();

            if ( !m_World )
            {
                m_World          = std::make_unique<Physics::PhysicsWorld>();
                m_AppliedGravity = m_Scene ? m_Scene->GetSettings().Gravity : Core::SceneSettings{}.Gravity;
                m_World->Init( m_AppliedGravity );
                m_Lifetime  = std::make_unique<PhysicsBodyLifetime>( *m_World );
                m_Overlaps      = std::make_unique<EntityOverlapRouter>( *m_World );
                m_Landscape = std::make_unique<LandscapeCollision>( *m_World );
                m_Destruction   = std::make_unique<Destruction::DestructionWorld>( *m_World );
                m_Destructibles = std::make_unique<DestructibleLifetime>( *m_Destruction );
                m_Ragdolls      = std::make_unique<RagdollLifetime>( *m_World );
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
            m_Ragdolls->Attach( registry );
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
                desc.IsTrigger   = collider.Data.IsTrigger;
                desc.Overlaps    = { .Static     = collider.Data.OverlapStatic,
                                     .Kinematic  = collider.Data.OverlapKinematic,
                                     .Dynamic    = collider.Data.OverlapDynamic,
                                     .Characters = collider.Data.OverlapCharacters };

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
                m_Overlaps->Track( rb.RuntimeBody, entity );
            }

            // Create a Jolt CharacterVirtual for any character entity that doesn't have one (authored pose).
            auto characters = registry.view<TransformComponent, CharacterControllerComponent>();
            for ( auto entity : characters )
            {
                auto& cc = characters.get<CharacterControllerComponent>( entity );
                if ( cc.RuntimeCharacter != Physics::kInvalidCharacter )
                    continue;
                const auto& transform = characters.get<TransformComponent>( entity );

                CharacterMovement::CreateCharacter( cc, *m_World, transform.Translation ); // capsule centre
                if ( cc.RuntimeCharacter != Physics::kInvalidCharacter )
                    m_Overlaps->Track( m_World->GetCharacterBody( cc.RuntimeCharacter ), entity );
            }

            if ( !playing )
                return; // Paused (and not stepping): bodies exist but time is frozen.

            // The events of this frame's steps are readable until the next frame's physics.
            m_Destruction->ClearEvents();

            m_Overlaps->BeginFrame( registry );
            // A kinematic body follows its entity (the transform leads, the body travels to it over the step);
            // only a dynamic body's pose is written back below.
            DriveKinematicBodies( registry, *m_World );

            // Ragdolls around the step (UE: the physics asset's bodies in the physics scene, BlendInPhysics
            // after it). Animation ran BEFORE this system (RuntimeLayer / SceneWorkspace order), so the
            // animator's pose is this frame's animated pose: kinematic ragdolls are driven to it by this step,
            // and simulated ones are written back over it after the step, before skinning.
            m_Ragdolls->DriveBeforeStep(
                 registry,
                 []( const Assets::AssetHandle& asset, const Common::Content::AssetGuid& meshSkeleton,
                     std::string_view meshSkeletonName ) {
                     return Runtime::ResourceRegistry::GetPhysicsAssetService()->Get( asset, meshSkeleton,
                                                                                      meshSkeletonName );
                 },
                 &MeshSkeletonOf );
            const uint32_t fixedSteps = m_World->Step( ts.GetSeconds() );
            m_Destructibles->WritePoses( registry );
            m_Ragdolls->WriteBackAfterStep( registry );
            // Queued on both entities for their scripts (ScriptSystem, next frame) and given to C++ subscribers.
            m_Overlaps->Deliver( registry, fixedSteps );

            // Write the simulated pose back into the transform for dynamic bodies.
            for ( auto entity : bodies )
            {
                auto& rb = bodies.get<RigidBodyComponent>( entity );
                if ( rb.RuntimeBody == Physics::kInvalidBody || rb.Data.Type != Physics::BodyType::Dynamic )
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

                // World move intent from the script's camera-relative one (y = forward, x = right); the model
                // (walking / falling / crouch, UE CharacterMovementComponent) is CharacterMovement::Step.
                const glm::vec3 wish = camFwd * cc.MoveInput.y + camRight * cc.MoveInput.x;
                CharacterMovement::Step( cc, *m_World, wish, dt );

                auto& transform       = characters.get<TransformComponent>( entity );
                transform.Translation = m_World->GetCharacterPosition( cc.RuntimeCharacter );

                // NOTE: physics only PRODUCES state (cc.Velocity / cc.OnGround). Mapping that state to a
                // locomotion clip is behaviour and lives in LocomotionSystem (runs after this), not here.
            }

            // Spring arms after the characters moved (and after the scripts pitched them, ScriptSystem runs
            // first): the camera is placed against this frame's pawn and this frame's world.
            SpringArm::UpdateAll( registry, m_World.get(), dt );
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

        /// The scene's trigger overlaps as entity events while Play runs, null in Edit: where C++ gameplay
        /// subscribes (scripts get theirs through OverlapEventsComponent).
        [[nodiscard]] EntityOverlapRouter* GetOverlapRouter() const
        {
            return m_Overlaps.get();
        }

    private:
        // The skeleton the entity's skinned mesh names (UE USkeletalMesh::Skeleton) — what a physics asset
        // must be authored on. Only asked once the entity's Animator exists, so the mesh is resident.
        static Common::ResultStr<Common::Content::AssetGuid> MeshSkeletonOf( const entt::registry& registry,
                                                                             entt::entity          entity )
        {
            using Guid = Common::Content::AssetGuid;
            if ( !registry.has<SkinnedMeshComponent>( entity ) )
                return Common::MakeError<Guid>( "it has no Skinned Mesh: a ragdoll's bodies follow a skinned "
                                                "mesh's bones" );
            const auto& mesh = registry.get<SkinnedMeshComponent>( entity );
            if ( mesh.RuntimeMesh )
                return Common::MakeError<Guid>( "its mesh is an in-editor rig (Convert to Skinned) with no "
                                                "skeleton asset, and a physics asset names its skeleton by GUID" );
            const auto* asset = dynamic_cast<const Assets::SkinnedMeshAsset*>(
                 Runtime::ResourceRegistry::GetMeshService()->GetAsset( mesh.MeshHandle ) );
            if ( asset == nullptr )
                return Common::MakeError<Guid>( "its mesh is not a parsed skinned mesh" );
            return Common::MakeSuccess( asset->GetSkeleton() );
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
        // Same rule: subscribed to m_World's overlaps.
        std::unique_ptr<EntityOverlapRouter> m_Overlaps;
        // Same rule: the landscape's heightfield bodies live in m_World.
        std::unique_ptr<LandscapeCollision> m_Landscape;
        // Same rule: the scene's destructibles are bodies in m_World, advanced by its fixed step.
        std::unique_ptr<Destruction::DestructionWorld> m_Destruction;
        // Same rule, one level down: the destructible entities' objects live in m_Destruction.
        std::unique_ptr<DestructibleLifetime> m_Destructibles;
        // Same rule: the ragdolls' bodies live in m_World.
        std::unique_ptr<RagdollLifetime> m_Ragdolls;
        // Last value handed to the world, so a change in SceneSettings can be noticed without asking Jolt.
        float m_AppliedGravity = 0.0f;
        // Entities whose collider was refused during this Play; cleared with the world.
        std::unordered_set<entt::entity> m_RefusedColliders;
    };
} // namespace Desert::ECS
