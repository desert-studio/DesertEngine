// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeFileFormatPng.cpp:36-145 and
// LandscapeFileFormatRaw.cpp:40-200, adapted: FImage / IImageWrapper become stb_image (decode) and a direct PNG
// writer over stb's deflate (stb_image_write writes 8-bit PNG only); UE's warnings for 8-bit and colour PNGs are
// refusals; the RAW .json sidecar is not ported; UE's FLandscapeImportData becomes ResultStr.

#include <Engine/World/Landscape/LandscapeHeightmapIO.hpp>

#include <Engine/Assets/ContainerBytes.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <system_error>

// stb_image_write's deflate, compiled in ThirdParty/stb/stb_image.cpp; its header declares it only inside the
// implementation section, so the one function the PNG writer needs is declared here.
extern "C" unsigned char* stbi_zlib_compress( unsigned char* data, int data_len, int* out_len, int quality );

namespace Desert::World::Landscape
{
    namespace
    {
        constexpr std::array<unsigned char, 8> kPngSignature = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        /// PNG filter type 1: each byte less the byte one PIXEL (two bytes at 16-bit grey) to its left. Terrain
        /// is smooth, so the differences are small and deflate well; the decoder undoes it exactly.
        constexpr unsigned char kPngFilterSub = 1u;
        /// stb_image_write's default PNG compression level.
        constexpr int kDeflateQuality = 8;

        void PutBigEndian32( std::vector<unsigned char>& out, uint32_t value )
        {
            out.push_back( static_cast<unsigned char>( value >> 24u ) );
            out.push_back( static_cast<unsigned char>( value >> 16u ) );
            out.push_back( static_cast<unsigned char>( value >> 8u ) );
            out.push_back( static_cast<unsigned char>( value ) );
        }

        void PutChunk( std::vector<unsigned char>&    out, const char ( &type )[5],
                       std::span<const unsigned char> data )
        {
            PutBigEndian32( out, static_cast<uint32_t>( data.size() ) );
            const size_t typeAt = out.size();
            out.insert( out.end(), type, type + 4 );
            out.insert( out.end(), data.begin(), data.end() );
            // The CRC covers the chunk type and its data, not the length.
            PutBigEndian32( out, Assets::Crc32( out.data() + typeAt, 4u + data.size() ) );
        }

        Common::BoolResultStr CheckMap( const LandscapeHeightmap& map, const char* what )
        {
            if ( map.Width == 0u || map.Height == 0u )
                return Common::MakeFormattedError<bool>( "heightmap {}: the map is empty ({} x {})", what,
                                                         map.Width, map.Height );
            const size_t want = static_cast<size_t>( map.Width ) * map.Height;
            if ( map.Samples.size() != want )
                return Common::MakeFormattedError<bool>( "heightmap {}: {} samples for a {} x {} map (needs {})",
                                                         what, map.Samples.size(), map.Width, map.Height, want );
            return Common::MakeSuccess( true );
        }

        std::string SizeText( uint32_t width, uint32_t height )
        {
            return std::to_string( width ) + " x " + std::to_string( height );
        }

        /// The two sizes along one side that @p quads allows around @p samples: k · quads + 1 below and above.
        std::string NearestSides( uint32_t samples, uint32_t quads )
        {
            const uint32_t below = samples <= 1u ? 1u : ( samples - 1u ) / quads;
            const uint32_t lo    = std::max( below, 1u ) * quads + 1u;
            const uint32_t hi    = ( below + 1u ) * quads + 1u;
            return lo == hi || lo > samples ? std::to_string( hi )
                                            : std::to_string( lo ) + " or " + std::to_string( hi );
        }
    } // namespace

