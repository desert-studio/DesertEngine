#pragma once

// Element picking and selection on the PORTED core (P10): DynamicMesh3 for the elements, GroupTopology for the
// polygroups. The algorithms are the ones the EditMesh path runs (ElementSelectionAlgorithms.inl, one source for
// both until P8b), read through a view of the DynamicMesh3, so a selection's IDs are the DynamicMesh3's own:
// vertex, triangle and group IDs agree with the EditMesh view of the same mesh; EDGE IDs are the DynamicMesh3's,
// and an operation still on the bridge converts them (Bridge::ToEditMeshSelection).
//
// PickElement is ported from UE's FMeshTopologySelector::FindSelectedElement (see DynamicMeshSelection.cpp for
// what was adapted): a vertex is the within-tolerance one nearest ALONG THE RAY, an edge the one with the least
// ray-area metric, and an occluded choice is a miss. Grow / Shrink match UE's FMeshFaceSelection
// ExpandToOneRingNeighbours / ContractBorderByOneRingNeighbours on closed manifolds; UE 5.8 has no Grow/Shrink in
// GeometrySelectionUtil.

#include "Engine/Geometry/EditMeshSelection.hpp"
// The complete types, not forward declarations: this adapter is the editor's door to the two MeshCore types a
// selection is made of (MeshElementSelection holds the mesh and owns the topology), and the MeshCore include
// gate (scripts/CI/MeshCoreIncludes.sh) lets only the Geometry adapters include MeshCore/ from outside it.
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.hpp"

#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // Which topology Vertex / Edge picking runs on - UE's two Modeling Mode editors. Group (PolyEdit): an edge
    // is a GROUP edge (the span between two corners shared by two groups; a diagonal inside a group is not
    // pickable) and a vertex is a group CORNER. Triangle (TriEdit): every mesh edge and vertex.
    enum class TopologyLevel : uint8_t
    {
        Group,
        Triangle,
    };
    [[nodiscard]] const char* ToString( TopologyLevel level );

    // @p topology is GroupTopology( &mesh, true ): MeshElementSelection keeps the two together. The hit's Id is
    // a mesh ID in every mode and level (a group edge's picked SEGMENT, a corner's vertex); HitElements turns it
    // into what a click selects.
    [[nodiscard]] ElementHit PickElement( const DynamicMesh3& mesh, const GroupTopology& topology,
                                          ElementMode mode, const PickView& view, TopologyLevel level );
    // The mesh IDs a hit selects: at the Group level an Edge hit is every mesh edge of its group edge
    // (GetGroupEdgeEdges); otherwise the hit's own ID. Empty for a miss.
    [[nodiscard]] std::vector<int> HitElements( const GroupTopology& topology, ElementMode mode,
                                                TopologyLevel level, const ElementHit& hit );
    [[nodiscard]] ElementSelection ConvertSelection( const DynamicMesh3& mesh, const GroupTopology& topology,
                                                     const ElementSelection& selection, ElementMode target );
    [[nodiscard]] ElementSelection SelectConnected( const DynamicMesh3& mesh, const GroupTopology& topology,
                                                    const ElementSelection& selection );
    [[nodiscard]] ElementSelection GrowSelection( const DynamicMesh3& mesh, const GroupTopology& topology,
                                                  const ElementSelection& selection );
    [[nodiscard]] ElementSelection ShrinkSelection( const DynamicMesh3& mesh, const GroupTopology& topology,
                                                    const ElementSelection& selection );
    [[nodiscard]] ElementSelection InvertSelection( const DynamicMesh3& mesh, const GroupTopology& topology,
                                                    const ElementSelection& selection );
} // namespace Desert::Geometry
