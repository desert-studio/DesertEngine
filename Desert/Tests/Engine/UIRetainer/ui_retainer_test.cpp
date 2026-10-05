// UE Retainer Box (VIDEO-2c): a subtree recorded into its own layer, composited through one effect.
// Device-free: the layer/mask geometry is what DrawList2D records, and the per-pixel effect is
// RetainerEffect.hpp, which UIRetainer.shader mirrors line for line.
#include <Engine/Graphic/Render2D/DrawList2D.hpp>
#include <Engine/Graphic/Render2D/RetainerEffect.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace Desert::Graphic::Render2D;

namespace
{
    // Alpha a list puts at a pixel centre: the alpha of the last triangle covering it (the lists here
    // draw opaque, non-overlapping shapes, so "last" is "the one").
    float AlphaAt( const DrawList2D& list, glm::vec2 p )
    {
        const auto& v     = list.GetVertices();
        const auto& idx   = list.GetIndices();
        float       a     = 0.0f;
        const auto  cross = []( glm::vec2 a, glm::vec2 b, glm::vec2 c )
        { return ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x ); };
        for ( size_t i = 0; i + 2 < idx.size(); i += 3 )
        {
            const glm::vec2 A   = v[idx[i]].Position;
            const glm::vec2 B   = v[idx[i + 1]].Position;
            const glm::vec2 C   = v[idx[i + 2]].Position;
            const float     d1  = cross( A, B, p );
            const float     d2  = cross( B, C, p );
            const float     d3  = cross( C, A, p );
            const bool      neg = d1 < 0 || d2 < 0 || d3 < 0;
            const bool      pos = d1 > 0 || d2 > 0 || d3 > 0;
            if ( !( neg && pos ) )
                a = v[idx[i]].Color.a;
        }
        return a;
    }
} // namespace

// The sun (content) is a 100x100 square; the dune (mask) covers its left half. Not inverted, the sun shows
// only over the dune; inverted — the case the title needs — only where the dune is NOT.
TEST( UIRetainer, ThePixelUnderTheMaskIsHiddenWhenInvertedAndShownWhenNot )
{
    DrawList2D root;
    auto&      dune = root.MaskLayer( 7 );
    dune.AddRectFilled( { 0.0f, 0.0f }, { 50.0f, 100.0f }, glm::vec4( 1.0f ) );

    uint32_t index = 99;
    auto&    sun   = root.BeginRetainedLayer( &index );
    sun.AddRectFilled( { 0.0f, 0.0f }, { 100.0f, 100.0f }, glm::vec4( 1.0f, 0.8f, 0.3f, 1.0f ) );

    RetainerEffect fx;
    fx.Mask       = true;
    fx.InvertMask = true;
    ASSERT_TRUE( root.AddRetainedComposite( index, 7, fx, glm::vec4( 1.0f ) ) );

    const DrawCommand& cmd = root.GetCommands().back();
    ASSERT_TRUE( cmd.Retained );
    EXPECT_EQ( cmd.RetainedLayer, index );
    EXPECT_EQ( cmd.RetainedMask, 7 );
    EXPECT_EQ( cmd.RetainedRect, glm::vec4( 0.0f, 0.0f, 100.0f, 100.0f ) );
    ASSERT_EQ( root.GetMaskLayers().at( 7 ), 0u );

    const DrawList2D& mask = *root.GetLayers()[root.GetMaskLayers().at( 7 )];
    const glm::vec2   underDune( 25.5f, 50.5f );
    const glm::vec2   clearSky( 75.5f, 50.5f );
    EXPECT_EQ( RetainerMaskCoverage( AlphaAt( mask, underDune ), true ), 0.0f );
    EXPECT_EQ( RetainerMaskCoverage( AlphaAt( mask, clearSky ), true ), 1.0f );
    EXPECT_EQ( RetainerMaskCoverage( AlphaAt( mask, underDune ), false ), 1.0f );
    EXPECT_EQ( RetainerMaskCoverage( AlphaAt( mask, clearSky ), false ), 0.0f );
}

// Two runs at the same FixedStep displace every pixel identically, byte for byte; the next step does not.
TEST( UIRetainer, HazeIsAPureFunctionOfThePixelAndTheClock )
{
    RetainerEffect fx;
    fx.Haze          = true;
    fx.HazeAmplitude = 4.0f;
    fx.HazeScale     = 24.0f;
    fx.HazeSpeed     = 1.5f;

    const auto run = [&]( float time )
    {
        RetainerEffect at = fx;
        at.Time           = time;
        std::vector<glm::vec2> out;
        for ( int y = 0; y < 64; ++y )
            for ( int x = 0; x < 64; ++x )
                out.push_back( RetainerHazeOffsetPx(
                     at, glm::vec2( static_cast<float>( x ) + 0.5f, static_cast<float>( y ) + 0.5f ) ) );
        return out;
    };
    const float step = 1.0f / 60.0f;
    const auto  a    = run( 9 * step );
    const auto  b    = run( 9 * step );
    const auto  c    = run( 10 * step );
    ASSERT_EQ( a.size(), b.size() );
    EXPECT_EQ( std::memcmp( a.data(), b.data(), a.size() * sizeof( glm::vec2 ) ), 0 );
    EXPECT_NE( std::memcmp( a.data(), c.data(), a.size() * sizeof( glm::vec2 ) ), 0 );
    for ( const glm::vec2& o : a )
    {
        EXPECT_LE( std::abs( o.x ), fx.HazeAmplitude );
        EXPECT_LE( std::abs( o.y ), fx.HazeAmplitude );
    }
    fx.Haze = false;
    EXPECT_EQ( RetainerHazeOffsetPx( fx, glm::vec2( 3.0f, 4.0f ) ), glm::vec2( 0.0f ) );
}

// The composite covers what the layer drew grown by the haze amplitude (a displaced pixel may land outside
// the shape), in whole pixels, as its own command; an empty layer records nothing.
TEST( UIRetainer, TheCompositeCoversTheLayerPlusTheHazeAndIsItsOwnCommand )
{
    DrawList2D root;
    root.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, glm::vec4( 1.0f ) );
    uint32_t index = 0;
    auto&    layer = root.BeginRetainedLayer( &index );
    layer.AddRectFilled( { 20.25f, 30.0f }, { 60.0f, 70.5f }, glm::vec4( 1.0f ) );

    RetainerEffect fx;
    fx.Haze             = true;
    fx.HazeAmplitude    = 2.5f;
    const size_t before = root.GetCommands().size();
    ASSERT_TRUE( root.AddRetainedComposite( index, -1, fx, glm::vec4( 1.0f ) ) );
    ASSERT_EQ( root.GetCommands().size(), before + 1 );
    EXPECT_EQ( root.GetCommands().back().RetainedRect, glm::vec4( 17.0f, 27.0f, 63.0f, 74.0f ) );
    EXPECT_EQ( root.GetCommands().back().IndexCount, 6u );

    uint32_t empty = 0;
    (void)root.BeginRetainedLayer( &empty );
    EXPECT_FALSE( root.AddRetainedComposite( empty, -1, fx, glm::vec4( 1.0f ) ) );

    root.Reset();
    EXPECT_TRUE( root.GetLayers().empty() );
    EXPECT_TRUE( root.GetMaskLayers().empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
