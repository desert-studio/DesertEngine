#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // UE 5.8 MeshModelingTools HoleFillTool.h (EHoleFillOpFillType): how a hole is closed.
    //   Trivial - a triangle fan around the loop's centroid (UE TriangleFan, SimpleHoleFiller);
    //   Minimal - the loop triangulated with no new vertex, then edges flipped towards the least-area surface,
    //             ties (a planar hole) broken by the Delaunay criterion (UE MinimalHoleFiller);
    //   Smooth  - the Minimal fill refined to the loop's edge length and its interior relaxed into a membrane
    //             over the loop (UE SmoothHoleFiller: fill, remesh the fill, smooth it with the boundary held).
    enum class HoleFillType
    {
        Trivial,
        Minimal,
        Smooth,
    };
    [[nodiscard]] const char* ToString( HoleFillType type );

    // UE UHoleFillToolProperties / USmoothHoleFillProperties: the fill type and, for Smooth, how dense the new
    // triangles are relative to the loop's edges (FillDensityScalar) and how far the interior is relaxed
    // (InteriorSmoothness, 0..1).
    struct HoleFillSettings
    {
        HoleFillType Type               = HoleFillType::Minimal; // UE's default
        float        FillDensity        = 1.0f;
        float        InteriorSmoothness = 0.5f;

        bool operator==( const HoleFillSettings& ) const = default;
    };

    // UE FHoleFillOp::CalculateResult: every loop of MeshBoundaryLoops( input ) whose index is in @p loops is
    // filled with @p settings; each fill gets its own triangle group, normals computed over the fill and UVs
    // projected on the loop's plane (UE: Editor.SetTriangleNormals / SetTriangleUVsFromProjection), and the
    // tangent space is rebuilt. Refused when @p loops is empty, names a loop that does not exist, or a loop
    // cannot be filled (the reason names it).
    [[nodiscard]] Common::ResultStr<DeformOutcome>
    HoleFill( const DynamicMesh3& input, const std::vector<int32_t>& loops, const HoleFillSettings& settings );
} // namespace Desert::Geometry
