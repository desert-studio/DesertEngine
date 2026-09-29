// The weightmap layers (UE's ULandscapeLayerInfoObject + a component's weightmap) and the Paint stroke
// (FLandscapeToolStrokePaint): normalisation, NoWeightBlend, Hardness, the tile blob round trip, and a stroke
// over two tiles that keeps every sample's weight-blended sum at 255 and undoes byte for byte. And the way
// to the screen: the RGBA8 weightmap a tile uploads, the colours its channels take from the root, and the
// surface shader's blend (LandscapeWeights.glslh, compiled here as C++).
#include <Engine/World/Landscape/LandscapePaint.hpp>
#include <Engine/World/Landscape/LandscapeWeightmap.hpp>

#include <Common/Core/GlslAsCpp.hpp>
#include <Common/Utilities/Crc32c.hpp>

#include <glm/glm.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <numeric>
#include <utility>

using namespace Desert::World::Landscape;

namespace
{
    using glm::mix;
    using glm::vec2;
    using glm::vec3;
    using glm::vec4;
    DESERT_GLSL_AS_CPP_BEGIN
#include <Common/LandscapeWeights.glslh>
    DESERT_GLSL_AS_CPP_END
} // namespace

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

TEST( LandscapePaint, PaintingOverAFadedLayerDoesNotPumpItUp )
{
    // The LS-13 frame's staircase: at the soft edge of an earlier stroke a sample holds A = 100 and the rest
    // (155) is unpainted ground. Painting B to 30 there fits in the unclaimed share — A must stay 100.
    // Scaling A up to 225 to "keep the sum at 255" turned every faint sample the new brush touched into a
    // full one, a hard edge on the sample grid wherever the two strokes met.
    const std::vector<LandscapeLayerRule> rules = { { "A", 0.5f, false }, { "B", 0.5f, false } };
    std::vector<uint8_t>                  w     = { 100u, 0u };
    LandscapeNormalizeWeights( w, rules, 1, 30u );
    EXPECT_EQ( w, ( std::vector<uint8_t>{ 100u, 30u } ) );
    // Past the unclaimed share, A gives exactly the overflow.
    LandscapeNormalizeWeights( w, rules, 1, 200u );
    EXPECT_EQ( w, ( std::vector<uint8_t>{ 55u, 200u } ) );
    // Erasing B from a sample that was not full gives A back only what B held, not up to 255.
    std::vector<uint8_t> partial = { 60u, 40u };
    LandscapeNormalizeWeights( partial, rules, 1, 10u );
    EXPECT_EQ( partial, ( std::vector<uint8_t>{ 90u, 10u } ) );
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

// No legacy reader (LS-15): a well-formed v1 blob - valid checksum, valid sizes - is refused by its number,
// never read as a tile with no weight layers.
TEST( LandscapePaint, VersionOneBlobIsRefusedByItsNumber )
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
    ASSERT_FALSE( v1.IsSuccess() );
    EXPECT_NE( v1.GetError().find( "version 1 " ), std::string::npos ) << v1.GetError();
    EXPECT_NE( v1.GetError().find( "supported 3" ), std::string::npos ) << v1.GetError();
}

TEST( LandscapePaint, NinthLayerIsRefusedByName )
{
    // Owner decision O3: up to eight paint layers a tile.
    LandscapeTileData tile = Tile( 3 );
    for ( const char* name : { "A", "B", "C", "D", "E", "F", "G", "H" } )
        ASSERT_TRUE( tile.AddWeightLayer( name ).IsSuccess() ) << name;
    auto ninth = tile.AddWeightLayer( "I" );
    ASSERT_FALSE( ninth.IsSuccess() );
    EXPECT_NE( ninth.GetError().find( "'I'" ), std::string::npos );
    auto again = tile.AddWeightLayer( "B" );
    EXPECT_EQ( again.GetValue(), 1u ); // an existing name is found, not refused

    std::vector<LandscapeWeightLayer> nine = tile.WeightLayers();
    nine.push_back( { "I", std::vector<uint8_t>( 9u, 0u ) } );
    EXPECT_FALSE( tile.SetWeightLayers( nine ).IsSuccess() );
    EXPECT_EQ( tile.WeightLayers().size(), 8u ); // the refusal wrote nothing
}

TEST( LandscapePaint, EightLayersRoundTripAndANinthInABlobIsRefused )
{
    LandscapeTileData tile    = Tile( 5 );
    const char*       names[] = { "A", "B", "C", "D", "E", "F", "G", "H" };
    for ( uint8_t l = 0; l < 8u; ++l )
        Fill( tile, names[l], static_cast<uint8_t>( 10u + l ) );

    const std::vector<unsigned char> blob = EncodeLandscapeTile( tile );
    auto                             back = DecodeLandscapeTile( blob );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    ASSERT_EQ( back.GetValue().WeightLayers().size(), 8u );
    for ( size_t l = 0; l < 8u; ++l )
    {
        EXPECT_EQ( back.GetValue().WeightLayers()[l].Name, names[l] );
        EXPECT_EQ( back.GetValue().WeightLayers()[l].Weights, tile.WeightLayers()[l].Weights );
    }

    // The same blob claiming nine layers, with a checksum that matches: refused on the count, by number.
    std::vector<unsigned char> nine  = blob;
    const size_t               count = kLandscapeTileHeaderSize + 2u * 25u;
    ASSERT_EQ( nine[count], 8u );
    nine[count]         = 9u;
    const size_t   body = nine.size() - kLandscapeTileTrailerSize;
    const uint32_t crc  = Common::Utils::Crc32c( nine.data(), body );
    for ( size_t b = 0; b < 4u; ++b )
        nine[body + b] = static_cast<unsigned char>( crc >> ( 8u * b ) );
    auto refused = DecodeLandscapeTile( nine );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "9 weight layers" ), std::string::npos ) << refused.GetError();
}

