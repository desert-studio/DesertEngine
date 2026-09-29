#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
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
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // WHAT MAKES A REGISTERED SYSTEM THE SCENE'S RATHER THAN THE RENDERER'S, as one string. Two places
        // read it and they must not be able to disagree: ExternalSystemKey() stamps it onto every pass the
        // editor registers, and SceneRenderer::RebindScene() drops exactly the entries carrying it when a
        // different scene arrives. A prefix renamed in one of the two would leave the rebind matching
        // nothing while still compiling, and the previous scene's grid would keep drawing over the new one.
        constexpr std::string_view kExternalSystemPrefix = "External:";
    } // namespace

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

        // Ensure the phase registry exists before any system registers custom phases or passes.
        RenderPhaseRegistry::CreateInstance();

        // The surface's size, never the window's: see ViewExtent.
        const uint32_t width  = m_ViewExtent.Width;
        const uint32_t height = m_ViewExtent.Height;

        // Framebuffer. MSAA applies HERE only: every scene system renders into this target at N
        // samples and the render pass resolves to single-sample for the post stack. Read once —
        // pipelines bake their sample count, so a change applies on the next start.
        //
        // READ FROM THE MACHINE STORE DIRECTLY, not from m_Quality and not from a copy in RenderConfig.
        // Not m_Quality because SetQuality is a per-frame push and Init runs before the first one; not a
        // RenderConfig copy because that copy had exactly one writer, Editor::EditorPreferences, so a
        // packaged game ran with MSAA nailed to 1 whatever its player had chosen.
        FramebufferSpecification fbSpec;
        fbSpec.DebugName = "Composite framebuffer";
        fbSpec.Samples   = static_cast<uint32_t>( std::clamp( Common::Settings::MachineSettings::Get().MSAASamples,
                                                              1, RenderConfig::MaxMSAASamples.load() ) );
        // Validate against the device's actual sample MASK, not a hardcoded 1/2/4/8 list. Clamping to the
        // maximum is not enough: support is a bitmask, so a device can offer 1/4/8 and not 2 — the old
        // check accepted 2 there and the framebuffer failed to create. Fall back to the next lower
        // supported count rather than dropping straight to 1.
        {
            const uint32_t mask = EngineContext::GetInstance().GetCapabilities().MSAASampleMask;
            while ( fbSpec.Samples > 1 && !( mask & fbSpec.Samples ) )
                fbSpec.Samples >>= 1;
            if ( fbSpec.Samples < 1 )
                fbSpec.Samples = 1;
        }
        RenderConfig::MSAASamplesActive = static_cast<int>( fbSpec.Samples );
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
        // (RGBA32F for banding-free normals — the format enum has no RGBA16F yet); GBufferC = world position.xyz
        // (RGBA32F) so the lighting pass gets point/spot-light distances directly (bulletproof vs depth
        // reconstruction, which is error-prone under the GL-on-Vulkan depth conventions); shared depth.
        FramebufferSpecification gbufferSpec;
        gbufferSpec.DebugName = "GBuffer";
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferA ); // GBufferA Albedo+Metallic
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferB ); // GBufferB Normal+Roughness
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferC ); // GBufferC WorldPosition.xyz
        gbufferSpec.Attachments.Attachments.emplace_back(
             ViewTargetFormats::kGBufferEmissive ); // GBufferEmissive (HDR self-illum)
        // DEPTH32F for the same reason as the forward target above — and it is this attachment the
        // height fog reads back as a texture, so its precision is the precision of every distance it
        // reconstructs.
        gbufferSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kGBufferDepth );
        m_GBuffer = Graphic::Framebuffer::Create( gbufferSpec );
        m_GBuffer->Resize( width, height );

        // SSAO target: a single-channel-ish AO factor (RGBA8F, AO in .r) the deferred lighting reads.
        FramebufferSpecification ssaoSpec;
        ssaoSpec.DebugName = "SSAO";
        ssaoSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSSAO );
        m_SSAOBuffer = Graphic::Framebuffer::Create( ssaoSpec );
        m_SSAOBuffer->Resize( width, height );

        // Scene-colour snapshot (same format as the target) the glass pass samples for refraction.
        FramebufferSpecification copySpec;
        copySpec.DebugName = "SceneColorCopy";
        copySpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSceneColorCopy );
        m_SceneColorCopy = Graphic::Framebuffer::Create( copySpec );
        m_SceneColorCopy->Resize( width, height );

        // NOTE: SSR and RSM-GI resources are deliberately NOT created here — see EnsureSSRResources() /
        // EnsureGIResources(). Every PreviewViewport (asset thumbnails, the Details mesh preview) builds its
        // OWN SceneRenderer, so anything allocated in this constructor is paid for once PER PREVIEW. Between
        // them SSR and RSM-GI want six full-screen RGBA32F targets plus a five-attachment RSM, and a preview
        // never turns either feature on. They are now allocated on first actual use instead.

        // Scene systems render into the shared target framebuffer; post-process systems form an
        // explicit chain (Mesh silhouette mask -> Jump Flood outline -> Tonemap).
        RegisterSystem<System::SkyboxRenderer>( "SkyboxSystem", this, m_TargetFramebuffer, m_RenderGraphBuilder );
        RegisterSystem<System::MeshRenderer>( "MeshSystem", this, m_TargetFramebuffer, m_RenderGraphBuilder );
        RegisterSystem<System::JumpFloodOutlineRenderer>( "JumpFloodSystem", this, m_TargetFramebuffer,
                                                          m_RenderGraphBuilder );

        // NAMED AND SURVIVED, NOT VERIFIED. `DESERT_VERIFY( false )` stood at each of the nine sites
        // below, so a render system that refused to initialise took the whole process with it — and
        // after Г22 that became REACHABLE for the first time: a typo in StaticMeshPBR.shader now
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
        RegisterSystem<System::TerrainRenderer>( "TerrainSystem", this, m_TargetFramebuffer,
                                                 m_RenderGraphBuilder );
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
        RegisterSystem<System::TonemapRenderer>( "TonemapSystem", this, jumpFloodSystem->GetSystemFramebuffer(),
                                                 m_RenderGraphBuilder );
        const auto& tonemapSystem = SP_CAST( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
        if ( !tonemapSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] the tonemap system did not initialise; the viewport stays black." );

        // Backdrop blur: a blurred snapshot of the scene colour the UI canvas samples for "glass" panels.
        // Runs before the UI phase (which writes into this same target, so it cannot sample it directly).
        RegisterSystem<System::BackdropBlurRenderer>( "BackdropBlurSystem", this, m_TargetFramebuffer,
                                                      m_RenderGraphBuilder );
        if ( const auto& backdropSystem =
                  SP_CAST( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] );
             !backdropSystem->Initialize() )
        {
            LOG_WARN( "Backdrop blur unavailable — UI glass panels will draw as flat tint" );
        }

        // Bloom reads the HDR scene color and produces a compute mip-chain glow that tonemap adds in.
        RegisterSystem<System::BloomRenderer>( "BloomSystem", this, m_TargetFramebuffer, m_RenderGraphBuilder );
        const auto& bloomSystem = SP_CAST( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
        if ( !bloomSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] the bloom system did not initialise; the scene renders without glow." );

        // Light shafts: the atmosphere sun's screen-space streaks, masked and radially blurred from the
        // HDR scene colour; tonemap adds them in the way it adds bloom. Non-fatal: a sky without streaks
        // must never take a scene down.
        RegisterSystem<System::LightShaftRenderer>( "LightShaftSystem", this, m_TargetFramebuffer,
                                                    m_RenderGraphBuilder );
        const auto& lightShaftSystem = SP_CAST( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
        if ( !lightShaftSystem->Initialize() )
            LOG_WARN( "[SceneRenderer] Light shaft system unavailable." );

        // Lens flare: the camera's own response to the sun disc — ghosts, halo and streak gathered from
        // the same HDR scene colour, added in by the tonemap the way bloom is. Non-fatal, like the shafts.
        RegisterSystem<System::LensFlareRenderer>( "LensFlareSystem", this, m_TargetFramebuffer,
                                                   m_RenderGraphBuilder );
        const auto& lensFlareSystem = SP_CAST( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
        if ( const auto flareInit = lensFlareSystem->Initialize(); !flareInit )
            LOG_WARN( "[SceneRenderer] Lens flare system unavailable: {}", flareInit.GetError() );

        // SSAO (fullscreen G-buffer -> AO factor). Its target is the dedicated SSAO buffer; deferred lighting
        // reads the result. Runs in the manual chain only when Deferred. Non-fatal.
        RegisterSystem<System::SSAORenderer>( "SSAOSystem", this, m_SSAOBuffer, m_RenderGraphBuilder );
        if ( !SP_CAST( System::SSAORenderer, m_RenderSystems["SSAOSystem"] )->Initialize() )
            LOG_WARN( "[SceneRenderer] SSAO system unavailable." );

        RegisterSystem<System::CopyRenderer>( "SceneColorCopySystem", this, m_SceneColorCopy,
                                              m_RenderGraphBuilder );
        if ( !SP_CAST( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] )->Initialize() )
            LOG_WARN( "[SceneRenderer] Scene-color copy system unavailable (glass refraction off)." );

        // Atmosphere and fog: aerial perspective on opaque with exponential height fog over it — one
        // compute evaluation issued outside the graph (ExecuteAtmosphericFog) and one apply pass in the
        // Transparency phase, self-ordered below the particles by RenderPassOrder::AtmosphericFog.
        // Non-fatal: neither must ever take a scene down.
        RegisterSystem<System::HeightFogRenderer>( "HeightFogSystem", this, m_TargetFramebuffer,
                                                   m_RenderGraphBuilder );
        if ( const auto fogInit =
                  SP_CAST( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )->Initialize();
             !fogInit )
            LOG_WARN( "[SceneRenderer] Height fog system unavailable: {}", fogInit.GetError() );

        // Volumetric clouds: a march through a spherical shell, issued outside the graph
        // (ExecuteVolumetricClouds) with one composite pass in the Transparency phase, self-ordered above
        // the fog and below the particles by RenderPassOrder::FarField. Registered after the fog so that
        // if the two ever end up on the same rung the registration order breaks the tie the same way the
        // phase order already does. Non-fatal: a missing sky must never take a scene down.
        RegisterSystem<System::VolumetricCloudRenderer>( "VolumetricCloudSystem", this, m_TargetFramebuffer,
                                                         m_RenderGraphBuilder );
        if ( const auto cloudInit =
                  SP_CAST( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
                       ->Initialize();
             !cloudInit )
            LOG_WARN( "[SceneRenderer] Volumetric cloud system unavailable: {}", cloudInit.GetError() );

        // GPU particles: compute-simulated billboards drawn in the Transparency phase. Non-fatal.
        RegisterSystem<System::ParticleRenderer>( "ParticleSystem", this, m_TargetFramebuffer,
                                                  m_RenderGraphBuilder );
        if ( !SP_CAST( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->Initialize() )
            LOG_WARN( "[SceneRenderer] Particle system unavailable." );

        // Deferred lighting (fullscreen G-buffer shade + debug view). Runs in the manual chain, only when
        // RenderPath == Deferred. Non-fatal if it fails to init (deferred path is simply unavailable).
        RegisterSystem<System::DeferredLightingRenderer>( "DeferredLightingSystem", this, m_TargetFramebuffer,
                                                          m_RenderGraphBuilder );
        if ( !SP_CAST( System::DeferredLightingRenderer, m_RenderSystems["DeferredLightingSystem"] )
                   ->Initialize() )
            LOG_WARN( "[SceneRenderer] Deferred lighting system unavailable." );
        tonemapSystem->SetBloomImage( bloomSystem->GetBloomImage() );
        tonemapSystem->SetLightShaftImage( lightShaftSystem->GetShaftImage() );
        tonemapSystem->SetLensFlareImage( lensFlareSystem->GetFlareImage() );

        // Auto-exposure measures the HDR scene luminance into a 1x1 buffer that tonemap reads.
        RegisterSystem<System::AutoExposureRenderer>( "AutoExposureSystem", this, m_TargetFramebuffer,
                                                      m_RenderGraphBuilder );
        const auto& autoExposureSystem =
             SP_CAST( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
        if ( !autoExposureSystem->Initialize() )
            LOG_ERROR( "[SceneRenderer] auto-exposure did not initialise; exposure stays at its default." );
        tonemapSystem->SetAutoExposureImage( autoExposureSystem->GetAdaptedLuminanceImage() );

        // FXAA consumes the tonemapped image (LDR). It only runs when the machine's post AA is FXAA
        // (Common::Settings::MachineSettings::AA — it left SceneSettings with К3).
        RegisterSystem<System::FXAARenderer>( "FXAASystem", this, tonemapSystem->GetSystemFramebuffer(),
                                              m_RenderGraphBuilder );
        if ( !SP_CAST( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Initialize() )
            LOG_ERROR( "[SceneRenderer] FXAA did not initialise; the image is drawn without post AA." );

        // SMAA consumes the same tonemapped image. Runs only when the machine's post AA is SMAA.
        RegisterSystem<System::SMAARenderer>( "SMAASystem", this, tonemapSystem->GetSystemFramebuffer(),
                                              m_RenderGraphBuilder );
        if ( !SP_CAST( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Initialize() )
            LOG_ERROR( "[SceneRenderer] SMAA did not initialise; the image is drawn without post AA." );

        // The graph is NOT built here: RebindScene() runs immediately after and builds it once, over the
        // engine systems above plus whatever external passes survive. Building it twice on the first Init
        // would be the only place in the engine that did.
        return true;
    }

    void SceneRenderer::RebindScene()
    {
        // Everything below drops render systems, and a render system owns pipelines and descriptor pools
        // the last submitted frame may still be executing against. Same rule, same reason, as the five
        // sites that destroy a whole SceneRenderer (Desert/Tests/Engine/TeardownOrder).
        //
        // Paid on EVERY scene load even when there is nothing to drop. That is deliberate: the caller has
        // just cleared the entity registry and is about to destroy the RenderRegistry, so the frame that
        // was in flight when the load was requested is referencing objects on their way out either way.
        Renderer::GetInstance().WaitDeviceIdle();

        // THE PREVIOUS SCENE'S EDITOR PASSES. Collected first and erased after, because ForgetRenderSystem
        // mutates both containers being walked.
        //
        // Matched by the "External:" prefix that RegisterExternalPass itself stamps on — one place decides
        // what an external pass is called and one place decides what counts as one, so a rename cannot
        // leave this loop matching nothing while still compiling.
        std::vector<std::string> external;
        for ( const auto& name : m_RenderSystemOrder )
        {
            if ( name.starts_with( kExternalSystemPrefix ) )
                external.push_back( name );
        }
        for ( const auto& name : external )
            ForgetRenderSystem( name );

        if ( !external.empty() )
            LOG_INFO( "[SceneRenderer] Dropped {} external pass(es) belonging to the previous scene.",
                      external.size() );

        // ...and tell what is LEFT — the engine systems, which are the renderer's and stay — that the world
        // they have been accumulating over is gone. Most of them do nothing with it; the ones that do are a
        // temporal history and a per-entity GPU cache, and IRenderSystem::OnSceneReplaced says why those two
        // are the only kinds that can exist here.
        //
        // Walked over m_RenderSystemOrder rather than the map for the reason RebuildRenderGraph gives: the
        // map's operator[] accesses elsewhere in this file insert null entries, and the order vector never
        // holds one.
        for ( const auto& name : m_RenderSystemOrder )
        {
            if ( const auto it = m_RenderSystems.find( name ); it != m_RenderSystems.end() && it->second )
                it->second->OnSceneReplaced();
        }

        RebuildRenderGraph();
    }

    void SceneRenderer::ResetTemporalHistory()
    {
        // Over m_RenderSystemOrder for the reason RebindScene gives: the map can hold null entries.
        for ( const auto& name : m_RenderSystemOrder )
        {
            if ( const auto it = m_RenderSystems.find( name ); it != m_RenderSystems.end() && it->second )
                it->second->OnTemporalHistoryReset();
        }
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
               m_ViewResources.HeldBytes();
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
         : m_ViewResources( NameView( profile ) ), m_ViewProfile( profile ), m_ViewExtent( extent )
    {
        {
            const std::scoped_lock lock( LiveRenderersMutex() );
            LiveRenderers().push_back( this );
        }

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

        const auto& skyboxSystem = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );

        skyboxSystem->PrepareCamera( m_SceneInfo.ActiveCamera );

        m_ScenePlaying = scene.IsPlaying(); // grid & other authoring aids hide while the game runs

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
        const Common::Settings::MachineSettings& quality = m_Quality;

        m_AAMode = quality.AA;
        // TWO FORWARD-ONLY DEBUG VIEWS, and they force the path for the same reason.
        //
        // Wireframe has no deferred variant: the G-buffer pipeline has no wireframe polygon mode, which is
        // why turning it on in the default Deferred path did nothing at all.
        //
        // LightingDebug is the per-light attribution view, and it is a BRANCH IN THE PBR MESH SHADERS
        // (u_DebugParams.y, StaticMeshPBR / StaticMeshPBR_Instanced / SkinnedMeshPBR). The deferred
        // lighting pass writes `DebugParams = vec4(0)` unconditionally — MaterialDeferredLighting's
        // UploadShadow — so the flag reaches no shader on that path. Forty-six of this repository's
        // forty-nine scenes state Deferred and the struct's default is Deferred, so WITHOUT this line the
        // View Mode entry К7 added would be a control that does nothing in almost every scene: the §1.3
        // dead setting, reintroduced by the fix for one.
        m_RenderPath = ( m_DebugView.WireframeMode || m_DebugView.LightingDebug ) ? Core::RenderPath::Forward
                                                                                  : sceneSettings.RenderingPath;
        m_EnableSSAO = post.EnableSSAO;
        // The cloud layer's cost ceiling, refreshed here with every other cost-versus-quality choice
        // rather than read from a global at the point of use: several SceneRenderers are live at once
        // (Docs/RENDERER_FRAME_STATE.md) and a preview pane may be given a cheaper tier than the viewport.
        m_CloudQuality   = quality.CloudQualityTier;
        m_GIMode         = post.GlobalIllumination;
        m_GIIntensity    = post.GIIntensity;
        m_EnableSSR      = post.EnableSSR;
        m_SSRIntensity   = post.SSRIntensity;
        m_SSRMaxDistance = post.SSRMaxDistance;

        // GPU particles: snapshot the scene's emitters (CPU) here; the compute sim is dispatched in OnUpdate
        // before the render graph, and the billboard pass draws in the Transparency phase.
        UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->PrepareFrame( scene );

        // The rest of what reads time in a frame reads the SCENE'S clock (Core::WorldTime), handed over
        // here: the material Time uniform and the eye adaptation step.
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetWorldTimeSeconds( static_cast<float>( scene.GetWorldTime().GetGameTimeSeconds() ) );
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
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->SetLODEnabled( quality.MeshLOD );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetShadows( shadows.Enabled, shadows.Bias, static_cast<int>( m_DebugView.ShadowDebug ),
                           shadows.CascadeSplitLambda );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetDebugView( m_DebugView.ShowNormals, m_DebugView.ShowBoundingBoxes, m_DebugView.BoundingBoxColor,
                             m_DebugView.BoundingBoxLineWidth, m_DebugView.LightingDebug );

        // Global texture filter: push into RenderConfig (read by sampler creation). On an actual change,
        // recreate all image samplers so the new filter applies live (no reload).
        const int  desiredFilter = static_cast<int>( quality.TextureFilterMode );
        const int  desiredAniso  = quality.Anisotropy;
        const bool filterChanged = RenderConfig::TextureFilter.exchange( desiredFilter ) != desiredFilter;
        const bool anisoChanged  = RenderConfig::AnisotropyLevel.exchange( desiredAniso ) != desiredAniso;
        if ( filterChanged || anisoChanged )
            Renderer::GetInstance().RecreateImageSamplers();

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

        // Recompute CSM cascade matrices once per frame BEFORE the render graph records (intra-phase pass
        // order is nondeterministic, so the cascade passes can't compute them themselves).
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

        // THE FRAME IS A GRAPH (RDG3). Every GPU pass of this view from here on is one AddLegacyPass, added
        // in exactly the order the frame used to record them; nothing is culled or reordered (that is RDG4,
        // one pass at a time). A pass this frame does not run is not added. The lambdas run inside Execute,
        // after the whole graph is built, so a value one pass hands a later one travels through `values`,
        // which outlives Execute; everything else they need is captured by value.
        RDG::Builder        graph( "SceneView" );
        LegacyFrameTextures textures( graph );
        const auto          values = std::make_shared<LegacyFrameValues>();

        const auto sceneColor = [this, &textures]()
        {
            return textures.Refs(
                 { { m_TargetFramebuffer ? m_TargetFramebuffer->GetColorAttachmentImage( 0 ) : nullptr,
                     "SceneColor" } } );
        };

        AddFrameClearMainFramebuffer( graph, textures );

        AddFrameParticlesSimulate( graph, sceneRenderInfo );

        AddFrameCloudShadowMap( graph );

        // The registered systems' passes (and the editor's external passes) outside the overlay phases.
        AddGraphPhasePasses(
             graph, textures, []( RenderPhaseID phase ) { return !RenderPhase::IsDeferredOverlay( phase ); },
             true );

        // Deferred: fill the G-buffer, then shade it (or show a debug channel) into the scene target before
        // the post chain.
        if ( m_RenderPath == Core::RenderPath::Deferred && m_GBuffer )
        {
            auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            const std::vector<RDG::TextureRef> gbuffer = textures.Colors( m_GBuffer, "GBuffer" );

            AddFrameGBuffer( graph, gbuffer, meshRenderer );
            AddFrameTerrainGBuffer( graph, gbuffer );
            AddFrameDepthResolve( graph, textures );

            glm::vec4 lightDir( 0.0f, -1.0f, 0.0f, 0.0f );
            glm::vec4 lightColor( 1.0f, 0.98f, 0.92f, 3.0f );
            if ( const auto& dl = m_DirectionLights.DirectionLights; !dl.empty() )
            {
                lightDir   = dl[0].Direction;
                lightColor = dl[0].ColorIntensity;
            }
            glm::vec4 cameraPos( 0.0f );
            glm::mat4 viewProj( 1.0f );
            if ( const auto* cam = GetMainCamera() )
            {
                cameraPos = glm::vec4( cam->GetPosition(), 1.0f );
                viewProj  = cam->GetProjectionMatrix() * cam->GetViewMatrix();
            }

            std::vector<RDG::TextureRef> compositeReads = gbuffer;
            AddFrameSSAO( graph, textures, gbuffer, viewProj, cameraPos, values, compositeReads );

            if ( m_GIMode == Core::GIMode::RSM && meshRenderer && EnsureGIResources() )
            {
                const std::vector<RDG::TextureRef> rsm = textures.Colors( m_RSMBuffer, "RSM" );
                const glm::vec3                    sunDir( lightDir );
                AddFrameRSM( graph, rsm, meshRenderer, sunDir );
                m_RSMFrameCounter = ( m_RSMFrameCounter + 1 ) % kRSMRefreshEvery;

                AddFrameGIResolve( graph, textures, gbuffer, rsm, meshRenderer, viewProj, lightColor, values,
                                   compositeReads );
            }

            AddFrameComposite( graph, textures, compositeReads, meshRenderer, lightDir, lightColor, cameraPos,
                               values );
            AddFrameGeneric( graph, sceneColor(), meshRenderer );
            AddFrameSkinned( graph, sceneColor(), meshRenderer );

            auto* copy = UNIQUE_GET_AS( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] );
            std::vector<RDG::TextureRef> copyReads;
            AddFrameSceneCopy( graph, textures, sceneColor(), copy, values, copyReads );

            // SSR traces the copy made by the pass above; without a copy target there is nothing to trace.
            if ( m_EnableSSR && copy && EnsureSSRResources() )
                AddFrameSSR( graph, textures, gbuffer, copyReads, viewProj, cameraPos, values );

            AddFrameGlass( graph, copyReads, sceneColor(), meshRenderer, values );
        }

        AddFrameSkyAtmosphereLuts( graph );
        AddFrameAtmosphericFog( graph, sceneColor() );
        AddFrameVolumetricClouds( graph, sceneColor() );

        // Particles (Transparency phase), debug lines and the UI canvas run AFTER the deferred lighting
        // composite so lit geometry does not paint over them, and as LOAD overlays so a CLEAR begin never
        // wipes the depth later overlays test against (the particle top-down bug / grid-through-meshes).
        static_assert( RenderPhase::IsDeferredOverlay( RenderPhase::Transparency ) &&
                            RenderPhase::IsDeferredOverlay( RenderPhase::Debug ) &&
                            RenderPhase::IsDeferredOverlay( RenderPhase::UI ),
                       "the overlay phases added below must be the ones the main phase walk skips" );
        AddGraphPhasePasses(
             graph, textures, []( RenderPhaseID phase ) { return phase == RenderPhase::Transparency; }, false );

#if DESERT_DEV_INSTRUMENTS
        if ( m_DebugView.DeferredDebug == DeferredDebugMode::Overdraw )
        {
            AddFrameOverdraw( graph, sceneColor() );
        }
#endif // DESERT_DEV_INSTRUMENTS

        AddGraphPhasePasses(
             graph, textures, []( RenderPhaseID phase ) { return phase == RenderPhase::Debug; }, false );

        if ( m_BackdropBlurNeeded )
        {
            AddFrameBackdropBlur( graph, textures, sceneColor() );
        }

        AddGraphPhasePasses(
             graph, textures, []( RenderPhaseID phase ) { return phase == RenderPhase::UI; }, false );

        AddFrameJumpFlood( graph );
        AddFrameAutoExposure( graph, sceneColor() );
        if ( m_BloomEnabled )
        {
            AddFrameBloom( graph, sceneColor() );
        }
        AddFrameLightShafts( graph, sceneColor(), values );
        AddFrameLensFlare( graph, sceneColor(), values );
        AddFrameTonemap( graph, sceneColor() );

        if ( m_AAMode == Common::Settings::AntiAliasingMode::FXAA )
        {
            AddFrameFXAA( graph );
        }
        else if ( m_AAMode == Common::Settings::AntiAliasingMode::SMAA )
        {
            AddFrameSMAA( graph );
        }

        if ( const auto executed = Renderer::GetInstance().ExecuteGraph( graph ); !executed )
            LOG_ERROR( "SceneRenderer: frame graph '{}' did not execute: {}", graph.GetName(),
                       executed.GetError() );
    }

    NO_DISCARD Common::BoolResultStr SceneRenderer::EndScene()
    {
        const ActiveViewScope viewScope( m_ViewResources ); // same reason as OnUpdate: phases interleave

        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->ClearQueues();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->ClearQueue();

        m_PointLight.PointLights.clear();
        m_SpotLight.SpotLights.clear();

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

        FramebufferSpecification giSpec;
        giSpec.DebugName = "GIResolve";
        giSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kGIResolve );
        m_GIBuffer = Graphic::Framebuffer::Create( giSpec );
        m_GIBuffer->Resize( m_TargetFramebuffer->GetFramebufferWidth(),
                            m_TargetFramebuffer->GetFramebufferHeight() );

        // Reflective Shadow Map: a G-buffer rendered from the sun. The attachment layout MUST mirror
        // m_GBuffer (including the emissive target) — the RSM pass reuses the G-buffer pipeline, and that
        // only works while the two render passes stay compatible. Fixed light-space resolution, so it does
        // NOT resize with the viewport.
        FramebufferSpecification rsmSpec;
        rsmSpec.DebugName = "RSM";
        rsmSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kRSMAlbedo );   // Albedo (flux colour)
        rsmSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kRSMNormal );   // Normal
        rsmSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kRSMPosition ); // WorldPos
        rsmSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kRSMEmissive ); // Emissive (unused)
        // Matches the G-buffer's depth format because "mirror m_GBuffer" includes the depth attachment:
        // the RSM pipeline is created from the G-buffer's spec, and a differing depth format makes the
        // two render passes incompatible.
        rsmSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kRSMDepth );
        m_RSMBuffer = Graphic::Framebuffer::Create( rsmSpec );
        m_RSMBuffer->Resize( kRSMResolution, kRSMResolution );

        RegisterSystem<System::GIResolveRenderer>( "GISystem", this, m_GIBuffer, m_RenderGraphBuilder );
        if ( !SP_CAST( System::GIResolveRenderer, m_RenderSystems["GISystem"] )->Initialize() )
        {
            LOG_WARN( "[SceneRenderer] GI resolve system unavailable — RSM GI produces no indirect light." );
            m_GIResourcesFailed = true;
            m_GIBuffer.reset();
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

        RegisterSystem<System::SSRRenderer>( "SSRSystem", this, m_TargetFramebuffer, m_RenderGraphBuilder );
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
        m_TargetFramebuffer->Resize( width, height );
        if ( m_GBuffer )
            m_GBuffer->Resize( width, height );
        if ( m_SSAOBuffer )
            m_SSAOBuffer->Resize( width, height );
        if ( m_SceneColorCopy )
            m_SceneColorCopy->Resize( width, height );
        if ( m_GIBuffer )
            m_GIBuffer->Resize( width, height );
        // m_RSMBuffer is deliberately NOT resized: it is a fixed-resolution light-space target, unrelated
        // to the viewport. Its accumulation history is invalidated by the GI system's own size check.

        // Keep the post-process chain framebuffers in lock-step with the scene target.
        if ( const auto& maskFb = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
                                       ->GetSilhouetteMaskFramebuffer() )
            maskFb->Resize( width, height );

#if DESERT_DEV_INSTRUMENTS
        if ( const auto& overdrawFb =
                  UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetOverdrawFramebuffer() )
            overdrawFb->Resize( width, height );
#endif

        UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] )
             ->OnResize( width, height );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )->Resize( width, height );
        UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Resize( width, height );
        UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Resize( width, height );

        // The backdrop blur pyramid is sized from the target too. Its consumer (the UI pass) reads the
        // image through GetBackdropBlurImage() every frame, so nothing needs re-pointing here.
        if ( auto* backdrop =
                  UNIQUE_GET_AS( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] ) )
            backdrop->Resize( width, height );

        // Bloom recreates its (storage) mip-chain image on resize, so re-point tonemap at the new image.
        const auto& bloomSystem = UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
        bloomSystem->Resize( width, height );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetBloomImage( bloomSystem->GetBloomImage() );

        // Same contract for the light shafts: the ping-pong pair is recreated, so re-point tonemap.
        const auto& shaftSystem = UNIQUE_GET_AS( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
        shaftSystem->Resize( width, height );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetLightShaftImage( shaftSystem->GetShaftImage() );

        // And for the lens flare, whose source/feature pair is recreated at the new quarter resolution.
        const auto& flareSystem = UNIQUE_GET_AS( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
        flareSystem->Resize( width, height );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetLensFlareImage( flareSystem->GetFlareImage() );
    }

    void SceneRenderer::SubmitMesh( const Mesh* mesh, const MaterialSlotBindingPtr& materialSlots,
                                    const glm::mat4& transform, const RenderSubmissionExtra& extra )
    {
        if ( !mesh || !materialSlots || materialSlots->Slots.empty() )
        {
            return;
        }
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitMesh( { .Mesh            = (Mesh*)mesh,
                             .Transform       = transform,
                             .MaterialSlots   = materialSlots,
                             .BoneMatrices    = extra.BoneMatrices,
                             .Outlined        = extra.Outlined,
                             .HiddenSubmeshes = extra.HiddenSubmeshes,
                             .ForcedLOD       = extra.ForcedLOD,
                             .LODBias         = extra.LODBias,
                             .CastShadows     = extra.CastShadows,
                             .ReceiveShadows  = extra.ReceiveShadows } );
    }

    void SceneRenderer::SubmitLandscapeTile( Image2D* heightmap, const System::LandscapeTileDraw& tile,
                                             const MaterialOverrides&           overrides,
                                             const System::LandscapeWeightDraw& weights )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )
             ->Submit( { .Heightmap = heightmap, .Landscape = tile, .Weights = weights, .Overrides = overrides } );
    }

    void SceneRenderer::SubmitGenericMesh( const Mesh* mesh, const glm::mat4& transform,
                                           const std::string& shaderName, const MaterialOverrides& overrides,
                                           bool outlined, Image2D* directTexture,
                                           const std::string& directTextureSampler, bool castShadows )
    {
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitGenericMesh( { .Mesh                 = const_cast<Mesh*>( mesh ),
                                    .Transform            = transform,
                                    .ShaderName           = shaderName,
                                    .Overrides            = overrides,
                                    .Outlined             = outlined,
                                    .CastShadows          = castShadows,
                                    .DirectTexture        = directTexture,
                                    .DirectTextureSampler = directTextureSampler } );
    }

    void SceneRenderer::SubmitSlotMaterialMesh( const Mesh* mesh, const glm::mat4& transform, Material* material,
                                                uint64_t visibleSubmeshMask, bool outlined, bool castShadows )
    {
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitGenericMesh( { .Mesh               = const_cast<Mesh*>( mesh ),
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

    const std::shared_ptr<Desert::Graphic::Image2D> SceneRenderer::GetFinalImage()
    {
        // FXAA/SMAA write their own framebuffer downstream of tonemap; otherwise tonemap output IS final.
        const char* finalSystem = ( m_AAMode == Common::Settings::AntiAliasingMode::FXAA )   ? "FXAASystem"
                                  : ( m_AAMode == Common::Settings::AntiAliasingMode::SMAA ) ? "SMAASystem"
                                                                                             : "TonemapSystem";

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

    namespace
    {
        // Adapts an ExternalPassSpecification to the render-system interface so external passes flow
        // through the same graph build as engine systems (phases, dependencies, same-target merging).
        // The target framebuffer is looked up at RegisterPasses time rather than captured, so a resize
        // that recreates it cannot leave the pass pointing at the old one.
        //
        // These are the SCENE's, not the renderer's: the spec closes over the editor's RenderRegistry for a
        // particular scene, and SceneRenderer::RebindScene drops every one of them when a different scene
        // is bound. The owner re-registers against the new scene immediately afterwards.
        class ExternalPassSystem final : public IRenderSystem
        {
        public:
            ExternalPassSystem( SceneRenderer* renderer, ExternalPassSpecification&& spec )
                 : m_Renderer( renderer ), m_Spec( std::move( spec ) )
            {
            }

            void RegisterPasses( RenderGraphBuilder& builder ) override
            {
                const auto& target = m_Renderer->GetTargetFramebuffer();
                if ( !target || !m_Spec.Execute )
                    return;

                builder.AddPass(
                     m_Spec.Name, m_Spec.Phase,
                     [this]()
                     {
                         const auto&         target = m_Renderer->GetTargetFramebuffer();
                         ExternalPassContext ctx;
                         ctx.Camera       = m_Renderer->GetMainCamera();
                         ctx.Target       = target.get();
                         ctx.Depth        = target && target->GetDepthAttachmentCount() > 0
                                                 ? target->GetDepthAttachmentImage().get()
                                                 : nullptr;
                         ctx.ScenePlaying = m_Renderer->IsScenePlaying();
                         ctx.Renderer     = m_Renderer;
                         m_Spec.Execute( ctx );
                     },
                     m_Spec.PipelineSpecification, target, m_Spec.Dependencies );
            }

        private:
            SceneRenderer*            m_Renderer;
            ExternalPassSpecification m_Spec;
        };

        // Namespace external passes so they can never collide with (or evict) an engine system.
        std::string ExternalSystemKey( const std::string& name )
        {
            return std::string( kExternalSystemPrefix ) + name;
        }
    } // namespace

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

    void SceneRenderer::RegisterExternalPass( ExternalPassSpecification&& spec )
    {
        DESERT_VERIFY( !spec.Name.empty() && spec.Execute );

        // THE KEY IS TAKEN BEFORE THE SPEC IS MOVED, AND THAT ORDER MUST BE A STATEMENT RATHER THAN AN
        // ARGUMENT LIST. Reading `spec.Name` and moving `spec` inside one argument list leaves the order
        // to the compiler: clang evaluates arguments left to right, MSVC right to left, and both conform.
        // Under MSVC the move ran first, so every external pass registered itself under the bare prefix
        // "External:" — one key for all of them, the second registration evicting the first, and
        // UnregisterExternalPass() looking up "External:<name>" and never finding it. Consecutive
        // statements ARE sequenced; see Desert/Tests/Engine/ArgumentOrder for the census over the tree.
        const std::string key = ExternalSystemKey( spec.Name );
        TrackRenderSystem( key, std::make_shared<ExternalPassSystem>( this, std::move( spec ) ) );
        RebuildRenderGraph();
    }

    void SceneRenderer::UnregisterExternalPass( const std::string& name )
    {
        const auto key = ExternalSystemKey( name );
        if ( m_RenderSystems.find( key ) == m_RenderSystems.end() )
            return;

        ForgetRenderSystem( key );
        RebuildRenderGraph();
    }

    void SceneRenderer::RegisterRenderSystem( const std::string& name, std::shared_ptr<IRenderSystem> system )
    {
        TrackRenderSystem( name, std::move( system ) );
        RebuildRenderGraph();
    }

    void SceneRenderer::UnregisterRenderSystem( const std::string& name )
    {
        ForgetRenderSystem( name );
        RebuildRenderGraph();
    }

    void SceneRenderer::RebuildRenderGraph()
    {
        m_RenderGraphBuilder.Clear();

        // Registration order, not map order: passes registered earlier draw earlier inside a phase
        // (RenderGraphBuilder::AddPass), so walking the hash map here would have made the draw order a
        // property of the system NAMES. Looking each name up also skips the null entries that the
        // m_RenderSystems[...] accesses elsewhere in this file insert for systems that were never
        // registered — the map walk used to call RegisterPasses through those.
        for ( const auto& name : m_RenderSystemOrder )
        {
            const auto it = m_RenderSystems.find( name );
            if ( it != m_RenderSystems.end() && it->second )
                it->second->RegisterPasses( m_RenderGraphBuilder );
        }

        m_RenderGraphBuilder.AddPhaseDependency( RenderPhase::DepthPrePass, RenderPhase::Geometry );
        m_RenderGraphBuilder.AddPhaseDependency( RenderPhase::Sky, RenderPhase::Geometry );
        m_RenderGraphBuilder.AddPhaseDependency( RenderPhase::Geometry, RenderPhase::Outline );
        m_RenderGraphBuilder.AddPhaseDependency( RenderPhase::Geometry, RenderPhase::Lighting );
        m_RenderGraphBuilder.AddPhaseDependency( RenderPhase::Lighting, RenderPhase::PostProcess );

        if ( !m_RenderGraphBuilder.Build() )
        {
            LOG_ERROR( "Failed to build render graph" );
        }
    }

    void SceneRenderer::AddGraphPhasePasses( RDG::Builder& graph, LegacyFrameTextures&      textures,
                                             bool ( *selects )( RenderPhaseID ), const bool clearFirst )
    {
        std::vector<const RenderGraphBuilder::PassConfig*> passes;
        for ( const auto& pass : m_RenderGraphBuilder.GetSortedPasses() )
            if ( pass.CachedRenderPass && selects( pass.Phase ) )
                passes.push_back( &pass );

        // Consecutive passes that share a target framebuffer share ONE vkCmdBeginRenderPass/EndRenderPass:
        // the first opens it (CLEAR on the main walk, so the skybox draws first and the geometry on top
        // without either clearing the other; LOAD for the overlay phases), the last closes it. The render
        // pass declares its attachments when it begins, so the group's opener carries the group's
        // declarations; a barrier between two passes of one group would sit inside the render pass.
        const auto targetOf = []( const RenderGraphBuilder::PassConfig* pass )
        { return pass->CachedRenderPass->GetSpecification().TargetFramebuffer; };
        for ( size_t i = 0; i < passes.size(); ++i )
        {
            const RenderGraphBuilder::PassConfig* pass   = passes[i];
            const auto                            target = targetOf( pass );
            const bool                            opens  = i == 0 || targetOf( passes[i - 1] ) != target;
            const bool closes = i + 1 == passes.size() || targetOf( passes[i + 1] ) != target;

            AddLegacy( graph, pass->Name, {},
                       opens ? textures.Colors( target, pass->CachedRenderPass->GetSpecification().DebugName )
                             : std::vector<RDG::TextureRef>{},
                       [pass, opens, closes, clearFirst]()
                       {
                           auto& renderer = Renderer::GetInstance();
                           if ( opens )
                           {
                               DESERT_PROFILE_SCOPE(
                                    "RenderPass Begin/End" ); // vkCmdBeginRenderPass + transitions
                               renderer.BeginRenderPass( pass->CachedRenderPass.get(), clearFirst );
                           }
                           pass->ExecuteFunc();
                           if ( closes )
                               renderer.EndRenderPass();
                       } );
        }
    }

    void SceneRenderer::SetHeightFog( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY )
    {
        UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )
             ->SetFogSettings( present, data, fogHeightY );
    }

    void SceneRenderer::ExecuteAtmosphericFog()
    {
        UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )->ExecuteInFrame();
    }

    void SceneRenderer::SetVolumetricClouds( bool present, const ECS::VolumetricCloudData& data,
                                             const glm::vec3&                      windOffset,
                                             const std::vector<HeroCloudInstance>& heroClouds )
    {
        UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
             ->SetCloudSettings( present && m_ViewProfile.VolumetricClouds, data, windOffset, m_CloudQuality,
                                 heroClouds ); // a profile without clouds never allocates their targets
    }

    void SceneRenderer::ExecuteVolumetricClouds()
    {
        UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
             ->ExecuteInFrame();
    }

    void SceneRenderer::ExecuteCloudShadowMap()
    {
        UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
             ->ExecuteShadowMapInFrame();
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

        cloudShadow.Map        = clouds->GetShadowMapImage();
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

    const std::shared_ptr<Desert::Graphic::Image2D>& SceneRenderer::GetBackdropBlurImage() const
    {
        static const std::shared_ptr<Image2D> kNone;
        const auto                            it = m_RenderSystems.find( "BackdropBlurSystem" );
        if ( it == m_RenderSystems.end() )
            return kNone;
        return SP_CAST( System::BackdropBlurRenderer, it->second )->GetImage();
    }

    uint32_t SceneRenderer::GetBackdropBlurMaxLod() const
    {
        const auto it = m_RenderSystems.find( "BackdropBlurSystem" );
        return it == m_RenderSystems.end() ? 0u : SP_CAST( System::BackdropBlurRenderer, it->second )->GetMaxLod();
    }

} // namespace Desert::Graphic