#pragma once

// THE VIEW'S INTERFACE (UE: ISceneRenderer / FSceneInterface, forward-declared to the editor and the engine). The
// API takes and returns its types by reference or pointer where it can, so this header carries the types it NAMES
// (forward-declared below) and not the passes that produce them; the members live in SceneRendererImpl.hpp.
#include <Common/Core/DevInstruments.hpp>
#include <Engine/Animation/AnimationUpdateRate.hpp>
#include <Engine/Animation/Modular/LeaderPoseLink.hpp>
#include <Engine/Graphic/DebugViewState.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/SunLightFx.hpp>
#include <Engine/Graphic/ViewMemory.hpp>
#include <Engine/Graphic/WindGust.hpp>
#include <Common/Core/Timestep.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace Common::Math
{
    struct AABB;
}
namespace Desert
{
    class Mesh;
}
namespace Desert::Core
{
    class Camera;
    enum class RenderPath : int;
} // namespace Desert::Core
namespace Desert::ECS
{
    struct ExponentialHeightFogData;
    struct VolumetricCloudData;
} // namespace Desert::ECS
namespace Desert::Engine::Neural
{
    struct NrcStats;
}
namespace Desert::Engine::ViewBudget
{
    struct HeldView;
}
namespace Desert::ShaderResources
{
    class StorageBuffer;
}
namespace Desert::Graphic
{
    class Image2D;
    class Framebuffer;
    class Texture;
    class Material;
    class MaterialSkybox;
    class PipelineCache;
    struct MaterialOverrides;
    struct MaterialSlotBinding;
    using MaterialSlotBindingPtr = std::shared_ptr<const MaterialSlotBinding>;
    struct RenderMethods;
    struct StochasticLightMethod;
    struct PrimitiveReadout;
    struct LocalLightShadowSettings;
    struct LocalFogVolumeGpu;
    struct HeterogeneousVolumeInstance;
    struct HeroCloudInstance;
    struct SkySettings;
    struct AtmosphereEnv;
    struct CloudEnvironmentBake;
    struct CloudShadowInput;
    struct Environment;
    struct ViewFrame;
    namespace System
    {
        struct GrassFieldDraw;
        struct LandscapeTileDraw;
        struct LandscapeWeightDraw;
        struct DecalProxy;
        struct LightFunctionProxy;
        struct ForwardPrepassStats;
        struct CloudMarchCounters;
        struct VsmPageStats;
        struct VsmLocalLightStats;
        struct VirtualShadowMapQuality;
        struct PathTracerRadianceCacheStatus;
    } // namespace System
    namespace GpuDriven
    {
        struct CullPhaseViewCount;
        class DrawCommandCache;
    } // namespace GpuDriven
    namespace GlobalIllumination
    {
        struct ScreenProbeMissReasons;
        struct ScreenProbeCensus;
        struct RadianceCacheUpdateCounts;
        struct DirectionalLighting;
        struct LocalLighting;
    } // namespace GlobalIllumination
    namespace SceneResources
    {
        struct ContactShadowSettings;
        struct ShadowFilterSettings;
    } // namespace SceneResources
    namespace RayTracing
    {
        struct RayTracingProgramHit;
        class RayTracingSceneRenderer;
    } // namespace RayTracing
    namespace DistanceField
    {
        class GlobalDistanceField;
    }
    namespace SurfaceCache
    {
        class SurfaceCacheRenderer;
    }
} // namespace Desert::Graphic
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
    namespace RayTracing
    {
        // EmissiveLightsRenderer.hpp: held by pointer, so this header does not carry the RT emissive list
        // (and the mesh headers it reads) into every includer (UE: FSceneRenderer keeps its RT state in
        // forward-declared members).
        class EmissiveLightsRenderer;
        class EmissiveTriangleListCache;
    } // namespace RayTracing
    namespace Render
    {
        struct StaticSceneSnapshot;
        struct InstancedSceneSnapshot;
        struct InstancedSceneFrame;
    }
    class FrameTextures;
    struct FrameValues;
    class ReflectionCaptureRenderer;
    class LocalShadowCache;
    namespace System
    {
        class AutoExposureRenderer;
        class BackdropBlurRenderer;
        class BloomRenderer;
        class CopyRenderer;
        class DeferredLightingRenderer;
        class DepthExpandRenderer;
        class FXAARenderer;
        class GraphColorResolveRenderer;
        class GrassRenderer;
        class HeightFogRenderer;
        class HZBRenderer;
        class JumpFloodOutlineRenderer;
        class LensFlareRenderer;
        class LightShaftRenderer;
        class LocalExposureRenderer;
        class MeshRenderer;
        struct ParticleViewEmitterCull;
        class PathTracer;
        class SceneColorHistoryRenderer;
        class SceneDepthResolveRenderer;
        class SkyboxRenderer;
        class SMAARenderer;
        class SSAORenderer;
        class SSRRenderer;
        class SubsurfaceRenderer;
        class TerrainRenderer;
        class TonemapRenderer;
        class CombineLUTsRenderer;
        class VelocityViewRenderer;
        class IndirectViewRenderer;
        class VolumetricCloudRenderer;
        class VolumetricFogRenderer;
        class HeterogeneousVolumeRenderer;
        class TranslucencyLightingVolumeRenderer;
        class ISingleLayerWaterPass;
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
    namespace VirtualGeometry
    {
        class VirtualGeometryRenderer;
        class ClusterCutCounts;
    } // namespace VirtualGeometry
#if DESERT_DEV_INSTRUMENTS
    class DebugDrawRenderer;
    namespace VirtualGeometry
    {
        class ClusterView;
    }
#endif

    /**
     * One view of a scene (UE: ISceneRenderer / FSceneInterface): the calls the engine and the editor make on a
     * view. The implementation - its members, passes and render systems - is SceneRendererImpl, private to the
     * renderer (SceneRendererImpl.hpp); SceneRenderer::Create makes one.
     */
    class SceneRenderer
    {
    public:
        [[nodiscard]] static std::unique_ptr<SceneRenderer>
        Create( const ViewExtent& extent, const ViewProfile& profile = kSceneViewProfile,
                ResourceOwnerInstance accountUnder = ResourceOwnerInstance{} );
        virtual ~SceneRenderer()                         = default;
        SceneRenderer( const SceneRenderer& )            = delete;
        SceneRenderer& operator=( const SceneRenderer& ) = delete;

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
            // The sun's (DirLights[0]) VolumetricScatteringIntensity: how much of it the volumetric fog scatters.
            // Beside the payload rather than in it: DirectionLightsUB is a GPU layout every lit shader shares, and
            // only the volumetric fog reads this.
            float SunVolumetricScatteringIntensity = 1.0f;
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
            std::shared_ptr<const std::vector<glm::mat4>> BoneMatrices; // optional, co-owned
            // Skinned only: the entity's MorphComponent weights and their revision (empty = no morph).
            std::vector<float>     MorphWeights;
            uint64_t               MorphRevision            = 0;
            std::shared_ptr<Animation::SkinnedLodFeedback> LodFeedback; // skinned + animated only
            Animation::LeaderPoseLink Leader; // skinned only: a follower mesh's leader (UE LeaderPoseComponent)
            bool                   Outlined        = false;
            uint64_t               HiddenSubmeshes = 0;  // bit i = submesh i hidden (static meshes)
            int                    ForcedLOD       = -1; // -1 = auto (by distance)
            int                    LODBias         = 0;  // shifts the auto LOD (ignored when forced)
            bool                   CastShadows     = true;
            bool                   ReceiveShadows  = true;
            bool                   CastContactShadow        = true; // UE bCastContactShadow (static meshes)
            // UE bReceivesDecals: the component's (static: on by default; skinned: off by default).
            bool                   ReceivesDecals           = true;
            int                    TranslucencySortPriority = 0; // lower draws first (translucency pass)
        };

        // This renderer's shadow budget. Read by its own MeshRenderer in Initialize; a level's viewport moves it
        // with the Shadows quality level (BeginScene -> MeshRenderer::RebudgetShadows).
        [[nodiscard]] virtual const ShadowQuality& GetShadowQuality() const = 0;
        // Scalability ViewDistance.DisplacementDicingRate: pixels a diced edge covers (0 = runtime displacement off),
        // read by the mesh displacement (DisplacementCache) and the landscape's (TerrainRenderer) alike.
        [[nodiscard]] virtual int GetDisplacementDicingRate() const = 0;
        // The view is a reflection capture's face (ViewProfile::SceneCapture), not a level's viewport.
        [[nodiscard]] virtual bool IsSceneCapture() const = 0;
        // The cached point/spot-light shadow depth of the scene BeginScene was handed (Core::Scene owns it, one
        // for all its views); null outside a BeginScene/EndScene bracket.
        [[nodiscard]] virtual LocalShadowCache* GetFrameLocalShadows() const = 0;

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
        virtual void Init() = 0;

        // THE SAME BUILD, ONE STAGE PER CALL, for an owner that must not pay it in one frame (the asset
        // thumbnail renderer builds its view while the editor keeps drawing). Each call builds the next stage —
        // the scene targets, then one engine render system — and returns true once every stage has run;
        // Init() then builds nothing again and binds the scene. Init() without it builds every stage itself.
        virtual bool AdvanceRendererBuild() = 0;

        // @p camera is the VIEW's camera — the scene holds a list of views and hands in the one this
        // renderer is recording (Engine/Core/SceneViewList.hpp). Null is legal and renders a scene with
        // no view matrix, which is what a world whose camera has not been made yet looks like.
        [[nodiscard]] virtual Common::BoolResultStr BeginScene( const Desert::Core::Scene& scene,
                                                                Core::Camera*              camera ) = 0;

        virtual void OnUpdate( const UpdateInfo& sceneRenderInfo ) = 0;

        [[nodiscard]] virtual Common::BoolResultStr EndScene() = 0;

        // THE REFLECTION CAPTURES (ReflectionCaptures/ReflectionCaptureRenderer.hpp) are the scene's; a view only
        // reads them. Whether this view does (ViewProfile::ReflectionCaptures and a resolved reflection method of
        // Captures or above), and the identity of the sky bake it holds (0 before it has one) - the scene
        // recaptures when it changes.
        [[nodiscard]] virtual bool     ReadsReflectionCaptures() const = 0;
        [[nodiscard]] virtual uint64_t GetEnvironmentIdentity()        = 0;
        // The view's HDR scene colour as its last frame left it (the resolved image at MSAA > 1): what a
        // reflection capture resamples into its cube. Null before the targets exist.
        [[nodiscard]] virtual std::shared_ptr<Image2D> GetSceneColorImage() const = 0;
        // HDR-OUTPUT p3: the tonemap's HDR rendition of its last capture frame
        // (TonemapRenderer::GetHdrRenditionImage); null before the first window capture of an HDR output.
        [[nodiscard]] virtual std::shared_ptr<Image2D> GetHdrRenditionImage() const = 0;
        // HDR-OUTPUT p4: logs the peak of that rendition ("[Display] HDR rendition of the captured frame: peak
        // ...") after a capture of an HDR output — the evidence the HDR picture carries above paper white. Waits
        // for the device. One body for every host that captures (the editor's shot.window, the runtime's --shot).
        virtual void LogHdrRenditionPeak() const = 0;

        virtual void Resize( const uint32_t width, const uint32_t height ) = 0;
        // The Render set (ViewTargetSet::Render) to @p render: the scene target, G-buffer, scene depth resolve,
        // silhouette mask, overdraw target and outline. Resize() sizes the Output set (tonemap, FXAA, SMAA) and
        // calls this with the split of the new extent; OnUpdate calls it with the frame's split. Equal: nothing.
        virtual void ResizeRenderTargets( ViewExtent render ) = 0;

        // CAMERA CUT: the next frame this view renders starts a new temporal sequence in the SAME world.
        // Every render system gets IRenderSystem::OnTemporalHistoryReset (frame indices to zero, histories
        // invalid, exposure snaps, particle simulation restarts). Nothing is released and no pass is added,
        // so it is legal between any two frames. Not tied to any one caller: the headless capture calls it
        // when its frame count starts, a camera cut or a teleport asks for the same thing.
        virtual void ResetTemporalHistory() = 0;

        // THE WORLD THIS VIEW DREW WAS TORN DOWN (UE: the FSceneViewState and the Lumen/distance-field scene data
        // die with their FScene). Releases everything this view built FROM the old world - its surface cache,
        // radiance cache, probe gathers, global distance field and the terrain's materials and layer arrays - and
        // cuts the temporal history. Each is made again on first use by the next world. Called between frames by
        // the opener, after the world is cleared and before the next one loads
        // (SceneWorkspace::ReleaseActiveWorld), so the old world's device memory is given back before the new
        // one's is asked for (FIELD-MEM p3).
        virtual void ReleaseWorldResources() = 0;

        [[nodiscard]] virtual const ViewExtent& GetViewExtent() const = 0;

        // GAME THREAD, at the update's fence (RenderCommand::Prepare): applies @p mesh's pending edit - the render
        // revision, the BLAS release, the device buffers made from its CPU geometry (Mesh::EnsureGpuResources) -
        // with no view job in flight. The submits below only read the result (they may run on the render task).
        static void PrepareMesh( Mesh& mesh );

        // WHETHER A MESH IS WITHIN ITS MAX DRAW DISTANCE FROM THIS VIEW (UE's LDMaxDrawDistance): asked by the
        // mesh draw commands before they submit, so a mesh beyond it enters no queue and no pass — lit, shadow,
        // velocity or debug — of this view. Measured from the view's main camera and stretched by its lens
        // (Graphic::IsWithinDrawDistance). maxDrawDistance <= 0, or no camera yet: drawn.
        [[nodiscard]] virtual bool IsWithinMaxDrawDistance( const Mesh* mesh, const glm::mat4& transform,
                                                            float maxDrawDistance ) const = 0;

        // The slot binding is taken BY SHARED HANDLE, not by reference to the caller's storage: the
        // caller is a draw command whose recorder (an ECS component) may already be gone. See
        // Graphic::MaterialSlotBinding (A8-3).
        virtual void SubmitMesh( const Mesh* mesh, const MaterialSlotBindingPtr& materialSlots,
                                 const glm::mat4& transform, const RenderSubmissionExtra& extra ) = 0;

        // THE RETAINED STATIC SCENE'S REPLAY (Render::DrawStaticSceneCommand): every section of @p scene through
        // the same per-view admission SubmitMesh applies (readiness, max draw distance, texture demand, the LOD
        // preview), with the section's cached local bounds instead of a union over the submeshes per call.
        virtual void SubmitStaticScene( const Render::StaticSceneSnapshot& scene ) = 0;

        // THE VIEW'S LOD PREVIEW (UE LOD Manager / Static Mesh Editor "LOD" preview): draw `entity`'s static mesh
        // at `level` in THIS view only — every pass (G-buffer, shadows, forward) reads it through the draw's
        // ForcedLOD, the authored StaticMeshComponent::ForcedLOD is untouched and other views keep their own pick.
        // It is a LEASE, not a setting: per-frame renderer state that lapses one frame after the caller stops
        // renewing it (BeginScene), so a closed tool can never leave a view pinned. level < 0 clears it now.
        virtual void              SetLODPreview( uint32_t entity, int level )                   = 0;
        [[nodiscard]] virtual int LODPreviewFor( uint32_t entity, int authoredForcedLOD ) const = 0;

        // Submit one landscape tile for this frame (from LandscapeECSSystem via DrawLandscapeTileCommand):
        // the Terrain pipeline reading the tile's R16 copy, surfaced with its root's material and layers.
        virtual void SubmitLandscapeTile( const std::shared_ptr<Image2D>&                 heightmap,
                                          const System::LandscapeTileDraw&                tile,
                                          const std::shared_ptr<const MaterialOverrides>& overrides,
                                          const System::LandscapeWeightDraw&              weights ) = 0;

        // Submit one landscape root's grass for this frame (from LandscapeECSSystem via
        // DrawLandscapeGrassCommand). BY REFERENCE, NOT COPIED (REVIEW-FIX R6; UE: a mesh draw command reads its
        // proxy's data in place): @p draw is the recorded command's, which outlives this bracket - the arena is
        // cleared only behind every view and capture face that replayed it (Scene::OnUpdate, "Scene: CmdBuffer
        // Clear") - and the grass renderer keeps the reference only until EndScene clears its queue.
        virtual void SubmitLandscapeGrass( const System::GrassFieldDraw& draw ) = 0;
        // A temporary would dangle in the queue (the renderer keeps a reference until EndScene).
        void SubmitLandscapeGrass( System::GrassFieldDraw&& ) = delete;

        // The single layer water pass the Water module registered (SingleLayerWaterPass.hpp), null without it:
        // the module's zone command submits its zones there (WATER-W6), kept until EndScene.
        [[nodiscard]] virtual System::ISingleLayerWaterPass* GetSingleLayerWaterPass() = 0;

        // Submit a mesh drawn with a generic data-driven material (MaterialComponent with a non-lit shader).
        // directTexture (optional): a runtime-owned Image2D bound to `directTextureSampler`, for
        // procedural textures with no TextureAsset handle (the text SDF atlas).
        // castShadows: rasterize this draw into the shadow cascades. Off by default — see the note on
        // GenericMeshRenderData::CastShadows; only a producer that knows the draw is a solid mesh, and
        // that no OTHER draw of the same entity is already casting, may turn it on.
        // @p shaderName, @p overrides, @p directTextureSampler and @p rayHit are KEPT BY ADDRESS in the queued
        // record (MeshRenderer::GenericMeshRenderData::ShaderNameRef): they must outlive the frame's passes, as
        // the recorded command's members and its producer-kept GenericMeshDrawParams do. No defaults, because a
        // defaulted `{}` would be a temporary that dies at the call's semicolon.
        // @p revision: what of those values the producer's GenericMeshDrawParams::Revision stands for
        // (MeshRenderer::GenericMeshRenderData::ValuesRevision); {} = the command's own values, hashed per frame.
        struct GenericValuesRevision
        {
            // GenericMeshDrawParams::Revision of @p overrides and @p rayHit; 0 = none (the command's own values).
            uint64_t Revision = 0;
            // The revision covers @p shaderName too (the params carry it), not only the overrides and the hit.
            bool HasShaderName = false;
        };
        virtual void SubmitGenericMesh( uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                        const std::string& shaderName, const MaterialOverrides& overrides,
                                        bool outlined, Image2D* directTexture,
                                        const std::string& directTextureSampler, bool castShadows,
                                        bool visibleInRayTracing, const RayTracing::RayTracingProgramHit& rayHit,
                                        GenericValuesRevision revision ) = 0;

        // v3 per-slot custom shaders: draw only @p visibleSubmeshMask submeshes of the mesh with the
        // slot's own runtime material (a MaterialService-owned DataDrivenMaterial). @p motionPart: the stable part
        // of the entity the draw belongs to (RenderSubmissionExtra::MotionPart), 0 = the entity itself.
        virtual void SubmitSlotMaterialMesh( uint32_t entity, Mesh* mesh, const glm::mat4& transform,
                                             Material* material, uint64_t visibleSubmeshMask,
                                             bool outlined = false, bool castShadows = false,
                                             uint32_t motionPart = 0 ) = 0;

        /// ISM instances the last geometry pass drew (MeshRenderer::GetIsmInstancesDrawn).
        [[nodiscard]] virtual uint32_t GetIsmInstancesDrawn() const = 0;
        /// The neural radiance cache's last read-back (System::PathTracer::GetRadianceCacheStats; DesertCtl state
        /// "nrc"); nullptr while no PathTracing.RadianceCache Nrc frame made the cache.
        [[nodiscard]] virtual const Engine::Neural::NrcStats* GetRadianceCacheStats() const = 0;
        /// PathTracing.RadianceCache as the path tracer last ran it (System::PathTracer::GetRadianceCacheStatus;
        /// DesertCtl state "nrc" status / reason); all Off without a path tracer.
        [[nodiscard]] virtual System::PathTracerRadianceCacheStatus GetRadianceCacheStatus() const = 0;
        /// Per view the GPU-culled draw bins served (main view, drawn sun cascades): the instances drawn in
        /// occlusion phase 1 and phase 2, read back slots-in-flight late (GpuDriven::CullPhaseCounts). Empty while
        /// no view draws from bins.
        [[nodiscard]] virtual const std::vector<GpuDriven::CullPhaseViewCount>& GetDrawBinPhaseCounts() const = 0;
        /// GPU-FARFIELD-4: the cloud march's counters of the newest frame read back (System::CloudMarchCounters);
        /// nullptr while the view has no cloud renderer.
        [[nodiscard]] virtual const System::CloudMarchCounters* GetCloudMarchCounters() const = 0;
#if DESERT_DEV_INSTRUMENTS
        /// PROBE-TURN-1: this view's screen probe far-field reads of the radiance cache by why they answered or not
        /// (GlobalIllumination::ScreenProbeMissReasons, read back slots-in-flight late); nullptr while the view has
        /// no screen probe gather.
        [[nodiscard]] virtual const GlobalIllumination::ScreenProbeMissReasons*
        GetScreenProbeMissReasons() const = 0;
        /// FPS-SP-INTEGRATE: the screen probe gather's census (GlobalIllumination::ScreenProbeCensus: probes
        /// placed against the budget, the integrate's tiles by kind), slots-in-flight late; nullptr without a
        /// gather.
        [[nodiscard]] virtual const GlobalIllumination::ScreenProbeCensus* GetScreenProbeCensus() const = 0;
#endif
        /// PROBE-TRACE-COST-3: the radiance cache update's counts (GlobalIllumination::RadianceCacheUpdateCounts,
        /// read back slots-in-flight late): stamped, never traced, traced, slots taken / refused / freed by why.
        /// nullptr while the view has no radiance cache.
        [[nodiscard]] virtual const GlobalIllumination::RadianceCacheUpdateCounts*
        GetRadianceCacheUpdateCounts() const = 0;
        /// VIRT-GEOM (DesertCtl `state views.virtualGeometry`): the VG instances this frame's draw bins made cut
        /// candidates of, and the clusters the main view's cluster cut drew (read back slots-in-flight late,
        /// VirtualGeometry::ClusterCutCounts). Zero while no VG mesh is drawn. Overflow: what the cut could not do
        /// (full traversal stacks + clusters past the visible capacity + candidates without a draw slot);
        /// OccludedClusters: clusters the previous-frame HZB hid.
        struct VirtualGeometryCounts
        {
            uint32_t Instances       = 0;
            uint32_t ClustersVisible = 0;
            // Clusters the rasters drew (RasterBins::GroupArgs over the groups with a `.VirtualGeometry`
            // pipeline): the main view's main + post phase, and every shadow cascade's.
            uint32_t ClustersDrawn       = 0;
            uint32_t ShadowClustersDrawn = 0;
            // Per VSM clipmap level (index = level): the clusters "VSM: Raster VirtualGeometry" drew into it.
            std::vector<uint32_t> VsmClustersDrawn;
            // VSM material groups with binned VG clusters whose template declares a `.VirtualGeometry` twin and
            // has no pipeline of it (held refusals: their primitives stay in the levels' CPU walk;
            // MeshRenderer::VsmRefusedGroups). Must be 0.
            uint32_t VsmRefusedGroups = 0;
            // VG submeshes binned into a VSM group whose template has no `.VirtualGeometry` twin: the fallback,
            // walked by design (MeshRenderer::VsmFallbackSubmeshes; UE: a material without a Nanite path draws the
            // fallback).
            uint32_t VsmFallbackSubmeshes = 0;
            uint32_t Overflow             = 0;
            uint32_t OccludedClusters     = 0;
            // VG instances still drawn by the bins of a view with a cut (main or cascade; must be 0: the cluster
            // raster draws them).
            uint32_t BinnedVgInstances = 0;
            // VG-SW-RASTER: the main view's clusters per raster (main + post phase): the hardware raster's and the
            // software raster's ("VirtualGeometry: Software Raster" -> the visibility buffer).
            uint32_t HwClusters = 0;
            uint32_t SwClusters = 0;
        };
        [[nodiscard]] virtual VirtualGeometryCounts GetVirtualGeometryCounts() const = 0;
        /// This frame's sun shadow is the virtual shadow map (the Shadows method resolved to VirtualMaps). Public
        /// for MeshRenderer: the VSM's own pass table (MeshPass::VirtualShadow) is resolved only while it is.
        [[nodiscard]] virtual bool SunShadowIsVirtual() const = 0;
        /// The mesh passes' cached draw commands (MeshRenderer::GetDrawCommandCache): bins, slots and the CPU
        /// fallbacks per pass. Null before the first GpuScene sync.
        [[nodiscard]] virtual const GpuDriven::DrawCommandCache* GetDrawCommandCache() const = 0;
        /// Draws the last frame gave to the default surface while their own pipeline compiled
        /// (MeshRenderer::GetStandInDraws): the control gate's PendingWork::MaterialPipelines.
        [[nodiscard]] virtual uint32_t GetStandInDraws() const = 0;
        /// Triangles the last frame's view pass drew, at the LOD each mesh was drawn with
        /// (MeshRenderer::GetViewTrianglesDrawn).
        [[nodiscard]] virtual uint64_t GetViewTrianglesDrawn() const = 0;
        /// The last frame's forward depth prepass (MeshRenderer::GetForwardPrepassStats); zero without a mesh renderer.
        [[nodiscard]] virtual System::ForwardPrepassStats GetForwardPrepassStats() const = 0;

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
        virtual float NoteTextureDemand( const Mesh& mesh, std::span<const glm::mat4> transforms,
                                         std::span<const Material* const> materials,
                                         const MaterialOverrides*         overrides = nullptr ) const = 0;
        // The same, for a mesh whose local bounds the caller already holds (@p local, Geometry::LocalBounds).
        virtual float NoteTextureDemandOfBounds( const Common::Math::AABB&        local,
                                                 std::span<const glm::mat4>       transforms,
                                                 std::span<const Material* const> materials,
                                                 const MaterialOverrides*         overrides = nullptr ) const = 0;
        /// NoteTextureDemand for an instanced field: the same size, the nearest of @p transforms, measured
        /// through the field's cached bounds tree (TextureStreamingDemand::FieldScreenPixels).
        virtual float NoteInstanceFieldDemand( const Mesh&                                          mesh,
                                               const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                               std::span<const Material* const> materials ) const = 0;

        // THE RETAINED ISM LIST'S REPLAY (Render::DrawInstancedSceneCommand): every row of @p scene that is ready
        // to draw, with @p frame's values of the same index, into this view's instanced queue
        // (MeshRenderer::SubmitInstancedRow: no handle copied that the queue entry already holds).
        virtual void SubmitInstancedScene( const Render::InstancedSceneSnapshot& scene,
                                           const Render::InstancedSceneFrame&    frame ) = 0;

        virtual void SetEnvironment( const std::shared_ptr<MaterialSkybox>& material, const SkyLook& look ) = 0;

        // Selection-outline (Jump Flood) appearance. Editor-only: pushed each frame from EditorPreferences
        // (the outline is a viewport visualization, not a scene property, so it does not live in SceneSettings).
        virtual void SetOutlineSettings( const glm::vec3& color, float width, float smoothness, bool enabled ) = 0;

        // What this view draws ON TOP of the world: grid, colliders, bounding boxes, wireframe, the buffer
        // and shadow debug views. Pushed in for exactly the same reason the outline is — it is a property
        // of the view, not of the scene (Graphic/DebugViewState.hpp records the measurement that settled
        // it). A renderer nobody pushes to shows the lit world and no overlay, which is what the Runtime
        // and every offscreen preview renderer rely on.
        //
        // Call it BEFORE BeginScene: the flags reach the mesh renderer from there, so a push afterwards
        // lands one frame late.
        //
        // EVERY SETTER THE INTERFACE CALLS (this one, SetQuality, SetOutlineSettings, SetEnvironment,
        // ResetTemporalHistory, WatchPrimitive, SetLODPreview) CHANGES THE VIEW BETWEEN RECORDINGS (UE:
        // ENQUEUE_RENDER_COMMAND): while this view records on the render task the write waits for the fence
        // (RenderTask::AfterJoin), which runs before the next BeginScene of any view; with nothing in flight it
        // applies now, exactly as before. A Debug build reports a write that lands inside a recording
        // (CheckWriteBetweenRecordings).
        virtual void SetDebugView( const DebugViewState& state ) = 0;

#if DESERT_DEV_INSTRUMENTS
        // THE GAMEPLAY DEBUGGER'S RENDERING CATEGORY (Graphic/PrimitiveReadout.hpp): asks this view to describe
        // @p entity at the end of the frame being rendered. The request is good for the frame it was made in and
        // the next one (the debugger collects in the scene update, before or after this view's render phase);
        // a renderer nobody asks does one frame compare at EndScene and nothing else.
        virtual void WatchPrimitive( uint32_t entity ) = 0;
        // What the last EndScene that had a watch found: null before any. Its Entity names whom it describes.
        [[nodiscard]] virtual const PrimitiveReadout* GetPrimitiveReadout() const = 0;
#endif

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
        // Between recordings, as SetDebugView says.
        virtual void SetQuality( const Common::Scalability::ResolvedQuality& quality ) = 0;
        // The GI / reflection / shadow methods this view renders and why any came out below its request — what
        // the viewport overlay draws (Graphic/RenderMethods.hpp). Refreshed every BeginScene.
        // VFX-PORT-2: this view's particle cull (ISceneFXSystem::ViewEmitterCulls); empty without the system.
        [[nodiscard]] virtual std::span<const System::ParticleViewEmitterCull>
                                                   ParticleViewEmitterCulls() const = 0;
        [[nodiscard]] virtual const RenderMethods& GetRenderMethods() const         = 0;
        // GI-ALL-LIGHTS step 5: how the emissive light list followed the scene since the view was made - full
        // scene gathers vs in-place primitive moves (RayTracingSceneRenderer), CPU list rebuilds vs row moves
        // (EmissiveTriangleListCache), GPU static-part rebuilds vs moved-row passes (EmissiveLightsRenderer). A
        // still scene or one whose emitters only move: one full rebuild of each.
        struct EmissiveLightCounters
        {
            uint64_t GatherFullRebuilds   = 0;
            uint64_t GatherMoves          = 0;
            uint64_t ListFullRebuilds     = 0;
            uint64_t ListMoves            = 0;
            uint64_t GpuFullRebuilds      = 0;
            uint64_t GpuMoves             = 0;
            uint64_t GpuMovesRetried      = 0;
            // GI-ALL-LIGHTS step 8: how the view's TLAS followed the scene - frames it was built, frames it was
            // updated in place, and rows those updates patched with other bytes (RayTracingSceneRenderer).
            uint64_t TlasFullBuilds = 0;
            uint64_t TlasUpdates    = 0;
            uint64_t TlasMovedRows  = 0;
        };
        [[nodiscard]] virtual EmissiveLightCounters GetEmissiveLightCounters() const = 0;
        // Read back by the editor's own external passes (grid, colliders), which draw INTO this view and
        // therefore must ask this view what it is showing — not a global, or every offscreen preview would
        // inherit the main viewport's flags.
        [[nodiscard]] virtual const DebugViewState& GetDebugView() const = 0;
        // BY VALUE, and it has to be. SkyboxRenderer::GetEnvironment() composes its answer — procedural
        // bake or skybox-asset environment — and therefore returns a temporary; this used to hand back a
        // reference to it, which dangled the instant the call returned. The forward path read that
        // reference for a long time and mostly got away with it, because a just-freed stack frame usually
        // still holds the bytes. The deferred composite's new environment read did not: it saw a valid
        // irradiance handle beside a zeroed prefiltered one and correctly reported a half-baked
        // environment. Found 2026-09-03 by the diagnostic that was supposed to catch a failed bake.
        virtual std::optional<Environment> GetEnvironment() = 0;
        // A reflection capture's face view (ViewProfile::SceneCapture) is lit by the scene's sky light and bakes
        // none (SkyboxRenderer::BorrowEnvironment): @p environment is the reading view's, handed before each
        // frame.
        virtual void BorrowEnvironment( std::optional<Environment> environment ) = 0;
        // A reflection capture's face views stand at one capture point and read ONE global distance field, their
        // capture renderer's (DistanceField::GlobalDistanceField: updated once per frame): @p field is lent before
        // each frame and outlives the frames in flight that read it; null returns the view to its own field.
        virtual void ShareGlobalDistanceField( DistanceField::GlobalDistanceField* field ) = 0;
        // ... and ONE surface cache (the Lumen scene of their cards): @p cache is their capture renderer's, lent
        // before each frame (one face renders per engine frame, so the cache sees one Update per frame) and
        // outliving the frames in flight that recorded its passes; null returns the view to its scene's cache
        // (Scene::GetSurfaceCache).
        virtual void ShareSurfaceCache( SurfaceCache::SurfaceCacheRenderer* cache ) = 0;

        // Procedural sky configuration (from the SkyAtmosphereComponent + the atmosphere sun, via the ECS).
        // sunDir is the direction TOWARD the sun, already normalized; bakeNow is the one-shot request from
        // the editor's Bake button.
        virtual void SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow, const SkySettings& sky,
                                       const SunLightFx& fx, const AtmosphereLight1& light1 ) = 0;

        // This frame's exponential height fog (from ExponentialHeightFogComponent, via the ECS).
        // `present` = false means the scene has no enabled fog component at all — said explicitly, because
        // the renderer keeps its settings across frames and would otherwise keep evaluating a deleted one.
        // `fogHeightY` is the fog entity's transform Y — the fog floor, owned by the transform and never
        // authored twice.
        virtual void SetHeightFog( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY ) = 0;

        // This frame's volumetric cloud layer (from VolumetricCloudComponent, via the ECS). The wind
        // offset is accumulated by the ECS system, which is where the timestep lives.
        virtual void SetVolumetricClouds( bool present, const ECS::VolumetricCloudData& data,
                                          const glm::vec3& windOffset, const glm::vec3& windDirection,
                                          const std::vector<HeroCloudInstance>& heroClouds ) = 0;

        // This gameplay step's gust fronts and the previous step's (Graphic::PackWindGust), from the ECS, which
        // advances them with the timestep (Render::WindGustCommand). Every view's frame carries them
        // (ViewFrame::WindGust, OnUpdate), and the light cameras the current ones (MeshRenderer::SetSceneWind).
        // It opens the step's local wind list: the step's point sources follow it (AddLocalWindSource), and a
        // step that sends none leaves none.
        virtual void SetWindGust( const glm::vec4& gust, const glm::vec4& prevGust,
                                  const glm::vec2& directionalVelocityXZ ) = 0;

        // One point wind source of this gameplay step (Render::LocalWindSourceCommand), a candidate for the
        // camera block's local sources (Graphic::SelectLocalWind, BeginScene).
        virtual void AddLocalWindSource( const LocalWindCandidate& source ) = 0;

        // The evaluated per-frame sky: sun direction and radiance, ambient above/below, night factor, the
        // planet radius, and an OPAQUE handle to the packed sky-parameter buffer. Consumers never see the
        // sky's authoring representation, so a change to the palette cannot break them.
        virtual const AtmosphereEnv& GetAtmosphere() const = 0;

        // This view's cloud field, prepared for the sky's environment bake. The bake is SkyboxRenderer's
        // and the field is VolumetricCloudRenderer's — two sibling systems that share no resource — so the
        // route between them is here, exactly like GetAtmosphere() above going the other way. See
        // Engine/Graphic/Clouds/CloudEnvironmentBake.hpp for what crosses.
        virtual CloudEnvironmentBake BuildCloudEnvironmentBake() = 0;

        // Is this view rebuilding its cloud modelling volume right now? See
        // System::VolumetricCloudRenderer::IsModellingVolumeBaking for what the answer is FOR — in one
        // line, about half of a cloud material's parameters cost a multi-second bake on a worker, and
        // without a signal the artist cannot tell "expensive" from "broken". The route is here for the same
        // reason BuildCloudEnvironmentBake above is: an editor panel must not reach into a render system.
        //
        // False for a view with no cloud layer, which is every mesh preview and every asset thumbnail.
        virtual bool IsCloudVolumeBaking() const = 0;

        // Whether this view has a sky light (SkyLightState): no sky at all, or a sky whose light is captured in
        // the frame graph from the view's first frame. What tells a defined black ambient (no sky) from a fault.
        [[nodiscard]] virtual SkyLightState GetSkyLightState() const = 0;

        // Whether the scene's sky light is being, or will be, captured again for a sun TOWARD @p sunDir
        // (SkyboxRenderer::SkyLightRecapturesFor); false without a sky system.
        [[nodiscard]] virtual bool SkyLightRecapturesFor( const glm::vec3& sunDir ) const = 0;

        // Whether this view draws a cloud layer: a picture that reprojects a history and converges over frames (a
        // reflection capture's face waits for it: ReflectionCaptureRenderer).
        [[nodiscard]] virtual bool DrawsVolumetricClouds() const = 0;

        // How far that rebuild has got, 0..1. Meaningless unless IsCloudVolumeBaking(); see
        // System::VolumetricCloudRenderer::ModellingBakeProgress for why the wait is worth a number.
        virtual float CloudVolumeBakeProgress() const = 0;

        // "N live, X MiB": views alive right now (ViewResourceRegistry::LiveCount) and what they hold
        // together. Every log line that opens or closes a view prints it, so a view that is never destroyed
        // is readable as a count that only goes up.
        static std::string DescribeLiveViews();

        // Device memory this view holds, MEASURED (RT-LEDGER-3): the bytes of the ledger rows that carry this
        // view's instance — its targets, its temporal history, its per-view buffer copies, its TLAS and its
        // landscape tiles. Not a forecast: ViewTargetCensus only answers "may a view of this size be CREATED"
        // (ViewBudgetGate), before any row exists. The scene-shared rows are the scene's and are NOT here
        // (SceneHoldings). What the view-budget rule counts as "open views hold" (Engine/Core/ViewBudget.hpp).
        [[nodiscard]] virtual uint64_t HeldBytes() const = 0;

        // This view's and its scene's ledger rows, taken now (ViewLedgerMemory).
        [[nodiscard]] virtual ViewLedgerMemory TakeLedgerMemory() const = 0;

        // This view's ledger instance: what an object drawing FOR this view but owned elsewhere (the game's
        // Render2D over it) tags its rows with, so they are in this view's budget (RT-LEDGER-4).
        [[nodiscard]] virtual ResourceOwnerInstance GetLedgerInstance() const = 0;

        // Every live view and what it holds, for the budget's refusal text and its usage stand-in.
        [[nodiscard]] static std::vector<Engine::ViewBudget::HeldView> LiveHoldings();

        // Every live scene instance in the ledger and what its shared rows hold — each scene ONCE, however many
        // views draw it, and never inside a view's entry. The budget's scene entries (ViewBudgetGate).
        [[nodiscard]] static std::vector<Engine::ViewBudget::HeldView> SceneHoldings();

        // The SUMS of LiveHoldings and SceneHoldings, with no names, strings or vectors: the per-frame readers
        // (the status bar, ReadViewBudget) need the total, not the list, and these answer it from the ledger's
        // running per-instance totals — one lookup per live view, one walk of the live scene instances.
        [[nodiscard]] static uint64_t LiveHeldBytes();
        [[nodiscard]] static uint64_t SceneHeldBytes();

        virtual Core::Camera* const& GetMainCamera() const = 0;

        // The virtual shadow map's local lights as last read back: per light its requested, rendered and cached
        // pages (DesertCtl state "vsm"). Empty while no frame drew a local light into the map.
        [[nodiscard]] virtual const std::vector<System::VsmLocalLightStats>&
        GetVirtualShadowLocalStats() const = 0;
        // The virtual shadow map's drawn pages as last read back, static layer and movers apart (state "vsm").
        [[nodiscard]] virtual const System::VsmPageStats& GetVirtualShadowPageStats() const = 0;

        // THE CURRENT FRAME'S VIEW (TAA1 step 3) — what every camera-block writer of this renderer fills
        // CameraUB from (ShaderProtocols::MakeCameraUB). CONTRACT: non-null only while OnUpdate runs, from the
        // moment m_ViewState.BeginFrame accepted the frame until OnUpdate returns (the graph executes and
        // EndFrame commits inside that window). Outside it — before the first frame, after a refused frame,
        // between frames, from another view — it is nullptr, never the last frame's view: a writer that gets
        // nullptr has no view to draw for and writes nothing.
        [[nodiscard]] virtual const ViewFrame* GetViewFrame() const = 0;

        virtual const ShaderProtocols::DirectionLight& GetDirectionLights() const = 0;
        // Atmosphere light 1 as this frame lights it (UpdateInfo::AtmosphereLight1, its colour through its own
        // transmittance), when the scene marks one: MeshRenderer fits its cascades from it.
        [[nodiscard]] virtual const std::optional<AtmosphereLight1Frame>& GetAtmosphereLight1() const = 0;

        // CSM debug: a cascade's static-caster cache (MeshRenderer::GetCascadeCacheImage), for the editor's
        // cascade viewer; the sampled maps are layers of one graph array, which ImGui does not show.
        // THE LAST FINISHED FRAME'S CASCADES, published at EndScene: the interface reads them while the next frame
        // records on the render task, and that frame's OnUpdate may re-budget the shadows (RebudgetShadowsFor
        // re-makes the maps). A published image stays alive in the snapshot until the next EndScene replaces it.
        [[nodiscard]] virtual std::shared_ptr<Image2D> GetShadowCascadeCacheImage( uint32_t cascade ) const = 0;
        [[nodiscard]] virtual uint32_t                 GetShadowCascadeCount() const                        = 0;

        virtual const std::shared_ptr<Image2D> GetFinalImage() = 0;
        // THE DISPLAY-REFERRED FINAL IMAGE'S FRAMEBUFFER: the last post pass's output (tonemap, or FXAA / SMAA
        // after it), the image the viewport shows. The game UI draws on it after the post chain
        // (RDG::ExtensionPoint::UI, UE: Slate/UMG over the final image), so its pipelines are built against it.
        // Every AA mode's output has the one format (ViewTargetFormats::kTonemap), so a pipeline built against one
        // is compatible with all. Null before Init.
        virtual std::shared_ptr<Framebuffer> GetDisplayFramebuffer() const = 0;
        // The single-sample scene depth compute passes sample (height fog, volumetric clouds): the scene depth
        // itself at MSAA 1, SceneDepthResolved at MSAA > 1 ("Scene: DepthResolve"); null when that failed to
        // build.
        virtual std::shared_ptr<Image2D>            GetComputeSceneDepth() const = 0;
        virtual const std::shared_ptr<Framebuffer>& GetTargetFramebuffer() const = 0;

        // Deferred G-buffer (Albedo+Metallic / Normal+Roughness / depth). Populated only in the Deferred path.
        virtual const std::shared_ptr<Framebuffer>& GetGBuffer() const = 0;

        // Active rendering path, refreshed from SceneSettings each BeginScene.
        virtual Core::RenderPath GetRenderPath() const = 0;

        // This frame's point/spot shadows are the virtual shadow map's (Shadows resolved to VirtualMaps, not path
        // traced): "VSM: Project Local" per slot, read by the same RayTracedLocal programs, and every forward
        // reader (the mesh cells, the forward terrain) reads the virtual map in place of the atlas.
        // This frame's RenderPipeline.ParallelSetup (or the --parallel-setup override): a pass may build its
        // setup on Common::TaskGroup tasks (MeshRenderer::DeclareGBufferDraws' early-Z halves).
        [[nodiscard]] virtual bool ParallelSetup() const          = 0;
        [[nodiscard]] virtual bool LocalShadowsAreVirtual() const = 0;
        // This frame's point/spot lights have virtual-shadow-map pages (m_LocalLightsVirtualPages, resolved once
        // per frame by AssignLocalShadowMaskSlots through RenderMethods' LocalLightsVirtualPages):
        // LocalShadowsAreVirtual, or Shadows Stochastic with a light of the view on vsm+screen (MegaLights' trace
        // and volume read the pages by ShadowMaskSlot). The one value every page reader of the frame takes: the
        // page passes' local lights, MegaLights, and the surface cache's cards when this view updates them.
        [[nodiscard]] virtual bool LocalLightsReadVirtualPages() const = 0;
        // The visibility sources MegaLights' trace and volume run this frame: one per source a casting light of
        // the view resolved to (m_StochasticSourceLights, AssignLocalShadowMaskSlots), so each light is tested by
        // its own payload ShadowVisibility. A view with no casting light in it names the frame's Visibility (a
        // vsm+screen frame: screen, no local page was drawn), so the programs always have one source.
        [[nodiscard]] virtual Common::Scalability::StochasticSources StochasticTraceSources() const = 0;
        // The lights whose Shadow Method is not Auto, as AssignLocalShadowMaskSlots resolved them this frame
        // (DesertCtl state render.shadows.lights).
        [[nodiscard]] virtual const std::vector<StochasticLightMethod>& GetStochasticLightMethods() const = 0;
        [[nodiscard]] virtual const System::VirtualShadowMapQuality&    GetVirtualShadowQuality() const   = 0;

        // Shared GraphicsPipeline cache (keyed by shader + target + render-state). Renderers request
        // pipelines from here instead of creating their own; cleared on Init (full rebuild).
        virtual PipelineCache& GetPipelineCache() = 0;

        // True while the scene runs in Play mode (refreshed each BeginScene). Extension passes use this
        // to hide authoring aids during gameplay.
        virtual bool IsScenePlaying() const = 0;

        // BY CONST REFERENCE, because the caller is a RECORDED COMMAND that is replayed once per view
        // of the scene (Scene::OnUpdate). An rvalue parameter invited the recorder to hand its only copy
        // away on the first view, and the second view then got a moved-from light.
        // @p shadow is what the light asks of its shadow; the payload's ShadowView is written later, by
        // MeshRenderer::UpdateLocalShadows, once the frame's atlas is allocated.
        virtual void AddPointLight( const ShaderProtocols::PointLightPayload& pointLight,
                                    const LocalLightShadowSettings&           shadow,
                                    const System::LightFunctionProxy&         function ) = 0;

        virtual const ShaderProtocols::PointLight& GetPointLights() const = 0;

        // This frame's decal boxes (ECS::DecalECSSystem): "Deferred: Decals" draws them into the DBuffer, and any
        // decal forces the full early-Z (the receivers are the prepass depth).
        virtual void AddDecal( const System::DecalProxy& decal ) = 0;

        virtual void AddSpotLight( const ShaderProtocols::SpotLightPayload& spotLight,
                                   const LocalLightShadowSettings&          shadow,
                                   const System::LightFunctionProxy&        function ) = 0;

        // One local fog volume of this frame (LocalFogVolumeCommand, replayed per view: by const reference).
        // The list is reserved once at kVolumetricFogMaxLocalVolumes and emptied with the lights at frame end.
        virtual void AddLocalFogVolume( const LocalFogVolumeGpu& volume ) = 0;
        // One heterogeneous volume of this frame (HeterogeneousVolumeCommand, replayed per view). Reserved once at
        // kHeterogeneousVolumeMaxVolumes, emptied with the lights at frame end; culled per view by the renderer.
        virtual void AddHeterogeneousVolume( const HeterogeneousVolumeInstance& volume ) = 0;

        virtual const ShaderProtocols::SpotLight& GetSpotLights() const = 0;

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
        virtual CloudShadowInput GetCloudShadowInput() const = 0;
        /// A Scalar world parameter as this frame's snapshot holds it (BeginScene): for the CPU side of a pass whose
        /// program takes the value through its own params (GrassRenderer: SnowDepth); 0 when the collection has none.
        [[nodiscard]] virtual float GetWorldParameterScalar( std::string_view name ) const = 0;

        /// The view's snapshot of the world's parameters this frame (Common/WorldParameters.glslh), written once
        /// at BeginScene from Scene::GetWorldParameters; every capture of the frame binds this one buffer.
        [[nodiscard]] virtual const std::shared_ptr<ShaderResources::StorageBuffer>&
        GetWorldParametersBuffer() const = 0;
        // The frame's sun as every lit pass receives it: the first directional light (direction it travels,
        // colour/intensity), or the default sun when the scene has none. The deferred composite and the
        // surface-cache lighting read this one rule.
        struct FrameSun
        {
            glm::vec4 Direction{ 0.0f, -1.0f, 0.0f, 0.0f };
            glm::vec4 ColorIntensity{ 1.0f, 0.98f, 0.92f, 3.0f };
        };
        [[nodiscard]] virtual FrameSun GetFrameSun() const = 0;
        // Atmosphere light 1 (the moon) as the same lit passes receive it: its direction and colour/intensity, or
        // zero colour/intensity when the scene has none (the shaders then add nothing).
        [[nodiscard]] virtual FrameSun GetFrameLight1() const = 0;
        // Both as the world-space GI histories compare them (GlobalIllumination::InvalidatesLightingHistory).
        [[nodiscard]] virtual GlobalIllumination::DirectionalLighting GetFrameDirectionalLighting() const = 0;
        // This frame's point and spot lights (m_PointLight / m_SpotLight with their m_*LightShadows identities)
        // as the world-space GI histories compare them (GlobalIllumination::UpdateLocalLightingHistory),
        // ascending Key; made into m_FrameLocalLighting, its capacity kept across frames.
        [[nodiscard]] virtual std::span<const GlobalIllumination::LocalLighting> GatherFrameLocalLighting() = 0;

        /// THE view's contact shadows (Common/ContactShadow.glslh): the march's view values and the sun's length,
        /// packed into ShadowUB for every lit pass (the deferred composite, the forward meshes, the glass).
        /// @p hasOpaqueDepth says whether the pass has the opaque scene depth to march: the deferred path's
        /// G-buffer depth does; the forward path draws its opaque surfaces with no prepass, so its passes get
        /// Steps = 0 (no march) - there is nothing in front of them for the ray to hit.
        [[nodiscard]] virtual SceneResources::ContactShadowSettings
        GetContactShadowSettings( const ViewFrame& frame, bool hasOpaqueDepth ) const = 0;
        /// THE view's sun PCSS kernel (Scalability Shadows.BlockerSearchTaps / Shadows.FilterTaps), packed into
        /// ShadowUB by every lit pass that samples the cascades.
        [[nodiscard]] virtual SceneResources::ShadowFilterSettings GetShadowFilterSettings() const = 0;
        /// SSS-PROFILE: this view scatters and its Burley history has not yet averaged its full window (a Scale
        /// change, a camera cut, the first frames) - the control gate's PendingWork::SubsurfaceHistory.
        [[nodiscard]] virtual bool SubsurfaceHistoryConverging() const = 0;
        /// GRASS-SETTLE p14: this view's last frame drew a grass variety that may hold fewer instances than its
        /// generator keeps (GrassRenderer::InstancesSettling) - the control gate's PendingWork::GrassInstances.
        [[nodiscard]] virtual bool GrassInstancesSettling() const = 0;

        // --- UI glass (backdrop blur) -----------------------------------------------------------
        // The UI canvas raises this when it recorded a glass element; the blur pyramid is then built
        // before the NEXT frame's UI extension point. Latched per frame, so a canvas that stops using glass stops
        // paying for it.
        virtual void SetBackdropBlurNeeded( bool needed ) = 0;

        // This view's RT scene (instances, skips, stats) as of its last frame - the viewport's RT scene overlay.
        [[nodiscard]] virtual const RayTracing::RayTracingSceneRenderer& GetRayTracingScene() const = 0;

    protected:
        SceneRenderer() = default;
    };
} // namespace Desert::Graphic
