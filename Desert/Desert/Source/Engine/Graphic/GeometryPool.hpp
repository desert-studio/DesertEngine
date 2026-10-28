#pragma once

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// THE GEOMETRY POOL (UE: the GPU scene's shared geometry buffers that one bind serves for many meshes; Nanite's
// streaming pool is the same shape). Every mesh's vertices, optional streams and indices live in ONE device
// vertex buffer per vertex layout (an arena: Static = Vertex, Skinned = SkinnedVertex), ONE stream buffer per
// arena laid out in parallel (vertex i of the arena's vertex buffer and stream i of its stream buffer are the
// same vertex, so one base vertex addresses both) and ONE index buffer shared by every arena. A mesh holds a
// GeometryPoolAllocation: its base vertex and first index in those buffers. Indices stay mesh-relative (a
// draw adds the base vertex as its vertexOffset), so the same allocation serves a per-mesh bind (the buffers
// bound at the mesh's byte offsets, every mesh-relative draw unchanged) and the GPU-driven bins (the buffers
// bound once at offset 0 for every mesh of the pipeline, the rows carrying FirstIndex/VertexOffset with the
// pool base, DrawCommandCache).
//
// GROWTH: an arena that has no free span large enough grows its buffer (a new, larger device buffer, the old
// contents copied on the device, the old buffer retired on the frame rule). The VertexBuffer / IndexBuffer
// OBJECT stays the same across a growth, its VkBuffer and device address change: nothing may cache a device
// address of a pool buffer across frames (the ray-tracing geometries read it when they build or describe,
// VulkanRayTracingGeometry::GetMetadataLayout), and a graph import of it re-wraps on the new handle
// (VulkanRendererAPI::ImportVertexBuffer). UE's GPU-scene buffers grow the same way (resize + copy).
//
// FREE: a released allocation's spans go back to the free list only after every frame that could still read
// them has retired (kRetireFrames frames of BeginFrame), so an upload into a reused span never overwrites
// geometry an in-flight frame draws.
namespace Desert::Graphic
{
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

    // Where a mesh's geometry sits in the pool: vertices (and streams) [BaseVertex, +VertexCount) of its arena,
    // indices [FirstIndex, +IndexCount) of the shared index buffer (IndexCount 0 = a non-indexed mesh).
    struct GeometryPoolRange
    {
        GeometryArena Arena       = GeometryArena::Static;
        uint32_t      BaseVertex  = 0;
        uint32_t      VertexCount = 0;
        uint32_t      FirstIndex  = 0;
        uint32_t      IndexCount  = 0;

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

        [[nodiscard]] bool operator==( const MeshGeometryBinding& ) const = default;
    };

