#pragma once

#include "EditMesh.hpp"
#include "EditMeshSelection.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>

namespace Desert::Geometry
{
    // OPERATIONS ON A MESH ELEMENT SELECTION - UE's Modeling Mode PolyEdit verbs (UEditMeshPolygonsTool:
    // Delete, Extrude, Push/Pull, Offset, Inset/Outset), over an EditMesh and an ElementSelection of it.
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
    // THE PATTERN is UE's (FDynamicMeshEditor::DisconnectTriangles + StitchVertexLoopsMinimal, FExtrudeMesh,
    // FInsetMeshRegion, FOffsetMeshRegion), re-implemented over EditMesh's own edits: the region is cut
    // loose along its boundary loops (its triangles get their own copies of every vertex), the copies are
    // moved, and each boundary loop is stitched to its copy by a strip of quads - the extrude's side walls or
    // the inset's ring. What the strip gets, and why:
    //   * polygroups - one NEW group per run of boundary edges that share the same outside neighbour group,
    //     so extruding a cube face gives four side faces (each its own group) and extruding a cylinder cap
    //     one smooth ring;
    //   * normals - rebuilt per polygroup (ComputeNormalsByPolyGroup's rule) at every vertex the operation
    //     moved or created and at the old boundary: hard edges between the walls and the region, smooth
    //     inside one wall group. Normals elsewhere are left exactly as they were;
    //   * UVs - Extrude/Push-Pull walls are unwrapped by arc length along the boundary and by height, at the
    //     region's own texel density, so a wall is not stretched; Inset keeps the region's UV chart: the
    //     moved boundary vertices get the UV of the point they moved to, the ring shares the region's UVs on
    //     its inner side, and the texture does not move;
    //   * tangents - computed from UV layer 0 on the new triangles; the region keeps its own;
    //   * colours - copied from the region's corner on either side of the strip;
    //   * material ID - the one of the region triangle on that boundary edge.
    //
    // REFUSED, with the mesh untouched and a message naming what and where: an empty selection or one
    // covering no triangle; distance 0 (nothing would change - an explicit no-op is a refusal, not a silent
    // success); Extrude / Push-Pull / Inset on a region that reaches the mesh's OPEN border (there is no
    // outside triangle to stitch that side to); a region that touches itself at one vertex (two boundary
    // loops through it - the copy would be a bowtie); Extrude of a closed piece (no boundary: nothing to
    // wall - Offset moves it); and any result in which a triangle flips over (a push deeper than the
    // region's own normals allow, an inset wider than the region).
    //
    // Units: distances in centimetres, in the mesh's own space.

    enum class ExtrudeDirection : uint8_t
    {
        // Each vertex along the angle-weighted average of the region's face normals at it, lengthened so
        // every face moves by the full distance (a cube corner moves by d * sqrt(3)). UE's default.
        VertexNormals,
        // Every vertex along ONE direction: the area-weighted average normal of the region. Walls are
        // parallel, so a negative push into a solid cannot fold its own walls.
        RegionNormal,
    };

    [[nodiscard]] const char* ToString( ExtrudeDirection direction );

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

    // Moves the region out by distance (> 0) and walls it to where it was. Topology: +Vb vertices and
    // +2*Eb triangles for a boundary of Vb vertices and Eb edges.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> ExtrudeSelection( const EditMesh&         mesh,
                                                                       const ElementSelection& selection,
                                                                       float                   distance,
                                                                       ExtrudeDirection        direction );

    // Extrude along the region's average normal with a SIGNED distance: negative pushes the region into
    // the surface (a pocket), positive pulls it out.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    PushPullSelection( const EditMesh& mesh, const ElementSelection& selection, float distance );

    // Moves the region's vertices along their normals (VertexNormals rule) by a signed distance with no new
    // geometry: the triangles around the region stretch. Topology and IDs are unchanged.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    OffsetSelection( const EditMesh& mesh, const ElementSelection& selection, float distance );

    // Inset (distance > 0): the region's boundary moves inwards by distance, in the region's own surface,
    // and a ring of quads fills the band between the old and the new boundary. The region plus the ring
    // cover exactly the area the region did on a planar face.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    InsetSelection( const EditMesh& mesh, const ElementSelection& selection, float distance );

    // Outset (distance > 0) - UE's Outset, which is its Inset with the distance negated: the region's
    // boundary moves OUTWARDS by distance and the ring joins it back to the old boundary. The ring therefore
    // lies folded under the grown region (it faces the other way, UE's result too); it is the start of a
    // lip or a flange, meant to be extruded or moved next.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome>
    OutsetSelection( const EditMesh& mesh, const ElementSelection& selection, float distance );
} // namespace Desert::Geometry
