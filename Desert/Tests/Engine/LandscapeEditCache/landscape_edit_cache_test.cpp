// L2: the landscape height edit cache, ported from UE's TLandscapeEditCache (LandscapeEdModeTools.h).
//
//   1. A rectangle read through the cache across a 2x2 block of tiles equals the heights read straight from
//      the tiles; the expected values come from the generator that filled the tiles, not from the cache.
//   2. A write reads back through the cache AND through every tile directly, and every stored copy of a seam
//      sample (two on an edge, four on a corner) holds the one value written.
//   3. The changed-tile list is exactly the tiles whose stored samples the write covered, and those tiles'
//      dirty rectangles — what the GPU upload and the Jolt heightfield consume — are set, others are not.
//   4. A write that touches an unloaded tile, or a sample no tile stores, is refused and changes nothing.

#include <Engine/World/Landscape/LandscapeEditCache.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

using namespace Desert::World::Landscape;

namespace
{
    constexpr uint32_t kQuads = 7u;
    constexpr int32_t  kQ     = static_cast<int32_t>( kQuads );

    /// A height that differs at every global sample, so a sample read from the wrong place is visible.
    uint16_t Height( int32_t gx, int32_t gz )
    {
        return static_cast<uint16_t>( 20000 + gx * 37 + gz * 1009 );
    }

    struct World
    {
        LandscapeRoot                                            Root;
        std::map<std::pair<int32_t, int32_t>, LandscapeTileData> Tiles;
        std::set<std::pair<int32_t, int32_t>>                    Unloaded;

        World()
        {
            Root.QuadsPerTile = kQuads;
            for ( int32_t tz = 0; tz < 2; ++tz )
                for ( int32_t tx = 0; tx < 2; ++tx )
                {
                    std::vector<uint16_t> samples;
                    for ( int32_t z = 0; z <= kQ; ++z )
                        for ( int32_t x = 0; x <= kQ; ++x )
                            samples.push_back( Height( tx * kQ + x, tz * kQ + z ) );
                    auto tile = LandscapeTileData::FromSamples( kQuads + 1u, kQuads + 1u, std::move( samples ) );
                    EXPECT_TRUE( tile.IsSuccess() );
                    LandscapeTileData data = tile.GetValue();
                    data.TakeDirtyRects( LandscapeDirtyConsumer::Gpu );
                    data.TakeDirtyRects( LandscapeDirtyConsumer::Physics );
                    Tiles.emplace( std::make_pair( tx, tz ), std::move( data ) );
                }
        }

        LandscapeTileLookup Lookup()
        {
            return [this]( int32_t tx, int32_t tz ) -> LandscapeTileSlot
            {
                const auto key = std::make_pair( tx, tz );
                if ( Unloaded.count( key ) )
                    return { LandscapeTileState::Unloaded, nullptr };
                auto it = Tiles.find( key );
                if ( it == Tiles.end() )
                    return { LandscapeTileState::Absent, nullptr };
                return { LandscapeTileState::Present, &it->second };
            };
        }

        uint16_t Direct( int32_t tx, int32_t tz, int32_t gx, int32_t gz )
        {
            return Tiles.at( { tx, tz } )
                 .Sample( static_cast<uint32_t>( gx - tx * kQ ), static_cast<uint32_t>( gz - tz * kQ ) );
        }

        bool Dirty( int32_t tx, int32_t tz )
        {
            const LandscapeTileData& t   = Tiles.at( { tx, tz } );
            const bool               gpu = !t.DirtyRects( LandscapeDirtyConsumer::Gpu ).empty();
            EXPECT_EQ( gpu, !t.DirtyRects( LandscapeDirtyConsumer::Physics ).empty() );
            return gpu;
        }
    };

    std::set<std::pair<int32_t, int32_t>> TilesOf( const std::vector<LandscapeChangedTile>& changed )
    {
        std::set<std::pair<int32_t, int32_t>> out;
        for ( const LandscapeChangedTile& c : changed )
            EXPECT_TRUE( out.insert( { c.TileX, c.TileZ } ).second ) << "a tile listed twice";
        return out;
    }