    Common::ResultStr<LandscapeHeightmapFormat> LandscapeHeightmapFormatOf( const std::filesystem::path& path )
    {
        std::string ext = path.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        if ( ext == ".png" )
            return Common::MakeSuccess( LandscapeHeightmapFormat::Png16 );
        if ( ext == ".r16" || ext == ".raw" )
            return Common::MakeSuccess( LandscapeHeightmapFormat::Raw16 );
        return Common::MakeFormattedError<LandscapeHeightmapFormat>(
             "heightmap {}: '{}' is not a heightmap format (16-bit .png, or .r16 / .raw)", path.generic_string(),
             ext );
    }

    Common::ResultStr<std::vector<unsigned char>> EncodeLandscapeHeightmapPng( const LandscapeHeightmap& map )
    {
        if ( auto ok = CheckMap( map, "PNG export" ); !ok )
            return Common::MakeError<std::vector<unsigned char>>( ok.GetError() );

        // Scanlines: one filter byte, then the samples big-endian (PNG's byte order for 16-bit samples).
        const size_t               rowBytes = static_cast<size_t>( map.Width ) * 2u;
        std::vector<unsigned char> filtered( ( rowBytes + 1u ) * map.Height );
        std::vector<unsigned char> row( rowBytes );
        for ( uint32_t y = 0; y < map.Height; ++y )
        {
            for ( uint32_t x = 0; x < map.Width; ++x )
            {
                const uint16_t s = map.Samples[static_cast<size_t>( y ) * map.Width + x];
                row[x * 2u]      = static_cast<unsigned char>( s >> 8u );
                row[x * 2u + 1u] = static_cast<unsigned char>( s & 0xFFu );
            }
            unsigned char* out = filtered.data() + static_cast<size_t>( y ) * ( rowBytes + 1u );
            out[0]             = kPngFilterSub;
            for ( size_t i = 0; i < rowBytes; ++i )
                out[1u + i] = static_cast<unsigned char>( row[i] - ( i >= 2u ? row[i - 2u] : 0u ) );
        }

        int            zlibSize = 0;
        unsigned char* zlib = stbi_zlib_compress( filtered.data(), static_cast<int>( filtered.size() ), &zlibSize,
                                                  kDeflateQuality );
        if ( zlib == nullptr )
            return Common::MakeFormattedError<std::vector<unsigned char>>(
                 "heightmap PNG export: deflate failed on {} bytes of a {} map", filtered.size(),
                 SizeText( map.Width, map.Height ) );
        const std::vector<unsigned char> idat( zlib, zlib + zlibSize );
        std::free( zlib ); // NOLINT(cppcoreguidelines-no-malloc) — stb allocates with malloc.

        std::vector<unsigned char> ihdr;
        PutBigEndian32( ihdr, map.Width );
        PutBigEndian32( ihdr, map.Height );
        ihdr.push_back( 16u ); // bit depth
        ihdr.push_back( 0u );  // colour type 0: greyscale
        ihdr.push_back( 0u );  // compression: deflate
        ihdr.push_back( 0u );  // filter method 0
        ihdr.push_back( 0u );  // no interlace

        std::vector<unsigned char> png( kPngSignature.begin(), kPngSignature.end() );
        PutChunk( png, "IHDR", ihdr );
        PutChunk( png, "IDAT", idat );
        PutChunk( png, "IEND", {} );
        return Common::MakeSuccess( std::move( png ) );
    }

