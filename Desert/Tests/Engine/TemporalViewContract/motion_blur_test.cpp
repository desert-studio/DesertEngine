// MR2: motion blur from the velocity buffer (Graphic/View/MotionBlur.hpp). Device-free: the tile-max math the
// shaders mirror, the settings that decide whether the nodes exist, the camera's motion in the velocity, and the
// four graph nodes' declared reads and writes.

#include <Engine/Graphic/RDG/RDGBackend.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/View/MotionBlur.hpp>
#include <Engine/Graphic/View/Velocity.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <vector>

namespace MotionBlurTest
{
    using namespace Desert::Graphic;
    using namespace Desert::Graphic::RDG;
    using Desert::Core::Formats::ImageFormat;

    class FlatMemory final : public IMemoryRequirementsProvider
    {
    public:
        ::Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
                                                                        uint32_t ) const override
        {
            return ::Common::MakeSuccess(
                 MemoryRequirements{ uint64_t{ desc.Size.Width } * desc.Size.Height * 8u, 256u, ~0u } );
        }
        ::Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                       uint32_t ) const override
        {
            return ::Common::MakeSuccess( MemoryRequirements{ desc.Bytes, 256u, ~0u } );
        }
    };

    TextureDesc Tex2D( uint32_t width, uint32_t height, ImageFormat format )
    {
        TextureDesc desc;
        desc.Size   = { width, height, 1 };
        desc.Format = format;
        return desc;
    }

    // A 60 Hz frame of a 64x32 render / 128x64 output view with UE's default motion blur at High.
    ViewFrame MovingFrame()
    {
        ViewFrame frame;
        frame.Split.Render = { 64, 32 };
        frame.Split.Output = { 128, 64 };
        frame.DeltaSeconds = 1.0f / 60.0f;
        return frame;
    }

    const MotionBlurSettings kDefault{
         .Amount = 0.5f, .MaxPercent = 5.0f, .TargetFPS = 30.0f, .Samples = kMotionBlurSamplesHigh };
} // namespace MotionBlurTest

using namespace MotionBlurTest;

// TileMax: the longest vector of the tile wins, not the largest component and not the average; NeighborMax: a
// tile with no motion next to a moving one takes the moving one's velocity (the soft edge), and the grid's edge
// is clamped. Mutation: comparing |x| + |y|, averaging, or a 1x1 neighbourhood goes red.
TEST( MotionBlur, TileMaxKeepsTheLongestVelocityAndTheNeighbourhoodSpreadsIt )
{
    const std::vector<glm::vec2> tile = { { 3.0f, 0.0f }, { 2.5f, 2.5f }, { -4.0f, 0.5f }, { 0.0f, 0.0f } };
    // |(2.5, 2.5)| = 3.54 < |(-4, 0.5)| = 4.03, though 2.5 + 2.5 > 4 + 0.5.
    EXPECT_EQ( LongestVelocity( tile ), glm::vec2( -4.0f, 0.5f ) );
    EXPECT_EQ( LongestVelocity( std::vector<glm::vec2>( 4, glm::vec2( 0.0f ) ) ), glm::vec2( 0.0f ) );

    // A 4x3 grid, still except one tile at (3, 2) (the corner).
    std::vector<glm::vec2> grid( 12, glm::vec2( 0.0f ) );
    grid[2 * 4 + 3] = glm::vec2( 0.0f, 7.0f );
    grid[0]         = glm::vec2( 1.0f, 0.0f );
    EXPECT_EQ( NeighborhoodMaxVelocity( grid, 4, 3, 2, 1 ), glm::vec2( 0.0f, 7.0f ) ); // diagonal neighbour
    EXPECT_EQ( NeighborhoodMaxVelocity( grid, 4, 3, 3, 2 ), glm::vec2( 0.0f, 7.0f ) ); // itself, edge clamped
    EXPECT_EQ( NeighborhoodMaxVelocity( grid, 4, 3, 1, 1 ), glm::vec2( 1.0f, 0.0f ) ); // (3, 2) is two away
    EXPECT_EQ( NeighborhoodMaxVelocity( grid, 4, 3, 0, 0 ), glm::vec2( 1.0f, 0.0f ) );

    EXPECT_EQ( MotionBlurTileExtent( ViewExtent{ 64, 33 } ), ( Extent3D{ 4, 3, 1 } ) );
}

