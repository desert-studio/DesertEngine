// LS-11: UE's heightmap Import / Export — 16-bit greyscale PNG and little-endian RAW (.r16 / .raw).
//
// What must hold:
//   1. export → import is bit for bit, for PNG and for RAW, through real files, over the full uint16 range;
//   2. the values are the stored samples unchanged (UE's encoding, 32768 = height zero) and RAW is little-endian;
//   3. a file of the wrong size is refused naming both sizes — never cropped, padded or resampled — and an 8-bit
//      or colour PNG is refused rather than widened;
//   4. an import into a landscape replaces every stored copy of a seam sample, and its record undoes it exactly;
//   5. a new landscape from a map has the map's tile grid and exports back to the same map.

#include <Editor/Import/LandscapeHeightmapIO.hpp>

#include <gtest/gtest.h>

// stb_image_write's implementation is compiled into ThirdParty/stb/stb_image.cpp; used here only to make the 8-bit
// and colour PNGs that must be refused.
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace Desert::World::Landscape;

namespace
{
    class TempDir
    {
    public:
        TempDir()
        {
            m_Path = std::filesystem::temp_directory_path() /
                     ( "desert_ls11_" +
                       std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
            std::filesystem::create_directories( m_Path );
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Path, ec );
        }
        TempDir( const TempDir& )            = delete;
        TempDir& operator=( const TempDir& ) = delete;

        std::filesystem::path operator/( const std::string& name ) const
        {
            return m_Path / name;
        }

    private:
        std::filesystem::path m_Path;
    };

    /// Every value class a heightmap can hold: both extremes, height zero, and a hashed spread in between —
    /// neighbouring samples differ in both bytes, so a filter or byte-order slip changes the result.
    LandscapeHeightmap MakeMap( uint32_t width, uint32_t height )
    {
        LandscapeHeightmap map;
        map.Width  = width;
        map.Height = height;
        map.Samples.resize( static_cast<size_t>( width ) * height );
        for ( size_t i = 0; i < map.Samples.size(); ++i )
            map.Samples[i] = static_cast<uint16_t>( ( i * 40503u + 17u ) & 0xFFFFu );
        map.Samples[0]                       = 0u;
        map.Samples[1]                       = 65535u;
        map.Samples[2]                       = kLandscapeMidSample;
        map.Samples[map.Samples.size() - 1u] = 65535u;
        return map;
    }

    /// A landscape of tiles in memory, addressed like the editor's (LandscapeEditTarget's lookup).
    struct Landscape
    {
        LandscapeRoot                                            Root;
        std::map<std::pair<int32_t, int32_t>, LandscapeTileData> Tiles;
        /// The root's stack: the Base layer a new landscape has (kLandscapeBaseEditLayerGuid).
        LandscapeEditLayerStack Stack{ { { Common::UUID( kLandscapeBaseEditLayerGuid ), "Base" } } };

        /// What the import writes: layer @p guid of Stack (the Base by default).
        [[nodiscard]] LandscapeEditLayerTarget Editing( uint64_t guid = kLandscapeBaseEditLayerGuid ) const
        {
            return { Stack, {}, Common::UUID( guid ) };
        }

        LandscapeTileLookup Lookup()
        {
            return [this]( int32_t tx, int32_t tz ) -> LandscapeTileSlot
            {
                const auto it = Tiles.find( { tx, tz } );
                if ( it == Tiles.end() )
                    return { LandscapeTileState::Absent, nullptr };
                return { LandscapeTileState::Present, &it->second };
            };
        }
    };

    /// @p tilesX x @p tilesZ tiles of 7 quads, flat at height zero.
    Landscape MakeLandscape( int32_t tilesX, int32_t tilesZ )
    {
        Landscape l;
        l.Root.QuadsPerTile = 7u;
        for ( int32_t tz = 0; tz < tilesZ; ++tz )
            for ( int32_t tx = 0; tx < tilesX; ++tx )
            {
                auto made = LandscapeTileData::Create( 8u, 8u );
                auto tile = made.ExtractValue();
                // The Base layer holds no heights yet: mid, which is the flat tile's merge.
                EXPECT_TRUE( tile.SetEditLayer( { Common::UUID( kLandscapeBaseEditLayerGuid ), {}, {} } ) );
                l.Tiles.emplace( std::make_pair( tx, tz ), std::move( tile ) );
            }
        return l;
    }

} // namespace

