#include <Engine/Graphic/Systems/Scene/Deferred/SceneDepthResolveRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/GraphColorResolveRenderer.hpp>
#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/GraphImageImporter.hpp>
#include <Engine/Graphic/ImageFactory.hpp>
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
#include <vector>

namespace Desert::Graphic
{
    void SceneRenderer::Init()
    {
        // Init runs on the first scene AND on every scene after it — see the header for the two lifetimes
        // this splits into and the rule that separates them. The timings are logged because the cost of
        // this call is what Г11 was about and a claim about it should be readable from any run's log
        // rather than re-measured.
        const auto started = std::chrono::steady_clock::now();

        const bool built = EnsureRendererResources();

        const auto resourcesDone = std::chrono::steady_clock::now();

        RebindScene();

        const auto done = std::chrono::steady_clock::now();

        const auto ms = []( auto from, auto to )
        { return std::chrono::duration<float, std::milli>( to - from ).count(); };

        // The pipeline number is the SHARED CACHE's size and not this renderer's pipeline count — most
        // systems still build their own with GraphicsPipeline::Create rather than asking the cache, so the
        // total is several times larger. Printed anyway, and named accurately, because it is the one
        // pipeline number that can be read without a graphics debugger and it moves when the cache is
        // wrongly dropped.
        if ( built )
        {
            LOG_INFO( "[SceneRenderer] View '{}' built in {:.1f} ms ({} systems, {} cached pipelines) "
                      "and bound its first scene in {:.1f} ms.",
                      m_ViewResources.GetName(), ms( started, resourcesDone ), m_RenderSystemOrder.size(),
                      m_PipelineCache.Size(), ms( resourcesDone, done ) );
        }
        else
        {
            LOG_INFO( "[SceneRenderer] View '{}' rebound to a new scene in {:.1f} ms ({} systems and "
                      "{} cached pipelines kept).",
                      m_ViewResources.GetName(), ms( started, done ), m_RenderSystemOrder.size(),
                      m_PipelineCache.Size() );
        }

        // THE LEDGER, ONCE PER SCENE LOAD, IN THE LOG. Scene load is the moment the population changes and
        // the only moment anybody has ever wanted this number: "the editor keeps growing" is a claim about
        // successive loads, and until now the only instrument for it was the process's RSS, which mixes
        // device memory, the asset layer's CPU copies and the allocator's own slack. A line per load makes
        // the growth attributable to an owner instead of merely visible. See Graphic/ResourceLedger.hpp.
        // THE FOUR READOUTS BELOW ARE THE INSTRUMENT; the ledgers they read are not all of them (see
        // Common/Core/DevInstruments.hpp: ResourceLedger is the OWNERSHIP mechanism and stays). What a
        // shipping build drops is the reporting — four multi-line strings built and formatted at every
        // scene load for a reader who is not there.
#if DESERT_DEV_INSTRUMENTS
        if ( built && m_TargetFramebuffer )
        {
            const uint32_t viewW = m_TargetFramebuffer->GetFramebufferWidth();
            const uint32_t viewH = m_TargetFramebuffer->GetFramebufferHeight();
            LOG_INFO( "[ViewMemory] view '{}' {}", m_ViewResources.GetName(),
                      FormatViewTargetCensus( ViewTargetCensus( m_ViewProfile, viewW, viewH ), viewW, viewH ) );
        }
        LOG_INFO( "[Resources] {}", ResourceLedger::Report() );

        // THE OTHER TWO NUMBERS, BESIDE IT, AT THE SAME MOMENT — because the line above was measured
        // BLIND to the thing that grows. Between the control scene and a 50 179-entity world the ledger
        // total moved 677 829 bytes (0.15 %) while the process grew 315 MB, and nothing in the log said
        // so. Printed here rather than somewhere new for the reason this site was chosen in the first
        // place: scene load is the moment the population changes, and three numbers taken at different
        // moments cannot be subtracted from each other.
        LOG_INFO( "[Memory] {}", MemoryReadout::Take().Report() );
        LOG_INFO( "[Memory] {}", MemoryWatch::Report() );
        // AND WHAT LOADING THIS SCENE COST IN BLOCKING READS, split by phase. A scene switch during play
        // is the case this line was written for: the boot's total is expected and large, and an in-frame
        // count that is not zero after it names a hitch.
        LOG_INFO( "[SyncLoad] {}", Assets::SyncLoadLedger::Report() );
#endif
    }

    void SceneRenderer::ApplySceneSampleCount( const uint32_t samples )
    {
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetSpecification().Samples == samples )
            return;

