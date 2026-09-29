// Landscape edit layers (UE 5.8 ULandscapeEditLayerBase + ULandscapeComponent::LayersData): the stack lives on
// the root, each layer's data on the tiles it touched, and the tile's samples and weights are the merge — the
// base layer alone reproduces the tile byte for byte, an additive layer moves only where it holds a delta,
// alpha scales it, an invisible layer does nothing, the stack's order decides the paint but not the height sum,
// and a merge over a rectangle writes nothing outside it.
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <Common/Utilities/Crc32c.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace Desert::World::Landscape;
using Common::UUID;

namespace
{
    constexpr uint32_t kSize = 8u; // 7 quads: a UE section size (the brush refuses any other)
    constexpr size_t   kArea = static_cast<size_t>( kSize ) * kSize;
    constexpr uint16_t kMid  = kLandscapeMidSample;

    const std::vector<LandscapeLayerRule> kRules{ { "Grass", 0.5f, false }, { "Rock", 0.5f, false } };

    LandscapeEditLayer Layer( uint64_t guid, const char* name, float heightAlpha = 1.0f, float weightAlpha = 1.0f )
    {
        return { UUID( guid ), name, true, false, heightAlpha, weightAlpha };
    }

    LandscapeTileData Flat()
    {
        auto tile = LandscapeTileData::Create( kSize, kSize );
        EXPECT_TRUE( tile ) << tile.GetError();
        return tile.GetValue();
    }

    /// A delta plane: mid everywhere, mid + @p delta inside @p rect.
    std::vector<uint16_t> Delta( const LandscapeRect& rect, int delta )
    {
        std::vector<uint16_t> heights( kArea, kMid );
        for ( uint32_t z = rect.Z0; z < rect.Z1; ++z )
            for ( uint32_t x = rect.X0; x < rect.X1; ++x )
                heights[z * kSize + x] = static_cast<uint16_t>( kMid + delta );
        return heights;
    }

    LandscapeWeightLayer Plane( const char* name, uint8_t value )
    {
        return { name, std::vector<uint8_t>( kArea, value ) };
    }

    std::vector<uint8_t> WeightsOf( const LandscapeTileData& tile, const char* name )
    {
        const auto index = tile.FindWeightLayer( name );
        return index ? tile.WeightLayers()[*index].Weights : std::vector<uint8_t>();
    }

    void Merge( const LandscapeEditLayerStack& stack, LandscapeTileData& tile, const LandscapeRect& rect )
    {
        auto merged = MergeLandscapeEditLayers( stack, kRules, rect, tile );
        ASSERT_TRUE( merged ) << merged.GetError();
    }
} // namespace

TEST( LandscapeEditLayers, OneBaseLayerReproducesTheTileByteForByte )
{
    std::vector<uint16_t> samples( kArea );
    std::vector<uint8_t>  grass( kArea );
    std::vector<uint8_t>  rock( kArea );
    for ( size_t i = 0; i < kArea; ++i )
    {
        samples[i] = static_cast<uint16_t>( ( i * 7919u ) % 65536u );
        grass[i]   = static_cast<uint8_t>( ( i * 37u ) % 256u );
        rock[i]    = static_cast<uint8_t>( 255u - grass[i] );
    }
    LandscapeTileData tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), samples, { { "Grass", grass }, { "Rock", rock } } } ) );

    Merge( { { Layer( 1u, "Base" ) } }, tile, tile.Bounds() );

    EXPECT_EQ( tile.Samples(), samples );
    EXPECT_EQ( WeightsOf( tile, "Grass" ), grass );
    EXPECT_EQ( WeightsOf( tile, "Rock" ), rock );
}

