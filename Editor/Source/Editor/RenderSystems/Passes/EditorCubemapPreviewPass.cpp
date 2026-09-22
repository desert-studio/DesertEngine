#include "EditorCubemapPreviewPass.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Editor::Render
{
    EditorCubemapPreviewPass::~EditorCubemapPreviewPass()
    {
        if ( const auto scene = m_Scene.lock() )
            scene->UnregisterExternalPass( "CubemapPreview" );
    }

    Common::BoolResultStr EditorCubemapPreviewPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "CubemapSphere" );
        if ( !shader )
            return Common::MakeError( "EditorCubemapPreviewPass: missing shader 'CubemapSphere'" );

        Graphic::GraphicsPipelineSpecification spec;
        spec.DebugName   = "EditorCubemapPreviewPipeline";
        spec.Shader      = shader;
        spec.Framebuffer = scene->GetTargetFramebuffer();
        // A real object, not an overlay: the ball writes its own ray-traced depth so the scene's
        // backdrop stays behind it whichever order the graph runs the sky, and tests against what the
        // geometry pass left so a mesh in the same scene would still occlude it correctly.
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = true;
        spec.DepthCompareOp    = Graphic::DepthCompare::Closer;
        spec.CullMode          = Graphic::CullMode::None;
        spec.BlendEnable       = false; // radiance, opaque; the post chain tonemaps it like any sky

        const auto pipeline = Graphic::GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError( "EditorCubemapPreviewPass: " + pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_Material = std::make_unique<Graphic::MaterialCubemapSphere>();

        Graphic::ExternalPassSpecification pass;
        pass.Name                  = "CubemapPreview";
        pass.Phase                 = Graphic::RenderPhase::Debug;
        pass.Dependencies          = { Graphic::RenderPassDependency( Graphic::RenderPhase::Geometry ) };
        pass.PipelineSpecification = m_Pipeline->GetSpecification();
        pass.Execute               = [this]( const Graphic::ExternalPassContext& ctx )
        {
            if ( !ctx.Camera || !m_ResolveCube )
                return;

            // Resolved EVERY frame on purpose — the pass owns no copy of the material's state, so a
            // cubemap dropped onto (or cleared from) the subject shows next frame with no
            // invalidation protocol. The closure is two map lookups; see the header.
            const Graphic::ImageCube* cube = m_ResolveCube();
            if ( !cube )
                return;

            m_Material->Update( ctx.Camera, cube, m_Radius );
            Graphic::Renderer::GetInstance().SubmitFullscreenQuad( m_Pipeline.get(),
                                                                   m_Material->GetMaterialExecutor() );
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
