#pragma once

// A RAGDOLLED CHARACTER'S BODIES IN THE PHYSICS WORLD, AROUND THE STEP.
//
// DriveBeforeStep() (UE: the physics asset's bodies registered with the physics scene, kinematic bodies
// targeted before the tick): every Transform + RagdollComponent + AnimationComponent entity whose Animator
// exists gets its ragdoll — BuildRagdollDesc of the physics asset on the Animator's skeleton, created at the
// entity's world pose and placed in the current animated pose. Then the authored Mode is applied (a change to
// Kinematic snaps the bodies to the animation; a change to Simulated keeps their pose and motion), and a
// kinematic ragdoll is given the animated pose as its target, which the step reaches exactly.
//
// WriteBackAfterStep() (UE BlendInPhysics at weight 1): a simulated ragdoll's bodies become the Animator's
// bones through Animator::ApplyPhysicsPose — after the animation pipeline and the step, before skinning.
//
// The physics asset and the mesh's skeleton come from the caller's lookups, so PhysicsECSSystem reads them from
// PhysicsAssetService / the mesh and a suite hands them over in memory. The release rides EnTT's
// on_destroy<RagdollComponent> (PhysicsBodyLifetime.hpp's shape). Refused by name, once per entity per Play: no
// Animation component, an empty Physics Asset, an asset that cannot be read or is authored on another skeleton
// (the lookup's answer), an asset that does not fit the rig, and a world scale other than one.

#include <Engine/Assets/Common.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Physics/RagdollDesc.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::ECS
{
    class RagdollLifetime final
    {
    public:
        /// The physics asset for a mesh on @p meshSkeleton: success with null = still loading (asked again next
        /// frame); an error = never (unreadable, or authored on another skeleton).
        using PhysicsAssetLookup =
             std::function<Common::ResultStr<std::shared_ptr<const Physics::PhysicsAssetData>>(
                  const Assets::AssetHandle& asset, const Common::Content::AssetGuid& meshSkeleton,
                  std::string_view meshSkeletonName )>;
        /// The skeleton GUID the entity's skinned mesh names; an error when it has none.
        using MeshSkeletonLookup = std::function<Common::ResultStr<Common::Content::AssetGuid>(
             const entt::registry& registry, entt::entity entity )>;

        explicit RagdollLifetime( Physics::PhysicsWorld& world ) : m_World( &world )
        {
        }
        ~RagdollLifetime();

        RagdollLifetime( const RagdollLifetime& )            = delete;
        RagdollLifetime& operator=( const RagdollLifetime& ) = delete;

        /// Listens on @p registry from now on; idempotent, moves to a new registry (callable every frame).
        void Attach( entt::registry& registry );
        /// Stops listening. Must run before the world it releases into goes.
        void Detach();

        /// Before PhysicsWorld::Step: creates missing ragdolls, applies Mode, targets kinematic ones.
        void DriveBeforeStep( entt::registry& registry, const PhysicsAssetLookup& assets,
                              const MeshSkeletonLookup& skeletons );
        /// After PhysicsWorld::Step: simulated ragdolls' bodies into their Animator's pose.
        void WriteBackAfterStep( entt::registry& registry );

    private:
        struct Live
        {
            Physics::RagdollDesc Desc;
            uint64_t             SkeletonSignature = 0; // the rig Desc was built on; a reimport rebuilds
        };

        void OnRagdollDestroyed( entt::registry& registry, entt::entity entity );
        void Refuse( entt::entity entity, const std::string& reason );
        void Release( entt::entity entity, uint32_t& runtimeRagdoll );

        Physics::PhysicsWorld*                     m_World    = nullptr;
        entt::registry*                            m_Registry = nullptr;
        std::unordered_map<entt::entity, Live>     m_Live;
        std::unordered_set<entt::entity>           m_Refused;
        std::vector<Physics::RagdollPartTransform> m_Parts; // WriteBackAfterStep's read, reused
    };
} // namespace Desert::ECS
