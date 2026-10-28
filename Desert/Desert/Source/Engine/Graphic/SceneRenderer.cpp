#include <Engine/Graphic/Systems/Scene/PostProcessing/LocalExposureRenderer.hpp>
#include <Engine/Graphic/PostProcessing/LocalExposureRules.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>
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
#include <Engine/Graphic/VisibilityCulling.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <unordered_set>
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

#include <Engine/Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Debug/DebugDrawRenderer.hpp>
#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>
#include <Engine/Graphic/WorldParameterTable.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>

#include <cstring>
#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>

namespace Desert::Graphic
{
    void SceneRenderer::Init()
    {
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
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
            // The build is staged (AdvanceRendererBuild), so "built in" is the stages' sum wherever they ran
            // and "of it here" is what THIS call still had to do; the worst stage is the frame an owner that
            // advances one stage per tick actually pays.
            LOG_INFO( "[SceneRenderer] View '{}' built in {:.1f} ms over {} stages ({:.1f} ms of it here; worst "
                      "stage '{}' {:.1f} ms; {} systems, {} cached pipelines) and bound its first scene in "
                      "{:.1f} ms.",
                      m_ViewResources.GetName(), m_RendererBuildMs, m_RendererBuildStage,
                      ms( started, resourcesDone ), m_RendererBuildWorstStage, m_RendererBuildWorstMs,
                      m_RenderSystemOrder.size(), m_PipelineCache.Size(), ms( resourcesDone, done ) );
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
            // MEASURED: the view's ledger rows and its scene's, not the ViewTargetCensus forecast (RT-LEDGER-3).
            LOG_INFO( "[ViewMemory] view '{}' {}x{} ledger: {}", m_ViewResources.GetName(), viewW, viewH,
                      FormatViewLedgerMemory( TakeLedgerMemory() ) );
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

        // Whatever an owner already advanced ahead of time (AdvanceRendererBuild) is not rebuilt; the rest
        // is built here, in this call.
        while ( !AdvanceRendererBuild() )
        {
        }
        m_RendererResourcesBuilt = true;
        return true;
    }

    bool SceneRenderer::AdvanceRendererBuild()
    {
        if ( m_RendererBuildDone )
            return true;

        const auto  started = std::chrono::steady_clock::now();
        const char* stage   = BuildRendererStage( m_RendererBuildStage );
        if ( stage == nullptr )
        {
            m_RendererBuildDone = true;
            return true;
        }
        const float ms =
             std::chrono::duration<float, std::milli>( std::chrono::steady_clock::now() - started ).count();
        m_RendererBuildMs += ms;
        if ( ms >= m_RendererBuildWorstMs )
        {
            m_RendererBuildWorstMs    = ms;
            m_RendererBuildWorstStage = stage;
        }
        ++m_RendererBuildStage;
        return false;
    }

