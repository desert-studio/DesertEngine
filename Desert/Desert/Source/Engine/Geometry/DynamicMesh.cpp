#include "DynamicMesh.hpp"

#include "MeshLOD.hpp"

namespace Desert
{
    Common::BoolResultWithCodes<Desert::MeshError> DynamicMesh::Invalidate()
    {
        m_GpuStale = false; // this IS the upload; a direct call (a caller with a renderer) settles the flag
        m_VertexBuffer =
             Graphic::VertexBuffer::Create( (void*)m_Vertices.data(), m_Vertices.size() * sizeof( Vertex ) );
        const auto vertices = m_VertexBuffer->RT_Invalidate();
        if ( !vertices.IsSuccess() )
            return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                vertices.GetError() );

        if ( !m_Indices.empty() )
        {
            // With LODs, upload base + appended LOD indices (fills each submesh's LOD ranges) while
            // GetIndices() stays the base geometry; otherwise upload the base indices as-is.
            if ( m_GenerateLODs )
            {
                const std::vector<Index> gpu =
                     Geometry::BuildLODIndexBuffer( m_Vertices, m_Indices, m_Submeshes );
                m_IndexBuffer = Graphic::IndexBuffer::Create( gpu.data(), gpu.size() * sizeof( Index ) );
            }
            else
            {
                m_IndexBuffer =
                     Graphic::IndexBuffer::Create( m_Indices.data(), m_Indices.size() * sizeof( Index ) );
            }
            const auto uploaded = m_IndexBuffer->RT_Invalidate();
            if ( !uploaded.IsSuccess() )
                return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                    uploaded.GetError() );
        }
        else
        {
            m_IndexBuffer = nullptr;
        }

        return Common::MakeSuccessWithCodes<bool, MeshError>( true );
    }

    // THE IN-PLACE BRANCHES BELOW HAD NEVER ONCE WRITTEN A BYTE, AND Г13 IS HOW THAT BECAME VISIBLE.
    // Every DynamicMesh buffer is created through `Create( data, size )`, whose usage argument defaults
    // to BufferUsage::Static — and a static buffer is device-local with no mapping, so
    // VulkanVertexBuffer::SetData returned at its first line and wrote nothing. An edit that did not
    // CHANGE THE VERTEX COUNT therefore never reached the GPU: dragging a vertex in PolyEditTool moved
    // it on the CPU, left the picture alone, and said nothing, because SetData returned `void`.
    //
    // The fix is NOT to create these buffers Dynamic. DynamicMesh backs every primitive, every terrain
    // patch and every text mesh in the engine, so that would move all of the scene's geometry out of
    // device-local memory to make one editor drag cheaper. Instead the in-place write is ATTEMPTED and
    // its refusal is a fall-through to the re-upload that does work — which is what the size-change
    // branch has always done, and the reason nobody noticed the other one was dead.
    void DynamicMesh::Update( const std::vector<Vertex>& vertices, const std::vector<Index>& indices )
    {
        // A MESH THAT CARRIES A LOD CHAIN IS NOT EDITABLE THROUGH Update, AND NOW IT SAYS SO. Such a
        // mesh's GPU index buffer is the base indices PLUS an appended chain, with every submesh's
        // `LODs` range pointing into it (Geometry::BuildLODIndexBuffer, called from Invalidate above).
        // Update rebuilds the BASE list only — it always has — so re-uploading leaves each of those
        // ranges pointing past the end of the new buffer, which is an out-of-range indexed draw.
        //
        // It cannot happen today: every DynamicMesh in the engine, the editor and the runtime is
        // constructed with the default `generateLODs = false`, which also makes the LOD branch in
        // Invalidate dead code (named for the LOD task, not fixed here). The check exists so the first
        // `true` finds out from this line instead of from a driver. Regenerating the chain here is not
        // the alternative it looks like: Update runs once per mouse-move during a vertex drag, and the
        // chain is a meshopt simplification pass over the whole mesh.
        //
        // REFUSED BEFORE THE CPU DATA IS TOUCHED, so the two sides stay in agreement — a half-applied
        // edit that is on the CPU and not the GPU is the exact silence this task exists to remove.
        if ( m_GenerateLODs )
        {
            LOG_ERROR( "[DynamicMesh] Update was called on a mesh that carries a LOD chain; the edit was "
                       "NOT applied. Rebuild the mesh through Invalidate instead." );
            return;
        }

        m_Vertices = vertices;
        m_Indices  = indices;

        // NOT YET ON THE GPU (never drawn, or no renderer in this process): the edit is CPU data only and the
        // whole mesh goes up in one Invalidate when the renderer first submits it (Mesh::EnsureGpuResources).
        if ( m_GpuStale || !m_VertexBuffer )
        {
            MarkGpuStale();
            return;
        }

        // LOGGED RATHER THAN RETURNED, and the asymmetry with Invalidate above is deliberate. Update is
        // called once per mouse-move while a vertex is being dragged (PolyEditTool), and its single
        // caller is inside an ImGui interaction that has nowhere to put a failure — it can neither undo
        // the edit nor stop the drag meaningfully. What was missing was not a channel but a REPORT:
        // before this, a re-upload that failed left the mesh drawing its previous contents and the edit
        // simply did not appear.
        const uint32_t vertexBytes = static_cast<uint32_t>( m_Vertices.size() * sizeof( Vertex ) );
        bool           vertexFits  = m_VertexBuffer && vertexBytes <= m_VertexBuffer->GetSize();
        if ( vertexFits )
        {
            const auto written = m_VertexBuffer->SetData( (void*)m_Vertices.data(), vertexBytes );
            vertexFits         = written.IsSuccess();
        }
        if ( !vertexFits )
        {
            m_VertexBuffer      = Graphic::VertexBuffer::Create( (void*)m_Vertices.data(), vertexBytes );
            const auto uploaded = m_VertexBuffer->RT_Invalidate();
            if ( !uploaded.IsSuccess() )
                LOG_ERROR( "[DynamicMesh] vertex buffer re-upload failed, the edit is not on the GPU: {}",
                           uploaded.GetError() );
        }

        if ( m_Indices.empty() )
        {
            m_IndexBuffer = nullptr;
            return;
        }

        const uint32_t indexBytes = static_cast<uint32_t>( m_Indices.size() * sizeof( Index ) );
        bool           indexFits  = m_IndexBuffer && indexBytes <= m_IndexBuffer->GetSize();
        if ( indexFits )
        {
            const auto written = m_IndexBuffer->SetData( (void*)m_Indices.data(), indexBytes );
            indexFits          = written.IsSuccess();
        }
        if ( !indexFits )
        {
            m_IndexBuffer       = Graphic::IndexBuffer::Create( m_Indices.data(), indexBytes );
            const auto uploaded = m_IndexBuffer->RT_Invalidate(); // reported, not returned; see above
            if ( !uploaded.IsSuccess() )
                LOG_ERROR( "[DynamicMesh] index buffer re-upload failed, the edit is not on the GPU: {}",
                           uploaded.GetError() );
        }
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