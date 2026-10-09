#pragma once

// BETWEEN A SKELETON'S POSE AND A RAGDOLL'S BODIES — both directions, device-free and PhysicsSystem-free (UE:
// USkeletalMeshComponent's kinematic bodies following the component-space bone transforms, and BlendInPhysics
// writing the simulated bodies back into the bone transforms before skinning).
//
// A part's body frame IS its bone's component-space frame with the scale taken out (RagdollDesc.hpp: Position /
// Rotation = the bone's bind transform, rotation de-scaled). So:
//
//   pose -> bodies:  body_world = entityWorld * component(bone), split into its origin and its unit rotation;
//   bodies -> pose:  component(bone) = inverse(entityWorld) * T(body origin) * R(body rotation) * S(bone scale),
//                    where the bone scale is the bone's component-space scale in the animated pose — an FBX rig
//                    whose root carries 0.01 keeps 0.01 on every bone, and the bodies stay in world cm whatever
//                    that scale is. The overrides go through Animation::ApplyBoneOverrides, which turns them into
//                    locals through the parent chain; a bone without a body keeps its animated local.

#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Physics/RagdollDesc.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace Desert::Physics
{
    /// Every part's body in the world for @p pose of @p skeleton, the entity at @p entityWorld. Refuses an
    /// entity whose world scale is not one (the shapes are authored in cm of the rig, not of a scaled entity), a
    /// pose of another rig's length, and a part naming a bone the skeleton does not have.
    [[nodiscard]] Common::ResultStr<std::vector<RagdollPartTransform>>
    RagdollPartsFromPose( const RagdollDesc& desc, const Animation::Skeleton& skeleton,
                          const Animation::LocalPose& pose, const glm::mat4& entityWorld );

    /// The component-space overrides that put every part's bone where its body is, parents first (the part
    /// order), the scale of each bone taken from @p animated. Refuses a part count other than the desc's, a pose
    /// of another rig's length, and a transform that does not decompose (naming the bone).
    [[nodiscard]] Common::ResultStr<std::vector<Animation::BoneOverride>>
    RagdollBoneOverrides( const RagdollDesc& desc, const Animation::Skeleton& skeleton,
                          const Animation::LocalPose& animated, const glm::mat4& entityWorld,
                          std::span<const RagdollPartTransform> parts );
} // namespace Desert::Physics
