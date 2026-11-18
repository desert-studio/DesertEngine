#pragma once

/**
 * BLEND SPACE 2D — UE's UBlendSpace (two axes) played by a Blend Space Player (FAnimNode_BlendSpacePlayer).
 *
 * SAMPLES on a plane, each a clip at a point (X, Y): Speed in cm/s against Direction in degrees for locomotion.
 * The node's axes are its `X` and `Y` parameter pins (kBlendSpaceAxisPin, kBlendSpaceYAxisPin), bound to declared
 * Float graph parameters as every exposed pin is; a state names its two axis parameters (StateBlendSpace2D).
 *
 * TRIANGULATION, as UE 5 does it by default (FBlendSpaceData, bInterpolateUsingGrid off): the samples are
 * Delaunay-triangulated ONCE, in normalized coordinates (each axis over the samples' own range mapped to [0, 1],
 * so cm/s against degrees does not decide the triangles' shape). A point inside a triangle weights its three
 * samples by barycentric coordinates; a point outside the hull is projected onto the nearest hull edge and weights
 * that edge's two samples (UE's extrapolation onto the closest boundary). Every other sample weighs 0; the weights
 * sum to 1. The triangulation is DERIVED from the samples, never stored: the samples are the one source of truth.
 *
 * SMOOTHING and SYNC are the 1D blend space's own (BlendSpace1D.hpp): InterpolateBlendWeights moves the weights at
 * `WeightSpeed`, AdvanceSyncedPhase plays every sample at one shared normalized phase.
 *
 * The functions are defined in PoseGraph.cpp (compiled wherever the pose graph is), beside the 1D ones.
 */

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Animation::Graph
{
    /// The second axis pin of a Blend Space 2D node (UE: the Blend Space Player's exposed Y). The first is the 1D
    /// node's `kBlendSpaceAxisPin` ("X"), one spelling for both. Unbound, an axis reads 0.
    inline constexpr std::string_view kBlendSpaceYAxisPin = "Y";

    /// One sample of a 2D blend space: a clip (by name, resolved against the AnimationLibrary as a State's is) at
    /// a point of the plane (UE FBlendSample::SampleValue, in the axis parameters' units).
    struct BlendSample2D
    {
        std::string Clip;
        float       X = 0.0F;
        float       Y = 0.0F;
    };

    /// The payload of a BlendSpace2D node.
    struct BlendSpace2DNode
    {
        /// At least three, no two at one point, not all on one line (BlendSpace2DError): the plane the axes walk.
        std::vector<BlendSample2D> Samples;
        /// UE TargetWeightInterpolationSpeedPerSec: weight per second a sample's weight may move; 0 = no
        /// smoothing.
        float WeightSpeed = 0.0F;
        /// Whether the shared phase wraps (UE bLoop); otherwise it holds at the end.
        bool Loop = true;
    };

    /// What a state plays when it plays a Blend Space 2D (UE: a state whose graph is a Blend Space Player on a
    /// UBlendSpace). A state has no pins, so both axis parameters are named here: declared Float parameters.
    struct StateBlendSpace2D
    {
        std::string      AxisX;
        std::string      AxisY;
        BlendSpace2DNode Space;
    };

    /**
     * @brief The Delaunay triangulation of a 2D blend space's samples (UE FBlendSpaceData: its triangles and the
     *        hull edges a point outside is projected onto). Derived by TriangulateBlendSpace2D, kept by the player
     *        that runs the blend space (its sample data cache), never serialized.
     */
    struct BlendSpace2DTriangulation
    {
        /// The normalization: a point (x, y) sits at ((x - Origin) * Scale) in the space the triangles are in.
        std::array<float, 2> Origin{ 0.0F, 0.0F };
        std::array<float, 2> Scale{ 1.0F, 1.0F };
        /// Each sample's normalized point, parallel to the samples.
        std::vector<std::array<float, 2>> Points;
        /// Sample indices, counter-clockwise in the normalized space.
        std::vector<std::array<uint32_t, 3>> Triangles;
        /// The boundary: every triangle edge no second triangle shares, in its triangle's winding.
        std::vector<std::array<uint32_t, 2>> Hull;
    };

    /// Empty when `node` can be played; otherwise what is wrong: fewer than three samples, a sample naming no
    /// clip, a non-finite point, two samples at one point, a negative or non-finite WeightSpeed, or samples that
    /// all lie on one line (which spans no area: that is a Blend Space 1D).
    [[nodiscard]] std::string BlendSpace2DError( const BlendSpace2DNode& node );

    /// The Delaunay triangulation of `node`'s samples (Bowyer-Watson in normalized coordinates). `node` has at
    /// least one sample; a node BlendSpace2DError refuses may come back with no triangle.
    [[nodiscard]] BlendSpace2DTriangulation TriangulateBlendSpace2D( const BlendSpace2DNode& node );

    /// The weights the samples take at the point (`x`, `y`) of the axes (one per sample into `out`, sized by the
    /// caller to the samples): barycentric in the triangle holding the point, else the two samples of the nearest
    /// hull edge by the projection onto it; summing to 1.
    void BlendSpace2DTargetWeights( const BlendSpace2DTriangulation& triangulation, float x, float y,
                                    std::span<float> out );
} // namespace Desert::Animation::Graph
