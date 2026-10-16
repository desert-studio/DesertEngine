#include "JumpFloodOutlineRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr Core::Formats::ImageFormat kSeedFormat = ViewTargetFormats::kJFASeed;
    }

    uint32_t JumpFloodOutlineRenderer::ComputeStepCount( uint32_t width, uint32_t height )
    {
        const uint32_t maxDim = std::max( { width, height, 2u } );
        return std::max( 1u, static_cast<uint32_t>( std::ceil( std::log2( static_cast<float>( maxDim ) ) ) ) );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::Initialize()
    {
        if ( !CreatePipelines() )
        {
            return Common::MakeError( "JumpFloodOutlineRenderer: failed to create pipelines" );
        }

        m_MaterialComposite = std::make_unique<MaterialJFAComposite>();

        return BOOLSUCCESS;
    }

    bool JumpFloodOutlineRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

        // @p overDisplay: Final, blended over the display image (kTonemap; FXAA and SMAA write the same format);
        // otherwise a seed transient, one kJFASeed colour target. Both against the graph's canonical render pass.
        const auto makePipeline = [&]( const std::string& shaderName, const bool overDisplay,
                                       const std::string& debugName ) -> std::shared_ptr<GraphicsPipeline>
        {
            const auto shader = shaderService->GetByName( shaderName );
            if ( !shader )
            {
                LOG_ERROR( "JumpFloodOutlineRenderer: missing shader '{}'", shaderName );
                return nullptr;
            }

            GraphicsPipelineSpecification spec;
            spec.DebugName = debugName;
            spec.Shader    = shader;
            spec.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = { overDisplay ? ViewTargetFormats::kTonemap : kSeedFormat },
                                     .DepthFormat  = std::nullopt };
            if ( overDisplay )
            {
                spec.BlendEnable         = true;
                spec.SrcColorBlendFactor = BlendFactor::One; // JFA_Final's colour is premultiplied
                spec.DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
            }
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            spec.CullMode          = CullMode::None;

            const auto pipeline = GraphicsPipeline::Create( spec );
            if ( !pipeline )
            {
                LOG_ERROR( "JumpFloodOutlineRenderer: {}", pipeline.GetError() );
                return nullptr;
            }
            return pipeline.GetValue();
        };

        static_assert( ViewTargetFormats::kFXAA == ViewTargetFormats::kTonemap &&
                            ViewTargetFormats::kSMAABlend == ViewTargetFormats::kTonemap,
                       "the outline's one Final pipeline draws onto whichever of the three is the display image" );
        m_InitPipeline  = makePipeline( "JFA_Init", false, "JFA_InitPipeline" );
        m_StepPipeline  = makePipeline( "JFA_Step", false, "JFA_StepPipeline" );
        m_FinalPipeline = makePipeline( "JFA_Final", true, "JFA_FinalPipeline" );

        return m_InitPipeline && m_StepPipeline && m_FinalPipeline;
    }

    bool JumpFloodOutlineRenderer::Prepare() const
    {
        if ( !m_InitPipeline || !m_StepPipeline || !m_FinalPipeline || !m_MaterialComposite )
        {
            LOG_ERROR( "JumpFloodOutlineRenderer::Prepare: a pipeline or the composite material is unavailable" );
            return false;
        }
        return true;
    }

    uint32_t JumpFloodOutlineRenderer::GetStepCount( const RDG::TextureDesc& seed )
    {
        return ComputeStepCount( seed.Size.Width, seed.Size.Height );
    }

    RDG::TextureDesc JumpFloodOutlineRenderer::GetSeedDesc( const RDG::TextureDesc& mask )
    {
        return RDG::TextureDesc{ .Size = { .Width = mask.Size.Width, .Height = mask.Size.Height },
                                 .Format = kSeedFormat };
    }

    bool JumpFloodOutlineRenderer::Runs() const
    {
        return m_Enabled && m_OutlineActive;
    }

    // The mask and the seeds hold per-texel data (coverage, a seed coordinate) that must not be blended between
    // texels, and a neighbour beyond the border must not wrap to the opposite edge: PointClamp.
    void JumpFloodOutlineRenderer::DeclareInitBindings( RDG::PassBuilder& pass, RDG::TextureRef mask ) const
    {
        if ( !m_InitPipeline )
            return; // RecordInit refuses by name
        // No material: the other route fills nothing.
        pass.Bindings( m_InitLayout.Get( m_InitPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_StencilTexture", mask, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::RecordInit( const RDG::PassContext& context )
    {
        if ( !m_InitPipeline )
            return Common::MakeError( "JumpFloodOutlineRenderer: the init pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_InitPipeline, nullptr );
    }

    void JumpFloodOutlineRenderer::DeclareStepBindings( RDG::PassBuilder& pass, RDG::TextureRef source ) const
    {
        if ( !m_StepPipeline )
            return; // RecordStep refuses by name
        pass.Bindings( m_StepLayout.Get( m_StepPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_InputTexture", source, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( int32_t ) ) );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::RecordStep( const RDG::PassContext& context, uint32_t step,
                                                                const uint32_t stepCount )
    {
        if ( !m_StepPipeline )
            return Common::MakeError( "JumpFloodOutlineRenderer: the step pipeline is not initialised" );
        // Ping-pong propagation with halving sample distance.
        const int32_t stepLength = 1 << ( stepCount - 1 - step );

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &stepLength, sizeof( stepLength ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_StepPipeline, nullptr );
    }

    void JumpFloodOutlineRenderer::FillFinalMaterial( const float displayGamma )
    {
        if ( !m_MaterialComposite )
            return;
        m_MaterialComposite->SetParams( glm::vec4( m_OutlineColor, 1.0f ), m_OutlineWidth, m_Smoothness,
                                        displayGamma );
    }

    void JumpFloodOutlineRenderer::DeclareFinalBindings( RDG::PassBuilder& pass, RDG::TextureRef seed ) const
    {
        if ( !m_FinalPipeline || !m_MaterialComposite )
            return; // RecordFinal refuses by name
        pass.Bindings( m_FinalLayout.Get( m_FinalPipeline->GetSpecification().Shader ),
                       m_MaterialComposite->GetMaterialExecutor()->GetRouteFill() )
             .Sampled( "u_JFATexture", seed, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::RecordFinal( const RDG::PassContext& context )
    {
        if ( !m_FinalPipeline || !m_MaterialComposite )
            return Common::MakeError( "JumpFloodOutlineRenderer: the composite pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_FinalPipeline,
                                                       m_MaterialComposite->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
