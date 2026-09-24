// The weightmap layers (UE's ULandscapeLayerInfoObject + a component's weightmap) and the Paint stroke
// (FLandscapeToolStrokePaint): normalisation, NoWeightBlend, Hardness, the tile blob round trip, and a stroke
// over two tiles that keeps every sample's weight-blended sum at 255 and undoes byte for byte.
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <Common/Utilities/Crc32c.hpp>

#include <gtest/gtest.h>

#include <map>
#include <numeric>
#include <utility>

using namespace Desert::World::Landscape;

namespace
{
    int Sum( const std::vector<uint8_t>& w )
    {
        return std::accumulate( w.begin(), w.end(), 0 );
    }

    LandscapeTileData Tile( uint32_t samples )
    {
        auto tile = LandscapeTileData::Create( samples, samples );
        EXPECT_TRUE( tile.IsSuccess() );
        return tile.GetValue();
    }

    void Fill( LandscapeTileData& tile, const std::string& name, uint8_t value )
    {
        auto index = tile.AddWeightLayer( name );
        ASSERT_TRUE( index.IsSuccess() ) << index.GetError();
        std::vector<uint8_t> plane( tile.Samples().size(), value );
        ASSERT_TRUE( tile.WriteWeightRegion( index.GetValue(), tile.Bounds(), plane ).IsSuccess() );
    }
} // namespace

TEST( LandscapePaint, PaintingAOverBKeepsTheSumAt255 )
{
    const std::vector<LandscapeLayerRule> rules = { { "Grass", 0.5f, false }, { "Rock", 0.5f, false } };
    for ( int value = 0; value <= 255; value += 17 )
    {
        std::vector<uint8_t> w = { 0u, 255u };
        LandscapeNormalizeWeights( w, rules, 0, static_cast<uint8_t>( value ) );
        EXPECT_EQ( w[0], value );
        EXPECT_EQ( Sum( w ), 255 ) << "painting Grass to " << value;
    }
    // Three layers, uneven: the residue of rounding must still land the sum exactly.
    const std::vector<LandscapeLayerRule> three = {
         { "A", 0.5f, false }, { "B", 0.5f, false }, { "C", 0.5f, false } };
    std::vector<uint8_t> w = { 10u, 100u, 145u };
    LandscapeNormalizeWeights( w, three, 0, 77u );
    EXPECT_EQ( w[0], 77 );
    EXPECT_EQ( Sum( w ), 255 );
    EXPECT_LT( w[1], w[2] ); // proportional: B had less than C and still has
    // Erasing gives the weight back to the others.
    LandscapeNormalizeWeights( w, three, 0, 0u );
    EXPECT_EQ( w[0], 0 );
    EXPECT_EQ( Sum( w ), 255 );
}

TEST( LandscapePaint, NoWeightBlendLayerIsNeitherNormalisedNorCounted )
{
    const std::vector<LandscapeLayerRule> rules = {
         { "Grass", 0.5f, false }, { "Rock", 0.5f, false }, { "Puddles", 0.5f, true } };
    std::vector<uint8_t> w = { 0u, 255u, 40u };
    LandscapeNormalizeWeights( w, rules, 2, 200u ); // painting the NoWeightBlend layer moves nothing else
    EXPECT_EQ( w, ( std::vector<uint8_t>{ 0u, 255u, 200u } ) );
    LandscapeNormalizeWeights( w, rules, 0, 100u ); // painting a blended layer leaves it alone
    EXPECT_EQ( w[2], 200 );
    EXPECT_EQ( w[0] + w[1], 255 );
}

TEST( LandscapePaint, HardLayersGiveLessThanSoftOnes )
{
    const std::vector<LandscapeLayerRule> rules = {
         { "Paint", 0.5f, false }, { "Soft", 0.0f, false }, { "Hard", 1.0f, false } };
    std::vector<uint8_t> w = { 0u, 128u, 127u };
    LandscapeNormalizeWeights( w, rules, 0, 100u ); // 100 to take: Soft can give it all
    EXPECT_EQ( w[2], 127 );
    EXPECT_EQ( w[1], 28 );
    LandscapeNormalizeWeights( w, rules, 0, 200u ); // Soft runs out; the rest comes from Hard
    EXPECT_EQ( w[1], 0 );
    EXPECT_EQ( Sum( w ), 255 );
}

