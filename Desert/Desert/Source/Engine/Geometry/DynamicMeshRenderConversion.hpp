#pragma once

#include "RenderMeshData.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <vector>

namespace Desert::Geometry
{
    // DynamicMesh3 <-> the engine's render-mesh arrays (RenderMeshData.hpp), the same arrays and the same
    // rules as the EditMesh conversion (EditMeshConversion.hpp), so a caller can switch cores without the
    // render side seeing a difference.
    //
    // UE's counterparts (FDynamicMeshToMeshDescription / FMeshDescriptionToDynamicMesh) convert to
    // FMeshDescription, which Desert does not have; the render side here is Desert's own Vertex / Index /
    // Submesh, so this is the EditMesh conversion carried onto the ported core, not a port.
    //
    // WHAT CROSSES. The primary normal overlay; the primary tangent and bitangent overlays when the mesh has a
    // tangent space (UE stores the bitangent as its own overlay, so it crosses as stored rather than being
    // rebuilt from a handedness sign as the EditMesh layer does); ONE UV layer; the MaterialID attribute (no
    // MaterialID attribute means every triangle is material 0). A render vertex is one (vertex, normal
    // element, tangent element, bitangent element, UV element) tuple, emitted once per submesh.
    //
    // WINDING. A render (and EditMesh) triangle is front-facing counter-clockwise; DynamicMesh3 keeps UE's
    // clockwise front (VectorUtil::Normal is (V2-V0)x(V1-V0)), so MeshNormals and every other ported algorithm
    // stays verbatim. Both conversions swap corners 1 and 2; a round trip restores the render order exactly.
    //
    // Refused, naming the triangle, when the mesh has no attribute set or no normal overlay, or a live
    // triangle is unset in a carried overlay. uvLayer must exist when the mesh has UV layers.
    [[nodiscard]] Common::ResultStr<RenderMeshData> ToRenderMesh( const DynamicMesh3& mesh, int uvLayer = 0 );

    struct ImportedDynamicMesh
    {
        DynamicMesh3  Mesh;
        int           DroppedDegenerate = 0; // two corners welded onto one vertex
        int           DroppedDuplicate  = 0; // the same three welded vertices as an earlier triangle
        int           DetachedTriangles = 0; // a third triangle on an edge: given its own copies of its
                                             // already-used corners (DynamicMesh3 refuses such an edge)
        // Per render face (render.Indices order), the triangle it became, or DynamicMesh3::InvalidID for a
        // dropped one: what lets per-face data (the file's polygroups) follow its face past the dropped ones.
        std::vector<int> TriangleOfFace;
    };

    // Welds render vertices by position into mesh vertices, and each overlay by value per welded vertex into
    // elements (the first value seen is kept), so seams appear exactly where the render data differs across a
    // corner. Enables attributes with one normal, tangent, bitangent and UV layer and the MaterialID
    // attribute; MaterialID per triangle comes from its submesh. Refused, with numbers, on an index outside
    // its submesh's vertex range or a submesh outside the arrays.
    [[nodiscard]] Common::ResultStr<ImportedDynamicMesh>
    DynamicMeshFromRenderMesh( const RenderMeshData& render, const WeldOptions& options = {} );
} // namespace Desert::Geometry
