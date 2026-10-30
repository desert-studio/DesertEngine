#include <Engine/Graphic/GeometryPool.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <limits>
#include <numeric>
#include <utility>

namespace Desert::Graphic
{
    namespace
    {
        // A span's byte offset is bound as a storage-buffer range by the compute passes that read one mesh's
        // vertices (RDG sub-range imports: displacement, morph, skin cache, procedural BLAS); 256 is the largest
        // minStorageBufferOffsetAlignment the spec allows, so a span starting on it binds on every device.
        constexpr uint64_t kSpanByteAlignment = 256;

        // Elements per allocation granule for an element of @p stride bytes: the smallest count whose bytes
        // are a multiple of kSpanByteAlignment.
        constexpr uint64_t Granule( const uint64_t stride )
        {
            return kSpanByteAlignment / std::gcd( stride, kSpanByteAlignment );
        }
        // A vertex span starts on a granule of BOTH the arena's vertex stride and the stream stride (one base
        // vertex addresses both buffers).
        uint64_t VertexGranule( const GeometryArena arena )
        {
            return std::lcm( Granule( GeometryArenaVertexStride( arena ) ), Granule( kGeometryStreamStride ) );
        }
        constexpr uint64_t RoundUp( const uint64_t value, const uint64_t granule )
        {
            return ( value + granule - 1 ) / granule * granule;
        }

        // A page's vertex (or index) buffer: large enough that a scene binds a handful of pages per pipeline (the
        // GPU-driven bins split by page), small enough that the last, partly filled page of each arena wastes
        // little. A mesh larger than a page gets a page of its own size.
        constexpr uint64_t kPageBytes      = 64ull * 1024ull * 1024ull;
        constexpr uint64_t kMaxBufferBytes = std::numeric_limits<uint32_t>::max();
    } // namespace

    // ---------------------------------------------------------------------------------------------------------
    // GeometryPoolAllocation

    GeometryPoolAllocation::~GeometryPoolAllocation()
    {
        Reset();
    }

    GeometryPoolAllocation::GeometryPoolAllocation( GeometryPoolAllocation&& other ) noexcept
         : m_Pool( std::exchange( other.m_Pool, nullptr ) ), m_Range( other.m_Range )
    {
    }

    GeometryPoolAllocation& GeometryPoolAllocation::operator=( GeometryPoolAllocation&& other ) noexcept
    {
        if ( this != &other )
        {
            Reset();
            m_Pool  = std::exchange( other.m_Pool, nullptr );
            m_Range = other.m_Range;
        }
        return *this;
    }

    void GeometryPoolAllocation::Reset()
    {
        if ( m_Pool != nullptr )
            m_Pool->Release( m_Range );
        m_Pool  = nullptr;
        m_Range = {};
    }

    // ---------------------------------------------------------------------------------------------------------
    // SpanList

    std::optional<uint64_t> GeometryPool::SpanList::Take( const uint64_t count )
    {
        for ( auto it = Free.begin(); it != Free.end(); ++it )
        {
            if ( it->second < count )
                continue;
            const uint64_t first = it->first;
            it->first += count;
            it->second -= count;
            if ( it->second == 0 )
                Free.erase( it );
            Used += count;
            return first;
        }
        return std::nullopt;
    }

    void GeometryPool::SpanList::Give( const uint64_t first, const uint64_t count )
    {
        if ( count == 0 )
            return;
        Used -= count;
        auto it = std::lower_bound( Free.begin(), Free.end(), std::pair{ first, uint64_t{ 0 } } );
        it      = Free.insert( it, { first, count } );
        // Coalesce with the next, then with the previous.
        if ( auto next = std::next( it ); next != Free.end() && it->first + it->second == next->first )
        {
            it->second += next->second;
            Free.erase( next );
        }
        if ( it != Free.begin() )
        {
            auto prev = std::prev( it );
            if ( prev->first + prev->second == it->first )
            {
                prev->second += it->second;
                Free.erase( it );
            }
        }
    }

