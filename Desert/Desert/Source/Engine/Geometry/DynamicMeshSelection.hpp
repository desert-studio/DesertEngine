#pragma once

// Element picking and selection on the PORTED core (P10): DynamicMesh3 for the elements, GroupTopology for the
// polygroups. The algorithms (ElementSelectionAlgorithms.inl) read the DynamicMesh3 through a view, so a
// selection's IDs are the DynamicMesh3's own.
//
// PickElement is ported from UE's FMeshTopologySelector::FindSelectedElement (see DynamicMeshSelection.cpp for
// what was adapted): a vertex is the within-tolerance one nearest ALONG THE RAY, an edge the one with the least
// ray-area metric, and an occluded choice is a miss. Grow / Shrink match UE's FMeshFaceSelection
// ExpandToOneRingNeighbours / ContractBorderByOneRingNeighbours on closed manifolds; UE 5.8 has no Grow/Shrink in
// GeometrySelectionUtil.

#include "Engine/Geometry/ElementSelection.hpp"
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

    // The polygroup topology every Modeling Mode pick and operation runs on (UE UEditMeshPolygonsTool's
    // Topology): a group edge is also cut where its boundary turns sharper than
    // PolyEditExtraCornerAngleDegrees, so a plane's one-group border is four edges, not one loop. UE's
    // UPolyEditTopologyProperties default: bAddExtraCorners on, ExtraCornerAngleThresholdDegrees 135. The
    // selection and every operation that maps its mesh IDs back to group edges must build the SAME topology.
    inline constexpr double     PolyEditExtraCornerAngleDegrees = 135.0;
    [[nodiscard]] GroupTopology PolyEditTopology( const DynamicMesh3& mesh );

    // @p topology is PolyEditTopology( mesh ): MeshElementSelection keeps the two together. The hit's Id is
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

    // Every live triangle ID of @p mesh, in iteration order: the "whole mesh" region a mesh-wide operation
    // (UV unwrap/reset, XForm merge/duplicate) runs on. One definition; the per-file copies in
    // DynamicMeshUVOperations.cpp and DynamicMeshXformOperations.cpp collided in one unity blob (C2084).
    [[nodiscard]] std::vector<int> AllTriangleIds( const DynamicMesh3& mesh );
} // namespace Desert::Geometry
