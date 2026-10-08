// TAA1-B: the engine's temporal upscaler (TemporalAA, View/TemporalAA.hpp) against its contract
// (View/TemporalUpscaler.hpp): which method each object is, which splits it resolves, the history it declares, and
// what its compute node reads and writes in a graph — all device-free.
#include <Engine/Graphic/RDG/RDGBackend.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
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