// The nodes exist only for a frame that blurs: Amount 0, MotionBlurQuality off (0 samples), MotionBlurMax 0 or a
// frame with no duration under a TargetFPS (paused, reset) add nothing. TargetFPS normalises the blur to its frame
// time: at 60 Hz a 30 fps target doubles it.
TEST( MotionBlur, SettingsDecideTheNodesAndTargetFpsNormalisesTheFrameTime )
{
    const ViewFrame frame = MovingFrame();
    EXPECT_TRUE( MotionBlurRuns( kDefault, frame ) );
    EXPECT_FLOAT_EQ( MotionBlurVelocityScale( kDefault, frame.DeltaSeconds ), 1.0f ); // 0.5 * (1/30) / (1/60)
    EXPECT_FLOAT_EQ( MotionBlurVelocityScale( { .Amount = 0.5f, .TargetFPS = 0.0f }, frame.DeltaSeconds ), 0.5f );

    MotionBlurSettings off = kDefault;
    off.Amount             = 0.0f;
    EXPECT_FALSE( MotionBlurRuns( off, frame ) );
    off         = kDefault;
    off.Samples = MotionBlurSamplesForQuality( 0 );
    EXPECT_FALSE( MotionBlurRuns( off, frame ) );
    off            = kDefault;
    off.MaxPercent = 0.0f;
    EXPECT_FALSE( MotionBlurRuns( off, frame ) );

    ViewFrame paused    = frame;
    paused.DeltaSeconds = 0.0f;
    EXPECT_FALSE( MotionBlurRuns( kDefault, paused ) );
    EXPECT_FLOAT_EQ( MotionBlurVelocityScale( kDefault, 0.0f ), 0.0f );

    EXPECT_EQ( MotionBlurSamplesForQuality( 0 ), 0 );
    EXPECT_EQ( MotionBlurSamplesForQuality( 1 ), kMotionBlurSamplesLow );
    EXPECT_EQ( MotionBlurSamplesForQuality( 2 ), kMotionBlurSamplesHigh );

    const MotionBlurParams params = MakeMotionBlurParams( kDefault, frame );
    EXPECT_EQ( params.RenderSize, glm::vec2( 64.0f, 32.0f ) );
    EXPECT_EQ( params.OutputSize, glm::vec2( 128.0f, 64.0f ) );
    EXPECT_EQ( params.TileCount, glm::vec2( 4.0f, 2.0f ) );
    EXPECT_FLOAT_EQ( params.MaxPixels, 6.4f ); // 5 % of 128
    EXPECT_EQ( params.Samples, kMotionBlurSamplesHigh );
}

// The camera's own motion is in the velocity (VelocityNdc compares the previous view-projection with the current
// one), so a still world point blurs under a moving camera and does not under a still one; the flattened value is
// in output pixels with Velocity.hpp's uv sign and is clamped to MotionBlurMax.
TEST( MotionBlur, CameraMotionIsInTheVelocityAndAStillCameraHasNone )
{
    const glm::mat4 projection = glm::perspective( glm::radians( 60.0f ), 2.0f, 10.0f, 100000.0f );
    const glm::mat4 view = glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0, 1, 0 ) );
    const glm::mat4 prevView = glm::translate( view, glm::vec3( -20.0f, 0.0f, 0.0f ) ); // camera moved 20 cm
    const glm::vec4 point( 0.0f, 0.0f, -500.0f, 1.0f );                                 // still, 5 m ahead

    const glm::vec2 still = VelocityNdc( projection * view * point, projection * view * point );
    EXPECT_EQ( still, glm::vec2( 0.0f ) );
    EXPECT_EQ( MotionBlurPixelVelocity( still, 1.0f, ViewExtent{ 128, 64 }, 6.4f ), glm::vec2( 0.0f ) );

    const glm::vec2 moving = VelocityNdc( projection * view * point, projection * prevView * point );
    EXPECT_GT( glm::length( moving ), 0.0f );
    const glm::vec2 pixels = MotionBlurPixelVelocity( moving, 1.0f, ViewExtent{ 128, 64 }, 1000.0f );
    EXPECT_FLOAT_EQ( pixels.x, moving.x * 0.5f * 128.0f );
    EXPECT_FLOAT_EQ( pixels.y, moving.y * -0.5f * 64.0f );
    const glm::vec2 clamped =
         MotionBlurPixelVelocity( glm::vec2( 1.0f, 0.0f ), 1.0f, ViewExtent{ 128, 64 }, 6.4f );
    EXPECT_FLOAT_EQ( glm::length( clamped ), 6.4f );
}

