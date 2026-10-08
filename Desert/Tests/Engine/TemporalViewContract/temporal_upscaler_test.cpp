// TAA1-B: the engine's temporal upscaler (TemporalAA, View/TemporalAA.hpp) against its contract
// (View/TemporalUpscaler.hpp): which method each object is, which splits it resolves, the history it declares, and
// what its compute node reads and writes in a graph — all device-free.
#include <Engine/Graphic/RDG/RDGBackend.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/View/SpatialUpscale.hpp>
#include <Engine/Graphic/View/TemporalAA.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <gtest/gtest.h>

#include <string>

namespace TemporalUpscalerTest
{
    using namespace Desert::Graphic;
    using namespace Desert::Graphic::RDG;
    using ::Common::Scalability::ScaleMode;
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

    ResolutionSplit Split( uint32_t render, uint32_t output, ScaleMode mode, int percent )
    {
        ResolutionSplit split;
        split.Render             = { render, render / 2 };
        split.Output             = { output, output / 2 };
        split.Mode               = mode;
        split.RenderScalePercent = percent;
        return split;
    }

    const ResolutionSplit kNative  = Split( 64, 64, ScaleMode::Native, 100 );
    const ResolutionSplit kUpscale = Split( 32, 64, ScaleMode::Upscale, 50 );
    const ResolutionSplit kSuper   = Split( 96, 64, ScaleMode::Supersample, 150 );

    // A frame graph holding what SceneRenderer hands the temporal pass at @p split.
    struct Frame
    {
        ExternalTexture        black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture        white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture        blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture        previous;
        ExternalTexture        current;
        ExternalTexture        exposure{ Tex2D( 1, 1, ImageFormat::RGBA32F ), Access::SampledCompute };
        ExternalTexture        postOut;
        Builder                graph{ "temporal-upscaler" };
        TemporalUpscalerInputs inputs;
        HistoryRefs            history;
        ViewFrame              view;

