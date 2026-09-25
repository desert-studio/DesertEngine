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

#include <map>
#include <numeric>
#include <utility>

using namespace Desert::World::Landscape;

namespace
{
    using glm::mix;
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
    const std::vector<uint8_t> westBefore = LandscapeWeightmapTexels( tiles.at( { 0, 0 } ) );
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
    const std::vector<uint8_t> westStroke = LandscapeWeightmapTexels( tiles.at( { 0, 0 } ) );
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
    EXPECT_EQ( LandscapeWeightmapTexels( tiles.at( { 0, 0 } ) ), westBefore );
    EXPECT_TRUE( tiles.at( { 1, 0 } ).WeightLayers().empty() );

    drainAll();
    ASSERT_TRUE( ApplyLandscapePaintRecord( lookup, record.GetValue(), false ).IsSuccess() );
    EXPECT_FALSE( tiles.at( { 0, 0 } ).DirtyRects( LandscapeDirtyConsumer::Weights ).empty() );
    EXPECT_FALSE( tiles.at( { 1, 0 } ).DirtyRects( LandscapeDirtyConsumer::Weights ).empty() );
    EXPECT_EQ( LandscapeWeightmapTexels( tiles.at( { 0, 0 } ) ), westStroke );
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
    // The root's layer list as LandscapeECSSystem hands it over (ECS::LandscapeLayerInfo has these members).
    struct RootLayer
    {
        std::string Name;
        float       Hardness      = 0.5f;
        bool        NoWeightBlend = false;
        glm::vec3   Color         = glm::vec3( 1.0f );
    };

    const vec4 kC0( 0.9f, 0.1f, 0.1f, 1.0f );
    const vec4 kC1( 0.2f, 0.6f, 0.1f, 1.0f );
    const vec4 kC2( 0.1f, 0.2f, 0.8f, 1.0f );
    const vec4 kC3( 0.5f, 0.5f, 0.5f, 1.0f );
    const vec3 kRule( 0.35f, 0.3f, 0.25f );
    const vec4 kWeightBlendAll( 0.0f );
} // namespace

TEST( LandscapePaint, FullyPaintedLayerIsExactlyItsColour )
{
    for ( const vec3 rule : { kRule, vec3( 0.0f ), vec3( 1.0f ) } )
    {
        const LandscapeWeightBlendResult r =
             LandscapeWeightBlend( vec4( 0.0f, 1.0f, 0.0f, 0.0f ), kC0, kC1, kC2, kC3, kWeightBlendAll, rule );
        EXPECT_EQ( r.Albedo, vec3( kC1 ) );
        EXPECT_EQ( r.RuleShare, 0.0f );
    }
}

TEST( LandscapePaint, UnpaintedGroundKeepsTheRuleAlbedoExactly )
{
    // UE allocates a layer to a tile on its first stroke with every weight zero; the ground around the
    // stroke must stay what the height/slope rules made it, not turn black.
    const LandscapeWeightBlendResult r =
         LandscapeWeightBlend( vec4( 0.0f ), kC0, kC1, kC2, kC3, kWeightBlendAll, kRule );
    EXPECT_EQ( r.Albedo, kRule );
    EXPECT_EQ( r.RuleShare, 1.0f );
}

TEST( LandscapePaint, TwoWeightBlendedLayersMixByTheirWeights )
{
    const vec4                       w( 128.0f / 255.0f, 127.0f / 255.0f, 0.0f, 0.0f );
    const LandscapeWeightBlendResult r = LandscapeWeightBlend( w, kC0, kC1, kC2, kC3, kWeightBlendAll, kRule );
    const vec3                       expected = vec3( kC0 ) * w.x + vec3( kC1 ) * w.y;
    for ( int c = 0; c < 3; ++c )
        EXPECT_NEAR( r.Albedo[c], expected[c], 1e-6f ) << "channel " << c;
    EXPECT_NEAR( r.RuleShare, 0.0f, 1e-6f );
}

TEST( LandscapePaint, ChannelTheRootDoesNotNameIsIgnored )
{
    vec4 unnamed = kC2;
    unnamed.a    = 0.0f;
    const LandscapeWeightBlendResult r =
         LandscapeWeightBlend( vec4( 0.0f, 0.0f, 1.0f, 0.0f ), kC0, kC1, unnamed, kC3, kWeightBlendAll, kRule );
    EXPECT_EQ( r.Albedo, kRule );
    EXPECT_EQ( r.RuleShare, 1.0f );
}

TEST( LandscapePaint, NoWeightBlendLayerIsLaidOverTheBlendNotCountedInIt )
{
    // UE's LB_AlphaBlend: a lerp over the weight-blended result, applied after it.
    const vec4                       alpha3( 0.0f, 0.0f, 0.0f, 1.0f );
    const LandscapeWeightBlendResult half =
         LandscapeWeightBlend( vec4( 0.0f, 0.0f, 0.0f, 0.5f ), kC0, kC1, kC2, kC3, alpha3, kRule );
    const vec3 expected = mix( kRule, vec3( kC3 ), 0.5f );
    for ( int c = 0; c < 3; ++c )
        EXPECT_NEAR( half.Albedo[c], expected[c], 1e-6f ) << "channel " << c;
    EXPECT_NEAR( half.RuleShare, 0.5f, 1e-6f );

    // Full weight on a weight-blended layer AND on the no-weight-blend one: the latter covers, and its
    // weight never took anything from the former's claim.
    const LandscapeWeightBlendResult over =
         LandscapeWeightBlend( vec4( 1.0f, 0.0f, 0.0f, 1.0f ), kC0, kC1, kC2, kC3, alpha3, kRule );
    EXPECT_EQ( over.Albedo, vec3( kC3 ) );
    EXPECT_EQ( over.RuleShare, 0.0f );
}

TEST( LandscapePaint, WeightmapTexelCarriesTheTileLayersInChannelOrder )
{
    LandscapeTileData tile = Tile( 3 );
    Fill( tile, "A", 10u );
    Fill( tile, "B", 200u );
    ASSERT_TRUE(
         tile.WriteWeightRegion( 1u, LandscapeRect{ 2u, 1u, 3u, 2u }, std::vector<uint8_t>{ 77u } ).IsSuccess() );

    const std::vector<uint8_t> texels = LandscapeWeightmapTexels( tile );
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
    EXPECT_EQ( channels.AlphaBlend, vec4( 1.0f, 0.0f, 0.0f, 0.0f ) );
    ASSERT_EQ( channels.Unknown.size(), 1u );
    EXPECT_EQ( channels.Unknown[0], "Ghost" );
}