TEST( LandscapePaint, LonelyLayerCannotLoseWeightIntoNothing )
{
    const std::vector<LandscapeLayerRule> rules = { { "Grass", 0.5f, false }, { "Rock", 0.5f, false } };
    std::vector<uint8_t>                  w     = { 255u, 0u };
    LandscapeNormalizeWeights( w, rules, 0, 10u );
    EXPECT_EQ( w[0], 255 );
}

TEST( LandscapePaint, TileBlobRoundTripsWeightLayers )
{
    LandscapeTileData tile = Tile( 9 );
    Fill( tile, "Grass", 255u );
    Fill( tile, "Rock", 0u );
    std::vector<uint8_t> dab = { 7u, 9u, 11u, 13u };
    ASSERT_TRUE( tile.WriteWeightRegion( 1, { 2u, 3u, 4u, 5u }, dab ).IsSuccess() );

    const std::vector<unsigned char> blob = EncodeLandscapeTile( tile );
    EXPECT_EQ( blob.size(), kLandscapeTileHeaderSize + 2u * 81u + 4u + 2u * ( 4u + 81u ) + 5u + 4u +
                                 kLandscapeTileTrailerSize );
    auto back = DecodeLandscapeTile( blob );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    const LandscapeTileData& read = back.GetValue();
    ASSERT_EQ( read.WeightLayers().size(), 2u );
    EXPECT_EQ( read.WeightLayers()[0].Name, "Grass" );
    EXPECT_EQ( read.WeightLayers()[1].Name, "Rock" );
    EXPECT_EQ( read.WeightLayers()[0].Weights, tile.WeightLayers()[0].Weights );
    EXPECT_EQ( read.WeightLayers()[1].Weights, tile.WeightLayers()[1].Weights );
    EXPECT_EQ( read.Samples(), tile.Samples() );

    // A flipped weight byte is refused by the checksum, not decoded into a different colour.
    std::vector<unsigned char> bad = blob;
    bad[bad.size() - kLandscapeTileTrailerSize - 1u] ^= 0x10u;
    EXPECT_FALSE( DecodeLandscapeTile( bad ).IsSuccess() );
}

TEST( LandscapePaint, VersionOneBlobIsATileWithNoWeightLayers )
{
    LandscapeTileData tile = Tile( 3 );
    tile.SetSample( 1, 1, 40000u );
    std::vector<unsigned char> blob = EncodeLandscapeTile( tile ); // v2 with an empty weight section
    // Rebuild the v1 form: version 1, no weight-count word, fresh checksum.
    blob[4] = 1u;
    blob.resize( blob.size() - kLandscapeTileTrailerSize - 4u );
    const uint32_t crc = Common::Utils::Crc32c( blob.data(), blob.size() );
    for ( int i = 0; i < 4; ++i )
        blob.push_back( static_cast<unsigned char>( ( crc >> ( 8 * i ) ) & 0xFFu ) );
    auto v1 = DecodeLandscapeTile( blob );
    ASSERT_TRUE( v1.IsSuccess() ) << v1.GetError();
    EXPECT_TRUE( v1.GetValue().WeightLayers().empty() );
    EXPECT_EQ( v1.GetValue().Sample( 1, 1 ), 40000u );
}

TEST( LandscapePaint, FifthLayerIsRefusedByName )
{
    LandscapeTileData tile = Tile( 3 );
    for ( const char* name : { "A", "B", "C", "D" } )
        ASSERT_TRUE( tile.AddWeightLayer( name ).IsSuccess() );
    auto fifth = tile.AddWeightLayer( "E" );
    ASSERT_FALSE( fifth.IsSuccess() );
    EXPECT_NE( fifth.GetError().find( "'E'" ), std::string::npos );
    auto again = tile.AddWeightLayer( "B" );
    EXPECT_EQ( again.GetValue(), 1u ); // an existing name is found, not refused
}

