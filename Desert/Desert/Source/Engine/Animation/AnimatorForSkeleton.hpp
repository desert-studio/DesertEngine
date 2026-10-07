#pragma once

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Animation
{
    /**
     * @brief THE ONE RULE FOR WHEN AN ANIMATOR IS (RE)BUILT: there is none, or the rig it was built on changed.
     *
     * A reimport re-reads a skeleton AT THE SAME ADDRESS (SkeletonAsset::LoadFromFile — UE's Reimport rewrites
     * the USkeleton in its own UObject), so the Animator's `const Skeleton&` stays valid while its bind pose,
     * pose buffers and clip bindings were sized from the OLD bone list. The address cannot tell the two rigs
     * apart; the CONTENT signature can (UE re-initialises the anim instance when the skeleton changes). Not the
     * name signature: a reimport at another Uniform Scale keeps every bone name and moves every bind, and an
     * Animator kept across it posed the new mesh with the old binds (THM1l-b19, Fox.glb at Scale 10: a torn star).
     *
     * `builtSignature` is the caller's stamp (AnimationComponent::BuiltSkeletonSignature). Returns true when
     * it built a new Animator, so the caller can drop whatever it had attached to the old one. Both callers —
     * AnimationECSSystem and the editor's PreviewViewport, which has no system — go through here.
     */
    inline bool EnsureAnimatorFor( std::unique_ptr<Animator>& animator, uint64_t& builtSignature,
                                   const Skeleton& skeleton )
    {
        if ( animator && builtSignature == skeleton.GetContentSignature() )
            return false;
        animator       = std::make_unique<Animator>( skeleton );
        builtSignature = skeleton.GetContentSignature();
        return true;
    }
} // namespace Desert::Animation
