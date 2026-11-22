#pragma once

// THE DRAW SIDE OF THE LEADER-POSE CONTRACT (ModularCharacter.hpp; UE USkinnedMeshComponent::LeaderPoseComponent,
// whose follower takes the leader's transform and LOD instead of deciding its own).
//
// A follower mesh (AttachFollowerMesh: a separate entity skinned with the leader's pose) names its leader's entity
// in its draw. The renderer then derives what the leader's draw decides for the frame from the LEADER'S record -
// one source of truth - instead of deciding it again from the follower's own bounds: the far figure's growth
// (MeshRenderer::SelectSkinnedLods, VertexAnimationCrowd::kMinScreenSize) is the leader's scale about the leader's
// feet, so clothing grown with a far body stays on it. Filled by the follower mesh entities (EQP1); empty = the
// mesh leads itself. One level only, as UE: a leader is never itself a follower.

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