    const char* SceneRenderer::BuildRendererStage( std::size_t stage )
    {
        // EVERYTHING BUILT BELOW BELONGS TO THIS RENDERER, and one scope says so for all of it. This
        // function and the twenty render systems it constructs create roughly a hundred and twenty device
        // objects — framebuffers, LUT images, pipelines and the materials the passes own — across twenty
        // files, and none of them is rebuildable from a file. Claiming them individually would mean
        // editing twenty files to answer one question, and the twenty-first system would be attributed by
        // nobody. See Engine/Graphic/ResourceLedger.hpp.
        //
        // A texture or mesh lazily built INSIDE this window is still the asset's: the services claim
        // theirs explicitly after the build, and an explicit claim overrides the ambient one.
        // AND TO THIS VIEW (RT-LEDGER-3): the rows carry the view's instance, and the view's budget
        // (HeldBytes) is those rows measured, not the ViewTargetCensus forecast of them.
        const ResourceAttributionScope owned = OpenViewLedgerScope();

        // ONE STAGE PER CALL, in the order the systems depend on each other: the post chain is wired to
        // framebuffers the earlier stages made, so a later stage reads its predecessors from m_RenderSystems.
        switch ( stage )
        {
            case 0:
            {
                // The surface's size, never the window's: see ViewExtent.
                const uint32_t width  = m_ViewExtent.Width;
                const uint32_t height = m_ViewExtent.Height;
                // The render set starts at the output extent; the first frame's split resizes it
                // (ResizeRenderTargets).
                m_RenderExtent = m_ViewExtent;

                // Framebuffer. MSAA applies HERE only: every scene system renders into this target at N samples
                // and the render pass resolves to single-sample for the post stack. The count follows the
                // anti-aliasing method every frame (ApplySceneSampleCount, from BeginScene); this is the count it
                // starts at.
                //
                // ONE SAMPLE HERE, whatever the machine chose: MSAA applies only on the forward path (AA2,
                // Scalability::ResolveAntiAliasingForPath) and no scene — so no path — is known until the first
                // BeginScene, which raises the count for a forward scene under MSAA. Starting at 1 means a
                // deferred scene never allocates a multisampled target it cannot use.
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

                // Deferred G-buffer (populated only when SceneSettings::RenderPath == Deferred; allocated always
                // so the toggle is live). GBufferA = Albedo.rgb + Metallic.a (RGBA8F); GBufferB = Normal.rgb +
                // Roughness.a (kGBufferB); slot 2 = the shading word (R32_UINT, ShadingModelContract.glslh). World
                // position is not stored: readers rebuild it from the depth attachment
                // (Common/ReconstructPosition.glslh); shared depth.
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

                // NOTE: SSR resources are deliberately NOT created here — see EnsureSSRResources(). Every
                // PreviewViewport (asset thumbnails, the Details mesh preview) builds its OWN SceneRenderer, so
                // anything allocated in this constructor is paid for once PER PREVIEW, and a preview never turns
                // SSR on. They are allocated on first actual use instead (the traced GI's caches likewise, in
                // AddFrameTracedGI).

                // Scene systems render into the shared target framebuffer; post-process systems form an
                // explicit chain (Mesh silhouette mask -> Jump Flood outline -> Tonemap).
                RegisterSystem<System::SkyboxRenderer>( "SkyboxSystem", this, m_TargetFramebuffer );
                RegisterSystem<System::MeshRenderer>( "MeshSystem", this, m_TargetFramebuffer );
                RegisterSystem<System::JumpFloodOutlineRenderer>( "JumpFloodSystem", this, m_TargetFramebuffer );
                return "Targets";
            }
            case 1:
            {
                // NAMED AND SURVIVED, NOT VERIFIED. `DESERT_VERIFY( false )` stood at each of the nine sites
                // below, so a render system that refused to initialise took the whole process with it — and
                // after Г22 that became REACHABLE for the first time: a typo in StaticMeshLit.shader now
                // produces an honest refusal from MeshRenderer::Initialize, which this line then turned into a
                // crash. The engine already has the rule for this one rung lower — VulkanRendererAPI::
                // BindGraphicsPipeline skips every draw through a pipeline that was not built — so a system
                // that could not initialise means its passes draw nothing, not that the editor vanishes.
                if ( !SP_CAST( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )->Initialize() )
                    LOG_ERROR( "[SceneRenderer] the skybox system did not initialise; the sky will not draw." );
                return "Skybox";
            }
            case 2:
            {
                const auto& meshSystem = SP_CAST( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
                if ( !meshSystem->Initialize() )
                    LOG_ERROR( "[SceneRenderer] the mesh system did not initialise; meshes will not draw." );
                return "Mesh";
            }
            case 3:
            {
                const auto& meshSystem = SP_CAST( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
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
                else if ( const auto caster =
                               terrainSystem->CreateShadowPipeline( meshSystem->GetCascadeFramebuffer() );
                          !caster )
                {
                    LOG_ERROR( "[SceneRenderer] the terrain will not cast shadows: {}", caster.GetError() );
                }
                else
                {
                    meshSystem->AddShadowCaster( terrainSystem );
                }

                // The landscape grass (GRASS-GPU): generated per view before the cascades, drawn by the mesh
                // renderer's instanced cells (GrassRenderer::AddFrame).
                RegisterSystem<System::GrassRenderer>( "GrassSystem", this, m_TargetFramebuffer );
                if ( const auto init =
                          SP_CAST( System::GrassRenderer, m_RenderSystems["GrassSystem"] )->Initialize();
                     !init )
                {
                    LOG_ERROR( "[SceneRenderer] the grass system did not initialise; landscape grass will not "
                               "grow: {}",
                               init.GetError() );
                }

                // The water surface (WATER-W6): drawn by its own deferred node (AddFrameWater), after the scene
                // copy.
                RegisterSystem<System::WaterRenderer>( "WaterSystem", this, m_TargetFramebuffer );
                if ( const auto init =
                          SP_CAST( System::WaterRenderer, m_RenderSystems["WaterSystem"] )->Initialize();
                     !init )
                {
                    LOG_ERROR( "[SceneRenderer] the water system did not initialise; water will not draw: {}",
                               init.GetError() );
                }
                return "Terrain";
            }
            case 4:
            {
                const auto& meshSystem = SP_CAST( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
                const auto& jumpFloodSystem =
                     SP_CAST( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
                if ( !jumpFloodSystem->Initialize() )
                    LOG_ERROR(
                         "[SceneRenderer] the outline system did not initialise; selection outlines are off." );

                // Feed the silhouette mask (produced by the mesh system) into the Jump Flood outline.
                jumpFloodSystem->SetMaskFramebuffer( meshSystem->GetSilhouetteMaskFramebuffer() );
                return "JumpFlood";
            }
            case 5:
            {
                const auto& jumpFloodSystem =
                     SP_CAST( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
                // Tonemap consumes the Jump Flood output (the outlined scene).
                RegisterSystem<System::TonemapRenderer>( "TonemapSystem", this,
                                                         jumpFloodSystem->GetSystemFramebuffer() );
                const auto& tonemapSystem = SP_CAST( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
                if ( !tonemapSystem->Initialize() )
                    LOG_ERROR(
                         "[SceneRenderer] the tonemap system did not initialise; the viewport stays black." );
                return "Tonemap";
            }
            case 6:
            {
                // Backdrop blur: a blurred snapshot of the scene colour the UI canvas samples for "glass" panels.
                // Runs before the UI extension point (whose passes write into this same target, so it cannot
                // sample it directly).
                RegisterSystem<System::BackdropBlurRenderer>( "BackdropBlurSystem", this, m_TargetFramebuffer );
                if ( const auto& backdropSystem =
                          SP_CAST( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] );
                     !backdropSystem->Initialize() )
                {
                    LOG_WARN( "Backdrop blur unavailable — UI glass panels will draw as flat tint" );
                }
                return "BackdropBlur";
            }
            case 7:
            {
                // Bloom reads the HDR scene color and produces a compute mip-chain glow that tonemap adds in.
                RegisterSystem<System::BloomRenderer>( "BloomSystem", this, m_TargetFramebuffer );
                const auto& bloomSystem = SP_CAST( System::BloomRenderer, m_RenderSystems["BloomSystem"] );
                if ( !bloomSystem->Initialize() )
                    LOG_ERROR(
                         "[SceneRenderer] the bloom system did not initialise; the scene renders without glow." );
                return "Bloom";
            }
            case 8:
            {
                // Light shafts: the atmosphere sun's screen-space streaks, masked and radially blurred from the
                // HDR scene colour; tonemap adds them in the way it adds bloom. Non-fatal: a sky without streaks
                // must never take a scene down.
                RegisterSystem<System::LightShaftRenderer>( "LightShaftSystem", this, m_TargetFramebuffer );
                const auto& lightShaftSystem =
                     SP_CAST( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
                if ( !lightShaftSystem->Initialize() )
                    LOG_WARN( "[SceneRenderer] Light shaft system unavailable." );
                return "LightShaft";
            }
            case 9:
            {
                // Lens flare: the camera's own response to the sun disc — ghosts, halo and streak gathered from
                // the same HDR scene colour, added in by the tonemap the way bloom is. Non-fatal, like the shafts.
                RegisterSystem<System::LensFlareRenderer>( "LensFlareSystem", this, m_TargetFramebuffer );
                const auto& lensFlareSystem =
                     SP_CAST( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
                if ( const auto flareInit = lensFlareSystem->Initialize(); !flareInit )
                    LOG_WARN( "[SceneRenderer] Lens flare system unavailable: {}", flareInit.GetError() );
                return "LensFlare";
            }
            case 10:
            {
                // SSAO (fullscreen G-buffer -> AO factor). Its target is a per-frame graph transient
                // (AddFrameSSAO); the deferred Composite reads it. Deferred only. Non-fatal.
                RegisterSystem<System::SSAORenderer>( "SSAOSystem", this, m_TargetFramebuffer );
                if ( !SP_CAST( System::SSAORenderer, m_RenderSystems["SSAOSystem"] )->Initialize() )
                    LOG_WARN( "[SceneRenderer] SSAO system unavailable." );
                return "SSAO";
            }
            case 11:
            {
                // The G-buffer depth into a multisampled scene depth (Deferred: DepthExpand); nothing is built at
                // MSAA 1.
                RegisterSystem<System::DepthExpandRenderer>( "DepthExpandSystem", this, m_TargetFramebuffer );
                if ( const auto expandInit =
                          SP_CAST( System::DepthExpandRenderer, m_RenderSystems["DepthExpandSystem"] )
                               ->Initialize();
                     !expandInit )
                    LOG_ERROR( "[SceneRenderer] DepthExpand unavailable (deferred depth at MSAA): {}",
                               expandInit.GetError() );
                return "DepthExpand";
            }
            case 12:
            {
                // The multisampled scene depth into the 1x depth compute passes sample (Scene: DepthResolve); MSAA
                // > 1 only.
                RegisterSystem<System::SceneDepthResolveRenderer>( "SceneDepthResolveSystem", this,
                                                                   m_TargetFramebuffer );
                if ( const auto resolveInit =
                          SP_CAST( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] )
                               ->Initialize();
                     !resolveInit )
                    LOG_ERROR( "[SceneRenderer] SceneDepthResolve unavailable (fog and clouds at MSAA): {}",
                               resolveInit.GetError() );
                return "SceneDepthResolve";
            }
            case 13:
            {
                // The sample-0 shader resolve of the scene target's SampleZero graph colours (the view's velocity
                // at MSAA).
                RegisterSystem<System::GraphColorResolveRenderer>( "GraphColorResolveSystem", this,
                                                                   m_TargetFramebuffer );
                if ( const auto colorResolveInit =
                          SP_CAST( System::GraphColorResolveRenderer, m_RenderSystems["GraphColorResolveSystem"] )
                               ->Initialize();
                     !colorResolveInit )
                    LOG_ERROR(
                         "[SceneRenderer] GraphColorResolve unavailable (velocity at MSAA reads zero motion): {}",
                         colorResolveInit.GetError() );
                return "GraphColorResolve";
            }
            case 14:
            {
                RegisterSystem<System::CopyRenderer>( "SceneColorCopySystem", this, m_TargetFramebuffer );
                if ( !SP_CAST( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] )->Initialize() )
                    LOG_WARN( "[SceneRenderer] Scene-color copy system unavailable (glass refraction off)." );

                RegisterSystem<System::VelocityViewRenderer>( "VelocityViewSystem", this, m_TargetFramebuffer );
                if ( const auto velocityViewInit =
                          SP_CAST( System::VelocityViewRenderer, m_RenderSystems["VelocityViewSystem"] )
                               ->Initialize();
                     !velocityViewInit )
                    LOG_ERROR( "[SceneRenderer] Velocity view mode unavailable: {}", velocityViewInit.GetError() );
                return "SceneColorCopy";
            }
            case 15:
            {
                RegisterSystem<System::VelocityViewRenderer>( "VelocityViewSystem", this, m_TargetFramebuffer );
                if ( const auto velocityViewInit =
                          SP_CAST( System::VelocityViewRenderer, m_RenderSystems["VelocityViewSystem"] )
                               ->Initialize();
                     !velocityViewInit )
                    LOG_ERROR( "[SceneRenderer] Velocity view mode unavailable: {}", velocityViewInit.GetError() );
                return "VelocityView";
            }
            case 16:
            {
                // Atmosphere and fog: aerial perspective on opaque with exponential height fog over it — one
                // compute evaluation issued outside the graph (ExecuteAtmosphericFog) and one apply pass in the
                // translucency, below the particles by the order of AddFrameTranslucency's calls.
                // Non-fatal: neither must ever take a scene down.
                RegisterSystem<System::HeightFogRenderer>( "HeightFogSystem", this, m_TargetFramebuffer );
                if ( const auto fogInit =
                          SP_CAST( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] )->Initialize();
                     !fogInit )
                    LOG_WARN( "[SceneRenderer] Height fog system unavailable: {}", fogInit.GetError() );
                return "HeightFog";
            }
            case 17:
            {
                // Volumetric clouds: a march through a spherical shell, issued outside the graph
                // (VolumetricCloudRenderer::DeclareFrameNodes) with one composite pass in the translucency, above
                // the fog and below the particles by the order of AddFrameTranslucency's calls. Non-fatal: a
                // missing sky must never take a scene down.
                RegisterSystem<System::VolumetricCloudRenderer>( "VolumetricCloudSystem", this,
                                                                 m_TargetFramebuffer );
                if ( const auto cloudInit =
                          SP_CAST( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] )
                               ->Initialize();
                     !cloudInit )
                    LOG_WARN( "[SceneRenderer] Volumetric cloud system unavailable: {}", cloudInit.GetError() );
                return "VolumetricCloud";
            }
            case 18:
            {
                // GPU particles: compute-simulated sprites drawn by AddFrameTranslucency. Non-fatal.
                RegisterSystem<System::ParticleRenderer>( "ParticleSystem", this, m_TargetFramebuffer );
                if ( !SP_CAST( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->Initialize() )
                    LOG_WARN( "[SceneRenderer] Particle system unavailable." );
                return "Particle";
            }
            case 19:
            {
                // Deferred lighting (fullscreen G-buffer shade + debug view). Runs in the manual chain, only when
                // RenderPath == Deferred. Non-fatal if it fails to init (deferred path is simply unavailable).
                RegisterSystem<System::DeferredLightingRenderer>( "DeferredLightingSystem", this,
                                                                  m_TargetFramebuffer );
                // A program only some Shadows methods need failing refuses those methods by name
                // (DeferredLightingRenderer::Refusals, read by every ResolveRenderMethods), not the whole path.
                const auto& deferred =
                     SP_CAST( System::DeferredLightingRenderer, m_RenderSystems["DeferredLightingSystem"] );
                if ( const auto deferredInit = deferred->Initialize(); !deferredInit )
                    LOG_WARN( "[SceneRenderer] Deferred lighting system unavailable: {}",
                              deferredInit.GetError() );
                m_ProgramRefusals.Shadows = deferred->Refusals().Shadows;
                // The screen-space subsurface scatter (SSS-PROFILE): without it the profile materials shade unsplit.
                RegisterSystem<System::SubsurfaceRenderer>( "SubsurfaceSystem", this, m_TargetFramebuffer );
                if ( const auto subsurfaceInit =
                          SP_CAST( System::SubsurfaceRenderer, m_RenderSystems["SubsurfaceSystem"] )->Initialize();
                     !subsurfaceInit )
                    LOG_ERROR( "[SceneRenderer] Subsurface scattering unavailable: {}", subsurfaceInit.GetError() );
                return "DeferredLighting";
            }
            case 20:
            {
                const auto& tonemapSystem = SP_CAST( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
                // Auto-exposure measures the HDR scene luminance into a 1x1 buffer that tonemap reads.
                RegisterSystem<System::AutoExposureRenderer>( "AutoExposureSystem", this, m_TargetFramebuffer );
                const auto& autoExposureSystem =
                     SP_CAST( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
                if ( !autoExposureSystem->Initialize() )
                    LOG_ERROR(
                         "[SceneRenderer] auto-exposure did not initialise; exposure stays at its default." );
                tonemapSystem->SetAutoExposureImage( autoExposureSystem->GetAdaptedLuminanceImage() );
                // Local exposure (UE Bilateral): grid + blurred log luminance the tonemap and the bloom read.
                RegisterSystem<System::LocalExposureRenderer>( "LocalExposureSystem", this, m_TargetFramebuffer );
                if ( const auto le =
                          SP_CAST( System::LocalExposureRenderer, m_RenderSystems["LocalExposureSystem"] )->Initialize();
                     !le )
                    LOG_ERROR( "[SceneRenderer] local exposure did not initialise: {}", le.GetError() );
                return "AutoExposure";
            }
            case 21:
            {
                const auto& tonemapSystem = SP_CAST( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
                // FXAA consumes the tonemapped image (LDR). It only runs when the machine's post AA is FXAA
                // (Common::Settings::MachineSettings::AA — it left SceneSettings with К3).
                RegisterSystem<System::FXAARenderer>( "FXAASystem", this, tonemapSystem->GetSystemFramebuffer() );
                if ( !SP_CAST( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Initialize() )
                    LOG_ERROR( "[SceneRenderer] FXAA did not initialise; the image is drawn without post AA." );
                return "FXAA";
            }
            case 22:
            {
                const auto& tonemapSystem = SP_CAST( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );
                // SMAA consumes the same tonemapped image. Runs only when the machine's post AA is SMAA.
                RegisterSystem<System::SMAARenderer>( "SMAASystem", this, tonemapSystem->GetSystemFramebuffer() );
                if ( !SP_CAST( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Initialize() )
                    LOG_ERROR( "[SceneRenderer] SMAA did not initialise; the image is drawn without post AA." );

                return "SMAA";
            }
            default:
                return nullptr;
        }
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
            return std::format( "{} #{}", kind, number );
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

    std::string FormatViewLedgerMemory( const ViewLedgerMemory& memory )
    {
        constexpr double kMiB = 1024.0 * 1024.0;
        return std::format( "{} {} rows {:.2f} MiB (the view's budget); {} {} rows {:.2f} MiB (scene-shared, the "
                            "scene's own budget entry)",
                            ResourceOwnerInstanceName( memory.View.Instance ), memory.View.Live,
                            static_cast<double>( memory.View.Bytes ) / kMiB,
                            ResourceOwnerInstanceName( memory.Scene.Instance ), memory.Scene.Live,
                            static_cast<double>( memory.Scene.Bytes ) / kMiB );
    }

    uint64_t SceneRenderer::HeldBytes() const
    {
        // READ FROM THE ROWS (RT-LEDGER-3), and only from them: every entry point that can allocate opens the
        // view's ledger scope, so the targets, the history, the per-view copies and the RT rows all carry the
        // view's instance. The ledger is the one statement of those bytes; the budget, the [ViewMemory] lines,
        // the overlay and the [Resources] census cannot disagree. The scene cache's rows carry the scene's
        // instance and are its own entry (SceneHoldings), never here — nor are a capture face view's, which
        // accounts under its scene's instance (LedgerView::Borrowed).
        if ( m_LedgerView.Borrowed )
            return 0;
        return ResourceLedger::TakeFor( m_LedgerView.Instance ).Bytes;
    }

    ViewLedgerMemory SceneRenderer::TakeLedgerMemory() const
    {
        return ViewLedgerMemory{ .View  = ResourceLedger::TakeFor( m_LedgerView.Instance ),
                                 .Scene = ResourceLedger::TakeFor( m_SceneLedgerInstance ) };
    }

    std::vector<Engine::ViewBudget::HeldView> SceneRenderer::SceneHoldings()
    {
        std::vector<Engine::ViewBudget::HeldView> held;
        for ( const ResourceInstanceCensus& scene : ResourceLedger::TakeInstances( ResourceInstanceKind::Scene ) )
            held.push_back(
                 { std::format( "{} (shared by its views)", ResourceOwnerInstanceName( scene.Instance ) ),
                   scene.Bytes } );
        return held;
    }

    SceneRenderer::LedgerView::~LedgerView()
    try
    {
        // A view accounting under its scene's instance has no rows of its own to check: the scene's rows are
        // held by the scene and outlive this view by design.
        if ( Borrowed )
            return;
        // Every member that held a row of this view has been destroyed by now (this one is declared first).
        const ResourceInstanceCensus left = ResourceLedger::TakeFor( Instance );
        if ( left.Live == 0 )
        {
            LOG_INFO( "[Resources] {} closed: 0 ledger rows left.", ResourceOwnerInstanceName( Instance ) );
        }
        else
        {
            LOG_ERROR( "[Resources] {} closed but {} ledger rows ({} bytes) still carry it: a view member's "
                       "device object outlived the view.",
                       ResourceOwnerInstanceName( Instance ), left.Live, left.Bytes );
            // Each by name (where its creator recorded one), kind, claiming owner and size: these find its holder.
            for ( const ResourceRowSummary& row : ResourceLedger::RowsOf( Instance ) )
                LOG_ERROR( "[Resources]   row #{}: {} '{}' claimed by {}, {}", row.Id,
                           ResourceKindName( row.Kind ),
                           row.Name.empty() ? std::string_view( "unnamed" ) : std::string_view( row.Name ),
                           ResourceOwnerName( row.Owner ),
                           row.BytesKnown ? std::format( "{} bytes", row.Bytes ) : std::string( "size unknown" ) );
        }
    }
    DESERT_DESTRUCTOR_GUARD( "~SceneRenderer::LedgerView" )

    std::vector<Engine::ViewBudget::HeldView> SceneRenderer::LiveHoldings()
    {
        const std::scoped_lock                    lock( LiveRenderersMutex() );
        std::vector<Engine::ViewBudget::HeldView> held;
        held.reserve( LiveRenderers().size() );
        for ( const SceneRenderer* renderer : LiveRenderers() )
        {
            // A capture face view's rows are its scene's entry (SceneHoldings): listing it here would count them
            // twice, or list a view at 0 bytes that no viewer opened.
            if ( renderer->m_LedgerView.Borrowed )
                continue;
            held.push_back( { renderer->m_ViewResources.GetName(), renderer->HeldBytes() } );
        }
        return held;
    }

    std::string SceneRenderer::DescribeLiveViews()
    {
        uint64_t held = 0;
        for ( const Engine::ViewBudget::HeldView& view : LiveHoldings() )
            held += view.Bytes;
        uint64_t shared = 0;
        for ( const Engine::ViewBudget::HeldView& scene : SceneHoldings() )
            shared += scene.Bytes;
        return std::format( "{} live, {:.1f} MiB; scenes {:.1f} MiB shared", ViewResourceRegistry::LiveCount(),
                            static_cast<double>( held ) / ( 1024.0 * 1024.0 ),
                            static_cast<double>( shared ) / ( 1024.0 * 1024.0 ) );
    }

    SceneRenderer::SceneRenderer( const ViewExtent& extent, const ViewProfile& profile,
                                  const ResourceOwnerInstance accountUnder )
         : m_LedgerView( accountUnder ), m_ViewResources( NameView( profile ) ), m_ViewProfile( profile ),
           m_ViewExtent( extent ), m_RenderExtent( extent )
    {
        // Every device object this view makes, here and in each entry point that can allocate (Init, the staged
        // build, the three frame phases, Resize, the Ensure* builders), is opened under the view's ledger
        // instance: the view's budget is those rows (HeldBytes), measured, not a forecast (RT-LEDGER-3).
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
        {
            const std::scoped_lock lock( LiveRenderersMutex() );
            LiveRenderers().push_back( this );
        }

        // A level's viewport is BUILT at the machine's Shadows level, so a Low machine never allocates the High
        // maps only to re-allocate them on the first frame. Later level changes: BeginScene.
        // Built for the cascaded sun: the Shadows method is resolved by the first BeginScene; the first frame that
        // draws a ray-traced sun re-budgets to the low-detail cascades (OnUpdate, RebudgetShadowsFor).
        if ( const auto budget = ShadowReallocation( m_ViewProfile, m_Quality, /*sunRayTraced*/ false ) )
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

    Common::BoolResultStr SceneRenderer::UploadWorldParameters( const Desert::Core::Scene& scene )
    {
        const WorldParameterTable table = BuildWorldParameterTable( scene.GetWorldParameters().Values() );
        std::vector<std::byte>    bytes( sizeof( table.Header ) +
                                         table.Slots.size() * sizeof( GpuWorldParameterSlot ) );
        std::memcpy( bytes.data(), &table.Header, sizeof( table.Header ) );
        std::memcpy( bytes.data() + sizeof( table.Header ), table.Slots.data(),
                     table.Slots.size() * sizeof( GpuWorldParameterSlot ) );
        if ( !m_WorldParameters )
            m_WorldParameters = ShaderResources::StorageBuffer::Create( SceneResources::kWorldParametersName,
                                                                        static_cast<uint32_t>( bytes.size() ),
                                                                        Core::kWorldParametersBinding );
        if ( !m_WorldParameters )
            return Common::MakeError( "the view's WorldParameters buffer could not be created" );
        if ( const auto wrote = m_WorldParameters->SetData( bytes.data(), static_cast<uint32_t>( bytes.size() ) );
             !wrote )
            return Common::MakeFormattedError( "the view's WorldParameters ({} bytes) could not be uploaded: {}",
                                               bytes.size(), wrote.GetError() );
        return BOOLSUCCESS;
    }

    NO_DISCARD Common::BoolResultStr SceneRenderer::BeginScene( const Desert::Core::Scene& scene,
                                                                Core::Camera*              camera )
    {
        // This view is where per-frame writes go until the phase returns — set FIRST, before anything writes
        // a per-frame resource; the frame context gets them back on every exit, so nothing written after the
        // last view of a frame lands in this view's copies.
        const ActiveViewScope viewScope( m_ViewResources );
        const ResourceAttributionScope ledger = OpenViewLedgerScope();

        // The LOD preview lease (SetLODPreview) lapses unless the tool renewed it since the last frame.
        if ( m_LODPreview.FramesLeft > 0 )
            --m_LODPreview.FramesLeft;

        // HANDED IN, NOT READ OFF THE SCENE. This used to be `scene.GetMainCamera()`, which is the same
        // answer for every renderer of that scene — so a second view of one world rendered from the
        // first view's camera, and "several viewports" could only ever mean "several worlds". The scene
        // holds a LIST of views now and each one carries its own camera; the view says which.
        m_SceneInfo.ActiveCamera = camera;
        // The world this frame draws and its clock (ViewInputs::SceneIdentity / TimeSeconds, OnUpdate).
        m_SceneGeneration  = scene.GetGeneration();
        m_SceneTimeSeconds = scene.GetWorldTime().GetGameTimeSeconds();
        // The world's parameters, snapshotted once for every pass of this view's frame (per-frame state).
        if ( const auto uploaded = UploadWorldParameters( scene ); !uploaded )
            return uploaded;

        const auto& skyboxSystem = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );

        skyboxSystem->PrepareCamera( m_SceneInfo.ActiveCamera );
        // The scene's one sky light: this view reads it, and captures it when it is the frame's first view.
        skyboxSystem->SetSceneSkyLight( scene.GetSkyLight() );

        // Grid & other authoring aids hide while the game runs - not while it is simulated (UE: Simulate In
        // Editor keeps the editor's viewport and its overlays).
        m_ScenePlaying = scene.IsPlaying() && !scene.IsSimulating();
        // The scene's extension passes, for AddExtensionPoint while OnUpdate builds this frame.
        m_FrameExtensions = &scene.GetExtensionPasses();
#if DESERT_DEV_INSTRUMENTS
        // The scene's debug lines and strings, for this frame's DebugDraw passes.
        if ( !m_DebugDraw )
            m_DebugDraw = std::make_unique<DebugDrawRenderer>( *this );
        m_DebugDraw->SetWorld( &scene.GetDebugDraw() );
#endif
        // The scene's reflection captures, published into this frame's graph (SceneViewInputs).
        m_FrameReflectionCaptures = &scene.GetReflectionCaptures();
        // The scene's ray-tracing geometry (skin cache, BLAS variants), shared by this frame's views.
        m_FrameRayTracingCache = &scene.GetRayTracingCache();
        m_SceneLedgerInstance  = scene.GetLedgerInstance();
        // The scene's local-light shadow cache, which this view's lights draw into and read (UpdateLocalShadows).
        m_FrameLocalShadows = &scene.GetLocalShadows();
        // The scene's mesh distance fields, which this view's global distance field composites from.
        m_FrameDistanceFields = &scene.GetDistanceFields();

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
        // (DebugParams.y, StaticMeshLit / StaticMeshLit_Instanced / StandardSurface's Vertex_Skinned cell). The deferred
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
        // THE ONE RESOLUTION of the view's render methods (Graphic/RenderMethods.hpp): request x machine ceiling x
        // The captures' programs serve every Reflections method that reads the captures: built here, before the
        // resolution, so one that did not build refuses those methods from this frame on (by its name).
        if ( !m_FrameReflectionCaptures->EnsurePipelines() )
            RefuseProgramMethods( kCaptureReflectionMethods, m_FrameReflectionCaptures->FailedProgram(),
                                  "the reflection captures' compute program did not build" );
        // device x render path x the programs the view's renderers built, each lowered axis logged once.
        // m_RenderPath (set above, a debug view may force forward) is what the frame graph branches on, so a
        // method that needs the G-buffer is lowered exactly when the deferred block will not run - under forward,
        // ray-traced sun shadows resolve to CascadedMaps and the cascades are drawn for the forward meshes that
        // read them (UE's forward renderer does the same).
        m_RenderMethods =
             ResolveRenderMethods( post, quality, Common::Scalability::QualityState::Catalog(),
                                   m_RenderPath == Core::RenderPath::Deferred && m_GBuffer, m_ProgramRefusals );
        m_GIPipeline    = GlobalIllumination::PipelineFor( m_RenderMethods.GlobalIllumination.Effective, quality,
                                                           Common::Scalability::QualityState::Catalog() );
        ReportRenderMethods( m_RenderMethods );
        // A PATH-TRACED VIEW renders its own anti-aliasing: each path starts at a random point of its pixel and
        // the accumulation averages them (UE RenderPathTracing bypasses TAA/TSR the same way). No temporal method,
        // no FXAA/SMAA after it, no jitter; the view resolution is native (OnUpdate). The sample count stays the
        // scene path's one: PathTraced needs the G-buffer, and the deferred path is never multisampled. Except
        // under PathTracing.Denoiser RR (System::PathTracer::KeepsDenoisingUpscaler): the view runs DLSS Ray
        // Reconstruction whatever AntiAliasing.Method asks - the denoiser is the path tracer's setting, as UE's
        // r.PathTracing.Denoiser is - the paths go to it raw through the jittered pixel centres, and it is the
        // anti-aliasing.
        if ( PathTraced( m_RenderMethods ) && System::PathTracer::KeepsDenoisingUpscaler( quality ) &&
             !m_DebugView.WireframeMode )
        {
            m_RenderedAntiAliasing.Method      = Common::Scalability::AntiAliasingMethod::DLSSRayReconstruction;
            m_RenderedAntiAliasing.PostProcess = Common::Scalability::AntiAliasingMethod::None;
            m_RenderedAntiAliasing.Reason      = "PathTracing.Denoiser RR: Ray Reconstruction denoises the paths";
            m_AAMode                           = Common::Scalability::AntiAliasingMethod::None;
        }
        else if ( PathTraced( m_RenderMethods ) )
        {
            m_RenderedAntiAliasing.Method      = Common::Scalability::AntiAliasingMethod::None;
            m_RenderedAntiAliasing.PostProcess = Common::Scalability::AntiAliasingMethod::None;
            m_RenderedAntiAliasing.Reason      = "the path tracer averages its own sub-pixel paths";
            m_AAMode                           = Common::Scalability::AntiAliasingMethod::None;
        }
        // THE WIREFRAME VIEW RENDERS NO ANTI-ALIASING (UE EngineShowFlagOverride: "Disable AA in full wireframe
        // mode", AntiAliasing off for VMI_Wireframe). Its image is one-pixel lines over the background: a temporal
        // method's neighbourhood clip cannot tell a stale line from a current one beside it, so the history of a
        // mesh replaced in place (an Undo of a sculpt stroke: same entity, same transform, zero velocity) kept the
        // old dense lines for dozens of frames, and the jitter only made every line shimmer. No temporal method,
        // no FXAA/SMAA, no jitter, native resolution (OnUpdate), as for the path tracer above.
        else if ( m_DebugView.WireframeMode )
        {
            m_RenderedAntiAliasing.Method      = Common::Scalability::AntiAliasingMethod::None;
            m_RenderedAntiAliasing.PostProcess = Common::Scalability::AntiAliasingMethod::None;
            m_RenderedAntiAliasing.Reason      = "the wireframe view draws one-pixel lines, unfiltered";
            m_AAMode                           = Common::Scalability::AntiAliasingMethod::None;
        }
        m_GIIntensity    = post.GIIntensity;
        m_SubsurfaceScattering = post.SubsurfaceScattering;
        m_SubsurfaceScale      = post.SubsurfaceScale;
        m_SSRIntensity   = post.SSRIntensity;
        m_SSRMaxDistance = post.SSRMaxDistance;
        m_SSRMaxSteps    = quality.As<int>( Parameter::ReflectionMaxSteps );
        m_SSRRaysPerPixel      = quality.As<int>( Parameter::ReflectionRaysPerPixel );
        m_DistanceFieldPoolMiB = static_cast<uint32_t>( quality.As<int>( Parameter::DistanceFieldPoolMiB ) );
        m_DisplacementDicingRate = quality.As<int>( Parameter::DisplacementDicingRate );
        m_DisplacementPoolMiB    = static_cast<uint32_t>( quality.As<int>( Parameter::DisplacementPoolMiB ) );
        m_SSAOSamples    = quality.As<int>( Parameter::AmbientOcclusionSamples );
        m_ScreenProbeAdaptivePercent = quality.As<uint32_t>( Parameter::ScreenProbeAdaptiveAllocation );
        m_ScreenProbeShortRangeAO    = quality.As<int>( Parameter::ScreenProbeShortRangeAO ) != 0;
        m_ScreenProbeDownsample      = quality.As<uint32_t>( Parameter::ScreenProbeDownsample );
        m_ScreenProbeTracedTexels    = quality.As<uint32_t>( Parameter::ScreenProbeTracedTexels );
        m_ContactShadowSteps                  = quality.As<int>( Parameter::ShadowContactSteps );
        m_ParallaxQuality = ParallaxQuality{ .MinSteps    = quality.As<int>( Parameter::ShadingParallaxMinSteps ),
                                             .MaxSteps    = quality.As<int>( Parameter::ShadingParallaxMaxSteps ),
                                             .ShadowSteps = quality.As<int>( Parameter::ShadingParallaxShadowSteps ) };
        m_RayTracedShadowSamples =
             static_cast<uint32_t>( quality.As<int>( Parameter::ShadowRayTracedSamplesPerPixel ) );
        m_ShadowFilter = SceneResources::ShadowFilterSettings{
             .BlockerSearchTaps = quality.As<int>( Parameter::ShadowBlockerSearchTaps ),
             .FilterTaps        = quality.As<int>( Parameter::ShadowFilterTaps ) };
        if ( const auto mesh = m_RenderSystems.find( "MeshSystem" ); mesh != m_RenderSystems.end() )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            UNIQUE_GET_AS( System::MeshRenderer, mesh->second )
                 ->SetRainOcclusionQuality( RainOcclusionQuality{
                      .MapSize = quality.As<uint32_t>( Parameter::ShadowRainOcclusionMapSize ),
                      .Extent  = static_cast<float>( quality.As<int>( Parameter::ShadowRainOcclusionExtent ) ),
                      .RefreshFraction = 0.01f * static_cast<float>( quality.As<int>(
                                                      Parameter::ShadowRainOcclusionRefreshPercent ) ) } );
        }
        m_ShadowDenoiserRadius = quality.As<uint32_t>( Parameter::ShadowRayTracedDenoiserRadius );
        m_VirtualShadowQuality = System::VirtualShadowMapQuality{
             .MaxPhysicalPages =
                  static_cast<uint32_t>( quality.As<int>( Parameter::ShadowVirtualMaxPhysicalPages ) ),
             .ResolutionLodBias =
                  0.1f * static_cast<float>( quality.As<int>( Parameter::ShadowVirtualResolutionLodBias ) ),
             .SmrtRayCount = static_cast<uint32_t>( quality.As<int>( Parameter::ShadowVirtualSMRTRayCount ) ),
             .SmrtSamplesPerRay =
                  static_cast<uint32_t>( quality.As<int>( Parameter::ShadowVirtualSMRTSamplesPerRay ) ) };
        m_MegaLightsQuality = System::MegaLightsQuality{
             .SamplesPerPixel  = quality.As<uint32_t>( Parameter::ShadowMegaLightsSamplesPerPixel ),
             .SpatialSamples   = quality.As<uint32_t>( Parameter::ShadowMegaLightsSpatialSamples ),
             .MaxHistory       = quality.As<uint32_t>( Parameter::ShadowMegaLightsMaxHistory ),
             .DenoiserRadius   = quality.As<uint32_t>( Parameter::ShadowMegaLightsDenoiserRadius ),
             .DownsampleFactor = quality.As<uint32_t>( Parameter::ShadowMegaLightsDownsampleFactor ) };
        m_MegaLightsVolumeQuality = System::MegaLightsVolumeQuality{
             .TilePixels = quality.As<uint32_t>( Parameter::ShadowMegaLightsVolumeTilePixels ),
             .Slices     = quality.As<uint32_t>( Parameter::ShadowMegaLightsVolumeSlices ),
             .Samples    = quality.As<uint32_t>( Parameter::ShadowMegaLightsVolumeSamples ),
             .MaxHistory = quality.As<uint32_t>( Parameter::ShadowMegaLightsMaxHistory ) };
        m_ReflectionDenoiserRadius            = quality.As<uint32_t>( Parameter::ReflectionDenoiserRadius );
        // The view's memory census sizes the SSR trace from the profile, so the profile holds the factor.
        m_ViewProfile.ReflectionDownsampleFactor = quality.As<uint32_t>( Parameter::ReflectionDownsampleFactor );
        m_ScreenSpaceGISamples                = quality.As<int>( Parameter::ScreenSpaceGISamples );
        m_PathTracingSettings                 = System::PathTracer::Settings{
                             .SamplesPerPixel = quality.As<uint32_t>( Parameter::PathTracingSamplesPerPixel ),
                             .PathsPerFrame   = quality.As<uint32_t>( Parameter::PathTracingPathsPerFrame ),
                             .MaxBounces      = quality.As<uint32_t>( Parameter::PathTracingMaxBounces ),
                             .MaxPathIntensity =
                                  quality.As<float>( Parameter::PathTracingMaxPathIntensity ),
                             .Denoiser = quality.As<Common::Scalability::PathTracingDenoiserMethod>(
                                  Parameter::PathTracingDenoiser ) };
        m_SunSourceAngleDegrees = shadows.SourceAngleDegrees;
        m_SunCastsShadows       = shadows.Enabled;
        m_SunContactShadowLength              = shadows.Enabled ? shadows.ContactShadowLength : 0.0f;
        m_SunContactShadowLengthInWorldSpace  = shadows.ContactShadowLengthInWorldSpace;
        m_SunContactShadowCastingIntensity    = shadows.ContactShadowCastingIntensity;
        m_SunContactShadowNonCastingIntensity = shadows.ContactShadowNonCastingIntensity;
        m_LightGridQuality =
             LightGridQuality{ .TilePixels       = quality.As<uint32_t>( Parameter::LightGridPixelSize ),
                               .Slices           = quality.As<uint32_t>( Parameter::LightGridSlices ),
                               .MaxLightsPerCell = quality.As<uint32_t>( Parameter::LightGridMaxLightsPerCell ) };
        m_RadianceCacheInputs =
             RadianceCache::MakeRadianceCacheInputs( quality, m_RenderMethods.GlobalIllumination.Effective );

        // THE SHADOW BUDGET is not taken here: OnUpdate takes it at the head of the frame, before the cascades are
        // fitted (RebudgetShadowsFor), from the m_RenderMethods resolved above.

        // GPU particles: snapshot the scene's emitters (CPU) here; the compute sim is dispatched in OnUpdate
        // before the render graph, and the billboard pass draws in AddFrameTranslucency.
        // A reflection capture's face view neither draws nor simulates them (AddFrameParticlesSimulate).
        if ( !m_ViewProfile.SceneCapture )
        {
            // VFX-11: this view's origin and Effects particle rows go to the world's next scalability decision
            // (VFXWorld::Tick) - the world decides once for all views, not here.
            if ( const auto* cam = GetMainCamera() )
            {
                ::Desert::VFX::VFXViewReport report{
                     .Origin          = cam->GetPosition(),
                     .SpawnCountScale = quality.As<double>( Parameter::EffectsSpawnCountScalePercent ) / 100.0,
                     .EffectsLevel    = quality.As<int>( Parameter::EffectsQualityLevel ),
                     .MaxWorldParticles =
                          static_cast<std::uint32_t>( quality.As<int>( Parameter::EffectsMaxWorldParticles ) ) };
                // The view's frustum (Gribb-Hartmann on clip = P * V, depth 0..1): six inward planes, normalized;
                // a degenerate plane (an infinite far plane) culls nothing.
                const glm::mat4 clip = glm::mat4( cam->GetProjectionMatrix() ) * glm::mat4( cam->GetViewMatrix() );
                const auto      row  = [&clip]( const int r )
                { return glm::vec4( clip[0][r], clip[1][r], clip[2][r], clip[3][r] ); };
                const std::array<glm::vec4, 6> planes = {
                     row( 3 ) + row( 0 ), row( 3 ) - row( 0 ), row( 3 ) + row( 1 ), row( 3 ) - row( 1 ), row( 2 ),
                     row( 3 ) - row( 2 ) };
                for ( std::size_t i = 0; i < planes.size(); ++i )
                {
                    const float length = glm::length( glm::vec3( planes[i] ) );
                    report.FrustumPlanes[i] =
                         length > 1.0e-6f ? planes[i] / length : glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f );
                }
                report.HasFrustum = true;
                scene.GetVFXWorld().SubmitView( report );
            }
            UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )->PrepareFrame( scene );
            // VFX-LIGHT (UE Niagara Light Renderer): the scene's particle lights join this view's local lights -
            // the same light grid, composite and forward paths as a PointLightComponent's, casting no shadow.
            for ( const ShaderProtocols::PointLightPayload& light :
                  System::ParticleWorldGpu::Of( scene.GetVFXWorld() ).ParticleLights() )
                AddPointLight( light, LocalLightShadowSettings{ .CastShadows = false } );
        }

        // The rest of what reads time in a frame reads the SCENE'S clock (Core::WorldTime), handed over
        // here: the material Time uniform and the eye adaptation step.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetWorldTimeSeconds( static_cast<float>( scene.GetWorldTime().GetGameTimeSeconds() ) );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->SetParallaxQuality( m_ParallaxQuality );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] )
             ->SetDeltaSeconds( scene.GetWorldTime().GetDeltaSeconds() );

        // The manual exposure as the scale the composite multiplies by (UE Manual metering, EV100).
        const float manualExposure = ManualExposureScale( post.ManualExposureEV100, post.AutoExposureKey );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )->SetParams( manualExposure );
        // ScenePreExposure's source (UE FViewInfo::UpdatePreExposure): the exposure this view tonemaps with as the
        // CPU knows it — manual: the scale above; auto: key / the adapted luminance read back FramesInFlight
        // frames ago (0 until the first copy is written). A capture view keeps 0 -> 1.0: its output is stored
        // absolute.
        m_LastExposure = 0.0f;
        if ( !m_ViewProfile.SceneCapture )
        {
            if ( !post.AutoExposure )
                m_LastExposure = manualExposure;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            else if ( auto* ae =
                           UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] ) )
            {
                const float lum = ae->ReadBackAdaptedLuminance();
                m_LastExposure  = lum > 0.0f ? post.AutoExposureKey / std::max( lum, 1e-4f ) : 0.0f;
                LOG_TRACE( "[PreExposure] {}: read-back adapted luminance {} -> exposure {}",
                           m_ViewResources.GetName(), lum, m_LastExposure );
                // UE UpdatePreExposure x AverageLocalExposure (PostProcessEyeAdaptation.cpp:1559,1587): the
                // tonemap scales the scene term by local exposure, so the stored range follows it.
                m_LastAverageLocalExposure = ae->GetLastAverageLocalExposure();
            }
        }
        if ( !post.AutoExposure || m_ViewProfile.SceneCapture )
            m_LastAverageLocalExposure = 1.0f;
        {
            const LocalExposureParams localExposure = ResolveLocalExposure( post );
            const float               logMiddleGrey = LocalExposureLogMiddleGrey(
                 post.AutoExposureKey, localExposure.MiddleGreyBiasEV, !post.AutoExposure );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            if ( auto* le = UNIQUE_GET_AS( System::LocalExposureRenderer, m_RenderSystems["LocalExposureSystem"] ) )
                le->SetParams( localExposure, logMiddleGrey, m_LastExposure > 0.0f ? m_LastExposure : manualExposure );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            if ( auto* ae = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] ) )
                ae->SetLocalExposure( localExposure, logMiddleGrey, post.AutoExposureKey );
        }
        // The physical sky's on-screen ceiling follows the LOWEST exposure this view can use — the manual scale,
        // or the meter clamped at Max EV100 — so a sky the exposure can show is never cut (SkyLuminanceCeiling).
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->SetSkyLuminanceCeiling( SkyLuminanceCeiling(
                  post.AutoExposure ? LowestAutoExposureScale( post.AutoExposureMaxEV100, post.AutoExposureKey )
                                    : manualExposure ) );
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
             ->SetSkinCacheEnabled( quality.As<bool>( Parameter::SkinCache ) );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetShadows( shadows, static_cast<int>( m_DebugView.ShadowDebug ) );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetDebugView( m_DebugView.ShowNormals, m_DebugView.ShowBoundingBoxes, m_DebugView.BoundingBoxColor,
                             m_DebugView.BoundingBoxLineWidth, m_DebugView.LightingDebug );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SetCardPlacementView( m_DebugView.ShowCardPlacement );

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
        // AUTO-EXPOSURE does the same division on the GPU: the exposure there is `key / adaptedLuminance` from a
        // 1x1 image the CPU never reads, so the bloom pass binds that image and divides the threshold itself
        // (BloomRenderer::SetAutoExposure, BloomDownsample.shader) — here it is left in exposed units.
        const float exposureNormalisation = post.AutoExposure ? 1.0f : std::max( manualExposure, 1e-30f );

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

        // Motion blur (MR2): the authored grade and the scalability tap count (PostProcess.MotionBlurQuality).
        m_MotionBlurSettings = MotionBlurSettings{ .Amount     = post.MotionBlurAmount,
                                                   .MaxPercent = post.MotionBlurMax,
                                                   .TargetFPS  = post.MotionBlurTargetFPS,
                                                   .Samples    = MotionBlurSamplesForQuality( m_Quality.As<int>(
                                                        Common::Scalability::Parameter::MotionBlurQuality ) ) };

        // Depth of field (MR3): the authored lens (UE DepthOfFieldFocalDistance / Fstop / SensorWidth; the focal
        // length follows from the frame's FOV) and the scalability ring count (PostProcess.DepthOfFieldQuality).
        m_DepthOfFieldSettings = DepthOfFieldSettings{
             .FocalDistanceCm = post.DepthOfFieldFocalDistance,
             .FStop           = post.DepthOfFieldFstop,
             .SensorWidthMm   = post.DepthOfFieldSensorWidth,
             .MaxBokehPercent = post.DepthOfFieldMaxBokehSize,
             .Rings =
                  DofRingsForQuality( m_Quality.As<int>( Common::Scalability::Parameter::DepthOfFieldQuality ) ) };

        UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] )
             ->SetParams( post.AutoExposureSpeed, EV100ToLuminance( post.AutoExposureMinEV100 ),
                          EV100ToLuminance( post.AutoExposureMaxEV100 ) );
        UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
             ->SetAutoExposure( post.AutoExposure, post.AutoExposureKey );
        UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] )
             ->SetAutoExposure( post.AutoExposure, post.AutoExposureKey );

        return BOOLSUCCESS;
    }

    // THE SHADOW BUDGET FOLLOWS THE SHADOWS LEVEL AND THE VIEW'S RESOLVED SUN PATH (UE re-creates its shadow depth
    // targets on r.Shadow.*, never per frame). Asked when the quality generation or the resolved sun path the
    // budget was taken for moved - a level change or a Shadows method that starts/stops being a screen mask - and
    // the maps are re-allocated only when the budget really differs (ShadowReallocation, ViewMemory.hpp). The
    // profile is updated with them so the view's memory census keeps describing what is allocated. The sun path is
    // part of the budget (ShadowQualityOf): a screen-mask sun keeps only the low-detail cascades. A frame whose
    // TLAS is not built draws those low-detail cascades for that frame (LowerShadowsForFrame) rather than
    // re-allocating the maps twice around it. Called at the head of OnUpdate only, before any shadow view of the
    // frame is planned.
    void SceneRenderer::RebudgetShadowsFor( const Common::Scalability::ResolvedQuality& quality,
                                            const bool                                  sunRayTraced )
    {
        if ( quality.Generation == m_ShadowBudgetGeneration && sunRayTraced == m_ShadowBudgetSunRayTraced )
            return;
        m_ShadowBudgetGeneration   = quality.Generation;
        m_ShadowBudgetSunRayTraced = sunRayTraced;
        const auto budget          = ShadowReallocation( m_ViewProfile, quality, sunRayTraced );
        if ( !budget )
            return;
        m_ViewProfile.Shadows = *budget;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( !UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->RebudgetShadows( *budget ) )
            LOG_ERROR( "[Shadows] view '{}': the shadow pass could not be set up at {} cascades of {} px; nothing "
                       "casts a shadow in this view.",
                       m_ViewResources.GetName(), budget->CascadeCount, budget->ShadowMapSize );
    }

    void SceneRenderer::OnUpdate( const UpdateInfo& sceneRenderInfo )
    {
        DESERT_PROFILE_SCOPE( "SceneRenderer::OnUpdate" );

        // RE-OPENED HERE, and it is not belt-and-braces. Scene drives the views PHASE BY PHASE — every
        // view's BeginScene, then every view's OnUpdate — and each phase's scope is gone when it returns.
        // Without this line a second view writes its per-frame GPU state into the frame context instead of
        // its own copies, a torn picture with nothing in the log (Docs/RENDERER_FRAME_STATE.md).
        const ActiveViewScope viewScope( m_ViewResources );
        const ResourceAttributionScope ledger = OpenViewLedgerScope();

        const auto& skyboxSystem = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
        m_DirectionLights        = sceneRenderInfo.DirLights;
        m_AtmosphereLight1       = sceneRenderInfo.AtmosphereLight1;

        // THE SUN THE ATMOSPHERE LETS THROUGH (UE's PrepareSunLightProxy). The light's authored colour
        // is its OUTER-SPACE illuminance in the physical model; what reaches the ground has crossed the
        // whole atmosphere, so it is multiplied by that path's transmittance — and a sunset reddens and
        // dims every lit surface by the same law that reddens the sky behind it, for free.
        //
        // The factor is decided in one place for both models (Graphic::SunLightFactorAtGround): zero for
        // a sun below the horizon in either model, and above it the atmosphere's transmittance in either
        // model while this sun opts in — so there is no branch here and no second behaviour to test.
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
        // Atmosphere light 1 (the moon) by the same law, through its own direction's factor.
        if ( m_AtmosphereLight1 )
        {
            const glm::vec3 transmittance  = skyboxSystem->GetAtmosphere().Light1TransmittanceAtGround;
            glm::vec4&      colorIntensity = m_AtmosphereLight1->Light.ColorIntensity;
            colorIntensity.x *= transmittance.x;
            colorIntensity.y *= transmittance.y;
            colorIntensity.z *= transmittance.z;
        }

        // Decide whether the procedural sky's light is (re)captured this frame and allocate the capture, before
        // the render graph is built: its passes are recorded by AddFrameSkyLight at the head of the graph.
        {
            DESERT_PROFILE_PASS( "Sky: EnsureProceduralEnv" );
            // The sky block first, inside this view's scope: the capture and the graph below read it.
            skyboxSystem->UploadFrameSkyParams();
            skyboxSystem->EnsureProceduralEnvironment( sceneRenderInfo.RealTimestep.GetSeconds() );
        }

        // THE SHADOW BUDGET, AT THE FRAME BOUNDARY: taken before ANY shadow view of this frame is planned - the
        // cascades below, the rain occlusion's view, the local lights' views, the VSM pages - from the Shadows
        // method the view RESOLVED (BeginScene), not the per-frame TLAS fallback (LowerShadowsForFrame, later in
        // this function). A re-budget drains the device and re-makes the maps and the local rows
        // (MeshRenderer::RebudgetShadows); taken in the middle of the frame, it destroyed what that frame had
        // already planned and the frame recorded it (SHADOW-CINE-ABORT: Shadows Cinematic switched on live, the
        // rain occlusion's view drew through a null caster material and the editor aborted).
        RebudgetShadowsFor( m_Quality, SunShadowIsScreenMask() );

        // Recompute CSM cascade matrices once per frame BEFORE the render graph records (a cascade pass computes
        // nothing: its Declare and record only read the matrices).
        {
            DESERT_PROFILE_PASS( "Shadow: UpdateCascades" );
            auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            // The particles' Mesh rows submit their casters from the render graph, after this plan (VFX-09).
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            const auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
            meshes->ExpectGpuInstancedCasters( !m_ViewProfile.SceneCapture && particles != nullptr &&
                                               particles->HasMeshRows() );
            meshes->UpdateCascades();
        }
        // The rain occlusion's box, and whether its map is redrawn this frame (the same boundary: its pass only
        // reads the matrix).
        {
            DESERT_PROFILE_PASS( "Weather: UpdateRainOcclusion" );
            UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->UpdateRainOcclusion();
        }

        // Morph targets blend before anything skins (UE: the morph update precedes the GPU skin passes): its own
        // small graph, ending with the morphed vertices in vertex-input state for every pass of the view graph.
        {
            DESERT_PROFILE_PASS( "Mesh: UpdateMorphTargets" );
            UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->UpdateMorphTargets();
        }

        // THE FRAME IS A GRAPH. Every GPU pass of this view from here on is one graph node (Raster, Compute or
        // Copy) that declares what it reads and writes; the graph orders them by those declarations and places
        // every barrier and layout transition. A pass this frame does not run is not added. The lambdas run inside
        // Execute, after the whole graph is built, so a value one pass hands a later one travels through `values`,
        // which outlives Execute; everything else they need is captured by value.
        // Every CPU stage of the graph's build and execution is a row of its own, so the body has no time that
        // no child scope accounts for.
        DESERT_PROFILE_PHASES( phases );
        DESERT_PROFILE_PHASE( phases, "SceneRenderer: Frame setup" );
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
        // THE ONE PER-VIEW RESOLUTION (ResolveViewResolution): the resolved RenderScalePercent and AA method,
        // the temporal method they select, clamped to a split the method's upscaler supports. Its refusal refuses
        // the frame by name; a clamp is said. The view's temporal upscaler is made for the resolved method here.
        using Common::Scalability::Parameter;
        // A path-traced view traces every output pixel (UE: the path tracer renders at 100 %, no upscaler): its
        // accumulation is the image, and an upscaler's history would only blur the converging estimate.
        int                settingPercent  = m_Quality.As<int>( Parameter::RenderScalePercent );
        std::optional<int> overridePercent = m_DebugView.ScreenPercentage;
        // The wireframe view renders without anti-aliasing (BeginScene): no temporal method to reconstruct with. A
        // path-traced view under PathTracing.Denoiser RR keeps Ray Reconstruction and the render scale, at most
        // 100 %: the network takes no supersampled input (SupersampleRefusal), and a supersampled path-traced
        // frame would only trace pixels the network discards (System::PathTracer::KeepsDenoisingUpscaler).
        const bool pathTraced = PathTraced( m_RenderMethods );
        if ( ( pathTraced && !System::PathTracer::KeepsDenoisingUpscaler( m_Quality ) ) || m_DebugView.WireframeMode )
        {
            settingPercent  = 100;
            overridePercent = std::nullopt;
        }
        else if ( pathTraced )
        {
            settingPercent = std::min( settingPercent, 100 );
            if ( overridePercent )
                overridePercent = std::min( *overridePercent, 100 );
        }
        // A vendor method its runtime stopped offering (DLSS after an exception inside its evaluation) renders
        // with the engine's method instead, said once per reason; the frame never waits on a method that cannot
        // write its output.
        // Ray Reconstruction is refused also by the VIEW: it denoises traced lighting, so a view that traces none
        // (or renders the forward path, which has no G-buffer for its guides) runs DLSS Super Resolution instead.
        const VendorMethodFallback vendor = ReplaceUnavailableVendorMethods(
             m_RenderedAntiAliasing,
             [this]( const TemporalMethod method ) -> std::string
             {
                 if ( method == TemporalMethod::DLSSRR )
                     if ( std::string why = RayReconstructionRefusal(
                               m_RenderMethods.Reflections.Effective, m_RenderMethods.GlobalIllumination.Effective,
                               m_RenderMethods.Shadows.Effective, m_GBuffer != nullptr );
                          !why.empty() )
                         return why;
                 return TemporalMethodUnavailable( method );
             } );
        if ( vendor.Why != m_VendorFallbackSaid )
        {
            if ( !vendor.Why.empty() )
                LOG_ERROR( "[SceneRenderer] {}: {}; the view renders with the engine's temporal method instead",
                           m_ViewResources.GetName(), vendor.Why );
            m_VendorFallbackSaid = vendor.Why;
        }
        m_RenderedAntiAliasing                           = vendor.AntiAliasing;
        const Common::ResultStr<ViewResolution> resolved =
             ResolveViewResolution( m_ViewExtent, settingPercent, overridePercent, m_RenderedAntiAliasing,
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
        inputs.Scalability                       = &m_Quality;
        inputs.TimeSeconds                       = m_SceneTimeSeconds;
        inputs.Parallax                          = m_ParallaxQuality;
        // Index 0 is the scene's one directional light (see the atmosphere sun above); its Direction is the way the
        // light travels, the relief's self-shadow traces the other way.
        inputs.SunDirection = m_DirectionLights.DirectionLights.empty()
                                   ? glm::vec3( 0.0f )
                                   : glm::normalize( -glm::vec3( m_DirectionLights.DirectionLights[0].Direction ) );
        m_LastRenderScalePercent                 = inputs.RenderScalePercent;
        inputs.LastExposure                      = m_LastExposure * m_LastAverageLocalExposure;
        const Common::ResultStr<ViewFrame> begun = m_ViewState.BeginFrame( inputs, m_TemporalUpscaler.get() );
        if ( !begun )
        {
            LOG_ERROR( "[SceneRenderer] {}: the view refused this frame: {}", m_ViewResources.GetName(),
                       begun.GetError() );
            return;
        }
        m_CameraCutPending     = false;
        const ViewFrame& frame = begun.GetValue();
        // The scale this frame's scene colour is stored at: the readers that undo it (ViewFrame::PreExposure).
        // A view without the post chain (a capture face) has neither system.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* tonemap = UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] ) )
            tonemap->SetPreExposure( frame.PreExposure );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* autoExp = UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] ) )
            autoExp->SetPreExposure( frame.PreExposure );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* le = UNIQUE_GET_AS( System::LocalExposureRenderer, m_RenderSystems["LocalExposureSystem"] ) )
            le->SetPreExposure( frame.PreExposure );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* bloom = UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] ) )
            bloom->SetPreExposure( frame.PreExposure );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* shafts = UNIQUE_GET_AS( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] ) )
            shafts->SetPreExposure( frame.PreExposure );
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

        // The water's tiles, once, before its node records: the quadtree walked from the main camera. After the
        // view frame is current: PrepareFrame uploads this frame's view-projection pair from it.
        {
            DESERT_PROFILE_PASS( "Water: PrepareFrame" );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            UNIQUE_GET_AS( System::WaterRenderer, m_RenderSystems["WaterSystem"] )->PrepareFrame();
        }

        // The terrain's frame data, once, before any of its three passes records (the cascades in
        // DepthPrePass, the forward pass in Geometry, the G-buffer fill after the graph). INSIDE the view's
        // frame and handed it: the terrain's velocity rows read this frame's ViewProjection and the previous
        // one, which exist only once BeginFrame accepted the frame (before it, GetViewFrame() is nullptr — the
        // terrain read it there and a scene with landscape tiles faulted on a null ViewFrame).
        {
            DESERT_PROFILE_PASS( "Terrain: PrepareFrame" );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact
            // type
            // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
            // With the scene's lit-material state (the forward terrain receives the point and spot lights the
            // meshes do). The shadowed point/spot lights' draws follow UpdateLocalShadows below
            // (PrepareLocalShadowViews): their views do not exist yet here.
            const auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )
                 ->PrepareFrame( frame, meshes->CaptureFrameState( &frame ) );
            // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
        }
        if ( const Common::BoolResultStr physical =
                  m_ViewState.History().AllocatePhysical( DeviceImageFactory{}, RendererGraphImageImporter{} );
             !physical )
        {
            LOG_ERROR( "[SceneRenderer] {}: the view's temporal history has no images: {}",
                       m_ViewResources.GetName(), physical.GetError() );
            return;
        }
        FrameTextures textures( graph );
        AddFrameSkyLight( graph, textures );
        ImportSceneViewTextures( graph, textures );
        // The reflection captures, before any lit node reads them (SceneViewInputs).
        if ( m_FrameReflectionCaptures )
            m_FrameReflectionCaptures->AddPasses( graph, textures, ReadsReflectionCaptures() );
        // The view's velocity: one transient of this graph at the RENDER EXTENT (frame.Split.Render: the scene
        // target and the G-buffer are resized to it by ResizeRenderTargets), a colour slot of the scene target
        // (SceneTargetLayout, slot kSceneTargetVelocitySlot) and of the G-buffer (GBufferLayout, slot
        // kGBufferVelocitySlot) — never of a light view (cascades) or a debug target. Created before any node
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
            // After the velocity on the scene target (kSceneTargetResponsiveAASlot); the G-buffer has no such slot.
            const ViewVelocity responsive = CreateViewResponsiveAA(
                 graph, RDG::Extent3D{ frame.Split.Render.Width, frame.Split.Render.Height, 1 }, target.Samples );
            textures.Transients.ResponsiveAA = responsive.Resolved;
            textures.AddGraphColor( m_TargetFramebuffer, ResponsiveAAColor( responsive, target.Samples ) );
            // After them on both targets (kSceneTargetThinGeometryCoverageSlot / kGBufferThinGeometryCoverageSlot).
            const ViewVelocity coverage = CreateViewThinGeometryCoverage(
                 graph, RDG::Extent3D{ frame.Split.Render.Width, frame.Split.Render.Height, 1 }, target.Samples );
            textures.Transients.ThinGeometryCoverage = coverage.Resolved;
            textures.AddGraphColor( m_TargetFramebuffer, ThinGeometryCoverageColor( coverage, target.Samples ) );
            textures.AddGraphColor( m_GBuffer, ThinGeometryCoverageColor( coverage, 1 ) );
        }
        const auto values = std::make_shared<FrameValues>();

        // The view's per-primitive motion rows (current + previous world, both bone palettes) from the view's
        // MotionHistory, built once before any pass is declared: every view pass of this frame reads the same
        // rows.
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* const meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            const auto* const terrain = UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] );
            // Every landscape tile is a GPU-scene primitive too (its TLAS custom index): world-space vertices, so
            // an identity World.
            m_ScratchTileMotions.clear();
            for ( const System::TerrainDrawData& tile : terrain->GetQueue() )
                m_ScratchTileMotions.push_back(
                     { .Entity = tile.Landscape.Entity, .Part = 0, .World = glm::mat4( 1.0f ) } );
            // The scene's GPU skin cache (RayTracingSkinCache.hpp): every view of the scene shares it, each
            // skinned primitive is skinned once per frame by the skin step of the first view that queues it, the
            // raster and the TLAS of every view read the result.
            const uint64_t frameNumber = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
            RayTracing::RayTracingSkinCache* skinCache = nullptr;
            if ( m_FrameRayTracingCache != nullptr )
            {
                m_FrameRayTracingCache->BeginViewFrame( frameNumber );
                skinCache = &m_FrameRayTracingCache->GetSkinCache();
                skinCache->SetEnabled( meshes->IsSkinCacheEnabled() );
                skinCache->SetDisplacement( &m_FrameRayTracingCache->GetDisplacement() );
                // Mesh displacement (DisplacementCache.hpp): this view's level requests, the frame's one
                // displacement (first view), and the twins every pass of this view draws.
                System::DisplacementCache& displacement = m_FrameRayTracingCache->GetDisplacement();
                displacement.SetQuality( System::DisplacementQuality{
                     .Enabled          = m_DisplacementDicingRate > 0,
                     .DicingRatePixels = static_cast<float>( std::max( m_DisplacementDicingRate, 1 ) ),
                     .PoolBytes        = static_cast<uint64_t>( m_DisplacementPoolMiB ) << 20u } );
                if ( const auto* viewCamera = GetMainCamera(); viewCamera != nullptr )
                    meshes->ApplyDisplacement(
                         displacement,
                         Geometry::LODView{ viewCamera->GetPosition(), viewCamera->GetProjectionMatrix() },
                         static_cast<float>( m_ViewExtent.Height ), frameNumber, !m_ViewProfile.SceneCapture );
            }
            if ( const Common::BoolResultStr rows = meshes->BuildObjectMotions(
                      m_ViewState.Motion(), m_ScratchTileMotions, skinCache, frameNumber );
                 !rows )
            {
                LOG_ERROR( "[SceneRenderer] {}: the view's motion rows could not be built; nothing is drawn this "
                           "frame: {}",
                           m_ViewResources.GetName(), rows.GetError() );
                return;
            }
            // The skin step already ran in BuildObjectMotions (its own graph, submitted before this view graph):
            // this graph only reads the streams, barriered by their carried state.
            meshes->SkinCacheReads().clear();
            if ( skinCache != nullptr )
                skinCache->DeclareReads( graph, *meshes, frameNumber, meshes->SkinCacheReads() );
        }

        const auto sceneColor = [this, &textures]()
        {
            return textures.Refs(
                 { { m_TargetFramebuffer ? m_TargetFramebuffer->GetColorAttachmentImage( 0 ) : nullptr,
                     "SceneColor" } } );
        };

        DESERT_PROFILE_PHASE( phases, "SceneRenderer: Grass, TLAS, shadow plan" );
        AddFrameClearMainFramebuffer( graph, textures );

        // The landscape grass of this view, before every node that draws it (the cascades, the geometry) and
        // before the view's TLAS, whose GPU-written grass rows read its transforms and drawn counts
        // (RayTracingGpuInstanceSource). A face view grows none, as it simulates no particles.
        if ( !m_ViewProfile.SceneCapture )
        {
            // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the keys name these exact types
            UNIQUE_GET_AS( System::GrassRenderer, m_RenderSystems["GrassSystem"] )
                 ->AddFrame( graph, frame, *UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ),
                             RayTracing::RayTracingSceneRequired( m_RenderMethods ) );
            // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
        }

        // The view's TLAS, before any pass that traces rays is declared (RayTracingSceneRenderer.hpp). A frame
        // whose TLAS could not be declared still draws; its ray readers see GetFrame().Built == false.
        {
            // Every RT allocation this block makes is a ledger row of the scene renderer (RT-SCENE-2 contract,
            // Memory accounting). The VIEW's rows — its TLAS storage, its landscape tiles' vertex / index buffers
            // and BLASes — carry this view's instance (RT-LEDGER-2) and are counted in HeldBytes. The scene
            // cache's variant, procedural and skin-cache BLASes and skinned positions carry the SCENE's instance
            // (RayTracingSceneRenderer::AddPasses opens the cache's scope around them): shared, in no view's
            // budget. A mesh BLAS built here by a lazy upload is claimed by the mesh service for its asset (an
            // explicit claim wins and clears the instance). The graph transients (TLAS instance rows, build
            // scratch, PrimitiveInstances) live in the device's transient heaps, each a TransientHeap row of the
            // Device. The view's scope is OnUpdate's (RT-LEDGER-3), not a second one here.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* const     meshes     = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            const auto*     camera     = GetMainCamera();
            const glm::vec3 viewOrigin = camera != nullptr ? camera->GetPosition() : glm::vec3( 0.0f );
            const float     lensScale  = camera != nullptr
                                              ? DrawDistanceScale( Geometry::LODView{ camera->GetPosition(),
                                                                                 camera->GetProjectionMatrix() } )
                                              : 1.0f;
            const bool      required   = RayTracing::RayTracingSceneRequired( m_RenderMethods );

            // The landscape tiles first: their vertex compute, then their BLAS requests join the RT scene's
            // dynamic-geometry pass, which the TLAS pass reads (orders it after the builds).
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            const auto* const terrain = UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] );
            std::span<const System::TerrainDrawData>       queue     = terrain->GetQueue();
            const std::span<const System::TerrainInstance> instances = terrain->GetFrameInstances();
            const std::span<const System::TerrainFrameMaterial> materials = terrain->GetFrameMaterials();
            const std::span<const uint32_t>                rows      = meshes->GetOtherMotionRows();
            // Parallel by construction (PrepareFrame packs one row per queued tile, BuildObjectMotions one motion
            // row per tile record); a mismatch is a broken invariant, said and not traced, never truncated.
            std::vector<RayTracing::LandscapeRayTracingTile> tiles;
            if ( instances.size() != queue.size() || materials.size() != queue.size() ||
                 rows.size() != queue.size() )
            {
                LOG_ERROR(
                     "[SceneRenderer] {}: {} landscape tiles queued but {} terrain rows, {} materials and {} "
                     "GPU-scene rows; the landscape is not in this frame's TLAS",
                     m_ViewResources.GetName(), queue.size(), instances.size(), materials.size(), rows.size() );
                queue = {};
            }
            tiles.reserve( queue.size() );
            for ( std::size_t i = 0; i < queue.size(); ++i )
            {
                const System::TerrainDrawData& draw = queue[i];
                tiles.push_back( { .Entity         = draw.Landscape.Entity,
                                   .GpuSceneRow    = rows[i],
                                   .Heightmap      = draw.Heightmap,
                                   .HeightRevision = draw.Landscape.HeightRevision,
                                   .Weightmap      = draw.Weights.Weightmap,
                                   .WeightRevision = draw.Weights.Revision,
                                   .Instance       = instances[i],
                                   .Weights        = draw.Weights,
                                   .Material       = materials[i] } );
            }
            const Common::BoolResultStr landscape =
                 m_LandscapeRayTracing.AddPasses( graph, textures, required, tiles );
            if ( !landscape )
            {
                LOG_ERROR( "[SceneRenderer] {}: the view's landscape could not enter the TLAS; rays miss it this "
                           "frame: {}",
                           m_ViewResources.GetName(), landscape.GetError() );
            }
            // A failed AddPasses may have registered tiles whose build was never declared: none enters the TLAS.
            const RayTracing::LandscapeRayTracingFrame& landscapeFrame = m_LandscapeRayTracing.GetFrame();
            // The blade grass's level BLASes (GrassBladeRayTracing.hpp) join the same dynamic-geometry pass: their
            // primitives are the grass sections the gather reads from the mesh renderer. A face view grew no grass
            // this frame (above), so it hands none. The landscape stats stay the landscape's own.
            std::vector<RayTracing::RayTracingDynamicGeometryBuild> builds;
            std::vector<RDG::BufferRef>                             resident;
            if ( landscape.IsSuccess() )
            {
                builds.assign( landscapeFrame.Builds.begin(), landscapeFrame.Builds.end() );
                resident.assign( landscapeFrame.Resident.begin(), landscapeFrame.Resident.end() );
            }
            if ( !m_ViewProfile.SceneCapture )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
                const auto* const grass = UNIQUE_GET_AS( System::GrassRenderer, m_RenderSystems["GrassSystem"] );
                const std::span<const RayTracing::RayTracingDynamicGeometryBuild> bladeBuilds =
                     grass->GetBladeRayTracing().GetBuilds();
                const std::span<const RDG::BufferRef> bladeResident = grass->GetBladeRayTracing().GetResident();
                builds.insert( builds.end(), bladeBuilds.begin(), bladeBuilds.end() );
                resident.insert( resident.end(), bladeResident.begin(), bladeResident.end() );
            }
            RayTracing::RayTracingExternalPrimitives external{ .Builds = builds, .Resident = resident };
            if ( landscape.IsSuccess() )
                external = { .Primitives       = landscapeFrame.Primitives,
                             .Builds           = builds,
                             .Resident         = resident,
                             .Built            = landscapeFrame.Built,
                             .Refit            = landscapeFrame.Refit,
                             .VertexBytes      = landscapeFrame.VertexBytes,
                             .BlasBytes        = landscapeFrame.BlasBytes,
                             .SegmentRecords   = landscapeFrame.SegmentRecords,
                             .LandscapeOpacity = landscapeFrame.Opacity };

            Common::BoolResultStr tlas = BOOLSUCCESS;
            if ( m_FrameRayTracingCache == nullptr )
                tlas = Common::MakeError(
                     std::string( "OnUpdate ran outside BeginScene/EndScene: no scene ray-tracing cache" ) );
            else
                tlas = m_RayTracingScene.AddPasses( graph, required, *meshes, *m_FrameRayTracingCache, viewOrigin,
                                                    lensScale, external );
            for ( const std::string& error : m_RayTracingScene.GetGatherErrors() )
            {
                LOG_ERROR( "[SceneRenderer] {}: a mesh row traces nothing this frame: {}",
                           m_ViewResources.GetName(), error );
            }
            if ( !tlas )
                LOG_ERROR( "[SceneRenderer] {}: the view's TLAS could not be declared; ray-traced methods read "
                           "nothing this frame: {}",
                           m_ViewResources.GetName(), tlas.GetError() );
