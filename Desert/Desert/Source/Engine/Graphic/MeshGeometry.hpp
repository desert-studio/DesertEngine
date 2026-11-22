#pragma once

#include <Engine/Geometry/MeshTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

// A MESH'S GPU GEOMETRY, BY VALUE, WITHOUT THE POOL (UE: FStaticMeshVertexBuffers / FRawStaticIndexBuffer are
// named by the render data, the RHI buffer types stay forward-declared): which arena, which span of the pool
// (GeometryPoolRange / GeometryPoolAllocation) and which buffers a mesh draws from. Geometry/Mesh.hpp holds a
// MeshGeometry by value; the pool itself (Graphic/GeometryPool.hpp) and the buffer classes
// (VertexBuffer.hpp / IndexBuffer.hpp) are complete only where a renderer binds or uploads them.
namespace Desert::Graphic
{
    class VertexBuffer;
    class IndexBuffer;

    // One vertex layout's arena.
    enum class GeometryArena : uint8_t
    {
        Static  = 0, // Desert::Vertex
        Skinned = 1, // Desert::SkinnedVertex
    };
    inline constexpr std::size_t kGeometryArenaCount = 2;

    [[nodiscard]] constexpr uint32_t GeometryArenaVertexStride( const GeometryArena arena )
    {
        return arena == GeometryArena::Static ? static_cast<uint32_t>( sizeof( Vertex ) )
                                              : static_cast<uint32_t>( sizeof( SkinnedVertex ) );
    }
    inline constexpr uint32_t kGeometryStreamStride = static_cast<uint32_t>( sizeof( MeshVertexStreams ) );
    inline constexpr uint32_t kGeometryIndexStride  = static_cast<uint32_t>( sizeof( uint32_t ) );

    // Where a mesh's geometry sits in the pool: vertices (and streams) [BaseVertex, +VertexCount) of its arena's
    // page VertexPage, indices [FirstIndex, +IndexCount) of index page IndexPage (IndexCount 0 = a non-indexed
    // mesh). A virtual-geometry mesh's CLUSTER WORDS (every submesh DAG's VertexRefs then Triangles, in submesh
    // order) follow its indices in the same span: [FirstIndex + IndexCount, +ClusterWordCount), so its one index
    // span holds both and the VirtualMeshTable addresses the words from the span's device address.
    struct GeometryPoolRange
    {
        GeometryArena Arena       = GeometryArena::Static;
        uint32_t      VertexPage  = 0;
        uint32_t      IndexPage   = 0;
        uint32_t      BaseVertex  = 0;
        uint32_t      VertexCount = 0;
        uint32_t      FirstIndex  = 0;
        uint32_t      IndexCount  = 0;
        uint32_t      ClusterWordCount = 0;

        // The index span's elements: the indices and the cluster words after them.
        [[nodiscard]] uint32_t IndexSpanCount() const
        {
            return IndexCount + ClusterWordCount;
        }
        [[nodiscard]] bool operator==( const GeometryPoolRange& ) const = default;
    };

    class GeometryPool;

    // A mesh's spans in the pool, released (on the frame rule) when it is destroyed or replaced. Move-only.
    class GeometryPoolAllocation
    {
    public:
        GeometryPoolAllocation() = default;
        ~GeometryPoolAllocation();
        GeometryPoolAllocation( GeometryPoolAllocation&& other ) noexcept;
        GeometryPoolAllocation& operator=( GeometryPoolAllocation&& other ) noexcept;
        GeometryPoolAllocation( const GeometryPoolAllocation& )            = delete;
        GeometryPoolAllocation& operator=( const GeometryPoolAllocation& ) = delete;

        [[nodiscard]] explicit operator bool() const
        {
            return m_Pool != nullptr;
        }
        [[nodiscard]] const GeometryPoolRange& Range() const
        {
            return m_Range;
        }
        // Releases now (on the frame rule); the allocation is empty after.
        void Reset();

    private:
        friend class GeometryPool;
        GeometryPoolAllocation( GeometryPool* pool, const GeometryPoolRange& range )
             : m_Pool( pool ), m_Range( range )
        {
        }
        GeometryPool*     m_Pool = nullptr;
        GeometryPoolRange m_Range;
    };

