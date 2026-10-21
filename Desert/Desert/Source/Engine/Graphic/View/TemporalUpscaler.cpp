#include "TemporalUpscaler.hpp"

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/TemporalAA.hpp>
#include <Engine/Graphic/View/TemporalSuperResolution.hpp>
#if defined( DESERT_UPSCALER_DLSS )
#include <Engine/Graphic/Upscalers/DLSS/DLSSUpscaler.hpp>
#endif
#if defined( DESERT_UPSCALER_FSR )
#include <Engine/Graphic/Upscalers/FSR/Fsr3Upscaler.hpp>
#endif
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <cmath>
#include <format>
#include <mutex>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t    kResolveGroupSize  = 8; // SupersampleResolve.shader LocalSize(8, 8, 1)
        constexpr const char* kResolveShaderName = "SupersampleResolve";

        uint32_t ResolveGroups( uint32_t size )
        {
            return ( size + kResolveGroupSize - 1 ) / kResolveGroupSize;
        }
    } // namespace

    std::unique_ptr<ITemporalUpscaler> CreateTemporalUpscaler( TemporalMethod method )
    {
        switch ( method )
        {
            case TemporalMethod::None:
                return nullptr;
            case TemporalMethod::TAA:
            case TemporalMethod::TAAU:
                return std::make_unique<TemporalAA>( method );
            case TemporalMethod::TSR:
                return std::make_unique<TemporalSuperResolution>();
            // A vendor method outside its SDK's build is never selected (SelectTemporalMethod reads
            // UpscalerBuild.hpp); null is "no implementation", which SceneViewState::BeginFrame names.
            case TemporalMethod::DLSS:
#if defined( DESERT_UPSCALER_DLSS )
                return std::make_unique<DLSSUpscaler>( Streamline::DlssFeature::SuperResolution );
#else
                return nullptr;
#endif
            case TemporalMethod::DLSSRR:
#if defined( DESERT_UPSCALER_DLSS )
                return std::make_unique<DLSSUpscaler>( Streamline::DlssFeature::RayReconstruction );
#else
                return nullptr;
#endif
            case TemporalMethod::FSR:
#if defined( DESERT_UPSCALER_FSR )
                return std::make_unique<Fsr3Upscaler>();
#else
                return nullptr;
#endif
        }
        return nullptr;
    }

    std::string TemporalMethodUnavailable( const TemporalMethod method )
    {
#if defined( DESERT_UPSCALER_DLSS )
        if ( method == TemporalMethod::DLSS )
            return DLSSUpscaler::Unavailable( Streamline::DlssFeature::SuperResolution );
        if ( method == TemporalMethod::DLSSRR )
            return DLSSUpscaler::Unavailable( Streamline::DlssFeature::RayReconstruction );
#endif
        // The engine's methods run wherever the engine runs; FSR's backend never refuses itself after the
        // catalog offered it (Fsr3Upscaler reports a failed dispatch as that frame's error).
        static_cast<void>( method );
        return {};
    }

    VendorMethodFallback
    ReplaceUnavailableVendorMethods( const Common::Scalability::PathAntiAliasing&               antiAliasing,
                                     const Common::Scalability::Upscaler                        upscaler,
                                     const std::function<std::string( TemporalMethod method )>& unavailable )
    {
        using Common::Scalability::AntiAliasingMethod;
        using Common::Scalability::Upscaler;
        VendorMethodFallback fallback{ .AntiAliasing = antiAliasing, .Upscaler = upscaler, .Why = {} };
        const auto           replace =
             [&]( const TemporalMethod method, const AntiAliasingMethod nativeAA, const Upscaler vendorUpscaler )
        {
            if ( antiAliasing.Method != nativeAA && upscaler != vendorUpscaler )
                return;
            const std::string why = unavailable( method );
            if ( why.empty() )
                return;
            if ( antiAliasing.Method == nativeAA )
                fallback.AntiAliasing.Method = AntiAliasingMethod::TAA;
            if ( upscaler == vendorUpscaler )
                fallback.Upscaler = Upscaler::None;
            fallback.Why = why;
        };
        // Ray Reconstruction first, onto DLSS Super Resolution (the same network without the denoiser), which the
        // DLSS rule below then checks like any DLSS request.
        if ( upscaler == Upscaler::DLSSRayReconstruction )
            if ( const std::string why = unavailable( TemporalMethod::DLSSRR ); !why.empty() )
            {
                // The upscaler here is the vendor override UpscalerForScale reads: DLSS below 100 %, and at 100 %
                // (under DLAA) it resolves to None, where DLAA is plain DLSS.
                fallback.Upscaler = Upscaler::DLSS;
                fallback.Why = std::format( "{}; DLSS Super Resolution instead", why );
            }
        const std::string rrWhy = fallback.Why;
        const Upscaler    asked = fallback.Upscaler;
        const auto        replaceDlss = [&]()
        {
            const bool native = fallback.AntiAliasing.Method == AntiAliasingMethod::DLAA;
            if ( !native && asked != Upscaler::DLSS )
                return;
            const std::string why = unavailable( TemporalMethod::DLSS );
            if ( why.empty() )
                return;
            if ( native )
                fallback.AntiAliasing.Method = AntiAliasingMethod::TAA;
            if ( asked == Upscaler::DLSS )
                fallback.Upscaler = Upscaler::None;
            fallback.Why = rrWhy.empty() ? why : std::format( "{}; {}", rrWhy, why );
        };
        replaceDlss();
        replace( TemporalMethod::FSR, AntiAliasingMethod::FSRNative, Upscaler::FSR );
        return fallback;
    }

    std::string RayReconstructionRefusal( const Common::Scalability::ReflectionMethod reflections,
                                          const Common::Scalability::GIMethod         globalIllumination,
                                          const Common::Scalability::ShadowMethod     shadows,
                                          const bool                                  deferredPath )
    {
        namespace Scal = Common::Scalability;
        if ( !deferredPath )
            return "DLSS Ray Reconstruction reads its guides from the deferred G-buffer; this view renders the "
                   "forward path";
        if ( Scal::IsDenoisedSignal( reflections ) || Scal::IsDenoisedSignal( globalIllumination ) ||
             Scal::IsDenoisedSignal( shadows ) )
            return {};
        return std::format( "DLSS Ray Reconstruction denoises traced lighting and this view traces none "
                            "(Reflections.Method {}, GlobalIllumination.Method {}, Shadows.Method {})",
                            Scal::kReflectionMethodNames[static_cast<std::size_t>( reflections )],
                            Scal::kGIMethodNames[static_cast<std::size_t>( globalIllumination )],
                            Scal::kShadowMethodNames[static_cast<std::size_t>( shadows )] );
    }

    bool ITemporalUpscaler::TakesRawLightingSignals() const
    {
        return false;
    }

    uint32_t ITemporalUpscaler::JitterSequenceLength( const ResolutionSplit& split ) const
    {
        return TemporalJitterSequenceLength( Method(), split );
    }

    glm::vec2 ITemporalUpscaler::JitterPixels( const uint32_t index, const uint32_t length ) const
    {
        return TemporalJitterPixels( index, length );
    }

    Common::ResultStr<RDG::TextureRef> ITemporalUpscaler::AddOpaquePasses(
         RDG::Builder& /*graph*/, const ViewFrame& /*frame*/, const TemporalUpscalerOpaqueInputs& /*inputs*/ ) const
    {
        return Common::MakeSuccess( RDG::TextureRef{} );
    }

    float ITemporalUpscaler::MaterialMipBias( const ResolutionSplit& split ) const
    {
        // Only a TEMPORAL upscale reconstructs output-pixel detail from the jittered samples (UE applies the
        // screen-percentage mip bias under temporal upsampling only); the method None never reaches here.
        if ( split.Mode != Common::Scalability::ScaleMode::Upscale || split.Render.Width == 0 ||
             split.Output.Width == 0 )
            return 0.0f;
        return std::log2( static_cast<float>( split.Render.Width ) / static_cast<float>( split.Output.Width ) );
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& SupersampleResolveLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                  .ShaderName        = kResolveShaderName,
                  .Slots             = { { "SourceColorTexture", Kind::Texture },
                                         { "PointClampSampler", Kind::Sampler },
                                         { "DestinationColorTexture", Kind::StorageTexture } },
                  .PushConstantBytes = static_cast<uint32_t>( sizeof( SupersampleResolvePush ) ) } );
        return layout;
    }

    struct SupersampleResolve::PipelineHolder
    {
        std::mutex                       Mutex;
        std::shared_ptr<ComputePipeline> Pipeline;

        std::shared_ptr<ComputePipeline> Get( std::string& error )
        {
            const std::lock_guard lock( Mutex );
            if ( Pipeline )
                return Pipeline;
            Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> shader  = service ? service->GetByName( kResolveShaderName ) : nullptr;
            if ( !shader )
            {
                error = std::format( "SupersampleResolve: the shader '{}' is not registered — expected "
                                     "Editor/Resources/Shaders/Programs/TemporalAA/SupersampleResolve.shader",
                                     kResolveShaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = kResolveShaderName } );
            if ( !built )
            {
                error = std::format( "SupersampleResolve: {}", built.GetError() );
                return nullptr;
            }
            Pipeline = built.GetValue();
            return Pipeline;
        }
    };

    SupersampleResolve::SupersampleResolve() : m_Pipeline( std::make_unique<PipelineHolder>() )
    {
    }

    SupersampleResolve::~SupersampleResolve() = default;

    Common::ResultStr<RDG::TextureRef> SupersampleResolve::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                      RDG::TextureRef sceneColor ) const
    {
        const ResolutionSplit& split = frame.Split;
        if ( !sceneColor.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "SupersampleResolve: the input SceneColor is not a texture of this graph" );
        if ( split.Mode != Common::Scalability::ScaleMode::Supersample || split.Output.Width == 0 ||
             split.Output.Height == 0 )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", std::format( "SupersampleResolve: the split {}x{} -> {}x{} ({} %) is not a "
                                    "supersample",
                                    split.Render.Width, split.Render.Height, split.Output.Width,
                                    split.Output.Height, split.RenderScalePercent ) );

        const auto addAxis = [&]( const char* name, RDG::TextureRef source, RDG::Extent3D sourceSize,
                                  RDG::Extent3D destinationSize, int32_t axis )
        {
            RDG::TextureDesc desc;
            desc.Size                                = destinationSize;
            desc.Format                              = Core::Formats::ImageFormat::RGBA16F;
            const RDG::TextureRef        destination = graph.CreateTexture( desc, name );
            const SupersampleResolvePush push{ .SourceWidth       = static_cast<int32_t>( sourceSize.Width ),
                                               .SourceHeight      = static_cast<int32_t>( sourceSize.Height ),
                                               .DestinationWidth  = static_cast<int32_t>( destinationSize.Width ),
                                               .DestinationHeight = static_cast<int32_t>( destinationSize.Height ),
                                               .Axis              = axis };
            PipelineHolder*              holder = m_Pipeline.get();
            graph.AddPass(
                 name, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     // texelFetch only: the Catmull-Rom weights are the filter, the sampler filters nothing.
                     pass.Bindings( SupersampleResolveLayout(), RDG::OtherRouteFill{} )
                          .Texture( "SourceColorTexture", source, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( "PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Storage( "DestinationColorTexture", destination, RDG::Access::StorageWrite, 0 )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( SupersampleResolvePush ) ) );
                 },
                 [holder, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     std::string                            error;
                     const std::shared_ptr<ComputePipeline> pipeline = holder->Get( error );
                     if ( !pipeline )
                         return Common::MakeError( error );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, sizeof( push ) );
                     return Renderer::GetInstance().DispatchCompute(
                          bindings, *pipeline, ResolveGroups( static_cast<uint32_t>( push.DestinationWidth ) ),
                          ResolveGroups( static_cast<uint32_t>( push.DestinationHeight ) ), 1 );
                 } );
            return destination;
        };

        const RDG::Extent3D   render{ split.Render.Width, split.Render.Height, 1 };
        const RDG::Extent3D   horizontal{ split.Output.Width, split.Render.Height, 1 };
        const RDG::Extent3D   output{ split.Output.Width, split.Output.Height, 1 };
        const RDG::TextureRef halfway =
             addAxis( "SupersampleResolve.Horizontal", sceneColor, render, horizontal, 0 );
        return Common::MakeSuccess( addAxis( "SupersampleResolve.Output", halfway, horizontal, output, 1 ) );
    }
    ViewExtent ViewTargetSetExtent( const ViewTargetSet set, const ResolutionSplit& split )
    {
        return set == ViewTargetSet::Render ? split.Render : split.Output;
    }

    Common::ResultStr<ViewResolution> ResolveViewResolution(
         const ViewExtent output, const int settingPercent, const std::optional<int> viewportOverridePercent,
         const Common::Scalability::PathAntiAliasing& antiAliasing, const Common::Scalability::Upscaler upscaler,
         const Common::Scalability::EngineTemporalUpscaler                engineTemporal,
         const std::function<const ITemporalUpscaler*( TemporalMethod )>& upscalerFor )
    {
        const int requested = viewportOverridePercent.value_or( settingPercent );
        // The upscaler of THIS view's percent, by Resolve's own rule (Scalability UpscalerForScale): the setting's
        // resolved upscaler is for the setting's percent, and a viewport override may sit on the other side of
        // 100 % (or under another AA method's side: below 100 % without temporal AA it is Spatial).
        const auto upscalerAt = [&]( const int percent ) {
            return Common::Scalability::UpscalerForScale( antiAliasing.Method, percent, upscaler, engineTemporal );
        };
        const auto choose = [&]( const int percent ) -> Common::ResultStr<ViewResolution>
        {
            const auto split = MakeResolutionSplit( output, percent );
            if ( !split )
                return Common::MakeFormattedError<ViewResolution>( "{}", split.GetError() );
            const Common::Scalability::Upscaler upscalerOfView = upscalerAt( percent );
            const auto method = SelectTemporalMethod( antiAliasing, split.GetValue(), upscalerOfView );
            if ( !method )
                return Common::MakeFormattedError<ViewResolution>( "{}", method.GetError() );
            return Common::MakeSuccess( ViewResolution{
                 .Split = split.GetValue(), .Method = method.GetValue(), .Upscaler = upscalerOfView } );
        };
        auto chosen = choose( requested );
        if ( !chosen )
            return chosen;
        const ITemporalUpscaler* implementation = upscalerFor ? upscalerFor( chosen.GetValue().Method ) : nullptr;
        if ( implementation == nullptr || implementation->Supports( chosen.GetValue().Split ) )
            return chosen;
        const std::string refused = std::format( "{} does not resolve {} % of {}x{}", implementation->DebugName(),
                                                 requested, output.Width, output.Height );
        if ( requested == 100 )
            return Common::MakeFormattedError<ViewResolution>( "ResolveViewResolution: {}", refused );
        auto native = choose( 100 );
        if ( !native )
            return Common::MakeFormattedError<ViewResolution>( "ResolveViewResolution: {}; at 100 %: {}", refused,
                                                               native.GetError() );
        const ITemporalUpscaler* nativeImplementation = upscalerFor( native.GetValue().Method );
        if ( nativeImplementation != nullptr && !nativeImplementation->Supports( native.GetValue().Split ) )
            return Common::MakeFormattedError<ViewResolution>(
                 "ResolveViewResolution: {}; nor does {} resolve 100 %", refused,
                 nativeImplementation->DebugName() );
        ViewResolution clamped = native.GetValue();
        clamped.Clamped        = std::format( "{}: clamped to 100 %", refused );
        return Common::MakeSuccess( clamped );
    }

} // namespace Desert::Graphic