    void GeometryPool::SpanList::Extend( const uint64_t newCapacity )
    {
        if ( newCapacity <= Capacity )
            return;
        const uint64_t added = newCapacity - Capacity;
        const uint64_t first = Capacity;
        Capacity             = newCapacity;
        Used += added; // Give takes it back off
        Give( first, added );
    }

    // ---------------------------------------------------------------------------------------------------------
    // GeometryPool

    GeometryPool& GeometryPool::Get()
    {
        static GeometryPool pool;
        return pool;
    }

    namespace
    {
        // Creates a page's Pooled vertex buffer of @p bytes.
        Common::ResultStr<std::shared_ptr<VertexBuffer>> RT_CreateVertexPage( const uint64_t bytes,
                                                                               const char*    what )
        {
            if ( bytes > kMaxBufferBytes )
                return Common::MakeFormattedError<std::shared_ptr<VertexBuffer>>(
                     "GeometryPool: a {} page would pass 2^32 bytes ({} bytes)", what, bytes );
            auto buffer = VertexBuffer::Create( static_cast<uint32_t>( bytes ), BufferUsage::Pooled );
            buffer->ClaimOwnership( ResourceOwner::AssetService );
            if ( const auto built = buffer->RT_Invalidate(); !built.IsSuccess() )
                return Common::MakeFormattedError<std::shared_ptr<VertexBuffer>>(
                     "GeometryPool: a {} page was not created: {}", what, built.GetError() );
            buffer->RecordDeviceBytes( static_cast<std::size_t>( bytes ) );
            return Common::MakeSuccess( std::move( buffer ) );
        }
    } // namespace

