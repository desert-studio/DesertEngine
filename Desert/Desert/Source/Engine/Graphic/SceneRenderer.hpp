#pragma once

#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/RenderMethods.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Graphic/SunLightFx.hpp>
#include <Engine/Graphic/ViewMemory.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/ViewResources.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCullValidation.hpp>
#include <Engine/Graphic/View/DepthOfField.hpp>
#include <Engine/Graphic/View/MotionBlur.hpp>
#include <Engine/Graphic/View/SpatialUpscale.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>
#include <Engine/Core/ViewBudget.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Camera.hpp>

#include <Common/Core/Events/WindowEvents.hpp>
#include <Common/Core/Units.hpp>
#include <unordered_map>
#include <functional>
#include <optional>
#include <string>
#include <span>
#include <Common/Settings/MachineSettings.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Graphic/LightGrid.hpp>
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>

#include "Systems/Scene/Mesh/MeshRenderer.hpp"
#include "Systems/Scene/Skybox/SkyboxRenderer.hpp"
#include "Systems/Scene/Terrain/TerrainRenderer.hpp"
#include "Systems/Scene/Water/WaterRenderer.hpp"
#include "Systems/Scene/PostProcessing/TonemapRenderer.hpp"
#include "Systems/Scene/Deferred/PopulateSceneDepthRenderer.hpp"
#include "Systems/Scene/PostProcessing/JumpFloodOutlineRenderer.hpp"
#include "Systems/Scene/PostProcessing/FXAARenderer.hpp"
#include "Systems/Scene/PostProcessing/SMAARenderer.hpp"
#include "Systems/Scene/PostProcessing/BackdropBlurRenderer.hpp"
#include "Systems/Scene/PostProcessing/BloomRenderer.hpp"
#include "Systems/Scene/PostProcessing/LensFlareRenderer.hpp"
#include "Systems/Scene/PostProcessing/LightShaftRenderer.hpp"
#include "Systems/Scene/PostProcessing/AutoExposureRenderer.hpp"
#include "Systems/Scene/Deferred/DeferredLightingRenderer.hpp"
#include "Systems/Scene/Deferred/SSAORenderer.hpp"
#include "Systems/Scene/Deferred/CopyRenderer.hpp"
#include "Systems/Scene/Deferred/VelocityViewRenderer.hpp"
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Graphic/GlobalIllumination/IrradianceFieldGather.hpp>
#include <Engine/Graphic/GlobalIllumination/ScreenProbeGather.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheRenderer.hpp>
#include "Systems/Scene/Deferred/DepthExpandRenderer.hpp"
#include "Systems/Scene/Deferred/HZBRenderer.hpp"
#include "Systems/Scene/Deferred/SubsurfaceRenderer.hpp"
#include "Systems/Scene/Deferred/SSRRenderer.hpp"
#include "Systems/Scene/Deferred/SceneColorHistoryRenderer.hpp"
#include "Systems/Scene/Particles/ParticleRenderer.hpp"
#include "Systems/Scene/Clouds/VolumetricCloudRenderer.hpp"
#include "Systems/Scene/Fog/HeightFogRenderer.hpp"

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Graphic/DebugViewState.hpp>

#include <Engine/Graphic/IRenderSystem.hpp>
#include <Engine/Graphic/ExtensionPass.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLightsRenderer.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingProgramHit.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/LandscapeRayTracing.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/ScreenSpaceDenoiser.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/MegaLights.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/MegaLightsVolume.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/RayTracedLocalShadow.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/RayTracedSunShadow.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>
#include <Engine/Graphic/Systems/Scene/PathTracing/PathTracer.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>

namespace Desert::Core
{
    class Scene;
};

namespace Desert::Graphic
{
    namespace RDG
    {
        class Builder;
    }
    class FrameTextures;
    struct FrameValues;
    class ReflectionCaptureRenderer;
    class LocalShadowCache;
    namespace System
    {
        class LocalExposureRenderer;
        class TonemapRenderer;
    } // namespace System

    /**
     * What one view holds and what the scene it draws shares, READ FROM THE RESOURCE LEDGER (RT-LEDGER-3): the
     * view's rows (every device object opened under its instance: targets, history, per-view copies, its TLAS
     * and landscape tiles) and the scene's rows (its RayTracingSceneCache: BLAS variants, skin cache,
     * procedural BLASes). The ONE source of the view's budget entry (SceneRenderer::HeldBytes), of its
     * [ViewMemory] log lines and of the viewport's memory overlay rows, so the three cannot disagree.
     */
    struct ViewLedgerMemory
    {
        ResourceInstanceCensus View;
        ResourceInstanceCensus Scene; // Instance None until the view's first BeginScene
    };

    /// "view #3 41 rows 182.40 MiB (the view's budget); scene #1 12 rows 9.10 MiB (scene-shared, the
    /// scene's own budget entry)" — the [ViewMemory] line.
    [[nodiscard]] std::string FormatViewLedgerMemory( const ViewLedgerMemory& memory );
#if DESERT_DEV_INSTRUMENTS
    class DebugDrawRenderer;
#endif

    class SceneRenderer final
    {
    public:
        // Atmosphere light 1 (UE's Atmosphere Sun Light Index 1, the moon) as a frame lights it: the payload the
        // deferred pass shades (direction it travels, colour/intensity, as DirLights' sun) and what a TRACED ray of
        // it needs beside - its disc and whether it is occluded (the path tracer's light sample, the GI hit
        // lighting) - and its own cascades' policy (MeshRenderer fits slot 1 with it exactly as it fits the sun's
        // with ViewShadowSettings), read from the same DirectionLightComponent.
        struct AtmosphereLight1Frame
        {
            ShaderProtocols::DirectionLightPayload Light;
            float SourceAngleDegrees = 0.0f; // UE SourceAngle: the disc's full angular diameter (0: a delta)
            bool  CastShadows        = true; // UE bCastShadows: false = no ray of it is ever occluded, no cascades
            CascadeBiasSettings ShadowBias;  // ShadowBias / ShadowSlopeBias / ShadowCascadeBiasDistribution
            float CascadeSplitLambda = 0.6f; // UE CascadeDistributionExponent's role: the split blend
        };

        struct UpdateInfo
        {
            // The WORLD'S step this frame (Core::WorldTime::GetDeltaSeconds): zero while paused or while
            // an edited world's viewport is not Realtime, scaled by dilation, fixed under a capture.
            // Everything drawn that moves with time steps by this.
            Common::Timestep Timestep;
            // The wall-clock step, for what is about the machine rather than the world (the sky's
            // re-bake debounce must count while the world is frozen, or a sun moved in a paused editor
            // would never be re-baked).
            Common::Timestep                RealTimestep;
            ShaderProtocols::DirectionLight DirLights;
            // Atmosphere light 1 (UE's Atmosphere Sun Light Index 1, the moon), when the scene marks one:
            // direction it travels and colour/intensity, as DirLights' sun. Shaded by the deferred pass, and lit
            // by the path tracer and the GI hit lighting through the GI frame record.
            std::optional<AtmosphereLight1Frame> AtmosphereLight1;
        };

        struct RenderSubmissionExtra
        {
            uint32_t               Entity = 0;   // owner of the draw: its GpuScene MotionKey (velocity)
            uint32_t               MotionPart = 0;   // a stable part of the owner (MotionRecord::Part), 0 = none
            uint32_t               SectionSlot = 0;   // static: the slot this draw's submeshes shade with
            std::vector<glm::mat4> BoneMatrices; // optional
            // Skinned only: the entity's MorphComponent weights and their revision (empty = no morph).
            std::vector<float>     MorphWeights;
            uint64_t               MorphRevision            = 0;
            bool                   Outlined        = false;
            uint64_t               HiddenSubmeshes = 0;  // bit i = submesh i hidden (static meshes)
            int                    ForcedLOD       = -1; // -1 = auto (by distance)
            int                    LODBias         = 0;  // shifts the auto LOD (ignored when forced)
            bool                   CastShadows     = true;
            bool                   ReceiveShadows  = true;
            bool                   CastContactShadow        = true; // UE bCastContactShadow (static meshes)
            int                    TranslucencySortPriority = 0; // lower draws first (translucency pass)
        };

        // Each renderer IS a view: it owns its ViewResources, so per-frame state is stored per view instead of
        // being overwritten by the next one (Docs/RENDERER_FRAME_STATE.md). There is no count limit and no
        // shared fallback — every view gets its own copies; the only limit is the byte budget the caller
        // checks before creating one (Engine/Core/ViewBudget.hpp).
        //
        // @p shadowQuality is this renderer's directional-shadow BUDGET and is a CONSTRUCTOR argument
        // rather than a setter on purpose: MeshRenderer allocates the cascade framebuffers from it inside
        // Init(), so a value arriving afterwards would be read by nothing and look like a knob. A viewport
        // of a level takes the default; an asset preview passes Graphic::kPreviewShadowQuality, which is
        // the difference between 335 MB of shadow attachments per open window and 21 MB.
        //
        // @p profile widens that budget to the whole view (Graphic/ViewMemory.hpp): a preview also never
        // builds the volumetric-cloud or SSR targets, whatever its scene asks for.
        //
        // @p extent is the size of the surface this view renders into — the viewport panel, the preview
        // widget, the thumbnail, the runtime window — and the build allocates at exactly that; Resize()
        // follows the surface from then on. A surface that has no size yet passes kUnsizedViewExtent.
        //
        // @p accountUnder: None (every real view) mints this view's own ledger instance. A view that renders FOR
        // its scene's state rather than for a viewer — a reflection capture's face view — passes the scene's
        // instance: its rows are then the scene's ("scene #N", RT-LEDGER-4 lead decision), it is no budget entry
        // of its own (LiveHoldings skips it, HeldBytes is 0) and its close checks nothing (the scene's rows are
        // not its to account for).
        SceneRenderer( const ViewExtent& extent, const ViewProfile& profile = kSceneViewProfile,
                       ResourceOwnerInstance accountUnder = ResourceOwnerInstance{} );
        // Unregisters the view; its per-view copies defer their GPU release (ViewResources).
        ~SceneRenderer();

        // This renderer's shadow budget. Read by its own MeshRenderer in Initialize; a level's viewport moves it
        // with the Shadows quality level (BeginScene -> MeshRenderer::RebudgetShadows).
        [[nodiscard]] const ShadowQuality& GetShadowQuality() const
        {
            return m_ViewProfile.Shadows;
        }
        // Scalability ViewDistance.DisplacementDicingRate: pixels a diced edge covers (0 = runtime displacement off),
        // read by the mesh displacement (DisplacementCache) and the landscape's (TerrainRenderer) alike.
        [[nodiscard]] int GetDisplacementDicingRate() const
        {
            return m_DisplacementDicingRate;
        }
        // The view is a reflection capture's face (ViewProfile::SceneCapture), not a level's viewport.
        [[nodiscard]] bool IsSceneCapture() const
        {
            return m_ViewProfile.SceneCapture;
        }
        // The cached point/spot-light shadow depth of the scene BeginScene was handed (Core::Scene owns it, one
        // for all its views); null outside a BeginScene/EndScene bracket.
        [[nodiscard]] LocalShadowCache* GetFrameLocalShadows() const
        {
            return m_FrameLocalShadows;
        }

        // A SCENE HAS JUST BEEN (RE)INITIALISED ON THIS RENDERER. Called from Scene::Init(), which runs on
        // the first load AND on every load after it — opening a second level, "New Scene", or reopening the
        // one already up. A repeat call is legal and always was; what changed with Г11 is what it costs.
        //
        // TWO LIFETIMES, and until Г11 they were one function under one name. The rule that separates them,
        // in the shape Desert/Tests/Engine/ConfigOwnership uses for configuration files — there the actor is
        // "two people at the same time", here it is "two scenes one after the other":
        //
        //   1. Would two scenes loaded one after the other legitimately need a DIFFERENT one of these?
        //      Yes -> it belongs to the SCENE and RebindScene() releases it on every call.
        //   2. Does it come from the window size or the device's capabilities instead?
        //      Yes -> it belongs to the RENDERER; Resize() owns the one thing that changes it.
        //   3. Otherwise -> the RENDERER, built once by EnsureRendererResources().
        //
        // Every pipeline, framebuffer and render system in this class answers question 1 with NO: a render
        // system is constructed from a SceneRenderer* and a Framebuffer and never sees the Scene at all
        // (Systems/RenderSystem.hpp), and the framebuffers are sized from the WINDOW. The one thing that answers
        // YES — the extension passes the editor registers against a particular scene — is not held here at all:
        // the renderer reads them from the scene it is handed each BeginScene (m_FrameExtensions).
        //
        // MEASURED, Debug, on the two scenes Clouds_Protocol and Sky_PhysicalShowcase loaded alternately
        // through the control channel. Before and after INTERLEAVED across five sessions on a machine shared
        // with other agents; the spread is quoted and the figure is the MINIMUM of N, never the mean:
        //
        //                                          before                      after
        //   this function                    141-232 ms   (min 141, N=21)   0.4-0.5 ms (min 0.4, N=19)
        //
        //   whole load, command to the frame that shows it, BY SCENE — the split matters, because the
        //   saving is concentrated in the scene that has a cloud layer:
        //     Clouds_Protocol                6817-9510 ms (min 6817, N=8)   90-99 ms   reloaded onto
        //                                                                              itself (N=4)
        //                                                                   535-599 ms arriving after
        //                                                                              another scene (N=4)
        //     Sky_PhysicalShowcase           1551-1941 ms (min 1551, N=4)   474-494 ms (min 474, N=4)
        //
        // The second block is the one a person waits through, and it is much the larger — which is the
        // finding. The cost was never really the pipelines. Destroying the render systems destroyed
        // VolumetricCloudRenderer's modelling volume, and rebuilding it is a 5.8-SECOND bake whose own
        // staleness test had been comparing the authored parameters correctly all along and was simply
        // never asked, because the object holding the answer had been deleted first. The saving is not "we
        // skipped some work"; it is "we stopped throwing away a cache that was already right".
        //
        // What is LEFT of a cross-scene load is ~450 ms of sky IBL bake, which is correct work: the two
        // scenes have genuinely different skies, and it fires because the sky fingerprint says so.
        void Init();

