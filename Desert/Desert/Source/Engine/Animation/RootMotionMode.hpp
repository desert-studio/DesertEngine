#pragma once

// The per-component root motion choice alone: what a prefab, a component or a serializer names without the
// pose maths of RootMotion.hpp (which pulls Pose.hpp).

namespace Desert::Animation
{
    /**
     * @brief UE ERootMotionMode, per animated component. Serialized by NAME (reflect-cpp); append only.
     *
     * UE's fourth mode, RootMotionFromMontagesOnly, is NOT here: the engine has no montages, and a mode that
     * can only ever behave as IgnoreRootMotion would be a choice that changes nothing. It arrives with them.
     */
    enum class RootMotionMode
    {
        NoExtraction             = 0, ///< the root plays as authored; nothing is extracted (UE's default)
        IgnoreRootMotion         = 1, ///< the root is locked in the pose and its movement is discarded
        RootMotionFromEverything = 2, ///< locked and extracted from every clip that plays, weighted by blend
    };

    [[nodiscard]] constexpr bool LocksRoot( const RootMotionMode mode )
    {
        return mode != RootMotionMode::NoExtraction;
    }

} // namespace Desert::Animation
