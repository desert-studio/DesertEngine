#include <Common/Core/DevInstruments.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupBuild.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRendererInternal.hpp>
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
#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <memory>
#include <span>
#include <atomic>
#include <chrono>
#include <format>
#include <cmath>
#include <string>
#include <span>
#include <string_view>
#include <vector>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/DeferredFrameNodes.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SceneDepthResolveRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/GraphColorResolveRenderer.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // What the engine's RenderPass clears to when it opens with clear (RenderPassSpecification): the
        // graph nodes below that replace such a pass clear to the same values.
        RDG::LoadOp EngineClearColor()
        {
            const glm::vec4 c = RenderPassSpecification{}.ClearColor.Color;
            return RDG::LoadOp::ClearColor( c.r, c.g, c.b, c.a );
        }

        void ReadAll( RDG::PassBuilder& pass, const std::vector<RDG::TextureRef>& refs, RDG::Access access )
        {
            for ( const RDG::TextureRef ref : refs )
                pass.Read( ref, access );
        }

        // The name of a Trace program a refusal carries (static storage): the radiance cache's or the screen
        // probes' shader under @p pipeline's tracer and hit lighting (GlobalIllumination::TracerVariant).
        std::string_view TraceProgramName( const GlobalIllumination::Pipeline& pipeline, const bool screenProbes )
        {
            using GlobalIllumination::HitLighting;
            using GlobalIllumination::Tracer;
            if ( pipeline.Tracer == Tracer::SoftwareDistanceField )
                return screenProbes ? "ScreenProbeTrace (DESERT_GI_TRACER_SOFTWARE)"
                                    : "RadianceCacheTrace (DESERT_GI_TRACER_SOFTWARE)";
            if ( pipeline.Hits == HitLighting::InPlace )
                return screenProbes
                            ? "ScreenProbeTrace (DESERT_GI_TRACER_HARDWARE, DESERT_GI_HIT_LIGHTING_IN_PLACE)"
                            : "RadianceCacheTrace (DESERT_GI_TRACER_HARDWARE, DESERT_GI_HIT_LIGHTING_IN_PLACE)";
            return screenProbes ? "ScreenProbeTrace (DESERT_GI_TRACER_HARDWARE)"
                                : "RadianceCacheTrace (DESERT_GI_TRACER_HARDWARE)";
        }

        // The traced reflections' permutation of SSR (SSRRenderer::EnsureTracedProgram), by tracer and hit
        // lighting.
        std::string_view TracedReflectionProgramName( const GlobalIllumination::Pipeline& tracer,
                                                      const bool                          hitLit )
        {
            if ( tracer.Tracer == GlobalIllumination::Tracer::SoftwareDistanceField )
                return "SSRTrace (DESERT_GI_TRACER_SOFTWARE, DESERT_REFLECTIONS_TRACED)";
            return hitLit ? "SSRTrace (DESERT_GI_TRACER_HARDWARE, DESERT_REFLECTIONS_TRACED, "
                            "DESERT_REFLECTIONS_HIT_LIGHTING)"
                          : "SSRTrace (DESERT_GI_TRACER_HARDWARE, DESERT_REFLECTIONS_TRACED)";
        }
    } // namespace

    RDG::TextureRef SceneRenderer::AddFrameNoSunCascades( RDG::Builder& graph )
    {
        const RDG::TextureRef none =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = 1, .Height = 1 },
                                                    .Format = ViewTargetFormats::kShadowDepth,
                                                    .Layers = 1,
                                                    .Dim    = RDG::TextureDim::Tex2DArray },
                                  "CSM.None" );
        graph.AddPass(
             "CSM.None", RDG::PassFlags::Raster,
             [none]( RDG::PassBuilder& pass ) { pass.DepthTarget( none, RDG::LoadOp::ClearDepth( 1.0f ) ); },
             []( RDG::PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
        return none;
    }

    void SceneRenderer::ImportViewShadowMaps( RDG::Builder& graph, FrameTextures& textures )
    {
        FrameTransients& view = textures.Transients;
        if ( const auto it = m_RenderSystems.find( "MeshSystem" ); it != m_RenderSystems.end() )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, it->second );
            if ( mesh != nullptr )
            {
                view.ShadowCascades = mesh->AcquireCascadeMaps( graph );
                if ( const auto atlas = mesh->GetLocalShadowAtlasImage() )
                    view.LocalShadowAtlas = textures.Import( atlas, "LocalShadow.Atlas" );
                if ( const auto rain = mesh->GetRainOcclusionImage() )
                    view.RainOcclusionMap = textures.Import( rain, "RainOcclusion.Map" );
            }
        }
        // No cascades (shadows budgeted off, no mesh system): the lit shaders still declare a sampler2DArray,
        // which takes no 2D stand-in, so the view gets a one-texel array cleared to the far plane - 1 under the
        // cascades' standard Z, "no occluder" - and the shader's cascade count (0) never reads it anyway.
        if ( !view.ShadowCascades.IsValid() )
            view.ShadowCascades = AddFrameNoSunCascades( graph );
    }

    void SceneRenderer::ImportSceneViewTextures( RDG::Builder& graph, FrameTextures& textures )
    {
        // The sun's cascades and the local lights' atlas are NOT imported here: their budget follows the frame's
        // sun-shadow path, decided after the view's TLAS (OnUpdate), so OnUpdate imports them then
        // (ImportViewShadowMaps).
        FrameTransients& view   = textures.Transients;
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( const auto env = GetEnvironment(); env.has_value() )
        {
            if ( env->IrradianceMap.IsValid() )
                view.EnvIrradiance = textures.Import( images->Share( env->IrradianceMap ), "Env.Irradiance" );
            if ( env->PreFilteredMap.IsValid() )
                view.EnvSpecular = textures.Import( images->Share( env->PreFilteredMap ), "Env.Specular" );
        }
        if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
             brdf && brdf->GetImageHandle().IsValid() )
            view.BrdfLut = textures.Import( images->Share( brdf->GetImageHandle() ), "BRDF.LUT" );
        if ( const auto it = m_RenderSystems.find( "SkyboxSystem" ); it != m_RenderSystems.end() )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            if ( const auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, it->second );
                 sky != nullptr && sky->SkyPassSamplesLuts() )
            {
                view.SkyTransmittanceLut = textures.Import( sky->GetTransmittanceLut(), "Sky.TransmittanceLut" );
                view.SkyViewLut          = textures.Import( sky->GetSkyViewLut(), "Sky.SkyViewLut" );
            }
        // THE NEUTRAL RADIANCE CACHE (FrameTransients::RadianceCache*): a zeroed params block (no clipmap, so
        // every read answers "past the coarsest clipmap" and touches neither the indirection nor the probes, which
        // it also stands for) and the black system texture for both atlases. AddFrameTracedGI replaces it with the
        // frame's update where the view traces GI; the forward passes that declare the cache read it either way.
        {
            const std::array<std::byte, sizeof( RadianceCache::RadianceCacheParamsPayload )> zeros{};
            const RDG::BufferRef                                                             none =
                 graph.CreateBuffer( RDG::BufferDesc{ zeros.size() }, "RadianceCache.None" );
            graph.QueueBufferUpload( none, std::span<const std::byte>( zeros ) );
            view.RadianceCacheParams      = none;
            view.RadianceCacheIndirection = none;
            view.RadianceCacheProbes      = none;
            view.RadianceCacheIrradiance  = textures.GraphRefs().System.Black;
            view.RadianceCacheDepth       = textures.GraphRefs().System.Black;
        }
    }

    void SceneRenderer::AddFrameClearMainFramebuffer( RDG::Builder& graph, FrameTextures& textures )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "ClearMainFramebuffer" );
        if ( !targets )
            return;
        const RDG::ImportedFramebuffer& target = *targets;
        // The first node of the frame on both paths: the scene colour to the engine grey, and every graph colour
        // (the view's velocity) to its own clear (no motion) - FrameTextures::ColorLoads.
        const std::vector<RDG::LoadOp> colors = textures.ColorLoads( *targets, EngineClearColor() );
        graph.AddPass(
             "ClearMainFramebuffer", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 for ( uint32_t i = 0; i < target.Colors.size(); ++i )
                     pass.ColorTarget( i, target.Colors[i], colors[i] );
                 if ( target.Depth.IsValid() )
                 {
                     const glm::vec2 depthStencil = RenderPassSpecification{}.ClearColor.DepthStencil;
                     pass.DepthTarget(
                          target.Depth,
                          RDG::LoadOp::ClearDepth( depthStencil.x, static_cast<uint32_t>( depthStencil.y ) ) );
                 }
                 DeclareResolves( pass, target.Resolves );
             },
             []( RDG::PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
    }

    void SceneRenderer::AddFrameDepthResolve( RDG::Builder& graph, FrameTextures& textures )
    {
        // Copy the G-buffer depth (static opaque geometry) into the scene target depth. The deferred composite
        // writes only colour, so without this depth-tested overlays (grid, colliders) never get occluded by
        // static meshes. The graph brings both into the transfer layouts; "Deferred: Composite" below takes the
        // target depth back as its depth attachment before the forward-over-composite draws. Nothing reads the
        // G-buffer depth again this frame, so the graph ends it in the attachment layout the next frame's
        // G-buffer pass begins from.
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 ||
             m_GBuffer->GetDepthAttachmentCount() == 0 )
            return;
        const std::shared_ptr<Image2D> source = m_GBuffer->GetDepthAttachmentImage();
        const std::shared_ptr<Image2D> target = m_TargetFramebuffer->GetDepthAttachmentImage();
        const RDG::TextureRef          sourceRef =
             textures.Import( source, "GBuffer.Depth", DeferredFrameNodes::kGBufferDepthFinal );
        const RDG::TextureRef targetRef = textures.Depth( m_TargetFramebuffer, "SceneColor" );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* expand = UNIQUE_GET_AS( System::DepthExpandRenderer, m_RenderSystems["DepthExpandSystem"] );
        DeferredFrameNodes::AddDepthToScene(
             graph, m_TargetFramebuffer->GetSpecification().Samples, sourceRef, targetRef,
             [source, target]( RDG::PassContext& ) -> Common::BoolResultStr
             { return Renderer::GetInstance().CopyDepthImage( source.get(), target.get() ); },
             [expand]( RDG::PassBuilder& pass, RDG::TextureRef depth )
             {
                 if ( expand != nullptr )
                     expand->DeclareBindings( pass, depth );
             },
             [expand]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 if ( expand == nullptr )
                     return Common::MakeError( "Deferred: DepthExpand has no DepthExpandSystem" );
                 return expand->Record( context );
             } );
    }

    std::shared_ptr<Image2D> SceneRenderer::GetComputeSceneDepth() const
    {
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 )
            return nullptr;
        if ( m_TargetFramebuffer->GetSpecification().Samples <= 1 )
            return m_TargetFramebuffer->GetDepthAttachmentImage();
        const auto found = m_RenderSystems.find( "SceneDepthResolveSystem" );
        if ( found == m_RenderSystems.end() )
            return nullptr;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* resolve = UNIQUE_GET_AS( System::SceneDepthResolveRenderer, found->second );
        return resolve != nullptr && resolve->IsReady() ? resolve->GetFramebuffer()->GetDepthAttachmentImage()
                                                        : nullptr;
    }

    void SceneRenderer::AddFrameSceneDepthResolve( RDG::Builder& graph, FrameTextures& textures )
    {
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 )
            return;
        auto* resolve =
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
             UNIQUE_GET_AS( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] );
        if ( resolve == nullptr || !resolve->IsReady() )
            return;
        const RDG::TextureRef sceneDepth = textures.Depth( m_TargetFramebuffer, "SceneColor" );
        DeferredFrameNodes::AddSceneDepthResolve(
             graph, m_TargetFramebuffer->GetSpecification().Samples, sceneDepth,
             textures.Depth( resolve->GetFramebuffer(), "SceneDepthResolved" ),
             [resolve]( RDG::PassBuilder& pass, RDG::TextureRef depth )
             { resolve->DeclareBindings( pass, depth ); },
             [resolve]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return resolve->Record( context ); } );
    }

    void SceneRenderer::AddFrameGraphColorResolves( RDG::Builder& graph, FrameTextures& textures )
    {
        const std::vector<GraphColor>* colors = textures.GraphColorsOf( m_TargetFramebuffer.get() );
        if ( colors == nullptr )
            return;
        const auto                         found   = m_RenderSystems.find( "GraphColorResolveSystem" );
        System::GraphColorResolveRenderer* resolve = nullptr;
        if ( found != m_RenderSystems.end() )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            resolve = UNIQUE_GET_AS( System::GraphColorResolveRenderer, found->second );
        AddGraphColorResolves(
             graph, *colors,
             [resolve]( RDG::PassBuilder& pass, const GraphColor& color )
             {
                 if ( resolve != nullptr )
                     resolve->DeclareBindings( pass, color.Multisample );
             },
             [resolve, &graph]( const GraphColor& color )
             {
                 const auto                                      desc = graph.GetTextureDesc( color.Color );
                 const std::optional<Core::Formats::ImageFormat> format =
                      desc.IsSuccess() ? std::optional( desc.GetValue().Format ) : std::nullopt;
                 return [resolve, format]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     if ( resolve == nullptr || !format )
                         return Common::MakeError( "GraphColorResolve: no system or no format for the colour" );
                     return resolve->Record( context, *format );
                 };
             } );
    }

    void SceneRenderer::AddFrameSSAO( RDG::Builder& graph, FrameTextures& textures,
                                      const std::vector<RDG::TextureRef>& gbuffer, const ViewFrame& frame )
    {
        // SSAO first (reads the G-buffer depth + normal into the AO buffer); the lighting pass below multiplies
        // its ambient term by this. Skipped when disabled (the shader uses AO=1 then).
        //
        // GTAO's search, at UE's defaults (PostProcessAmbientOcclusion.cpp): r.GTAO.FalloffEnd 200 (UE units are
        // centimetres, as ours), r.GTAO.ThicknessBlend 0.5, r.GTAO.NumAngles 2. The taps per direction are the
        // scalability's AmbientOcclusionSamples (UE: GTAO_NUMTAPS by shader quality).
        constexpr float kGTAOWorldRadius    = Common::Units::Metres( 2.0f );
        constexpr float kGTAOThicknessBlend = 0.5f;
        constexpr int   kGTAOAngles         = 2;

        if ( !m_EnableSSAO || ScreenProbeAOReplacesSSAO() )
            return;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* ssao = UNIQUE_GET_AS( System::SSAORenderer, m_RenderSystems["SSAOSystem"] );
        if ( ssao == nullptr )
            return;
        if ( gbuffer.size() < 3 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSAO needs the G-buffer normal and position, the graph has {}",
                       gbuffer.size() );
            return;
        }
        // The AO image lives within this graph (UE: a transient from the scene textures' desc): the G-buffer's
        // size, the SSAO format, one mip, one sample.
        auto desc = graph.GetTextureDesc( gbuffer[0] );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSAO: {}", desc.GetError() );
            return;
        }
        RDG::TextureDesc aoDesc        = desc.GetValue();
        aoDesc.Format                  = ViewTargetFormats::kSSAO;
        aoDesc.Mips                    = 1;
        aoDesc.Layers                  = 1;
        aoDesc.Samples                 = 1;
        const RDG::TextureRef ao       = graph.CreateTexture( aoDesc, "SSAO" );
        // A lost SSAO term is "no occlusion": the composite goes on lit, unoccluded (RDG-FAULT1).
        graph.SetFaultDefault( ao, RDG::FaultDefault::White );
        const RDG::TextureRef depth    = textures.Depth( m_GBuffer, "GBuffer" ); // world position reconstructed
        const RDG::TextureRef normal   = gbuffer[1]; // GBufferB
        textures.Transients.SSAO       = ao;         // -> Deferred: Composite (u_SSAO)
        graph.AddPass(
             "Deferred: SSAO", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ssao->DeclareBindings( pass, depth, normal );
                 pass.ColorTarget( 0, ao, EngineClearColor() ); // AO is fully recomputed each frame
             },
             // The G-buffer was rasterised with the jittered matrix: SSAO projects its samples through it and
             // reconstructs positions with its inverse (both from the ViewFrame, one copy).
             [ssao, inputs = MaterialSSAO::Inputs{ .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                                                   .View                      = frame.View,
                                                   .Projection                = frame.Projection,
                                                   .TargetSize       = { aoDesc.Size.Width, aoDesc.Size.Height },
                                                   .FrameIndex       = frame.FrameIndex,
                                                   .WorldRadius      = kGTAOWorldRadius,
                                                   .ThicknessBlend   = kGTAOThicknessBlend,
                                                   .TapsPerDirection = m_SSAOSamples,
                                                   .Angles           = kGTAOAngles }](
                  RDG::PassContext& context ) -> Common::BoolResultStr
             { return ssao->Record( context, inputs ); } );
    }

    LightGridParamsPayload SceneRenderer::AddFrameLightGrid( RDG::Builder& graph, FrameTextures& textures,
                                                             const ViewFrame& frame )
    {
        auto* const deferred =
             dynamic_cast<System::DeferredLightingRenderer*>( m_RenderSystems["DeferredLightingSystem"].get() );
        if ( deferred == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] LightGrid: no DeferredLightingSystem, the view has no light grid" );
            return {};
        }
        // The grid is cut over the view's render extent (the scene target and the G-buffer are both drawn at it),
        // with the matrices the geometry is rasterised with (the jittered projection: a pixel's cell is the one
        // its depth sample fell in), on BOTH paths: the forward meshes read it as the deferred composite does.
        const LightGridParamsPayload grid = MakeLightGridParams(
             frame.View, frame.JitteredProjection, frame.NearPlane, frame.Split.Render.Width,
             frame.Split.Render.Height, GetPointLights(), GetSpotLights(), m_LightGridQuality );
        // "Upload: Lights.Point" / "Upload: Lights.Spot" / "Upload: Lights.ShadowViews" /
        // "Upload: Lights.GridParams", ordered before the nodes that read them.
        const System::DeferredCompositeLights lights = System::DeferredLightingRenderer::UploadLights(
             graph, GetPointLights(), GetSpotLights(),
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
             UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetLocalShadowRows(), grid );
        textures.Transients.Lights = lights;

        // UE LightGridInjection (ComputeLightGrid, before the base pass): every froxel lists the lights that reach
        // it, before any lit node reads the lists.
        graph.AddPass(
             "LightGrid", RDG::PassFlags::Compute, [deferred, lights]( RDG::PassBuilder& pass )
             { deferred->DeclareGridBindings( pass, lights, LightGridLayout::Frustum ); },
             [deferred, cells = glm::uvec3( grid.Size )]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return deferred->RecordGrid( context, cells ); } );
        return grid;
    }

    bool SceneRenderer::RadianceCacheRuns() const
    {
        // A preview profile (ViewMemory.hpp) never runs the traced GI, whatever its scene asks for.
        return m_RenderPath == Core::RenderPath::Deferred && m_GIPipeline.RadianceCache &&
               m_ViewProfile.GlobalIllumination;
    }

    void SceneRenderer::AddFrameReflectionCaptureUpdate( RDG::Builder& graph, FrameTextures& textures,
                                                         const bool tracedGI )
    {
        if ( !m_FrameReflectionCaptures )
            return;
        ReflectionCaptureLighting lighting;
        if ( tracedGI )
            lighting.RadianceCache = m_TracedFrameShared.RadianceCache;
        lighting.EnvIrradiance               = SceneViewInputsOf( textures.GraphRefs() ).EnvIrradiance;
        const std::optional<Environment> env = GetEnvironment();
        lighting.Environment                 = EnvironmentIdentity( env );
        if ( env.has_value() )
        {
            const SkyLookGPU look = ToGPU( env->Look );
            lighting.SkyYawCosSin = look.YawCosSin;
            lighting.SkyGain      = look.Gain;
        }
        m_FrameReflectionCaptures->AddCaptureUpdate( graph, textures, lighting );
    }

    bool SceneRenderer::ScreenProbeAOReplacesSSAO() const
    {
        return RadianceCacheRuns() && m_GIPipeline.ScreenProbes && m_ScreenProbeShortRangeAO;
    }

    void SceneRenderer::AddFrameTracedGI( RDG::Builder& graph, FrameTextures& textures,
                                          const std::vector<RDG::TextureRef>& gbuffer, const glm::vec4& lightDir,
                                          const glm::vec4& lightColor, const FrameScreenTrace& screenTrace,
                                          const ViewFrame& frame )
    {
        if ( !RadianceCacheRuns() )
            return;
        FrameTransients& t = textures.Transients;
        if ( gbuffer.size() < 4 || !m_GBuffer || !t.Lights.WorldGridCells.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] RadianceCache: not updated: {}",
                       gbuffer.size() < 4 ? "the G-buffer has fewer than 4 colours"
                                          : "the world light grid was not built" );
            return;
        }
        if ( !m_RadianceCache )
        {
            auto cache = std::make_unique<GlobalIllumination::WorldRadianceCache>();
            if ( const auto initialized = cache->Initialize(); !initialized )
            {
                RefuseProgramMethods( GlobalIllumination::kCacheGIMethods, "RadianceCache",
                                      initialized.GetError() );
                return;
            }
            m_RadianceCache = std::move( cache );
        }
        if ( m_GIPipeline.ScreenProbes && !m_ScreenProbeGather )
        {
            auto gather = std::make_unique<GlobalIllumination::ScreenProbeGather>();
            if ( const auto initialized = gather->Initialize(); !initialized )
            {
                RefuseProgramMethods( GlobalIllumination::kScreenProbeGIMethods, "ScreenProbes",
                                      initialized.GetError() );
                return;
            }
            m_ScreenProbeGather = std::move( gather );
        }
        if ( !m_GIPipeline.ScreenProbes && !m_IrradianceFieldGather )
        {
            auto gather = std::make_unique<GlobalIllumination::IrradianceFieldGather>();
            if ( const auto initialized = gather->Initialize(); !initialized )
            {
                RefuseProgramMethods( GlobalIllumination::kIrradianceFieldGIMethods, "IrradianceField",
                                      initialized.GetError() );
                return;
            }
            m_IrradianceFieldGather = std::move( gather );
        }
        // The Trace programs of this frame's tracer, asked before the update so the one that does not build
        // refuses the GI methods of that tracer alone (the update and the gather ask again, cached).
        if ( const auto traced = m_RadianceCache->EnsureTraceProgram( m_GIPipeline ); !traced )
        {
            RefuseProgramMethods( GlobalIllumination::TraceProgramGIMethods( m_GIPipeline, false ),
                                  TraceProgramName( m_GIPipeline, false ), traced.GetError() );
            return;
        }
        if ( m_GIPipeline.ScreenProbes )
            if ( const auto traced = m_ScreenProbeGather->EnsureTraceProgram( m_GIPipeline ); !traced )
            {
                RefuseProgramMethods( GlobalIllumination::TraceProgramGIMethods( m_GIPipeline, true ),
                                      TraceProgramName( m_GIPipeline, true ), traced.GetError() );
                return;
            }
        const RDG::TextureRef depth = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            desc  = graph.GetTextureDesc( depth );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] RadianceCache: {}", desc.GetError() );
            return;
        }
        const FrameGraphRefs                          refs = textures.GraphRefs();
        GlobalIllumination::RadianceCacheUpdateInputs inputs;
        inputs.GBufferDepth    = depth;
        inputs.GBufferA        = gbuffer[0];
        inputs.GBufferB        = gbuffer[1];
        inputs.GBufferEmissive = gbuffer[3];
        inputs.EnvSpecular     = SceneViewInputsOf( refs ).EnvSpecular;
        inputs.Tracer          = MakeTracerFrame( graph, textures, m_GIPipeline );
        // The cards a hit reads first (AddFrameSurfaceCache, earlier in this graph); a frame without them is
        // refused by the consumers (SurfaceCacheFrameMissing), never traced against stale records.
        if ( m_SurfaceCacheFrame )
            inputs.SurfaceCache = m_SurfaceCacheFrame->Hits;
        inputs.PointLights     = t.Lights.Point;
        inputs.SpotLights      = t.Lights.Spot;
        inputs.WorldGridParams = t.Lights.WorldGridParams;
        inputs.WorldGridCells  = t.Lights.WorldGridCells;
        inputs.CameraPosition  = frame.CameraPosition;
        // The captures' points are readers of this cache too: their stored diffuse is measured from it.
        if ( m_FrameReflectionCaptures && ReadsReflectionCaptures() )
            inputs.MarkPoints = m_FrameReflectionCaptures->CapturePoints();
        inputs.Extent = glm::uvec2( desc.GetValue().Size.Width, desc.GetValue().Size.Height );

        inputs.GIFrame              = AddFrameGIRecord( graph, frame, lightDir, lightColor, inputs.Extent );
        m_TracedFrameShared.GIFrame = inputs.GIFrame;

        const auto import = [&textures]( const std::shared_ptr<Image>& image, const std::string_view name )
        { return textures.Import( image, name ); };
        const auto cache = m_RadianceCache->AddUpdatePasses( graph, import, m_RadianceCacheInputs, inputs );
        if ( !cache )
        {
            LOG_ERROR( "[SceneRenderer] RadianceCache: {}", cache.GetError() );
            return;
        }
        m_TracedFrameShared.RadianceCache = cache.GetValue();
        // Every lit pass after this one reads this frame's cache (the forward meshes and the glass).
        textures.Transients.RadianceCacheParams      = cache.GetValue().Params;
        textures.Transients.RadianceCacheIndirection = cache.GetValue().Indirection;
        textures.Transients.RadianceCacheProbes      = cache.GetValue().Probes;
        textures.Transients.RadianceCacheIrradiance  = cache.GetValue().Irradiance;
        textures.Transients.RadianceCacheDepth       = cache.GetValue().Depth;

        // The cards' indirect light off this update (the radiosity slot): the frame's relit pages.
        if ( m_SurfaceCache && m_SurfaceCacheFrame )
        {
            const GlobalIllumination::RadianceCacheFrame& rc = cache.GetValue();
            if ( const auto lit = m_SurfaceCache->AddIndirectLightingPass(
                      graph, import, m_SurfaceCacheFrame->Scene, m_SurfaceCacheFrame->Buffers,
                      { .Params      = rc.Params,
                        .Indirection = rc.Indirection,
                        .Probes      = rc.Probes,
                        .Irradiance  = rc.Irradiance,
                        .Depth       = rc.Depth } );
                 !lit )
                LOG_ERROR( "[SceneRenderer] Surface Cache: {}", lit.GetError() );
        }

        // ProbeVolume (UE Lumen's irradiance field gather): the cache IS the final gather, read per pixel.
        if ( !m_GIPipeline.ScreenProbes )
        {
            GlobalIllumination::IrradianceFieldGatherInputs field{ .GBufferDepth  = inputs.GBufferDepth,
                                                                   .GBufferB      = inputs.GBufferB,
                                                                   .GIFrame       = inputs.GIFrame,
                                                                   .RadianceCache = cache.GetValue(),
                                                                   .Extent        = inputs.Extent };
            const auto gathered = m_IrradianceFieldGather->AddPasses( graph, import, field );
            if ( !gathered )
            {
                LOG_ERROR( "[SceneRenderer] IrradianceField: {}", gathered.GetError() );
                return;
            }
            t.GIDiffuse  = gathered.GetValue().Diffuse;
            t.GISpecular = gathered.GetValue().Specular;
            t.GIBackface = gathered.GetValue().Backface;
            return;
        }

        GlobalIllumination::ScreenProbeGatherInputs gather;
        gather.GBufferDepth       = inputs.GBufferDepth;
        gather.GBufferA           = inputs.GBufferA;
        gather.GBufferB           = inputs.GBufferB;
        gather.GBufferShadingWord   = gbuffer[2];
        gather.GBufferEmissive    = inputs.GBufferEmissive;
        gather.EnvSpecular        = inputs.EnvSpecular;
        gather.Tracer             = inputs.Tracer;
        gather.SurfaceCache       = inputs.SurfaceCache;
        gather.PointLights        = inputs.PointLights;
        gather.SpotLights         = inputs.SpotLights;
        gather.WorldGridParams    = inputs.WorldGridParams;
        gather.WorldGridCells     = inputs.WorldGridCells;
        gather.GIFrame            = inputs.GIFrame;
        gather.RadianceCache      = cache.GetValue();
        gather.Extent             = inputs.Extent;
        gather.AdaptiveProbePercent = m_ScreenProbeAdaptivePercent;
        gather.ShortRangeAO         = m_ScreenProbeShortRangeAO;
        gather.PrevViewProjection = frame.PrevViewProjection;
        gather.PrevCameraPosition = frame.PrevCameraPosition;
        gather.PrevPreExposure      = frame.PrevPreExposure;
        // The screen leg (UE ScreenProbeTraceScreenTextures): the view's HZB and scene colour history, or the
        // system black texture with the leg off when the view has no screen trace.
        gather.HZB = screenTrace.HZB.IsValid() ? screenTrace.HZB : refs.System.Black;
        gather.SceneColorHistory =
             screenTrace.SceneHistory.IsValid() ? screenTrace.SceneHistory : refs.System.Black;
        gather.SceneColorHistoryReadable =
             screenTrace.HZB.IsValid() && screenTrace.SceneHistory.IsValid() && screenTrace.SceneHistoryReadable;
        // The temporal filter's reprojection and the view's own history (SceneViewState, never the gather's).
        gather.Velocity         = t.Velocity;
        gather.JitterNdc        = frame.JitterNdc;
        gather.PrevJitterNdc    = frame.PrevJitterNdc;
        gather.ViewHistoryValid = frame.HistoryValid();
        gather.Black            = refs.System.Black;
        // THE ONE DECISION POINT of the gather's denoiser: a temporal method that denoises the raw lighting
        // signals itself (DLSS Ray Reconstruction) takes the integrate's output instead. No method of this
        // renderer does, so the gather denoises.
        gather.Denoise = true;
        const auto gathered =
             m_ScreenProbeGather->AddPasses( graph, import, gather, m_ViewState.ScreenProbeGather() );
        if ( !gathered )
        {
            LOG_ERROR( "[SceneRenderer] ScreenProbes: {}", gathered.GetError() );
            return;
        }
        t.GIDiffuse  = gathered.GetValue().Diffuse;
        t.GISpecular = gathered.GetValue().Specular;
        t.GIBackface = gathered.GetValue().Backface;
        t.GIOccluded = gathered.GetValue().ShortRangeAO;
    }

    bool SceneRenderer::WorldLightGridRead() const
    {
        // The traced GI's hits lit in place, the traced reflections' (the surface cache brings those up too), and
        // the surface cache's card lighting.
        if ( RadianceCacheRuns() || SurfaceCacheRuns() )
            return true;
#if DESERT_DEV_INSTRUMENTS
        // Its readers: the world grid's own view, and the surface cache's direct lighting (every surface cache
        // view mode lights the cards).
        const DeferredDebugMode mode = m_DebugView.DeferredDebug;
        return m_RenderPath == Core::RenderPath::Deferred &&
               ( mode == DeferredDebugMode::LightGridWorld || mode == DeferredDebugMode::SurfaceCacheAtlas ||
                 mode == DeferredDebugMode::SurfaceCacheScene || mode == DeferredDebugMode::SurfaceCacheSceneLit );
#else
        return false;
#endif // DESERT_DEV_INSTRUMENTS
    }

    void SceneRenderer::AddFrameLightGridWorld( RDG::Builder& graph, FrameTextures& textures,
                                                const ViewFrame& frame )
    {
        auto* const deferred =
             dynamic_cast<System::DeferredLightingRenderer*>( m_RenderSystems["DeferredLightingSystem"].get() );
        ViewLightBuffers& lights = textures.Transients.Lights;
        if ( deferred == nullptr || !lights.Point.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] LightGridWorld: {}",
                       deferred == nullptr ? "no DeferredLightingSystem"
                                           : "the view's lights were not uploaded this frame" );
            return;
        }
        // The radiance cache's clipmaps around the camera (snapped to whole cells, so they move with it), and the
        // SAME point/spot buffers the frustum grid listed — one light list, two layouts.
        const LightGridParamsPayload grid = MakeLightGridWorldParams( frame.CameraPosition, m_RadianceCacheInputs,
                                                                      GetPointLights(), GetSpotLights() );
        System::DeferredLightingRenderer::UploadWorldGrid( graph, grid, lights );
        graph.AddPass(
             "LightGridWorld", RDG::PassFlags::Compute, [deferred, lights]( RDG::PassBuilder& pass )
             { deferred->DeclareGridBindings( pass, lights, LightGridLayout::WorldClipmaps ); },
             [deferred, cells = glm::uvec3( grid.Size )]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return deferred->RecordGrid( context, cells ); } );
    }

    void SceneRenderer::AddFrameComposite( RDG::Builder& graph, FrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& gbuffer, RDG::TextureRef ssr,
                                           const RDG::TextureRef             sunShadowMask,
                                           const System::MegaLightsLighting& megaLights,
                                           System::MeshRenderer* meshRenderer, const glm::vec4& lightDir,
                                           const glm::vec4& lightColor, const ViewFrame& frame )
    {
        // w: what u_SSR holds (DeferredLighting.shader REFLECTION_INPUT_*): 0 nothing, 1 the screen trace, 2 the
        // traced reflection, which the composite fades into the traced GI's rough specular past
        // MaxRoughnessToTrace (UE MixSpecularAndRoughReflections).
        constexpr float kReflectionInputNone = 0.0f, kReflectionInputScreen = 1.0f, kReflectionInputTraced = 2.0f;
        const float     reflectionInput = !ssr.IsValid()        ? kReflectionInputNone
                                          : ReflectionsTraced() ? kReflectionInputTraced
                                                                : kReflectionInputScreen;
        const glm::vec4 cameraPos( frame.CameraPosition, reflectionInput );
        // The Reflections view shows the traced reflection alone (DeferredLighting.shader, mode 18): a view whose
        // reflections are not traced has none to show, and the screen trace is no stand-in for it.
        if ( m_DebugView.DeferredDebug == DeferredDebugMode::Reflections )
        {
            if ( !ReflectionsTraced() )
            {
                LOG_ERROR( "[SceneRenderer] Reflections view: reflections are not traced (ReflectionsMethod {}); "
                           "the view shows SoftwareTraced / HardwareTraced only",
                           static_cast<int>( m_RenderMethods.Reflections.Effective ) );
            }
            else if ( !ssr.IsValid() )
            {
                LOG_ERROR(
                     "[SceneRenderer] Reflections view: the traced reflections were not produced this frame" );
            }
        }
        // LOAD/STORE on every scene-target attachment. The depth is declared written because the passes after
        // this one that are not graph nodes yet (forward meshes, glass, fog, clouds, overlays) begin their own
        // render passes on it in the depth-attachment layout, and DepthResolve left it a transfer destination.
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Composite" );
        if ( !targets )
            return;
        if ( gbuffer.size() < 4 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: Composite needs the 4 G-buffer colours, the graph has {}",
                       gbuffer.size() );
            return;
        }
        const RDG::ImportedFramebuffer& target = *targets;
        const std::vector<RDG::LoadOp>  loads  = textures.ColorLoads( *targets, RDG::LoadOp::Load() );

        // Every input by name; an input not produced this frame is the system texture neutral for it (UE
        // GSystemTextures), so the lighting is drawn in every mode (SSAO off, GI not traced, fewer cascades).
        const FrameGraphRefs            refs = textures.GraphRefs();
        System::DeferredCompositeInputs inputs;
        inputs.GBufferA        = gbuffer[0];
        inputs.GBufferB        = gbuffer[1];
        inputs.GBufferShadingWord = gbuffer[2];
        inputs.GBufferDepth       = textures.Depth( m_GBuffer, "GBuffer" );
        // The opaque depth is final here: the forward meshes and the glass added after this node march it for
        // their contact shadows (SceneViewInputs::SceneDepth) and read the hit texel's caster bit from the shading
        // word (SceneViewInputs::SceneShadingWord). They draw on the scene target, not the G-buffer.
        textures.Transients.SceneDepth       = inputs.GBufferDepth;
        textures.Transients.SceneShadingWord = inputs.GBufferShadingWord;
        inputs.GBufferEmissive = gbuffer[3];
        inputs.SSAO            = refs.Transients.SSAO.IsValid() ? refs.Transients.SSAO : refs.System.White;
        inputs.SSR             = ssr.IsValid() ? ssr : refs.System.Black;
        // The traced GI's final gather (screen probes or the irradiance field), or black (unread in the other
        // modes).
        const bool traced      = refs.Transients.GIDiffuse.IsValid();
        inputs.GIDiffuse       = traced ? refs.Transients.GIDiffuse : refs.System.Black;
        inputs.GISpecular      = traced ? refs.Transients.GISpecular : refs.System.Black;
        inputs.GIBackface      = traced ? refs.Transients.GIBackface : refs.System.Black;
        inputs.View            = SceneViewInputsOf( refs );
        // CPU state of this frame, gathered at graph build: the material is filled in the node's setup, where the
        // pass validation reads its route fill (lead decision B), never in the exec.
        DeferredShadowInput shadow;
        if ( SunShadowIsScreenMask() )
        {
            // AddFrameRayTracedSunShadow logged why a mask is missing; the sun is then unshadowed (System.White),
            // never shadowed by cascades no pass drew.
            inputs.SunShadowMask = sunShadowMask.IsValid() ? sunShadowMask : refs.System.White;
            shadow.Sun           = DeferredScreenShadowMask{ .Mask = inputs.SunShadowMask };
            // Shadows Stochastic: MegaLights' local lighting (AddFrameMegaLights logged why one is missing; a view
            // without local lights has none to add): black adds no local light, never the unshadowed grid loop.
            shadow.MegaLightsLocalLights = LocalLightsAreStochastic();
        }
        else if ( meshRenderer != nullptr )
            shadow.Sun = DeferredCascadeShadow{ .CascadeVP            = meshRenderer->GetCascadeViewProj(),
                                                .Count                = meshRenderer->GetValidCascadeCount(),
                                                .Enabled              = meshRenderer->AreShadowsEnabled(),
                                                .CascadeWorldPerTexel = meshRenderer->GetCascadeWorldPerTexel(),
                                                .CascadeDepthRange    = meshRenderer->GetCascadeDepthRange(),
                                                .SourceTanHalf        = meshRenderer->GetSunSourceTanHalf() };
        inputs.MegaLightsDiffuse  = megaLights.Diffuse.IsValid() ? megaLights.Diffuse : refs.System.Black;
        inputs.MegaLightsSpecular = megaLights.Specular.IsValid() ? megaLights.Specular : refs.System.Black;
        // The composite marches the G-buffer depth, so its view always has one (GetContactShadowSettings).
        shadow.Contact                       = GetContactShadowSettings( frame, true );
        shadow.Filter                        = GetShadowFilterSettings();
        const CloudShadowInput   cloudShadow = GetCloudShadowInput();
        DeferredEnvironmentInput environment;
        {
            auto* imageService = Runtime::ResourceRegistry::GetImageService();
            environment.Sky    = GetSkyLightState();
            if ( const auto& env = GetEnvironment(); env.has_value() )
            {
                environment.Look = env->Look;
                if ( env->IrradianceMap.IsValid() )
                    environment.Irradiance =
                         dynamic_cast<ImageCube*>( imageService->Resolve( env->IrradianceMap ) );
                if ( env->PreFilteredMap.IsValid() )
                    environment.Prefiltered =
                         dynamic_cast<ImageCube*>( imageService->Resolve( env->PreFilteredMap ) );
            }
            if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
                 brdf && brdf->GetImageHandle().IsValid() )
                environment.BrdfLut = dynamic_cast<Image2D*>( imageService->Resolve( brdf->GetImageHandle() ) );
        }
        // The composite's GI branch (DeferredLighting.shader u_Params.w): 0 none, 1 the screen-space gather inside
        // this pass, 2 the traced final gather's diffuse and specular in place of the sky's (SSAO occludes it), 3
        // the same gather occluded by its own short-range AO (the material AO alone left to apply). Spelled here,
        // not cast from the enum, whose numbering is the method ladder's (Common/Settings/RenderMethods.hpp).
        using Common::Scalability::GIMethod;
        const GIMethod gi            = m_RenderMethods.GlobalIllumination.Effective;
        const int             giMode        = gi == GIMethod::ScreenSpace            ? 1
                                              : traced && refs.Transients.GIOccluded ? 3
                                              : traced                               ? 2
                                                                                     : 0;
        const DeferredGIInput compositeGI{ .Mode      = giMode,
                                           .Intensity = gi == GIMethod::ScreenSpace ? m_GIIntensity : 0.0f,
                                           .ScreenSpaceSamples = m_ScreenSpaceGISamples,
                                           .PreExposure        = frame.PreExposure };

        auto* const deferred =
             dynamic_cast<System::DeferredLightingRenderer*>( m_RenderSystems["DeferredLightingSystem"].get() );
        // The lights and the light grid were made by AddFrameLightGrid, before any lit node.
        const ViewLightBuffers& lights = refs.Transients.Lights;
        if ( !lights.GridCells.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: Composite: the view's light grid was not built this frame" );
            return;
        }
        const auto pointCount = static_cast<uint32_t>( GetPointLights().PointLights.size() );
        const auto spotCount  = static_cast<uint32_t>( GetSpotLights().SpotLights.size() );

        graph.AddPass(
             "Deferred: Composite", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 deferred->FillMaterial( lightDir, lightColor, cameraPos, frame.InvJitteredViewProjection,
                                         static_cast<int>( m_DebugView.DeferredDebug ), pointCount, spotCount,
                                         shadow, compositeGI, m_EnableSSAO, cloudShadow, environment,
                                         frame.JitteredViewProjection );
                 deferred->DeclareCompositeBindings( pass, inputs, lights );
                 DeferredFrameNodes::LoadTarget( pass, target, loads );
             },
             [deferred]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return deferred->Record( context ); } );
    }

    bool SceneRenderer::GlobalDistanceFieldRead() const
    {
        if ( RadianceCacheRuns() && GlobalIllumination::TracerReadsGlobalDistanceField( m_GIPipeline ) )
            return true;
        // The surface cache's shadow rays off the view's maps.
        if ( SurfaceCacheRuns() && GlobalIllumination::TracerReadsGlobalDistanceField( SurfaceCacheTracer() ) )
            return true;
        // The traced reflections' own rays: SoftwareTraced walks the field whatever GI's method is.
        if ( ReflectionsTraced() && GlobalIllumination::TracerReadsGlobalDistanceField( ReflectionsTracer() ) )
            return true;
#if DESERT_DEV_INSTRUMENTS
        return m_RenderPath == Core::RenderPath::Deferred &&
               m_DebugView.DeferredDebug == DeferredDebugMode::GlobalDistanceField;
#else
        return false;
#endif // DESERT_DEV_INSTRUMENTS
    }

    void SceneRenderer::AddFrameGlobalDistanceField( RDG::Builder& graph, FrameTextures& textures,
                                                     const ViewFrame& frame )
    {
        const auto  it = m_RenderSystems.find( "MeshSystem" );
        auto* const meshRenderer =
             it != m_RenderSystems.end() ? UNIQUE_GET_AS( System::MeshRenderer, it->second ) : nullptr;
        if ( meshRenderer == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] Global Distance Field: no MeshSystem" );
            return;
        }
        if ( m_FrameDistanceFields == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] Global Distance Field: no scene's distance fields (outside BeginScene)" );
            return;
        }
        // The field this view reads: the one lent to it (ShareGlobalDistanceField: a reflection capture's face
        // views share their capture's), else its own, made on first use.
        DistanceField::GlobalDistanceField* globalField = m_SharedGlobalDistanceField;
        if ( globalField == nullptr )
        {
            if ( !m_OwnGlobalDistanceField )
                m_OwnGlobalDistanceField = std::make_unique<DistanceField::GlobalDistanceField>();
            globalField = m_OwnGlobalDistanceField.get();
        }
        if ( !globalField->IsInitialized() )
            if ( const auto initialized = globalField->Initialize(); !initialized )
            {
                LOG_ERROR( "[SceneRenderer] Global Distance Field: {}", initialized.GetError() );
                return;
            }
        if ( !m_GlobalDistanceFieldView )
        {
            auto view = std::make_unique<DistanceField::GlobalDistanceFieldView>();
            if ( const auto initialized = view->Initialize(); !initialized )
            {
                LOG_ERROR( "[SceneRenderer] Global Distance Field: {}", initialized.GetError() );
                return;
            }
            m_GlobalDistanceFieldView = std::move( view );
        }

        // Every static and instanced mesh of the frame that carries a field, whole - the field holds what is
        // off-screen too (a trace leaves the frustum).
        std::vector<DistanceField::GlobalDistanceFieldObject> objects;
        // The surface cache's instance-table row of each placement (mesh, world), which the field's object grid
        // lists (UE: the object's GPUSceneInstanceIndex). The cache updates after the field in the frame, so these
        // are the last update's rows; a row is stable while its instance lives, and a new one recomposites the
        // object the frame it appears (GlobalDistanceField: the row is part of the object's identity).
        const std::unordered_map<uint64_t, uint32_t> rows = m_SurfaceCache
                                                                 ? m_SurfaceCache->GetScene().RowsByPlacement()
                                                                 : std::unordered_map<uint64_t, uint32_t>{};
        // One object per volume of the mesh's field, each owning the whole field (an aliasing pointer: the
        // volume lives as long as the field it belongs to). A mesh whose field is still being derived (the
        // asynchronous queue) is not in it yet.
        // Each object also carries where the view drew it last frame (UE: the instance's PrevLocalToWorld, which
        // a near-field hit's velocity reads): a static draw's motion row (BuildObjectMotions ran before any pass);
        // an instanced batch's instances have no motion record of their own and did not move.
        const auto place = [&objects, &rows]( const void*                                                 mesh,
                                              const std::shared_ptr<const Assets::MeshDistanceFieldData>& field,
                                              const glm::mat4& world, const glm::mat4& prevWorld,
                                              const bool mostlyTwoSided )
        {
            const auto     it  = rows.find( SurfaceCache::SurfaceCacheScene::PlacementKey( mesh, world ) );
            const uint32_t row = it != rows.end() ? it->second : DistanceField::kNoObjectGridRow;
            for ( const Assets::MeshDistanceFieldVolume& volume : field->Volumes )
                objects.push_back( DistanceField::GlobalDistanceFieldObject{
                     .Field          = std::shared_ptr<const Assets::MeshDistanceFieldVolume>( field, &volume ),
                     .World          = world,
                     .PrevWorld      = prevWorld,
                     .Row            = row,
                     .MostlyTwoSided = mostlyTwoSided } );
        };
        const auto& motionRows = meshRenderer->GetObjectMotionRows().Rows;
        for ( const auto& draw : meshRenderer->GetStaticQueue() )
            if ( draw.Mesh != nullptr )
                if ( const auto field = draw.Mesh->GetDistanceField() )
                    place( draw.Mesh, field, draw.Transform, motionRows[draw.MotionRow].PrevWorld,
                           draw.Mesh->IsDistanceFieldMostlyTwoSided() );
        for ( const auto& ism : meshRenderer->GetInstancedQueue() )
            if ( ism.Mesh != nullptr && ism.Transforms )
                if ( const auto field = ism.Mesh->GetDistanceField() )
                    for ( const glm::mat4& world : *ism.Transforms )
                        place( ism.Mesh, field, world, world, ism.Mesh->IsDistanceFieldMostlyTwoSided() );

        const auto import = [&textures]( const std::shared_ptr<Image>& image, const std::string_view name )
        { return textures.Import( image, name ); };
        // The view's camera (ViewFrame; SceneRenderer.hpp: a pass that reads a camera position takes the
        // ViewFrame) - not m_SceneInfo.ActiveCamera's, which would frame the field from another camera than the
        // frame's. The field is brought up to it (once per frame, whichever view of the field asks first), then
        // this view's near-field froxel grid (the GI tracer's first metres) is culled over it.
        const DistanceField::GlobalDistanceFieldView::NearFieldView nearView{
             .ViewProjection    = frame.ViewProjection,
             .InvViewProjection = frame.InvViewProjection,
             .Projection        = frame.Projection,
             .CameraPosition    = frame.CameraPosition,
             .Forward           = -glm::normalize( glm::vec3( frame.InvView[2] ) ),
             .Extent            = glm::uvec2( frame.Split.Render.Width, frame.Split.Render.Height ) };
        const auto field = m_GlobalDistanceFieldView->AddFramePasses(
             graph, *globalField, import, objects, *m_FrameDistanceFields,
             Engine::FrameManager::GetInstance().GetAbsoluteFrameCount(), m_DistanceFieldPoolMiB, nearView );
        if ( !field )
        {
            LOG_ERROR( "[SceneRenderer] Global Distance Field: {}", field.GetError() );
            return;
        }
        const DistanceField::GlobalDistanceField::FrameField& fieldFrame = field.GetValue();
        textures.Transients.GlobalDistanceField           = fieldFrame.Clipmaps;
        textures.Transients.GlobalDistanceFieldLevels     = fieldFrame.Levels;
        textures.Transients.MeshDistanceFields            = fieldFrame.MeshFields;
        textures.Transients.GlobalDistanceFieldObjectGrid = fieldFrame.ObjectGrid;
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameVelocityView( RDG::Builder& graph, FrameTextures& textures,
                                              const RDG::TextureRef target )
    {
        // Like the G-buffer view modes, Deferred only: on Forward the mode draws the lit frame.
        if ( m_DebugView.DeferredDebug != DeferredDebugMode::Velocity ||
             m_RenderPath != Core::RenderPath::Deferred )
            return;
        const RDG::TextureRef velocity = textures.Transients.Velocity;
        const auto            it       = m_RenderSystems.find( "VelocityViewSystem" );
        auto* const           view =
             it != m_RenderSystems.end() ? UNIQUE_GET_AS( System::VelocityViewRenderer, it->second ) : nullptr;
        if ( view == nullptr || !target.IsValid() || !velocity.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Velocity not drawn: {}",
                       view == nullptr ? "no VelocityViewSystem" : "the frame has no velocity or no post input" );
            return;
        }
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Velocity: {}", desc.GetError() );
            return;
        }
        if ( const auto prepared = view->Prepare( desc.GetValue().Format, desc.GetValue().Samples ); !prepared )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Velocity: {}", prepared.GetError() );
            return;
        }
        graph.AddPass(
             "Debug: Velocity", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 view->DeclareBindings( pass, velocity );
                 pass.ColorTarget( 0, target, EngineClearColor() );
             },
             [view]( RDG::PassContext& context ) -> Common::BoolResultStr { return view->Record( context ); } );
    }

    void SceneRenderer::AddFrameLightGridWorldView( RDG::Builder& graph, FrameTextures& textures,
                                                    const RDG::TextureRef target, const ViewFrame& frame )
    {
        if ( m_DebugView.DeferredDebug != DeferredDebugMode::LightGridWorld ||
             m_RenderPath != Core::RenderPath::Deferred )
            return;
        auto* const deferred =
             dynamic_cast<System::DeferredLightingRenderer*>( m_RenderSystems["DeferredLightingSystem"].get() );
        const ViewLightBuffers& lights = textures.Transients.Lights;
        if ( deferred == nullptr || !target.IsValid() || !lights.WorldGridCells.IsValid() || !m_GBuffer )
        {
            LOG_ERROR( "[SceneRenderer] Debug: LightGridWorld not drawn: {}",
                       deferred == nullptr ? "no DeferredLightingSystem"
                                           : "the frame has no post input, no G-buffer or no world light grid" );
            return;
        }
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Debug: LightGridWorld: {}", desc.GetError() );
            return;
        }
        if ( const auto prepared = deferred->PrepareWorldView( desc.GetValue().Format, desc.GetValue().Samples );
             !prepared )
        {
            LOG_ERROR( "[SceneRenderer] Debug: LightGridWorld: {}", prepared.GetError() );
            return;
        }
        const RDG::TextureRef depth = textures.Depth( m_GBuffer, "GBuffer" );
        const RDG::BufferRef  view =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( glm::mat4 ) }, "Debug.LightGridWorldView" );
        graph.QueueBufferUpload(
             view, std::as_bytes( std::span<const glm::mat4>( &frame.InvJitteredViewProjection, 1 ) ) );
        graph.AddPass(
             "Debug: LightGridWorld", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 deferred->DeclareWorldViewBindings( pass, depth, view, lights );
                 pass.ColorTarget( 0, target, EngineClearColor() );
             },
             [deferred]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return deferred->RecordWorldView( context ); } );
    }

    void SceneRenderer::AddFrameGlobalDistanceFieldView( RDG::Builder& graph, FrameTextures& textures,
                                                         const ViewFrame& frame, const RDG::TextureRef target )
    {
        // Like the G-buffer view modes, Deferred only: on Forward the mode draws the lit frame.
        if ( m_DebugView.DeferredDebug != DeferredDebugMode::GlobalDistanceField ||
             m_RenderPath != Core::RenderPath::Deferred )
            return;
        const FrameTransients& t = textures.Transients;
        if ( !m_GlobalDistanceFieldView || !t.GlobalDistanceField[0].IsValid() || !target.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Global Distance Field not drawn: {}",
                       !target.IsValid() ? "the frame has no post input"
                                         : "the field was not brought up this frame" );
            return;
        }
        DistanceField::GlobalDistanceField::FrameField field;
        field.Clipmaps = t.GlobalDistanceField;
        field.Levels   = t.GlobalDistanceFieldLevels;
        // The view's matrices, unjittered (ViewFrame, as every overlay reads them).
        if ( const auto drawn = m_GlobalDistanceFieldView->AddDebugViewPass( graph, field, target, EngineClearColor(),
                                                                         frame.InvViewProjection );
             !drawn )
            LOG_ERROR( "[SceneRenderer] Debug: Global Distance Field: {}", drawn.GetError() );
    }
