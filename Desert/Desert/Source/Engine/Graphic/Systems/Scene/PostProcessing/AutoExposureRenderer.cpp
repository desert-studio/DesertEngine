#include "AutoExposureRenderer.hpp"

#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <cstdint>

#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGroupSize = 16; // matches AEHistogram local_size

        // The meter's window is the frame's (SetParams -> MakeAutoExposureWindow, AutoExposureRules.hpp): UE's
        // histogram range and the volume's percentiles. The tests pin the range against the sky's own clamp.

        inline uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }
    } // namespace

    Common::BoolResultStr AutoExposureRenderer::Initialize()
    {
        if ( !CreateResources() )
            return Common::MakeError( "AutoExposureRenderer: failed to create compute resources" );
        if ( const auto valid = m_ClearLayout.Validate( m_ClearPipeline->GetSpecification().Shader, AutoExposureClearShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_HistogramLayout.Validate( m_HistogramPipeline->GetSpecification().Shader, AutoExposureHistogramShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_AverageLayout.Validate( m_AveragePipeline->GetSpecification().Shader, AutoExposureAverageShaderParameters::kAll );
             !valid )
            return valid;
        return BOOLSUCCESS;
    }

    bool AutoExposureRenderer::CreateResources()
    {
        const auto makeLum = [&]( const std::string& tag )
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = tag,
                 .Width      = 1,
                 .Height     = 1,
                 .Format     = Core::Formats::ImageFormat::RGBA32F,
                 .Mips       = 1u,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image2D::Create( spec );
        };
        m_LumImage[0] = makeLum( "AEAdaptedLum0" );
        m_LumImage[1] = makeLum( "AEAdaptedLum1" );
        m_Readback    = ShaderResources::StorageBuffer::Create( "AEReadback", 8 * sizeof( float ), 4 );

        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        const auto make          = [&]( const std::string& name ) -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaderService->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "AutoExposureRenderer: missing compute shader '{}'", name );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !built )
            {
                LOG_ERROR( "AutoExposureRenderer: {}", built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        };
        m_ClearPipeline     = make( "AEHistogramClear" );
        m_HistogramPipeline = make( "AEHistogram" );
        m_AveragePipeline   = make( "AEAverage" );

        return m_LumImage[0] && m_LumImage[1] && m_Readback && m_ClearPipeline && m_HistogramPipeline &&
               m_AveragePipeline;
    }

    bool AutoExposureRenderer::Prepare()
    {
        if ( !GetSceneColorImage() || !m_ClearPipeline || !m_HistogramPipeline || !m_AveragePipeline )
            return false;
        m_ReadIndex = 1 - m_ReadIndex; // this frame writes the other image and adapts from the last one
        return true;
    }

    void AutoExposureRenderer::DeclareClearBindings( RDG::PassBuilder& pass, RDG::BufferRef histogram ) const
    {
        if ( !m_ClearPipeline )
            return;
        pass.Bindings( m_ClearLayout.Get( m_ClearPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_ClearPipeline ) )
             .Storage( AutoExposureClearShaderParameters::Histogram, histogram, RDG::Access::StorageWrite );
    }

    Common::BoolResultStr AutoExposureRenderer::RecordClear( const RDG::PassContext& context )
    {
        if ( !m_ClearPipeline )
            return Common::MakeError( "PostFX: AutoExposureClear: the clear pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        // One group of kBins threads (AEHistogramClear LocalSize(256, 1, 1)).
        return Renderer::GetInstance().DispatchCompute( bindings, *m_ClearPipeline, 1, 1, 1 );
    }

    void AutoExposureRenderer::DeclareHistogramBindings( RDG::PassBuilder& pass, RDG::TextureRef scene,
                                                         RDG::BufferRef histogram ) const
    {
        if ( !m_HistogramPipeline )
            return;
        // The shader reads texels with texelFetch: no filtering or addressing applies, PointClamp states that.
        pass.Bindings( m_HistogramLayout.Get( m_HistogramPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_HistogramPipeline ) )
             .Texture( AutoExposureHistogramShaderParameters::SceneColorTexture, scene, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Sampler( AutoExposureHistogramShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Storage( AutoExposureHistogramShaderParameters::Histogram, histogram, RDG::Access::StorageWrite )
             .PushConstants<ShaderPush::AEHistogram>();
    }

    Common::BoolResultStr AutoExposureRenderer::RecordHistogram( const RDG::PassContext& context, uint32_t width,
                                                                 uint32_t height )
    {
        if ( !m_HistogramPipeline )
            return Common::MakeError( "PostFX: AutoExposureHistogram: the histogram pipeline is not initialised" );
        const ShaderPush::AEHistogram hp{ m_Window.MinLogLum, 1.0f / m_Window.Range(), 1.0f / m_PreExposure };
        RDG::PassBindings   bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( hp );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_HistogramPipeline, GroupCount( width ),
                                                        GroupCount( height ), 1 );
    }

    void AutoExposureRenderer::DeclareAverageBindings( RDG::PassBuilder& pass, RDG::BufferRef histogram,
                                                       RDG::TextureRef previous, RDG::TextureRef adapted,
                                                       RDG::BufferRef readback ) const
    {
        if ( !m_AveragePipeline )
            return;
        // PreviousLuminanceTexture is 1x1 and sampled at its centre: PointClamp returns exactly the stored luminance.
        pass.Bindings( m_AverageLayout.Get( m_AveragePipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_AveragePipeline ) )
             .Storage( AutoExposureAverageShaderParameters::Histogram, histogram, RDG::Access::StorageRead )
             .Texture( AutoExposureAverageShaderParameters::PreviousLuminanceTexture, previous, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
             .Sampler( AutoExposureAverageShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Storage( AutoExposureAverageShaderParameters::OutLuminanceTexture, adapted, RDG::Access::StorageWrite, 0 )
             .Storage( AutoExposureAverageShaderParameters::Readback, readback, RDG::Access::StorageWrite )
             .PushConstants<ShaderPush::AEAverage>();
    }

    Common::BoolResultStr AutoExposureRenderer::RecordAverage( const RDG::PassContext& context )
    {
        if ( !m_AveragePipeline )
            return Common::MakeError( "PostFX: AutoExposureAverage: the average pipeline is not initialised" );
        // 3) Resolve: percentile-clipped weighted average + temporal adaptation -> newLum (1x1).
        //
        // kSnapAdaptSpeed makes `1 - exp2(-dt * speed)` exactly 1 in float and the linear step longer than any
        // window, and an exponential slope of 1 makes that leg land too, so the shader reaches the measured
        // luminance with no ramp at all. Consumed here, once, so a scene load costs one instant adaptation and
        // every frame after it adapts normally (UE ComputeEyeAdaptation, the slope M matched at 1.5 stops).
        constexpr float kSnapAdaptSpeed = 1.0e6f;

        const bool  snap      = m_SnapNextAdaptation;
        const float speedUp   = snap ? kSnapAdaptSpeed : m_Params.SpeedUp;
        const float speedDown = snap ? kSnapAdaptSpeed : m_Params.SpeedDown;
        const float upM       = snap ? 1.0f : AutoExposureExponentialM( m_Params.SpeedUp );
        const float downM     = snap ? 1.0f : AutoExposureExponentialM( m_Params.SpeedDown );
        // A snap needs a step to act through: a frozen world's step is 0, and `1 - exp(-0 * speed)` is 0 —
        // the new scene would keep the old scene's exposure until time moved.
        const float deltaSeconds = m_SnapNextAdaptation ? 1.0f : m_DeltaSeconds;
        m_SnapNextAdaptation     = false;

        const LocalExposureParams& le = m_LocalExposure;
        ShaderPush::AEAverage      ap;
        ap.DeltaTime                  = deltaSeconds;
        ap.SpeedUp                    = speedUp;
        ap.SpeedDown                  = speedDown;
        ap.ExponentialUpM             = upM;
        ap.ExponentialDownM           = downM;
        ap.TransitionDistance         = kAutoExposureTransitionDistance;
        ap.MinLuma                    = m_Params.MinLuma;
        ap.MaxLuma                    = m_Params.MaxLuma;
        ap.MinLogLuminance            = m_Window.MinLogLum;
        ap.LogLuminanceRange          = m_Window.Range();
        ap.LowPercent                 = m_Window.LowPercent;
        ap.HighPercent                = m_Window.HighPercent;
        ap.LocalExposureContrast      = glm::vec4( le.HighlightContrast, le.ShadowContrast, 0.0f,
                                              IsLocalExposureIdentity( le ) ? 0.0f : 1.0f );
        ap.LocalExposureThreshold     = glm::vec4( le.HighlightThreshold, le.ShadowThreshold,
                                                   le.HighlightThresholdStrength, le.ShadowThresholdStrength );
        ap.LocalExposureLogMiddleGrey = m_LocalExposureLogGrey;
        ap.ExposureKey                = m_LocalExposureKey;
        ap.CompensationBias           = m_CompensationBias;
        ap.CompensationKeys           = static_cast<float>( m_CurveKeys );
        ap.CurveEV100                 = m_CurveEV100;
        ap.CurveBias                  = m_CurveBias;
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( ap );
        // One group of kBins threads, one per bin (AEAverage LocalSize(256, 1, 1): a parallel scan + reduce).
        return Renderer::GetInstance().DispatchCompute( bindings, *m_AveragePipeline, 1, 1, 1 );
    }

    RDG::BufferRef AutoExposureRenderer::ImportReadback( RDG::Builder& graph )
    {
        if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( m_Readback, m_ReadbackImport );
             !imported )
        {
            LOG_ERROR( "PostFX: AutoExposure: the readback buffer is not in the frame graph: {}",
                       imported.GetError() );
            return {};
        }
        return graph.RegisterExternal( m_ReadbackImport, "AutoExposure.Readback" );
    }

    float AutoExposureRenderer::ReadBackAdaptedLuminance()
    {
        if ( !m_Readback )
            return 0.0f;
        std::array<float, 8> value{};
        if ( const Common::BoolResultStr read = m_Readback->ReadBack( value.data(), sizeof( value ) );
             !read.IsSuccess() )
        {
            LOG_ERROR( "PostFX: AutoExposure: the adapted luminance was not read back: {}", read.GetError() );
            return 0.0f;
        }
        // Per frame, so trace: z = the measured luminance before clamp/adaptation (-1 = empty histogram), w =
        // samples.
        LOG_TRACE(
             "[PreExposure] read-back: adapted {} (written {}), measured {}, histogram samples {}, clamp [{}, {}]",
             value[0], value[1], value[2], value[3], m_Params.MinLuma, m_Params.MaxLuma );
        m_LastMeasuredLuminance = value[1] == 1.0f && value[2] > 0.0f && value[2] < 3.0e38f ? value[2] : 0.0f;
        // [4] = AverageLocalExposure (UE readback [1].X), 1 at identity.
        m_LastAverageLocalExposure = value[1] == 1.0f && value[4] > 0.0f && value[4] < 3.0e38f ? value[4] : 1.0f;
        // y == 1: the Average pass wrote this copy; otherwise it is the buffer's first frames.
        return value[1] == 1.0f && value[0] > 0.0f && value[0] < 3.0e38f ? value[0] : 0.0f;
    }
} // namespace Desert::Graphic::System