TEST( LandscapeEditLayers, AnAdditiveLayerRaisesOnlyWhereItHoldsADelta )
{
    const LandscapeRect raised{ 2u, 3u, 5u, 6u };
    LandscapeTileData   tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 1000 ), {} } ) );
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 2u ), Delta( raised, 400 ), {} } ) );

    Merge( { { Layer( 1u, "Base" ), Layer( 2u, "Raise" ) } }, tile, tile.Bounds() );

    for ( uint32_t z = 0; z < kSize; ++z )
        for ( uint32_t x = 0; x < kSize; ++x )
        {
            const bool inside = x >= raised.X0 && x < raised.X1 && z >= raised.Z0 && z < raised.Z1;
            EXPECT_EQ( tile.Sample( x, z ), kMid + 1000 + ( inside ? 400 : 0 ) ) << x << ", " << z;
        }
}

TEST( LandscapeEditLayers, HeightAlphaHalfAddsHalfTheDelta )
{
    LandscapeTileData tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 1000 ), {} } ) );
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 2u ), Delta( tile.Bounds(), 400 ), {} } ) );

    Merge( { { Layer( 1u, "Base" ), Layer( 2u, "Raise", 0.5f ) } }, tile, tile.Bounds() );

    EXPECT_EQ( tile.Sample( 4u, 4u ), kMid + 1200 );
}

TEST( LandscapeEditLayers, AnInvisibleLayerHasNoEffect )
{
    LandscapeTileData tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 1000 ), { Plane( "Grass", 255u ) } } ) );
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 2u ), Delta( tile.Bounds(), 400 ), { Plane( "Rock", 255u ) } } ) );
    LandscapeEditLayer hidden = Layer( 2u, "Hidden" );
    hidden.Visible            = false;

    Merge( { { Layer( 1u, "Base" ), hidden } }, tile, tile.Bounds() );

    EXPECT_EQ( tile.Sample( 4u, 4u ), kMid + 1000 );
    EXPECT_EQ( WeightsOf( tile, "Grass" ), std::vector<uint8_t>( kArea, 255u ) );
    EXPECT_FALSE( tile.FindWeightLayer( "Rock" ) ) << "an invisible layer's paint allocates nothing";
}

TEST( LandscapeEditLayers, StackOrderDecidesThePaintButNotTheHeightSum )
{
    auto merged = []( bool grassOnTop )
    {
        LandscapeTileData tile = Flat();
        EXPECT_TRUE(
             tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 100 ), { Plane( "Grass", 255u ) } } ) );
        EXPECT_TRUE( tile.SetEditLayer( { UUID( 2u ), Delta( tile.Bounds(), 300 ), { Plane( "Rock", 255u ) } } ) );
        LandscapeEditLayerStack stack{ { Layer( 1u, "Grass" ), Layer( 2u, "Rock" ) } };
        if ( grassOnTop )
            std::swap( stack.Layers[0], stack.Layers[1] );
        Merge( stack, tile, tile.Bounds() );
        return tile;
    };
    const LandscapeTileData rockOnTop  = merged( false );
    const LandscapeTileData grassOnTop = merged( true );

    EXPECT_EQ( rockOnTop.Samples(), grassOnTop.Samples() );
    EXPECT_EQ( rockOnTop.Sample( 0u, 0u ), kMid + 400 );
    EXPECT_EQ( WeightsOf( rockOnTop, "Rock" ), std::vector<uint8_t>( kArea, 255u ) );
    EXPECT_FALSE( rockOnTop.FindWeightLayer( "Grass" ) ) << "fully covered paint allocates no plane";
    EXPECT_EQ( WeightsOf( grassOnTop, "Grass" ), std::vector<uint8_t>( kArea, 255u ) );
    EXPECT_FALSE( grassOnTop.FindWeightLayer( "Rock" ) ) << "fully covered paint allocates no plane";
}

TEST( LandscapeEditLayers, WeightAlphaCoversOnlyThatShareOfThePaintBelow )
{
    LandscapeTileData tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), {}, { Plane( "Grass", 255u ) } } ) );
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 2u ), {}, { Plane( "Rock", 255u ) } } ) );

    Merge( { { Layer( 1u, "Base" ), Layer( 2u, "Rock", 1.0f, 0.5f ) } }, tile, tile.Bounds() );

    const int grass = WeightsOf( tile, "Grass" ).at( 4u * kSize + 4u );
    const int rock  = WeightsOf( tile, "Rock" ).at( 4u * kSize + 4u );
    EXPECT_NEAR( grass, 128, 1 );
    EXPECT_NEAR( rock, 128, 1 );
    EXPECT_LE( grass + rock, 256 ) << "rounding may add one, blending never scales the sum up";
}