TEST( LandscapePaint, EightWeightBlendedLayersAlwaysSumToExactly255 )
{
    // Every layer painted to every value in turn, from an uneven start: the rounding residue of seven
    // proportional shares must land somewhere every time, and never make the sum 254 or 256.
    const std::vector<LandscapeLayerRule> rules = {
         { "A", 0.0f, false }, { "B", 0.2f, false }, { "C", 0.5f, false }, { "D", 0.9f, false },
         { "E", 1.0f, false }, { "F", 0.5f, false }, { "G", 0.3f, false }, { "H", 0.7f, false } };
    std::vector<uint8_t> w = { 30u, 30u, 30u, 30u, 30u, 30u, 30u, 45u };
    ASSERT_EQ( Sum( w ), 255 );
    for ( size_t painted = 0; painted < 8u; ++painted )
        for ( int value = 0; value <= 255; value += 13 )
        {
            LandscapeNormalizeWeights( w, rules, painted, static_cast<uint8_t>( value ) );
            ASSERT_EQ( Sum( w ), 255 ) << "layer " << painted << " painted to " << value;
            EXPECT_EQ( w[painted], value ) << "layer " << painted;
        }
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
    // UE's first dab adds round(BrushValue · Strength · 255) = round(0.3 · 255) = 77 at full falloff weight: the
    // strength is applied once, not once in the brush weights and again in PaintStrength (that gave 23).
    ASSERT_TRUE( stroke.Apply( weights.GetValue(), brush, paint, false ).IsSuccess() );
    {
        const LandscapeTileData& west = tiles.at( { 0, 0 } );
        EXPECT_EQ( west.Weight( west.FindWeightLayer( "Grass" ).value(), 7, 3 ), 77 );
    }
    for ( int step = 1; step < 20; ++step )
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
    // The startup slowdown lets the source reach the current value after 1 / 0.05 = 20 units of influence; by
    // then the centre (full weight, 50 cm from the brush centre, inner radius 150 cm) is saturated. By hand with
    // UE's loop: 77, 80, 85, 89, ... 232, 255 at the 17th dab.
    EXPECT_EQ( west.Weight( wg, 7, 3 ), 255 );
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

// The screen half of undo: LandscapeECSSystem re-uploads a tile's weightmap only when the Weights consumer's
// dirty list is non-empty, and it has already drained that list on the stroke's own frames. So undo and redo
// must dirty it again (and only it: the heights did not change), and the texels it then uploads must be the
// pre-stroke ones exactly. A painted tile that had no layers before goes back to none (its weightmap is freed).
TEST( LandscapePaint, UndoAndRedoDirtyTheWeightmapAndRestoreItsTexels )
{
    LandscapeRoot root;
    root.QuadsPerTile = 7;
    root.SpacingCm    = 100.0f;
    std::map<std::pair<int32_t, int32_t>, LandscapeTileData> tiles;
    tiles.emplace( std::make_pair( 0, 0 ), Tile( 8 ) );
    tiles.emplace( std::make_pair( 1, 0 ), Tile( 8 ) );
    Fill( tiles.at( { 0, 0 } ), "Rock", 255u ); // (1, 0) starts with no layers at all
    const LandscapeTileLookup lookup = [&]( int32_t x, int32_t z )
    {
        auto it = tiles.find( { x, z } );
        if ( it == tiles.end() )
            return LandscapeTileSlot{};
        return LandscapeTileSlot{ LandscapeTileState::Present, &it->second };
    };
    const auto drainAll = [&]
    {
        for ( auto& [key, tile] : tiles )
            for ( auto c : { LandscapeDirtyConsumer::Gpu, LandscapeDirtyConsumer::Physics,
                             LandscapeDirtyConsumer::Weights } )
                tile.TakeDirtyRects( c );
    };
    const std::vector<uint8_t> westBefore = LandscapeWeightmapTexels( tiles.at( { 0, 0 } ), 0u );
    drainAll();

    LandscapePaintStroke   stroke( root, lookup, { { "Grass", 0.5f, false }, { "Rock", 0.5f, false } } );
    LandscapeBrushSettings brush;
    brush.RadiusCm = 300.0f;
    brush.Strength = 1.0f;
    const glm::vec2 at( 700.0f, 350.0f ); // on the seam: both tiles are painted
    auto            weights = ComputeLandscapeBrush( root, brush, { &at, 1 } );
    ASSERT_TRUE( weights.IsSuccess() );
    LandscapePaintSettings paint;
    paint.Layer = "Grass";
    for ( int step = 0; step < 5; ++step )
        ASSERT_TRUE( stroke.Apply( weights.GetValue(), brush, paint, false ).IsSuccess() );
    ASSERT_FALSE( tiles.at( { 1, 0 } ).WeightLayers().empty() );
    const std::vector<uint8_t> westStroke = LandscapeWeightmapTexels( tiles.at( { 0, 0 } ), 0u );
    ASSERT_NE( westStroke, westBefore ); // the stroke did change what the GPU shows
    auto record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    drainAll(); // the renderer uploaded the stroke on its own frames

    ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), true ).IsSuccess() );
    for ( auto& [key, tile] : tiles )
    {
        const auto& dirty = tile.DirtyRects( LandscapeDirtyConsumer::Weights );
        ASSERT_EQ( dirty.size(), 1u ) << "tile " << key.first;
        EXPECT_EQ( dirty[0], tile.Bounds() ) << "tile " << key.first;
        EXPECT_TRUE( tile.DirtyRects( LandscapeDirtyConsumer::Gpu ).empty() ) << "tile " << key.first;
        EXPECT_TRUE( tile.DirtyRects( LandscapeDirtyConsumer::Physics ).empty() ) << "tile " << key.first;
    }
    EXPECT_EQ( LandscapeWeightmapTexels( tiles.at( { 0, 0 } ), 0u ), westBefore );
    EXPECT_TRUE( tiles.at( { 1, 0 } ).WeightLayers().empty() );

    drainAll();
    ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), false ).IsSuccess() );
    EXPECT_FALSE( tiles.at( { 0, 0 } ).DirtyRects( LandscapeDirtyConsumer::Weights ).empty() );
    EXPECT_FALSE( tiles.at( { 1, 0 } ).DirtyRects( LandscapeDirtyConsumer::Weights ).empty() );
    EXPECT_EQ( LandscapeWeightmapTexels( tiles.at( { 0, 0 } ), 0u ), westStroke );
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

