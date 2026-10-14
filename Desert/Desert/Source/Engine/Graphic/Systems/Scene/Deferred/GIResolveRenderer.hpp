#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/ScreenSpaceDenoiser.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <string_view>

namespace Desert::Graphic::System
{
    // The graph textures the GI gather reads, bound by shader name (RecordGather).
    struct GIGatherInputs
    {
        RDG::TextureRef GBufferNormal; // GBufferB
        RDG::TextureRef GBufferDepth;  // the G-buffer depth: the pixel's world position is reconstructed from it
        RDG::TextureRef RSMAlbedo;     // RSM attachment 0 (flux colour)
        RDG::TextureRef RSMNormal;     // RSM attachment 1
        RDG::TextureRef RSMDepth;      // the RSM depth: the VPL world positions are reconstructed from it
    };

    // One-bounce RSM GI:
    //  1) GATHER: fullscreen jittered VPL gather into a per-frame graph transient (FrameTransients::GIResolve).
    //     The jitter seed changes EVERY frame.
    //  2) DENOISE: the screen-space denoiser's diffuse-indirect signal
    //  (ScreenSpaceDenoiser::DenoiseDiffuseIndirect)
    //     - bilateral reconstruction, reprojected history with variance rejection, history convolution. Close-up
    //     camera angles make the per-pixel VPL variance huge; only accumulation over frames converges it.
    // The deferred lighting reads the denoised result. The target framebuffer is the scene target: only its size
    // is read.
    class GIResolveRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "GIResolve" );
            if ( !m_Shader )
                return Common::MakeError( "GIResolve shader not found" );

            GraphicsPipelineSpecification spec;
            spec.DebugName         = "GIResolve";
            // The gather is a graph transient: built against the graph's canonical pass for that one target.
            spec.TargetLayout      = RenderTargetLayout{ .ColorFormats = { ViewTargetFormats::kGIResolve },
                                                         .DepthFormat  = std::nullopt };
            spec.Shader            = m_Shader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();

            m_Material = std::make_unique<MaterialGIResolve>();
            return m_Denoiser.Initialize();
        }

        // The denoiser's history is a reprojection of the previous frame, and the previous frame is now a
        // different world — see IRenderSystem::OnSceneReplaced, kind 1.
        void OnSceneReplaced() override
        {
            m_Denoiser.InvalidateHistory();
        }

        // Camera cut — see IRenderSystem::OnTemporalHistoryReset. The frame index is the jitter/noise seed, so
        // it restarts with the history rather than carrying the count of whatever frames came before the cut.
        void OnTemporalHistoryReset() override
        {
            m_FrameIndex = 0;
            m_Denoiser.InvalidateHistory();
        }

        // Called while the frame graph is built. False: nothing to draw.
        [[nodiscard]] bool Prepare() const
        {
            return !m_TargetFramebuffer.expired() && m_Pipeline && m_Material;
        }

        // Pass 1, inside the render pass the graph opens on the gather transient (cleared to 0): jittered VPL
        // gather. Its textures are block 0, declared by DeclareGatherBindings. @p invJitteredViewProjection:
        // ViewFrame::InvJitteredViewProjection (the pixel's world position from the G-buffer depth).
        [[nodiscard]] Common::BoolResultStr RecordGather( const RDG::PassContext& context,
                                                          const glm::mat4&        rsmViewProj,
                                                          const glm::mat4&        invJitteredViewProjection,
                                                          const glm::vec4& sunColorIntensity, float giIntensity,
                                                          int samples )
        {
            // @p samples: VPL gather taps per pixel (GlobalIllumination.Samples, Scalability), a uniform like
            // every other cost knob so one pipeline serves every level.
            m_Material->BindInputs( rsmViewProj, invJitteredViewProjection, sunColorIntensity, giIntensity,
                                    static_cast<float>( m_FrameIndex % 1024u ), samples );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            ++m_FrameIndex;
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

        // SETUP of pass 1: the node's one block (block 0) - every texture of @p inputs with the sampler the
        // material route sampled them with (the image's own: linear, REPEAT), the material as the other route.
        void DeclareGatherBindings( RDG::PassBuilder& pass, const GIGatherInputs& inputs ) const
        {
            constexpr RDG::SamplerDesc kSampler = RDG::SamplerDesc::LinearRepeat();
            auto                       block    = pass.Bindings( m_GatherLayout.Get( m_Pipeline->GetShader() ),
                                                                 m_Material->GetMaterialExecutor()->GetRouteFill() );
            const auto                 sampled  = [&]( std::string_view name, RDG::TextureRef texture ) {
                block.Sampled( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                                                kSampler );
            };
            // The two depths through a nearest sampler: DEPTH32F is not guaranteed linear-filterable (MoltenVK).
            const auto depth = [&]( std::string_view name, RDG::TextureRef texture )
            {
                block.Sampled( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                               RDG::SamplerDesc::PointClamp() );
            };
            sampled( "u_GBufferB", inputs.GBufferNormal );
            depth( "u_GBufferDepth", inputs.GBufferDepth );
            sampled( "u_RSMAlbedo", inputs.RSMAlbedo );
            sampled( "u_RSMNormal", inputs.RSMNormal );
            depth( "u_RSMDepth", inputs.RSMDepth );
        }

        // Pass 2 (GRAPH BUILD): the screen-space denoiser over @p gather - see ScreenSpaceDenoiser. Returns the
        // denoised indirect light the lighting reads (a transient of @p graph); invalid: not initialised.
        [[nodiscard]] RDG::TextureRef Denoise( RDG::Builder& graph, const RDG::TextureRef gather,
                                               const DenoiserSceneTextures& scene, const ViewFrame& frame )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return {};
            return m_Denoiser.DenoiseDiffuseIndirect( graph, DiffuseIndirectDenoiserInputs{ .Signal = gather },
                                                      scene, frame, target->GetFramebufferWidth(),
                                                      target->GetFramebufferHeight() );
        }

    private:
        std::shared_ptr<Shader>             m_Shader;
        std::shared_ptr<GraphicsPipeline>   m_Pipeline;
        std::unique_ptr<MaterialGIResolve>  m_Material;
        // The gather's block layout, derived from the pipeline shader's reflection once per compile.
        mutable ShaderBindingLayoutCache    m_GatherLayout;
        ScreenSpaceDenoiser                 m_Denoiser{ DenoiserSignal::DiffuseIndirect };

        uint32_t         m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::System
