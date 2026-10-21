#include <Engine/Graphic/DistanceField/DistanceFieldSceneData.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <limits>

namespace Desert::Graphic::DistanceField
{
    namespace DFShared
    {
#include <Common/DistanceFieldShared.glslh>
    } // namespace DFShared

    // The shader-side layout and the asset format are one layout.
    static_assert( DFShared::DF_BRICK_SIZE == static_cast<int>( Assets::kDistanceFieldBrickSize ) );
    static_assert( DFShared::DF_UNIQUE_BRICK_SIZE == static_cast<int>( Assets::kDistanceFieldUniqueBrickSize ) );
    static_assert( DFShared::DF_NUM_MIPS == static_cast<int>( Assets::kDistanceFieldNumMips ) );

    namespace
    {
        constexpr uint32_t kDfAtlasBricksX       = static_cast<uint32_t>( DFShared::DF_ATLAS_BRICKS_X );
        constexpr uint32_t kDfAtlasBricksY       = static_cast<uint32_t>( DFShared::DF_ATLAS_BRICKS_Y );
        constexpr uint32_t kDfBrick              = Assets::kDistanceFieldBrickSize;
        constexpr uint32_t kDfBricksPerSlice     = kDfAtlasBricksX * kDfAtlasBricksY;
        constexpr uint32_t kDfMaxAtlasDepthTexel = 2048; // the smallest maxImageDimension3D the engine targets
        constexpr uint32_t kDfBytesPerSlice      = kDfBricksPerSlice * Assets::kDistanceFieldBrickVoxels;
        // The finer mips uploaded per frame at most (4 MiB of bricks): what bounds the staging memory and the
        // frame's upload time while a camera move streams a district in. A mip is never split, so a frame always
        // takes at least one. The coarse mips of newly drawn fields are not counted: without them the object
        // would be drawn but absent from the field.
        constexpr uint32_t kDfStreamBricksPerFrame = 8192;

        // The mips a field wants at @p separation (cm): the largest per-axis gap between the camera and the box of
        // the nearest object drawn with it. Ported from UE ComputeDistanceFieldAssetWantedMipsCS
        // (DistanceFieldStreaming.usf:132-165, extents set in DistanceFieldStreaming.cpp:1240-1245): every mip for
        // a box overlapping the box of global clipmap NumClipmaps/2 - 1 around the camera, two for one
        // overlapping the coarsest clipmap's box, the coarse mip beyond. The mips follow the CLIPMAPS because
        // each clipmap voxel is composited from them: a clipmap must never read a mesh field coarser than it
        // can resolve. Requesting one clipmap level finer (all mips inside clipmap 0 only) left the street seen
        // from 30 m up - clipmap 1 - composited from the ground meshes' middle mip, whose voxels on a 100 m
        // ground sheet are 1.6 m; trilinear interpolation over an open sheet edge (a kerb, a cut-out around a
        // bollard) overestimated the distance by more than clipmap 1's half-voxel surface expansion, and the
        // trace saw through the ground there.
        uint32_t DfWantedMips( const float separation )
        {
            static_assert( Assets::kDistanceFieldNumMips == 3, "the policy below names three mips, as UE's" );
            const float allMipsExtent =
                 DFShared::GDF_CLIPMAP0_EXTENT *
                 static_cast<float>( 1 << std::max( DFShared::GDF_NUM_CLIPMAPS / 2 - 1, 0 ) );
            const float twoMipsExtent =
                 DFShared::GDF_CLIPMAP0_EXTENT * static_cast<float>( 1 << ( DFShared::GDF_NUM_CLIPMAPS - 1 ) );
            if ( separation <= allMipsExtent )
                return 3;
            return separation <= twoMipsExtent ? 2 : 1;
        }
        constexpr uint32_t kDfAssetStride        = static_cast<uint32_t>( DFShared::DF_ASSET_STRIDE );

        float DfBits( const uint32_t v )
        {
            return std::bit_cast<float>( v );
        }
    } // namespace

    uint32_t RangeAllocator::Allocate( const uint32_t count )
    {
        for ( auto it = m_Free.begin(); it != m_Free.end(); ++it )
        {
            if ( it->second < count )
                continue;
            const uint32_t offset = it->first;
            const uint32_t left   = it->second - count;
            m_Free.erase( it );
            if ( left > 0 )
                m_Free.emplace( offset + count, left );
            return offset;
        }
        const uint32_t offset = m_End;
        m_End += count;
        return offset;
    }