TEST( LandscapeHeightmapIO, PngRoundTripIsBitForBitThroughAFile )
{
    TempDir                  dir;
    const LandscapeHeightmap map  = MakeMap( 129u, 65u );
    const auto               path = dir / "terrain.png";
    ASSERT_TRUE( WriteLandscapeHeightmapFile( path, map ).IsSuccess() );
    auto back = ReadLandscapeHeightmapFile( path, LandscapeHeightmapSize{ 129u, 65u } );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Width, 129u );
    EXPECT_EQ( back.GetValue().Height, 65u );
    EXPECT_EQ( back.GetValue().Samples, map.Samples );
}

TEST( LandscapeHeightmapIO, PngIsSixteenBitGreyscaleAsStbReadsIt )
{
    const LandscapeHeightmap map = MakeMap( 5u, 3u );
    auto                     png = EncodeLandscapeHeightmapPng( map );
    ASSERT_TRUE( png.IsSuccess() );
    const auto& b = png.GetValue();
    // IHDR right after the signature: width, height, bit depth 16, colour type 0.
    ASSERT_GT( b.size(), 33u );
    EXPECT_EQ( b[8 + 4 + 4 + 8], 16u );
    EXPECT_EQ( b[8 + 4 + 4 + 9], 0u );
    int       w = 0, h = 0, n = 0;
    uint16_t* px = stbi_load_16_from_memory( b.data(), static_cast<int>( b.size() ), &w, &h, &n, 0 );
    ASSERT_NE( px, nullptr );
    EXPECT_EQ( w, 5 );
    EXPECT_EQ( h, 3 );
    EXPECT_EQ( n, 1 );
    // Pixel (x, y) is sample y · Width + x: image X is world X, image rows run along +Z.
    EXPECT_EQ( px[2 * 5 + 4], map.Samples[2 * 5 + 4] );
    stbi_image_free( px );
}

TEST( LandscapeHeightmapIO, RawRoundTripIsBitForBitAndLittleEndian )
{
    TempDir            dir;
    LandscapeHeightmap map = MakeMap( 33u, 17u );
    map.Samples[3]         = 0x1234u;
    auto raw               = EncodeLandscapeHeightmapRaw( map );
    ASSERT_TRUE( raw.IsSuccess() );
    ASSERT_EQ( raw.GetValue().size(), 33u * 17u * 2u );
    EXPECT_EQ( raw.GetValue()[6], 0x34u );
    EXPECT_EQ( raw.GetValue()[7], 0x12u );

    for ( const char* name : { "terrain.r16", "terrain.RAW" } )
    {
        const auto path = dir / name;
        ASSERT_TRUE( WriteLandscapeHeightmapFile( path, map ).IsSuccess() ) << name;
        auto back = ReadLandscapeHeightmapFile( path, LandscapeHeightmapSize{ 33u, 17u } );
        ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
        EXPECT_EQ( back.GetValue().Samples, map.Samples ) << name;
    }
}

TEST( LandscapeHeightmapIO, RawWithoutASizeIsASquareAsUeInfersIt )
{
    const LandscapeHeightmap square    = MakeMap( 15u, 15u );
    const auto               squareRaw = EncodeLandscapeHeightmapRaw( square );
    const auto               oblongRaw = EncodeLandscapeHeightmapRaw( MakeMap( 15u, 8u ) );
    auto                     back      = DecodeLandscapeHeightmapRaw( squareRaw.GetValue(), {} );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Width, 15u );
    EXPECT_EQ( back.GetValue().Samples, square.Samples );

    auto refused = DecodeLandscapeHeightmapRaw( oblongRaw.GetValue(), {} );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "120 samples (240 bytes)" ), std::string::npos ) << refused.GetError();
}

TEST( LandscapeHeightmapIO, AFileOfTheWrongSizeIsRefusedNamingBothSizes )
{
    TempDir                  dir;
    const LandscapeHeightmap map = MakeMap( 16u, 15u );
    for ( const char* name : { "wrong.png", "wrong.r16" } )
    {
        const auto path = dir / name;
        ASSERT_TRUE( WriteLandscapeHeightmapFile( path, map ).IsSuccess() );
        auto refused = ReadLandscapeHeightmapFile( path, LandscapeHeightmapSize{ 15u, 15u } );
        ASSERT_FALSE( refused.IsSuccess() ) << name;
        const std::string& e = refused.GetError();
        EXPECT_NE( e.find( path.generic_string() ), std::string::npos ) << e;
        EXPECT_NE( e.find( "15 x 15" ), std::string::npos ) << e;
        EXPECT_NE( e.find( std::string( name ).ends_with( ".png" ) ? "16 x 15" : "240 samples" ),
                   std::string::npos )
             << e;
    }
}

