#include "RainStreakRenderer.hpp"

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kRainStreakShaderName      = "RainStreaks";
        constexpr uint32_t    kVerticesPerDrop = 6; // two triangles (RainStreaks.shader corners)
    } // namespace

    Common::BoolResultStr RainStreakRenderer::Initialize()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return Common::MakeError( "RainStreakRenderer: no shader service" );
        const auto shader = shaderService->GetByName( kRainStreakShaderName );
        if ( !shader )
            return Common::MakeError( "RainStreakRenderer: shader 'RainStreaks' is not registered "
                                      "(Editor/Resources/Shaders/Programs/Weather/RainStreaks.shader)" );
        if ( !m_TargetFramebuffer.lock() )
            return Common::MakeError( "RainStreakRenderer: no scene target" );

        GraphicsPipelineSpecification spec;
        spec.DebugName    = kRainStreakShaderName;
        spec.Shader       = shader;
        spec.TargetLayout = SceneTargetLayout();
        // Tested against the opaque scene, never written: a translucent sheet (SystemRasterPass::DepthReadOnly).
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = false;
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.CullMode          = CullMode::None;
        spec.Topology          = PrimitiveTopology::Triangles;
        // Premultiplied over: scene = drop.rgb + scene * (1 - drop.a).
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::One;
        spec.DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;

        const auto pipeline = GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeFormattedError( "RainStreakRenderer: the pipeline was not built: {}",
                                               pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();
        return m_Layout.Validate( m_Pipeline->GetShader(), RainStreaksShaderParameters::kAll );
    }

    uint32_t RainStreakRenderer::DropCount( const float rainIntensity, const glm::vec4& rainDrops,
                                            const float spawnCountScale ) noexcept
    {
        if ( !( rainIntensity > 0.0f ) || !( rainDrops.x > 0.0f ) || !( rainDrops.y > 0.0f ) ||
             !( spawnCountScale > 0.0f ) )
            return 0;
        const double sideM  = 2.0 * static_cast<double>( rainDrops.y ) / 100.0;
        const double volume = sideM * sideM * sideM;
        const double drops  = static_cast<double>( rainDrops.x ) * volume *
                             static_cast<double>( std::min( rainIntensity, 1.0f ) ) *
                             static_cast<double>( std::min( spawnCountScale, 1.0f ) );
        return static_cast<uint32_t>( std::clamp( std::floor( drops ), 0.0, static_cast<double>( kMaxDrops ) ) );
    }

    SystemRasterPass RainStreakRenderer::DrawPass( const RainStreaksFrameInputs& inputs )
    {
        SystemRasterPass pass;
        const auto       target = m_TargetFramebuffer.lock();
        if ( !target || !m_Pipeline || inputs.Drops == 0 || !inputs.Uniforms.IsValid() ||
             !inputs.WorldParameters.IsValid() || !inputs.RainOcclusionMap.IsValid() ||
             !inputs.RainOcclusion.IsValid() )
            return pass;

        pass.Name              = "Weather: RainStreaks";
        pass.TargetFramebuffer = target;
        pass.DepthReadOnly     = true;
        const uint32_t drops   = inputs.Drops;
        pass.ExecuteFunc = [this, drops]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::DrawProcedural( bindings, *m_Pipeline, nullptr, kVerticesPerDrop, drops );
        };
        pass.Declare = [this, inputs]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            declared.Bindings( m_Layout.Get( m_Pipeline->GetShader() ), RDG::NoOtherRouteFill() )
                 .Uniform( RainStreaksShaderParameters::RainStreaksUB, inputs.Uniforms )
                 .Storage( RainStreaksShaderParameters::WorldParameters, inputs.WorldParameters,
                           RDG::Access::StorageRead )
                 .Texture( RainStreaksShaderParameters::RainOcclusionMapTexture, inputs.RainOcclusionMap,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Sampler( RainStreaksShaderParameters::RainOcclusionSampler, RDG::SamplerDesc::PointClamp() )
                 .Uniform( RainStreaksShaderParameters::RainOcclusionUB, inputs.RainOcclusion );
        };
        return pass;
    }
} // namespace Desert::Graphic::System
