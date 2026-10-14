#pragma once

#include <Engine/Geometry/MeshTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// THE BLAS BUILD INPUT OF A STATIC MESH (UE FRayTracingGeometryInitializer + its segments), device-free. One
// geometry per submesh, all reading the mesh's ONE vertex buffer and ONE index buffer — the same buffers the
// raster draw binds (MeshBufferUsage gives them the build-input usage where the device has acceleration
// structures). LOD 0 only: a ray sees the full-detail surface whatever LOD the raster picked (UE does the same for
// static meshes). The Vulkan object (VkAccelerationStructureGeometryTrianglesDataKHR +
// VkAccelerationStructureBuildRangeInfoKHR per entry) is a mapping of this and nothing more.
namespace Desert::Graphic::RayTracing
{
    // The position attribute's encoding in the vertex buffer. Vertex::Position is three floats at offset 0;
    // a quantized vertex format adds a value here and the Vulkan mapping follows (R16G16B16A16_SNORM, ...).
    enum class RayTracingVertexFormat : uint8_t
    {
        Float3,
    };

    // Index width in the index buffer. The engine's index buffers are uint32 (Desert::Index, LODRange units).
    enum class RayTracingIndexType : uint8_t
    {
        UInt32,
    };

    struct RayTracingGeometrySegment
    {
        RayTracingVertexFormat VertexFormat   = RayTracingVertexFormat::Float3;
        uint32_t               VertexStride   = 0; // bytes between two positions
        uint32_t               PositionOffset = 0; // bytes from a vertex's start to its position
        uint32_t FirstVertex          = 0; // the submesh's base vertex (VkAccelerationStructureBuildRangeInfoKHR)
        uint32_t MaxVertex            = 0; // highest vertex addressed (index + FirstVertex): Vulkan maxVertex
        RayTracingIndexType IndexType = RayTracingIndexType::UInt32;
        uint32_t            IndexByteOffset = 0; // primitiveOffset: bytes into the index buffer
        uint32_t            PrimitiveCount  = 0; // triangles
    };

    // Every submesh with at least one triangle at LOD 0, in submesh order. A submesh with no LOD chain (procedural
    // meshes) uses its own index range, which IS its LOD 0 (Submesh::LODs comment). A trailing partial triangle
    // (IndexCount not a multiple of 3) is not a triangle and is not counted.
    [[nodiscard]] inline std::vector<RayTracingGeometrySegment>
    DescribeStaticMeshGeometry( std::span<const Submesh> submeshes )
    {
        std::vector<RayTracingGeometrySegment> segments;
        segments.reserve( submeshes.size() );
        for ( const Submesh& submesh : submeshes )
        {
            const uint32_t indexOffset = submesh.LODs.empty() ? submesh.IndexOffset : submesh.LODs[0].IndexOffset;
            const uint32_t indexCount  = submesh.LODs.empty() ? submesh.IndexCount : submesh.LODs[0].IndexCount;
            const uint32_t triangles   = indexCount / 3u;
            if ( triangles == 0 || submesh.VertexCount == 0 )
                continue;

            RayTracingGeometrySegment segment;
            segment.VertexFormat    = RayTracingVertexFormat::Float3;
            segment.VertexStride    = static_cast<uint32_t>( sizeof( Vertex ) );
            segment.PositionOffset  = static_cast<uint32_t>( offsetof( Vertex, Position ) );
            segment.FirstVertex     = submesh.VertexOffset;
            segment.MaxVertex       = submesh.VertexOffset + submesh.VertexCount - 1u;
            segment.IndexType       = RayTracingIndexType::UInt32;
            segment.IndexByteOffset = indexOffset * static_cast<uint32_t>( sizeof( uint32_t ) );
            segment.PrimitiveCount  = triangles;
            segments.push_back( segment );
        }
        return segments;
    }
} // namespace Desert::Graphic::RayTracing
