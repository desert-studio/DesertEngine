#pragma once

#include "EditMeshOperations.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace Desert::Geometry
{
    // OPERATIONS THAT RE-CUT THE SURFACE - the second half of UE's PolyEdit verbs: Bevel (MeshBevel), Cut (a
    // plane cut in the manner of FMeshPlaneCut, applied to polygroups) and Clean (FMeshClean-like weld /
    // degenerate / isolated repair). Insert Edge Loop is the ported GroupEdgeInserter (MeshRegionOperation.hpp).
    //
    // Same contract as EditMeshOperations.hpp: pure functions, the input is not touched, the result is a NEW
    // EditMesh plus the region the operation produced; a refusal leaves nothing half-done and names what and
    // where. Units are centimetres in the mesh's own space.
    //
    // THE PLANE-CUT CORE is shared by Bevel and Cut: every edge of the working triangles whose ends lie
    // strictly on opposite sides of a plane is split where it crosses (EditMesh::SplitEdge, so every attribute
    // layer is interpolated at the new vertex and the neighbour across the edge is split too - no T-junction),
    // until no working triangle straddles the plane. Bevel then removes the corner on the plane's positive side
    // and closes the hole with a flat cap; Cut hands the positive side of each crossed polygroup a new group.

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

    // BEVEL (one segment, a chamfer) of the selected EDGES or VERTICES, width > 0.
    //   * Edge: a convex edge between two polygroups is replaced by a flat strip of its own new polygroup,
    //     `width` wide on each face measured perpendicular to the edge. The strip is where the plane through
    //     the two offset lines cuts the solid, so at a corner the third face loses a triangle: on a box edge
    //     of length L the volume drops by exactly width^2 * L / 2.
    //   * Vertex: the corner is cut by the plane perpendicular to its (angle-weighted) normal, placed so the
    //     cut runs `width` from the vertex along its edges on average; the cap gets its own polygroup.
    // The cap's normals are flat (a new polygroup: hard edge to its neighbours), its UVs a planar projection
    // at the neighbouring faces' texel density, its colours those of the corner it replaced, its material the
    // one of the triangles it was cut from. Refused: any other mode; an open-border, flat or concave edge (a
    // chamfer removes a convex corner - a concave one would need material added); edges that share a vertex
    // (a chained bevel needs mitred corners, which a one-segment cut through each edge cannot give); a width
    // at which the cut would reach any vertex but the bevelled ones. The result selection is the new caps in
    // PolyGroup mode.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    BevelSelection( const EditMesh& mesh, const ElementSelection& selection, float width );

    // CUT the selected polygroups (any mode: the groups its whole triangles belong to) along the plane. Every
    // selected group the plane crosses is split in two: its triangles on the positive side get a new group,
    // with a hard normal edge along the cut. The surface stays as closed as it was. Refused: a selection
    // covering no triangle; a plane that crosses none of the selected groups. The result selection is both
    // halves of every cut group, in PolyGroup mode.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    CutSelection( const EditMesh& mesh, const ElementSelection& selection, const CutPlane& plane );

    // ── the plane-cut core, shared with the Model tab's Plane Cut and Trim ──────────────────────────────

    // Splits every edge of the working triangles (`inSet`, indexed by triangle ID; it grows with the splits)
    // whose ends lie strictly on opposite sides of `plane`, until none does. A split triangle's halves stay in
    // the set; a neighbour outside it is split too (its halves stay outside). Every attribute layer is
    // interpolated at the new vertices (EditMesh::SplitEdge).
    [[nodiscard]] Common::BoolResultStr SplitMeshAlongPlane( EditMesh& mesh, std::vector<char>& inSet,
                                                             const CutPlane& plane, const char* what );

    struct PlaneCutCap
    {
        int Group            = InvalidId; // the cap's new polygroup; InvalidId when no cap was built
        int RemovedTriangles = 0;
        int CapTriangles     = 0;
        int CapLoops         = 0; // separate outlines capped (a U shape cut across its arms has two)
    };

    // Splits the working triangles along `plane` (as SplitMeshAlongPlane), then REMOVES the ones on its
    // positive side - and a triangle lying in the plane that faces against the normal (the removed side's
    // wall). With `fillHole`, each outline the removal opens is closed by a flat cap: one new polygroup for
    // all of them, ear-clipped, facing along the normal, UVs a planar projection at the neighbouring faces'
    // texel density, colours and material from the kept faces along the rim, normals rebuilt per polygroup
    // at the rim, tangents on the cap. Refused while capping: an outline left open by the mesh's own open
    // border, one that passes a vertex twice, and a HOLE inside another outline (a tube cut across: its
    // outline runs clockwise) - a cap with holes is not built. The mesh is left mid-edit on a refusal; the
    // callers work on a copy.
    [[nodiscard]] Common::ResultStr<PlaneCutCap> CutAwayPositiveSide( EditMesh& mesh, std::vector<char>& inSet,
                                                                      const CutPlane& plane, bool fillHole,
                                                                      const char* what );

    struct CleanCounts
    {
        int WeldedVertices   = 0; // vertices merged into another within the tolerance
        int CollapsedRemoved = 0; // triangles the weld collapsed onto an edge or a point
        int ZeroAreaFlipped  = 0; // three distinct but collinear corners, cured by flipping the longest edge
        int ZeroAreaKept     = 0; // ... where that flip was refused (a seam or the border): still in the mesh
        int IsolatedRemoved  = 0; // vertices with no triangle, dropped
    };

    struct CleanOutcome
    {
        MeshEditOutcome Edit;
        CleanCounts     Counts;
    };

    // CLEAN the whole mesh: vertices closer than `weldTolerance` (cm, >= 0) are merged into the lowest ID
    // among them (which keeps its position); triangles the merge collapses are removed; zero-area triangles
    // with distinct corners are removed by flipping their longest edge; vertices with no triangle are
    // dropped. Attribute elements are carried over, so a UV or normal seam at a welded vertex stays a seam.
    // Refused when a weld would put a third triangle on an edge or duplicate a triangle (two coincident
    // faces): that is a surface the weld cannot join, named with the triangle.
    [[nodiscard]] Common::ResultStr<CleanOutcome> CleanMesh( const EditMesh& mesh, float weldTolerance );
} // namespace Desert::Geometry
