#pragma once

#include "EditMesh.hpp"
#include "RenderMeshData.hpp"

#include <Common/Core/ResultStr.hpp>

#include <vector>

namespace Desert::Geometry
{
    // EditMesh <-> the engine's render-mesh arrays; RenderMeshData.hpp says what the render side is and what
    // crosses into it.
    //

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
        // Per render face (render.Indices order), the triangle it became, or InvalidId for a dropped one: what
        // lets per-face data (the file's polygroups) follow its face past the dropped ones.
        std::vector<int> TriangleOfFace;
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