// ── The weightmap on the GPU ──────────────────────────────────────────────────────────────────────────────

namespace
{
    // The root's layer list as LandscapeECSSystem hands it over: the `.delayerinfo` data of each target layer
    // (Assets::Serialization::LandscapeLayerInfoData has these members).
    struct RootLayer
    {
        std::string LayerName;
        float       Hardness             = 0.5f;
        bool        NoWeightBlend        = false;
        glm::vec3   LayerUsageDebugColor = glm::vec3( 1.0f );
    };

    using Colors = std::array<vec4, kLandscapeMaxWeightLayers>;

    const Colors kColors = { vec4( 0.9f, 0.1f, 0.1f, 1.0f ), vec4( 0.2f, 0.6f, 0.1f, 1.0f ),
                             vec4( 0.1f, 0.2f, 0.8f, 1.0f ), vec4( 0.5f, 0.5f, 0.5f, 1.0f ),
                             vec4( 0.7f, 0.6f, 0.2f, 1.0f ), vec4( 0.3f, 0.1f, 0.5f, 1.0f ),
                             vec4( 0.0f, 0.4f, 0.4f, 1.0f ), vec4( 0.95f, 0.9f, 0.85f, 1.0f ) };
    const vec3   kGround( 0.35f, 0.3f, 0.25f );
    const vec4   kWeightBlendAll( 0.0f );

    // The shader's call: two weightmap pages, the eight layer colours of the instance row, two alpha pages.
    vec3 Blend( vec4 w0, vec4 w1, const Colors& c, vec4 alpha0, vec4 alpha1, vec3 ground )
    {
        return LandscapeWeightBlend( w0, w1, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], alpha0, alpha1,
                                     ground );
    }

    vec3 Blend( vec4 w0, const Colors& c, vec4 alpha0, vec3 ground )
    {
        return Blend( w0, vec4( 0.0f ), c, alpha0, kWeightBlendAll, ground );
    }
} // namespace

TEST( LandscapePaint, FullyPaintedLayerIsExactlyItsColour )
{
    for ( const vec3 ground : { kGround, vec3( 0.0f ), vec3( 1.0f ) } )
    {
        const vec3 r = Blend( vec4( 0.0f, 1.0f, 0.0f, 0.0f ), kColors, kWeightBlendAll, ground );
        EXPECT_EQ( r, vec3( kColors[1] ) );
    }
}

TEST( LandscapePaint, EveryLayerOfBothPagesPaintsItsOwnColour )
{
    // Layer i is page i / 4, channel i % 4: a full weight on each of the eight in turn is exactly that
    // layer's colour — the fifth to eighth come from the second page, which one RGBA8 texture never reached.
    for ( uint32_t layer = 0; layer < kLandscapeMaxWeightLayers; ++layer )
    {
        std::array<vec4, 2> w{};
        w[layer / 4u][static_cast<glm::length_t>( layer % 4u )] = 1.0f;
        const vec3 r = Blend( w[0], w[1], kColors, kWeightBlendAll, kWeightBlendAll, kGround );
        EXPECT_EQ( r, vec3( kColors[layer] ) ) << "layer " << layer;
    }
}

TEST( LandscapePaint, UnpaintedGroundKeepsTheGroundAlbedoExactly )
{
    // UE allocates a layer to a tile on its first stroke with every weight zero; the ground around the
    // stroke must stay the ground (the root's first layer), not turn black.
    const vec3 r = Blend( vec4( 0.0f ), kColors, kWeightBlendAll, kGround );
    EXPECT_EQ( r, kGround );
}