#endif // DESERT_DEV_INSTRUMENTS

    RDG::TextureRef SceneRenderer::AddFrameSceneCopy( RDG::Builder& graph, FrameTextures& textures,
                                                      const std::vector<RDG::TextureRef>& sceneColor,
                                                      System::CopyRenderer* copy, RDG::TextureRef into,
                                                      std::function<void()> recorded )
    {
        if ( copy == nullptr || sceneColor.empty() )
            return {};
        // The snapshot lives within this graph (UE: a transient from the scene colour's desc): the scene
        // colour's size, the copy format, one mip, one layer, one sample (the scene colour ref is the
        // attachment 0 the copy always read).
        const auto desc = graph.GetTextureDesc( sceneColor.front() );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SceneCopy: {}", desc.GetError() );
            return {};
        }
        RDG::TextureDesc copyDesc          = desc.GetValue();
        copyDesc.Format                    = ViewTargetFormats::kSceneColorCopy;
        copyDesc.Mips                      = 1;
        copyDesc.Layers                    = 1;
        copyDesc.Samples                   = 1;
        const RDG::TextureRef sceneCopy    = into.IsValid() ? into : graph.CreateTexture( copyDesc, "SceneCopy" );
        const RDG::TextureRef source       = sceneColor.front();
        // -> Deferred: Glass, Water; and, as the view's scene colour history, next frame's screen traces
        // (Deferred: SSR's u_SceneColor, ScreenProbes: Trace's u_SceneColorHistory).
        textures.Transients.SceneColorCopy = sceneCopy;
        graph.AddPass(
             "Deferred: SceneCopy", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 // The copy samples attachment 0; the other scene-colour refs stay plain reads of the node.
                 copy->DeclareBindings( pass, source );
                 ReadAll( pass, { sceneColor.begin() + 1, sceneColor.end() }, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, sceneCopy, EngineClearColor() );
             },
             [copy, recorded = std::move( recorded )]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const Common::BoolResultStr result = copy->Record( context );
                 if ( result.IsSuccess() && recorded )
                     recorded();
                 return result;
             } );
        return sceneCopy;
    }

    RDG::TextureRef SceneRenderer::AddFrameHZB( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* hzb = UNIQUE_GET_AS( System::HZBRenderer, m_RenderSystems["HZBSystem"] );
        if ( hzb == nullptr )
            return {};
        const RDG::TextureRef depth = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            desc  = graph.GetTextureDesc( depth );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] HZB: {}", desc.GetError() );
            return {};
        }
        return hzb->AddPasses( graph, depth, desc.GetValue().Size.Width, desc.GetValue().Size.Height );
    }

    SceneRenderer::FrameScreenTrace
    SceneRenderer::AddFrameScreenTrace( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* history =
             UNIQUE_GET_AS( System::SceneColorHistoryRenderer, m_RenderSystems["SceneColorHistorySystem"] );
        if ( history == nullptr || !history->Prepare() )
            return {};
        const RDG::TextureRef hzb = AddFrameHZB( graph, textures );
        if ( !hzb.IsValid() )
            return {};
        // Build time, after Prepare: the exec runs after the frame was built and must not consult a stamp a later
        // frame may have written.
        return FrameScreenTrace{ .HZB          = hzb,
                                 .SceneHistory = textures.Import( history->GetImage(), "SceneColorHistory" ),
                                 .SceneHistoryReadable = history->ReadableIn( frame ) };
    }

    RDG::BufferRef SceneRenderer::AddFrameGIRecord( RDG::Builder& graph, const ViewFrame& frame,
                                                    const glm::vec4& lightDir, const glm::vec4& lightColor,
                                                    const glm::uvec2 extent )
    {
        const GlobalIllumination::GIFramePayload payload =
             MakeFrameGIPayload( frame, lightDir, lightColor, extent );
        const RDG::BufferRef record = graph.CreateBuffer( RDG::BufferDesc{ sizeof( payload ) }, "GI.Frame" );
        graph.QueueBufferUpload( record, std::as_bytes( std::span( &payload, 1 ) ) );
        return record;
    }

    GlobalIllumination::GIFramePayload SceneRenderer::MakeFrameGIPayload( const ViewFrame& frame,
                                                                          const glm::vec4& lightDir,
                                                                          const glm::vec4& lightColor,
                                                                          const glm::uvec2 extent )
    {
        GlobalIllumination::GIFramePayload payload;
        payload.InvViewProjection = frame.InvJitteredViewProjection;
        payload.ViewProjection    = glm::inverse( frame.InvJitteredViewProjection );
        payload.Camera            = glm::vec4( frame.CameraPosition, 1.0f );
        const bool hasSun         = !m_DirectionLights.DirectionLights.empty();
        payload.SunTravel         = glm::vec4( glm::normalize( glm::vec3( lightDir ) ), hasSun ? 1.0f : 0.0f );
        payload.SunRadiance       = glm::vec4( glm::vec3( lightColor ) * lightColor.w, 0.0f );
        if ( const auto& env = GetEnvironment(); env.has_value() )
        {
            const SkyLookGPU look = ToGPU( env->Look );
            payload.SkyYawCosSin  = look.YawCosSin;
            payload.SkyGain       = look.Gain;
        }
        payload.Screen = glm::vec4( glm::vec2( extent ), 1.0f / glm::vec2( extent ) );
        return payload;
    }

    uint64_t SceneRenderer::PathTracingSceneHash( const GlobalIllumination::GIFramePayload& giFrame,
                                                  const uint64_t environment, const uint64_t emissive,
                                                  const CloudShadowUniforms& cloudShadow ) const
    {
        System::PathTracer::SceneHasher         hash;
        const RayTracing::RayTracingSceneFrame& rt = m_RayTracingScene.GetFrame();
        hash.Value( rt.InstanceHash );
        hash.Values( std::span<const ShaderProtocols::PointLightPayload>( GetPointLights().PointLights ) );
        hash.Values( std::span<const ShaderProtocols::SpotLightPayload>( GetSpotLights().SpotLights ) );
        // The record's sun, sky look and extent (its camera rows restart through the ViewProjection test as well).
        hash.Value( giFrame );
        hash.Value( environment );
        for ( const std::string& name : m_ReflectionHitGroups.Templates )
            hash.Bytes( std::as_bytes( std::span<const char>( name.data(), name.size() ) ) );
        // RayTracingHitRecord has padding after HitGroup: field by field.
        for ( const RayTracing::RayTracingHitRecord& record : m_ReflectionHitGroups.Records )
        {
            hash.Value( record.HitGroup );
            hash.Value( record.Data.VertexAddress );
            hash.Value( record.Data.IndexAddress );
            hash.Value( record.Data.VertexStride );
            hash.Value( record.Data.FirstVertex );
            hash.Value( record.Data.FirstIndex );
            hash.Value( record.Data.NormalOffset );
            hash.Value( record.Data.UvOffset );
            hash.Value( record.Data.MaterialRow );
        }
        // The rows' values: a material parameter edited in place moves them.
        hash.Values( std::span<const glm::vec4>( m_ReflectionHitGroups.Rows ) );
        hash.Value( Runtime::ResourceRegistry::GetMaterialService()->GetInvalidationVersion() );
        hash.Value( emissive );
        // The cloud shadow's placement and strength (the sun's transmittance through the clouds).
        hash.Value( cloudShadow );
        return hash.Get();
    }

    Common::ResultStr<RayTracing::EmissiveLightsFrame> SceneRenderer::AddFrameEmissiveLights( RDG::Builder& graph )
    {
        using Result = RayTracing::EmissiveLightsFrame;
        if ( !m_RayTracingScene.GetFrame().Built )
            return Common::MakeError<Result>(
                 "the emissive lights are built from the view's TLAS, which this frame "
                 "did not build" );
        // The estimate the tree's powers are made of: the segment's emissive colour x intensity in the row the
        // raster draws it with. A landscape tile's hit group emits nothing (TerrainRayHit), a whole-entity
        // override has no surface row.
        const auto radianceOf = []( const RayTracing::RayTracingSegmentRecord& segment ) -> glm::vec3
        {
            if ( segment.LandscapeTile != RayTracing::kNoLandscapeTile )
                return glm::vec3( 0.0f );
            const auto* material = dynamic_cast<const DataDrivenMaterial*>( segment.SurfaceMaterial );
            if ( material == nullptr )
                return glm::vec3( 0.0f );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): EffectiveRow reads the instance's overrides
            const Core::Formats::MaterialParamRow row = System::MeshRendererDetail::EffectiveRow(
                 material, const_cast<MaterialInstance*>( segment.Instance ) );
            const Core::Formats::MaterialLayout& layout = material->GetMaterialLayout();
            const std::optional<glm::vec4>       color =
                 MaterialBinder::ReadRowParam( layout, row, RayTracing::kSurfaceEmissiveColorParam );
            const std::optional<glm::vec4> intensity =
                 MaterialBinder::ReadRowParam( layout, row, RayTracing::kSurfaceEmissiveIntensityParam );
            if ( !color || !intensity )
                return glm::vec3( 0.0f );
            return glm::vec3( *color ) * intensity->x;
        };
        auto list = RayTracing::BuildEmissiveTriangleList( m_RayTracingScene.GetInstances().Instances,
                                                           m_RayTracingScene.GetPrimitives(),
                                                           m_RayTracingScene.GetSegmentRecords(), radianceOf );
        if ( !list )
            return Common::MakeError<Result>( list.GetError() );
        return m_EmissiveLights.AddPasses( graph, list.GetValue() );
    }

    Common::BoolResultStr SceneRenderer::AddFramePathTracing( RDG::Builder& graph, FrameTextures& textures,
                                                              const ViewFrame& frame )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* const pathTracer = UNIQUE_GET_AS( System::PathTracer, m_RenderSystems["PathTracerSystem"] );
        if ( pathTracer == nullptr )
            return Common::MakeError( "the view has no path tracer (EnsurePathTracerResources)" );
        const RayTracing::RayTracingSceneFrame& rt = m_RayTracingScene.GetFrame();
        if ( !rt.Built )
            return Common::MakeError( "the path tracer traces the view's TLAS, which this frame did not build" );
        if ( const Common::BoolResultStr built = BuildReflectionHitGroups(); !built )
            return built;
        if ( const Common::BoolResultStr made = pathTracer->EnsurePipeline( m_ReflectionHitGroups.Templates );
             !made )
            return made;

        const RDG::TextureRef sceneColor =
             textures
                  .Refs( { { m_TargetFramebuffer ? m_TargetFramebuffer->GetColorAttachmentImage( 0 ) : nullptr,
                             "SceneColor" } } )
                  .front();
        const auto sceneDesc = graph.GetTextureDesc( sceneColor );
        if ( !sceneDesc )
            return Common::MakeError( sceneDesc.GetError() );
        const glm::uvec2 extent( sceneDesc.GetValue().Size.Width, sceneDesc.GetValue().Size.Height );
        if ( extent.y == 0u )
            return Common::MakeError( "the scene colour has no pixels" );

        const ShaderProtocols::Camera cameraBlock = ShaderProtocols::MakeCameraUB( frame );
        const RDG::BufferRef          camera =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( cameraBlock ) }, "PathTracing.CameraUB" );
        graph.QueueBufferUpload( camera,
                                 std::as_bytes( std::span<const ShaderProtocols::Camera>( &cameraBlock, 1 ) ) );

        const FrameSun                           sun = GetFrameSun();
        const GlobalIllumination::GIFramePayload giFrame =
             MakeFrameGIPayload( frame, sun.Direction, sun.ColorIntensity, extent );
        const RDG::BufferRef giRecord = graph.CreateBuffer( RDG::BufferDesc{ sizeof( giFrame ) }, "GI.Frame" );
        graph.QueueBufferUpload( giRecord, std::as_bytes( std::span( &giFrame, 1 ) ) );

        System::PathTracer::FrameInputs inputs;
        inputs.Tlas            = rt.Tlas;
        inputs.Camera          = camera;
        inputs.GIFrame         = giRecord;
        inputs.PointLights     = textures.Transients.Lights.Point;
        inputs.SpotLights      = textures.Transients.Lights.Spot;
        inputs.PointLightCount = static_cast<uint32_t>( GetPointLights().PointLights.size() );
        inputs.SpotLightCount  = static_cast<uint32_t>( GetSpotLights().SpotLights.size() );
        inputs.EnvSpecular     = SceneViewInputsOf( textures.GraphRefs() ).EnvSpecular;
        // The sun's way through the cloud layer: the map the lit passes read (Common/CloudShadowReceiver.glslh).
        const CloudShadowUniforms cloudShadow = CloudShadowPackUniforms( GetCloudShadowInput() );
        inputs.CloudShadowMap                 = SceneViewInputsOf( textures.GraphRefs() ).CloudShadowMap;
        inputs.CloudShadow =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( cloudShadow ) }, "PathTracing.CloudShadowUB" );
        graph.QueueBufferUpload( inputs.CloudShadow,
                                 std::as_bytes( std::span<const CloudShadowUniforms>( &cloudShadow, 1 ) ) );
        inputs.HitGroups       = &m_ReflectionHitGroups;
        auto emissive          = AddFrameEmissiveLights( graph );
        if ( !emissive )
            return Common::MakeError( emissive.GetError() );
        inputs.Emissive = emissive.GetValue();
        // The sun's disc (UE: the directional light's SourceAngle is its full angular diameter).
        inputs.SunSinHalfAngle = std::sin( glm::radians( m_SunSourceAngleDegrees ) * 0.5f );
        // The angle one pixel subtends at the centre of the view: the camera ray's cone (ray cone LOD).
        inputs.PixelSpread     = std::atan( 2.0f / ( frame.Projection[1][1] * static_cast<float>( extent.y ) ) );
        inputs.SceneHash =
             PathTracingSceneHash( giFrame, GetEnvironmentIdentity(), inputs.Emissive.Hash, cloudShadow );
        inputs.DynamicGeometry = rt.DynamicGeometry;
        return pathTracer->AddPasses( graph, sceneColor, frame, inputs, m_PathTracingSettings );
    }

    bool SceneRenderer::ReflectionsTraced() const
    {
        const Common::Scalability::ReflectionMethod method = m_RenderMethods.Reflections.Effective;
        return m_RenderPath == Core::RenderPath::Deferred && m_ViewProfile.ScreenSpaceReflections &&
               ( method == Common::Scalability::ReflectionMethod::SoftwareTraced ||
                 method == Common::Scalability::ReflectionMethod::HardwareTraced );
    }

    GlobalIllumination::Pipeline SceneRenderer::ReflectionsTracer() const
    {
        GlobalIllumination::Pipeline pipeline;
        pipeline.Traced = true;
        // Hit-lit reflections light each hit in place from its own material (the closest-hit payload); the
        // others read the surface cache's cards.
        pipeline.Hits = ReflectionsHitLit() ? GlobalIllumination::HitLighting::InPlace
                                            : GlobalIllumination::HitLighting::SurfaceCache;
        pipeline.Tracer =
             m_RenderMethods.Reflections.Effective == Common::Scalability::ReflectionMethod::HardwareTraced
                  ? GlobalIllumination::Tracer::HardwareRayQuery
                  : GlobalIllumination::Tracer::SoftwareDistanceField;
        return pipeline;
    }

    bool SceneRenderer::ReflectionsHitLit() const
    {
        using namespace Common::Scalability;
        return m_RenderMethods.Reflections.Effective == ReflectionMethod::HardwareTraced &&
               QualityState::Resolved().As<int>( Parameter::ReflectionsHitLighting ) != 0 &&
               CapabilityCatalog::Offers( QualityState::Catalog().RayTracingModes,
                                          RayTracingMode::RayTracingPipeline );
    }

    Common::BoolResultStr SceneRenderer::BuildReflectionHitGroups()
    {
        auto*      materials       = Runtime::ResourceRegistry::GetMaterialService();
        auto*      shaders         = Runtime::ResourceRegistry::GetShaderService();
        const auto defaultTemplate = materials->DefaultSurfaceTemplate();
        if ( !defaultTemplate )
            return Common::MakeError( defaultTemplate.GetError() );
        // The default surface's row: a material of its (Static x GBuffer) cell - the same MaterialParams as its
        // hit cell, with the template's default textures in their slots - made once (UE: the default material's
        // hit group shades what has no ray tracing material of its own).
        const auto defaultCell = materials->DefaultSurfaceShader( MeshVertexPath::Static, MeshPass::GBuffer );
        if ( !defaultCell )
            return Common::MakeError( defaultCell.GetError() );
        if ( !m_ReflectionDefaultSurface || m_ReflectionDefaultSurface->GetShaderName() != defaultCell.GetValue() )
            m_ReflectionDefaultSurface = std::make_shared<DataDrivenMaterial>( defaultCell.GetValue() );
        const auto hasHitGroup = [shaders]( const std::string& name )
        {
            const auto hit = shaders->GetByName( std::format( "{}/RayTracing.Hit", name ) );
            return hit && hit->IsCompiled();
        };
        const RayTracing::RayTracingSegmentSurface defaultSurface{ defaultTemplate.GetValue(),
                                                                   m_ReflectionDefaultSurface->GetParamRow() };
        // A landscape tile's segment is shaded by the landscape's own hit group from the raster's inputs (UE: the
        // landscape material's hit shaders); its row is the tile's, by the record's LandscapeTile.
        const std::span<const RayTracing::LandscapeRayTracingMaterial> landscape =
             m_LandscapeRayTracing.GetFrame().Materials;
        const auto surfaceOf = [landscape]( const RayTracing::RayTracingSegmentRecord& segment )
        {
            RayTracing::RayTracingSegmentSurface surface;
            if ( segment.LandscapeTile != RayTracing::kNoLandscapeTile )
            {
                if ( segment.LandscapeTile < landscape.size() )
                {
                    surface.Template = std::string( RayTracing::kLandscapeRayHitTemplate );
                    surface.Row      = RayTracing::LandscapeRayHitRow( landscape[segment.LandscapeTile] );
                }
                return surface; // past the tile list: a broken invariant, the default surface, counted
            }
            // A generic record with its own hit group (3D text: TextRayHit) is shaded by it from its own row (UE:
            // the proxy's ray tracing material); a template that did not compile takes the default surface,
            // counted.
            if ( segment.ProgramHit != nullptr )
            {
                surface.Template = segment.ProgramHit->Template;
                surface.Row      = segment.ProgramHit->Row;
                return surface;
            }
            const auto* material = dynamic_cast<const DataDrivenMaterial*>( segment.SurfaceMaterial );
            if ( material == nullptr )
                return surface; // a whole-entity override: the default surface, counted
            const std::string& cell  = material->GetShaderName();
            const auto         slash = cell.find( '/' );
            surface.Template         = slash == std::string::npos ? cell : cell.substr( 0, slash );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): EffectiveRow reads the instance's overrides
            surface.Row = System::MeshRendererDetail::EffectiveRow(
                 material, const_cast<MaterialInstance*>( segment.Instance ) );
            return surface;
        };
        auto table = RayTracing::BuildRayTracingHitGroupTable( m_RayTracingScene.GetGeometryRecords(),
                                                               m_RayTracingScene.GetSegmentRecords(), surfaceOf,
                                                               defaultSurface, hasHitGroup );
        if ( !table )
            return Common::MakeError( table.GetError() );
        m_ReflectionHitGroups = std::move( table.GetValue() );
        for ( const RayTracing::RayTracingHitGroupFallback& fallback :
              m_ReflectionHitGroups.TemplatesWithoutHitGroup )
        {
            if ( std::ranges::find( m_ReportedHitGroupFallbacks, fallback.Template ) !=
                 m_ReportedHitGroupFallbacks.end() )
                continue;
            m_ReportedHitGroupFallbacks.push_back( fallback.Template );
            LOG_WARN(
                 "[SceneRenderer] Reflections hit lighting: '{}' has no compiled hit group ('{}/RayTracing.Hit' "
                 "refused or failed: DShaderParser SurfaceTemplateInfo::RayHitRefusal, or the shader log) - its "
                 "{} segments this frame shade with the default surface '{}'",
                 fallback.Template, fallback.Template, fallback.Segments, defaultTemplate.GetValue() );
        }
        return BOOLSUCCESS;
    }

    std::optional<System::SSRRenderer::TracedReads>
    SceneRenderer::MakeTracedReflectionReads( RDG::Builder& graph, FrameTextures& textures,
                                              System::SSRRenderer&                ssr,
                                              const std::vector<RDG::TextureRef>& gbuffer, const ViewFrame& frame )
    {
        FrameTransients& t = textures.Transients;
        const auto refuse  = [&]( const std::string_view why ) -> std::optional<System::SSRRenderer::TracedReads>
        {
            LOG_ERROR( "[SceneRenderer] Reflections ({}): {} - the frame reflects the screen alone",
                       Common::Scalability::kReflectionMethodNames[static_cast<std::size_t>(
                            m_RenderMethods.Reflections.Effective )],
                       why );
            return std::nullopt;
        };
        if ( gbuffer.size() < 4 )
            return refuse( "the G-buffer has fewer than 4 colours" );
        if ( !t.Lights.WorldGridCells.IsValid() )
            return refuse( "the world light grid was not built" );
        const GlobalIllumination::Pipeline tracer = ReflectionsTracer();
        {
            using namespace Common::Scalability;
            if ( m_RenderMethods.Reflections.Effective == ReflectionMethod::HardwareTraced &&
                 QualityState::Resolved().As<int>( Parameter::ReflectionsHitLighting ) != 0 &&
                 !ReflectionsHitLit() && !m_ReportedHitLightingDevice )
            {
                m_ReportedHitLightingDevice = true;
                LOG_WARN( "[SceneRenderer] Reflections.HitLighting is on, but the device offers no ray tracing "
                          "pipeline: the hardware-traced reflections read the surface cache" );
            }
        }
        const bool hitLit = ReflectionsHitLit();
        if ( const auto made = ssr.EnsureTracedProgram( tracer, hitLit ); !made )
        {
            // The traced permutation serves the method of its tracer alone.
            RefuseProgramMethods(
                 std::span( kTracedReflectionMethods )
                      .subspan( tracer.Tracer == GlobalIllumination::Tracer::HardwareRayQuery ? 1 : 0, 1 ),
                 TracedReflectionProgramName( tracer, hitLit ), made.GetError() );
            return refuse( made.GetError() );
        }
        if ( hitLit )
        {
            if ( !m_RayTracingScene.GetFrame().Built )
                return refuse( "hit lighting traces the view's TLAS, which this frame did not build" );
            if ( const auto built = BuildReflectionHitGroups(); !built )
                return refuse( built.GetError() );
            if ( const auto made = ssr.EnsureHitLightingPipeline( m_ReflectionHitGroups.Templates ); !made )
            {
                RefuseProgramMethods( std::span( kTracedReflectionMethods ).subspan( 1, 1 ), "SSRHitLighting",
                                      made.GetError() );
                return refuse( made.GetError() );
            }
        }

        System::SSRRenderer::TracedReads reads;
        reads.Tracer = MakeTracerFrame( graph, textures, tracer );
        if ( const std::string_view missing = GlobalIllumination::TracerFrameMissing( reads.Tracer );
             !missing.empty() )
            return refuse( missing );
        if ( tracer.Hits == GlobalIllumination::HitLighting::SurfaceCache )
        {
            if ( !m_SurfaceCacheFrame )
                return refuse( "the surface cache was not brought up this frame" );
            reads.SurfaceCache = m_SurfaceCacheFrame->Hits;
            if ( const std::string_view missing =
                      GlobalIllumination::SurfaceCacheFrameMissing( reads.SurfaceCache );
                 !missing.empty() )
                return refuse( missing );
        }

        const FrameGraphRefs refs = textures.GraphRefs();
        // The GI's record of this frame when it traced; otherwise this frame's own (the same payload).
        reads.GIFrame = m_TracedFrameShared.GIFrame;
        if ( !reads.GIFrame.IsValid() )
        {
            const auto desc = graph.GetTextureDesc( gbuffer[0] );
            if ( !desc )
                return refuse( desc.GetError() );
            const FrameSun sun = GetFrameSun();
            reads.GIFrame =
                 AddFrameGIRecord( graph, frame, sun.Direction, sun.ColorIntensity,
                                   glm::uvec2( desc.GetValue().Size.Width, desc.GetValue().Size.Height ) );
        }
        // The radiance cache's read: the GI's when it runs (an in-place lit hit takes its bounce), otherwise one
        // that covers nothing (no clipmap: RadianceCacheIrradianceAt answers "no coverage", the hit keeps its
        // direct light).
        if ( m_TracedFrameShared.RadianceCache )
            reads.RadianceCache = *m_TracedFrameShared.RadianceCache;
        else
        {
            const std::array<uint8_t, sizeof( RadianceCache::RadianceCacheParamsPayload )> zeros{};
            const RDG::BufferRef                                                           none =
                 graph.CreateBuffer( RDG::BufferDesc{ zeros.size() }, "Reflections.RadianceCacheNone" );
            graph.QueueBufferUpload( none, std::as_bytes( std::span( zeros ) ) );
            reads.RadianceCache = GlobalIllumination::RadianceCacheFrame{ .Params      = none,
                                                                          .Indirection = none,
                                                                          .Probes      = none,
                                                                          .Irradiance  = refs.System.Black,
                                                                          .Depth       = refs.System.Black,
                                                                          .Radiance    = refs.System.Black };
        }
        reads.PointLights     = t.Lights.Point;
        reads.SpotLights      = t.Lights.Spot;
        reads.WorldGridParams = t.Lights.WorldGridParams;
        reads.WorldGridCells  = t.Lights.WorldGridCells;
        reads.EnvSpecular     = SceneViewInputsOf( refs ).EnvSpecular;
        reads.GBufferA        = gbuffer[0];
        reads.GBufferEmissive = gbuffer[3];
        reads.HitGroups       = hitLit ? &m_ReflectionHitGroups : nullptr;
        return reads;
    }

    SceneRenderer::FrameSSR SceneRenderer::AddFrameSSR( RDG::Builder& graph, FrameTextures& textures,
                                                        System::SSRRenderer&                ssr,
                                                        const std::vector<RDG::TextureRef>& gbuffer,
                                                        const FrameScreenTrace&             screenTrace,
                                                        const ViewFrame&                    frame,
                                                        const FrontLayerSurface* frontLayer )
    {
        if ( gbuffer.size() < 3 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSR needs the G-buffer albedo and normal, the graph has {}",
                       gbuffer.size() );
            return {};
        }
        // Raw under an upscaler that denoises (DLSS Ray Reconstruction): one trace per pixel (DownsampleFactor 1),
        // no denoiser. The opaque reflections only: the front layer's are composited by the glass before the
        // upscaler, which takes the opaque surface's guides and hit distance.
        const bool raw = frontLayer == nullptr && m_TemporalUpscaler != nullptr &&
                         m_TemporalUpscaler->TakesRawLightingSignals();
        const uint32_t downsampleFactor = raw ? 1u : m_ViewProfile.ReflectionDownsampleFactor;
        if ( !System::SSRRenderer::IsSupportedDownsampleFactor( downsampleFactor ) )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: Reflections.DownsampleFactor is {}, the reflection trace knows 1 "
                       "and 2",
                       downsampleFactor );
            return {};
        }
        const std::optional<System::SSRRenderer::TraceTargets> traceTargets = ssr.Prepare( downsampleFactor );
        if ( !traceTargets || !screenTrace.HZB.IsValid() )
            return {};
        const RDG::TextureRef velocity = textures.Transients.Velocity;
        if ( !velocity.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSR needs the view's velocity (where a hit was last frame), the "
                       "frame has none" );
            return {};
        }
        const RDG::TextureRef hzb = screenTrace.HZB;
        // Transients of this graph, sized from this frame's view: written by the trace, read by the tiled resolve.
        const RDG::TextureRef trace    = graph.CreateTexture( traceTargets->Trace, "SSR.Trace" );
        const RDG::TextureRef traceRay = graph.CreateTexture( traceTargets->TraceRay, "SSR.TraceRay" );
        const RDG::TextureRef tiles    = graph.CreateTexture( traceTargets->TileMask, "SSR.TileMask" );
        // A lost trace reflects nothing; a lost tile mask marks no tile (RDG-FAULT1). A lost ray (a zero
        // direction) is skipped by the reconstruction's estimator.
        graph.SetFaultDefault( trace, RDG::FaultDefault::Black );
        graph.SetFaultDefault( traceRay, RDG::FaultDefault::Black );
        graph.SetFaultDefault( tiles, RDG::FaultDefault::Black );
        const RDG::TextureRef sceneHistory = screenTrace.SceneHistory;
        // Sampled by name in every SSR pass (no G-buffer image crosses into a pass exec).
        // World positions come from the G-buffer depth (Common/ReconstructPosition.glslh).
        const System::SSRRenderer::GBufferInputs inputs{ gbuffer[0], gbuffer[1],
                                                         textures.Depth( m_GBuffer, "GBuffer" ) };
        const RDG::BufferRef                     traceUniforms = System::SSRRenderer::UploadTraceUniforms(
             graph, frame.InvJitteredViewProjection, frame.JitteredViewProjection );
        // Build time, after Prepare: the exec runs after the frame was built and must not consult a stamp a later
        // frame may have written.
        const bool                         sceneHistoryReadable = screenTrace.SceneHistoryReadable;
        const uint32_t                     seed                 = ssr.NextTraceSeed();
        System::SSRRenderer::TraceSettings settings;
        settings.RaysPerPixel = m_SSRRaysPerPixel;
        settings.Iterations   = m_SSRMaxSteps;
        settings.MaxDistance  = m_SSRMaxDistance;
        settings.Intensity    = m_SSRIntensity;
        settings.Thickness    = Common::Units::Metres( 0.5f ); // literature: 0.5 m
        settings.DownsampleFactor = traceTargets->DownsampleFactor;
        settings.InvPrevPreExposure = 1.0f / std::max( frame.PrevPreExposure, 1e-30f );
        // SoftwareTraced / HardwareTraced: the rays the screen does not answer go on through the reflections'
        // tracer (ReflectionsTracer, TracedReads).
        std::optional<System::SSRRenderer::TracedReads> traced;
        if ( ReflectionsTraced() )
            traced = MakeTracedReflectionReads( graph, textures, ssr, gbuffer, frame );
        settings.Traced   = traced.has_value();
        const bool hitLit = traced && traced->HitGroups != nullptr;
        // The front layer's renderer starts its rays from the front layer's surface; every hit still reads the
        // opaque G-buffer, HZB and velocity above (Common/SSRTrace.glslh SSRSurfaceNormal).
        const System::SSRRenderer::FrontLayerInputs frontInputs =
             frontLayer != nullptr ? System::SSRRenderer::FrontLayerInputs{ .Normal = frontLayer->Normal,
                                                                            .Depth  = frontLayer->Depth }
                                   : System::SSRRenderer::FrontLayerInputs{};
        graph.AddPass(
             !traced  ? "Deferred: SSR"
             : hitLit ? "Deferred: Reflections (classify)"
             : traced->Tracer.Pipeline.Tracer == GlobalIllumination::Tracer::HardwareRayQuery
                  ? "Deferred: Reflections (HardwareTraced)"
                  : "Deferred: Reflections (SoftwareTraced)",
             RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 ssr.DeclareTraceBindings( pass, trace, traceRay, tiles, inputs, velocity, sceneHistory, hzb,
                                           traceUniforms, traced ? &*traced : nullptr,
                                           frontLayer != nullptr ? &frontInputs : nullptr );
                 // The G-buffer colours the trace does not sample stay declared as before.
                 ReadAll( pass, { gbuffer[0] }, RDG::Access::SampledCompute );
                 ReadAll( pass, { gbuffer.begin() + 2, gbuffer.end() }, RDG::Access::SampledCompute );
             },
             [ssrPtr = &ssr, cameraPos = glm::vec4( frame.CameraPosition, 1.0f ), settings, sceneHistoryReadable,
              seed]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return ssrPtr->RecordTrace( context, cameraPos, settings, sceneHistoryReadable, seed ); } );
        if ( hitLit )
        {
            // THE HIT-LIT TRACE (Reflections.HitLighting): every trace texel from the ray tracing pipeline, after
            // the classify wrote the tile mask - the screen walk, then a world ray whose hit runs its surface
            // template's closest-hit shader and is lit here in place (SSR.shader "HitLighting",
            // Common/SSRTrace.glslh).
            const ShaderProtocols::Camera cameraBlock = ShaderProtocols::MakeCameraUB( frame );
            const RDG::BufferRef          camera = graph.CreateBuffer( RDG::BufferDesc{ sizeof( cameraBlock ) },
                                                                       "Reflections.HitLighting.CameraUB" );
            graph.QueueBufferUpload(
                 camera, std::as_bytes( std::span<const ShaderProtocols::Camera>( &cameraBlock, 1 ) ) );
            graph.AddPass(
                 "Deferred: Reflections (HitLighting)", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     ssr.DeclareHitLightingBindings( pass, trace, traceRay, tiles, inputs, velocity, sceneHistory,
                                                     hzb, traceUniforms, camera, *traced,
                                                     frontLayer != nullptr ? &frontInputs : nullptr );
                     ReadAll( pass, { gbuffer[0] }, RDG::Access::SampledCompute );
                     ReadAll( pass, { gbuffer.begin() + 2, gbuffer.end() }, RDG::Access::SampledCompute );
                 },
                 [ssrPtr = &ssr, cameraPos = glm::vec4( frame.CameraPosition, 1.0f ), settings,
                  sceneHistoryReadable, seed,
                  hits = traced->HitGroups]( RDG::PassContext& context ) -> Common::BoolResultStr {
                     return ssrPtr->RecordHitLighting( context, cameraPos, settings, sceneHistoryReadable, seed,
                                                       *hits );
                 } );
        }
        if ( raw )
        {
            // The trace IS the per-pixel reflection (rgb radiance, a coverage: what the denoiser outputs), and
            // its ray is where each pixel's reflection came from - the upscaler's specular hit distance.
            ssr.SkipDenoise();
            textures.Transients.ReflectionRay = traceRay;
            return FrameSSR{ .Resolved = trace };
        }
        const RDG::TextureRef resolved =
             ssr.Denoise( graph,
                          System::ReflectionsDenoiserInputs{ .Trace                = trace,
                                                             .TraceRay             = traceRay,
                                                             .TileMask             = tiles,
                                                             .TraceSeed            = seed,
                                                             .ReconstructionRadius = m_ReflectionDenoiserRadius,
                                                             .DownsampleFactor     = traceTargets->DownsampleFactor },
                          // The reflecting surface's own normal, depth and motion: the front layer's for its
                          // renderer, whose history is its own.
                          System::DenoiserSceneTextures{
                               .GBufferNormal = frontLayer != nullptr ? frontLayer->Normal : gbuffer[1],
                               .GBufferDepth  = frontLayer != nullptr ? frontLayer->Depth : inputs[2],
                               .Velocity      = frontLayer != nullptr ? frontLayer->Velocity : velocity,
                                                         .Black         = textures.GraphRefs().System.Black },
                          frame );
        return FrameSSR{ .Resolved = resolved };
    }

    bool SceneRenderer::TranslucentFrontLayerReflections() const
    {
        using namespace Common::Scalability;
        return QualityState::Resolved().As<int>( Parameter::ReflectionsTranslucentFrontLayer ) != 0;
    }

    SceneRenderer::FrameFrontLayer SceneRenderer::AddFrameTranslucentFrontLayer(
         RDG::Builder& graph, FrameTextures& textures, System::MeshRenderer* meshRenderer,
         const std::vector<RDG::TextureRef>& gbuffer, const FrameScreenTrace& screenTrace, const ViewFrame& frame )
    {
        // UE RenderFrontLayerTranslucency: the pass and its resources exist only when a view draws into it.
        if ( meshRenderer == nullptr || gbuffer.size() < 2 || m_FrontLayerReflectionsFailed ||
             !meshRenderer->BuildFrontLayerDraws() )
            return {};
        if ( m_RenderSystems.find( "FrontLayerReflectionsSystem" ) == m_RenderSystems.end() )
        {
            Renderer::GetInstance().WaitDeviceIdle();
            RegisterSystem<System::SSRRenderer>( "FrontLayerReflectionsSystem", this, m_TargetFramebuffer );
            const auto& system = SP_CAST( System::SSRRenderer, m_RenderSystems["FrontLayerReflectionsSystem"] );
            system->TraceFrom( System::SSRRenderer::ReflectingSurface::TranslucentFrontLayer );
            if ( const Common::BoolResultStr initialized = system->Initialize(); !initialized )
            {
                LOG_ERROR( "[SceneRenderer] Reflections.TranslucentFrontLayer: the front layer's reflections are "
                           "unavailable ({}); glass reflects the sky alone",
                           initialized.GetError() );
                m_FrontLayerReflectionsFailed = true;
                return {};
            }
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* reflections = UNIQUE_GET_AS( System::SSRRenderer, m_RenderSystems["FrontLayerReflectionsSystem"] );

        // The front layer's targets: the G-buffer normal slot's, the view velocity's and the G-buffer depth's
        // descriptions (FrontLayerTargetLayout), transients of this frame.
        const RDG::TextureRef opaqueDepth = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            normalDesc  = graph.GetTextureDesc( gbuffer[1] );
        const auto            motionDesc  = graph.GetTextureDesc( textures.Transients.Velocity );
        const auto            depthDesc   = graph.GetTextureDesc( opaqueDepth );
        if ( !normalDesc || !motionDesc || !depthDesc )
        {
            LOG_ERROR( "[SceneRenderer] Translucency: Front Layer G-buffer: {}",
                       !normalDesc ? normalDesc.GetError()
                       : !motionDesc ? motionDesc.GetError()
                                     : depthDesc.GetError() );
            return {};
        }
        const FrontLayerSurface surface{ .Normal   = graph.CreateTexture( normalDesc.GetValue(), "FrontLayer.Normal" ),
                                         .Velocity = graph.CreateTexture( motionDesc.GetValue(), "FrontLayer.Velocity" ),
                                         .Depth    = graph.CreateTexture( depthDesc.GetValue(), "FrontLayer.Depth" ) };
        // Cleared: a zero normal (no surface) and the far depth, so the depth test keeps each pixel's nearest
        // translucent surface; whether that is in front of the opaque scene is the trace's question
        // (SSRSurfaceNormal) and the glass's (its own depth test).
        graph.AddPass(
             "Translucency: Front Layer G-buffer", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.ColorTarget( 0, surface.Normal, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                 pass.ColorTarget( 1, surface.Velocity, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                 pass.DepthTarget( surface.Depth, RDG::LoadOp::ClearDepth( Core::kDepthClear ) );
                 meshRenderer->DeclareFrontLayerDraws( pass );
             },
             [meshRenderer]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return meshRenderer->RenderFrontLayerManual( context ); } );

        const FrameSSR traced = AddFrameSSR( graph, textures, *reflections, gbuffer, screenTrace, frame, &surface );
        if ( !traced.Resolved.IsValid() )
            return {};
        return FrameFrontLayer{ .Reflection = traced.Resolved, .Depth = surface.Depth };
    }
} // namespace Desert::Graphic