TEST( LandscapeEditLayers, AMergeOverARectangleWritesNothingOutsideIt )
{
    LandscapeTileData             tile = Flat();
    const LandscapeEditLayerStack stack{ { Layer( 1u, "Base" ) } };
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 1000 ), { Plane( "Grass", 100u ) } } ) );
    Merge( stack, tile, tile.Bounds() );
    for ( int c = 0; c < static_cast<int>( kLandscapeDirtyConsumerCount ); ++c )
        (void)tile.TakeDirtyRects( static_cast<LandscapeDirtyConsumer>( c ) );

    // The layer changes everywhere; only the rectangle is merged.
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 2000 ), { Plane( "Grass", 200u ) } } ) );
    const LandscapeRect rect{ 3u, 3u, 6u, 5u };
    Merge( stack, tile, rect );

    for ( uint32_t z = 0; z < kSize; ++z )
        for ( uint32_t x = 0; x < kSize; ++x )
        {
            const bool inside = x >= rect.X0 && x < rect.X1 && z >= rect.Z0 && z < rect.Z1;
            EXPECT_EQ( tile.Sample( x, z ), kMid + ( inside ? 2000 : 1000 ) ) << x << ", " << z;
            EXPECT_EQ( WeightsOf( tile, "Grass" ).at( z * kSize + x ), inside ? 200 : 100 ) << x << ", " << z;
        }
    for ( int c = 0; c < static_cast<int>( kLandscapeDirtyConsumerCount ); ++c )
        for ( const LandscapeRect& dirty : tile.DirtyRects( static_cast<LandscapeDirtyConsumer>( c ) ) )
            EXPECT_TRUE( dirty.X0 >= rect.X0 && dirty.X1 <= rect.X1 && dirty.Z0 >= rect.Z0 &&
                         dirty.Z1 <= rect.Z1 );
}

TEST( LandscapeEditLayers, ALayeredTileRefusesEveryWriteButTheMerge )
{
    LandscapeTileData tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 10 ), {} } ) );
    const std::vector<uint16_t> one( 1u, 7u );
    const std::vector<uint8_t>  paint( 1u, 7u );
    EXPECT_FALSE( tile.WriteRegion( { 0u, 0u, 1u, 1u }, one ) );
    EXPECT_FALSE( tile.AddWeightLayer( "Grass" ) );
    EXPECT_FALSE( tile.SetWeightLayers( {} ) );
    EXPECT_EQ( tile.Sample( 0u, 0u ), kMid ) << "nothing was written before the merge";
}

TEST( LandscapeEditLayers, TheMergeRefusesWhatTheStackOrTheRulesDoNotName )
{
    LandscapeTileData tile = Flat();
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 10 ), {} } ) );
    ASSERT_TRUE( tile.SetEditLayer( { UUID( 9u ), {}, { Plane( "Sand", 255u ) } } ) );

    EXPECT_FALSE( MergeLandscapeEditLayers( { { Layer( 1u, "Base" ) } }, kRules, tile.Bounds(), tile ) )
         << "layer 9's data on the tile, but no layer 9 in the stack";
    EXPECT_FALSE( MergeLandscapeEditLayers( { { Layer( 1u, "Base" ), Layer( 9u, "Sand" ) } }, kRules,
                                            tile.Bounds(), tile ) )
         << "'Sand' is not a layer of the landscape";
    EXPECT_EQ( tile.Sample( 4u, 4u ), kMid ) << "a refused merge leaves the tile as it was";

    EXPECT_FALSE( ValidateLandscapeEditLayerStack( { { Layer( 1u, "A" ), Layer( 1u, "B" ) } } ) );
    EXPECT_FALSE( ValidateLandscapeEditLayerStack( { { Layer( 0u, "Null" ) } } ) );
    EXPECT_FALSE( ValidateLandscapeEditLayerStack( { { Layer( 1u, "A", 1.5f ) } } ) );
    EXPECT_FALSE( tile.SetEditLayer( { UUID( 2u ), std::vector<uint16_t>( 3u, kMid ), {} } ) );
    EXPECT_TRUE( tile.RemoveEditLayer( UUID( 9u ) ) );
    EXPECT_FALSE( tile.RemoveEditLayer( UUID( 9u ) ) );
}

