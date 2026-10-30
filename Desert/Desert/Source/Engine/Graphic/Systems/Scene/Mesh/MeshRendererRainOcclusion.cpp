// The view's rain occlusion (Graphic/RainOcclusion.hpp): the top-down depth the wetted surfaces read.

#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/RainOcclusion.hpp>

#include <glm/gtc/constants.hpp>

#include <format>
#include <optional>

namespace Desert::Graphic::System
{
    void MeshRenderer::SetRainOcclusionQuality( const RainOcclusionQuality& quality )
    {
        if ( quality == m_RainOcclusionQuality )
            return;
        m_RainOcclusionQuality = quality;
        m_RainOcclusionBox.reset();
        m_RainOcclusionSignature.reset();
        if ( quality.MapSize == 0 )
        {
            m_RainOcclusionFB.reset();
            return;
        }
        // The cascades' attachment format, so the caster pipelines (made on m_CascadeStaticFB[0], and the
        // terrain's on the cascade target) are render-pass compatible with it.
        FramebufferSpecification spec;
        spec.DebugName = "RainOcclusion";
        spec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowDepth );
        m_RainOcclusionFB = Graphic::Framebuffer::Create( spec );
        m_RainOcclusionFB->Resize( quality.MapSize, quality.MapSize );
    }

    void MeshRenderer::UpdateRainOcclusion()
    {
        m_RainOcclusionRedraw = false;
        const auto* camera    = m_SceneRenderer->GetMainCamera();
        // No map (budgeted off), no caster pipeline (shadows budgeted off: SetupShadowPass made none) or no
        // camera: no box, so the receivers' block says "invalid" and every surface is open to the sky.
        if ( !m_RainOcclusionFB || !m_ShadowPipeline || camera == nullptr ||
             !EnsureShadowViewMaterials( kRainOcclusionView ) )
        {
            m_RainOcclusionBox.reset();
            return;
        }

        const glm::vec3 eye = camera->GetPosition();
        if ( !m_RainOcclusionBox ||
             RainOcclusionNeedsRecentre( *m_RainOcclusionBox, eye, m_RainOcclusionQuality ) )
        {
            m_RainOcclusionBox    = FitRainOcclusionBox( eye, m_RainOcclusionQuality );
            m_RainOcclusionRedraw = true;
        }
        // The casters inside the box (the local lights' signature, over the sphere around the box): a caster
        // added, removed or moved redraws the map. A caster that moves by itself (a posed mesh, swaying foliage)
        // answers no signature; the map is not redrawn every frame for it — rain does not follow a swaying leaf.
        const float radius = 0.5f * m_RainOcclusionQuality.Extent * glm::root_three<float>();
        if ( const std::optional<uint64_t> signature = LocalCasterSignature( m_RainOcclusionBox->Centre, radius );
             signature && signature != m_RainOcclusionSignature )
        {
            m_RainOcclusionSignature = signature;
            m_RainOcclusionRedraw    = true;
        }
        m_CascadeCasterVP[kRainOcclusionView]   = m_RainOcclusionBox->ViewProj;
        m_CascadeCasterBias[kRainOcclusionView] = CascadeCasterBias{};
    }

    std::optional<SystemRasterPass> MeshRenderer::RainOcclusionPass()
    {
        if ( !m_RainOcclusionRedraw || !m_RainOcclusionBox )
            return std::nullopt;
        constexpr uint32_t view = kRainOcclusionView;
        SystemRasterPass   pass{
               .Name        = "Weather: RainOcclusion",
               .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 if ( auto drawn = m_RainOcclusionDraws.Record( context ); !drawn.IsSuccess() )
                     return drawn;
                 // The terrain (IShadowCaster), into the same pass so the map holds everyone's top.
                 for ( const auto& [weak, firstBlock] : m_RainOcclusionCasters )
                 {
                     if ( const auto caster = weak.lock() )
                     {
                         if ( auto cast = caster->RecordShadowView(
                                   context, view, firstBlock, m_CascadeCasterVP[view], m_CascadeCasterBias[view] );
                              !cast.IsSuccess() )
                             return cast;
                     }
                 }
                 return BOOLSUCCESS;
             },
               .TargetFramebuffer = m_RainOcclusionFB,
               .ClearDepth        = 1.0f };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            m_RainOcclusionDraws.Clear();
            m_RainOcclusionCasters.clear();
            BuildShadowCascadeDraws( view, m_RainOcclusionDraws );
            m_RainOcclusionDraws.Declare( declared, std::nullopt );
            for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                declared.Read( streams, RDG::Access::StorageRead );
            uint32_t nextBlock = m_RainOcclusionDraws.BlockCount();
            for ( const auto& weak : m_ShadowCasters )
            {
                if ( const auto caster = weak.lock(); caster && caster->CastsShadowThisFrame() )
                {
                    const uint32_t declaredBlocks = caster->DeclareShadowView( declared, view );
                    m_RainOcclusionCasters.emplace_back( weak, nextBlock );
                    nextBlock += declaredBlocks;
                }
            }
        };
        return pass;
    }
} // namespace Desert::Graphic::System
