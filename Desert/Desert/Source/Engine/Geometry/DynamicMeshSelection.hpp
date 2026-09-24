#pragma once

// Element picking and selection on the PORTED core (P10): FDynamicMesh3 for the elements, FGroupTopology for the
// polygroups. The algorithms are the ones the EditMesh path runs (ElementSelectionAlgorithms.inl, one source for
// both until P8b), read through a view of the FDynamicMesh3, so a selection's IDs are the FDynamicMesh3's own:
// vertex, triangle and group IDs agree with the EditMesh view of the same mesh; EDGE IDs are the FDynamicMesh3's,
// and an operation still on the bridge converts them (Bridge::ToEditMeshSelection).
//
// PickElement is ported from UE's FMeshTopologySelector::FindSelectedElement (see DynamicMeshSelection.cpp for
// what was adapted): a vertex is the within-tolerance one nearest ALONG THE RAY, an edge the one with the least
// ray-area metric, and an occluded choice is a miss. Grow / Shrink match UE's FMeshFaceSelection
// ExpandToOneRingNeighbours / ContractBorderByOneRingNeighbours on closed manifolds; UE 5.8 has no Grow/Shrink in
// GeometrySelectionUtil.

#include "Engine/Geometry/EditMeshSelection.hpp"

namespace Desert::Geometry
{
    class FDynamicMesh3;
    class FGroupTopology;

    // @p topology is FGroupTopology( &mesh, true ): MeshElementSelection keeps the two together.
    [[nodiscard]] ElementHit       PickElement( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                                ElementMode mode, const PickView& view );
    [[nodiscard]] ElementSelection ConvertSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                                     const ElementSelection& selection, ElementMode target );
    [[nodiscard]] ElementSelection SelectConnected( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                                    const ElementSelection& selection );
    [[nodiscard]] ElementSelection GrowSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                                  const ElementSelection& selection );
    [[nodiscard]] ElementSelection ShrinkSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                                    const ElementSelection& selection );
} // namespace Desert::Geometry
