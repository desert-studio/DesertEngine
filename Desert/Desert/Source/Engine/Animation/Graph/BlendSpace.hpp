#pragma once

/**
 * BLEND SPACE — UE's UBlendSpace (one or two axes) played by ONE Blend Space Player (FAnimNode_BlendSpacePlayer).
 *
 * One setup serves the 1D and the 2D space, the payload of a BlendSpace1D / BlendSpace2D pose node and of a state
 * that plays a blend space (StateBlendSpace): the axis count is the kind's contract (1 or 2), so the pins stay per
 * kind as in UE (X / X, Y) and every fix of the player is made once.
 *
 * AXES are UE's FBlendParameter (Min, Max, Wrap) with FInterpolationParameter (input smoothing): the input is
 * clamped (or wrapped) into the axis range, then filtered per axis (FilterBlendSpaceAxis) before it reaches the
 * weights.
 *
 * WEIGHTS: 1D — the two samples bracketing the axis value, linearly. 2D — UE 5's default triangulation
 * (bInterpolateUsingGrid off): the samples are Delaunay-triangulated ONCE in the unit box the AXIS ranges map to
 * (UE SetGridBox), a point inside a triangle weights its three samples barycentrically, a point outside the hull
 * is projected onto the nearest hull edge. The triangulation is DERIVED (TriangulateBlendSpace), never stored.
 *
 * SMOOTHING of the weights is UE's TargetWeightInterpolationSpeedPerSec (+ ease-in-out); SYNC is UE's marker-less
 * sync of a blend space's samples: every sample plays at ONE shared normalized phase of its own length
 * (Duration / RateScale), the phase advancing over the weight-averaged length (AdvanceSyncedPhase).
 *
 * Defined in BlendSpace.cpp (compiled wherever the pose graph is: the loader refuses an unplayable space).
 */

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Animation::Graph
{
    // Pin names of a blend-space node (UE FAnimNode_BlendSpacePlayer X / Y / PlayRate). Unbound, an axis reads
    // 0 and PlayRate 1.
    inline constexpr const char* kBlendSpaceXPin        = "X";
    inline constexpr const char* kBlendSpaceYPin        = "Y";
    inline constexpr const char* kBlendSpacePlayRatePin = "PlayRate";

    // A sample weighted at or below this is not sampled (its pose, curves and root motion would not show).
    inline constexpr float kBlendSampleWeight = 1e-4f;

    // UE EFilterInterpolationType: the shape of an axis's input smoothing.
    enum class BlendAxisSmoothing : uint8_t
    {
        Average          = 0,
        Linear           = 1,
        Cubic            = 2,
        EaseInOut        = 3,
        ExponentialDecay = 4,
        SpringDamper     = 5,
    };

    // One axis: UE FBlendParameter (range, wrap) + FInterpolationParameter (input smoothing).
    struct BlendSpaceAxis
    {
        float              Min           = 0.0f;
        float              Max           = 100.0f; // Max > Min, refused otherwise
        bool               Wrap          = false;  // input wraps in [Min, Max) and is smoothed the short way
        float              SmoothingTime = 0.0f;   // seconds; 0 = the raw input
        BlendAxisSmoothing Smoothing     = BlendAxisSmoothing::Average;
        float              DampingRatio  = 1.0f; // SpringDamper only
        float              MaxSpeed      = 0.0f; // axis units per second; 0 = unlimited

        bool operator==( const BlendSpaceAxis& ) const = default;
    };

    // UE FBlendSample. Y is 0 and ignored in a 1D space.
    struct BlendSample
    {
        std::string Clip; // resolved by name against the AnimationLibrary, as a State's clip is
        float       X         = 0.0f;
        float       Y         = 0.0f;
        float       RateScale = 1.0f; // > 0: matches the clip's native ground speed to its axis position

        bool operator==( const BlendSample& ) const = default;
    };

    // The whole blend space: UE UBlendSpace minus the asset wrapper. Axes.size() is 1 or 2.
    struct BlendSpaceSetup
    {
        std::vector<BlendSpaceAxis> Axes;
        std::vector<BlendSample>    Samples;
        float   WeightSpeed     = 0.0f;  // per second; 0 = weights jump (UE TargetWeightInterpolationSpeedPerSec)
        bool    WeightEaseInOut = false; // UE bTargetWeightInterpolationEaseInOut
        bool    Loop            = true;
        int32_t ScaleAxis       = -1; // UE AxisToScaleAnimation: -1 none, else axis index

        bool operator==( const BlendSpaceSetup& ) const = default;
    };

    // Derived, never serialized. Points are normalized by the axis Min/Max (UE SetGridBox).
    // 1D: no triangles; Hull holds the row's segments between neighbouring samples in ascending X.
    struct BlendSpaceTriangulation
    {
        std::vector<std::array<float, 2>>    Points;    // normalized, one per sample (parallel to Samples)
        std::vector<std::array<uint32_t, 3>> Triangles; // sample indices, CCW
        std::vector<std::array<uint32_t, 2>> Hull;      // boundary edges, CCW (1D: the row's segments)
    };

    // Runtime filter state of one axis (UE FBlendFilter / FFIRFilterTimeBased).
    struct BlendSpaceAxisFilter
    {
        struct Entry
        {
            float Value = 0.0f;
            float Age   = 0.0f;
        };
        std::vector<Entry> History; // FIR window, oldest first (unwrapped on a wrapping axis)
        float              Value       = 0.0f;
        float              Rate        = 0.0f;  // SpringDamper velocity
        bool               Initialised = false; // first input is taken as is (UE: no blend-in from 0)
    };

    // Empty when the setup is usable for a space of `axisCount` axes; the message names the first defect
    // (axis count, Max <= Min, no sample, empty clip, RateScale <= 0, duplicate position, a sample outside the
    // axis range, all samples collinear in 2D).
    [[nodiscard]] std::string BlendSpaceError( const BlendSpaceSetup& setup, size_t axisCount );

    // 2D: Bowyer-Watson Delaunay in the normalized box; deterministic for co-circular points (insertion order,
    // strict in-circle test). 1D: Points along X, no triangles, Hull = the ascending row's segments. Call only on
    // a setup BlendSpaceError accepted.
    [[nodiscard]] BlendSpaceTriangulation TriangulateBlendSpace( const BlendSpaceSetup& setup );

    // Clamps or wraps each axis value into [Min, Max], then: 1D — the two bracketing samples, linear;
    // 2D — barycentric in the containing triangle, or the nearest hull edge's projection outside the hull.
    // `out` is resized to Samples.size(); weights sum to 1.
    void BlendSpaceTargetWeights( const BlendSpaceSetup& setup, const BlendSpaceTriangulation& triangulation,
                                  std::span<const float> axisValues, std::vector<float>& out );

    // `raw` clamped into the axis range, or wrapped into [Min, Max) on a wrapping axis.
    [[nodiscard]] float ClampBlendSpaceAxis( const BlendSpaceAxis& axis, float raw );

    // One tick of the axis filter; returns the filtered value. SmoothingTime 0 returns `raw` (clamped/wrapped).
    float FilterBlendSpaceAxis( const BlendSpaceAxis& axis, BlendSpaceAxisFilter& filter, float raw, float dt );

    // Moves `weights` toward `targets` at `speed` per second (ease-in-out if asked), then renormalizes.
    // `speed` 0 copies `targets`.
    void InterpolateBlendWeights( std::vector<float>& weights, const std::vector<float>& targets, float speed,
                                  bool easeInOut, float dt );

    // Shared normalized phase over the weight-averaged length (sample length = Duration / RateScale);
    // `rate` = PlayRate pin x axis scale. Loop wraps into [0, 1), otherwise clamps at 1. A blend with no weighted
    // length keeps its phase.
    [[nodiscard]] float AdvanceSyncedPhase( float phase, std::span<const float> weights,
                                            std::span<const float> durations, std::span<const BlendSample> samples,
                                            float rate, bool loop, float dt );
} // namespace Desert::Animation::Graph
