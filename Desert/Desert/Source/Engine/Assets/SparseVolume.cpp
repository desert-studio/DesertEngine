#include <Engine/Assets/SparseVolume.hpp>

#include <Common/Utilities/PakFile.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace Desert::Assets
{
    namespace
    {
        // THE PAYLOAD, little-endian (the engine's hosts are; the cloud noise payload makes the same
        // assumption):
        //   u32 magic "SVB1"   u32 brick size (16)   u32x3 resolution   u32x3 pages
        //   f32 peak density   f32 peak temperature  u32 brick count     u64 hash of everything after it
        //   u32 x pages: the page table         u16 x 2 x 18^3 x bricks: the bricks (RG halves)
        constexpr uint32_t kPayloadMagic      = Common::Content::FourCC( "SVB1" );
        constexpr size_t   kPayloadHeaderSize = 4u * 11u + 8u; // 11 u32 / f32 words, then the u64 hash

        template <typename T>
        void Put( std::vector<unsigned char>& out, const T value )
        {
            const size_t at = out.size();
            out.resize( at + sizeof( T ) );
            std::memcpy( out.data() + at, &value, sizeof( T ) );
        }

        template <typename T>
        T Take( const unsigned char* at )
        {
            T value;
            std::memcpy( &value, at, sizeof( T ) );
            return value;
        }

        Common::ResultStr<std::string> BuildPayloadString( std::span<const unsigned char>    source,
                                                           const SparseVolumeImportSettings& settings ) noexcept
        {
            auto grid = ParseVolGrid( source );
            if ( !grid )
                return Common::MakeError<std::string>( grid.GetError() );
            auto built = BuildSparseVolume( grid.GetValue(), settings );
            if ( !built )
                return Common::MakeError<std::string>( built.GetError() );
            const std::vector<unsigned char> payload = EncodeSparseVolumePayload( built.GetValue() );
            return Common::MakeSuccess( std::string( payload.begin(), payload.end() ) );
        }
    } // namespace

    Common::ResultStr<DenseVolumeGrid> ParseVolGrid( const std::span<const unsigned char> file )
    {
        // "VOL" 3, i32 encoding (1 = float32), i32 x y z, i32 channels, f32 x 6 bounding box, then the data.
        constexpr size_t kHeader = 3u + 1u + 4u * 5u + 4u * 6u;
        if ( file.size() < kHeader || file[0] != 'V' || file[1] != 'O' || file[2] != 'L' )
            return Common::MakeError<DenseVolumeGrid>( "not a .vol grid (no 'VOL' magic)" );
        if ( file[3] != 3u )
            return Common::MakeFormattedError<DenseVolumeGrid>( ".vol version {} (this importer reads version 3)",
                                                                static_cast<unsigned>( file[3] ) );
        const int32_t encoding = Take<int32_t>( file.data() + 4 );
        const int32_t x        = Take<int32_t>( file.data() + 8 );
        const int32_t y        = Take<int32_t>( file.data() + 12 );
        const int32_t z        = Take<int32_t>( file.data() + 16 );
        const int32_t channels = Take<int32_t>( file.data() + 20 );
        if ( encoding != 1 )
            return Common::MakeFormattedError<DenseVolumeGrid>(
                 ".vol encoding {} (this importer reads 1 = float32; half / uint8 grids are refused)", encoding );
        const int32_t maxRes = static_cast<int32_t>( kSparseVolumeMaxResolution );
        if ( x <= 0 || y <= 0 || z <= 0 || x > maxRes || y > maxRes || z > maxRes )
            return Common::MakeFormattedError<DenseVolumeGrid>(
                 ".vol resolution {}x{}x{} is outside 1..{} per axis", x, y, z, kSparseVolumeMaxResolution );
        if ( channels < 1 || channels > 16 )
            return Common::MakeFormattedError<DenseVolumeGrid>( ".vol states {} channels (1..16)", channels );
        const uint64_t voxels = uint64_t( x ) * uint64_t( y ) * uint64_t( z );
        const uint64_t need   = kHeader + voxels * uint64_t( channels ) * 4u;
        if ( file.size() < need )
            return Common::MakeFormattedError<DenseVolumeGrid>( ".vol is truncated: {} bytes, {} needed",
                                                                file.size(), need );

        DenseVolumeGrid grid;
        grid.Resolution = glm::uvec3( uint32_t( x ), uint32_t( y ), uint32_t( z ) );
        grid.Density.resize( voxels );
        if ( channels >= 2 )
            grid.Temperature.resize( voxels );
        const unsigned char* data = file.data() + kHeader;
        for ( uint64_t i = 0; i < voxels; ++i )
        {
            const unsigned char* voxel = data + i * uint64_t( channels ) * 4u;
            grid.Density[i]            = std::max( Take<float>( voxel ), 0.0f );
            if ( channels >= 2 )
                grid.Temperature[i] = std::max( Take<float>( voxel + 4 ), 0.0f );
        }
        return Common::MakeSuccess( std::move( grid ) );
    }

    Common::ResultStr<SparseVolumeData> BuildSparseVolume( const DenseVolumeGrid&            grid,
                                                           const SparseVolumeImportSettings& settings )
    {
        const glm::uvec3 res    = grid.Resolution;
        const uint64_t   voxels = uint64_t( res.x ) * res.y * res.z;
        if ( voxels == 0 || grid.Density.size() != voxels )
            return Common::MakeFormattedError<SparseVolumeData>(
                 "the grid states {}x{}x{} voxels but carries {} densities", res.x, res.y, res.z,
                 grid.Density.size() );
        if ( !grid.Temperature.empty() && grid.Temperature.size() != voxels )
            return Common::MakeFormattedError<SparseVolumeData>( "the grid carries {} temperatures for {} voxels",
                                                                 grid.Temperature.size(), voxels );

        float peakD = 0.0f;
        float peakT = 0.0f;
        for ( uint64_t i = 0; i < voxels; ++i )
        {
            if ( std::isfinite( grid.Density[i] ) )
                peakD = std::max( peakD, grid.Density[i] );
            if ( !grid.Temperature.empty() && std::isfinite( grid.Temperature[i] ) )
                peakT = std::max( peakT, grid.Temperature[i] );
        }
        if ( peakD <= 0.0f )
            return Common::MakeError<SparseVolumeData>( "the grid holds no medium (every density is 0)" );

        SparseVolumeData out;
        out.Resolution      = res;
        out.Pages           = ( res + glm::uvec3( kSparseVolumeBrickSize - 1u ) ) / kSparseVolumeBrickSize;
        out.PeakDensity     = peakD;
        out.PeakTemperature = peakT;
        out.PageTable.assign( out.PageCount(), 0u );

        const float threshold = std::max( settings.EmptyThreshold, 0.0f ) * peakD;
        const auto  index     = [&]( const int32_t x, const int32_t y, const int32_t z ) -> uint64_t
        {
            const uint32_t cx = uint32_t( std::clamp( x, 0, int32_t( res.x ) - 1 ) );
            const uint32_t cy = uint32_t( std::clamp( y, 0, int32_t( res.y ) - 1 ) );
            const uint32_t cz = uint32_t( std::clamp( z, 0, int32_t( res.z ) - 1 ) );
            return uint64_t( cx ) + uint64_t( res.x ) * ( uint64_t( cy ) + uint64_t( res.y ) * cz );
        };
        const auto finite01 = []( const float v, const float peak ) -> float
        { return std::isfinite( v ) && peak > 0.0f ? std::clamp( v / peak, 0.0f, 1.0f ) : 0.0f; };

        const int32_t b = int32_t( kSparseVolumeBrickSize );
        for ( uint32_t pz = 0; pz < out.Pages.z; ++pz )
            for ( uint32_t py = 0; py < out.Pages.y; ++py )
                for ( uint32_t px = 0; px < out.Pages.x; ++px )
                {
                    const int32_t ox = int32_t( px ) * b - 1;
                    const int32_t oy = int32_t( py ) * b - 1;
                    const int32_t oz = int32_t( pz ) * b - 1;
                    // EMPTY when the page AND its apron hold nothing above the threshold: the trilinear filter
                    // at a neighbour's edge reads this page's border voxels only through that neighbour's apron.
                    bool occupied = false;
                    for ( int32_t z = 0; z < b + 2 && !occupied; ++z )
                        for ( int32_t y = 0; y < b + 2 && !occupied; ++y )
                            for ( int32_t x = 0; x < b + 2; ++x )
                                if ( grid.Density[index( ox + x, oy + y, oz + z )] > threshold )
                                {
                                    occupied = true;
                                    break;
                                }
                    if ( !occupied )
                        continue;
                    const size_t brick = out.Bricks.size() / ( kSparseVolumeBrickVoxels * 2u );
                    out.Bricks.resize( out.Bricks.size() + kSparseVolumeBrickVoxels * 2u );
                    uint16_t* texels = out.Bricks.data() + brick * kSparseVolumeBrickVoxels * 2u;
                    for ( int32_t z = 0; z < b + 2; ++z )
                        for ( int32_t y = 0; y < b + 2; ++y )
                            for ( int32_t x = 0; x < b + 2; ++x )
                            {
                                const uint64_t src = index( ox + x, oy + y, oz + z );
                                const float    d   = finite01( grid.Density[src], peakD );
                                const float    t =
                                     grid.Temperature.empty() ? 0.0f : finite01( grid.Temperature[src], peakT );
                                *texels++ = glm::packHalf1x16( d );
                                *texels++ = glm::packHalf1x16( t );
                            }
                    out.PageTable[px + out.Pages.x * ( py + uint64_t( out.Pages.y ) * pz )] =
                         static_cast<uint32_t>( brick + 1u );
                }
        if ( out.Bricks.empty() )
            return Common::MakeError<SparseVolumeData>(
                 "every page of the grid is below the empty threshold; lower EmptyThreshold" );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<SparseVolumeData> ImportSparseVolumeCached( const std::span<const unsigned char> source,
                                                                  const SparseVolumeImportSettings&    settings )
    {
        const uint64_t sourceHash = Common::Utils::PakContentHash( source.data(), source.size() );
        const float    threshold  = settings.EmptyThreshold;
        const uint64_t key =
             Common::DDC::MakeKey( kSparseVolumeDeriver, sourceHash, &threshold, sizeof( threshold ) );
        auto derived = Common::DDC::GetOrBuild( kSparseVolumeDeriver, key, [source, settings]() noexcept
                                                { return BuildPayloadString( source, settings ); } );
        if ( !derived )
            return Common::MakeFormattedError<SparseVolumeData>( "sparse volume import: {}", derived.GetError() );
        const std::string& bytes = derived.GetValue();
        return DecodeSparseVolumePayload( std::span<const unsigned char>(
             reinterpret_cast<const unsigned char*>( bytes.data() ), bytes.size() ) );
    }

    std::vector<unsigned char> EncodeSparseVolumePayload( const SparseVolumeData& data )
    {
        std::vector<unsigned char> body;
        body.reserve( data.PageTable.size() * 4u + data.Bricks.size() * 2u );
        for ( const uint32_t entry : data.PageTable )
            Put( body, entry );
        const size_t brickBytes = data.Bricks.size() * sizeof( uint16_t );
        body.resize( body.size() + brickBytes );
        std::memcpy( body.data() + body.size() - brickBytes, data.Bricks.data(), brickBytes );

        std::vector<unsigned char> out;
        out.reserve( kPayloadHeaderSize + body.size() );
        Put( out, kPayloadMagic );
        Put( out, kSparseVolumeBrickSize );
        Put( out, data.Resolution.x );
        Put( out, data.Resolution.y );
        Put( out, data.Resolution.z );
        Put( out, data.Pages.x );
        Put( out, data.Pages.y );
        Put( out, data.Pages.z );
        Put( out, data.PeakDensity );
        Put( out, data.PeakTemperature );
        Put( out, data.BrickCount() );
        Put( out, Common::Utils::PakContentHash( body.data(), body.size() ) );
        out.insert( out.end(), body.begin(), body.end() );
        return out;
    }

    Common::ResultStr<SparseVolumeData> DecodeSparseVolumePayload( const std::span<const unsigned char> bytes )
    {
        constexpr size_t kHeader = kPayloadHeaderSize;
        if ( bytes.size() < kHeader )
            return Common::MakeFormattedError<SparseVolumeData>(
                 "sparse volume payload of {} bytes (header is {})", bytes.size(), kHeader );
        const unsigned char* p = bytes.data();
        if ( Take<uint32_t>( p ) != kPayloadMagic )
            return Common::MakeError<SparseVolumeData>( "sparse volume payload has no 'SVB1' magic" );
        if ( Take<uint32_t>( p + 4 ) != kSparseVolumeBrickSize )
            return Common::MakeFormattedError<SparseVolumeData>(
                 "sparse volume bricks are {}^3 voxels, this build reads {}^3", Take<uint32_t>( p + 4 ),
                 kSparseVolumeBrickSize );
        SparseVolumeData data;
        data.Resolution =
             glm::uvec3( Take<uint32_t>( p + 8 ), Take<uint32_t>( p + 12 ), Take<uint32_t>( p + 16 ) );
        data.Pages = glm::uvec3( Take<uint32_t>( p + 20 ), Take<uint32_t>( p + 24 ), Take<uint32_t>( p + 28 ) );
        data.PeakDensity      = Take<float>( p + 32 );
        data.PeakTemperature  = Take<float>( p + 36 );
        const uint32_t bricks = Take<uint32_t>( p + 40 );
        const uint64_t hash   = Take<uint64_t>( p + 44 );

        const glm::uvec3 expectPages =
             ( data.Resolution + glm::uvec3( kSparseVolumeBrickSize - 1u ) ) / kSparseVolumeBrickSize;
        if ( glm::any( glm::equal( data.Resolution, glm::uvec3( 0u ) ) ) ||
             glm::any( glm::greaterThan( data.Resolution, glm::uvec3( kSparseVolumeMaxResolution ) ) ) ||
             data.Pages != expectPages )
            return Common::MakeFormattedError<SparseVolumeData>(
                 "sparse volume states {}x{}x{} voxels in {}x{}x{} pages", data.Resolution.x, data.Resolution.y,
                 data.Resolution.z, data.Pages.x, data.Pages.y, data.Pages.z );
        if ( !( data.PeakDensity > 0.0f ) || !std::isfinite( data.PeakDensity ) ||
             !( data.PeakTemperature >= 0.0f ) )
            return Common::MakeFormattedError<SparseVolumeData>( "sparse volume peaks {} / {} K are not usable",
                                                                 data.PeakDensity, data.PeakTemperature );
        const uint64_t pages = data.PageCount();
        const uint64_t need  = kHeader + pages * 4u + uint64_t( bricks ) * kSparseVolumeBrickBytes;
        if ( bytes.size() != need )
            return Common::MakeFormattedError<SparseVolumeData>(
                 "sparse volume payload is {} bytes; {} pages and {} bricks need {}", bytes.size(), pages, bricks,
                 need );
        if ( Common::Utils::PakContentHash( p + kHeader, bytes.size() - kHeader ) != hash )
            return Common::MakeError<SparseVolumeData>( "sparse volume payload hash disagrees (corrupt file)" );

        data.PageTable.resize( pages );
        std::memcpy( data.PageTable.data(), p + kHeader, pages * 4u );
        for ( const uint32_t entry : data.PageTable )
            if ( entry > bricks )
                return Common::MakeFormattedError<SparseVolumeData>(
                     "sparse volume page table names brick {} of {}", entry - 1u, bricks );
        data.Bricks.resize( uint64_t( bricks ) * kSparseVolumeBrickVoxels * 2u );
        std::memcpy( data.Bricks.data(), p + kHeader + pages * 4u, data.Bricks.size() * sizeof( uint16_t ) );
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<std::vector<unsigned char>> EncodeSparseVolume( const SparseVolumeData& data )
    {
        namespace CC                             = Common::Content;
        const std::vector<unsigned char> payload = EncodeSparseVolumePayload( data );

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = CC::ContentKind::SparseVolume;
        envelope.Asset.Guid       = data.Guid.IsNull() ? CC::AssetGuid::Generate() : data.Guid;
        envelope.Asset.Subsystems = { { kSparseVolumeSubsystemTag, kSparseVolumeContainerVersion } };
        const std::span<const std::byte> payloadBytes = std::as_bytes( std::span( payload ) );
        envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( payloadBytes.begin(), payloadBytes.end() ) } );
        auto file = CC::WriteAssetEnvelope( envelope );
        if ( !file )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "cannot wrap sparse volume: {}",
                                                                           file.GetError() );
        const std::vector<std::byte>& wrapped = file.GetValue();
        std::vector<unsigned char>    out( wrapped.size() );
        std::memcpy( out.data(), wrapped.data(), wrapped.size() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<SparseVolumeData> DecodeSparseVolume( const std::vector<unsigned char>& file )
    {
        namespace CC                        = Common::Content;
        const CC::SubsystemVersion kKnown[] = { { kSparseVolumeSubsystemTag, kSparseVolumeContainerVersion } };
        auto                       envelope =
             CC::ReadAssetEnvelope( std::as_bytes( std::span( file ) ), CC::AssetHeaderReadContext{ kKnown } );
        if ( !envelope )
            return Common::MakeFormattedError<SparseVolumeData>( "not a sparse volume envelope: {}",
                                                                 envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( e.Asset.Kind != CC::ContentKind::SparseVolume )
            return Common::MakeFormattedError<SparseVolumeData>( "the envelope's kind is {}, not SparseVolume",
                                                                 CC::KindName( e.Asset.Kind ) );
        const auto section = std::find_if( e.Sections.begin(), e.Sections.end(),
                                           []( const auto& s ) { return s.Tag == CC::EnvelopeSection::Payload; } );
        if ( section == e.Sections.end() )
            return Common::MakeError<SparseVolumeData>( "the sparse volume envelope has no PAYL section" );
        auto decoded = DecodeSparseVolumePayload( std::span<const unsigned char>(
             reinterpret_cast<const unsigned char*>( section->Bytes.data() ), section->Bytes.size() ) );
        if ( !decoded )
            return decoded;
        SparseVolumeData data = decoded.ExtractValue();
        data.Guid             = e.Asset.Guid;
        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets
