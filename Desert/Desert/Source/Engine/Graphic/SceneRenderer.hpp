#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Graphic/SunLightFx.hpp>
#include <Engine/Graphic/WindEnv.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/RendererSlotPool.hpp>

#include <Common/Core/Events/WindowEvents.hpp>
#include <Common/Core/EventRegistry.hpp>
#include <Common/Core/Units.hpp>
#include <Common/Settings/MachineSettings.hpp>

#include "Systems/Scene/Mesh/MeshRenderer.hpp"
#include "Systems/Scene/Skybox/SkyboxRenderer.hpp"
#include "Systems/Scene/Terrain/TerrainRenderer.hpp"
#include "Systems/Scene/PostProcessing/TonemapRenderer.hpp"
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
#include "Systems/Scene/Deferred/SSRRenderer.hpp"
#include "Systems/Scene/Deferred/GIResolveRenderer.hpp"
#include "Systems/Scene/Particles/ParticleRenderer.hpp"
#include "Systems/Scene/Clouds/VolumetricCloudRenderer.hpp"
#include "Systems/Scene/Fog/HeightFogRenderer.hpp"

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Graphic/DebugViewState.hpp>

#include <Engine/Graphic/IRenderSystem.hpp>
#include <Engine/Graphic/ExternalRenderPass.hpp>

namespace Desert::Core
{
    class Scene;
};

namespace Desert::Graphic
{
    class SceneRenderer final
    {
    public:
        struct UpdateInfo
        {
            Common::Timestep                Timestep;
            ShaderProtocols::DirectionLight DirLights;
        };

        struct RenderSubmissionExtra
        {
            std::vector<glm::mat4> BoneMatrices; // optional
            bool                   Outlined        = false;
            uint64_t               HiddenSubmeshes = 0;  // bit i = submesh i hidden (static meshes)
            int                    ForcedLOD       = -1; // -1 = auto (by distance)
            int                    LODBias         = 0;  // shifts the auto LOD (ignored when forced)
            bool                   CastShadows     = true;
            bool                   ReceiveShadows  = true;
        };

        // Each renderer LEASES a slot on construction — the index that says WHICH view is recording, so
        // per-frame state is stored per renderer instead of being overwritten by the next one
        // (EngineContext::GetActiveRendererSlot, Docs/RENDERER_FRAME_STATE.md). The lowest free slot is
        // taken, so a slot handed back by a closed view is reused; past kMaxRendererSlots the renderer
        // records into slot 0 and warns, and holds no lease to give back.
        //
        // @p shadowQuality is this renderer's directional-shadow BUDGET and is a CONSTRUCTOR argument
        // rather than a setter on purpose: MeshRenderer allocates the cascade framebuffers from it inside
        // Init(), so a value arriving afterwards would be read by nothing and look like a knob. A viewport
        // of a level takes the default; an asset preview passes Graphic::kPreviewShadowQuality, which is
        // the difference between 335 MB of shadow attachments per open window and 21 MB.
        explicit SceneRenderer( const ShadowQuality& shadowQuality = kSceneShadowQuality );
        // Returns the leased slot, so closing a view hands it back instead of using it up.
        ~SceneRenderer();

