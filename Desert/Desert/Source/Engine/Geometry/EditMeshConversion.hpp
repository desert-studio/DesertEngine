#pragma once

#include "EditMesh.hpp"
#include "MeshTypes.hpp"

#include <Common/Core/ResultStr.hpp>

#include <vector>

namespace Desert::Geometry
{
    // EditMesh <-> the engine's render-mesh arrays (Vertex / Index / Submesh, what DynamicMesh and
    // StaticMesh are built from). Kept free of the GPU classes so it is testable without a device.
    //
    // THE RENDER SIDE HAS NO TOPOLOGY. A render vertex is one (position, normal, tangent frame, UV) tuple, so
    // an EditMesh corner is emitted once per distinct combination of the elements its triangles use there:
    // every seam in a carried layer becomes duplicated render vertices, and nothing else does. A cube with
    // a hard-edged normal layer is 8 EditMesh vertices and 24 render vertices.
    //
    // WHAT CROSSES AND WHAT CANNOT. The render Vertex carries one normal, one tangent + bitangent and ONE UV
    // channel. So the conversion carries the normal overlay, the tangent overlay (bitangent = cross(N, T) * w,
    // the convention of ShapeGenerators and the procedural factories) and ONE chosen UV layer. The colour
    // overlay and the other UV layers have no field to land in; they are not part of the render key either,
    // so a colour seam does not multiply render vertices for data that is then dropped.
    //
    // MATERIALS. One Submesh per distinct MaterialID, in ascending MaterialID order, each with its own
    // contiguous vertex range (indices are submesh-local, drawn with baseVertex = VertexOffset, the way
    // VulkanRenderer draws them). A corner used by two materials is emitted once per submesh.

    struct RenderMeshData
    {
        std::vector<Vertex>  Vertices;
        std::vector<Index>   Indices; // submesh-local: add the submesh's VertexOffset
        std::vector<Submesh> Submeshes;
        // Index-aligned with Submeshes. ToRenderMesh fills it; FromRenderMesh reads it, and when it is empty
        // submesh i becomes MaterialID i.
        std::vector<int> SubmeshMaterialIds;
        // ToRenderMesh only: the EditMesh vertex each render vertex came from, and the EditMesh triangle each
        // render triangle came from - what a tool needs to map a pick or a paint stroke back.
        std::vector<int> SourceVertices;
        std::vector<int> SourceTriangles;
    };

    // Refused, naming the triangle, when the normal layer is disabled or any live triangle is unset in a
    // layer that is carried (normals; tangents when enabled; the UV layer when the mesh has any). A mesh
    // with no UV layer at all converts with TexCoord 0, and one with no tangent layer with a zero tangent
    // frame: those are the mesh's actual state, not a gap the conversion papers over. uvLayer names the
    // layer that goes to Vertex::TexCoord and must exist when the mesh has UV layers.
    [[nodiscard]] Common::ResultStr<RenderMeshData> ToRenderMesh( const EditMesh& mesh, int uvLayer = 0 );

    struct ImportedEditMesh
    {
        EditMesh Mesh;
        // Everything the import had to do that was not a straight copy, so a caller can say it out loud.
        int DroppedDegenerate = 0; // two corners welded onto one vertex, or the indices repeat a vertex
        int DroppedDuplicate  = 0; // the same three welded vertices as an earlier triangle
        int DetachedTriangles = 0; // a third triangle on an edge or an opposite winding: given its own copies
                                   // of its already-used corners (EditMesh refuses such an edge; the render
                                   // mesh had it)
    };

    // The render side cannot say which of its vertices are "the same corner"; the import decides by
    // distance. Exact equality is too strict for real data: the engine's own MakeSphere puts the seam
    // column and the south pole ring ~1e-7 apart (cos(2 pi) != cos(0) in float), and an exact weld turns
    // that into an open sphere with a slit down one side. The defaults are far below anything a modeler
    // means as two points (1e-3 cm = 10 microns) and far above float noise at editing scales.
    struct WeldOptions
    {
        float PositionTolerance  = 1e-3f; // cm, per axis
        float AttributeTolerance = 1e-5f; // per component, for elements at one welded vertex
    };

    // Welds render vertices by position into EditMesh vertices, and each layer by value per welded vertex
    // into overlay elements (the first value seen is kept), so seams appear exactly where the render data
    // differs across a corner. Enables normals, tangents (w from the bitangent's handedness) and one UV
    // layer. MaterialID per triangle comes from its submesh. An empty Submeshes list means one submesh over
    // everything. Refused, with numbers, on an index outside its submesh's vertex range or a submesh
    // outside the arrays.
    [[nodiscard]] Common::ResultStr<ImportedEditMesh> FromRenderMesh( const RenderMeshData& render,
                                                                      const WeldOptions&    options = {} );
} // namespace Desert::Geometry