TEST( LandscapePaint, WeightWritesDirtyOnlyTheWeightConsumer )
{
    LandscapeTileData tile = Tile( 5 );
    for ( auto c :
          { LandscapeDirtyConsumer::Gpu, LandscapeDirtyConsumer::Physics, LandscapeDirtyConsumer::Weights } )
        tile.TakeDirtyRects( c );
    Fill( tile, "Grass", 100u );
    EXPECT_TRUE( tile.DirtyRects( LandscapeDirtyConsumer::Gpu ).empty() );
    EXPECT_TRUE( tile.DirtyRects( LandscapeDirtyConsumer::Physics ).empty() );
    EXPECT_FALSE( tile.DirtyRects( LandscapeDirtyConsumer::Weights ).empty() );
    tile.SetSample( 0, 0, 1u );
    tile.TakeDirtyRects( LandscapeDirtyConsumer::Weights );
    tile.SetSample( 1, 0, 1u );
    EXPECT_TRUE( tile.DirtyRects( LandscapeDirtyConsumer::Weights ).empty() );
}

TEST( LandscapePaint, StrokeAcrossTwoTilesKeepsSumsAndEdgesAndUndoes )
{
    LandscapeRoot root;
    root.QuadsPerTile = 7;
    root.SpacingCm    = 100.0f;
    std::map<std::pair<int32_t, int32_t>, LandscapeTileData> tiles;
    for ( int32_t tx = 0; tx < 2; ++tx )
    {
        tiles.emplace( std::make_pair( tx, 0 ), Tile( 8 ) );
        Fill( tiles.at( { tx, 0 } ), "Rock", 255u );
    }
    const LandscapeTileLookup lookup = [&]( int32_t x, int32_t z )
    {
        auto it = tiles.find( { x, z } );
        if ( it == tiles.end() )
            return LandscapeTileSlot{};
        return LandscapeTileSlot{ LandscapeTileState::Present, &it->second };
    };
    const auto before = tiles.at( { 0, 0 } ).WeightLayers();

    LandscapePaintStroke   stroke( root, lookup, { { "Grass", 0.5f, false }, { "Rock", 0.5f, false } } );
    LandscapeBrushSettings brush;
    brush.RadiusCm = 300.0f;
    brush.Strength = 0.3f;
    const glm::vec2 at( 700.0f, 350.0f ); // on the seam between the two tiles
    auto            weights = ComputeLandscapeBrush( root, brush, { &at, 1 } );
    ASSERT_TRUE( weights.IsSuccess() );
    LandscapePaintSettings paint;
    paint.Layer = "Grass";
    for ( int step = 0; step < 20; ++step )
        ASSERT_TRUE( stroke.Apply( weights.GetValue(), brush, paint, false ).IsSuccess() );

    for ( auto& [key, tile] : tiles )
    {
        ASSERT_EQ( tile.WeightLayers().size(), 2u );
        const size_t grass = tile.FindWeightLayer( "Grass" ).value();
        const size_t rock  = tile.FindWeightLayer( "Rock" ).value();
        for ( uint32_t z = 0; z < 8; ++z )
            for ( uint32_t x = 0; x < 8; ++x )
                EXPECT_EQ( tile.Weight( grass, x, z ) + tile.Weight( rock, x, z ), 255 )
                     << "tile " << key.first << " (" << x << ", " << z << ")";
    }
    const LandscapeTileData& west = tiles.at( { 0, 0 } );
    const LandscapeTileData& east = tiles.at( { 1, 0 } );
    const size_t             wg   = west.FindWeightLayer( "Grass" ).value();
    const size_t             eg   = east.FindWeightLayer( "Grass" ).value();
    EXPECT_GT( west.Weight( wg, 7, 3 ), 0 ); // the brush centre took paint
    for ( uint32_t z = 0; z < 8; ++z )
        EXPECT_EQ( west.Weight( wg, 7, z ), east.Weight( eg, 0, z ) ) << "seam row " << z;

    auto record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), true ).IsSuccess() );
    EXPECT_EQ( tiles.at( { 0, 0 } ).WeightLayers().size(), before.size() );
    EXPECT_EQ( tiles.at( { 0, 0 } ).WeightLayers()[0].Weights, before[0].Weights );
    ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), false ).IsSuccess() );
    EXPECT_EQ( tiles.at( { 0, 0 } ).WeightLayers().size(), 2u );
}

TEST( LandscapePaint, UnknownTargetLayerIsRefused )
{
    LandscapeRoot             root;
    const LandscapeTileLookup lookup = []( int32_t, int32_t ) { return LandscapeTileSlot{}; };
    LandscapePaintStroke      stroke( root, lookup, { { "Grass", 0.5f, false } } );
    LandscapePaintSettings    paint;
    paint.Layer = "Snow";
    auto result = stroke.Apply( {}, {}, paint, false );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "Snow" ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