    Common::ResultStr<LandscapeHeightmap> DecodeLandscapeHeightmapPng( std::span<const unsigned char> bytes )
    {
        const int len = static_cast<int>( bytes.size() );
        int       w = 0, h = 0, channels = 0;
        if ( bytes.empty() || stbi_info_from_memory( bytes.data(), len, &w, &h, &channels ) == 0 )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "not a readable PNG ({} bytes): {}", bytes.size(),
                 stbi_failure_reason() ? stbi_failure_reason() : "unknown" );
        if ( stbi_is_16_bit_from_memory( bytes.data(), len ) == 0 )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "the PNG ({} x {}) is not 16-bit; an 8-bit heightmap has 256 levels and is refused, not widened",
                 w, h );
        if ( channels != 1 )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "the PNG ({} x {}) has {} channels; a heightmap is single-channel greyscale", w, h, channels );

        int       gotW = 0, gotH = 0, gotChannels = 0;
        uint16_t* pixels = stbi_load_16_from_memory( bytes.data(), len, &gotW, &gotH, &gotChannels, 1 );
        if ( pixels == nullptr )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "the PNG ({} x {}) could not be decoded: {}", w, h,
                 stbi_failure_reason() ? stbi_failure_reason() : "unknown" );
        LandscapeHeightmap map;
        map.Width  = static_cast<uint32_t>( gotW );
        map.Height = static_cast<uint32_t>( gotH );
        map.Samples.assign( pixels, pixels + static_cast<size_t>( gotW ) * static_cast<size_t>( gotH ) );
        stbi_image_free( pixels );
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::ResultStr<std::vector<unsigned char>> EncodeLandscapeHeightmapRaw( const LandscapeHeightmap& map )
    {
        if ( auto ok = CheckMap( map, "RAW export" ); !ok )
            return Common::MakeError<std::vector<unsigned char>>( ok.GetError() );
        std::vector<unsigned char> raw( map.Samples.size() * 2u );
        for ( size_t i = 0; i < map.Samples.size(); ++i )
        {
            raw[i * 2u]      = static_cast<unsigned char>( map.Samples[i] & 0xFFu );
            raw[i * 2u + 1u] = static_cast<unsigned char>( map.Samples[i] >> 8u );
        }
        return Common::MakeSuccess( std::move( raw ) );
    }

    Common::ResultStr<LandscapeHeightmap>
    DecodeLandscapeHeightmapRaw( std::span<const unsigned char>               bytes,
                                 const std::optional<LandscapeHeightmapSize>& expected )
    {
        if ( bytes.empty() || bytes.size() % 2u != 0u )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "a 16-bit RAW heightmap is two bytes a sample; this one is {} bytes", bytes.size() );
        const size_t       count = bytes.size() / 2u;
        LandscapeHeightmap map;
        if ( expected )
        {
            const size_t want = static_cast<size_t>( expected->Width ) * expected->Height;
            if ( count != want )
                return Common::MakeFormattedError<LandscapeHeightmap>(
                     "the RAW file has {} samples ({} bytes); the landscape is {} = {} samples ({} bytes)", count,
                     bytes.size(), SizeText( expected->Width, expected->Height ), want, want * 2u );
            map.Width  = expected->Width;
            map.Height = expected->Height;
        }
        else
        {
            // UE's GetRawResolution without a sidecar: the file is square.
            const auto side = static_cast<size_t>( std::llround( std::sqrt( static_cast<double>( count ) ) ) );
            if ( side * side != count )
                return Common::MakeFormattedError<LandscapeHeightmap>(
                     "the RAW file has {} samples ({} bytes), which is not a square; a RAW file carries no size, "
                     "so a "
                     "new landscape can only be made from a square one — use a 16-bit PNG",
                     count, bytes.size() );
            map.Width  = static_cast<uint32_t>( side );
            map.Height = static_cast<uint32_t>( side );
        }
        map.Samples.resize( count );
        for ( size_t i = 0; i < count; ++i )
            map.Samples[i] = static_cast<uint16_t>( bytes[i * 2u] | ( bytes[i * 2u + 1u] << 8u ) );
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::BoolResultStr WriteLandscapeHeightmapFile( const std::filesystem::path& path,
                                                       const LandscapeHeightmap&    map )
    {
        auto format = LandscapeHeightmapFormatOf( path );
        if ( !format )
            return Common::MakeError<bool>( format.GetError() );
        auto bytes = format.GetValue() == LandscapeHeightmapFormat::Png16 ? EncodeLandscapeHeightmapPng( map )
                                                                          : EncodeLandscapeHeightmapRaw( map );
        if ( !bytes )
            return Common::MakeFormattedError<bool>( "heightmap {}: {}", path.generic_string(), bytes.GetError() );

        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "heightmap {}: could not create its directory: {}",
                                                     path.generic_string(), ec.message() );
        const auto& blob = bytes.GetValue();
        if ( auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  path, std::as_bytes( std::span( blob.data(), blob.size() ) ) );
             !written )
            return Common::MakeFormattedError<bool>( "heightmap {}: {}", path.generic_string(),
                                                     written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<LandscapeHeightmap>
    ReadLandscapeHeightmapFile( const std::filesystem::path&                 path,
                                const std::optional<LandscapeHeightmapSize>& expected )
    {
        auto format = LandscapeHeightmapFormatOf( path );
        if ( !format )
            return Common::MakeError<LandscapeHeightmap>( format.GetError() );
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( path, ec ) )
            return Common::MakeFormattedError<LandscapeHeightmap>( "heightmap {}: no such file",
                                                                   path.generic_string() );
        auto bytes = Common::Utils::FileSystem::ReadByteFileContent( path );
        if ( !bytes )
            return Common::MakeFormattedError<LandscapeHeightmap>( "heightmap {}: {}", path.generic_string(),
                                                                   bytes.GetError() );
        const std::vector<uint8_t>           content = bytes.ExtractValue();
        const std::span<const unsigned char> span( content.data(), content.size() );

        auto map = format.GetValue() == LandscapeHeightmapFormat::Png16
                        ? DecodeLandscapeHeightmapPng( span )
                        : DecodeLandscapeHeightmapRaw( span, expected );
        if ( !map )
            return Common::MakeFormattedError<LandscapeHeightmap>( "heightmap {}: {}", path.generic_string(),
                                                                   map.GetError() );
        if ( expected && ( map.GetValue().Width != expected->Width || map.GetValue().Height != expected->Height ) )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "heightmap {}: the file is {} samples, the landscape is {}; the sizes must match (no resampling)",
                 path.generic_string(), SizeText( map.GetValue().Width, map.GetValue().Height ),
                 SizeText( expected->Width, expected->Height ) );
        return map;
    }

    LandscapeSampleBounds LandscapeTileRangeSamples( const LandscapeRoot& root, int32_t tileX1, int32_t tileZ1,
                                                     int32_t tileX2, int32_t tileZ2 )
    {
        const auto q = static_cast<int32_t>( root.QuadsPerTile );
        return { tileX1 * q, tileZ1 * q, ( tileX2 + 1 ) * q, ( tileZ2 + 1 ) * q };
    }

    Common::ResultStr<LandscapeHeightmap> ReadLandscapeHeightmap( const LandscapeRoot&         root,
                                                                  LandscapeTileLookup          lookup,
                                                                  const LandscapeSampleBounds& rect )
    {
        if ( rect.Empty() )
            return Common::MakeFormattedError<LandscapeHeightmap>(
                 "heightmap export: the rectangle ({}, {})..({}, {}) is empty", rect.X1, rect.Z1, rect.X2,
                 rect.Z2 );
        LandscapeHeightCache cache( root, std::move( lookup ) );
        if ( auto cached = cache.CacheData( rect.X1, rect.Z1, rect.X2, rect.Z2 ); !cached )
            return Common::MakeFormattedError<LandscapeHeightmap>( "heightmap export: {}", cached.GetError() );
        auto values = cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !values )
            return Common::MakeFormattedError<LandscapeHeightmap>( "heightmap export: {}", values.GetError() );
        LandscapeHeightmap map;
        map.Width   = static_cast<uint32_t>( rect.X2 - rect.X1 + 1 );
        map.Height  = static_cast<uint32_t>( rect.Z2 - rect.Z1 + 1 );
        map.Samples = values.ExtractValue();
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::ResultStr<LandscapeStrokeRecord> ImportLandscapeHeightmap( const LandscapeRoot&         root,
                                                                       LandscapeTileLookup          lookup,
                                                                       const LandscapeSampleBounds& rect,
                                                                       const LandscapeHeightmap&    map )
    {
        if ( auto ok = CheckMap( map, "import" ); !ok )
            return Common::MakeError<LandscapeStrokeRecord>( ok.GetError() );
        const auto width  = static_cast<uint32_t>( rect.X2 - rect.X1 + 1 );
        const auto height = static_cast<uint32_t>( rect.Z2 - rect.Z1 + 1 );
        if ( rect.Empty() || map.Width != width || map.Height != height )
            return Common::MakeFormattedError<LandscapeStrokeRecord>(
                 "heightmap import: the file is {} samples, the landscape is {}; the sizes must match (no "
                 "resampling)",
                 SizeText( map.Width, map.Height ),
                 rect.Empty() ? std::string( "empty" ) : SizeText( width, height ) );

        LandscapeHeightCache cache( root, std::move( lookup ) );
        if ( auto cached = cache.CacheData( rect.X1, rect.Z1, rect.X2, rect.Z2 ); !cached )
            return Common::MakeFormattedError<LandscapeStrokeRecord>( "heightmap import: {}", cached.GetError() );
        auto before = cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !before )
            return Common::MakeFormattedError<LandscapeStrokeRecord>( "heightmap import: {}", before.GetError() );
        if ( auto written = cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, map.Samples ); !written )
            return Common::MakeFormattedError<LandscapeStrokeRecord>( "heightmap import: {}", written.GetError() );

        LandscapeStrokeRecord record;
        record.Rect   = rect;
        record.Before = before.ExtractValue();
        record.After  = map.Samples;
        return Common::MakeSuccess( std::move( record ) );
    }

    Common::ResultStr<LandscapeGenerated> LandscapeFromHeightmap( const LandscapeGenerateSettings& frame,
                                                                  const LandscapeHeightmap&        map )
    {
        if ( auto ok = CheckMap( map, "new landscape" ); !ok )
            return Common::MakeError<LandscapeGenerated>( ok.GetError() );
        const uint32_t q = frame.QuadsPerTile;
        if ( std::find( std::begin( kLandscapeTileQuadsValues ), std::end( kLandscapeTileQuadsValues ), q ) ==
             std::end( kLandscapeTileQuadsValues ) )
            return Common::MakeFormattedError<LandscapeGenerated>(
                 "heightmap new landscape: {} quads a tile is not one of UE's sizes", q );
        const auto fits = []( uint32_t samples, uint32_t quads )
        { return samples > quads && ( samples - 1u ) % quads == 0u; };
        if ( !fits( map.Width, q ) || !fits( map.Height, q ) )
            return Common::MakeFormattedError<LandscapeGenerated>(
                 "heightmap new landscape: a {} map does not cut into tiles of {} quads — each side must be "
                 "a multiple of {} plus one (width: {}, height: {})",
                 SizeText( map.Width, map.Height ), q, q, NearestSides( map.Width, q ),
                 NearestSides( map.Height, q ) );

        LandscapeGenerateSettings settings = frame;
        settings.TilesX                    = static_cast<int32_t>( ( map.Width - 1u ) / q );
        settings.TilesZ                    = static_cast<int32_t>( ( map.Height - 1u ) / q );
        if ( ClampLandscapeTileCount( settings.TilesX, q ) != settings.TilesX ||
             ClampLandscapeTileCount( settings.TilesZ, q ) != settings.TilesZ )
            return Common::MakeFormattedError<LandscapeGenerated>(
                 "heightmap new landscape: a {} map is {} x {} tiles of {} quads, past UE's limit of {} quads a "
                 "side",
                 SizeText( map.Width, map.Height ), settings.TilesX, settings.TilesZ, q,
                 kLandscapeMaxQuadsPerSide );

        LandscapeGeneratedMap whole;
        whole.SamplesX = map.Width;
        whole.SamplesZ = map.Height;
        whole.Samples  = map.Samples;
        return CutLandscapeMap( settings, whole );
    }
} // namespace Desert::World::Landscape