        Frame( const ITemporalUpscaler& upscaler, const ResolutionSplit& split )
        {
            RegisterSystemTextures( graph, black, white, blackCube );
            const HistoryTextureDesc desc = upscaler.HistoryDescs( split ).at( 0 );
            // As a real frame hands it over: last frame's TemporalAA node wrote it as storage, so this frame's
            // read needs a barrier — and the barrier is what AccessOf sees.
            previous          = ExternalTexture( desc.Desc, Access::StorageWrite );
            current           = ExternalTexture( desc.Desc, Access::None );
            postOut           = ExternalTexture( desc.Desc, Access::None );
            const uint32_t w  = split.Render.Width;
            const uint32_t h  = split.Render.Height;
            inputs.SceneColor = graph.CreateTexture( Tex2D( w, h, ImageFormat::RGBA16F ), "SceneColor" );
            inputs.SceneDepth = graph.CreateTexture( Tex2D( w, h, ImageFormat::DEPTH32F ), "SceneDepth" );
            inputs.Velocity   = graph.CreateTexture( Tex2D( w, h, ImageFormat::RG16F ), "Velocity" );
            graph.AddPass(
                 "Scene", PassFlags::Raster,
                 [&]( PassBuilder& pass )
                 {
                     pass.ColorTarget( 0, inputs.SceneColor, LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.ColorTarget( 1, inputs.Velocity, LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.DepthTarget( inputs.SceneDepth, LoadOp::ClearDepth( 0.0f ) );
                 },
                 []( PassContext& ) -> ::Common::BoolResultStr { return BOOLSUCCESS; } );
            inputs.Exposure  = graph.RegisterExternal( exposure, "AutoExposure.Previous" );
            history.Previous = graph.RegisterExternal( previous, desc.PreviousName );
            history.Current  = graph.RegisterExternal( current, desc.Name );
            inputs.History   = std::span<const HistoryRefs>( &history, 1 );
            view.Split       = split;
            view.Method      = upscaler.Method();
        }

        // The pass's barriers into @p resource, ORed (the accesses the node declared on it).
        MemoryAccessFlags AccessOf( const CompileResult& result, TextureRef resource ) const
        {
            const CompiledPass* pass = result.FindPass( "TemporalAA" );
            uint32_t            mask = MemoryAccess_None;
            if ( pass )
                for ( const Barrier& barrier : pass->Barriers )
                    if ( barrier.Kind == ResourceKind::Texture && barrier.Resource == resource.Index )
                        mask |= barrier.After.Memory;
            return static_cast<MemoryAccessFlags>( mask );
        }
    };
    // TAA1-B step 6: the one per-view resolution function and the two target sets it sizes. The path is what
    // Resolve hands a scene below 100 %: temporal AA (Resolve's step 5 makes the method TAA when it upscales).
    ::Common::Scalability::PathAntiAliasing TemporalPath()
    {
        return { .Method      = ::Common::Scalability::AntiAliasingMethod::TAA,
                 .Samples     = 1,
                 .PostProcess = ::Common::Scalability::AntiAliasingMethod::None };
    }

    TEST( TemporalUpscalerResolution, AtFiftyPercentTheRenderSetIsHalfAndTheOutputSetFull )
    {
        const auto taau     = CreateTemporalUpscaler( TemporalMethod::TAAU );
        const auto resolved = ResolveViewResolution( ViewExtent{ 1920, 1080 }, 50, std::nullopt, TemporalPath(),
                                                     ::Common::Scalability::Upscaler::TAAU,
                                                     [&]( TemporalMethod ) { return taau.get(); } );
        ASSERT_TRUE( resolved ) << resolved.GetError();
        EXPECT_EQ( resolved.GetValue().Method, TemporalMethod::TAAU );
        EXPECT_TRUE( resolved.GetValue().Clamped.empty() );
        EXPECT_EQ( ViewTargetSetExtent( ViewTargetSet::Render, resolved.GetValue().Split ),
                   ( ViewExtent{ 960, 540 } ) );
        EXPECT_EQ( ViewTargetSetExtent( ViewTargetSet::Output, resolved.GetValue().Split ),
                   ( ViewExtent{ 1920, 1080 } ) );
    }

    // The live bug at c57226960: the game setting at 100 % resolves Upscaler None; an editor viewport at 50 %
    // then asked SelectTemporalMethod for "50 % with Upscaler None" and the frame was refused (black viewport).
    // The view applies Resolve's rule (UpscalerForScale) to ITS percent: TAA below 100 % upscales with TAAU.
    TEST( TemporalUpscalerResolution, AViewportOverrideBelowTheSettingUpscalesByResolvesRule )
    {
        const auto taau     = CreateTemporalUpscaler( TemporalMethod::TAAU );
        const auto resolved = ResolveViewResolution( ViewExtent{ 1000, 1000 }, 100, 50, TemporalPath(),
                                                     ::Common::Scalability::Upscaler::None,
                                                     [&]( TemporalMethod ) { return taau.get(); } );
        ASSERT_TRUE( resolved ) << resolved.GetError();
        EXPECT_EQ( resolved.GetValue().Split.Render, ( ViewExtent{ 500, 500 } ) );
        EXPECT_EQ( resolved.GetValue().Method, TemporalMethod::TAAU );
        // The frame's ViewInputs::Upscaler (SceneViewState::BeginFrame re-checks the split with it).
        EXPECT_EQ( resolved.GetValue().Upscaler, ::Common::Scalability::Upscaler::TAAU );
        EXPECT_TRUE( resolved.GetValue().Clamped.empty() );
    }

    // And the other way: a setting at 50 % (resolved TAAU) under a viewport at 100 % runs plain TAA at native.
    TEST( TemporalUpscalerResolution, AViewportOverrideAtNativeDropsTheSettingsUpscaler )
    {
        const auto taa      = CreateTemporalUpscaler( TemporalMethod::TAA );
        const auto resolved = ResolveViewResolution( ViewExtent{ 1000, 1000 }, 50, 100, TemporalPath(),
                                                     ::Common::Scalability::Upscaler::TAAU,
                                                     [&]( TemporalMethod ) { return taa.get(); } );
        ASSERT_TRUE( resolved ) << resolved.GetError();
        EXPECT_EQ( resolved.GetValue().Split.Render, ( ViewExtent{ 1000, 1000 } ) );
        EXPECT_EQ( resolved.GetValue().Method, TemporalMethod::TAA );
        EXPECT_EQ( resolved.GetValue().Upscaler, ::Common::Scalability::Upscaler::None );
    }

    // SCAL-SPATIAL1: below 100 % with no temporal method the spatial upscaler brings the view to the output -
    // Resolve's rule. Mutation: the old 100 % clamp keeps the render set full; a temporal method here is red.
    TEST( TemporalUpscalerResolution, AtFiftyPercentFxaaSplitsAndUpscalesSpatially )
    {
        const ::Common::Scalability::PathAntiAliasing fxaa{
             .Method      = ::Common::Scalability::AntiAliasingMethod::FXAA,
             .Samples     = 1,
             .PostProcess = ::Common::Scalability::AntiAliasingMethod::FXAA };
        const auto resolved =
             ResolveViewResolution( ViewExtent{ 1920, 1080 }, 100, 50, fxaa, ::Common::Scalability::Upscaler::None,
                                    []( TemporalMethod ) -> const ITemporalUpscaler* { return nullptr; } );
        ASSERT_TRUE( resolved ) << resolved.GetError();
        EXPECT_EQ( resolved.GetValue().Split.Mode, ScaleMode::Upscale );
        EXPECT_EQ( resolved.GetValue().Split.Render, ( ViewExtent{ 960, 540 } ) );
        EXPECT_EQ( resolved.GetValue().Split.Output, ( ViewExtent{ 1920, 1080 } ) );
        EXPECT_EQ( resolved.GetValue().Method, TemporalMethod::None );
        EXPECT_EQ( resolved.GetValue().Upscaler, ::Common::Scalability::Upscaler::Spatial );
        EXPECT_TRUE( resolved.GetValue().Clamped.empty() ) << resolved.GetValue().Clamped;
    }
} // namespace TemporalUpscalerTest

using namespace TemporalUpscalerTest;

// Mutations: CreateTemporalUpscaler(TAAU) returning a TAA object -> red; None returning an object -> red.
TEST( TemporalUpscaler, CreateMakesOneObjectPerMethodAndNoneForNone )
{
    EXPECT_EQ( CreateTemporalUpscaler( TemporalMethod::None ), nullptr );
    const auto taa  = CreateTemporalUpscaler( TemporalMethod::TAA );
    const auto taau = CreateTemporalUpscaler( TemporalMethod::TAAU );
    ASSERT_NE( taa, nullptr );
    ASSERT_NE( taau, nullptr );
    EXPECT_EQ( taa->Method(), TemporalMethod::TAA );
    EXPECT_EQ( taau->Method(), TemporalMethod::TAAU );
    EXPECT_NE( taa->DebugName(), taau->DebugName() );
}

TEST( TemporalUpscaler, EachMethodSupportsExactlyItsSplits )
{
    const TemporalAA taa( TemporalMethod::TAA );
    const TemporalAA taau( TemporalMethod::TAAU );
    EXPECT_TRUE( taa.Supports( kNative ) );
    EXPECT_TRUE( taa.Supports( kSuper ) );
    EXPECT_FALSE( taa.Supports( kUpscale ) );
    EXPECT_TRUE( taau.Supports( kUpscale ) );
    EXPECT_FALSE( taau.Supports( kNative ) );
    EXPECT_FALSE( taau.Supports( kSuper ) );
}

// Mutation: TAAU HistoryDescs at the Render extent -> red.
TEST( TemporalUpscaler, HistoryIsOneRgba16fTextureAtTheResolveExtent )
{
    const TemporalAA taa( TemporalMethod::TAA );
    const TemporalAA taau( TemporalMethod::TAAU );
    const auto       native = taa.HistoryDescs( kNative );
    const auto       super  = taa.HistoryDescs( kSuper );
    const auto       up     = taau.HistoryDescs( kUpscale );
    ASSERT_EQ( native.size(), 1u );
    ASSERT_EQ( super.size(), 1u );
    ASSERT_EQ( up.size(), 1u );
    EXPECT_EQ( native[0].Desc.Size, ( Extent3D{ 64, 32, 1 } ) );
    EXPECT_EQ( super[0].Desc.Size, ( Extent3D{ 96, 48, 1 } ) ) << "under SSAA the downsample follows at Render";
    EXPECT_EQ( up[0].Desc.Size, ( Extent3D{ 64, 32, 1 } ) ) << "TAAU's history is the OUTPUT it reconstructs";
    EXPECT_EQ( up[0].Desc.Format, ImageFormat::RGBA16F );
    EXPECT_EQ( std::string( up[0].Name ), "TAA.History" );
    EXPECT_EQ( std::string( up[0].PreviousName ), "TAA.History.Previous" );
}

// Mutation: AddPasses not writing History.Current -> red.
TEST( TemporalUpscaler, TheNodeReadsDepthVelocityAndHistoryAndWritesBothOutputs )
{
    for ( const TemporalMethod method : { TemporalMethod::TAA, TemporalMethod::TAAU } )
    {
        const TemporalAA upscaler( method );
        Frame            frame( upscaler, method == TemporalMethod::TAAU ? kUpscale : kNative );
        const auto       out = upscaler.AddPasses( frame.graph, frame.view, frame.inputs );
        ASSERT_TRUE( out.IsSuccess() ) << out.GetError();
        const TextureRef color = out.GetValue().SceneColor;
        ASSERT_TRUE( color.IsValid() );
        EXPECT_NE( color.Index, frame.history.Current.Index ) << "the post input is not the history";
        const auto desc = frame.graph.GetTextureDesc( color );
        ASSERT_TRUE( desc.IsSuccess() );
        EXPECT_EQ( desc.GetValue().Size, upscaler.ResolveExtent( frame.view.Split ) );
        frame.graph.Extract( color, frame.postOut, Access::SampledCompute );

        const auto compiled = frame.graph.Compile( FlatMemory{} );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        const CompileResult& result = compiled.GetValue();
        EXPECT_TRUE( result.Faults.empty() )
             << result.Faults.front().PassName << ": " << result.Faults.front().Reason;
        ASSERT_NE( result.FindPass( "TemporalAA" ), nullptr );
        EXPECT_TRUE( frame.AccessOf( result, frame.inputs.Velocity ) & MemoryAccess_ShaderSampledRead );
        EXPECT_TRUE( frame.AccessOf( result, frame.inputs.SceneDepth ) & MemoryAccess_ShaderSampledRead );
        EXPECT_TRUE( frame.AccessOf( result, frame.history.Previous ) & MemoryAccess_ShaderSampledRead );
        EXPECT_TRUE( frame.AccessOf( result, frame.history.Current ) & MemoryAccess_ShaderStorageWrite );
        EXPECT_TRUE( frame.AccessOf( result, color ) & MemoryAccess_ShaderStorageWrite );
    }
}

TEST( TemporalUpscaler, AMalformedCallIsAnErrorNamingWhatIsMissing )
{
    const TemporalAA taa( TemporalMethod::TAA );
    {
        Frame frame( taa, kNative );
        frame.inputs.Velocity = {};
        const auto out        = taa.AddPasses( frame.graph, frame.view, frame.inputs );
        ASSERT_FALSE( out.IsSuccess() );
        EXPECT_NE( out.GetError().find( "Velocity" ), std::string::npos ) << out.GetError();
    }
    {
        Frame frame( taa, kNative );
        frame.inputs.History = {};
        const auto out       = taa.AddPasses( frame.graph, frame.view, frame.inputs );
        ASSERT_FALSE( out.IsSuccess() );
        EXPECT_NE( out.GetError().find( "0 histories" ), std::string::npos ) << out.GetError();
    }
}

TEST( TemporalUpscaler, ParamsCarryTheUnjitteredMatricesAndTheResetFlag )
{
    ViewFrame view;
    view.Split                    = kUpscale;
    view.InvViewProjection        = glm::mat4( 2.0f );
    view.PrevViewProjection       = glm::mat4( 3.0f );
    view.JitterNdc                = glm::vec2( 0.2f, 0.4f );
    view.HistoryReset             = HistoryResetReason::None;
    const TemporalAAParams params = MakeTemporalAAParams( view, Extent3D{ 64, 32, 1 } );
    EXPECT_EQ( params.InvViewProjection, view.InvViewProjection );
    EXPECT_EQ( params.PrevViewProjection, view.PrevViewProjection );
    EXPECT_EQ( params.RenderSize, glm::vec2( 32.0f, 16.0f ) );
    EXPECT_EQ( params.OutputSize, glm::vec2( 64.0f, 32.0f ) );
    EXPECT_EQ( params.JitterUv, glm::vec2( 0.1f, -0.2f ) );
    EXPECT_EQ( params.HistoryValid, 1.0f );
    view.HistoryReset = HistoryResetReason::FirstFrame;
    EXPECT_EQ( MakeTemporalAAParams( view, Extent3D{ 64, 32, 1 } ).HistoryValid, 0.0f );
}

// SCAL-SPATIAL1: which frames sharpen and upscale spatially. Mutation: SharpenRuns ignoring the percent, or
// sharpening a native FXAA frame (nothing resolved it), or IsSpatialUpscale true under TAAU -> red.
TEST( SpatialUpscale, SharpenFollowsAResolveAndSpatialIsUpscaleWithoutATemporalMethod )
{
    ViewFrame spatial;
    spatial.Split  = kUpscale;
    spatial.Method = TemporalMethod::None;
    ViewFrame taau = spatial;
    taau.Method    = TemporalMethod::TAAU;
    ViewFrame nativeFxaa;
    nativeFxaa.Split    = kNative;
    ViewFrame nativeTaa = nativeFxaa;
    nativeTaa.Method    = TemporalMethod::TAA;
    EXPECT_TRUE( IsSpatialUpscale( spatial ) );
    EXPECT_FALSE( IsSpatialUpscale( taau ) );
    EXPECT_FALSE( IsSpatialUpscale( nativeFxaa ) );
    EXPECT_TRUE( SharpenRuns( spatial, 20 ) );
    EXPECT_TRUE( SharpenRuns( taau, 20 ) );
    EXPECT_TRUE( SharpenRuns( nativeTaa, 1 ) );
    EXPECT_FALSE( SharpenRuns( nativeFxaa, 100 ) );
    EXPECT_FALSE( SharpenRuns( taau, 0 ) );
}

// SCAL-SPATIAL1 graph census: the spatial upscale is one compute node reading the render-extent scene colour and
// writing an output-extent transient; the sharpen is one compute node reading that and writing its own.
// Mutation: either node sized at the render extent, a missing sampled read or storage write, or a node name
// other than the one SceneRenderer's census and the profiler read -> red.
TEST( SpatialUpscale, TheNodesReadTheResolvedColourAndWriteTheOutputExtent )
{
    ExternalTexture black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture postOut{ Tex2D( kUpscale.Output.Width, kUpscale.Output.Height, ImageFormat::RGBA16F ),
                             Access::None };
    Builder         graph{ "spatial-upscale" };
    RegisterSystemTextures( graph, black, white, blackCube );
    const TextureRef scene = graph.CreateTexture(
         Tex2D( kUpscale.Render.Width, kUpscale.Render.Height, ImageFormat::RGBA16F ), "SceneColor" );
    graph.AddPass(
         "Scene", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, scene, LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ); },
         []( PassContext& ) -> ::Common::BoolResultStr { return BOOLSUCCESS; } );
    ViewFrame view;
    view.Split  = kUpscale;
    view.Method = TemporalMethod::None;

