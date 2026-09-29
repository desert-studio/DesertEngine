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
     * apart; the signature can (UE re-initialises the anim instance when the skeleton changes).
     *
     * `builtSignature` is the caller's stamp (AnimationComponent::BuiltSkeletonSignature). Returns true when
     * it built a new Animator, so the caller can drop whatever it had attached to the old one. Both callers —
     * AnimationECSSystem and the editor's PreviewViewport, which has no system — go through here.
     */
    inline bool EnsureAnimatorFor( std::unique_ptr<Animator>& animator, uint64_t& builtSignature,
                                   const Skeleton& skeleton )
    {
        if ( animator && builtSignature == skeleton.GetSignature() )
            return false;
        animator       = std::make_unique<Animator>( skeleton );
        builtSignature = skeleton.GetSignature();
        return true;
    }
} // namespace Desert::Animation