        // This renderer's shadow budget. Read by its own MeshRenderer in Initialize and fixed thereafter.
        [[nodiscard]] const ShadowQuality& GetShadowQuality() const
        {
            return m_ShadowQuality;
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
        // (Systems/RenderSystem.hpp), and the framebuffers are sized from the WINDOW. Exactly one row of
        // m_RenderSystems answers YES — the "External:" passes the editor registers against a particular
        // scene's RenderRegistry — and that row is what a rebind drops.
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

        [[nodiscard]] Common::BoolResultStr BeginScene( const Desert::Core::Scene& scene );

        void OnUpdate( const UpdateInfo& sceneRenderInfo );

        [[nodiscard]] Common::BoolResultStr EndScene();

        void Resize( const uint32_t width, const uint32_t height );

        // The slot binding is taken BY SHARED HANDLE, not by reference to the caller's storage: the
        // caller is a draw command whose recorder (an ECS component) may already be gone. See
        // Graphic::MaterialSlotBinding (A8-3).
        void SubmitMesh( const Mesh* mesh, const MaterialSlotBindingPtr& materialSlots, const glm::mat4& transform,
                         const RenderSubmissionExtra& extra );

        // Submit one terrain entity for this frame (from TerrainECSSystem via DrawTerrainCommand).
        void SubmitTerrain( const glm::mat4& transform, float size, int resolution, float heightScale,
                            float noiseFrequency, int seed, const glm::vec3& layerModes = glm::vec3( 0.0f ),
                            Image2D* splatMap = nullptr, const MaterialOverrides& overrides = {} );

        // Submit a mesh drawn with a generic data-driven material (MaterialComponent with a non-PBR shader).
        // directTexture (optional): a runtime-owned Image2D bound to `directTextureSampler`, for
        // procedural textures with no TextureAsset handle (the text SDF atlas).
        // castShadows: rasterize this draw into the shadow cascades. Off by default — see the note on
        // GenericMeshRenderData::CastShadows; only a producer that knows the draw is a solid mesh, and
        // that no OTHER draw of the same entity is already casting, may turn it on.
        void SubmitGenericMesh( const Mesh* mesh, const glm::mat4& transform, const std::string& shaderName,
                                const MaterialOverrides& overrides, bool outlined = false,
                                Image2D* directTexture = nullptr, const std::string& directTextureSampler = {},
                                bool castShadows = false );

        // v3 per-slot custom shaders: draw only @p visibleSubmeshMask submeshes of the mesh with the
        // slot's own runtime material (a MaterialService-owned DataDrivenMaterial).
        void SubmitSlotMaterialMesh( const Mesh* mesh, const glm::mat4& transform, Material* material,
                                     uint64_t visibleSubmeshMask, bool outlined = false,
                                     bool castShadows = false );

        // UE-style Instanced Static Mesh: one mesh + one PBR material drawn for every transform in
        // @p transforms. Material and transforms are both co-owned handles for the reason SubmitMesh's
        // binding is (A8-3).
        void SubmitInstancedMesh( const Mesh* mesh, const MaterialInstancePtr& material,
                                  const std::shared_ptr<const std::vector<glm::mat4>>& transforms );

        void SetEnvironment( const std::shared_ptr<MaterialSkybox>& material, float intensity = 1.0f );

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
        void SetQuality( const Common::Settings::MachineSettings& quality )
        {
            m_Quality = quality;
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

        // Procedural sky configuration (from the SkyAtmosphereComponent + the atmosphere sun, via the ECS).
        // sunDir is the direction TOWARD the sun, already normalized; bakeNow is the one-shot request from
        // the editor's Bake button.
        void SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow, const SkySettings& sky,
                               const SunLightFx& fx );

        // This frame's exponential height fog (from ExponentialHeightFogComponent, via the ECS).
        // `present` = false means the scene has no enabled fog component at all — said explicitly, because
        // the renderer keeps its settings across frames and would otherwise keep evaluating a deleted one.
        // `fogHeightY` is the fog entity's transform Y — the fog floor, owned by the transform and never
        // authored twice.
        void SetHeightFog( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY );

        // This frame's volumetric cloud layer (from VolumetricCloudComponent, via the ECS). The wind
        // offset is accumulated by the ECS system, which is where the timestep lives.
        void SetVolumetricClouds( bool present, const ECS::VolumetricCloudData& data, const glm::vec3& windOffset,
                                  const std::vector<HeroCloudInstance>& heroClouds );

        // The evaluated per-frame sky: sun direction and radiance, ambient above/below, night factor, the
        // planet radius, and an OPAQUE handle to the packed sky-parameter buffer. Consumers never see the
        // sky's authoring representation, so a change to the palette cannot break them. Mirrors
        // GetWind()/WindEnv.
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

        // How far that rebuild has got, 0..1. Meaningless unless IsCloudVolumeBaking(); see
        // System::VolumetricCloudRenderer::ModellingBakeProgress for why the wait is worth a number.
        float CloudVolumeBakeProgress() const;

        // How many SceneRenderers are alive right now. Every one of them pays for its own baked sky
        // environment, which is why the bake announces its cost with this number beside it.
        static uint32_t GetLiveRendererCount();

        const auto& GetMainCamera() const
        {
            return m_SceneInfo.ActiveCamera;
        }

        const auto& GetDirectionLights() const
        {
            return m_DirectionLights;
        }

        // Scene-global SHARED wind (authored in SceneSettings, refreshed each BeginScene). Renderers that
        // respond to wind read it from here so one direction + strength animate the whole world
        // coherently.
        //
        // IT HAS NO CONSUMER TODAY, and that is stated here rather than left to be discovered. Its only
        // reader was the procedural grass generator, which Г25 removed because grass becomes mesh assets;
        // the next reader is whatever sways an asset (instanced foliage, hair, cloth). The three
        // SceneSettings fields behind it are authored level data that EVERY scene in the repository
        // states, so Г25 raised them with the owner instead of retiring a scene-wide field on a
        // rendering task's initiative - see the report for Г25.
        const WindEnv& GetWind() const
        {
            return m_Wind;
        }

        // CSM debug: the per-cascade shadow depth maps (for the editor's cascade viewer).
        std::shared_ptr<Image2D> GetShadowCascadeImage( uint32_t cascade );
        uint32_t                 GetShadowCascadeCount();

        const std::shared_ptr<Image2D>      GetFinalImage();
        const std::shared_ptr<Framebuffer>& GetTargetFramebuffer() const
        {
            return m_TargetFramebuffer;
        }

        // Deferred G-buffer (Albedo+Metallic / Normal+Roughness / depth). Populated only in the Deferred path.
        const std::shared_ptr<Framebuffer>& GetGBuffer() const
        {
            return m_GBuffer;
        }

        // Reflective Shadow Map (a G-buffer rendered from the sun) — the bounce source for GIMode::RSM.
        // Same attachment layout as the G-buffer, at a fixed light-space resolution.
        const std::shared_ptr<Framebuffer>& GetRSMBuffer() const
        {
            return m_RSMBuffer;
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

        void RegisterRenderPass( RenderPhaseID phase, const std::string& name, std::function<void()> executeFunc,
                                 const GraphicsPipelineSpecification& pipeSpec = {} );

        // External (editor) pass injection: wraps the specification into an internal render system so
        // the pass participates in the normal graph build (phases, dependencies, pass merging).
        // Re-registering the same name replaces the previous pass; both rebuild the graph.
        void RegisterExternalPass( ExternalPassSpecification&& spec );
        void UnregisterExternalPass( const std::string& name );

        // True while the scene runs in Play mode (refreshed each BeginScene). External passes use this
        // to hide authoring aids during gameplay.
        bool IsScenePlaying() const
        {
            return m_ScenePlaying;
        }

        std::shared_ptr<Framebuffer> GetFramebufferForPhase( RenderPhaseID phase );
        std::shared_ptr<Texture>     GetTexture( const std::string& name );

        void RegisterRenderSystem( const std::string& name, std::shared_ptr<IRenderSystem> system );
        void UnregisterRenderSystem( const std::string& name );

        void RebuildRenderGraph();

        void AddPointLight( ShaderProtocols::PointLightPayload&& pointLight );

        const auto& GetPointLights() const
        {
            return m_PointLight;
        }

        void AddSpotLight( ShaderProtocols::SpotLightPayload&& spotLight );

        const auto& GetSpotLights() const
        {
            return m_SpotLight;
        }

        /// THE frame's cloud-shadow payload — the map, its projection, and the numbers a receiver needs
        /// to read it. Gathered HERE, once, and handed to every consumer: the deferred composite, the
        /// forward PBR materials (through Graphic::PBRSceneFrame), the skinned material and the terrain
        /// material. While the composite was the only reader, this gather sat inline in the deferred
        /// branch and the answer to "does this surface receive a cloud shadow" was "only if a deferred
        /// pass drew it".
        ///
        /// Valid after ExecuteCloudShadowMap() has run for this frame — which is before the render graph
        /// records, so every pass in the frame may ask. Returns the default (disabled, no map) whenever
        /// the layer is absent, off, not casting or at zero strength.
        CloudShadowInput GetCloudShadowInput() const;

    private:
        // Which view this renderer is; see the constructor. Held as a lease so the slot goes back when
        // this renderer is destroyed, whatever destroys it.
        Engine::RendererSlotLease m_SlotLease;

        // Constructor-set, const in everything but name: MeshRenderer copies it in Initialize and the
        // cascade framebuffers exist from that moment until this renderer dies.
        ShadowQuality m_ShadowQuality;

        // Has EnsureRendererResources() run? Set once, never cleared — see its comment for why there is no
        // path that invalidates it.
        bool m_RendererResourcesBuilt = false;

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

        // THE SCENE'S HALF, and it runs on EVERY Init() including the first. Releases what belonged to the
        // scene that was here before and rebuilds the graph over what is left.
        //
        // What that is, exhaustively: the "External:" render systems. The editor registers its authoring
        // passes (grid, colliders, gizmo overlays) against the scene it built its RenderRegistry for, by
        // name; a different scene's registry re-registers its own, and the registry that owned these is
        // destroyed by the same caller a few lines later. Leaving them would leave passes closing over a
        // registry that no longer exists.
        //
        // What is deliberately NOT here: any reset of the engine systems' own state. They keep it across a
        // scene load for the same reason they keep it across a frame — every per-frame input is RESTATED by
        // its producer, absence included (SkyboxECSSystem emits an explicit "no sky" command rather than
        // emitting nothing), and everything expensive enough to be cached across frames is keyed on a
        // fingerprint of the content it was built from rather than on "have I built one". Those two
        // properties are what makes a render system survivable, and they are asserted in
        // Desert/Tests/Engine/RendererSceneLifetime rather than left as a claim.
        void RebindScene();

        void ClearMainFramebuffer();
        void ExecuteRenderGraph();
        // Debug-phase passes (bounding boxes, colliders) drawn as a LOAD overlay AFTER the deferred
        // lighting composite — in Deferred the composite would otherwise paint lit meshes over any
        // debug lines recorded earlier in the graph, hiding them wherever geometry is present.
        void ExecuteDebugOverlay();
        // Transparency-phase passes (GPU particles, ...) drawn as a LOAD overlay AFTER the deferred
        // lighting composite, for the exact same reason as ExecuteDebugOverlay: recorded inside the
        // graph they land on the target BEFORE the composite and get painted over wherever geometry
        // exists (visible against sky, gone against the ground — the particle "top-down" bug).
        void ExecuteTransparency();
        // Exponential height fog: the closed-form COMPUTE evaluation. Called between the deferred block
        // and ExecuteTransparency() — the one point in the frame where the scene depth is finished in
        // BOTH paths and no render pass is open (an in-frame dispatch inside one is illegal). Its apply
        // is a graph pass in Transparency at RenderPassOrder::AtmosphericFog, BELOW the particles, so
        // they composite over the fogged scene. When Sky Phase 3 lands, this pass composes fog OVER the
        // aerial perspective (UE's order).
        void ExecuteAtmosphericFog();

        // The cloud march and its noise bake. Issued immediately after the atmospheric fog: both are
        // in-frame compute and must be outside an open render pass, and by that point the scene depth is
        // final and this frame's atmosphere LUTs have been filled. The composite itself is a graph pass in
        // Transparency at RenderPassOrder::FarField, ABOVE the fog and BELOW the particles.
        void ExecuteVolumetricClouds();
        // The cloud layer's shadow on the WORLD, which is a different pass at a different point in the
        // frame from the march above and belongs to a different consumer. Issued BEFORE the render graph
        // records, because the deferred lighting pass reads it and runs immediately after the graph. It
        // depends on nothing the frame produces — no scene depth, no G-buffer, no atmosphere LUT — so
        // nothing forces it later, and its consumer forces it earlier.
        void ExecuteCloudShadowMap();
        // UI-phase passes (the Render2D canvas) drawn as a LOAD overlay AFTER the deferred lighting
        // composite — same reason as ExecuteTransparency/ExecuteDebugOverlay: recorded inside the graph
        // they land on the target BEFORE the composite (painted over) AND a CLEAR begin would wipe the
        // depth the grid/overlays load afterwards. Runs on top of the finished scene.
        void ExecuteUI();

    private:
        struct
        {
            Core::Camera* ActiveCamera;
        } m_SceneInfo;

        // The atmosphere sun light's render-effect slice, refreshed by SetProceduralSky each frame; the
        // post chain reads it to run (or zero out) the light shafts.
        SunLightFx m_SunLightFx;

        ShaderProtocols::DirectionLight m_DirectionLights;
        ShaderProtocols::PointLight     m_PointLight;
        ShaderProtocols::SpotLight      m_SpotLight;

        WindEnv m_Wind; // scene-global shared wind, refreshed from SceneSettings each BeginScene

        // Selected post-process anti-aliasing technique, taken from m_Quality each BeginScene.
        Common::Settings::AntiAliasingMode m_AAMode       = Common::Settings::AntiAliasingMode::FXAA;
        bool                               m_BloomEnabled = false;

        // Lens flare, refreshed from SceneSettings each BeginScene. The tint is held apart from the rest
        // because the pass never sees it — the tonemap applies it, the way the shafts' tint works.
        System::LensFlareRenderer::Params m_LensFlare;
        glm::vec3                         m_LensFlareTint = glm::vec3( 1.0f );
        // Raised by the UI canvas when it drew glass; consumed at the top of the next frame's UI phase.
        bool                   m_BackdropBlurNeeded = false;
        bool                   m_ScenePlaying = false; // set per frame in BeginScene (hides authoring aids)

    public:
        // --- UI glass (backdrop blur) -----------------------------------------------------------
        // The UI canvas raises this when it recorded a glass element; the blur pyramid is then built
        // before the NEXT frame's UI phase. Latched per frame, so a canvas that stops using glass stops
        // paying for it.
        void SetBackdropBlurNeeded( bool needed )
        {
            m_BackdropBlurNeeded = needed;
        }

        // The blur pyramid glass samples, or null when it has never been built (the UI then falls back to
        // a flat tint). Mip 0 is a mild blur; higher LODs are blurrier — see BackdropBlurRenderer.
        const std::shared_ptr<Image2D>& GetBackdropBlurImage() const;
        uint32_t                        GetBackdropBlurMaxLod() const;

    private:
        std::shared_ptr<Framebuffer> m_TargetFramebuffer;
        std::shared_ptr<Framebuffer> m_GBuffer;                    // deferred G-buffer (MRT)
        std::shared_ptr<Framebuffer> m_SSAOBuffer;                 // deferred SSAO (AO factor)
        std::shared_ptr<Framebuffer> m_SceneColorCopy;             // scene snapshot for glass refraction
        std::shared_ptr<Framebuffer> m_SSRBuffer;                  // SSR trace target (denoised, then composited)
        std::shared_ptr<Framebuffer> m_GIBuffer;                   // RSM-GI resolve target (blur-read by lighting)
        std::shared_ptr<Framebuffer> m_RSMBuffer;                  // reflective shadow map (G-buffer from the sun)
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
        Common::Settings::MachineSettings m_Quality = Common::Settings::MachineSettings::Get();
        // What this VIEW is drawing on top of the world. NOT refreshed from the scene — pushed in by
        // whoever owns the view (SetDebugView), and "show nothing" until someone does. See
        // Graphic/DebugViewState.hpp for why it stopped being scene data.
        DebugViewState          m_DebugView;
        bool                    m_EnableSSAO    = true; // deferred SSAO pass on/off (refreshed from SceneSettings)
        Core::GIMode            m_GIMode        = Core::GIMode::ScreenSpace; // indirect-light source
        float                   m_GIIntensity   = 2.0f;
        bool                    m_EnableSSR     = false;
        float                   m_SSRIntensity  = 1.0f;
        // World units (= centimetres). Mirrors SceneSettings::SSRMaxDistance's default; refreshed from
        // SceneSettings every frame, so this value only matters before the first Update.
        float m_SSRMaxDistance = Common::Units::Metres( 40.0f );

        // The RSM is a LOW-FREQUENCY input to a temporally-accumulated resolve, so it does not need to be
        // re-rendered every frame — refreshing it every 4th frame (and immediately when the sun moves) keeps
        // the GI stable while cutting the extra geometry pass to a quarter of its cost.
        static constexpr uint32_t kRSMResolution   = 512;
        static constexpr uint32_t kRSMRefreshEvery = 4;
        glm::vec3                 m_RSMLastSunDir{ 0.0f };
        uint32_t                  m_RSMFrameCounter = 0;

        // Allocate the SSR / RSM-GI targets + their systems on FIRST USE, not in the constructor: each
        // PreviewViewport (thumbnails, the Details mesh preview) owns a SceneRenderer, and a preview never
        // enables either feature — building them up front multiplied a lot of VRAM by the preview count.
        // Return false when the feature is unavailable; the failure is latched so it is not retried each frame.
        bool EnsureGIResources();
        bool EnsureSSRResources();
        // Device can sample+blend RGBA32F colour attachments — the precondition both features share.
        bool HasFloatRenderTargetSupport() const;
        bool m_GIResourcesReady   = false;
        bool m_GIResourcesFailed  = false;
        bool m_SSRResourcesReady  = false;
        bool m_SSRResourcesFailed = false;
        RenderGraphBuilder      m_RenderGraphBuilder;
        std::unordered_map<std::string, std::shared_ptr<IRenderSystem>> m_RenderSystems;

        // The names above, in the order they were first registered. RebuildRenderGraph walks THIS, not
        // the map: the map hands its systems out in hash-bucket order, so "the pass registered first"
        // meant "the pass whose system name happened to hash low", and it changed whenever a system was
        // added. The render graph tie-breaks equal passes inside a phase by registration order, so this
        // vector is what turns the order of the RegisterSystem calls in Init into the draw order.
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