// MR3: depth of field, UE Diaphragm DOF (Graphic/View/DepthOfField.hpp). Device-free: the thin-lens circle of
// confusion the shaders mirror, the focal length from the FOV and the sensor, the device depth back to a distance,
// the settings that decide whether the nodes exist, and the six graph nodes' reads and writes ahead of motion
// blur.

#include <Engine/Graphic/RDG/RDGBackend.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/View/DepthOfField.hpp>
#include <Engine/Graphic/View/MotionBlur.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

namespace DepthOfFieldTest
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

    // tan(hFOV / 2) = 18 / 50: a 36 mm sensor behind a 50 mm lens.
    constexpr float kHalfFovTan = 0.36f;

    // A reversed-Z perspective (near and far swapped) whose HORIZONTAL half-FOV tangent is kHalfFovTan, for a 2:1
    // view: the vertical FOV is narrower, so a focal length taken from Projection[1][1] would be twice as long.
    glm::mat4 LensProjection()
    {
        constexpr float aspect = 2.0f;
        return glm::perspectiveRH_ZO( 2.0f * std::atan( kHalfFovTan / aspect ), aspect, 100000.0f, 10.0f );
    }

    ViewFrame LensFrame()
    {
        ViewFrame frame;
        frame.Split.Render  = { 64, 32 };
        frame.Split.Output  = { 128, 64 };
        frame.Projection    = LensProjection();
        frame.InvProjection = glm::inverse( frame.Projection );
        frame.DeltaSeconds  = 1.0f / 60.0f;
        return frame;
    }

    // f/2, focus 2 m, 36 mm sensor (50 mm from the FOV above), 2.5 % bokeh, High.
    const DepthOfFieldSettings kLens{ .FocalDistanceCm = 200.0f,
                                      .FStop           = 2.0f,
                                      .SensorWidthMm   = 36.0f,
                                      .MaxBokehPercent = 2.5f,
                                      .Rings           = kDofRingsHigh };
} // namespace DepthOfFieldTest

using namespace DepthOfFieldTest;

// The thin lens, worked by hand for f = 50 mm, N = 2 (A = 25 mm), s = 2000 mm:
//   z = 4000 mm: CoC = 25 * 50 * 2000 / (4000 * 1950) = 0.3205128 mm -> 0.3205128 / 36 * 1920 / 2 = 8.547 px
//   radius z = 1000 mm: CoC = 25 * 50 * 1000 / (1000 * 1950) = 0.6410256 mm, IN FRONT -> -17.094 px z = inf: CoC =
//   A f / (s - f) = 0.6410256 mm
// Mutation: a missing A = f / N, cm taken as mm, the radius as the diameter, the sign lost, the focal length from
// the vertical FOV, or no clamp goes red.
TEST( DepthOfField, ThinLensCocIsZeroInFocusAndMatchesKnownLensValues )
{
    const ViewFrame frame = LensFrame();
    EXPECT_NEAR( DofFocalLengthMm( 36.0f, frame.Projection ), 50.0f, 1e-3f );
    const DofLens lens = MakeDofLens( kLens, frame );
    ASSERT_NEAR( lens.FocalLengthMm, 50.0f, 1e-3f );

    EXPECT_FLOAT_EQ( DofCocDiameterMm( lens, 200.0f ), 0.0f );
    EXPECT_FLOAT_EQ( DofCocRadiusPixels( lens, 200.0f, 1920, 1000.0f ), 0.0f );

    EXPECT_NEAR( DofCocDiameterMm( lens, 400.0f ), 0.3205128f, 1e-4f );
    EXPECT_NEAR( DofCocRadiusPixels( lens, 400.0f, 1920, 1000.0f ), 8.547009f, 1e-2f );
    EXPECT_NEAR( DofCocDiameterMm( lens, 100.0f ), 0.6410256f, 1e-4f );
    EXPECT_NEAR( DofCocRadiusPixels( lens, 100.0f, 1920, 1000.0f ), -17.094017f, 2e-2f );
    EXPECT_NEAR( DofCocDiameterMm( lens, std::numeric_limits<float>::infinity() ), 0.6410256f, 1e-4f );

    // A smaller aperture (larger f-number) shrinks the circle in proportion.
    DofLens narrow = lens;
    narrow.FStop   = 4.0f;
    EXPECT_NEAR( DofCocDiameterMm( narrow, 400.0f ), 0.3205128f / 2.0f, 1e-4f );

    // MaxBokehSize clamps both sides.
    EXPECT_FLOAT_EQ( DofCocRadiusPixels( lens, 400.0f, 1920, 5.0f ), 5.0f );
    EXPECT_FLOAT_EQ( DofCocRadiusPixels( lens, 100.0f, 1920, 5.0f ), -5.0f );
}

