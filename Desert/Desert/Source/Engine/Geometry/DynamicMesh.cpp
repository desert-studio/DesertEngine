#include "DynamicMesh.hpp"

#include "MeshLOD.hpp"

#include <cstdint>
#include <vector>

namespace Desert
{
    // THE WHOLE MESH GOES INTO A NEW POOL ALLOCATION (GeometryPool, static arena) on every upload: the first one,
    // a rebuild, and every Update edit. Never an in-place rewrite of the current span - a frame still in flight
    // may be reading it; the previous allocation is released on the pool's frame rule, which also covers an edit
    // that changes the vertex or index count (the sculpt resize).
    Common::BoolResultWithCodes<Desert::MeshError> DynamicMesh::Invalidate()
    {
        m_GpuStale = false; // this IS the upload; a direct call (a caller with a renderer) settles the flag
        if ( m_Vertices.empty() )
        {
            ReleaseGeometry(); // nothing to draw: no allocation, IsResident() false
            return Common::MakeSuccessWithCodes<bool, MeshError>( true );
        }

        // With LODs, upload base + appended LOD indices (fills each submesh's LOD ranges) while GetIndices()
        // stays the base geometry; otherwise the base indices as-is. No indices: a non-indexed mesh.
        std::vector<uint32_t> indices;
        if ( !m_Indices.empty() )
        {
            if ( m_GenerateLODs )
            {
                const std::vector<Index> gpu = Geometry::BuildLODIndexBuffer( m_Vertices, m_Indices, m_Submeshes,
                                                                              Geometry::UnbakedLODChain() );
                ResolveLODScreenSizes();
                indices = FlattenIndices( gpu );
            }
            else
            {
                indices = FlattenIndices( m_Indices );
            }
        }

        auto uploaded = Graphic::GeometryPool::Get().RT_Upload(
             Graphic::GeometryArena::Static, std::as_bytes( std::span<const Vertex>( m_Vertices ) ), {}, indices );
        if ( !uploaded.IsSuccess() )
            return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                uploaded.GetError() );
        AdoptPoolAllocation( uploaded.ExtractValue(), false );
        return Common::MakeSuccessWithCodes<bool, MeshError>( true );
    }

    void DynamicMesh::Update( const std::vector<Vertex>& vertices, const std::vector<Index>& indices )
    {
        // A MESH THAT CARRIES A LOD CHAIN IS NOT EDITABLE THROUGH Update, AND NOW IT SAYS SO. Such a
        // mesh's GPU index data is the base indices PLUS an appended chain, with every submesh's
        // `LODs` range pointing into it (Geometry::BuildLODIndexBuffer, called from Invalidate above).
        // Update rebuilds the BASE list only, so re-uploading would leave each of those ranges pointing
        // past the end of the new span, which is an out-of-range indexed draw. Every DynamicMesh is
        // constructed with the default `generateLODs = false`; the check makes the first `true` say so.
        //
        // REFUSED BEFORE THE CPU DATA IS TOUCHED, so the two sides stay in agreement.
        if ( m_GenerateLODs )
        {
            LOG_ERROR( "[DynamicMesh] Update was called on a mesh that carries a LOD chain; the edit was "
                       "NOT applied. Rebuild the mesh through Invalidate instead." );
            return;
        }

        m_Vertices = vertices;
        m_Indices  = indices;
        // New content under the same address: every cache keyed by the mesh's render identity redraws it.
        m_RenderRevision.Bump();

        // NOT YET ON THE GPU (never drawn, or no renderer in this process): the edit is CPU data only and the
        // whole mesh goes up in one Invalidate when the renderer first submits it (Mesh::EnsureGpuResources).
        if ( m_GpuStale || !GetGeometry().IsResident() )
        {
            MarkGpuStale();
            return;
        }

        // LOGGED RATHER THAN RETURNED: Update is called once per mouse-move while a vertex is dragged
        // (PolyEditTool), inside an ImGui interaction that can neither undo the edit nor stop the drag; a
        // failed upload leaves the mesh drawing its previous contents, so it is reported here.
        if ( const auto uploaded = Invalidate(); !uploaded.IsSuccess() )
            LOG_ERROR( "[DynamicMesh] re-upload failed, the edit is not on the GPU: {}", uploaded.GetError() );
    }

    void DynamicMesh::Flatten()
    {
        if ( m_Indices.empty() )
            return;

        std::vector<Vertex> newVertices;
        newVertices.reserve( m_Indices.size() * 3 );

        for ( const auto& index : m_Indices )
        {
            newVertices.push_back( m_Vertices[index.V1] );
            newVertices.push_back( m_Vertices[index.V2] );
            newVertices.push_back( m_Vertices[index.V3] );
        }

        m_Vertices = std::move( newVertices );
        m_Indices.clear();

        // Update submeshes to reflect new vertex counts and lack of indices
        uint32_t vertexOffset = 0;
        for ( auto& submesh : m_Submeshes )
        {
            // This is a bit naive as it assumes submeshes were partitioning the indices
            // In a real scenario, we might need to track which submesh each index belonged to.
            // For now, let's assume one submesh or simple linear split.
            uint32_t triangleCount = submesh.IndexCount / 3;
            submesh.VertexCount    = triangleCount * 3;
            submesh.VertexOffset   = vertexOffset;
            submesh.IndexCount     = 0;
            submesh.IndexOffset    = 0;

            vertexOffset += submesh.VertexCount;
        }

        Invalidate();
    }

} // namespace Desert