TEST( LandscapePaint, TwoWeightBlendedLayersMixByTheirWeights )
{
    const vec4                       w( 128.0f / 255.0f, 127.0f / 255.0f, 0.0f, 0.0f );
    const vec3                       r        = Blend( w, kColors, kWeightBlendAll, kGround );
    const vec3                       expected = vec3( kColors[0] ) * w.x + vec3( kColors[1] ) * w.y;
    for ( int c = 0; c < 3; ++c )
        EXPECT_NEAR( r[c], expected[c], 1e-6f ) << "channel " << c;
}

TEST( LandscapePaint, LayersOnDifferentPagesMixByTheirWeights )
{
    // Weights that sum to 255 across the page boundary (layers 2, 5 and 7): the claim is counted over both
    // pages, so nothing of the ground ground shows through.
    const vec4                       w0( 0.0f, 0.0f, 100.0f / 255.0f, 0.0f );
    const vec4                       w1( 0.0f, 80.0f / 255.0f, 0.0f, 75.0f / 255.0f );
    const vec3                       r = Blend( w0, w1, kColors, kWeightBlendAll, kWeightBlendAll, kGround );
    const vec3 expected = vec3( kColors[2] ) * w0.z + vec3( kColors[5] ) * w1.y + vec3( kColors[7] ) * w1.w;
    for ( int c = 0; c < 3; ++c )
        EXPECT_NEAR( r[c], expected[c], 1e-6f ) << "channel " << c;
}

TEST( LandscapePaint, ChannelTheRootDoesNotNameIsIgnored )
{
    for ( const uint32_t layer : { 2u, 6u } )
    {
        Colors unnamed   = kColors;
        unnamed[layer].a = 0.0f;
        std::array<vec4, 2> w{};
        w[layer / 4u][static_cast<glm::length_t>( layer % 4u )] = 1.0f;
        const vec3 r = Blend( w[0], w[1], unnamed, kWeightBlendAll, kWeightBlendAll, kGround );
        EXPECT_EQ( r, kGround ) << "layer " << layer;
    }
}

TEST( LandscapePaint, NoWeightBlendLayerIsLaidOverTheBlendNotCountedInIt )
{
    // UE's LB_AlphaBlend: a lerp over the weight-blended result, applied after it.
    const vec4                       alpha3( 0.0f, 0.0f, 0.0f, 1.0f );
    const vec3                       half     = Blend( vec4( 0.0f, 0.0f, 0.0f, 0.5f ), kColors, alpha3, kGround );
    const vec3                       expected = mix( kGround, vec3( kColors[3] ), 0.5f );
    for ( int c = 0; c < 3; ++c )
        EXPECT_NEAR( half[c], expected[c], 1e-6f ) << "channel " << c;

    // Full weight on a weight-blended layer AND on the no-weight-blend one: the latter covers, and its
    // weight never took anything from the former's claim.
    const vec3 over = Blend( vec4( 1.0f, 0.0f, 0.0f, 1.0f ), kColors, alpha3, kGround );
    EXPECT_EQ( over, vec3( kColors[3] ) );
}

TEST( LandscapePaint, NoWeightBlendLayerOnTheSecondPageIsLaidOverLast )
{
    // Layer 6 (page 1, channel 2) is NoWeightBlend: at half weight over a fully painted layer 0 it is a
    // half lerp, and it is applied after the first page's own lerps (layer order).
    const vec4                       alpha1( 0.0f, 0.0f, 1.0f, 0.0f );
    const vec3 r = Blend( vec4( 1.0f, 0.0f, 0.0f, 0.0f ), vec4( 0.0f, 0.0f, 0.5f, 0.0f ), kColors, kWeightBlendAll,
                          alpha1, kGround );
    const vec3                       expected = mix( vec3( kColors[0] ), vec3( kColors[6] ), 0.5f );
    for ( int c = 0; c < 3; ++c )
        EXPECT_NEAR( r[c], expected[c], 1e-6f ) << "channel " << c;
}

TEST( LandscapePaint, PageUVStaysInsideItsPageRows )
{
    // A 5 x 5 sample tile with two pages is a 5 x 10 image; sample row z of page p is image row p * 5 + z.
    const vec2 size( 5.0f, 10.0f );
    for ( const float page : { 0.0f, 1.0f } )
        for ( const float z : { 0.0f, 2.0f, 4.0f } )
        {
            const vec2 uv = LandscapeWeightmapPageUV( vec2( 3.0f, z ), page, size, 2.0f );
            EXPECT_FLOAT_EQ( uv.x, 3.5f / 5.0f );
            EXPECT_FLOAT_EQ( uv.y * size.y, page * 5.0f + z + 0.5f ) << "page " << page << " row " << z;
        }
    // The tile's last sample row is a texel centre: the bilinear filter reaches no row of the other page.
    EXPECT_FLOAT_EQ( LandscapeWeightmapPageUV( vec2( 0.0f, 4.0f ), 0.0f, size, 2.0f ).y * size.y, 4.5f );
    EXPECT_FLOAT_EQ( LandscapeWeightmapPageUV( vec2( 0.0f, 0.0f ), 1.0f, size, 2.0f ).y * size.y, 5.5f );
    // One page: the whole image.
    EXPECT_FLOAT_EQ( LandscapeWeightmapPageUV( vec2( 0.0f, 4.0f ), 0.0f, vec2( 5.0f ), 1.0f ).y, 4.5f / 5.0f );
}