#if DESERT_DEV_INSTRUMENTS
            // The RT scene's view-memory lines, beside the targets' census, when its TLAS storage is
            // (re)allocated: the moment the view's RT memory steps, not every frame.
            const RayTracing::RayTracingSceneStats& rtStats = m_RayTracingScene.GetStats();
            if ( rtStats.Built && rtStats.TlasBytes != m_LoggedRayTracingTlasBytes )
            {
                m_LoggedRayTracingTlasBytes = rtStats.TlasBytes;
                LOG_INFO( "[ViewMemory] view '{}' {}", m_ViewResources.GetName(),
                          RayTracing::FormatRayTracingSceneMemory( rtStats ) );
                // The view's and the scene's budget entries, from the same rows the budget and the overlay read.
                LOG_INFO( "[ViewMemory] view '{}' ledger: {}", m_ViewResources.GetName(),
                          FormatViewLedgerMemory( TakeLedgerMemory() ) );
            }
#endif

            // THE FRAME'S SUN-SHADOW PATH, decided here ONCE, before any pass that reads it is declared: a
            // ray-traced sun whose frame has no traceable scene (TLAS not built - m_RayTracingScene's frame is
            // only this frame's when AddPasses succeeded - or the trace resources could not be created) resolves
            // to CascadedMaps for the whole frame (LowerShadowsForFrame: the overlay row and one log line name
            // it). Every reader - the cascades below, the depth/normal prepass, the trace, the composite,
            // "Deferred: Generic" - asks SunShadowIsScreenMask() (the ray-traced sun or the virtual shadow map),
            // which reads this resolution. Stochastic traces the sun AND the local lights: both its resources, or
            // neither method this frame.
            const bool sunTraced = SunShadowIsRayTraced();
            LowerShadowsForFrame(
                 m_RenderMethods,
                 !sunTraced || ( tlas && m_RayTracingScene.GetFrame().Built && EnsureRayTracedShadowResources() &&
                                 ( !LocalLightsAreStochastic() || EnsureMegaLightsResources() ) ) );
            // THE FRAME'S PATH TRACER, decided here the same way: a path-traced view whose frame has no traceable
            // scene (TLAS not built) or whose path tracer could not be brought up resolves to HardwareTraced GI
            // and reflections for the whole frame (LowerPathTracingForFrame: the overlay row and one log line name
            // it). Every reader below asks PathTraced( m_RenderMethods ).
            LowerPathTracingForFrame( m_RenderMethods, !PathTraced( m_RenderMethods ) ||
                                                            ( tlas && m_RayTracingScene.GetFrame().Built &&
                                                              EnsurePathTracerResources() ) );
            ReportRenderMethods( m_RenderMethods );
            // The point and spot lights' shadow views, once the frame's Shadows method is final (and before the
            // maps are imported): tiles of the atlas allocated (and the atlas itself, on the first light that
            // casts), each light's ShadowView written into its payload, and the views whose cached depth is stale
            // marked for this frame's graph (MeshRenderer::LocalShadowPasses). Under Stochastic MegaLights traces
            // the local lights: no map is drawn for any (UE: MegaLights lights render no shadow depths). A
            // path-traced frame draws none either: its paths trace every light's visibility through the TLAS.
            // Under HardwareTraced the deferred composite reads "Shadows: Ray Traced Local" (ShadowMaskSlot), and
            // the maps are drawn only while a queued forward draw in view samples them
            // (LocalShadowMapsRead: meshes, forward terrain, particles) - no forward surface loses its shadows.
            {
                DESERT_PROFILE_PASS( "Shadow: UpdateLocalShadows" );
                System::MeshRenderer* const localShadowMeshes =
                     UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
                localShadowMeshes->UpdateLocalShadows(
                     m_PointLight.PointLights, m_PointLightShadows, m_SpotLight.SpotLights, m_SpotLightShadows,
                     !LocalLightsAreStochastic() && !PathTraced( m_RenderMethods ) &&
                          ( !LocalShadowsAreRayTraced() || LocalShadowMapsRead() ) );
                // The terrain's draws into those views, at the LOD each light asks for (not the camera's, so a
                // cached local shadow survives a camera move), and its cache key, before LocalShadowPasses asks
                // for it (PlanLocalShadowCache). Before this ran, GetLocalShadowCasterLights was empty and every
                // light's key failed - TERRAIN-LOCAL-SHADOW-LOG.
                // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
                UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )
                     ->PrepareLocalShadowViews( localShadowMeshes->GetLocalShadowCasterLights() );
                // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
                AssignLocalShadowMaskSlots( Core::Frustum( frame.Projection, frame.View ) );
                // The per-light denoisers' histories: a frame off HardwareTraced parks every one (a method that
                // comes back starts new histories), and the retire queue is drained EVERY frame - a parked history
                // is released once every frame in flight that read it completed, whatever the method is now.
                const auto&    retireFrames      = Engine::FrameManager::GetInstance();
                const uint64_t retireFrame = retireFrames.GetAbsoluteFrameCount();
                if ( !LocalShadowsAreRayTraced() )
                {
                    for ( auto& [sourceId, denoiser] : m_LocalShadowDenoisers )
                        if ( denoiser )
                            m_RetiredLocalShadowDenoisers.Park( std::move( denoiser ), retireFrame );
                    m_LocalShadowDenoisers.clear();
                }
                m_RetiredLocalShadowDenoisers.Collect( retireFrame, retireFrames.GetMaxFramesInFlight() );
            }
            // The maps are imported at the budget taken above, before any pass reads them.
            ImportViewShadowMaps( graph, textures );
            // The sun's cascades this frame: always under CascadedMaps; under a ray-traced sun only when a program
            // of this frame samples them (SunCascadesRead: blended/glass mesh draws, forward terrain, particles,
            // whose program
            // declares ShadowCascadesTexture - the low-detail sun CSM, budgeted by the Shadows.RayTracedTranslucency*
            // rows). Not drawn -> the view binds the far-plane stand-in, never last drawn frame's maps; drawn
            // after a gap -> every cascade's static cache is redrawn (it went stale while nothing drew it). A
            // path-traced frame reads no cascade: its paths trace the sun's visibility through the TLAS, and the
            // lit passes that sample the maps do not run.
            // EVERY OTHER DIRECTIONAL LIGHT keeps its cascades under the sun's mask (the moon, slot
            // kShadowLightAtmosphere1: DeferredScreenShadowMask::Lights, the surface cache's cascade arm), so the
            // array is drawn while any slot casts. The sun's own slot is dropped when nothing reads it
            // (MeshRenderer::DropDirectionalShadow): its layers are not drawn for the moon's sake, and the receivers
            // that pack DirectionalShadows() below (the composite, the surface cache) see it with no cascade.
            const bool sunCascadesUnread = SunShadowIsScreenMask() && !SunCascadesRead();
            if ( meshes != nullptr && sunCascadesUnread )
                meshes->DropDirectionalShadow( SceneResources::kDirectionalShadowSun );
            const auto anySlotCasts = [meshes]
            {
                for ( const SceneResources::DirectionalShadowView& light : meshes->DirectionalShadows() )
                    if ( light.CastShadows && light.Count > 0 )
                        return true;
                return false;
            };
            m_FrameCascades = meshes != nullptr && !PathTraced( m_RenderMethods ) &&
                              ( !sunCascadesUnread || anySlotCasts() );
            if ( !m_FrameCascades )
                textures.Transients.ShadowCascades = AddFrameNoSunCascades( graph );
            else if ( !m_CascadesDrawnLastFrame )
                meshes->InvalidateCascadeCaches();
            m_CascadesDrawnLastFrame = m_FrameCascades;
        }

        // A face view draws no particles (UE: a capture's ShowFlags.Particles = 0, "usually dynamic, and can't be
        // captured well"), and simulates none: the face views render first in a frame, and the first view to
        // prepare the tick is the one that simulates it.
        DESERT_PROFILE_PHASE( phases, "SceneRenderer: Add passes" );
        if ( !m_ViewProfile.SceneCapture )
            AddFrameParticlesSimulate( graph );

        AddFrameCloudShadowMap( graph, textures );

        // The opaque raster of the systems, in this order: the cascade depths (the geometry's shadow lookups read
        // them), the scene target's sky and geometry, the outline mask.
        // This graph's shared traced records (m_TracedFrameShared) are made by their first reader - the surface
        // cache comes before the traced GI - so they are forgotten HERE, before any reader: a handle from the last
        // frame's graph is not a handle in this one.
        m_TracedFrameShared = {};
        AddFrameShadowDepths( graph, textures );
        // The view's light grid, read by every lit node after it (the forward meshes of the base pass, the
        // deferred composite, the glass).
        const LightGridParamsPayload lightGrid = AddFrameLightGrid( graph, textures, frame );
        // The MegaLights translucency volume the forward and glass cells read (off: the neutral block), after the
        // grid it samples its lights from and before the first lit node.
        AddFrameMegaLightsVolume( graph, textures, frame, lightGrid );
        if ( WorldLightGridRead() )
            AddFrameLightGridWorld( graph, textures, frame );
        if ( GlobalDistanceFieldRead() )
            AddFrameGlobalDistanceField( graph, textures, frame );
        AddFrameBasePass( graph, textures );
        // The selection outline's mask, which a face view's missing post chain would never read.
        if ( !m_ViewProfile.SceneCapture )
            AddFrameSilhouette( graph, textures );
        // A forward view lights its pixels with the sky's cubes: its captures are measured with the same.
        if ( m_RenderPath != Core::RenderPath::Deferred || !m_GBuffer )
            AddFrameReflectionCaptureUpdate( graph, textures, false );

        // Deferred: fill the G-buffer, then shade it (or show a debug channel) into the scene target before
        // the post chain.
        if ( m_RenderPath == Core::RenderPath::Deferred && m_GBuffer )
        {
            auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            const std::vector<RDG::TextureRef> gbuffer = textures.Colors( m_GBuffer, "GBuffer" );

            AddFrameGBuffer( graph, textures, meshRenderer );
            AddFrameTerrainGBuffer( graph, textures );
            AddFrameForwardDepthNormal( graph, textures, meshRenderer );
            AddFrameDepthResolve( graph, textures );

            // THE PATH-TRACED FRAME (UE RenderPathTracing): the lit image is the path tracer's accumulation,
            // written over the scene colour in place of the deferred lighting chain - no SSAO, surface cache,
            // screen or traced GI, SSR, sun mask, composite, scene copy, water or glass (the paths integrate what
            // those approximate). The G-buffer and depth above stay: the overlays, the editor's grid and the
            // velocity read them.
            if ( PathTraced( m_RenderMethods ) )
            {
                if ( const Common::BoolResultStr traced = AddFramePathTracing( graph, textures, frame ); !traced )
                {
                    LOG_ERROR( "[SceneRenderer] {}: the path tracer refused the frame; the scene colour shows the "
                               "unlit base pass: {}",
                               m_ViewResources.GetName(), traced.GetError() );
                }
            }
            else
            {
                const FrameSun  sun        = GetFrameSun();
                const glm::vec4 lightDir   = sun.Direction;
                const glm::vec4 lightColor = sun.ColorIntensity;
                AddFrameSSAO( graph, textures, gbuffer, frame );
                AddFrameSurfaceCache( graph, textures, frame );

                // The screen traces - SSR's rays and the screen probes' (UE ScreenProbeTraceScreenTextures) - walk
                // this frame's HZB and return LAST frame's lit scene at a hit: the view's scene colour history,
                // which this frame's scene copy below refills.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
                // ScreenSpace, SoftwareTraced and HardwareTraced run the same pass (the traced two its traced
                // permutation over their tracer, AddFrameSSR).
                const Common::Scalability::ReflectionMethod reflections = m_RenderMethods.Reflections.Effective;
                System::SSRRenderer* const                  ssr =
                     ( reflections == Common::Scalability::ReflectionMethod::ScreenSpace ||
                       reflections == Common::Scalability::ReflectionMethod::SoftwareTraced ||
                       reflections == Common::Scalability::ReflectionMethod::HardwareTraced ) &&
                               EnsureSSRResources()
                                           ? UNIQUE_GET_AS( System::SSRRenderer, m_RenderSystems["SSRSystem"] )
                                           : nullptr;
                const bool             probesTrace = RadianceCacheRuns() && m_GIPipeline.ScreenProbes;
                const FrameScreenTrace screenTrace =
                     ( ssr != nullptr || ( probesTrace && EnsureScreenTraceResources() ) )
                          ? AddFrameScreenTrace( graph, textures, frame )
                          : FrameScreenTrace{};
                AddFrameTracedGI( graph, textures, gbuffer, lightDir, lightColor, screenTrace, frame );
                // The pending capture's cube, measured by this view's GI (the radiance cache this frame's update
                // just wrote) or, without traced GI, by the sky's irradiance - before the composite that reads it.
                AddFrameReflectionCaptureUpdate( graph, textures, RadianceCacheRuns() );

                // SSR before the lighting, which composes it over the reflection environment (UE: SSR, then the
                // reflection environment pass).
                const FrameSSR ssrFrame = ssr != nullptr
                                               ? AddFrameSSR( graph, textures, *ssr, gbuffer, screenTrace, frame )
                                               : FrameSSR{};
                // The translucent front layer (Reflections.TranslucentFrontLayer, UE Lumen front layer
                // translucency reflections): its G-buffer, then its rays through the opaque reflections' method -
                // the same screen trace (HZB, last frame's scene), tracer, hit lighting and denoiser - for the
                // glass below to read.
                const FrameFrontLayer frontLayer =
                     ssr != nullptr && TranslucentFrontLayerReflections()
                          ? AddFrameTranslucentFrontLayer( graph, textures, meshRenderer, gbuffer, screenTrace,
                                                           frame )
                          : FrameFrontLayer{};

                // The cascades and the cloud shadow map reach the composite as scene view inputs (block entries
                // with their neutral defaults), not as a second, separately resolved read list.
                // "Shadows: Ray Traced Sun" in place of the cascades (SunShadowIsRayTraced): the composite's
                // DESERT_SUN_SHADOW_SCREEN_MASK permutation reads this mask.
                // "VSM: Project" likewise: the same mask, the same permutations.
                const RDG::TextureRef sunShadowMask =
                     SunShadowIsVirtual()
                          ? AddFrameVirtualSunShadow( graph, textures, gbuffer, -glm::vec3( lightDir ), frame )
                     : SunShadowIsRayTraced()
                          ? AddFrameRayTracedSunShadow( graph, textures, gbuffer, -glm::vec3( lightDir ), frame )
                          : RDG::TextureRef{};
                // Shadows Stochastic: the local lights sampled, traced and denoised by MegaLights, which the
                // composite's MegaLights program reads in place of its light-grid loop.
                const System::MegaLightsLighting megaLights =
                     LocalLightsAreStochastic() ? AddFrameMegaLights( graph, textures, gbuffer, frame )
                                                : System::MegaLightsLighting{};
                // Shadows HardwareTraced: the point/spot lights' ray-traced masks, one layer per ShadowMaskSlot.
                const RDG::TextureRef localShadowMasks = LocalShadowsAreRayTraced()
                                                              ? AddFrameRayTracedLocalShadows( graph, textures,
                                                                                               gbuffer, frame )
                                                              : RDG::TextureRef{};
                // SSS-PROFILE: the composite splits the scattered texels' diffuse into this target when the view
                // scatters; Burley + Recombine add it back BEFORE the forward meshes (AddFrameGeneric), which may
                // stand in front of a scattered texel and must not receive its diffuse.
                const RDG::TextureRef subsurfaceDiffuse =
                     SubsurfaceRuns()
                          ? graph.CreateTexture(
                                 RDG::TextureDesc{ .Size   = { .Width  = m_TargetFramebuffer->GetFramebufferWidth(),
                                                               .Height = m_TargetFramebuffer->GetFramebufferHeight() },
                                                   .Format = ViewTargetFormats::kSubsurfaceDiffuse },
                                 "SubsurfaceDiffuse" )
                          : RDG::TextureRef{};
                AddFrameComposite( graph, textures, gbuffer, ssrFrame.Resolved, sunShadowMask, localShadowMasks,
                                   megaLights, meshRenderer, lightDir, lightColor, frame, subsurfaceDiffuse );
                if ( subsurfaceDiffuse.IsValid() )
                    AddFrameSubsurface( graph, textures, gbuffer, subsurfaceDiffuse, frame );
                AddFrameGeneric( graph, textures, meshRenderer, sunShadowMask, localShadowMasks );

                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
                auto* copy = UNIQUE_GET_AS( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] );
                // With a screen trace on, the copy IS the view's scene colour history (what next frame's traces
                // read): one copy serves this frame's refraction and next frame's SSR and screen probes.
                std::function<void()> stampHistory;
                if ( screenTrace.SceneHistory.IsValid() )
                {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact
                    // type
                    auto* history = UNIQUE_GET_AS( System::SceneColorHistoryRenderer,
                                                   m_RenderSystems["SceneColorHistorySystem"] );
                    stampHistory  = [history, index = frame.FrameIndex]() { history->Stamp( index ); };
                }
                const RDG::TextureRef sceneCopy = AddFrameSceneCopy(
                     graph, textures, sceneColor(), copy, screenTrace.SceneHistory, std::move( stampHistory ) );

                // The water after the copy (the scene without water, which WATER-W7's refraction reads) and before
                // the glass, which then refracts the water behind it.
                AddFrameWater( graph, textures );
                AddFrameGlass( graph, textures, sceneCopy, meshRenderer, frontLayer );
            }
        }
        else
        {
            // FORWARD: the base pass drew the opaque scene (its translucent objects are the glass node's alone,
            // MeshRenderer's opaque buckets skip them), so the glass composites over it through the same route
            // as Deferred - the scene copy it refracts, then the glass node. No screen trace runs on this path:
            // the copy is no scene history, and there is no front layer (the glass reads "the sky alone").
            // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the keys name these exact types
            auto* meshRenderer = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
            auto* copy         = UNIQUE_GET_AS( System::CopyRenderer, m_RenderSystems["SceneColorCopySystem"] );
            // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
            const RDG::TextureRef sceneCopy = AddFrameSceneCopy( graph, textures, sceneColor(), copy, {}, {} );
            AddFrameGlass( graph, textures, sceneCopy, meshRenderer, FrameFrontLayer{} );
        }

        AddFrameSceneDepthResolve( graph, textures );
        AddFrameSkyAtmosphereLuts( graph, textures );
        // The path tracer's paths carry no participating media (PathTracer.hpp: a task of its own), so a
        // path-traced frame composites no height fog, cloud layer or particles over its accumulation; the
        // editor's passes at the extension points still draw. The clouds reach its paths through the sky
        // capture (the environment) and the cloud shadow map (the sun), so the layer's sky-occlusion volume,
        // which the capture marches through, is still written.
        if ( !pathTraced )
        {
            AddFrameAtmosphericFog( graph, textures );
            AddFrameVolumetricClouds( graph, textures, frame );
        }
        else
            AddFrameCloudSkyOcclusion( graph, textures );

        // The opaque scene is complete (lit, composited, the sky/fog/cloud nodes declared): passes from outside
        // the engine that belong under every translucent layer join here.
        AddExtensionPoint( graph, textures, RDG::ExtensionPoint::AfterOpaque, {} );

        // AutoExposure's build-time step, before the first node that reads last frame's adapted luminance: the
        // temporal method's opaque passes below and the temporal node.
        const RDG::TextureRef exposurePrevious = PrepareFrameAutoExposure( textures );
        // The temporal method's passes over the OPAQUE scene, before the translucency composites into the scene
        // colour (ITemporalUpscaler::AddOpaquePasses; UE AddTSRMeasureFlickeringLuma). A refusal is said; the
        // temporal node then refuses itself by name if it needed the result.
        RDG::TextureRef opaqueLuma;
        // A multisampled scene target has no temporal resolve (AddFrameTemporal's refusal), so nothing to measure.
        // A reflection capture's face view ends at the lit scene (below): no temporal resolve to measure for.
        if ( !m_ViewProfile.SceneCapture && frame.Method != TemporalMethod::None &&
             m_TemporalUpscaler != nullptr && m_TargetFramebuffer &&
             m_TargetFramebuffer->GetSpecification().Samples == 1 )
        {
            const Common::ResultStr<RDG::TextureRef> measured = m_TemporalUpscaler->AddOpaquePasses(
                 graph, frame,
                 TemporalUpscalerOpaqueInputs{
                      .SceneColor =
                           textures.Import( m_TargetFramebuffer->GetColorAttachmentImage( 0 ), "SceneColor" ),
                      .Exposure = exposurePrevious.IsValid() ? exposurePrevious : textures.System.White,
                      .Quality  = &m_Quality } );
            if ( measured )
                opaqueLuma = measured.GetValue();
            else
                LOG_ERROR( "[SceneRenderer] {}: the temporal method's opaque passes are refused: {}",
                           m_ViewResources.GetName(), measured.GetError() );
        }

        // The translucency, the debug lines and the UI canvas run AFTER the deferred lighting composite so lit
        // geometry does not paint over them, and as LOAD nodes so a CLEAR never wipes the depth later overlays
        // test against (the particle top-down bug / grid-through-meshes).
        if ( pathTraced )
            AddExtensionPoint( graph, textures, RDG::ExtensionPoint::AfterTranslucency, {} );
        else
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

        // WHAT THIS GRAPH HANDS OVER at its end, to a reader outside it.
        std::shared_ptr<Image2D> handed;
        std::string_view         handedName;
        RDG::Access              handedAccess = RDG::Access::SampledGraphics;
        if ( m_ViewProfile.SceneCapture )
        {
            // A REFLECTION CAPTURE'S FACE VIEW ENDS AT THE LIT HDR SCENE (UE CaptureSceneIntoScratchCubemap: the
            // capture's view family has PostProcessing, MotionBlur, LightShafts, on-screen debug and the editor's
            // primitives off). The capture resamples the scene colour, so the temporal resolve, the overlays, the
            // outline, eye adaptation, bloom, shafts, flare, tonemap and AA were a face view's work for nothing
            // (~1.8 ms of a 3.9 ms Bistro face, REFL-CAPTURE-NORM-2). The capture's Assemble samples the colour in
            // the next graph's compute.
            handed       = GetSceneColorImage();
            handedName   = "scene colour";
            handedAccess = RDG::Access::SampledCompute;
        }
        else
        {
            // THE TEMPORAL RESOLVE (TAA1-B 5c). Everything after it reads its output and the overlays draw into
            // it - never into the history, never into the pre-resolve scene colour. Overlays (debug lines, grid,
            // gizmos, UI) are UNJITTERED: their shaders read ViewFrame::ViewProjection or the camera's matrices,
            // never JitteredViewProjection (Camera.hpp WHICH MATRIX). Without a temporal method the post input is
            // the scene colour, as before.
            const OverlayTargets overlay =
                 AddFrameTemporal( graph, textures, frame, exposurePrevious, opaqueLuma );
            const std::vector<RDG::TextureRef> postInput =
                 overlay.IsValid() ? std::vector<RDG::TextureRef>{ overlay.Color } : sceneColor();
#if DESERT_DEV_INSTRUMENTS
            AddFrameVelocityView( graph, textures, postInput.empty() ? RDG::TextureRef{} : postInput.front() );
            AddFrameLightGridWorldView( graph, textures, postInput.empty() ? RDG::TextureRef{} : postInput.front(),
                                        frame );
            AddFrameGlobalDistanceFieldView( graph, textures, frame,
                                             postInput.empty() ? RDG::TextureRef{} : postInput.front() );
            AddFrameSurfaceCacheView( graph, textures, frame,
                                      postInput.empty() ? RDG::TextureRef{} : postInput.front() );
#endif // DESERT_DEV_INSTRUMENTS

#if DESERT_DEV_INSTRUMENTS
            // The engine's debug lines (bounding boxes), the first overlay.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            if ( auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ) )
                AddSystemRaster( graph, textures, mesh->DebugLinesPass(), overlay );
            // The scene's gameplay debug drawing (Core/DebugDraw.hpp): its lines over the engine's, its strings
            // over both. A reflection capture's face shows the game scene only, as for the extension passes.
            if ( m_DebugDraw && !m_ViewProfile.SceneCapture )
            {
                if ( m_DebugDraw->HasLines() )
                    AddSystemRaster( graph, textures, m_DebugDraw->LinesPass(), overlay );
                if ( m_DebugDraw->HasStrings() )
                    AddSystemRaster( graph, textures, m_DebugDraw->StringsPass(), overlay );
            }
#endif // DESERT_DEV_INSTRUMENTS
       // The editor's authoring overlays (colliders, cubemap preview) over the engine's debug lines.
            AddExtensionPoint( graph, textures, RDG::ExtensionPoint::Overlay, overlay );

            // The pyramid the UI samples. The UI pass that samples it declares that read itself (the editor's UI
            // pass, through ExtensionPass::Declare), and the graph orders it after the blur.
            if ( m_BackdropBlurNeeded )
                AddFrameBackdropBlur( graph, textures, postInput );

            // The UI canvas, last before the post chain.
            AddExtensionPoint( graph, textures, RDG::ExtensionPoint::UI, overlay );

            // The outline composites over the post input and hands the tonemap its result: the tonemap reading
            // the post input directly left the composite's output unread, so no selection was ever outlined.
            const RDG::TextureRef postSource = postInput.empty() ? RDG::TextureRef{} : postInput.front();
            const RDG::TextureRef outlined   = AddFrameJumpFlood( graph, textures, postSource );
            AddFrameAutoExposure( graph, textures, postInput, exposurePrevious );
            AddFrameLocalExposure( graph, textures, postInput );
            if ( m_BloomEnabled )
            {
                AddFrameBloom( graph, textures, postInput );
            }
            AddFrameLightShafts( graph, textures, postInput, values );
            AddFrameLensFlare( graph, textures, postInput, values );
            AddFrameTonemap( graph, textures, outlined.IsValid() ? outlined : postSource );

            if ( m_AAMode == Common::Scalability::AntiAliasingMethod::FXAA )
            {
                AddFrameFXAA( graph, textures );
            }
            else if ( m_AAMode == Common::Scalability::AntiAliasingMethod::SMAA )
            {
                AddFrameSMAA( graph, textures );
            }
            handed     = GetFinalImage();
            handedName = "final image";
        }
        // The final image is sampled after the graph (editor viewport, runtime blit), a face view's scene colour
        // by the capture that resamples it: the graph ends it there.
        if ( const auto extracted = textures.ExtractImported( handed, handedName, handedAccess ); !extracted )
        {
            LOG_ERROR( "SceneRenderer: frame graph '{}' cannot hand over its {}: {}", graph.GetName(), handedName,
                       extracted.GetError() );
        }
        else
        {
            // What the viewport / runtime blit shows (or the capture reads): without its writer this frame has no
            // picture (black).
            graph.SetFaultPolicy( textures.Import( handed, handedName ), RDG::ExternalFaultPolicy::FrameFatal );
        }

        // Its faults are logged by the graph backend and its own failures by ExecuteGraph; a FrameFault leaves
        // the final image black for this frame.
        // A FrameFault never reaches EndFrame: the next BeginFrame still sees the last committed frame as
        // previous. A frame that executed commits as previous with what its report says it lost.
        DESERT_PROFILE_PHASE( phases, "SceneRenderer: ExecuteGraph" );
        if ( Renderer::ExecuteGraph( graph ).IsSuccess() )
            m_ViewState.EndFrame( graph.GetExecuteReport() );
        textures.ResetInvalidatedHistories();
    }

    NO_DISCARD Common::BoolResultStr SceneRenderer::EndScene()
    {
        const ActiveViewScope viewScope( m_ViewResources ); // same reason as OnUpdate: phases interleave
        const ResourceAttributionScope ledger = OpenViewLedgerScope();

        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->ClearQueues();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )->ClearQueue();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::GrassRenderer, m_RenderSystems["GrassSystem"] )->ClearQueue();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::WaterRenderer, m_RenderSystems["WaterSystem"] )->ClearQueue();

        m_PointLight.PointLights.clear();
        m_SpotLight.SpotLights.clear();
        m_PointLightShadows.clear();
        m_SpotLightShadows.clear();
        m_FrameExtensions         = nullptr;
