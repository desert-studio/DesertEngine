#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>

namespace Desert::Graphic
{
    // "Scene: DepthResolve": binds the multisampled scene depth SceneDepthResolve.shader fetches sample 0 of. The
    // node reads it as SampledGraphics; the descriptor names that declared access's layout. Header-only.
    class MaterialSceneDepthResolve final : public Material
    {
    public:
        MaterialSceneDepthResolve() : Material( "MaterialSceneDepthResolve", "SceneDepthResolve" )
        {
            m_Depth = m_MaterialExecutor->GetTexture2DProperty( "u_Depth" ).get();
        }

        void BindInputs( const std::shared_ptr<Image2D>& depth )
        {
            if ( m_Depth && depth )
                m_Depth->SetImage( depth.get(), RDG::Access::SampledGraphics );
        }

    private:
        Texture2DProperty* m_Depth = nullptr;
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // The compute passes that read scene depth (height fog, volumetric clouds) sample a SINGLE-SAMPLE depth: a
    // sampler2D over a multisampled image is invalid (VUID-RuntimeSpirv-samples-08725). At MSAA > 1 this system
    // owns SceneDepthResolved, a 1x depth-only framebuffer resized with the scene target, and records the raster
    // node that writes sample 0 of the multisampled scene depth into it (UE: ResolveSceneDepth before the
    // post-processing). The pipeline is built against the graph's canonical render pass for exactly that target
    // (TargetLayout: no colour, the scene depth format, one sample). At MSAA 1 nothing is built: the consumers
    // read the scene depth itself (SceneRenderer::GetComputeSceneDepth).
    class SceneDepthResolveRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "SceneDepthResolve: scene target framebuffer missing" );
            const FramebufferSpecification spec = target->GetSpecification();
            // Called again when the scene's sample count changes (SceneRenderer::ApplySceneSampleCount):
            // what was built for the previous count goes first.
            m_Resolved.reset();
            m_Pipeline.reset();
            m_Material.reset();
            if ( spec.Samples <= 1 || target->GetDepthAttachmentCount() == 0 )
                return BOOLSUCCESS;

            const auto depthFormat = target->GetDepthAttachmentImage()->GetImageSpecification().Format;
            FramebufferSpecification resolvedSpec;
            resolvedSpec.DebugName = "SceneDepthResolved";
            resolvedSpec.Samples   = 1;
            resolvedSpec.Attachments.Attachments.emplace_back( depthFormat );
            m_Resolved = Framebuffer::Create( resolvedSpec );
            if ( !m_Resolved )
                return Common::MakeError( "SceneDepthResolved framebuffer could not be created" );
            m_Resolved->Resize( target->GetFramebufferWidth(), target->GetFramebufferHeight() );

            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SceneDepthResolve" );
            if ( !m_Shader )
                return Common::MakeError( "SceneDepthResolve shader not found" );

            GraphicsPipelineSpecification pipelineSpec;
            pipelineSpec.DebugName = "SceneDepthResolve";
            pipelineSpec.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = {}, .DepthFormat = depthFormat, .Samples = 1 };
            pipelineSpec.Shader            = m_Shader;
            pipelineSpec.DepthTestEnabled  = true;
            pipelineSpec.DepthCompareOp    = CompareOp::Always;
            pipelineSpec.DepthWriteEnabled = true;
            const auto pipeline            = Graphic::GraphicsPipeline::Create( pipelineSpec );
            if ( !pipeline )
                return Common::MakeError( std::format( "SceneDepthResolve pipeline: {}", pipeline.GetError() ) );
            m_Pipeline = pipeline.GetValue();
            m_Material = std::make_unique<MaterialSceneDepthResolve>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // The scene target is multisampled and the resolve was built for it.
        bool IsReady() const
        {
            return m_Resolved && m_Pipeline && m_Material;
        }

        // With the scene target (SceneRenderer::Resize).
        void Resize( uint32_t width, uint32_t height )
        {
            if ( m_Resolved )
                m_Resolved->Resize( width, height );
        }

        const std::shared_ptr<Framebuffer>& GetFramebuffer() const
        {
            return m_Resolved;
        }

        // Inside the render pass the frame graph opens on SceneDepthResolved ("Scene: DepthResolve").
        Common::BoolResultStr Record( const std::shared_ptr<Image2D>& sceneDepth )
        {
            if ( !IsReady() || !sceneDepth )
                return Common::MakeError( "SceneDepthResolve recorded without its pipeline or the scene depth" );
            m_Material->BindInputs( sceneDepth );
            Renderer::GetInstance().SubmitFullscreenQuad( m_Pipeline.get(), m_Material->GetMaterialExecutor() );
            return BOOLSUCCESS;
        }

    private:
        std::shared_ptr<Framebuffer>               m_Resolved;
        std::shared_ptr<Shader>                    m_Shader;
        std::shared_ptr<GraphicsPipeline>          m_Pipeline;
        std::unique_ptr<MaterialSceneDepthResolve> m_Material;
    };
} // namespace Desert::Graphic::System
