#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
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
        // One raster node: the graph opens the render pass on @p targets (colour slot i with @p colors[i] -
        // FrameTextures::ColorLoads, so a slot with its own clear clears on its first writer - the depth with
        // @p depth, all stored), @p sampled are read by its fragment shaders, and @p body records the draws
        // with this node's PassContext (graph textures bound by name through RDG::PassBindings); its error is the
        // node's. NeverCull: the bodies also write per-frame material state later passes of the frame rely on.
        void AddRaster( RDG::Builder& graph, std::string_view name, const RasterTargets& targets,
                        const std::vector<RDG::LoadOp>& colors, const RDG::LoadOp& depth,
                        const std::vector<RDG::TextureRef>&                             sampled,
                        std::function<Common::BoolResultStr( const RDG::PassContext& )> body,
                        const std::function<void( RDG::PassBuilder& )>&                 declareBindings = {} )
        {
            graph.AddPass(
                 name, RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
                 [&]( RDG::PassBuilder& pass )
                 {
                     for ( const RDG::TextureRef read : sampled )
                         pass.Read( read, RDG::Access::SampledGraphics );
                     // An invalid colour is an unused slot (FrameTextures::Colors): no target, so the backend's
                     // render pass references it as VK_ATTACHMENT_UNUSED (CreateRdgRenderPass).
                     for ( uint32_t slot = 0; slot < targets.Colors.size(); ++slot )
                         if ( targets.Colors[slot].IsValid() )
                             pass.ColorTarget( slot, targets.Colors[slot], colors[slot] );
                     if ( targets.Depth.IsValid() )
                         pass.DepthTarget( targets.Depth, depth );
                     DeclareResolves( pass, targets.Resolves );
                     if ( declareBindings )
                     {
                         declareBindings( pass );
                     }
                 },
                 [body = std::move( body )]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return body( context ); } );
        }
    } // namespace

    void SceneRenderer::AddGraphPhasePasses( RDG::Builder& graph, FrameTextures&            textures,
                                             bool ( *selects )( RenderPhaseID ), const bool clearFirst,
                                             const OverlayTargets& overlay )
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
        for ( const SystemRasterPass& pass : m_RenderGraphBuilder.GetSortedPasses() )
        {
            if ( !pass.CachedRenderPass || !selects( pass.Phase ) )
                continue;
            const auto&                         spec   = pass.CachedRenderPass->GetSpecification();
            const std::shared_ptr<Framebuffer>& target = spec.TargetFramebuffer;
            const bool                          clears = clearFirst && target != previous;
            previous                                   = target;

            const glm::vec4   clearColor = spec.ClearColor.Color;
            const RDG::LoadOp color =
                 clears ? RDG::LoadOp::ClearColor( clearColor.r, clearColor.g, clearColor.b, clearColor.a )
                        : RDG::LoadOp::Load();
            const RDG::LoadOp depth =
                 clears ? RDG::LoadOp::ClearDepth( spec.ClearColor.DepthStencil.x ) : RDG::LoadOp::Load();
            AddPassNode( graph, textures, pass, target, spec.DebugName, color, depth, overlay );
        }
    }

    void SceneRenderer::AddSystemRaster( RDG::Builder& graph, FrameTextures& textures,
                                         const SystemRasterPass& pass )
    {
        // A system whose pipeline failed to build hands back a pass with no target (it logged why at Initialize).
        if ( !pass.TargetFramebuffer || !pass.ExecuteFunc )
            return;
        AddPassNode( graph, textures, pass, pass.TargetFramebuffer, pass.Name, RDG::LoadOp::Load(),
                     RDG::LoadOp::Load(), {} );
    }

    void SceneRenderer::AddFrameTranslucency( RDG::Builder& graph, FrameTextures& textures )
    {
        // The translucency of the frame, in draw order - the order of these calls IS the order, there is no
        // numeric placement. The height fog apply first: it modifies the OPAQUE scene itself (every pixel gains
        // the fog between it and the camera), so everything composited after lands over the fogged world. Then
        // the far field, the cloud composite: everything after it is nearer the camera and paints over it.
        // Then the Transparency phase's registered passes (particles), then the passes registered at the
        // AfterTranslucency extension point (the editor grid). All on the scene target: the graph merges them into
        // one render pass.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        AddSystemRaster(
             graph, textures,
             UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )->ApplyPass() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        AddSystemRaster( graph, textures,
                         UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
                              ->CompositePass() );
        AddGraphPhasePasses(
             graph, textures, []( RenderPhaseID phase ) { return phase == RenderPhase::Transparency; }, false );
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::AfterTranslucency, {} );
    }

    void SceneRenderer::AddExtensionPoint( RDG::Builder& graph, FrameTextures& textures,
                                           const RDG::ExtensionPoint point, const OverlayTargets& overlay )
    {
        // Outside a BeginScene/EndScene bracket there is no scene, so no extension passes; without a scene target
        // there is nothing for them to draw over.
        if ( !m_FrameExtensions || !m_TargetFramebuffer )
            return;
        m_FrameExtensions->ForEachAt(
             point,
             [&]( const ExtensionPass& extension )
             {
                 // The context is made when the graph calls the pass (Declare while it is built, Execute when it
                 // runs), from this renderer's state then: the camera, the target a resize may have recreated.
                 const auto context = [this]( const FrameGraphRefs& refs )
                 {
                     ExtensionPassContext ctx;
                     ctx.Camera       = GetMainCamera();
                     ctx.Target       = m_TargetFramebuffer.get();
                     ctx.Depth        = m_TargetFramebuffer && m_TargetFramebuffer->GetDepthAttachmentCount() > 0
                                             ? m_TargetFramebuffer->GetDepthAttachmentImage().get()
                                             : nullptr;
                     ctx.ScenePlaying = IsScenePlaying();
                     ctx.Renderer     = this;
                     ctx.Graph        = refs;
                     return ctx;
                 };
                 // The pass's functions are COPIED into the node: the scene may replace the registration while the
                 // graph built this frame still holds the node.
                 SystemRasterPass node;
                 node.Name              = extension.Name;
                 node.TargetFramebuffer = m_TargetFramebuffer;
                 node.ExecuteFunc =
                      [context, execute = extension.Execute]( RDG::PassContext& pass, const FrameGraphRefs& refs )
                 { return execute( context( refs ), pass ); };
                 if ( extension.Declare )
                     node.Declare = [context, declare = extension.Declare]( RenderPassDeclaration& declared,
                                                                            const FrameGraphRefs&  refs )
                     { declare( declared, context( refs ) ); };
                 AddPassNode( graph, textures, node, m_TargetFramebuffer, node.Name, RDG::LoadOp::Load(),
                              RDG::LoadOp::Load(), overlay );
             } );
    }

    void SceneRenderer::AddPassNode( RDG::Builder& graph, FrameTextures& textures,
                                     const SystemRasterPass& pass,
                                     const std::shared_ptr<Framebuffer>& target, const std::string& debugName,
                                     const RDG::LoadOp& color, const RDG::LoadOp& depth,
                                     const OverlayTargets& overlay )
    {
        auto targets = TargetsOf( textures, target, debugName, pass.Name );
        if ( !targets )
            return;
        // After the temporal resolve the scene target is the RENDER-extent pre-resolve scene: the overlay
        // draws into the OUTPUT-extent overlay set instead - every attachment replaced, so the render pass has
        // one extent (colour 0 the resolved colour, the velocity slot the overlay velocity, the depth the one
        // PopulateSceneDepth filled; one sample, so no resolves).
        if ( overlay.IsValid() && target == m_TargetFramebuffer )
        {
            if ( targets->Colors.size() != kSceneTargetVelocitySlot + 1 )
            {
                LOG_ERROR( "[SceneRenderer] pass '{}' refused: the scene target has {} colours, the overlay set "
                           "replaces exactly colour 0 and the velocity slot",
                           pass.Name, targets->Colors.size() );
                return;
            }
            targets->Colors[0]                        = overlay.Color;
            targets->Colors[kSceneTargetVelocitySlot] = overlay.Velocity;
            targets->Depth                            = overlay.Depth;
            targets->Resolves                         = {};
        }
        RenderPassDeclaration declared;
        if ( pass.Declare )
            pass.Declare( declared, textures.GraphRefs() );
        std::vector<RDG::TextureRef> images;
        if ( !ResolveDeclared( textures, declared, pass.Name, images ) )
            return;

        const std::vector<RDG::LoadOp> colors = textures.ColorLoads( *targets, color );
        graph.AddPass(
             pass.Name, RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& node )
             {
                 DeclareOn( node, images, declared );
                 for ( uint32_t slot = 0; slot < targets->Colors.size(); ++slot )
                     node.ColorTarget( slot, targets->Colors[slot], colors[slot] );
                 if ( targets->Depth.IsValid() )
                     node.DepthTarget( targets->Depth, depth );
                 DeclareResolves( node, targets->Resolves );
             },
             [execute = pass.ExecuteFunc, refs = textures.GraphRefs()](
                  RDG::PassContext& context ) -> Common::BoolResultStr { return execute( context, refs ); } );
    }

    void SceneRenderer::AddFrameGBuffer( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Deferred: GBuffer" );
        if ( !targets || meshRenderer == nullptr )
            return;
        // ZERO, not the default 0.1 grey: empty texels need a zero normal, the lighting pass tells geometry from
        // sky by dot(normal, normal).
        // The shading word target (slot 2, R32_UINT) takes the same clear: the bits of 0.0f are uint 0, which is
        // the contract's clear value (Unlit, receives sun shadows).
        static_assert( Core::ShadingModels::kShadingWordClearValue == 0u,
                       "the G-buffer clear writes 0.0f bits into the uint shading word" );
        AddRaster(
             graph, "Deferred: GBuffer", *targets,
             textures.ColorLoads( *targets, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ),
             RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGBufferManual( context ); },
             [meshRenderer]( RDG::PassBuilder& pass ) { meshRenderer->DeclareGBufferDraws( pass ); } );
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
        auto* terrain = UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] );
        AddRaster(
             graph, "TerrainGBuffer", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [terrain]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return terrain->RenderGBufferManual( context ); },
             [terrain]( RDG::PassBuilder& pass ) { terrain->DeclareGBufferDraws( pass ); } );
        // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
    }

    void SceneRenderer::AddFrameRSM( RDG::Builder& graph, FrameTextures& textures,
                                     System::MeshRenderer* meshRenderer, const glm::vec3& sunDir )
    {
        if ( glm::distance( sunDir, m_RSMLastSunDir ) > 1e-4f || m_RSMFrameCounter == 0 )
        {
            const auto targets = TargetsOf( textures, m_RSMBuffer, "RSM", "Deferred: RSM" );
            if ( !targets || meshRenderer == nullptr )
                return;
            // Colour 0 = "no caster here" for the VPL gather. Standard-Z pass (drawn through a cascade matrix), so
            // depth clears to 1 = far, not to the engine's reversed-Z clear.
            AddRaster(
                 graph, "Deferred: RSM", *targets,
                 textures.ColorLoads( *targets, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ),
                 RDG::LoadOp::ClearDepth( 1.0f ), {},
                 [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
                 { return meshRenderer->RenderRSMManual( context ); },
                 [meshRenderer]( RDG::PassBuilder& pass ) { meshRenderer->DeclareRSMDraws( pass ); } );
            m_RSMLastSunDir = sunDir;
        }
    }

    void SceneRenderer::AddFrameGeneric( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Generic" );
        if ( !targets || meshRenderer == nullptr )
            return;
        // The frame's draw list, built in the setup with one binding block per material (the scene/view inputs -
        // cascades, environment cubes, BRDF LUT, cloud shadow map - bound where its shader has slots for them).
        const SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        AddRaster(
             graph, "Deferred: Generic", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGenericManual( context ); },
             [meshRenderer, view]( RDG::PassBuilder& pass ) { meshRenderer->DeclareGenericDraws( pass, view ); } );
    }

    void SceneRenderer::AddFrameSkinned( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Skinned" );
        if ( !targets || meshRenderer == nullptr )
            return;
        // The frame's draw list, built in the setup with one binding block per material (the scene/view inputs -
        // cascades, environment cubes, BRDF LUT, cloud shadow map - bound where its shader has slots for them).
        const SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        AddRaster(
             graph, "Deferred: Skinned", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderSkinnedManual( context ); },
             [meshRenderer, view]( RDG::PassBuilder& pass ) { meshRenderer->DeclareSkinnedDraws( pass, view ); } );
    }

    void SceneRenderer::AddFrameGlass( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef sceneCopy,
                                       System::MeshRenderer* meshRenderer )
    {
        if ( !sceneCopy.IsValid() || meshRenderer == nullptr )
            return;
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Glass" );
        if ( !targets )
            return;
        // The glass samples the scene copy for its refraction and the scene/view inputs its shader has slots for:
        // its binding block, declared here (the block entries are the node's reads), resolved in the body.
        const SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        AddRaster(
             graph, "Deferred: Glass", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGlassManual( context ); },
             [meshRenderer, sceneCopy, view]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareGlassBindings( pass, sceneCopy, view ); } );
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameOverdraw( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( meshRenderer == nullptr )
            return;
        const auto accum =
             TargetsOf( textures, meshRenderer->GetOverdrawFramebuffer(), "Overdraw", "Debug: Overdraw" );
        const auto scene = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Debug: Overdraw Resolve" );
        if ( !accum || !scene || accum->Colors.empty() )
            return;
        AddRaster(
             graph, "Debug: Overdraw", *accum,
             textures.ColorLoads( *accum, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ),
             RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderOverdrawAccumManual( context ); },
             [meshRenderer]( RDG::PassBuilder& pass ) { meshRenderer->DeclareOverdrawDraws( pass ); } );
        // The accumulation is read through the resolve's block (its u_Overdraw entry is the node's read).
        AddRaster(
             graph, "Debug: Overdraw Resolve", *scene, textures.ColorLoads( *scene, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RecordOverdrawResolve( context ); },
             [meshRenderer, overdraw = accum->Colors[0]]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareOverdrawResolve( pass, overdraw ); } );
    }
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Graphic