        // THE SAME BUILD, ONE STAGE PER CALL, for an owner that must not pay it in one frame (the asset
        // thumbnail renderer builds its view while the editor keeps drawing). Each call builds the next stage —
        // the scene targets, then one engine render system — and returns true once every stage has run;
        // Init() then builds nothing again and binds the scene. Init() without it builds every stage itself.
        bool AdvanceRendererBuild();

        // @p camera is the VIEW's camera — the scene holds a list of views and hands in the one this
        // renderer is recording (Engine/Core/SceneViewList.hpp). Null is legal and renders a scene with
        // no view matrix, which is what a world whose camera has not been made yet looks like.
        [[nodiscard]] Common::BoolResultStr BeginScene( const Desert::Core::Scene& scene, Core::Camera* camera );

        void OnUpdate( const UpdateInfo& sceneRenderInfo );

        [[nodiscard]] Common::BoolResultStr EndScene();

        // THE REFLECTION CAPTURES (ReflectionCaptures/ReflectionCaptureRenderer.hpp) are the scene's; a view only
        // reads them. Whether this view does (ViewProfile::ReflectionCaptures and a resolved reflection method of
        // Captures or above), and the identity of the sky bake it holds (0 before it has one) - the scene
        // recaptures when it changes.
        [[nodiscard]] bool     ReadsReflectionCaptures() const;
        [[nodiscard]] uint64_t GetEnvironmentIdentity();
        // The view's HDR scene colour as its last frame left it (the resolved image at MSAA > 1): what a
        // reflection capture resamples into its cube. Null before the targets exist.
        [[nodiscard]] std::shared_ptr<Image2D> GetSceneColorImage() const;
        // HDR-OUTPUT p3: the tonemap's HDR rendition of its last capture frame
        // (TonemapRenderer::GetHdrRenditionImage); null before the first window capture of an HDR output.
        [[nodiscard]] std::shared_ptr<Image2D> GetHdrRenditionImage() const;

        void Resize( const uint32_t width, const uint32_t height );
        // The Render set (ViewTargetSet::Render) to @p render: the scene target, G-buffer, scene depth resolve,
        // silhouette mask, overdraw target and outline. Resize() sizes the Output set (tonemap, FXAA, SMAA) and
        // calls this with the split of the new extent; OnUpdate calls it with the frame's split. Equal: nothing.
        void ResizeRenderTargets( ViewExtent render );

        // CAMERA CUT: the next frame this view renders starts a new temporal sequence in the SAME world.
        // Every render system gets IRenderSystem::OnTemporalHistoryReset (frame indices to zero, histories
        // invalid, exposure snaps, particle simulation restarts). Nothing is released and no pass is added,
        // so it is legal between any two frames. Not tied to any one caller: the headless capture calls it
        // when its frame count starts, a camera cut or a teleport asks for the same thing.
        void ResetTemporalHistory();

        // THE WORLD THIS VIEW DREW WAS TORN DOWN (UE: the FSceneViewState and the Lumen/distance-field scene data
        // die with their FScene). Releases everything this view built FROM the old world - its surface cache,
        // radiance cache, probe gathers, global distance field and the terrain's materials and layer arrays - and
        // cuts the temporal history. Each is made again on first use by the next world. Called between frames by
        // the opener, after the world is cleared and before the next one loads
        // (SceneWorkspace::ReleaseActiveWorld), so the old world's device memory is given back before the new
        // one's is asked for (FIELD-MEM p3).
        void ReleaseWorldResources();

        [[nodiscard]] const ViewExtent& GetViewExtent() const
        {
            return m_ViewExtent;
        }

        // WHETHER A MESH IS WITHIN ITS MAX DRAW DISTANCE FROM THIS VIEW (UE's LDMaxDrawDistance): asked by the
        // mesh draw commands before they submit, so a mesh beyond it enters no queue and no pass — lit, shadow,
        // velocity or debug — of this view. Measured from the view's main camera and stretched by its lens
        // (Graphic::IsWithinDrawDistance). maxDrawDistance <= 0, or no camera yet: drawn.
        [[nodiscard]] bool IsWithinMaxDrawDistance( const Mesh* mesh, const glm::mat4& transform,
                                                    float maxDrawDistance ) const;

        // The slot binding is taken BY SHARED HANDLE, not by reference to the caller's storage: the
        // caller is a draw command whose recorder (an ECS component) may already be gone. See
        // Graphic::MaterialSlotBinding (A8-3).
        void SubmitMesh( const Mesh* mesh, const MaterialSlotBindingPtr& materialSlots, const glm::mat4& transform,
                         const RenderSubmissionExtra& extra );

        // THE VIEW'S LOD PREVIEW (UE LOD Manager / Static Mesh Editor "LOD" preview): draw `entity`'s static mesh
        // at `level` in THIS view only — every pass (G-buffer, shadows, forward) reads it through the draw's
        // ForcedLOD, the authored StaticMeshComponent::ForcedLOD is untouched and other views keep their own pick.
        // It is a LEASE, not a setting: per-frame renderer state that lapses one frame after the caller stops
        // renewing it (BeginScene), so a closed tool can never leave a view pinned. level < 0 clears it now.
        void              SetLODPreview( uint32_t entity, int level );
        [[nodiscard]] int LODPreviewFor( uint32_t entity, int authoredForcedLOD ) const;

        // Submit one landscape tile for this frame (from LandscapeECSSystem via DrawLandscapeTileCommand):
        // the Terrain pipeline reading the tile's R16 copy, surfaced with its root's material and layers.
        void SubmitLandscapeTile( const std::shared_ptr<Image2D>& heightmap, const System::LandscapeTileDraw& tile,
                                  const MaterialOverrides& overrides, const System::LandscapeWeightDraw& weights );

        // Submit one landscape root's grass for this frame (from LandscapeECSSystem via
        // DrawLandscapeGrassCommand).
        void SubmitLandscapeGrass( System::GrassFieldDraw draw );

        // Submit one water zone for this frame (from WaterZoneECSSystem via WaterZoneCommand): its quadtree is
        // walked from the main camera and its tiles drawn by the water renderer (WATER-W6).
        void SubmitWaterZone( System::WaterZoneDraw zone );

        // Submit a mesh drawn with a generic data-driven material (MaterialComponent with a non-lit shader).
        // directTexture (optional): a runtime-owned Image2D bound to `directTextureSampler`, for
        // procedural textures with no TextureAsset handle (the text SDF atlas).
        // castShadows: rasterize this draw into the shadow cascades. Off by default — see the note on
        // GenericMeshRenderData::CastShadows; only a producer that knows the draw is a solid mesh, and
        // that no OTHER draw of the same entity is already casting, may turn it on.
        void SubmitGenericMesh( uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                const std::string& shaderName, const MaterialOverrides& overrides,
                                bool outlined = false, Image2D* directTexture = nullptr,
                                const std::string& directTextureSampler = {}, bool castShadows = false,
                                bool                                    visibleInRayTracing = false,
                                const RayTracing::RayTracingProgramHit& rayHit              = {} );

        // v3 per-slot custom shaders: draw only @p visibleSubmeshMask submeshes of the mesh with the
        // slot's own runtime material (a MaterialService-owned DataDrivenMaterial). @p motionPart: the stable part
        // of the entity the draw belongs to (RenderSubmissionExtra::MotionPart), 0 = the entity itself.
        void SubmitSlotMaterialMesh( uint32_t entity, Mesh* mesh, const glm::mat4& transform, Material* material,
                                     uint64_t visibleSubmeshMask, bool outlined = false, bool castShadows = false,
                                     uint32_t motionPart = 0 );

        /// ISM instances the last geometry pass drew (MeshRenderer::GetIsmInstancesDrawn).
        [[nodiscard]] uint32_t GetIsmInstancesDrawn() const;
        /// Draws the last frame gave to the default surface while their own pipeline compiled
        /// (MeshRenderer::GetStandInDraws): the control gate's PendingWork::MaterialPipelines.
        [[nodiscard]] uint32_t GetStandInDraws() const;
        /// Triangles the last frame's view pass drew, at the LOD each mesh was drawn with
        /// (MeshRenderer::GetViewTrianglesDrawn).
        [[nodiscard]] uint64_t GetViewTrianglesDrawn() const;

        // UE-style Instanced Static Mesh: one mesh + one lit material drawn for every transform in
        // @p transforms. Material and transforms are both co-owned handles for the reason SubmitMesh's
        // binding is (A8-3).
        // `castShadows` is NOT defaulted: a default here is a place for a caller to forget the flag,
        // and the one this replaces was an unconditional append in the cascade pass.
        // `Mesh*` AND NOT `const Mesh*`, and that removes a const_cast rather than adding a hazard: the
        // only caller holds a non-const mesh, and the queue this reaches needs one too.
        /// TEX-STREAMING: tells Graphic::TextureStreamingDemand how many screen pixels the nearest of
        /// @p transforms covers with @p mesh, for every texture @p materials bind and every texture asset
        /// @p overrides name; answers that size (0 when there is no main camera or the mesh has no bounds).
        float NoteTextureDemand( const Mesh& mesh, std::span<const glm::mat4> transforms,
                                 std::span<const Material* const> materials,
                                 const MaterialOverrides*         overrides = nullptr ) const;

        void SubmitInstancedMesh( uint32_t entity, Mesh* mesh, const MaterialInstancePtr& material,
                                  uint64_t                                             hiddenSubmeshes,
                                  const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                  bool castShadows, bool castContactShadow,
                                  const InstanceCullDistance& cullDistance, const InstanceWind& wind,
                                  const MaterialInstancePtr& displacedMaterial );

        void SetEnvironment( const std::shared_ptr<MaterialSkybox>& material, const SkyLook& look );

        // Selection-outline (Jump Flood) appearance. Editor-only: pushed each frame from EditorPreferences
        // (the outline is a viewport visualization, not a scene property, so it does not live in SceneSettings).
        void SetOutlineSettings( const glm::vec3& color, float width, float smoothness, bool enabled );

        // What this view draws ON TOP of the world: grid, colliders, bounding boxes, wireframe, the buffer
        // and shadow debug views. Pushed in for exactly the same reason the outline is — it is a property
        // of the view, not of the scene (Graphic/DebugViewState.hpp records the measurement that settled
        // it). A renderer nobody pushes to shows the lit world and no overlay, which is what the Runtime
        // and every offscreen preview renderer rely on.
        //
        // Call it BEFORE BeginScene: the flags reach the mesh renderer from there, so a push afterwards
        // lands one frame late.
        void SetDebugView( const DebugViewState& state )
        {
            m_DebugView = state;
        }

        // WHAT THIS MACHINE CAN AFFORD — post AA, mesh LOD, the sampler's filter and anisotropy, the
        // cloud tier. Pushed in for the same reason the two above are: it is not a property of the scene
        // (К3 took it out of the level file, where a weak machine could not turn it down without editing
        // a file that goes to everybody), and it is not a property of the renderer either.
        //
        // WHY EVERY VIEW IS TOLD SEPARATELY INSTEAD OF READING THE GLOBAL HERE. An offscreen preview
        // renders a 512-pixel pane and has no use for the viewport's cloud budget, so it pushes the
        // machine's answer with its own tier substituted — applied to a COPY on the way in, never to the
        // stored one. That is К10's rule for viewport modes, and the reason it exists: a view's transient
        // idea of what it needs must never be written back into the user's permanent answer.
        //
        // A renderer nobody pushes to holds the machine's own answer as of its construction (see the
        // member), not the schema defaults — because two of these five reach a GLOBAL the sampler path
        // reads, and a stale copy there is everyone's problem, not just this view's. MSAA is NOT here:
        // pipelines bake their sample count, so Init reads MachineSettings directly and a later push
        // could not change it.
        //
        // Call it BEFORE BeginScene: the values reach the systems from there.
        void SetQuality( const Common::Scalability::ResolvedQuality& quality )
        {
            m_Quality = quality;
        }
        // The GI / reflection / shadow methods this view renders and why any came out below its request — what
        // the viewport overlay draws (Graphic/RenderMethods.hpp). Refreshed every BeginScene.
        // VFX-PORT-2: this view's particle cull (ParticleRenderer::ViewEmitterCulls); empty without the system.
        [[nodiscard]] std::span<const System::ParticleViewEmitterCull> ParticleViewEmitterCulls() const;
        [[nodiscard]] const RenderMethods& GetRenderMethods() const
        {
            return m_RenderMethods;
        }
        // Read back by the editor's own external passes (grid, colliders), which draw INTO this view and
        // therefore must ask this view what it is showing — not a global, or every offscreen preview would
        // inherit the main viewport's flags.
        [[nodiscard]] const DebugViewState& GetDebugView() const
        {
            return m_DebugView;
        }
        // BY VALUE, and it has to be. SkyboxRenderer::GetEnvironment() composes its answer — procedural
        // bake or skybox-asset environment — and therefore returns a temporary; this used to hand back a
        // reference to it, which dangled the instant the call returned. The forward path read that
        // reference for a long time and mostly got away with it, because a just-freed stack frame usually
        // still holds the bytes. The deferred composite's new environment read did not: it saw a valid
        // irradiance handle beside a zeroed prefiltered one and correctly reported a half-baked
        // environment. Found 2026-09-03 by the diagnostic that was supposed to catch a failed bake.
        std::optional<Environment> GetEnvironment();
        // A reflection capture's face view (ViewProfile::SceneCapture) is lit by the scene's sky light and bakes
        // none (SkyboxRenderer::BorrowEnvironment): @p environment is the reading view's, handed before each
        // frame.
        void BorrowEnvironment( std::optional<Environment> environment );
        // A reflection capture's face views stand at one capture point and read ONE global distance field, their
        // capture renderer's (DistanceField::GlobalDistanceField: updated once per frame): @p field is lent before
        // each frame and outlives the frames in flight that read it; null returns the view to its own field.
        void ShareGlobalDistanceField( DistanceField::GlobalDistanceField* field );
        // ... and ONE surface cache (the Lumen scene of their cards; UE: one Lumen scene per FScene, a view owns
        // no cards): @p cache is their capture renderer's, lent before each frame (one face renders per engine
        // frame, so the cache sees one Update per frame, as a view's own does) and outliving the frames in flight
        // that recorded its passes; null returns the view to its own cache.
        void ShareSurfaceCache( SurfaceCache::SurfaceCacheRenderer* cache );

