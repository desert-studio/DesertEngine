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
// The complete types, not forward declarations: this adapter is the editor's door to the MeshCore types its
// modeling tools work on - the mesh and topology a selection is made of (MeshElementSelection holds the mesh and
// owns the topology), the attribute set the material / UV / tangent tools read and write per element, and the
// AABB tree the brush tools (Paint Groups) ray-pick through. The MeshCore include gate
// (scripts/CI/MeshCoreIncludes.sh) lets only the Geometry adapters include MeshCore/ from outside it.
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
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

    // UE PolyEdit's Select Edge Loops / Select Edge Rings (FGroupTopologySelector::ExpandSelectionByEdgeLoops /
    // ExpandSelectionByEdgeRings). A LOOP runs on through a corner where exactly four faces meet, to the edge that
    // borders neither face of the edge it arrives on, and along an open border through a corner where exactly two
    // border edges meet - UE's "paths through valence-4 vertices, or boundaries of holes". A RING steps across a
    // four-sided face to its opposite edge and on through the face beyond, until a border, a face that is not a
    // quad, or the ring closes.
    enum class EdgeExpansion : uint8_t
    {
        Loops,
        Rings,
    };
    [[nodiscard]] const char* ToString( EdgeExpansion expansion );

    // @p edges (mesh edge IDs, as an Edge selection holds them) plus every mesh edge of the loops / rings through
    // them; ascending, unique. @p topology is PolyEditTopology( mesh ). At the Group level the faces are
    // polygroups and the steps go group edge to group edge (each adds all of its mesh edges); an edge of @p edges
    // inside a polygroup (on no group edge) is kept and starts nothing. At the Triangle level every triangle is a
    // face (UE TriEdit's TriangleGroupTopology), so a loop runs through vertices with four triangles and a ring,
    // with no four-sided face to cross, adds nothing - as in UE. Refused, by ID, when an edge is not the mesh's.
    [[nodiscard]] Common::ResultStr<std::vector<int>> ExpandEdges( const DynamicMesh3&  mesh,
                                                                   const GroupTopology& topology,
                                                                   TopologyLevel level, EdgeExpansion expansion,
                                                                   std::span<const int> edges );

    // Every live triangle ID of @p mesh, in iteration order: the "whole mesh" region a mesh-wide operation
    // (UV unwrap/reset, XForm merge/duplicate) runs on. One definition; the per-file copies in
    // DynamicMeshUVOperations.cpp and DynamicMeshXformOperations.cpp collided in one unity blob (C2084).
    [[nodiscard]] std::vector<int> AllTriangleIds( const DynamicMesh3& mesh );
} // namespace Desert::Geometry
