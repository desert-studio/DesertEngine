#pragma once

#include "EditMesh.hpp"
#include "EditMeshSelection.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>

namespace Desert::Geometry
{
    // OPERATIONS ON A MESH ELEMENT SELECTION that still run on the EditMesh: Delete and Offset. Extrude,
    // Push/Pull, Inset and Outset run on DynamicMesh3 through the ported OffsetMeshRegion / InsetMeshRegion
    // (Engine/Geometry/UECore/Operations, P11); the editor calls those directly.
    //
    // Pure functions: the input mesh is not touched, the result is a NEW EditMesh plus the operated region
    // on it, in the selection's own mode. That is what the editor's undo needs (the mesh is immutable once an
    // entity holds it; undo keeps the old one by reference) and what makes every operation testable without
    // an editor.
    //
    // THE REGION. Every operation works on TRIANGLES: a Vertex / Edge / PolyGroup selection is converted with
    // ConvertSelection (towards a coarser mode a triangle counts only when ALL its parts are selected), so
    // "delete these two vertices" deletes the triangles they fully cover, not every triangle that touches
    // them. A selection that covers no whole triangle is refused, naming the mode and the count.
    //
    // REFUSED, with the mesh untouched and a message naming what and where: an empty selection or one
    // covering no triangle; distance 0 (nothing would change - an explicit no-op is a refusal, not a silent
    // success); a region that touches itself at one vertex; and an Offset in which a triangle flips over.
    //
    // Units: distances in centimetres, in the mesh's own space.

    struct MeshEditOutcome
    {
        EditMesh Mesh;
        // The region the operation acted on, on the result, in the input selection's mode (empty after
        // Delete). Triangle IDs change for the region's triangles: they are re-created on the new vertices.
        ElementSelection Selection{ ElementMode::Triangle };
        // What the operation did beyond the mesh itself, in numbers, for the log - where a ring stopped, what
        // a clean removed. Empty when the topology change says it all.
        std::string Report;
    };

    // Removes the region's triangles, and every vertex left with no triangle. On a closed mesh the open
    // edges of the result are exactly the region's boundary.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> DeleteSelection( const EditMesh&         mesh,
                                                                      const ElementSelection& selection );

    // Moves the region's vertices along their normals (VertexNormals rule) by a signed distance with no new
    // geometry: the triangles around the region stretch. Topology and IDs are unchanged.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    OffsetSelection( const EditMesh& mesh, const ElementSelection& selection, float distance );
} // namespace Desert::Geometry