    std::vector<uint16_t> Pattern( int32_t x1, int32_t z1, int32_t x2, int32_t z2 )
    {
        std::vector<uint16_t> v;
        for ( int32_t z = z1; z <= z2; ++z )
            for ( int32_t x = x1; x <= x2; ++x )
                v.push_back( static_cast<uint16_t>( 50000 + x * 3 + z * 101 ) );
        return v;
    }
} // namespace

TEST( LandscapeEditCache, ReadAcrossFourTilesEqualsDirectRead )
{
    World                w;
    LandscapeHeightCache cache( w.Root, w.Lookup() );
    ASSERT_TRUE( cache.CacheData( 0, 0, 2 * kQ, 2 * kQ ).IsSuccess() );
    auto read = cache.GetCachedData( 3, 2, 11, 12 );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const std::vector<uint16_t>& got = read.GetValue();
    ASSERT_EQ( got.size(), 9u * 11u );
    for ( int32_t z = 2; z <= 12; ++z )
        for ( int32_t x = 3; x <= 11; ++x )
        {
            const uint16_t v = got[static_cast<size_t>( ( z - 2 ) * 9 + ( x - 3 ) )];
            EXPECT_EQ( v, Height( x, z ) ) << x << "," << z;
            EXPECT_EQ( v, w.Direct( std::min( x / kQ, 1 ), std::min( z / kQ, 1 ), x, z ) ) << x << "," << z;
        }
}

TEST( LandscapeEditCache, WriteReadsBackAndEverySeamCopyAgrees )
{
    World                w;
    LandscapeHeightCache cache( w.Root, w.Lookup() );
    ASSERT_TRUE( cache.CacheData( 4, 4, 10, 10 ).IsSuccess() );
    const auto values = Pattern( 4, 4, 10, 10 );
    auto       set    = cache.SetCachedData( 4, 4, 10, 10, values );
    ASSERT_TRUE( set.IsSuccess() ) << set.GetError();

    auto back = cache.GetCachedData( 4, 4, 10, 10 );
    ASSERT_TRUE( back.IsSuccess() );
    EXPECT_EQ( back.GetValue(), values );

    // Every tile that stores a global sample holds the written value: two copies on the x=7 and z=7 seams,
    // four at the (7, 7) corner.
    for ( int32_t z = 4; z <= 10; ++z )
        for ( int32_t x = 4; x <= 10; ++x )
        {
            const uint16_t expected = values[static_cast<size_t>( ( z - 4 ) * 7 + ( x - 4 ) )];
            int            copies   = 0;
            for ( int32_t tz = 0; tz < 2; ++tz )
                for ( int32_t tx = 0; tx < 2; ++tx )
                    if ( x >= tx * kQ && x <= tx * kQ + kQ && z >= tz * kQ && z <= tz * kQ + kQ )
                    {
                        EXPECT_EQ( w.Direct( tx, tz, x, z ), expected ) << "tile " << tx << "," << tz;
                        ++copies;
                    }
            EXPECT_EQ( copies, ( x == kQ ? 2 : 1 ) * ( z == kQ ? 2 : 1 ) );
        }
    // Outside the rectangle nothing moved.
    EXPECT_EQ( w.Direct( 0, 0, 3, 4 ), Height( 3, 4 ) );
    EXPECT_EQ( w.Direct( 1, 1, 11, 11 ), Height( 11, 11 ) );
}

