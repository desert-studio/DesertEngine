#pragma once

#include <Engine/Graphic/DistanceField/MeshDistanceFieldReads.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/View/PathTracedGuides.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    // RDG-A2. The transients of THIS frame graph that one renderer's passes produce and a LATER renderer's passes
    // read (UE: FSceneTextures / the RDG texture fields passed between AddPass helpers). The producer's
    // AddFrame* creates the texture with Builder::CreateTexture (desc from this frame's view) and stores the ref
    // here; the consumer's AddFrame* declares a read of it and binds it with RDG::PassBindings. An invalid ref
    // means the producer did not run this frame (effect off, culled input): the consumer declares a read of
    // FrameTextures::System.Black, binds it at that slot and writes zero intensity in its uniform values - an
    // explicit choice at the call site (UE: FRDGSystemTextures::Black), never a stale image.
    // Only cross-renderer transients live here; a transient read only by its own renderer's passes (SMAA edges,
    // JFA ping-pong, SSR trace/tiles, cloud trace/guide, the exposure histogram) stays a local of that
    // AddFrame*. A history (read in a LATER frame) is never here: it is an external the renderer owns.
    // The struct is rebuilt with every graph and dies with it.
    // The most shadow cascades a lit pass samples (the layers of ShadowCascadesTexture: every shadowed directional
    // light's); SceneResources::kMaxCascadeViews is asserted equal where both are visible (MeshRendererInternal.hpp).
    inline constexpr uint32_t kSceneViewShadowCascades = 8;

    // The view's dynamic lights as buffers of THIS frame's graph (SceneRenderer::AddFrameLightGrid, before any lit
    // node): the point and spot rows, the local shadow views their ShadowView indices name, and the view's light
    // grid (Graphic/LightGrid.hpp, UE's ForwardLightData) — its parameters (uploaded) and its cells (written by
    // the "LightGrid" compute node). Every lit pass reads the grid: the deferred composite, the forward meshes.
    // THE VIEW'S VIRTUAL SHADOW MAP AS ITS READERS SEE IT (Shadows.Method VirtualMaps, System::VirtualShadowMapFrame
    // after "VSM: Raster"): the level table, the page table, the physical pool and the frame's shadowed point / spot
    // lights, read through Common/VirtualShadowMap.glslh's reader by every DESERT_LOCAL_SHADOW_VSM program - the
    // forward and translucent cells, the forward terrain, the deferred composite's subsurface transmission and the
    // surface cache's cards - in place of the local shadow atlas. Invalid refs before SceneRenderer::
    // AddFrameVirtualShadows ran or when it drew nothing; a VSM reader then faults its pass by name.
    struct VirtualShadowMapReadRefs
    {
        RDG::BufferRef  FrameData;    // "VsmFrameData"
        RDG::BufferRef  PageTable;    // "VsmPageTable"
        RDG::TextureRef PhysicalPool; // "PhysicalPoolTexture" (R32_UINT, read as a storage image)
        RDG::BufferRef  LocalLights;  // "VsmLocalLightsUB": one VsmLocalLight per shadowMaskSlot

        [[nodiscard]] bool IsValid() const
        {
            return FrameData.IsValid() && PageTable.IsValid() && PhysicalPool.IsValid() && LocalLights.IsValid();
        }
    };

    struct ViewLightBuffers
    {
        RDG::BufferRef Point;       // "Upload: Lights.Point"
        RDG::BufferRef Spot;        // "Upload: Lights.Spot"
        RDG::BufferRef ShadowViews; // "Upload: Lights.ShadowViews"
        RDG::BufferRef GridParams;  // "Upload: Lights.GridParams"
        RDG::BufferRef GridCells;   // written by "LightGrid"
        // The world layout of the same lights (LightGridLayout::WorldClipmaps: the radiance cache's clipmap cells,
        // for GI hits off screen). Invalid unless the frame has a reader of it
        // (SceneRenderer::WorldLightGridRead).
        RDG::BufferRef WorldGridParams; // "Upload: Lights.WorldGridParams"
        RDG::BufferRef WorldGridCells;  // written by "LightGridWorld"
    };

    // THE MEGALIGHTS TRANSLUCENCY VOLUME of a view (System::MegaLightsVolumeRenderer;
    // Common/MegaLightsVolume.glslh): the view's frustum cut into froxels, each holding the local lights' incident
    // radiance as L1 spherical harmonics, one RGBA16F volume per colour channel (L0, L1 x y z). The forward and
    // translucent surfaces read their local lights from it, shadowed, under Shadows Stochastic. Off (any other
    // Shadows method, or no local light) it is a block whose Size.w is 0 and System.Black3D in the three slots:
    // the forward cell then loops the light grid, the glass adds nothing. Set by
    // SceneRenderer::OnUpdate right after the light grid (off), then lit by SceneRenderer::AddFrameMegaLightsVolume
    // once its visibility source is drawn; invalid refs before the grid, as the grid's.
    struct MegaLightsVolumeRefs
    {
        RDG::BufferRef                 Params;   // "MegaLights.Volume.Params", MegaLightsVolumeReadParams
        std::array<RDG::TextureRef, 3> Lighting; // R, G, B
    };

    struct FrameTransients
    {
        RDG::TextureRef Bloom;          // BloomRenderer chain (mip 0 read)   -> Tonemap
        RDG::TextureRef LightShafts;    // LightShaftRenderer result          -> Tonemap
        RDG::TextureRef LensFlare;      // LensFlareRenderer result           -> Tonemap
        // LocalExposureRenderer: the bilateral grid (3D) and the blurred 1/32 log luminance -> Bloom mip 0, Tonemap
        RDG::TextureRef LocalExposureGrid;
        RDG::TextureRef BlurredLogLuminance;
        // LocalExposureRenderer, Fusion: level 0 of the blended pyramid (fused display value, R32F) -> Tonemap
        RDG::TextureRef ExposureFusion;
        RDG::TextureRef SSAO;           // SSAO factor                        -> deferred lighting
        // The closest-depth HZB of the opaque scene depth (the G-buffer's; on the forward path the depth prepass's,
        // single-sample), built once per graph by its first reader (SceneRenderer::AddFrameHZB)   -> the screen
        // traces (SSR, screen probes), MegaLights' trace and its volume
        RDG::TextureRef ClosestHzb;
        // SSR's full-resolution trace ray (xyz the point each pixel's reflection came from, w log2 pdf, -126 no
        // ray), set only when the temporal upscaler takes the reflection raw  -> TemporalUpscalerInputs
        RDG::TextureRef ReflectionRay;
        // The path tracer's own surface guides of this frame's raw paths, set only when it handed them to the
        // upscaler that denoises (System::PathTracer::AddPasses)              -> TemporalUpscalerInputs
        PathTracedGuides PathGuides;
        RDG::TextureRef SceneColorCopy; // scene snapshot                     -> glass refraction
        // The opaque scene's device depth (the G-buffer depth, reversed-Z), set by the deferred composite; invalid
        // on the forward path, which has no depth before its meshes draw   -> contact shadows of the forward
        // meshes and the glass (SceneViewInputs::SceneDepth)
        RDG::TextureRef SceneDepth;
        // The opaque scene's shading word (the G-buffer's, RG32_UINT), set beside SceneDepth by the deferred
        // composite: the NO_CONTACT_SHADOW bit of the texel a forward contact-shadow ray hit (UE
        // CastContactShadow( GBuffer ))                                    -> SceneViewInputs::SceneShadingWord
        RDG::TextureRef SceneShadingWord;
        RDG::TextureRef BackdropBlur;   // BackdropBlurRenderer pyramid (all mips) -> UI glass (Render2D)
        RDG::TextureRef HeightFog;      // HeightFogRenderer evaluation (RGBA16F) -> HeightFogApply
        // The frame's fog as every TRANSLUCENT surface evaluates it at its own depth (Common/ViewFog.glslh): the
        // FogParams block (graph buffer, uploaded every frame - gates 0 when there is no fog), the sky's aerial
        // perspective volume and distant sky light, and the volumetric fog's integrated froxels (each the engine's
        // fallback when absent). Set by SceneRenderer::AddFrameAtmosphericFog, before any translucent draw.
        RDG::BufferRef  FogParams;
        RDG::TextureRef FogAerialPerspective;
        RDG::TextureRef FogDistantSkyLight;
        RDG::TextureRef FogVolume;
        // The clouds' full-resolution image written this frame (VolumetricCloudRenderer::GetFrameResult().Upsampled,
        // CloudUpsample.shader: premultiplied radiance + transmittance per framebuffer pixel); invalid when the frame
        // has none                                                             -> CloudComposite
        RDG::TextureRef CloudScatter;
        // The cloud layer's shadow map (VolumetricCloudRenderer::GetShadowMap, a transient of this graph), set
        // once its shadow node was accepted; invalid when the frame has none   -> Deferred: Composite
        RDG::TextureRef CloudShadowMap;
        // The view's rain occlusion map (MeshRenderer::GetRainOcclusionImage, imported: the renderer owns it and
        // redraws it only when its box moves); invalid when the view has none   -> the wetted mesh cells
        RDG::TextureRef RainOcclusionMap;
        // The view's velocity (ViewRasterTargets.hpp CreateViewVelocity): one per view graph, the colour slot
        // every depth-writing node on the scene target / G-buffer writes, never on a light view   -> TAA
        RDG::TextureRef Velocity;
        // The view's responsive-AA mask (ViewRasterTargets.hpp CreateViewResponsiveAA): a colour slot of the scene
        // target only, written by ResponsiveAA translucency (glass, particles)   -> TAA
        RDG::TextureRef ResponsiveAA;
        // The view's thin-geometry coverage (ViewRasterTargets.hpp CreateViewThinGeometryCoverage): a colour slot of
        // the scene target and of the G-buffer, written by the passes that write the view's depth   -> TSR
        RDG::TextureRef ThinGeometryCoverage;
        // The scene/view inputs every lit pass samples (UE: the view uniform buffer's shadow / sky /
        // PreIntegratedGF textures), imported once per graph by SceneRenderer::ImportSceneViewTextures before any
        // node is added: the sun's cascade array (MeshRenderer::AcquireCascadeMaps, layer c = cascade c; a cleared
        // one-layer array when the view has no cascades - the shader's sampler2DArray takes no 2D stand-in), the
        // scene environment's
        // two cubes (invalid when the scene has no baked sky) and the renderer's split-sum BRDF LUT. Never read
        // directly: a pass takes SceneViewInputsOf, which states the neutral texture for each one that is absent.
        RDG::TextureRef ShadowCascades;
        // The scene's local-light shadow atlas (MeshRenderer::GetLocalShadowAtlasImage, imported: Core::Scene owns
        // it and keeps cached tiles in it across frames). Invalid while no light of the scene has cast a shadow.
        RDG::TextureRef                                       LocalShadowAtlas;
        RDG::TextureRef                                       EnvIrradiance;
        RDG::TextureRef                                       EnvSpecular;
        RDG::TextureRef                                       BrdfLut;
        // The view's reflection captures (ReflectionCaptureRenderer::AddPasses, every graph): the cube array and
        // the capture list the lighting pass reads (Common/ReflectionEnvironment.glslh)   -> Deferred: Composite
        RDG::TextureRef ReflectionCaptureCubes;
        RDG::BufferRef  ReflectionCaptures;
        // The procedural sky's transmittance / sky-view LUTs (SkyboxRenderer, imported under the names its LUT
        // nodes use), set only when SkyboxRenderer::SkyPassSamplesLuts; invalid otherwise   -> SkyboxPass
        RDG::TextureRef SkyTransmittanceLut;
        RDG::TextureRef SkyViewLut;
        // Atmosphere light 1's (the moon's) Sky-View LUT, set only when SkyboxRenderer::SkyPassSamplesLight1Lut.
        RDG::TextureRef SkyViewLut1;
        // The view's lights and light grid (ViewLightBuffers); invalid refs until AddFrameLightGrid ran.
        ViewLightBuffers Lights;
        // The view's virtual shadow map for its readers (VirtualShadowMapReadRefs); invalid until
        // AddFrameVirtualShadows ran.
        VirtualShadowMapReadRefs Vsm;
        // The MegaLights translucency volume (MegaLightsVolumeRefs); invalid refs until AddFrameMegaLightsVolume.
        MegaLightsVolumeRefs MegaLightsVolume;
        // The global distance field brought up to this frame (SceneRenderer::AddFrameGlobalDistanceField,
        // imported: DistanceField::GlobalDistanceField owns the clipmaps) and its clipmap parameters (centre.xyz
        // cm, voxel cm); invalid refs when nothing of the frame reads it   -> the radiance cache's trace, "Debug:
        // Global Distance Field"
        std::array<RDG::TextureRef, 4> GlobalDistanceField;
        std::array<glm::vec4, 4>       GlobalDistanceFieldLevels{};
        RDG::TextureRef                GlobalDistanceFieldObjectGrid; // its object grid (GlobalDistanceField.hpp)
        // The mesh fields it is composited from and the near-field object grid (the same update) -> the GI
        // tracer's near field
        DistanceField::MeshDistanceFieldReads MeshDistanceFields;
        // The traced GI's final gather of this frame (SceneRenderer::AddFrameTracedGI: the world radiance cache's
        // update, then GlobalIllumination::ScreenProbeGather under SoftwareTraced / HardwareTraced or
        // GlobalIllumination::IrradianceFieldGather under ProbeVolume): per pixel, the diffuse irradiance and the
        // specular radiance. Invalid when the GI method traces nothing   -> Deferred: Composite
        RDG::TextureRef GIDiffuse;
        RDG::TextureRef GISpecular;
        RDG::TextureRef GIBackface; // the irradiance from behind the surface (-N), a two-sided leaf's
        // THE VIEW'S WORLD RADIANCE CACHE as its readers bind it (GlobalIllumination::RadianceCacheFrame, the read
        // section of Common/RadianceCache.glslh): the neutral cache - one zeroed params block standing for all
        // three buffers (no clipmap: every read answers "past the coarsest clipmap"), System.Black for both
        // atlases - from SceneRenderer::ImportSceneViewTextures, replaced by this frame's update once
        // AddFrameTracedGI ran it   -> the forward meshes' and the glass's diffuse estimator, which must be the
        // one the reflection captures' stored diffuse was measured with
        RDG::BufferRef  RadianceCacheParams;
        RDG::BufferRef  RadianceCacheIndirection;
        RDG::BufferRef  RadianceCacheProbes;
        RDG::TextureRef RadianceCacheIrradiance;
        RDG::TextureRef RadianceCacheDepth;
        // The gather above already carries its own contact occlusion (the screen probes' short-range AO): the
        // composite then applies only the material AO to it, never SSAO on top (UE r.Lumen.DiffuseIndirect.SSAO
        // 0).
        bool GIOccluded = false;
    };

    // RDG-A2 (owner decision 2, 2026-10-05). What EVERY node of the frame graph is handed, whatever registered
    // it (an AddFrame* lambda, a system's SystemRasterPass, a system's ComputeNodeDeclaration, an editor
    // ExtensionPass): this frame's cross-renderer transients and the engine's system textures, as
    // refs of THIS graph. Its declaration callback gets it to name what it reads (RenderPassDeclaration::Read of
    // a TextureRef), its body gets it with the node's RDG::PassContext to bind those refs by shader name
    // (RDG::PassBindings). The value is taken when the node is added, so a node sees every transient a node
    // added before it produced; the refs are handles of this graph and die with it. No Image2D* of a graph
    // resource crosses into a node body.
    struct FrameGraphRefs
    {
        FrameTransients     Transients;
        RDG::SystemTextures System;
    };

    // What a cloud-shadow receiver samples as CloudShadowMapTexture this frame: the cloud layer's map, or
    // System.White (the sun is not occluded) when the frame has none. The ONE choice every receiver pass
    // (deferred composite, forward meshes, glass, terrain) declares and binds.
    [[nodiscard]] inline RDG::TextureRef CloudShadowMapOrWhite( const FrameGraphRefs& refs )
    {
        return refs.Transients.CloudShadowMap.IsValid() ? refs.Transients.CloudShadowMap : refs.System.White;
    }

    // MESH-PB1. THE scene/view inputs of a lit pass (UE: the view uniform buffer's textures - shadow maps, sky
    // cubes, PreIntegratedGF - which are pass parameters and never material parameters): every ref valid, an
    // absent producer stated as the engine's neutral texture (UE GSystemTextures). Forward meshes, glass and the
    // deferred composite take this ONE value, declare its reads (Refs) and bind it (BindSceneViewInputs).
    struct SceneViewInputs
    {
        RDG::TextureRef ShadowCascades; // the cascade array (always one: ImportSceneViewTextures)
        RDG::TextureRef                                       EnvIrradiance; // diffuse cube, or System.BlackCube
        RDG::TextureRef                                       EnvSpecular; // prefiltered cube, or System.BlackCube
        RDG::TextureRef                                       BrdfLut;     // split-sum LUT, or System.Black
        RDG::TextureRef                                       CloudShadowMap; // CloudShadowMapOrWhite
        // The reflection captures: their cube array (System.BlackCubeArray when the view reads none or their
        // programs did not build; System.BlackCube is a cube, not an array) and their list.
        RDG::TextureRef ReflectionCaptureCubes;
        RDG::BufferRef  ReflectionCaptures;
        // The point/spot shadow atlas, or System.White (depth 1 everywhere = "no occluder"; no light then names a
        // view of it, so it is never read).
        RDG::TextureRef LocalShadowAtlas;
        // The opaque scene's device depth the contact shadows march (Common/ContactShadow.glslh), or System.Black
        // (reversed-Z 0 = the far plane everywhere: nothing stands in front of any ray, so no contact shadow).
        RDG::TextureRef SceneDepth;
        // The opaque scene's shading word, whose NO_CONTACT_SHADOW bit picks the hit's Casting / Non Casting
        // Intensity, or System.ZeroUint (an integer texture for the usampler2D; never decisive: with the black
        // depth no ray hits).
        RDG::TextureRef SceneShadingWord;
        // The view's light grid (ViewLightBuffers::GridParams / GridCells): a lit pass shades a pixel with the
        // point and spot lights of its froxel. Bound where the layout has the two storage blocks.
        RDG::BufferRef LightGridParams;
        RDG::BufferRef LightGridCells;
        // The table of the local shadow atlas's views (ViewLightBuffers::ShadowViews) each point/spot row's
        // shadowView names: read with the atlas wherever a lit pass shades the grid's lights
        // (Mesh/LocalLightShadow.glslh).
        RDG::BufferRef LocalShadowViews;
        // "Shadows: Ray Traced Sun": the render-resolution screen shadow mask the trace wrote (R = sun visibility,
        // SceneRenderer::AddFrameRayTracedSunShadow), read by a forward draw's DESERT_SUN_SHADOW_SCREEN_MASK
        // permutation in place of the cascades. Invalid when the sun is not ray traced (nothing then reads it).
        RDG::TextureRef SunShadowMask;
        // "Shadows: Ray Traced Local": the view's local-light masks (SceneRenderer::AddFrameRayTracedLocalShadows, one
        // layer per ShadowMaskSlot), read by an OPAQUE forward draw's DESERT_LOCAL_SHADOW_SCREEN_MASK permutation in
        // place of the atlas. Invalid when the local shadows are not ray traced (the permutation is then not chosen).
        RDG::TextureRef LocalShadowMasks;
        // Shadows.Method VirtualMaps: the view's virtual shadow map (FrameTransients::Vsm), read by a
        // DESERT_LOCAL_SHADOW_VSM program in place of the atlas.
        VirtualShadowMapReadRefs Vsm;
        // The view's world radiance cache (FrameTransients::RadianceCache*, the neutral one where the view traces
        // no GI): the forward passes read their diffuse estimator from it, as the deferred composite's final
        // gather does, so a capture measured by the cache is normalised by the cache on a forward pixel too.
        RDG::BufferRef  RadianceCacheParams;
        RDG::BufferRef  RadianceCacheIndirection;
        RDG::BufferRef  RadianceCacheProbes;
        RDG::TextureRef RadianceCacheIrradiance;
        RDG::TextureRef RadianceCacheDepth;
        // The MegaLights translucency volume (MegaLightsVolumeRefs): its block and three volumes, bound where the
        // layout reads them (the forward and translucent cells, Common/MegaLightsVolume.glslh).
        MegaLightsVolumeRefs MegaLightsVolume;
        RDG::TextureRef RainOcclusionMap; // the view's map, or System.White (depth 1: nothing overhead)
        // The fog a translucent surface applies at its own depth (FrameTransients::FogParams and its three
        // images).
        RDG::BufferRef  FogParams;
        RDG::TextureRef FogAerialPerspective;
        RDG::TextureRef FogDistantSkyLight;
        RDG::TextureRef FogVolume;

        // Each distinct texture once (System.White fills several slots): what the node declares as read.
        [[nodiscard]] std::vector<RDG::TextureRef> Refs() const
        {
            std::vector<RDG::TextureRef> refs;
            const auto                   add = [&refs]( RDG::TextureRef ref )
            {
                if ( ref.IsValid() && std::find( refs.begin(), refs.end(), ref ) == refs.end() )
                    refs.push_back( ref );
            };
            add( ShadowCascades );
            for ( const RDG::TextureRef ref :
                  { EnvIrradiance, EnvSpecular, BrdfLut, CloudShadowMap, ReflectionCaptureCubes, LocalShadowAtlas,
                    SceneDepth, SceneShadingWord, SunShadowMask, LocalShadowMasks, RadianceCacheIrradiance, RadianceCacheDepth,
                    RainOcclusionMap, FogAerialPerspective, FogDistantSkyLight, FogVolume } )
                add( ref );
            for ( const RDG::TextureRef ref : MegaLightsVolume.Lighting )
                add( ref );
            return refs;
        }
    };

    // The shader names the inputs are bound under (Mesh/CascadedShadow.glslh, Mesh/AmbientIBL.glslh, the cloud
    // shadow receiver). Every one is a separate texture read through a shared sampler state of the pass (the slot
    // names in SceneViewDetail::BindWhere); the cascades are one `texture2DArray ShadowCascadesTexture`, layer c =
    // cascade c.
    inline constexpr std::string_view kSceneViewShadowCascadesName   = "ShadowCascadesTexture";
    inline constexpr std::string_view kSceneViewEnvIrradianceName  = "EnvironmentIrradianceTexture";
    inline constexpr std::string_view kSceneViewEnvSpecularName    = "EnvironmentSpecularTexture";
    inline constexpr std::string_view kSceneViewBrdfLutName        = "BrdfLutTexture";
    inline constexpr std::string_view kSceneViewCloudShadowMapName   = "CloudShadowMapTexture";
    inline constexpr std::string_view kSceneViewReflectionCubesName = "ReflectionCaptureCubesTexture";
    inline constexpr std::string_view kSceneViewReflectionListName  = "ReflectionCapturesUB";
    inline constexpr std::string_view kSceneViewLocalShadowAtlasName = "LocalShadowAtlasTexture";
    inline constexpr std::string_view kSceneViewSceneDepthName       = "SceneDepthTexture";
    inline constexpr std::string_view kSceneViewSceneShadingWordName = "SceneShadingWordTexture";
    // DeferredLighting.shader and Mesh/Surface/Pass_Forward.glslh's DESERT_SUN_SHADOW_SCREEN_MASK branch.
    inline constexpr std::string_view kSceneViewSunShadowMaskName = "ScreenShadowMaskTexture";
    // DeferredLighting.shader and Mesh/ForwardLocalLights.glslh's DESERT_LOCAL_SHADOW_SCREEN_MASK branch.
    inline constexpr std::string_view kSceneViewLocalShadowMasksName = "LocalShadowMasksTexture";
    inline constexpr std::string_view kSceneViewRainOcclusionMapName = "RainOcclusionMapTexture";
    // Its own shared sampler state (SharedSamplerBinding.hpp): the G-buffer cells declare none of the others.
    inline constexpr std::string_view kSceneViewRainOcclusionSamplerName = "RainOcclusionSampler";
    // The permutation define of every program that reads kSceneViewSunShadowMaskName in place of the cascades
    // (DeferredLighting.shader, Mesh/Surface/Pass_Forward.glslh): one name for every renderer that acquires it.
    inline constexpr std::string_view kSunShadowScreenMaskDefine = "DESERT_SUN_SHADOW_SCREEN_MASK";
    // The deferred composite's ray-traced local lights (Shadows HardwareTraced): each light with a ShadowMaskSlot
    // reads its layer of LocalShadowMasksTexture (System::RayTracedLocalShadowRenderer) in place of its shadow map.
    inline constexpr std::string_view kLocalShadowScreenMaskDefine = "DESERT_LOCAL_SHADOW_SCREEN_MASK";
    // Shadows.Method VirtualMaps: every reader of the point/spot lights' shadows (the forward and translucent cells,
    // the forward terrain, the composite's transmission, the surface cache's cards) reads the virtual shadow map
    // (VirtualShadowMapReadRefs, Common/VirtualShadowMap.glslh's reader) in place of the atlas, which is not drawn.
    inline constexpr std::string_view kLocalShadowVsmDefine = "DESERT_LOCAL_SHADOW_VSM";
    // The deferred composite's MegaLights program (Shadows Stochastic): the local lights from MegaLights'
    // MegaLightsDiffuseTexture / MegaLightsSpecularTexture (System::MegaLightsRenderer) in place of the light-grid loop.
    inline constexpr std::string_view kMegaLightsDefine = "DESERT_MEGALIGHTS";
    // The radiance cache's read (Common/RadianceCache.glslh; the names every reader of the cache binds).
    inline constexpr std::string_view kSceneViewRadianceCacheParamsName      = "RadianceCacheParamsUB";
    inline constexpr std::string_view kSceneViewRadianceCacheIndirectionName = "RadianceCacheIndirectionUB";
    inline constexpr std::string_view kSceneViewRadianceCacheProbesName      = "RadianceCacheProbesUB";
    inline constexpr std::string_view kSceneViewRadianceCacheIrradianceName  = "RadianceCacheIrradianceTexture";
    inline constexpr std::string_view kSceneViewRadianceCacheDepthName       = "RadianceCacheDepthTexture";
    // The light grid's two blocks (Mesh/LightGrid.glslh): the build shader, the composite, the forward cells.
    inline constexpr std::string_view kLightGridParamsBlockName = "LightGridParamsUB";
    inline constexpr std::string_view kLightGridCellsBlockName  = "LightGridCellsUB";
    // The local shadow atlas's view table (Mesh/LocalLightShadowView.glslh): the composite, the forward cells.
    inline constexpr std::string_view kLocalShadowViewsBlockName = "LocalShadowViewsUB";
    // The virtual shadow map's reader tables (Common/VirtualShadowMap.glslh, VirtualShadowMapReadRefs).
    inline constexpr std::string_view kVsmFrameDataBlockName    = "VsmFrameData";
    inline constexpr std::string_view kVsmPageTableBlockName    = "VsmPageTable";
    inline constexpr std::string_view kVsmPhysicalPoolName      = "PhysicalPoolTexture";
    inline constexpr std::string_view kVsmLocalLightsBlockName  = "VsmLocalLightsUB";
    // A READER of the world layout's cells (Mesh/LightGrid.glslh, LIGHT_GRID_WORLD_CELLS); the build shader writes
    // either layout through kLightGridCellsBlockName.
    inline constexpr std::string_view kLightGridWorldCellsBlockName = "LightGridWorldCellsUB";
    // The MegaLights translucency volume's names (Common/MegaLightsVolume.glslh): its block and its three volumes.
    inline constexpr std::string_view                kMegaLightsVolumeBlockName = "MegaLightsVolumeUB";
    inline constexpr std::array<std::string_view, 3> kMegaLightsVolumeNames     = {
         "MegaLightsVolumeRTexture", "MegaLightsVolumeGTexture", "MegaLightsVolumeBTexture" };
    // Mesh/Surface/SceneFog.glslh (and Programs/Water/WaterSurface.shader through it): the fog block and its
    // images, each a separate texture read through the pass's LinearClampSampler.
    inline constexpr std::string_view kSceneFogParamsBlockName       = "FogParamsBuffer";
    inline constexpr std::string_view kSceneFogAerialPerspectiveName = "SceneFogAerialPerspectiveTexture";
    inline constexpr std::string_view kSceneFogDistantSkyLightName   = "SceneFogDistantSkyLightTexture";
    inline constexpr std::string_view kSceneFogVolumeName            = "SceneFogVolumeTexture";
    // The same names as the parameters a block binds them through (RDGShaderParameters.hpp): the string spelling
    // above stays the one source, these are its binding form.
    inline constexpr RDG::TextureParameter       kSceneViewShadowCascadesTexture{ kSceneViewShadowCascadesName };
    inline constexpr RDG::TextureParameter       kSceneViewEnvIrradiance{ kSceneViewEnvIrradianceName };
    inline constexpr RDG::TextureParameter       kSceneViewEnvSpecular{ kSceneViewEnvSpecularName };
    inline constexpr RDG::TextureParameter       kSceneViewBrdfLut{ kSceneViewBrdfLutName };
    inline constexpr RDG::TextureParameter       kSceneViewCloudShadowMap{ kSceneViewCloudShadowMapName };
    inline constexpr RDG::TextureParameter       kSceneViewReflectionCubes{ kSceneViewReflectionCubesName };
    inline constexpr RDG::StorageBufferParameter kSceneViewReflectionList{ kSceneViewReflectionListName };
    inline constexpr RDG::TextureParameter       kSceneViewLocalShadowAtlas{ kSceneViewLocalShadowAtlasName };
    inline constexpr RDG::TextureParameter       kSceneViewSceneDepth{ kSceneViewSceneDepthName };
    inline constexpr RDG::TextureParameter       kSceneViewSceneShadingWord{ kSceneViewSceneShadingWordName };
    inline constexpr RDG::TextureParameter       kSceneViewSunShadowMask{ kSceneViewSunShadowMaskName };
    inline constexpr RDG::TextureParameter       kSceneViewLocalShadowMasks{ kSceneViewLocalShadowMasksName };
    inline constexpr RDG::StorageBufferParameter kLightGridParamsBlock{ kLightGridParamsBlockName };
    inline constexpr RDG::StorageBufferParameter kLightGridCellsBlock{ kLightGridCellsBlockName };
    inline constexpr RDG::StorageBufferParameter kLocalShadowViewsBlock{ kLocalShadowViewsBlockName };
    inline constexpr RDG::StorageBufferParameter  kVsmFrameDataBlock{ kVsmFrameDataBlockName };
    inline constexpr RDG::StorageBufferParameter  kVsmPageTableBlock{ kVsmPageTableBlockName };
    inline constexpr RDG::StorageTextureParameter kVsmPhysicalPool{ kVsmPhysicalPoolName };
    inline constexpr RDG::StorageBufferParameter  kVsmLocalLightsBlock{ kVsmLocalLightsBlockName };
    inline constexpr RDG::StorageBufferParameter kLightGridWorldCellsBlock{ kLightGridWorldCellsBlockName };
    inline constexpr RDG::TextureParameter       kSceneViewRainOcclusionMap{ kSceneViewRainOcclusionMapName };
    inline constexpr RDG::SamplerParameter kSceneViewRainOcclusionSampler{ kSceneViewRainOcclusionSamplerName };
    inline constexpr RDG::StorageBufferParameter kSceneViewRadianceCacheParams{
         kSceneViewRadianceCacheParamsName };
    inline constexpr RDG::StorageBufferParameter kSceneViewRadianceCacheIndirection{
         kSceneViewRadianceCacheIndirectionName };
    inline constexpr RDG::StorageBufferParameter kSceneViewRadianceCacheProbes{
         kSceneViewRadianceCacheProbesName };
    inline constexpr RDG::TextureParameter kSceneViewRadianceCacheIrradiance{
         kSceneViewRadianceCacheIrradianceName };
    inline constexpr RDG::TextureParameter       kSceneViewRadianceCacheDepth{ kSceneViewRadianceCacheDepthName };
    inline constexpr RDG::StorageBufferParameter kMegaLightsVolumeBlock{ kMegaLightsVolumeBlockName };
    inline constexpr std::array<RDG::TextureParameter, 3> kMegaLightsVolumeTextures{
         RDG::TextureParameter( kMegaLightsVolumeNames[0] ), RDG::TextureParameter( kMegaLightsVolumeNames[1] ),
         RDG::TextureParameter( kMegaLightsVolumeNames[2] ) };
    inline constexpr RDG::StorageBufferParameter kSceneFogParamsBlock{ kSceneFogParamsBlockName };
    inline constexpr RDG::TextureParameter       kSceneFogAerialPerspective{ kSceneFogAerialPerspectiveName };
    inline constexpr RDG::TextureParameter       kSceneFogDistantSkyLight{ kSceneFogDistantSkyLightName };
    inline constexpr RDG::TextureParameter       kSceneFogVolume{ kSceneFogVolumeName };

    [[nodiscard]] inline SceneViewInputs SceneViewInputsOf( const FrameGraphRefs& refs )
    {
        const FrameTransients& t = refs.Transients;
        SceneViewInputs        inputs;
        inputs.ShadowCascades         = t.ShadowCascades;
        inputs.EnvIrradiance  = t.EnvIrradiance.IsValid() ? t.EnvIrradiance : refs.System.BlackCube;
        inputs.EnvSpecular    = t.EnvSpecular.IsValid() ? t.EnvSpecular : refs.System.BlackCube;
        inputs.BrdfLut        = t.BrdfLut.IsValid() ? t.BrdfLut : refs.System.Black;
        inputs.CloudShadowMap         = CloudShadowMapOrWhite( refs );
        inputs.ReflectionCaptureCubes = t.ReflectionCaptureCubes;
        inputs.ReflectionCaptures     = t.ReflectionCaptures;
        inputs.LocalShadowAtlas       = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;
        inputs.SceneDepth             = t.SceneDepth.IsValid() ? t.SceneDepth : refs.System.Black;
        inputs.SceneShadingWord       = t.SceneShadingWord.IsValid() ? t.SceneShadingWord : refs.System.ZeroUint;
        inputs.RainOcclusionMap       = t.RainOcclusionMap.IsValid() ? t.RainOcclusionMap : refs.System.White;
        inputs.LightGridParams        = t.Lights.GridParams;
        inputs.LightGridCells         = t.Lights.GridCells;
        inputs.LocalShadowViews       = t.Lights.ShadowViews;
        inputs.Vsm                    = t.Vsm;
        inputs.FogParams              = t.FogParams;
        inputs.FogAerialPerspective   = t.FogAerialPerspective;
        inputs.FogDistantSkyLight     = t.FogDistantSkyLight;
        inputs.FogVolume              = t.FogVolume;
        inputs.RadianceCacheParams      = t.RadianceCacheParams;
        inputs.RadianceCacheIndirection = t.RadianceCacheIndirection;
        inputs.RadianceCacheProbes      = t.RadianceCacheProbes;
        inputs.RadianceCacheIrradiance =
             t.RadianceCacheIrradiance.IsValid() ? t.RadianceCacheIrradiance : refs.System.Black;
        inputs.RadianceCacheDepth = t.RadianceCacheDepth.IsValid() ? t.RadianceCacheDepth : refs.System.Black;
        // Before AddFrameMegaLightsVolume the volumes read as zero; the block stays invalid, as the grid's.
        inputs.MegaLightsVolume = t.MegaLightsVolume;
        for ( RDG::TextureRef& channel : inputs.MegaLightsVolume.Lighting )
            if ( !channel.IsValid() )
                channel = refs.System.Black3D;
        return inputs;
    }

    namespace SceneViewDetail
    {
        // Whether the layout a pass's SETUP declares against has a separate-texture slot of that name.
        [[nodiscard]] inline bool LayoutSamples( const RDG::ShaderBindingLayout& layout, std::string_view name )
        {
            return std::any_of( layout.Slots.begin(), layout.Slots.end(), [name]( const RDG::ShaderSlot& slot )
                                { return slot.Name == name && slot.Kind == RDG::ShaderResourceKind::Texture; } );
        }
        [[nodiscard]] inline bool LayoutReadsBuffer( const RDG::ShaderBindingLayout& layout,
                                                     std::string_view                name )
        {
            return std::any_of(
                 layout.Slots.begin(), layout.Slots.end(), [name]( const RDG::ShaderSlot& slot )
                 { return slot.Name == name && slot.Kind == RDG::ShaderResourceKind::StorageBuffer; } );
        }

        // THE one list of scene/view inputs with their sampler states: binds each one @p samples( name ) admits as
        // a separate texture, with the shared sampler slot it is read through (the receivers' shaders name these
        // slots: Mesh/Surface/Pass_Forward.glslh, Programs/Deferred/DeferredLighting.shader, ...). The cascades
        // and the cloud map linear, REPEAT (the state the images carried as their own); the cubes and the LUT
        // clamped (a LUT edge must not wrap into the opposite one); the local shadow atlas point-clamped (it is
        // read with texelFetch and filtered by hand inside each tile, Mesh/LocalLightShadow.glslh). A shared slot
        // named by several inputs is one entry (BindingBlockBuilder::Sampler).
        template <typename Block, typename Samples>
        void BindWhere( Block& block, const SceneViewInputs& inputs, const Samples& samples )
        {
            const auto bind = [&]( RDG::TextureParameter name, RDG::TextureRef texture,
                                   RDG::SamplerParameter samplerSlot, RDG::SamplerDesc sampler )
            {
                if ( !samples( name.Name ) )
                    return;
                block.Texture( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                     .Sampler( samplerSlot, sampler );
            };
            constexpr RDG::SamplerParameter kRepeat{ "LinearRepeatSampler" };
            constexpr RDG::SamplerParameter kClamp{ "LinearClampSampler" };
            constexpr RDG::SamplerParameter kPoint{ "PointClampSampler" };
            if ( inputs.ShadowCascades.IsValid() )
                bind( kSceneViewShadowCascadesTexture, inputs.ShadowCascades, kRepeat,
                      RDG::SamplerDesc::LinearRepeat() );
            bind( kSceneViewEnvIrradiance, inputs.EnvIrradiance, kClamp, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewEnvSpecular, inputs.EnvSpecular, kClamp, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewBrdfLut, inputs.BrdfLut, kClamp, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewCloudShadowMap, inputs.CloudShadowMap, kRepeat, RDG::SamplerDesc::LinearRepeat() );
            if ( inputs.ReflectionCaptureCubes.IsValid() )
                bind( kSceneViewReflectionCubes, inputs.ReflectionCaptureCubes, kClamp,
                      RDG::SamplerDesc::LinearClamp() );
            // Read with texelFetch (Mesh/LocalLightShadow.glslh filters by hand inside each tile).
            bind( kSceneViewLocalShadowAtlas, inputs.LocalShadowAtlas, kPoint, RDG::SamplerDesc::PointClamp() );
            // Marched texel by texel (Common/ContactShadow.glslh): nearest, clamped at the screen's edge.
            bind( kSceneViewSceneDepth, inputs.SceneDepth, kPoint, RDG::SamplerDesc::PointClamp() );
            // Read with texelFetch at the fragment (the mask is the render resolution the forward draws raster
            // at).
            if ( inputs.SunShadowMask.IsValid() )
                bind( kSceneViewSunShadowMask, inputs.SunShadowMask, kPoint, RDG::SamplerDesc::PointClamp() );
            // Read with texelFetch at the fragment, layer = the light's ShadowMaskSlot.
            if ( inputs.LocalShadowMasks.IsValid() )
                bind( kSceneViewLocalShadowMasks, inputs.LocalShadowMasks, kPoint, RDG::SamplerDesc::PointClamp() );
            // Read with texelFetch at the hit texel (an integer format is never filtered).
            bind( kSceneViewSceneShadingWord, inputs.SceneShadingWord, kPoint, RDG::SamplerDesc::PointClamp() );
            // Trilinear between froxel centres, clamped at the frustum's edges (Common/MegaLightsVolume.glslh).
            for ( std::size_t channel = 0; channel < kMegaLightsVolumeTextures.size(); ++channel )
                bind( kMegaLightsVolumeTextures[channel], inputs.MegaLightsVolume.Lighting[channel], kClamp,
                      RDG::SamplerDesc::LinearClamp() );
            // Gathered 2x2 and compared by hand (SurfaceWeather.glslh RainOcclusion): nearest, clamped.
            if ( inputs.RainOcclusionMap.IsValid() )
                bind( kSceneViewRainOcclusionMap, inputs.RainOcclusionMap, kSceneViewRainOcclusionSampler,
                      RDG::SamplerDesc::PointClamp() );
        }
    } // namespace SceneViewDetail

    // Whether @p layout samples any scene/view input: the lit programs do; a G-buffer, unlit or custom program
    // without the receivers does not. Read from the shader's REFLECTED resources, never from material properties.
    [[nodiscard]] inline bool SamplesSceneViewInputs( const RDG::ShaderBindingLayout& layout )
    {
        for ( std::string_view name :
              { kSceneViewShadowCascadesName, kSceneViewEnvIrradianceName, kSceneViewEnvSpecularName,
                kSceneViewBrdfLutName, kSceneViewCloudShadowMapName, kSceneViewLocalShadowAtlasName,
                kSceneViewRainOcclusionMapName, kSceneFogVolumeName } )
            if ( SceneViewDetail::LayoutSamples( layout, name ) )
                return true;
        return false;
    }

    // SETUP: the virtual shadow map's reader tables (VirtualShadowMapReadRefs) where @p layout declares them - a
    // DESERT_LOCAL_SHADOW_VSM program. Invalid refs are handed on: such a program then faults its pass by name
    // rather than reading an atlas nobody drew. Shared by BindSceneViewInputs and the passes that bind their own
    // view inputs (the surface cache's card lighting).
    template <typename Block>
    void BindVirtualShadowMapReads( Block& block, const VirtualShadowMapReadRefs& vsm,
                                    const RDG::ShaderBindingLayout& layout )
    {
        for ( const auto& [name, buffer] : { std::pair{ kVsmFrameDataBlock, vsm.FrameData },
                                             std::pair{ kVsmPageTableBlock, vsm.PageTable },
                                             std::pair{ kVsmLocalLightsBlock, vsm.LocalLights } } )
            if ( SceneViewDetail::LayoutReadsBuffer( layout, name.Name ) )
                block.Storage( name, buffer, RDG::Access::StorageRead );
        if ( std::any_of( layout.Slots.begin(), layout.Slots.end(),
                          []( const RDG::ShaderSlot& slot ) {
                              return slot.Name == kVsmPhysicalPoolName &&
                                     slot.Kind == RDG::ShaderResourceKind::StorageTexture;
                          } ) )
            block.Storage( kVsmPhysicalPool, vsm.PhysicalPool, RDG::Access::StorageRead );
    }

    // SETUP: declares on @p block (RenderPassDeclaration::BlockDeclaration or RDG::BindingBlockBuilder) every
    // input
    // @p layout has a sampled slot for, so a shader that samples no cascade (the glass) is never handed one - the
    // block would fault the pass in ValidatePassBindings ("'ShadowCascadesTexture' is not a resource of shader
    // ..."). A block entry IS the read: the node does not also declare inputs.Refs(). The fog a translucent
    // surface applies at its own depth, where the layout declares it (Mesh/Surface/SceneFog.glslh): the block and
    // its three images, each read trilinear and clamped through the pass's LinearClampSampler. Called by
    // BindSceneViewInputs, and on its own by the passes that bind the rest of their view inputs themselves (the
    // particle sprites). A layout that declares the slots in a frame with no fog renderer has nothing to bind them
    // to: that is a registration fault, and the pass faults by name in ValidatePassBindings exactly as any other
    // missing scene input does.
    template <typename Block>
    void BindSceneFogInputs( Block& block, const SceneViewInputs& inputs, const RDG::ShaderBindingLayout& layout )
    {
        if ( inputs.FogParams.IsValid() && SceneViewDetail::LayoutReadsBuffer( layout, kSceneFogParamsBlockName ) )
            block.Storage( kSceneFogParamsBlock, inputs.FogParams, RDG::Access::StorageRead );
        for ( const auto& [name, texture] :
              { std::pair{ kSceneFogAerialPerspective, inputs.FogAerialPerspective },
                std::pair{ kSceneFogDistantSkyLight, inputs.FogDistantSkyLight },
                std::pair{ kSceneFogVolume, inputs.FogVolume } } )
            if ( texture.IsValid() && SceneViewDetail::LayoutSamples( layout, name.Name ) )
                block.Texture( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                     .Sampler( RDG::SamplerParameter( "LinearClampSampler" ), RDG::SamplerDesc::LinearClamp() );
    }

    template <typename Block>
    void BindSceneViewInputs( Block& block, const SceneViewInputs& inputs, const RDG::ShaderBindingLayout& layout )
    {
        SceneViewDetail::BindWhere( block, inputs, [&layout]( std::string_view name )
                                    { return SceneViewDetail::LayoutSamples( layout, name ); } );
        // The capture list is a storage buffer, bound where the layout has that block.
        if ( inputs.ReflectionCaptures.IsValid() &&
             std::any_of( layout.Slots.begin(), layout.Slots.end(),
                          []( const RDG::ShaderSlot& slot ) {
                              return slot.Name == kSceneViewReflectionListName &&
                                     slot.Kind == RDG::ShaderResourceKind::StorageBuffer;
                          } ) )
            block.Storage( kSceneViewReflectionList, inputs.ReflectionCaptures, RDG::Access::StorageRead );
        // The light grid and the local shadow views its lights name, where the shader reads them. A frame graph
        // without them (no AddFrameLightGrid) hands invalid refs, and a shader that reads them then faults its
        // pass by name rather than reading nothing. The MegaLights volume's block likewise (invalid before
        // AddFrameMegaLightsVolume: a reader faults by name).
        for ( const auto& [name, buffer] :
              { std::pair{ kLightGridParamsBlock, inputs.LightGridParams },
                std::pair{ kLightGridCellsBlock, inputs.LightGridCells },
                std::pair{ kLocalShadowViewsBlock, inputs.LocalShadowViews },
                std::pair{ kMegaLightsVolumeBlock, inputs.MegaLightsVolume.Params } } )
            if ( SceneViewDetail::LayoutReadsBuffer( layout, name.Name ) )
                block.Storage( name, buffer, RDG::Access::StorageRead );
        BindVirtualShadowMapReads( block, inputs.Vsm, layout );
        // The radiance cache, where the shader reads it (the forward passes' estimator). Bound here and not in
        // BindWhere: the surface cache's lighting passes bind their own cache under the same names.
        for ( const auto& [name, buffer] :
              { std::pair{ kSceneViewRadianceCacheParams, inputs.RadianceCacheParams },
                std::pair{ kSceneViewRadianceCacheIndirection, inputs.RadianceCacheIndirection },
                std::pair{ kSceneViewRadianceCacheProbes, inputs.RadianceCacheProbes } } )
            if ( SceneViewDetail::LayoutReadsBuffer( layout, name.Name ) )
                block.Storage( name, buffer, RDG::Access::StorageRead );
        for ( const auto& [name, texture] :
              { std::pair{ kSceneViewRadianceCacheIrradiance, inputs.RadianceCacheIrradiance },
                std::pair{ kSceneViewRadianceCacheDepth, inputs.RadianceCacheDepth } } )
            if ( SceneViewDetail::LayoutSamples( layout, name.Name ) )
                block.Texture( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                     .Sampler( RDG::SamplerParameter( "LinearClampSampler" ), RDG::SamplerDesc::LinearClamp() );
        BindSceneFogInputs( block, inputs, layout );
    }
} // namespace Desert::Graphic