// The device depth the shaders read maps back to the distance along the view axis through the unjittered inverse
// projection (reversed-Z here). Mutation: a wrong DepthToView column / row, or forgetting the w divide, goes red.
TEST( DepthOfField, DeviceDepthMapsBackToTheViewDistance )
{
    const ViewFrame frame       = LensFrame();
    const glm::vec4 depthToView = DofDepthToView( frame.InvProjection );
    for ( const float distance : { 25.0f, 200.0f, 400.0f, 5000.0f } )
    {
        const glm::vec4 clip  = frame.Projection * glm::vec4( 0.0f, 0.0f, -distance, 1.0f );
        const float     depth = clip.z / clip.w;
        EXPECT_NEAR( DofSceneDistanceCm( depth, depthToView ), distance, distance * 1e-3f ) << distance;
    }
}

// The nodes exist only for a lens that defocuses: focal distance 0 (UE's off), quality 0, no bokeh room, or a
// focus nearer than the focal length adds nothing; the params carry the CoC numerator, the bokeh limit and the
// dilation reach. Mutation: quality off still running, the ring count wrong, the dilation not covering the largest
// bokeh.
TEST( DepthOfField, SettingsDecideTheNodesAndTheParamsCarryTheLens )
{
    EXPECT_EQ( DofRingsForQuality( 0 ), 0 );
    EXPECT_EQ( DofRingsForQuality( 1 ), kDofRingsLow );
    EXPECT_EQ( DofRingsForQuality( 2 ), kDofRingsHigh );
    EXPECT_EQ( DofSampleCount( 3 ), 49 ); // 1 + 8 + 16 + 24
    EXPECT_EQ( DofSampleCount( 0 ), 0 );

    const ViewFrame frame = LensFrame();
    EXPECT_TRUE( DepthOfFieldRuns( kLens, frame ) );
    DepthOfFieldSettings off = kLens;
    off.FocalDistanceCm      = 0.0f;
    EXPECT_FALSE( DepthOfFieldRuns( off, frame ) );
    off       = kLens;
    off.Rings = DofRingsForQuality( 0 );
    EXPECT_FALSE( DepthOfFieldRuns( off, frame ) );
    off                 = kLens;
    off.MaxBokehPercent = 0.0f;
    EXPECT_FALSE( DepthOfFieldRuns( off, frame ) );
    off                 = kLens;
    off.FocalDistanceCm = 4.0f; // 40 mm, nearer than the 50 mm focal length
    EXPECT_FALSE( DepthOfFieldRuns( off, frame ) );

    const DofParams params = MakeDofParams( kLens, frame );
    EXPECT_EQ( params.HalfSize, glm::vec2( 64.0f, 32.0f ) );
    EXPECT_EQ( params.TileCount, glm::vec2( 8.0f, 4.0f ) );
    EXPECT_NEAR( params.FocalLengthMm, 50.0f, 1e-3f );
    EXPECT_FLOAT_EQ( params.FocalDistanceMm, 2000.0f );
    EXPECT_NEAR( params.CocScale, 0.5f * 25.0f * 50.0f * 128.0f / 36.0f, 1e-1f );
    EXPECT_FLOAT_EQ( params.MaxRadius, 0.025f * 128.0f );
    EXPECT_EQ( params.Rings, kDofRingsHigh );
    EXPECT_EQ( DofDilateTiles( 48.0f ), 3 ); // 24 half-res pixels = 3 tiles of 8
    EXPECT_EQ( DofDilateTiles( 50.0f ), 4 );
    EXPECT_EQ( params.DilateTiles, DofDilateTiles( params.MaxRadius ) );
}

