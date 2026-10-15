#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>
#include <memory>

namespace Desert::Graphic
{
    class MaterialCompositeEditorPrimitives final : public Material
    {
    public:
        MaterialCompositeEditorPrimitives()
             : Material( "MaterialCompositeEditorPrimitives", "CompositeEditorPrimitives" )
        {
        }

        // The view's Film Gamma (the output device the tonemap encodes for) and UE's occluded-primitive look.
        void BindValues( const float displayGamma )
        {
            SetDisplayGamma( displayGamma );
            UploadRegisteredProperties();
            // Every per-frame-in-flight copy of the UB receives the value (see MaterialTonemap::BindValues).
            FlushFieldFilledUniformBuffers();
        }

        MPROPERTY( float, DisplayGamma, "u_DisplayGamma", 2.2f )
        // UE FSceneView::FPostProcessCompositePrimitiveParams defaults (SceneView.h): the checker size in pixels,
        // its strength and the brightness of a primitive behind the scene. Constants in UE's editor too.
        MPROPERTY( float, OccludedDitheringPatternSize, "u_OccludedDitheringPatternSize", 2.0f )
        MPROPERTY( float, OccludedDithering, "u_OccludedDithering", 0.8f )
        MPROPERTY( float, OccludedBrightness, "u_OccludedBrightness", 0.7f )
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // "Editor: CompositeEditorPrimitives" (UE PostProcessCompositeEditorPrimitives.cpp AddEditorPrimitivePass, the
    // composite half): after the tonemap and the spatial AA, the view's editor-primitives target - the passes at
    // RDG::ExtensionPoint::EditorPrimitives and the engine's debug lines, drawn multisampled in display-linear
    // colour against the populated scene depth - is resolved, encoded for the display and blended over the display
    // image (CompositeEditorPrimitives.shader). Its target is the display image (kTonemap; FXAA and SMAA write the
    // same format), LOADED: the fullscreen triangle blends (One, OneMinusSrcAlpha) and discards where nothing was
    // drawn.
    //
    // Not a RenderSystem: no pass of its own and no framebuffer; the SceneRenderer owns one and the graph owns the
    // targets. The pipeline is made on first use (Prepare), an error names why it could not be.
    class EditorPrimitivesCompositeRenderer final
    {
    public:
        static_assert( ViewTargetFormats::kFXAA == ViewTargetFormats::kTonemap &&
                            ViewTargetFormats::kSMAABlend == ViewTargetFormats::kTonemap,
                       "the composite's one pipeline draws onto whichever of the three is the display image" );

        [[nodiscard]] Common::BoolResultStr Prepare()
        {
            if ( m_Pipeline )
                return BOOLSUCCESS;
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "CompositeEditorPrimitives" );
            if ( !m_Shader )
                return Common::MakeError( "CompositeEditorPrimitives shader not found" );
            GraphicsPipelineSpecification spec;
            spec.DebugName    = "CompositeEditorPrimitives";
            spec.TargetLayout = RenderTargetLayout{
                 .ColorFormats = { ViewTargetFormats::kTonemap }, .DepthFormat = std::nullopt, .Samples = 1 };
            spec.Shader              = m_Shader;
            spec.DepthTestEnabled    = false;
            spec.DepthWriteEnabled   = false;
            spec.CullMode            = CullMode::None;
            spec.BlendEnable         = true;
            spec.SrcColorBlendFactor = BlendFactor::One; // the shader's colour is premultiplied
            spec.DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
            const auto pipeline      = GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError(
                     std::format( "CompositeEditorPrimitives pipeline: {}", pipeline.GetError() ) );
            m_Pipeline = pipeline.GetValue();
            m_Material = std::make_unique<MaterialCompositeEditorPrimitives>();
            return BOOLSUCCESS;
        }

        [[nodiscard]] bool IsReady() const
        {
            return m_Pipeline && m_Material;
        }

        // The values first (the block naming them is declared from the material's route fill), then the reads.
        void DeclareBindings( RDG::PassBuilder& pass, const RDG::TextureRef editorColor,
                              const RDG::TextureRef editorDepth, const RDG::TextureRef sceneDepth,
                              const float displayGamma ) const
        {
            m_Material->BindValues( displayGamma );
            pass.Bindings( m_BindingLayout.Get( m_Shader ), m_Material->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_EditorColor", editorColor, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_EditorDepth", editorDepth, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_SceneDepth", sceneDepth, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() );
        }

        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const
        {
            if ( !IsReady() )
                return Common::MakeError( "CompositeEditorPrimitives recorded without its pipeline" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        std::shared_ptr<Shader>                            m_Shader;
        mutable ShaderBindingLayoutCache                   m_BindingLayout;
        std::shared_ptr<GraphicsPipeline>                  m_Pipeline;
        std::unique_ptr<MaterialCompositeEditorPrimitives> m_Material;
    };
} // namespace Desert::Graphic::System
