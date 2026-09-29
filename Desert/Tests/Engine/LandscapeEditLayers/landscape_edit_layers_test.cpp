// Landscape edit layers (UE 5.8 ULandscapeEditLayerBase + ULandscapeComponent::LayersData): the stack lives on
// the root, each layer's data on the tiles it touched, and the tile's samples and weights are the merge — the
// base layer alone reproduces the tile byte for byte, an additive layer moves only where it holds a delta,
// alpha scales it, an invisible layer does nothing, the stack's order decides the paint but not the height sum,
// and a merge over a rectangle writes nothing outside it.
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace Desert::World::Landscape;
using Common::UUID;

namespace
{
    constexpr uint32_t kSize = 9u;
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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