        // Procedural sky configuration (from the SkyAtmosphereComponent + the atmosphere sun, via the ECS).
        // sunDir is the direction TOWARD the sun, already normalized; bakeNow is the one-shot request from
        // the editor's Bake button.
        void SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow, const SkySettings& sky,
                               const SunLightFx& fx, const AtmosphereLight1& light1 );

        // This frame's exponential height fog (from ExponentialHeightFogComponent, via the ECS).
        // `present` = false means the scene has no enabled fog component at all — said explicitly, because
        // the renderer keeps its settings across frames and would otherwise keep evaluating a deleted one.
        // `fogHeightY` is the fog entity's transform Y — the fog floor, owned by the transform and never
        // authored twice.
        void SetHeightFog( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY );

        // This frame's volumetric cloud layer (from VolumetricCloudComponent, via the ECS). The wind
        // offset is accumulated by the ECS system, which is where the timestep lives.
        void SetVolumetricClouds( bool present, const ECS::VolumetricCloudData& data, const glm::vec3& windOffset,
                                  const glm::vec3&                      windDirection,
                                  const std::vector<HeroCloudInstance>& heroClouds );

        // The evaluated per-frame sky: sun direction and radiance, ambient above/below, night factor, the
        // planet radius, and an OPAQUE handle to the packed sky-parameter buffer. Consumers never see the
        // sky's authoring representation, so a change to the palette cannot break them.
        const AtmosphereEnv& GetAtmosphere() const;

        // This view's cloud field, prepared for the sky's environment bake. The bake is SkyboxRenderer's
        // and the field is VolumetricCloudRenderer's — two sibling systems that share no resource — so the
        // route between them is here, exactly like GetAtmosphere() above going the other way. See
        // Engine/Graphic/Clouds/CloudEnvironmentBake.hpp for what crosses.
        CloudEnvironmentBake BuildCloudEnvironmentBake();

        // Is this view rebuilding its cloud modelling volume right now? See
        // System::VolumetricCloudRenderer::IsModellingVolumeBaking for what the answer is FOR — in one
        // line, about half of a cloud material's parameters cost a multi-second bake on a worker, and
        // without a signal the artist cannot tell "expensive" from "broken". The route is here for the same
        // reason BuildCloudEnvironmentBake above is: an editor panel must not reach into a render system.
        //
        // False for a view with no cloud layer, which is every mesh preview and every asset thumbnail.
        bool IsCloudVolumeBaking() const;

        // Whether this view has a sky light (SkyLightState): no sky at all, or a sky whose light is captured in
        // the frame graph from the view's first frame. What tells a defined black ambient (no sky) from a fault.
        [[nodiscard]] SkyLightState GetSkyLightState() const;

        // Whether the scene's sky light is being, or will be, captured again for a sun TOWARD @p sunDir
        // (SkyboxRenderer::SkyLightRecapturesFor); false without a sky system.
        [[nodiscard]] bool SkyLightRecapturesFor( const glm::vec3& sunDir ) const;

        // Whether this view draws a cloud layer: a picture that reprojects a history and converges over frames (a
        // reflection capture's face waits for it: ReflectionCaptureRenderer).
        [[nodiscard]] bool DrawsVolumetricClouds() const;

        // How far that rebuild has got, 0..1. Meaningless unless IsCloudVolumeBaking(); see
        // System::VolumetricCloudRenderer::ModellingBakeProgress for why the wait is worth a number.
        float CloudVolumeBakeProgress() const;

        // "N live, X MiB": views alive right now (ViewResourceRegistry::LiveCount) and what they hold
        // together. Every log line that opens or closes a view prints it, so a view that is never destroyed
        // is readable as a count that only goes up.
        static std::string DescribeLiveViews();

        // Device memory this view holds, MEASURED (RT-LEDGER-3): the bytes of the ledger rows that carry this
        // view's instance — its targets, its temporal history, its per-view buffer copies, its TLAS and its
        // landscape tiles. Not a forecast: ViewTargetCensus only answers "may a view of this size be CREATED"
        // (ViewBudgetGate), before any row exists. The scene-shared rows are the scene's and are NOT here
        // (SceneHoldings). What the view-budget rule counts as "open views hold" (Engine/Core/ViewBudget.hpp).
        [[nodiscard]] uint64_t HeldBytes() const;

        // This view's and its scene's ledger rows, taken now (ViewLedgerMemory).
        [[nodiscard]] ViewLedgerMemory TakeLedgerMemory() const;

        // This view's ledger instance: what an object drawing FOR this view but owned elsewhere (the game's
        // Render2D over it) tags its rows with, so they are in this view's budget (RT-LEDGER-4).
        [[nodiscard]] ResourceOwnerInstance GetLedgerInstance() const
        {
            return m_LedgerView.Instance;
        }

        // Every live view and what it holds, for the budget's refusal text and its usage stand-in.
        [[nodiscard]] static std::vector<Engine::ViewBudget::HeldView> LiveHoldings();

        // Every live scene instance in the ledger and what its shared rows hold — each scene ONCE, however many
        // views draw it, and never inside a view's entry. The budget's scene entries (ViewBudgetGate).
        [[nodiscard]] static std::vector<Engine::ViewBudget::HeldView> SceneHoldings();

        const auto& GetMainCamera() const
        {
            return m_SceneInfo.ActiveCamera;
        }

        // THE CURRENT FRAME'S VIEW (TAA1 step 3) — what every camera-block writer of this renderer fills
        // CameraUB from (ShaderProtocols::MakeCameraUB). CONTRACT: non-null only while OnUpdate runs, from the
        // moment m_ViewState.BeginFrame accepted the frame until OnUpdate returns (the graph executes and
        // EndFrame commits inside that window). Outside it — before the first frame, after a refused frame,
        // between frames, from another view — it is nullptr, never the last frame's view: a writer that gets
        // nullptr has no view to draw for and writes nothing.
        [[nodiscard]] const ViewFrame* GetViewFrame() const
        {
            return m_CurrentViewFrame;
        }

        const auto& GetDirectionLights() const
        {
            return m_DirectionLights;
        }
        // Atmosphere light 1 as this frame lights it (UpdateInfo::AtmosphereLight1, its colour through its own
        // transmittance), when the scene marks one: MeshRenderer fits its cascades from it.
        [[nodiscard]] const std::optional<AtmosphereLight1Frame>& GetAtmosphereLight1() const
        {
            return m_AtmosphereLight1;
        }

        // CSM debug: a cascade's static-caster cache (MeshRenderer::GetCascadeCacheImage), for the editor's
        // cascade viewer; the sampled maps are layers of one graph array, which ImGui does not show.
        std::shared_ptr<Image2D> GetShadowCascadeCacheImage( uint32_t cascade );
        uint32_t                 GetShadowCascadeCount();

        const std::shared_ptr<Image2D>      GetFinalImage();
        // The single-sample scene depth compute passes sample (height fog, volumetric clouds): the scene depth
        // itself at MSAA 1, SceneDepthResolved at MSAA > 1 ("Scene: DepthResolve"); null when that failed to
        // build.
        std::shared_ptr<Image2D>            GetComputeSceneDepth() const;
        const std::shared_ptr<Framebuffer>& GetTargetFramebuffer() const
        {
            return m_TargetFramebuffer;
        }

        // Deferred G-buffer (Albedo+Metallic / Normal+Roughness / depth). Populated only in the Deferred path.
        const std::shared_ptr<Framebuffer>& GetGBuffer() const
        {
            return m_GBuffer;
        }

        // Active rendering path, refreshed from SceneSettings each BeginScene.
        Core::RenderPath GetRenderPath() const
        {
            return m_RenderPath;
        }

        // Shared GraphicsPipeline cache (keyed by shader + target + render-state). Renderers request
        // pipelines from here instead of creating their own; cleared on Init (full rebuild).
        PipelineCache& GetPipelineCache()
        {
            return m_PipelineCache;
        }

        // True while the scene runs in Play mode (refreshed each BeginScene). Extension passes use this
        // to hide authoring aids during gameplay.
        bool IsScenePlaying() const
        {
            return m_ScenePlaying;
        }

        std::shared_ptr<Texture>     GetTexture( const std::string& name );

        void RegisterRenderSystem( const std::string& name, std::shared_ptr<IRenderSystem> system );
        void UnregisterRenderSystem( const std::string& name );

        // BY CONST REFERENCE, because the caller is a RECORDED COMMAND that is replayed once per view
        // of the scene (Scene::OnUpdate). An rvalue parameter invited the recorder to hand its only copy
        // away on the first view, and the second view then got a moved-from light.
        // @p shadow is what the light asks of its shadow; the payload's ShadowView is written later, by
        // MeshRenderer::UpdateLocalShadows, once the frame's atlas is allocated.
        void AddPointLight( const ShaderProtocols::PointLightPayload& pointLight,
                            const LocalLightShadowSettings&           shadow );

        const auto& GetPointLights() const
        {
            return m_PointLight;
        }

        void AddSpotLight( const ShaderProtocols::SpotLightPayload& spotLight,
                           const LocalLightShadowSettings&          shadow );

        const auto& GetSpotLights() const
        {
            return m_SpotLight;
        }

        /// THE frame's cloud-shadow payload — the map, its projection, and the numbers a receiver needs
        /// to read it. Gathered HERE, once, and handed to every consumer: the deferred composite, the
        /// forward lit materials (through Graphic::SceneFrameBinding), the skinned material and the terrain
        /// material. While the composite was the only reader, this gather sat inline in the deferred
        /// branch and the answer to "does this surface receive a cloud shadow" was "only if a deferred
        /// pass drew it".
        ///
        /// Valid after the cloud shadow node was declared for this frame (AddFrameCloudShadowMap) — which is
        /// before the render graph records, so every pass in the frame may ask. Returns the default (disabled, no
        /// map) whenever the layer is absent, off, not casting or at zero strength.
        CloudShadowInput GetCloudShadowInput() const;
        /// The view's snapshot of the world's parameters this frame (Common/WorldParameters.glslh), written once
        /// at BeginScene from Scene::GetWorldParameters; every capture of the frame binds this one buffer.
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetWorldParametersBuffer() const
        {
            return m_WorldParameters;
        }
        // The frame's sun as every lit pass receives it: the first directional light (direction it travels,
        // colour/intensity), or the default sun when the scene has none. The deferred composite and the
        // surface-cache lighting read this one rule.
        struct FrameSun
        {
            glm::vec4 Direction{ 0.0f, -1.0f, 0.0f, 0.0f };
            glm::vec4 ColorIntensity{ 1.0f, 0.98f, 0.92f, 3.0f };
        };
        [[nodiscard]] FrameSun GetFrameSun() const
        {
            FrameSun sun;
            if ( const auto& dl = m_DirectionLights.DirectionLights; !dl.empty() )
            {
                sun.Direction      = dl[0].Direction;
                sun.ColorIntensity = dl[0].ColorIntensity;
            }
            return sun;
        }
        // Atmosphere light 1 (the moon) as the same lit passes receive it: its direction and colour/intensity, or
        // zero colour/intensity when the scene has none (the shaders then add nothing).
        [[nodiscard]] FrameSun GetFrameLight1() const
        {
            if ( !m_AtmosphereLight1 )
                return FrameSun{ .ColorIntensity = glm::vec4( 0.0f ) };
            return FrameSun{ .Direction      = m_AtmosphereLight1->Light.Direction,
                             .ColorIntensity = m_AtmosphereLight1->Light.ColorIntensity };
        }
        // Both as the world-space GI histories compare them (GlobalIllumination::InvalidatesLightingHistory).
        [[nodiscard]] GlobalIllumination::DirectionalLighting GetFrameDirectionalLighting() const
        {
            const FrameSun sun    = GetFrameSun();
            const FrameSun light1 = GetFrameLight1();
            return { .SunDirection         = sun.Direction,
                     .SunColorIntensity    = sun.ColorIntensity,
                     .Light1Direction      = light1.Direction,
                     .Light1ColorIntensity = light1.ColorIntensity };
        }

        /// THE view's contact shadows (Common/ContactShadow.glslh): the march's view values and the sun's length,
        /// packed into ShadowUB for every lit pass (the deferred composite, the forward meshes, the glass).
        /// @p hasOpaqueDepth says whether the pass has the opaque scene depth to march: the deferred path's
        /// G-buffer depth does; the forward path draws its opaque surfaces with no prepass, so its passes get
        /// Steps = 0 (no march) - there is nothing in front of them for the ray to hit.
        [[nodiscard]] SceneResources::ContactShadowSettings GetContactShadowSettings( const ViewFrame& frame,
                                                                                      bool hasOpaqueDepth ) const;
        /// THE view's sun PCSS kernel (Scalability Shadows.BlockerSearchTaps / Shadows.FilterTaps), packed into
        /// ShadowUB by every lit pass that samples the cascades.
        [[nodiscard]] SceneResources::ShadowFilterSettings GetShadowFilterSettings() const;
        /// SSS-PROFILE: this view scatters and its Burley history has not yet averaged its full window (a Scale
        /// change, a camera cut, the first frames) - the control gate's PendingWork::SubsurfaceHistory.
        [[nodiscard]] bool SubsurfaceHistoryConverging() const;
        /// GRASS-SETTLE p14: this view's last frame drew a grass variety that may hold fewer instances than its
        /// generator keeps (GrassRenderer::InstancesSettling) - the control gate's PendingWork::GrassInstances.
        [[nodiscard]] bool GrassInstancesSettling() const;

    private:
        // THIS VIEW in the resource ledger (RT-LEDGER-2). Its RT rows — the TLAS storage, the landscape tiles'
        // buffers and BLASes — carry this instance (the RT block's scope in OnUpdate); HeldBytes reads them back
        // from the ledger. DECLARED FIRST so it is destroyed LAST: its destructor reads the ledger after every
        // member that held a row of the view is gone, and logs what is left (0, or a leak by name).
        struct LedgerView
        {
            // The view's own instance, or the scene's it accounts under (the constructor's accountUnder).
            ResourceOwnerInstance Instance;
            // True when Instance is the scene's: the rows are not this view's to check on close or to budget.
            bool Borrowed = false;

            explicit LedgerView( const ResourceOwnerInstance accountUnder )
                 : Instance( accountUnder.IsNone() ? ResourceOwnerInstance::NewView() : accountUnder ),
                   Borrowed( !accountUnder.IsNone() )
            {
            }
            ~LedgerView();
            LedgerView( const LedgerView& )            = delete;
            LedgerView& operator=( const LedgerView& ) = delete;
            LedgerView( LedgerView&& )                 = delete;
            LedgerView& operator=( LedgerView&& )      = delete;
        };
        LedgerView m_LedgerView;
        // The ledger instance of the scene this view last began (Scene::GetLedgerInstance). A value, not the
        // cache pointer: the overlay reads it after EndScene cleared m_FrameRayTracingCache.
        ResourceOwnerInstance m_SceneLedgerInstance;