        // THE ANTI-ALIASING METHOD CHANGED THE SAMPLE COUNT: the same path as a resize. The frames in flight
        // finish, the scene target is recreated at the new count, and the two MSAA-only helpers (DepthExpand,
        // SceneDepthResolve) rebuild for it. Pipelines are not rebuilt: each binds its variant for the open
        // render pass's count (VulkanPipeline::GetVkPipelineFor, resolved against the open pass).
        const uint32_t before = m_TargetFramebuffer->GetSpecification().Samples;
        Renderer::GetInstance().WaitDeviceIdle();
        if ( const auto set = m_TargetFramebuffer->SetSamples( samples ); !set )
        {
            LOG_ERROR( "[SceneRenderer] the scene target was not recreated at {}x: {}", samples, set.GetError() );
            return;
        }
        if ( const auto expand = m_RenderSystems.find( "DepthExpandSystem" ); expand != m_RenderSystems.end() )
            if ( const auto init = SP_CAST( System::DepthExpandRenderer, expand->second )->Initialize(); !init )
                LOG_ERROR( "[SceneRenderer] DepthExpand unavailable at {}x: {}", samples, init.GetError() );
        if ( const auto resolve = m_RenderSystems.find( "SceneDepthResolveSystem" );
             resolve != m_RenderSystems.end() )
            if ( const auto init = SP_CAST( System::SceneDepthResolveRenderer, resolve->second )->Initialize();
                 !init )
                LOG_ERROR( "[SceneRenderer] SceneDepthResolve unavailable at {}x: {}", samples, init.GetError() );
        LOG_INFO( "[SceneRenderer] scene samples {}x -> {}x (anti-aliasing method)", before, samples );
    }

    bool SceneRenderer::EnsureRendererResources()
    {
        if ( m_RendererResourcesBuilt )
            return false;

        m_RendererResourcesBuilt = true;

        // EVERYTHING BUILT BELOW BELONGS TO THIS RENDERER, and one scope says so for all of it. This
        // function and the twenty render systems it constructs create roughly a hundred and twenty device
        // objects — framebuffers, LUT images, pipelines and the materials the passes own — across twenty
        // files, and none of them is rebuildable from a file. Claiming them individually would mean
        // editing twenty files to answer one question, and the twenty-first system would be attributed by
        // nobody. See Engine/Graphic/ResourceLedger.hpp.
        //
        // A texture or mesh lazily built INSIDE this window is still the asset's: the services claim
        // theirs explicitly after the build, and an explicit claim overrides the ambient one.
        const ResourceAttributionScope owned( ResourceOwner::SceneRenderer );

        // The surface's size, never the window's: see ViewExtent.
        const uint32_t width  = m_ViewExtent.Width;
        const uint32_t height = m_ViewExtent.Height;
        // The render set starts at the output extent; the first frame's split resizes it (ResizeRenderTargets).
        m_RenderExtent = m_ViewExtent;

        // Framebuffer. MSAA applies HERE only: every scene system renders into this target at N samples and
        // the render pass resolves to single-sample for the post stack. The count follows the anti-aliasing
        // method every frame (ApplySceneSampleCount, from BeginScene); this is the count it starts at.
        //
        // ONE SAMPLE HERE, whatever the machine chose: MSAA applies only on the forward path (AA2,
        // Scalability::ResolveAntiAliasingForPath) and no scene — so no path — is known until the first
        // BeginScene, which raises the count for a forward scene under MSAA. Starting at 1 means a deferred scene
        // never allocates a multisampled target it cannot use.
        FramebufferSpecification fbSpec;
        fbSpec.DebugName = "Composite framebuffer";
        fbSpec.Samples   = 1;
        fbSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSceneColor );
        // DEPTH32F, AND THE FLOAT IS THE POINT. Reversed-Z (Core/Projection.hpp) works by lining the
        // 1/z curve up against the float exponent so the two cancel; on a UNORM24 attachment, which
        // quantizes uniformly in NDC, reversing the range just relabels the same 2^24 levels and buys
        // literally nothing. This was DEPTH24STENCIL8, and no pass in the engine enables a stencil test,
        // so the packed stencil byte was paying for nothing either.
        fbSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSceneDepth );

        m_TargetFramebuffer = Graphic::Framebuffer::Create( fbSpec );
        m_TargetFramebuffer->Resize( width, height );

        // Deferred G-buffer (populated only when SceneSettings::RenderPath == Deferred; allocated always so the
        // toggle is live). GBufferA = Albedo.rgb + Metallic.a (RGBA8F); GBufferB = Normal.rgb + Roughness.a
        // (kGBufferB); slot 2 = the shading word (R32_UINT, ShadingModelContract.glslh). World position is not
        // stored: readers rebuild it from the depth attachment (Common/ReconstructPosition.glslh); shared depth.
        FramebufferSpecification gbufferSpec;
        gbufferSpec.DebugName = "GBuffer";
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferA ); // GBufferA Albedo+Metallic
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferB ); // GBufferB Normal+Roughness
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferShadingWord ); // shading word (uint)
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferEmissive ); // GBufferEmissive (HDR self-illum)
        // DEPTH32F for the same reason as the forward target above — and it is this attachment the
        // height fog reads back as a texture, so its precision is the precision of every distance it
        // reconstructs.
        gbufferSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kGBufferDepth );
        m_GBuffer = Graphic::Framebuffer::Create( gbufferSpec );
        m_GBuffer->Resize( width, height );

        // NOTE: SSR and RSM-GI resources are deliberately NOT created here — see EnsureSSRResources() /
        // EnsureGIResources(). Every PreviewViewport (asset thumbnails, the Details mesh preview) builds its
        // OWN SceneRenderer, so anything allocated in this constructor is paid for once PER PREVIEW. Between
        // them SSR and RSM-GI want six full-screen RGBA32F targets plus a five-attachment RSM, and a preview
        // never turns either feature on. They are now allocated on first actual use instead.

        // Scene systems render into the shared target framebuffer; post-process systems form an
        // explicit chain (Mesh silhouette mask -> Jump Flood outline -> Tonemap).
        RegisterSystem<System::SkyboxRenderer>( "SkyboxSystem", this, m_TargetFramebuffer );
        RegisterSystem<System::MeshRenderer>( "MeshSystem", this, m_TargetFramebuffer );
        RegisterSystem<System::JumpFloodOutlineRenderer>( "JumpFloodSystem", this, m_TargetFramebuffer );

        // NAMED AND SURVIVED, NOT VERIFIED. `DESERT_VERIFY( false )` stood at each of the nine sites
        // below, so a render system that refused to initialise took the whole process with it — and
        // after Г22 that became REACHABLE for the first time: a typo in StaticMeshLit.shader now
        // produces an honest refusal from MeshRenderer::Initialize, which this line then turned into a
        // crash. The engine already has the rule for this one rung lower — VulkanRendererAPI::
        // BindGraphicsPipeline skips every draw through a pipeline that was not built — so a system
        // that could not initialise means its passes draw nothing, not that the editor vanishes.
        if ( !SP_CAST( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )->Initialize() )
            LOG_ERROR( "[SceneRenderer] the skybox system did not initialise; the sky will not draw." );

        const auto& meshSystem = SP_CAST( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( !meshSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] the mesh system did not initialise; meshes will not draw." );

        // GPU terrain (tessellated patch grid; opaque geometry, depth-tested with the meshes).
        RegisterSystem<System::TerrainRenderer>( "TerrainSystem", this, m_TargetFramebuffer );
        const auto& terrainSystem = SP_CAST( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] );
        if ( const auto init = terrainSystem->Initialize(); !init )
        {
            LOG_ERROR( "[SceneRenderer] the terrain system did not initialise; terrain will not draw: {}",
                       init.GetError() );
        }
        // The terrain casts into the sun's cascades from inside the mesh renderer's cascade passes, which
        // own and clear those targets (IShadowCaster says why it is not a pass of its own).
        else if ( const auto caster = terrainSystem->CreateShadowPipeline( meshSystem->GetCascadeFramebuffer() );
                  !caster )
        {
            LOG_ERROR( "[SceneRenderer] the terrain will not cast shadows: {}", caster.GetError() );
        }
        else
        {
            meshSystem->AddShadowCaster( terrainSystem );
        }

        const auto& jumpFloodSystem =
             SP_CAST( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
        if ( !jumpFloodSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] the outline system did not initialise; selection outlines are off." );

        // Feed the silhouette mask (produced by the mesh system) into the Jump Flood outline.
        jumpFloodSystem->SetMaskFramebuffer( meshSystem->GetSilhouetteMaskFramebuffer() );

        // Tonemap consumes the Jump Flood output (the outlined scene).
        RegisterSystem<System::TonemapRenderer>( "TonemapSystem", this, jumpFloodSystem->GetSystemFramebuffer() );
        const auto& tonemapSystem = SP_CAST( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( !tonemapSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] the tonemap system did not initialise; the viewport stays black." );

        // Backdrop blur: a blurred snapshot of the scene colour the UI canvas samples for "glass" panels.
        // Runs before the UI extension point (whose passes write into this same target, so it cannot sample it
        // directly).
        RegisterSystem<System::BackdropBlurRenderer>( "BackdropBlurSystem", this, m_TargetFramebuffer );
        if ( const auto& backdropSystem =
                  SP_CAST( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] );
             !backdropSystem->Initialize() )
        {
            LOG_WARN( "Backdrop blur unavailable — UI glass panels will draw as flat tint" );
        }

        // Bloom reads the HDR scene color and produces a compute mip-chain glow that tonemap adds in.
        RegisterSystem<System::BloomRenderer>( "BloomSystem", this, m_TargetFramebuffer );
        const auto& bloomSystem = SP_CAST( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
        if ( !bloomSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] the bloom system did not initialise; the scene renders without glow." );

        // Light shafts: the atmosphere sun's screen-space streaks, masked and radially blurred from the
        // HDR scene colour; tonemap adds them in the way it adds bloom. Non-fatal: a sky without streaks
        // must never take a scene down.
        RegisterSystem<System::LightShaftRenderer>( "LightShaftSystem", this, m_TargetFramebuffer );
        const auto& lightShaftSystem = SP_CAST( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
        if ( !lightShaftSystem->Initialize() )
            LOG_WARN( "[SceneRenderer] Light shaft system unavailable." );

        // Lens flare: the camera's own response to the sun disc — ghosts, halo and streak gathered from
        // the same HDR scene colour, added in by the tonemap the way bloom is. Non-fatal, like the shafts.
        RegisterSystem<System::LensFlareRenderer>( "LensFlareSystem", this, m_TargetFramebuffer );
        const auto& lensFlareSystem = SP_CAST( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
        if ( const auto flareInit = lensFlareSystem->Initialize(); !flareInit )
            LOG_WARN( "[SceneRenderer] Lens flare system unavailable: {}", flareInit.GetError() );

        // SSAO (fullscreen G-buffer -> AO factor). Its target is a per-frame graph transient (AddFrameSSAO);
        // the deferred Composite reads it. Deferred only. Non-fatal.
        RegisterSystem<System::SSAORenderer>( "SSAOSystem", this, m_TargetFramebuffer );
        if ( !SP_CAST( System::SSAORenderer, m_RenderSystems["SSAOSystem"] )->Initialize() )
            LOG_WARN( "[SceneRenderer] SSAO system unavailable." );

        // The G-buffer depth into a multisampled scene depth (Deferred: DepthExpand); nothing is built at MSAA 1.
        RegisterSystem<System::DepthExpandRenderer>( "DepthExpandSystem", this, m_TargetFramebuffer );
        if ( const auto expandInit =
                  SP_CAST( System::DepthExpandRenderer, m_RenderSystems["DepthExpandSystem"] )->Initialize();
             !expandInit )
            LOG_ERROR( "[SceneRenderer] DepthExpand unavailable (deferred depth at MSAA): {}",
                       expandInit.GetError() );

        // The multisampled scene depth into the 1x depth compute passes sample (Scene: DepthResolve); MSAA > 1
        // only.
        RegisterSystem<System::SceneDepthResolveRenderer>( "SceneDepthResolveSystem", this, m_TargetFramebuffer );
        if ( const auto resolveInit =
                  SP_CAST( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] )
                       ->Initialize();
             !resolveInit )
            LOG_ERROR( "[SceneRenderer] SceneDepthResolve unavailable (fog and clouds at MSAA): {}",
                       resolveInit.GetError() );

        // The sample-0 shader resolve of the scene target's SampleZero graph colours (the view's velocity at
        // MSAA).
        RegisterSystem<System::GraphColorResolveRenderer>( "GraphColorResolveSystem", this, m_TargetFramebuffer );
        if ( const auto colorResolveInit =
                  SP_CAST( System::GraphColorResolveRenderer, m_RenderSystems["GraphColorResolveSystem"] )
                       ->Initialize();
             !colorResolveInit )
            LOG_ERROR( "[SceneRenderer] GraphColorResolve unavailable (velocity at MSAA reads zero motion): {}",
                       colorResolveInit.GetError() );

        RegisterSystem<System::CopyRenderer>( "SceneColorCopySystem", this, m_TargetFramebuffer );
        if ( !SP_CAST( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] )->Initialize() )
            LOG_WARN( "[SceneRenderer] Scene-color copy system unavailable (glass refraction off)." );

        RegisterSystem<System::VelocityViewRenderer>( "VelocityViewSystem", this, m_TargetFramebuffer );
        if ( const auto velocityViewInit =
                  SP_CAST( System::VelocityViewRenderer, m_RenderSystems["VelocityViewSystem"] )->Initialize();
             !velocityViewInit )
            LOG_ERROR( "[SceneRenderer] Velocity view mode unavailable: {}", velocityViewInit.GetError() );

        // Atmosphere and fog: aerial perspective on opaque with exponential height fog over it — one
        // compute evaluation issued outside the graph (ExecuteAtmosphericFog) and one apply pass in the
        // translucency, below the particles by the order of AddFrameTranslucency's calls.
        // Non-fatal: neither must ever take a scene down.
        RegisterSystem<System::HeightFogRenderer>( "HeightFogSystem", this, m_TargetFramebuffer );
        if ( const auto fogInit =
                  SP_CAST( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )->Initialize();
             !fogInit )
            LOG_WARN( "[SceneRenderer] Height fog system unavailable: {}", fogInit.GetError() );

        // Volumetric clouds: a march through a spherical shell, issued outside the graph
        // (VolumetricCloudRenderer::DeclareFrameNodes) with one composite pass in the translucency, above the
        // fog and below the particles by the order of AddFrameTranslucency's calls. Non-fatal: a missing sky
        // must never take a scene down.
        RegisterSystem<System::VolumetricCloudRenderer>( "VolumetricCloudSystem", this, m_TargetFramebuffer );
        if ( const auto cloudInit =
                  SP_CAST( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
                       ->Initialize();
             !cloudInit )
            LOG_WARN( "[SceneRenderer] Volumetric cloud system unavailable: {}", cloudInit.GetError() );

        // GPU particles: compute-simulated billboards drawn by AddFrameTranslucency. Non-fatal.
        RegisterSystem<System::ParticleRenderer>( "ParticleSystem", this, m_TargetFramebuffer );
        if ( !SP_CAST( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->Initialize() )
            LOG_WARN( "[SceneRenderer] Particle system unavailable." );

        // Deferred lighting (fullscreen G-buffer shade + debug view). Runs in the manual chain, only when
        // RenderPath == Deferred. Non-fatal if it fails to init (deferred path is simply unavailable).
        RegisterSystem<System::DeferredLightingRenderer>( "DeferredLightingSystem", this, m_TargetFramebuffer );
        if ( !SP_CAST( System::DeferredLightingRenderer, m_RenderSystems["DeferredLightingSystem"] )
                   ->Initialize() )
            LOG_WARN( "[SceneRenderer] Deferred lighting system unavailable." );

        // Auto-exposure measures the HDR scene luminance into a 1x1 buffer that tonemap reads.
        RegisterSystem<System::AutoExposureRenderer>( "AutoExposureSystem", this, m_TargetFramebuffer );
        const auto& autoExposureSystem =
             SP_CAST( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( !autoExposureSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] auto-exposure did not initialise; exposure stays at its default." );
        tonemapSystem->SetAutoExposureImage( autoExposureSystem->GetAdaptedLuminanceImage() );

        // FXAA consumes the tonemapped image (LDR). It only runs when the machine's post AA is FXAA
        // (Common::Settings::MachineSettings::AA — it left SceneSettings with К3).
        RegisterSystem<System::FXAARenderer>( "FXAASystem", this, tonemapSystem->GetSystemFramebuffer() );
        if ( !SP_CAST( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Initialize() )
            LOG_ERROR( "[SceneRenderer] FXAA did not initialise; the image is drawn without post AA." );

        // SMAA consumes the same tonemapped image. Runs only when the machine's post AA is SMAA.
        RegisterSystem<System::SMAARenderer>( "SMAASystem", this, tonemapSystem->GetSystemFramebuffer() );
        if ( !SP_CAST( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Initialize() )
            LOG_ERROR( "[SceneRenderer] SMAA did not initialise; the image is drawn without post AA." );

        // The graph is NOT built here: RebindScene() runs immediately after and builds it once, over the
        // engine systems above plus whatever external passes survive. Building it twice on the first Init
        // would be the only place in the engine that did.
        return true;
    }

    void SceneRenderer::RebindScene()
    {
        // Everything below releases render-system state (OnSceneReplaced), and a render system owns pipelines and
        // descriptor pools the last submitted frame may still be executing against. Same rule, same reason, as the
        // five sites that destroy a whole SceneRenderer (Desert/Tests/Engine/TeardownOrder).
        //
        // Paid on EVERY scene load even when there is nothing to release. That is deliberate: the caller has
        // just cleared the entity registry and is about to destroy the RenderRegistry, so the frame that
        // was in flight when the load was requested is referencing objects on their way out either way.
        Renderer::GetInstance().WaitDeviceIdle();

        // Tell the engine systems — which are the renderer's and stay — that the world
        // they have been accumulating over is gone. Most of them do nothing with it; the ones that do are a
        // temporal history and a per-entity GPU cache, and IRenderSystem::OnSceneReplaced says why those two
        // are the only kinds that can exist here.
        //
        // Walked over m_RenderSystemOrder rather than the map: the map's operator[] accesses elsewhere in this
        // file insert null entries, and the order vector never holds one.
        for ( const auto& name : m_RenderSystemOrder )
        {
            if ( const auto it = m_RenderSystems.find( name ); it != m_RenderSystems.end() && it->second )
                it->second->OnSceneReplaced();
        }
    }

    void SceneRenderer::ResetTemporalHistory()
    {
        // Over m_RenderSystemOrder for the reason RebindScene gives: the map can hold null entries.
        for ( const auto& name : m_RenderSystemOrder )
        {
            if ( const auto it = m_RenderSystems.find( name ); it != m_RenderSystems.end() && it->second )
                it->second->OnTemporalHistoryReset();
        }
        // The view's own history (previous matrices, history textures, motion records) resets with the next
        // frame: an explicit camera cut (ViewInputs::CameraCut), consumed by the BeginFrame that succeeds.
        m_CameraCutPending = true;
        LOG_INFO( "[SceneRenderer] {}: temporal history reset over {} render system(s).",
                  m_ViewResources.GetName(), m_RenderSystemOrder.size() );
    }

    namespace
    {
        // A view's name: what kind of surface it is and a serial that is never reused, so two log lines
        // about "preview #4" are about the same window and a reopened preview is visibly a new view.
        std::string NameView( const ViewProfile& profile )
        {
            static std::atomic<uint32_t> serial{ 0 };
            const uint32_t               number = ++serial;
            const char*                  kind   = "scene view";
            if ( profile == kPreviewViewProfile )
                kind = "preview";
            else if ( profile == kThumbnailViewProfile )
                kind = "thumbnail";
            return std::string( kind ) + " #" + std::to_string( number );
        }

        // Every live renderer, so the view budget can name what each holds. Guarded: views are built and
        // destroyed from panels and from the UI producer, not only from one loop.
        std::mutex& LiveRenderersMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
        std::vector<const SceneRenderer*>& LiveRenderers()
        {
            static std::vector<const SceneRenderer*> live;
            return live;
        }
    } // namespace

    uint64_t SceneRenderer::HeldBytes() const
    {
        return SumViewTargets( ViewTargetCensus( m_ViewProfile, m_ViewExtent.Width, m_ViewExtent.Height ) )
                    .Total() +
               m_ViewResources.HeldBytes() + m_ViewState.HeldBytes();
    }

    std::vector<Engine::ViewBudget::HeldView> SceneRenderer::LiveHoldings()
    {
        const std::scoped_lock                    lock( LiveRenderersMutex() );
        std::vector<Engine::ViewBudget::HeldView> held;
        held.reserve( LiveRenderers().size() );
        for ( const SceneRenderer* renderer : LiveRenderers() )
            held.push_back( { renderer->m_ViewResources.GetName(), renderer->HeldBytes() } );
        return held;
    }

    std::string SceneRenderer::DescribeLiveViews()
    {
        uint64_t held = 0;
        for ( const Engine::ViewBudget::HeldView& view : LiveHoldings() )
            held += view.Bytes;
        return std::format( "{} live, {:.1f} MiB", ViewResourceRegistry::LiveCount(),
                            static_cast<double>( held ) / ( 1024.0 * 1024.0 ) );
    }

    SceneRenderer::SceneRenderer( const ViewExtent& extent, const ViewProfile& profile )
         : m_ViewResources( NameView( profile ) ), m_ViewProfile( profile ), m_ViewExtent( extent ),
           m_RenderExtent( extent )
    {
        {
            const std::scoped_lock lock( LiveRenderersMutex() );
            LiveRenderers().push_back( this );
        }

        // A level's viewport is BUILT at the machine's Shadows level, so a Low machine never allocates the High
        // maps only to re-allocate them on the first frame. Later level changes: BeginScene.
        if ( const auto budget = ShadowReallocation( m_ViewProfile, m_Quality ) )
            m_ViewProfile.Shadows = *budget;
        m_ShadowBudgetGeneration = m_Quality.Generation;

        // Logged on BOTH edges with the resulting count and bytes: a surface that never destroys its view
        // produces no error at all, only a budget that fills up some minutes later, so the numbers are
        // printed rather than left for a reader to derive. There is no ceiling on the count — each view
        // owns its own copies, and the only limit is the byte budget the editor checks before opening one.
        LOG_INFO( "[SceneRenderer] Created view '{}' ({}).", m_ViewResources.GetName(), DescribeLiveViews() );
    }

    SceneRenderer::~SceneRenderer()
    try
    {
        {
            const std::scoped_lock lock( LiveRenderersMutex() );
            std::erase( LiveRenderers(), this );
        }
        // Logged while this view's resources are still registered, so the count includes it.
        LOG_INFO( "[SceneRenderer] Destroying view '{}' ({} before release).", m_ViewResources.GetName(),
                  DescribeLiveViews() );
    }
    DESERT_DESTRUCTOR_GUARD( "~SceneRenderer" )

    NO_DISCARD Common::BoolResultStr SceneRenderer::BeginScene( const Desert::Core::Scene& scene,
                                                                Core::Camera*              camera )
    {
        // This view is where per-frame writes go until the phase returns — set FIRST, before anything writes
        // a per-frame resource; the frame context gets them back on every exit, so nothing written after the
        // last view of a frame lands in this view's copies.
        const ActiveViewScope viewScope( m_ViewResources );

        // HANDED IN, NOT READ OFF THE SCENE. This used to be `scene.GetMainCamera()`, which is the same
        // answer for every renderer of that scene — so a second view of one world rendered from the
        // first view's camera, and "several viewports" could only ever mean "several worlds". The scene
        // holds a LIST of views now and each one carries its own camera; the view says which.
        m_SceneInfo.ActiveCamera = camera;
        // The world this frame draws and its clock (ViewInputs::SceneIdentity / TimeSeconds, OnUpdate).
        m_SceneGeneration  = scene.GetGeneration();
        m_SceneTimeSeconds = scene.GetWorldTime().GetGameTimeSeconds();

        const auto& skyboxSystem = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );

        skyboxSystem->PrepareCamera( m_SceneInfo.ActiveCamera );

        m_ScenePlaying = scene.IsPlaying(); // grid & other authoring aids hide while the game runs
        // The scene's extension passes, for AddExtensionPoint while OnUpdate builds this frame.
        m_FrameExtensions = &scene.GetExtensionPasses();

        const auto& sceneSettings = scene.GetSettings();

        // THE ONE POINT THIS VIEW'S GRADE AND SHADOW POLICY ARE READ FROM (SET1) — UE's
        // FFinalPostProcessSettings. The PostProcessVolume entities are blended at this camera's position and
        // the directional light's shadow fields are taken from the light the scene shades with; everything
        // below reads `post` and `shadows`, and no system reads a volume or the light's shadow fields itself.
        const std::optional<glm::vec3> viewPosition =
             camera != nullptr ? std::optional<glm::vec3>( camera->GetPosition() ) : std::nullopt;
        const FinalViewSettings          viewSettings = ResolveViewSettings( scene.GetRegistry(), viewPosition );
        const Core::PostProcessSettings& post         = viewSettings.Post;
        const ViewShadowSettings&        shadows      = viewSettings.Shadows;
        // Selection-outline appearance is NOT read from the scene: it's an editor-only viewport aid pushed
        // each frame via SetOutlineSettings (from EditorPreferences). Runtime builds never push -> the
        // JumpFlood system keeps its defaults, and MeshRenderer::HasOutline() gates whether it draws.
        //
        // The DEBUG VIEW (m_DebugView) is pushed the same way and read here rather than from the scene, for
        // the same reason and on the same terms — SetDebugView, from EditorPreferences, every frame.

        // THE FIVE QUALITY VALUES COME FROM HERE, NOT FROM THE SCENE (К3). They describe what this
        // MACHINE can afford, so they are pushed in per view (SetQuality) exactly as the debug view and
        // the outline are, and a scene file cannot state them at all. Named as one local because they are
        // one answer arriving from one place — and because a census that asks "does anything read this
        // setting" has to be able to SEE the read (Desert/Tests/Engine/ConfigOwnership).
        const Common::Scalability::ResolvedQuality& quality = m_Quality;
        using Common::Scalability::Parameter;

        // TWO FORWARD-ONLY DEBUG VIEWS, and they force the path for the same reason.
        //
        // Wireframe has no deferred variant: the G-buffer pipeline has no wireframe polygon mode, which is
        // why turning it on in the default Deferred path did nothing at all.
        //
        // LightingDebug is the per-light attribution view, and it is a BRANCH IN THE Lit MESH SHADERS
        // (u_DebugParams.y, StaticMeshLit / StaticMeshLit_Instanced / SkinnedMeshLit). The deferred
        // lighting pass writes `DebugParams = vec4(0)` unconditionally — MaterialDeferredLighting's
        // UploadShadow — so the flag reaches no shader on that path. Forty-six of this repository's
        // forty-nine scenes state Deferred and the struct's default is Deferred, so WITHOUT this line the
        // View Mode entry К7 added would be a control that does nothing in almost every scene: the §1.3
        // dead setting, reintroduced by the fix for one.
        m_RenderPath = ( m_DebugView.WireframeMode || m_DebugView.LightingDebug ) ? Core::RenderPath::Forward
                                                                                  : sceneSettings.RenderingPath;

        // THE ANTI-ALIASING THIS FRAME RUNS (AA2), read from the settings layer's resolution of the
        // machine's choice for the SCENE'S path — not m_RenderPath: a debug view that forces forward must
        // not reallocate the scene target at another sample count, and the Scalability panel reports
        // against the same scene path. Under MSAA in a deferred scene this is FXAA at one sample, so no
        // multisampled target, no DepthExpand/SceneDepthResolve resources and no multisampled pipeline
        // variant is ever built there. The sample count is already one the device offers (Resolve walks the
        // catalog's MSAACounts down and reports it), so nothing is clamped here.
        const Common::Scalability::PathAntiAliasing aa = Common::Scalability::ResolveAntiAliasingForPath(
             quality, Core::RenderPathSupportsMSAA( sceneSettings.RenderingPath ) );
        m_AAMode = aa.PostProcess;
        // What the frame renders, handed to SceneViewState::BeginFrame as resolved: TAA stays TAA (its
        // PostProcess is None, so no FXAA/SMAA runs after it and the tonemap output is the final image).
        m_RenderedAntiAliasing = aa;
        m_TemporalAAQuality    = static_cast<TemporalAAQuality>(
             quality.As<int>( Common::Scalability::Parameter::TemporalAAQuality ) );
        ApplySceneSampleCount( static_cast<uint32_t>( aa.Samples ) );
        m_EnableSSAO = post.EnableSSAO;
        // The cloud layer's cost ceiling, refreshed here with every other cost-versus-quality choice
        // rather than read from a global at the point of use: several SceneRenderers are live at once
        // (Docs/RENDERER_FRAME_STATE.md) and a preview pane may be given a cheaper tier than the viewport.
        m_CloudQuality   = quality.As<Common::Settings::CloudQuality>( Parameter::CloudQuality );
        m_GIMode         = post.GlobalIllumination;
        m_GIIntensity    = post.GIIntensity;
        m_EnableSSR      = post.EnableSSR;
        m_SSRIntensity   = post.SSRIntensity;
        m_SSRMaxDistance = post.SSRMaxDistance;
        m_SSRMaxSteps    = quality.As<int>( Parameter::ReflectionMaxSteps );
        m_GISamples      = quality.As<int>( Parameter::GlobalIlluminationSamples );
        m_SSAOSamples    = quality.As<int>( Parameter::AmbientOcclusionSamples );

        // THE SHADOW BUDGET FOLLOWS THE SHADOWS LEVEL (UE re-creates its shadow depth targets on r.Shadow.*).
        // Asked only when the quality generation moved — a level change, never per frame — and the maps are
        // re-allocated only when the budget really differs (ShadowReallocation, ViewMemory.hpp). The profile is
        // updated with them so the view's memory census keeps describing what is allocated.
        if ( m_Quality.Generation != m_ShadowBudgetGeneration )
        {
            m_ShadowBudgetGeneration = m_Quality.Generation;
            if ( const auto budget = ShadowReallocation( m_ViewProfile, quality ) )
            {
                m_ViewProfile.Shadows = *budget;
                if ( !UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
                           ->RebudgetShadows( *budget ) )
                    LOG_ERROR( "[Shadows] view '{}': the shadow pass could not be set up at {} cascades of {} px; "
                               "nothing casts a shadow in this view.",
                               m_ViewResources.GetName(), budget->CascadeCount, budget->ShadowMapSize );
            }
        }

        // GPU particles: snapshot the scene's emitters (CPU) here; the compute sim is dispatched in OnUpdate
        // before the render graph, and the billboard pass draws in AddFrameTranslucency.
        UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->PrepareFrame( scene );

        // The rest of what reads time in a frame reads the SCENE'S clock (Core::WorldTime), handed over
        // here: the material Time uniform and the eye adaptation step.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetWorldTimeSeconds( static_cast<float>( scene.GetWorldTime().GetGameTimeSeconds() ) );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] )
             ->SetDeltaSeconds( scene.GetWorldTime().GetDeltaSeconds() );

        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetParams( post.Exposure, post.Gamma );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetTonemapOperator( post.Tonemapper );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetWhitePoint( post.WhitePoint );

        // The systems map is keyed by the name each system was registered under, so the downcast is to the type
        // registered there — the same UNIQUE_GET_AS every neighbouring line uses.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->SetBackdropVisible( m_DebugView.ShowSkyBackdrop );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetWireframe( m_DebugView.WireframeMode );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetLODEnabled( quality.As<bool>( Parameter::MeshLOD ) );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetShadows( shadows.Enabled, shadows.Bias, static_cast<int>( m_DebugView.ShadowDebug ),
                           shadows.CascadeSplitLambda );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetDebugView( m_DebugView.ShowNormals, m_DebugView.ShowBoundingBoxes, m_DebugView.BoundingBoxColor,
                             m_DebugView.BoundingBoxLineWidth, m_DebugView.LightingDebug );

        // The texture filter and anisotropy are NOT pushed from here: they are global sampler state, written once
        // per change by QualityBoot's QualityState listener, not by every view each frame.

        // THE TWO BRIGHT-PASS THRESHOLDS ARE AUTHORED IN THE EXPOSED IMAGE AND COMPARED IN THE RAW ONE,
        // and until this line they were simply handed across that boundary unchanged.
        //
        // SceneComposite computes `(scene + bloom + shafts + flare) * exposure`, so `exposure` is the only
        // thing standing between scene radiance and what the tonemapper sees. Both bright passes run
        // UPSTREAM of it and threshold the raw HDR: BloomDownsample takes `max(brightness - threshold, 0)`
        // on the scene image, LensFlareBrightPass does the same. An authored 2.5 therefore means "2.5" in
        // a scene at Exposure 1.0 and "0.55" in one at Exposure 0.22 — one knob with two meanings, decided
        // by an unrelated field, which is the disagreement §2.3.1 of the contract is about.
        //
        // Measured, Clouds_Demo (Exposure 0.22), zenith looking into the sun, with the shadow ray
        // converged: bloom and the flare added 1.389 of linear scene radiance on top of 0.989 — they MORE
        // THAN DOUBLED the highlight, and took a frame that landed on the UE reference's p95 (0.802
        // against 0.800) up to 0.922. At the authored 2.5 the effective cutoff was 0.55 of a normalised
        // unit, so ordinary daylight sky was a bloom source. Dividing here puts the comparison back in the
        // space the number was written in; neither authored number was retuned.
        //
        // AUTO-EXPOSURE IS NOT FIXED BY THIS AND IS NOT PRETENDED TO BE. There the exposure is
        // `key / adaptedLuminance` evaluated in the composite from a 1x1 image the CPU never reads, so
        // this function has nothing to divide by and leaves the threshold in raw radiance — the behaviour
        // it has always had. No repository scene enables auto-exposure. Closing it means giving the bloom
        // pass the adapted-luminance image and doing the comparison on the GPU, which is a binding this
        // pass does not have; it is written down in Docs/Clouds/CALIBRATION.md rather than left to be
        // rediscovered.
        const float exposureNormalisation = post.AutoExposure ? 1.0f : std::max( post.Exposure, 1e-4f );

        m_BloomEnabled = post.EnableBloom;
        UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] )
             ->SetThreshold( post.BloomThreshold / exposureNormalisation );
        // PostProcess.BloomMips (Scalability; High 6 = the former BloomRenderer::kMaxBloomMips).
        UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] )
             ->SetMaxMips(
                  static_cast<uint32_t>( m_Quality.As<int>( Common::Scalability::Parameter::BloomMips ) ) );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetBloomIntensity( post.EnableBloom ? post.BloomIntensity : 0.0f );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetChromaticBloom( post.EnableBloom ? post.LensDispersion : 0.0f );

        // Lens flare: the authored "Lens Flare" group, copied whole. Intensity and Tint are held out of
        // the pass's own params because the pass never applies them — the tonemap does, so that a flare
        // whose sun has left the screen fades through ONE number instead of two that could disagree.
        m_LensFlare.Enabled   = post.EnableLensFlare;
        m_LensFlare.Intensity = post.LensFlareIntensity;
        // Normalised for the reason given at the bloom threshold above, and by the same number: this pass
        // thresholds the same raw HDR image, so leaving one of the two in raw radiance would only move the
        // defect from one bright pass to the other.
        m_LensFlare.Threshold       = post.LensFlareThreshold / exposureNormalisation;
        m_LensFlare.GhostCount      = post.LensFlareGhostCount;
        m_LensFlare.GhostSpacing    = post.LensFlareGhostSpacing;
        m_LensFlare.GhostSizeNear   = post.LensFlareGhostSizeNear;
        m_LensFlare.GhostSizeFar    = post.LensFlareGhostSizeFar;
        m_LensFlare.GhostTintInner  = post.LensFlareGhostTintInner;
        m_LensFlare.GhostTintOuter  = post.LensFlareGhostTintOuter;
        m_LensFlare.HaloIntensity   = post.LensFlareHaloIntensity;
        m_LensFlare.HaloRadius      = post.LensFlareHaloRadius;
        m_LensFlare.StreakIntensity = post.LensFlareStreakIntensity;
        m_LensFlare.StreakLength    = post.LensFlareStreakLength;
        m_LensFlare.StreakAngle     = post.LensFlareStreakAngle;
        m_LensFlare.ChromaShift     = post.LensFlareChromaShift;
        m_LensFlareTint             = post.LensFlareTint;

        UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] )
             ->SetParams( post.AutoExposureSpeed, post.AutoExposureMin, post.AutoExposureMax );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetAutoExposure( post.AutoExposure, post.AutoExposureKey );

        return BOOLSUCCESS;
    }

    void SceneRenderer::OnUpdate( const UpdateInfo& sceneRenderInfo )
    {
        DESERT_PROFILE_SCOPE( "SceneRenderer::OnUpdate" );

        // RE-OPENED HERE, and it is not belt-and-braces. Scene drives the views PHASE BY PHASE — every
        // view's BeginScene, then every view's OnUpdate — and each phase's scope is gone when it returns.
        // Without this line a second view writes its per-frame GPU state into the frame context instead of
        // its own copies, a torn picture with nothing in the log (Docs/RENDERER_FRAME_STATE.md).
        const ActiveViewScope viewScope( m_ViewResources );

        const auto& skyboxSystem = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
        m_DirectionLights        = sceneRenderInfo.DirLights;

        // THE SUN THE ATMOSPHERE LETS THROUGH (UE's PrepareSunLightProxy). The light's authored colour
        // is its OUTER-SPACE illuminance in the physical model; what reaches the ground has crossed the
        // whole atmosphere, so it is multiplied by that path's transmittance — and a sunset reddens and
        // dims every lit surface by the same law that reddens the sky behind it, for free.
        //
        // The factor is exactly (1,1,1) unless the physical model is running AND this sun opted in, so
        // there is no branch here and no second behaviour to test: SkyModel::ArtisticGradient keeps the
        // documented independence of sky radiance and surface illuminance, bit for bit.
        //
        // Index 0 is the atmosphere sun because the engine renders exactly one directional light and
        // Scene::OnUpdate says so with an error when a scene holds more. This runs AFTER the frame's
        // ProceduralSkyCommand (Scene::OnUpdate executes the command buffers before calling us), so the
        // transmittance is this frame's sun, not last frame's.
        if ( !m_DirectionLights.DirectionLights.empty() )
        {
            const glm::vec3 transmittance = skyboxSystem->GetAtmosphere().SunTransmittanceAtGround;

            glm::vec4& colorIntensity = m_DirectionLights.DirectionLights[0].ColorIntensity;
            colorIntensity.x *= transmittance.x;
            colorIntensity.y *= transmittance.y;
            colorIntensity.z *= transmittance.z;
        }

        // Bake/rebake the procedural-sky IBL if the sun moved (throttled). Done here — before the render
        // graph records its command buffer — so the heavy compute + device idle stays at a safe boundary.
        {
            DESERT_PROFILE_PASS( "Sky: EnsureProceduralEnv" );
            skyboxSystem->EnsureProceduralEnvironment( sceneRenderInfo.RealTimestep.GetSeconds() );
        }

        // Recompute CSM cascade matrices once per frame BEFORE the render graph records (a cascade pass computes
        // nothing: its Declare and record only read the matrices).
        {
            DESERT_PROFILE_PASS( "Shadow: UpdateCascades" );
            UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->UpdateCascades();
        }

        // The terrain's frame data, once, before any of its three passes records (the cascades in
        // DepthPrePass, the forward pass in Geometry, the G-buffer fill after the graph).
        {
            DESERT_PROFILE_PASS( "Terrain: PrepareFrame" );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact
            // type
            // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
            UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->PrepareFrame();
            // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
        }

        // THE FRAME IS A GRAPH. Every GPU pass of this view from here on is one graph node (Raster, Compute or
        // Copy) that declares what it reads and writes; the graph orders them by those declarations and places
        // every barrier and layout transition. A pass this frame does not run is not added. The lambdas run inside
        // Execute, after the whole graph is built, so a value one pass hands a later one travels through `values`,
        // which outlives Execute; everything else they need is captured by value.
        RDG::Builder graph( "SceneView" );
        graph.SetPassCulling( !m_DebugView.DisablePassCulling );

        // THE ONE PER-FRAME VIEW (TAA1 step 3). Every pass below that needs a matrix, the camera position or a
        // previous-frame value reads `frame`; nothing reads the camera for them again. A refused frame builds no
        // graph: SceneViewState treats it as never having happened.
        ViewInputs inputs;
        if ( const auto* cam = GetMainCamera() )
        {
            inputs.View           = cam->GetViewMatrix();
            inputs.Projection     = cam->GetProjectionMatrix();
            inputs.CameraPosition = cam->GetPosition();
            inputs.NearPlane      = cam->GetNear();
            inputs.FarPlane       = cam->GetFar();
            inputs.CameraIdentity = MakeViewCameraIdentity( m_SceneGeneration, cam->GetSourceId() );
        }
        inputs.CameraCut                         = m_CameraCutPending;
        inputs.SceneIdentity                     = m_SceneGeneration;
        inputs.Output                            = m_ViewExtent;
        // THE ONE PER-VIEW RESOLUTION (ResolveViewResolution): the resolved RenderScalePercent and Upscaler,
        // the method they select, clamped to a split the method's upscaler supports. Its refusal refuses the
        // frame by name; a clamp is said. The view's temporal upscaler is made for the resolved method here.
        using Common::Scalability::Parameter;
        const Common::ResultStr<ViewResolution> resolved = ResolveViewResolution(
             m_ViewExtent, m_Quality.As<int>( Parameter::RenderScalePercent ), m_DebugView.ScreenPercentage,
             m_RenderedAntiAliasing, m_Quality.As<Common::Scalability::Upscaler>( Parameter::Upscaler ),
             [this]( const TemporalMethod method ) -> const ITemporalUpscaler*
             {
                 EnsureTemporalUpscaler( method );
                 return m_TemporalUpscaler.get();
             } );
        if ( !resolved )
        {
            LOG_ERROR( "[SceneRenderer] {}: the view's resolution is refused, frame not drawn: {}",
                       m_ViewResources.GetName(), resolved.GetError() );
            return;
        }
        if ( !resolved.GetValue().Clamped.empty() )
            LOG_WARN( "[SceneRenderer] {}: {}", m_ViewResources.GetName(), resolved.GetValue().Clamped );
        inputs.RenderScalePercent                = resolved.GetValue().Split.RenderScalePercent;
        inputs.AntiAliasing                      = m_RenderedAntiAliasing;
        inputs.Upscaler                          = resolved.GetValue().Upscaler; // the view's, not the setting's
        inputs.Quality                           = m_TemporalAAQuality;
        inputs.TimeSeconds                       = m_SceneTimeSeconds;
        m_LastRenderScalePercent                 = inputs.RenderScalePercent;
        const Common::ResultStr<ViewFrame> begun = m_ViewState.BeginFrame( inputs, m_TemporalUpscaler.get() );
        if ( !begun )
        {
            LOG_ERROR( "[SceneRenderer] {}: the view refused this frame: {}", m_ViewResources.GetName(),
                       begun.GetError() );
            return;
        }
        m_CameraCutPending     = false;
        const ViewFrame& frame = begun.GetValue();
        // THE RENDER SET AT THE FRAME'S SPLIT (ViewTargetSet::Render): a scale change resizes the scene's targets
        // here, before any node reads them; the output set stays at m_ViewExtent (Resize).
        ResizeRenderTargets( frame.Split.Render );
        // GetViewFrame() answers `frame` from here until OnUpdate returns, by every path (the destructor clears
        // it), so a writer can never read a finished frame's view.
        struct CurrentViewFrameScope
        {
            const ViewFrame*& Slot;
            ~CurrentViewFrameScope()
            {
                Slot = nullptr;
            }
        };
        m_CurrentViewFrame = &frame;
        const CurrentViewFrameScope currentViewFrameScope{ m_CurrentViewFrame };
        if ( const Common::BoolResultStr physical =
                  m_ViewState.History().AllocatePhysical( DeviceImageFactory{}, RendererGraphImageImporter{} );
             !physical )
        {
            LOG_ERROR( "[SceneRenderer] {}: the view's temporal history has no images: {}",
                       m_ViewResources.GetName(), physical.GetError() );
            return;
        }
        FrameTextures textures( graph );
        ImportSceneViewTextures( textures );
        // The view's velocity: one transient of this graph at the RENDER EXTENT (frame.Split.Render: the scene
        // target and the G-buffer are resized to it by ResizeRenderTargets), a colour slot of the scene target
        // (SceneTargetLayout, slot kSceneTargetVelocitySlot) and of the G-buffer (GBufferLayout, slot
        // kGBufferVelocitySlot) — never of a light view (RSM, cascades) or a debug target. Created before any node
        // is added; its first writer (ClearMainFramebuffer, the first node on both paths) clears it to no motion.
        // A target at another extent than the view's is refused by name: the frame is not drawn.
        if ( m_TargetFramebuffer )
        {
            const FramebufferSpecification& target = m_TargetFramebuffer->GetSpecification();
            std::string mismatch = ViewTargetExtentMismatch( frame.Split.Render.Width, frame.Split.Render.Height,
                                                             "scene target", target.Width, target.Height );
            if ( mismatch.empty() && m_GBuffer )
                mismatch = ViewTargetExtentMismatch( frame.Split.Render.Width, frame.Split.Render.Height,
                                                     "G-buffer", m_GBuffer->GetSpecification().Width,
                                                     m_GBuffer->GetSpecification().Height );
            if ( !mismatch.empty() )
            {
                LOG_ERROR( "[SceneRenderer] view '{}' frame refused: {}", m_ViewResources.GetName(), mismatch );
                return;
            }
            const ViewVelocity velocity = CreateViewVelocity(
                 graph, RDG::Extent3D{ frame.Split.Render.Width, frame.Split.Render.Height, 1 }, target.Samples );
            textures.Transients.Velocity = velocity.Resolved;
            textures.AddGraphColor( m_TargetFramebuffer, VelocityColor( velocity, target.Samples ) );
            textures.AddGraphColor( m_GBuffer, VelocityColor( velocity, 1 ) );
        }
        const auto values = std::make_shared<FrameValues>();

        // The view's per-primitive motion rows (current + previous world, both bone palettes) from the view's
        // MotionHistory, built once before any pass is declared: every view pass of this frame reads the same
        // rows.
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* const meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            meshes->SetPrevWorldTimeSeconds( GetViewFrame()->PrevTimeSeconds );
            if ( const Common::BoolResultStr rows = meshes->BuildObjectMotions( m_ViewState.Motion() ); !rows )
            {
                LOG_ERROR( "[SceneRenderer] {}: the view's motion rows could not be built; nothing is drawn this "
                           "frame: {}",
                           m_ViewResources.GetName(), rows.GetError() );
                return;
            }
        }

        const auto sceneColor = [this, &textures]()
        {
            return textures.Refs(
                 { { m_TargetFramebuffer ? m_TargetFramebuffer->GetColorAttachmentImage( 0 ) : nullptr,
                     "SceneColor" } } );
        };

        AddFrameClearMainFramebuffer( graph, textures );

        AddFrameParticlesSimulate( graph );

        AddFrameCloudShadowMap( graph, textures );

        // The opaque raster of the systems, in this order: the cascade depths (the geometry's shadow lookups read
        // them), the scene target's sky and geometry, the outline mask.
        AddFrameShadowDepths( graph, textures );
        AddFrameBasePass( graph, textures );
        AddFrameSilhouette( graph, textures );

        // Deferred: fill the G-buffer, then shade it (or show a debug channel) into the scene target before
        // the post chain.
        if ( m_RenderPath == Core::RenderPath::Deferred && m_GBuffer )
        {
            auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            const std::vector<RDG::TextureRef> gbuffer = textures.Colors( m_GBuffer, "GBuffer" );

            AddFrameGBuffer( graph, textures, meshRenderer );
            AddFrameTerrainGBuffer( graph, textures );
            AddFrameDepthResolve( graph, textures );

            glm::vec4 lightDir( 0.0f, -1.0f, 0.0f, 0.0f );
            glm::vec4 lightColor( 1.0f, 0.98f, 0.92f, 3.0f );
            if ( const auto& dl = m_DirectionLights.DirectionLights; !dl.empty() )
            {
                lightDir   = dl[0].Direction;
                lightColor = dl[0].ColorIntensity;
            }
            AddFrameSSAO( graph, textures, gbuffer, frame );

            RDG::TextureRef giAccum;

            if ( m_GIMode == Core::GIMode::RSM && meshRenderer != nullptr && EnsureGIResources() )
            {
                const std::vector<RDG::TextureRef> rsm = textures.Colors( m_RSMBuffer, "RSM" );
                const glm::vec3                    sunDir( lightDir );
                AddFrameRSM( graph, textures, meshRenderer, sunDir );
                m_RSMFrameCounter = ( m_RSMFrameCounter + 1 ) % kRSMRefreshEvery;

                giAccum = AddFrameGIResolve( graph, textures, gbuffer, rsm, meshRenderer, frame, lightColor );
            }

            // The cascades and the cloud shadow map reach the composite as scene view inputs (block entries
            // with their neutral defaults), not as a second, separately resolved read list.
            AddFrameComposite( graph, textures, gbuffer, giAccum, meshRenderer, lightDir, lightColor, frame );
            AddFrameGeneric( graph, textures, meshRenderer );
            AddFrameSkinned( graph, textures, meshRenderer );

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* copy = UNIQUE_GET_AS( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] );
            const RDG::TextureRef sceneCopy = AddFrameSceneCopy( graph, textures, sceneColor(), copy );

            // SSR traces the copy made by the pass above; without a copy target there is nothing to trace.
            if ( m_EnableSSR && sceneCopy.IsValid() && EnsureSSRResources() )
                AddFrameSSR( graph, textures, gbuffer, sceneCopy, frame );

            AddFrameGlass( graph, textures, sceneCopy, meshRenderer );
        }

        AddFrameSceneDepthResolve( graph, textures );
        AddFrameSkyAtmosphereLuts( graph, textures );
        AddFrameAtmosphericFog( graph, textures );
        AddFrameVolumetricClouds( graph, textures, frame );

        // The opaque scene is complete (lit, composited, the sky/fog/cloud nodes declared): passes from outside
        // the engine that belong under every translucent layer join here.
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::AfterOpaque, {} );

        // The translucency, the debug lines and the UI canvas run AFTER the deferred lighting composite so lit
        // geometry does not paint over them, and as LOAD nodes so a CLEAR never wipes the depth later overlays
        // test against (the particle top-down bug / grid-through-meshes).
        AddFrameTranslucency( graph, textures );