// The four nodes: Flatten reads the velocity and the depth and writes the flattened velocity; TileMax reads that
// and writes the tiles; NeighborMax reads the tiles and writes the neighbourhood; Gather reads the resolved
// colour, the flattened velocity and the neighbourhood and writes the output (OutputExtent). A frame that does not
// blur is refused by name and adds no node.
TEST( MotionBlur, TheFourNodesDeclareTheirReadsAndWritesAndANonBlurringFrameAddsNone )
{
    ExternalTexture black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture postOut{ Tex2D( 128, 64, ImageFormat::RGBA16F ), Access::None };
    Builder         graph{ "motion-blur" };
    RegisterSystemTextures( graph, black, white, blackCube );

    const ViewFrame  frame    = MovingFrame();
    const TextureRef color    = graph.CreateTexture( Tex2D( 128, 64, ImageFormat::RGBA16F ), "Resolved" );
    const TextureRef velocity = graph.CreateTexture( Tex2D( 64, 32, ViewTargetFormats::kVelocity ), "Velocity" );
    const TextureRef depth = graph.CreateTexture( Tex2D( 64, 32, ViewTargetFormats::kSceneDepth ), "SceneDepth" );
    graph.AddPass(
         "Scene", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, velocity, LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             pass.DepthTarget( depth, LoadOp::ClearDepth( 0.0f ), true );
         },
         []( PassContext& ) -> ::Common::BoolResultStr { return BOOLSUCCESS; } );
    graph.AddPass(
         "Resolve", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, color, LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ); },
         []( PassContext& ) -> ::Common::BoolResultStr { return BOOLSUCCESS; } );

    const MotionBlur   blur;
    MotionBlurSettings off = kDefault;
    off.Samples            = 0;
    const auto refused     = blur.AddPasses( graph, frame, off, { color, depth, velocity } );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "nothing to blur" ), std::string::npos ) << refused.GetError();

    const auto blurred = blur.AddPasses( graph, frame, kDefault, { color, depth, velocity } );
    ASSERT_TRUE( blurred.IsSuccess() ) << blurred.GetError();
    const auto desc = graph.GetTextureDesc( blurred.GetValue() );
    ASSERT_TRUE( desc.IsSuccess() );
    EXPECT_EQ( desc.GetValue().Size, ( Extent3D{ 128, 64, 1 } ) );
    graph.Extract( blurred.GetValue(), postOut, Access::SampledCompute );

    const auto compiled = graph.Compile( FlatMemory{} );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const CompileResult& result = compiled.GetValue();
    EXPECT_TRUE( result.Faults.empty() ) << result.Faults.front().PassName << ": " << result.Faults.front().Reason;
    const auto accessOf = [&]( const char* passName, TextureRef resource )
    {
        uint32_t mask = MemoryAccess_None;
        if ( const CompiledPass* pass = result.FindPass( passName ) )
            for ( const Barrier& barrier : pass->Barriers )
                if ( barrier.Kind == ResourceKind::Texture && barrier.Resource == resource.Index )
                    mask |= barrier.After.Memory;
        return mask;
    };
    const auto written = [&]( const char* passName, const char* by ) -> TextureRef
    {
        // The node's storage-written texture other than the output: found through its reader's sampled read.
        for ( const CompiledPass* pass = result.FindPass( passName ); pass != nullptr; pass = nullptr )
            for ( const Barrier& barrier : pass->Barriers )
                if ( barrier.Kind == ResourceKind::Texture &&
                     ( barrier.After.Memory & MemoryAccess_ShaderStorageWrite ) )
                {
                    const TextureRef ref{ barrier.Resource };
                    if ( accessOf( by, ref ) & MemoryAccess_ShaderSampledRead )
                        return ref;
                }
        return {};
    };
    for ( const char* name :
          { "MotionBlur: Flatten", "MotionBlur: TileMax", "MotionBlur: NeighborMax", "MotionBlur: Gather" } )
        ASSERT_NE( result.FindPass( name ), nullptr ) << name;

    EXPECT_TRUE( accessOf( "MotionBlur: Flatten", velocity ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "MotionBlur: Flatten", depth ) & MemoryAccess_ShaderSampledRead );
    const TextureRef flat = written( "MotionBlur: Flatten", "MotionBlur: TileMax" );
    ASSERT_TRUE( flat.IsValid() ) << "TileMax samples what Flatten writes";
    EXPECT_TRUE( accessOf( "MotionBlur: Gather", flat ) & MemoryAccess_ShaderSampledRead );
    const TextureRef tiles = written( "MotionBlur: TileMax", "MotionBlur: NeighborMax" );
    ASSERT_TRUE( tiles.IsValid() ) << "NeighborMax samples what TileMax writes";
    const TextureRef neighbourhood = written( "MotionBlur: NeighborMax", "MotionBlur: Gather" );
    ASSERT_TRUE( neighbourhood.IsValid() ) << "Gather samples what NeighborMax writes";
    EXPECT_TRUE( accessOf( "MotionBlur: Gather", color ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "MotionBlur: Gather", blurred.GetValue() ) & MemoryAccess_ShaderStorageWrite );
}
