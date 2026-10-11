#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief A named attach point on a bone, OWNED BY THE RIG (UE USkeleton::Sockets, USkeletalMeshSocket).
     *
     * "The right hand's grip" is a fact about the skeleton, not about every weapon that hangs off it: authored
     * once here, every SocketAttachmentComponent that names it follows the same point, and moving the grip moves
     * all of them. The component's own offset composes ON TOP (UE: the attached component's relative transform
     * is relative to the socket). Written straight to the `.skeleton` (SKEL 4) by reflection, so every field is
     * initialised; Rotation is a quaternion like every rotation this engine stores.
     */
    struct SkeletonSocket
    {
        std::string Name;
        std::string Bone; ///< the parent bone, by name (UE USkeletalMeshSocket::BoneName)
        glm::vec3   Translation = glm::vec3( 0.0F );
        glm::quat   Rotation    = glm::quat( 1.0F, 0.0F, 0.0F, 0.0F );
        glm::vec3   Scale       = glm::vec3( 1.0F );

        /// Bone space -> socket space: T * R * S, the order SocketAttachmentComponent's offset uses.
        [[nodiscard]] glm::mat4 LocalTransform() const
        {
            return glm::translate( glm::mat4( 1.0F ), Translation ) * glm::mat4_cast( Rotation ) *
                   glm::scale( glm::mat4( 1.0F ), Scale );
        }
    };

    /// One bone's weight in a BoneMask (UE FBlendProfileBoneEntry). IncludeDescendants = the weight is a BRANCH:
    /// every bone below takes it until another entry overrides it; false = this bone alone.
    struct BoneMaskEntry
    {
        std::string Bone;
        float       Weight             = 1.0F;
        bool        IncludeDescendants = true;
    };

    /**
     * @brief A named per-bone weight table OWNED BY THE RIG (UE USkeleton::BlendProfiles in BlendMask mode).
     *
     * "Upper body" is authored once on the skeleton; a Layered Blend Per Bone layer names it instead of
     * restating the same branch filters in every graph (LayerSetup::BoneMask). Resolution is
     * ResolveBoneMaskWeights (Skeleton.hpp).
     */
    struct BoneMask
    {
        std::string                Name;
        std::vector<BoneMaskEntry> Entries;
    };
} // namespace Desert::Animation
