#pragma once

// THE ECS MODULAR CHARACTER (EQP1) - Animation::IModularCharacter over a scene's registry (UE
// USkinnedMeshComponent::SetLeaderPoseComponent + a modular character assembled from components).
//
// THE STATE LIVES IN THE WORLD, NOT IN THIS OBJECT. ModularCharacter is a VIEW over one leader entity: every id it
// hands out and every cloth it created is kept in the leader's runtime ModularCharacterState component, so a
// script (a stateless library call, Libraries/ModularCharacterLibrary) and the physics pre-step (StepAll) reach
// the same character through its entity. Nothing here is reflected or serialized: the script that dressed the
// character dresses it again (LeaderPoseLink.hpp).
//
//   * AttachFollowerMesh: a CHILD entity of the leader with an identity local transform (it moves with the body
//     in the same transform update, never a frame late) carrying the clothing's SkinnedMeshComponent and an
//     Animation::LeaderPoseLink - MeshECSSystem skins it with the leader's pose, MeshRenderer gives it the
//     leader's LOD, VAT hand-off and growth. Refused unless both meshes reference ONE skeleton: the rule is
//     Animation::ClipPlaysOnMesh with no compatible list, because a compatible skeleton is another bone index
//     space and a follower is skinned index for index.
//   * AttachToSocket: a child entity with the static mesh and an ECS::SocketAttachmentComponent on the leader,
//     driven by AttachmentSystem. Refused on a name the leader's rig has neither as a socket nor as a bone
//     (Rules::ResolveAttachPoint, the rule AttachmentSystem follows).
//   * SetBodySectionVisible: the leader's Animation::HiddenSections mask, carried by its draw
//     (DrawSkinnedMeshCommand::HiddenSubmeshes) into MeshRenderer::SkinnedSectionsFor. 64 sections at most, the
//     width of every section mask the renderer has.
//   * Cloth: the backend registry and the cloth assets are the WORLD's - ClothServices in the registry context,
//     emplaced by the cloth backend's owner (CLO1); a world without them refuses by name, no fallback. StepAll
//     (PhysicsECSSystem's pre-step, the physics fixed step) calls ApplyLeaderPose for every leader with cloth.

#include <Engine/Animation/Modular/ModularCharacter.hpp>
#include <PhysicsCore/Cloth/ClothingAsset.hpp>
#include <PhysicsCore/Cloth/ClothingSimulation.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
} // namespace Desert::Core

namespace Desert::ECS
{
    // What the cloth backend's owner (CLO1) puts in the registry context: its factories and its cloth assets.
    struct ClothServices
    {
        const Physics::Cloth::ClothingSimulationFactoryRegistry* Backends = nullptr;
        std::function<Common::ResultStr<const Physics::Cloth::ClothingAsset*>( Common::AssetHandle )> FindAsset;
    };

    // RUNTIME component on the leader: what its IModularCharacter handed out. Never reflected or serialized.
    struct ModularCharacterState
    {
        struct Attached
        {
            uint32_t     Id     = 0;
            entt::entity Entity = entt::null;
        };
        struct Cloth
        {
            uint32_t              Id       = 0;
            uint32_t              Follower = 0;
            std::vector<uint32_t> Bones; // the leader's bone index of each ClothingAsset::UsedBoneNames entry
            std::unique_ptr<Physics::Cloth::IClothingSimulation> Simulation;
        };

        uint32_t              NextId = 0; // ids are never reused within one character
        std::vector<Attached> Followers;
        std::vector<Attached> Sockets;
        std::vector<Cloth>    Cloths;
        // Per-step scratch, kept so a step does not allocate.
        std::vector<glm::mat4> ScratchPose;
        std::vector<glm::mat4> ScratchClothBones;
    };

    class ModularCharacter final : public Animation::IModularCharacter
    {
    public:
        ModularCharacter( Core::Scene& scene, entt::entity leader ) noexcept;

        Common::ResultStr<Animation::FollowerMeshId>
                              AttachFollowerMesh( Common::AssetHandle skinnedMesh ) override;
        Common::BoolResultStr DetachFollowerMesh( Animation::FollowerMeshId follower ) override;

        Common::ResultStr<Animation::SocketAttachmentId>
                              AttachToSocket( const Animation::SocketAttachmentDesc& desc ) override;
        Common::BoolResultStr DetachFromSocket( Animation::SocketAttachmentId attachment ) override;

        Common::BoolResultStr SetBodySectionVisible( uint32_t section, bool visible ) override;

        Common::ResultStr<Animation::ClothInstanceId> CreateCloth( Animation::FollowerMeshId follower,
                                                                   Common::AssetHandle       clothAsset,
                                                                   std::string_view          backend ) override;
        Common::BoolResultStr                         DestroyCloth( Animation::ClothInstanceId cloth ) override;

        // Followers need nothing here: MeshECSSystem skins them with this same pose through their LeaderPoseLink.
        // Steps every cloth of the character once, with the pose gathered into each asset's bone order.
        Common::BoolResultStr ApplyLeaderPose( const Animation::LeaderPose& pose,
                                               float                        fixedDeltaSeconds ) override;

        // ONE FIXED STEP OF EVERY CHARACTER WITH CLOTH (PhysicsECSSystem's pre-step): the leader's animated pose,
        // its world transform, the scene's gravity (@p gravityCmPerS2, downward) and ECS::WindAt at the leader.
        static void StepAll( entt::registry& registry, float fixedDeltaSeconds, float gravityCmPerS2 ) noexcept;

    private:
        // The pose-only view StepAll uses: no scene, so nothing can be attached or detached through it.
        ModularCharacter( entt::registry& registry, entt::entity leader ) noexcept;

        // The leader's state, emplaced on first use; refused for a dead leader, one with no skinned mesh asset,
        // or one that is itself a follower (one level only, as UE).
        Common::ResultStr<ModularCharacterState*> State() noexcept;
        Common::ResultStr<Core::Scene*>           SceneOf( const char* call ) const noexcept;

        entt::registry& m_Registry;
        Core::Scene*    m_Scene = nullptr;
        entt::entity    m_Leader;
    };
} // namespace Desert::ECS
