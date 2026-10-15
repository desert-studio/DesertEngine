#include <Engine/Assets/MeshDistanceField.hpp>

#include <algorithm>
#include <bit>
#include <Common/Core/JobSystem.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <format>
#include <mutex>

namespace Desert::Assets
{
    namespace
    {
        constexpr char kDfMagic[8] = { 'D', 'E', 'S', 'T', 'M', 'S', 'D', 'F' };

        std::mutex               s_DfBuilderMutex;
        MeshDistanceFieldBuilder s_DfBuilder;

        // Little-endian fixed-width writer/reader: the container is read on the platform that did not write it.
        void DfPutBytes( std::string& out, const void* data, const size_t size )
        {
            out.append( static_cast<const char*>( data ), size );
        }
        void DfPutU32( std::string& out, const uint32_t v )
        {
            const uint8_t b[4] = { static_cast<uint8_t>( v ), static_cast<uint8_t>( v >> 8 ),
                                   static_cast<uint8_t>( v >> 16 ), static_cast<uint8_t>( v >> 24 ) };
            DfPutBytes( out, b, 4 );
        }
        void DfPutF32( std::string& out, const float v )
        {
            DfPutU32( out, std::bit_cast<uint32_t>( v ) );
        }

        class DfReader
        {
        public:
            explicit DfReader( const std::string_view bytes ) : m_Bytes( bytes )
            {
            }
            bool U32( uint32_t& v )
            {
                if ( m_Bytes.size() - m_At < 4 )
                    return false;
                const auto* p = reinterpret_cast<const uint8_t*>( m_Bytes.data() + m_At );
                v             = static_cast<uint32_t>( p[0] ) | ( static_cast<uint32_t>( p[1] ) << 8 ) |
                    ( static_cast<uint32_t>( p[2] ) << 16 ) | ( static_cast<uint32_t>( p[3] ) << 24 );
                m_At += 4;
                return true;
            }
            bool F32( float& v )
            {
                uint32_t u = 0;
                if ( !U32( u ) )
                    return false;
                v = std::bit_cast<float>( u );
                return true;
            }
            bool Raw( void* out, const size_t size )
            {
                if ( m_Bytes.size() - m_At < size )
                    return false;
                std::memcpy( out, m_Bytes.data() + m_At, size );
                m_At += size;
                return true;
            }
            [[nodiscard]] size_t Remaining() const
            {
                return m_Bytes.size() - m_At;
            }

        private:
            std::string_view m_Bytes;
            size_t           m_At = 0;
        };
    } // namespace

    uint32_t MeshDistanceFieldVolume::TotalBrickCount() const
    {
        uint32_t total = 0;
        for ( const auto& mip : Mips )
            total += mip.BrickCount();
        return total;
    }

    uint32_t MeshDistanceFieldData::TotalBrickCount() const
    {
        uint32_t total = 0;
        for ( const auto& volume : Volumes )
            total += volume.TotalBrickCount();
        return total;
    }

    glm::vec3 MeshDistanceFieldVolume::VolumePositionExtent() const
    {
        return ( LocalSpaceMeshBounds.Max - LocalSpaceMeshBounds.Min ) * 0.5f * LocalToVolumeScale();
    }

    float MeshDistanceFieldVolume::LocalToVolumeScale() const
    {
        const glm::vec3 extent  = ( LocalSpaceMeshBounds.Max - LocalSpaceMeshBounds.Min ) * 0.5f;
        const float     largest = std::max( { extent.x, extent.y, extent.z } );
        return largest > 0.0f ? 1.0f / largest : 0.0f;
    }

