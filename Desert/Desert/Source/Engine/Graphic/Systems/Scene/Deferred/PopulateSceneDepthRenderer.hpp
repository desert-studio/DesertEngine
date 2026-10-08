#pragma once

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>
#include <memory>

namespace Desert::Graphic
{
    class MaterialPopulateSceneDepth final : public Material
    {
    public:
        MaterialPopulateSceneDepth() : Material( "MaterialPopulateSceneDepth", "PopulateSceneDepth" )
        {
        }
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // "Scene: PopulateSceneDepth" (TAA1-B step 6, UE PopulateSceneDepth before the editor primitives): after the
    // temporal resolve the overlay phases (debug lines, grid, gizmos, UI) draw into an OUTPUT-extent target set —
    // the resolved colour, an overlay velocity and an overlay depth. This node fills that depth from the
    // RENDER-extent scene depth, point-sampled (PopulateSceneDepth.shader), so the overlays depth-test against the
    // scene at any render scale, 100 % included (one path). Its target layout is the overlay set's: colour 0
    // velocity (kVelocity, written as no motion), depth kSceneDepth, one sample.
    //
    // Not a RenderSystem: it registers no pass and has no framebuffer; the SceneRenderer owns one and the graph
    // owns the targets. The pipeline is made on first use (Prepare), an error names why it could not be.
    class PopulateSceneDepthRenderer final
    {
    public:
        [[nodiscard]] Common::BoolResultStr Prepare()
        {
            if ( m_Pipeline )
                return BOOLSUCCESS;
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "PopulateSceneDepth" );
            if ( !m_Shader )
                return Common::MakeError( "PopulateSceneDepth shader not found" );
            GraphicsPipelineSpecification pipelineSpec;
            pipelineSpec.DebugName    = "PopulateSceneDepth";
            pipelineSpec.TargetLayout = RenderTargetLayout{ .ColorFormats = { ViewTargetFormats::kVelocity },
                                                            .DepthFormat  = ViewTargetFormats::kSceneDepth,
                                                            .Samples      = 1 };
            pipelineSpec.Shader            = m_Shader;
            pipelineSpec.DepthTestEnabled  = true;
            pipelineSpec.DepthCompareOp    = CompareOp::Always;
            pipelineSpec.DepthWriteEnabled = true;
            const auto pipeline            = Graphic::GraphicsPipeline::Create( pipelineSpec );
            if ( !pipeline )
                return Common::MakeError( std::format( "PopulateSceneDepth pipeline: {}", pipeline.GetError() ) );
            m_Pipeline = pipeline.GetValue();
            m_Material = std::make_unique<MaterialPopulateSceneDepth>();
            return BOOLSUCCESS;
        }

        [[nodiscard]] bool IsReady() const
        {
            return m_Pipeline && m_Material;
        }

        void DeclareBindings( RDG::PassBuilder& pass, const RDG::TextureRef sceneDepth ) const
        {
            pass.Bindings( m_BindingLayout.Get( m_Shader ), m_Material->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_Depth", sceneDepth, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() );
        }

        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const
        {
            if ( !IsReady() )
                return Common::MakeError( "PopulateSceneDepth recorded without its pipeline" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        std::shared_ptr<Shader>                     m_Shader;
        mutable ShaderBindingLayoutCache            m_BindingLayout;
        std::shared_ptr<GraphicsPipeline>           m_Pipeline;
        std::unique_ptr<MaterialPopulateSceneDepth> m_Material;
    };
} // namespace Desert::Graphic::System
