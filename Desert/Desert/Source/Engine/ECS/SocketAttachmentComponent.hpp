#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // UE-style SOCKET attachment: makes this entity follow a BONE of another (skinned) entity, not just its
    // root. The hand is a bone inside a Skeleton — it has no entity, so RelationshipComponent can't parent to
    // it. AttachmentSystem (after AnimationECSSystem) computes the bone's world transform from the target's
    // animator pose and writes it (plus the local offset) into this entity's TransformComponent each frame.
    // Used for weapons-in-hand, hats, backpacks, scope attachments, etc.
    struct SocketAttachmentComponent
    {
        // The skinned-mesh entity whose bone we follow. Null = detached, and it starts detached: a socket
        // that has not been pointed at anything must not claim to follow a target that does not exist.
        Common::UUID Target = Common::UUID::Null();
        // UE's AttachSocketName: a SOCKET of the target's skeleton (Skeleton::FindSocket - its bone and its local
        // transform come from the rig) or, when the rig has no socket of that name, a bone (UE
        // USkinnedMeshComponent::GetSocketTransform: socket first, then bone). E.g. "hand_r_grip",
        // "mixamorig:Head".
        std::string SocketName;

        // This attachment's own alignment ON TOP of the socket (or of the bone when the name is a bone) - UE's
        // relative transform of an attached component. The grip shared by every weapon belongs on the socket.
        glm::vec3 OffsetTranslation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 OffsetRotation    = { 0.0f, 0.0f, 0.0f }; // euler radians
        glm::vec3 OffsetScale       = { 1.0f, 1.0f, 1.0f };
    };
} // namespace Desert::ECS
