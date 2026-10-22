#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

namespace Desert::Geometry
{
    // UE 5.8 MeshModelingTools WeldMeshEdgesTool.h (UWeldMeshEdgesToolProperties), its defaults and ranges:
    // Tolerance - two boundary edges are coincident when both endpoint pairs are closer than this (cm; UE
    //             FMathf::ZeroTolerance by default, clamped to [1e-8, 1000]);
    // Only Unique Pairs - only edges with exactly one coincident partner are merged (UE bOnlyUnique);
    // Weld Attributes - the split UV / normal / tangent seams at the merged edges' vertices are welded too
    //             (FMergeCoincidentMeshEdges::bWeldAttrsOnMergedEdges).
    struct WeldEdgesSettings
    {
        float Tolerance       = 1e-6f;
        bool  OnlyUniquePairs = false;
        bool  WeldAttributes  = true;

        bool operator==( const WeldEdgesSettings& ) const = default;
    };

    // UE UWeldMeshEdgesTool / FWeldMeshEdgesOp::CalculateResult on the whole mesh: FMergeCoincidentMeshEdges
    // merges every pair of coincident boundary edges into one interior edge (no bowtie can form, two edges of
    // the same orientation never merge). DeformOutcome::MovedVertices carries the welded edge pairs,
    // PinnedVertices the open edges left (UE RemainingEdges); Report names the initial open edges (UE
    // InitialEdges), the pairs and the remainder. Refused when a setting is out of its range or the merge fails;
    // a mesh with no open edge is reported, not refused (UE accepts it unchanged).
    [[nodiscard]] Common::ResultStr<DeformOutcome> WeldMeshEdges( const DynamicMesh3&      input,
                                                                  const WeldEdgesSettings& settings );
} // namespace Desert::Geometry
