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
#include <string_view>
#include <vector>

// THE GEOMETRY POOL (UE: the GPU scene's shared geometry buffers that one bind serves for many meshes; Nanite's
// streaming pool is the same shape). Every mesh's vertices, optional streams and indices live in shared device
// buffers: per vertex layout (an arena: Static = Vertex, Skinned = SkinnedVertex) a list of vertex PAGES, each
// one vertex buffer and one stream buffer laid out in parallel (vertex i of a page's vertex buffer and stream i
// of its stream buffer are the same vertex, so one base vertex addresses both), and a list of index pages shared
// by every arena. A mesh holds a GeometryPoolAllocation: its vertex page and base vertex, its index page and
// first index. Indices stay mesh-relative (a draw adds the base vertex as its vertexOffset), so the same
// allocation serves a per-mesh bind (the buffers bound at the mesh's byte offsets, every mesh-relative draw
// unchanged) and the GPU-driven bins (a page's buffers bound once at offset 0 for every mesh of the pipeline in
// that page, the rows carrying FirstIndex/VertexOffset with the page base, DrawCommandCache groups by binding).
//
// GROWTH: an arena with no free span large enough in any page ADDS a page (kPageBytes, or the mesh's own size
// when it is larger); nothing is copied and no buffer is ever reallocated, so the pool's device memory is the
// sum of its pages at every moment - never an old and a new buffer at once (a resize-and-copy pool peaks at
// three times its contents while it doubles; FIELD-MEM: 406 + 728 MB transient on FIELD_Barn). A mesh's
// buffers and device addresses therefore never change while it is resident.
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
            return { Vertices.get(),   VertexByteOffset, Streams.get(),
                     StreamByteOffset, Indices.get(),    IndexByteOffset };
        }
        // The shared bind (GPU-driven bins): a pooled mesh binds its pages at offset 0 and its draws add
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
        uint32_t                                  Pages         = 0; // vertex pages of every arena + index pages
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
        // non-indexed mesh; @p clusterWords (a virtual-geometry mesh's DAG VertexRefs + Triangles) are written
        // right after the indices in the same index span. Refused: no vertex, a streams count other than 0 or the
        // vertex count, cluster words without indices, a span past 2^32 bytes.
        [[nodiscard]] Common::ResultStr<GeometryPoolAllocation>
        RT_Upload( GeometryArena arena, std::span<const std::byte> vertices,
                   std::span<const MeshVertexStreams> streams, std::span<const uint32_t> indices,
                   std::span<const uint32_t> clusterWords = {} );

        // Scene open (FIELD-MEM): one page per arena (and one index page) sized to the scene's meshes plus an
        // eighth, so the meshes the scene then streams in add no page of their own (UE: the pool is sized for
        // the level up front). @p vertices per arena and @p indices are the sums over the scene's mesh closure;
        // a zero sum adds nothing. Free room the pool already has counts against the sum; a page never passes
        // 2^32 bytes (a larger sum takes several). Logs one line "[GeometryPool] RT_Reserve ...".
        [[nodiscard]] Common::BoolResultStr RT_Reserve( std::string_view                                 scene,
                                                        const std::array<uint64_t, kGeometryArenaCount>& vertices,
                                                        uint64_t                                         indices );
        // Scene change: frees every page no allocation holds, live or retiring (the pool's device memory goes
        // back, as a UE pool budget gives it back; the pool does not only grow). Released spans still waiting
        // for in-flight frames keep their page; BeginFrame frees it once they retire (the request stays armed
        // for 3 x kRetireFrames frames, then until no span is retiring). Logs "[GeometryPool] released ..." when a page is freed.
        void RequestReleaseEmptyPages();

        // A MeshGeometry over an allocation (Pooled set, its pages' buffers, byte offsets of the span).
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

        // A first-fit span allocator in elements, with coalescing, over one page.
        struct SpanList
        {
            std::vector<std::pair<uint64_t, uint64_t>> Free; // (first, count), sorted by first
            uint64_t                                   Capacity = 0;
            uint64_t                                   Used     = 0;
            [[nodiscard]] std::optional<uint64_t>      Take( uint64_t count );
            void                                       Give( uint64_t first, uint64_t count );
            void                                       Extend( uint64_t newCapacity );
        };
        struct VertexPage
        {
            std::shared_ptr<VertexBuffer> Vertices;
            std::shared_ptr<VertexBuffer> Streams;
            SpanList                      Spans;
            bool                          Reserved = false; // RT_Reserve's page: kept empty until the next scene change
        };
        struct IndexPage
        {
            std::shared_ptr<IndexBuffer> Indices;
            SpanList                     Spans;
            bool                         Reserved = false;
        };
        struct Arena
        {
            std::vector<VertexPage> Pages;
        };
        // A span taken from a page: the page's index and the span's first element in it.
        struct PageSpan
        {
            uint32_t Page  = 0;
            uint64_t First = 0;
        };
        struct Retired
        {
            GeometryPoolRange Range;
            uint64_t          Frame = 0;
        };

        [[nodiscard]] Common::ResultStr<uint32_t> RT_AddVertexPage( Arena& arena, GeometryArena which,
                                                                    uint64_t capacity, const char* reason );
        [[nodiscard]] Common::ResultStr<uint32_t> RT_AddIndexPage( uint64_t capacity, const char* reason );
        void                                      ReleaseEmptyPages();
        void                                      Release( const GeometryPoolRange& range );
        [[nodiscard]] Common::ResultStr<PageSpan> RT_TakeVertices( Arena& arena, GeometryArena which,
                                                                   uint64_t count );
        [[nodiscard]] Common::ResultStr<PageSpan> RT_TakeIndices( uint64_t count );
        void                                      GiveBack( const GeometryPoolRange& range );
        [[nodiscard]] Common::BoolResultStr       RT_Write( const GeometryPoolRange&           range,
                                                            std::span<const std::byte>         vertices,
                                                            std::span<const MeshVertexStreams> streams,
                                                            std::span<const uint32_t>          indices,
                                                            std::span<const uint32_t>          clusterWords );

        mutable std::mutex                     m_Mutex;
        std::array<Arena, kGeometryArenaCount> m_Arenas;
        std::vector<IndexPage>                 m_IndexPages;
        std::vector<Retired>                   m_Retired;
        uint64_t                               m_Frame         = 0;
        uint32_t                               m_Allocations   = 0;
        uint32_t                               m_Binds         = 0;
        uint32_t                               m_MeshBinds     = 0;
        uint32_t                               m_LastBinds     = 0;
        uint32_t                               m_LastMeshBinds = 0;
        uint64_t                               m_ReleaseEmptyUntil = 0; // 0 = no request armed
        bool m_ShutDown = false;
    };
} // namespace Desert::Graphic
