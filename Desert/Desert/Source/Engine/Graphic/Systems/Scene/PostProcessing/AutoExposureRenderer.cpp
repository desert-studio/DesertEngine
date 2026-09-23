#include "AutoExposureRenderer.hpp"

#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kBins      = 256;
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

        constexpr float kDeltaTime = 0.016f;

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
        m_Histogram = ShaderResources::StorageBuffer::Create( "AEHistogram", kBins * sizeof( uint32_t ), 1 );

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

        return m_Histogram && m_LumImage[0] && m_LumImage[1] && m_ClearPipeline && m_HistogramPipeline &&
               m_AveragePipeline;
    }

    void AutoExposureRenderer::Execute()
    {
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !m_Histogram || !m_ClearPipeline || !m_HistogramPipeline || !m_AveragePipeline )
            return;

        Image2D* sceneColor = scene->GetColorAttachmentImage().get();
        if ( !sceneColor )
            return;

        const uint32_t sceneW = scene->GetFramebufferWidth();
        const uint32_t sceneH = scene->GetFramebufferHeight();

        const int prev  = m_ReadIndex;     // last frame's adapted luminance (sampled)
        const int write = 1 - m_ReadIndex; // new value written here
        Image2D*  prevLum = m_LumImage[prev].get();
        Image2D*  newLum  = m_LumImage[write].get();

        auto& renderer = Renderer::GetInstance();

        // newLum -> GENERAL + a graphics->compute barrier so the scene color (and prevLum) are visible.
        renderer.ComputeImageBeginWrite( newLum );

        // 1) Zero the histogram.
        m_ClearPipeline->SetStorageBuffer( 1, m_Histogram.get() );
        renderer.DispatchComputeInFrame( m_ClearPipeline.get(), 1, 1, 1 );

        // 2) Build the histogram from the full scene (one thread per texel, atomic adds).
        HistogramPush hp{ kWindow.MinLogLum, 1.0f / kWindow.Range() };
        m_HistogramPipeline->SetInput( 0, sceneColor );
        m_HistogramPipeline->SetStorageBuffer( 1, m_Histogram.get() );
        m_HistogramPipeline->SetPushConstants( &hp, sizeof( hp ) );
        renderer.DispatchComputeInFrame( m_HistogramPipeline.get(), GroupCount( sceneW ),
                                         GroupCount( sceneH ), 1 );

        // 3) Resolve: percentile-clipped weighted average + temporal adaptation -> newLum (1x1).
        //
        // kSnapAdaptSpeed makes `1 - exp(-dt * speed)` exactly 1 in float for any dt this engine produces,
        // so the shader's mix lands on the measured luminance with no ramp at all. Consumed here, once, so
        // a scene load costs one instant adaptation and every frame after it adapts normally.
        constexpr float kSnapAdaptSpeed = 1.0e6f;

        const float adaptSpeed = m_SnapNextAdaptation ? kSnapAdaptSpeed : m_AdaptSpeed;
        m_SnapNextAdaptation   = false;

        AveragePush ap{ kDeltaTime,        adaptSpeed,      m_MinLuma,          m_MaxLuma,
                        kWindow.MinLogLum, kWindow.Range(), kWindow.LowPercent, kWindow.HighPercent };
        m_AveragePipeline->SetStorageBuffer( 0, m_Histogram.get() );
        m_AveragePipeline->SetInput( 1, prevLum );
        m_AveragePipeline->SetOutput( 2, newLum, 0 );
        m_AveragePipeline->SetPushConstants( &ap, sizeof( ap ) );
        renderer.DispatchComputeInFrame( m_AveragePipeline.get(), 1, 1, 1 );

        // newLum -> SHADER_READ_ONLY so tonemap can sample it.
        renderer.ComputeImageEndWrite( newLum );

        m_ReadIndex = write; // the freshly written buffer is now the latest
    }
} // namespace Desert::Graphic::System