    void RangeAllocator::Free( const uint32_t offset, const uint32_t count )
    {
        if ( count == 0 )
            return;
        auto [it, inserted] = m_Free.emplace( offset, count );
        // Coalesce with the following and the preceding range.
        if ( const auto next = std::next( it ); next != m_Free.end() && it->first + it->second == next->first )
        {
            it->second += next->second;
            m_Free.erase( next );
        }
        if ( it != m_Free.begin() )
        {
            const auto previous = std::prev( it );
            if ( previous->first + previous->second == it->first )
            {
                previous->second += it->second;
                m_Free.erase( it );
                it = previous;
            }
        }
        // A free range touching the end shrinks the end instead of staying on the list.
        if ( it->first + it->second == m_End )
        {
            m_End = it->first;
            m_Free.erase( it );
        }
    }

    void BrickAllocator::Reset( const uint32_t capacity )
    {
        m_Capacity = capacity;
        m_Next     = 0;
        m_Free.clear();
    }

    std::optional<std::vector<uint32_t>> BrickAllocator::Allocate( const uint32_t count )
    {
        if ( count > Available() )
            return std::nullopt;
        std::vector<uint32_t> slots;
        slots.reserve( count );
        while ( slots.size() < count && !m_Free.empty() )
        {
            slots.push_back( m_Free.back() );
            m_Free.pop_back();
        }
        while ( slots.size() < count )
            slots.push_back( m_Next++ );
        return slots;
    }

    void BrickAllocator::Free( const std::vector<uint32_t>& slots )
    {
        m_Free.insert( m_Free.end(), slots.begin(), slots.end() );
    }

    namespace
    {
        template <typename T>
        Common::BoolResultStr UploadRecords( ShaderResources::StorageBuffer& buffer, const std::vector<T>& data,
                                             const char* what )
        {
            // A zero-sized write has nowhere to go; an empty list is uploaded as one zero element nobody reads.
            static const T zero{};
            const void*    bytes = data.empty() ? static_cast<const void*>( &zero ) : data.data();
            const auto     size  = static_cast<uint32_t>( std::max<size_t>( 1, data.size() ) * sizeof( T ) );
            if ( const auto wrote = buffer.SetData( bytes, size ); !wrote )
                return Common::MakeFormattedError<bool>( "mesh distance fields: {} upload ({} bytes): {}", what,
                                                         size, wrote.GetError() );
            return BOOLSUCCESS;
        }
    } // namespace

    DistanceFieldSceneData::DistanceFieldSceneData( const ResourceOwnerInstance scene ) : m_LedgerInstance( scene )
    {
    }
    DistanceFieldSceneData::~DistanceFieldSceneData() = default;

    bool DistanceFieldSceneData::SetPoolMiB( const uint32_t poolMiB )
    {
        if ( poolMiB == m_PoolMiB )
            return false;
        // Everything resident lived in the old atlas: drop it all, the next frame acquires again.
        m_Resident.clear();
        m_IndirectionSlots = {};
        m_AssetSlots       = {};
        m_AssetData.clear();
        m_Indirection.clear();
        ++m_Revision;
        m_Pending.clear();
        m_Atlas.reset();
        m_PoolMiB = poolMiB;
        const uint64_t slices =
             std::clamp<uint64_t>( static_cast<uint64_t>( poolMiB ) * 1024u * 1024u / kDfBytesPerSlice, 1u,
                                   kDfMaxAtlasDepthTexel / kDfBrick );
        m_CapacityBricks = static_cast<uint32_t>( slices ) * kDfBricksPerSlice;
        m_BrickSlots.Reset( m_CapacityBricks );
        return true;
    }

    void DistanceFieldSceneData::BeginFrame()
    {
        for ( auto& [key, resident] : m_Resident )
        {
            resident.Used     = false;
            resident.Distance = std::numeric_limits<float>::max();
        }
        m_UploadedThisFrame = 0;
    }

