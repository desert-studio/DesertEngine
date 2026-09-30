#pragma once

#include <Engine/Animation/AnimationClip.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string_view>

namespace Desert::Animation
{
    class Skeleton;

    // The humanoid mannequin's locomotion clips, generated in code over its rig (Humanoid.skeleton): limb joints
    // swung with sine cycles, every track also storing a constant bind-local position key (or the bone would
    // collapse to the parent origin). A GENERATOR, not a runtime source: Geometry::ProceduralCharacterFactory::
    // WriteEngineAssets writes each clip once as engine content (Humanoid_<Name>.anim), and characters play
    // those files like any imported clip.
    class ProceduralCharacterAnimations
    {
    public:
        // The clip named @p name (Idle, Walk, Run, Jump - Geometry::kHumanoidClips) over @p skeleton; the clip's
        // Skeleton reference is left for the caller to state. An unknown name is refused by name.
        [[nodiscard]] static Common::ResultStr<AnimationClip> Build( const Skeleton&  skeleton,
                                                                     std::string_view name );
    };
} // namespace Desert::Animation
