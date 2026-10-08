#include "AutoExposureRenderer.hpp"

#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGroupSize = 16; // matches AEHistogram local_size

        // What the meter can SEE, in log2 luminance. The ceiling is not a taste value: the procedural
        // sky writes the sun disc at up to its own kSkyLuminanceClamp of 1000 (ProceduralSky.shader), and
        // a meter whose top bin means 4 cannot tell that disc from a merely bright sky — it was eight
        // stops blind at the top, and every pixel above 4 metered as though it were 4. AutoExposureRules
        // asserts this covers the sky's clamp, so the two cannot drift apart again.
        //
        // Widening the window costs bin resolution: 20 stops over 256 bins is 0.078 EV per bin against
        // the old 0.047. The metered average is reconstructed from each bin's own log-luminance, not from
        // the window, so ordinary scenes move by at most that quantisation — measured at under 0.02 EV.
        constexpr float kMinLogLum = -10.0f;
        constexpr float kMaxLogLum = 10.0f;

        // Outlier rejection, UNCHANGED. It was the obvious suspect for "the sun does not move the
        // exposure" and it is not the cause, so it keeps its values — see AutoExposureRules.hpp for the
        // measurement. The short version: the sun disc subtends half a degree, which at this field of
        // view is about forty PIXELS, four thousandths of one percent of the frame. A pixel-count
        // weighted average of log luminance shifts by (that fraction) x (its excess in stops), i.e. by
        // ten-thousandths of a stop. Widening this tail to 99.5% was tried and measured: the metered
        // background moved by one 8-bit level. The tail is not what is stopping the response; the
        // disc's solid angle is, and no percentile can change that.
        constexpr float kLowPercent  = 0.0f;
        constexpr float kHighPercent = 0.95f;

        // The one description of the meter's window, shared with the tests that pin it against the sky's
        // own luminance clamp. The dispatch below reads its fields rather than the constants above, so
        // there is no second copy to drift.
        constexpr AutoExposureWindow kWindow{ kMinLogLum, kMaxLogLum, kLowPercent, kHighPercent };

        struct HistogramPush
        {
            float MinLogLum;
            float InvLogLumRange;
        };

        struct AveragePush
        {
            float Dt;
            float AdaptSpeed;
            float MinLuma;
            float MaxLuma;
            float MinLogLum;
            float LogLumRange;
            float LowPct;
            float HighPct;
        };

        inline uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }
    } // namespace

    Common::BoolResultStr AutoExposureRenderer::Initialize()
    {
        if ( !CreateResources() )
            return Common::MakeError( "AutoExposureRenderer: failed to create compute resources" );
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

        return m_LumImage[0] && m_LumImage[1] && m_ClearPipeline && m_HistogramPipeline && m_AveragePipeline;
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
             .Storage( "Histogram", histogram, RDG::Access::StorageWrite );
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
             .Sampled( "u_Scene", scene, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() )
             .Storage( "Histogram", histogram, RDG::Access::StorageWrite )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( HistogramPush ) ) );
    }

    Common::BoolResultStr AutoExposureRenderer::RecordHistogram( const RDG::PassContext& context, uint32_t width,
                                                                 uint32_t height )
    {
        if ( !m_HistogramPipeline )
            return Common::MakeError( "PostFX: AutoExposureHistogram: the histogram pipeline is not initialised" );
        const HistogramPush hp{ kWindow.MinLogLum, 1.0f / kWindow.Range() };
        RDG::PassBindings   bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &hp, sizeof( hp ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_HistogramPipeline, GroupCount( width ),
                                                        GroupCount( height ), 1 );
    }

    void AutoExposureRenderer::DeclareAverageBindings( RDG::PassBuilder& pass, RDG::BufferRef histogram,
                                                       RDG::TextureRef previous, RDG::TextureRef adapted ) const
    {
        if ( !m_AveragePipeline )
            return;
        // u_PrevLum is 1x1 and sampled at its centre: PointClamp returns exactly the stored luminance.
        pass.Bindings( m_AverageLayout.Get( m_AveragePipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_AveragePipeline ) )
             .Storage( "Histogram", histogram, RDG::Access::StorageRead )
             .Sampled( "u_PrevLum", previous, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() )
             .Storage( "u_OutLum", adapted, RDG::Access::StorageWrite, 0 )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( AveragePush ) ) );
    }

    Common::BoolResultStr AutoExposureRenderer::RecordAverage( const RDG::PassContext& context )
    {
        if ( !m_AveragePipeline )
            return Common::MakeError( "PostFX: AutoExposureAverage: the average pipeline is not initialised" );
        // 3) Resolve: percentile-clipped weighted average + temporal adaptation -> newLum (1x1).
        //
        // kSnapAdaptSpeed makes `1 - exp(-dt * speed)` exactly 1 in float for any dt this engine produces,
        // so the shader's mix lands on the measured luminance with no ramp at all. Consumed here, once, so
        // a scene load costs one instant adaptation and every frame after it adapts normally.
        constexpr float kSnapAdaptSpeed = 1.0e6f;

        const float adaptSpeed = m_SnapNextAdaptation ? kSnapAdaptSpeed : m_AdaptSpeed;
        // A snap needs a step to act through: a frozen world's step is 0, and `1 - exp(-0 * speed)` is 0 —
        // the new scene would keep the old scene's exposure until time moved.
        const float deltaSeconds = m_SnapNextAdaptation ? 1.0f : m_DeltaSeconds;
        m_SnapNextAdaptation     = false;

        const AveragePush ap{ deltaSeconds,      adaptSpeed,      m_MinLuma,          m_MaxLuma,
                              kWindow.MinLogLum, kWindow.Range(), kWindow.LowPercent, kWindow.HighPercent };
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &ap, sizeof( ap ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_AveragePipeline, 1, 1, 1 );
    }
} // namespace Desert::Graphic::System
