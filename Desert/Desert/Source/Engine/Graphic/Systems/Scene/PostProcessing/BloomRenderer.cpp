#include "BloomRenderer.hpp"
#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
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

        if ( const auto valid = m_DownsampleLayout.Validate( m_DownsamplePipeline->GetSpecification().Shader, BloomDownsampleShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_UpsampleLayout.Validate( m_UpsamplePipeline->GetSpecification().Shader, BloomUpsampleShaderParameters::kAll );
             !valid )
            return valid;
        // The convolution's programs: a failure leaves the Gaussian chain (the method every tier can run) and is
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

        m_DownsamplePipeline = make( "BloomDownsample" );
        m_UpsamplePipeline   = make( "BloomUpsample" );
        m_Pack.Pipeline        = make( "BloomConvolutionPack" );
        m_KernelPlace.Pipeline = make( "BloomConvolutionKernel" );
        m_FFT.Pipeline         = make( "BloomFFT" );
        m_Convolve.Pipeline    = make( "BloomConvolve" );
        m_Unpack.Pipeline      = make( "BloomConvolutionUnpack" );

        return m_DownsamplePipeline && m_UpsamplePipeline;
    }

    std::optional<RDG::TextureDesc> BloomRenderer::GetChainDesc() const
    {
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !scene->GetColorAttachmentImage() || !m_DownsamplePipeline || !m_UpsamplePipeline )
            return std::nullopt;
        // Half-resolution chain (mip 0 = scene / 2), capped so the smallest mip stays usable.
        const uint32_t bw = std::max( 1u, scene->GetFramebufferWidth() / 2 );
        const uint32_t bh = std::max( 1u, scene->GetFramebufferHeight() / 2 );
        return RDG::TextureDesc{ .Size   = { .Width = bw, .Height = bh },
                                 .Format = kBloomFormat,
                                 .Mips   = std::min( m_MaxMips, Utils::CalculateMipCount( bw, bh ) ) };
    }

    void BloomRenderer::DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor,
                                                   RDG::TextureRef chain, RDG::TextureRef adaptedLuminance,
                                                   RDG::TextureRef localExposureGrid,
                                                   RDG::TextureRef blurredLogLuminance, uint32_t mip ) const
    {
        if ( !m_DownsamplePipeline )
            return;
        auto block = pass.Bindings( m_DownsampleLayout.Get( m_DownsamplePipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_DownsamplePipeline ) );
        if ( mip == 0 )
            block.Texture( BloomDownsampleShaderParameters::SourceMipTexture, sceneColor, RDG::Access::SampledCompute, RDG::SubresourceRange::All() );
        else
            block.Texture( BloomDownsampleShaderParameters::SourceMipTexture, chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) );
        block.Sampler( BloomDownsampleShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Sampler( BloomDownsampleShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Texture( BloomDownsampleShaderParameters::AdaptedLuminanceTexture, adaptedLuminance,
                       RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Texture( BloomDownsampleShaderParameters::LumBilateralGrid, localExposureGrid,
                       RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Texture( BloomDownsampleShaderParameters::BlurredLogLuminance, blurredLogLuminance,
                       RDG::Access::SampledCompute, RDG::SubresourceRange::All() );
        block.Storage( BloomDownsampleShaderParameters::DestinationMipTexture, chain, RDG::Access::StorageWrite, mip )
             .PushConstants<ShaderPush::BloomDownsample>();
    }

    Common::BoolResultStr BloomRenderer::RecordDownsample( const RDG::PassContext& context,
                                                           const RDG::TextureDesc& chainDesc, uint32_t mip )
    {
        if ( !m_DownsamplePipeline )
            return Common::MakeError( "BloomRenderer: the downsample pipeline is not initialised" );
        const bool     first = ( mip == 0 );
        const uint32_t bw    = chainDesc.Size.Width;
        const uint32_t bh    = chainDesc.Size.Height;
        uint32_t       srcW  = 0;
        uint32_t       srcH  = 0;
        if ( first )
        {
            // Mip 0 samples the full-resolution scene colour.
            const auto& scene = m_TargetFramebuffer.lock();
            if ( !scene )
                return Common::MakeError( "BloomRenderer: the scene framebuffer is gone" );
            srcW = scene->GetFramebufferWidth();
            srcH = scene->GetFramebufferHeight();
        }
        else
        {
            srcW = MipSize( bw, mip - 1 );
            srcH = MipSize( bh, mip - 1 );
        }

        // Local exposure: the composite's own numbers (TonemapRenderer::FillMaterial), applied on mip 0 only.
        const TonemapRenderer::LocalExposureFrame& le = m_LocalExposure;
        const LocalExposureParams                  p  = le.Produced ? le.Params : LocalExposureParams{};
        const ShaderPush::BloomDownsample                       push{
             glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
             first ? 1 : 0,
             m_Threshold * m_PreExposure,
             m_ExposureKey,
             m_AutoExposure ? 1 : 0,
             m_ManualExposure,
             1.0f / std::max( m_PreExposure, 1e-30f ),
             glm::vec4( p.HighlightContrast, p.ShadowContrast, p.DetailStrength, p.BlurredLumBlend ),
             glm::vec4( p.HighlightThreshold, p.ShadowThreshold, p.HighlightThresholdStrength,
                                              p.ShadowThresholdStrength ),
             glm::vec4( le.GridUVScale, le.BlurUVScale ),
             glm::vec4( le.LogMiddleGrey, le.SliceLogMin, 1.0f / kLocalExposureSliceEV,
                        le.Produced ? 1.0f : 0.0f ) };

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( push );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_DownsamplePipeline,
                                                        GroupCount( MipSize( bw, mip ) ),
                                                        GroupCount( MipSize( bh, mip ) ), 1 );
    }

    void BloomRenderer::DeclareUpsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef chain,
                                                 uint32_t mip ) const
    {
        if ( !m_UpsamplePipeline || mip == 0 )
            return; // RecordUpsample refuses both by name
        pass.Bindings( m_UpsampleLayout.Get( m_UpsamplePipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_UpsamplePipeline ) )
             .Texture( BloomUpsampleShaderParameters::SourceMipTexture, chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip ) )
             .Sampler( BloomUpsampleShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Storage( BloomUpsampleShaderParameters::DestinationMipTexture, chain, RDG::Access::StorageWrite, mip - 1 )
             .PushConstants<ShaderPush::BloomUpsample>();
    }

    Common::BoolResultStr BloomRenderer::RecordUpsample( const RDG::PassContext& context,
                                                         const RDG::TextureDesc& chainDesc, uint32_t mip )
    {
        if ( !m_UpsamplePipeline )
            return Common::MakeError( "BloomRenderer: the upsample pipeline is not initialised" );
        if ( mip == 0 )
            return Common::MakeError( "BloomRenderer: an upsample reads mip >= 1" );
        const uint32_t bw = chainDesc.Size.Width;
        const uint32_t bh = chainDesc.Size.Height;

        const ShaderPush::BloomUpsample push{ glm::vec2( 1.0f / static_cast<float>( MipSize( bw, mip ) ),
                                            1.0f / static_cast<float>( MipSize( bh, mip ) ) ),
                                 kFilterRadius };

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( push );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_UpsamplePipeline,
                                                        GroupCount( MipSize( bw, mip - 1 ) ),
                                                        GroupCount( MipSize( bh, mip - 1 ) ), 1 );
    }

    std::optional<BloomRenderer::ConvolutionPlan> BloomRenderer::PrepareConvolution( const RDG::TextureDesc& chainDesc,
                                                                                     const Texture2D*        kernel,
                                                                                     const std::string&      kernelName )
    {
        const auto report = [this]( std::string line )
        {
            if ( line != m_LoggedOutcome )
            {
                LOG_INFO( "{}", line );
                m_LoggedOutcome = std::move( line );
            }
        };
        if ( m_Method != Common::Scalability::BloomMethod::Convolution )
        {
            report( "[Bloom] method=Gaussian" );
            return std::nullopt;
        }
        if ( !m_ConvolutionReady )
        {
            report( "[Bloom] method=Gaussian (tier Convolution: the convolution pipelines did not build)" );
            return std::nullopt;
        }
        if ( static_cast<uint64_t>( m_Kernel ) == 0 )
        {
            report( "[Bloom] method=Gaussian (tier Convolution: the view's post-process names no convolution kernel)" );
            return std::nullopt;
        }
        if ( kernel == nullptr || !kernel->GetImageHandle().IsValid() )
        {
            report( std::format( "[Bloom] method=Gaussian (tier Convolution: kernel {} is not loaded yet)", kernelName ) );
            return std::nullopt;
        }

        // The FFT size: the half-resolution input plus its zero border, rounded up to a power of two, capped; when
        // the cap bites, the content shrinks to fit (UE does the same through its buffer scale).
        const float bw   = static_cast<float>( chainDesc.Size.Width );
        const float bh   = static_cast<float>( chainDesc.Size.Height );
        const float pad  = std::ceil( kBufferScale * std::max( bw, bh ) );
        const auto  size = [&]( float content )
        { return std::min( kMaxFFTLength, std::bit_ceil( static_cast<uint32_t>( content + pad ) ) ); };
        ConvolutionPlan plan;
        plan.Width      = size( bw );
        plan.Height     = size( bh );
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
                 .Format     = Core::Formats::ImageFormat::RGBA32F,
                 .Mips       = 1u,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_KernelSpectrum = Image2D::Create( spec );
            m_BuiltKernel    = nullptr; // a new image holds nothing yet
        }
        if ( !m_KernelSpectrum )
        {
            report( "[Bloom] method=Gaussian (tier Convolution: the kernel spectrum image was not created)" );
            return std::nullopt;
        }
        // The resident size is part of the key: a streamed-in finer mip changes what the kernel image samples.
        plan.KernelResident = glm::uvec2( kernel->GetWidth(), kernel->GetHeight() );
        plan.KernelImage    = kernel;
        plan.RebuildKernel  = m_BuiltKernel != kernel || m_BuiltScale != m_KernelScale ||
                             m_BuiltContent != plan.Content || m_BuiltResident != plan.KernelResident;
        report( std::format( "[Bloom] method=Convolution kernel={} fft={}x{}", kernelName, plan.Width, plan.Height ) );
        return plan;
    }

    void BloomRenderer::AddConvolutionPasses( RDG::Builder& graph, const ConvolutionPlan& plan,
                                              RDG::TextureRef chain, RDG::TextureRef kernelImage,
                                              RDG::TextureRef kernelSpectrum )
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
        const RDG::TextureRef spectrum = graph.CreateTexture(
             RDG::TextureDesc{ .Size   = { .Width = 2 * plan.Width, .Height = plan.Height },
                               .Format = Core::Formats::ImageFormat::RGBA32F,
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
                 [program, chain, spectrum]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Texture( BloomConvolutionPackShaderParameters::BloomInput, chain,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( 0 ) )
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
            const ShaderPush::BloomConvolutionUnpack push{
                 plan.Content, static_cast<int>( plan.Width ),
                 1.0f / ( static_cast<float>( plan.Width ) * static_cast<float>( plan.Height ) ) };
            graph.AddPass(
                 "PostFX: BloomConvolutionUnpack", RDG::PassFlags::Compute,
                 [program, chain, spectrum]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Pipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *program->Pipeline ) )
                          .Storage( BloomConvolutionUnpackShaderParameters::Spectrum, spectrum,
                                    RDG::Access::StorageRead, 0 )
                          .Storage( BloomConvolutionUnpackShaderParameters::BloomOutput, chain,
                                    RDG::Access::StorageWrite, 0 )
                          .PushConstants<ShaderPush::BloomConvolutionUnpack>();
                 },
                 [this, program, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const auto desc = GetChainDesc();
                     if ( !desc )
                         return Common::MakeError( "BloomRenderer: the scene framebuffer is gone" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( push );
                     return Renderer::GetInstance().DispatchCompute( bindings, *program->Pipeline,
                                                                     GroupCount( desc->Size.Width ),
                                                                     GroupCount( desc->Size.Height ), 1 );
                 } );
        }
    }
} // namespace Desert::Graphic::System
