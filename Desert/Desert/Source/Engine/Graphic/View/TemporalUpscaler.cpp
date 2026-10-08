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
} // namespace Desert::Graphic
