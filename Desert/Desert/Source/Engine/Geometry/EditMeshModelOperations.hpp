#pragma once

#include "EditMeshTopologyOperations.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Geometry
{
    // WHOLE-MESH SHAPE OPERATIONS - UE's Model tab: Subdivide (USubdividePolyTool, the Loop scheme on
    // triangles) and Trim (UTrimMeshesTool). Plane Cut and Mirror run on the DynamicMesh3 (MeshPlaneOperation.hpp).
    //
    // Same contract as EditMeshOperations.hpp: pure functions, the input is not touched, the result is a NEW
    // EditMesh; a refusal leaves nothing half-done and names what and where. None takes a selection: each
    // acts on every triangle and hands back an empty one (Plane Cut: its cap), as every ID changes. Units are
    // centimetres in the mesh's own space.

    enum class SubdivideScheme : uint8_t
    {
        // Every triangle is cut into four at its edge midpoints; no vertex moves, so the shape, the volume
        // and every attribute value stay exactly what they were - only the tessellation is finer.
        Uniform,
        // The same split, then Loop's smoothing masks: an old interior vertex of valence n moves to
        // (1 - n*beta) * itself + beta * its neighbours (beta = 3/16 for n = 3, else 3/(8n)); a new interior
        // edge vertex to 3/8 of each end + 1/8 of each opposite corner. The surface shrinks towards the
        // smooth limit (a cube's volume falls monotonically, level after level).
        Loop,
    };

    [[nodiscard]] const char* ToString( SubdivideScheme scheme );

    // Level N quadruples the triangle count N times; above this the mesh is refused, named with the count.
    inline constexpr int kMaxSubdivideLevels     = 5;
    inline constexpr int kMaxSubdividedTriangles = 4 * 1024 * 1024;

    // SUBDIVIDE the whole mesh `levels` times (1 .. kMaxSubdivideLevels).
    //   * Open border: its vertices do not move and its new edge vertices sit at the exact midpoint, under
    //     both schemes - the border stays the polyline it was (Loop's own boundary mask is a B-spline that
    //     pulls a curved border inwards; a hole the user modelled must keep its size). A bowtie vertex is
    //     pinned the same way: it has no single ring to average.
    //   * Polygroup and material: each of the four children keeps its parent triangle's.
    //   * UVs, colours: carried with their seams - a new edge vertex gets one element per distinct pair of
    //     end elements, so an edge that was a seam is a seam in both of its halves; the value is the
    //     midpoint of the two (UVs are not smoothed: a texture must not slide over the surface).
    //   * Normals, Uniform: carried the same way, the midpoint renormalised - the faces are the same planes,
    //     so the interpolated normal is still the right one.
    //   * Normals, Loop: REBUILT smooth, one element per vertex, the angle-weighted average of its
    //     triangles. Loop rounds every edge, the ones a hard normal marked included; a hard normal left on a
    //     surface with no crease under it would draw a line the geometry does not have.
    //   * Tangents: rebuilt from UV layer 0 and the new normals (ComputeTangentsAt).
    // Refused: levels outside the range, an empty mesh, a result above kMaxSubdividedTriangles.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> SubdivideMesh( const EditMesh& mesh, int levels,
                                                                    SubdivideScheme scheme );

    enum class TrimSide : uint8_t
    {
        RemoveInside,  // the part of the mesh inside the cutter goes
        RemoveOutside, // only the part inside the cutter stays
    };

    [[nodiscard]] const char* ToString( TrimSide side );

    // TRIM the mesh with another mesh's surface (UE's UTrimMeshesTool / the Boolean tool's Trim mode): the part
    // inside (or outside) the cutter is removed and the cut is left OPEN, as UE's Trim leaves it - a trimmed
    // surface, not a solid. `cutterToMesh` maps the cutter's space into the mesh's (inverse(meshWorld) *
    // cutterWorld).
    //   THE BOUNDARY: there is no mesh Boolean here (decision A4 keeps Boolean out of the first wave), so the
    //   cutter must be a CLOSED CONVEX mesh - a box, a wedge, a prism, any shape bounded by flat faces with no
    //   dent. Its inside is then the intersection of its face planes' back sides: the mesh is split along
    //   every distinct face plane (edges crossing a plane get a vertex, attributes interpolated) and each
    //   triangle is inside when its centroid is behind all of them. The splits reach the whole mesh, not only
    //   the part the cutter touches, so the triangles beyond the cutter are re-cut along the planes too - the
    //   shape and attributes there are unchanged.
    // Refused: an empty or open cutter (an edge with one triangle, named); a non-convex one (a cutter vertex in
    // front of one of its face planes, named with the distance); a degenerate cutter (no face with area); a
    // cutter that does not reach the mesh, or one that would remove all of it.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> TrimMesh( const EditMesh& mesh, const EditMesh& cutter,
                                                               const glm::mat4& cutterToMesh, TrimSide side );
} // namespace Desert::Geometry