        // Rows opened while the returned scope lives are this view's (owner SceneRenderer, the view's instance).
        // Opened by every entry point that can allocate; C++17 guaranteed elision returns the pinned scope.
        [[nodiscard]] ResourceAttributionScope OpenViewLedgerScope() const
        {
            return ResourceAttributionScope( ResourceOwner::SceneRenderer, m_LedgerView.Instance );
        }

        // Everything this view keeps per frame in flight, keyed by the shared resource it copies; its name is
        // the view's name in every log line. Made the target of per-frame writes by an ActiveViewScope at the
        // top of each frame phase (BeginScene/OnUpdate/EndScene).
        ViewResources m_ViewResources;

        // TAA1 step 3 — THE one previous-frame source of this view (UE: FSceneViewState): every AddFrame* that
        // reads a matrix, a camera position or a previous-frame value takes the ViewFrame BeginFrame returned.
        SceneViewState m_ViewState;
        // TAA1-B 5c — the temporal resolve of this view's frames: the implementation of the frame's
        // TemporalMethod (CreateTemporalUpscaler), null for None. Re-created only when the method changes
        // (EnsureTemporalUpscaler), so its lazily built pipelines survive frames; it outlives every graph that
        // recorded its node (the graph executes inside OnUpdate). Handed to SceneViewState::BeginFrame.
        std::unique_ptr<ITemporalUpscaler> m_TemporalUpscaler;
        // The view's own global distance field (clipmaps follow this view's camera), made on first use - unless
        // one is lent (ShareGlobalDistanceField): a face view of a reflection capture reads its capture's.
        std::unique_ptr<DistanceField::GlobalDistanceField> m_OwnGlobalDistanceField;
        DistanceField::GlobalDistanceField*                 m_SharedGlobalDistanceField = nullptr;
        // What this view does with the field it reads (its near-field grid, the debug view), made on first use.
        std::unique_ptr<DistanceField::GlobalDistanceFieldView> m_GlobalDistanceFieldView;
        // The view's world radiance cache (a traced GI method, either tracer; clipmaps follow this view's camera),
        // made on first use.
        std::unique_ptr<GlobalIllumination::WorldRadianceCache> m_RadianceCache;
        // The view's screen probe gather (a traced GI method; its history follows this view), made on first use.
        std::unique_ptr<GlobalIllumination::ScreenProbeGather> m_ScreenProbeGather;
        // The surface cache (GI-SURFACE-CACHE-2): the Lumen scene of the view's mesh cards, its atlases and
        // capture. Read by the traced GI's hits (GI-CARD-LOOKUP) and by its view modes. The view's own, made on
        // first use - unless one is lent (ShareSurfaceCache): a face view of a reflection capture reads its
        // capture's. ReadSurfaceCache() is the one the frame reads.
        std::unique_ptr<SurfaceCache::SurfaceCacheRenderer> m_OwnSurfaceCache;
        SurfaceCache::SurfaceCacheRenderer*                 m_SharedSurfaceCache = nullptr;
        uint64_t m_SurfaceCacheLoggedCensus = 0; // cards << 32 | captured
        // This frame's surface cache (AddFrameSurfaceCache): the scene's records, their graph buffers and what a
        // traced hit reads - for the traced GI and the view modes of the same graph; reset every frame.
        struct SurfaceCacheFrameRecord
        {
            SurfaceCache::SceneFrame                         Scene;
            SurfaceCache::SurfaceCacheRenderer::FrameBuffers Buffers;
            GlobalIllumination::SurfaceCacheFrame            Hits;
            // This view's call updated the cache this frame (SurfaceCacheRenderer::UpdateScene): it records the
            // passes that write the atlases; false - a lent cache another view updated this frame - only reads.
            bool Updated = false;
        };
        std::optional<SurfaceCacheFrameRecord> m_SurfaceCacheFrame;
        // The traced GI's records of this frame that the traced reflections read too: the GI frame record and the
        // radiance cache's read (AddFrameTracedGI, when the GI traces). The mesh SDF near field is the tracer's
        // own (TracerFrame::MeshDistanceFields, the view's froxel grid). Reset by AddFrameTracedGI every frame.
        struct TracedFrameShared
        {
            RDG::BufferRef                                        GIFrame;
            std::optional<GlobalIllumination::RadianceCacheFrame> RadianceCache;
            RDG::BufferRef InstanceSurfaceCacheRows; // AddFrameInstanceSurfaceCacheRows
        };
        TracedFrameShared m_TracedFrameShared;
        // The view's irradiance field gather (GIMethod::ProbeVolume: the radiance cache read per pixel, no screen
        // probes), made on first use.
        std::unique_ptr<GlobalIllumination::IrradianceFieldGather> m_IrradianceFieldGather;
        // The "Scene: PopulateSceneDepth" node's pipeline (made on the first temporal frame).
        std::unique_ptr<System::PopulateSceneDepthRenderer> m_PopulateSceneDepth;
        // The fixed SSAA downsample for a Supersample split (Split.Mode == Supersample, TAA1-B step 6): owns its
        // compute pipeline, so it lives with the view rather than in a static that would outlive the device.
        SupersampleResolve m_SupersampleResolve;
        // SCAL-SPATIAL1: the spatial upscale of a frame below 100 % without a temporal method, and the post
        // sharpen after any resolve (View/SpatialUpscale.hpp). Each owns its compute pipeline, like the above.
        SpatialUpscale m_SpatialUpscale;
        Sharpen        m_Sharpen;
        // MR2: the view's motion blur (View/MotionBlur.hpp) and this frame's settings (BeginScene).
        std::unique_ptr<MotionBlur> m_MotionBlur = std::make_unique<MotionBlur>();
        MotionBlurSettings          m_MotionBlurSettings;
        // MR3: the view's depth of field (View/DepthOfField.hpp) and this frame's settings (BeginScene).
        std::unique_ptr<DepthOfField> m_DepthOfField = std::make_unique<DepthOfField>();
        DepthOfFieldSettings          m_DepthOfFieldSettings;
        // ViewInputs::Quality: SCAL1 AntiAliasing.TemporalQuality, read with the rest of the quality (BeginScene).
        TemporalAAQuality m_TemporalAAQuality = TemporalAAQuality::Medium;
        // GetViewFrame's answer: the ViewFrame OnUpdate's BeginFrame returned, set and cleared by OnUpdate's
        // CurrentViewFrameScope so no exit path leaves it pointing at a finished frame.
        const ViewFrame* m_CurrentViewFrame = nullptr;
        // Set by ResetTemporalHistory (the one cut entry point: ShotDirector, headless capture); consumed by the
        // next BeginFrame that succeeds as ViewInputs::CameraCut.
        bool m_CameraCutPending = false;
        // SetLODPreview's lease: the entity, its level, and the frames it still holds (renewed by the tool).
        struct LODPreviewLease
        {
            uint32_t Entity     = 0;
            int      Level      = -1;
            int      FramesLeft = 0;
        };
        LODPreviewLease m_LODPreview;
        // ScenePreExposure's source this frame (ViewInputs::LastExposure); 0 = none known.
        float m_LastExposure = 0.0f;
        // The AverageLocalExposure the eye adaptation read back with it (1 = none): pre-exposure follows it.
        float m_LastAverageLocalExposure = 1.0f;
        // Scene::GetGeneration() of the scene BeginScene was handed (a reload at the same address is a new
        // generation, so a new identity: SceneViewState resets the history).
        uint64_t m_SceneGeneration = 0;
        // The scene's game time at BeginScene, seconds (ViewInputs::TimeSeconds).
        double m_SceneTimeSeconds = 0.0;
        // The world's parameters as of BeginScene (GetWorldParametersBuffer): per (view x frame in flight).
        std::shared_ptr<ShaderResources::StorageBuffer> m_WorldParameters;
        // Uploads Scene::GetWorldParameters into m_WorldParameters (BeginScene, inside the view's scope).
        [[nodiscard]] Common::BoolResultStr UploadWorldParameters( const Desert::Core::Scene& scene );

        // Constructor-set, const in everything but name: MeshRenderer copies it in Initialize and the
        // cascade framebuffers exist from that moment until this renderer dies.
        ViewProfile m_ViewProfile;

        // The last size a Resize() went past the view budget at, so the warning is logged once and not every
        // frame the panel keeps asking.
        ViewExtent m_OverrunWarnedAt{};

        // The surface's size: the constructor's until the first Resize(), then the last one's. The build
        // reads it and nothing reads the window, so a view never holds targets larger than its surface.
        ViewExtent m_ViewExtent;

        // THE RENDER SET'S EXTENT (ViewTargetSet::Render): what the scene target, the G-buffer, the scene depth
        // resolve, the silhouette mask, the overdraw target and the outline were last built at — the frame's
        // ResolutionSplit::Render, set by ResizeRenderTargets. The constructor's extent until the first frame.
        ViewExtent m_RenderExtent;
        // The render scale the view last resolved (ResolveViewResolution): Resize() sizes the render set by it, so
        // a resize does not rebuild the render targets at the output extent only for the next frame to shrink
        // them.
        int m_LastRenderScalePercent = 100;

        // Has EnsureRendererResources() run? Set once, never cleared — see its comment for why there is no
        // path that invalidates it.
        bool m_RendererResourcesBuilt = false;

        // THE STAGED BUILD (AdvanceRendererBuild): the next stage to run, whether every stage has run, and what
        // the stages cost — summed, and the worst one by name, which is what Init() logs.
        std::size_t m_RendererBuildStage      = 0;
        bool        m_RendererBuildDone       = false;
        float       m_RendererBuildMs         = 0.0f;
        float       m_RendererBuildWorstMs    = 0.0f;
        const char* m_RendererBuildWorstStage = "";

        // THE RENDERER'S HALF OF Init(), and it runs exactly ONCE per SceneRenderer. Builds the scene
        // target and the deferred buffers, constructs every engine render system, initialises them (which
        // is where the ~35 graphics pipelines are compiled) and wires the post chain together.
        //
        // Once, and not "once per device generation": device loss is NOT recoverable in this engine by a
        // measured decision (Graphic/DeviceLost.hpp), so there is no second generation to rebuild for, and
        // inventing a re-entry here would be inventing a path nothing can reach. The other thing that could
        // invalidate these resources — the window size — is Resize()'s, and it resizes them in place.
        //
        // Returns false when it has already run, so the caller can tell a first build from a rebind
        // without keeping a second copy of the flag.
        bool EnsureRendererResources();

        // Builds stage @p stage of the above (the scene targets, then one engine render system per stage, in
        // dependency order) and returns its name; nullptr once there is no such stage.
        const char* BuildRendererStage( std::size_t stage );

        // THE SCENE'S HALF, and it runs on EVERY Init() including the first. Tells the engine systems the world
        // they accumulated over is gone and rebuilds the graph. The renderer holds nothing of the scene's to
        // release: the editor's extension passes (grid, colliders, gizmo overlays) live on the Scene and are read
        // from the scene each BeginScene hands over, so a different scene's passes cannot outlive it here.
        //
        // What is deliberately NOT here: any reset of the engine systems' own state. They keep it across a
        // scene load for the same reason they keep it across a frame — every per-frame input is RESTATED by
        // its producer, absence included (SkyboxECSSystem emits an explicit "no sky" command rather than
        // emitting nothing), and everything expensive enough to be cached across frames is keyed on a
        // fingerprint of the content it was built from rather than on "have I built one". Those two
        // properties are what makes a render system survivable, and they are asserted in
        // Desert/Tests/Engine/RendererSceneLifetime rather than left as a claim.
        void RebindScene();