    namespace
    {
        constexpr const char* ArenaName( const GeometryArena which )
        {
            return which == GeometryArena::Static ? "static" : "skinned";
        }
        constexpr double ToMiB( const uint64_t bytes )
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }
    } // namespace

    Common::ResultStr<uint32_t> GeometryPool::RT_AddVertexPage( Arena& arena, const GeometryArena which,
                                                                const uint64_t capacity, const char* reason )
    {
        const uint64_t stride = GeometryArenaVertexStride( which );
        auto vertices = RT_CreateVertexPage( capacity * stride, which == GeometryArena::Static ? "static vertex"
                                                                                               : "skinned vertex" );
        if ( !vertices.IsSuccess() )
            return Common::MakeError<uint32_t>( vertices.GetError() );
        auto streams = RT_CreateVertexPage( capacity * kGeometryStreamStride,
                                            which == GeometryArena::Static ? "static stream" : "skinned stream" );
        if ( !streams.IsSuccess() )
            return Common::MakeError<uint32_t>( streams.GetError() );
        VertexPage page;
        page.Vertices = std::move( vertices.GetValue() );
        page.Streams  = std::move( streams.GetValue() );
        page.Spans.Extend( capacity );
        // A slot a released page left keeps every other page's index (allocations address pages by index).
        auto     slot  = std::find_if( arena.Pages.begin(), arena.Pages.end(),
                                       []( const VertexPage& p ) { return p.Vertices == nullptr; } );
        uint32_t index = 0;
        if ( slot != arena.Pages.end() )
        {
            index = static_cast<uint32_t>( slot - arena.Pages.begin() );
            *slot = std::move( page );
        }
        else
        {
            index = static_cast<uint32_t>( arena.Pages.size() );
            arena.Pages.push_back( std::move( page ) );
        }
        LOG_INFO( "[GeometryPool] page +{} {} vertex page #{}: {} vertices, {:.1f} MiB vertex + {:.1f} MiB stream "
                  "(no copy)",
                  reason, ArenaName( which ), index, capacity, ToMiB( capacity * stride ),
                  ToMiB( capacity * kGeometryStreamStride ) );
        return Common::MakeSuccess( index );
    }

    Common::ResultStr<uint32_t> GeometryPool::RT_AddIndexPage( const uint64_t capacity, const char* reason )
    {
        const uint64_t bytes = capacity * kGeometryIndexStride;
        if ( bytes > kMaxBufferBytes )
            return Common::MakeFormattedError<uint32_t>( "GeometryPool: an index page would pass 2^32 bytes ({} bytes)",
                                                         bytes );
        IndexPage page;
        page.Indices = IndexBuffer::Create( static_cast<uint32_t>( bytes ), BufferUsage::Pooled );
        page.Indices->ClaimOwnership( ResourceOwner::AssetService );
        if ( const auto built = page.Indices->RT_Invalidate(); !built.IsSuccess() )
            return Common::MakeFormattedError<uint32_t>( "GeometryPool: an index page was not created: {}",
                                                         built.GetError() );
        page.Indices->RecordDeviceBytes( static_cast<std::size_t>( bytes ) );
        page.Spans.Extend( capacity );
        auto     slot  = std::find_if( m_IndexPages.begin(), m_IndexPages.end(),
                                       []( const IndexPage& p ) { return p.Indices == nullptr; } );
        uint32_t index = 0;
        if ( slot != m_IndexPages.end() )
        {
            index = static_cast<uint32_t>( slot - m_IndexPages.begin() );
            *slot = std::move( page );
        }
        else
        {
            index = static_cast<uint32_t>( m_IndexPages.size() );
            m_IndexPages.push_back( std::move( page ) );
        }
        LOG_INFO( "[GeometryPool] page +{} index page #{}: {} indices, {:.1f} MiB (no copy)", reason, index, capacity,
                  ToMiB( bytes ) );
        return Common::MakeSuccess( index );
    }

    Common::ResultStr<GeometryPool::PageSpan> GeometryPool::RT_TakeVertices( Arena& arena, const GeometryArena which,
                                                                             const uint64_t count )
    {
        for ( std::size_t page = 0; page < arena.Pages.size(); ++page )
            if ( const auto first = arena.Pages[page].Spans.Take( count ) )
                return Common::MakeSuccess( PageSpan{ static_cast<uint32_t>( page ), *first } );
        const uint64_t capacity =
             RoundUp( std::max( kPageBytes / GeometryArenaVertexStride( which ), count ), VertexGranule( which ) );
        const auto added = RT_AddVertexPage( arena, which, capacity, "load" );
        if ( !added.IsSuccess() )
            return Common::MakeError<PageSpan>( added.GetError() );
        const auto first = arena.Pages[added.GetValue()].Spans.Take( count );
        if ( !first )
            return Common::MakeFormattedError<PageSpan>( "GeometryPool: {} vertices do not fit a page of {}", count,
                                                         capacity );
        return Common::MakeSuccess( PageSpan{ added.GetValue(), *first } );
    }

    Common::ResultStr<GeometryPool::PageSpan> GeometryPool::RT_TakeIndices( const uint64_t count )
    {
        for ( std::size_t page = 0; page < m_IndexPages.size(); ++page )
            if ( const auto first = m_IndexPages[page].Spans.Take( count ) )
                return Common::MakeSuccess( PageSpan{ static_cast<uint32_t>( page ), *first } );
        const uint64_t capacity =
             RoundUp( std::max( kPageBytes / kGeometryIndexStride, count ), Granule( kGeometryIndexStride ) );
        const auto added = RT_AddIndexPage( capacity, "load" );
        if ( !added.IsSuccess() )
            return Common::MakeError<PageSpan>( added.GetError() );
        const auto first = m_IndexPages[added.GetValue()].Spans.Take( count );
        if ( !first )
            return Common::MakeFormattedError<PageSpan>( "GeometryPool: {} indices do not fit a page of {}", count,
                                                         capacity );
        return Common::MakeSuccess( PageSpan{ added.GetValue(), *first } );
    }

    void GeometryPool::GiveBack( const GeometryPoolRange& range )
    {
        m_Arenas[static_cast<std::size_t>( range.Arena )].Pages[range.VertexPage].Spans.Give(
             range.BaseVertex, RoundUp( range.VertexCount, VertexGranule( range.Arena ) ) );
        if ( range.IndexSpanCount() != 0 )
            m_IndexPages[range.IndexPage].Spans.Give(
                 range.FirstIndex, RoundUp( range.IndexSpanCount(), Granule( kGeometryIndexStride ) ) );
    }

    Common::BoolResultStr GeometryPool::RT_Write( const GeometryPoolRange&           range,
                                                  std::span<const std::byte>         vertices,
                                                  std::span<const MeshVertexStreams> streams,
                                                  std::span<const uint32_t>          indices,
                                                  std::span<const uint32_t>          clusterWords )
    {
        const VertexPage& page   = m_Arenas[static_cast<std::size_t>( range.Arena )].Pages[range.VertexPage];
        const uint64_t    stride = GeometryArenaVertexStride( range.Arena );
        if ( const auto wrote = page.Vertices->RT_WriteRange( uint64_t{ range.BaseVertex } * stride,
                                                               vertices.data(), vertices.size() );
             !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "GeometryPool: vertex upload: {}", wrote.GetError() );

        // A mesh without streams gets the defaults (white, UV1 0,0) in its span: every pooled vertex has a stream.
        std::vector<MeshVertexStreams> defaults;
        if ( streams.empty() )
        {
            defaults.resize( range.VertexCount );
            streams = defaults;
        }
        if ( const auto wrote = page.Streams->RT_WriteRange( uint64_t{ range.BaseVertex } * kGeometryStreamStride,
                                                              streams.data(), streams.size_bytes() );
             !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "GeometryPool: stream upload: {}", wrote.GetError() );

        if ( !indices.empty() )
            if ( const auto wrote = m_IndexPages[range.IndexPage].Indices->RT_WriteRange( uint64_t{ range.FirstIndex } * kGeometryIndexStride,
                                                              indices.data(), indices.size_bytes() );
                 !wrote.IsSuccess() )
                return Common::MakeFormattedError<bool>( "GeometryPool: index upload: {}", wrote.GetError() );
        if ( !clusterWords.empty() )
            if ( const auto wrote = m_IndexPages[range.IndexPage].Indices->RT_WriteRange(
                      ( uint64_t{ range.FirstIndex } + range.IndexCount ) * kGeometryIndexStride,
                      clusterWords.data(), clusterWords.size_bytes() );
                 !wrote.IsSuccess() )
                return Common::MakeFormattedError<bool>( "GeometryPool: cluster word upload: {}",
                                                         wrote.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<GeometryPoolAllocation> GeometryPool::RT_Upload( const GeometryArena                arena,
                                                                       std::span<const std::byte>         vertices,
                                                                       std::span<const MeshVertexStreams> streams,
                                                                       std::span<const uint32_t>          indices,
                                                                       std::span<const uint32_t> clusterWords )
    {
        const uint64_t stride = GeometryArenaVertexStride( arena );
        if ( vertices.empty() || vertices.size() % stride != 0 )
            return Common::MakeFormattedError<GeometryPoolAllocation>(
                 "GeometryPool: {} vertex bytes are not a whole number of {}-byte vertices", vertices.size(),
                 stride );
        const uint64_t vertexCount = vertices.size() / stride;
        if ( !streams.empty() && streams.size() != vertexCount )
            return Common::MakeFormattedError<GeometryPoolAllocation>(
                 "GeometryPool: {} streams for {} vertices (a stream per vertex, or none)", streams.size(),
                 vertexCount );
        if ( !clusterWords.empty() && indices.empty() )
            return Common::MakeFormattedError<GeometryPoolAllocation>(
                 "GeometryPool: {} cluster words without indices (they live in the index span)",
                 clusterWords.size() );
        if ( indices.size() + clusterWords.size() > std::numeric_limits<uint32_t>::max() )
            return Common::MakeFormattedError<GeometryPoolAllocation>(
                 "GeometryPool: {} indices + {} cluster words pass 2^32 elements", indices.size(),
                 clusterWords.size() );

        std::lock_guard lock( m_Mutex );
        Arena&          target     = m_Arenas[static_cast<std::size_t>( arena )];
        const uint64_t  vertexSpan = RoundUp( vertexCount, VertexGranule( arena ) );
        const auto      base       = RT_TakeVertices( target, arena, vertexSpan );
        if ( !base.IsSuccess() )
            return Common::MakeError<GeometryPoolAllocation>( base.GetError() );

        GeometryPoolRange range;
        range.Arena       = arena;
        range.VertexPage  = base.GetValue().Page;
        range.BaseVertex  = static_cast<uint32_t>( base.GetValue().First );
        range.VertexCount = static_cast<uint32_t>( vertexCount );
        if ( !indices.empty() )
        {
            const auto first = RT_TakeIndices(
                 RoundUp( indices.size() + clusterWords.size(), Granule( kGeometryIndexStride ) ) );
            if ( !first.IsSuccess() )
            {
                target.Pages[range.VertexPage].Spans.Give( range.BaseVertex, vertexSpan );
                return Common::MakeError<GeometryPoolAllocation>( first.GetError() );
            }
            range.IndexPage  = first.GetValue().Page;
            range.FirstIndex = static_cast<uint32_t>( first.GetValue().First );
            range.IndexCount       = static_cast<uint32_t>( indices.size() );
            range.ClusterWordCount = static_cast<uint32_t>( clusterWords.size() );
        }
        if ( const auto wrote = RT_Write( range, vertices, streams, indices, clusterWords ); !wrote.IsSuccess() )
        {
            GiveBack( range );
            return Common::MakeError<GeometryPoolAllocation>( wrote.GetError() );
        }
        ++m_Allocations;
        return Common::MakeSuccess( GeometryPoolAllocation( this, range ) );
    }

    Common::BoolResultStr GeometryPool::RT_Reserve( const std::string_view                           scene,
                                                    const std::array<uint64_t, kGeometryArenaCount>& vertices,
                                                    const uint64_t                                   indices )
    {
        std::lock_guard lock( m_Mutex );
        // Pages emptied by the scene that is closing go first, so the reservation never sits beside them.
        ReleaseEmptyPages();
        std::string summary;
        uint32_t    added = 0;
        for ( std::size_t i = 0; i < kGeometryArenaCount; ++i )
        {
            const auto     which   = static_cast<GeometryArena>( i );
            const uint64_t granule = VertexGranule( which );
            const uint64_t want    = RoundUp( vertices[i] + vertices[i] / 8, granule );
            uint64_t       free    = 0;
            for ( const VertexPage& page : m_Arenas[i].Pages )
                free += page.Spans.Capacity - page.Spans.Used;
            const uint64_t stride  = GeometryArenaVertexStride( which );
            const uint64_t perPage = ( kMaxBufferBytes / std::max<uint64_t>( stride, kGeometryStreamStride ) ) /
                                     granule * granule;
            uint64_t missing = want > free ? want - free : 0;
            if ( missing != 0 )
                missing = std::max( RoundUp( missing, granule ), RoundUp( kPageBytes / stride, granule ) );
            std::format_to( std::back_inserter( summary ), "{}{} {} vertices ({:.1f} MiB, free {})",
                            summary.empty() ? "" : " + ", ArenaName( which ), vertices[i],
                            ToMiB( vertices[i] * ( stride + kGeometryStreamStride ) ), free );
            while ( missing != 0 )
            {
                const uint64_t capacity = std::min( missing, perPage );
                const auto page = RT_AddVertexPage( m_Arenas[i], which, capacity, "reserve" );
                if ( !page.IsSuccess() )
                    return Common::MakeError<bool>( page.GetError() );
                m_Arenas[i].Pages[page.GetValue()].Reserved = true;
                missing -= capacity;
                ++added;
            }
        }
        {
            const uint64_t granule = Granule( kGeometryIndexStride );
            const uint64_t want    = RoundUp( indices + indices / 8, granule );
            uint64_t       free    = 0;
            for ( const IndexPage& page : m_IndexPages )
                free += page.Spans.Capacity - page.Spans.Used;
            const uint64_t perPage = kMaxBufferBytes / kGeometryIndexStride / granule * granule;
            uint64_t       missing = want > free ? want - free : 0;
            if ( missing != 0 )
                missing = std::max( RoundUp( missing, granule ), RoundUp( kPageBytes / kGeometryIndexStride, granule ) );
            std::format_to( std::back_inserter( summary ), " + {} indices ({:.1f} MiB, free {})", indices,
                            ToMiB( indices * kGeometryIndexStride ), free );
            while ( missing != 0 )
            {
                const uint64_t capacity = std::min( missing, perPage );
                const auto page = RT_AddIndexPage( capacity, "reserve" );
                if ( !page.IsSuccess() )
                    return Common::MakeError<bool>( page.GetError() );
                m_IndexPages[page.GetValue()].Reserved = true;
                missing -= capacity;
                ++added;
            }
        }
        LOG_INFO( "[GeometryPool] RT_Reserve {}: {} = sum of meshes + 1/8, {} new page(s)", scene, summary, added );
        return Common::MakeSuccess( true );
    }

    void GeometryPool::RequestReleaseEmptyPages()
    {
        std::lock_guard lock( m_Mutex );
        if ( m_ShutDown )
            return;
        // The closing scene's reservation is spent: its pages are ordinary from here on.
        for ( Arena& arena : m_Arenas )
            for ( VertexPage& page : arena.Pages )
                page.Reserved = false;
        for ( IndexPage& page : m_IndexPages )
            page.Reserved = false;
        // The window covers the dropped meshes' own retire queue and then the pool's retire margin.
        m_ReleaseEmptyUntil = m_Frame + 3 * kRetireFrames;
        ReleaseEmptyPages();
    }

    void GeometryPool::ReleaseEmptyPages()
    {
        uint32_t freed = 0;
        uint64_t bytes = 0;
        for ( std::size_t i = 0; i < kGeometryArenaCount; ++i )
        {
            auto& pages = m_Arenas[i].Pages;
            for ( VertexPage& page : pages )
                if ( page.Vertices != nullptr && !page.Reserved && page.Spans.Used == 0 )
                {
                    bytes += page.Spans.Capacity * ( GeometryArenaVertexStride( static_cast<GeometryArena>( i ) ) +
                                                     kGeometryStreamStride );
                    page = VertexPage{}; // the slot stays: allocations address pages by index
                    ++freed;
                }
            while ( !pages.empty() && pages.back().Vertices == nullptr )
                pages.pop_back();
        }
        for ( IndexPage& page : m_IndexPages )
            if ( page.Indices != nullptr && !page.Reserved && page.Spans.Used == 0 )
            {
                bytes += page.Spans.Capacity * kGeometryIndexStride;
                page = IndexPage{};
                ++freed;
            }
        while ( !m_IndexPages.empty() && m_IndexPages.back().Indices == nullptr )
            m_IndexPages.pop_back();
        if ( freed != 0 )
            LOG_INFO( "[GeometryPool] released {} empty page(s), {:.1f} MiB returned", freed, ToMiB( bytes ) );
    }

    MeshGeometry GeometryPool::Describe( const GeometryPoolAllocation& allocation,
                                         const bool                    authoredStreams ) const
    {
        MeshGeometry geometry;
        if ( !allocation )
            return geometry;
        const GeometryPoolRange& range = allocation.Range();
        std::lock_guard          lock( m_Mutex );
        const VertexPage&        page  = m_Arenas[static_cast<std::size_t>( range.Arena )].Pages[range.VertexPage];
        geometry.Vertices              = page.Vertices;
        geometry.Streams               = page.Streams;
        geometry.Indices = range.IndexSpanCount() != 0 ? m_IndexPages[range.IndexPage].Indices : nullptr;
        geometry.VertexStride          = GeometryArenaVertexStride( range.Arena );
        geometry.VertexByteOffset      = uint64_t{ range.BaseVertex } * geometry.VertexStride;
        geometry.StreamByteOffset      = uint64_t{ range.BaseVertex } * kGeometryStreamStride;
        geometry.IndexByteOffset       = uint64_t{ range.FirstIndex } * kGeometryIndexStride;
        geometry.VertexCount           = range.VertexCount;
        geometry.IndexCount            = range.IndexCount;
        geometry.ClusterWordCount      = range.ClusterWordCount;
        geometry.AuthoredStreams       = authoredStreams;
        geometry.Pooled                = range;
        return geometry;
    }

    void GeometryPool::Release( const GeometryPoolRange& range )
    {
        std::lock_guard lock( m_Mutex );
        if ( !m_ShutDown )
            m_Retired.push_back( { range, m_Frame } );
    }

    void GeometryPool::BeginFrame()
    {
        std::lock_guard lock( m_Mutex );
        ++m_Frame;
        m_LastBinds     = std::exchange( m_Binds, 0u );
        m_LastMeshBinds = std::exchange( m_MeshBinds, 0u );
        std::erase_if( m_Retired,
                       [&]( const Retired& retired )
                       {
                           if ( retired.Frame + kRetireFrames > m_Frame )
                               return false;
                           GiveBack( retired.Range );
                           --m_Allocations;
                           return true;
                       } );
        // A release request stays armed for its window and then until every retiring span is back.
        if ( m_ReleaseEmptyUntil != 0 )
        {
            ReleaseEmptyPages();
            if ( m_Frame >= m_ReleaseEmptyUntil && m_Retired.empty() )
                m_ReleaseEmptyUntil = 0;
        }
    }

    void GeometryPool::RecordBind( const bool shared )
    {
        std::lock_guard lock( m_Mutex );
        ++( shared ? m_Binds : m_MeshBinds );
    }

    GeometryPoolStats GeometryPool::GetStats() const
    {
        std::lock_guard   lock( m_Mutex );
        GeometryPoolStats stats;
        stats.Binds       = m_LastBinds;
        stats.MeshBinds   = m_LastMeshBinds;
        stats.Allocations = m_Allocations;
        for ( std::size_t i = 0; i < kGeometryArenaCount; ++i )
            for ( const VertexPage& page : m_Arenas[i].Pages )
            {
                if ( page.Vertices == nullptr )
                    continue;
                stats.VertexCapacity[i] += page.Spans.Capacity;
                stats.VerticesUsed[i] += page.Spans.Used;
                ++stats.Pages;
            }
        for ( const IndexPage& page : m_IndexPages )
        {
            if ( page.Indices == nullptr )
                continue;
            stats.IndexCapacity += page.Spans.Capacity;
            stats.IndicesUsed += page.Spans.Used;
            ++stats.Pages;
        }
        return stats;
    }

    void GeometryPool::RT_Shutdown()
    {
        std::lock_guard lock( m_Mutex );
        // Meshes cached by the asset system outlive the device at exit; the arenas own the memory, so
        // dropping them frees every span at once and a later Free() is a no-op (m_ShutDown).
        if ( m_Allocations != 0 )
            LOG_INFO( "[GeometryPool] device teardown releases {} allocation(s) still held by cached meshes",
                      m_Allocations );
        for ( Arena& arena : m_Arenas )
            arena = Arena{};
        m_IndexPages.clear();
        m_Retired.clear();
        m_ReleaseEmptyUntil = 0;
        m_Allocations = 0;
        m_ShutDown    = true;
    }
} // namespace Desert::Graphic