TEST( LandscapeHeightmapIO, AMissingFileOrAnUnknownExtensionIsRefusedWithThePath )
{
    TempDir    dir;
    const auto missing = dir / "absent.png";
    auto       refused = ReadLandscapeHeightmapFile( missing, {} );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( missing.generic_string() ), std::string::npos ) << refused.GetError();

    auto unknown = WriteLandscapeHeightmapFile( dir / "terrain.tga", MakeMap( 4u, 4u ) );
    ASSERT_FALSE( unknown.IsSuccess() );
    EXPECT_NE( unknown.GetError().find( ".tga" ), std::string::npos ) << unknown.GetError();
}

TEST( LandscapeHeightmapIO, EightBitAndColourPngsAreRefusedNotWidened )
{
    TempDir                    dir;
    std::vector<unsigned char> grey8( 4u * 4u, 128u );
    std::vector<unsigned char> rgb8( 4u * 4u * 3u, 128u );
    ASSERT_NE( stbi_write_png( ( dir / "grey8.png" ).string().c_str(), 4, 4, 1, grey8.data(), 4 ), 0 );
    ASSERT_NE( stbi_write_png( ( dir / "rgb8.png" ).string().c_str(), 4, 4, 3, rgb8.data(), 12 ), 0 );
    auto grey = ReadLandscapeHeightmapFile( dir / "grey8.png", {} );
    auto rgb  = ReadLandscapeHeightmapFile( dir / "rgb8.png", {} );
    ASSERT_FALSE( grey.IsSuccess() );
    ASSERT_FALSE( rgb.IsSuccess() );
    EXPECT_NE( grey.GetError().find( "not 16-bit" ), std::string::npos ) << grey.GetError();
    EXPECT_NE( rgb.GetError().find( "not 16-bit" ), std::string::npos ) << rgb.GetError();
}

TEST( LandscapeHeightmapIO, ExportImportOfALandscapeRoundTripsAndSeamsAgree )
{
    Landscape  l    = MakeLandscape( 2, 2 );
    const auto rect = LandscapeTileRangeSamples( l.Root, 0, 0, 1, 1 );
    EXPECT_EQ( rect.X2 - rect.X1 + 1, 15 );

    const LandscapeHeightmap imported = MakeMap( 15u, 15u );
    auto                     record = ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, imported, l.Editing() );
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    EXPECT_EQ( record.GetValue().Before, std::vector<uint16_t>( 225u, kLandscapeMidSample ) );
    EXPECT_EQ( record.GetValue().After, imported.Samples );

    // The shared column x = 7 lives in tile (0, 0) as its last column and in tile (1, 0) as its first.
    for ( uint32_t z = 0; z < 8u; ++z )
    {
        EXPECT_EQ( l.Tiles.at( { 0, 0 } ).Sample( 7u, z ), imported.Samples[z * 15u + 7u] ) << z;
        EXPECT_EQ( l.Tiles.at( { 1, 0 } ).Sample( 0u, z ), imported.Samples[z * 15u + 7u] ) << z;
    }

    auto exported = ReadLandscapeHeightmap( l.Root, l.Lookup(), rect );
    ASSERT_TRUE( exported.IsSuccess() ) << exported.GetError();
    EXPECT_EQ( exported.GetValue().Samples, imported.Samples );

    // Through both files: landscape → PNG → landscape is the same bits.
    TempDir dir;
    for ( const char* name : { "l.png", "l.r16" } )
    {
        ASSERT_TRUE( WriteLandscapeHeightmapFile( dir / name, exported.GetValue() ).IsSuccess() );
        Landscape fresh = MakeLandscape( 2, 2 );
        auto      read  = ReadLandscapeHeightmapFile( dir / name, LandscapeHeightmapSize{ 15u, 15u } );
        ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
        ASSERT_TRUE( ImportLandscapeHeightmap( fresh.Root, fresh.Lookup(), rect, read.GetValue(), fresh.Editing() )
                          .IsSuccess() );
        for ( const auto& [key, tile] : l.Tiles )
            EXPECT_EQ( fresh.Tiles.at( key ).Samples(), tile.Samples() ) << name;
    }
}