        // The overlay phases' targets after the temporal resolve (ViewTargetSet::Output): the resolved colour, an
        // overlay velocity (never read) and the overlay depth PopulateSceneDepth filled. Invalid: no resolve.
        struct OverlayTargets
        {
            RDG::TextureRef    Color;
            RDG::TextureRef    Velocity;
            RDG::TextureRef    ResponsiveAA; // never read; cleared by its first overlay writer (OwnClears)
            RDG::TextureRef    ThinGeometryCoverage; // never read; cleared by its first overlay writer (OwnClears)
            RDG::TextureRef    Depth;
            [[nodiscard]] bool IsValid() const
            {
                return Color.IsValid();
            }
        };
        // Adds @p passes, one raster node each, in the given order; consecutive passes on one framebuffer share
        // one render pass (the first on each framebuffer CLEARS iff @p clearFirst, with the pass's clear values).
        // @p overlay: when valid, the OUTPUT-extent target set the passes drawing on the scene target draw into
        // instead of it (colour 0, the velocity slot and the depth replaced, no resolves; same formats and one
        // sample, so the render pass stays compatible with the one their pipelines were built against) - the
        // overlays after the temporal resolve.
        void AddSystemRasters( RDG::Builder& graph, FrameTextures& textures,
                               std::span<const SystemRasterPass> passes, bool clearFirst,
                               const OverlayTargets& overlay = {} );
        // The systems' opaque raster, called in this order by OnUpdate: the mesh renderer's cascade depths (each
        // clears its cascade), the scene target's sky + mesh geometry + terrain (the sky clears, the rest load),
        // the outline mask (clears).
        void AddFrameShadowDepths( RDG::Builder& graph, FrameTextures& textures );
        // The directional lights' cascade depths (MeshRenderer::ShadowCascadePasses); declared only when m_FrameCascades.
        void AddFrameSunCascades( RDG::Builder& graph, FrameTextures& textures, System::MeshRenderer& mesh );
        // The one-texel far-plane cascade array a view binds when no cascade is drawn this frame ("CSM.None").
        RDG::TextureRef AddFrameNoSunCascades( RDG::Builder& graph );
        // This frame's sun shadow is "Shadows: Ray Traced Sun": the Shadows method resolved to HardwareTraced AND
        // this view-frame's TLAS was built (OnUpdate lowers it with LowerShadowsForFrame before any pass is
        // declared, so every reader of one frame sees one answer). The composite's and the opaque generic
        // programs' DESERT_SUN_SHADOW_SCREEN_MASK permutations read the mask.
        [[nodiscard]] bool SunShadowIsRayTraced() const;
        // "Shadows: Ray Traced Sun" over the G-buffer, then the DENOISE-CORE ShadowVisibility signal (SourceAngle,
        // closest occluder): the composite's screen shadow mask. Invalid (logged) only when the trace pass itself
        // could not be declared; its readers then bind System.White (the sun unshadowed that frame).
        RDG::TextureRef AddFrameRayTracedSunShadow( RDG::Builder& graph, FrameTextures& textures,
                                                    const std::vector<RDG::TextureRef>& gbuffer,
                                                    const glm::vec3& directionToSun, const ViewFrame& frame );
        bool            EnsureRayTracedShadowResources();
        // This frame's local lights are MegaLights' (the Shadows method resolved to Stochastic, its sun ray
        // traced): the composite's MegaLights program reads AddFrameMegaLights' lighting in place of the
        // light-grid loop.
        [[nodiscard]] bool LocalLightsAreStochastic() const;
        // This frame's point/spot shadows are ray traced (Shadows resolved to HardwareTraced, not path traced):
        // "Shadows: Ray Traced Local" per slot, read by the composite's RayTracedLocal program.
        [[nodiscard]] bool LocalShadowsAreRayTraced() const;
        // THE ONE ANSWER TO "does a program of this frame read the point/spot shadow maps" while the composite
        // reads the ray-traced masks: a mesh forward draw (MeshRenderer::LocalShadowMapReadersQueued), the forward
        // terrain (TerrainRenderer::ForwardReadsTexture) or a view emitter's sprite (ParticleRenderer::
        // ViewEmittersSample) whose program samples LocalShadowAtlasTexture. OnUpdate draws the maps only when true.
        [[nodiscard]] bool LocalShadowMapsRead() const;
        // The same derivation for the sun's cascades (ShadowCascadesTexture) while the sun is a screen mask.
        [[nodiscard]] bool SunCascadesRead() const;
        // Every point/spot light that casts in this view gets a ShadowMaskSlot (and a record of
        // m_LocalShadowMaskLights) when LocalShadowsAreRayTraced and its attenuation sphere meets @p view (the
        // frame's unjittered frustum), every other -1. After UpdateLocalShadows.
        void AssignLocalShadowMaskSlots( const Core::Frustum& view );
        // Per slot: trace, denoise (history per light, m_LocalShadowDenoisers) and pack into the frame's 2D-array
        // mask (one White layer when no slot). Invalid (logged) only when the array could not be filled at all;
        // the composite then draws its sun-mask program (the lights' shadow maps, if drawn, or unshadowed).
        RDG::TextureRef AddFrameRayTracedLocalShadows( RDG::Builder& graph, FrameTextures& textures,
                                                       const std::vector<RDG::TextureRef>& gbuffer,
                                                       const ViewFrame&                    frame );
        // MegaLights (System::MegaLightsRenderer) over the G-buffer and the view's light grid and TLAS: the
        // denoised, demodulated local lighting, diffuse and specular. Both invalid when the view has no local
        // light (nothing to shade), or - logged - when the passes could not be declared; the composite then binds
        // System.Black (no local light that frame).
        System::MegaLightsLighting AddFrameMegaLights( RDG::Builder& graph, FrameTextures& textures,
                                                       const std::vector<RDG::TextureRef>& gbuffer,
                                                       const ViewFrame&                    frame );
        bool                       EnsureMegaLightsResources();
        // The MegaLights translucency volume (System::MegaLightsVolumeRenderer) into FrameTransients::
        // MegaLightsVolume, sliced over @p lightGrid's depth range: on under Stochastic with a local light, the
        // neutral block (System::AddMegaLightsVolumeOff) otherwise or - logged - when its pass could not be
        // declared.
        void AddFrameMegaLightsVolume( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame,
                                       const LightGridParamsPayload& lightGrid );
        // This frame's sun shadow is the virtual shadow map (the Shadows method resolved to VirtualMaps).
        [[nodiscard]] bool SunShadowIsVirtual() const;
        // THE ONE ANSWER TO "is the sun shadowed from a screen mask this frame" (the ray-traced sun or the virtual
        // shadow map, both ShadowMask.Sun): every mask READER asks it - the composite's and the opaque generic
        // programs' DESERT_SUN_SHADOW_SCREEN_MASK permutations, the depth/normal prepass, the cascade budget (the
        // low-detail cascades kept for the blended ShadowCascadesTexture readers). The producers ask their own method.
        [[nodiscard]] bool SunShadowIsScreenMask() const;
        // "Shadows: Virtual Shadow Map" (System::VirtualShadowMapRenderer): the page passes over this view's
        // G-buffer, then "VSM: Project" (SMRT inside the sun's SourceAngle) into the screen mask. Invalid (logged)
        // when the passes could not be declared; the readers then bind System.White, as for the ray-traced sun.
        RDG::TextureRef AddFrameVirtualSunShadow( RDG::Builder& graph, FrameTextures& textures,
                                                  const std::vector<RDG::TextureRef>& gbuffer,
                                                  const glm::vec3& directionToSun, const ViewFrame& frame );
        bool            EnsureVirtualShadowResources();
        // GPU-DRIVEN CULL, only under DebugViewState::ValidateGpuCulling (the drawing stays on the CPU path): the
        // InstanceCulling phases over the main view (two-phase FURTHEST-HZB occlusion against m_CullHzbHistory),
        // the sun's cascades (m_FrameCascades) and the VSM clipmap levels (m_FrameVsmPages), then this frame's
        // furthest HZB extracted into m_CullHzbHistory. A refusal is logged once per message and skips the frame.
        void AddFrameGpuCull( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame );
        void AddFrameBasePass( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameSilhouette( RDG::Builder& graph, FrameTextures& textures );
        // One raster node for @p pass on @p target (its whole attachment set, or @p overlay's), with the pass's
        // declared reads; @p color / @p depth are the loads of the node's targets.
        void AddPassNode( RDG::Builder& graph, FrameTextures& textures, const SystemRasterPass& pass,
                          const std::shared_ptr<Framebuffer>& target, const std::string& debugName,
                          const RDG::LoadOp& color, const RDG::LoadOp& depth, const OverlayTargets& overlay );
        // One system raster pass: one LOAD node on the pass's own target (or @p overlay's set), placed by the
        // position of this call. Nothing when the pass has no target (the system did not initialize).
        void AddSystemRaster( RDG::Builder& graph, FrameTextures& textures, const SystemRasterPass& pass,
                              const OverlayTargets& overlay = {} );
        // The frame's translucency, in draw order by call order: the height fog apply, the cloud composite (far
        // field), the particles, then the AfterTranslucency extension point.
        void AddFrameTranslucency( RDG::Builder& graph, FrameTextures& textures );
        // The passes registered at @p point on this frame's scene (m_FrameExtensions), in registration order: one
        // LOAD raster node each on the scene target (or @p overlay's set after the temporal resolve), placed by
        // the position of this call. The frame build calls it once per point, at that point's place.
        void AddExtensionPoint( RDG::Builder& graph, FrameTextures& textures, RDG::ExtensionPoint point,
                                const OverlayTargets& overlay );
        // Makes m_TemporalUpscaler the implementation of @p method (kept when it already is; null for None).
        void EnsureTemporalUpscaler( TemporalMethod method );
        // The frame's temporal resolve, after the translucency: registers the view's histories and adds
        // the upscaler's node on the scene colour, the scene depth, the resolved velocity and @p exposure (last
        // frame's adapted luminance). Returns the temporal output - the colour every later node reads and the
        // overlays draw into; invalid when the frame has no temporal method or the node was refused
        // (logged with the reason), and the frame then continues on the scene colour.
        //
        // With the resolve it returns the OVERLAY TARGET SET at the output extent: Color = the temporal output,
        // Velocity and Depth = transients the "Scene: PopulateSceneDepth" node fills from the render-extent scene
        // depth (at every scale, 100 % included). A frame the resolve cannot run on (a multisampled scene target,
        // no PopulateSceneDepth pipeline, the upscaler's refusal) is RENDERED WITHOUT IT: ERROR "rendered without
        // the temporal resolve this frame: <why>", an invalid set, and the caller draws the overlays into the
        // scene target and post-processes the scene colour.
        // @p opaqueLuma: what the method's AddOpaquePasses returned this frame (TemporalUpscalerInputs::OpaqueLuma).
        OverlayTargets AddFrameTemporal( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame,
                                         RDG::TextureRef exposure, RDG::TextureRef opaqueLuma );
        // AutoExposure's build-time step (Prepare advances its ping-pong), taken before the temporal node so that
        // node and the exposure nodes agree on which image is last frame's: returns the imported previous
        // adapted luminance ("AutoExposure.Previous"); invalid when no exposure runs this frame.
        RDG::TextureRef PrepareFrameAutoExposure( FrameTextures& textures );
        // Exponential height fog: the closed-form COMPUTE evaluation. Called between the deferred block
        // and the translucency (AddFrameTranslucency) — the one point in the frame where the scene depth is
        // finished in BOTH paths and no render pass is open (an in-frame dispatch inside one is illegal). Its
        // apply is the first node of AddFrameTranslucency, BELOW the particles, so they composite over the fogged
        // scene. When Sky Phase 3 lands, this pass composes fog OVER the aerial perspective (UE's order).

        // The cloud march and its noise bake. Issued immediately after the atmospheric fog: both are
        // in-frame compute and must be outside an open render pass, and by that point the scene depth is
        // final and this frame's atmosphere LUTs have been filled. The composite itself is the second node of
        // AddFrameTranslucency, ABOVE the fog and BELOW the particles.
        // The cloud layer's shadow on the WORLD, which is a different pass at a different point in the
        // frame from the march above and belongs to a different consumer. Issued BEFORE the render graph
        // records, because the deferred lighting pass reads it and runs immediately after the graph. It
        // depends on nothing the frame produces — no scene depth, no G-buffer, no atmosphere LUT — so
        // nothing forces it later, and its consumer forces it earlier.

        // Frame assembly: one graph pass each, moved out of OnUpdate (SceneRendererFrame*.cpp).
        void AddFrameGBuffer( RDG::Builder& graph, FrameTextures& textures, System::MeshRenderer* meshRenderer );
        void AddFrameTerrainGBuffer( RDG::Builder& graph, FrameTextures& textures );
        // "Deferred: Forward Depth/Normal": the generic queue's opaque draws into G-buffer depth + normal.
        void AddFrameForwardDepthNormal( RDG::Builder& graph, FrameTextures& textures,
                                         System::MeshRenderer* meshRenderer );
        // @p sunShadowMask: the ray-traced sun's screen mask (invalid when the sun is not ray traced), bound where
        // a generic program's DESERT_SUN_SHADOW_SCREEN_MASK permutation reads it (SceneViewInputs.SunShadowMask).
        // @p localShadowMasks: the ray-traced local shadows' masks (invalid when not traced), likewise for
        // DESERT_LOCAL_SHADOW_SCREEN_MASK (SceneViewInputs.LocalShadowMasks).
        void AddFrameGeneric( RDG::Builder& graph, FrameTextures& textures, System::MeshRenderer* meshRenderer,
                              RDG::TextureRef sunShadowMask, RDG::TextureRef localShadowMasks );
        // THE TRANSLUCENT FRONT LAYER of the frame (UE FFrontLayerTranslucencyData, Reflections.TranslucentFrontLayer):
        // the nearest translucent surface's normal + roughness, motion and depth ("Translucency: Front Layer G-buffer",
        // Mesh/Surface/Pass_FrontLayer.glslh), graph transients of the frame - the surface AddFrameSSR's front layer
        // renderer starts its rays from.
        struct FrontLayerSurface
        {
            RDG::TextureRef Normal;
            RDG::TextureRef Velocity;
            RDG::TextureRef Depth;
        };
        // What the glass reads of it: the denoised reflection and the depth it belongs to. Invalid both: no front layer.
        struct FrameFrontLayer
        {
            RDG::TextureRef Reflection;
            RDG::TextureRef Depth;
        };
        // @p sceneCopy: this frame's scene snapshot (FrameTransients::SceneColorCopy); invalid -> no glass node.
        void AddFrameGlass( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef sceneCopy,
                            System::MeshRenderer* meshRenderer ,
                           const FrameFrontLayer& frontLayer );
        // The water surface (WATER-W6), into the scene target with depth, after the scene copy and before the
        // glass.
        void AddFrameWater( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef sceneCopy );
#if DESERT_DEV_INSTRUMENTS
        void AddFrameOverdraw( RDG::Builder& graph, FrameTextures& textures );
        // The Velocity view mode (DeferredDebugMode::Velocity, Deferred only): "Debug: Velocity" draws the view's
        // velocity over @p target, the post input, after the temporal resolve.
        void AddFrameVelocityView( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef target );
        // The surface cache view modes (DeferredDebugMode::SurfaceCacheAtlas / SurfaceCacheScene / SceneLit,
        // Deferred only): "Debug: Surface Cache Atlas" or "Debug: Lumen Scene" over @p target, from the cache
        // AddFrameSurfaceCache brought up in this frame.
        void AddFrameSurfaceCacheView( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame,
                                       RDG::TextureRef target );
        // "Debug: LightGridWorld" — the world light grid's count per surface, over the post input.
        void AddFrameLightGridWorldView( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef target,
                                         const ViewFrame& frame );
        // The Global Distance Field view mode (DeferredDebugMode::GlobalDistanceField, Deferred only): draws
        // "Debug: Global Distance Field" over @p target from the field AddFrameGlobalDistanceField brought up.
        void AddFrameGlobalDistanceFieldView( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame,
                                              RDG::TextureRef target );
#endif // DESERT_DEV_INSTRUMENTS
        void AddFrameClearMainFramebuffer( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameDepthResolve( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameSceneDepthResolve( RDG::Builder& graph, FrameTextures& textures );
        // The shader resolves of the scene target's SampleZero graph colours ("Velocity: Resolve", MSAA only).
        void AddFrameGraphColorResolves( RDG::Builder& graph, FrameTextures& textures );
        // THE PATH-TRACED FRAME (PathTraced( m_RenderMethods ), after the G-buffer): System::PathTracer's restart,
        // trace and resolve over the scene colour, fed this frame's TLAS, hit groups, camera, GI record, lights and
        // environment, and the hash of what the accumulation integrated (PathTracingSceneHash). An error names
        // what was refused.
        [[nodiscard]] Common::BoolResultStr AddFramePathTracing( RDG::Builder& graph, FrameTextures& textures,
                                                                 const ViewFrame& frame );
        // What a path-traced view's sum stands on besides the camera (UE: the path tracer's invalidation on any
        // scene change): the TLAS rows, the lights, the GI record (sun, sky look), the environment, the hit
        // groups' templates, records and material rows, and the material service's invalidations. @p environment:
        // GetEnvironmentIdentity().
        [[nodiscard]] uint64_t PathTracingSceneHash( const GlobalIllumination::GIFramePayload& giFrame,
                                                     uint64_t environment, uint64_t emissive,
                                                     const CloudShadowUniforms& cloudShadow ) const;
        // THE SCENE'S EMISSIVE TRIANGLE LIST in this frame's graph (RayTracing::EmissiveLights.hpp): built from
        // this frame's TLAS instances and segment records (a built m_RayTracingScene), each segment's emission
        // estimated from its material's effective row; m_EmissiveLights keeps the static part on the GPU and
        // rebuilds it when its hash moves, and expands the skinned primitives' segments every frame from the skin
        // cache's positions (RayTracingPrimitive::DeformedPositions). Every consumer of emissive lights reads the
        // frame this returns. @p emissive in PathTracingSceneHash: its Hash.
        [[nodiscard]] Common::ResultStr<RayTracing::EmissiveLightsFrame>
        AddFrameEmissiveLights( RDG::Builder& graph );
        // The view's path tracer system on first use (latched like EnsureSSRResources); false lowers the frame
        // (LowerPathTracingForFrame).
        bool EnsurePathTracerResources();

        // The scene sample count the device can run for `requested` (the method's effective count).
        // Recreates the scene target at `samples` when it differs (an anti-aliasing change), next frame.
        void ApplySceneSampleCount( uint32_t samples );
        void AddFrameSSAO( RDG::Builder& graph, FrameTextures& textures,
                           const std::vector<RDG::TextureRef>& gbuffer, const ViewFrame& frame );
        // The view's lights as graph buffers and its light grid ("LightGrid" compute node), into
        // FrameTransients::Lights — before the first lit node, on both paths.
        // Returns the grid's parameters (a default payload, logged, when the view has no DeferredLightingSystem).
        LightGridParamsPayload AddFrameLightGrid( RDG::Builder& graph, FrameTextures& textures,
                                                  const ViewFrame& frame );
        // The WORLD layout of the same lights ("LightGridWorld" compute node: the radiance cache's clipmap cells
        // around the camera), into FrameTransients::Lights.World* — only when WorldLightGridRead().
        void AddFrameLightGridWorld( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame );
        // Whether a node of this frame reads the world light grid: the radiance cache's trace (a traced GI method
        // lights its hits with it) and the "Light Grid (World)" view mode (Deferred).
        [[nodiscard]] bool WorldLightGridRead() const;
        // Whether this frame updates and reads the world radiance cache: the GI pipeline (m_GIPipeline) runs it —
        // either tracer — on the Deferred path (its reader is the deferred composite).
        [[nodiscard]] bool RadianceCacheRuns() const;
        // The screen probe gather runs this frame with its short-range AO: its contact occlusion replaces SSAO,
        // which is then not computed at all (UE: SSAO is off under Lumen, r.Lumen.DiffuseIndirect.SSAO 0).
        [[nodiscard]] bool ScreenProbeAOReplacesSSAO() const;
        // Whether a node of this frame reads the global distance field: the GI tracer when it is the software walk
        // (GlobalIllumination::TracerReadsGlobalDistanceField), the debug view.
        [[nodiscard]] bool GlobalDistanceFieldRead() const;
        // Whether this frame brings the surface cache up: the GI pipeline reads it (m_GIPipeline.SurfaceCache),
        // the traced reflections read it (ReflectionsTraced), or one of its view modes is on. Deferred only.
        [[nodiscard]] bool SurfaceCacheRuns() const;
        // The surface cache this view reads - the lent one (ShareSurfaceCache), else its own - once initialised
        // (AddFrameSurfaceCache brings it up); null before.
        [[nodiscard]] SurfaceCache::SurfaceCacheRenderer* ReadSurfaceCache() const;
        // Whether this frame's reflections are traced (ReflectionMethod::SoftwareTraced or HardwareTraced): SSR's
        // pass in its traced permutation, over the tracer of ReflectionsTracer(), the surface cache and the world
        // light grid (and, for the software walk, the global distance field and the mesh SDFs). Deferred only.
        [[nodiscard]] bool ReflectionsTraced() const;
        // THE traced reflections' tracer (UE Lumen reflections: one pipeline, the world tracer chosen by the
        // method): HardwareTraced walks the view's TLAS by ray query, SoftwareTraced the distance fields; hits lit
        // from the surface cache either way. Only meaningful when ReflectionsTraced().
        [[nodiscard]] GlobalIllumination::Pipeline ReflectionsTracer() const;
        // Whether this frame's traced reflections are HIT-LIT (UE Lumen reflections, hit lighting): the effective
        // method is HardwareTraced, the quality's Reflections.HitLighting is on and the device offers the ray
        // tracing pipeline. A request the device cannot honour is reported once (m_ReportedHitLightingDevice) and
        // the reflections stay on the surface cache.
        [[nodiscard]] bool ReflectionsHitLit() const;
        // GRAPH BUILD of a hit-lit frame: m_ReflectionHitGroups from the view's ray tracing scene - each segment's
        // surface template and effective row (RayTracingHitGroupTable.hpp). An error: no default surface, or the
        // two sides of the per-segment table disagree.
        [[nodiscard]] Common::BoolResultStr BuildReflectionHitGroups();
        // This frame's TLAS custom index (GPU-scene row) -> surface cache instance-table row (contract B3), the
        // hardware tracer's GIInstanceSurfaceCacheRows: made by the first reader of the frame, kept in
        // m_TracedFrameShared. Invalid with no surface cache or no TLAS.
        [[nodiscard]] RDG::BufferRef AddFrameInstanceSurfaceCacheRows( RDG::Builder& graph );
        // This frame's GI record for a G-buffer of @p extent: the payload (also what the path tracer hashes), and
        // the payload uploaded as "GIFrameUB".
        [[nodiscard]] GlobalIllumination::GIFramePayload MakeFrameGIPayload( const ViewFrame& frame,
                                                                             const glm::vec4& lightDir,
                                                                             const glm::vec4& lightColor,
                                                                             glm::uvec2       extent );
        [[nodiscard]] RDG::BufferRef AddFrameGIRecord( RDG::Builder& graph, const ViewFrame& frame,
                                                       const glm::vec4& lightDir, const glm::vec4& lightColor,
                                                       glm::uvec2 extent );
        // The traced permutation's reads of SSR (System::SSRRenderer::TracedReads) for this frame; nullopt
        // (logged): a read is missing and the frame reflects the screen alone.
        [[nodiscard]] std::optional<System::SSRRenderer::TracedReads>
        MakeTracedReflectionReads( RDG::Builder& graph, FrameTextures& textures, System::SSRRenderer& ssr,
                                   const std::vector<RDG::TextureRef>& gbuffer, const ViewFrame& frame );
        // The tracer of the surface cache's shadow rays: the GI pipeline's when it traces, otherwise the software
        // walk.
        [[nodiscard]] GlobalIllumination::Pipeline SurfaceCacheTracer() const;
        // The active tracer's reads of this frame for @p pipeline (GlobalIllumination::TracerFrame).
        [[nodiscard]] GlobalIllumination::TracerFrame
        MakeTracerFrame( RDG::Builder& graph, const FrameTextures& textures,
                         const GlobalIllumination::Pipeline& pipeline );
        // The surface cache of this frame - only when SurfaceCacheRuns(), after the G-buffer, the shadow depths,
        // the light grids and the global distance field, before the traced GI: the Lumen scene update, the card
        // captures and their copy, the cards' direct lighting, into m_SurfaceCacheFrame.
        void AddFrameSurfaceCache( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame );
        // Brings the view's global distance field up to this frame's camera and meshes, into
        // FrameTransients::GlobalDistanceField* — only when GlobalDistanceFieldRead().
        void AddFrameGlobalDistanceField( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame );
        // The traced GI of this frame — only when RadianceCacheRuns(), after the G-buffer: the GI frame record and
        // the tracer's frame (GlobalIllumination::TracerFrame), the world radiance cache's update
        // (GlobalIllumination::WorldRadianceCache), then the final gather that reads it — the screen probe gather
        // (GlobalIllumination::ScreenProbeGather) or, under ProbeVolume, the irradiance field gather
        // (GlobalIllumination::IrradianceFieldGather) — into FrameTransients::GIDiffuse / GISpecular.
        // The screen traces' shared inputs of this frame (UE: the view's ClosestHZB and PrevSceneColor): the
        // closest-depth hierarchy of the G-buffer depth and the view's scene colour history
        // (SceneColorHistoryRenderer), imported; SceneHistoryReadable: whether it holds the previous frame. All
        // invalid when no screen trace runs (EnsureScreenTraceResources failed or nobody asked).
        struct FrameScreenTrace
        {
            RDG::TextureRef HZB;
            RDG::TextureRef SceneHistory;
            bool            SceneHistoryReadable = false;
        };
        FrameScreenTrace AddFrameScreenTrace( RDG::Builder& graph, FrameTextures& textures,
                                              const ViewFrame& frame );
        void             AddFrameTracedGI( RDG::Builder& graph, FrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& gbuffer, const glm::vec4& lightDir,
                                           const glm::vec4& lightColor, const FrameScreenTrace& screenTrace,
                                           const ViewFrame& frame );
        // The pending reflection capture's cube, written in this view's graph with its capture point measured by
        // the view's diffuse estimator (ReflectionCaptureLighting): the world radiance cache when @p tracedGI and
        // this frame's AddFrameTracedGI produced one, otherwise the sky's irradiance cube.
        void AddFrameReflectionCaptureUpdate( RDG::Builder& graph, FrameTextures& textures, bool tracedGI );
        void AddFrameComposite( RDG::Builder& graph, FrameTextures& textures,
                                const std::vector<RDG::TextureRef>& gbuffer, RDG::TextureRef ssr,
                                RDG::TextureRef sunShadowMask, RDG::TextureRef localShadowMasks,
                                const System::MegaLightsLighting& megaLights,
                                System::MeshRenderer* meshRenderer, const glm::vec4& lightDir,
                                const glm::vec4& lightColor, const ViewFrame& frame,
                                RDG::TextureRef subsurfaceDiffuse );
        // SSS-PROFILE (CONTRACT §5): whether this view scatters - SubsurfaceScattering on with a Scale above 0, the
        // Lit view, a profile registered and the SubsurfaceRenderer built.
        [[nodiscard]] bool SubsurfaceRuns() const;
        // "Subsurface: Burley" + "Subsurface: Recombine" over the composite's @p subsurfaceDiffuse (its split).
        void AddFrameSubsurface( RDG::Builder& graph, FrameTextures& textures,
                                 const std::vector<RDG::TextureRef>& gbuffer, RDG::TextureRef subsurfaceDiffuse,
                                 const ViewFrame& frame );
        // The scene snapshot (UE: CreateTexture from the scene colour's desc, copied by a raster node): published
        // as FrameTransients::SceneColorCopy and returned; invalid when no copy was made (no copy system, no scene
        // colour, its desc refused - logged). @p into: the imported image to copy into (the view's scene
        // colour history, which next frame's screen traces read), or invalid for a per-frame transient; @p
        // recorded runs in the copy's exec once the copy was recorded (the history's stamp), may be empty.
        static RDG::TextureRef AddFrameSceneCopy( RDG::Builder& graph, FrameTextures& textures,
                                                  const std::vector<RDG::TextureRef>& sceneColor,
                                                  System::CopyRenderer* copy, RDG::TextureRef into,
                                                  std::function<void()> recorded );
        // The closest-depth hierarchy of the G-buffer depth (HZBRenderer), invalid when it was not built.
        RDG::TextureRef AddFrameHZB( RDG::Builder& graph, FrameTextures& textures );
        // SSR's trace + denoiser over @p screenTrace, before the lighting. Resolved: the reflection the lighting
        // composes (SsrTexture); invalid when SSR did not run.
        // Reflections.TranslucentFrontLayer of the resolved quality.
        [[nodiscard]] bool TranslucentFrontLayerReflections() const;
        // The front layer's G-buffer, then its reflections through the "FrontLayerReflectionsSystem" (an SSRRenderer
        // tracing from TranslucentFrontLayer: the same tracer, hit lighting and denoiser as the opaque reflections).
        // Empty: no translucent object is visible, or the reflections could not run (said by the step that refused).
        FrameFrontLayer AddFrameTranslucentFrontLayer( RDG::Builder& graph, FrameTextures& textures,
                                                       System::MeshRenderer*               meshRenderer,
                                                       const std::vector<RDG::TextureRef>& gbuffer,
                                                       const FrameScreenTrace& screenTrace, const ViewFrame& frame );
        bool m_FrontLayerReflectionsFailed = false;

        struct FrameSSR
        {
            RDG::TextureRef Resolved;
        };
        FrameSSR AddFrameSSR( RDG::Builder& graph, FrameTextures& textures, System::SSRRenderer& ssr,
                              const std::vector<RDG::TextureRef>& gbuffer, const FrameScreenTrace& screenTrace,
                              const ViewFrame& frame ,
                              const FrontLayerSurface* frontLayer = nullptr );
        void AddFrameParticlesSimulate( RDG::Builder& graph );
        // MESH-PB1: imports this frame's scene/view inputs (CSM cascades, the environment's cubes, the BRDF LUT)
        // into FrameTransients before any node is added; what is absent stays invalid (SceneViewInputsOf).
        void ImportSceneViewTextures( RDG::Builder& graph, FrameTextures& textures );
        // The sun's cascade array (or the far-plane stand-in) and the local lights' atlas, imported once per frame
        // AFTER the frame's sun-shadow path is decided; the cascade budget was taken at the head of the frame
        // (RebudgetShadowsFor).
        void ImportViewShadowMaps( RDG::Builder& graph, FrameTextures& textures );
        // Re-budgets the sun's cascades when the quality generation or the view's RESOLVED sun path the budget was
        // taken for moved (ShadowReallocation): a screen-mask sun keeps only the low-detail cascades, CascadedMaps
        // the full ones. Called at the head of OnUpdate, before any shadow view of the frame is planned.
        void RebudgetShadowsFor( const Common::Scalability::ResolvedQuality& quality, bool sunRayTraced );
        void AddFrameCloudShadowMap( RDG::Builder& graph, FrameTextures& textures );
        // A path-traced frame: the cloud layer's sky-occlusion volume alone
        // (VolumetricCloudRenderer::DeclareSkyOcclusionNodes).
        void AddFrameCloudSkyOcclusion( RDG::Builder& graph, FrameTextures& textures );
        // The sky light, at the head of the graph: the atmosphere nodes that depend on the sky alone (the cached
        // LUT pair, the distant sky light), then this frame's slice of the sky capture, which marches them. Before
        // ImportSceneViewTextures, so the environment it imports is the one this frame's capture completed.
        void AddFrameSkyLight( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameSkyAtmosphereLuts( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameAtmosphericFog( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameVolumetricClouds( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame );
        // The selection outline composited over @p scene (the post input: the temporal resolve's output with the
        // overlays, or the scene colour). Returns what the tonemap reads - the outlined scene - or an invalid ref
        // when the outline system cannot record this frame (the tonemap then reads @p scene).
        [[nodiscard]] RDG::TextureRef AddFrameJumpFlood( RDG::Builder& graph, FrameTextures& textures,
                                                         RDG::TextureRef scene );
        // @p previous: PrepareFrameAutoExposure's result (nothing is added when it is invalid).
        void AddFrameAutoExposure( RDG::Builder& graph, FrameTextures& textures,
                                   const std::vector<RDG::TextureRef>& sceneColor, RDG::TextureRef previous );
        // UE Local Exposure (Bilateral): Setup (grid + 1/32 log luminance) then BlurH/BlurV of the latter, into
        // FrameTransients::LocalExposureGrid / BlurredLogLuminance. Nothing is added at identity settings.
        void AddFrameLocalExposure( RDG::Builder& graph, FrameTextures& textures,
                                    const std::vector<RDG::TextureRef>& sceneColor );
        // Fusion half of AddFrameLocalExposure (UE 5.4 AddLocalExposureFusionPass): sets
        // Transients.ExposureFusion.
        void AddFrameLocalExposureFusion( RDG::Builder& graph, FrameTextures& textures,
                                          System::LocalExposureRenderer& le,
                                          const System::TonemapRenderer& tonemap, RDG::TextureRef scene,
                                          uint32_t width, uint32_t height );
        void AddFrameBloom( RDG::Builder& graph, FrameTextures& textures,
                            const std::vector<RDG::TextureRef>& sceneColor );
        void AddFrameLightShafts( RDG::Builder& graph, FrameTextures& textures,
                                  const std::vector<RDG::TextureRef>& sceneColor,
                                  const std::shared_ptr<FrameValues>& values );
        void AddFrameLensFlare( RDG::Builder& graph, FrameTextures& textures,
                                const std::vector<RDG::TextureRef>& sceneColor,
                                const std::shared_ptr<FrameValues>& values );
        // @p source: the colour the post chain reads (the temporal output, or the scene colour without one).
        void AddFrameTonemap( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef source );
        void AddFrameFXAA( RDG::Builder& graph, FrameTextures& textures );
        void AddFrameSMAA( RDG::Builder& graph, FrameTextures& textures );
        // The backdrop pyramid the UI samples (invalid when the blur is not recorded this frame).
        RDG::TextureRef AddFrameBackdropBlur( RDG::Builder& graph, FrameTextures& textures,
                                              const std::vector<RDG::TextureRef>& sceneColor );

    private:
        struct
        {
            Core::Camera* ActiveCamera = nullptr;
        } m_SceneInfo;

        // The atmosphere sun light's render-effect slice, refreshed by SetProceduralSky each frame; the
        // post chain reads it to run (or zero out) the light shafts.
        SunLightFx m_SunLightFx;

        ShaderProtocols::DirectionLight m_DirectionLights;
        // This frame's atmosphere light 1 (the moon) as the deferred pass shades it: the scene's value times
        // AtmosphereEnv::Light1TransmittanceAtGround (OnUpdate), or nothing.
        std::optional<AtmosphereLight1Frame> m_AtmosphereLight1;
        ShaderProtocols::PointLight     m_PointLight;
        ShaderProtocols::SpotLight      m_SpotLight;
        // What each light of m_PointLight / m_SpotLight asks of its shadow, index for index.
        std::vector<LocalLightShadowSettings> m_PointLightShadows;
        std::vector<LocalLightShadowSettings> m_SpotLightShadows;

        // Selected post-process anti-aliasing technique, taken from m_Quality each BeginScene.
        Common::Scalability::AntiAliasingMethod m_AAMode = Common::Scalability::AntiAliasingMethod::FXAA;
        // The anti-aliasing this view's frames ACTUALLY render, as ViewInputs::AntiAliasing (BeginScene).
        Common::Scalability::PathAntiAliasing m_RenderedAntiAliasing;
        // The vendor-method fallback last logged (ReplaceUnavailableVendorMethods::Why): logged when it changes.
        std::string                        m_VendorFallbackSaid;
        bool                               m_BloomEnabled = false;

        // Lens flare, refreshed from SceneSettings each BeginScene. The tint is held apart from the rest
        // because the pass never sees it — the tonemap applies it, the way the shafts' tint works.
        System::LensFlareRenderer::Params m_LensFlare;
        glm::vec3                         m_LensFlareTint = glm::vec3( 1.0f );
        // Raised by the UI canvas when it drew glass; consumed at the top of the next frame's UI extension point.
        bool                   m_BackdropBlurNeeded = false;
        bool                   m_ScenePlaying = false; // set per frame in BeginScene (hides authoring aids)
        // The extension passes of the scene BeginScene was handed: read by AddExtensionPoint while OnUpdate
        // builds the frame, cleared by EndScene. The scene owns them; null outside a BeginScene/EndScene bracket.
        const ExtensionPassRegistry* m_FrameExtensions = nullptr;
        // The reflection captures of the scene BeginScene was handed (Core::Scene owns them, one set for all its
        // views): published into this view's graph by OnUpdate, cleared by EndScene.
        ReflectionCaptureRenderer* m_FrameReflectionCaptures = nullptr;
        // The ray-tracing geometry of the scene BeginScene was handed (Core::Scene owns it, shared by its views):
        // read by this view's TLAS gather in OnUpdate, cleared by EndScene.
        RayTracing::RayTracingSceneCache* m_FrameRayTracingCache = nullptr;
        // The GPU scene of the scene BeginScene was handed (Core::Scene owns it, shared by its views): synced by
        // this view's MeshRenderer::SyncGpuScene in OnUpdate, committed after the view's graph executed, cleared
        // by EndScene.
        GpuDriven::GpuSceneRegistry* m_FrameGpuScene = nullptr;
        // The scene's local-light shadow cache, the same bracket (GetFrameLocalShadows).
        LocalShadowCache* m_FrameLocalShadows = nullptr;
        // The scene's mesh distance fields (brick atlas, records, streaming), the same bracket: what this view's
        // global distance field composites from (AddFrameGlobalDistanceField).
        DistanceField::DistanceFieldSceneData* m_FrameDistanceFields = nullptr;
#if DESERT_DEV_INSTRUMENTS
        // The gameplay debug drawing of the scene BeginScene was handed (Core/DebugDraw.hpp): this view's lines
        // and strings passes, fed the scene's store by BeginScene and emptied by EndScene. Made on first use.
        std::unique_ptr<DebugDrawRenderer> m_DebugDraw;
#endif

    public:
        // --- UI glass (backdrop blur) -----------------------------------------------------------
        // The UI canvas raises this when it recorded a glass element; the blur pyramid is then built
        // before the NEXT frame's UI extension point. Latched per frame, so a canvas that stops using glass stops
        // paying for it.
        void SetBackdropBlurNeeded( bool needed )
        {
            m_BackdropBlurNeeded = needed;
        }

    private:
        std::shared_ptr<Framebuffer> m_TargetFramebuffer;
        std::shared_ptr<Framebuffer> m_GBuffer;                    // deferred G-buffer (MRT)
        Core::RenderPath m_RenderPath = Core::RenderPath::Forward; // refreshed from SceneSettings each BeginScene
        // The volumetric cloud layer's cost ceiling, taken from m_Quality each BeginScene and handed to
        // the cloud renderer with the layer itself. HIGH is the calibrated reference, so a renderer
        // nobody pushes to renders correctly rather than cheaply.
        Common::Settings::CloudQuality m_CloudQuality = Common::Settings::CloudQuality::High;
        // WHAT THIS MACHINE CAN AFFORD. NOT read from the scene — pushed in by whoever owns the view
        // (SetQuality). See Common/Settings/MachineSettings.hpp for why it stopped being scene data.
        //
        // INITIALISED FROM THE MACHINE'S OWN ANSWER, and this is where it differs from m_DebugView beside
        // it, whose "nobody pushed to me" default is deliberately all-off. Two of these five escape into
        // GLOBAL state — RenderConfig::TextureFilter and AnisotropyLevel, which the Vulkan sampler path
        // reads off whichever thread is cooking a texture — so a renderer holding the schema defaults
        // would overwrite the user's choice for every other renderer the moment it drew a frame. The
        // offscreen thumbnail and photogrammetry previews are exactly such renderers: nobody pushes to
        // them, and before this initialiser they would have quietly reset the sampler to Trilinear/8x.
        // "Not pushed to" therefore has to mean "this machine's answer" here rather than "the defaults".
        // The RESOLVED quality (SCAL1): the only product of the scalability system a renderer reads.
        Common::Scalability::ResolvedQuality m_Quality = Common::Scalability::QualityState::Resolved();
        // What this VIEW is drawing on top of the world. NOT refreshed from the scene — pushed in by
        // whoever owns the view (SetDebugView), and "show nothing" until someone does. See
        // Graphic/DebugViewState.hpp for why it stopped being scene data.
        DebugViewState          m_DebugView;
        bool                    m_EnableSSAO    = true; // deferred SSAO pass on/off (refreshed from SceneSettings)
        // What this view renders for GI / reflections / shadows: Graphic::ResolveRenderMethods of the view's
        // request, the machine's ceiling and the device, refreshed in BeginScene. The passes read `.Effective`.
        RenderMethods           m_RenderMethods;
        // The methods the view's renderers could not build a program for on this device, an input of every
        // ResolveRenderMethods: Shadows from the DeferredLightingRenderer's initialisation, GI and Reflections
        // from their renderers' programs as each is first built (RefuseProgramMethods), PathTraced on both from
        // the path tracer's bring-up (RefusePathTracer). Every refusal stands until the shader service recompiles
        // a program (RetryRefusedProgramsAfterRecompile): the same bytes fail the same way, new bytes are tried
        // again on the next frame.
        ProgramRefusals m_ProgramRefusals;
        // The refusals that stood when the shader service last recompiled, and the
        // ShaderService::RecompileGeneration they were dropped at. A method refused again for the same program
        // and the same reason (another shader was recompiled, not this one) is NOT said again.
        ProgramRefusals m_RetriedProgramRefusals;
        std::uint64_t   m_ProgramRefusalsGeneration = 0;
        // At BeginScene, before the resolution: when the shader service has recompiled since the standing refusals
        // were made, they are moved to m_RetriedProgramRefusals, the renderers' latched failures cleared and the
        // deferred composite's refused permutations built again, so each refused method is resolved again and its
        // programs asked for again.
        void RetryRefusedProgramsAfterRecompile();
        // A program only @p methods need did not build (@p program: its name, static storage; @p error: why). Each
        // method not yet refused is added to m_ProgramRefusals with one ERROR line naming it and the program (none
        // when it is the same refusal retried after a recompile); the next BeginScene resolves the method below it
        // (MethodLimit::Program), the rest of the frame is untouched.
        void RefuseProgramMethods( std::span<const Common::Scalability::GIMethod> methods,
                                   std::string_view program, std::string_view error );
        void RefuseProgramMethods( std::span<const Common::Scalability::ShadowMethod> methods,
                                   std::string_view program, std::string_view error );
        // The DeferredLightingRenderer's refused Shadows programs (its Refusals) through RefuseProgramMethods.
        void RefuseShadowPrograms( const ProgramRefusals& deferred );
        // The path tracer (@p program: "PathTracer" or "EmissiveLightList", static storage) could not be brought
        // up: GI and Reflections PathTraced are refused together with one ERROR line (none when it is the same
        // refusal retried after a recompile); EnsurePathTracerResources is asked again once a recompile retries
        // it.
        void RefusePathTracer( std::string_view program, std::string_view error );
        void RefuseProgramMethods( std::span<const Common::Scalability::ReflectionMethod> methods,
                                   std::string_view program, std::string_view error );
        // The Reflections methods each reflection program serves. The screen trace (SSR's programs, the HZB, the
        // scene colour history) serves the three that trace the screen; the reflection captures' programs every
        // method that reads the captures (all above Off); the surface cache the two traced methods, whose hits
        // it lights.
        static constexpr std::array<Common::Scalability::ReflectionMethod, 3> kScreenTraceReflectionMethods{
             Common::Scalability::ReflectionMethod::ScreenSpace,
             Common::Scalability::ReflectionMethod::SoftwareTraced,
             Common::Scalability::ReflectionMethod::HardwareTraced };
        static constexpr std::array<Common::Scalability::ReflectionMethod, 4> kCaptureReflectionMethods{
             Common::Scalability::ReflectionMethod::Captures, Common::Scalability::ReflectionMethod::ScreenSpace,
             Common::Scalability::ReflectionMethod::SoftwareTraced,
             Common::Scalability::ReflectionMethod::HardwareTraced };
        static constexpr std::array<Common::Scalability::ReflectionMethod, 2> kTracedReflectionMethods{
             Common::Scalability::ReflectionMethod::SoftwareTraced,
             Common::Scalability::ReflectionMethod::HardwareTraced };
        // The view's TLAS (built when a resolved method traces rays; RT-SHADOWS reads GetFrame().Tlas).
        RayTracing::RayTracingSceneRenderer m_RayTracingScene;
        // Hit-lit reflections: this frame's hit group table (BuildReflectionHitGroups; the pass reads it at exec,
        // the next frame's build replaces it), the default surface's material for segments without a template,
        // and whether a hit-lighting request the device cannot run was reported.
        RayTracing::RayTracingHitGroupTable m_ReflectionHitGroups;
        // The scene's emissive triangles as lights, kept on the GPU across frames (AddFrameEmissiveLights).
        RayTracing::EmissiveLightsRenderer  m_EmissiveLights;
        std::shared_ptr<DataDrivenMaterial> m_ReflectionDefaultSurface;
        bool                                m_ReportedHitLightingDevice = false;
        // Templates already reported as shading on the default hit group (no compiled `RayTracing.Hit` cell):
        // each is warned about once per session, not per frame.
        std::vector<std::string> m_ReportedHitGroupFallbacks;
        // The view's landscape tiles in that TLAS (their BLAS at the view's raster LOD).
        RayTracing::LandscapeRayTracing m_LandscapeRayTracing;
#if DESERT_DEV_INSTRUMENTS
        // The TLAS storage size the RT scene's [ViewMemory] lines were last logged at (logged again when it
        // steps).
        uint64_t m_LoggedRayTracingTlasBytes = 0;
#endif
        // SyncGpuScene's landscape tile records, kept across frames (no per-frame allocation).
        std::vector<MotionRecord> m_ScratchTileMotions;
        // What the effective GI method runs of the traced machinery and which tracer walks its rays
        // (GlobalIllumination::PipelineFor of the effective method, the quality and the device), with
        // m_RenderMethods.
        GlobalIllumination::Pipeline m_GIPipeline;

    public:
        // This view's RT scene (instances, skips, stats) as of its last frame - the viewport's RT scene overlay.
        [[nodiscard]] const RayTracing::RayTracingSceneRenderer& GetRayTracingScene() const
        {
            return m_RayTracingScene;
        }

    private:
        float                   m_GIIntensity   = 2.0f;
        // PostProcessSettings SubsurfaceScattering / SubsurfaceScale of this view (SSS-PROFILE, CONTRACT §6).
        bool  m_SubsurfaceScattering = true;
        float m_SubsurfaceScale      = 1.0f;
        float                   m_SSRIntensity  = 1.0f;
        // World units (= centimetres). Mirrors SceneSettings::SSRMaxDistance's default; refreshed from
        // SceneSettings every frame, so this value only matters before the first Update.
        float m_SSRMaxDistance = Common::Units::Metres( 40.0f );
        // Cost knobs taken from m_Quality each BeginScene (Scalability: Reflections.MaxSteps,
        // PostProcess.AmbientOcclusionSamples). They reach their shaders as uniform
        // values, so one pipeline serves every level. The initialisers are the High column, only read before
        // the first BeginScene.
        int m_SSRMaxSteps = 32;
        // Reflections.RaysPerPixel (Scalability), refreshed per frame like m_SSRMaxSteps.
        int m_SSRRaysPerPixel = 1;
        // Reflections.DenoiserRadius (Scalability): the reflections denoiser's reconstruction kernel, taps per
        // side from the centre. High column: 2 (5x5).
        uint32_t m_ReflectionDenoiserRadius = 2;
        // GlobalIllumination.ScreenSpaceSamples (Scalability): the screen-space gather's samples per pixel.
        int m_ScreenSpaceGISamples = 12;
        // PathTracing.SamplesPerPixel / PathsPerFrame / MaxBounces / MaxPathIntensity (Scalability), taken each
        // BeginScene.
        System::PathTracer::Settings m_PathTracingSettings;
        bool                         m_PathTracerReady = false;
        // The mesh distance-field brick pool, MiB (Scalability GlobalIllumination.DistanceFieldPoolMiB): this
        // view's request to its scene's pool (DistanceFieldSceneData::BeginViewFrame) - a scene budget, counted
        // once in the scene's ledger rows, not per view.
        uint32_t m_DistanceFieldPoolMiB = 256;
        // Mesh displacement (Scalability ViewDistance.DisplacementDicingRate / DisplacementPoolMiB), handed to the
        // scene's DisplacementCache before this view requests levels. Rate 0 = runtime displacement off.
        int      m_DisplacementDicingRate = 2;
        uint32_t m_DisplacementPoolMiB    = 256;
        int m_SSAOSamples = 16;
        // Adaptive screen probes per uniform one, percent (Scalability GlobalIllumination.
        // AdaptiveProbeAllocationPercent, UE r.Lumen.ScreenProbeGather.AdaptiveProbeAllocationFraction), refreshed
        // each BeginScene and handed to the screen probe gather's layout.
        uint32_t m_ScreenProbeAdaptivePercent = 50;
        // The screen probe gather's short-range AO (Scalability GlobalIllumination.ShortRangeAO, UE
        // r.Lumen.ScreenProbeGather.ShortRangeAO), refreshed each BeginScene. On, the gather occludes its own
        // diffuse and rough specular and the composite applies no SSAO to it; off, SSAO does — one AO source
        // either way.
        bool m_ScreenProbeShortRangeAO = true;
        uint32_t m_ScreenProbeDownsample   = 16; // Scalability GlobalIllumination.ScreenProbeDownsample
        uint32_t m_ScreenProbeTracedTexels = 8;  // Scalability GlobalIllumination.ScreenProbeTracedTexels
        // Contact-shadow ray steps (Scalability: Shadows.ContactShadowSteps; 0 = the march is skipped for every
        // light) and the sun's contact shadow (ViewShadowSettings, from the elected sun), taken each BeginScene
        // and handed to the deferred composite (Common/ContactShadow.glslh).
        int   m_ContactShadowSteps                  = 12;
        // Shading.Parallax*Steps, read in BeginScene; every view's frame carries them (ViewFrame::Parallax).
        ParallaxQuality m_ParallaxQuality;
        float m_SunContactShadowLength              = 0.0f;
        bool  m_SunContactShadowLengthInWorldSpace  = false;
        float m_SunContactShadowCastingIntensity    = 1.0f;
        float m_SunContactShadowNonCastingIntensity = 0.0f;
        // The light grid's cut (Scalability: Shadows.LightGrid*), taken each BeginScene like the knobs above; the
        // initialiser is the High column.
        LightGridQuality m_LightGridQuality;
        // The world radiance cache's shape and budgets (RadianceCache::MakeRadianceCacheInputs), taken each
        // BeginScene: the world light grid is cut by its clipmaps.
        RadianceCache::RadianceCacheInputs m_RadianceCacheInputs;
        // The quality generation the shadow budget was last compared at (constructor, then BeginScene).
        uint64_t m_ShadowBudgetGeneration = 0;

        // Allocate the SSR targets + their system on FIRST USE, not in the constructor: each PreviewViewport
        // (thumbnails, the Details mesh preview) owns a SceneRenderer, and a preview never enables SSR —
        // building them up front multiplied a lot of VRAM by the preview count. Return false when the feature is
        // unavailable; the failure is latched so it is not retried each frame.
        bool EnsureSSRResources();
        // The screen traces' view resources (HZBRenderer, SceneColorHistoryRenderer) on first use by SSR or the
        // screen probe gather's screen trace; latched like EnsureSSRResources.
        bool EnsureScreenTraceResources();
        bool m_ScreenTraceResourcesReady  = false;
        bool m_ScreenTraceResourcesFailed = false;
        bool m_SSRResourcesReady  = false;
        // "Shadows: Ray Traced Sun" and its denoiser (per view: the denoiser keeps this view's history), created
        // on the first frame that resolves the sun to HardwareTraced.
        System::RayTracedSunShadowRenderer m_RayTracedSunShadow;
        System::ScreenSpaceDenoiser        m_SunShadowDenoiser{ System::DenoiserSignal::ShadowVisibility };
        // "Shadows: Ray Traced Local", this frame's slots, and one ShadowVisibility denoiser per light (keyed by
        // LocalLightShadowSettings::SourceId: slots move between frames, the history follows the light - UE keys
        // its shadow denoiser history per light too). Pruned each frame to the frame's lights.
        System::RayTracedLocalShadowRenderer                                     m_RayTracedLocalShadow;
        std::vector<System::RayTracedLocalShadowLight>                           m_LocalShadowMaskLights;
        std::unordered_map<uint32_t, std::unique_ptr<System::ScreenSpaceDenoiser>> m_LocalShadowDenoisers;
        // A pruned light's denoiser (its extracted history textures) is PARKED here, not destroyed: frames still
        // in flight may read its history; released once no frame recorded before the prune can (FrameRetireQueue).
        Assets::FrameRetireQueue<std::unique_ptr<System::ScreenSpaceDenoiser>> m_RetiredLocalShadowDenoisers;
        bool                               m_RayTracedShadowReady  = false;
        bool                               m_RayTracedShadowFailed = false;
        // MegaLights (per view: its reservoir and denoiser histories are this view's), created on the first frame
        // that resolves Shadows to Stochastic; its quality (Shadows.MegaLights*) taken each BeginScene.
        System::MegaLightsRenderer m_MegaLights;
        System::MegaLightsQuality  m_MegaLightsQuality;
        bool                       m_MegaLightsReady  = false;
        bool                       m_MegaLightsFailed = false;
        // The MegaLights translucency volume (per view: its history is this view's), created on the first frame
        // that needs it; its quality (Shadows.MegaLightsVolume*, Shadows.MegaLightsMaxHistory) taken each
        // BeginScene.
        System::MegaLightsVolumeRenderer m_MegaLightsVolume;
        System::MegaLightsVolumeQuality  m_MegaLightsVolumeQuality;
        bool                             m_MegaLightsVolumeReady = false;
        // "Shadows: Virtual Shadow Map" (per view: the page cache is this view's), created on the first frame that
        // resolves the sun to VirtualMaps; its quality (Shadows.Virtual*) taken each BeginScene.
        System::VirtualShadowMapRenderer m_VirtualShadowMap;
        System::VirtualShadowMapQuality  m_VirtualShadowQuality;
        bool                             m_VirtualShadowReady  = false;
        bool                             m_VirtualShadowFailed = false;
        // This frame draws the directional cascade array (OnUpdate's frame decision): CascadedMaps; or a screen-mask
        // sun with a cascade reader queued (MeshRenderer::SunCascadeReadersQueued), or with another directional
        // light (the moon) that casts its own cascades. Last frame's, to redraw the static caches after a gap in
        // which nothing drew them.
        bool m_FrameCascades          = true;
        bool m_CascadesDrawnLastFrame = false;
        // GPU-DRIVEN CULL (DebugViewState::ValidateGpuCulling): last frame's FURTHEST HZB of the main view, phase 1's
        // occlusion input (registered only when its size is this frame's depth size).
        RDG::ExternalTexture m_CullHzbHistory;
        GpuDriven::InstanceCulling m_GpuCull;
        // The CPU recount of the cull's Visibility words, logged per view (one frame-in-flight late).
        GpuDriven::InstanceCullValidation m_GpuCullValidation;
        // This frame's VSM page passes (AddFrameVirtualSunShadow), for the cull's VSM level views; reset before
        // the sun shadow is declared each frame, so a frame without VSM culls no level.
        std::optional<System::VirtualShadowMapFrame> m_FrameVsmPages;
        // The last cull refusal logged (AddFrameGpuCull): the same message is not logged again every frame.
        std::string m_GpuCullLastError;
        // One per-frame step that can be refused: Claim answers true for a reason that is not the one last said
        // (the caller logs it), false for the same reason again; Clear when the step records again, so a later
        // failure - even with the old reason - is said once more.
        class RepeatedRefusal
        {
        public:
            [[nodiscard]] bool Claim( std::string reason )
            {
                if ( reason == m_Last )
                    return false;
                m_Last = std::move( reason );
                return true;
            }
            void Clear()
            {
                m_Last.clear();
            }
            [[nodiscard]] const std::string& Last() const
            {
                return m_Last;
            }

        private:
            std::string m_Last;
        };
        // AddFrameTracedGI's refusals that skip the frame's GI (a frame input missing, the cache update or the
        // gather refused - ScreenProbeLayoutFits among them), and its card-lighting refusal, which does not.
        RepeatedRefusal m_TracedGIRefusal;
        RepeatedRefusal m_TracedGICardLightingRefusal;
        // The sun path the cascade budget was taken for (ShadowQualityOf): a ray-traced sun's cascades are the
        // low-detail ones. Re-budgeted when it or the quality generation moves.
        bool m_ShadowBudgetSunRayTraced = false;
        // Scalability Shadows.RayTracedSamplesPerPixel and the elected sun's SourceAngle, taken each BeginScene.
        uint32_t m_RayTracedShadowSamples = 1;
        // Shadows.RayTracedDenoiserRadius (Scalability): the ShadowVisibility reconstruction kernel. High: 2.
        uint32_t m_ShadowDenoiserRadius = 2;
        // Shadows.BlockerSearchTaps / Shadows.FilterTaps (Scalability): the sun's PCSS kernel, taken each
        // BeginScene and packed into every ShadowUB (GetShadowFilterSettings).
        SceneResources::ShadowFilterSettings                            m_ShadowFilter;
        float                                                           m_SunSourceAngleDegrees = 0.0f;
        // The elected sun's CastShadows (ViewShadowSettings::Enabled), taken each BeginScene: the GI frame record's
        // SunRadiance.w, so the path tracer and the GI hit lighting leave its rays unoccluded as the raster does.
        bool m_SunCastsShadows = true;
        bool                                                            m_SSRResourcesFailed = false;
        std::unordered_map<std::string, std::shared_ptr<IRenderSystem>> m_RenderSystems;

        // The names above, in the order they were first registered. RebindScene and ResetTemporalHistory walk
        // THIS, not the map: the map hands its systems out in hash-bucket order and holds the null entries its
        // operator[] inserts. The draw order is not this order: it is the order of the AddFrame* calls.
        std::vector<std::string>                                        m_RenderSystemOrder;
        PipelineCache                                                   m_PipelineCache;

        // Registers a system under `name`, or replaces the system already registered under it while
        // keeping its original position in the registration order.
        void TrackRenderSystem( const std::string& name, std::shared_ptr<IRenderSystem> system );
        void ForgetRenderSystem( const std::string& name );

    private:
        template <typename System, typename... Args>
        void RegisterSystem( const std::string& system, Args&&... args )
        {
            TrackRenderSystem( system, std::make_shared<System>( std::forward<Args>( args )... ) );
        }
    };
} // namespace Desert::Graphic