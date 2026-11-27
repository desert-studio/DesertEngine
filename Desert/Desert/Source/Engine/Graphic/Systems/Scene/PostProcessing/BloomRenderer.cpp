#include "BloomRenderer.hpp"
#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/PostProcessing/BloomConvolutionRules.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr Core::Formats::ImageFormat kBloomFormat = ViewTargetFormats::kBloom;
        constexpr uint32_t                   kGroupSize   = 16; // must match local_size_* in the shaders



        inline uint32_t MipSize( uint32_t base, uint32_t mip )
        {
            return std::max( 1u, base >> mip );
        }

        inline uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }
    } // namespace

    Common::BoolResultStr BloomRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "BloomRenderer: failed to create compute pipelines" );

        if ( const auto valid = m_SetupLayout.Validate( m_SetupPipeline->GetSpecification().Shader,
                                                        BloomSetupShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_DownsampleLayout.Validate( m_DownsamplePipeline->GetSpecification().Shader,
                                                             BloomDownsampleShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_BlurLayout.Validate( m_BlurPipeline->GetSpecification().Shader,
                                                       BloomGaussianBlurShaderParameters::kAll );
             !valid )
            return valid;
        // The convolution's programs: a failure leaves the Gaussian chain (the method every view can run) and is
        // named once here; PrepareConvolution then says "pipelines did not build" on its Gaussian line.
        const auto check = [&]( ConvolutionProgram& program, const auto& all ) -> bool
        {
            if ( !program.Pipeline )
                return false;
            if ( const auto valid = program.Layout.Validate( program.Pipeline->GetSpecification().Shader, all ); !valid )
            {
                LOG_ERROR( "BloomRenderer: {}", valid.GetError() );
                return false;
            }
            return true;
        };
        const bool pack     = check( m_Pack, BloomConvolutionPackShaderParameters::kAll );
        const bool place    = check( m_KernelPlace, BloomConvolutionKernelShaderParameters::kAll );
        const bool fft      = check( m_FFT, BloomFFTShaderParameters::kAll );
        const bool convolve = check( m_Convolve, BloomConvolveShaderParameters::kAll );
        const bool unpack   = check( m_Unpack, BloomConvolutionUnpackShaderParameters::kAll );
        m_ConvolutionReady  = pack && place && fft && convolve && unpack;
        return BOOLSUCCESS;
    }

    bool BloomRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

        const auto make = [&]( const std::string& shaderName ) -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaderService->GetByName( shaderName );
            if ( !shader )
            {
                LOG_ERROR( "BloomRenderer: missing compute shader '{}'", shaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = shaderName } );
            if ( !built )
            {
                LOG_ERROR( "BloomRenderer: {}", built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        };

        m_SetupPipeline        = make( "BloomSetup" );
        m_DownsamplePipeline   = make( "BloomDownsample" );
        m_BlurPipeline         = make( "BloomGaussianBlur" );
        m_Pack.Pipeline        = make( "BloomConvolutionPack" );
        m_KernelPlace.Pipeline = make( "BloomConvolutionKernel" );
        m_FFT.Pipeline         = make( "BloomFFT" );
        m_Convolve.Pipeline    = make( "BloomConvolve" );
        m_Unpack.Pipeline      = make( "BloomConvolutionUnpack" );

        return m_SetupPipeline && m_DownsamplePipeline && m_BlurPipeline;
    }

    std::optional<RDG::TextureDesc> BloomRenderer::GetSetupDesc() const
    {
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !scene->GetColorAttachmentImage() || !m_SetupPipeline || !m_DownsamplePipeline ||
             !m_BlurPipeline )
            return std::nullopt;
        // UE's bloom setup runs at half resolution (PostProcessBloomSetup.cpp: the 1/2 scene downsample).
        return RDG::TextureDesc{ .Size   = { .Width  = std::max( 1u, scene->GetFramebufferWidth() / 2 ),
                                             .Height = std::max( 1u, scene->GetFramebufferHeight() / 2 ) },
                                 .Format = kBloomFormat,
                                 .Mips   = 1 };
    }

    RDG::TextureDesc BloomRenderer::GetStageDesc( const RDG::TextureDesc& setupDesc, const uint32_t stage )
    {
        RDG::TextureDesc desc = setupDesc;
        desc.Size.Width       = MipSize( setupDesc.Size.Width, stage );
        desc.Size.Height      = MipSize( setupDesc.Size.Height, stage );
        desc.Mips             = 1;
        return desc;
    }

    void BloomRenderer::DeclareSetupBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor,
                                              RDG::TextureRef setup, RDG::TextureRef adaptedLuminance,
                                              RDG::TextureRef localExposureGrid,
                                              RDG::TextureRef blurredLogLuminance ) const
    {
        if ( !m_SetupPipeline )
            return; // RecordSetup refuses by name
        pass.Bindings( m_SetupLayout.Get( m_SetupPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_SetupPipeline ) )
             .Texture( BloomSetupShaderParameters::SceneColorTexture, sceneColor, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( BloomSetupShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Sampler( BloomSetupShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Texture( BloomSetupShaderParameters::AdaptedLuminanceTexture, adaptedLuminance,
                       RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Texture( BloomSetupShaderParameters::LumBilateralGrid, localExposureGrid,
                       RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Texture( BloomSetupShaderParameters::BlurredLogLuminance, blurredLogLuminance,
                       RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Storage( BloomSetupShaderParameters::BloomSetupTexture, setup, RDG::Access::StorageWrite, 0 )
             .PushConstants<ShaderPush::BloomSetup>();
    }

    Common::BoolResultStr BloomRenderer::RecordSetup( const RDG::PassContext& context,
                                                      const RDG::TextureDesc& setupDesc )
    {
        if ( !m_SetupPipeline )
            return Common::MakeError( "BloomRenderer: the setup pipeline is not initialised" );
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene )
            return Common::MakeError( "BloomRenderer: the scene framebuffer is gone" );

        // Local exposure: the composite's own numbers (TonemapRenderer::FillMaterial).
        const TonemapRenderer::LocalExposureFrame& le = m_LocalExposure;
        const LocalExposureParams                  p  = le.Produced ? le.Params : LocalExposureParams{};
        ShaderPush::BloomSetup                     push;
        push.SourceTexelSize    = glm::vec2( 1.0f / static_cast<float>( scene->GetFramebufferWidth() ),
                                             1.0f / static_cast<float>( scene->GetFramebufferHeight() ) );
        push.Threshold          = m_Frame.Threshold;
        push.ExposureKey        = m_Frame.ExposureKey;
        push.AutoExposure       = m_Frame.AutoExposure ? 1 : 0;
        push.Exposure           = m_ManualExposure;
        push.InversePreExposure = 1.0f / std::max( m_PreExposure, 1e-30f );
        push.LocalExposureContrast =
             glm::vec4( p.HighlightContrast, p.ShadowContrast, p.DetailStrength, p.BlurredLumBlend );
        push.LocalExposureThreshold = glm::vec4( p.HighlightThreshold, p.ShadowThreshold,
                                                 p.HighlightThresholdStrength, p.ShadowThresholdStrength );
        push.LocalExposureUV        = glm::vec4( le.GridUVScale, le.BlurUVScale );
        push.LocalExposureSlice     = glm::vec4( le.LogMiddleGrey, le.SliceLogMin, 1.0f / kLocalExposureSliceEV,
                                             le.Produced ? 1.0f : 0.0f );

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( push );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_SetupPipeline,
                                                        GroupCount( setupDesc.Size.Width ),
                                                        GroupCount( setupDesc.Size.Height ), 1 );
    }

    void BloomRenderer::DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef source,
                                                   RDG::TextureRef destination ) const
    {
        if ( !m_DownsamplePipeline )
            return; // RecordDownsample refuses by name
        pass.Bindings( m_DownsampleLayout.Get( m_DownsamplePipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_DownsamplePipeline ) )
             .Texture( BloomDownsampleShaderParameters::SourceTexture, source, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( BloomDownsampleShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Storage( BloomDownsampleShaderParameters::DestinationTexture, destination, RDG::Access::StorageWrite,
                       0 )
             .PushConstants<ShaderPush::BloomDownsample>();
    }

    Common::BoolResultStr BloomRenderer::RecordDownsample( const RDG::PassContext& context,
                                                           const RDG::TextureDesc& sourceDesc,
                                                           const RDG::TextureDesc& destinationDesc ) const
    {
        if ( !m_DownsamplePipeline )
            return Common::MakeError( "BloomRenderer: the downsample pipeline is not initialised" );
        const ShaderPush::BloomDownsample push{
             .SourceTexelSize = glm::vec2( 1.0f / static_cast<float>( sourceDesc.Size.Width ),
                                           1.0f / static_cast<float>( sourceDesc.Size.Height ) ) };
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( push );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_DownsamplePipeline,
                                                        GroupCount( destinationDesc.Size.Width ),
                                                        GroupCount( destinationDesc.Size.Height ), 1 );
    }

    void BloomRenderer::DeclareBlurBindings( RDG::PassBuilder& pass, RDG::TextureRef source,
                                             RDG::TextureRef additive, RDG::TextureRef destination,
                                             RDG::BufferRef kernel ) const
    {
        if ( !m_BlurPipeline )
            return; // RecordBlur refuses by name
        pass.Bindings( m_BlurLayout.Get( m_BlurPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_BlurPipeline ) )
             .Texture( BloomGaussianBlurShaderParameters::SourceTexture, source, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Texture( BloomGaussianBlurShaderParameters::AdditiveTexture, additive, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( BloomGaussianBlurShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Storage( BloomGaussianBlurShaderParameters::DestinationTexture, destination,
                       RDG::Access::StorageWrite, 0 )
             .Storage( BloomGaussianBlurShaderParameters::KernelBuffer, kernel, RDG::Access::StorageRead )
             .PushConstants<ShaderPush::BloomGaussianBlur>();
    }

    Common::BoolResultStr BloomRenderer::RecordBlur( const RDG::PassContext&              context,
                                                     const ShaderPush::BloomGaussianBlur& push,
                                                     const RDG::TextureDesc&              destinationDesc ) const
    {
        if ( !m_BlurPipeline )
            return Common::MakeError( "BloomRenderer: the Gaussian blur pipeline is not initialised" );
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( push );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_BlurPipeline,
                                                        GroupCount( destinationDesc.Size.Width ),
                                                        GroupCount( destinationDesc.Size.Height ), 1 );
    }

    ShaderPush::BloomGaussianBlur BloomRenderer::MakeBlurPush( const RDG::TextureDesc&      stageDesc,
                                                               const BloomGaussian::Kernel& kernel,
                                                               const bool vertical, const glm::vec3& tint,
                                                               const bool additive )
    {
        // UE AddGaussianBloomPasses: the horizontal pass is plain, the vertical one carries the stage's tint and
        // adds the coarser stage's result (FGaussianBlurInputs::TintColor / Additive). One kernel serves both
        // axes, its radius taken from the stage's width (PostProcessWeightedSampleSum.cpp:476).
        ShaderPush::BloomGaussianBlur push;
        push.Tint        = vertical ? glm::vec4( tint, 1.0f ) : glm::vec4( 1.0f );
        push.Direction   = vertical ? glm::vec2( 0.0f, 1.0f / static_cast<float>( stageDesc.Size.Height ) )
                                    : glm::vec2( 1.0f / static_cast<float>( stageDesc.Size.Width ), 0.0f );
        push.SampleCount = static_cast<std::int32_t>( kernel.SampleCount );
        push.UseAdditive = vertical && additive ? 1 : 0;
        return push;
    }

    const BloomGaussian::Kernel& BloomRenderer::AcquireKernel( RDG::Builder& graph, const uint32_t stage,
                                                               const uint32_t  stageWidth,
                                                               const float     kernelSizePercent,
                                                               RDG::BufferRef& buffer )
    {
        StageKernel&      cached = m_StageKernels[stage];
        const std::string name   = std::format( "Bloom.Kernel{}", stage );
        if ( cached.Buffer.Physical && cached.StageWidth == stageWidth && cached.SizePercent == kernelSizePercent )
        {
            buffer = graph.RegisterExternal( cached.Buffer, name );
            return cached.Kernel;
        }
        cached.StageWidth  = stageWidth;
        cached.SizePercent = kernelSizePercent;
        cached.Kernel =
             BloomGaussian::Compute1DKernel( BloomGaussian::BlurRadius( stageWidth, kernelSizePercent ) );
        cached.Buffer = {};
        buffer        = graph.CreateBuffer( RDG::BufferDesc{ sizeof( cached.Kernel.OffsetWeight ) }, name );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span( cached.Kernel.OffsetWeight ) ) );
        graph.Extract( buffer, cached.Buffer, RDG::Access::StorageRead );
        LOG_DEBUG( "[Bloom] kernel stage {} uploaded: width {} size {}% samples {}", stage, stageWidth,
                   kernelSizePercent, cached.Kernel.SampleCount );
        return cached.Kernel;
    }

    std::optional<BloomRenderer::ConvolutionPlan>
    BloomRenderer::PrepareConvolution( const RDG::TextureDesc& setupDesc, const Texture2D* kernel,
                                       const std::string& kernelName )
    {
        const auto report = [this]( std::string line ) { ReportOutcome( std::move( line ), false ); };
        if ( m_Frame.Method != Core::BloomMethod::Convolution )
        {
            report( "[Bloom] method=Gaussian" );
            return std::nullopt;
        }
        if ( !m_ConvolutionReady )
        {
            report( "[Bloom] method=Gaussian (view Convolution: the convolution pipelines did not build)" );
            return std::nullopt;
        }
        if ( static_cast<uint64_t>( m_Kernel ) == 0 )
        {
            report( "[Bloom] method=Gaussian (view Convolution: "
                    "the view's post-process names no convolution kernel)" );
            return std::nullopt;
        }
        if ( kernel == nullptr || !kernel->GetImageHandle().IsValid() )
        {
            report( std::format( "[Bloom] method=Gaussian (view Convolution: kernel {} is not loaded yet)",
                                 kernelName ) );
            return std::nullopt;
        }

        // The FFT size: the half-resolution input plus its zero border, rounded up to a power of two, capped; when
        // the cap bites, the content shrinks to fit (UE does the same through its buffer scale).
        // BloomConvolutionFFTSize is also what the view's memory census lists (ViewTargetCensus "Bloom.Spectrum").
        const float        bw  = static_cast<float>( setupDesc.Size.Width );
        const float        bh  = static_cast<float>( setupDesc.Size.Height );
        const BloomFFTSize fft = BloomConvolutionFFTSize( setupDesc.Size.Width, setupDesc.Size.Height );
        const float        pad = fft.Pad;
        ConvolutionPlan    plan;
        plan.KernelName = kernelName;
        plan.Width      = fft.Width;
        plan.Height     = fft.Height;
        plan.Log2Width  = static_cast<uint32_t>( std::countr_zero( plan.Width ) );
        plan.Log2Height = static_cast<uint32_t>( std::countr_zero( plan.Height ) );
        const float fit = std::min( { 1.0f, ( static_cast<float>( plan.Width ) - pad ) / bw,
                                      ( static_cast<float>( plan.Height ) - pad ) / bh } );
        plan.Content = glm::max( glm::floor( glm::vec2( bw, bh ) * std::max( fit, 0.25f ) ), glm::vec2( 1.0f ) );
        // UE BloomConvolutionSize: the kernel image's larger side spans this fraction of the content's minor axis.
        const float kw   = static_cast<float>( std::max( 1u, kernel->GetWidth() ) );
        const float kh   = static_cast<float>( std::max( 1u, kernel->GetHeight() ) );
        const float span = std::max( 1.0f, m_KernelScale * std::min( plan.Content.x, plan.Content.y ) );
        plan.KernelSize  = glm::vec2( kw, kh ) * ( span / std::max( kw, kh ) );
        plan.KernelLod   = std::max( 0.0f, std::log2( std::max( kw, kh ) / span ) );

        if ( !m_KernelSpectrum || m_KernelSpectrum->GetWidth() != 2 * plan.Width ||
             m_KernelSpectrum->GetHeight() != plan.Height )
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "BloomKernelSpectrum",
                 .Width      = 2 * plan.Width,
                 .Height     = plan.Height,
                 .Format     = ViewTargetFormats::kBloomSpectrum,
                 .Mips       = 1u,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_KernelSpectrum = Image2D::Create( spec );
            m_BuiltKernel    = nullptr; // a new image holds nothing yet
        }
        if ( !m_KernelSpectrum )
        {
            ReportOutcome( "[Bloom] method=Gaussian (view Convolution: the kernel spectrum image was not created)",
                           true );
            return std::nullopt;
        }
        // The resident size is part of the key: a streamed-in finer mip changes what the kernel image samples.
        plan.KernelResident = glm::uvec2( kernel->GetWidth(), kernel->GetHeight() );
        plan.KernelImage    = kernel;
        plan.RebuildKernel  = m_BuiltKernel != kernel || m_BuiltScale != m_KernelScale ||
                             m_BuiltContent != plan.Content || m_BuiltResident != plan.KernelResident;
        // The Convolution line is AddConvolutionPasses': a plan the caller then drops must not log it first.
        return plan;
    }

    void BloomRenderer::ReportOutcome( std::string line, const bool warning )
    {
        if ( line == m_LoggedOutcome )
            return;
        if ( warning )
        {
            LOG_WARN( "{}", line );
        }
        else
        {
            LOG_INFO( "{}", line );
        }
        m_LoggedOutcome = std::move( line );
    }

    void BloomRenderer::DropConvolution( const std::string_view reason )
    {
        ReportOutcome( std::format( "[Bloom] method=Gaussian (view Convolution: {})", reason ), true );
    }

    void BloomRenderer::AddConvolutionPasses( RDG::Builder& graph, const ConvolutionPlan& plan,
                                              RDG::TextureRef setup, const RDG::TextureDesc& setupDesc,
                                              RDG::TextureRef kernelImage, RDG::TextureRef kernelSpectrum )
    {
        // The spectrum counts as built from this plan once its passes are in the graph (not when planned: a frame
        // that drops the convolution after PrepareConvolution must not leave a never-written spectrum marked
        // built).
        if ( plan.RebuildKernel )
        {
            m_BuiltKernel   = plan.KernelImage;
            m_BuiltScale    = m_KernelScale;
            m_BuiltContent  = plan.Content;
            m_BuiltResident = plan.KernelResident;
        }
        ReportOutcome( std::format( "[Bloom] method=Convolution kernel={} fft={}x{}", plan.KernelName, plan.Width,
                                    plan.Height ),
                       false );
        const RDG::TextureRef spectrum =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = 2 * plan.Width, .Height = plan.Height },
                                                    .Format = ViewTargetFormats::kBloomSpectrum,
                                                    .Mips   = 1 },
                                  "Bloom.Spectrum" );
        const uint32_t gx = GroupCount( plan.Width );
        const uint32_t gy = GroupCount( plan.Height );

        // One BloomFFT pass over @p target: every row (axis 0) or column (axis 1), forward (-1) or inverse (+1).
        const auto fftPass = [&]( RDG::TextureRef target, uint32_t axis, float direction, std::string_view name )
        {
            const ConvolutionProgram* program = &m_FFT;
            const uint32_t            length  = axis == 0 ? plan.Width : plan.Height;
            const uint32_t            log2    = axis == 0 ? plan.Log2Width : plan.Log2Height;
            const uint32_t            lines   = axis == 0 ? plan.Height : plan.Width;
            const ShaderPush::BloomFFT push{ static_cast<int>( length ), static_cast<int>( log2 ),
                                             static_cast<int>( axis ), direction, static_cast<int>( plan.Width ) };
            graph.AddPass(
                 std::string( name ), RDG::PassFlags::Compute,
                 [program, target]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Storage( BloomFFTShaderParameters::Spectrum, target, RDG::Access::StorageWrite, 0 )
                          .PushConstants<ShaderPush::BloomFFT>();
                 },
                 [program, push, lines]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( push );
                     return Renderer::GetInstance().DispatchCompute( bindings, *program->Pipeline, lines, 1, 1 );
                 } );
        };

        if ( plan.RebuildKernel )
        {
            const ConvolutionProgram*                  program = &m_KernelPlace;
            const ShaderPush::BloomConvolutionKernel push{ plan.KernelSize, plan.KernelLod,
                                                           static_cast<int>( plan.Width ),
                                                           static_cast<int>( plan.Height ) };
            graph.AddPass(
                 "PostFX: BloomConvolutionKernel", RDG::PassFlags::Compute,
                 [program, kernelImage, kernelSpectrum]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Texture( BloomConvolutionKernelShaderParameters::KernelImage, kernelImage,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( BloomConvolutionKernelShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( BloomConvolutionKernelShaderParameters::Spectrum, kernelSpectrum,
                                    RDG::Access::StorageWrite, 0 )
                          .PushConstants<ShaderPush::BloomConvolutionKernel>();
                 },
                 [program, push, gx, gy]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( push );
                     return Renderer::GetInstance().DispatchCompute( bindings, *program->Pipeline, gx, gy, 1 );
                 } );
            fftPass( kernelSpectrum, 0, -1.0f, "PostFX: BloomKernelFFTRows" );
            fftPass( kernelSpectrum, 1, -1.0f, "PostFX: BloomKernelFFTColumns" );
        }

        {
            const ConvolutionProgram*                program = &m_Pack;
            const ShaderPush::BloomConvolutionPack push{ plan.Content, static_cast<int>( plan.Width ),
                                                         static_cast<int>( plan.Height ) };
            graph.AddPass(
                 "PostFX: BloomConvolutionPack", RDG::PassFlags::Compute,
                 [program, setup, spectrum]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Texture( BloomConvolutionPackShaderParameters::BloomInput, setup,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( BloomConvolutionPackShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( BloomConvolutionPackShaderParameters::Spectrum, spectrum,
                                    RDG::Access::StorageWrite, 0 )
                          .PushConstants<ShaderPush::BloomConvolutionPack>();
                 },
                 [program, push, gx, gy]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( push );
                     return Renderer::GetInstance().DispatchCompute( bindings, *program->Pipeline, gx, gy, 1 );
                 } );
        }
        fftPass( spectrum, 0, -1.0f, "PostFX: BloomFFTRows" );
        {
            const ConvolutionProgram*         program = &m_Convolve;
            const ShaderPush::BloomConvolve push{ static_cast<int>( plan.Height ), static_cast<int>( plan.Log2Height ),
                                                  static_cast<int>( plan.Width ) };
            const uint32_t                    columns = plan.Width;
            graph.AddPass(
                 "PostFX: BloomConvolve", RDG::PassFlags::Compute,
                 [program, spectrum, kernelSpectrum]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Storage( BloomConvolveShaderParameters::Spectrum, spectrum, RDG::Access::StorageWrite, 0 )
                          .Storage( BloomConvolveShaderParameters::KernelSpectrum, kernelSpectrum,
                                    RDG::Access::StorageRead, 0 )
                          .PushConstants<ShaderPush::BloomConvolve>();
                 },
                 [program, push, columns]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( push );
                     return Renderer::GetInstance().DispatchCompute( bindings, *program->Pipeline, columns, 1, 1 );
                 } );
        }
        fftPass( spectrum, 0, 1.0f, "PostFX: BloomFFTRowsInverse" );
        {
            const ConvolutionProgram*                  program = &m_Unpack;
            // UE FFTBloom: the convolution's own strength (BloomConvolutionIntensity, times BloomIntensity here).
            const ShaderPush::BloomConvolutionUnpack push{
                 .ContentSize  = plan.Content,
                 .Width        = static_cast<int>( plan.Width ),
                 .InverseCount = 1.0f / ( static_cast<float>( plan.Width ) * static_cast<float>( plan.Height ) ),
                 .Intensity    = m_Frame.ConvolutionIntensity };
            const uint32_t outX = GroupCount( setupDesc.Size.Width );
            const uint32_t outY = GroupCount( setupDesc.Size.Height );
            graph.AddPass(
                 "PostFX: BloomConvolutionUnpack", RDG::PassFlags::Compute,
                 [program, setup, spectrum]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Storage( BloomConvolutionUnpackShaderParameters::Spectrum, spectrum,
                                    RDG::Access::StorageRead, 0 )
                          .Storage( BloomConvolutionUnpackShaderParameters::BloomOutput, setup,
                                    RDG::Access::StorageWrite, 0 )
                          .PushConstants<ShaderPush::BloomConvolutionUnpack>();
                 },
                 [program, push, outX, outY]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( push );
                     return Renderer::GetInstance().DispatchCompute( bindings, *program->Pipeline, outX, outY, 1 );
                 } );
        }
    }
} // namespace Desert::Graphic::System
