#pragma once

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    class IndexBuffer;
    class VertexBuffer;
} // namespace Desert::Graphic

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
        // The submesh this geometry is: what a hit's geometry index maps back to (its material slot). Not the
        // segment's position, because a hidden submesh contributes no segment and shifts the ones after it.
        uint32_t SubmeshIndex = 0;
        // OPAQUE: a hit is final (no any-hit). Not opaque: every candidate goes to the alpha test, once per
        // primitive (NO_DUPLICATE_ANY_HIT) - a landscape tile's cells that touch a hole.
        bool Opaque                                               = true;
        bool operator==( const RayTracingGeometrySegment& ) const = default;
    };

    // Is submesh @p index hidden by @p hiddenSubmeshes (MeshComponent::HiddenSubmeshes: bit i = submesh i hidden)?
    // The mask holds 64 submeshes; one past that cannot be hidden and is visible, as at draw.
    [[nodiscard]] constexpr bool IsSubmeshHidden( const uint64_t hiddenSubmeshes, const std::size_t index )
    {
        return index < 64u && ( hiddenSubmeshes >> index & 1ull ) != 0;
    }

    // Every submesh with at least one triangle at LOD 0 that @p hiddenSubmeshes does not hide, in submesh order
    // (UE: a section hidden in ray tracing has no segment, so a ray passes where the raster draws nothing). A
    // submesh with no LOD chain (procedural meshes) uses its own index range, which IS its LOD 0 (Submesh::LODs
    // comment). A trailing partial triangle (IndexCount not a multiple of 3) is not a triangle and is not counted.
    // The vertex layout a BLAS reads positions from: the mesh's own vertex buffer (Desert::Vertex) for a static
    // mesh, the skin pass's packed positions for a deformed one.
    struct RayTracingVertexLayout
    {
        uint32_t Stride         = 0;
        uint32_t PositionOffset = 0;
    };

    inline constexpr RayTracingVertexLayout kStaticMeshVertexLayout{
         static_cast<uint32_t>( sizeof( Vertex ) ), static_cast<uint32_t>( offsetof( Vertex, Position ) ) };
    // UE's skin cache output for ray tracing: one float3 position per vertex, nothing else, in the mesh's own
    // vertex order — so the mesh's index buffer and its submeshes' vertex ranges address it unchanged.
    inline constexpr RayTracingVertexLayout kDeformedPositionLayout{ static_cast<uint32_t>( sizeof( glm::vec3 ) ),
                                                                     0u };

    [[nodiscard]] inline std::vector<RayTracingGeometrySegment>
    DescribeMeshGeometry( std::span<const Submesh> submeshes, const uint64_t hiddenSubmeshes,
                          const RayTracingVertexLayout layout )
    {
        std::vector<RayTracingGeometrySegment> segments;
        segments.reserve( submeshes.size() );
        for ( std::size_t index = 0; index < submeshes.size(); ++index )
        {
            if ( IsSubmeshHidden( hiddenSubmeshes, index ) )
                continue;
            const Submesh& submesh     = submeshes[index];
            const uint32_t indexOffset = submesh.LODs.empty() ? submesh.IndexOffset : submesh.LODs[0].IndexOffset;
            const uint32_t indexCount  = submesh.LODs.empty() ? submesh.IndexCount : submesh.LODs[0].IndexCount;
            const uint32_t triangles   = indexCount / 3u;
            if ( triangles == 0 || submesh.VertexCount == 0 )
                continue;

            RayTracingGeometrySegment segment;
            segment.VertexFormat    = RayTracingVertexFormat::Float3;
            segment.VertexStride    = layout.Stride;
            segment.PositionOffset  = layout.PositionOffset;
            segment.FirstVertex     = submesh.VertexOffset;
            segment.MaxVertex       = submesh.VertexOffset + submesh.VertexCount - 1u;
            segment.IndexType       = RayTracingIndexType::UInt32;
            segment.IndexByteOffset = indexOffset * static_cast<uint32_t>( sizeof( uint32_t ) );
            segment.PrimitiveCount  = triangles;
            segment.SubmeshIndex    = static_cast<uint32_t>( index );
            segments.push_back( segment );
        }
        return segments;
    }

    [[nodiscard]] inline std::vector<RayTracingGeometrySegment>
    DescribeStaticMeshGeometry( std::span<const Submesh> submeshes, const uint64_t hiddenSubmeshes )
    {
        return DescribeMeshGeometry( submeshes, hiddenSubmeshes, kStaticMeshVertexLayout );
    }

    // A skinned mesh's per-frame BLAS input (UE dynamic geometry): the same submesh rule over the skin pass's
    // packed positions, with the instance's own hidden mask — the BLAS is the instance's, so it can leave out what
    // that instance hides.
    [[nodiscard]] inline std::vector<RayTracingGeometrySegment>
    DescribeDeformedMeshGeometry( std::span<const Submesh> submeshes, const uint64_t hiddenSubmeshes )
    {
        return DescribeMeshGeometry( submeshes, hiddenSubmeshes, kDeformedPositionLayout );
    }

    // THE DEVICE-LEVEL BLAS INPUT OF ONE MESH (UE FRayTracingGeometryInitializer): the buffers a build reads and
    // the segments over them (vertex stride, position offset and format, index type, per-segment ranges, opaque).
    // The RenderCore builders take this and nothing above it; the side that knows the mesh fills it
    // (Engine/Graphic/Render/RayTracingMeshInput.hpp), so the API layer never includes Mesh or SkinnedMesh.
    struct RayTracingMeshInput
    {
        // The mesh's vertex buffer: what a static / procedural BLAS reads positions from, and what the skin pass
        // reads for a deformed one (its BLAS reads the cached positions instead).
        std::shared_ptr<VertexBuffer> Vertices;
        std::shared_ptr<IndexBuffer>  Indices;
        // Vertices in Vertices: a deformed BLAS sizes its cached positions by it (a static input leaves it 0).
        uint32_t VertexCount = 0;
        // Empty: nothing to build (no visible triangle, or a skinned mesh asked for a static BLAS).
        std::vector<RayTracingGeometrySegment> Segments;
    };

    // Which level an acceleration structure is. The ledger keeps the two under their own kinds (a mesh BLAS and a
    // view TLAS answer different questions), and this is the one place the level picks the kind.
    enum class AccelerationStructureLevel : uint8_t
    {
        Bottom,
        Top,
    };

    // THE LEDGER ROW OF AN ACCELERATION STRUCTURE'S STORAGE. The device object holds the returned token from the
    // moment its storage is allocated to the moment that storage is released: destroying (or resetting) the token
    // is the release, so a BLAS dropped with its mesh or a TLAS reallocated for a larger scene cannot leave a row
    // behind. @p bytes is the allocation's device footprint (VulkanAllocator::AllocationSize). The owner is the
    // attribution scope the build runs in (the scene renderer's).
    [[nodiscard]] inline ResourceOwnership
    RecordAccelerationStructureMemory( const AccelerationStructureLevel level, const std::size_t bytes )
    {
        const ResourceKind kind = level == AccelerationStructureLevel::Bottom
                                       ? ResourceKind::BottomLevelAccelerationStructure
                                       : ResourceKind::TopLevelAccelerationStructure;
        return ResourceOwnership::Take( kind, bytes );
    }

    // A built BLAS as the renderer sees it: the address a TLAS instance row points at. The API object behind it
    // (VulkanRayTracingGeometry) owns the acceleration structure and its storage and releases them on the
    // frame-retire rule when the last mesh reference drops.
    class RayTracingGeometryResource
    {
    public:
        virtual ~RayTracingGeometryResource() = default;

        // VkAccelerationStructureInstanceKHR::accelerationStructureReference. Never 0 for a built resource.
        [[nodiscard]] virtual uint64_t GetDeviceAddress() const = 0;
        // Bytes of the acceleration-structure storage this resource owns (the RT scene stats overlay; the
        // ledger row of the same allocation is the census).
        [[nodiscard]] virtual uint64_t GetMemoryBytes() const = 0;
    };
} // namespace Desert::Graphic::RayTracing