#if DESERT_DEV_INSTRUMENTS
        if ( m_DebugView.DeferredDebug == DeferredDebugMode::Overdraw )
        {
            AddFrameOverdraw( graph, textures );
        }
#endif // DESERT_DEV_INSTRUMENTS

        // After the last node that draws the scene geometry's velocity into the scene target (the translucency),
        // before its one reader, the temporal node. The overlays and post nodes below never write velocity (their
        // fragment shaders do not write slot 1: colour write mask 0, VulkanPipeline::CreateColorBlendState).
        AddFrameGraphColorResolves( graph, textures );

        // THE TEMPORAL RESOLVE (TAA1-B 5c). Everything after it reads its output and the overlays draw into
        // it - never into the history, never into the pre-resolve scene colour. Overlays (debug lines, grid,
        // gizmos, UI) are UNJITTERED: their shaders read ViewFrame::ViewProjection or the camera's matrices, never
        // JitteredViewProjection (Camera.hpp WHICH MATRIX). Without a temporal method the post input is the scene
        // colour, as before.
        const RDG::TextureRef              exposurePrevious = PrepareFrameAutoExposure( textures );
        const OverlayTargets               overlay = AddFrameTemporal( graph, textures, frame, exposurePrevious );
        const std::vector<RDG::TextureRef> postInput =
             overlay.IsValid() ? std::vector<RDG::TextureRef>{ overlay.Color } : sceneColor();
        AddFrameVelocityView( graph, textures, postInput.empty() ? RDG::TextureRef{} : postInput.front() );