    Common::ResultStr<MeshDistanceFieldBuildInput>
    MeshDistanceFieldInputOf( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                              const std::vector<Submesh>& submeshes, const std::vector<uint8_t>& submeshTwoSided )
    {
        // LOD0 of every submesh (UE builds the field from LOD0's render data). A submesh's LOD0 is its own index
        // range when it carries no LOD chain, and LODs[0] when it does. Vertices are copied per submesh because
        // two submeshes may share a vertex range under different transforms.
        if ( submeshTwoSided.size() != submeshes.size() )
            return Common::MakeFormattedError<MeshDistanceFieldBuildInput>(
                 "distance field: {} two-sided flags for {} submeshes", submeshTwoSided.size(), submeshes.size() );
        MeshDistanceFieldBuildInput input;
        for ( size_t s = 0; s < submeshes.size(); ++s )
        {
            const Submesh& sm          = submeshes[s];
            const uint32_t indexOffset = sm.LODs.empty() ? sm.IndexOffset : sm.LODs.front().IndexOffset;
            const uint32_t indexCount  = sm.LODs.empty() ? sm.IndexCount : sm.LODs.front().IndexCount;
            // Index records are triangles: IndexOffset/IndexCount count indices, three per record.
            const uint32_t firstTri = indexOffset / 3;
            const uint32_t triCount = indexCount / 3;
            const uint32_t base     = static_cast<uint32_t>( input.Positions.size() );
            const uint32_t vBegin   = sm.VertexOffset;
            const uint32_t vEnd =
                 std::min<uint32_t>( sm.VertexOffset + sm.VertexCount, static_cast<uint32_t>( vertices.size() ) );
            for ( uint32_t v = vBegin; v < vEnd; ++v )
                input.Positions.push_back( glm::vec3( sm.Transform * glm::vec4( vertices[v].Position, 1.0f ) ) );
            for ( uint32_t t = firstTri; t < firstTri + triCount && t < indices.size(); ++t )
            {
                const Index& tri = indices[t];
                // Indices are relative to the submesh's vertex range.
                const uint32_t a = tri.V1, b = tri.V2, c = tri.V3;
                if ( a >= vEnd - vBegin || b >= vEnd - vBegin || c >= vEnd - vBegin )
                    continue;
                input.Indices.push_back( base + a );
                input.Indices.push_back( base + b );
                input.Indices.push_back( base + c );
                input.TriangleTwoSided.push_back( submeshTwoSided[s] != 0 ? 1 : 0 );
            }
        }
        return Common::MakeSuccess( std::move( input ) );
    }

    std::string EncodeMeshDistanceField( const MeshDistanceFieldData& data )
    {
        std::string out;
        DfPutBytes( out, kDfMagic, sizeof( kDfMagic ) );
        DfPutU32( out, kMeshDistanceFieldFormatVersion );
        DfPutU32( out, kDistanceFieldNumMips );
        DfPutU32( out, static_cast<uint32_t>( data.Volumes.size() ) );
        for ( const MeshDistanceFieldVolume& volume : data.Volumes )
        {
            for ( int i = 0; i < 3; ++i )
                DfPutF32( out, volume.LocalSpaceMeshBounds.Min[i] );
            for ( int i = 0; i < 3; ++i )
                DfPutF32( out, volume.LocalSpaceMeshBounds.Max[i] );
            for ( const auto& mip : volume.Mips )
            {
                for ( int i = 0; i < 3; ++i )
                    DfPutU32( out, mip.IndirectionDimensions[i] );
                DfPutU32( out, mip.BrickCount() );
                DfPutF32( out, mip.DistanceFieldToVolumeScaleBias.x );
                DfPutF32( out, mip.DistanceFieldToVolumeScaleBias.y );
                for ( int i = 0; i < 3; ++i )
                    DfPutF32( out, mip.VolumeToVirtualUVScale[i] );
                for ( int i = 0; i < 3; ++i )
                    DfPutF32( out, mip.VolumeToVirtualUVAdd[i] );
                for ( const uint32_t entry : mip.IndirectionTable )
                    DfPutU32( out, entry );
                DfPutBytes( out, mip.BrickData.data(), mip.BrickData.size() );
            }
        }
        return out;
    }

