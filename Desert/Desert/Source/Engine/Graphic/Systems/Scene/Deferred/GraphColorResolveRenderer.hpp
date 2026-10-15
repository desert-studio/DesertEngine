#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <memory>

namespace Desert::Graphic
{
    // "<Color>: Resolve": GraphColorResolve.shader (a float colour) or GraphColorResolveUint.shader (an integer
    // one) fetches sample 0 of a multisampled graph colour. The material holds nothing of its own: u_Input is a
    // pass parameter bound by GraphColorResolveRenderer::Record through RDG::PassBindings from the node's
    // SampledGraphics read. Header-only.
    class MaterialGraphColorResolve final : public Material
    {
    public:
        explicit MaterialGraphColorResolve( const char* shaderName )
            : Material( "MaterialGraphColorResolve", shaderName )
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
    // single-sample one. One program per format a SampleZero colour of the view has (kFormats) - the fragment's
    // output type follows the attachment's (vec4 for a float target, uvec4 for an integer one) - each pipeline
    // built against the graph's canonical render pass for exactly that target (one colour, no depth, one sample).
    class GraphColorResolveRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // The formats of the SampleZero graph colours SceneRenderer creates, with the program that resolves each:
        // the view's velocity, and the forward depth prepass's contact-shadow shading word (an integer target,
        // which Vulkan never averages). A graph colour of another format declared SampleZero is refused by
        // Record, naming it.
        struct FormatProgram
        {
            Core::Formats::ImageFormat Format;
            const char*                ShaderName;
        };
        static constexpr std::array<FormatProgram, 2> kFormats = {
             FormatProgram{ ViewTargetFormats::kVelocity, "GraphColorResolve" },
             FormatProgram{ ViewTargetFormats::kGBufferShadingWord, "GraphColorResolveUint" } };

        Common::BoolResultStr Initialize() override
        {
            // A refusal leaves fewer programs than formats: IsReady stays false.
            m_Programs.clear();
            for ( const FormatProgram& entry : kFormats )
            {
                Program& program      = m_Programs[entry.Format];
                program.ResolveShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( entry.ShaderName );
                if ( !program.ResolveShader )
                    return Common::MakeError( std::format( "{} shader not found", entry.ShaderName ) );
                GraphicsPipelineSpecification pipelineSpec;
                pipelineSpec.DebugName = entry.ShaderName;
                pipelineSpec.TargetLayout =
                     RenderTargetLayout{ .ColorFormats = { entry.Format }, .DepthFormat = std::nullopt, .Samples = 1 };
                pipelineSpec.Shader            = program.ResolveShader;
                pipelineSpec.DepthTestEnabled  = false;
                pipelineSpec.DepthWriteEnabled = false;
                const auto pipeline            = Graphic::GraphicsPipeline::Create( pipelineSpec );
                if ( !pipeline )
                    return Common::MakeError( std::format( "{} pipeline: {}", entry.ShaderName, pipeline.GetError() ) );
                program.Pipeline = pipeline.GetValue();
                program.Material = std::make_unique<MaterialGraphColorResolve>( entry.ShaderName );
            }
            return BOOLSUCCESS;
        }

        [[nodiscard]] bool IsReady() const
        {
            return m_Programs.size() == kFormats.size() &&
                   std::ranges::all_of( m_Programs, []( const auto& entry )
                                        { return entry.second.Pipeline && entry.second.Material; } );
        }

        // SETUP of "<Color>: Resolve": the node's one block (block 0) - @p multisample as u_Input, fetched at
        // sample 0 (PointClamp) by the program of @p format. Not ready or no program for the format: nothing
        // declared, Record refuses (the node faults, the colour reads its fault default).
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef multisample,
                              const Core::Formats::ImageFormat format ) const
        {
            const auto found = m_Programs.find( format );
            if ( !IsReady() || !multisample.IsValid() || found == m_Programs.end() )
                return;
            const Program& program = found->second;
            pass.Bindings( program.BindingLayout.Get( program.ResolveShader ),
                           program.Material->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_Input", multisample, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() );
        }

        // Inside the render pass the graph opens on the single-sample colour of @p format: opens block 0.
        Common::BoolResultStr Record( const RDG::PassContext& context, Core::Formats::ImageFormat format )
        {
            if ( !IsReady() )
                return Common::MakeError( "GraphColorResolve recorded without its pipelines" );
            const auto found = m_Programs.find( format );
            if ( found == m_Programs.end() )
                return Common::MakeError(
                     std::format( "GraphColorResolve has no program for format {} (add it to "
                                  "GraphColorResolveRenderer::kFormats)",
                                  static_cast<int>( format ) ) );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *found->second.Pipeline,
                                                           found->second.Material->GetMaterialExecutor() );
        }

    private:
        struct Program
        {
            std::shared_ptr<Shader> ResolveShader;
            // The block layout, derived from ResolveShader's reflection once per compile (not per frame).
            mutable ShaderBindingLayoutCache           BindingLayout;
            std::shared_ptr<GraphicsPipeline>          Pipeline;
            std::unique_ptr<MaterialGraphColorResolve> Material;
        };
        std::map<Core::Formats::ImageFormat, Program> m_Programs;
    };
} // namespace Desert::Graphic::System
