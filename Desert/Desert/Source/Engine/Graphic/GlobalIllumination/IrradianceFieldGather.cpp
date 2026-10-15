// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenIrradianceFieldGather.cpp (the one gather
// pass over the irradiance field), adapted as described in IrradianceFieldGather.hpp.
#include <Engine/Graphic/GlobalIllumination/IrradianceFieldGather.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <string>

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        constexpr std::string_view kShaderName = "IrradianceFieldGather";

        std::shared_ptr<Image2D> CreateTarget( const std::string_view tag, const glm::uvec2 extent )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( tag ),
                 .Width      = extent.x,
                 .Height     = extent.y,
                 .Format     = ViewTargetFormats::kGIFinalGather,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image2D::Create( spec );
        }
    } // namespace

    IrradianceFieldGather::IrradianceFieldGather()  = default;
    IrradianceFieldGather::~IrradianceFieldGather() = default;

    Common::BoolResultStr IrradianceFieldGather::Initialize()
    {
        m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( kShaderName ) );
        if ( !m_Shader )
            return Common::MakeFormattedError<bool>( "irradiance field: shader {} not found", kShaderName );
        auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = std::string( kShaderName ) } );
        if ( !pipeline )
            return Common::MakeFormattedError<bool>( "irradiance field: {}: {}", kShaderName, pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr IrradianceFieldGather::EnsureImages( const glm::uvec2 extent )
    {
        if ( m_Diffuse && m_Specular && extent == m_Extent )
            return BOOLSUCCESS;
        m_Diffuse  = CreateTarget( "IrradianceField.Diffuse", extent );
        m_Specular = CreateTarget( "IrradianceField.Specular", extent );
        if ( !m_Diffuse || !m_Specular )
            return Common::MakeError<bool>( "irradiance field: an image could not be created" );
        m_Extent = extent;
        return BOOLSUCCESS;
    }

    Common::ResultStr<IrradianceFieldGatherFrame>
    IrradianceFieldGather::AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                                      const IrradianceFieldGatherInputs& inputs )
    {
        if ( inputs.Extent.x == 0 || inputs.Extent.y == 0 )
            return Common::MakeError<IrradianceFieldGatherFrame>( "irradiance field: the G-buffer extent is empty" );
        if ( !inputs.GIFrame.IsValid() || !inputs.RadianceCache.Irradiance.IsValid() )
            return Common::MakeError<IrradianceFieldGatherFrame>(
                 "irradiance field: the GI frame record or the radiance cache of this frame is missing" );
        if ( const auto images = EnsureImages( inputs.Extent ); !images )
            return Common::MakeError<IrradianceFieldGatherFrame>( images.GetError() );

        const RDG::TextureRef diffuse  = import( m_Diffuse, "IrradianceField.Diffuse" );
        const RDG::TextureRef specular = import( m_Specular, "IrradianceField.Specular" );

        constexpr RDG::Access  kR       = RDG::Access::StorageRead;
        constexpr RDG::Access  kW       = RDG::Access::StorageWrite;
        constexpr RDG::Access  kS       = RDG::Access::SampledCompute;
        const auto             all      = RDG::SubresourceRange::All();
        ComputePipeline* const pipeline = m_Pipeline.get();
        const glm::uvec2       groups( ( inputs.Extent.x + 7u ) / 8u, ( inputs.Extent.y + 7u ) / 8u );
        graph.AddPass(
             "IrradianceField: Gather", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& builder )
             {
                 auto block =
                      builder.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *pipeline ) );
                 block.Storage( "GIFrameUB", inputs.GIFrame, kR )
                      .Storage( "RadianceCacheParamsUB", inputs.RadianceCache.Params, kR )
                      .Storage( "RadianceCacheIndirectionUB", inputs.RadianceCache.Indirection, kR )
                      .Storage( "RadianceCacheProbesUB", inputs.RadianceCache.Probes, kR )
                      .Sampled( "u_GBufferDepth", inputs.GBufferDepth, kS, all, RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_GBufferB", inputs.GBufferB, kS, all, RDG::SamplerDesc::PointClamp() )
                      .Sampled( "u_RadianceCacheIrradiance", inputs.RadianceCache.Irradiance, kS, all,
                                RDG::SamplerDesc::LinearClamp() )
                      .Sampled( "u_RadianceCacheDepth", inputs.RadianceCache.Depth, kS, all,
                                RDG::SamplerDesc::LinearClamp() )
                      .Storage( "u_GIDiffuseOut", diffuse, kW )
                      .Storage( "u_GISpecularOut", specular, kW );
             },
             [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
             } );

        return Common::MakeSuccess( IrradianceFieldGatherFrame{ .Diffuse = diffuse, .Specular = specular } );
    }
} // namespace Desert::Graphic::GlobalIllumination
