#include <Engine/Graphic/DistanceField/DistanceFieldSceneData.hpp>

#include <Engine/Graphic/Image.hpp>

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

        // The mips a field wants at @p distance (cm) from the camera: all of them inside the finest global
        // clipmap, one fewer per clipmap level farther out, never fewer than the coarse one. The finest mip is
        // detail only the finest clipmap (and the near-camera traces that start in it) can resolve.
        uint32_t DfWantedMips( const float distance )
        {
            uint32_t level  = 0;
            float    extent = DFShared::GDF_CLIPMAP0_EXTENT;
            while ( level + 1 < Assets::kDistanceFieldNumMips && distance > extent )
            {
                ++level;
                extent *= 2.0f;
            }
            return Assets::kDistanceFieldNumMips - level;
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

    std::optional<uint32_t> RangeAllocator::Allocate( const uint32_t count, const uint32_t limit )
    {
        for ( auto it = m_Free.begin(); it != m_Free.end() && it->first + count <= limit; ++it )
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
        if ( static_cast<uint64_t>( m_End ) + count > limit )
            return std::nullopt;
        const uint32_t offset = m_End;
        m_End += count;
        return offset;
    }

    uint32_t RangeAllocator::Used() const
    {
        uint32_t freeSlots = 0;
        for ( const auto& [offset, count] : m_Free )
            freeSlots += count;
        return m_End - freeSlots;
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

    DistanceFieldSceneData::DistanceFieldSceneData()  = default;
    DistanceFieldSceneData::~DistanceFieldSceneData() = default;

    bool DistanceFieldSceneData::SetPoolMiB( const uint32_t poolMiB )
    {
        if ( poolMiB == m_PoolMiB )
            return false;
        // Everything resident lived in the old atlas: drop it all, the next frame acquires again.
        m_Resident.clear();
        m_BrickSlots       = {};
        m_IndirectionSlots = {};
        m_AssetSlots       = {};
        m_AssetData.clear();
        m_Indirection.clear();
        m_Pending.clear();
        m_Atlas.reset();
        m_PoolMiB = poolMiB;
        const uint64_t slices =
             std::clamp<uint64_t>( static_cast<uint64_t>( poolMiB ) * 1024u * 1024u / kDfBytesPerSlice, 1u,
                                   kDfMaxAtlasDepthTexel / kDfBrick );
        m_CapacityBricks = static_cast<uint32_t>( slices ) * kDfBricksPerSlice;
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
    DistanceFieldSceneData::Acquire( const std::shared_ptr<const Assets::MeshDistanceFieldData>& field,
                                     const float                                                 distance )
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
        while ( !added && EvictFartherThan( -1.0f, coarseBricks, nullptr ) )
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

    bool DistanceFieldSceneData::AddMip( Resident& resident, const Assets::MeshDistanceFieldData& field )
    {
        const uint32_t                        r      = resident.NumMips;
        const uint32_t                        source = Assets::kDistanceFieldNumMips - 1 - r;
        const Assets::SparseDistanceFieldMip& mip    = field.Mips[source];
        const auto                            bricks = m_BrickSlots.Allocate( mip.BrickCount(), m_CapacityBricks );
        if ( !bricks )
            return false;

        ResidentMip& slot      = resident.Mips[r];
        slot.BrickOffset       = *bricks;
        slot.BrickCount        = mip.BrickCount();
        slot.IndirectionCount  = static_cast<uint32_t>( mip.IndirectionTable.size() );
        slot.IndirectionOffset = m_IndirectionSlots.Allocate( slot.IndirectionCount );
        if ( m_Indirection.size() < m_IndirectionSlots.End() )
            m_Indirection.resize( m_IndirectionSlots.End(), Assets::kDistanceFieldInvalidBrick );
        for ( size_t i = 0; i < mip.IndirectionTable.size(); ++i )
        {
            const uint32_t local = mip.IndirectionTable[i];
            m_Indirection[slot.IndirectionOffset + i] =
                 local == Assets::kDistanceFieldInvalidBrick ? local : slot.BrickOffset + local;
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

        const std::shared_ptr<const Assets::MeshDistanceFieldData> held = resident.Field.lock();
        for ( uint32_t b = 0; b < slot.BrickCount; ++b )
            m_Pending.push_back( { slot.BrickOffset + b, held, source, b } );
        // The bricks are written by FlushUploads before any pass of this frame reads the record.
        ++resident.NumMips;
        record[0] = glm::vec4( DfBits( resident.NumMips ), 0.0f, 0.0f, 0.0f );
        return true;
    }

    void DistanceFieldSceneData::DropMip( Resident& resident )
    {
        const uint32_t r        = --resident.NumMips;
        glm::vec4*     record   = &m_AssetData[static_cast<size_t>( resident.AssetIndex ) * kDfAssetStride];
        record[0]               = glm::vec4( DfBits( resident.NumMips ), 0.0f, 0.0f, 0.0f );
        const ResidentMip& slot = resident.Mips[r];
        std::fill_n( m_Indirection.begin() + slot.IndirectionOffset, slot.IndirectionCount,
                     Assets::kDistanceFieldInvalidBrick );
        m_IndirectionSlots.Free( slot.IndirectionOffset, slot.IndirectionCount );
        m_BrickSlots.Free( slot.BrickOffset, slot.BrickCount );
        std::erase_if( m_Pending, [&slot]( const PendingBrick& p )
                       { return p.Slot >= slot.BrickOffset && p.Slot < slot.BrickOffset + slot.BrickCount; } );
        resident.Mips[r] = {};
    }

    void DistanceFieldSceneData::Release( Resident& resident )
    {
        while ( resident.NumMips > 0 )
            DropMip( resident );
        m_AssetSlots.Free( resident.AssetIndex, 1 );
    }

    bool DistanceFieldSceneData::EvictFartherThan( const float distance, const uint32_t bricks,
                                                   const Resident* except )
    {
        std::vector<Resident*> holders;
        for ( auto& [key, resident] : m_Resident )
            if ( &resident != except && resident.NumMips > 1 && resident.Distance > distance )
                holders.push_back( &resident );
        std::sort( holders.begin(), holders.end(),
                   []( const Resident* a, const Resident* b ) { return a->Distance > b->Distance; } );
        uint32_t freed = 0;
        for ( Resident* holder : holders )
        {
            while ( holder->NumMips > 1 && freed < bricks )
            {
                freed += holder->Mips[holder->NumMips - 1].BrickCount;
                DropMip( *holder );
            }
            if ( freed >= bricks )
                break;
        }
        // Freed slots need not be contiguous: the caller retries the allocation.
        return freed > 0;
    }

    void DistanceFieldSceneData::Stream()
    {
        // Out first: a field farther than its finer mips are worth gives them back.
        std::vector<std::pair<Resident*, std::shared_ptr<const Assets::MeshDistanceFieldData>>> requests;
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
            while ( !added && EvictFartherThan( resident->Distance, bricks, resident ) )
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
            m_Stats.CoarseBricks += resident.Mips[0].BrickCount;
            if ( resident.NumMips > 0 )
                ++m_Stats.ResidentByMips[resident.NumMips - 1];
        }
    }

    uint32_t DistanceFieldSceneData::ResidentMips( const Assets::MeshDistanceFieldData* field ) const
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
} // namespace Desert::Graphic::DistanceField