    Common::ResultStr<std::optional<uint32_t>>
    DistanceFieldSceneData::Acquire( const std::shared_ptr<const Assets::MeshDistanceFieldVolume>& field,
                                     const float                                                   distance )
    {
        using Result = std::optional<uint32_t>;
        if ( !field )
            return Common::MakeSuccess<Result>( std::nullopt );
        if ( const auto it = m_Resident.find( field.get() ); it != m_Resident.end() )
        {
            if ( it->second.Field.lock() == field )
            {
                it->second.Used     = true;
                it->second.Distance = std::min( it->second.Distance, distance );
                return Common::MakeSuccess<Result>( it->second.AssetIndex );
            }
            // A new field at the address of one that died since the last sweep.
            Release( it->second );
            m_Resident.erase( it );
        }
        if ( field->Mips[0].IndirectionDimensions.x == 0 )
            return Common::MakeSuccess<Result>( std::nullopt );

        Resident resident;
        resident.Field      = field;
        resident.Used       = true;
        resident.Distance   = distance;
        resident.AssetIndex = m_AssetSlots.Allocate( 1 );
        if ( m_AssetData.size() < static_cast<size_t>( m_AssetSlots.End() ) * kDfAssetStride )
            m_AssetData.resize( static_cast<size_t>( m_AssetSlots.End() ) * kDfAssetStride, glm::vec4( 0.0f ) );
        // THE COARSE MIP IS ALWAYS RESIDENT: room is made for it by evicting finer mips of any other field.
        const uint32_t coarseBricks = field->Mips[Assets::kDistanceFieldNumMips - 1].BrickCount();
        bool           added        = AddMip( resident, *field );
        if ( !added && MakeRoom( -1.0f, coarseBricks, nullptr ) )
            added = AddMip( resident, *field );
        if ( !added )
        {
            m_AssetSlots.Free( resident.AssetIndex, 1 );
            return Common::MakeFormattedError<Result>(
                 "the {} MiB distance-field pool (Scalability GlobalIllumination.DistanceFieldPoolMiB) cannot "
                 "hold "
                 "the coarse mips of {} field(s): {} of {} brick(s) in use, {} more needed",
                 m_PoolMiB, m_Resident.size() + 1, m_BrickSlots.Used(), m_CapacityBricks, coarseBricks );
        }
        const uint32_t index = resident.AssetIndex;
        m_Resident.emplace( field.get(), std::move( resident ) );
        return Common::MakeSuccess<Result>( index );
    }

    bool DistanceFieldSceneData::AddMip( Resident& resident, const Assets::MeshDistanceFieldVolume& field )
    {
        const uint32_t                        r      = resident.NumMips;
        const uint32_t                        source = Assets::kDistanceFieldNumMips - 1 - r;
        const Assets::SparseDistanceFieldMip& mip    = field.Mips[source];
        auto                                  bricks = m_BrickSlots.Allocate( mip.BrickCount() );
        if ( !bricks )
            return false;

        ResidentMip& slot      = resident.Mips[r];
        slot.Bricks            = std::move( *bricks );
        slot.IndirectionCount  = static_cast<uint32_t>( mip.IndirectionTable.size() );
        slot.IndirectionOffset = m_IndirectionSlots.Allocate( slot.IndirectionCount );
        if ( m_Indirection.size() < m_IndirectionSlots.End() )
            m_Indirection.resize( m_IndirectionSlots.End(), Assets::kDistanceFieldInvalidBrick );
        for ( size_t i = 0; i < mip.IndirectionTable.size(); ++i )
        {
            const uint32_t local = mip.IndirectionTable[i];
            m_Indirection[slot.IndirectionOffset + i] =
                 local == Assets::kDistanceFieldInvalidBrick ? local : slot.Bricks[local];
        }
        // UE FDFAssetData: the volume -> indirection transform is the virtual UV one scaled by the grid.
        glm::vec4*      record = &m_AssetData[static_cast<size_t>( resident.AssetIndex ) * kDfAssetStride];
        const glm::vec3 dims   = glm::vec3( mip.IndirectionDimensions );
        const glm::vec3 scale  = mip.VolumeToVirtualUVScale * dims;
        const glm::vec3 add    = mip.VolumeToVirtualUVAdd * dims;
        glm::vec4*      rows   = record + 1 + r * DFShared::DF_ASSET_MIP_STRIDE;
        rows[0] = glm::vec4( DfBits( mip.IndirectionDimensions.x ), DfBits( mip.IndirectionDimensions.y ),
                             DfBits( mip.IndirectionDimensions.z ), DfBits( slot.IndirectionOffset ) );
        rows[1] = glm::vec4( mip.DistanceFieldToVolumeScaleBias, scale.x, scale.y );
        rows[2] = glm::vec4( scale.z, add );

        const std::shared_ptr<const Assets::MeshDistanceFieldVolume> held = resident.Field.lock();
        for ( uint32_t b = 0; b < slot.Bricks.size(); ++b )
            m_Pending.push_back( { slot.Bricks[b], held, source, b } );
        // The bricks are written by FlushUploads before any pass of this frame reads the record.
        ++resident.NumMips;
        record[0] = glm::vec4( DfBits( resident.NumMips ), 0.0f, 0.0f, 0.0f );
        ++m_Revision;
        return true;
    }