    // A MESH'S GPU GEOMETRY: the buffers holding it and where its data begins in each. A pooled mesh's three are
    // the arena's (Pooled set); a twin built from GPU-written buffers holds its own, or a mix (a morphed twin's
    // own vertices over its base's pooled streams and indices).
    struct MeshGeometry
    {
        std::shared_ptr<VertexBuffer> Vertices;
        std::shared_ptr<VertexBuffer> Streams; // null = the mesh has no stream buffer: default streams
        std::shared_ptr<IndexBuffer>  Indices; // null = non-indexed
        uint64_t                      VertexByteOffset = 0;
        uint64_t                      StreamByteOffset = 0;
        uint64_t                      IndexByteOffset  = 0;
        uint32_t                      VertexStride     = 0;
        uint32_t                      VertexCount      = 0;
        uint32_t                      IndexCount       = 0;
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
            return { Vertices.get(),   VertexByteOffset, Streams.get(),
                     StreamByteOffset, Indices.get(),    IndexByteOffset };
        }
        // The shared bind (GPU-driven bins): a pooled mesh binds its arena at offset 0 and its draws add
        // SharedBaseVertex / SharedFirstIndex; an unpooled one is its own binding with a zero base.
        [[nodiscard]] MeshGeometryBinding SharedBinding() const
        {
            if ( !Pooled )
                return MeshBinding();
            return { Vertices.get(), 0, Streams.get(), 0, Indices.get(), 0 };
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

    // The pool's counters. Binds: the per-frame number of times a renderer bound geometry buffers it had not
    // bound already on that command buffer for that pipeline (GeometryPool::RecordBind), the scenario's
    // "GeometryPool.Binds" - with every scene mesh pooled it equals the pipeline switches of the binned passes,
    // not the mesh count. MeshBinds: the per-mesh binds (WalkMeshSubmeshes / RenderMeshIndirect paths), counted
    // apart so the two are never summed into one number.
    struct GeometryPoolStats
    {
        uint32_t                                  Binds       = 0;
        uint32_t                                  MeshBinds   = 0;
        uint32_t                                  Allocations = 0;
        std::array<uint64_t, kGeometryArenaCount> VertexCapacity{};
        std::array<uint64_t, kGeometryArenaCount> VerticesUsed{};
        uint64_t                                  IndexCapacity = 0;
        uint64_t                                  IndicesUsed   = 0;
        uint32_t                                  Growths       = 0;
    };

    class GeometryPool
    {
    public:
        // Frames a released span waits before it can be reused (frames in flight + the one recording).
        static constexpr uint32_t kRetireFrames = 3;

        static GeometryPool& Get();

        // Allocates the mesh's spans and uploads its data (blocking one-off copies, like the per-mesh upload it
        // replaces). @p vertices are VertexCount x GeometryArenaVertexStride( arena ) bytes; @p streams are
        // empty (default values written) or one per vertex; @p indices are mesh-relative, empty for a
        // non-indexed mesh. Refused: no vertex, a streams count other than 0 or the vertex count, a span past
        // 2^32 bytes.
        [[nodiscard]] Common::ResultStr<GeometryPoolAllocation>
        RT_Upload( GeometryArena arena, std::span<const std::byte> vertices,
                   std::span<const MeshVertexStreams> streams, std::span<const uint32_t> indices );

        // The arena's buffers (null before the first upload into it). The objects live as long as the pool.
        [[nodiscard]] std::shared_ptr<VertexBuffer> GetVertexBuffer( GeometryArena arena ) const;
        [[nodiscard]] std::shared_ptr<VertexBuffer> GetStreamBuffer( GeometryArena arena ) const;
        [[nodiscard]] std::shared_ptr<IndexBuffer>  GetIndexBuffer() const;

        // A MeshGeometry over an allocation (Pooled set, the arena's buffers, byte offsets of the span).
        [[nodiscard]] MeshGeometry Describe( const GeometryPoolAllocation& allocation,
                                             bool                          authoredStreams ) const;

        // Once per frame, before recording (VulkanRendererAPI::BeginFrame): rolls the bind counters into the
        // last frame's and returns released spans older than kRetireFrames to the free lists.
        void BeginFrame();
        // A renderer bound geometry it had not bound for this command buffer and pipeline (@p shared: the
        // binned path's shared binding; otherwise a per-mesh bind).
        void RecordBind( bool shared );
        // The last completed frame's counters and the current occupancy.
        [[nodiscard]] GeometryPoolStats GetStats() const;

        // Device teardown: drops the buffers; allocations still held by cached meshes are released with them.
        void RT_Shutdown();

    private:
        friend class GeometryPoolAllocation;
        GeometryPool() = default;

        // A first-fit span allocator in elements, with coalescing; grows by doubling.
        struct SpanList
        {
            std::vector<std::pair<uint64_t, uint64_t>> Free; // (first, count), sorted by first
            uint64_t                                   Capacity = 0;
            uint64_t                                   Used     = 0;
            [[nodiscard]] std::optional<uint64_t>      Take( uint64_t count );
            void                                       Give( uint64_t first, uint64_t count );
            void                                       Extend( uint64_t newCapacity );
        };
        struct Arena
        {
            std::shared_ptr<VertexBuffer> Vertices;
            std::shared_ptr<VertexBuffer> Streams;
            SpanList                      Spans;
        };
        struct Retired
        {
            GeometryPoolRange Range;
            uint64_t          Frame = 0;
        };

        void                                      Release( const GeometryPoolRange& range );
        [[nodiscard]] Common::ResultStr<uint64_t> RT_TakeVertices( Arena& arena, GeometryArena which,
                                                                   uint64_t count );
        [[nodiscard]] Common::ResultStr<uint64_t> RT_TakeIndices( uint64_t count );
        [[nodiscard]] Common::BoolResultStr       RT_Write( const GeometryPoolRange&           range,
                                                            std::span<const std::byte>         vertices,
                                                            std::span<const MeshVertexStreams> streams,
                                                            std::span<const uint32_t>          indices );

        mutable std::mutex                     m_Mutex;
        std::array<Arena, kGeometryArenaCount> m_Arenas;
        std::shared_ptr<IndexBuffer>           m_Indices;
        SpanList                               m_IndexSpans;
        std::vector<Retired>                   m_Retired;
        uint64_t                               m_Frame         = 0;
        uint32_t                               m_Allocations   = 0;
        uint32_t                               m_Growths       = 0;
        uint32_t                               m_Binds         = 0;
        uint32_t                               m_MeshBinds     = 0;
        uint32_t                               m_LastBinds     = 0;
        uint32_t                               m_LastMeshBinds = 0;
        bool m_ShutDown = false;
    };
} // namespace Desert::Graphic
