#pragma once

#include "SavedMeshForm.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <string_view>

namespace Desert::Geometry
{
    // FDynamicMesh3 <-> the saved mesh form a scene stores under StaticMesh.EditMesh (SavedMeshForm.hpp). The
    // form is the one EditMeshSerialization reads and writes, unchanged, so a scene written by either core
    // opens in the other and the scene schema does not move.
    //
    // UE has no counterpart: its dynamic meshes are saved through FMeshDescription / UObject serialization,
    // neither of which Desert has. This is the EditMesh serializer carried onto the ported core.
    //
    // WHAT MAPS TO WHAT. Positions -> vertices (float widened to double and back, exact); PolyGroups -> the
    // mesh's triangle groups; MaterialIds -> the MaterialID attribute; Normals -> normal layer 0; Colors ->
    // the primary colour overlay; UVs[i] -> UV layer i. An absent Normals / Colors block is an absent layer, an
    // enabled-but-empty one is a layer with no elements, and the two stay different across a round trip.
    //
    // TANGENTS. The form stores a tangent element as xyz + a handedness sign; FDynamicMesh3 stores the
    // tangent (normal layer 1) and the bitangent (normal layer 2) as overlays of their own, the way UE does.
    // Reading makes one bitangent element per (tangent element, normal element) pair a triangle uses, valued
    // cross(N, T) * sign - bit for bit what the EditMesh render conversion computes per render vertex, so the
    // two cores draw the same thing. Writing recovers the sign as UE's VectorUtil::BinormalSign does:
    // -1 where dot(cross(N, T), B) < 0 at the first corner (ascending triangle, saved corner order) that has
    // both a normal and a bitangent, +1 otherwise. So only a sign of exactly +1 or -1 is readable, a -1 must
    // sit on at least one corner whose frame is not degenerate (cross(N, T) != 0), and a triangle set in the
    // tangent layer must be set in the normal layer; anything else is refused rather than read into a mesh
    // that would write back something different. A tangent layer without a bitangent layer (two normal
    // layers) writes every sign as +1 by the same rule.
    //
    // WINDING. The form's triangles are front-facing counter-clockwise (as EditMesh and the render mesh are);
    // FDynamicMesh3 keeps UE's clockwise front. Both directions swap corners 1 and 2 of every triangle, of
    // the mesh and of every overlay, the same boundary DynamicMeshRenderConversion keeps.
    //
    // IDS ARE COMPACTED ON WRITE, as the EditMesh writer does: vertices, triangles and each overlay's
    // elements are numbered densely in ascending ID order; an element no live triangle uses is not written.

    [[nodiscard]] EditMeshSer ToSerialized( const FDynamicMesh3& mesh );

    // Refused, naming @p owner (the entity the block belongs to), the array and the numbers, on anything the
    // writer above could not have produced: a length that is not a whole number of records, an index out of
    // range, a degenerate / duplicate / non-manifold triangle, a triangle only partly set in a layer, an
    // element named twice by one triangle or by corners of two different vertices, an element no triangle
    // uses, more UV layers than the core holds, and the tangent cases above. The result passes
    // FDynamicMesh3::CheckValidity.
    [[nodiscard]] Common::ResultStr<FDynamicMesh3> DynamicMeshFromSerialized( const EditMeshSer& saved,
                                                                              std::string_view   owner );
} // namespace Desert::Geometry