#if DESERT_DEV_INSTRUMENTS
        // The engine's debug lines (bounding boxes), the first overlay.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ) )
            AddSystemRaster( graph, textures, mesh->DebugLinesPass(), overlay );
#endif // DESERT_DEV_INSTRUMENTS
       // The editor's authoring overlays (colliders, cubemap preview) over the engine's debug lines.
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::Overlay, overlay );

        // The pyramid the UI samples. The UI pass that samples it declares that read itself (the editor's UI pass,
        // through ExtensionPass::Declare), and the graph orders it after the blur.
        if ( m_BackdropBlurNeeded )
            AddFrameBackdropBlur( graph, textures, postInput );

        // The UI canvas, last before the post chain.
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::UI, overlay );

        AddFrameJumpFlood( graph, textures );
        AddFrameAutoExposure( graph, textures, postInput, exposurePrevious );
        if ( m_BloomEnabled )
        {
            AddFrameBloom( graph, textures, postInput );
        }
        AddFrameLightShafts( graph, textures, postInput, values );
        AddFrameLensFlare( graph, textures, postInput, values );
        AddFrameTonemap( graph, textures, postInput.empty() ? RDG::TextureRef{} : postInput.front() );

        if ( m_AAMode == Common::Scalability::AntiAliasingMethod::FXAA )
        {
            AddFrameFXAA( graph, textures );
        }
        else if ( m_AAMode == Common::Scalability::AntiAliasingMethod::SMAA )
        {
            AddFrameSMAA( graph, textures );
        }
        // The final image is sampled after the graph (editor viewport, runtime blit): the graph ends it there.
        if ( const auto extracted =
                  textures.ExtractImported( GetFinalImage(), "final image", RDG::Access::SampledGraphics );
             !extracted )
        {
            LOG_ERROR( "SceneRenderer: frame graph '{}' cannot hand over its final image: {}", graph.GetName(),
                       extracted.GetError() );
        }
        else
        {
            // What the viewport / runtime blit shows: without its writer this frame has no picture (black).
            graph.SetFaultPolicy( textures.Import( GetFinalImage(), "final image" ),
                                  RDG::ExternalFaultPolicy::FrameFatal );
        }

        // Its faults are logged by the graph backend and its own failures by ExecuteGraph; a FrameFault leaves
        // the final image black for this frame.
        // A FrameFault never reaches EndFrame: the next BeginFrame still sees the last committed frame as
        // previous. A frame that executed commits as previous with what its report says it lost.
        if ( Renderer::ExecuteGraph( graph ).IsSuccess() )
            m_ViewState.EndFrame( graph.GetExecuteReport() );
        textures.ResetInvalidatedHistories();
    }

    NO_DISCARD Common::BoolResultStr SceneRenderer::EndScene()
    {
        const ActiveViewScope viewScope( m_ViewResources ); // same reason as OnUpdate: phases interleave

        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->ClearQueues();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->ClearQueue();

        m_PointLight.PointLights.clear();
        m_SpotLight.SpotLights.clear();
        m_FrameExtensions = nullptr;

        return BOOLSUCCESS;
    }

    // Capability gate shared by the two Ensure* helpers. Both SSR and RSM-GI accumulate into RGBA32F
    // targets that are sampled and blended, so a device that cannot do that cannot run either feature.
    //
    // IT ASKS ABOUT THE FORMAT IT ACTUALLY USES. This used to read the cached
    // `DeviceCapabilities::SupportsFloatRenderTargets` bool while the comment above it claimed to "read
    // the introspection layer (Engine::Device) rather than assuming — the whole point of it". The bool
    // IS an assumption: VulkanDevice computes it once at init for one hardcoded format
    // (VK_FORMAT_R32G32B32A32_SFLOAT) against one hardcoded feature set, so every later reader inherits
    // whichever format the initialiser happened to pick. Reading it here was a comment describing the
    // code somebody meant to write.
    //
    // `Device::IsFormatSupported` is that code, and until now it had no caller anywhere — it stood in
    // the PureVirtualCensus register, and its own doc comment says "Prefer this over adding another
    // Supports<Feature> bool", which made the recommended question the one nobody asked. The three bits
    // below map exactly onto the three VkFormatFeatureFlags the cached bool hardcodes, so the ANSWER is
    // unchanged today; what changes is that the question now names its format, and a second float target
    // in another format gets a truthful answer instead of this one's.
    bool SceneRenderer::HasFloatRenderTargetSupport() const
    {
        const auto device = EngineContext::GetInstance().GetDevice();
        if ( !device )
            return false;
        return device->IsFormatSupported( ViewTargetFormats::kSceneColor,
                                          static_cast<Engine::FormatUsage>( Engine::FormatUsage_Sampled |
                                                                            Engine::FormatUsage_ColorAttachment |
                                                                            Engine::FormatUsage_Blendable ) );
    }

    // Both Ensure* helpers below are LAZY on purpose: every PreviewViewport owns a SceneRenderer, so
    // allocating these eagerly multiplied six full-screen RGBA32F targets (+ a 5-attachment RSM, + the
    // systems' own ping-pong accumulation pairs) by the number of live previews — for features a preview
    // never enables. They run outside any render pass, and go through WaitDeviceIdle before touching
    // GPU resources, matching what Resize() below does. A failure is non-fatal and latched, so a broken
    // shader cannot make this retry every frame.

    bool SceneRenderer::EnsureGIResources()
    {
        if ( m_GIResourcesReady )
            return true;
        // A preview profile never builds GI targets; the scene's GI mode stays on the screen-space path.
        if ( m_GIResourcesFailed || !m_ViewProfile.GlobalIllumination )
            return false;

        // ASK before allocating. The GI resolve and its temporal history are RGBA32F targets that get
        // sampled and blended; on a device without blendable float attachments the framebuffers would be
        // created and only fail later, deep in a pass.
        if ( !HasFloatRenderTargetSupport() )
        {
            LOG_WARN( "[SceneRenderer] RSM GI needs blendable float render targets, which this device does "
                      "not report — staying on the screen-space GI path." );
            m_GIResourcesFailed = true;
            return false;
        }

        Renderer::GetInstance().WaitDeviceIdle();

        // Reflective Shadow Map: a G-buffer rendered from the sun by the G-buffer program's DESERT_GBUFFER_RSM
        // permutation. Its colour slots are ViewTargetFormats::kRSMColourSlots — the one list the RSM pipeline's
        // target layout is built from too (MeshRenderer::SetupGBufferPass) — albedo (flux colour), normal, an
        // UNUSED slot 2 (no image: the permutation writes no shading word, VPL positions come from the depth) and
        // emissive. Fixed light-space resolution, so it does NOT resize with the viewport.
        FramebufferSpecification rsmSpec;
        rsmSpec.DebugName = "RSM";
        for ( const std::optional<Core::Formats::ImageFormat>& slot : ViewTargetFormats::kRSMColourSlots )
            rsmSpec.Attachments.Attachments.push_back( slot ? FramebufferAttachment( *slot )
                                                            : FramebufferAttachment::UnusedColourSlot() );
        rsmSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kRSMDepth );
        m_RSMBuffer = Graphic::Framebuffer::Create( rsmSpec );
        m_RSMBuffer->Resize( kRSMResolution, kRSMResolution );

        // The gather is a per-frame graph transient (AddFrameGIResolve); the system reads the scene target's size.
        RegisterSystem<System::GIResolveRenderer>( "GISystem", this, m_TargetFramebuffer );
        if ( !SP_CAST( System::GIResolveRenderer, m_RenderSystems["GISystem"] )->Initialize() )
        {
            LOG_WARN( "[SceneRenderer] GI resolve system unavailable — RSM GI produces no indirect light." );
            m_GIResourcesFailed = true;
            m_RSMBuffer.reset();
            return false;
        }

        m_GIResourcesReady = true;
        return true;
    }

    bool SceneRenderer::EnsureSSRResources()
    {
        if ( m_SSRResourcesReady )
            return true;
        if ( m_SSRResourcesFailed || !m_ViewProfile.ScreenSpaceReflections )
            return false;

        // Same gate as GI: the trace target and its ping-pong history are sampled/blended float targets
        // (ViewTargetFormats::kSSRTrace / kSSRAccum).
        if ( !HasFloatRenderTargetSupport() )
        {
            LOG_WARN( "[SceneRenderer] SSR needs blendable float render targets, which this device does not "
                      "report — reflections stay off." );
            m_SSRResourcesFailed = true;
            return false;
        }

        Renderer::GetInstance().WaitDeviceIdle();

        RegisterSystem<System::SSRRenderer>( "SSRSystem", this, m_TargetFramebuffer );
        const auto& ssrSys = SP_CAST( System::SSRRenderer, m_RenderSystems["SSRSystem"] );
        if ( !ssrSys->Initialize() )
        {
            LOG_WARN( "[SceneRenderer] SSR system unavailable." );
            m_SSRResourcesFailed = true;
            return false;
        }

        m_SSRResourcesReady = true;
        return true;
    }

    void SceneRenderer::Resize( const uint32_t width, const uint32_t height )
    {
        // A SIZE THAT NO TARGET CAN BE BUILT AT IS SKIPPED, and `width == 0 && height == 0` was not that
        // test. It let through exactly the sizes the UI actually produces: a collapsed dock panel is 0 on ONE
        // side, and a panel dragged shut hands out a negative ImGui float that becomes ~4.29e9 on the cast to
        // uint32_t. Both reached vmaCreateImage, which refuses a 0-pixel or 4-billion-pixel image with
        // VK_ERROR_INITIALIZATION_FAILED -- and VK_CHECK_RESULT turns that refusal into a debugger break. The
        // extent is NOT recorded either: committing an unusable extent would make the next Resize to the real
        // size compare equal to it and return early, leaving the targets at whatever they were last built at.
        if ( !IsUsableViewExtent( width, height ) )
            return;
        // Same size: nothing to rebuild. The thumbnail renderer resizes to the extent it was built at so
        // that its camera turns square, and a rebuild would idle the device for identical targets.
        if ( m_ViewExtent == ViewExtent{ width, height } )
            return;
        // A LIVE VIEW'S RESIZE IS NEVER REFUSED (Engine::ViewBudget::ResizeOverrunBytes): the view is on
        // screen, and refusing its size is what left the main view at its 64x64 placeholder on a full device.
        // Going past the budget is said once per size, since the panel asks again every frame it stays that
        // size; admission of a NEW view is where the budget says no.
        if ( m_TargetFramebuffer )
        {
            const ViewExtent requested{ width, height };
            if ( const auto overrun =
                      DescribeResizeOverrun( m_ViewResources.GetName(), m_ViewProfile, m_ViewExtent, requested ) )
            {
                if ( !( m_OverrunWarnedAt == requested ) )
                    LOG_WARN( "[ViewBudget] {}.", *overrun );
                m_OverrunWarnedAt = requested;
            }
        }
        m_ViewExtent = ViewExtent{ width, height };
        // Before the first build there is no target yet and the extent is all there is to update: the
        // build reads it.
        if ( !m_TargetFramebuffer )
            return;
        auto& renderer = Renderer::GetInstance();
        // Ensure all in-flight GPU work is done before destroying/recreating Vulkan resources
        // (framebuffers, descriptor pools). Resize can be triggered from UI code while a command
        // buffer is still recording or submitted frames are executing.
        renderer.WaitDeviceIdle();
        renderer.ResizeWindowEvent( width, height );
        // THE OUTPUT SET (ViewTargetSet::Output): the post-process chain after the temporal resolve.
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )->Resize( width, height );
        UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Resize( width, height );
        UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Resize( width, height );
        // THE RENDER SET at the last accepted frame's scale; the next frame's own split corrects it if the
        // scale changed (OnUpdate -> ResizeRenderTargets( frame.Split.Render )).
        if ( const auto split = MakeResolutionSplit( m_ViewExtent, m_LastRenderScalePercent ) )
            ResizeRenderTargets( split.GetValue().Render );
        else
            ResizeRenderTargets( m_ViewExtent );
    }

    void SceneRenderer::ResizeRenderTargets( const ViewExtent render )
    {
        if ( !m_TargetFramebuffer || m_RenderExtent == render )
            return;
        m_RenderExtent        = render;
        const uint32_t width  = render.Width;
        const uint32_t height = render.Height;
        Renderer::GetInstance().WaitDeviceIdle();
        m_TargetFramebuffer->Resize( width, height );
        if ( m_GBuffer )
            m_GBuffer->Resize( width, height );
        if ( auto resolve =
                  SP_CAST( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] ) )
            resolve->Resize( width, height );
        // m_RSMBuffer is deliberately NOT resized: it is a fixed-resolution light-space target, unrelated
        // to the viewport. Its accumulation history is invalidated by the GI system's own size check.

        // The silhouette mask and the overdraw target are drawn with the scene's meshes: the scene's extent.
        if ( const auto& maskFb = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
                                       ->GetSilhouetteMaskFramebuffer() )
            maskFb->Resize( width, height );