    Common::ResultStr<MeshDistanceFieldData> DecodeMeshDistanceField( const std::string_view bytes )
    {
        DfReader reader( bytes );
        char   magic[8]{};
        if ( !reader.Raw( magic, sizeof( magic ) ) || std::memcmp( magic, kDfMagic, sizeof( kDfMagic ) ) != 0 )
            return Common::MakeError<MeshDistanceFieldData>( "not a mesh distance field (magic is not DESTMSDF)" );
        uint32_t version = 0, mipCount = 0, volumeCount = 0;
        if ( !reader.U32( version ) || version != kMeshDistanceFieldFormatVersion )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "mesh distance field version {} is not the supported {}", version,
                 kMeshDistanceFieldFormatVersion );
        if ( !reader.U32( mipCount ) || mipCount != kDistanceFieldNumMips )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "mesh distance field states {} mips, the format has {}", mipCount, kDistanceFieldNumMips );
        // A volume is at least its bounds and three mip headers: a count the bytes cannot hold is corrupt.
        constexpr uint64_t kVolumeHeaderBytes = 24 + kDistanceFieldNumMips * 48;
        if ( !reader.U32( volumeCount ) ||
             static_cast<uint64_t>( volumeCount ) * kVolumeHeaderBytes > reader.Remaining() )
            return Common::MakeFormattedError<MeshDistanceFieldData>(
                 "mesh distance field states {} volumes, more than the {} bytes left hold", volumeCount,
                 reader.Remaining() );
        MeshDistanceFieldData data;
        data.Volumes.resize( volumeCount );
        bool ok = true;
        for ( uint32_t v = 0; ok && v < volumeCount; ++v )
        {
            MeshDistanceFieldVolume& volume = data.Volumes[v];
            for ( int i = 0; i < 3; ++i )
                ok = ok && reader.F32( volume.LocalSpaceMeshBounds.Min[i] );
            for ( int i = 0; i < 3; ++i )
                ok = ok && reader.F32( volume.LocalSpaceMeshBounds.Max[i] );
            for ( uint32_t m = 0; ok && m < kDistanceFieldNumMips; ++m )
            {
                auto&    mip    = volume.Mips[m];
                uint32_t bricks = 0;
                for ( int i = 0; i < 3; ++i )
                    ok = ok && reader.U32( mip.IndirectionDimensions[i] );
                ok = ok && reader.U32( bricks ) && reader.F32( mip.DistanceFieldToVolumeScaleBias.x ) &&
                     reader.F32( mip.DistanceFieldToVolumeScaleBias.y );
                for ( int i = 0; i < 3; ++i )
                    ok = ok && reader.F32( mip.VolumeToVirtualUVScale[i] );
                for ( int i = 0; i < 3; ++i )
                    ok = ok && reader.F32( mip.VolumeToVirtualUVAdd[i] );
                if ( !ok )
                    break;
                const uint64_t cells = static_cast<uint64_t>( mip.IndirectionDimensions.x ) *
                                       mip.IndirectionDimensions.y * mip.IndirectionDimensions.z;
                const uint64_t brickBytes = static_cast<uint64_t>( bricks ) * kDistanceFieldBrickVoxels;
                if ( cells * 4 + brickBytes > reader.Remaining() )
                    return Common::MakeFormattedError<MeshDistanceFieldData>(
                         "mesh distance field volume {} mip {} states {} cells and {} bricks, more than the {} "
                         "bytes left",
                         v, m, cells, bricks, reader.Remaining() );
                mip.IndirectionTable.resize( cells );
                for ( auto& entry : mip.IndirectionTable )
                {
                    ok = ok && reader.U32( entry );
                    if ( ok && entry != kDistanceFieldInvalidBrick && entry >= bricks )
                        return Common::MakeFormattedError<MeshDistanceFieldData>(
                             "mesh distance field volume {} mip {} names brick {} of {}", v, m, entry, bricks );
                }
                mip.BrickData.resize( brickBytes );
                ok = ok && reader.Raw( mip.BrickData.data(), mip.BrickData.size() );
            }
        }
        if ( !ok )
            return Common::MakeError<MeshDistanceFieldData>( "mesh distance field is truncated" );
        if ( reader.Remaining() != 0 )
            return Common::MakeFormattedError<MeshDistanceFieldData>( "mesh distance field has {} trailing bytes",
                                                                      reader.Remaining() );
        return Common::MakeSuccess( std::move( data ) );
    }

    uint64_t MeshDistanceFieldDerivedDataKey( const uint64_t                        renderDataKey,
                                              const MeshDistanceFieldBuildSettings& settings,
                                              const std::vector<uint8_t>&           submeshTwoSided )
    {
        std::string image;
        DfPutU32( image, kMeshDistanceFieldBuilderVersion );
        DfPutU32( image, kMeshDistanceFieldFormatVersion );
        DfPutF32( image, settings.VoxelDensity );
        DfPutU32( image, settings.MaxPerMeshResolution );
        DfPutF32( image, settings.ResolutionScale );
        DfPutF32( image, settings.ChunkVoxelSize );
        // The materials' sidedness shapes the sign (UE keys the field on each section's bTwoSided): turning a
        // material two-sided re-derives every mesh drawing it, with no change to the mesh's render data.
        DfPutU32( image, static_cast<uint32_t>( submeshTwoSided.size() ) );
        for ( const uint8_t twoSided : submeshTwoSided )
            DfPutU32( image, twoSided != 0 ? 1u : 0u );
        return Common::DDC::MakeKey( kMeshDistanceFieldDeriver, renderDataKey, image.data(), image.size() );
    }

    void SetMeshDistanceFieldBuilder( MeshDistanceFieldBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_DfBuilderMutex );
        s_DfBuilder = std::move( builder );
    }

    Common::ResultStr<std::string> BuildMeshDistanceFieldBytes( const MeshDistanceFieldBuildInput& input,
                                                                const MeshDistanceFieldCancel&     cancel )
    {
        auto built = BuildMeshDistanceField( input, MeshDistanceFieldBuildSettings{}, cancel );
        if ( !built.IsSuccess() )
            return Common::MakeError<std::string>( built.GetError() );
        return Common::MakeSuccess( EncodeMeshDistanceField( built.GetValue() ) );
    }

    Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>>
    LoadMeshDistanceField( const std::filesystem::path& asset, const uint64_t renderDataKey,
                           const std::vector<uint8_t>&                                            submeshTwoSided,
                           const std::function<Common::ResultStr<MeshDistanceFieldBuildInput>()>& input,
                           bool* built, const MeshDistanceFieldCancel& cancel )
    {
        if ( built != nullptr )
            *built = false;
        using Result = std::shared_ptr<const MeshDistanceFieldData>;
        const uint64_t key =
             MeshDistanceFieldDerivedDataKey( renderDataKey, MeshDistanceFieldBuildSettings{}, submeshTwoSided );
        MeshDistanceFieldBuilder builder;
        {
            const std::lock_guard<std::mutex> lock( s_DfBuilderMutex );
            builder = s_DfBuilder;
        }
        std::string bytes;
        if ( !builder )
        {
            auto hit = Common::DDC::Get( kMeshDistanceFieldDeriver, key );
            if ( !hit.has_value() )
                return Common::MakeFormattedError<Result>(
                     "mesh asset '{}' has no distance field under DDC key {:016x} ({}), and this build cannot "
                     "derive it: the package was cooked without it",
                     asset.string(), key,
                     Common::DDC::RelativePath( kMeshDistanceFieldDeriver, key ).generic_string() );
            bytes = std::move( *hit );
        }
        else
        {
            auto derived =
                 Common::DDC::GetOrBuild( kMeshDistanceFieldDeriver, key,
                                          [&builder, &input, built, &cancel]() -> Common::ResultStr<std::string>
                                          {
                                              if ( built != nullptr )
                                                  *built = true;
                                              auto in = input();
                                              if ( !in.IsSuccess() )
                                                  return Common::MakeError<std::string>( in.GetError() );
                                              return builder( in.GetValue(), cancel );
                                          } );
            if ( !derived.IsSuccess() )
                return Common::MakeFormattedError<Result>( "mesh asset '{}': distance field: {}", asset.string(),
                                                           derived.GetError() );
            bytes = derived.ExtractValue();
        }
        auto decoded = DecodeMeshDistanceField( bytes );
        if ( !decoded.IsSuccess() )
            return Common::MakeFormattedError<Result>(
                 "mesh asset '{}': cached distance field {:016x} is corrupt: {}", asset.string(), key,
                 decoded.GetError() );
        return Common::MakeSuccess<Result>(
             std::make_shared<const MeshDistanceFieldData>( decoded.ExtractValue() ) );
    }

    std::shared_ptr<const MeshDistanceFieldData> MeshDistanceFieldSlot::Get() const
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        return m_Field;
    }

    void MeshDistanceFieldSlot::Publish( std::shared_ptr<const MeshDistanceFieldData> field )
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        m_Field = std::move( field );
    }

    namespace
    {
        struct DfQueuedWork
        {
            // Weak: the queue does not keep a mesh's field alive. A slot nobody holds any more (the mesh was
            // unloaded or reloaded, its service went away) cancels the work if it has not started.
            std::weak_ptr<MeshDistanceFieldSlot>   Slot;
            std::string                            Asset;
            MeshDistanceFieldWork                  Work;
        };

        // The queue outlives every static: a job still running when the process exits reads it after the
        // statics of this file are gone, so it is allocated once and never destroyed.
        struct DfQueue
        {
            std::mutex               Mutex;
            std::deque<DfQueuedWork> Waiting;
            size_t                   Running = 0;
            bool                     Exiting = false;
            // Raised once, by the exit hook: every running build's token reads it (MeshDistanceFieldCancel) and
            // the build stops at its next brick. Never lowered - the process is going away.
            std::atomic<bool> Abort{ false };
            // Signalled whenever Running drops: CancelMeshDistanceFields waits on it for the running work.
            std::condition_variable Idle;
            // The current burst (from the first request into an idle queue until it drains), for its log line.
            std::chrono::steady_clock::time_point BurstStart;
            uint32_t                              FromCache = 0;
            uint32_t                              Built     = 0;
            uint32_t                              Failed    = 0;
            uint32_t                              Cancelled = 0;
            double                                WorkMs    = 0.0;
        };

        // Drops the waiting work and returns once none is running. @p exiting also refuses new work and cancels
        // the running work, so the wait is for the brick each build is on, not for the build.
        void DfQuiesce( bool exiting );

        DfQueue& DfQueueState()
        {
            static DfQueue* const s_Queue = []
            {
                // The pool first, then the exit hook: atexit handlers run in reverse, so the waiting builds are
                // dropped and the running ones finished before the pool joins its workers - a closing editor
                // does not finish a 1300-mesh burst, and no build outlives the statics it logs through.
                (void)Common::JobSystem::Get();
                auto* queue = new DfQueue();
                std::atexit( [] { DfQuiesce( true ); } );
                // Every write of this queue goes through the DDC: a root that moves (a suite's sandbox set up
                // or torn down) first waits for the work started under it.
                Common::DDC::AddRootRetireHook( [] { DfQuiesce( false ); } );
                return queue;
            }();
            return *s_Queue;
        }

        size_t DfConcurrency()
        {
            const size_t workers = Common::JobSystem::Get().WorkerCount();
            return workers > 1 ? workers - 1 : 1;
        }

        void DfRun( DfQueuedWork item );

        void DfQuiesce( const bool exiting )
        {
            DfQueue&                     q = DfQueueState();
            std::unique_lock<std::mutex> lock( q.Mutex );
            if ( exiting )
            {
                q.Exiting = true;
                q.Abort.store( true, std::memory_order_relaxed );
            }
            q.Cancelled += static_cast<uint32_t>( q.Waiting.size() );
            q.Waiting.clear();
            q.Idle.wait( lock, [&q] { return q.Running == 0; } );
        }

        // Starts waiting work while there is room. Called with the lock NOT held.
        void DfPump()
        {
            DfQueue& q = DfQueueState();
            for ( ;; )
            {
                DfQueuedWork next;
                {
                    const std::lock_guard<std::mutex> lock( q.Mutex );
                    if ( q.Exiting || q.Waiting.empty() || q.Running >= DfConcurrency() )
                        return;
                    next = std::move( q.Waiting.front() );
                    q.Waiting.pop_front();
                    ++q.Running;
                }
                auto shared = std::make_shared<DfQueuedWork>( std::move( next ) );
                if ( Common::JobSystem::Get().Submit( [shared] { DfRun( std::move( *shared ) ); } ) )
                    continue;
                // The pool was shut down ahead of this queue's exit hook (it drains what it holds, then refuses
                // more): the work never runs, so it is not running - otherwise DfQuiesce waits for it forever
                // and the process never exits. Nothing more can start; the waiting work is dropped with it.
                {
                    const std::lock_guard<std::mutex> lock( q.Mutex );
                    --q.Running;
                    q.Exiting = true;
                    q.Cancelled += 1 + static_cast<uint32_t>( q.Waiting.size() );
                    q.Waiting.clear();
                }
                q.Idle.notify_all();
                return;
            }
        }

        void DfRun( DfQueuedWork item )
        {
            const auto start     = std::chrono::steady_clock::now();
            bool       built     = false;
            bool       cancelled = false;
            Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>> field =
                 Common::MakeSuccess<std::shared_ptr<const MeshDistanceFieldData>>( nullptr );
            DfQueue&                      q = DfQueueState();
            const MeshDistanceFieldCancel cancel( &q.Abort, item.Slot );
            // The owner is gone or the process exits: nothing would read the field, so it is neither read nor
            // built - and a build already running stops at its next brick (the token) and writes no DDC entry.
            for ( ;; )
            {
                if ( cancel.IsCancelled() )
                {
                    cancelled = true;
                    break;
                }
                field = item.Work( built, cancel );
                if ( field.IsSuccess() ||
                     field.GetError().find( kMeshDistanceFieldCancelled ) == std::string::npos )
                    break;
                // Cancelled: ours (the loop's check above says so), or the DDC flight this work followed was
                // another owner's (the same render data, unloaded): this owner still wants it, so derive again.
                built = false;
            }
            const double ms =
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
            if ( !field.IsSuccess() )
            {
                LOG_ERROR( "[MeshDistanceField] '{}': {}; the mesh is left out of every distance-field trace",
                           item.Asset, field.GetError() );
            }
            else if ( const auto slot = item.Slot.lock(); slot != nullptr && !cancelled )
                slot->Publish( field.ExtractValue() );

            bool     drained   = false;
            uint32_t fromCache = 0, builtCount = 0, failed = 0, dropped = 0;
            double   wallSeconds = 0.0, workMs = 0.0;
            {
                const std::lock_guard<std::mutex> lock( q.Mutex );
                --q.Running;
                if ( cancelled )
                    ++q.Cancelled;
                else if ( !field.IsSuccess() )
                    ++q.Failed;
                else if ( built )
                    ++q.Built;
                else
                    ++q.FromCache;
                q.WorkMs += ms;
                if ( q.Running == 0 && q.Waiting.empty() )
                {
                    drained    = true;
                    fromCache  = q.FromCache;
                    builtCount = q.Built;
                    failed     = q.Failed;
                    dropped    = q.Cancelled;
                    workMs     = q.WorkMs;
                    wallSeconds =
                         std::chrono::duration<double>( std::chrono::steady_clock::now() - q.BurstStart ).count();
                    q.FromCache = q.Built = q.Failed = q.Cancelled = 0;
                    q.WorkMs                         = 0.0;
                }
            }
            q.Idle.notify_all();
            // A burst of cache hits while a scene streams in drains many times in a few frames: those lines are
            // debug; a burst that built a field, failed one or took over a second is the line a load is read by.
            if ( drained && ( builtCount > 0 || failed > 0 || wallSeconds >= 1.0 ) )
            {
                LOG_INFO( "[MeshDistanceField] async queue drained: {} field(s) - {} built, {} from the DDC, {} "
                          "failed, {} cancelled (owner gone or cache root retired) - in {:.1f} s wall ({:.1f} s "
                          "of work on up to {} worker(s))",
                          fromCache + builtCount + failed + dropped, builtCount, fromCache, failed, dropped,
                          wallSeconds, workMs / 1000.0, DfConcurrency() );
            }
            else if ( drained )
            {
                LOG_DEBUG( "[MeshDistanceField] async queue drained: {} field(s) from the DDC in {:.2f} s",
                           fromCache, wallSeconds );
            }
            DfPump();
        }
    } // namespace

    void EnqueueMeshDistanceField( std::shared_ptr<MeshDistanceFieldSlot> slot, std::string asset,
                                   MeshDistanceFieldWork work )
    {
        DfQueue& q = DfQueueState();
        {
            const std::lock_guard<std::mutex> lock( q.Mutex );
            if ( q.Exiting )
                return;
            if ( q.Running == 0 && q.Waiting.empty() )
            {
                q.BurstStart = std::chrono::steady_clock::now();
                q.Cancelled  = 0; // dropped by a retire while idle: not this burst's
            }
            q.Waiting.push_back( { std::move( slot ), std::move( asset ), std::move( work ) } );
        }
        DfPump();
    }

    size_t PendingMeshDistanceFields()
    {
        DfQueue&                          q = DfQueueState();
        const std::lock_guard<std::mutex> lock( q.Mutex );
        return q.Waiting.size() + q.Running;
    }
} // namespace Desert::Assets