    void DistanceFieldSceneData::DropMip( Resident& resident )
    {
        ++m_Revision;
        const uint32_t r        = --resident.NumMips;
        glm::vec4*     record   = &m_AssetData[static_cast<size_t>( resident.AssetIndex ) * kDfAssetStride];
        record[0]               = glm::vec4( DfBits( resident.NumMips ), 0.0f, 0.0f, 0.0f );
        const ResidentMip& slot = resident.Mips[r];
        std::fill_n( m_Indirection.begin() + slot.IndirectionOffset, slot.IndirectionCount,
                     Assets::kDistanceFieldInvalidBrick );
        m_IndirectionSlots.Free( slot.IndirectionOffset, slot.IndirectionCount );
        m_BrickSlots.Free( slot.Bricks );
        // The mip's bricks not yet written: by the field and mip they would come from.
        const Assets::MeshDistanceFieldVolume* field  = resident.Field.lock().get();
        const uint32_t                       source = Assets::kDistanceFieldNumMips - 1 - r;
        std::erase_if( m_Pending, [field, source]( const PendingBrick& p )
                       { return p.Field.get() == field && p.Mip == source; } );
        resident.Mips[r] = {};
    }

    void DistanceFieldSceneData::Release( Resident& resident )
    {
        while ( resident.NumMips > 0 )
            DropMip( resident );
        m_AssetSlots.Free( resident.AssetIndex, 1 );
    }

    bool DistanceFieldSceneData::MakeRoom( const float distance, const uint32_t bricks, const Resident* except )
    {
        if ( m_BrickSlots.Available() >= bricks )
            return true;
        std::vector<Resident*> holders;
        uint64_t               droppable = 0;
        for ( auto& [key, resident] : m_Resident )
            if ( &resident != except && resident.NumMips > 1 && resident.Distance > distance )
            {
                holders.push_back( &resident );
                for ( uint32_t r = 1; r < resident.NumMips; ++r )
                    droppable += resident.Mips[r].Bricks.size();
            }
        if ( m_BrickSlots.Available() + droppable < bricks )
            return false;
        std::sort( holders.begin(), holders.end(),
                   []( const Resident* a, const Resident* b ) { return a->Distance > b->Distance; } );
        for ( Resident* holder : holders )
            while ( holder->NumMips > 1 && m_BrickSlots.Available() < bricks )
                DropMip( *holder );
        return m_BrickSlots.Available() >= bricks;
    }

    void DistanceFieldSceneData::Stream()
    {
        // Out first: a field farther than its finer mips are worth gives them back.
        std::vector<std::pair<Resident*, std::shared_ptr<const Assets::MeshDistanceFieldVolume>>> requests;
        for ( auto& [key, resident] : m_Resident )
        {
            const uint32_t wanted = resident.Used ? DfWantedMips( resident.Distance ) : 1u;
            while ( resident.NumMips > wanted )
                DropMip( resident );
            if ( resident.NumMips < wanted )
                if ( auto field = resident.Field.lock() )
                    requests.emplace_back( &resident, std::move( field ) );
        }
        // In, nearest first, one mip per field per frame, inside the pool and the frame's upload budget; a
        // nearer field may take the room of a farther one's finer mips.
        std::sort( requests.begin(), requests.end(),
                   []( const auto& a, const auto& b ) { return a.first->Distance < b.first->Distance; } );
        uint32_t unmet = 0;
        for ( auto& [resident, field] : requests )
        {
            const uint32_t bricks =
                 field->Mips[Assets::kDistanceFieldNumMips - 1 - resident->NumMips].BrickCount();
            if ( m_UploadedThisFrame > 0 && m_UploadedThisFrame + bricks > kDfStreamBricksPerFrame )
            {
                ++unmet;
                continue;
            }
            bool added = AddMip( *resident, *field );
            if ( !added && MakeRoom( resident->Distance, bricks, resident ) )
                added = AddMip( *resident, *field );
            if ( !added )
            {
                ++unmet;
                continue;
            }
            m_UploadedThisFrame += bricks;
            if ( resident->NumMips < DfWantedMips( resident->Distance ) )
                ++unmet;
        }

        m_Stats                = {};
        m_Stats.Fields         = static_cast<uint32_t>( m_Resident.size() );
        m_Stats.CapacityBricks = m_CapacityBricks;
        m_Stats.UsedBricks     = m_BrickSlots.Used();
        m_Stats.UnmetRequests  = unmet;
        m_Stats.UploadedBricks = m_UploadedThisFrame;
        for ( const auto& [key, resident] : m_Resident )
        {
            m_Stats.CoarseBricks += static_cast<uint32_t>( resident.Mips[0].Bricks.size() );
            if ( resident.NumMips > 0 )
                ++m_Stats.ResidentByMips[resident.NumMips - 1];
        }
    }

