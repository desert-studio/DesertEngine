#include "AutoExposureRenderer.hpp"

#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGroupSize = 16; // matches AEHistogram local_size

        // What the meter can SEE, in log2 luminance. The ceiling is not a taste value: the procedural
        // sky writes the sun disc at up to kSkyLuminanceCeilingMax (AutoExposureRules.hpp, ~2^15), and
        // a meter whose top bin means 4 cannot tell that disc from a merely bright sky — it was eight
        // stops blind at the top, and every pixel above 4 metered as though it were 4. AutoExposureRules
        // asserts this covers the sky's clamp, so the two cannot drift apart again.
        //
        // Widening the window costs bin resolution: 20 stops over 256 bins is 0.078 EV per bin against
        // the old 0.047. The metered average is reconstructed from each bin's own log-luminance, not from
        // the window, so ordinary scenes move by at most that quantisation — measured at under 0.02 EV.
        //
        // PHYSICAL UNITS (LIGHT-UNITS). With lights in lux / candelas a sunlit scene meters ~2^12.5 cd/m^2 and a
        // street at night ~2^-2: the window is UE's histogram range (Histogram Log Min/Max -10 .. 20 EV100), i.e.
        // log2 luminance -13 .. 17 (EV100ToLuminance), 30 stops = 0.118 EV per bin; an ordinary scene's
        // average moves by at most half a bin against the former 20-stop window.
        constexpr float kMinLogLum = -13.0f;
        constexpr float kMaxLogLum = 17.0f;

        // The outlier tails are the view's (AutoExposureLowPercent / HighPercent, UE's 10 / 90 by default) and are
        // set per frame through ComputeEyeAdaptationParameters; the window here only fixes what the meter can see.
        // The tails used to be 0 / 95 here: with no dark tail every black pixel of a frame (an unlit interior, the
        // void under a floor) pulled the metered average down a bin at -10 EV, i.e. brightened the whole frame.
        constexpr AutoExposureWindow kWindow{ kMinLogLum, kMaxLogLum, 0.0f, 1.0f };

        struct HistogramPush
        {
            float MinLogLum;
            float InvLogLumRange;
            float WeightScale;          // AutoExposureWeightScale of this frame's texel count
            float BlackBucketInfluence; // kEyeAdaptationBlackHistogramBucketInfluence
        };

        // AEAverage's push block, in its declaration order: UE's EyeAdaptation_* parameters it reads.
        struct AveragePush
        {
            float Dt;
            float SpeedUp;
            float SpeedDown;
            float ExponentialUpM;
            float ExponentialDownM;
            float StartDistance;
            float MinAverageLuminance;
            float MaxAverageLuminance;
            float MinLogLum;
            float LogLumRange;
            float LowPct;
            float HighPct;
            float ForceTarget; // 1 = land on the target this dispatch (scene replaced, camera cut)
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

        return m_ClearPipeline && m_HistogramPipeline && m_AveragePipeline;
    }

    bool AutoExposureRenderer::Prepare() const
    {
        return GetSceneColorImage() && m_ClearPipeline && m_HistogramPipeline && m_AveragePipeline;
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
                                                         RDG::BufferRef  histogram,
                                                         RDG::TextureRef meterMask ) const
    {
        if ( !m_HistogramPipeline )
            return;
        // The shader reads scene texels with texelFetch: no filtering or addressing applies, PointClamp states
        // that. The mask is UE's: bilinear, clamped (PostProcessEyeAdaptation.cpp:805).
        pass.Bindings( m_HistogramLayout.Get( m_HistogramPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_HistogramPipeline ) )
             .Sampled( "u_Scene", scene, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() )
             .Sampled( "u_MeterMask", meterMask, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Storage( "Histogram", histogram, RDG::Access::StorageWrite )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( HistogramPush ) ) );
    }

    std::shared_ptr<Image> AutoExposureRenderer::MeterMaskImage() const
    {
        const Assets::AssetHandle& mask = m_Settings.AutoExposureMeterMask;
        if ( mask.IsNull() )
            return nullptr;
        auto* const textures = Runtime::ResourceRegistry::GetTextureService();
        auto* const images   = Runtime::ResourceRegistry::GetImageService();
        if ( textures == nullptr || images == nullptr )
            return nullptr;
        const Assets::AssetRef<Texture2D> texture = textures->Require( mask );
        if ( texture.IsPending() )
            return nullptr;
        if ( !texture.IsValid() )
        {
            if ( static_cast<uint64_t>( m_ReportedMissingMeterMask ) != static_cast<uint64_t>( mask ) )
            {
                LOG_ERROR( "[AutoExposure] the metering mask {} is not a texture of this project; the view meters "
                           "every texel alike until it is",
                           mask.ToString() );
                m_ReportedMissingMeterMask = mask;
            }
            return nullptr;
        }
        return images->Share( texture.Get()->GetImageHandle() );
    }

    Common::BoolResultStr AutoExposureRenderer::RecordHistogram( const RDG::PassContext& context, uint32_t width,
                                                                 uint32_t height )
    {
        if ( !m_HistogramPipeline )
            return Common::MakeError( "PostFX: AutoExposureHistogram: the histogram pipeline is not initialised" );
        const HistogramPush hp{ .MinLogLum            = kWindow.MinLogLum,
                                .InvLogLumRange       = 1.0f / kWindow.Range(),
                                .WeightScale          = static_cast<float>( AutoExposureWeightScale(
                                     static_cast<uint64_t>( width ) * static_cast<uint64_t>( height ) ) ),
                                .BlackBucketInfluence = kEyeAdaptationBlackHistogramBucketInfluence };
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
        // 3) Resolve: percentile-clipped log average, clamped into the EV100 range, adapted in EV toward it at the
        //    up / down speed (UE's ComputeEyeAdaptation) -> newLum (1x1).
        const EyeAdaptationParameters eye =
             ComputeEyeAdaptationParameters( m_Settings.AutoExposureMinEV100, m_Settings.AutoExposureMaxEV100,
                                             m_Settings.AutoExposureLowPercent, m_Settings.AutoExposureHighPercent,
                                             m_Settings.AutoExposureSpeedUp, m_Settings.AutoExposureSpeedDown );

        const float forceTarget = ( m_SnapNextAdaptation || eye.ForceTarget ) ? 1.0f : 0.0f;
        m_SnapNextAdaptation    = false;

        const AveragePush ap{ .Dt                  = m_DeltaSeconds,
                              .SpeedUp             = eye.SpeedUp,
                              .SpeedDown           = eye.SpeedDown,
                              .ExponentialUpM      = eye.ExponentialUpM,
                              .ExponentialDownM    = eye.ExponentialDownM,
                              .StartDistance       = eye.StartDistance,
                              .MinAverageLuminance = eye.MinAverageLuminance,
                              .MaxAverageLuminance = eye.MaxAverageLuminance,
                              .MinLogLum           = kWindow.MinLogLum,
                              .LogLumRange         = kWindow.Range(),
                              .LowPct              = eye.LowPercent,
                              .HighPct             = eye.HighPercent,
                              .ForceTarget         = forceTarget };
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &ap, sizeof( ap ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_AveragePipeline, 1, 1, 1 );
    }
} // namespace Desert::Graphic::System
