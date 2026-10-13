#include <Engine/Graphic/DistanceField/DistanceFieldSceneData.hpp>

#include <Engine/Graphic/Image.hpp>

#include <algorithm>
#include <bit>
#include <format>

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

    DistanceFieldSceneData::DistanceFieldSceneData()  = default;
    DistanceFieldSceneData::~DistanceFieldSceneData() = default;

    Common::ResultStr<std::optional<uint32_t>>
    DistanceFieldSceneData::Acquire( const std::shared_ptr<const Assets::MeshDistanceFieldData>& field )
    {
        using Result = std::optional<uint32_t>;
        if ( !field )
            return Common::MakeSuccess<Result>( std::nullopt );
        if ( const auto it = m_Resident.find( field.get() ); it != m_Resident.end() )
        {
            if ( it->second.Field.lock() == field )
                return Common::MakeSuccess<Result>( it->second.AssetIndex );
            // A new field at the address of one that died since the last sweep.
            Release( it->second );
            m_Resident.erase( it );
        }
        if ( field->Mips[0].IndirectionDimensions.x == 0 )
            return Common::MakeSuccess<Result>( std::nullopt );

        Resident resident;
        resident.Field      = field;
        resident.AssetIndex = m_AssetSlots.Allocate( 1 );
        if ( m_AssetData.size() < static_cast<size_t>( m_AssetSlots.End() ) * kDfAssetStride )
            m_AssetData.resize( static_cast<size_t>( m_AssetSlots.End() ) * kDfAssetStride, glm::vec4( 0.0f ) );
        glm::vec4* record = &m_AssetData[static_cast<size_t>( resident.AssetIndex ) * kDfAssetStride];
        record[0]         = glm::vec4( DfBits( Assets::kDistanceFieldNumMips ), 0.0f, 0.0f, 0.0f );
        for ( uint32_t m = 0; m < Assets::kDistanceFieldNumMips; ++m )
        {
            const Assets::SparseDistanceFieldMip& mip = field->Mips[m];
            resident.BrickOffset[m]                   = m_BrickSlots.Allocate( mip.BrickCount() );
            resident.IndirectionOffset[m] =
                 m_IndirectionSlots.Allocate( static_cast<uint32_t>( mip.IndirectionTable.size() ) );
            if ( m_Indirection.size() < m_IndirectionSlots.End() )
                m_Indirection.resize( m_IndirectionSlots.End(), Assets::kDistanceFieldInvalidBrick );
            for ( size_t i = 0; i < mip.IndirectionTable.size(); ++i )
            {
                const uint32_t local = mip.IndirectionTable[i];
                m_Indirection[resident.IndirectionOffset[m] + i] =
                     local == Assets::kDistanceFieldInvalidBrick ? local : resident.BrickOffset[m] + local;
            }
            // UE FDFAssetData: the volume -> indirection transform is the virtual UV one scaled by the grid.
            const glm::vec3 dims  = glm::vec3( mip.IndirectionDimensions );
            const glm::vec3 scale = mip.VolumeToVirtualUVScale * dims;
            const glm::vec3 add   = mip.VolumeToVirtualUVAdd * dims;
            glm::vec4*      rows  = record + 1 + m * DFShared::DF_ASSET_MIP_STRIDE;
            rows[0] = glm::vec4( DfBits( mip.IndirectionDimensions.x ), DfBits( mip.IndirectionDimensions.y ),
                                 DfBits( mip.IndirectionDimensions.z ), DfBits( resident.IndirectionOffset[m] ) );
            rows[1] = glm::vec4( mip.DistanceFieldToVolumeScaleBias, scale.x, scale.y );
            rows[2] = glm::vec4( scale.z, add );
        }
        QueueBricks( *field, resident );
        const uint32_t index = resident.AssetIndex;
        m_Resident.emplace( field.get(), std::move( resident ) );
        return Common::MakeSuccess<Result>( index );
    }

    void DistanceFieldSceneData::QueueBricks( const Assets::MeshDistanceFieldData& field,
                                              const Resident&                      resident )
    {
        const std::shared_ptr<const Assets::MeshDistanceFieldData> held = resident.Field.lock();
        for ( uint32_t m = 0; m < Assets::kDistanceFieldNumMips; ++m )
            for ( uint32_t b = 0; b < field.Mips[m].BrickCount(); ++b )
                m_Pending.push_back( { resident.BrickOffset[m] + b, held, m, b } );
    }

    void DistanceFieldSceneData::Release( const Resident& resident )
    {
        // The record and the table range are released by size; the field may already be gone, so the sizes are
        // read back from the record this class wrote rather than from the field.
        const glm::vec4* record = &m_AssetData[static_cast<size_t>( resident.AssetIndex ) * kDfAssetStride];
        for ( uint32_t m = 0; m < Assets::kDistanceFieldNumMips; ++m )
        {
            const glm::vec4 rows0 = record[1 + m * DFShared::DF_ASSET_MIP_STRIDE];
            const uint32_t  cells = std::bit_cast<uint32_t>( rows0.x ) * std::bit_cast<uint32_t>( rows0.y ) *
                                   std::bit_cast<uint32_t>( rows0.z );
            m_IndirectionSlots.Free( resident.IndirectionOffset[m], cells );
            // Brick counts: the slots of mip m run up to the next mip's, or are counted from the field.
            uint32_t bricks = 0;
            for ( uint32_t c = 0; c < cells; ++c )
                if ( const uint32_t slot = m_Indirection[resident.IndirectionOffset[m] + c];
                     slot != Assets::kDistanceFieldInvalidBrick )
                    bricks = std::max( bricks, slot - resident.BrickOffset[m] + 1 );
            m_BrickSlots.Free( resident.BrickOffset[m], bricks );
            for ( uint32_t c = 0; c < cells; ++c )
                m_Indirection[resident.IndirectionOffset[m] + c] = Assets::kDistanceFieldInvalidBrick;
        }
        m_AssetSlots.Free( resident.AssetIndex, 1 );
        std::erase_if( m_Pending, [&resident]( const PendingBrick& p )
                       { return p.Field == resident.Field.lock(); } );
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

    Common::BoolResultStr DistanceFieldSceneData::EnsureAtlasCapacity()
    {
        const uint32_t neededDepth =
             std::max<uint32_t>( 1, ( m_BrickSlots.End() + kDfBricksPerSlice - 1 ) / kDfBricksPerSlice );
        if ( m_Atlas && neededDepth <= m_AtlasDepthInBricks )
            return BOOLSUCCESS;
        uint32_t depth = std::max<uint32_t>( 4, m_AtlasDepthInBricks );
        while ( depth < neededDepth )
            depth *= 2;
        if ( depth * kDfBrick > kDfMaxAtlasDepthTexel )
        {
            if ( neededDepth * kDfBrick > kDfMaxAtlasDepthTexel )
                return Common::MakeFormattedError<bool>(
                     "the distance-field brick atlas needs {} bricks, more than the {} a {}x{}x{} R8 volume holds",
                     m_BrickSlots.End(), kDfBricksPerSlice * ( kDfMaxAtlasDepthTexel / kDfBrick ),
                     kDfAtlasBricksX * kDfBrick, kDfAtlasBricksY * kDfBrick, kDfMaxAtlasDepthTexel );
            depth = kDfMaxAtlasDepthTexel / kDfBrick;
        }
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "DistanceFieldBrickAtlas",
             .Width      = kDfAtlasBricksX * kDfBrick,
             .Height     = kDfAtlasBricksY * kDfBrick,
             .Depth      = depth * kDfBrick,
             .Format     = Core::Formats::ImageFormat::R8_UNORM,
             .Data       = Core::Formats::EmptyPixelData{},
             .Properties = Core::Formats::Sample,
        };
        auto atlas = Image3D::Create( spec );
        if ( !atlas )
            return Common::MakeFormattedError<bool>(
                 "the {}x{}x{} R8 distance-field brick atlas could not be created", spec.Width, spec.Height,
                 spec.Depth );
        // A new image starts empty: every resident brick is written again.
        const bool regrown   = m_Atlas != nullptr;
        m_Atlas              = std::move( atlas );
        m_AtlasDepthInBricks = depth;
        if ( regrown )
        {
            m_Pending.clear();
            for ( const auto& [key, resident] : m_Resident )
                if ( const auto field = resident.Field.lock() )
                    QueueBricks( *field, resident );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DistanceFieldSceneData::FlushUploads()
    {
        if ( const auto capacity = EnsureAtlasCapacity(); !capacity )
            return capacity;
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