TEST( LandscapeEditCache, ChangedTilesAreExactlyTheTouchedOnes )
{
    struct Case
    {
        int32_t                               X1, Z1, X2, Z2;
        std::set<std::pair<int32_t, int32_t>> Expected;
    };
    const Case cases[] = {
         { 1, 1, 3, 3, { { 0, 0 } } },
         { 8, 1, 10, 3, { { 1, 0 } } },
         { 7, 2, 7, 2, { { 0, 0 }, { 1, 0 } } },                     // one sample on the X seam
         { 2, 7, 3, 7, { { 0, 0 }, { 0, 1 } } },                     // the Z seam
         { 7, 7, 7, 7, { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } } }, // the corner
         { 8, 8, 14, 14, { { 1, 1 } } },
    };
    for ( const Case& c : cases )
    {
        World                w;
        LandscapeHeightCache cache( w.Root, w.Lookup() );
        ASSERT_TRUE( cache.CacheData( c.X1, c.Z1, c.X2, c.Z2 ).IsSuccess() );
        ASSERT_TRUE(
             cache.SetCachedData( c.X1, c.Z1, c.X2, c.Z2, Pattern( c.X1, c.Z1, c.X2, c.Z2 ) ).IsSuccess() );
        EXPECT_EQ( TilesOf( cache.ChangedTiles() ), c.Expected ) << c.X1 << "," << c.Z1;
        for ( int32_t tz = 0; tz < 2; ++tz )
            for ( int32_t tx = 0; tx < 2; ++tx )
                EXPECT_EQ( w.Dirty( tx, tz ), c.Expected.count( { tx, tz } ) == 1u ) << "tile " << tx << "," << tz;
        EXPECT_EQ( cache.TakeChangedTiles().size(), c.Expected.size() );
        EXPECT_TRUE( cache.ChangedTiles().empty() );
    }
}

TEST( LandscapeEditCache, ChangedTileRectangleIsTheLocalUnion )
{
    World                w;
    LandscapeHeightCache cache( w.Root, w.Lookup() );
    ASSERT_TRUE( cache.CacheData( 0, 0, 7, 7 ).IsSuccess() );
    ASSERT_TRUE( cache.SetCachedData( 1, 1, 2, 2, Pattern( 1, 1, 2, 2 ) ).IsSuccess() );
    ASSERT_TRUE( cache.SetCachedData( 5, 4, 7, 6, Pattern( 5, 4, 7, 6 ) ).IsSuccess() );
    ASSERT_EQ( cache.ChangedTiles().size(), 2u );
    const LandscapeChangedTile& own = cache.ChangedTiles()[0];
    EXPECT_EQ( std::make_pair( own.TileX, own.TileZ ), std::make_pair( 0, 0 ) );
    EXPECT_EQ( own.Samples.X0, 1u );
    EXPECT_EQ( own.Samples.Z0, 1u );
    EXPECT_EQ( own.Samples.X1, 8u );
    EXPECT_EQ( own.Samples.Z1, 7u );
    const LandscapeChangedTile& east = cache.ChangedTiles()[1];
    EXPECT_EQ( std::make_pair( east.TileX, east.TileZ ), std::make_pair( 1, 0 ) );
    EXPECT_EQ( east.Samples.X0, 0u );
    EXPECT_EQ( east.Samples.X1, 1u );
}

TEST( LandscapeEditCache, WriteTouchingAnUnloadedTileIsRefusedAndChangesNothing )
{
    World                w;
    LandscapeHeightCache cache( w.Root, w.Lookup() );
    ASSERT_TRUE( cache.CacheData( 3, 3, 9, 9 ).IsSuccess() );
    w.Unloaded.insert( { 1, 1 } );
    // The corner (7, 7) is stored in tile (1, 1) as well: writing it would leave that copy stale.
    auto set = cache.SetCachedData( 5, 5, 7, 7, Pattern( 5, 5, 7, 7 ) );
    ASSERT_FALSE( set.IsSuccess() );
    EXPECT_NE( set.GetError().find( "(1, 1)" ), std::string::npos ) << set.GetError();
    EXPECT_TRUE( cache.ChangedTiles().empty() );
    for ( int32_t tz = 0; tz < 2; ++tz )
        for ( int32_t tx = 0; tx < 2; ++tx )
            EXPECT_FALSE( w.Dirty( tx, tz ) );
    EXPECT_EQ( w.Direct( 0, 0, 5, 5 ), Height( 5, 5 ) );
    auto kept = cache.GetCachedData( 5, 5, 5, 5 );
    ASSERT_TRUE( kept.IsSuccess() );
    EXPECT_EQ( kept.GetValue()[0], Height( 5, 5 ) ) << "the cache moved";
    // One sample away from the unloaded tile the same write goes through.
    EXPECT_TRUE( cache.SetCachedData( 5, 5, 6, 6, Pattern( 5, 5, 6, 6 ) ).IsSuccess() );
    // A sample stored only by the unloaded tile cannot even be cached.
    LandscapeHeightCache fresh( w.Root, w.Lookup() );
    EXPECT_FALSE( fresh.CacheData( 8, 8, 8, 8 ).IsSuccess() );
}

