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
        // This IS the upload, and the fence's (Mesh::EnsureGpuResources, SceneRenderer::PrepareMesh): a fresh mesh
        // too (TextECSSystem's glyph quads, PrimitiveMeshFactory's shared shapes) waits for it. It settles the
        // flag.
        m_GpuStale = false;
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
                m_LODScreenCoverage = Geometry::LODScreenCoverage( m_Vertices, m_Indices, m_Submeshes );
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

        // THE EDIT'S HALF, NOTHING A REPLAY READS (CPU-ONE-SYNC step 5; UE: the game thread builds the new data,
        // the render side swaps it in). The CPU arrays are the game side's: no render code reads a DynamicMesh's
        // m_Vertices / m_Indices (a replay reads the device geometry, the submeshes, the revision and the BLAS),
        // so they are written here even while a view job replays this mesh. The re-upload, the revision bump and
        // the BLAS release are the fence's, once however many edits landed in between (Mesh::EnsureGpuResources
        // from SceneRenderer::PrepareMesh); a failed upload is reported there, once. Not drawn this frame: the
        // edit waits for the first submit, as a mesh that was never drawn always did.
        m_Vertices = vertices;
        m_Indices  = indices;
        MarkGpuStale();
    }

} // namespace Desert