// ── THE BLOB (DLHT v3) ─────────────────────────────────────────────────────────────────────────────
namespace
{
    /// A tile with two edit layers (one with heights and paint, one with paint only) and their merge.
    LandscapeTileData LayeredTile( const LandscapeEditLayerStack& stack )
    {
        LandscapeTileData tile = Flat();
        EXPECT_TRUE(
             tile.SetEditLayer( { UUID( 1u ), Delta( tile.Bounds(), 100 ), { Plane( "Grass", 200u ) } } ) );
        EXPECT_TRUE( tile.SetEditLayer( { UUID( 0xF00DFACE12345678ull ), {}, { Plane( "Rock", 90u ) } } ) );
        Merge( stack, tile, tile.Bounds() );
        return tile;
    }

    const LandscapeEditLayerStack kTwoLayers{
         { Layer( 1u, "Base" ), Layer( 0xF00DFACE12345678ull, "Rocks", 1.0f, 0.5f ) } };

    /// Re-stamps a blob's trailer after a test edited its bytes, so the refusal is the section's, not the CRC's.
    void Restamp( std::vector<unsigned char>& bytes )
    {
        const size_t   covered = bytes.size() - kLandscapeTileTrailerSize;
        const uint32_t crc     = Common::Utils::Crc32c( bytes.data(), covered );
        for ( size_t i = 0; i < 4u; ++i )
            bytes[covered + i] = static_cast<unsigned char>( ( crc >> ( 8u * i ) ) & 0xFFu );
    }
} // namespace

TEST( LandscapeEditLayers, ALayeredTileSurvivesTheBlobByteForByte )
{
    const LandscapeTileData          tile  = LayeredTile( kTwoLayers );
    const std::vector<unsigned char> bytes = EncodeLandscapeTile( tile );
    auto                             read  = DecodeLandscapeTile( bytes );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( EncodeLandscapeTile( read.GetValue() ), bytes );

    ASSERT_EQ( read.GetValue().EditLayers().size(), 2u );
    for ( size_t i = 0; i < 2u; ++i )
    {
        const LandscapeEditLayerTileData& r = read.GetValue().EditLayers()[i];
        const LandscapeEditLayerTileData& w = tile.EditLayers()[i];
        EXPECT_EQ( static_cast<uint64_t>( r.Layer ), static_cast<uint64_t>( w.Layer ) ) << i;
        EXPECT_EQ( r.Heights, w.Heights ) << i;
        ASSERT_EQ( r.Weights.size(), w.Weights.size() ) << i;
        for ( size_t k = 0; k < r.Weights.size(); ++k )
        {
            EXPECT_EQ( r.Weights[k].Name, w.Weights[k].Name ) << i;
            EXPECT_EQ( r.Weights[k].Weights, w.Weights[k].Weights ) << i;
        }
    }

    // The layers read back merge to the tile that was written.
    LandscapeTileData merged = read.ExtractValue();
    Merge( kTwoLayers, merged, merged.Bounds() );
    EXPECT_EQ( merged.Samples(), tile.Samples() );
    EXPECT_EQ( WeightsOf( merged, "Grass" ), WeightsOf( tile, "Grass" ) );
    EXPECT_EQ( WeightsOf( merged, "Rock" ), WeightsOf( tile, "Rock" ) );
}