TEST( LandscapeEditCache, LandscapeEdgeIsWritableAndBeyondItIsRefused )
{
    World w;
    w.Tiles.erase( { 1, 1 } ); // an L-shaped landscape: (1, 1) does not exist
    LandscapeHeightCache cache( w.Root, w.Lookup() );
    // Row x = 14 is tile (1, 0)'s east edge; its "other side" is off the landscape.
    ASSERT_TRUE( cache.CacheData( 12, 0, 14, 7 ).IsSuccess() );
    ASSERT_TRUE( cache.SetCachedData( 13, 0, 14, 6, Pattern( 13, 0, 14, 6 ) ).IsSuccess() );
    EXPECT_EQ( w.Direct( 1, 0, 14, 3 ), Pattern( 13, 0, 14, 6 )[3 * 2 + 1] );
    // The corner (7, 7) exists in three tiles; the missing fourth is the landscape's edge, not a refusal.
    ASSERT_TRUE( cache.CacheData( 7, 7, 7, 7 ).IsSuccess() );
    ASSERT_TRUE( cache.SetCachedData( 7, 7, 7, 7, std::vector<uint16_t>{ 123 } ).IsSuccess() );
    EXPECT_EQ( w.Direct( 0, 0, 7, 7 ), 123 );
    EXPECT_EQ( w.Direct( 1, 0, 7, 7 ), 123 );
    EXPECT_EQ( w.Direct( 0, 1, 7, 7 ), 123 );
    // Off the landscape: x = 15 and the interior of the missing tile.
    EXPECT_FALSE( cache.CacheData( 13, 0, 15, 2 ).IsSuccess() );
    LandscapeHeightCache fresh( w.Root, w.Lookup() );
    auto                 off = fresh.CacheData( 9, 9, 10, 10 );
    ASSERT_FALSE( off.IsSuccess() );
    EXPECT_NE( off.GetError().find( "(9, 9)" ), std::string::npos ) << off.GetError();
}

TEST( LandscapeEditCache, ExtendingKeepsWrittenValuesAndRefusesOutsideTheCache )
{
    World                w;
    LandscapeHeightCache cache( w.Root, w.Lookup() );
    ASSERT_TRUE( cache.CacheData( 5, 5, 6, 6 ).IsSuccess() );
    ASSERT_TRUE( cache.SetCachedData( 5, 5, 6, 6, Pattern( 5, 5, 6, 6 ) ).IsSuccess() );
    // UE extends by the new strips only: a sample already cached is the cache's, even if the tile under it
    // was changed behind the cache's back since.
    w.Tiles.at( { 0, 0 } ).SetSample( 6, 6, 7u );
    ASSERT_TRUE( cache.CacheData( 0, 9, 1, 12 ).IsSuccess() ); // grows to the bounding box [0..6] x [5..12]
    auto all = cache.GetCachedData( 0, 5, 6, 12 );
    ASSERT_TRUE( all.IsSuccess() ) << all.GetError();
    EXPECT_EQ( all.GetValue()[0 * 7 + 5], Pattern( 5, 5, 6, 6 )[0] );
    EXPECT_EQ( all.GetValue()[7 * 7 + 3], Height( 3, 12 ) );
    EXPECT_EQ( all.GetValue()[1 * 7 + 6], Pattern( 5, 5, 6, 6 )[3] ) << "the extension re-read a cached sample";

    EXPECT_FALSE( cache.GetCachedData( 0, 4, 6, 12 ).IsSuccess() );
    EXPECT_FALSE( cache.SetCachedData( 7, 5, 7, 5, std::vector<uint16_t>{ 1 } ).IsSuccess() );
    EXPECT_FALSE( cache.SetCachedData( 0, 5, 1, 5, std::vector<uint16_t>{ 1 } ).IsSuccess() ) << "count mismatch";
    EXPECT_FALSE( cache.CacheData( 3, 3, 2, 3 ).IsSuccess() );
    LandscapeHeightCache empty( w.Root, w.Lookup() );
    EXPECT_FALSE( empty.GetCachedData( 0, 0, 0, 0 ).IsSuccess() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