TEST( LandscapeHeightmapIO, SelectedTilesExportTheirOwnRectangle )
{
    Landscape l = MakeLandscape( 2, 2 );
    ASSERT_TRUE( ImportLandscapeHeightmap( l.Root, l.Lookup(), LandscapeTileRangeSamples( l.Root, 0, 0, 1, 1 ),
                                           MakeMap( 15u, 15u ), l.Editing() )
                      .IsSuccess() );
    auto column = ReadLandscapeHeightmap( l.Root, l.Lookup(), LandscapeTileRangeSamples( l.Root, 1, 0, 1, 1 ) );
    ASSERT_TRUE( column.IsSuccess() ) << column.GetError();
    EXPECT_EQ( column.GetValue().Width, 8u );
    EXPECT_EQ( column.GetValue().Height, 15u );
    const LandscapeHeightmap whole = MakeMap( 15u, 15u );
    for ( uint32_t z = 0; z < 15u; ++z )
        for ( uint32_t x = 0; x < 8u; ++x )
            ASSERT_EQ( column.GetValue().Samples[z * 8u + x], whole.Samples[z * 15u + 7u + x] ) << x << "," << z;
}

TEST( LandscapeHeightmapIO, AnImportOfTheWrongShapeWritesNothing )
{
    Landscape  l    = MakeLandscape( 2, 2 );
    const auto rect = LandscapeTileRangeSamples( l.Root, 1, 0, 1, 1 ); // 8 x 15
    // 15 x 8 has the same sample count: only the shape tells it apart.
    auto refused = ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, MakeMap( 15u, 8u ), l.Editing() );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "15 x 8" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "8 x 15" ), std::string::npos ) << refused.GetError();
    for ( const auto& [key, tile] : l.Tiles )
        EXPECT_EQ( tile.Samples(), std::vector<uint16_t>( 64u, kLandscapeMidSample ) );
}

TEST( LandscapeHeightmapIO, TheImportRecordUndoesAndRedoesExactly )
{
    Landscape  l    = MakeLandscape( 2, 2 );
    const auto rect = LandscapeTileRangeSamples( l.Root, 0, 0, 1, 1 );
    ASSERT_TRUE(
         ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, MakeMap( 15u, 15u ), l.Editing() ).IsSuccess() );
    const auto original = l.Tiles;

    auto record = ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, MakeMap( 15u, 15u ), l.Editing() );
    ASSERT_TRUE( record.IsSuccess() );
    LandscapeHeightmap second = MakeMap( 15u, 15u );
    for ( auto& s : second.Samples )
        s = static_cast<uint16_t>( 65535u - s );
    record = ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, second, l.Editing() );
    ASSERT_TRUE( record.IsSuccess() );

    // Undo: the heights before the import, in every tile, bit for bit.
    ASSERT_TRUE(
         WriteLandscapeHeights( l.Root, l.Lookup(), rect, record.GetValue().Before, l.Editing() ).IsSuccess() );
    for ( const auto& [key, tile] : original )
        EXPECT_EQ( l.Tiles.at( key ).Samples(), tile.Samples() );
    // Redo: the imported map again.
    ASSERT_TRUE(
         WriteLandscapeHeights( l.Root, l.Lookup(), rect, record.GetValue().After, l.Editing() ).IsSuccess() );
    auto again = ReadLandscapeHeightmap( l.Root, l.Lookup(), rect );
    ASSERT_TRUE( again.IsSuccess() );
    EXPECT_EQ( again.GetValue().Samples, second.Samples );
}

TEST( LandscapeHeightmapIO, ANewLandscapeFromAMapHasItsTileGridAndExportsTheSameMap )
{
    LandscapeGenerateSettings frame;
    frame.QuadsPerTile            = 7u;
    const LandscapeHeightmap map  = MakeMap( 15u, 22u );
    auto                     made = LandscapeFromHeightmap( frame, map );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    Landscape l;
    l.Root = made.GetValue().Root;
    for ( const auto& t : made.GetValue().Tiles )
        l.Tiles.emplace( std::make_pair( t.TileX, t.TileZ ), t.Heights );
    EXPECT_EQ( l.Tiles.size(), 6u ); // 2 x 3
    // A new landscape has one edit layer, Base, and every tile is its merge (UE: a new ALandscape's layer 0).
    ASSERT_EQ( made.GetValue().EditLayers.Layers.size(), 1u );
    EXPECT_EQ( made.GetValue().EditLayers.Layers[0].Name, "Base" );
    l.Stack = made.GetValue().EditLayers;
    for ( auto& [key, tile] : l.Tiles )
    {
        const auto* base = tile.FindEditLayer( l.Stack.Layers[0].Guid );
        ASSERT_NE( base, nullptr );
        EXPECT_EQ( base->Heights, tile.Samples() );
        const auto samples = tile.Samples();
        ASSERT_TRUE( MergeLandscapeEditLayers( l.Stack, {}, tile.Bounds(), tile ) );
        EXPECT_EQ( tile.Samples(), samples );
    }
    auto back = ReadLandscapeHeightmap( l.Root, l.Lookup(), LandscapeTileRangeSamples( l.Root, 0, 0, 1, 2 ) );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Samples, map.Samples );

    auto refused = LandscapeFromHeightmap( frame, MakeMap( 16u, 15u ) );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "width: 15 or 22" ), std::string::npos ) << refused.GetError();
}