TEST( LandscapeEditLayers, ATileWithoutLayersHasAnEmptySection )
{
    const std::vector<unsigned char> bytes = EncodeLandscapeTile( Flat() );
    EXPECT_EQ( bytes.size(), kLandscapeTileHeaderSize + 2u * kArea + 4u + kLandscapeTileTrailerSize );
    EXPECT_EQ( bytes[16], 0u ) << "the header's edit-layer count";
    auto read = DecodeLandscapeTile( bytes );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_TRUE( read.GetValue().EditLayers().empty() );
}

TEST( LandscapeEditLayers, ABrokenEditLayerSectionIsRefusedWithTheReason )
{
    // A header that counts a layer the section does not hold.
    std::vector<unsigned char> counted = EncodeLandscapeTile( Flat() );
    counted[16]                        = 1u;
    Restamp( counted );
    auto missing = DecodeLandscapeTile( counted );
    ASSERT_FALSE( missing );
    EXPECT_NE( missing.GetError().find( "edit layer 0" ), std::string::npos ) << missing.GetError();

    // A section cut one byte short: the last layer's last weight plane no longer fits.
    std::vector<unsigned char> cut = EncodeLandscapeTile( LayeredTile( kTwoLayers ) );
    cut.erase( cut.end() - static_cast<std::ptrdiff_t>( kLandscapeTileTrailerSize ) - 1 );
    Restamp( cut );
    auto shortened = DecodeLandscapeTile( cut );
    ASSERT_FALSE( shortened );
    EXPECT_NE( shortened.GetError().find( "edit layer 1" ), std::string::npos ) << shortened.GetError();

    // Heights that are neither none nor a whole plane.
    std::vector<unsigned char> sized = EncodeLandscapeTile( LayeredTile( kTwoLayers ) );
    // Counted from the end: layer 1 is Guid, count 0, one plane "Rock"; layer 0 is Guid, count, heights, "Grass".
    const size_t record1     = 8u + 4u + 4u + ( 4u + 4u + kArea );
    const size_t record0     = 8u + 4u + 2u * kArea + 4u + ( 4u + 5u + kArea );
    const size_t firstRecord = sized.size() - kLandscapeTileTrailerSize - record1 - record0;
    sized[firstRecord + 8u]  = 3u; // edit layer 0's height count, low byte
    Restamp( sized );
    auto heights = DecodeLandscapeTile( sized );
    ASSERT_FALSE( heights );
    EXPECT_NE( heights.GetError().find( "edit layer 0 states 3 heights" ), std::string::npos )
         << heights.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── Brushes write the editing layer (UE: ALandscape::GetEditingLayer), and the tile is its merge ─────────
namespace
{
    constexpr uint64_t kBaseGuid = 1u;
    constexpr uint64_t kTopGuid  = 2u;

    LandscapeEditLayerStack BaseAndTop( bool topLocked = false )
    {
        LandscapeEditLayerStack stack{ { Layer( kBaseGuid, "Base" ), Layer( kTopGuid, "Top" ) } };
        stack.Layers[1].Locked = topLocked;
        return stack;
    }

    /// An 8x8 tile whose Base holds +50 and full Grass, Top a +5 plane, merged.
    LandscapeTileData BrushTile( const LandscapeEditLayerStack& stack )
    {
        LandscapeTileData tile = Flat();
        EXPECT_TRUE(
             tile.SetEditLayer( { UUID( kBaseGuid ), Delta( tile.Bounds(), 50 ), { Plane( "Grass", 255u ) } } ) );
        EXPECT_TRUE( tile.SetEditLayer( { UUID( kTopGuid ), Delta( tile.Bounds(), 5 ), {} } ) );
        Merge( stack, tile, tile.Bounds() );
        return tile;
    }

    LandscapeRoot BrushRoot()
    {
        LandscapeRoot root;
        root.QuadsPerTile = kSize - 1u;
        root.SpacingCm    = 100.0f;
        return root;
    }

    LandscapeTileLookup OneTile( LandscapeTileData& tile )
    {
        return [&tile]( int32_t x, int32_t z ) -> LandscapeTileSlot
        {
            if ( x == 0 && z == 0 )
                return { LandscapeTileState::Present, &tile };
            return { LandscapeTileState::Absent, nullptr };
        };
    }

    LandscapeEditLayerTarget Editing( const LandscapeEditLayerStack& stack, uint64_t layer )
    {
        return { stack, kRules, UUID( layer ) };
    }

    /// Raises the cache's 2..4 square to mid + @p delta through @p cache.
    Common::BoolResultStr Raise( LandscapeHeightCache& cache, int delta )
    {
        if ( auto cached = cache.CacheData( 2, 2, 4, 4 ); !cached )
            return cached;
        return cache.SetCachedData( 2, 2, 4, 4,
                                    std::vector<uint16_t>( 9u, static_cast<uint16_t>( kMid + delta ) ) );
    }
} // namespace

TEST( LandscapeEditLayers, ASculptStrokeInTheTopLayerChangesOnlyItsDataAndTheTileIsTheMerge )
{
    const LandscapeEditLayerStack stack = BaseAndTop();
    LandscapeTileData             tile  = BrushTile( stack );
    const auto                    base  = tile.FindEditLayer( UUID( kBaseGuid ) )->Heights;

    LandscapeHeightCache cache( BrushRoot(), OneTile( tile ), Editing( stack, kTopGuid ) );
    ASSERT_TRUE( cache.CacheData( 2, 2, 4, 4 ) );
    // The cache reads the LAYER (mid + 5), not the merge (mid + 55).
    const auto layerHeight = cache.GetCachedData( 3, 3, 3, 3 );
    ASSERT_TRUE( layerHeight ) << layerHeight.GetError();
    EXPECT_EQ( layerHeight.GetValue()[0], kMid + 5 );
    auto raised = Raise( cache, 30 );
    ASSERT_TRUE( raised ) << raised.GetError();

    EXPECT_EQ( tile.FindEditLayer( UUID( kBaseGuid ) )->Heights, base );
    EXPECT_EQ( tile.FindEditLayer( UUID( kTopGuid ) )->Heights[3u * kSize + 3u], kMid + 30 );
    EXPECT_EQ( tile.FindEditLayer( UUID( kTopGuid ) )->Heights[0], kMid + 5 );
    EXPECT_EQ( tile.Sample( 3u, 3u ), kMid + 80 );
    EXPECT_EQ( tile.Sample( 0u, 0u ), kMid + 55 );

    LandscapeEditLayerStack hidden = stack;
    hidden.Layers[1].Visible       = false;
    Merge( hidden, tile, tile.Bounds() );
    EXPECT_EQ( tile.Sample( 3u, 3u ), kMid + 50 );
    EXPECT_EQ( tile.Sample( 0u, 0u ), kMid + 50 );
}

TEST( LandscapeEditLayers, UndoingASculptStrokeGivesTheLayerBackByteForByte )
{
    const LandscapeEditLayerStack stack = BaseAndTop();
    LandscapeTileData             tile  = BrushTile( stack );
    const auto                    top   = tile.FindEditLayer( UUID( kTopGuid ) )->Heights;
    const auto                    merge = tile.Samples();

    LandscapeHeightCache stroke( BrushRoot(), OneTile( tile ), Editing( stack, kTopGuid ) );
    ASSERT_TRUE( stroke.CacheData( 2, 2, 4, 4 ) );
    const auto read = stroke.GetCachedData( 2, 2, 4, 4 ); // the record's Before: layer data
    ASSERT_TRUE( read ) << read.GetError();
    const auto& before = read.GetValue();
    ASSERT_TRUE( Raise( stroke, 400 ) );
    ASSERT_NE( tile.FindEditLayer( UUID( kTopGuid ) )->Heights, top );

    LandscapeHeightCache undo( BrushRoot(), OneTile( tile ), Editing( stack, kTopGuid ) );
    ASSERT_TRUE( undo.CacheData( 2, 2, 4, 4 ) );
    ASSERT_TRUE( undo.SetCachedData( 2, 2, 4, 4, before ) );
    EXPECT_EQ( tile.FindEditLayer( UUID( kTopGuid ) )->Heights, top );
    EXPECT_EQ( tile.Samples(), merge );
}

TEST( LandscapeEditLayers, ALockedLayerRefusesEveryBrushAndTheTileIsUnchanged )
{
    const LandscapeEditLayerStack stack = BaseAndTop( true );
    LandscapeTileData             tile  = BrushTile( stack );
    const auto                    merge = tile.Samples();

    LandscapeHeightCache cache( BrushRoot(), OneTile( tile ), Editing( stack, kTopGuid ) );
    auto                 sculpt = Raise( cache, 30 );
    ASSERT_FALSE( sculpt );
    EXPECT_NE( sculpt.GetError().find( "locked" ), std::string::npos ) << sculpt.GetError();

    LandscapePaintStroke   paint( BrushRoot(), OneTile( tile ), Editing( stack, kTopGuid ) );
    const glm::vec2        centre( 400.0f, 400.0f );
    LandscapeBrushSettings brush;
    brush.RadiusCm = 200.0f;
    auto weights   = ComputeLandscapeBrush( BrushRoot(), brush, std::span( &centre, 1u ) );
    ASSERT_TRUE( weights ) << weights.GetError();
    LandscapePaintSettings settings;
    settings.Layer = "Rock";
    auto painted   = paint.Apply( weights.GetValue(), brush, settings, false );
    ASSERT_FALSE( painted );
    EXPECT_NE( painted.GetError().find( "locked" ), std::string::npos ) << painted.GetError();
    EXPECT_EQ( tile.Samples(), merge );
    EXPECT_EQ( tile.FindEditLayer( UUID( kTopGuid ) )->Weights.size(), 0u );
}

TEST( LandscapeEditLayers, PaintingTheTopLayerLeavesTheBaseWeightsAndUndoGivesThemBack )
{
    const LandscapeEditLayerStack stack = BaseAndTop();
    LandscapeTileData             tile  = BrushTile( stack );
    const auto                    base  = tile.FindEditLayer( UUID( kBaseGuid ) )->Weights;

    LandscapePaintStroke   paint( BrushRoot(), OneTile( tile ), Editing( stack, kTopGuid ) );
    const glm::vec2        centre( 400.0f, 400.0f );
    LandscapeBrushSettings brush;
    brush.RadiusCm = 200.0f;
    auto weights   = ComputeLandscapeBrush( BrushRoot(), brush, std::span( &centre, 1u ) );
    ASSERT_TRUE( weights ) << weights.GetError();
    LandscapePaintSettings settings;
    settings.Layer = "Rock";
    for ( int step = 0; step < 4; ++step )
    {
        auto painted = paint.Apply( weights.GetValue(), brush, settings, false );
        ASSERT_TRUE( painted ) << painted.GetError();
    }

    const auto& baseAfter = tile.FindEditLayer( UUID( kBaseGuid ) )->Weights;
    ASSERT_EQ( baseAfter.size(), base.size() );
    EXPECT_EQ( baseAfter[0].Weights, base[0].Weights );
    const auto& top = tile.FindEditLayer( UUID( kTopGuid ) )->Weights;
    ASSERT_FALSE( top.empty() );
    EXPECT_GT( WeightsOf( tile, "Rock" )[4u * kSize + 4u], 0u );
    EXPECT_LT( WeightsOf( tile, "Grass" )[4u * kSize + 4u], 255u );

    auto record = paint.Finish();
    ASSERT_TRUE( record ) << record.GetError();
    auto undone =
         ApplyLandscapePaintRecord( OneTile( tile ), record.GetValue(), true, Editing( stack, kTopGuid ) );
    ASSERT_TRUE( undone ) << undone.GetError();
    EXPECT_TRUE( tile.FindEditLayer( UUID( kTopGuid ) )->Weights.empty() );
    EXPECT_EQ( WeightsOf( tile, "Grass" ), base[0].Weights );
    const auto rock = WeightsOf( tile, "Rock" );
    EXPECT_TRUE( rock.empty() || rock[4u * kSize + 4u] == 0u );
}
