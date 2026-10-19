#include <Engine/Graphic/Upscalers/DLSS/DLSSUpscaler.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        constexpr const char* kGuidesShaderName = "DLSSRRGuides";
        constexpr const char* kGuidesPassName   = "DLSS-RR: Guides";
        constexpr uint32_t    kGuidesGroupSize  = 8; // DLSSRRGuides.shader LocalSize
        // DLSSRRGuides.shader's push block: u_InvJitteredViewProjection, u_CameraPos, u_Params.
        struct GuidesPush
        {
            glm::mat4 InvJitteredViewProjection;
            glm::vec4 CameraPos;
            glm::vec4 Params; // xy render extent, z = 1 when the reflection ray is this frame's trace
        };

        const std::shared_ptr<const RDG::ShaderBindingLayout>& GuidesLayout()
        {
            using Kind = RDG::ShaderResourceKind;
            static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
                 std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                      .ShaderName = kGuidesShaderName,
                      .Slots      = { { "u_GBufferAlbedo", Kind::SampledTexture },
                                      { "u_GBufferNormal", Kind::SampledTexture },
                                      { "u_GBufferDepth", Kind::SampledTexture },
                                      { "u_BRDFLUTTexture", Kind::SampledTexture },
                                      { "u_ReflectionRay", Kind::SampledTexture },
                                      { "u_DiffuseAlbedo", Kind::StorageTexture },
                                      { "u_SpecularAlbedo", Kind::StorageTexture },
                                      { "u_SpecularHitDistance", Kind::StorageTexture } } } );
            return layout;
        }
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

        // The Scalability row that holds @p mode's Ray Reconstruction preset.
        Common::Scalability::Parameter RRPresetParameter( const Streamline::DlssMode mode )
        {
            using Common::Scalability::Parameter;
            switch ( mode )
            {
                case Streamline::DlssMode::UltraPerformance:
                    return Parameter::DlssRRPresetUltraPerformance;
                case Streamline::DlssMode::Performance:
                    return Parameter::DlssRRPresetPerformance;
                case Streamline::DlssMode::Balanced:
                    return Parameter::DlssRRPresetBalanced;
                case Streamline::DlssMode::Quality:
                    return Parameter::DlssRRPresetQuality;
                case Streamline::DlssMode::UltraQuality:
                    return Parameter::DlssRRPresetUltraQuality;
                case Streamline::DlssMode::DLAA:
                    return Parameter::DlssRRPresetDLAA;
            }
            return Parameter::DlssRRPresetDLAA;
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
            explicit DlssHistory( const Streamline::DlssFeature feature )
                : m_Feature( feature ), m_Viewport( Streamline::Runtime::Get().AcquireViewport( feature ) )
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
                return Streamline::DlssFeatureName( m_Feature );
            }

            [[nodiscard]] Streamline::DlssFeature Feature() const
            {
                return m_Feature;
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
            const Streamline::DlssFeature          m_Feature;
            const uint32_t                         m_Viewport;
            std::optional<Streamline::DlssOptions> m_Options;
            uint64_t                               m_HeldBytes = 0;
        };
    } // namespace

    struct DLSSUpscaler::GuidesPipeline
    {
        std::mutex                       Mutex;
        std::shared_ptr<ComputePipeline> Pipeline;

        std::shared_ptr<ComputePipeline> Get( std::string& error )
        {
            const std::lock_guard lock( Mutex );
            if ( Pipeline )
                return Pipeline;
            Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> shader  = service ? service->GetByName( kGuidesShaderName ) : nullptr;
            if ( !shader )
            {
                error = std::format( "{}: the shader '{}' is not registered - expected "
                                     "Editor/Resources/Shaders/Programs/TemporalAA/DLSSRRGuides.shader",
                                     kGuidesPassName, kGuidesShaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = kGuidesShaderName } );
            if ( !built )
            {
                error = std::format( "{}: {}", kGuidesPassName, built.GetError() );
                return nullptr;
            }
            Pipeline = built.GetValue();
            return Pipeline;
        }
    };

    DLSSUpscaler::DLSSUpscaler( const Streamline::DlssFeature feature )
        : m_Feature( feature ), m_GuidesPipeline( std::make_unique<GuidesPipeline>() )
    {
    }
    DLSSUpscaler::~DLSSUpscaler() = default;

    std::string_view DLSSUpscaler::DebugName() const
    {
        return m_Feature == Streamline::DlssFeature::RayReconstruction ? "DLSS-RR" : "DLSS";
    }

    Common::ResultStr<Streamline::DlssRenderRange> DLSSUpscaler::RangeOf( const Streamline::DlssMode mode,
                                                                          const ViewExtent           output ) const
    {
        const std::scoped_lock lock( m_RangeMutex );
        const auto             key = std::make_tuple( static_cast<uint8_t>( mode ), output.Width, output.Height );
        if ( const auto found = m_Ranges.find( key ); found != m_Ranges.end() )
            return found->second;
        auto range = Streamline::Runtime::Get().RenderRange( m_Feature, mode, output.Width, output.Height );
        m_Ranges.emplace( key, range );
        return range;
    }

    Common::ResultStr<Streamline::DlssMode> DLSSUpscaler::ModeFor( const ResolutionSplit& split ) const
    {
        const Streamline::Runtime& runtime = Streamline::Runtime::Get();
        if ( !runtime.Ready( m_Feature ) )
            return Common::MakeFormattedError<Streamline::DlssMode>(
                 "{} is not available: {}", Streamline::DlssFeatureName( m_Feature ), runtime.Refusal( m_Feature ) );
        switch ( split.Mode )
        {
            case Common::Scalability::ScaleMode::Supersample:
                return Common::MakeFormattedError<Streamline::DlssMode>( "{} does not take a supersampled input ({} %)",
                                                                         Streamline::DlssFeatureName( m_Feature ),
                                                                         split.RenderScalePercent );
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
                 "no {} mode renders {}x{} for an output of {}x{} ({} %):{}", Streamline::DlssFeatureName( m_Feature ),
                 split.Render.Width,
                 split.Render.Height, split.Output.Width, split.Output.Height, split.RenderScalePercent, tried );
        return Common::MakeSuccess( *best );
    }

    std::string DLSSUpscaler::Unavailable( const Streamline::DlssFeature feature )
    {
        const Streamline::Runtime& runtime = Streamline::Runtime::Get();
        if ( runtime.Ready( feature ) )
            return {};
        return std::format( "{} is not available: {}", Streamline::DlssFeatureName( feature ),
                            runtime.Refusal( feature ) );
    }

    Common::ResultStr<DLSSUpscaler::Guides> DLSSUpscaler::AddGuidesPass( RDG::Builder& graph, const ViewFrame& frame,
                                                                         const TemporalUpscalerInputs& inputs ) const
    {
        const auto missing = [&]( const char* what ) -> Common::ResultStr<Guides>
        {
            return Common::MakeFormattedError<Guides>(
                 "{}: the input {} is not a texture of this graph (Ray Reconstruction reads its guides from the "
                 "deferred G-buffer)",
                 DebugName(), what );
        };
        if ( !inputs.GBufferAlbedo.IsValid() )
            return missing( "GBufferAlbedo" );
        if ( !inputs.GBufferNormal.IsValid() )
            return missing( "GBufferNormal" );
        if ( !inputs.BrdfLut.IsValid() )
            return missing( "BrdfLut" );
        if ( !inputs.Black.IsValid() )
            return missing( "Black" );

        const uint32_t   width  = frame.Split.Render.Width;
        const uint32_t   height = frame.Split.Render.Height;
        RDG::TextureDesc albedoDesc;
        albedoDesc.Size   = RDG::Extent3D{ width, height, 1 };
        albedoDesc.Format = Core::Formats::ImageFormat::RGBA16F;
        RDG::TextureDesc distanceDesc = albedoDesc;
        distanceDesc.Format           = Core::Formats::ImageFormat::R32F;
        const Guides guides{ .DiffuseAlbedo       = graph.CreateTexture( albedoDesc, "DLSS-RR.DiffuseAlbedo" ),
                             .SpecularAlbedo      = graph.CreateTexture( albedoDesc, "DLSS-RR.SpecularAlbedo" ),
                             .SpecularHitDistance = graph.CreateTexture( distanceDesc, "DLSS-RR.SpecularHitDistance" ) };
        const bool            traced = inputs.ReflectionRay.IsValid();
        const RDG::TextureRef ray    = traced ? inputs.ReflectionRay : inputs.Black;
        const GuidesPush      push{ .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                                    .CameraPos                 = glm::vec4( frame.CameraPosition, 1.0f ),
                                    .Params = glm::vec4( static_cast<float>( width ), static_cast<float>( height ),
                                                         traced ? 1.0f : 0.0f, 0.0f ) };
        GuidesPipeline* holder = m_GuidesPipeline.get();
        graph.AddPass(
             kGuidesPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( GuidesLayout(), RDG::OtherRouteFill{} )
                      .Sampled( "u_GBufferAlbedo", inputs.GBufferAlbedo, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_GBufferNormal", inputs.GBufferNormal, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_GBufferDepth", inputs.SceneDepth, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_BRDFLUTTexture", inputs.BrdfLut, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() )
                      .Sampled( "u_ReflectionRay", ray, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                                RDG::SamplerDesc::PointClamp() )
                      .Storage( "u_DiffuseAlbedo", guides.DiffuseAlbedo, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_SpecularAlbedo", guides.SpecularAlbedo, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_SpecularHitDistance", guides.SpecularHitDistance, RDG::Access::StorageWrite, 0 )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( GuidesPush ) ) );
             },
             [holder, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            error;
                 const std::shared_ptr<ComputePipeline> pipeline = holder->Get( error );
                 if ( !pipeline )
                     return Common::MakeError( error );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline,
                                                                 ( width + kGuidesGroupSize - 1 ) / kGuidesGroupSize,
                                                                 ( height + kGuidesGroupSize - 1 ) / kGuidesGroupSize,
                                                                 1 );
             } );
        return Common::MakeSuccess( guides );
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

        // The view's feature: its own from last frame, or a new one (first frame of this feature, after a reset
        // or another method's history - DLSS Super Resolution's history is not Ray Reconstruction's).
        std::shared_ptr<ITemporalUpscalerHistory> history = inputs.PrevHistory;
        if ( const auto* own = dynamic_cast<DlssHistory*>( history.get() ); own == nullptr || own->Feature() != m_Feature )
            history = std::make_shared<DlssHistory>( m_Feature );
        auto&                         feature = static_cast<DlssHistory&>( *history );
        const bool                    rr      = m_Feature == Streamline::DlssFeature::RayReconstruction;
        const Streamline::DlssOptions options{
             .Feature      = m_Feature,
             .Mode         = mode.GetValue(),
             .OutputWidth  = frame.Split.Output.Width,
             .OutputHeight = frame.Split.Output.Height,
             .Preset = inputs.Quality->As<Common::Scalability::DlssPreset>( PresetParameter( mode.GetValue() ) ),
             .RRPreset =
                  inputs.Quality->As<Common::Scalability::DlssRRPreset>( RRPresetParameter( mode.GetValue() ) ) };
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

        Guides guides;
        if ( rr )
        {
            const Common::ResultStr<Guides> added = AddGuidesPass( graph, frame, inputs );
            if ( !added )
                return Common::MakeError<TemporalUpscalerOutputs>( added.GetError() );
            guides = added.GetValue();
        }

        Streamline::DlssEvaluation evaluation;
        evaluation.Viewport       = feature.Viewport();
        evaluation.Options        = options;
        evaluation.WorldToView    = frame.View;
        evaluation.ViewToWorld    = frame.InvView;
        evaluation.RenderWidth    = frame.Split.Render.Width;
        evaluation.RenderHeight   = frame.Split.Render.Height;
        evaluation.OutputWidth    = frame.Split.Output.Width;
        evaluation.OutputHeight   = frame.Split.Output.Height;
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
        const RDG::TextureRef normal      = rr ? inputs.GBufferNormal : RDG::TextureRef{};
        const bool            traced      = rr && inputs.ReflectionRay.IsValid();
        graph.AddPass(
             DebugName().data(), RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( color, RDG::Access::SampledCompute );
                 pass.Read( depth, RDG::Access::SampledCompute );
                 pass.Read( velocity, RDG::Access::SampledCompute );
                 pass.Read( exposureRef, RDG::Access::SampledCompute );
                 if ( rr )
                 {
                     pass.Read( guides.DiffuseAlbedo, RDG::Access::SampledCompute );
                     pass.Read( guides.SpecularAlbedo, RDG::Access::SampledCompute );
                     pass.Read( normal, RDG::Access::SampledCompute );
                     if ( traced )
                         pass.Read( guides.SpecularHitDistance, RDG::Access::SampledCompute );
                 }
                 // ExternalWrite, not StorageWrite: NGX also clears the output (vkCmdClearColorImage on a
                 // reset), which needs TRANSFER_DST usage and the transfer stage in the barrier into this pass.
                 pass.Write( output, RDG::Access::ExternalWrite );
             },
             [evaluation, color, depth, velocity, exposureRef, output, guides, normal, rr,
              traced]( RDG::PassContext& context ) mutable -> Common::BoolResultStr
             {
                 const auto commandBuffer = API::Vulkan::VulkanRdgBackend::CommandBufferOf( context );
                 if ( !commandBuffer )
                     return Common::MakeError( commandBuffer.GetError() );
                 std::vector<std::pair<Streamline::DlssImage*, std::pair<RDG::TextureRef, RDG::Access>>> images{
                      { &evaluation.Color, { color, RDG::Access::SampledCompute } },
                      { &evaluation.Depth, { depth, RDG::Access::SampledCompute } },
                      { &evaluation.MotionVectors, { velocity, RDG::Access::SampledCompute } },
                      { &evaluation.Exposure, { exposureRef, RDG::Access::SampledCompute } },
                      { &evaluation.Output, { output, RDG::Access::ExternalWrite } } };
                 if ( rr )
                 {
                     images.push_back(
                          { &evaluation.DiffuseAlbedo, { guides.DiffuseAlbedo, RDG::Access::SampledCompute } } );
                     images.push_back(
                          { &evaluation.SpecularAlbedo, { guides.SpecularAlbedo, RDG::Access::SampledCompute } } );
                     images.push_back( { &evaluation.NormalRoughness, { normal, RDG::Access::SampledCompute } } );
                     // Not tagged where the view traced no reflection (the image stays null): RR then has no
                     // hit distance rather than a field of zeros it would read as hits at the surface.
                     if ( traced )
                         images.push_back( { &evaluation.SpecularHitDistance,
                                             { guides.SpecularHitDistance, RDG::Access::SampledCompute } } );
                 }
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