TEST( LandscapeHeightmapIO, ThePngOfARealTerrainIsSmallerThanItsRaw )
{
    // A smooth ramp: the Sub filter turns it into near-constant bytes. Guards that the PNG is actually deflated.
    LandscapeHeightmap ramp;
    ramp.Width  = 257u;
    ramp.Height = 257u;
    ramp.Samples.resize( 257u * 257u );
    for ( uint32_t z = 0; z < 257u; ++z )
        for ( uint32_t x = 0; x < 257u; ++x )
            ramp.Samples[z * 257u + x] = static_cast<uint16_t>( 20000u + x * 7u + z * 3u );
    auto png = EncodeLandscapeHeightmapPng( ramp );
    ASSERT_TRUE( png.IsSuccess() );
    EXPECT_LT( png.GetValue().size(), 257u * 257u * 2u / 4u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

TEST( LandscapeHeightmapIO, AnImportIntoTheSecondLayerLeavesTheBaseAndTheTileIsTheMerge )
{
    constexpr uint64_t kTop  = 7u;
    Landscape          l     = MakeLandscape( 2, 2 );
    const auto         rect  = LandscapeTileRangeSamples( l.Root, 0, 0, 1, 1 );
    LandscapeHeightmap first = MakeMap( 15u, 15u );
    for ( auto& s : first.Samples )
        s = static_cast<uint16_t>( 20000u + s % 20000u );
    ASSERT_TRUE( ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, first, l.Editing() ).IsSuccess() );
    l.Stack.Layers.push_back( { Common::UUID( kTop ), "Top" } );
    std::map<std::pair<int32_t, int32_t>, std::vector<uint16_t>> base;
    for ( const auto& [key, tile] : l.Tiles )
        base[key] = tile.FindEditLayer( Common::UUID( kLandscapeBaseEditLayerGuid ) )->Heights;

    LandscapeHeightmap delta = MakeMap( 15u, 15u );
    for ( auto& s : delta.Samples )
        s = static_cast<uint16_t>( kLandscapeMidSample - 1000u + s % 2000u );
    auto record = ImportLandscapeHeightmap( l.Root, l.Lookup(), rect, delta, l.Editing( kTop ) );
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    EXPECT_EQ( record.GetValue().Before, std::vector<uint16_t>( 225u, kLandscapeMidSample ) );
    for ( const auto& [key, tile] : l.Tiles )
    {
        EXPECT_EQ( tile.FindEditLayer( Common::UUID( kLandscapeBaseEditLayerGuid ) )->Heights, base.at( key ) );
        const auto* top = tile.FindEditLayer( Common::UUID( kTop ) );
        ASSERT_NE( top, nullptr );
        for ( uint32_t z = 0; z < 8u; ++z )
            for ( uint32_t x = 0; x < 8u; ++x )
            {
                const size_t i = static_cast<size_t>( key.second * 7 + static_cast<int32_t>( z ) ) * 15u +
                                 static_cast<size_t>( key.first * 7 + static_cast<int32_t>( x ) );
                EXPECT_EQ( top->Heights[z * 8u + x], delta.Samples[i] );
                EXPECT_EQ( tile.Sample( x, z ), first.Samples[i] + delta.Samples[i] - kLandscapeMidSample )
                     << x << ", " << z;
            }
    }
    // Undo with the same layer gives the Top layer back its mid plane and the tiles the Base alone.
    ASSERT_TRUE( WriteLandscapeHeights( l.Root, l.Lookup(), rect, record.GetValue().Before, l.Editing( kTop ) ) );
    auto back = ReadLandscapeHeightmap( l.Root, l.Lookup(), rect );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Samples, first.Samples );
}