    // The buffers one draw binds, and where in them: what a renderer compares to skip a redundant bind. A
    // pooled mesh's shared binding is its arena's three buffers at offset 0 (the same for every mesh of the
    // arena); an unpooled mesh (a GPU-written twin) binds its own at its offsets.
    struct MeshGeometryBinding
    {
        const VertexBuffer* Vertices     = nullptr;
        uint64_t            VertexOffset = 0;       // bytes
        const VertexBuffer* Streams      = nullptr; // null = the renderer's default streams
        uint64_t            StreamOffset = 0;
        const IndexBuffer*  Indices      = nullptr; // null = non-indexed
        uint64_t            IndexOffset  = 0;
        // Last frame's positions (ShaderProtocols::kMeshPreviousPositionBinding, float3 a vertex), bound only for
        // a pipeline that reads them; null = Vertices at VertexOffset, bound there but not read (the row's flag).
        const VertexBuffer* PreviousVertices     = nullptr;
        uint64_t            PreviousVertexOffset = 0;

        [[nodiscard]] bool operator==( const MeshGeometryBinding& ) const = default;
    };

    // A MESH'S GPU GEOMETRY: the buffers holding it and where its data begins in each. A pooled mesh's three are
    // its pages' (Pooled set); a twin built from GPU-written buffers holds its own, or a mix (a morphed twin's
    // own vertices over its base's pooled streams and indices).
    struct MeshGeometry
    {
        std::shared_ptr<VertexBuffer> Vertices;
        std::shared_ptr<VertexBuffer> Streams; // null = the mesh has no stream buffer: default streams
        std::shared_ptr<IndexBuffer>  Indices; // null = non-indexed
        // THE VERTICES' POSITIONS LAST FRAME (the skinned format's binding 3, VertexInputs.hpp): one float3 per
        // vertex (kMeshPreviousPositionStride) - a morphed twin's previous blend when no skin cache keeps the
        // history (MeshRendererMorph); null for every other mesh (its row lacks PrimitiveFlags::
        // kPreviousPositions, its stage reads its own positions). Never pooled: only a GPU-written twin has one.
        std::shared_ptr<VertexBuffer> PreviousVertices;
        uint64_t                      VertexByteOffset = 0;
        uint64_t                      StreamByteOffset = 0;
        uint64_t                      IndexByteOffset  = 0;
        uint32_t                      VertexStride     = 0;
        uint32_t                      VertexCount      = 0;
        uint32_t                      IndexCount       = 0;
        // Cluster words after the indices (IndexByteOffset + IndexCount * 4): a virtual-geometry mesh's DAG data.
        uint32_t ClusterWordCount = 0;
        // The mesh's own data carries colours / a second UV set (a pooled mesh without them has default values
        // written into its stream span, so Streams alone does not say whether the asset authored any).
        bool AuthoredStreams = false;
        // Set when Vertices, Streams and Indices are all the pool's (Range is then the mesh's span).
        std::optional<GeometryPoolRange> Pooled;

        [[nodiscard]] bool IsResident() const
        {
            return Vertices != nullptr && VertexCount > 0;
        }
        // The per-mesh bind: every buffer at this mesh's own offsets, so a mesh-relative draw (submesh
        // IndexOffset / VertexOffset as authored) reads this mesh.
        [[nodiscard]] MeshGeometryBinding MeshBinding() const
        {
            return { Vertices.get(),   VertexByteOffset, Streams.get(),           StreamByteOffset,
                     Indices.get(),    IndexByteOffset,  PreviousVertices.get(), 0 };
        }
        // The shared bind (GPU-driven bins): a pooled mesh binds its pages at offset 0 and its draws add
        // SharedBaseVertex / SharedFirstIndex; an unpooled one is its own binding with a zero base.
        [[nodiscard]] MeshGeometryBinding SharedBinding() const
        {
            if ( !Pooled )
                return MeshBinding();
            return { Vertices.get(), 0, Streams.get(), 0, Indices.get(), 0, nullptr, 0 };
        }
        [[nodiscard]] uint32_t SharedBaseVertex() const
        {
            return Pooled ? Pooled->BaseVertex : 0u;
        }
        [[nodiscard]] uint32_t SharedFirstIndex() const
        {
            return Pooled ? Pooled->FirstIndex : 0u;
        }
    };
} // namespace Desert::Graphic
