#pragma once

// Extrude / Push-Pull / Inset / Outset on a selected region of an FDynamicMesh3, as UE's PolyEdit runs them on a
// copy of the mesh: FExtrudeOp (ModelingOperators/Private/DeformationOps/ExtrudeOp.cpp:94-146) drives
// FOffsetMeshRegion, UPolyEditInsetOutsetActivity (PolyEditInsetOutsetActivity.cpp:181) drives FInsetMeshRegion
// with the distance negated for Outset.
//
// TANGENTS. The ported region operations set normals and UVs on the triangles they create but never the tangent
// overlays: UE treats tangents as derived data and rebuilds them when the mesh description is built. A mesh that
// carries a tangent space here would otherwise leave the new triangles unset in it, and ToRenderMesh refuses such
// a mesh; so the result's tangents are recomputed from its normals and UV layer 0 (FMeshTangents, per triangle).

#include "Engine/Geometry/DynamicMeshSelection.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Geometry
{
    enum class RegionOperation : uint8_t
    {
        Extrude,
        PushPull,
        Inset,
        Outset,
    };

    [[nodiscard]] const char* ToString( RegionOperation operation );

    struct RegionOutcome
    {
        std::shared_ptr<const FDynamicMesh3> Mesh;
        // The moved region (Extrude, Push/Pull) or the inset region (Inset, Outset), in the selection's mode.
        ElementSelection Selection{ ElementMode::Triangle };
    };

    // Runs `operation` on a copy of `before`. `distance` is in cm: positive for Extrude / Inset / Outset, signed
    // for Push/Pull. Refused, by name and numbers, on a zero or wrongly signed distance, a selection covering no
    // whole triangle, the ported operation's own failure, or a tangent space with no UV layer 0 to derive from.
    [[nodiscard]] Common::ResultStr<RegionOutcome> RunRegionOperation( RegionOperation         operation,
                                                                       const FDynamicMesh3&    before,
                                                                       const ElementSelection& selection,
                                                                       float                   distance );

    // Fill Hole, as UE's FHoleFillOp::CalculateResult runs it with the TriangleFan fill
    // (ModelingOperators/Private/CleaningOps/HoleFillOp.cpp:331-450): per open loop, the Newell plane of its
    // vertices (PolygonTriangulation::ComputePolygonPlane), negated as UE does, a fan in a new polygroup, the
    // plane normal on the fan and UVs projected on the plane at 1 / the mesh's largest bounds dimension
    // (HoleFillTool.cpp:218). An EDGE selection fills the loops through its edges; any other selection fills
    // every loop. The result selects the new triangles. Refused on a mesh with no open loop, a selected edge on
    // no loop, or a loop the filler rejects.
    [[nodiscard]] Common::ResultStr<RegionOutcome> FillHoles( const FDynamicMesh3&    before,
                                                              const ElementSelection& selection );

    // Insert Edge Loop, as UE's UPolyEditInsertEdgeLoopActivity drives FEdgeLoopInsertionOp
    // (PolyEditInsertEdgeLoopActivity.cpp:30-75, EdgeLoopInsertionOp.cpp:25-66) with the PlaneCut mode and one
    // ProportionOffset: FGroupEdgeInserter::InsertEdgeLoops across the group edge the EDGE selection lies on,
    // `position` in (0, 1) measured from that group edge's EndpointCorners.A (UE's unflipped direction). Every
    // crossed group is split in two new groups (a cube: 6 -> 10). The result selects the new loop's edges.
    // Refused, by name and numbers: a selection not in Edge mode or empty, edges on more than one group edge,
    // position outside (0, 1), or the inserter's own failure (with the problem group edge count).
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    InsertEdgeLoop( const FDynamicMesh3& before, const ElementSelection& selection, float position );

    // Weld Edges: FMergeCoincidentMeshEdges with UE's defaults (MergeCoincidentMeshEdges.h) and the split
    // attributes welded along merged edges, the settings of test WeldClosesACubeCutAlongEverySeam. Mesh-wide;
    // leaves an empty selection in @p mode. Refused when there is no boundary edge to weld.
    [[nodiscard]] Common::ResultStr<RegionOutcome> WeldEdges( const FDynamicMesh3& before, ElementMode mode );
} // namespace Desert::Geometry
