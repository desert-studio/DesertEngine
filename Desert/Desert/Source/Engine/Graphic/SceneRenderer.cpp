#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RenderPhaseRegistry.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/RendererSlotPool.hpp>
#include <Common/Core/Units.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
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
            LOG_INFO( "[SceneRenderer] Renderer slot {} built in {:.1f} ms ({} systems, {} cached pipelines) "
                      "and bound its first scene in {:.1f} ms.",
                      m_SlotLease.RecordingSlot(), ms( started, resourcesDone ), m_RenderSystemOrder.size(),
                      m_PipelineCache.Size(), ms( resourcesDone, done ) );
        }
        else
        {
            LOG_INFO( "[SceneRenderer] Renderer slot {} rebound to a new scene in {:.1f} ms ({} systems and "
                      "{} cached pipelines kept).",
                      m_SlotLease.RecordingSlot(), ms( started, done ), m_RenderSystemOrder.size(),
                      m_PipelineCache.Size() );
        }

        // THE LEDGER, ONCE PER SCENE LOAD, IN THE LOG. Scene load is the moment the population changes and
        // the only moment anybody has ever wanted this number: "the editor keeps growing" is a claim about
        // successive loads, and until now the only instrument for it was the process's RSS, which mixes
        // device memory, the asset layer's CPU copies and the allocator's own slack. A line per load makes
        // the growth attributable to an owner instead of merely visible. See Graphic/ResourceLedger.hpp.
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

        const auto window = EngineContext::GetInstance().GetWindow();
        const auto width  = window ? window->GetWidth() : 1280;
        const auto height = window ? window->GetHeight() : 720;

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
        fbSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F );
        // DEPTH32F, AND THE FLOAT IS THE POINT. Reversed-Z (Core/Projection.hpp) works by lining the
        // 1/z curve up against the float exponent so the two cancel; on a UNORM24 attachment, which
        // quantizes uniformly in NDC, reversing the range just relabels the same 2^24 levels and buys
        // literally nothing. This was DEPTH24STENCIL8, and no pass in the engine enables a stencil test,
        // so the packed stencil byte was paying for nothing either.
        fbSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::DEPTH32F );

        m_TargetFramebuffer = Graphic::Framebuffer::Create( fbSpec );
        m_TargetFramebuffer->Resize( width, height );

        // Deferred G-buffer (populated only when SceneSettings::RenderPath == Deferred; allocated always so the
        // toggle is live). GBufferA = Albedo.rgb + Metallic.a (RGBA8F); GBufferB = Normal.rgb + Roughness.a
        // (RGBA32F for banding-free normals — the format enum has no RGBA16F yet); GBufferC = world position.xyz
        // (RGBA32F) so the lighting pass gets point/spot-light distances directly (bulletproof vs depth
        // reconstruction, which is error-prone under the GL-on-Vulkan depth conventions); shared depth.
        FramebufferSpecification gbufferSpec;
        gbufferSpec.DebugName = "GBuffer";
        gbufferSpec.Attachments.Attachments.push_back(
             Core::Formats::ImageFormat::RGBA8F ); // GBufferA Albedo+Metallic
        gbufferSpec.Attachments.Attachments.push_back(
             Core::Formats::ImageFormat::RGBA32F ); // GBufferB Normal+Roughness
        gbufferSpec.Attachments.Attachments.push_back(
             Core::Formats::ImageFormat::RGBA32F ); // GBufferC WorldPosition.xyz
        gbufferSpec.Attachments.Attachments.push_back(
             Core::Formats::ImageFormat::RGBA32F ); // GBufferEmissive (HDR self-illum)
        // DEPTH32F for the same reason as the forward target above — and it is this attachment the
        // height fog reads back as a texture, so its precision is the precision of every distance it
        // reconstructs.
        gbufferSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::DEPTH32F );
        m_GBuffer = Graphic::Framebuffer::Create( gbufferSpec );
        m_GBuffer->Resize( width, height );

        // SSAO target: a single-channel-ish AO factor (RGBA8F, AO in .r) the deferred lighting reads.
        FramebufferSpecification ssaoSpec;
        ssaoSpec.DebugName = "SSAO";
        ssaoSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA8F );
        m_SSAOBuffer = Graphic::Framebuffer::Create( ssaoSpec );
        m_SSAOBuffer->Resize( width, height );

        // Scene-colour snapshot (same format as the target) the glass pass samples for refraction.
        FramebufferSpecification copySpec;
        copySpec.DebugName = "SceneColorCopy";
        copySpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F );
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
        if ( !SP_CAST( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->Initialize() )
            LOG_ERROR( "[SceneRenderer] the terrain system did not initialise; terrain will not draw." );

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

    namespace
    {
        // The one lease register for the process. The accounting itself lives in a pure header so a test
        // can drive it without a GPU (Engine/Core/RendererSlotPool.hpp); this is only where it is kept.
        Engine::RendererSlotPool& SlotPool()
        {
            static Engine::RendererSlotPool pool;
            return pool;
        }
    } // namespace

    uint32_t SceneRenderer::GetLiveRendererCount()
    {
        return SlotPool().InUseCount();
    }

    SceneRenderer::SceneRenderer( const ShadowQuality& shadowQuality )
         : m_SlotLease( SlotPool() ), m_ShadowQuality( shadowQuality )
    {
        if ( !m_SlotLease.IsValid() )
        {
            // More live renderers than slots: the newcomer records into slot 0 and says so, because the
            // symptom (two views borrowing each other's camera) is otherwise a mystery.
            LOG_WARN( "[SceneRenderer] No free renderer slot ({} in use) — this renderer shares slot 0 and "
                      "will trade per-frame state with the main view.",
                      EngineContext::kMaxRendererSlots );
            return;
        }

        // Occupancy is logged on BOTH edges, with the resulting count, because the failure this guards
        // against is silent by construction: a surface that never returns its slot produces no error at
        // all, only two panels quietly sharing a camera some minutes later. Counting slots is the only way
        // to see it, so the count is printed rather than left for a reader to derive.
        LOG_INFO( "[SceneRenderer] Claimed renderer slot {} ({}/{} in use).", m_SlotLease.RecordingSlot(),
                  SlotPool().InUseCount(), EngineContext::kMaxRendererSlots );
    }

    SceneRenderer::~SceneRenderer()
    try
    {
        if ( !m_SlotLease.IsValid() )
            return;

        // Logged BEFORE the lease's destructor runs, so the slot number is still readable; the count it
        // prints is therefore the one that includes this renderer, minus itself.
        LOG_INFO( "[SceneRenderer] Releasing renderer slot {} ({}/{} in use after release).",
                  m_SlotLease.RecordingSlot(), SlotPool().InUseCount() - 1, EngineContext::kMaxRendererSlots );
    }
    DESERT_DESTRUCTOR_GUARD( "~SceneRenderer" )

    NO_DISCARD Common::BoolResultStr SceneRenderer::BeginScene( const Desert::Core::Scene& scene )
    {
        // Which renderer is recording, alongside which frame is in flight — see
        // EngineContext::GetActiveRendererSlot. Set FIRST, before anything writes a per-frame resource.
        EngineContext::GetInstance().SetActiveRendererSlot( m_SlotLease.RecordingSlot() );

        const auto& mainCamera   = scene.GetMainCamera().lock();
        m_SceneInfo.ActiveCamera = mainCamera.get();

        const auto& skyboxSystem = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );

        skyboxSystem->PrepareCamera( m_SceneInfo.ActiveCamera );

        m_ScenePlaying = scene.IsPlaying(); // grid & other authoring aids hide while the game runs

        const auto& sceneSettings = scene.GetSettings();
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
        m_EnableSSAO = sceneSettings.EnableSSAO;
        // The cloud layer's cost ceiling, refreshed here with every other cost-versus-quality choice
        // rather than read from a global at the point of use: several SceneRenderers are live at once
        // (Docs/RENDERER_FRAME_STATE.md) and a preview pane may be given a cheaper tier than the viewport.
        m_CloudQuality   = quality.CloudQualityTier;
        m_GIMode         = sceneSettings.GlobalIllumination;
        m_GIIntensity    = sceneSettings.GIIntensity;
        m_EnableSSR      = sceneSettings.EnableSSR;
        m_SSRIntensity   = sceneSettings.SSRIntensity;
        m_SSRMaxDistance = sceneSettings.SSRMaxDistance;

        // GPU particles: snapshot the scene's emitters (CPU) here; the compute sim is dispatched in OnUpdate
        // before the render graph, and the billboard pass draws in the Transparency phase.
        UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->PrepareFrame( scene );

        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetParams( sceneSettings.Exposure, sceneSettings.Gamma );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetTonemapOperator( sceneSettings.Tonemapper );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetWhitePoint( sceneSettings.WhitePoint );

        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetWireframe( m_DebugView.WireframeMode );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->SetLODEnabled( quality.MeshLOD );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetShadows( sceneSettings.EnableShadows, sceneSettings.ShadowBias,
                           static_cast<int>( m_DebugView.ShadowDebug ), sceneSettings.CascadeSplitLambda );
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
        const float exposureNormalisation =
             sceneSettings.AutoExposure ? 1.0f : std::max( sceneSettings.Exposure, 1e-4f );

        m_BloomEnabled = sceneSettings.EnableBloom;
        UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] )
             ->SetThreshold( sceneSettings.BloomThreshold / exposureNormalisation );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetBloomIntensity( sceneSettings.EnableBloom ? sceneSettings.BloomIntensity : 0.0f );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetChromaticBloom( sceneSettings.EnableBloom ? sceneSettings.LensDispersion : 0.0f );

        // Lens flare: the authored "Lens Flare" group, copied whole. Intensity and Tint are held out of
        // the pass's own params because the pass never applies them — the tonemap does, so that a flare
        // whose sun has left the screen fades through ONE number instead of two that could disagree.
        m_LensFlare.Enabled         = sceneSettings.EnableLensFlare;
        m_LensFlare.Intensity       = sceneSettings.LensFlareIntensity;
        // Normalised for the reason given at the bloom threshold above, and by the same number: this pass
        // thresholds the same raw HDR image, so leaving one of the two in raw radiance would only move the
        // defect from one bright pass to the other.
        m_LensFlare.Threshold       = sceneSettings.LensFlareThreshold / exposureNormalisation;
        m_LensFlare.GhostCount      = sceneSettings.LensFlareGhostCount;
        m_LensFlare.GhostSpacing    = sceneSettings.LensFlareGhostSpacing;
        m_LensFlare.GhostSizeNear   = sceneSettings.LensFlareGhostSizeNear;
        m_LensFlare.GhostSizeFar    = sceneSettings.LensFlareGhostSizeFar;
        m_LensFlare.GhostTintInner  = sceneSettings.LensFlareGhostTintInner;
        m_LensFlare.GhostTintOuter  = sceneSettings.LensFlareGhostTintOuter;
        m_LensFlare.HaloIntensity   = sceneSettings.LensFlareHaloIntensity;
        m_LensFlare.HaloRadius      = sceneSettings.LensFlareHaloRadius;
        m_LensFlare.StreakIntensity = sceneSettings.LensFlareStreakIntensity;
        m_LensFlare.StreakLength    = sceneSettings.LensFlareStreakLength;
        m_LensFlare.StreakAngle     = sceneSettings.LensFlareStreakAngle;
        m_LensFlare.ChromaShift     = sceneSettings.LensFlareChromaShift;
        m_LensFlareTint             = sceneSettings.LensFlareTint;

        UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] )
             ->SetParams( sceneSettings.AutoExposureSpeed, sceneSettings.AutoExposureMin,
                          sceneSettings.AutoExposureMax );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetAutoExposure( sceneSettings.AutoExposure, sceneSettings.AutoExposureKey );

        return BOOLSUCCESS;
    }

    void SceneRenderer::OnUpdate( const UpdateInfo& sceneRenderInfo )
    {
        DESERT_PROFILE_SCOPE( "SceneRenderer::OnUpdate" );

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
            skyboxSystem->EnsureProceduralEnvironment( sceneRenderInfo.Timestep.GetSeconds() );
        }

        // Recompute CSM cascade matrices once per frame BEFORE the render graph records (intra-phase pass
        // order is nondeterministic, so the cascade passes can't compute them themselves).
        {
            DESERT_PROFILE_PASS( "Shadow: UpdateCascades" );
            UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->UpdateCascades();
        }

        {
            DESERT_PROFILE_PASS( "ClearMainFramebuffer" );
            ClearMainFramebuffer();
        }

        // Particle simulation compute (outside any render pass) BEFORE the graph records the billboard draw,
        // so the freshly-integrated particle buffer is ready + visible to the vertex stage.
        {
            DESERT_PROFILE_PASS( "Particles: SimulateInFrame" );
            UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->SimulateInFrame();
        }

        // The cloud layer's shadow on the world. HERE, and not beside the cloud march at the other end of
        // the frame, because its consumer is the DEFERRED LIGHTING pass — which runs immediately after
        // the graph, long before ExecuteVolumetricClouds(). Sampling a map written after it was read
        // would shade the world with the sun's position of one frame ago, and under a moving sun that is
        // a shadow that lags its cloud.
        //
        // Nothing forces it later: it reads no scene depth, no G-buffer and no atmosphere LUT — only the
        // cloud field, the sun direction and the camera position, all of which are final before the graph
        // records. It is an in-frame compute dispatch and so must be outside any open render pass, which
        // this point is.
        {
            DESERT_PROFILE_PASS( "CloudShadowMap" );
            ExecuteCloudShadowMap();
        }

        {
            DESERT_PROFILE_PASS( "ExecuteRenderGraph" );
            ExecuteRenderGraph();
        }

        // Deferred: fill the G-buffer (manual pass, outside the graph) then shade it (or show a debug channel)
        // into the scene target before the post chain.
        if ( m_RenderPath == Core::RenderPath::Deferred && m_GBuffer )
        {
            DESERT_PROFILE_PASS( "Deferred: Lighting" );

            auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            meshRenderer->RenderGBufferManual();

            // Resolve the G-buffer depth (static opaque geometry) into the scene target depth. The deferred
            // composite writes only colour, so without this the target depth stays empty and depth-tested
            // overlays (grid, colliders) never get occluded by static meshes — they drew "through" them.
            // Done here, outside any render pass, before the forward-over-composite draws so they too test
            // against real geometry depth.
            if ( m_TargetFramebuffer && m_TargetFramebuffer->GetDepthAttachmentCount() > 0 &&
                 m_GBuffer->GetDepthAttachmentCount() > 0 )
            {
                Renderer::GetInstance().CopyDepthImage( m_GBuffer->GetDepthAttachmentImage().get(),
                                                        m_TargetFramebuffer->GetDepthAttachmentImage().get() );
            }

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

            // SSAO first (reads the G-buffer world pos + normal into the AO buffer); the lighting pass below
            // multiplies its ambient term by this. Skipped when disabled (the shader uses AO=1 then).
            //
            // The hemisphere radius and depth bias are WORLD distances (SSAO.shader offsets samples by
            // TBN*dir*radius in world space), and a world unit is a centimetre. The classic recipe these
            // numbers come from (John Chapman-style hemisphere SSAO, radius 0.5, bias 0.025) states them
            // in METRES; they were passed bare, so the occlusion hemisphere was five MILLIMETRES wide and
            // the pass returned ~1.0 for every pixel of every scene while costing a full-screen 16-tap
            // pass per frame. The radius keeps the literature's 0.5 m after an A/B against 0.25 m and
            // 1 m on CornellDemo's AO channel (corner-band p05 occlusion vs a flat wall's 0%): 0.25 m
            // darkens only a narrow seam (8%), 1 m reaches 16% but smears a grainy gradient a third of
            // the way up the wall — 16 samples spread over a metre read as dirt, not contact — and 0.5 m
            // (12%) keeps the darkening at the corner with the sample noise still unobtrusive.
            constexpr float          kSSAORadius = Common::Units::Metres( 0.5f );   // literature: 0.5 m
            constexpr float          kSSAOBias   = Common::Units::Metres( 0.025f ); // literature: 0.025 m
            std::shared_ptr<Image2D> aoImage;
            if ( m_EnableSSAO )
                if ( auto* ssao = UNIQUE_GET_AS( System::SSAORenderer, m_RenderSystems["SSAOSystem"] ) )
                {
                    ssao->Execute( m_GBuffer, viewProj, cameraPos, kSSAORadius, kSSAOBias,
                                   /*power*/ 1.5f, /*samples*/ 16 );
                    aoImage = ssao->GetAOImage();
                }

            // RSM GI mode: rasterize the sun's G-buffer, then resolve one bounce out of it. The RSM is a
            // LOW-FREQUENCY input to a temporally-accumulated resolve, so it is refreshed every Nth frame
            // (and immediately when the sun moves) rather than every frame.
            std::shared_ptr<Image2D> giImage;
            if ( m_GIMode == Core::GIMode::RSM && meshRenderer && EnsureGIResources() )
            {
                const glm::vec3 sunDir( lightDir );
                if ( glm::distance( sunDir, m_RSMLastSunDir ) > 1e-4f || m_RSMFrameCounter == 0 )
                {
                    DESERT_PROFILE_PASS( "Deferred: RSM" );
                    meshRenderer->RenderRSMManual();
                    m_RSMLastSunDir = sunDir;
                }
                m_RSMFrameCounter = ( m_RSMFrameCounter + 1 ) % kRSMRefreshEvery;

                if ( auto* gi = UNIQUE_GET_AS( System::GIResolveRenderer, m_RenderSystems["GISystem"] ) )
                {
                    DESERT_PROFILE_PASS( "Deferred: GIResolve" );
                    gi->Execute( m_GBuffer, m_RSMBuffer->GetColorAttachmentImage( 0 ),
                                 m_RSMBuffer->GetColorAttachmentImage( 1 ),
                                 m_RSMBuffer->GetColorAttachmentImage( 2 ), meshRenderer->GetRSMViewProj(),
                                 viewProj, lightColor, m_GIIntensity );
                    giImage = gi->GetGIImage();
                }
            }

            // Gather the same CSM data the forward material uses so the deferred sun casts identical shadows.
            DeferredShadowInput shadow;
            if ( meshRenderer )
            {
                shadow.CascadeVP = meshRenderer->GetCascadeViewProj();
                // The count FITTED this frame, not the count allocated — the same number the forward path
                // publishes (MeshRenderer::CaptureFrameState). The two paths shadow the same sun and a
                // disagreement here is a deferred frame shading against matrices the forward one refused.
                shadow.Count                = meshRenderer->GetValidCascadeCount();
                shadow.Bias                 = meshRenderer->GetShadowBias();
                shadow.Enabled              = meshRenderer->AreShadowsEnabled();
                shadow.CascadeWorldPerTexel = meshRenderer->GetCascadeWorldPerTexel();
                for ( uint32_t c = 0; c < shadow.Count && c < 4u; ++c )
                {
                    const auto img        = meshRenderer->GetCascadeShadowImage( c );
                    shadow.CascadeMaps[c] = img ? img.get() : nullptr;
                }
            }

            // The cloud layer's shadow on the world — a SECOND occluder of the same sun, filled by
            // ExecuteCloudShadowMap() before the graph recorded. The SAME payload the forward mesh
            // materials and the terrain material are handed (GetCloudShadowInput), gathered once: this
            // used to be gathered inline right here, and that is exactly why nothing outside this branch
            // ever received a cloud shadow.
            const CloudShadowInput cloudShadow = GetCloudShadowInput();

            // The baked sky the composite shades its ambient with — resolved from the SAME Environment
            // and the SAME BRDF LUT that MeshRenderer::CaptureFrameState hands the forward materials
            // (through Graphic::PBRSceneFrame). One bake, one generation, both paths: gathering it a second
            // way here would be the mirror that drifts, and the forward-drawn skinned/glass meshes
            // composited over this pass a few lines below would eventually reflect a different sky from
            // the wall behind them.
            DeferredEnvironmentInput environment;
            {
                auto* imageService = Runtime::ResourceRegistry::GetImageService();
                if ( const auto& env = GetEnvironment(); env.has_value() )
                {
                    if ( env->IrradianceMap.IsValid() )
                        environment.Irradiance =
                             static_cast<ImageCube*>( imageService->Resolve( env->IrradianceMap ) );
                    if ( env->PreFilteredMap.IsValid() )
                        environment.Prefiltered =
                             static_cast<ImageCube*>( imageService->Resolve( env->PreFilteredMap ) );
                }
                if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
                     brdf && brdf->GetImageHandle().IsValid() )
                    environment.BrdfLut = static_cast<Image2D*>( imageService->Resolve( brdf->GetImageHandle() ) );
            }

            // The RSM path pre-applies its intensity in GIResolve, so pass 0 there to avoid scaling twice;
            // the screen-space gather is scaled inside the lighting shader.
            const float giIntensity = ( m_GIMode == Core::GIMode::ScreenSpace ) ? m_GIIntensity : 0.0f;
            {
                // The composite ALONE. "Deferred: Lighting" above spans the whole deferred stage — the
                // G-buffer fill, SSAO, the RSM, the forward-over draws, the copy and SSR — so its slope
                // cannot price a change to this pass, and a frame-to-frame delta prices the machine.
                // Its siblings (RSM, GIResolve, SSR) were already itemised; this was the one full-screen
                // pass in the stage with no line of its own, which made the cost of the ambient
                // unmeasurable at exactly the moment it acquired two cube samples and a LUT fetch.
                DESERT_PROFILE_PASS( "Deferred: Composite" );
                UNIQUE_GET_AS( System::DeferredLightingRenderer, m_RenderSystems["DeferredLightingSystem"] )
                     ->Execute( m_GBuffer, lightDir, lightColor, cameraPos,
                                static_cast<int>( m_DebugView.DeferredDebug ), GetPointLights(), GetSpotLights(),
                                shadow, aoImage, giIntensity, m_EnableSSAO, static_cast<int>( m_GIMode ), giImage,
                                cloudShadow, environment );
            }

            // Custom-shader (generic) meshes have no G-buffer variant — draw them forward OVER
            // the deferred composite (before the glass snapshot so glass refracts them too).
            meshRenderer->RenderGenericManual();

            // Skinned meshes also have no G-buffer variant (the G-buffer pass draws static only), so draw
            // them forward over the composite too — otherwise they only show in the silhouette/outline pass.
            meshRenderer->RenderSkinnedManual();

            // Snapshot the composited opaque scene, then draw the transparent (glass) meshes over it. The
            // snapshot lets the glass sample the scene BEHIND it for refraction without a read+write feedback
            // loop on the target. Uses a dedicated glass material (no double-written per-frame UB ring).
            std::shared_ptr<Image2D> sceneCopy;
            if ( auto* copy = UNIQUE_GET_AS( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] ) )
            {
                copy->Execute( m_TargetFramebuffer->GetColorAttachmentImage( 0 ) );
                sceneCopy = copy->GetImage();
            }

            // SSR reflects the COMPOSITED opaque scene, so it runs off that same snapshot — reading the
            // target while writing it would be a feedback loop. Before glass, so glass refracts the
            // reflections too.
            if ( m_EnableSSR && sceneCopy && EnsureSSRResources() )
                if ( auto* ssr = UNIQUE_GET_AS( System::SSRRenderer, m_RenderSystems["SSRSystem"] ) )
                {
                    DESERT_PROFILE_PASS( "Deferred: SSR" );
                    // Hit-acceptance band in WORLD units (SSR.shader compares the refined camera-radial
                    // delta against it), and a world unit is a centimetre. The usual SSR thickness of
                    // 0.5 m was passed bare — a 5 mm band — so rays that genuinely crossed geometry
                    // refined to a delta wider than the band and were discarded as silhouette gaps.
                    constexpr float kSSRThickness = Common::Units::Metres( 0.5f ); // literature: 0.5 m
                    ssr->Execute( m_GBuffer, sceneCopy, viewProj, cameraPos, /*maxSteps*/ 32, m_SSRMaxDistance,
                                  m_SSRIntensity, kSSRThickness );
                }

            meshRenderer->RenderGlassManual( sceneCopy );
        }

        // The physical atmosphere's LUTs: the cached pair (transmittance + multi-scattering), the
        // per-view Sky-View LUT and the per-view aerial-perspective volume. In-frame compute, outside any
        // open render pass, and BEFORE the atmospheric-fog pass, which samples the AP volume this very
        // frame. The cached pair is almost always a fingerprint compare and an immediate return; nothing
        // here runs at all for SkyModel::ArtisticGradient.
        {
            DESERT_PROFILE_PASS( "SkyAtmosphereLuts" );
            UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )->ExecuteAtmosphereLuts();
        }

        // Atmosphere and fog: aerial perspective on opaque, with the closed-form height fog over it.
        // HERE and not earlier — it reads the finished scene depth, which only exists after the graph in
        // Forward and after the G-buffer depth copy in Deferred, and an in-frame compute dispatch has to
        // be issued outside an open render pass. Both hold at exactly this point, and it follows the LUT
        // slot above, which just filled the AP volume it samples. Its apply is replayed by
        // ExecuteTransparency at RenderPassOrder::AtmosphericFog, under the particles, so they are drawn
        // OVER the fogged scene.
        {
            DESERT_PROFILE_PASS( "AtmosphericFog" );
            ExecuteAtmosphericFog();
        }

        // Volumetric clouds, on the same terms as the fog above and immediately after it: an in-frame
        // compute dispatch outside any render pass, at the one point where the scene depth is final in
        // both paths and this frame's atmosphere is already evaluated. Its composite is replayed by
        // ExecuteTransparency at RenderPassOrder::FarField — over the fog, under the particles.
        {
            DESERT_PROFILE_PASS( "VolumetricClouds" );
            ExecuteVolumetricClouds();
        }

        // Transparent billboards (GPU particles) over the finished opaque scene. Runs in BOTH paths here,
        // after the deferred composite (Deferred) / after the forward geometry graph (Forward), but BEFORE
        // the post chain so particles are tonemapped + bloom'd like everything else. Deferred recorded them
        // inside the graph and the composite painted over them wherever geometry existed (the top-down bug).
        {
            DESERT_PROFILE_PASS( "Transparency" );
            ExecuteTransparency();
        }

        // Overdraw debug view: re-rasterize all meshes additively into a heat map over the finished scene
        // color. Path-independent (redraws geometry, ignores the G-buffer), so it runs for Forward too.
        if ( m_DebugView.DeferredDebug == DeferredDebugMode::Overdraw )
        {
            DESERT_PROFILE_PASS( "Debug: Overdraw" );
            UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->RenderOverdrawManual();
        }

        // Debug overlays (bounding boxes, colliders) drawn LAST over the finished scene color — in both
        // paths, but critically in Deferred where the lighting composite above would otherwise cover any
        // debug lines recorded inside the graph. Runs before the post chain so tonemap treats them uniformly.
        {
            DESERT_PROFILE_PASS( "Debug: Overlay" );
            ExecuteDebugOverlay();
        }

        // Backdrop blur for UI glass, BEFORE the UI overlay draws into this target. Only when the canvas
        // asked for it last frame: a full mip pyramid every frame for a UI that has no glass is waste,
        // and the one-frame delay is invisible (the first glass frame simply blurs the previous image).
        if ( m_BackdropBlurNeeded )
        {
            DESERT_PROFILE_PASS( "UI: BackdropBlur" );
            if ( auto* backdrop =
                      UNIQUE_GET_AS( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] ) )
                backdrop->Execute();
        }

        // UI canvas (Render2D) on top of the finished scene, as a LOAD overlay — see ExecuteUI(). Kept out of
        // the main graph so its CLEAR begin can't wipe the depth the grid/overlays above load.
        {
            DESERT_PROFILE_PASS( "UI" );
            ExecuteUI();
        }

        // Explicit post-process chain (runs after the scene graph has produced the scene color and
        // the silhouette mask): Jump Flood outline -> Tonemap.
        {
            DESERT_PROFILE_PASS( "PostFX: JumpFlood" );
            const auto& jfa =
                 UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
            // Skip the JFA step passes when nothing is outlined (sync is handled by render-pass layouts +
            // the EndRenderPass barrier, not by the steps — see JumpFloodOutlineRenderer::Execute).
            jfa->SetOutlineActive(
                 UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->HasOutline() );
            jfa->Execute();
        }

        // Eye adaptation: measure scene luminance into the 1x1 buffer, then point tonemap at the latest
        // (the ping-pong target alternates each frame, so the reference must be refreshed here).
        {
            const auto& autoExp =
                 UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
            autoExp->Execute();
            UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
                 ->SetAutoExposureImage( autoExp->GetAdaptedLuminanceImage() );
        }

        // Bloom (HDR scene color -> blurred bright) runs before tonemap, which adds it in.
        if ( m_BloomEnabled )
        {
            DESERT_PROFILE_PASS( "PostFX: Bloom" );
            UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] )->Execute();
        }

        // Light shafts: the sun light's streaks, masked and radially blurred from the same HDR scene
        // colour bloom reads. The intensity handed to tonemap carries the sun's screen-edge fade, and it
        // is derived HERE, from the same numbers that decide whether the dispatches run — a zero
        // intensity therefore always means the (possibly stale) shaft image is inert, the exact contract
        // the bloom image has.
        // Where the sun lands on screen, computed ONCE: the shafts stream from it and the flare is
        // reflected about it, and two evaluations of one quantity is the defect class that has cost this
        // project the most. Fade is 0 when the sun is behind the camera or far past the screen edge, and
        // both effects multiply it — that, not a branch, is what stops either painting from a sun behind
        // the viewer.
        SunScreen            sun{ glm::vec2( 0.5f ), 0.0f };
        const AtmosphereEnv& atmosphere = GetAtmosphere();
        if ( m_SceneInfo.ActiveCamera && atmosphere.Valid )
        {
            const glm::mat4 viewProjection =
                 m_SceneInfo.ActiveCamera->GetProjectionMatrix() * m_SceneInfo.ActiveCamera->GetViewMatrix();
            sun = ComputeSunScreen( viewProjection, atmosphere.SunDirection );
        }

        {
            DESERT_PROFILE_PASS( "PostFX: LightShafts" );
            const auto& shafts  = UNIQUE_GET_AS( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
            const auto& tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );

            shafts->SetParams( System::LightShaftRenderer::Params{
                 .Enabled       = m_SunLightFx.LightShaftBloom,
                 .BloomScale    = m_SunLightFx.BloomScale,
                 .Threshold     = m_SunLightFx.BloomThreshold,
                 .MaxBrightness = m_SunLightFx.BloomMaxBrightness,
                 .BloomTint     = m_SunLightFx.BloomTint,
            } );
            shafts->Execute( sun.Uv, sun.Fade );

            const float intensity = m_SunLightFx.LightShaftBloom ? m_SunLightFx.BloomScale * sun.Fade : 0.0f;
            tonemap->SetLightShaftImage( shafts->GetShaftImage() );
            tonemap->SetLightShafts( intensity, m_SunLightFx.BloomTint );
        }

        {
            DESERT_PROFILE_PASS( "PostFX: LensFlare" );
            const auto& flare   = UNIQUE_GET_AS( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
            const auto& tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );

            flare->SetParams( m_LensFlare );
            flare->Execute( sun.Uv, sun.Fade );

            // Derived HERE, from the same two numbers that decided whether the dispatches ran, so a zero
            // intensity and a skipped dispatch can never disagree — the bloom image's contract.
            const float intensity =
                 m_LensFlare.Enabled ? LensFlareStrength( sun.Fade, m_LensFlare.Intensity ) : 0.0f;
            tonemap->SetLensFlareImage( flare->GetFlareImage() );
            tonemap->SetLensFlare( intensity, m_LensFlareTint );
        }

        {
            DESERT_PROFILE_PASS( "PostFX: Tonemap" );
            UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )->Execute();
        }

        if ( m_AAMode == Common::Settings::AntiAliasingMode::FXAA )
        {
            DESERT_PROFILE_PASS( "PostFX: FXAA" );
            UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Execute();
        }
        else if ( m_AAMode == Common::Settings::AntiAliasingMode::SMAA )
        {
            DESERT_PROFILE_PASS( "PostFX: SMAA" );
            UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Execute();
        }

        // NO COMPOSITE PASS HERE, AND THE ABSENCE IS DELIBERATE. `CompositeRenderPass()` used to be called
        // at this point inside its own profiler scope, and its entire body had been commented out — so the
        // frame graph reported a pass that did nothing, and the two locals it still declared are what
        // `-Wunused-variable` finally pointed at. The editor composites through the ImGui layer's swapchain
        // pass; a build that renders WITHOUT ImGui has no compositing step at all and shows a black screen,
        // and that is a missing feature to be written where the passes above live, not an empty function
        // kept as a reminder.
    }

    NO_DISCARD Common::BoolResultStr SceneRenderer::EndScene()
    {
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->ClearQueues();
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
        return device->IsFormatSupported( Core::Formats::ImageFormat::RGBA32F,
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
        if ( m_GIResourcesFailed )
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
        giSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F );
        m_GIBuffer = Graphic::Framebuffer::Create( giSpec );
        m_GIBuffer->Resize( m_TargetFramebuffer->GetFramebufferWidth(),
                            m_TargetFramebuffer->GetFramebufferHeight() );

        // Reflective Shadow Map: a G-buffer rendered from the sun. The attachment layout MUST mirror
        // m_GBuffer (including the emissive target) — the RSM pass reuses the G-buffer pipeline, and that
        // only works while the two render passes stay compatible. Fixed light-space resolution, so it does
        // NOT resize with the viewport.
        FramebufferSpecification rsmSpec;
        rsmSpec.DebugName = "RSM";
        rsmSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA8F );  // Albedo (flux colour)
        rsmSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F ); // Normal
        rsmSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F ); // WorldPos
        rsmSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F ); // Emissive (unused)
        // Matches the G-buffer's depth format because "mirror m_GBuffer" includes the depth attachment:
        // the RSM pipeline is created from the G-buffer's spec, and a differing depth format makes the
        // two render passes incompatible.
        rsmSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::DEPTH32F );
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
        if ( m_SSRResourcesFailed )
            return false;

        // Same gate as GI: the trace target and its ping-pong history are sampled/blended RGBA32F.
        if ( !HasFloatRenderTargetSupport() )
        {
            LOG_WARN( "[SceneRenderer] SSR needs blendable float render targets, which this device does not "
                      "report — reflections stay off." );
            m_SSRResourcesFailed = true;
            return false;
        }

        Renderer::GetInstance().WaitDeviceIdle();

        // SSR trace target (HDR reflection colour, reflectance in .a). Traced first, then denoised and
        // composited — blending the raw single-sample trace straight onto the scene looks stippled.
        FramebufferSpecification ssrSpec;
        ssrSpec.DebugName = "SSRTrace";
        ssrSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F );
        m_SSRBuffer = Graphic::Framebuffer::Create( ssrSpec );
        m_SSRBuffer->Resize( m_TargetFramebuffer->GetFramebufferWidth(),
                             m_TargetFramebuffer->GetFramebufferHeight() );

        RegisterSystem<System::SSRRenderer>( "SSRSystem", this, m_TargetFramebuffer, m_RenderGraphBuilder );
        const auto& ssrSys = SP_CAST( System::SSRRenderer, m_RenderSystems["SSRSystem"] );
        ssrSys->SetTraceBuffer( m_SSRBuffer ); // must be set BEFORE Initialize()
        if ( !ssrSys->Initialize() )
        {
            LOG_WARN( "[SceneRenderer] SSR system unavailable." );
            m_SSRResourcesFailed = true;
            m_SSRBuffer.reset();
            return false;
        }

        m_SSRResourcesReady = true;
        return true;
    }

    void SceneRenderer::Resize( const uint32_t width, const uint32_t height )
    {
        if ( width == 0 && height == 0 )
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
        if ( m_SSRBuffer )
            m_SSRBuffer->Resize( width, height );
        if ( m_GIBuffer )
            m_GIBuffer->Resize( width, height );
        // m_RSMBuffer is deliberately NOT resized: it is a fixed-resolution light-space target, unrelated
        // to the viewport. Its accumulation history is invalidated by the GI system's own size check.

        // Keep the post-process chain framebuffers in lock-step with the scene target.
        if ( const auto& maskFb = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
                                       ->GetSilhouetteMaskFramebuffer() )
            maskFb->Resize( width, height );

        if ( const auto& overdrawFb =
                  UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetOverdrawFramebuffer() )
            overdrawFb->Resize( width, height );

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

    void SceneRenderer::SubmitTerrain( const glm::mat4& transform, float size, int resolution, float heightScale,
                                       float noiseFrequency, int seed, const glm::vec3& layerModes,
                                       Image2D* splatMap, const MaterialOverrides& overrides )
    {
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )
             ->Submit( { .Transform      = transform,
                         .Size           = size,
                         .Resolution     = resolution,
                         .HeightScale    = heightScale,
                         .NoiseFrequency = noiseFrequency,
                         .Seed           = seed,
                         .LayerModes     = layerModes,
                         .SplatMap       = splatMap,
                         .Overrides      = overrides } );
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

    void SceneRenderer::SubmitInstancedMesh( const Mesh* mesh, const MaterialInstancePtr& material,
                                             const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                             bool castShadows )
    {
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitInstancedMesh( { .Mesh        = static_cast<Desert::StaticMesh*>( const_cast<Mesh*>( mesh ) ),
                                      .Material    = material,
                                      .Transforms  = transforms,
                                      .CastShadows = castShadows } );
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

    void SceneRenderer::SetEnvironment( const std::shared_ptr<MaterialSkybox>& material, float intensity )
    {
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->PrepareMaterial( material, intensity );
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

    void SceneRenderer::AddPointLight( ShaderProtocols::PointLightPayload&& pointLight )
    {
        m_PointLight.PointLights.push_back( std::move( pointLight ) );
    }

    void SceneRenderer::AddSpotLight( ShaderProtocols::SpotLightPayload&& spotLight )
    {
        m_SpotLight.SpotLights.push_back( std::move( spotLight ) );
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

    void SceneRenderer::ExecuteRenderGraph()
    {
        const auto& sortedPasses = m_RenderGraphBuilder.GetSortedPasses();

        auto& renderer = Renderer::GetInstance();

        // Consecutive passes that share the same target framebuffer are merged into a single
        // vkCmdBeginRenderPass/EndRenderPass pair.  The first pass in each group issues a
        // CLEAR begin; subsequent passes in the same group just call ExecuteFunc() inside the
        // already-open render pass.  This lets the skybox draw first and the geometry draw on
        // top without either pass clearing the other's output.
        std::shared_ptr<Framebuffer> currentFb;

        for ( const auto& pass : sortedPasses )
        {
            if ( !pass.CachedRenderPass )
                continue;

            // Debug-phase passes (BB / colliders) are deferred to ExecuteDebugOverlay(), Transparency-phase
            // (particles) to ExecuteTransparency(), and UI-phase (Render2D canvas) to ExecuteUI(): all run
            // AFTER the deferred lighting composite so they aren't painted over by lit geometry, and as LOAD
            // overlays so a CLEAR begin here never wipes the depth later overlays test against (the particle
            // top-down bug / grid-through-meshes).
            //
            // THE SET IS NAMED ONCE, in RenderPhase.hpp. It used to be this disjunction plus one equality
            // in each of the three Execute* functions below — four expressions of one policy, and nothing
            // forced them to agree.
            if ( RenderPhase::IsDeferredOverlay( pass.Phase ) )
            {
                continue;
            }

            const auto passFb = pass.CachedRenderPass->GetSpecification().TargetFramebuffer;

            if ( passFb != currentFb )
            {
                DESERT_PROFILE_SCOPE( "RenderPass Begin/End" ); // driver vkCmdBeginRenderPass + layout transitions
                if ( currentFb )
                    renderer.EndRenderPass();
                renderer.BeginRenderPass( pass.CachedRenderPass.get(), true );
                currentFb = passFb;
            }

            DESERT_PROFILE_PASS_DYNAMIC( pass.Name.c_str() );

            // Debug-utils region: RenderDoc/Xcode show every graph pass by name in the event tree.
            renderer.BeginDebugLabel( pass.Name.c_str() );
            pass.ExecuteFunc();
            renderer.EndDebugLabel();
        }

        if ( currentFb )
            renderer.EndRenderPass();
    }

    void SceneRenderer::ExecuteDebugOverlay()
    {
        // The main graph loop skipped this phase for this function to draw it; if the register
        // in RenderPhase.hpp ever stops naming it, the loop would draw it BEFORE the composite
        // and this function would find nothing. That is a compile error rather than a frame.
        static_assert( RenderPhase::IsDeferredOverlay( RenderPhase::Debug ),
                       "ExecuteDebugOverlay draws RenderPhase::Debug, so the main loop must skip it" );

        const auto& sortedPasses = m_RenderGraphBuilder.GetSortedPasses();

        auto& renderer = Renderer::GetInstance();

        // LOAD (clearFrame = false) each debug pass over the finished scene color so the lines sit on
        // top of both the forward geometry and the deferred lighting composite. Depth-tested against the
        // target's depth: in Forward that occludes lines behind meshes; in Deferred the target has no
        // opaque-geometry depth (it lives in the G-buffer), so the debug lines read as a clear overlay —
        // exactly what's wanted, since the point is to SEE the boxes even where they hug the mesh.
        std::shared_ptr<Framebuffer> currentFb;
        for ( const auto& pass : sortedPasses )
        {
            if ( !pass.CachedRenderPass || pass.Phase != RenderPhase::Debug )
                continue;

            const auto passFb = pass.CachedRenderPass->GetSpecification().TargetFramebuffer;
            if ( passFb != currentFb )
            {
                if ( currentFb )
                    renderer.EndRenderPass();
                renderer.BeginRenderPass( pass.CachedRenderPass.get(), false );
                currentFb = passFb;
            }

            DESERT_PROFILE_PASS_DYNAMIC( pass.Name.c_str() );
            renderer.BeginDebugLabel( pass.Name.c_str() );
            pass.ExecuteFunc();
            renderer.EndDebugLabel();
        }

        if ( currentFb )
            renderer.EndRenderPass();
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
             ->SetCloudSettings( present, data, windOffset, m_CloudQuality, heroClouds );
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

    void SceneRenderer::ExecuteTransparency()
    {
        // The main graph loop skipped this phase for this function to draw it; if the register
        // in RenderPhase.hpp ever stops naming it, the loop would draw it BEFORE the composite
        // and this function would find nothing. That is a compile error rather than a frame.
        static_assert( RenderPhase::IsDeferredOverlay( RenderPhase::Transparency ),
                       "ExecuteTransparency draws RenderPhase::Transparency, so the main loop must skip it" );

        const auto& sortedPasses = m_RenderGraphBuilder.GetSortedPasses();

        auto& renderer = Renderer::GetInstance();

        // LOAD (clearFrame = false) each transparency pass over the finished scene color so billboards
        // (particles) sit on top of the composited opaque scene instead of under it. In Deferred the
        // lighting composite runs AFTER the graph, so recording these inside the graph made them show only
        // against the sky (no geometry to overwrite them) and vanish against the ground — see the header.
        // Depth against the target is intentionally NOT tested (the particle pipelines set DepthTest off),
        // so glow/sparks read as "always on top"; a per-emitter occlude toggle can bring it back later.
        std::shared_ptr<Framebuffer> currentFb;
        for ( const auto& pass : sortedPasses )
        {
            if ( !pass.CachedRenderPass || pass.Phase != RenderPhase::Transparency )
                continue;

            const auto passFb = pass.CachedRenderPass->GetSpecification().TargetFramebuffer;
            if ( passFb != currentFb )
            {
                if ( currentFb )
                    renderer.EndRenderPass();
                renderer.BeginRenderPass( pass.CachedRenderPass.get(), false );
                currentFb = passFb;
            }

            DESERT_PROFILE_PASS_DYNAMIC( pass.Name.c_str() );
            renderer.BeginDebugLabel( pass.Name.c_str() );
            pass.ExecuteFunc();
            renderer.EndDebugLabel();
        }

        if ( currentFb )
            renderer.EndRenderPass();
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

    void SceneRenderer::ExecuteUI()
    {
        // The main graph loop skipped this phase for this function to draw it; if the register
        // in RenderPhase.hpp ever stops naming it, the loop would draw it BEFORE the composite
        // and this function would find nothing. That is a compile error rather than a frame.
        static_assert( RenderPhase::IsDeferredOverlay( RenderPhase::UI ),
                       "ExecuteUI draws RenderPhase::UI, so the main loop must skip it" );

        const auto& sortedPasses = m_RenderGraphBuilder.GetSortedPasses();

        auto& renderer = Renderer::GetInstance();

        // LOAD (clearFrame = false) each UI pass over the finished scene so the Render2D canvas sits on top
        // of everything. A CLEAR begin (as the main graph loop uses) would wipe the target's depth, which
        // the grid/debug overlays LOAD afterwards — that clear was the grid-through-meshes regression.
        std::shared_ptr<Framebuffer> currentFb;
        for ( const auto& pass : sortedPasses )
        {
            if ( !pass.CachedRenderPass || pass.Phase != RenderPhase::UI )
                continue;

            const auto passFb = pass.CachedRenderPass->GetSpecification().TargetFramebuffer;
            if ( passFb != currentFb )
            {
                if ( currentFb )
                    renderer.EndRenderPass();
                renderer.BeginRenderPass( pass.CachedRenderPass.get(), false );
                currentFb = passFb;
            }

            DESERT_PROFILE_PASS_DYNAMIC( pass.Name.c_str() );
            renderer.BeginDebugLabel( pass.Name.c_str() );
            pass.ExecuteFunc();
            renderer.EndDebugLabel();
        }

        if ( currentFb )
            renderer.EndRenderPass();
    }

    void SceneRenderer::ClearMainFramebuffer()
    {
        auto& renderer = Renderer::GetInstance();

        auto clearRenderPass = RenderPass::Create( {
             .TargetFramebuffer = m_TargetFramebuffer,
             .DebugName         = "ClearTargetFramebuffer",
        } );

        renderer.BeginRenderPass( clearRenderPass.get(), true );
        renderer.EndRenderPass();
    }

} // namespace Desert::Graphic