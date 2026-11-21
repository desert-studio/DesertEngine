#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace Desert::Animation
{
    /**
     * @brief WHAT THE RENDERER SAW OF ONE SKINNED INSTANCE, handed back to its animation (UE: the render thread's
     *        FSkeletalMeshObject::MaxDistanceFactor / bRecentlyRendered, read by the game thread's
     *        USkinnedMeshComponent::UpdateLODStatus and FAnimUpdateRateManager on the NEXT tick).
     *
     * One per animated skinned entity, co-owned by its AnimationComponent and by the frame's skinned record
     * (DrawSkinnedMeshCommand -> SkinnedMeshRenderData::LodFeedback). The renderer writes the level it picked, the
     * instance's screen size and whether the main view saw it (MeshRenderer::SelectSkinnedLods); the animation
     * system reads them on its next tick and writes back the coarsest level its evaluated bones cover, which the
     * renderer never draws finer than (a level drawn with bones its pose did not evaluate would show stale limbs).
     * The fields are atomics: the two sides run on different threads and each value is independent.
     */
    struct SkinnedLodFeedback
    {
        // Renderer -> animation.
        std::atomic<uint32_t> DesiredLod{ 0u };
        // UE's significance / MaxDistanceFactor: the bounding sphere's diameter as a fraction of the screen (the
        // measure the LOD is picked by, FOV-aware). A large value when the view does not scale by distance.
        std::atomic<float> ScreenSize{ 1.0f };
        std::atomic<bool>  Rendered{ true }; // inside the main view's frustum
        std::atomic<bool>  Seen{ false };    // the renderer wrote at least once
        // Animation -> renderer: the finest level whose required bones the published pose evaluated.
        std::atomic<uint32_t> EvaluatedLod{ 0u };
    };

    // UE FAnimUpdateRateParameters defaults, by screen size instead of UE's two thresholds alone: the rate (tick
    // every N frames) for an instance at least this large on screen. UE BaseVisibleDistanceFactorThesholds
    // {0.4, 0.2} -> rates 1, 2, 3; extended below 0.1 so a crowd at hundreds of metres costs a fraction of one.
    struct UpdateRateBand
    {
        float    MinScreenSize;
        uint32_t Rate;
    };
    inline constexpr std::array<UpdateRateBand, 5> kUpdateRateBands{
         { { 0.4f, 1u }, { 0.2f, 2u }, { 0.1f, 3u }, { 0.03f, 4u }, { 0.0f, 8u } } };
    // UE BaseNonRenderedUpdateRate: an instance outside the view still ticks (root motion, notifies, montages keep
    // their clocks - UE's default VisibilityBasedAnimTickOption AlwaysTickPose), every this many frames.
    inline constexpr uint32_t kNonRenderedUpdateRate = 8u;
    // UE MaxEvalRateForInterpolation: skipped frames are interpolated up to this rate; slower ones hold the pose.
    inline constexpr uint32_t kMaxEvalRateForInterpolation = 4u;

    [[nodiscard]] inline uint32_t UpdateRateForScreenSize( const float screenSize, const bool rendered ) noexcept
    {
        if ( !rendered )
            return kNonRenderedUpdateRate;
        for ( const UpdateRateBand& band : kUpdateRateBands )
            if ( screenSize >= band.MinScreenSize )
                return band.Rate;
        return kUpdateRateBands.back().Rate;
    }

    /**
     * @brief ONE INSTANCE'S UPDATE RATE OPTIMIZATION STATE (UE FAnimUpdateRateParameters): ticks the Animator
     * every `Rate` frames with the time it skipped, and says on which frames the skipped pose is interpolated.
     */
    struct AnimUpdateRate
    {
        uint32_t Rate            = 1u;
        uint32_t FramesUntilEval = 0u; // 0: this frame evaluates
        float    PendingSeconds  = 0.0f;
        bool     Interpolate     = false;
        bool     Snap            = false; // the shown pose jumps to this frame's evaluation (the bones grew)

        /// Sets this frame's rate; @p forceEval makes this frame evaluate (the required bones grew).
        void SetRate( const uint32_t rate, const bool forceEval ) noexcept
        {
            Rate            = std::max( 1u, rate );
            Interpolate     = Rate > 1u && Rate <= kMaxEvalRateForInterpolation;
            FramesUntilEval = forceEval ? 0u : std::min( FramesUntilEval, Rate - 1u );
            Snap            = Snap || forceEval;
        }

        /// Accumulates @p seconds; returns the seconds to step the Animator by when this frame evaluates (the
        /// whole skipped interval), or a negative value when it skips.
        [[nodiscard]] float Advance( const float seconds ) noexcept
        {
            PendingSeconds += seconds;
            if ( FramesUntilEval > 0u )
            {
                --FramesUntilEval;
                return -1.0f;
            }
            const float step = PendingSeconds;
            PendingSeconds   = 0.0f;
            FramesUntilEval  = Rate - 1u;
            return step;
        }

        /// UE InterpolateSkippedFrames' alpha: the share of the way from the shown pose to the evaluated one
        /// covered this frame, reaching it on the frame before the next evaluation.
        [[nodiscard]] float InterpolationAlpha() noexcept
        {
            if ( Snap )
            {
                Snap = false;
                return 1.0f;
            }
            return 1.0f / static_cast<float>( FramesUntilEval + 1u );
        }
    };
} // namespace Desert::Animation
