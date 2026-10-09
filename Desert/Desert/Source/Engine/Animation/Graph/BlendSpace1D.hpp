#pragma once

/**
 * BLEND SPACE 1D — UE's UBlendSpace1D played by a Blend Space Player (FAnimNode_BlendSpacePlayer).
 *
 * A row of SAMPLES on one axis, each a clip at an axis value. The axis is the node's `X` parameter pin, bound to
 * a declared graph parameter (Speed in cm/s for locomotion): that binding IS the axis parameter, so the name is
 * stated once, in ParameterInputs, as every other exposed pin is. For an axis value the two samples around it are
 * weighted linearly (a value outside the row is clamped to its end samples, as UE clamps to the axis range);
 * every other sample weighs 0.
 *
 * SMOOTHING is UE's TargetWeightInterpolationSpeedPerSec: each sample's weight moves toward its target by at most
 * `WeightSpeed` per second and the weights are renormalised to sum 1; 0 means the weights follow the axis at once.
 *
 * SYNC BY NORMALIZED TIME (UE's sync of a blend space's samples, the marker-less sync group): every sample plays
 * at ONE shared phase in [0, 1) of its own length, so a 1.0 s walk and a 0.7 s run cycle put the same foot down
 * at the same phase. The phase advances by dt over the weight-averaged length of the weighted samples, so the
 * blend's cycle stretches smoothly from one sample's length to the next.
 *
 * The functions are defined in PoseGraph.cpp (compiled wherever the pose graph is).
 */

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Animation::Graph
{
    /// The axis pin of a Blend Space 1D node (UE: the Blend Space Player's exposed X). Unbound, the axis reads 0.
    inline constexpr std::string_view kBlendSpaceAxisPin = "X";

    /// One sample of a blend space: a clip (by name, resolved against the AnimationLibrary as a State's is) at an
    /// axis value (UE FBlendSample::SampleValue, in the axis parameter's unit — cm/s for Speed).
    struct BlendSample
    {
        std::string Clip;
        float       Value = 0.0F;
    };

    /// The payload of a BlendSpace1D node.
    struct BlendSpace1DNode
    {
        /// Strictly ascending by Value (BlendSpace1DError refuses otherwise): the row the axis walks.
        std::vector<BlendSample> Samples;
        /// UE TargetWeightInterpolationSpeedPerSec: weight per second a sample's weight may move; 0 = no
        /// smoothing.
        float WeightSpeed = 0.0F;
        /// Whether the shared phase wraps (UE bLoop); otherwise it holds at the end.
        bool Loop = true;
    };

    /// Empty when `node` can be played; otherwise what is wrong: no sample, a sample naming no clip, values not
    /// strictly ascending, a negative or non-finite WeightSpeed.
    [[nodiscard]] std::string BlendSpace1DError( const BlendSpace1DNode& node );

    /// The weights the samples of `node` take at axis value `x` (one per sample into `out`, sized by the caller):
    /// the two samples around `x` linearly, summing to 1; clamped to the end samples outside the row.
    void BlendSpace1DTargetWeights( const BlendSpace1DNode& node, float x, std::span<float> out );

    /// UE FBlendSpace::InterpolateWeightOfSampleData: moves each of `weights` toward `target` by at most
    /// `speedPerSecond` x `seconds` and renormalises them to sum 1. `speedPerSecond` 0 copies `target`.
    void InterpolateBlendWeights( std::span<float> weights, std::span<const float> target, float speedPerSecond,
                                  float seconds );

    /// The shared normalized phase after `seconds`: advanced by `seconds` over the weight-averaged length
    /// (seconds) of the samples with weight and a length; wrapped into [0, 1) when `loop`, else held at 1. A blend
    /// with no weighted length keeps its phase.
    [[nodiscard]] float AdvanceSyncedPhase( float phase, std::span<const float> weights,
                                            std::span<const float> lengthSeconds, float seconds, bool loop );
} // namespace Desert::Animation::Graph