TEST( LandscapePaint, WeightmapTexelCarriesTheTileLayersInChannelOrder )
{
    LandscapeTileData tile = Tile( 3 );
    Fill( tile, "A", 10u );
    Fill( tile, "B", 200u );
    ASSERT_TRUE(
         tile.WriteWeightRegion( 1u, LandscapeRect{ 2u, 1u, 3u, 2u }, std::vector<uint8_t>{ 77u } ).IsSuccess() );

    const std::vector<uint8_t> texels = LandscapeWeightmapTexels( tile, 0u );
    ASSERT_EQ( texels.size(), 9u * 4u );
    for ( uint32_t i = 0; i < 9u; ++i )
    {
        EXPECT_EQ( texels[i * 4u + 0u], 10u ) << "texel " << i;
        EXPECT_EQ( texels[i * 4u + 1u], i == 1u * 3u + 2u ? 77u : 200u ) << "texel " << i; // row-major, X fastest
        EXPECT_EQ( texels[i * 4u + 2u], 0u ) << "texel " << i;
        EXPECT_EQ( texels[i * 4u + 3u], 0u ) << "texel " << i;
    }
}

TEST( LandscapePaint, ChannelsTakeTheirLookFromTheRootLayerOfTheSameName )
{
    LandscapeTileData tile = Tile( 3 );
    Fill( tile, "Sand", 0u );
    Fill( tile, "Ghost", 0u );
    Fill( tile, "Moss", 0u );

    const std::vector<RootLayer>  root     = { { "Moss", 0.5f, false, glm::vec3( 0.1f, 0.5f, 0.1f ) },
                                               { "Sand", 0.5f, true, glm::vec3( 0.8f, 0.7f, 0.4f ) } };
    const LandscapeWeightChannels channels = ResolveLandscapeWeightChannels( tile, root );
    EXPECT_EQ( channels.Count, 3u );
    EXPECT_EQ( channels.Colors[0], vec4( 0.8f, 0.7f, 0.4f, 1.0f ) );
    EXPECT_EQ( channels.Colors[1], vec4( 0.0f ) ); // "Ghost" is not the root's: drawn as unpainted, reported
    EXPECT_EQ( channels.Colors[2], vec4( 0.1f, 0.5f, 0.1f, 1.0f ) );
    EXPECT_EQ( channels.Colors[3], vec4( 0.0f ) );
    EXPECT_EQ( channels.AlphaBlend[0], 1.0f );
    EXPECT_EQ( channels.AlphaBlend[1], 0.0f );
    EXPECT_EQ( channels.AlphaBlend[2], 0.0f );
    ASSERT_EQ( channels.Unknown.size(), 1u );
    EXPECT_EQ( channels.Unknown[0], "Ghost" );
}

TEST( LandscapePaint, LayersFiveToEightFillTheSecondWeightmapPage )
{
    // UE's allocation: layer i in weightmap texture i / 4, channel i % 4.
    LandscapeTileData tile = Tile( 2 );
    for ( uint8_t l = 0; l < 6u; ++l )
        Fill( tile, std::string( 1, static_cast<char>( 'A' + l ) ), static_cast<uint8_t>( 1u + l ) );
    EXPECT_EQ( LandscapeWeightmapPageCount( 0u ), 0u );
    EXPECT_EQ( LandscapeWeightmapPageCount( 4u ), 1u );
    EXPECT_EQ( LandscapeWeightmapPageCount( 5u ), 2u );
    EXPECT_EQ( LandscapeWeightmapPageCount( 8u ), 2u );

    const std::vector<uint8_t> page0 = LandscapeWeightmapTexels( tile, 0u );
    const std::vector<uint8_t> page1 = LandscapeWeightmapTexels( tile, 1u );
    ASSERT_EQ( page1.size(), 4u * 4u );
    for ( uint32_t t = 0; t < 4u; ++t )
    {
        EXPECT_EQ( page0[t * 4u + 0u], 1u );
        EXPECT_EQ( page0[t * 4u + 3u], 4u );
        EXPECT_EQ( page1[t * 4u + 0u], 5u ) << "texel " << t;
        EXPECT_EQ( page1[t * 4u + 1u], 6u ) << "texel " << t;
        EXPECT_EQ( page1[t * 4u + 2u], 0u ) << "texel " << t; // no seventh layer
        EXPECT_EQ( page1[t * 4u + 3u], 0u ) << "texel " << t;
    }
}

