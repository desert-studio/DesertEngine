#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RenderPhaseRegistry.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <mutex>
#include <Common/Core/Units.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <format>
#include <cmath>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <Engine/Graphic/SceneRendererFrame.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // The whole of a framebuffer as graph names: every colour attachment by slot, and the depth.
        struct RasterTargets
        {
            std::vector<RDG::TextureRef> Colors;
            RDG::TextureRef              Depth;
            std::vector<RDG::TextureRef> Resolves; // multisampled framebuffer: what each colour resolves into
        };

        // A target the graph cannot declare whole is refused with its pass, never half-declared: a render pass
        // missing an attachment is not the one the pass's pipelines were built against (an image the graph cannot
        // import). A multisampled framebuffer is declared as its engine render pass is: the multisampled colours
        // and depth, plus the single-sample image each colour resolves into.
        std::optional<RasterTargets> TargetsOf( FrameTextures&                      textures,
                                                const std::shared_ptr<Framebuffer>& framebuffer,
                                                std::string_view name, std::string_view pass )
        {
            if ( !framebuffer )
                return std::nullopt;
            const uint32_t samples      = framebuffer->GetSpecification().Samples;
            const bool     multisampled = samples > 1;
            RasterTargets  targets;
            targets.Colors = multisampled ? textures.MultisampleColors( framebuffer, name )
                                          : textures.Colors( framebuffer, name );
            targets.Depth  = textures.Depth( framebuffer, name );
            if ( multisampled )
                targets.Resolves = textures.Colors( framebuffer, name );
            const uint32_t colours  = framebuffer->GetColorAttachmentCount();
            const bool     hasDepth = framebuffer->GetDepthAttachmentCount() != 0;
            if ( targets.Colors.size() != colours || hasDepth != targets.Depth.IsValid() ||
                 targets.Resolves.size() != ( multisampled ? colours : 0u ) )
            {
                LOG_ERROR( "[SceneRenderer] '{}' is not recorded: the graph can declare {} of the {} colour "
                           "attachment(s) of '{}' ({} sample(s), {} resolve(s)), depth {}",
                           pass, targets.Colors.size(), colours, name, samples, targets.Resolves.size(),
                           targets.Depth.IsValid() ? "declared" : ( hasDepth ? "NOT declared" : "absent" ) );
                return std::nullopt;
            }
            return targets;
        }

        // One raster node: the graph opens the render pass on @p targets (every colour with @p color, the depth
        // with @p depth, all stored), @p sampled are read by its fragment shaders, and @p body records the draws.
        // NeverCull: the bodies also write per-frame material state later passes of the frame rely on.
        void AddRaster( RDG::Builder& graph, std::string_view name, const RasterTargets& targets,
                        const RDG::LoadOp& color, const RDG::LoadOp& depth,
                        const std::vector<RDG::TextureRef>& sampled, std::function<void()> body )
        {
            graph.AddPass(
                 name, RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
                 [&]( RDG::PassBuilder& pass )
                 {
                     for ( const RDG::TextureRef read : sampled )
                         pass.Read( read, RDG::Access::SampledGraphics );
                     for ( uint32_t slot = 0; slot < targets.Colors.size(); ++slot )
                         pass.ColorTarget( slot, targets.Colors[slot], color );
                     if ( targets.Depth.IsValid() )
                         pass.DepthTarget( targets.Depth, depth );
                     for ( uint32_t slot = 0; slot < targets.Resolves.size(); ++slot )
                         pass.ResolveTarget( slot, targets.Resolves[slot] );
                 },
                 [body = std::move( body )]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     body();
                     return BOOLSUCCESS;
                 } );
        }

        // The shadow images a lit forward pass samples (SceneRenderer::DeclareShadowReads), as graph textures.
        // When one cannot be imported the error is logged and the pass declares none of them.
        std::vector<RDG::TextureRef> ShadowSamples( const SceneRenderer& renderer, FrameTextures& textures,
                                                    std::string_view node )
        {
            RenderPassDeclaration declared;
            renderer.DeclareShadowReads( declared );
            std::vector<RDG::TextureRef> images;
            if ( !ResolveDeclared( textures, declared, node, images ) )
                images.clear();
            return images;
        }
    } // namespace

    void SceneRenderer::AddGraphPhasePasses( RDG::Builder& graph, FrameTextures&            textures,
                                             bool ( *selects )( RenderPhaseID ), const bool clearFirst )
    {
        // Every registered pass of the selected phases (engine systems and the editor's external passes), in the
        // phase graph's order, is a raster node: its targets are its framebuffer WHOLE (TargetsOf: every colour,
        // the depth, and at MSAA the resolve images), its reads are what its PassConfig::Declare names. The GRAPH
        // opens the render pass and merges consecutive nodes on one framebuffer into one. The first node on a
        // framebuffer CLEARS it on the main walk (@p clearFirst: the skybox draws first and the geometry over it,
        // neither clearing the other) with the pass's own clear values (a cascade clears its depth to 1); every
        // other node LOADS, and the overlay phases only LOAD, so a CLEAR never wipes the depth a later overlay
        // tests against. A pass whose target or reads the graph cannot declare is refused with its error, never
        // half-declared. NeverCull: a pass body (the editor's external passes too) may change state outside
        // the graph - per-frame material state, picking - which no declaration shows.
        std::shared_ptr<Framebuffer> previous;
        for ( const RenderGraphBuilder::PassConfig& pass : m_RenderGraphBuilder.GetSortedPasses() )
        {
            if ( !pass.CachedRenderPass || !selects( pass.Phase ) )
                continue;
            const auto&                         spec   = pass.CachedRenderPass->GetSpecification();
            const std::shared_ptr<Framebuffer>& target = spec.TargetFramebuffer;
            const bool                          clears = clearFirst && target != previous;
            previous                                   = target;

            const auto targets = TargetsOf( textures, target, spec.DebugName, pass.Name );
            if ( !targets )
                continue;
            RenderPassDeclaration declared;
            if ( pass.Declare )
                pass.Declare( declared );
            std::vector<RDG::TextureRef> images;
            if ( !ResolveDeclared( textures, declared, pass.Name, images ) )
                continue;

            const glm::vec4   clearColor = spec.ClearColor.Color;
            const RDG::LoadOp color =
                 clears ? RDG::LoadOp::ClearColor( clearColor.r, clearColor.g, clearColor.b, clearColor.a )
                        : RDG::LoadOp::Load();
            const RDG::LoadOp depth =
                 clears ? RDG::LoadOp::ClearDepth( spec.ClearColor.DepthStencil.x ) : RDG::LoadOp::Load();
            graph.AddPass(
                 pass.Name, RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
                 [&]( RDG::PassBuilder& node )
                 {
                     DeclareOn( node, images, declared );
                     for ( uint32_t slot = 0; slot < targets->Colors.size(); ++slot )
                         node.ColorTarget( slot, targets->Colors[slot], color );
                     if ( targets->Depth.IsValid() )
                         node.DepthTarget( targets->Depth, depth );
                     for ( uint32_t slot = 0; slot < targets->Resolves.size(); ++slot )
                         node.ResolveTarget( slot, targets->Resolves[slot] );
                 },
                 [execute = pass.ExecuteFunc]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     execute();
                     return BOOLSUCCESS;
                 } );
        }
    }

    void SceneRenderer::AddFrameGBuffer( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Deferred: GBuffer" );
        if ( !targets || !meshRenderer )
            return;
        // ZERO, not the default 0.1 grey: empty texels need a zero normal, the lighting pass tells geometry from
        // sky by dot(normal, normal).
        AddRaster( graph, "Deferred: GBuffer", *targets, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ),
                   RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
                   [meshRenderer]() { meshRenderer->RenderGBufferManual(); } );
    }

    void SceneRenderer::AddFrameTerrainGBuffer( RDG::Builder& graph, FrameTextures& textures )
    {
        // Its own row, so the ground's G-buffer cost reads as a pass line (the forward path's is the
        // graph's "TerrainPass"). LOAD: the meshes' G-buffer node cleared it; the graph merges the two into
        // one render pass.
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "TerrainGBuffer" );
        if ( !targets )
            return;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        AddRaster(
             graph, "TerrainGBuffer", *targets, RDG::LoadOp::Load(), RDG::LoadOp::Load(), {},
             [this]() {
                 UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->RenderGBufferManual();
             } );
        // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
    }

    void SceneRenderer::AddFrameRSM( RDG::Builder& graph, FrameTextures& textures,
                                     System::MeshRenderer* meshRenderer, const glm::vec3& sunDir )
    {
        if ( glm::distance( sunDir, m_RSMLastSunDir ) > 1e-4f || m_RSMFrameCounter == 0 )
        {
            const auto targets = TargetsOf( textures, m_RSMBuffer, "RSM", "Deferred: RSM" );
            if ( !targets || !meshRenderer )
                return;
            // Colour 0 = "no caster here" for the VPL gather. Standard-Z pass (drawn through a cascade matrix), so
            // depth clears to 1 = far, not to the engine's reversed-Z clear.
            AddRaster( graph, "Deferred: RSM", *targets, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ),
                       RDG::LoadOp::ClearDepth( 1.0f ), {},
                       [meshRenderer]() { meshRenderer->RenderRSMManual(); } );
            m_RSMLastSunDir = sunDir;
        }
    }

    void SceneRenderer::AddFrameGeneric( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Generic" );
        if ( !targets || !meshRenderer )
            return;
        AddRaster( graph, "Deferred: Generic", *targets, RDG::LoadOp::Load(), RDG::LoadOp::Load(),
                   ShadowSamples( *this, textures, "Deferred: Generic" ),
                   [meshRenderer]() { meshRenderer->RenderGenericManual(); } );
    }

    void SceneRenderer::AddFrameSkinned( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Skinned" );
        if ( !targets || !meshRenderer )
            return;
        AddRaster( graph, "Deferred: Skinned", *targets, RDG::LoadOp::Load(), RDG::LoadOp::Load(),
                   ShadowSamples( *this, textures, "Deferred: Skinned" ),
                   [meshRenderer]() { meshRenderer->RenderSkinnedManual(); } );
    }

    void SceneRenderer::AddFrameGlass( RDG::Builder& graph, FrameTextures& textures,
                                       const std::vector<RDG::TextureRef>& copyReads,
                                       System::MeshRenderer*               meshRenderer,
                                       const std::shared_ptr<FrameValues>& values )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Glass" );
        if ( !targets || !meshRenderer )
            return;
        // The glass samples the scene copy for its refraction, and the shadow maps as every lit forward pass does.
        std::vector<RDG::TextureRef> sampled = ShadowSamples( *this, textures, "Deferred: Glass" );
        sampled.insert( sampled.end(), copyReads.begin(), copyReads.end() );
        AddRaster( graph, "Deferred: Glass", *targets, RDG::LoadOp::Load(), RDG::LoadOp::Load(), sampled,
                   [meshRenderer, values]() { meshRenderer->RenderGlassManual( values->SceneCopy ); } );
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameOverdraw( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( !meshRenderer )
            return;
        const auto accum =
             TargetsOf( textures, meshRenderer->GetOverdrawFramebuffer(), "Overdraw", "Debug: Overdraw" );
        const auto scene = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Debug: Overdraw Resolve" );
        if ( !accum || !scene )
            return;
        AddRaster( graph, "Debug: Overdraw", *accum, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ),
                   RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
                   [meshRenderer]() { meshRenderer->RenderOverdrawAccumManual(); } );
        AddRaster( graph, "Debug: Overdraw Resolve", *scene, RDG::LoadOp::Load(), RDG::LoadOp::Load(),
                   accum->Colors, [meshRenderer]() { meshRenderer->RenderOverdrawResolveManual(); } );
    }
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Graphic