#if DESERT_DEV_INSTRUMENTS
        if ( const auto& overdrawFb =
                  UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetOverdrawFramebuffer() )
            overdrawFb->Resize( width, height );
#endif

        // The outline's composite reads the scene target: the scene's extent.
        UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] )
             ->OnResize( width, height );
    }

    void SceneRenderer::SubmitMesh( const Mesh* mesh, const MaterialSlotBindingPtr& materialSlots,
                                    const glm::mat4& transform, const RenderSubmissionExtra& extra )
    {
        if ( !mesh || !materialSlots || materialSlots->Slots.empty() )
        {
            return;
        }
        // SubmitMesh's const is the caller-facing contract; MeshRenderData and the RenderMesh chain below it
        // still carry a mutable Mesh* and never write through it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        auto* drawnMesh = const_cast<Mesh*>( mesh );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitMesh( { .Entity                   = extra.Entity,
                             .MotionPart               = extra.MotionPart,
                             .Mesh                     = drawnMesh,
                             .Transform                = transform,
                             .MaterialSlots            = materialSlots,
                             .BoneMatrices             = extra.BoneMatrices,
                             .Outlined                 = extra.Outlined,
                             .HiddenSubmeshes          = extra.HiddenSubmeshes,
                             .ForcedLOD                = extra.ForcedLOD,
                             .LODBias                  = extra.LODBias,
                             .CastShadows              = extra.CastShadows,
                             .ReceiveShadows           = extra.ReceiveShadows,
                             .TranslucencySortPriority = extra.TranslucencySortPriority } );
    }

    void SceneRenderer::SubmitLandscapeTile( Image2D* heightmap, const System::LandscapeTileDraw& tile,
                                             const MaterialOverrides&           overrides,
                                             const System::LandscapeWeightDraw& weights )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )
             ->Submit( { .Heightmap = heightmap, .Landscape = tile, .Weights = weights, .Overrides = overrides } );
    }

    void SceneRenderer::SubmitGenericMesh( const uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                           const std::string& shaderName, const MaterialOverrides& overrides,
                                           bool outlined, Image2D* directTexture,
                                           const std::string& directTextureSampler, bool castShadows )
    {
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitGenericMesh( { .Entity               = entity,
                                    .Mesh                 = mesh,
                                    .Transform            = transform,
                                    .ShaderName           = shaderName,
                                    .Overrides            = overrides,
                                    .Outlined             = outlined,
                                    .CastShadows          = castShadows,
                                    .DirectTexture        = directTexture,
                                    .DirectTextureSampler = directTextureSampler } );
    }

    void SceneRenderer::SubmitSlotMaterialMesh( const uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                                Material* material, uint64_t visibleSubmeshMask, bool outlined,
                                                bool castShadows, const uint32_t motionPart )
    {
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitGenericMesh( { .Entity             = entity,
                                    .MotionPart         = motionPart,
                                    .Mesh               = mesh,
                                    .Transform          = transform,
                                    .Outlined           = outlined,
                                    .CastShadows        = castShadows,
                                    .SlotMaterial       = material,
                                    .VisibleSubmeshMask = visibleSubmeshMask } );
    }

    uint32_t SceneRenderer::GetIsmInstancesDrawn() const
    {
        const auto found = m_RenderSystems.find( "MeshSystem" );
        if ( found == m_RenderSystems.end() )
            return 0;
        return static_cast<const System::MeshRenderer*>( found->second.get() )->GetIsmInstancesDrawn();
    }

    void SceneRenderer::SubmitInstancedMesh( Mesh* mesh, const MaterialInstancePtr& material,
                                             const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                             bool castShadows, const InstanceCullDistance& cullDistance,
                                             const InstanceWind& wind )
    {
        // NO CAST, AND THAT IS THE POINT. This used to read
        // `static_cast<Desert::StaticMesh*>( const_cast<Mesh*>( mesh ) )`, and the downcast was a lie
        // the type told: an ISM's mesh is very often a DynamicMesh — every primitive one is, and the
        // component's own `RuntimeMesh` member is a `shared_ptr<DynamicMesh>`. The queue never used it
        // as a StaticMesh (RenderMesh takes a Mesh), so nothing broke; the type simply claimed
        // something untrue, and a reader who believed it would reach for members that are not there.
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitInstancedMesh( { .Mesh         = mesh,
                                      .Material     = material,
                                      .Transforms   = transforms,
                                      .CastShadows  = castShadows,
                                      .CullDistance = cullDistance,
                                      .Wind         = wind } );
    }

    void SceneRenderer::SetOutlineSettings( const glm::vec3& color, float width, float smoothness, bool enabled )
    {
        const auto& jumpFloodSystem =
             UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
        jumpFloodSystem->SetEnabled( enabled );
        jumpFloodSystem->SetOutlineColor( color );
        jumpFloodSystem->SetOutlineWidth( width );
        jumpFloodSystem->SetOutlineSmoothness( smoothness );
    }

    void SceneRenderer::SetEnvironment( const std::shared_ptr<MaterialSkybox>& material, const SkyLook& look )
    {
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->PrepareMaterial( material, look );
    }

    void SceneRenderer::SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow,
                                          const SkySettings& sky, const SunLightFx& fx )
    {
        m_SunLightFx = fx;
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->SetProceduralSky( enabled, sunDir, bakeNow, sky, fx );
    }

    const AtmosphereEnv& SceneRenderer::GetAtmosphere() const
    {
        return UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems.at( "SkyboxSystem" ) )->GetAtmosphere();
    }

    CloudEnvironmentBake SceneRenderer::BuildCloudEnvironmentBake()
    {
        return UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
             ->BuildEnvironmentBake();
    }

    bool SceneRenderer::IsCloudVolumeBaking() const
    {
        // `find` rather than `operator[]`, exactly as GetCloudShadowInput does it: this is a const observer
        // and must not insert an empty system into the map on the way to answering "is there one".
        const auto it = m_RenderSystems.find( "VolumetricCloudSystem" );
        if ( it == m_RenderSystems.end() )
            return false;

        const auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, it->second );
        return clouds && clouds->IsModellingVolumeBaking();
    }

    float SceneRenderer::CloudVolumeBakeProgress() const
    {
        const auto it = m_RenderSystems.find( "VolumetricCloudSystem" );
        if ( it == m_RenderSystems.end() )
            return 0.0f;

        const auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, it->second );
        return clouds ? clouds->ModellingBakeProgress() : 0.0f;
    }

    std::optional<Environment> SceneRenderer::GetEnvironment()
    {
        return UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )->GetEnvironment();
    }

    std::shared_ptr<Image2D> SceneRenderer::GetShadowCascadeImage( uint32_t cascade )
    {
        return UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->GetCascadeShadowImage( cascade );
    }

    uint32_t SceneRenderer::GetShadowCascadeCount()
    {
        return UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetCascadeCount();
    }

    void SceneRenderer::EnsureTemporalUpscaler( const TemporalMethod method )
    {
        const TemporalMethod held = m_TemporalUpscaler ? m_TemporalUpscaler->Method() : TemporalMethod::None;
        if ( held != method )
            m_TemporalUpscaler = CreateTemporalUpscaler( method );
    }

    SceneRenderer::OverlayTargets SceneRenderer::AddFrameTemporal( RDG::Builder& graph, FrameTextures& textures,
                                                                   const ViewFrame&      frame,
                                                                   const RDG::TextureRef exposure )
    {
        // The resolve of this frame: the temporal method's, the spatial upscale (below 100 % without one), and above
        // 100 % the fixed SSAA downsample after the temporal method, or alone without one (TemporalUpscaler.hpp
        // WHERE IT RUNS).
        const bool spatial     = IsSpatialUpscale( frame );
        const bool supersample = frame.Split.Mode == Common::Scalability::ScaleMode::Supersample;
        const bool temporal    = frame.Method != TemporalMethod::None && m_TemporalUpscaler != nullptr;
        if ( !m_TargetFramebuffer || ( !spatial && !supersample && !temporal ) )
            return {};
        // Every refusal below renders the frame WITHOUT the resolve and says so by name: an invalid set, so the
        // caller post-processes the scene colour and draws the overlays into the scene target.
        const auto withoutTemporal = [this]( const std::string& why ) -> OverlayTargets
        {
            LOG_ERROR( "[SceneRenderer] {}: rendered without the temporal resolve this frame: {}",
                       m_ViewResources.GetName(), why );
            return {};
        };
        const uint32_t samples = m_TargetFramebuffer->GetSpecification().Samples;
        if ( samples > 1 )
            return withoutTemporal( std::format( "the scene target has {} samples; the overlay target set after "
                                                 "the resolve is single-sample",
                                                 samples ) );
        if ( !m_PopulateSceneDepth )
            m_PopulateSceneDepth = std::make_unique<System::PopulateSceneDepthRenderer>();
        if ( const Common::BoolResultStr prepared = m_PopulateSceneDepth->Prepare(); !prepared )
            return withoutTemporal( prepared.GetError() );
        // Registered every frame the method runs: EndFrame reads which Current a fault lost. The spatial upscale
        // and the SSAA downsample have no history.
        const std::vector<HistoryRefs> histories =
             temporal ? m_ViewState.History().Register( graph ) : std::vector<HistoryRefs>{};
        const TemporalUpscalerInputs inputs{
             .SceneColor = textures.Import( m_TargetFramebuffer->GetColorAttachmentImage( 0 ), "SceneColor" ),
             .SceneDepth = textures.Depth( m_TargetFramebuffer, "SceneColor" ),
             .Velocity   = textures.Transients.Velocity,
             // No exposure node this frame: unit luminance (System.White), the weight of a neutral exposure.
             .Exposure = exposure.IsValid() ? exposure : textures.System.White,
             .History  = histories };
        RDG::TextureRef resolvedColor;
        if ( spatial )
        {
            const Common::ResultStr<RDG::TextureRef> upscaled =
                 m_SpatialUpscale.AddPasses( graph, frame, inputs.SceneColor );
            if ( !upscaled )
                return withoutTemporal( upscaled.GetError() );
            resolvedColor = upscaled.GetValue();
        }
        else
        {
            resolvedColor = inputs.SceneColor;
            if ( temporal )
            {
                const Common::ResultStr<TemporalUpscalerOutputs> added =
                     m_TemporalUpscaler->AddPasses( graph, frame, inputs );
                if ( !added )
                    return withoutTemporal( added.GetError() );
                resolvedColor = added.GetValue().SceneColor;
            }
            // Above 100 % the temporal output is at RenderExtent; the downsample brings it to OutputExtent, where the
            // overlay target set and the post chain are.
            if ( supersample )
            {
                const Common::ResultStr<RDG::TextureRef> downsampled =
                     m_SupersampleResolve.AddPasses( graph, frame, resolvedColor );
                if ( !downsampled )
                    return withoutTemporal( downsampled.GetError() );
                resolvedColor = downsampled.GetValue();
            }
        }
        // The post sharpen (Resolution.Sharpness) on the resolved colour, outside the history.
        const int sharpness = m_Quality.As<int>( Common::Scalability::Parameter::UpscalerSharpness );
        if ( SharpenRuns( frame, sharpness ) )
        {
            const Common::ResultStr<RDG::TextureRef> sharpened =
                 m_Sharpen.AddPasses( graph, frame, resolvedColor, sharpness );
            if ( !sharpened )
                return withoutTemporal( sharpened.GetError() );
            resolvedColor = sharpened.GetValue();
        }

        // THE OVERLAY TARGET SET (ViewTargetSet::Output): the resolved colour, and a velocity and a depth at the
        // output extent; "Scene: PopulateSceneDepth" fills the depth from the render-extent scene depth.
        OverlayTargets overlay;
        overlay.Color = resolvedColor;
        RDG::TextureDesc desc;
        desc.Size        = RDG::Extent3D{ frame.Split.Output.Width, frame.Split.Output.Height, 1 };
        desc.Format      = ViewTargetFormats::kVelocity;
        overlay.Velocity = graph.CreateTexture( desc, "Overlay.Velocity" );
        desc.Format      = ViewTargetFormats::kSceneDepth;
        overlay.Depth    = graph.CreateTexture( desc, "Overlay.SceneDepth" );
        const System::PopulateSceneDepthRenderer* populate = m_PopulateSceneDepth.get();
        graph.AddPass(
             "Scene: PopulateSceneDepth", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 populate->DeclareBindings( pass, inputs.SceneDepth );
                 pass.ColorTarget( 0, overlay.Velocity, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                 pass.DepthTarget( overlay.Depth, RDG::LoadOp::ClearDepth( Core::kDepthClear ), /*write*/ true );
             },
             [populate]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return populate->Record( context ); } );
        return overlay;
    }

    const std::shared_ptr<Desert::Graphic::Image2D> SceneRenderer::GetFinalImage()
    {
        // FXAA/SMAA write their own framebuffer downstream of tonemap; otherwise tonemap output IS final.
        const char* finalSystem = "TonemapSystem";
        if ( m_AAMode == Common::Scalability::AntiAliasingMethod::FXAA )
        {
            finalSystem = "FXAASystem";
        }
        else if ( m_AAMode == Common::Scalability::AntiAliasingMethod::SMAA )
        {
            finalSystem = "SMAASystem";
        }

        return std::static_pointer_cast<System::RenderSystem>( m_RenderSystems[finalSystem] )
             ->GetSystemFramebuffer()
             ->GetColorAttachmentImage();
    }

    void SceneRenderer::AddPointLight( const ShaderProtocols::PointLightPayload& pointLight )
    {
        m_PointLight.PointLights.push_back( pointLight );
    }

    void SceneRenderer::AddSpotLight( const ShaderProtocols::SpotLightPayload& spotLight )
    {
        m_SpotLight.SpotLights.push_back( spotLight );
    }

    void SceneRenderer::TrackRenderSystem( const std::string& name, std::shared_ptr<IRenderSystem> system )
    {
        // Replacing a system keeps the slot it already holds in the registration order: an editor tool
        // that re-registers its pass every time a setting changes must not walk to the back of the queue
        // and start drawing over neighbours it used to draw under.
        if ( m_RenderSystems.find( name ) == m_RenderSystems.end() )
            m_RenderSystemOrder.push_back( name );

        m_RenderSystems[name] = std::move( system );
    }

    void SceneRenderer::ForgetRenderSystem( const std::string& name )
    {
        m_RenderSystems.erase( name );
        m_RenderSystemOrder.erase( std::remove( m_RenderSystemOrder.begin(), m_RenderSystemOrder.end(), name ),
                                   m_RenderSystemOrder.end() );
    }

    void SceneRenderer::RegisterRenderSystem( const std::string& name, std::shared_ptr<IRenderSystem> system )
    {
        TrackRenderSystem( name, std::move( system ) );
    }

    void SceneRenderer::UnregisterRenderSystem( const std::string& name )
    {
        ForgetRenderSystem( name );
    }

    void SceneRenderer::SetHeightFog( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY )
    {
        UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )
             ->SetFogSettings( present, data, fogHeightY );
    }

    void SceneRenderer::SetVolumetricClouds( bool present, const ECS::VolumetricCloudData& data,
                                             const glm::vec3& windOffset, const glm::vec3& windDirection,
                                             const std::vector<HeroCloudInstance>& heroClouds )
    {
        UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
             ->SetCloudSettings( present && m_ViewProfile.VolumetricClouds, data, windOffset, windDirection,
                                 m_CloudQuality,
                                 heroClouds ); // a profile without clouds never allocates their targets
    }

    CloudShadowInput SceneRenderer::GetCloudShadowInput() const
    {
        CloudShadowInput cloudShadow;

        // `find` rather than `operator[]`: this is a const observer and must not insert an empty system
        // into the map on the way to answering "is there one".
        const auto it = m_RenderSystems.find( "VolumetricCloudSystem" );
        if ( it == m_RenderSystems.end() )
            return cloudShadow;

        auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, it->second );
        if ( !clouds || !clouds->HasShadowMap() )
            return cloudShadow;

        const CloudShadowMapView& view = clouds->GetShadowMapView();

        cloudShadow.HasMap     = clouds->GetShadowMapImage() != nullptr;
        cloudShadow.WorldToMap = view.WorldToMap;
        cloudShadow.FarDepthKm = view.FarDepthKm;
        cloudShadow.Strength   = clouds->GetShadowStrength();
        // FROM THE VIEW AND NOT FROM THE CONSTANT, because the quality tier scales the map's extent and
        // the fade is a fixed WORLD width across it — a consumer reading a fixed UV would put the
        // gradient in the wrong place on every tier but one.
        cloudShadow.BorderFadeUv = view.BorderFadeUv;
        cloudShadow.Enabled      = true;
        return cloudShadow;
    }

} // namespace Desert::Graphic