#pragma once

// THE SKELETON-DEPENDENT HALF OF THE RAGDOLL DESCRIPTION: a physics asset on a skeleton's bind pose -> RagdollDesc
// (UE: USkeletalMeshComponent::InitArticulated reading UPhysicsAsset against the ref pose). RagdollDesc and its
// parts are PhysicsCore's (RagdollDesc.hpp, no skeleton); validating the asset against the bones and building the
// description need Animation::Skeleton, so they are Engine's. Every unit conversion of the ragdoll path
// (RagdollDesc.hpp's header comment) is made here, once, in BuildRagdollDesc.

#include <Engine/Physics/RagdollDesc.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Physics
{
    /**
     * @brief Everything wrong with @p asset against @p skeleton, one message per fault, each naming the bone or
     * the constraint it is about. Empty means the asset builds a ragdoll.
     *
     * Checked: a null skeleton GUID; no bodies; a body on a bone the skeleton does not have; two bodies on one
     * bone; a non-positive radius, length or box extent, mass below zero, or a non-positive density where the
     * density is what gives the mass; a constraint naming a bone the skeleton does not have or that carries no
     * body; a constraint whose bodies are not parent and child — the parent body of a body is the body on the
     * nearest ancestor bone that carries one (UE's physics asset tool links a body to it the same way); two
     * constraints on one child; a swing or twist limit outside [0, 180] degrees.
     */
    [[nodiscard]] std::vector<std::string> ValidatePhysicsAsset( const PhysicsAssetData&    asset,
                                                                 const Animation::Skeleton& skeleton );

    /// The ragdoll description of @p asset on @p skeleton's bind pose; refused with every ValidatePhysicsAsset
    /// message when there is any.
    [[nodiscard]] Common::ResultStr<RagdollDesc> BuildRagdollDesc( const PhysicsAssetData&    asset,
                                                                   const Animation::Skeleton& skeleton );
} // namespace Desert::Physics