// Device-free graph: the six nodes declare their reads and writes, and motion blur (MR2) reads the recombined
// colour, so DOF is ordered before it by the data (UE: TAA -> DOF -> MotionBlur). A frame that does not defocus is
// refused by name and adds nothing. Mutation: a pass reading the wrong layer, Recombine not reading both, or a
// refusal that adds nodes goes red.
TEST( DepthOfField, TheSixNodesDeclareTheirReadsAndWritesAndRunBeforeMotionBlur )
{
    ExternalTexture black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture postOut{ Tex2D( 128, 64, ImageFormat::RGBA16F ), Access::None };
    Builder         graph{ "depth-of-field" };
    RegisterSystemTextures( graph, black, white, blackCube );

    const ViewFrame  frame    = LensFrame();
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

    const DepthOfField   dof;
    DepthOfFieldSettings off = kLens;
    off.Rings                = 0;
    const auto refused       = dof.AddPasses( graph, frame, off, { color, depth } );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "nothing to defocus" ), std::string::npos ) << refused.GetError();

    const auto defocused = dof.AddPasses( graph, frame, kLens, { color, depth } );
    ASSERT_TRUE( defocused.IsSuccess() ) << defocused.GetError();
    const auto desc = graph.GetTextureDesc( defocused.GetValue() );
    ASSERT_TRUE( desc.IsSuccess() );
    EXPECT_EQ( desc.GetValue().Size, ( Extent3D{ 128, 64, 1 } ) );

    const MotionBlur         blur;
    const MotionBlurSettings blurSettings{
         .Amount = 0.5f, .MaxPercent = 5.0f, .TargetFPS = 30.0f, .Samples = kMotionBlurSamplesHigh };
    const auto blurred = blur.AddPasses( graph, frame, blurSettings, { defocused.GetValue(), depth, velocity } );
    ASSERT_TRUE( blurred.IsSuccess() ) << blurred.GetError();
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
    // The texture a node storage-writes that another node samples.
    const auto written = [&]( const char* passName, const char* by ) -> TextureRef
    {
        if ( const CompiledPass* pass = result.FindPass( passName ) )
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
          { "DepthOfField: Setup", "DepthOfField: TileFlatten", "DepthOfField: TileDilate",
            "DepthOfField: GatherForeground", "DepthOfField: GatherBackground", "DepthOfField: Recombine" } )
        ASSERT_NE( result.FindPass( name ), nullptr ) << name;

    EXPECT_TRUE( accessOf( "DepthOfField: Setup", color ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "DepthOfField: Setup", depth ) & MemoryAccess_ShaderSampledRead );
    const TextureRef half = written( "DepthOfField: Setup", "DepthOfField: TileFlatten" );
    ASSERT_TRUE( half.IsValid() ) << "TileFlatten samples what Setup writes";
    EXPECT_TRUE( accessOf( "DepthOfField: GatherForeground", half ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "DepthOfField: GatherBackground", half ) & MemoryAccess_ShaderSampledRead );
    const TextureRef tiles = written( "DepthOfField: TileFlatten", "DepthOfField: TileDilate" );
    ASSERT_TRUE( tiles.IsValid() ) << "TileDilate samples what TileFlatten writes";
    const TextureRef dilated = written( "DepthOfField: TileDilate", "DepthOfField: GatherForeground" );
    ASSERT_TRUE( dilated.IsValid() ) << "GatherForeground samples what TileDilate writes";
    EXPECT_TRUE( accessOf( "DepthOfField: GatherBackground", dilated ) & MemoryAccess_ShaderSampledRead );
    const TextureRef foreground = written( "DepthOfField: GatherForeground", "DepthOfField: Recombine" );
    const TextureRef background = written( "DepthOfField: GatherBackground", "DepthOfField: Recombine" );
    ASSERT_TRUE( foreground.IsValid() ) << "Recombine samples the foreground layer";
    ASSERT_TRUE( background.IsValid() ) << "Recombine samples the background layer";
    EXPECT_NE( foreground.Index, background.Index ) << "the two layers are gathered apart";
    EXPECT_TRUE( accessOf( "DepthOfField: Recombine", color ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "DepthOfField: Recombine", depth ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "DepthOfField: Recombine", defocused.GetValue() ) & MemoryAccess_ShaderStorageWrite );

    // Motion blur reads the recombined colour: DOF runs before it.
    EXPECT_TRUE( accessOf( "MotionBlur: Gather", defocused.GetValue() ) & MemoryAccess_ShaderSampledRead );
    EXPECT_FALSE( accessOf( "MotionBlur: Gather", color ) & MemoryAccess_ShaderSampledRead );
}
