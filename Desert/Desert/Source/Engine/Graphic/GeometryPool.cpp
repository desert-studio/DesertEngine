#include <Engine/Graphic/GeometryPool.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <format>
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

        constexpr uint64_t kInitialVertices = 256ull * 1024ull;
        constexpr uint64_t kInitialIndices  = 1024ull * 1024ull;
        constexpr uint64_t kMaxBufferBytes  = std::numeric_limits<uint32_t>::max();
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
        // Creates (first use) or grows a Pooled vertex buffer to @p bytes.
        Common::BoolResultStr RT_SizeVertexBuffer( std::shared_ptr<VertexBuffer>& buffer, const uint64_t bytes,
                                                   const char* what )
        {
            if ( bytes > kMaxBufferBytes )
                return Common::MakeFormattedError<bool>(
                     "GeometryPool: the {} buffer would pass 2^32 bytes ({} bytes)", what, bytes );
            if ( buffer == nullptr )
            {
                buffer = VertexBuffer::Create( static_cast<uint32_t>( bytes ), BufferUsage::Pooled );
                buffer->ClaimOwnership( ResourceOwner::AssetService );
                const auto built = buffer->RT_Invalidate();
                if ( !built.IsSuccess() )
                {
                    buffer.reset();
                    return Common::MakeFormattedError<bool>( "GeometryPool: the {} buffer was not created: {}",
                                                             what, built.GetError() );
                }
            }
            else if ( const auto grown = buffer->RT_Grow( static_cast<uint32_t>( bytes ) ); !grown.IsSuccess() )
                return Common::MakeFormattedError<bool>(
                     "GeometryPool: the {} buffer did not grow to {} bytes: {}", what, bytes, grown.GetError() );
            buffer->RecordDeviceBytes( static_cast<std::size_t>( bytes ) );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<uint64_t> GeometryPool::RT_TakeVertices( Arena& arena, const GeometryArena which,
                                                               const uint64_t count )
    {
        if ( const auto first = arena.Spans.Take( count ) )
            return Common::MakeSuccess( *first );
        const uint64_t granule     = VertexGranule( which );
        const uint64_t newCapacity = RoundUp(
             std::max( { kInitialVertices, arena.Spans.Capacity * 2, arena.Spans.Capacity + count } ), granule );
        if ( const auto sized =
                  RT_SizeVertexBuffer( arena.Vertices, newCapacity * GeometryArenaVertexStride( which ),
                                       which == GeometryArena::Static ? "static vertex" : "skinned vertex" );
             !sized.IsSuccess() )
            return Common::MakeError<uint64_t>( sized.GetError() );
        if ( const auto sized =
                  RT_SizeVertexBuffer( arena.Streams, newCapacity * kGeometryStreamStride,
                                       which == GeometryArena::Static ? "static stream" : "skinned stream" );
             !sized.IsSuccess() )
            return Common::MakeError<uint64_t>( sized.GetError() );
        if ( arena.Spans.Capacity != 0 )
            ++m_Growths;
        arena.Spans.Extend( newCapacity );
        const auto first = arena.Spans.Take( count );
        if ( !first )
            return Common::MakeFormattedError<uint64_t>(
                 "GeometryPool: {} vertices do not fit an arena grown to {}", count, newCapacity );
        return Common::MakeSuccess( *first );
    }

    Common::ResultStr<uint64_t> GeometryPool::RT_TakeIndices( const uint64_t count )
    {
        if ( const auto first = m_IndexSpans.Take( count ) )
            return Common::MakeSuccess( *first );
        const uint64_t granule     = Granule( kGeometryIndexStride );
        const uint64_t newCapacity = RoundUp(
             std::max( { kInitialIndices, m_IndexSpans.Capacity * 2, m_IndexSpans.Capacity + count } ), granule );
        const uint64_t bytes = newCapacity * kGeometryIndexStride;
        if ( bytes > kMaxBufferBytes )
            return Common::MakeFormattedError<uint64_t>(
                 "GeometryPool: the index buffer would pass 2^32 bytes ({} bytes)", bytes );
        if ( m_Indices == nullptr )
        {
            m_Indices = IndexBuffer::Create( static_cast<uint32_t>( bytes ), BufferUsage::Pooled );
            m_Indices->ClaimOwnership( ResourceOwner::AssetService );
            if ( const auto built = m_Indices->RT_Invalidate(); !built.IsSuccess() )
            {
                m_Indices.reset();
                return Common::MakeFormattedError<uint64_t>( "GeometryPool: the index buffer was not created: {}",
                                                             built.GetError() );
            }
        }
        else if ( const auto grown = m_Indices->RT_Grow( static_cast<uint32_t>( bytes ) ); !grown.IsSuccess() )
            return Common::MakeFormattedError<uint64_t>(
                 "GeometryPool: the index buffer did not grow to {} bytes: {}", bytes, grown.GetError() );
        m_Indices->RecordDeviceBytes( static_cast<std::size_t>( bytes ) );
        if ( m_IndexSpans.Capacity != 0 )
            ++m_Growths;
        m_IndexSpans.Extend( newCapacity );
        const auto first = m_IndexSpans.Take( count );
        if ( !first )
            return Common::MakeFormattedError<uint64_t>(
                 "GeometryPool: {} indices do not fit an index buffer grown to {}", count, newCapacity );
        return Common::MakeSuccess( *first );
    }

    Common::BoolResultStr GeometryPool::RT_Write( const GeometryPoolRange&           range,
                                                  std::span<const std::byte>         vertices,
                                                  std::span<const MeshVertexStreams> streams,
                                                  std::span<const uint32_t>          indices )
    {
        Arena&         arena  = m_Arenas[static_cast<std::size_t>( range.Arena )];
        const uint64_t stride = GeometryArenaVertexStride( range.Arena );
        if ( const auto wrote = arena.Vertices->RT_WriteRange( uint64_t{ range.BaseVertex } * stride,
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
        if ( const auto wrote = arena.Streams->RT_WriteRange( uint64_t{ range.BaseVertex } * kGeometryStreamStride,
                                                              streams.data(), streams.size_bytes() );
             !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "GeometryPool: stream upload: {}", wrote.GetError() );

        if ( !indices.empty() )
            if ( const auto wrote = m_Indices->RT_WriteRange( uint64_t{ range.FirstIndex } * kGeometryIndexStride,
                                                              indices.data(), indices.size_bytes() );
                 !wrote.IsSuccess() )
                return Common::MakeFormattedError<bool>( "GeometryPool: index upload: {}", wrote.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<GeometryPoolAllocation> GeometryPool::RT_Upload( const GeometryArena                arena,
                                                                       std::span<const std::byte>         vertices,
                                                                       std::span<const MeshVertexStreams> streams,
                                                                       std::span<const uint32_t>          indices )
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

        std::lock_guard lock( m_Mutex );
        Arena&          target     = m_Arenas[static_cast<std::size_t>( arena )];
        const uint64_t  vertexSpan = RoundUp( vertexCount, VertexGranule( arena ) );
        const auto      base       = RT_TakeVertices( target, arena, vertexSpan );
        if ( !base.IsSuccess() )
            return Common::MakeError<GeometryPoolAllocation>( base.GetError() );

        GeometryPoolRange range;
        range.Arena       = arena;
        range.BaseVertex  = static_cast<uint32_t>( base.GetValue() );
        range.VertexCount = static_cast<uint32_t>( vertexCount );
        if ( !indices.empty() )
        {
            const auto first = RT_TakeIndices( RoundUp( indices.size(), Granule( kGeometryIndexStride ) ) );
            if ( !first.IsSuccess() )
            {
                target.Spans.Give( base.GetValue(), vertexSpan );
                return Common::MakeError<GeometryPoolAllocation>( first.GetError() );
            }
            range.FirstIndex = static_cast<uint32_t>( first.GetValue() );
            range.IndexCount = static_cast<uint32_t>( indices.size() );
        }
        if ( const auto wrote = RT_Write( range, vertices, streams, indices ); !wrote.IsSuccess() )
        {
            target.Spans.Give( base.GetValue(), vertexSpan );
            if ( range.IndexCount != 0 )
                m_IndexSpans.Give( range.FirstIndex,
                                   RoundUp( range.IndexCount, Granule( kGeometryIndexStride ) ) );
            return Common::MakeError<GeometryPoolAllocation>( wrote.GetError() );
        }
        ++m_Allocations;
        return Common::MakeSuccess( GeometryPoolAllocation( this, range ) );
    }

    std::shared_ptr<VertexBuffer> GeometryPool::GetVertexBuffer( const GeometryArena arena ) const
    {
        std::lock_guard lock( m_Mutex );
        return m_Arenas[static_cast<std::size_t>( arena )].Vertices;
    }

    std::shared_ptr<VertexBuffer> GeometryPool::GetStreamBuffer( const GeometryArena arena ) const
    {
        std::lock_guard lock( m_Mutex );
        return m_Arenas[static_cast<std::size_t>( arena )].Streams;
    }

    std::shared_ptr<IndexBuffer> GeometryPool::GetIndexBuffer() const
    {
        std::lock_guard lock( m_Mutex );
        return m_Indices;
    }

    MeshGeometry GeometryPool::Describe( const GeometryPoolAllocation& allocation,
                                         const bool                    authoredStreams ) const
    {
        MeshGeometry geometry;
        if ( !allocation )
            return geometry;
        const GeometryPoolRange& range = allocation.Range();
        std::lock_guard          lock( m_Mutex );
        const Arena&             arena = m_Arenas[static_cast<std::size_t>( range.Arena )];
        geometry.Vertices              = arena.Vertices;
        geometry.Streams               = arena.Streams;
        geometry.Indices               = range.IndexCount != 0 ? m_Indices : nullptr;
        geometry.VertexStride          = GeometryArenaVertexStride( range.Arena );
        geometry.VertexByteOffset      = uint64_t{ range.BaseVertex } * geometry.VertexStride;
        geometry.StreamByteOffset      = uint64_t{ range.BaseVertex } * kGeometryStreamStride;
        geometry.IndexByteOffset       = uint64_t{ range.FirstIndex } * kGeometryIndexStride;
        geometry.VertexCount           = range.VertexCount;
        geometry.IndexCount            = range.IndexCount;
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
                           const GeometryPoolRange& range = retired.Range;
                           m_Arenas[static_cast<std::size_t>( range.Arena )].Spans.Give(
                                range.BaseVertex, RoundUp( range.VertexCount, VertexGranule( range.Arena ) ) );
                           if ( range.IndexCount != 0 )
                               m_IndexSpans.Give( range.FirstIndex,
                                                  RoundUp( range.IndexCount, Granule( kGeometryIndexStride ) ) );
                           --m_Allocations;
                           return true;
                       } );
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
        stats.Growths     = m_Growths;
        for ( std::size_t i = 0; i < kGeometryArenaCount; ++i )
        {
            stats.VertexCapacity[i] = m_Arenas[i].Spans.Capacity;
            stats.VerticesUsed[i]   = m_Arenas[i].Spans.Used;
        }
        stats.IndexCapacity = m_IndexSpans.Capacity;
        stats.IndicesUsed   = m_IndexSpans.Used;
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
        m_Indices.reset();
        m_IndexSpans = SpanList{};
        m_Retired.clear();
        m_Allocations = 0;
        m_ShutDown    = true;
    }
} // namespace Desert::Graphic
