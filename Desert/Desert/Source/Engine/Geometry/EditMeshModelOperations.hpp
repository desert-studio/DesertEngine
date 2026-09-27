#pragma once

#include "EditMeshTopologyOperations.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Geometry
{
    // WHOLE-MESH SHAPE OPERATIONS - UE's Model tab: Trim (UTrimMeshesTool). Plane Cut and Mirror run on the
    // DynamicMesh3 (MeshPlaneOperation.hpp, P14), Subdivide on the ported core (MeshRegionOperation.hpp
    // SubdivideMesh, P15).
    //
    // Same contract as EditMeshOperations.hpp: pure functions, the input is not touched, the result is a NEW
    // EditMesh; a refusal leaves nothing half-done and names what and where. None takes a selection: each
    // acts on every triangle and hands back an empty one, as every ID changes. Units are
    // centimetres in the mesh's own space.


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
