#pragma once

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <cstdint>

// THE MESH SIDE OF A BLAS BUILD (Engine): turns a Mesh / SkinnedMesh into the device-level RayTracingMeshInput the
// RenderCore builders take (VulkanRayTracingGeometry, the procedural and skinned BLAS). The API layer sits below
// Engine and never sees the mesh; this is the one place that reads the mesh for it.
namespace Desert::Graphic::RayTracing
{
    // A static or procedural mesh's BLAS input: its own vertex and index buffers, one segment per visible LOD-0
    // submesh (DescribeStaticMeshGeometry), each opaque unless @p masks says its section is masked. A skinned mesh
    // has no static BLAS (its triangles move every frame; the skin cache builds its BLAS from
    // DescribeDeformedMeshBlas), so its input has no segment. VertexCount stays 0: a static build addresses
    // vertices through the segments' ranges only.
    [[nodiscard]] inline RayTracingMeshInput DescribeStaticMeshBlas( const Mesh&                   mesh,
                                                                     const RayTracingSubmeshMasks& masks )
    {
        RayTracingMeshInput input;
        const MeshGeometry& geometry = mesh.GetGeometry();
        input.Vertices         = geometry.Vertices;
        input.Indices          = geometry.Indices;
        input.VertexByteOffset = geometry.VertexByteOffset;
        input.IndexByteOffset  = geometry.IndexByteOffset;
        if ( !mesh.IsSkinned() )
            input.Segments = DescribeStaticMeshGeometry( mesh.GetSubmeshes(), masks );
        return input;
    }

    // A skinned instance's BLAS input: the mesh's buffers (the skin pass reads the vertices), its vertex count
    // (the cached positions' size) and the segments over the packed positions (DescribeDeformedMeshGeometry) with
    // the instance's own hidden and non-opaque masks. @p lod: the level the record skins and draws this frame
    // (SkinnedMeshRenderData::Lod) - its triangles, over its own vertex copies (SkinnedMesh::GetSkinnedLODs), are
    // the only ones whose cached positions are this frame's pose.
    [[nodiscard]] inline RayTracingMeshInput DescribeDeformedMeshBlas( const SkinnedMesh&            mesh,
                                                                       const RayTracingSubmeshMasks& masks,
                                                                       const uint32_t                lod = 0u )
    {
        RayTracingMeshInput input;
        const MeshGeometry& geometry = mesh.GetGeometry();
        input.Vertices         = geometry.Vertices;
        input.Indices          = geometry.Indices;
        input.VertexByteOffset = geometry.VertexByteOffset;
        input.IndexByteOffset  = geometry.IndexByteOffset;
        input.VertexCount = mesh.GetVertexCount();
        input.Segments    = DescribeDeformedMeshGeometry( mesh.GetSubmeshes(), masks );
        if ( lod == 0u )
            return input;
        for ( RayTracingGeometrySegment& segment : input.Segments )
        {
            const Submesh& submesh = mesh.GetSubmeshes()[segment.SubmeshIndex];
            if ( submesh.LODs.empty() )
                continue;
            const LODRange& range   = submesh.LODs[std::min<std::size_t>( lod, submesh.LODs.size() - 1u )];
            segment.IndexByteOffset = range.IndexOffset * static_cast<uint32_t>( sizeof( uint32_t ) );
            segment.PrimitiveCount  = range.IndexCount / 3u;
            segment.MaxVertex       = mesh.GetVertexCount() - 1u; // the level's copies follow LOD 0's vertices
        }
        return input;
    }
} // namespace Desert::Graphic::RayTracing
