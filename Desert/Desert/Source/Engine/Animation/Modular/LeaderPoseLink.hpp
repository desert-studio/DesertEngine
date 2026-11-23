#pragma once

// THE DRAW SIDE OF THE LEADER-POSE CONTRACT (ModularCharacter.hpp; UE USkinnedMeshComponent::LeaderPoseComponent,
// whose follower takes the leader's transform and LOD instead of deciding its own).
//
// A follower mesh (AttachFollowerMesh: a separate entity skinned with the leader's pose) names its leader's entity
// in its draw. The renderer then derives what the leader's draw decides for the frame from the LEADER'S record -
// one source of truth - instead of deciding it again from the follower's own bounds (MeshRenderer::
// SelectSkinnedLods): the leader's skeletal level clamped to the follower's own levels, the leader's hand-off to
// the baked crowd (VertexAnimationCrowd, same clip, same time) and the far figure's growth (kMinScreenSize: the
// leader's scale about the leader's feet), so clothing on a far body stays on it, at its size and level.
//
// THE PRODUCER: the follower mesh entity carries this struct as a RUNTIME ECS component (emplaced by the
// IModularCharacter implementation at AttachFollowerMesh, removed at DetachFollowerMesh; never reflected or
// serialized - the script that put the clothing on re-attaches it). MeshECSSystem reads it per skinned entity: the
// follower is skinned with the leader's animated pose and its draw carries the link. Absent / empty = the mesh
// leads itself. One level only, as UE: a leader is never itself a follower.

#include <cstdint>
#include <optional>

namespace Desert::Animation
{
    struct LeaderPoseLink
    {
        // The leader's entity (entt id incl. version), the same key its own draw carries (SkinnedMeshRenderData::
        // Entity); empty for a mesh that is not a follower.
        std::optional<uint32_t> LeaderEntity;

        [[nodiscard]] bool Follows() const noexcept
        {
            return LeaderEntity.has_value();
        }
        bool operator==( const LeaderPoseLink& ) const = default;
    };
} // namespace Desert::Animation
