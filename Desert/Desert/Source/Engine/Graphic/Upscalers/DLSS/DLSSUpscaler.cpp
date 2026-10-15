#include <Engine/Graphic/Upscalers/DLSS/DLSSUpscaler.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Graphic
{
    namespace
    {
        constexpr const char* kPassName   = "DLSS";
        constexpr const char* kOutputName = "DLSS.Output";
        // What the post chain reads after the temporal resolve (TemporalAA's output is the same).
        constexpr auto kOutputFormat = Core::Formats::ImageFormat::RGBA16F;

        // The quality modes below 100 %, best first.
        constexpr std::array kUpscaleModes{ Streamline::DlssMode::UltraQuality, Streamline::DlssMode::Quality,
                                            Streamline::DlssMode::Balanced, Streamline::DlssMode::Performance,
                                            Streamline::DlssMode::UltraPerformance };

        Common::ResultStr<Streamline::DlssImage> ImageOf( const RDG::PassContext& context,
                                                          const RDG::TextureRef ref, const RDG::Access access )
        {
            const auto binding = context.GetTexture( ref, access );
            if ( !binding )
                return Common::MakeError<Streamline::DlssImage>( binding.GetError() );
            const auto texture = API::Vulkan::VulkanRdgBackend::TextureOf( binding.GetValue() );
            if ( !texture )
                return Common::MakeError<Streamline::DlssImage>( texture.GetError() );
            API::Vulkan::VulkanRdgTexture* image = texture.GetValue();
            const auto                     view  = image->GetView();
            if ( !view )
                return Common::MakeError<Streamline::DlssImage>( view.GetError() );
            return Common::MakeSuccess( Streamline::DlssImage{
                 .Image  = image->GetImage(),
                 .View   = view.GetValue(),
                 .Format = image->GetFormat(),
                 .Layout = API::Vulkan::RdgVulkanLayout( RDG::GetAccessInfo( access ).State.Layout ),
                 .Usage  = API::Vulkan::RdgImageUsage( image->GetAccessMask() ),
                 .Width  = image->GetDesc().Size.Width,
                 .Height = image->GetDesc().Size.Height } );
        }

        bool Holds( const Streamline::DlssRenderRange& range, const ViewExtent render )
        {
            return render.Width >= range.MinWidth && render.Width <= range.MaxWidth &&
                   render.Height >= range.MinHeight && render.Height <= range.MaxHeight;
        }

        // The Scalability row that holds @p mode's preset.
        Common::Scalability::Parameter PresetParameter( const Streamline::DlssMode mode )
        {
            using Common::Scalability::Parameter;
            switch ( mode )
            {
                case Streamline::DlssMode::UltraPerformance:
                    return Parameter::DlssPresetUltraPerformance;
                case Streamline::DlssMode::Performance:
                    return Parameter::DlssPresetPerformance;
                case Streamline::DlssMode::Balanced:
                    return Parameter::DlssPresetBalanced;
                case Streamline::DlssMode::Quality:
                    return Parameter::DlssPresetQuality;
                case Streamline::DlssMode::UltraQuality:
                    return Parameter::DlssPresetUltraQuality;
                case Streamline::DlssMode::DLAA:
                    return Parameter::DlssPresetDLAA;
            }
            return Parameter::DlssPresetDLAA;
        }

        // One view's DLSS feature: its Streamline viewport and the options it was last given. Released with the
        // view's history, after the frames that recorded with it (the frame ring drains RT_DestroyLater).
        class DlssHistory final : public ITemporalUpscalerHistory
        {
        public:
            DlssHistory() : m_Viewport( Streamline::Runtime::Get().AcquireViewport() )
            {
            }

            ~DlssHistory() override
            {
                const auto ctx = EngineContext::GetInstance().GetRendererContext();
                auto*      allocator =
                     ctx ? SP_CAST( API::Vulkan::VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
                const uint32_t viewport = m_Viewport;
                if ( allocator != nullptr )
                    allocator->RT_DestroyLater( [viewport]()
                                                { Streamline::Runtime::Get().ReleaseViewport( viewport ); } );
                else
                    Streamline::Runtime::Get().ReleaseViewport( viewport ); // no renderer: no frame in flight
            }

            DlssHistory( const DlssHistory& )            = delete;
            DlssHistory& operator=( const DlssHistory& ) = delete;

            [[nodiscard]] std::string_view DebugName() const override
            {
                return "DLSS";
            }
            [[nodiscard]] uint64_t HeldBytes() const override
            {
                return m_HeldBytes;
            }

            [[nodiscard]] uint32_t Viewport() const
            {
                return m_Viewport;
            }

            // Re-options the feature when @p options differ from the last ones (first use, a mode, output size or
            // preset change).
            [[nodiscard]] Common::BoolResultStr Apply( const Streamline::DlssOptions& options )
            {
                if ( m_Options && *m_Options == options )
                    return Common::MakeSuccess( true );
                const auto bytes = Streamline::Runtime::Get().SetOptions( m_Viewport, options );
                if ( !bytes )
                {
                    m_Options.reset(); // the feature's options are unknown now: set them again next frame
                    return Common::MakeError( bytes.GetError() );
                }
                m_Options   = options;
                m_HeldBytes = bytes.GetValue();
                return Common::MakeSuccess( true );
            }

        private:
            const uint32_t                         m_Viewport;
            std::optional<Streamline::DlssOptions> m_Options;
            uint64_t                               m_HeldBytes = 0;
        };
    } // namespace

    DLSSUpscaler::DLSSUpscaler()  = default;
    DLSSUpscaler::~DLSSUpscaler() = default;

    std::string_view DLSSUpscaler::DebugName() const
    {
        return "DLSS";
    }

    Common::ResultStr<Streamline::DlssRenderRange> DLSSUpscaler::RangeOf( const Streamline::DlssMode mode,
                                                                          const ViewExtent           output ) const
    {
        const std::scoped_lock lock( m_RangeMutex );
        const auto             key = std::make_tuple( static_cast<uint8_t>( mode ), output.Width, output.Height );
        if ( const auto found = m_Ranges.find( key ); found != m_Ranges.end() )
            return found->second;
        auto range = Streamline::Runtime::Get().RenderRange( mode, output.Width, output.Height );
        m_Ranges.emplace( key, range );
        return range;
    }

    Common::ResultStr<Streamline::DlssMode> DLSSUpscaler::ModeFor( const ResolutionSplit& split ) const
    {
        const Streamline::Runtime& runtime = Streamline::Runtime::Get();
        if ( !runtime.DlssReady() )
            return Common::MakeFormattedError<Streamline::DlssMode>( "DLSS is not available: {}",
                                                                     runtime.DlssRefusal() );
        switch ( split.Mode )
        {
            case Common::Scalability::ScaleMode::Supersample:
                return Common::MakeFormattedError<Streamline::DlssMode>(
                     "DLSS does not take a supersampled input ({} %)", split.RenderScalePercent );
            case Common::Scalability::ScaleMode::Native:
                return Common::MakeSuccess( Streamline::DlssMode::DLAA );
            case Common::Scalability::ScaleMode::Upscale:
                break;
        }

        std::optional<Streamline::DlssMode> best;
        uint32_t                            bestDistance = 0;
        std::string                         tried;
        for ( const Streamline::DlssMode mode : kUpscaleModes )
        {
            const auto range = RangeOf( mode, split.Output );
            if ( !range )
            {
                tried += std::format( " {}: {};", Streamline::DlssModeName( mode ), range.GetError() );
                continue;
            }
            const Streamline::DlssRenderRange& r = range.GetValue();
            tried += std::format( " {} {}x{}..{}x{};", Streamline::DlssModeName( mode ), r.MinWidth, r.MinHeight,
                                  r.MaxWidth, r.MaxHeight );
            if ( !Holds( r, split.Render ) )
                continue;
            const auto distance = static_cast<uint32_t>(
                 std::abs( static_cast<int64_t>( r.OptimalWidth ) - static_cast<int64_t>( split.Render.Width ) ) );
            if ( !best || distance < bestDistance )
            {
                best         = mode;
                bestDistance = distance;
            }
        }
        if ( !best )
            return Common::MakeFormattedError<Streamline::DlssMode>(
                 "no DLSS mode renders {}x{} for an output of {}x{} ({} %):{}", split.Render.Width,
                 split.Render.Height, split.Output.Width, split.Output.Height, split.RenderScalePercent, tried );
        return Common::MakeSuccess( *best );
    }

    bool DLSSUpscaler::Supports( const ResolutionSplit& split ) const
    {
        return ModeFor( split ).IsSuccess();
    }

    std::vector<HistoryTextureDesc>
    DLSSUpscaler::HistoryDescs( const ResolutionSplit& /*split*/,
                                const Common::Scalability::ResolvedQuality& /*quality*/ ) const
    {
        return {}; // the DLSS feature keeps its own history
    }

    float DLSSUpscaler::MaterialMipBias( const ResolutionSplit& split ) const
    {
        if ( split.Render.Width == 0 || split.Output.Width == 0 )
            return 0.0f;
        return std::log2( static_cast<float>( split.Render.Width ) / static_cast<float>( split.Output.Width ) ) -
               1.0f;
    }

    Common::ResultStr<TemporalUpscalerOutputs>
    DLSSUpscaler::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                             const TemporalUpscalerInputs& inputs ) const
    {
        const auto missing = [&]( const char* what ) -> Common::ResultStr<TemporalUpscalerOutputs>
        {
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}: the input {} is not a texture of this graph", DebugName(), what );
        };
        if ( !inputs.SceneColor.IsValid() )
            return missing( "SceneColor" );
        if ( !inputs.SceneDepth.IsValid() )
            return missing( "SceneDepth" );
        if ( !inputs.Velocity.IsValid() )
            return missing( "Velocity" );
        if ( !inputs.History.empty() )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}: {} histories given, HistoryDescs declares none", DebugName(), inputs.History.size() );
        if ( !inputs.Exposure.IsValid() )
            return missing( "Exposure" );
        if ( inputs.Quality == nullptr )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}: no resolved quality was given (the preset of each DLSS mode is a Scalability row)",
                 DebugName() );
        const auto mode = ModeFor( frame.Split );
        if ( !mode )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>( "{}: {}", DebugName(), mode.GetError() );

        // The view's feature: its own from last frame, or a new one (first DLSS frame, after a reset or another
        // method's history).
        std::shared_ptr<ITemporalUpscalerHistory> history = inputs.PrevHistory;
        if ( dynamic_cast<DlssHistory*>( history.get() ) == nullptr )
            history = std::make_shared<DlssHistory>();
        auto&                         feature = static_cast<DlssHistory&>( *history );
        const Streamline::DlssOptions options{
             .Mode         = mode.GetValue(),
             .OutputWidth  = frame.Split.Output.Width,
             .OutputHeight = frame.Split.Output.Height,
             .Preset = inputs.Quality->As<Common::Scalability::DlssPreset>( PresetParameter( mode.GetValue() ) ) };
        if ( const auto applied = feature.Apply( options ); !applied )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>( "{}: {}", DebugName(),
                                                                        applied.GetError() );

        const Common::ResultStr<RDG::TextureRef> exposure = m_Exposure.AddPass( graph, inputs.Exposure );
        if ( !exposure )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>( "{}: {}", DebugName(),
                                                                        exposure.GetError() );

        RDG::TextureDesc outputDesc;
        outputDesc.Size              = RDG::Extent3D{ frame.Split.Output.Width, frame.Split.Output.Height, 1 };
        outputDesc.Format            = kOutputFormat;
        const RDG::TextureRef output = graph.CreateTexture( outputDesc, kOutputName );

        Streamline::DlssEvaluation evaluation;
        evaluation.Viewport       = feature.Viewport();
        evaluation.Mode           = mode.GetValue();
        evaluation.ViewToClip     = frame.Projection;
        evaluation.ClipToView     = frame.InvProjection;
        evaluation.ClipToPrevClip = frame.PrevViewProjection * frame.InvViewProjection;
        evaluation.PrevClipToClip = frame.ViewProjection * frame.PrevInvViewProjection;
        // The engine moves the geometry by +JitterPixels in render pixels with NDC y up (ApplyJitter,
        // negative-height viewport); Streamline takes the offset in pixels with y down, as UE hands it
        // TemporalJitterPixels.
        evaluation.JitterPixels = glm::vec2( frame.JitterPixels.x, -frame.JitterPixels.y );
        // Velocity is current minus previous NDC.xy (Velocity.hpp); Streamline wants the UV offset from the
        // current pixel to where it was: prevUV - uv = velocity * (-0.5, 0.5) under the y-up NDC.
        evaluation.MotionVectorScale = glm::vec2( -0.5f, 0.5f );
        evaluation.CameraPosition    = frame.CameraPosition;
        evaluation.CameraRight       = glm::vec3( frame.InvView[0] );
        evaluation.CameraUp          = glm::vec3( frame.InvView[1] );
        evaluation.CameraForward     = -glm::vec3( frame.InvView[2] );
        evaluation.NearPlane         = frame.NearPlane;
        evaluation.FarPlane          = frame.FarPlane;
        evaluation.VerticalFov       = 2.0f * std::atan( 1.0f / std::abs( frame.Projection[1][1] ) );
        evaluation.AspectRatio       = static_cast<float>( frame.Split.Output.Width ) /
                                 static_cast<float>( std::max( frame.Split.Output.Height, 1u ) );
        evaluation.Reset = !frame.HistoryValid();

        const RDG::TextureRef color       = inputs.SceneColor;
        const RDG::TextureRef depth       = inputs.SceneDepth;
        const RDG::TextureRef velocity    = inputs.Velocity;
        const RDG::TextureRef exposureRef = exposure.GetValue();
        graph.AddPass(
             kPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( color, RDG::Access::SampledCompute );
                 pass.Read( depth, RDG::Access::SampledCompute );
                 pass.Read( velocity, RDG::Access::SampledCompute );
                 pass.Read( exposureRef, RDG::Access::SampledCompute );
                 pass.Write( output, RDG::Access::StorageWrite );
             },
             [evaluation, color, depth, velocity, exposureRef,
              output]( RDG::PassContext& context ) mutable -> Common::BoolResultStr
             {
                 const auto commandBuffer = API::Vulkan::VulkanRdgBackend::CommandBufferOf( context );
                 if ( !commandBuffer )
                     return Common::MakeError( commandBuffer.GetError() );
                 const std::array<std::pair<Streamline::DlssImage*, std::pair<RDG::TextureRef, RDG::Access>>, 5>
                      images{ { { &evaluation.Color, { color, RDG::Access::SampledCompute } },
                                { &evaluation.Depth, { depth, RDG::Access::SampledCompute } },
                                { &evaluation.MotionVectors, { velocity, RDG::Access::SampledCompute } },
                                { &evaluation.Exposure, { exposureRef, RDG::Access::SampledCompute } },
                                { &evaluation.Output, { output, RDG::Access::StorageWrite } } } };
                 for ( const auto& [target, source] : images )
                 {
                     auto image = ImageOf( context, source.first, source.second );
                     if ( !image )
                         return Common::MakeError( image.GetError() );
                     *target = image.GetValue();
                 }
                 evaluation.EngineFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
                 return Streamline::Runtime::Get().Evaluate( commandBuffer.GetValue(), evaluation );
             } );

        return Common::MakeSuccess(
             TemporalUpscalerOutputs{ .SceneColor = output, .NewHistory = std::move( history ) } );
    }
} // namespace Desert::Graphic
