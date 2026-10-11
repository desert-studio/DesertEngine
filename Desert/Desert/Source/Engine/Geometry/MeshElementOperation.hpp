#pragma once

// The Select Elements verbs that are not region extrusions (MeshRegionOperation.hpp) nor plane operations
// (MeshPlaneOperation.hpp): Delete, Offset, Bevel, Cut and Clean, on a copy of an DynamicMesh3 as UE's PolyEdit
// runs its verbs (EditMeshPolygonsTool.cpp ApplyDeleteFaces / UPolyEditBevelEdgeActivity / the Cut activity).
//
// Same contract as MeshRegionOperation.hpp: the input is not touched, the result is a NEW mesh plus the region
// the operation produced, in the selection's own mode; a refusal leaves nothing and names what and where. Units
// are centimetres in the mesh's own space. A Vertex / Edge / PolyGroup selection is converted to its WHOLE
// triangles (ConvertSelection) where the verb works on faces; one that covers no whole triangle is refused.

#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace Desert::Geometry
{
    // A plane in the mesh's space. Normal need not be unit; the positive side is where it points.
    struct CutPlane
    {
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
    };

    // The plane that contains the view rays through two points of the screen: a line drawn across the
    // viewport, extended into the scene - UE's knife. `modelViewProj` maps the mesh's space to clip space
    // (projection * view * model), the points are in NDC ([-1, 1] both axes). Refused when the two points
    // coincide or the matrix cannot be inverted.
    [[nodiscard]] Common::ResultStr<CutPlane>
    CutPlaneFromScreenLine( const glm::mat4& modelViewProj, const glm::vec2& ndcA, const glm::vec2& ndcB );

    // DELETE, UE's ApplyDeleteFaces: the region's triangles go (FDynamicMeshEditor::RemoveTriangles), and every
    // vertex left with no triangle. The result selection is empty, in the input's mode.
    [[nodiscard]] Common::ResultStr<RegionOutcome> DeleteSelection( const DynamicMesh3&     mesh,
                                                                    const ElementSelection& selection );

    // OFFSET (ours, no UE counterpart): the region's vertices move along their area- and angle-weighted vertex
    // normals (MeshNormals::ComputeVertexNormal) by a signed distance; no new geometry, the triangles around the
    // region stretch, the normals of every triangle touching a moved vertex are recomputed. Topology and IDs are
    // unchanged, so the selection comes back as it went in. Refused on distance 0 and on a triangle that flips.
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    OffsetSelection( const DynamicMesh3& mesh, const ElementSelection& selection, float distance );

    // BEVEL, UE's MeshBevel (UPolyEditBevelEdgeActivity: InitializeFromGroupTopologyEdges, one segment, the
    // material inferred from the neighbours): an Edge selection bevels the GROUP edges its mesh edges lie on; a
    // Triangle / PolyGroup selection bevels the group edges on the border of its groups (UE's face bevel). A
    // Vertex selection is refused - UE bevels edges and faces, not corners. `width` (> 0) is the inset into each
    // neighbouring face. The result selects the new triangles, in PolyGroup mode.
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    BevelSelection( const DynamicMesh3& mesh, const ElementSelection& selection, float width );

    // CUT the selected polygroups (any mode: the groups its whole triangles belong to) along the plane: every
    // edge of their triangles whose ends lie strictly on opposite sides is split where it crosses
    // (DynamicMesh3::SplitEdge, attributes interpolated, the neighbour across split too - no T-junction), and in
    // every group the plane crossed the triangles on the positive side get a new group, with a hard normal edge
    // along the cut. Refused: a selection covering no triangle; a plane that crosses none of the selected groups.
    // The result selection is both halves of every cut group, in PolyGroup mode.
    [[nodiscard]] Common::ResultStr<RegionOutcome>
    CutSelection( const DynamicMesh3& mesh, const ElementSelection& selection, const CutPlane& plane );

    // CLEAN the whole mesh: coincident open edges within `weldTolerance` are welded (MergeCoincidentMeshEdges,
    // split attributes welded along them, as Weld Edges), every edge shorter than the tolerance is collapsed onto
    // its lower vertex ID, a zero-area triangle with distinct corners is cured by flipping its longest edge, and
    // vertices left with no triangle are dropped. The counts are in the outcome's Report. Mesh-wide: the result
    // selection is empty, in @p mode. Refused on a negative tolerance and when there is nothing to clean.
    [[nodiscard]] Common::ResultStr<RegionOutcome> CleanMesh( const DynamicMesh3& mesh, float weldTolerance,
                                                              ElementMode mode );
} // namespace Desert::Geometry