#if DESERT_DEV_INSTRUMENTS
        if ( m_DebugDraw )
            m_DebugDraw->SetWorld( nullptr );
#endif
        m_FrameReflectionCaptures = nullptr;
        m_FrameRayTracingCache    = nullptr;
        m_FrameLocalShadows       = nullptr;
        m_FrameDistanceFields     = nullptr;

        return BOOLSUCCESS;
    }

    // EnsureSSRResources is LAZY on purpose: every PreviewViewport owns a SceneRenderer, so allocating its
    // full-screen targets eagerly multiplied them (and the system's own ping-pong accumulation pair) by the number
    // of live previews — for a feature a preview never enables. It runs outside any render pass, and goes through
    // WaitDeviceIdle before touching GPU resources, matching what Resize() below does. A failure is non-fatal and
    // latched, so a broken shader cannot make this retry every frame.

    bool SceneRenderer::SunShadowIsRayTraced() const
    {
        // Stochastic's sun is the ray-traced one (MegaLights shades the local lights).
        using Common::Scalability::ShadowMethod;
        return m_RenderMethods.Shadows.Effective == ShadowMethod::HardwareTraced ||
               m_RenderMethods.Shadows.Effective == ShadowMethod::Stochastic;
    }

    bool SceneRenderer::LocalLightsAreStochastic() const
    {
        return m_RenderMethods.Shadows.Effective == Common::Scalability::ShadowMethod::Stochastic;
    }

    bool SceneRenderer::LocalShadowsAreRayTraced() const
    {
        return m_RenderMethods.Shadows.Effective == Common::Scalability::ShadowMethod::HardwareTraced &&
               !PathTraced( m_RenderMethods );
    }

    namespace
    {
        // The forward readers of one shadow map name outside the mesh renderer: the forward terrain and the view's
        // particle sprites (both prepared before OnUpdate's shadow gates).
        template <typename Systems>
        bool TerrainOrParticlesSample( const Systems& systems, const std::string_view mapName )
        {
            const auto terrain = systems.find( "TerrainSystem" );
            if ( terrain != systems.end() && terrain->second &&
                 static_cast<const System::TerrainRenderer*>( terrain->second.get() )->ForwardReadsTexture( mapName ) )
                return true;
            const auto particles = systems.find( "ParticleSystem" );
            return particles != systems.end() && particles->second &&
                   static_cast<const System::ParticleRenderer*>( particles->second.get() )
                        ->ViewEmittersSample( mapName );
        }
    } // namespace

    std::span<const System::ParticleViewEmitterCull> SceneRenderer::ParticleViewEmitterCulls() const
    {
        const auto particles = m_RenderSystems.find( "ParticleSystem" );
        if ( particles == m_RenderSystems.end() || !particles->second )
            return {};
        return static_cast<const System::ParticleRenderer*>( particles->second.get() )->ViewEmitterCulls();
    }

    bool SceneRenderer::LocalShadowMapsRead() const
    {
        const auto meshes = m_RenderSystems.find( "MeshSystem" );
        if ( meshes != m_RenderSystems.end() && meshes->second &&
             static_cast<const System::MeshRenderer*>( meshes->second.get() )->LocalShadowMapReadersQueued() )
            return true;
        return TerrainOrParticlesSample( m_RenderSystems, kSceneViewLocalShadowAtlasName );
    }

    bool SceneRenderer::SunCascadesRead() const
    {
        const auto meshes = m_RenderSystems.find( "MeshSystem" );
        if ( meshes != m_RenderSystems.end() && meshes->second &&
             static_cast<const System::MeshRenderer*>( meshes->second.get() )->SunCascadeReadersQueued() )
            return true;
        return TerrainOrParticlesSample( m_RenderSystems, kSceneViewShadowCascadesName );
    }

    void SceneRenderer::AssignLocalShadowMaskSlots( const Core::Frustum& view )
    {
        // ShadowCaster is UpdateLocalShadows' answer "this light casts in this view" (CastShadows, and the view
        // draws shadows), whether or not a map was drawn for it.
        m_LocalShadowMaskLights.clear();
        const bool traced = LocalShadowsAreRayTraced();
        int32_t    slot   = 0;
        for ( std::size_t i = 0; i < m_PointLight.PointLights.size(); ++i )
        {
            auto& point          = m_PointLight.PointLights[i];
            point.ShadowMaskSlot = -1;
            // Per-light view cull (UE: a light whose bounds miss the view frustum renders no shadow): no pixel of
            // the view is inside its attenuation sphere, so no slot, no trace, no pack.
            if ( !traced || point.ShadowCaster == 0 || !view.IntersectsSphere( point.Position, point.Radius ) )
                continue;
            point.ShadowMaskSlot = slot++;
            m_LocalShadowMaskLights.push_back( System::RayTracedLocalShadowLight{
                 .Position          = point.Position,
                 .AttenuationRadius = point.Radius,
                 .SourceRadius      = point.SourceRadius,
                 .SourceId          = i < m_PointLightShadows.size() ? m_PointLightShadows[i].SourceId : 0u } );
        }
        for ( std::size_t i = 0; i < m_SpotLight.SpotLights.size(); ++i )
        {
            auto& spot          = m_SpotLight.SpotLights[i];
            spot.ShadowMaskSlot = -1;
            if ( !traced || spot.ShadowCaster == 0 || !view.IntersectsSphere( spot.Position, spot.Range ) )
                continue;
            spot.ShadowMaskSlot = slot++;
            m_LocalShadowMaskLights.push_back( System::RayTracedLocalShadowLight{
                 .Position          = spot.Position,
                 .AttenuationRadius = spot.Range,
                 .SourceRadius      = spot.SourceRadius,
                 .SpotDirection     = spot.Direction,
                 .CosOuter          = spot.CosOuter,
                 .SourceId          = i < m_SpotLightShadows.size() ? m_SpotLightShadows[i].SourceId : 0u } );
        }
    }

    bool SceneRenderer::EnsureMegaLightsResources()
    {
        if ( m_MegaLightsReady )
            return true;
        if ( m_MegaLightsFailed )
            return false;
        if ( const Common::BoolResultStr initialized = m_MegaLights.Initialize(); !initialized )
        {
            LOG_ERROR( "[SceneRenderer] {}: MegaLights could not be created; Shadows Stochastic resolves to "
                       "CascadedMaps: {}",
                       m_ViewResources.GetName(), initialized.GetError() );
            m_MegaLightsFailed = true;
            return false;
        }
        m_MegaLightsReady = true;
        return true;
    }

    System::MegaLightsLighting SceneRenderer::AddFrameMegaLights( RDG::Builder& graph, FrameTextures& textures,
                                                                  const std::vector<RDG::TextureRef>& gbuffer,
                                                                  const ViewFrame&                    frame )
    {
        const auto pointCount = static_cast<uint32_t>( GetPointLights().PointLights.size() );
        const auto spotCount  = static_cast<uint32_t>( GetSpotLights().SpotLights.size() );
        if ( pointCount + spotCount == 0 )
            return {}; // no local light: nothing to sample, the composite adds none
        if ( gbuffer.size() < 4 )
        {
            LOG_ERROR( "[SceneRenderer] {}: MegaLights needs the 4 G-buffer colours, the graph has {}; the local "
                       "lights are unlit this frame",
                       m_ViewResources.GetName(), gbuffer.size() );
            return {};
        }
        // The frame resolved to Stochastic only with this frame's TLAS built (OnUpdate, LowerShadowsForFrame).
        const RayTracing::RayTracingSceneFrame& scene = m_RayTracingScene.GetFrame();
        const FrameGraphRefs                    refs  = textures.GraphRefs();
        const System::MegaLightsInputs          inputs{ .Depth       = textures.Depth( m_GBuffer, "GBuffer" ),
                                                        .GBufferA    = gbuffer[0],
                                                        .GBufferB    = gbuffer[1],
                                                        .ShadingWord = gbuffer[2],
                                                        .Velocity    = textures.Transients.Velocity,
                                                        .Black       = refs.System.Black,
                                                        .Tlas        = scene.Tlas,
                                                        .Metadata    = scene.Metadata,
                                                        .Lights      = refs.Transients.Lights,
                                                        .PointLights = pointCount,
                                                        .SpotLights  = spotCount,
                                                        .Width       = frame.Split.Render.Width,
                                                        .Height      = frame.Split.Render.Height,
                                               // The G-buffer depth is the opaque scene: the march has
                                               // something to hit (as the composite's contact shadows).
                                                        .ContactShadowView = SceneResources::ContactShadowViewVector(
                                                    GetContactShadowSettings( frame, true ) ) };
        const Common::ResultStr<System::MegaLightsLighting> lighting =
             m_MegaLights.AddPasses( graph, inputs, m_MegaLightsQuality, frame );
        if ( !lighting )
        {
            LOG_ERROR( "[SceneRenderer] {}: {}; the local lights are unlit this frame", m_ViewResources.GetName(),
                       lighting.GetError() );
            return {};
        }
        return lighting.GetValue();
    }

    void SceneRenderer::AddFrameMegaLightsVolume( RDG::Builder& graph, FrameTextures& textures,
                                                  const ViewFrame& frame, const LightGridParamsPayload& lightGrid )
    {
        const FrameGraphRefs refs            = textures.GraphRefs();
        const auto           pointCount      = static_cast<uint32_t>( GetPointLights().PointLights.size() );
        const auto           spotCount       = static_cast<uint32_t>( GetSpotLights().SpotLights.size() );
        textures.Transients.MegaLightsVolume = System::AddMegaLightsVolumeOff( graph, refs.System.Black3D );
        // A path-traced frame's lit image is the accumulation: no forward or glass cell lights over it.
        if ( !LocalLightsAreStochastic() || pointCount + spotCount == 0 || PathTraced( m_RenderMethods ) )
            return; // the forward cells loop their light grid; the glass adds no local light
        if ( !m_MegaLightsVolumeReady )
        {
            if ( const Common::BoolResultStr initialized = m_MegaLightsVolume.Initialize(); !initialized )
            {
                LOG_ERROR( "[SceneRenderer] {}: {}; the forward and translucent surfaces get no local light under "
                           "Stochastic",
                           m_ViewResources.GetName(), initialized.GetError() );
                return;
            }
            m_MegaLightsVolumeReady = true;
        }
        // The frame resolved to Stochastic only with this frame's TLAS built (OnUpdate, LowerShadowsForFrame).
        const RayTracing::RayTracingSceneFrame&       scene = m_RayTracingScene.GetFrame();
        const System::MegaLightsVolumeInputs          inputs{ .Tlas        = scene.Tlas,
                                                              .Metadata    = scene.Metadata,
                                                              .Lights      = textures.Transients.Lights,
                                                              .Black3D     = refs.System.Black3D,
                                                              .PointLights = pointCount,
                                                              .SpotLights  = spotCount,
                                                              .Width       = frame.Split.Render.Width,
                                                              .Height      = frame.Split.Render.Height,
                                                              .Near        = lightGrid.ZParams.x,
                                                              .Far         = lightGrid.ZParams.z };
        const Common::ResultStr<MegaLightsVolumeRefs> volume =
             m_MegaLightsVolume.AddPasses( graph, inputs, m_MegaLightsVolumeQuality, frame );
        if ( !volume )
        {
            LOG_ERROR(
                 "[SceneRenderer] {}: {}; the forward and translucent surfaces get no local light this frame",
                 m_ViewResources.GetName(), volume.GetError() );
            return;
        }
        textures.Transients.MegaLightsVolume = volume.GetValue();
    }

    bool SceneRenderer::SunShadowIsVirtual() const
    {
        return m_RenderMethods.Shadows.Effective == Common::Scalability::ShadowMethod::VirtualMaps;
    }

    bool SceneRenderer::SunShadowIsScreenMask() const
    {
        return SunShadowIsRayTraced() || SunShadowIsVirtual();
    }

    bool SceneRenderer::EnsureVirtualShadowResources()
    {
        if ( m_VirtualShadowReady )
            return true;
        if ( m_VirtualShadowFailed )
            return false;
        if ( const Common::BoolResultStr initialized = m_VirtualShadowMap.Initialize(); !initialized )
        {
            LOG_ERROR(
                 "[SceneRenderer] {}: Shadows: Virtual Shadow Map could not be created; the sun casts no shadow "
                 "while the method resolves to VirtualMaps: {}",
                 m_ViewResources.GetName(), initialized.GetError() );
            m_VirtualShadowFailed = true;
            return false;
        }
        m_VirtualShadowReady = true;
        return true;
    }

    RDG::TextureRef SceneRenderer::AddFrameVirtualSunShadow( RDG::Builder& graph, FrameTextures& textures,
                                                             const std::vector<RDG::TextureRef>& gbuffer,
                                                             const glm::vec3&                    directionToSun,
                                                             const ViewFrame&                    frame )
    {
        if ( !EnsureVirtualShadowResources() )
            return {};
        const System::VirtualShadowMapViewInputs view{ .Depth  = textures.Depth( m_GBuffer, "GBuffer" ),
                                                       .Normal = gbuffer[1],
                                                       .InvJitteredViewProjection =
                                                            frame.InvJitteredViewProjection,
                                                       .Projection     = frame.Projection,
                                                       .CameraPosition = frame.CameraPosition,
                                                       .Width          = frame.Split.Render.Width,
                                                       .Height         = frame.Split.Render.Height,
                                                       .FrameIndex     = frame.FrameIndex };
        const System::VirtualShadowMapSun        sun{ .DirectionToSun     = directionToSun,
                                                      .SourceAngleDegrees = m_SunSourceAngleDegrees };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* const meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        // The casters that changed since the last frame drop their cached pages (UE VSM cache invalidation).
        const System::MeshRenderer::VirtualShadowInvalidation invalidation =
             meshes->CollectVirtualShadowInvalidation();
        const Common::ResultStr<System::VirtualShadowMapFrame> pages = m_VirtualShadowMap.AddPagePasses(
             graph, view, sun, m_VirtualShadowQuality, invalidation.Boxes, invalidation.StaticCastersChanged );
        if ( !pages )
        {
            LOG_ERROR( "[SceneRenderer] {}: {}; the sun casts no shadow this frame", m_ViewResources.GetName(),
                       pages.GetError() );
            return {};
        }
        // "VSM: Raster": every caster into the dirty pages, before the projection reads the pool. A refusal is
        // a missing caster program or pipeline, which no later frame repairs: said once, and the method stays
        // dark.
        if ( const Common::BoolResultStr raster =
                  meshes->AddVirtualShadowRasterPasses( graph, pages.GetValue(), m_VirtualShadowMap );
             !raster.IsSuccess() )
        {
            LOG_ERROR( "[SceneRenderer] {}: Shadows: Virtual Shadow Map casters cannot be drawn; the sun casts no "
                       "shadow while the method resolves to VirtualMaps: {}",
                       m_ViewResources.GetName(), raster.GetError() );
            m_VirtualShadowFailed = true;
            m_VirtualShadowReady  = false;
            return {};
        }
        const Common::ResultStr<RDG::TextureRef> mask =
             m_VirtualShadowMap.AddProjection( graph, pages.GetValue(), view );
        if ( !mask )
        {
            LOG_ERROR( "[SceneRenderer] {}: {}; the sun casts no shadow this frame", m_ViewResources.GetName(),
                       mask.GetError() );
            return {};
        }
        return mask.GetValue();
    }

    bool SceneRenderer::EnsureRayTracedShadowResources()
    {
        if ( m_RayTracedShadowReady )
            return true;
        if ( m_RayTracedShadowFailed )
            return false;
        Common::BoolResultStr initialized = m_RayTracedSunShadow.Initialize();
        if ( initialized )
            initialized = m_SunShadowDenoiser.Initialize();
        if ( initialized )
            initialized = m_RayTracedLocalShadow.Initialize();
        if ( !initialized )
        {
            LOG_ERROR( "[SceneRenderer] {}: Shadows: Ray Traced Sun could not be created; the sun casts no shadow "
                       "while the method resolves to HardwareTraced: {}",
                       m_ViewResources.GetName(), initialized.GetError() );
            m_RayTracedShadowFailed = true;
            return false;
        }
        m_RayTracedShadowReady = true;
        return true;
    }

    RDG::TextureRef SceneRenderer::AddFrameRayTracedSunShadow( RDG::Builder& graph, FrameTextures& textures,
                                                               const std::vector<RDG::TextureRef>& gbuffer,
                                                               const glm::vec3&                    directionToSun,
                                                               const ViewFrame&                    frame )
    {
        // The frame resolved to a ray-traced sun only with this frame's TLAS built (OnUpdate,
        // LowerShadowsForFrame).
        const RayTracing::RayTracingSceneFrame& scene  = m_RayTracingScene.GetFrame();
        const uint32_t                          width  = frame.Split.Render.Width;
        const uint32_t                          height = frame.Split.Render.Height;
        const RDG::TextureRef                   depth  = textures.Depth( m_GBuffer, "GBuffer" );
        const System::ScreenShadowMaskInputs    inputs{ .Depth    = depth,
                                                        .Normal   = gbuffer[1],
                                                        .Tlas     = scene.Tlas,
                                                        .Metadata = scene.Metadata,
                                                        .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                                                        .Width  = width,
                                                        .Height = height };
        const System::RayTracedSunShadowParams  params = System::MakeRayTracedSunShadowParams(
             m_SunSourceAngleDegrees, directionToSun, m_RayTracedShadowSamples,
             System::kRayTracedSunShadowMaxDistance, frame.FrameIndex );
        const Common::ResultStr<System::RayTracedSunShadowOutputs> traced =
             m_RayTracedSunShadow.AddPass( graph, inputs, params );
        if ( !traced )
        {
            LOG_ERROR( "[SceneRenderer] {}: {}; the sun casts no shadow this frame", m_ViewResources.GetName(),
                       traced.GetError() );
            return {};
        }
        const RDG::TextureRef velocity = textures.Transients.Velocity;
        if ( !velocity.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] {}: Shadows: Ray Traced Sun: the denoiser reprojects its history through "
                       "the view's velocity, the frame has none; the undenoised mask is shaded",
                       m_ViewResources.GetName() );
            return traced.GetValue().Mask;
        }
        return m_SunShadowDenoiser.DenoiseShadowVisibility(
             graph,
             System::ShadowVisibilityDenoiserInputs{ .Mask                    = traced.GetValue().Mask,
                                                     .ClosestOccluder         = traced.GetValue().ClosestOccluder,
                                                     .LightSourceAngleRadians = params.SourceAngleRadians,
                                                     .ReconstructionRadius    = m_ShadowDenoiserRadius },
             System::DenoiserSceneTextures{ .GBufferNormal = gbuffer[1],
                                            .GBufferDepth  = depth,
                                            .Velocity      = velocity,
                                            .Black         = textures.GraphRefs().System.Black },
             frame, width, height );
    }

    RDG::TextureRef SceneRenderer::AddFrameRayTracedLocalShadows( RDG::Builder& graph, FrameTextures& textures,
                                                                  const std::vector<RDG::TextureRef>& gbuffer,
                                                                  const ViewFrame&                    frame )
    {
        // As the sun: the frame resolved to HardwareTraced only with this frame's TLAS built and the renderers
        // initialised (LowerShadowsForFrame, EnsureRayTracedShadowResources).
        const RayTracing::RayTracingSceneFrame& scene  = m_RayTracingScene.GetFrame();
        const uint32_t                          width  = frame.Split.Render.Width;
        const uint32_t                          height = frame.Split.Render.Height;
        const RDG::TextureRef                   depth  = textures.Depth( m_GBuffer, "GBuffer" );
        const RDG::TextureRef                   white  = textures.GraphRefs().System.White;
        const RDG::TextureRef                   velocity = textures.Transients.Velocity;
        const System::ScreenShadowMaskInputs    inputs{ .Depth    = depth,
                                                        .Normal   = gbuffer[1],
                                                        .Tlas     = scene.Tlas,
                                                        .Metadata = scene.Metadata,
                                                        .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                                                        .Width  = width,
                                                        .Height = height };
        const auto            count = static_cast<uint32_t>( m_LocalShadowMaskLights.size() );
        const RDG::TextureRef masks = System::RayTracedLocalShadowRenderer::CreateMasks( graph, width, height, count );
        std::unordered_set<uint32_t> live;
        for ( uint32_t slot = 0; slot < std::max( count, 1u ); ++slot )
        {
            RDG::TextureRef visibility = white; // no slot at all: layer 0 lit, never unwritten
            if ( slot < count )
            {
                const System::RayTracedLocalShadowLight& light = m_LocalShadowMaskLights[slot];
                const Common::ResultStr<System::RayTracedSunShadowOutputs> traced = m_RayTracedLocalShadow.AddTracePass(
                     graph, inputs, light, m_RayTracedShadowSamples, frame.FrameIndex );
                if ( !traced )
                {
                    LOG_ERROR( "[SceneRenderer] {}: {}; local light slot {} casts no shadow this frame",
                               m_ViewResources.GetName(), traced.GetError(), slot );
                }
                else
                    visibility = traced.GetValue().Mask;
                // A point emitter (SourceRadius 0) casts a hard shadow from one ray: nothing to denoise. A light
                // with no identity (SourceId 0) has no history to keep: its mask is shaded as traced.
                if ( traced && light.SourceRadius > 0.0f && light.SourceId != 0 )
                {
                    if ( !velocity.IsValid() )
                    {
                        LOG_ERROR( "[SceneRenderer] {}: Shadows: Ray Traced Local: the denoiser reprojects its "
                                   "history through the view's velocity, the frame has none; the undenoised mask "
                                   "is shaded",
                                   m_ViewResources.GetName() );
                    }
                    else
                    {
                        live.insert( light.SourceId );
                        std::unique_ptr<System::ScreenSpaceDenoiser>& denoiser =
                             m_LocalShadowDenoisers[light.SourceId];
                        if ( !denoiser )
                        {
                            denoiser = std::make_unique<System::ScreenSpaceDenoiser>(
                                 System::DenoiserSignal::ShadowVisibility );
                            const Common::BoolResultStr initialized = denoiser->Initialize();
                            if ( !initialized )
                            {
                                LOG_ERROR( "[SceneRenderer] {}: Shadows: Ray Traced Local: the light's denoiser "
                                           "could not be created; its undenoised mask is shaded: {}",
                                           m_ViewResources.GetName(), initialized.GetError() );
                                denoiser.reset();
                            }
                        }
                        if ( denoiser )
                        {
                            const RDG::TextureRef denoised = denoiser->DenoiseShadowVisibility(
                                 graph,
                                 System::ShadowVisibilityDenoiserInputs{
                                      .Mask                 = traced.GetValue().Mask,
                                      .ClosestOccluder      = traced.GetValue().ClosestOccluder,
                                      .LocalLight           = glm::vec4( light.Position, light.SourceRadius ),
                                      .ReconstructionRadius = m_ShadowDenoiserRadius },
                                 System::DenoiserSceneTextures{ .GBufferNormal = gbuffer[1],
                                                                .GBufferDepth  = depth,
                                                                .Velocity      = velocity,
                                                                .Black         = textures.GraphRefs().System.Black },
                                 frame, width, height );
                            if ( denoised.IsValid() )
                                visibility = denoised;
                        }
                    }
                }
            }
            const Common::BoolResultStr packed =
                 m_RayTracedLocalShadow.AddPackPass( graph, visibility, masks, slot, width, height );
            if ( !packed )
            {
                LOG_ERROR( "[SceneRenderer] {}: {}; the local lights read their shadow maps (or none) this frame",
                           m_ViewResources.GetName(), packed.GetError() );
                return {};
            }
        }
        // The histories of lights that left the frame go with them (a light that comes back starts a new one),
        // through the frame-retire path: parked at this frame, released after every frame in flight completed.
        const auto&    frames      = Engine::FrameManager::GetInstance();
        const uint64_t frameNumber = frames.GetAbsoluteFrameCount();
        for ( auto it = m_LocalShadowDenoisers.begin(); it != m_LocalShadowDenoisers.end(); )
        {
            if ( live.contains( it->first ) )
            {
                ++it;
                continue;
            }
            if ( it->second )
                m_RetiredLocalShadowDenoisers.Park( std::move( it->second ), frameNumber );
            it = m_LocalShadowDenoisers.erase( it );
        }
        return masks;
    }

    bool SceneRenderer::EnsureSSRResources()
    {
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
        if ( m_SSRResourcesReady )
            return true;
        if ( m_SSRResourcesFailed || !m_ViewProfile.ScreenSpaceReflections )
            return false;

        Renderer::GetInstance().WaitDeviceIdle();

        RegisterSystem<System::SSRRenderer>( "SSRSystem", this, m_TargetFramebuffer );
        const auto& ssrSys = SP_CAST( System::SSRRenderer, m_RenderSystems["SSRSystem"] );
        if ( const Common::BoolResultStr initialized = ssrSys->Initialize(); !initialized )
        {
            RefuseProgramMethods( kScreenTraceReflectionMethods, "SSR", initialized.GetError() );
            m_SSRResourcesFailed = true;
            return false;
        }
        if ( !EnsureScreenTraceResources() )
        {
            m_SSRResourcesFailed = true;
            return false;
        }

        m_SSRResourcesReady = true;
        return true;
    }

    namespace
    {
        template <typename Method, std::size_t N>
        void RefuseMethods( std::vector<MethodRefusal<Method>>& refusals, const std::span<const Method> methods,
                            const std::string_view axis, const std::array<std::string_view, N>& names,
                            const std::string_view program, const std::string_view error )
        {
            for ( const Method method : methods )
                if ( AddRefusal( refusals, method, program ) )
                    LOG_ERROR( "[SceneRenderer] {} {} refused on this device: its program '{}' did not build: {}",
                               axis, names[static_cast<std::size_t>( method )], program, error );
        }
    } // namespace

    void SceneRenderer::RefuseProgramMethods( const std::span<const Common::Scalability::GIMethod> methods,
                                              const std::string_view program, const std::string_view error )
    {
        RefuseMethods( m_ProgramRefusals.GlobalIllumination, methods, "GI", Common::Scalability::kGIMethodNames,
                       program, error );
    }

    void SceneRenderer::RefuseProgramMethods( const std::span<const Common::Scalability::ReflectionMethod> methods,
                                              const std::string_view program, const std::string_view error )
    {
        RefuseMethods( m_ProgramRefusals.Reflections, methods, "Reflections",
                       Common::Scalability::kReflectionMethodNames, program, error );
    }

    bool SceneRenderer::EnsurePathTracerResources()
    {
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
        if ( m_PathTracerReady )
            return true;
        if ( m_PathTracerFailed )
            return false;

        Renderer::GetInstance().WaitDeviceIdle();

        RegisterSystem<System::PathTracer>( "PathTracerSystem", this, m_TargetFramebuffer );
        const auto& pathTracer = SP_CAST( System::PathTracer, m_RenderSystems["PathTracerSystem"] );
        if ( const Common::BoolResultStr initialized = pathTracer->Initialize(); !initialized )
        {
            LOG_ERROR( "[SceneRenderer] {}: the path tracer could not be brought up; the view renders "
                       "HardwareTraced GI and reflections instead: {}",
                       m_ViewResources.GetName(), initialized.GetError() );
            m_PathTracerFailed = true;
            return false;
        }
        if ( const Common::BoolResultStr emissive = m_EmissiveLights.Initialize(); !emissive )
        {
            LOG_ERROR( "[SceneRenderer] {}: the emissive light list could not be brought up; the view renders "
                       "HardwareTraced GI and reflections instead: {}",
                       m_ViewResources.GetName(), emissive.GetError() );
            m_PathTracerFailed = true;
            return false;
        }
        m_PathTracerReady = true;
        return true;
    }

    bool SceneRenderer::EnsureScreenTraceResources()
    {
        if ( m_ScreenTraceResourcesReady )
            return true;
        if ( m_ScreenTraceResourcesFailed )
            return false;
        Renderer::GetInstance().WaitDeviceIdle();
        // The hierarchy every screen trace walks (HZBRenderer) and the scene colour history every trace returns at
        // a hit (SceneColorHistoryRenderer): the view's, built with the first trace that needs them.
        RegisterSystem<System::HZBRenderer>( "HZBSystem", this, m_TargetFramebuffer );
        if ( const Common::BoolResultStr initialized =
                  SP_CAST( System::HZBRenderer, m_RenderSystems["HZBSystem"] )->Initialize();
             !initialized )
        {
            // The screen probes run without their screen leg (black HZB, AddFrameTracedGI); the reflections that
            // trace the screen cannot.
            RefuseProgramMethods( kScreenTraceReflectionMethods, "HZB", initialized.GetError() );
            m_ScreenTraceResourcesFailed = true;
            return false;
        }
        RegisterSystem<System::SceneColorHistoryRenderer>( "SceneColorHistorySystem", this, m_TargetFramebuffer );
        if ( const Common::BoolResultStr initialized =
                  SP_CAST( System::SceneColorHistoryRenderer, m_RenderSystems["SceneColorHistorySystem"] )
                       ->Initialize();
             !initialized )
        {
            RefuseProgramMethods( kScreenTraceReflectionMethods, "SceneColorHistory", initialized.GetError() );
            m_ScreenTraceResourcesFailed = true;
            return false;
        }
        m_ScreenTraceResourcesReady = true;
        return true;
    }

    void SceneRenderer::Resize( const uint32_t width, const uint32_t height )
    {
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
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
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
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

    namespace
    {
        // THE ONE PLACE A MESH'S GPU SIDE IS MADE FROM ITS CPU DATA: the renderer's intake. Geometry set on the
        // game side (ECS::SetEditableMesh, DynamicMesh::Update before a first draw) carries no device buffers
        // until here (Mesh::EnsureGpuResources). A failed upload is reported once and the draw is dropped.
        [[nodiscard]] bool ReadyForDraw( Mesh& mesh )
        {
            if ( const auto uploaded = mesh.EnsureGpuResources(); !uploaded.IsSuccess() )
                LOG_ERROR( "[SceneRenderer] a mesh could not be uploaded and is not drawn: {}",
                           uploaded.GetError() );
            return mesh.GetVertexBuffer() != nullptr;
        }
    } // namespace

    namespace
    {
        // TEX-STREAMING: a draw's world bounding sphere (the LOD rule's: box centre, half-diagonal x largest
        // scale) through @p transform.
        std::pair<glm::vec3, float> WorldSphere( const Common::Math::AABB& local, const glm::mat4& transform )
        {
            const float scale = glm::max(
                 glm::length( glm::vec3( transform[0] ) ),
                 glm::max( glm::length( glm::vec3( transform[1] ) ), glm::length( glm::vec3( transform[2] ) ) ) );
            return { glm::vec3( transform * glm::vec4( 0.5f * ( local.Min + local.Max ), 1.0f ) ),
                     glm::length( local.Max - local.Min ) * 0.5f * scale };
        }
    } // namespace

    float SceneRenderer::NoteTextureDemand( const Mesh& mesh, const std::span<const glm::mat4> transforms,
                                            const std::span<const Material* const> materials,
                                            const MaterialOverrides*               overrides ) const
    {
        const auto* camera = GetMainCamera();
        if ( camera == nullptr || transforms.empty() || GetViewExtent().Height == 0 )
            return 0.0f;
        const Common::Math::AABB local = Geometry::LocalBounds( mesh.GetSubmeshes() );
        if ( Geometry::IsEmpty( local ) )
            return 0.0f;
        const Geometry::LODView view{ camera->GetPosition(), camera->GetProjectionMatrix() };
        // The largest any instance covers: the nearest instance decides what the shared textures must hold.
        float pixels = 0.0f;
        for ( const glm::mat4& transform : transforms )
        {
            const auto [center, radius] = WorldSphere( local, transform );
            pixels                      = std::max(
                 pixels, TextureStreamingDemand::ScreenPixels( center, radius, view, GetViewExtent().Height ) );
        }
        auto& demand = TextureStreamingDemand::Get();
        for ( const Material* material : materials )
            demand.Note( material, pixels );
        if ( overrides != nullptr )
            for ( const auto& [slot, asset] : overrides->Textures )
                demand.NoteAsset( Common::AssetHandle( asset ), pixels );
        return pixels;
    }

    bool SceneRenderer::IsWithinMaxDrawDistance( const Mesh* mesh, const glm::mat4& transform,
                                                 float maxDrawDistance ) const
    {
        const auto* camera = GetMainCamera();
        if ( maxDrawDistance <= 0.0f || mesh == nullptr || camera == nullptr )
            return true;
        return IsWithinDrawDistance( Geometry::LODView{ camera->GetPosition(), camera->GetProjectionMatrix() },
                                     transform, Geometry::LocalBounds( mesh->GetSubmeshes() ), maxDrawDistance );
    }

    void SceneRenderer::SetLODPreview( const uint32_t entity, const int level )
    {
        if ( level < 0 )
        {
            m_LODPreview = {};
            return;
        }
        // Two frames: the renewal and the frame's BeginScene may come in either order within one frame.
        m_LODPreview = { .Entity = entity, .Level = level, .FramesLeft = 2 };
    }

    int SceneRenderer::LODPreviewFor( const uint32_t entity, const int authoredForcedLOD ) const
    {
        if ( m_LODPreview.FramesLeft > 0 && m_LODPreview.Level >= 0 && m_LODPreview.Entity == entity )
            return m_LODPreview.Level;
        return authoredForcedLOD;
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
        if ( !ReadyForDraw( *drawnMesh ) )
            return;
        {
            // Main thread only (the submit path): one scratch list of the slots' parents, reused per draw.
            static std::vector<const Material*> parents;
            parents.clear();
            for ( const MaterialInstance* slot : materialSlots->Slots )
                parents.push_back( slot != nullptr ? slot->GetParentMaterial() : nullptr );
            NoteTextureDemand( *drawnMesh, std::span<const glm::mat4>( &transform, 1 ), parents );
        }
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitMesh( { .Entity                   = extra.Entity,
                             .MotionPart               = extra.MotionPart,
                             .Mesh                     = drawnMesh,
                             .Transform                = transform,
                             .MaterialSlots            = materialSlots,
                             .SectionSlot              = extra.SectionSlot,
                             .BoneMatrices             = extra.BoneMatrices,
                             .MorphWeights             = extra.MorphWeights,
                             .MorphRevision            = extra.MorphRevision,
                             .Outlined                 = extra.Outlined,
                             .HiddenSubmeshes          = extra.HiddenSubmeshes,
                             .ForcedLOD                = LODPreviewFor( extra.Entity, extra.ForcedLOD ),
                             .LODBias                  = extra.LODBias,
                             .CastShadows              = extra.CastShadows,
                             .ReceiveShadows           = extra.ReceiveShadows,
                             .CastContactShadow        = extra.CastContactShadow,
                             .TranslucencySortPriority = extra.TranslucencySortPriority } );
    }

    void SceneRenderer::SubmitLandscapeTile( const std::shared_ptr<Image2D>&    heightmap,
                                             const System::LandscapeTileDraw&   tile,
                                             const MaterialOverrides&           overrides,
                                             const System::LandscapeWeightDraw& weights )
    {
        // TEX-STREAMING: the tile's layer textures are asked for at the size the tile covers; its sphere spans the
        // tile's square around its base height, so a camera over the tile asks for everything.
        if ( const auto* camera = GetMainCamera(); camera != nullptr && GetViewExtent().Height != 0 )
        {
            const float     side   = static_cast<float>( tile.QuadsPerTile ) * tile.SpacingCm;
            const glm::vec3 center = { tile.OriginX + ( static_cast<float>( tile.FirstSampleX ) +
                                                        0.5f * static_cast<float>( tile.QuadsPerTile ) ) *
                                                           tile.SpacingCm,
                                       tile.BaseY,
                                       tile.OriginZ + ( static_cast<float>( tile.FirstSampleZ ) +
                                                        0.5f * static_cast<float>( tile.QuadsPerTile ) ) *
                                                           tile.SpacingCm };
            const float     pixels = TextureStreamingDemand::ScreenPixels(
                 center, side * 0.70710678f, { camera->GetPosition(), camera->GetProjectionMatrix() },
                 GetViewExtent().Height );
            for ( const auto& [slot, asset] : overrides.Textures )
                TextureStreamingDemand::Get().NoteAsset( Common::AssetHandle( asset ), pixels );
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        UNIQUE_GET_AS( System::TerrainRenderer, m_RenderSystems["TerrainSystem"] )
             ->Submit( { .Heightmap = heightmap, .Landscape = tile, .Weights = weights, .Overrides = overrides } );
    }

    void SceneRenderer::SubmitLandscapeGrass( System::GrassFieldDraw draw )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::GrassRenderer, m_RenderSystems["GrassSystem"] )->Submit( std::move( draw ) );
    }

    void SceneRenderer::SubmitWaterZone( System::WaterZoneDraw zone )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::WaterRenderer, m_RenderSystems["WaterSystem"] )->Submit( std::move( zone ) );
    }

    void SceneRenderer::SubmitGenericMesh( const uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                           const std::string& shaderName, const MaterialOverrides& overrides,
                                           bool outlined, Image2D* directTexture,
                                           const std::string& directTextureSampler, bool castShadows,
                                           bool                                    visibleInRayTracing,
                                           const RayTracing::RayTracingProgramHit& rayHit )
    {
        if ( mesh == nullptr || !ReadyForDraw( *mesh ) )
            return;
        if ( const float pixels =
                  NoteTextureDemand( *mesh, std::span<const glm::mat4>( &transform, 1 ), {}, &overrides );
             directTexture != nullptr )
            TextureStreamingDemand::Get().NoteAsset( directTexture->GetOwningAsset(), pixels );
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitGenericMesh( { .Entity               = entity,
                                    .Mesh                 = mesh,
                                    .Transform            = transform,
                                    .ShaderName           = shaderName,
                                    .Overrides            = overrides,
                                    .Outlined             = outlined,
                                    .CastShadows          = castShadows,
                                    .VisibleInRayTracing  = visibleInRayTracing,
                                    .DirectTexture        = directTexture,
                                    .DirectTextureSampler = directTextureSampler,
                                    .RayHit               = rayHit } );
    }

    void SceneRenderer::SubmitSlotMaterialMesh( const uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                                Material* material, uint64_t visibleSubmeshMask, bool outlined,
                                                bool castShadows, const uint32_t motionPart )
    {
        if ( mesh == nullptr || !ReadyForDraw( *mesh ) )
            return;
        {
            const Material* const drawn[] = { material };
            NoteTextureDemand( *mesh, std::span<const glm::mat4>( &transform, 1 ), drawn );
        }
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitGenericMesh( { .Entity      = entity,
                                    .MotionPart  = motionPart,
                                    .Mesh        = mesh,
                                    .Transform   = transform,
                                    .Outlined    = outlined,
                                    .CastShadows = castShadows,
                                    // A per-slot material draw is always a mesh's submeshes: a surface rays hit.
                                    .VisibleInRayTracing = true,
                                    .SlotMaterial        = material,
                                    .VisibleSubmeshMask  = visibleSubmeshMask } );
    }

    uint32_t SceneRenderer::GetIsmInstancesDrawn() const
    {
        const auto found = m_RenderSystems.find( "MeshSystem" );
        if ( found == m_RenderSystems.end() )
            return 0;
        return static_cast<const System::MeshRenderer*>( found->second.get() )->GetIsmInstancesDrawn();
    }

    uint64_t SceneRenderer::GetViewTrianglesDrawn() const
    {
        const auto found = m_RenderSystems.find( "MeshSystem" );
        if ( found == m_RenderSystems.end() )
            return 0;
        return static_cast<const System::MeshRenderer*>( found->second.get() )->GetViewTrianglesDrawn();
    }

    void SceneRenderer::SubmitInstancedMesh( const uint32_t entity, Mesh* mesh,
                                             const MaterialInstancePtr& material, uint64_t hiddenSubmeshes,
                                             const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                             bool castShadows, bool castContactShadow,
                                             const InstanceCullDistance& cullDistance, const InstanceWind& wind,
                                             const MaterialInstancePtr& displacedMaterial )
    {
        if ( mesh == nullptr || !ReadyForDraw( *mesh ) )
            return;
        if ( transforms )
        {
            const Material* const drawn[] = { material ? material->GetParentMaterial() : nullptr,
                                              displacedMaterial ? displacedMaterial->GetParentMaterial()
                                                                : nullptr };
            NoteTextureDemand( *mesh, *transforms, drawn );
        }
        // NO CAST, AND THAT IS THE POINT. This used to read
        // `static_cast<Desert::StaticMesh*>( const_cast<Mesh*>( mesh ) )`, and the downcast was a lie
        // the type told: an ISM's mesh is very often a DynamicMesh — every primitive one is, and the
        // component's own `RuntimeMesh` member is a `shared_ptr<DynamicMesh>`. The queue never used it
        // as a StaticMesh (RenderMesh takes a Mesh), so nothing broke; the type simply claimed
        // something untrue, and a reader who believed it would reach for members that are not there.
        UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->SubmitInstancedMesh( { .Entity            = entity,
                                      .Mesh              = mesh,
                                      .Material          = material,
                                      .DisplacedMaterial = displacedMaterial,
                                      .HiddenSubmeshes   = hiddenSubmeshes,
                                      .Transforms        = transforms,
                                      .CastShadows       = castShadows,
                                      .CastContactShadow = castContactShadow,
                                      .CullDistance      = cullDistance,
                                      .Wind              = wind } );
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
                                          const SkySettings& sky, const SunLightFx& fx,
                                          const AtmosphereLight1& light1 )
    {
        m_SunLightFx = fx;
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->SetProceduralSky( enabled, sunDir, bakeNow, sky, fx, light1 );
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

    bool SceneRenderer::DrawsVolumetricClouds() const
    {
        const auto it = m_RenderSystems.find( "VolumetricCloudSystem" );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        return it != m_RenderSystems.end() &&
               UNIQUE_GET_AS( System::VolumetricCloudRenderer, it->second )->IsPresent();
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

    SkyLightState SceneRenderer::GetSkyLightState() const
    {
        const auto it = m_RenderSystems.find( "SkyboxSystem" );
        if ( it == m_RenderSystems.end() )
            return SkyLightState::NoSky;

        const auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, it->second );
        return sky ? sky->GetSkyLightState() : SkyLightState::NoSky;
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

    void SceneRenderer::BorrowEnvironment( std::optional<Environment> environment )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )
             ->BorrowEnvironment( std::move( environment ) );
    }

    void SceneRenderer::ShareGlobalDistanceField( DistanceField::GlobalDistanceField* field )
    {
        m_SharedGlobalDistanceField = field;
    }

    void SceneRenderer::ShareSurfaceCache( SurfaceCache::SurfaceCacheRenderer* cache )
    {
        m_SharedSurfaceCache = cache;
    }

    std::shared_ptr<Image2D> SceneRenderer::GetShadowCascadeCacheImage( uint32_t cascade )
    {
        return UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )
             ->GetCascadeCacheImage( cascade );
    }

    uint32_t SceneRenderer::GetShadowCascadeCount()
    {
        return UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetCascadeCount();
    }

    void SceneRenderer::EnsureTemporalUpscaler( const TemporalMethod method )
    {
        const ResourceAttributionScope ledger = OpenViewLedgerScope();
        const TemporalMethod held = m_TemporalUpscaler ? m_TemporalUpscaler->Method() : TemporalMethod::None;
        if ( held != method )
            m_TemporalUpscaler = CreateTemporalUpscaler( method );
    }

    SceneRenderer::OverlayTargets SceneRenderer::AddFrameTemporal( RDG::Builder& graph, FrameTextures& textures,
                                                                   const ViewFrame&      frame,
                                                                   const RDG::TextureRef exposure,
                                                                   const RDG::TextureRef opaqueLuma )
    {
        // The resolve of this frame: the temporal method's, the spatial upscale (below 100 % without one), and
        // above 100 % the fixed SSAA downsample after the temporal method, or alone without one
        // (TemporalUpscaler.hpp WHERE IT RUNS).
        const bool spatial     = IsSpatialUpscale( frame );
        const bool supersample = frame.Split.Mode == Common::Scalability::ScaleMode::Supersample;
        const bool temporal    = frame.Method != TemporalMethod::None && m_TemporalUpscaler != nullptr;
        // MR2: motion blur runs on the resolved colour at the end of this function (View/MotionBlur.hpp WHERE IT
        // RUNS), so a frame with motion blur and no resolve still comes here and builds the overlay target set:
        // the overlays draw onto the blurred colour, never under it. A multisampled scene target has no
        // single-sample overlay set (the refusal below), so MSAA frames have no motion blur - a rule of the
        // combination, not a fault: they never enter this function for it.
        const bool motionBlur = MotionBlurRuns( m_MotionBlurSettings, frame ) && m_TargetFramebuffer &&
                                m_TargetFramebuffer->GetSpecification().Samples == 1;
        // MR3: depth of field (View/DepthOfField.hpp) runs here too, on the resolved colour before motion blur
        // (UE: TAA -> DOF -> MotionBlur), under the same single-sample rule.
        const bool depthOfField = DepthOfFieldRuns( m_DepthOfFieldSettings, frame ) && m_TargetFramebuffer &&
                                  m_TargetFramebuffer->GetSpecification().Samples == 1;
        if ( !m_TargetFramebuffer || ( !spatial && !supersample && !temporal && !depthOfField && !motionBlur ) )
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
             .SceneColor   = textures.Import( m_TargetFramebuffer->GetColorAttachmentImage( 0 ), "SceneColor" ),
             .SceneDepth   = textures.Depth( m_TargetFramebuffer, "SceneColor" ),
             .Velocity     = textures.Transients.Velocity,
             .ResponsiveAA = textures.Transients.ResponsiveAA,
             // No exposure node this frame: unit luminance (System.White), the weight of a neutral exposure.
             .Exposure             = exposure.IsValid() ? exposure : textures.System.White,
             .ThinGeometryCoverage = textures.Transients.ThinGeometryCoverage,
             .OpaqueLuma           = opaqueLuma,
             .Black                = textures.System.Black,
             // The surface guides of an upscaler that takes the lighting raw (deferred path only).
             .GBufferAlbedo = m_GBuffer ? textures.Colors( m_GBuffer, "GBuffer" )[0] : RDG::TextureRef{},
             .GBufferNormal = m_GBuffer ? textures.Colors( m_GBuffer, "GBuffer" )[1] : RDG::TextureRef{},
             .BrdfLut       = textures.Transients.BrdfLut,
             .ReflectionRay = textures.Transients.ReflectionRay,
             .PathGuides    = textures.Transients.PathGuides,
             .History              = histories,
             .PrevHistory          = temporal ? m_ViewState.UpscalerHistory() : nullptr,
             .Quality              = &m_Quality };
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
                m_ViewState.SetUpscalerHistory( added.GetValue().NewHistory );
            }
            // Above 100 % the temporal output is at RenderExtent; the downsample brings it to OutputExtent, where
            // the overlay target set and the post chain are.
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
        // MR2: motion blur (UE PostProcessMotionBlur) after the resolve and before the overlays and the post
        // chain: Flatten -> TileMax -> NeighborMax -> Gather on the resolved (OutputExtent) colour, from the
        // render-extent velocity and depth. No nodes when Amount is 0, MotionBlurQuality is off or the frame has
        // no duration under a TargetFPS (MotionBlurRuns).
        // MR3: depth of field (UE Diaphragm DOF) after the resolve and before motion blur: Setup -> TileFlatten ->
        // TileDilate -> GatherForeground / GatherBackground -> Recombine on the resolved (OutputExtent) colour,
        // with the CoC from the render-extent scene depth. No nodes when the focal distance is 0,
        // DepthOfFieldQuality is off or the lens cannot focus there (DepthOfFieldRuns).
        if ( depthOfField )
        {
            const Common::ResultStr<RDG::TextureRef> defocused = m_DepthOfField->AddPasses(
                 graph, frame, m_DepthOfFieldSettings,
                 DepthOfFieldInputs{ .SceneColor = resolvedColor, .SceneDepth = inputs.SceneDepth } );
            if ( !defocused )
                return withoutTemporal( defocused.GetError() );
            resolvedColor = defocused.GetValue();
        }
        if ( motionBlur )
        {
            const Common::ResultStr<RDG::TextureRef> blurred =
                 m_MotionBlur->AddPasses( graph, frame, m_MotionBlurSettings,
                                          MotionBlurInputs{ .SceneColor = resolvedColor,
                                                            .SceneDepth = inputs.SceneDepth,
                                                            .Velocity   = inputs.Velocity } );
            if ( !blurred )
                return withoutTemporal( blurred.GetError() );
            resolvedColor = blurred.GetValue();
        }

        // THE OVERLAY TARGET SET (ViewTargetSet::Output): the resolved colour, and a velocity and a depth at the
        // output extent; "Scene: PopulateSceneDepth" fills the depth from the render-extent scene depth.
        OverlayTargets overlay;
        overlay.Color = resolvedColor;
        RDG::TextureDesc desc;
        desc.Size        = RDG::Extent3D{ frame.Split.Output.Width, frame.Split.Output.Height, 1 };
        desc.Format      = ViewTargetFormats::kVelocity;
        overlay.Velocity = graph.CreateTexture( desc, "Overlay.Velocity" );
        desc.Format          = ViewTargetFormats::kResponsiveAA;
        overlay.ResponsiveAA = graph.CreateTexture( desc, "Overlay.ResponsiveAA" );
        desc.Format                  = ViewTargetFormats::kThinGeometryCoverage;
        overlay.ThinGeometryCoverage = graph.CreateTexture( desc, "Overlay.ThinGeometryCoverage" );
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

    // The view reads the captures when its profile has them and its resolved reflection method is Captures or
    // above (ScreenSpace composes over them, UE's SSR-then-reflection-environment); Off reads the sky alone. The
    // scene asks before this frame's BeginScene, so a method change reaches the capture schedule one frame later.
    bool SceneRenderer::ReadsReflectionCaptures() const
    {
        // The profile admits captures for the view; the resolved method decides whether this view reflects them
        // (Off is the sky's environment only - ReflectionMethod). A path-traced view reads none: its reflections are
        // the tracer's own bounces (UE RenderPathTracing never samples a reflection capture), so it neither drives
        // the scene's captures nor holds a capture's start on their first fill (ReflectionCaptureRenderer::
        // FirstFillPending) - a fill its frame never measures (AddFrameReflectionCaptureUpdate is not on its path).
        return m_ViewProfile.ReflectionCaptures &&
               m_RenderMethods.Reflections.Effective != Common::Scalability::ReflectionMethod::Off &&
               !PathTraced( m_RenderMethods );
    }

    uint64_t SceneRenderer::GetEnvironmentIdentity()
    {
        return EnvironmentIdentity( GetEnvironment() );
    }

    std::shared_ptr<Image2D> SceneRenderer::GetSceneColorImage() const
    {
        return m_TargetFramebuffer ? m_TargetFramebuffer->GetColorAttachmentImage( 0 ) : nullptr;
    }

    std::shared_ptr<Image2D> SceneRenderer::GetHdrRenditionImage() const
    {
        const auto it = m_RenderSystems.find( "TonemapSystem" );
        if ( it == m_RenderSystems.end() || !it->second )
            return nullptr;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        return static_cast<const System::TonemapRenderer*>( it->second.get() )->GetHdrRenditionImage();
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

    void SceneRenderer::AddPointLight( const ShaderProtocols::PointLightPayload& pointLight,
                                       const LocalLightShadowSettings&           shadow )
    {
        m_PointLight.PointLights.push_back( pointLight );
        m_PointLightShadows.push_back( shadow );
    }

    void SceneRenderer::AddSpotLight( const ShaderProtocols::SpotLightPayload& spotLight,
                                      const LocalLightShadowSettings&          shadow )
    {
        m_SpotLight.SpotLights.push_back( spotLight );
        m_SpotLightShadows.push_back( shadow );
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

    SceneResources::ContactShadowSettings SceneRenderer::GetContactShadowSettings( const ViewFrame& frame,
                                                                                   bool hasOpaqueDepth ) const
    {
        // The projection's [1][1] is 1 / tan(fovY / 2), signed by the Y flip.
        return SceneResources::ContactShadowSettings{
             .Steps                  = hasOpaqueDepth ? m_ContactShadowSteps : 0,
             .TanHalfFovY            = 1.0f / std::max( std::abs( frame.Projection[1][1] ), 1e-6f ),
             .DitherFrame            = static_cast<uint32_t>( frame.FrameIndex % 8u ),
             .SunLength              = m_SunContactShadowLength,
             .SunInWorldSpace        = m_SunContactShadowLengthInWorldSpace,
             .SunCastingIntensity    = m_SunContactShadowCastingIntensity,
             .SunNonCastingIntensity = m_SunContactShadowNonCastingIntensity };
    }

    SceneResources::ShadowFilterSettings SceneRenderer::GetShadowFilterSettings() const
    {
        return m_ShadowFilter;
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

        cloudShadow.HasMap     = clouds->GetShadowMap().IsValid();
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