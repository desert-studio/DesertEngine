// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenIrradianceFieldGather.cpp (the gather over
// the irradiance field), adapted as described in IrradianceFieldGather.hpp (half resolution + upsample).
#include <Engine/Graphic/GlobalIllumination/IrradianceFieldGather.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <array>
#include <string>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct IrradianceFieldGatherShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GiHalfDiffuseOutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GiHalfSpecularOutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GiHalfBackfaceOutTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct IrradianceFieldGatherUpsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( Texture, GiHalfDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, GiHalfSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GiDiffuseOutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GiSpecularOutTexture );
        DESERT_SHADER_PARAMETER( Texture, GiHalfBackfaceTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GiBackfaceOutTexture );
    };
} // namespace

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        constexpr std::array<std::string_view, 2> kIrradianceFieldShaderNames{ "IrradianceFieldGather",
                                                                               "IrradianceFieldUpsample" };

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
        for ( uint32_t program = 0; program < ProgramCount; ++program )
        {
            const std::string name( kIrradianceFieldShaderNames[program] );
            m_Shaders[program] = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
            if ( !m_Shaders[program] )
                return Common::MakeFormattedError<bool>( "irradiance field: shader {} not found", name );
            auto pipeline = ComputePipeline::Create( { .Shader = m_Shaders[program], .DebugName = name } );
            if ( !pipeline )
                return Common::MakeFormattedError<bool>( "irradiance field: {}: {}", name, pipeline.GetError() );
            m_Pipelines[program] = pipeline.GetValue();
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr IrradianceFieldGather::EnsureImages( const glm::uvec2 extent )
    {
        if ( m_Diffuse && m_Specular && m_Backface && extent == m_Extent )
            return BOOLSUCCESS;
        const glm::uvec2 half = ( extent + 1u ) / 2u;
        m_HalfDiffuse         = CreateTarget( "IrradianceField.HalfDiffuse", half );
        m_HalfSpecular        = CreateTarget( "IrradianceField.HalfSpecular", half );
        m_HalfBackface        = CreateTarget( "IrradianceField.HalfBackface", half );
        m_Diffuse             = CreateTarget( "IrradianceField.Diffuse", extent );
        m_Specular            = CreateTarget( "IrradianceField.Specular", extent );
        m_Backface            = CreateTarget( "IrradianceField.Backface", extent );
        if ( !m_HalfDiffuse || !m_HalfSpecular || !m_HalfBackface || !m_Diffuse || !m_Specular || !m_Backface )
            return Common::MakeError<bool>( "irradiance field: an image could not be created" );
        m_Extent = extent;
        return BOOLSUCCESS;
    }

    Common::ResultStr<IrradianceFieldGatherFrame>
    IrradianceFieldGather::AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                                      const IrradianceFieldGatherInputs& inputs )
    {
        if ( inputs.Extent.x == 0 || inputs.Extent.y == 0 )
            return Common::MakeError<IrradianceFieldGatherFrame>(
                 "irradiance field: the G-buffer extent is empty" );
        if ( !inputs.GIFrame.IsValid() || !inputs.RadianceCache.Irradiance.IsValid() )
            return Common::MakeError<IrradianceFieldGatherFrame>(
                 "irradiance field: the GI frame record or the radiance cache of this frame is missing" );
        if ( const auto images = EnsureImages( inputs.Extent ); !images )
            return Common::MakeError<IrradianceFieldGatherFrame>( images.GetError() );

        const RDG::TextureRef halfDiffuse  = import( m_HalfDiffuse, "IrradianceField.HalfDiffuse" );
        const RDG::TextureRef halfSpecular = import( m_HalfSpecular, "IrradianceField.HalfSpecular" );
        const RDG::TextureRef halfBackface = import( m_HalfBackface, "IrradianceField.HalfBackface" );
        const RDG::TextureRef diffuse      = import( m_Diffuse, "IrradianceField.Diffuse" );
        const RDG::TextureRef specular     = import( m_Specular, "IrradianceField.Specular" );
        const RDG::TextureRef backface     = import( m_Backface, "IrradianceField.Backface" );

        constexpr RDG::Access kR   = RDG::Access::StorageRead;
        constexpr RDG::Access kW   = RDG::Access::StorageWrite;
        constexpr RDG::Access kS   = RDG::Access::SampledCompute;
        const auto            all  = RDG::SubresourceRange::All();
        // ASYNC COMPUTE (with the radiance cache it reads): both nodes read the G-buffer and the cache and write
        // their imports only; the graph joins them at the first graphics reader of the gathered lighting.
        const RDG::PassFlags  kAsyncPass = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        const glm::uvec2      half = ( inputs.Extent + 1u ) / 2u;
        {
            ComputePipeline* const pipeline = m_Pipelines[Gather].get();
            const glm::uvec2       groups( ( half.x + 7u ) / 8u, ( half.y + 7u ) / 8u );
            graph.AddPass(
                 "IrradianceField: Gather", kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Gather].Get( m_Shaders[Gather] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( IrradianceFieldGatherShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( IrradianceFieldGatherShaderParameters::RadianceCacheParamsUB,
                                    inputs.RadianceCache.Params, kR )
                          .Storage( IrradianceFieldGatherShaderParameters::RadianceCacheIndirectionUB,
                                    inputs.RadianceCache.Indirection, kR )
                          .Storage( IrradianceFieldGatherShaderParameters::RadianceCacheProbesUB,
                                    inputs.RadianceCache.Probes, kR )
                          .Texture( IrradianceFieldGatherShaderParameters::GBufferDepthTexture,
                                    inputs.GBufferDepth, kS, all )
                          .Sampler( IrradianceFieldGatherShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( IrradianceFieldGatherShaderParameters::GBufferBTexture, inputs.GBufferB, kS,
                                    all )
                          .Texture( IrradianceFieldGatherShaderParameters::RadianceCacheIrradianceTexture,
                                    inputs.RadianceCache.Irradiance, kS, all )
                          .Sampler( IrradianceFieldGatherShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Texture( IrradianceFieldGatherShaderParameters::RadianceCacheDepthTexture,
                                    inputs.RadianceCache.Depth, kS, all )
                          .Storage( IrradianceFieldGatherShaderParameters::GiHalfDiffuseOutTexture, halfDiffuse,
                                    kW )
                          .Storage( IrradianceFieldGatherShaderParameters::GiHalfSpecularOutTexture, halfSpecular,
                                    kW )
                          .Storage( IrradianceFieldGatherShaderParameters::GiHalfBackfaceOutTexture, halfBackface,
                                    kW );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }
        {
            ComputePipeline* const pipeline = m_Pipelines[Upsample].get();
            const glm::uvec2       groups( ( inputs.Extent.x + 7u ) / 8u, ( inputs.Extent.y + 7u ) / 8u );
            graph.AddPass(
                 "IrradianceField: Upsample", kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Upsample].Get( m_Shaders[Upsample] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( IrradianceFieldGatherUpsampleShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Texture( IrradianceFieldGatherUpsampleShaderParameters::GBufferDepthTexture,
                                    inputs.GBufferDepth, kS, all )
                          .Sampler( IrradianceFieldGatherUpsampleShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( IrradianceFieldGatherUpsampleShaderParameters::GBufferBTexture,
                                    inputs.GBufferB, kS, all )
                          .Texture( IrradianceFieldGatherUpsampleShaderParameters::GiHalfDiffuseTexture,
                                    halfDiffuse, kS, all )
                          .Texture( IrradianceFieldGatherUpsampleShaderParameters::GiHalfSpecularTexture,
                                    halfSpecular, kS, all )
                          .Storage( IrradianceFieldGatherUpsampleShaderParameters::GiDiffuseOutTexture, diffuse,
                                    kW )
                          .Storage( IrradianceFieldGatherUpsampleShaderParameters::GiSpecularOutTexture, specular,
                                    kW )
                          .Texture( IrradianceFieldGatherUpsampleShaderParameters::GiHalfBackfaceTexture,
                                    halfBackface, kS, all )
                          .Sampler( IrradianceFieldGatherUpsampleShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Storage( IrradianceFieldGatherUpsampleShaderParameters::GiBackfaceOutTexture, backface,
                                    kW );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        return Common::MakeSuccess(
             IrradianceFieldGatherFrame{ .Diffuse = diffuse, .Specular = specular, .Backface = backface } );
    }
} // namespace Desert::Graphic::GlobalIllumination