    uint32_t DistanceFieldSceneData::ResidentMips( const Assets::MeshDistanceFieldVolume* field ) const
    {
        const auto it = m_Resident.find( field );
        return it != m_Resident.end() ? it->second.NumMips : 0u;
    }

    void DistanceFieldSceneData::ReleaseUnused()
    {
        for ( auto it = m_Resident.begin(); it != m_Resident.end(); )
        {
            if ( it->second.Field.expired() )
            {
                Release( it->second );
                it = m_Resident.erase( it );
            }
            else
                ++it;
        }
    }

    Common::BoolResultStr DistanceFieldSceneData::EnsureAtlas()
    {
        if ( m_Atlas )
            return BOOLSUCCESS;
        if ( m_CapacityBricks == 0 )
            return Common::MakeError<bool>( "the distance-field pool was never sized (SetPoolMiB)" );
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "DistanceFieldBrickAtlas",
             .Width      = kDfAtlasBricksX * kDfBrick,
             .Height     = kDfAtlasBricksY * kDfBrick,
             .Depth      = ( m_CapacityBricks / kDfBricksPerSlice ) * kDfBrick,
             .Format     = Core::Formats::ImageFormat::R8_UNORM,
             .Data       = Core::Formats::EmptyPixelData{},
             .Properties = Core::Formats::Sample,
        };
        auto atlas = Image3D::Create( spec );
        if ( !atlas )
            return Common::MakeFormattedError<bool>(
                 "the {}x{}x{} R8 distance-field brick atlas ({} MiB pool) could not be created", spec.Width,
                 spec.Height, spec.Depth, m_PoolMiB );
        m_Atlas = std::move( atlas );
        LOG_INFO( "[DistanceField] brick atlas {}x{}x{} R8: {} brick(s), {} MiB (GlobalIllumination."
                  "DistanceFieldPoolMiB)",
                  spec.Width, spec.Height, spec.Depth, m_CapacityBricks,
                  static_cast<uint64_t>( m_CapacityBricks ) * Assets::kDistanceFieldBrickVoxels /
                       ( 1024u * 1024u ) );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DistanceFieldSceneData::FlushUploads()
    {
        if ( const auto atlas = EnsureAtlas(); !atlas )
            return atlas;
        if ( m_Pending.empty() )
            return BOOLSUCCESS;
        std::vector<Image3D::RegionWrite> writes;
        writes.reserve( m_Pending.size() );
        for ( const PendingBrick& p : m_Pending )
        {
            const auto& bytes = p.Field->Mips[p.Mip].BrickData;
            writes.push_back(
                 { .X      = ( p.Slot % kDfAtlasBricksX ) * kDfBrick,
                   .Y      = ( ( p.Slot / kDfAtlasBricksX ) % kDfAtlasBricksY ) * kDfBrick,
                   .Z      = ( p.Slot / kDfBricksPerSlice ) * kDfBrick,
                   .Width  = kDfBrick,
                   .Height = kDfBrick,
                   .Depth  = kDfBrick,
                   .Bytes  = bytes.data() + static_cast<size_t>( p.Brick ) * Assets::kDistanceFieldBrickVoxels,
                   .Size   = Assets::kDistanceFieldBrickVoxels } );
        }
        const auto wrote = m_Atlas->WriteRegions( writes );
        m_Pending.clear();
        if ( !wrote )
            return Common::MakeFormattedError<bool>( "distance-field brick atlas upload of {} bricks: {}",
                                                     writes.size(), wrote.GetError() );
        return BOOLSUCCESS;
    }

