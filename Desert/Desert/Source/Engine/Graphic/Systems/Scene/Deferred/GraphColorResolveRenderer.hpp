#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <array>
#include <format>
#include <map>
#include <memory>

namespace Desert::Graphic
{
    // "<Color>: Resolve": GraphColorResolve.shader fetches sample 0 of a multisampled graph colour. The material
    // holds nothing of its own: u_Input is a pass parameter bound by GraphColorResolveRenderer::Record through
    // RDG::PassBindings from the node's SampledGraphics read. Header-only.
    class MaterialGraphColorResolve final : public Material
    {
    public:
        MaterialGraphColorResolve() : Material( "MaterialGraphColorResolve", "GraphColorResolve" )
        {
        }
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // THE SHADER RESOLVE OF A SampleZero GRAPH COLOUR (ViewRasterTargets.hpp GraphColorResolve). Vulkan resolves a
    // float colour attachment in a render pass only by averaging, which is wrong for vector data; a graph colour
    // declared SampleZero gets no in-pass resolve and this system records the raster node the graph adds for it
    // (AddGraphColorResolves): a full-screen triangle writing sample 0 of the multisampled attachment into the
    // single-sample one. One pipeline per format a SampleZero colour of the view has (kFormats), each built
    // against the graph's canonical render pass for exactly that target (one colour, no depth, one sample).
    class GraphColorResolveRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // The formats of the SampleZero graph colours SceneRenderer creates: the view's velocity. A graph colour
        // of another format declared SampleZero is refused by Record, naming it.
        static constexpr std::array<Core::Formats::ImageFormat, 1> kFormats = { ViewTargetFormats::kVelocity };

        Common::BoolResultStr Initialize() override
        {
            m_Pipelines.clear();
            m_Material.reset();
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "GraphColorResolve" );
            if ( !m_Shader )
                return Common::MakeError( "GraphColorResolve shader not found" );
            for ( const Core::Formats::ImageFormat format : kFormats )
            {
                GraphicsPipelineSpecification pipelineSpec;
                pipelineSpec.DebugName = "GraphColorResolve";
                pipelineSpec.TargetLayout =
                     RenderTargetLayout{ .ColorFormats = { format }, .DepthFormat = std::nullopt, .Samples = 1 };
                pipelineSpec.Shader            = m_Shader;
                pipelineSpec.DepthTestEnabled  = false;
                pipelineSpec.DepthWriteEnabled = false;
                const auto pipeline            = Graphic::GraphicsPipeline::Create( pipelineSpec );
                if ( !pipeline )
                    return Common::MakeError(
                         std::format( "GraphColorResolve pipeline: {}", pipeline.GetError() ) );
                m_Pipelines.emplace( format, pipeline.GetValue() );
            }
            m_Material = std::make_unique<MaterialGraphColorResolve>();
            return BOOLSUCCESS;
        }

        [[nodiscard]] bool IsReady() const
        {
            return m_Shader && m_Material && m_Pipelines.size() == kFormats.size();
        }

        // SETUP of "<Color>: Resolve": the node's one block (block 0) - @p multisample as u_Input, fetched at
        // sample 0 (PointClamp). Not ready: nothing declared, Record refuses (the node faults, the colour reads
        // its fault default).
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef multisample ) const
        {
            if ( !IsReady() || !multisample.IsValid() )
                return;
            pass.Bindings( m_BindingLayout.Get( m_Shader ), m_Material->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_Input", multisample, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() );
        }

        // Inside the render pass the graph opens on the single-sample colour of @p format: opens block 0.
        Common::BoolResultStr Record( const RDG::PassContext& context, Core::Formats::ImageFormat format )
        {
            if ( !IsReady() )
                return Common::MakeError( "GraphColorResolve recorded without its pipelines" );
            const auto found = m_Pipelines.find( format );
            if ( found == m_Pipelines.end() )
                return Common::MakeError(
                     std::format( "GraphColorResolve has no pipeline for format {} (add it to "
                                  "GraphColorResolveRenderer::kFormats)",
                                  static_cast<int>( format ) ) );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *found->second,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        std::shared_ptr<Shader> m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache                                        m_BindingLayout;
        std::map<Core::Formats::ImageFormat, std::shared_ptr<GraphicsPipeline>> m_Pipelines;
        std::unique_ptr<MaterialGraphColorResolve>                              m_Material;
    };
} // namespace Desert::Graphic::System