TEST( LandscapePaint, AtlasStacksEveryPageAlongTheHeight )
{
    // The upload the terrain samples: page 0's rows, then page 1's, each exactly LandscapeWeightmapTexels.
    LandscapeTileData tile = Tile( 3 );
    for ( uint8_t l = 0; l < 7u; ++l )
        Fill( tile, std::string( 1, static_cast<char>( 'A' + l ) ), static_cast<uint8_t>( 10u + l ) );
    const std::vector<uint8_t> atlas = LandscapeWeightmapAtlasTexels( tile );
    const std::vector<uint8_t> page0 = LandscapeWeightmapTexels( tile, 0u );
    const std::vector<uint8_t> page1 = LandscapeWeightmapTexels( tile, 1u );
    ASSERT_EQ( atlas.size(), 3u * 3u * 2u * 4u );
    EXPECT_TRUE( std::equal( page0.begin(), page0.end(), atlas.begin() ) );
    EXPECT_TRUE(
         std::equal( page1.begin(), page1.end(), atlas.begin() + static_cast<std::ptrdiff_t>( page0.size() ) ) );
    EXPECT_EQ( atlas[page0.size() + 2u], 16u ); // seventh layer: page 1, channel 2

    LandscapeTileData four = Tile( 3 );
    for ( uint8_t l = 0; l < 4u; ++l )
        Fill( four, std::string( 1, static_cast<char>( 'A' + l ) ), 1u );
    EXPECT_EQ( LandscapeWeightmapAtlasTexels( four ), LandscapeWeightmapTexels( four, 0u ) );
    EXPECT_TRUE( LandscapeWeightmapAtlasTexels( Tile( 3 ) ).empty() );
}

TEST( LandscapePaint, AlphaBlendFlagsPackLikeTheWeights )
{
    LandscapeWeightChannels channels;
    channels.Count                  = 7u;
    channels.AlphaBlend[1]          = 1.0f;
    channels.AlphaBlend[6]          = 1.0f;
    const std::array<vec4, 2> pages = LandscapeAlphaBlendPages( channels );
    EXPECT_EQ( pages[0], vec4( 0.0f, 1.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( pages[1], vec4( 0.0f, 0.0f, 1.0f, 0.0f ) );
}

// LS-14: one press of the brush is ONE transaction, and undo/redo are exact. The press drags across three
// tiles (one of which starts with no layers at all), paints and then erases with Shift, so the record must
// carry a tile that gained its first layers, a tile that gained a second one, and a tile it never reached must
// stay out of it. Undo gives back every byte of every tile — names, order and weights — and redo every byte
// of the painted state, twice over, so neither direction drifts.
TEST( LandscapePaint, OneStrokeUndoesAndRedoesBitForBitAcrossTiles )
{
    LandscapeRoot root;
    root.QuadsPerTile = 7;
    root.SpacingCm    = 100.0f;
    std::map<std::pair<int32_t, int32_t>, LandscapeTileData> tiles;
    for ( int32_t tx = 0; tx < 4; ++tx )
        tiles.emplace( std::make_pair( tx, 0 ), Tile( 8 ) );
    Fill( tiles.at( { 0, 0 } ), "Rock", 255u );
    Fill( tiles.at( { 1, 0 } ), "Rock", 200u ); // an unclaimed share of 55: the rule ground
    const LandscapeTileLookup lookup = [&]( int32_t x, int32_t z )
    {
        auto it = tiles.find( { x, z } );
        if ( it == tiles.end() )
            return LandscapeTileSlot{};
        return LandscapeTileSlot{ LandscapeTileState::Present, &it->second };
    };
    using Snapshot      = std::vector<std::vector<LandscapeWeightLayer>>;
    const auto snapshot = [&]
    {
        Snapshot all;
        for ( auto& [key, tile] : tiles )
            all.push_back( tile.WeightLayers() );
        return all;
    };
    const auto same = []( const Snapshot& a, const Snapshot& b )
    {
        if ( a.size() != b.size() )
            return false;
        for ( size_t t = 0; t < a.size(); ++t )
        {
            if ( a[t].size() != b[t].size() )
                return false;
            for ( size_t l = 0; l < a[t].size(); ++l )
                if ( a[t][l].Name != b[t][l].Name || a[t][l].Weights != b[t][l].Weights )
                    return false;
        }
        return true;
    };
    const Snapshot before = snapshot();

    LandscapePaintStroke   stroke( root, lookup, { { "Grass", 0.5f, false }, { "Rock", 0.5f, false } } );
    LandscapeBrushSettings brush;
    brush.RadiusCm = 250.0f;
    brush.Strength = 0.6f;
    LandscapePaintSettings paint;
    paint.Layer = "Grass";
    for ( int step = 0; step < 12; ++step )
    {
        const glm::vec2 at( 300.0f + 100.0f * static_cast<float>( step ), 350.0f ); // drags west to east
        auto            weights = ComputeLandscapeBrush( root, brush, { &at, 1 } );
        ASSERT_TRUE( weights.IsSuccess() );
        ASSERT_TRUE( stroke.Apply( weights.GetValue(), brush, paint, step >= 9 ).IsSuccess() );
    }
    const Snapshot after = snapshot();
    ASSERT_FALSE( same( before, after ) );
    ASSERT_TRUE( tiles.at( { 3, 0 } ).WeightLayers().empty() ) << "the brush never reached tile 3";

    auto record = stroke.Finish();
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    EXPECT_EQ( record.GetValue().Tiles.size(), 3u ) << "tiles 0..2 were touched, tile 3 was not";

    for ( int round = 0; round < 2; ++round )
    {
        ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), true ).IsSuccess() );
        EXPECT_TRUE( same( snapshot(), before ) ) << "undo, round " << round;
        ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), false ).IsSuccess() );
        EXPECT_TRUE( same( snapshot(), after ) ) << "redo, round " << round;
    }
}

