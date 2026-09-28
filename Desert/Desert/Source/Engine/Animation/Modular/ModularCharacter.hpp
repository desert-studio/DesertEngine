#pragma once

// MODULAR CHARACTER MECHANISMS — the engine half of "changing equipment".
//
// Owner's decision (2026-09-27): putting on, taking off and swapping equipment, slots, items and their
// events are GAMEPLAY, written in scripts. The engine knows none of them. It offers only the mechanisms a
// script composes, as UE does for gameplay code/Blueprint:
//   * follow the leader's skeleton with another skinned mesh, and stop  (UE SetLeaderPoseComponent);
//   * put a rigid mesh on a bone socket, and take it off                (UE AttachToComponent + socket);
//   * hide / show sections of the body mesh under clothing              (UE ShowMaterialSection / HideBoneByName);
//   * create / destroy cloth on a follower through the cloth factory    (UE clothing on the follower mesh).
// "Put on the vest" is therefore a SCRIPT: AttachFollowerMesh + CreateCloth + SetBodySectionVisible(false);
// Desert/Tests/Engine/ClothHairEquipmentApi shows it as test code standing in for the script.
//
// POSE CONTRACT (what ApplyLeaderPose promises; UE's leader-pose rule, restated for this engine):
//   * followers evaluate NO animation of their own: they are skinned with the leader's model-space bone
//     transforms, index for index. A follower whose skeleton does not match the leader's bone index space is
//     refused at AttachFollowerMesh (the same question Animation/ClipSkeletonMatch answers for clips);
//   * it runs once per fixed step AFTER AnimationECSSystem has produced this frame's pose and BEFORE
//     AttachmentSystem (sockets) and rendering, so nothing worn lags the body by a frame;
//   * cloth created on a follower is stepped inside ApplyLeaderPose, with that same pose, at that same dt.
//
// Why Engine/Animation/Modular: every mechanism here is a consequence of sharing ONE animated pose; the
// socket half already lives next to it (ECS SocketAttachmentComponent, driven by AttachmentSystem), and
// AttachToSocket is expected to be implemented on top of that component, not beside it.
//
// NOT HERE: EQP1 — the implementation (ECS side: follower mesh entities, section visibility on the mesh
// renderer, cloth lifetime) and the Lua bindings of these mechanisms. No ECS component is declared, so no
// reflection, serialization or scene format moves with CLO0.

#include <Engine/Physics/Cloth/ClothingSimulation.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Animation
{
    // Ids are scoped to one IModularCharacter and never reused within it, so a stale id is an error, not
    // another object.
    struct FollowerMeshId
    {
        uint32_t Value                                     = 0;
        bool     operator==( const FollowerMeshId& ) const = default;
    };

    struct SocketAttachmentId
    {
        uint32_t Value                                         = 0;
        bool     operator==( const SocketAttachmentId& ) const = default;
    };

    struct ClothInstanceId
    {
        uint32_t Value                                      = 0;
        bool     operator==( const ClothInstanceId& ) const = default;
    };

    // Same fields and meaning as ECS SocketAttachmentComponent (offsets relative to the bone; euler radians).
    struct SocketAttachmentDesc
    {
        Common::AssetHandle Mesh; // a static mesh: helmets, scopes, pouches
        std::string         BoneName;
        glm::vec3           OffsetTranslation = { 0.0f, 0.0f, 0.0f };
        glm::vec3           OffsetRotation    = { 0.0f, 0.0f, 0.0f };
        glm::vec3           OffsetScale       = { 1.0f, 1.0f, 1.0f };
    };

    // The leader's pose for one fixed step.
    struct LeaderPose
    {
        std::span<const glm::mat4> BoneModelTransforms; // leader's bone index space
        glm::mat4                  ComponentToWorld = glm::mat4( 1.0f );
        glm::vec3                  Gravity          = { 0.0f, -980.0f, 0.0f }; // cm/s^2, for cloth
        glm::vec3                  WindVelocity     = { 0.0f, 0.0f, 0.0f };    // cm/s, for cloth
    };

    // One character: a leader skinned mesh (the body) plus whatever the script attached to it.
    class IModularCharacter
    {
    public:
        virtual ~IModularCharacter() = default;

        // Fails when the mesh's skeleton does not match the leader's bone index space (names both skeletons).
        virtual Common::ResultStr<FollowerMeshId> AttachFollowerMesh( Common::AssetHandle skinnedMesh ) = 0;
        // Destroys every cloth instance created on that follower first; fails on an unknown id.
        virtual Common::BoolResultStr DetachFollowerMesh( FollowerMeshId follower ) = 0;

        // Fails on a bone the leader's skeleton does not have.
        virtual Common::ResultStr<SocketAttachmentId> AttachToSocket( const SocketAttachmentDesc& desc ) = 0;
        virtual Common::BoolResultStr                 DetachFromSocket( SocketAttachmentId attachment )  = 0;

        // Sections of the LEADER (body) mesh. Fails on a section index the mesh does not have.
        virtual Common::BoolResultStr SetBodySectionVisible( uint32_t section, bool visible ) = 0;

        // Resolves the cloth asset, finds the backend in the cloth registry by name and creates the simulation
        // through its factory. Every failure (unknown follower, unknown asset, unknown backend, unsupported
        // asset) is returned with its reason; there is no fallback backend.
        virtual Common::ResultStr<ClothInstanceId>
        CreateCloth( FollowerMeshId follower, Common::AssetHandle clothAsset, std::string_view backend ) = 0;
        virtual Common::BoolResultStr DestroyCloth( ClothInstanceId cloth )                              = 0;

        // The pose contract above: skins every follower with the pose, then steps every cloth instance once.
        virtual Common::BoolResultStr ApplyLeaderPose( const LeaderPose& pose, float fixedDeltaSeconds ) = 0;
    };
} // namespace Desert::Animation
