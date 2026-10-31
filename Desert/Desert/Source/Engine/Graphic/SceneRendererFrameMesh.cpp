#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryClusterView.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>
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
#include <bit>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <format>
#include <cmath>
#include <string>
#include <span>
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

    void SceneRenderer::AddSystemRasters( RDG::Builder& graph, FrameTextures& textures,
                                          const std::span<const SystemRasterPass> passes, const bool clearFirst,
                                          const OverlayTargets& overlay, const bool clearDepth )
    {
        // @p passes in the given order, each a raster node: its targets are its framebuffer WHOLE (TargetsOf:
        // every colour, the depth, and at MSAA the resolve images), its reads are what its Declare names. The
        // GRAPH opens the render pass and merges consecutive nodes on one framebuffer into one. With @p clearFirst
        // the first node on each framebuffer CLEARS it with the pass's own clear values (a cascade clears its
        // depth to 1) and the next ones on it LOAD (the sky clears the scene target, the geometry draws over it);
        // without it every node LOADS, so a CLEAR never wipes the depth a later overlay tests against. A pass
        // whose target or reads the graph cannot declare is refused with its error, never half-declared.
        // NeverCull: a pass body may change state outside the graph - per-frame material state - which no
        // declaration shows.
        const RenderPassSpecification defaults;
        std::shared_ptr<Framebuffer>  previous;
        for ( const SystemRasterPass& pass : passes )
        {
            // A system whose pipeline or target failed to build hands back a pass with no target (it logged why).
            if ( ( !pass.TargetFramebuffer && !pass.TargetDepthArray.IsValid() ) || !pass.ExecuteFunc )
                continue;
            const std::shared_ptr<Framebuffer>& target = pass.TargetFramebuffer;
            const bool                          clears = clearFirst && target != previous;
            previous                                   = target;

            const glm::vec4   clearColor = pass.ClearColor.value_or( defaults.ClearColor.Color );
            const RDG::LoadOp color =
                 clears ? RDG::LoadOp::ClearColor( clearColor.r, clearColor.g, clearColor.b, clearColor.a )
                        : RDG::LoadOp::Load();
            const RDG::LoadOp depth =
                 clears && clearDepth
                      ? RDG::LoadOp::ClearDepth( pass.ClearDepth.value_or( defaults.ClearColor.DepthStencil.x ) )
                      : RDG::LoadOp::Load();
            AddPassNode( graph, textures, pass, target, pass.Name, color, depth, overlay );
        }
    }

    void SceneRenderer::AddSystemRaster( RDG::Builder& graph, FrameTextures& textures,
                                         const SystemRasterPass& pass, const OverlayTargets& overlay )
    {
        AddSystemRasters( graph, textures, std::span<const SystemRasterPass>( &pass, 1 ), false, overlay );
    }

    void SceneRenderer::AddFrameShadowDepths( RDG::Builder& graph, FrameTextures& textures )
    {
        // The sun's cascades (MeshRenderer::ShadowCascadePasses): the static layers that went stale, each clearing
        // its cache (to 1: standard Z); each rebuilt map copied from its cache; the movable casters LOADING the
        // map over the copy. A cascade with none of the three keeps last frame's map.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ) )
        {
            // OnUpdate's frame decision: under "Shadows: Ray Traced Sun" the composite and the opaque programs
            // read the screen mask, and the cascades are drawn only for the blended programs that sample them.
            if ( m_FrameCascades )
                AddFrameSunCascades( graph, textures, *mesh );
            // The local lights' views LOAD the atlas: each clears and draws only its own tile, and a tile whose
            // light and casters stood still keeps last frame's depth (it has no pass at all).
            const std::vector<SystemRasterPass> local = mesh->LocalShadowPasses();
            AddSystemRasters( graph, textures, local, false );
            // The rain occlusion's top-down depth, when its box moved or its casters changed (it clears its own
            // target); otherwise the map keeps the depth it was drawn with.
            if ( const std::optional<SystemRasterPass> rain = mesh->RainOcclusionPass() )
                AddSystemRasters( graph, textures, std::span<const SystemRasterPass>( &*rain, 1 ), true );
        }
    }

    void SceneRenderer::AddFrameSunCascades( RDG::Builder& graph, FrameTextures& textures,
                                             System::MeshRenderer& mesh )
    {
        {
            // The cascade array ImportSceneViewTextures acquired (layer c = cascade c).
            const RDG::TextureRef                          maps     = textures.Transients.ShadowCascades;
            const System::MeshRenderer::ShadowCascadeFrame cascades = mesh.ShadowCascadePasses( maps );
            AddSystemRasters( graph, textures, cascades.StaticPasses, true );
            for ( const auto& copy : cascades.Copies )
            {
                const RDG::TextureRef from = textures.Import( copy.From, copy.Name + ".Cache" );
                if ( !from.IsValid() )
                    continue;
                graph.AddPass(
                     copy.Name, RDG::PassFlags::Copy,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Read( from, RDG::Access::CopySrc );
                         pass.Write( maps, RDG::Access::CopyDst, RDG::SubresourceRange::Layer( copy.Layer ) );
                     },
                     [from, maps, layer = copy.Layer]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return Renderer::CopyGraphDepthToLayer( context, from, maps, layer ); } );
            }
            AddSystemRasters( graph, textures, cascades.MovablePasses, false );
        }
    }

    void SceneRenderer::AddFrameBasePass( RDG::Builder& graph, FrameTextures& textures )
    {
        // The scene target's opaque raster, one clearing sequence: on the forward path the sky first (it CLEARS
        // colour; the depth is its depth prepass's, LOADED), then the meshes, then the terrain, both LOADING and
        // sharing the depth (CloserOrEqual against the prepass, no write). On the deferred path the first node
        // CLEARS colour and depth and the sky is not here: it is drawn behind the G-buffer's depth once that
        // reached the scene target (AddFrameSkyBehindScene), so no texel geometry covers is shaded by it.
        const bool                    forward = m_RenderPath != Core::RenderPath::Deferred || !m_GBuffer;
        std::vector<SystemRasterPass> passes;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
             sky != nullptr && forward )
            passes.push_back( sky->SkyPass() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ) )
            passes.push_back( mesh->GeometryPass() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* terrain = UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] ) )
            passes.push_back( terrain->GeometryPass() );
        AddSystemRasters( graph, textures, passes, true, {}, /*clearDepth*/ !forward );
    }

    void SceneRenderer::AddFrameSkyBehindScene( RDG::Builder& graph, FrameTextures& textures )
    {
        // THE DEFERRED SKY (UE: the sky after the base pass, where the depth is the far plane): over the scene
        // target LOADED - its depth is the G-buffer's (AddFrameDepthResolve: every opaque draw, the forward
        // meshes' depth/normal prepass included) - at CloserOrEqual against depth 0, writing none. The composite
        // discards the texels without geometry and the forward meshes are drawn at their own depth, so the sky
        // and they never shade the same texel.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] ) )
            AddSystemRaster( graph, textures, sky->SkyBehindScenePass() );
    }

    void SceneRenderer::AddFrameSilhouette( RDG::Builder& graph, FrameTextures& textures )
    {
        // The outlined meshes' mask, cleared every frame so the Jump Flood outline has a fresh input.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ) )
        {
            const SystemRasterPass silhouette = mesh->SilhouettePass();
            AddSystemRasters( graph, textures, std::span<const SystemRasterPass>( &silhouette, 1 ), true );
        }
    }

    void SceneRenderer::AddFrameTranslucency( RDG::Builder& graph, FrameTextures& textures )
    {
        // The translucency of the frame, in draw order - the order of these calls IS the order, there is no
        // numeric placement. The height fog's apply is NOT here: it fogs the OPAQUE scene and ran before the scene
        // copy and the first translucent draw; what is drawn here fogs itself at its own depth.
        // First the far field, the cloud composite: everything after it is nearer the camera and paints over it.
        // Then the particles, then the passes registered at the AfterTranslucency extension point (the editor
        // grid). All on the scene target: the graph merges them into one render pass.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        AddSystemRaster( graph, textures,
                         UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
                              ->CompositePass() );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
             particles != nullptr && !m_ViewProfile.SceneCapture )
            AddSystemRaster( graph, textures, particles->DrawPass() );
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::AfterTranslucency, {} );
    }

    void SceneRenderer::AddExtensionPoint( RDG::Builder& graph, FrameTextures& textures,
                                           const RDG::ExtensionPoint point, const OverlayTargets& overlay )
    {
        // Outside a BeginScene/EndScene bracket there is no scene, so no extension passes; without a scene target
        // there is nothing for them to draw over.
        if ( !m_FrameExtensions || !m_TargetFramebuffer )
            return;
        // A reflection capture's face view draws the game scene only: the editor's grid, gizmos and UI are not
        // what a reflection shows.
        if ( m_ViewProfile.SceneCapture )
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

    void SceneRenderer::AddPassNode( RDG::Builder& graph, FrameTextures& textures, const SystemRasterPass& pass,
                                     const std::shared_ptr<Framebuffer>& target, const std::string& debugName,
                                     const RDG::LoadOp& color, const RDG::LoadOp& depth,
                                     const OverlayTargets& overlay )
    {
        // A pass on one layer of a graph depth array declares that layer alone; one on a framebuffer, all of it.
        const bool                   layered = pass.TargetDepthArray.IsValid();
        std::optional<RasterTargets> targets;
        if ( layered )
        {
            targets.emplace();
            targets->Depth = pass.TargetDepthArray;
        }
        else
            targets = TargetsOf( textures, target, debugName, pass.Name );
        if ( !targets )
            return;
        // After the temporal resolve the scene target is the RENDER-extent pre-resolve scene: the overlay
        // draws into the OUTPUT-extent overlay set instead - every attachment replaced, so the render pass has
        // one extent (colour 0 the resolved colour, the velocity slot the overlay velocity, the depth the one
        // PopulateSceneDepth filled, the responsive-AA and thin-geometry-coverage slots overlay textures - never
        // read; one sample, so no resolves).
        if ( overlay.IsValid() && target == m_TargetFramebuffer )
        {
            if ( targets->Colors.size() != kSceneTargetThinGeometryCoverageSlot + 1 )
            {
                LOG_ERROR( "[SceneRenderer] pass '{}' refused: the scene target has {} colours, the overlay set "
                           "replaces exactly colour 0, the velocity, responsive-AA and thin-geometry-coverage slots",
                           pass.Name, targets->Colors.size() );
                return;
            }
            targets->Colors[0]                            = overlay.Color;
            targets->Colors[kSceneTargetVelocitySlot]     = overlay.Velocity;
            targets->Colors[kSceneTargetResponsiveAASlot] = overlay.ResponsiveAA;
            targets->Colors[kSceneTargetThinGeometryCoverageSlot] = overlay.ThinGeometryCoverage;
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
                     node.DepthTarget( targets->Depth, depth, !pass.DepthReadOnly,
                                       layered ? pass.TargetLayer : RDG::kAllRemaining );
                 DeclareResolves( node, targets->Resolves );
             },
             [execute = pass.ExecuteFunc, refs = textures.GraphRefs()](
                  RDG::PassContext& context ) -> Common::BoolResultStr { return execute( context, refs ); } );
    }

    RDG::TextureRef SceneRenderer::AddFrameForwardDepthPrepass( RDG::Builder& graph, FrameTextures& textures )
    {
        // THE FORWARD PATH'S DEPTH PREPASS (UE: the forward renderer's full DepthPass; FDepthPassMeshProcessor):
        // into the scene target's depth ALONE - no colour attachment, no shading. The meshes' node CLEARS it and
        // draws every opaque mesh the base pass shades (static, instanced, skinned, the opaque generics through
        // their DepthNormal program - MeshRenderer::DeclareDepthPrepassDraws); the terrain's LOADS it. The base
        // pass (AddFrameBasePass) then clears only colour, tests CloserOrEqual and writes no depth, and the
        // virtual shadow map marks the local lights' pages from this depth.
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Forward: Depth Prepass" );
        if ( !targets || !targets->Depth.IsValid() )
            return {};
        RasterTargets depthOnly;
        depthOnly.Depth = targets->Depth;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the keys name these exact types
        auto* const meshes  = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        auto* const terrain = UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] );
        // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
        // The meshes' node CLEARS (it is the first writer of the frame's scene depth); without a mesh renderer the
        // terrain's node does.
        if ( meshes != nullptr )
            AddRaster(
                 graph, "Forward: Depth Prepass", depthOnly, {}, RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
                 [meshes]( const RDG::PassContext& context ) -> Common::BoolResultStr
                 { return meshes->RenderDepthPrepassManual( context ); },
                 [meshes, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
                 { meshes->DeclareDepthPrepassDraws( pass, view ); } );
        if ( terrain != nullptr )
            AddRaster(
                 graph, "Forward: Terrain Depth Prepass", depthOnly, {},
                 meshes != nullptr ? RDG::LoadOp::Load() : RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
                 [terrain]( const RDG::PassContext& context ) -> Common::BoolResultStr
                 { return terrain->RenderDepthManual( context ); },
                 [terrain]( RDG::PassBuilder& pass ) { terrain->DeclareDepthDraws( pass ); } );
        return depthOnly.Depth;
    }

    void SceneRenderer::AddFrameGBuffer( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer )
    {
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Deferred: GBuffer" );
        if ( !targets || meshRenderer == nullptr )
            return;
        // ZERO, not the default 0.1 grey: empty texels need a zero normal, the lighting pass tells geometry from
        // sky by dot(normal, normal).
        // The shading word target (slot 2, RG32_UINT) takes the same clear: the bits of 0.0f are uint 0, which is
        // the contract's clear value (Unlit, receives sun shadows).
        static_assert( Core::ShadingModels::kShadingWordClearValue == 0u,
                       "the G-buffer clear writes 0.0f bits into the uint shading word" );
        // THE MASKED DEPTH PREPASS (UE: the depth pass with masked materials, r.EarlyZPass): the G-buffer's depth
        // alone, cleared here; the masked GPU-instanced sections write their clipped depth, and the G-buffer below
        // LOADs it and draws those sections without depth writes (MeshRenderer::DeclareMaskedDepthDraws).
        RasterTargets depthOnly;
        depthOnly.Depth = targets->Depth;
        AddRaster(
             graph, "Deferred: Masked Depth Prepass", depthOnly, {}, RDG::LoadOp::ClearDepth( Core::kDepthClear ), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderMaskedDepthManual( context ); },
             [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareMaskedDepthDraws( pass, view ); } );
        AddRaster(
             graph, "Deferred: GBuffer", *targets,
             textures.ColorLoads( *targets, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ),
             RDG::LoadOp::Load(), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGBufferManual( context ); },
             [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareGBufferDraws( pass, view ); } );
    }

    void SceneRenderer::AddFrameGBufferPostOcclusion( RDG::Builder& graph, FrameTextures& textures,
                                                      System::MeshRenderer* meshRenderer )
    {
        // LOAD: "Deferred: GBuffer" and "TerrainGBuffer" wrote it; only the main view's phase-2 bins are drawn.
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Deferred: GBuffer Post-Occlusion" );
        if ( !targets || meshRenderer == nullptr )
            return;
        // THE SECOND PHASE'S DEPTH PASS (UE two-pass occlusion: each phase's depth pass, then its base pass
        // EQUAL): the G-buffer's depth LOADed, the phase-2 bins and post-cut clusters the early-Z split pairs
        // write their depth through their depth twins; the G-buffer pass below draws them EQUAL, without writes or
        // clip.
        if ( meshRenderer->DrawsEarlyZ() )
        {
            RasterTargets depthOnly;
            depthOnly.Depth = targets->Depth;
            AddRaster(
                 graph, "Deferred: Depth Prepass Post-Occlusion", depthOnly, {}, RDG::LoadOp::Load(), {},
                 [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
                 { return meshRenderer->RenderEarlyZPostOcclusionManual( context ); },
                 [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
                 { meshRenderer->DeclareEarlyZPostOcclusionDraws( pass, view ); } );
        }
        AddRaster(
             graph, "Deferred: GBuffer Post-Occlusion", *targets,
             textures.ColorLoads( *targets, RDG::LoadOp::Load() ), RDG::LoadOp::Load(), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGBufferPostOcclusionManual( context ); },
             [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareGBufferPostOcclusionDraws( pass, view ); } );
    }

    void SceneRenderer::AddFrameGrassVisibility( RDG::Builder& graph, FrameTextures& textures,
                                                 System::MeshRenderer*                         meshRenderer,
                                                 const VirtualGeometry::VisibilityBufferFrame& visibility,
                                                 const bool                                    postOcclusion )
    {
        // THE GRASS VISIBILITY BUFFER (GRASS-VIS; UE 5 Nanite: the raster into VisBuffer64, then the material pass).
        // The raster LOADs the G-buffer's depth (the opaque scene, the terrain, the particles), tests it and writes
        // none: a blade fragment that survives its mask atomic-maxes its word, so the nearest wins whatever order
        // the draws ran in and no overdraw is shaded. GRASS-VIS-23 (UE Nanite EmitDepthTargets): after phase 1's
        // raster "Grass: Visibility Depth" writes the words' depth WITHOUT the material, so the HZB phase 2 tests
        // sees the grass; the material runs once, after phase 2 (AddFrameGrassVisibilityResolve) - before, the
        // phase-1 words were resolved twice (FPS-FIELD-BARN scn-1031-101332: Resolve 1.64 + Resolve Post 1.67 ms).
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Grass: Visibility Raster" );
        if ( !targets || meshRenderer == nullptr || !meshRenderer->DrawsGrassVisibility() )
            return;
        RasterTargets depthOnly;
        depthOnly.Depth = targets->Depth;
        const SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        // GRASS-VIS phase 2 (@p postOcclusion): the same two nodes over phase 2's records, after this frame's HZB;
        // the words still hold phase 1's winners, and the G-buffer depth they resolved rejects what lies behind.
        AddRaster(
             graph, postOcclusion ? "Grass: Visibility Raster Post-Occlusion" : "Grass: Visibility Raster", depthOnly,
             {}, RDG::LoadOp::Load(), {},
             [meshRenderer, postOcclusion]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGrassVisibilityRasterManual( context, postOcclusion ); },
             [meshRenderer, view, words = visibility.Words, postOcclusion]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareGrassVisibilityRasterDraws( pass, view, words, postOcclusion ); } );
        if ( postOcclusion )
            return;
        AddRaster(
             graph, "Grass: Visibility Depth", depthOnly, {}, RDG::LoadOp::Load(), {},
             [this]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return m_GrassVisibility.RecordDepth( context ); },
             [this, words = visibility.Words]( RDG::PassBuilder& pass )
             { m_GrassVisibility.DeclareDepthBindings( pass, words ); } );
    }

    void SceneRenderer::AddFrameGrassVisibilityResolve( RDG::Builder& graph, FrameTextures& textures,
                                                        System::MeshRenderer*                         meshRenderer,
                                                        const VirtualGeometry::VisibilityBufferFrame& visibility )
    {
        // One full-screen triangle per blade section over the camera's list: phase 2 appended its slots to the
        // same ids (GrassGenerate PostOcclusion), so phase 1's draws name every word's row. The G-buffer depth
        // holds the phase-1 words' depth ("Grass: Visibility Depth") and phase 2's meshes; the resolve writes each
        // word's own depth CloserOrEqual, so a phase-2 mesh in front keeps its texel.
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Grass: Visibility Resolve" );
        if ( !targets || meshRenderer == nullptr || !meshRenderer->DrawsGrassVisibility() )
            return;
        AddRaster(
             graph, "Grass: Visibility Resolve", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGrassVisibilityResolveManual( context ); },
             [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() ), words = visibility.Words,
              args = visibility.ResolveArgs]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareGrassVisibilityResolveDraws( pass, view, words, args ); } );
    }

    void SceneRenderer::AddFrameGrassPostOcclusion( RDG::Builder& graph, FrameTextures& textures,
                                                    System::MeshRenderer* meshRenderer, const ViewFrame& frame,
                                                    const VirtualGeometry::VisibilityBufferFrame& visibility )
    {
        // GRASS-VIS phase 2 (UE two-phase occlusion): the grass last frame's HZB held back from the camera, tested
        // against this frame's furthest HZB of the phase-1 depth (built by AddFrameDrawBinsPostOcclusion), then
        // drawn into the same G-buffer: the visibility buffer's sections by a second raster and resolve, the rest
        // by their cells. None built this frame (GRASS-VIS-6): the candidates are drawn untested - phase 1 kept
        // them out on last frame's depth alone, so dropping them would lose grass on the frame's own say-so.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* grass = UNIQUE_GET_AS( System::GrassRenderer, m_RenderSystems["GrassSystem"] );
        if ( grass == nullptr || meshRenderer == nullptr || m_ViewProfile.SceneCapture )
            return;
        const std::optional<glm::uvec2> hzbSize = m_FrameFurthestHzb.IsValid()
                                                       ? FrameHzbSize( graph, textures )
                                                       : std::optional<glm::uvec2>{};
        const RDG::TextureRef hzb = hzbSize ? m_FrameFurthestHzb : RDG::TextureRef{};
        const glm::uvec3      extent =
             hzbSize ? glm::uvec3( *hzbSize,
                                   static_cast<uint32_t>( std::bit_width( std::max( hzbSize->x, hzbSize->y ) ) ) )
                     : glm::uvec3( 0u );
        if ( !grass->AddFramePostOcclusion( graph, hzb, frame.ViewProjection, extent ) )
            return;
        if ( const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Grass: GBuffer Post-Occlusion" ) )
        {
            // PHASE 2'S GRASS DEPTH PASS (GRASS-VIS-13; UE two-pass occlusion with r.EarlyZPass: the grass HISM by
            // the rule of every mesh): the G-buffer's depth LOADed, the phase-2 sections EarlyZAdmits write their
            // clipped depth; "Grass: GBuffer Post-Occlusion" draws them EQUAL, without writes or clip. A node of its
            // own, not "Deferred: Depth Prepass Post-Occlusion": that one runs before AddFramePostOcclusion above
            // wrote phase 2's records, and only when the bins test this frame's HZB.
            if ( meshRenderer->DrawsEarlyZ() )
            {
                RasterTargets depthOnly;
                depthOnly.Depth = targets->Depth;
                AddRaster(
                     graph, "Grass: Depth Prepass Post-Occlusion", depthOnly, {}, RDG::LoadOp::Load(), {},
                     [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
                     { return meshRenderer->RenderGrassDepthPostOcclusionManual( context ); },
                     [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
                     { meshRenderer->DeclareGrassDepthPostOcclusionDraws( pass, view ); } );
            }
            AddRaster(
                 graph, "Grass: GBuffer Post-Occlusion", *targets,
                 textures.ColorLoads( *targets, RDG::LoadOp::Load() ), RDG::LoadOp::Load(), {},
                 [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
                 { return meshRenderer->RenderGrassGBufferPostOcclusionManual( context ); },
                 [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() )]( RDG::PassBuilder& pass )
                 { meshRenderer->DeclareGrassGBufferPostOcclusionDraws( pass, view ); } );
        }
        if ( visibility.IsValid() )
            AddFrameGrassVisibility( graph, textures, meshRenderer, visibility, true );
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

    void SceneRenderer::AddFrameForwardDepthNormal( RDG::Builder& graph, FrameTextures& textures,
                                                    System::MeshRenderer* meshRenderer )
    {
        // The generic queue's opaque draws are shaded forward over the composite, but the screen passes that run
        // before it (the ray-traced sun trace above all, which reads G-buffer depth + normal) must see them: their
        // depth and normal go into the G-buffer here, UE's depth prepass for what the base pass does not draw.
        // LOAD on the static/terrain G-buffer; each program writes only the normal slot (reflection masks the
        // others), and AddFrameDepthResolve then copies this depth to the scene target.
        const auto targets = TargetsOf( textures, m_GBuffer, "GBuffer", "Deferred: Forward Depth/Normal" );
        if ( !targets || meshRenderer == nullptr )
            return;
        AddRaster(
             graph, "Deferred: Forward Depth/Normal", *targets,
             textures.ColorLoads( *targets, RDG::LoadOp::Load() ), RDG::LoadOp::Load(), {},
             [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderForwardDepthNormalManual( context ); },
             [meshRenderer, view = SceneViewInputsOf( textures.GraphRefs() ),
              rayTracedSun = SunShadowIsScreenMask(), localShadowMasks = LocalShadowsAreScreenMask(),
              virtualLocal = LocalShadowsAreVirtual()]( RDG::PassBuilder& pass ) {
                 meshRenderer->DeclareForwardDepthNormalDraws( pass, view, rayTracedSun, localShadowMasks,
                                                               virtualLocal );
             } );
    }

    void SceneRenderer::AddFrameGeneric( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer, const RDG::TextureRef sunShadowMask,
                                         const RDG::TextureRef localShadowMasks )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Generic" );
        if ( !targets || meshRenderer == nullptr )
            return;
        // The frame's draw list, built in the setup with one binding block per material (the scene/view inputs -
        // cascades, environment cubes, BRDF LUT, cloud shadow map - bound where its shader has slots for them).
        SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        // Under the frame's ray-traced sun the RT permutations read the mask; a trace pass that could not be
        // declared (AddFrameRayTracedSunShadow logged it) leaves the sun unshadowed, as in the composite.
        if ( SunShadowIsScreenMask() )
            view.SunShadowMask = sunShadowMask.IsValid() ? sunShadowMask : textures.GraphRefs().System.White;
        // Under ray-traced local shadows the opaque programs read the masks in place of the atlas (the prepass chose
        // the same permutation by LocalShadowsAreScreenMask). The masks exist whenever the frame traced them
        // (AddFrameRayTracedLocalShadows packs at least one layer); a pack that failed (logged there) leaves them
        // invalid and the draws on the atlas program - no stand-in array is read in place of a missing mask.
        if ( LocalShadowsAreScreenMask() )
            view.LocalShadowMasks = localShadowMasks;
        AddRaster(
             graph, "Deferred: Generic", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGenericManual( context ); },
             [meshRenderer, view]( RDG::PassBuilder& pass ) { meshRenderer->DeclareGenericDraws( pass, view ); } );
    }

    void SceneRenderer::AddFrameGlass( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef sceneCopy,
                                       System::MeshRenderer* meshRenderer, const FrameFrontLayer& frontLayer )
    {
        if ( !sceneCopy.IsValid() || meshRenderer == nullptr )
            return;
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Glass" );
        if ( !targets )
            return;
        // The glass samples the scene copy for its refraction and the scene/view inputs its shader has slots for:
        // its binding block, declared here (the block entries are the node's reads), resolved in the body.
        const SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        // No front layer this frame: the graph's black texture for both, which the glass reads as "the sky alone".
        const RDG::TextureRef                              black = textures.GraphRefs().System.Black;
        const System::MeshRenderer::GlassFrontLayer glassFront{
             .Reflection = frontLayer.Reflection.IsValid() ? frontLayer.Reflection : black,
             .Depth      = frontLayer.Depth.IsValid() ? frontLayer.Depth : black };
        AddRaster(
             graph, "Deferred: Glass", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderGlassManual( context ); },
             [meshRenderer, sceneCopy, view, glassFront]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareGlassBindings( pass, sceneCopy, view, glassFront ); } );
    }

    void SceneRenderer::AddFrameWater( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef sceneCopy )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* water = UNIQUE_GET_AS( System::WaterRenderer, m_RenderSystems["WaterSystem"] );
        if ( water == nullptr || !water->HasDraws() || !sceneCopy.IsValid() )
            return;
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Water" );
        if ( !targets )
            return;
        // LOAD colour and depth: the water is depth-tested against the composited opaque scene and writes its own
        // depth, so the glass, the clouds and the particles after it see the surface. It samples the opaque
        // scene's copy (refraction) and the G-buffer depth (SceneViewInputs::SceneDepth: the bottom, a different
        // image from the scene target's depth it writes -
        // UE's SceneDepthWithoutSingleLayerWater). It fogs itself at its own depth (the opaque fog ran before it;
        // Mesh/Surface/SceneFog.glslh, bound by name through BindSceneViewInputs).
        const SceneViewInputs view = SceneViewInputsOf( textures.GraphRefs() );
        // The surface cell's scene blocks (camera, sun, sky cubes, sky look), as the mesh draws get them.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( meshRenderer == nullptr )
            return;
        const SceneFrameBinding frame = meshRenderer->CaptureFrameState( GetViewFrame() );
        // The persistent WaterInfo ground, imported into this graph and left shader-readable for the next frame.
        const RDG::TextureRef waterInfo =
             textures.Import( water->GetWaterInfo(), "WaterInfoGround", RDG::Access::SampledGraphics );
        if ( !waterInfo.IsValid() )
            return;
        AddRaster(
             graph, "Deferred: Water", *targets, textures.ColorLoads( *targets, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [water]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return water->RenderManual( context ); },
             [water, sceneCopy, waterInfo, view, frame]( RDG::PassBuilder& pass )
             { water->DeclareDraws( pass, sceneCopy, waterInfo, view, frame ); } );
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameOverdraw( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( meshRenderer == nullptr || !meshRenderer->EnsureOverdrawPass() )
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
        // The accumulation is read through the resolve's block (its OverdrawTexture entry is the node's read).
        AddRaster(
             graph, "Debug: Overdraw Resolve", *scene, textures.ColorLoads( *scene, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [meshRenderer]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RecordOverdrawResolve( context ); },
             [meshRenderer, overdraw = accum->Colors[0]]( RDG::PassBuilder& pass )
             { meshRenderer->DeclareOverdrawResolve( pass, overdraw ); } );
    }
    void SceneRenderer::AddFrameVirtualGeometryClusters( RDG::Builder& graph, FrameTextures& textures )
    {
        // This frame's cluster cut (AddFrameVirtualGeometry): nothing to draw without one.
        const ViewFrame* view = GetViewFrame();
        if ( view == nullptr || !m_FrameVirtualGeometry || !m_FrameDrawBins )
            return;
        if ( !m_VirtualGeometryClusterView )
            m_VirtualGeometryClusterView = std::make_unique<VirtualGeometry::ClusterView>();
        if ( !m_VirtualGeometryClusterView->Ensure() )
            return;
        const VirtualGeometry::VirtualMeshBuffers buffers =
             VirtualGeometry::VirtualMeshTable::Get().Import( graph );
        const auto scene =
             TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Debug: Virtual Geometry Clusters" );
        if ( !scene )
            return;
        const FrameDrawBins&                   bins = *m_FrameDrawBins;
        const FrameVirtualGeometry&            cut  = *m_FrameVirtualGeometry;
        const VirtualGeometry::ClusterViewDraw draw{ .Clusters       = buffers.Clusters,
                                                     .Dags           = buffers.Dags,
                                                     .Primitives     = bins.Scene.Primitives,
                                                     .Instances      = bins.Scene.Instances,
                                                     .PrimitiveDraws = bins.Upload.Passes[cut.Pass].PrimitiveDraws,
                                                     .DrawSlots      = bins.Upload.Passes[cut.Pass].DrawSlots,
                                                     .SubmeshTransforms = bins.Upload.SubmeshTransforms,
                                                     .VisibleClusters   = cut.VisibleClusters,
                                                     .RasterArgs        = cut.RasterArgs,
                                                     .VisibleCapacity   = cut.VisibleCapacity,
                                                     .ViewProjection    = view->JitteredViewProjection };
        // LOAD colour and depth: the clusters are depth-tested against the composited scene and write no depth.
        VirtualGeometry::ClusterView* const clusterView = m_VirtualGeometryClusterView.get();
        AddRaster(
             graph, "Debug: Virtual Geometry Clusters", *scene, textures.ColorLoads( *scene, RDG::LoadOp::Load() ),
             RDG::LoadOp::Load(), {}, [clusterView]( const RDG::PassContext& context ) -> Common::BoolResultStr
             { return clusterView->Record( context ); },
             [clusterView, draw]( RDG::PassBuilder& pass ) { clusterView->Declare( pass, draw ); } );
    }
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Graphic