// ── The visibility layer (UE's Landscape Visibility Layer / Visibility Mask) ──────────────────────────

TEST( LandscapePaint, VisibilityLayerIsNeitherDrawnNorReportedButNamed )
{
    LandscapeTileData tile = Tile( 3 );
    Fill( tile, "Moss", 0u );
    Fill( tile, std::string( kLandscapeVisibilityLayerName ), 255u );

    const std::vector<RootLayer>  root     = { { "Moss", 0.5f, false, glm::vec3( 0.1f, 0.5f, 0.1f ) } };
    const LandscapeWeightChannels channels = ResolveLandscapeWeightChannels( tile, root );
    EXPECT_EQ( channels.Count, 2u );
    EXPECT_EQ( channels.Visibility, 1 );
    EXPECT_EQ( channels.Colors[1], vec4( 0.0f ) ) << "the holes are not a colour";
    EXPECT_EQ( channels.AlphaBlend[1], 0.0f );
    EXPECT_TRUE( channels.Unknown.empty() ) << "the visibility layer is not a layer the root forgot";

    // Even a root layer carrying the reserved name does not turn the mask into a colour.
    const std::vector<RootLayer> impostor = {
         { std::string( kLandscapeVisibilityLayerName ), 0.5f, false, glm::vec3( 1.0f ) } };
    EXPECT_EQ( ResolveLandscapeWeightChannels( tile, impostor ).Colors[1], vec4( 0.0f ) );

    LandscapeTileData plain = Tile( 3 );
    Fill( plain, "Moss", 0u );
    EXPECT_EQ( ResolveLandscapeWeightChannels( plain, root ).Visibility, -1 );
}

TEST( LandscapePaint, PaintingOverAHoleNeitherMovesNorCountsTheVisibilityLayer )
{
    // The painter's normalisation is the one place a paint layer and the mask meet: a stroke of Grass over a
    // tile of Rock (255) with a half-open hole must end with Grass + Rock = 255 and the mask exactly as it was.
    LandscapeRoot root;
    root.QuadsPerTile = 7;
    root.SpacingCm    = 100.0f;
    std::map<std::pair<int32_t, int32_t>, LandscapeTileData> tiles;
    tiles.emplace( std::make_pair( 0, 0 ), Tile( 8 ) );
    Fill( tiles.at( { 0, 0 } ), "Rock", 255u );
    Fill( tiles.at( { 0, 0 } ), std::string( kLandscapeVisibilityLayerName ), 128u );
    const LandscapeTileLookup lookup = [&]( int32_t x, int32_t z )
    {
        auto it = tiles.find( { x, z } );
        if ( it == tiles.end() )
            return LandscapeTileSlot{};
        return LandscapeTileSlot{ LandscapeTileState::Present, &it->second };
    };

    // The root's rules name only the paint layers, as LandscapeComponent::Layers does.
    LandscapePaintStroke   stroke( root, lookup, { { "Grass", 0.5f, false }, { "Rock", 0.5f, false } } );
    LandscapeBrushSettings brush;
    brush.RadiusCm = 300.0f;
    brush.Strength = 1.0f;
    const glm::vec2 at( 350.0f, 350.0f );
    auto            weights = ComputeLandscapeBrush( root, brush, { &at, 1 } );
    ASSERT_TRUE( weights.IsSuccess() );
    LandscapePaintSettings paint;
    paint.Layer = "Grass";
    for ( int step = 0; step < 5; ++step )
    {
        const auto applied = stroke.Apply( weights.GetValue(), brush, paint, false );
        ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    }

    const LandscapeTileData& tile = tiles.at( { 0, 0 } );

    const auto grassLayer = tile.FindWeightLayer( "Grass" );
    // clang-tidy 18 does not see gtest's ASSERT as the check it is.
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    ASSERT_TRUE( grassLayer.has_value() );
    const size_t grass = *grassLayer;

    const auto rockLayer = tile.FindWeightLayer( "Rock" );
    ASSERT_TRUE( rockLayer.has_value() );
    const size_t rock = *rockLayer;

    const auto maskLayer = tile.VisibilityLayer();
    ASSERT_TRUE( maskLayer.has_value() );
    const size_t mask = *maskLayer;
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( tile.Weight( grass, 3, 3 ), 255 ) << "the stroke's centre was painted";
    for ( uint32_t z = 0; z < tile.SamplesZ(); ++z )
        for ( uint32_t x = 0; x < tile.SamplesX(); ++x )
        {
            EXPECT_EQ( tile.Weight( mask, x, z ), 128 ) << x << ", " << z;
            EXPECT_EQ( tile.Weight( grass, x, z ) + tile.Weight( rock, x, z ), 255 ) << x << ", " << z;
        }
}

TEST( LandscapePaint, VisibilityReadsItsOwnChannelOfEitherPage )
{
    const vec4 w0( 0.1f, 0.2f, 0.3f, 0.4f );
    const vec4 w1( 0.5f, 0.6f, 0.7f, 0.8f );
    EXPECT_EQ( LandscapeVisibility( w0, w1, 0.0f ), 0.0f ) << "no visibility layer: no hole anywhere";
    for ( int layer = 0; layer < 8; ++layer )
        EXPECT_EQ( LandscapeVisibility( w0, w1, static_cast<float>( layer + 1 ) ),
                   layer < 4 ? w0[layer] : w1[layer - 4] )
             << "layer " << layer;
}