    const SpatialUpscale spatial;
    const Sharpen        sharpen;
    const auto           upscaled = spatial.AddPasses( graph, view, scene );
    ASSERT_TRUE( upscaled.IsSuccess() ) << upscaled.GetError();
    const auto sharpened = sharpen.AddPasses( graph, view, upscaled.GetValue(), 50 );
    ASSERT_TRUE( sharpened.IsSuccess() ) << sharpened.GetError();
    for ( const TextureRef ref : { upscaled.GetValue(), sharpened.GetValue() } )
    {
        const auto desc = graph.GetTextureDesc( ref );
        ASSERT_TRUE( desc.IsSuccess() );
        EXPECT_EQ( desc.GetValue().Size, ( Extent3D{ kUpscale.Output.Width, kUpscale.Output.Height, 1 } ) );
    }
    graph.Extract( sharpened.GetValue(), postOut, Access::SampledCompute );

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
    ASSERT_NE( result.FindPass( "SpatialUpscale" ), nullptr );
    ASSERT_NE( result.FindPass( "Sharpen" ), nullptr );
    EXPECT_TRUE( accessOf( "SpatialUpscale", scene ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "SpatialUpscale", upscaled.GetValue() ) & MemoryAccess_ShaderStorageWrite );
    EXPECT_TRUE( accessOf( "Sharpen", upscaled.GetValue() ) & MemoryAccess_ShaderSampledRead );
    EXPECT_TRUE( accessOf( "Sharpen", sharpened.GetValue() ) & MemoryAccess_ShaderStorageWrite );

    // Not a spatial frame (a temporal method resolves it): the upscale refuses by name.
    ViewFrame taau     = view;
    taau.Method        = TemporalMethod::TAAU;
    const auto refused = spatial.AddPasses( graph, taau, scene );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "not a spatial upscale" ), std::string::npos ) << refused.GetError();
}
