#include "TemporalUpscaler.hpp"

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/TemporalAA.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

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
        }
        return nullptr;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& SupersampleResolveLayout()
    {
        using Kind = RDG::ShaderResourceKind;
        static const std::shared_ptr<const RDG::ShaderBindingLayout> layout =
             std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                  .ShaderName = kResolveShaderName,
                  .Slots = { { "u_Source", Kind::SampledTexture }, { "u_Destination", Kind::StorageTexture } },
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
                          .Sampled( "u_Source", source, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                                    RDG::SamplerDesc::PointClamp() )
                          .Storage( "u_Destination", destination, RDG::Access::StorageWrite, 0 )
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
         const std::function<const ITemporalUpscaler*( TemporalMethod )>& upscalerFor )
    {
        const int requested = viewportOverridePercent.value_or( settingPercent );
        // The upscaler of THIS view's percent, by Resolve's own rule (Scalability UpscalerForScale): the setting's
        // resolved upscaler is for the setting's percent, and a viewport override may sit on the other side of
        // 100 %. Only called where the rule has an answer (percent >= 100, or a temporal method).
        const auto upscalerAt = [&]( const int percent )
        { return Common::Scalability::UpscalerForScale( antiAliasing.Method, percent, upscaler ); };
        const auto choose = [&]( const int percent ) -> Common::ResultStr<ViewResolution>
        {
            const auto split = MakeResolutionSplit( output, percent );
            if ( !split )
                return Common::MakeFormattedError<ViewResolution>( "{}", split.GetError() );
            const auto method = SelectTemporalMethod( antiAliasing, split.GetValue(), *upscalerAt( percent ) );
            if ( !method )
                return Common::MakeFormattedError<ViewResolution>( "{}", method.GetError() );
            return Common::MakeSuccess( ViewResolution{ .Split = split.GetValue(), .Method = method.GetValue() } );
        };
        if ( !upscalerAt( requested ) )
        {
            // Below 100 % with no temporal method: nothing can upscale (no spatial upscaler) - Resolve's rule.
            auto native = choose( 100 );
            if ( native )
                native.GetValue().Clamped = std::format( "no spatial upscaler: {} % needs a temporal AA method, "
                                                         "the view's is not one: clamped to 100 %",
                                                         requested );
            return native;
        }
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
        clamped.Clamped        = refused + ": clamped to 100 %";
        return Common::MakeSuccess( clamped );
    }

} // namespace Desert::Graphic