    void DistanceFieldSceneData::BeginViewFrame( const uint64_t frame, const uint32_t poolMiB )
    {
        if ( m_HasFrame && frame == m_Frame )
        {
            m_RequestedPoolMiB = std::max( m_RequestedPoolMiB, poolMiB );
            return;
        }
        // The scene's frame starts. The pool every view of the frame that ended asked for at most (a view's
        // request lands the frame after: views never resize the pool under one another within a frame).
        const bool     hadFrame = m_HasFrame;
        const uint32_t pool     = hadFrame ? std::max( m_RequestedPoolMiB, poolMiB ) : poolMiB;
        m_Frame                 = frame;
        m_HasFrame              = true;
        m_RequestedPoolMiB      = poolMiB;
        ReleaseUnused();
        if ( SetPoolMiB( pool ) )
            ++m_PoolGeneration;
        else if ( hadFrame )
        {
            // The streaming, once for the scene: toward the nearest distance any view acquired at last frame.
            m_UploadedThisFrame = 0;
            Stream();
            LogStreaming();
        }
        BeginFrame();
    }

    Common::BoolResultStr DistanceFieldSceneData::PublishViewFrame()
    {
        // The atlas and the buffers are the scene's rows, whichever view's scope is open.
        const ResourceAttributionScope scope( ResourceOwner::SceneRenderer, m_LedgerInstance );
        if ( !m_AssetBuffer || !m_IndirectionBuffer )
        {
            m_AssetBuffer =
                 ShaderResources::StorageBuffer::Create( "DFAssetData", 16, kMeshDistanceFieldAssetBinding, false );
            m_IndirectionBuffer = ShaderResources::StorageBuffer::Create( "DFIndirection", 16,
                                                                          kMeshDistanceFieldIndirectionBinding, false );
            if ( !m_AssetBuffer || !m_IndirectionBuffer )
            {
                m_AssetBuffer.reset();
                m_IndirectionBuffer.reset();
                return Common::MakeError<bool>(
                     "mesh distance fields: the asset / indirection buffers could not be created" );
            }
        }
        if ( const auto flushed = FlushUploads(); !flushed )
            return flushed;
        if ( m_HasPublished && m_PublishedFrame == m_Frame && m_PublishedRevision == m_Revision )
            return BOOLSUCCESS;
        if ( const auto r = UploadRecords( *m_AssetBuffer, m_AssetData, "asset data" ); !r )
            return r;
        if ( const auto r = UploadRecords( *m_IndirectionBuffer, m_Indirection, "indirection" ); !r )
            return r;
        m_HasPublished      = true;
        m_PublishedFrame    = m_Frame;
        m_PublishedRevision = m_Revision;
        return BOOLSUCCESS;
    }

    void DistanceFieldSceneData::LogStreaming()
    {
        const DistanceFieldStreamingStats& stats = m_Stats;
        if ( stats.UploadedBricks > 0 )
            m_StreamingMoved = true;
        // Settled = a frame that uploaded nothing; a field still wanting a finer mip then waits for room in the
        // pool.
        if ( !m_StreamingMoved || stats.UploadedBricks > 0 )
            return;
        m_StreamingMoved = false;
        const auto mib   = []( const uint64_t bricks )
        { return static_cast<double>( bricks * Assets::kDistanceFieldBrickVoxels ) / ( 1024.0 * 1024.0 ); };
        LOG_INFO(
             "[DistanceField] streaming settled: {} field(s) resident ({} coarse only, {} with 2 mips, {} with "
             "all 3); {:.1f} of {:.1f} MiB of bricks in use, {:.1f} MiB of them coarse; {} field(s) want a finer "
             "mip the pool has no room for",
             stats.Fields, stats.ResidentByMips[0], stats.ResidentByMips[1], stats.ResidentByMips[2],
             mib( stats.UsedBricks ), mib( stats.CapacityBricks ), mib( stats.CoarseBricks ),
             stats.UnmetRequests );
    }
} // namespace Desert::Graphic::DistanceField