TEST( LandscapePaint, HoleStartsWhereUEsVisibilityMaskFallsBelowItsClipValue )
{
    // UE: mask = 1 - w, clipped below OpacityMaskClipValue 0.3333 — a hole from w > 2/3, i.e. 171 of 255.
    EXPECT_FALSE( LandscapeIsHole( 0.0f ) );
    EXPECT_FALSE( LandscapeIsHole( 0.5f ) );
    EXPECT_FALSE( LandscapeIsHole( 170.0f / 255.0f ) );
    EXPECT_TRUE( LandscapeIsHole( 171.0f / 255.0f ) );
    EXPECT_TRUE( LandscapeIsHole( 1.0f ) );
    // The byte form the collider and the CPU raycast use is the same function.
    for ( int w = 0; w < 256; ++w )
        EXPECT_EQ( LandscapeWeightIsHole( static_cast<uint8_t>( w ) ),
                   LandscapeIsHole( static_cast<float>( w ) / 255.0f ) )
             << w;
    EXPECT_FALSE( LandscapeWeightIsHole( 170u ) );
    EXPECT_TRUE( LandscapeWeightIsHole( 171u ) );
}

TEST( LandscapePaint, TheVisibilityBrushCutsAHoleShiftFillsItAndPhysicsIsTold )
{
    // The editor's Visibility target: paint.Layer = the reserved name, no root layer needed (UE's Visibility
    // tool paints 255, its erase 0). Its writes, unlike a paint layer's, are owed to the collider too.
    LandscapeRoot root;
    root.QuadsPerTile = 7;
    root.SpacingCm    = 100.0f;
    std::map<std::pair<int32_t, int32_t>, LandscapeTileData> tiles;
    tiles.emplace( std::make_pair( 0, 0 ), Tile( 8 ) );
    Fill( tiles.at( { 0, 0 } ), "Rock", 255u );
    const LandscapeTileLookup lookup = [&]( int32_t x, int32_t z )
    {
        auto it = tiles.find( { x, z } );
        if ( it == tiles.end() )
            return LandscapeTileSlot{};
        return LandscapeTileSlot{ LandscapeTileState::Present, &it->second };
    };
    LandscapeTileData& tile = tiles.at( { 0, 0 } );

    LandscapeBrushSettings brush;
    brush.RadiusCm = 300.0f;
    brush.Strength = 1.0f;
    const glm::vec2 at( 350.0f, 350.0f );
    auto            weights = ComputeLandscapeBrush( root, brush, { &at, 1 } );
    ASSERT_TRUE( weights.IsSuccess() );

    // A paint layer's stroke is not the collider's business.
    tile.TakeDirtyRects( LandscapeDirtyConsumer::Physics );
    {
        LandscapePaintStroke   rock( root, lookup, { { "Rock", 0.5f, false } } );
        LandscapePaintSettings paint;
        paint.Layer = "Rock";
        ASSERT_TRUE( rock.Apply( weights.GetValue(), brush, paint, true ).IsSuccess() );
        EXPECT_TRUE( tile.DirtyRects( LandscapeDirtyConsumer::Physics ).empty() );
    }

    LandscapePaintStroke   stroke( root, lookup, { { "Rock", 0.5f, false } } );
    LandscapePaintSettings paint;
    paint.Layer = std::string( kLandscapeVisibilityLayerName );
    for ( int step = 0; step < 8; ++step )
        ASSERT_TRUE( stroke.Apply( weights.GetValue(), brush, paint, false ).IsSuccess() );
    const auto maskLayer = tile.VisibilityLayer();
    ASSERT_TRUE( maskLayer.has_value() );
    // clang-tidy 18 does not see gtest's ASSERT as the check it is.
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    const size_t mask = *maskLayer;
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( tile.Weight( mask, 3, 3 ), 255 ) << "the stroke's centre is a hole";
    EXPECT_TRUE( LandscapeWeightIsHole( tile.Weight( mask, 3, 3 ) ) );
    EXPECT_EQ( tile.Weight( mask, 7, 7 ), 0 ) << "outside the brush";
    EXPECT_FALSE( tile.TakeDirtyRects( LandscapeDirtyConsumer::Physics ).empty() )
         << "a hole painted must reach the collider";

    for ( int step = 0; step < 8; ++step )
        ASSERT_TRUE( stroke.Apply( weights.GetValue(), brush, paint, true ).IsSuccess() );
    EXPECT_EQ( tile.Weight( mask, 3, 3 ), 0 ) << "Shift (invert) fills the hole back";
    EXPECT_FALSE( tile.TakeDirtyRects( LandscapeDirtyConsumer::Physics ).empty() );

    // Undo/redo replaces the layers wholesale: the collider is told there too.
    ASSERT_TRUE( tile.SetWeightLayers( tile.WeightLayers() ).IsSuccess() );
    EXPECT_FALSE( tile.TakeDirtyRects( LandscapeDirtyConsumer::Physics ).empty() );
}
