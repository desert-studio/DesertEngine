#include <Common/Settings/Scalability.hpp>

#include <Common/Core/Logger.hpp>

#include <Common/Json/Document.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <limits>
#include <type_traits>

namespace Common::Scalability
{
    namespace
    {
        using P                         = Parameter;
        using G                         = Group;
        using CL                        = CatalogList;
        using Vis                       = ParameterVisibility;
        // A by-name value's Default, written as its enumerator (the JSON writes the same value by name).
        template <typename E>
        constexpr ParameterValue Of( E value )
        {
            return static_cast<ParameterValue>( value );
        }
        // The last enumerator of a by-name list (RenderMethods.hpp's name arrays are one entry per enumerator).
        constexpr ParameterValue LastOf( std::size_t count )
        {
            return static_cast<ParameterValue>( count ) - 1;
        }

        // THE spec list (Scalability.hpp: one row per Parameter, in enum order - the census asserts it).
        // The three shadow rows' High values are the renderer constant kSceneShadowQuality (ShadowCascades.hpp).
        // A level's viewport re-allocates its cascade maps when they change (SceneRenderer::BeginScene ->
        // MeshRenderer::RebudgetShadows, on a generation change only).
        constexpr std::array<ParameterSpec, kParameterCount> kParameterSpecs{ {
             { P::AntiAliasingMethod, G::AntiAliasing, "AntiAliasing.Method", 0,
               LastOf( kAntiAliasingMethodNames.size() ), CL::AntiAliasingMethods,
               "SceneRenderer framebuffer setup + post AA pass", Of( AntiAliasingMethod::TAA ), Vis::Basic },
             { P::AntiAliasingSamples, G::AntiAliasing, "AntiAliasing.Samples", 1, 8, CL::MSAACounts,
               "SceneRenderer framebuffer sample count (MSAA)", 1, Vis::Basic },
             { P::RenderScalePercent, G::ResolutionScale, "Resolution.Percent", 50, 200, CL::RenderScale,
               "RDG view-target size + the method's reconstruction pass (UpscalerForScale)", 100, Vis::Basic },
             { P::TextureFilter, G::Filtering, "Filtering.Texture", 0, 3, CL::None,
               "sampler cache (RenderConfig push)", 3, Vis::Basic },
             { P::Anisotropy, G::Filtering, "Filtering.Anisotropy", 1, 16, CL::AnisotropyLevels,
               "sampler cache (RenderConfig push)", 16, Vis::Basic },
             { P::MeshLOD, G::ViewDistance, "ViewDistance.MeshLOD", 0, 1, CL::None, "mesh LOD selection", 1,
               Vis::Advanced },
             { P::CloudQuality, G::Effects, "Effects.CloudQuality", 0, 2, CL::None, "Graphic::CloudQualityScale",
               2, Vis::Basic },
             { P::ShadowCascades, G::Shadows, "Shadows.Cascades", 1, 4, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::RebudgetShadows (scene view)", 4, Vis::Advanced },
             { P::ShadowMapSize, G::Shadows, "Shadows.MapSize", 512, 4096, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::RebudgetShadows (scene view)", 4096, Vis::Advanced },
             { P::ShadowDistance, G::Shadows, "Shadows.Distance", 1000, 100000, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::RebudgetShadows (scene view)", 20000, Vis::Advanced },
             { P::ShadowContactSteps, G::Shadows, "Shadows.ContactShadowSteps", 0, 32, CL::None,
               "SceneRenderer::BeginScene -> DeferredLighting.shader contact-shadow march "
               "(ContactShadowView.x)",
               16, Vis::Advanced },
             { P::ReflectionMaxSteps, G::Reflections, "Reflections.MaxSteps", 8, 128, CL::None,
               "SSRRenderer trace push constant: HZB traversal iterations per ray (SceneRenderer SSR pass)", 64,
               Vis::Advanced },
             { P::DistanceFieldPoolMiB, G::GlobalIllumination, "GlobalIllumination.DistanceFieldPoolMiB", 32, 512,
               CL::None,
               "DistanceFieldSceneData::SetPoolMiB - the brick atlas the finer mesh-field mips stream within", 384,
               Vis::Advanced },
             { P::AmbientOcclusionSamples, G::PostProcess, "PostProcess.AmbientOcclusionSamples", 4, 32, CL::None,
               "SSAORenderer GTAO horizon taps per direction (SSAO.shader kMaxTaps = 32)", 24, Vis::Advanced },
             { P::BloomMips, G::PostProcess, "PostProcess.BloomMips", 2, 6, CL::None,
               "BloomRenderer::SetMaxMips (SceneRenderer::BeginScene)", 6, Vis::Advanced },
             { P::MotionBlurQuality, G::PostProcess, "PostProcess.MotionBlurQuality", 0, 2, CL::None,
               "Graphic::MotionBlurSamplesForQuality (SceneRenderer::BeginScene -> MotionBlur gather taps)", 2,
               Vis::Advanced },
             { P::DepthOfFieldQuality, G::PostProcess, "PostProcess.DepthOfFieldQuality", 0, 2, CL::None,
               "Graphic::DofRingsForQuality (SceneRenderer::BeginScene -> DepthOfField gather rings)", 2,
               Vis::Advanced },
             { P::ReflectionCaptureSize, G::Reflections, "Reflections.CaptureSize", 32, 1024, CL::None,
               "Graphic::ReflectionCaptureFaceSize (ReflectionCaptureRenderer: the scene's capture cube face)",
               128, Vis::Advanced },
             { P::ReflectionRaysPerPixel, G::Reflections, "Reflections.RaysPerPixel", 1, 12, CL::None,
               "SSRRenderer trace push constant: GGX-VNDF rays per traced texel (SceneRenderer SSR pass)", 4,
               Vis::Advanced },
             { P::GlobalIlluminationMethod, G::GlobalIllumination, "GlobalIllumination.Method", 0,
               LastOf( kGIMethodNames.size() ), CL::None,
               "Graphic::ResolveRenderMethods (SceneRenderer::BeginScene) - the GI method ceiling",
               Of( GIMethod::HardwareTraced ), Vis::Basic },
             { P::ReflectionsMethod, G::Reflections, "Reflections.Method", 0,
               LastOf( kReflectionMethodNames.size() ), CL::None,
               "Graphic::ResolveRenderMethods (SceneRenderer::BeginScene) - the reflection method ceiling",
               Of( ReflectionMethod::HardwareTraced ), Vis::Basic },
             { P::ShadowsMethod, G::Shadows, "Shadows.Method", 0, LastOf( kShadowMethodNames.size() ), CL::None,
               "Graphic::ResolveRenderMethods (SceneRenderer::BeginScene) - the shadow method ceiling",
               Of( ShadowMethod::HardwareTraced ), Vis::Basic },
             { P::SkinCache, G::Effects, "Effects.SkinCache", 0, 1, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::SetSkinCacheEnabled -> "
               "RayTracingSkinCache::SetEnabled",
               1, Vis::Advanced },
             { P::DisplacementDicingRate, G::ViewDistance, "ViewDistance.DisplacementDicingRate", 0, 8, CL::None,
               "SceneRenderer::BeginScene -> DisplacementCache::SetQuality (0 = runtime displacement off)", 2,
               Vis::Advanced },
             { P::DisplacementPoolMiB, G::ViewDistance, "ViewDistance.DisplacementPoolMiB", 64, 1024, CL::None,
               "SceneRenderer::BeginScene -> DisplacementCache::SetQuality - the displaced geometry pool", 512,
               Vis::Advanced },
             { P::LightGridPixelSize, G::Shadows, "Shadows.LightGridPixelSize", 16, 256, CL::None,
               "SceneRenderer::BeginScene -> Graphic::MakeLightGridParams (\"Deferred: LightGrid\" tiles)", 32,
               Vis::Advanced },
             { P::LightGridSlices, G::Shadows, "Shadows.LightGridSlices", 8, 128, CL::None,
               "SceneRenderer::BeginScene -> Graphic::MakeLightGridParams (\"Deferred: LightGrid\" slices)", 32,
               Vis::Advanced },
             { P::LightGridMaxLightsPerCell, G::Shadows, "Shadows.LightGridMaxLightsPerCell", 8, 256, CL::None,
               "SceneRenderer::BeginScene -> Graphic::MakeLightGridParams (\"Deferred: LightGrid\" lists)", 64,
               Vis::Advanced },
             { P::VolumetricFog, G::Effects, "Effects.VolumetricFog", 0, 1, CL::None,
               "SceneRenderer::BeginScene -> VolumetricFogRenderer (\"VolumetricFog\" passes run)", 1,
               Vis::Basic },
             { P::VolumetricFogGridPixelSize, G::Effects, "Effects.VolumetricFogGridPixelSize", 4, 32, CL::None,
               "SceneRenderer::BeginScene -> Graphic::MakeVolumetricFogGridSize (froxel columns)", 8,
               Vis::Advanced },
             { P::VolumetricFogGridSizeZ, G::Effects, "Effects.VolumetricFogGridSizeZ", 32, 256, CL::None,
               "SceneRenderer::BeginScene -> Graphic::MakeVolumetricFogGridSize (froxel slices)", 128,
               Vis::Advanced },
             { P::ShadowRayTracedSamplesPerPixel, G::Shadows, "Shadows.RayTracedSamplesPerPixel", 1, 8, CL::None,
               "SceneRenderer::BeginScene -> System::MakeRayTracedSunShadowParams (\"Shadows: Ray Traced Sun\")",
               1, Vis::Advanced },
             { P::ShadowRayTracedTranslucencyCascades, G::Shadows, "Shadows.RayTracedTranslucencyCascades", 1, 4,
               CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::RebudgetShadows under a ray-traced sun (blended "
               "ShadowCascadesTexture readers, MeshRenderer::SunCascadeReadersQueued)",
               2, Vis::Advanced },
             { P::ShadowRayTracedTranslucencyMapSize, G::Shadows, "Shadows.RayTracedTranslucencyMapSize", 256,
               2048, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::RebudgetShadows under a ray-traced sun (blended "
               "ShadowCascadesTexture readers, MeshRenderer::SunCascadeReadersQueued)",
               1024, Vis::Advanced },
             { P::ShadowFilterTaps, G::Shadows, "Shadows.FilterTaps", 4, 64, CL::None,
               "SceneRenderer::BeginScene -> SceneResources::PackShadowFilter (ShadowUB ShadowFilter.y: "
               "Mesh/CascadedShadow.glslh PCSS filter taps)",
               32, Vis::Advanced },
             { P::ShadowBlockerSearchTaps, G::Shadows, "Shadows.BlockerSearchTaps", 4, 32, CL::None,
               "SceneRenderer::BeginScene -> SceneResources::PackShadowFilter (ShadowUB ShadowFilter.x: "
               "Mesh/CascadedShadow.glslh PCSS blocker search taps)",
               16, Vis::Advanced },
             { P::ShadowRayTracedDenoiserRadius, G::Shadows, "Shadows.RayTracedDenoiserRadius", 1, 3, CL::None,
               "SceneRenderer::BeginScene -> ScreenSpaceDenoiser::DenoiseShadowVisibility (reconstruction "
               "kernel)",
               2, Vis::Advanced },
             { P::ShadowVirtualMaxPhysicalPages, G::Shadows, "Shadows.VirtualMaxPhysicalPages", 256, 8192,
               CL::None,
               "SceneRenderer::BeginScene -> VirtualShadowMapRenderer::AddPagePasses (the physical pool)", 4096,
               Vis::Advanced },
             { P::ShadowVirtualResolutionLodBias, G::Shadows, "Shadows.VirtualResolutionLodBias", -30, 30,
               CL::None,
               "SceneRenderer::BeginScene -> VirtualShadowMapRenderer::AddPagePasses (VsmFrame Lod.x, tenths)",
               -15, Vis::Advanced },
             { P::ShadowVirtualSMRTRayCount, G::Shadows, "Shadows.VirtualSMRTRayCount", 0, 16, CL::None,
               "SceneRenderer::BeginScene -> VirtualShadowMapRenderer::AddPagePasses (VsmFrame Smrt.x: "
               "VirtualShadowMapProject.shader rays)",
               8, Vis::Advanced },
             { P::ShadowVirtualSMRTSamplesPerRay, G::Shadows, "Shadows.VirtualSMRTSamplesPerRay", 1, 16, CL::None,
               "SceneRenderer::BeginScene -> VirtualShadowMapRenderer::AddPagePasses (VsmFrame Smrt.y: "
               "VirtualShadowMapProject.shader samples per ray)",
               4, Vis::Advanced },
             { P::ShadowVirtualSMRTSamplesPerRayLocal, G::Shadows, "Shadows.VirtualSMRTSamplesPerRayLocal", 1, 16,
               CL::None,
               "SceneRenderer::BeginScene -> VirtualShadowMapRenderer::AddPagePasses (VsmFrame SmrtLocal.x: "
               "VsmLocalVisibilityRay samples, MegaLights vsm visibility)",
               8, Vis::Advanced },
             { P::ShadowWindAnimationDistance, G::Shadows, "Shadows.WindAnimationDistance", 500, 200000,
               CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::SetShadowWindDistance (ShadowIsmInstances: a swaying "
               "caster's movable / still split)",
               15000, Vis::Advanced },
             { P::ShadowMegaLightsSamplesPerPixel, G::Shadows, "Shadows.MegaLightsSamplesPerPixel", 1, 4, CL::None,
               "SceneRenderer::BeginScene -> MegaLightsRenderer::AddPasses (reservoirs per pixel)", 2,
               Vis::Advanced },
             { P::ShadowMegaLightsSpatialSamples, G::Shadows, "Shadows.MegaLightsSpatialSamples", 0, 8, CL::None,
               "SceneRenderer::BeginScene -> MegaLightsRenderer::AddPasses (\"MegaLights: Spatial Resampling\")",
               4, Vis::Advanced },
             { P::ShadowMegaLightsMaxHistory, G::Shadows, "Shadows.MegaLightsMaxHistory", 0, 30, CL::None,
               "SceneRenderer::BeginScene -> MegaLightsRenderer::AddPasses (\"MegaLights: Temporal "
               "Resampling\")",
               20, Vis::Advanced },
             { P::ShadowMegaLightsDenoiserRadius, G::Shadows, "Shadows.MegaLightsDenoiserRadius", 1, 3, CL::None,
               "SceneRenderer::BeginScene -> ScreenSpaceDenoiser::DenoiseDiffuseIndirect / DenoiseDirectSpecular "
               "(MegaLights kernel)",
               2, Vis::Advanced },
             { P::ShadowMegaLightsDownsampleFactor, G::Shadows, "Shadows.MegaLightsDownsampleFactor", 1, 2,
               CL::None, "SceneRenderer::BeginScene -> MegaLightsRenderer::AddPasses (sample pixels per side)", 2,
               Vis::Advanced },
             { P::ShadowMegaLightsVolumeTilePixels, G::Shadows, "Shadows.MegaLightsVolumeTilePixels", 8, 64,
               CL::None, "SceneRenderer::BeginScene -> MegaLightsVolumeRenderer::AddPasses (froxel edge, px)", 16,
               Vis::Advanced },
             { P::ShadowMegaLightsVolumeSlices, G::Shadows, "Shadows.MegaLightsVolumeSlices", 16, 128, CL::None,
               "SceneRenderer::BeginScene -> MegaLightsVolumeRenderer::AddPasses (depth slices)", 64,
               Vis::Advanced },
             { P::ShadowMegaLightsVolumeSamples, G::Shadows, "Shadows.MegaLightsVolumeSamples", 1, 4, CL::None,
               "SceneRenderer::BeginScene -> MegaLightsVolumeRenderer::AddPasses (traced samples per froxel)", 2,
               Vis::Advanced },
             { P::ShadowRainOcclusionMapSize, G::Shadows, "Shadows.RainOcclusionMapSize", 256, 4096, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::SetRainOcclusionQuality (\"Weather: RainOcclusion\" "
               "target side)",
               2048, Vis::Advanced },
             { P::ShadowRainOcclusionExtent, G::Shadows, "Shadows.RainOcclusionExtent", 1000, 100000, CL::None,
               "SceneRenderer::BeginScene -> MeshRenderer::SetRainOcclusionQuality (box side, world cm)", 20000,
               Vis::Advanced },
             { P::ShadowRainOcclusionRefreshPercent, G::Shadows, "Shadows.RainOcclusionRefreshPercent", 5, 50,
               CL::None, "SceneRenderer::BeginScene -> MeshRenderer::SetRainOcclusionQuality (recentre distance)",
               25, Vis::Advanced },
             { P::ScreenSpaceGISamples, G::GlobalIllumination, "GlobalIllumination.ScreenSpaceSamples", 4, 64,
               CL::None,
               "SceneRenderer::BeginScene -> MaterialDeferredLighting (ScreenSpaceGi.x: DeferredLighting.shader "
               "GatherIndirectGI samples)",
               16, Vis::Advanced },
             { P::ReflectionDenoiserRadius, G::Reflections, "Reflections.DenoiserRadius", 1, 3, CL::None,
               "SceneRenderer::BeginScene -> ScreenSpaceDenoiser::DenoiseReflections (reconstruction kernel)", 2,
               Vis::Advanced },
             { P::ReflectionDownsampleFactor, G::Reflections, "Reflections.DownsampleFactor", 1, 2, CL::None,
               "SceneRenderer::BeginScene -> SSRRenderer::Prepare (trace resolution) + "
               "ScreenSpaceDenoiser::DenoiseReflections (2x2 block rotation)",
               2, Vis::Advanced },
             { P::DlssPresetNative, G::AntiAliasing, "AntiAliasing.DLSSPreset.Native", 0,
               LastOf( kDlssPresetNames.size() ), CL::None,
               "DLSSUpscaler (DLSS options of the view's 100 % mode, NVIDIA's DLAA)", Of( DlssPreset::Default ),
               Vis::Advanced },
             { P::DlssPresetUltraQuality, G::AntiAliasing, "AntiAliasing.DLSSPreset.UltraQuality", 0,
               LastOf( kDlssPresetNames.size() ), CL::None,
               "DLSSUpscaler (DLSS options of the view's Ultra Quality mode)", Of( DlssPreset::Default ),
               Vis::Advanced },
             { P::DlssPresetQuality, G::AntiAliasing, "AntiAliasing.DLSSPreset.Quality", 0,
               LastOf( kDlssPresetNames.size() ), CL::None,
               "DLSSUpscaler (DLSS options of the view's Quality mode)", Of( DlssPreset::Default ),
               Vis::Advanced },
             { P::DlssPresetBalanced, G::AntiAliasing, "AntiAliasing.DLSSPreset.Balanced", 0,
               LastOf( kDlssPresetNames.size() ), CL::None,
               "DLSSUpscaler (DLSS options of the view's Balanced mode)", Of( DlssPreset::Default ),
               Vis::Advanced },
             { P::DlssPresetPerformance, G::AntiAliasing, "AntiAliasing.DLSSPreset.Performance", 0,
               LastOf( kDlssPresetNames.size() ), CL::None,
               "DLSSUpscaler (DLSS options of the view's Performance mode)", Of( DlssPreset::Default ),
               Vis::Advanced },
             { P::DlssPresetUltraPerformance, G::AntiAliasing, "AntiAliasing.DLSSPreset.UltraPerformance", 0,
               LastOf( kDlssPresetNames.size() ), CL::None,
               "DLSSUpscaler (DLSS options of the view's Ultra Performance mode)", Of( DlssPreset::Default ),
               Vis::Advanced },
             { P::DlssRRPresetNative, G::AntiAliasing, "AntiAliasing.DLSSRRPreset.Native", 0,
               LastOf( kDlssRRPresetNames.size() ), CL::None,
               "DLSSUpscaler (Ray Reconstruction options of the view's 100 % mode)", Of( DlssRRPreset::Default ),
               Vis::Advanced },
             { P::DlssRRPresetUltraQuality, G::AntiAliasing, "AntiAliasing.DLSSRRPreset.UltraQuality", 0,
               LastOf( kDlssRRPresetNames.size() ), CL::None,
               "DLSSUpscaler (Ray Reconstruction options of the view's Ultra Quality mode)",
               Of( DlssRRPreset::Default ), Vis::Advanced },
             { P::DlssRRPresetQuality, G::AntiAliasing, "AntiAliasing.DLSSRRPreset.Quality", 0,
               LastOf( kDlssRRPresetNames.size() ), CL::None,
               "DLSSUpscaler (Ray Reconstruction options of the view's Quality mode)", Of( DlssRRPreset::Default ),
               Vis::Advanced },
             { P::DlssRRPresetBalanced, G::AntiAliasing, "AntiAliasing.DLSSRRPreset.Balanced", 0,
               LastOf( kDlssRRPresetNames.size() ), CL::None,
               "DLSSUpscaler (Ray Reconstruction options of the view's Balanced mode)",
               Of( DlssRRPreset::Default ), Vis::Advanced },
             { P::DlssRRPresetPerformance, G::AntiAliasing, "AntiAliasing.DLSSRRPreset.Performance", 0,
               LastOf( kDlssRRPresetNames.size() ), CL::None,
               "DLSSUpscaler (Ray Reconstruction options of the view's Performance mode)",
               Of( DlssRRPreset::Default ), Vis::Advanced },
             { P::DlssRRPresetUltraPerformance, G::AntiAliasing, "AntiAliasing.DLSSRRPreset.UltraPerformance", 0,
               LastOf( kDlssRRPresetNames.size() ), CL::None,
               "DLSSUpscaler (Ray Reconstruction options of the view's Ultra Performance mode)",
               Of( DlssRRPreset::Default ), Vis::Advanced },
             { P::TsrHistoryScreenPercentage, G::AntiAliasing, "AntiAliasing.TSR.HistoryScreenPercentage", 100,
               200, CL::None, "TemporalSuperResolution::HistoryDescs (history extent)", 200, Vis::Advanced },
             { P::TsrHistoryUpdateQuality, G::AntiAliasing, "AntiAliasing.TSR.HistoryUpdateQuality", 0, 3,
               CL::None, "TemporalSuperResolution (TSR.UpdateHistory shader variant)", 3, Vis::Advanced },
             { P::TsrShadingRejectionFlickering, G::AntiAliasing, "AntiAliasing.TSR.ShadingRejectionFlickering", 0,
               1, CL::None, "TemporalSuperResolution (TSR.RejectShading flicker tolerance)", 1, Vis::Advanced },
             { P::TsrRejectionAntiAliasing, G::AntiAliasing, "AntiAliasing.TSR.RejectionAntiAliasingQuality", 0, 2,
               CL::None, "TemporalSuperResolution (TSR.SpatialAntiAliasing)", 2, Vis::Advanced },
             { P::TsrReprojectionField, G::AntiAliasing, "AntiAliasing.TSR.ReprojectionField", 0, 1, CL::None,
               "TemporalSuperResolution (TSR.DilateVelocity reprojection field, TSR.UpdateHistory)", 1,
               Vis::Advanced },
             { P::TsrResurrection, G::AntiAliasing, "AntiAliasing.TSR.Resurrection", 0, 1, CL::None,
               "TemporalSuperResolution::HistoryDescs (persistent frames) + TSR.RejectShading resurrection", 1,
               Vis::Advanced },
             { P::TextureMipBias, G::Textures, "Textures.MipBias", -200, 400, CL::None, std::nullopt, 0,
               Vis::Advanced },
             { P::TextureStreamingPoolMiB, G::Textures, "Textures.StreamingPoolMiB", 256, 16384, CL::None,
               "Runtime::TextureService::UpdateStreaming (mip streaming pool budget, r.Streaming.PoolSize)", 4096,
               Vis::Advanced },
             { P::TemporalAAQuality, G::AntiAliasing, "AntiAliasing.TemporalQuality", 0, 2, CL::None, std::nullopt,
               2, Vis::Basic },
             { P::UpscalerSharpness, G::ResolutionScale, "Resolution.Sharpness", 0, 100, CL::None,
               "post sharpen pass (Graphic::Sharpen)", 0, Vis::Basic },
             { P::SurfaceCacheAtlasSize, G::GlobalIllumination, "GlobalIllumination.SurfaceCacheAtlasSize", 1024,
               8192, CL::None, "surface cache scene + atlas (Graphic::SurfaceCache::SurfaceCacheScene)", 4096,
               Vis::Advanced },
             { P::SurfaceCacheCardTexelDensity, G::GlobalIllumination, "GlobalIllumination.CardTexelDensityScale",
               10, 400, CL::None, "surface cache scene + atlas (Graphic::SurfaceCache::SurfaceCacheScene)", 100,
               Vis::Advanced },
             { P::SurfaceCacheCardMinResolution, G::GlobalIllumination, "GlobalIllumination.CardMinResolution", 1,
               64, CL::None, "surface cache scene + atlas (Graphic::SurfaceCache::SurfaceCacheScene)", 2,
               Vis::Advanced },
             { P::SurfaceCacheCardMaxResolution, G::GlobalIllumination, "GlobalIllumination.CardMaxResolution", 64,
               2048, CL::None, "surface cache scene + atlas (Graphic::SurfaceCache::SurfaceCacheScene)", 512,
               Vis::Advanced },
             { P::SurfaceCacheDirectLightingUpdateFactor, G::GlobalIllumination,
               "GlobalIllumination.DirectLightingUpdateFactor", 1, 1024, CL::None,
               "surface cache direct lighting (Graphic::SurfaceCache::SurfaceCacheScene)", 32, Vis::Advanced },
             { P::SurfaceCacheRadiosityUpdateFactor, G::GlobalIllumination,
               "GlobalIllumination.RadiosityUpdateFactor", 1, 1024, CL::None, std::nullopt, 64, Vis::Advanced },
             { P::SurfaceCacheRadiosityProbeSpacing, G::GlobalIllumination,
               "GlobalIllumination.RadiosityProbeSpacing", 1, 16, CL::None, std::nullopt, 4, Vis::Advanced },
             { P::SurfaceCacheRadiosityProbeResolution, G::GlobalIllumination,
               "GlobalIllumination.RadiosityHemisphereProbeResolution", 2, 8, CL::None, std::nullopt, 4,
               Vis::Advanced },
             { P::RadianceCacheProbeResolution, G::GlobalIllumination,
               "GlobalIllumination.RadianceCacheProbeResolution", 8, 64, CL::None,
               "SceneRenderer::BeginScene -> RadianceCache::MakeRadianceCacheInputs (probe texels per side)", 32,
               Vis::Advanced },
             { P::RadianceCacheProbesPerFrame, G::GlobalIllumination,
               "GlobalIllumination.RadianceCacheProbesPerFrame", 10, 2000, CL::None,
               "SceneRenderer::BeginScene -> RadianceCache::MakeRadianceCacheInputs (screen probes' probes traced "
               "per frame)",
               150, Vis::Advanced },
             { P::HitLightingAllowed, G::GlobalIllumination, "GlobalIllumination.HitLighting", 0, 1, CL::None,
               "SceneRenderer::BeginScene -> GlobalIllumination::PipelineFor (hardware-traced hits lit in "
               "place)",
               1, Vis::Advanced },
             { P::PathTracingPathsPerFrame, G::PathTracing, "PathTracing.PathsPerFrame", 1, 64, CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (paths per pixel per frame)", 2,
               Vis::Advanced },
             { P::PathTracingMaxBounces, G::PathTracing, "PathTracing.MaxBounces", 0, 64, CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (bounces per path)", 32, Vis::Basic },
             { P::PathTracingSamplesPerPixel, G::PathTracing, "PathTracing.SamplesPerPixel", 1, 65536, CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (paths per pixel until it stops)", 4096,
               Vis::Basic },
             { P::PathTracingMaxPathIntensity, G::PathTracing, "PathTracing.MaxPathIntensity", 0, 65536, CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (the clamp of one path's exposed "
               "contribution)",
               24, Vis::Advanced },
             { P::PathTracingDenoiser, G::PathTracing, "PathTracing.Denoiser", 0, 4, CL::PathTracingDenoisers,
               "SceneRenderer::BeginScene -> System::PathTracer::KeepsDenoisingUpscaler / AddPasses (what "
               "denoises the path-traced view: none, the engine's ReLAX, DLSS Ray Reconstruction, or ReLAX "
               "until converged and then Intel Open Image Denoise once)",
               Of( PathTracingDenoiserMethod::Auto ), Vis::Basic },
             { P::PathTracingDenoiserAntiFirefly, G::PathTracing, "PathTracing.Denoiser.AntiFirefly", 0, 1,
               CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (the ReLAX denoiser's firefly clamp "
               "before "
               "the a-trous)",
               1, Vis::Advanced },
             { P::PathTracingRadianceCache, G::PathTracing, "PathTracing.RadianceCache", 0, 1, CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (paths cut early into the neural "
               "radiance cache: Off or Nrc)",
               Of( PathTracingRadianceCacheMethod::Off ), Vis::Advanced },
             { P::PathTracingLightSampling, G::PathTracing, "PathTracing.LightSampling", 0, 1, CL::None,
               "SceneRenderer::BeginScene -> System::PathTracer::AddPasses (the light choice at each vertex: "
               "every "
               "light by its estimate, or the light grid)",
               0, Vis::Advanced },
             { P::ScreenProbeDownsample, G::GlobalIllumination, "GlobalIllumination.ScreenProbeDownsample", 8, 64,
               CL::None,
               "SceneRenderer::BeginScene -> GlobalIllumination::ScreenProbeGather (pixels a uniform probe side: "
               "the SP_DOWNSAMPLE permutation, 8 / 16 / 32 / 64)",
               16, Vis::Advanced },
             { P::ScreenProbeTracedTexels, G::GlobalIllumination, "GlobalIllumination.ScreenProbeTracedTexels", 4,
               16, CL::None,
               "SceneRenderer::BeginScene -> GlobalIllumination::ScreenProbeGather (traced texels a probe side: "
               "the SP_RES permutation)",
               8, Vis::Advanced },
             { P::IrradianceFieldProbesPerFrame, G::GlobalIllumination,
               "GlobalIllumination.IrradianceFieldProbesPerFrame", 10, 2000, CL::None,
               "SceneRenderer::BeginScene -> RadianceCache::MakeRadianceCacheInputs (ProbeVolume's probes traced "
               "per frame)",
               100, Vis::Advanced },
             { P::ReflectionsHitLighting, G::Reflections, "Reflections.HitLighting", 0, 1, CL::None,
               "SceneRenderer::ReflectionsTracer -> SSR.shader \"HitLighting\" ray generation "
               "(Renderer::TraceRays)",
               0, Vis::Advanced },
             { P::ScreenProbeAdaptiveAllocation, G::GlobalIllumination,
               "GlobalIllumination.AdaptiveProbeAllocationPercent", 0, 100, CL::None,
               "screen probe adaptive budget (Graphic::GlobalIllumination::ScreenProbeGather)", 100,
               Vis::Advanced },
             { P::ScreenProbeShortRangeAO, G::GlobalIllumination, "GlobalIllumination.ShortRangeAO", 0, 1,
               CL::None, "screen probe short-range AO (Graphic::GlobalIllumination::ScreenProbeGather)", 1,
               Vis::Advanced },
             { P::ReflectionsTranslucentFrontLayer, G::Reflections, "Reflections.TranslucentFrontLayer", 0, 1,
               CL::None,
               "SceneRenderer::TranslucentFrontLayerReflections -> \"Translucency: Front Layer G-buffer\" + "
               "\"Translucency: Front Layer Reflections\" -> Pass_Forward_Translucent.glslh",
               1, Vis::Advanced },
             { P::ShadingParallaxMinSteps, G::Shading, "Shading.ParallaxMinSteps", 1, 64, CL::None,
               "SceneRenderer::BeginScene -> ViewFrame::Parallax -> cameraUB.ParallaxMinSteps "
               "(Mesh/Surface/SurfaceParallax.glslh)",
               16, Vis::Advanced },
             { P::ShadingParallaxMaxSteps, G::Shading, "Shading.ParallaxMaxSteps", 1, 128, CL::None,
               "SceneRenderer::BeginScene -> ViewFrame::Parallax -> cameraUB.ParallaxMaxSteps "
               "(Mesh/Surface/SurfaceParallax.glslh)",
               48, Vis::Advanced },
             { P::ShadingParallaxShadowSteps, G::Shading, "Shading.ParallaxShadowSteps", 0, 64, CL::None,
               "SceneRenderer::BeginScene -> ViewFrame::Parallax -> cameraUB.ParallaxShadowSteps "
               "(Mesh/Surface/SurfaceParallax.glslh)",
               16, Vis::Advanced },
             { P::EffectsSpawnCountScalePercent, G::Effects, "Effects.SpawnCountScalePercent", 1, 100, CL::None,
               "SceneRenderer::BeginScene -> VFXWorld::SubmitView -> SpawnState::Step (rate and burst counts)",
               100, Vis::Advanced },
             { P::EffectsQualityLevel, G::Effects, "Effects.QualityLevel", 0, 4, CL::None,
               "SceneRenderer::BeginScene -> VFXWorld::SubmitView -> VFXWorld::Tick (effect type level)", 3,
               Vis::Advanced },
             { P::EffectsMaxWorldParticles, G::Effects, "Effects.MaxWorldParticles", 0, 4194304, CL::None,
               "SceneRenderer::BeginScene -> VFXWorld::SubmitView -> EvaluateVFXScalability (world pool budget)",
               131072, Vis::Advanced },
             { P::GlobalIlluminationTlasUpdatesBeforeRebuild, G::GlobalIllumination,
               "GlobalIllumination.TlasUpdatesBeforeRebuild", 0, 240, CL::None,
               "SceneRenderer::BeginScene -> RayTracingSceneRenderer::SetTlasUpdatesBeforeRebuild (the TLAS is "
               "built after this many in-place updates in a row; 0 = every frame)",
               60, Vis::Advanced },
             { P::ViewDistanceVirtualGeometryMaxPixelsPerEdge, G::ViewDistance,
               "ViewDistance.VirtualGeometryMaxPixelsPerEdge", 1, 1000, CL::None,
               "SceneRenderer::AddFrameVirtualGeometry -> VirtualGeometryRenderer::AddClusterCull (cluster LOD "
               "cut, "
               "tenths of a pixel)",
               10, Vis::Advanced },
             { P::AsyncCompute, G::Effects, "RenderPipeline.AsyncCompute", 0, 1, CL::AsyncCompute,
               "SceneRenderer::OnUpdate -> RDG::Builder::SetAsyncCompute (the view graph's AsyncCompute passes on "
               "the "
               "compute queue family)",
               1, Vis::Advanced },
             { P::FoliageGrassDensityScalePercent, G::Foliage, "Foliage.GrassDensityScalePercent", 10, 100, CL::None,
               "SceneRenderer::BeginScene -> GrassRenderer::SetQuality -> GrassRenderer::AddVariety (the variety's "
               "grid cell, GrassGenerate)",
               100, Vis::Basic },
             { P::FoliageGrassCullDistanceScalePercent, G::Foliage, "Foliage.GrassCullDistanceScalePercent", 10, 400,
               CL::None,
               "SceneRenderer::BeginScene -> GrassRenderer::SetQuality -> GrassRenderer::AddVariety (Start / End "
               "cull distances, GrassGenerate)",
               100, Vis::Basic },
        } };

        // Every Default is a value the loader would accept for its row (SCAL-UX contract §1).
        constexpr bool DefaultsInRange()
        {
            for ( const ParameterSpec& spec : kParameterSpecs )
                if ( spec.Default < spec.Min || spec.Default > spec.Max )
                    return false;
            return true;
        }
        static_assert( DefaultsInRange(), "a ParameterSpec Default lies outside its [Min, Max]" );
    } // namespace

    namespace
    {
        // Enum-valued parameters are written by NAME in Scalability.json; this is the one place a parameter is
        // tied to its name list (CapabilityCatalog.hpp owns the lists).
        std::span<const std::string_view> ValueNames( Parameter parameter )
        {
            // The method ceilings are named by their enum (RenderMethods.hpp), not by a catalog list.
            if ( parameter == Parameter::GlobalIlluminationMethod )
                return kGIMethodNames;
            if ( parameter == Parameter::ReflectionsMethod )
                return kReflectionMethodNames;
            if ( parameter == Parameter::ShadowsMethod )
                return kShadowMethodNames;
            if ( parameter == Parameter::PathTracingRadianceCache )
                return kPathTracingRadianceCacheNames;
            if ( parameter >= Parameter::DlssPresetNative && parameter <= Parameter::DlssPresetUltraPerformance )
                return kDlssPresetNames;
            if ( parameter >= Parameter::DlssRRPresetNative &&
                 parameter <= Parameter::DlssRRPresetUltraPerformance )
                return kDlssRRPresetNames;
            switch ( SpecOf( parameter ).NarrowedBy )
            {
                case CatalogList::AntiAliasingMethods:
                    return kAntiAliasingMethodNames;
                case CatalogList::PathTracingDenoisers:
                    return kPathTracingDenoiserNames;
                case CatalogList::AsyncCompute:
                case CatalogList::None:
                case CatalogList::MSAACounts:
                case CatalogList::RenderScale:
                case CatalogList::AnisotropyLevels:
                    break;
            }
            return {};
        }

        const ParameterSpec* FindSpec( std::string_view key )
        {
            for ( const ParameterSpec& spec : ParameterSpecs() )
                if ( spec.Key == key )
                    return &spec;
            return nullptr;
        }

        bool GroupHasParameters( Group group )
        {
            return std::any_of( ParameterSpecs().begin(), ParameterSpecs().end(),
                                [group]( const ParameterSpec& spec ) { return spec.Owner == group; } );
        }

        std::optional<Group> FindGroup( std::string_view key )
        {
            for ( std::size_t g = 0; g < kGroupCount; ++g )
                if ( GroupKey( static_cast<Group>( g ) ) == key )
                    return static_cast<Group>( g );
            return std::nullopt;
        }

        std::optional<Level> FindLevel( std::string_view key )
        {
            for ( std::size_t l = 0; l < kLevelCount; ++l )
                if ( LevelKey( static_cast<Level>( l ) ) == key )
                    return static_cast<Level>( l );
            return std::nullopt;
        }

        // Every loader error names the JSON path of the value and the key, and all of them are reported together.
        class Errors
        {
        public:
            template <typename... Args>
            void Add( const Json::Node& at, std::format_string<Args...> format, Args&&... args )
            {
                std::format_to( std::back_inserter( m_Text ), "{}{}: ", m_Text.empty() ? "" : "\n",
                                at.Where().ToString() );
                std::format_to( std::back_inserter( m_Text ), format, std::forward<Args>( args )... );
            }
            [[nodiscard]] bool Empty() const
            {
                return m_Text.empty();
            }
            [[nodiscard]] const std::string& Text() const
            {
                return m_Text;
            }

        private:
            std::string m_Text;
        };

        // A number of the table into T: an integral T takes only an integer inside its range, a float any number.
        template <typename T>
        [[nodiscard]] bool ReadNumber( const Json::Node& node, T& out )
        {
            if constexpr ( std::is_integral_v<T> )
            {
                const auto v = node.AsInteger();
                if ( !v || v.GetValue() < static_cast<std::int64_t>( std::numeric_limits<T>::min() ) ||
                     v.GetValue() > static_cast<std::int64_t>( std::numeric_limits<T>::max() ) )
                    return false;
                out = static_cast<T>( v.GetValue() );
            }
            else
            {
                const auto v = node.AsNumber();
                if ( !v )
                    return false;
                out = static_cast<T>( v.GetValue() );
            }
            return true;
        }

        // The render-method ceilings' refusal (RenderMethods.hpp, RefuseGIMethod and its siblings): empty for
        // every other parameter and for an implemented method.
        [[nodiscard]] std::string_view RefuseMethodCeiling( Parameter parameter, ParameterValue value )
        {
            switch ( parameter )
            {
                case Parameter::GlobalIlluminationMethod:
                    return RefuseGIMethod( value );
                case Parameter::ReflectionsMethod:
                    return RefuseReflectionMethod( value );
                case Parameter::ShadowsMethod:
                    return RefuseShadowMethod( value );
                default:
                    return {};
            }
        }

        std::optional<ParameterValue> ParseValue( const ParameterSpec& spec, const Json::Node& node,
                                                  Errors& errors )
        {
            const std::span<const std::string_view> names = ValueNames( spec.Id );
            ParameterValue                          value = 0;
            if ( !names.empty() )
            {
                const auto text = node.AsString();
                if ( !text )
                {
                    errors.Add( node, "{}: expected the name of a value, found {}", spec.Key,
                                Json::Detail::DescribeFound( node.Raw() ) );
                    return std::nullopt;
                }
                const auto it = std::find( names.begin(), names.end(), text.GetValue() );
                if ( it == names.end() )
                {
                    errors.Add( node, "{}: '{}' is not a value of this parameter", spec.Key, text.GetValue() );
                    return std::nullopt;
                }
                value = static_cast<ParameterValue>( it - names.begin() );
            }
            else if ( !ReadNumber( node, value ) )
            {
                errors.Add( node, "{}: expected an integer, found {}", spec.Key,
                            Json::Detail::DescribeFound( node.Raw() ) );
                return std::nullopt;
            }
            if ( value < spec.Min || value > spec.Max )
            {
                errors.Add( node, "{}: {} is outside [{}, {}]", spec.Key, value, spec.Min, spec.Max );
                return std::nullopt;
            }
            // A level states the render method it renders (RenderMethods.hpp): one this build has not implemented
            // would be lowered on every machine, so the level would say one thing and every frame show another.
            if ( const std::string_view refused = RefuseMethodCeiling( spec.Id, value ); !refused.empty() )
            {
                errors.Add( node, "{}: '{}' is {}; a level names a method this build renders", spec.Key,
                            names[static_cast<std::size_t>( value )], refused );
                return std::nullopt;
            }
            return value;
        }

        // True for an object; otherwise an error naming what the file holds instead.
        [[nodiscard]] bool ExpectObject( const Json::Node& node, std::string_view what, Errors& errors )
        {
            if ( node.GetKind() == Json::Kind::Object )
                return true;
            errors.Add( node, "{}: expected an object, found {}", what,
                        Json::Detail::DescribeFound( node.Raw() ) );
            return false;
        }

        // AntiAliasing.Samples is the MSAA sample count and nothing else: the framebuffer reads it only under
        // MSAA, and the temporal methods' quality is AntiAliasing.TemporalQuality. A level that pairs FXAA or TAA
        // with Samples 4 reads as "TAA at 4 samples" to whoever edits the table, so the loader refuses it (TAA1
        // brief).
        void CheckSamplesOnlyUnderMsaa( const Json::Node& levelNode, std::string_view groupKey,
                                        std::string_view                                   levelKey,
                                        const std::array<ParameterValue, kParameterCount>& values,
                                        const std::array<bool, kParameterCount>& parsed, Errors& errors )
        {
            constexpr auto kMethod  = static_cast<std::size_t>( Parameter::AntiAliasingMethod );
            constexpr auto kSamples = static_cast<std::size_t>( Parameter::AntiAliasingSamples );
            if ( !parsed[kMethod] || !parsed[kSamples] )
                return;
            const ParameterValue method = values[kMethod];
            if ( method == static_cast<ParameterValue>( AntiAliasingMethod::MSAA ) || values[kSamples] == 1 )
                return;
            errors.Add( levelNode, "{}.{}: '{}' is the MSAA sample count; under method '{}' it must be 1, not {}",
                        groupKey, levelKey, SpecOf( Parameter::AntiAliasingSamples ).Key,
                        kAntiAliasingMethodNames[static_cast<std::size_t>( method )], values[kSamples] );
        }

        void ParseGroups( const Json::Node& groups, ScalabilityTable& table, Errors& errors )
        {
            if ( !ExpectObject( groups, "Groups", errors ) )
                return;
            std::array<bool, kGroupCount> seen{};
            groups.ForEachMember(
                 [&]( std::string_view groupKey, const Json::Node& groupNode )
                 {
                     const std::optional<Group> group = FindGroup( groupKey );
                     if ( !group )
                     {
                         errors.Add( groupNode, "unknown group '{}'", groupKey );
                         return;
                     }
                     const auto g = static_cast<std::size_t>( *group );
                     seen[g]      = true;
                     if ( !GroupHasParameters( *group ) )
                     {
                         errors.Add( groupNode, "group '{}' has no parameters; levels for it would move nothing",
                                     groupKey );
                         return;
                     }
                     if ( !ExpectObject( groupNode, groupKey, errors ) )
                         return;
                     std::array<bool, kLevelCount> levelSeen{};
                     groupNode.ForEachMember(
                          [&]( std::string_view levelKey, const Json::Node& levelNode )
                          {
                              const std::optional<Level> level = FindLevel( levelKey );
                              if ( !level )
                              {
                                  errors.Add( levelNode, "{}: unknown level '{}'", groupKey, levelKey );
                                  return;
                              }
                              const auto l = static_cast<std::size_t>( *level );
                              levelSeen[l] = true;
                              if ( !ExpectObject( levelNode, levelKey, errors ) )
                                  return;
                              std::array<bool, kParameterCount> set{};
                              std::array<bool, kParameterCount> parsed{};
                              levelNode.ForEachMember(
                                   [&]( std::string_view key, const Json::Node& valueNode )
                                   {
                                       const ParameterSpec* spec = FindSpec( key );
                                       if ( spec == nullptr )
                                       {
                                           errors.Add( valueNode, "{}.{}: unknown parameter '{}'", groupKey,
                                                       levelKey, key );
                                           return;
                                       }
                                       if ( spec->Owner != *group )
                                       {
                                           errors.Add( valueNode, "{}.{}: '{}' belongs to group '{}'", groupKey,
                                                       levelKey, key, GroupKey( spec->Owner ) );
                                           return;
                                       }
                                       const auto p = static_cast<std::size_t>( spec->Id );
                                       if ( set[p] )
                                       {
                                           errors.Add( valueNode, "{}.{}: '{}' is set twice", groupKey, levelKey,
                                                       key );
                                           return;
                                       }
                                       set[p] = true;
                                       if ( const auto value = ParseValue( *spec, valueNode, errors ) )
                                       {
                                           // The level lists what differs from the spec; a line restating the
                                           // Default is a second copy of it (SCAL-UX contract §3).
                                           if ( *value == spec->Default )
                                           {
                                               errors.Add( valueNode, "{}.{}: '{}' restates its default {}; drop "
                                                           "the line",
                                                           groupKey, levelKey, key, *value );
                                               return;
                                           }
                                           table.Values[g][l][p] = *value;
                                           parsed[p]             = true;
                                       }
                                   } );
                              // A parameter the level does not name keeps its Default (filled by Parse), and is
                              // checked as part of the level like any value.
                              for ( const ParameterSpec& spec : ParameterSpecs() )
                                  if ( spec.Owner == *group && !set[static_cast<std::size_t>( spec.Id )] )
                                      parsed[static_cast<std::size_t>( spec.Id )] = true;
                              CheckSamplesOnlyUnderMsaa( levelNode, groupKey, levelKey, table.Values[g][l], parsed,
                                                         errors );
                          } );
                     for ( std::size_t l = 0; l < kLevelCount; ++l )
                         if ( !levelSeen[l] )
                             errors.Add( groupNode, "{}: level '{}' is missing", groupKey,
                                         LevelKey( static_cast<Level>( l ) ) );
                 } );
            for ( std::size_t g = 0; g < kGroupCount; ++g )
                if ( !seen[g] && GroupHasParameters( static_cast<Group>( g ) ) )
                    errors.Add( groups, "group '{}' has parameters but no levels",
                                GroupKey( static_cast<Group>( g ) ) );
        }

        // A per-group list under Recommend (Thresholds / MinVideoMemoryMiB): known groups that have parameters,
        // exactly N numbers each.
        template <typename T, std::size_t N>
        void ParsePerGroupList( const Json::Node& node, std::string_view section,
                                std::array<std::array<T, N>, kGroupCount>& out,
                                std::array<bool, kGroupCount>& seen, Errors& errors )
        {
            if ( !ExpectObject( node, section, errors ) )
                return;
            node.ForEachMember(
                 [&]( std::string_view key, const Json::Node& list )
                 {
                     const std::optional<Group> group = FindGroup( key );
                     if ( !group || !GroupHasParameters( *group ) )
                     {
                         errors.Add( list, "Recommend.{}: '{}' is not a group with parameters", section, key );
                         return;
                     }
                     const auto g      = static_cast<std::size_t>( *group );
                     seen[g]           = true;
                     std::size_t count = 0;
                     list.ForEachElement(
                          [&]( std::size_t i, const Json::Node& element )
                          {
                              ++count;
                              if ( i < N && !ReadNumber( element, out[g][i] ) )
                                  errors.Add( element, "Recommend.{}.{}[{}]: not a number in range", section, key,
                                              i );
                          } );
                     if ( list.GetKind() != Json::Kind::Array || count != N )
                         errors.Add( list, "Recommend.{}.{}: expected an array of {} numbers", section, key, N );
                 } );
        }

        void ParseRecommend( const Json::Node& recommend, ScalabilityTable& table, Errors& errors )
        {
            if ( !ExpectObject( recommend, "Recommend", errors ) )
                return;
            std::array<bool, kGroupCount> thresholdsSeen{};
            std::array<bool, kGroupCount> memorySeen{};
            bool                          deviceClassSeen = false;
            recommend.ForEachMember(
                 [&]( std::string_view key, const Json::Node& entry )
                 {
                     if ( key == "Thresholds" )
                         ParsePerGroupList( entry, key, table.RecommendThresholds, thresholdsSeen, errors );
                     else if ( key == "MinVideoMemoryMiB" )
                         ParsePerGroupList( entry, key, table.MinVideoMemoryMiB, memorySeen, errors );
                     else if ( key == "DeviceClass" )
                     {
                         deviceClassSeen = true;
                         if ( !ExpectObject( entry, "Recommend.DeviceClass", errors ) )
                             return;
                         constexpr std::array<std::string_view, 4> kClasses{ "Unknown", "Integrated",
                                                                             "AppleUnified", "Discrete" };
                         for ( std::size_t c = 0; c < kClasses.size(); ++c )
                         {
                             const std::optional<Json::Node> value = entry.Find( kClasses[c] );
                             if ( !value || !ReadNumber( *value, table.DeviceClassPerfIndex[c] ) )
                                 errors.Add( entry, "Recommend.DeviceClass: '{}' is missing or not a number",
                                             kClasses[c] );
                         }
                         entry.ForEachMember(
                              [&]( std::string_view name, const Json::Node& member )
                              {
                                  if ( std::find( kClasses.begin(), kClasses.end(), name ) == kClasses.end() )
                                      errors.Add( member, "Recommend.DeviceClass: unknown device class '{}'",
                                                  name );
                              } );
                     }
                     else
                         errors.Add( entry, "Recommend: unknown key '{}'", key );
                 } );
            if ( !deviceClassSeen )
                errors.Add( recommend, "Recommend.DeviceClass is missing" );
            for ( std::size_t g = 0; g < kGroupCount; ++g )
            {
                if ( !GroupHasParameters( static_cast<Group>( g ) ) )
                    continue;
                if ( !thresholdsSeen[g] )
                    errors.Add( recommend, "Recommend.Thresholds.{} is missing",
                                GroupKey( static_cast<Group>( g ) ) );
                const auto& t = table.RecommendThresholds[g];
                for ( std::size_t i = 1; i < t.size(); ++i )
                    if ( !( t[i - 1] < t[i] ) )
                        errors.Add( recommend, "Recommend.Thresholds.{} is not strictly ascending",
                                    GroupKey( static_cast<Group>( g ) ) );
            }
        }

        // ---- Resolution helpers -----------------------------------------------------------------------------

        // The working state of one Resolve: the requested value per parameter and the first reason each one
        // moved, so the result carries ONE fallback per parameter (requested -> final effective).
        struct Resolution
        {
            ParameterValues                               Requested{};
            ParameterValues                               Values{};
            std::array<std::string_view, kParameterCount> Reasons{};
            std::array<bool, kParameterCount>             Overridden{};

            [[nodiscard]] ParameterValue Get( Parameter p ) const
            {
                return Values[static_cast<std::size_t>( p )];
            }
            void Set( Parameter p, ParameterValue value, std::string_view reason )
            {
                const auto i = static_cast<std::size_t>( p );
                if ( Values[i] == value )
                    return;
                Values[i] = value;
                if ( Reasons[i].empty() )
                    Reasons[i] = reason;
            }
        };

        constexpr std::string_view kNotOffered = "not offered by this device";
        // CapabilityCatalog::RefusedAtRuntime: the device offered it, its vendor runtime then refused it (the
        // runtime's own reason is in its log line, e.g. "[Streamline] ... is not offered: ...").
        constexpr std::string_view kRefusedAtRuntime = "refused by its runtime on this device";

        // A render-method ceiling (RenderMethods.hpp) down its ladder - the enum's order - past what this device
        // cannot run (no ray query) and what this build does not implement. The table cannot name an unimplemented
        // method (ParseValue refuses it), an override can; either lowering is a Fallback, logged by Publish like
        // every other, so the resolved ceiling is what the machine renders and the log says why it is lower.
        template <typename Method>
        void NarrowMethod( Parameter parameter, bool rayQuery, bool fragmentStores, Resolution& r )
        {
            auto             method = static_cast<Method>( r.Get( parameter ) );
            std::string_view reason;
            for ( ;; )
            {
                std::string_view blocked;
                if ( NeedsRayQuery( method ) && !rayQuery )
                    blocked = "the device offers no ray query";
                else if ( NeedsFragmentStores( method ) && !fragmentStores )
                    blocked = "the device offers no fragmentStoresAndAtomics";
                else if ( !IsImplemented( method ) )
                    blocked = "not implemented in this build";
                if ( blocked.empty() )
                    break;
                if ( reason.empty() )
                    reason = blocked;
                method = static_cast<Method>( static_cast<int>( method ) - 1 );
            }
            r.Set( parameter, static_cast<ParameterValue>( method ), reason );
        }

        [[nodiscard]] bool OffersRayQuery( const CapabilityCatalog& catalog )
        {
            return std::any_of( catalog.RayTracingModes.begin(), catalog.RayTracingModes.end(),
                                []( RayTracingMode m ) { return m != RayTracingMode::None; } );
        }

        // Largest offered <= value; the list's front when none is (MSAACounts / AnisotropyLevels are ascending
        // and start at 1).
        template <typename T>
        ParameterValue LargestOfferedAtMost( const std::vector<T>& list, ParameterValue value )
        {
            std::optional<ParameterValue> best;
            for ( const T& item : list )
            {
                const auto v = static_cast<ParameterValue>( item );
                if ( v <= value && ( !best || v > *best ) )
                    best = v;
            }
            return best ? *best : static_cast<ParameterValue>( list.front() );
        }

        void NarrowByCatalog( const ParameterSpec& spec, const CapabilityCatalog& catalog, Resolution& r )
        {
            const ParameterValue value = r.Get( spec.Id );
            switch ( spec.NarrowedBy )
            {
                case CatalogList::None:
                    return;
                case CatalogList::AntiAliasingMethods:
                {
                    const auto method = static_cast<AntiAliasingMethod>( value );
                    if ( CapabilityCatalog::Offers( catalog.AntiAliasingMethods, method ) )
                        return;
                    // MSAA (no count > 1) falls back to FXAA. A vendor temporal method falls back to the engine's
                    // best temporal method, TSR (always offered), except Ray Reconstruction, which is DLSS with
                    // the denoiser: a device without the RR runtime keeps DLSS Super Resolution when it has that,
                    // as UE's plugin falls back from RR to DLSS-SR.
                    AntiAliasingMethod to = AntiAliasingMethod::TSR;
                    if ( method == AntiAliasingMethod::MSAA )
                        to = AntiAliasingMethod::FXAA;
                    else if ( method == AntiAliasingMethod::DLSSRayReconstruction &&
                              CapabilityCatalog::Offers( catalog.AntiAliasingMethods, AntiAliasingMethod::DLSS ) )
                        to = AntiAliasingMethod::DLSS;
                    else if ( !CapabilityCatalog::Offers( catalog.AntiAliasingMethods, to ) )
                        to = AntiAliasingMethod::TAA;
                    r.Set( spec.Id, static_cast<ParameterValue>( to ),
                           CapabilityCatalog::Offers( catalog.RefusedAtRuntime, method ) ? kRefusedAtRuntime
                                                                                         : kNotOffered );
                    return;
                }
                case CatalogList::MSAACounts:
                    r.Set( spec.Id, LargestOfferedAtMost( catalog.MSAACounts, value ), kNotOffered );
                    return;
                case CatalogList::AnisotropyLevels:
                    r.Set( spec.Id, LargestOfferedAtMost( catalog.AnisotropyLevels, value ), kNotOffered );
                    return;
                case CatalogList::RenderScale:
                    r.Set( spec.Id,
                           std::clamp( value, catalog.RenderScale.MinPercent, catalog.RenderScale.MaxPercent ),
                           kNotOffered );
                    return;
                case CatalogList::AsyncCompute:
                    // A request for the compute queue on a device that has no separate compute family (or is a
                    // portability device): every pass stays on the graphics queue.
                    if ( value != 0 && !catalog.AsyncCompute )
                        r.Set( spec.Id, 0, kNotOffered );
                    return;
                case CatalogList::PathTracingDenoisers:
                {
                    // Ray Reconstruction is the one denoiser a device may lack; the engine's ReLAX runs
                    // everywhere.
                    const bool offersRR = CapabilityCatalog::Offers( catalog.AntiAliasingMethods,
                                                                     AntiAliasingMethod::DLSSRayReconstruction );
                    const bool refusedRR = CapabilityCatalog::Offers( catalog.RefusedAtRuntime,
                                                                      AntiAliasingMethod::DLSSRayReconstruction );
                    const auto method    = static_cast<PathTracingDenoiserMethod>( value );
                    const auto rr        = static_cast<ParameterValue>( PathTracingDenoiserMethod::RR );
                    const auto relax     = static_cast<ParameterValue>( PathTracingDenoiserMethod::ReLAX );
                    // Auto is a request to settle, not a value that fell back: Resolve reports no Fallback for it
                    // (SettlesUnreported). Except where the device offered RR and its runtime then refused it:
                    // Auto settled to RR on that offer, so the request becomes RR and its lowering to ReLAX is a
                    // Fallback like an explicit RR's, logged and shown, exactly as the runtime's refusal of an
                    // offered method is everywhere else.
                    if ( method == PathTracingDenoiserMethod::Auto )
                    {
                        if ( offersRR || !refusedRR )
                        {
                            r.Set( spec.Id, offersRR ? rr : relax, {} );
                            return;
                        }
                        r.Requested[static_cast<std::size_t>( spec.Id )] = rr;
                    }
                    else if ( method != PathTracingDenoiserMethod::RR || offersRR )
                        return;
                    r.Set( spec.Id, relax, refusedRR ? kRefusedAtRuntime : kNotOffered );
                    return;
                }
            }
        }

        // A requested value Resolve settles by definition rather than lowers: PathTracing.Denoiser Auto becomes RR
        // or ReLAX on every device, which is the meaning of Auto, not a fallback to log.
        [[nodiscard]] bool SettlesUnreported( const Parameter parameter, const ParameterValue requested )
        {
            return parameter == Parameter::PathTracingDenoiser &&
                   requested == static_cast<ParameterValue>( PathTracingDenoiserMethod::Auto );
        }

        // ---- QualityState storage --------------------------------------------------------------------------
        struct State
        {
            ScalabilityTable                                      Table;
            CapabilityCatalog                                     Catalog;
            QualitySelection                                      Selection;
            ResolvedQuality                                       Resolved;
            QualityState::Saver                                   Save = nullptr;
            bool                                                  SessionOnly = false;
            std::vector<std::pair<QualityState::Listener, void*>> Listeners;
            bool                                                  Initialized = false;
        };
        State& S()
        {
            static State state;
            return state;
        }

        void LogUnknownOverrides( const QualitySelection& selection, const QualitySelection* previous )
        {
            for ( const ParameterOverride& o : selection.Overrides )
            {
                if ( FindSpec( o.Key ) != nullptr )
                    continue;
                const bool known = previous != nullptr &&
                                   std::any_of( previous->Overrides.begin(), previous->Overrides.end(),
                                                [&]( const ParameterOverride& p ) { return p.Key == o.Key; } );
                if ( !known )
                    LOG_WARN(
                         "[Scalability] override '{}' = {} names no parameter of this build; it is not applied",
                         o.Key, o.Value );
            }
        }

        // Publish a new resolution: log what is new, bump the generation, notify. Shared by Apply and
        // ReplaceCatalog — the selection does not change on a new device, the resolution may.
        QualityState::ApplyReport Publish( ResolvedQuality resolved )
        {
            State&                    s = S();
            QualityState::ApplyReport report;
            for ( const Fallback& f : resolved.Fallbacks )
                if ( std::find( s.Resolved.Fallbacks.begin(), s.Resolved.Fallbacks.end(), f ) ==
                     s.Resolved.Fallbacks.end() )
                {
                    LOG_WARN( "{}", FormatFallback( f ) );
                    report.NewFallbacks.push_back( f );
                }
            report.ValuesChanged = resolved.Values != s.Resolved.Values || resolved.Scale != s.Resolved.Scale;
            resolved.Generation  = s.Resolved.Generation + ( report.ValuesChanged ? 1 : 0 );
            s.Resolved           = std::move( resolved );
            for ( const auto& [listener, user] : s.Listeners )
                listener( s.Resolved, user );
            return report;
        }

        void EraseOverride( QualitySelection& selection, std::string_view key )
        {
            std::erase_if( selection.Overrides, [key]( const ParameterOverride& o ) { return o.Key == key; } );
        }
    } // namespace

    // ---- Specs -------------------------------------------------------------------------------------------

    std::span<const ParameterSpec> ParameterSpecs()
    {
        return kParameterSpecs;
    }

    const ParameterSpec& SpecOf( Parameter parameter )
    {
        return kParameterSpecs[static_cast<std::size_t>( parameter )];
    }

    bool IsGroupListed( Group group )
    {
        return std::any_of( kParameterSpecs.begin(), kParameterSpecs.end(), [group]( const ParameterSpec& spec )
                            { return spec.Owner == group && !IsPlaceholder( spec ); } );
    }

    std::vector<Parameter> ListedParameters()
    {
        std::vector<Parameter> listed;
        for ( const ParameterSpec& spec : kParameterSpecs )
            if ( !IsPlaceholder( spec ) )
                listed.push_back( spec.Id );
        return listed;
    }

    std::vector<Parameter> ListedParameters( const ParameterVisibility visibility )
    {
        std::vector<Parameter> listed;
        for ( const ParameterSpec& spec : kParameterSpecs )
            if ( !IsPlaceholder( spec ) && spec.Visibility == visibility )
                listed.push_back( spec.Id );
        return listed;
    }

    std::span<const std::string_view> ParameterValueNames( const Parameter parameter )
    {
        return ValueNames( parameter );
    }

    std::string_view GroupKey( Group group )
    {
        constexpr std::array<std::string_view, kGroupCount> kKeys{
             "Textures",    "Filtering", "Shadows",      "GlobalIllumination", "Reflections",
             "PostProcess", "Effects",   "ViewDistance", "AntiAliasing",       "ResolutionScale",
             "Shading",     "PathTracing", "Foliage" };
        return kKeys[static_cast<std::size_t>( group )];
    }

    std::string_view LevelKey( Level level )
    {
        constexpr std::array<std::string_view, kLevelCount> kKeys{ "Low", "Medium", "High", "Epic", "Cinematic" };
        return kKeys[static_cast<std::size_t>( level )];
    }

    // ---- Table -------------------------------------------------------------------------------------------

    Common::ResultStr<ScalabilityTable> ScalabilityTable::Parse( std::string_view jsonText )
    {
        const auto parsed = Json::Parse( jsonText );
        if ( !parsed )
            return Common::MakeError<ScalabilityTable>(
                 std::format( "Scalability.json is not JSON: {}", parsed.GetError() ) );
        const Json::Node root = Json::Root( parsed.GetValue() );

        ScalabilityTable table;
        // Every slot starts at the spec Default: a level that does not name a parameter of its group runs it there;
        // slots a group does not own are never read (Resolve takes a parameter from its own group).
        for ( auto& group : table.Values )
            for ( auto& level : group )
                for ( const ParameterSpec& spec : kParameterSpecs )
                    level[static_cast<std::size_t>( spec.Id )] = spec.Default;

        Errors errors;
        if ( root.GetKind() != Json::Kind::Object )
            return Common::MakeError<ScalabilityTable>( "Scalability.json: the document is not an object" );
        bool versionSeen   = false;
        bool groupsSeen    = false;
        bool recommendSeen = false;
        root.ForEachMember(
             [&]( std::string_view key, const Json::Node& entry )
             {
                 if ( key == "Version" )
                 {
                     versionSeen = true;
                     if ( !ReadNumber( entry, table.Version ) || table.Version == 0 )
                         errors.Add( entry, "Version: expected a positive integer" );
                 }
                 else if ( key == "Groups" )
                 {
                     groupsSeen = true;
                     ParseGroups( entry, table, errors );
                 }
                 else if ( key == "Recommend" )
                 {
                     recommendSeen = true;
                     ParseRecommend( entry, table, errors );
                 }
                 else
                     errors.Add( entry, "unknown top-level key '{}'", key );
             } );
        if ( !versionSeen )
            errors.Add( root, "Version is missing" );
        if ( !groupsSeen )
            errors.Add( root, "Groups is missing" );
        if ( !recommendSeen )
            errors.Add( root, "Recommend is missing" );

        if ( !errors.Empty() )
            return Common::MakeError<ScalabilityTable>( errors.Text() );
        return Common::MakeSuccess( table );
    }

    ParameterValue ScalabilityTable::ValueAt( Parameter parameter, Level level ) const
    {
        const ParameterSpec& spec = SpecOf( parameter );
        return Values[static_cast<std::size_t>( spec.Owner )][static_cast<std::size_t>( level )]
                     [static_cast<std::size_t>( parameter )];
    }

    // ---- Resolve -----------------------------------------------------------------------------------------

    ResolvedQuality Resolve( const QualitySelection& selection, const ScalabilityTable& table,
                             const CapabilityCatalog& catalog )
    {
        Resolution r;
        // 1. Each parameter from its group's level.
        for ( const ParameterSpec& spec : kParameterSpecs )
            r.Requested[static_cast<std::size_t>( spec.Id )] =
                 table.ValueAt( spec.Id, selection.Levels[static_cast<std::size_t>( spec.Owner )] );
        // 2. Overrides on top; a key no spec has is not applied (QualityState::Apply logs it).
        for ( const ParameterOverride& o : selection.Overrides )
            if ( const ParameterSpec* spec = FindSpec( o.Key ) )
            {
                r.Requested[static_cast<std::size_t>( spec->Id )]  = o.Value;
                r.Overridden[static_cast<std::size_t>( spec->Id )] = true;
            }
        r.Values = r.Requested;

        // 3. The spec range. 4. The device's list.
        for ( const ParameterSpec& spec : kParameterSpecs )
        {
            r.Set( spec.Id, std::clamp( r.Get( spec.Id ), spec.Min, spec.Max ), "outside the parameter's range" );
            NarrowByCatalog( spec, catalog, r );
        }
        {
            const bool rayQuery = OffersRayQuery( catalog );
            NarrowMethod<GIMethod>( Parameter::GlobalIlluminationMethod, rayQuery, catalog.FragmentStores, r );
            NarrowMethod<ReflectionMethod>( Parameter::ReflectionsMethod, rayQuery, catalog.FragmentStores, r );
            NarrowMethod<ShadowMethod>( Parameter::ShadowsMethod, rayQuery, catalog.FragmentStores, r );
        }

        // 5. The two axes meet in two places only (UE: r.AntiAliasingMethod is valid at any r.ScreenPercentage up
        // to 100; the vendor plugins refuse supersampled input). A temporal method reconstructs the output itself
        // at any percent <= 100 (Ray Reconstruction included), a method without history is brought to the output
        // below 100 % by the spatial upscale (UpscalerForScale, not a parameter). Above 100 % the vendor networks
        // and TSR are refused by name and TAA resolves the supersampled frame; MSAA renders at native scale only.
        const auto           aa    = static_cast<AntiAliasingMethod>( r.Get( Parameter::AntiAliasingMethod ) );
        const ParameterValue scale = r.Get( Parameter::RenderScalePercent );
        ResolvedQuality      resolved;
        resolved.Scale = scale < 100    ? ScaleMode::Upscale
                         : scale == 100 ? ScaleMode::Native
                                        : ScaleMode::Supersample;
        if ( scale > 100 )
            if ( const std::string_view refusal = SupersampleRefusal( aa ); !refusal.empty() )
                r.Set( Parameter::AntiAliasingMethod, static_cast<ParameterValue>( AntiAliasingMethod::TAA ),
                       refusal );
        if ( scale != 100 && aa == AntiAliasingMethod::MSAA )
            r.Set( Parameter::AntiAliasingMethod, static_cast<ParameterValue>( AntiAliasingMethod::FXAA ),
                   "MSAA renders at native scale only" );

        resolved.Values     = r.Values;
        resolved.Overridden = r.Overridden;
        for ( std::size_t i = 0; i < kParameterCount; ++i )
            if ( r.Values[i] != r.Requested[i] &&
                 !SettlesUnreported( static_cast<Parameter>( i ), r.Requested[i] ) )
                resolved.Fallbacks.push_back(
                     { static_cast<Parameter>( i ), r.Requested[i], r.Values[i], r.Reasons[i] } );
        return resolved;
    }

    bool IsTemporalAntiAliasing( const AntiAliasingMethod method )
    {
        return method == AntiAliasingMethod::TAA || method == AntiAliasingMethod::TSR ||
               IsVendorTemporalMethod( method );
    }

    bool IsVendorTemporalMethod( const AntiAliasingMethod method )
    {
        switch ( method )
        {
            case AntiAliasingMethod::FSR:
            case AntiAliasingMethod::DLSS:
            case AntiAliasingMethod::DLSSRayReconstruction:
            case AntiAliasingMethod::XeSS:
            case AntiAliasingMethod::MetalFX:
                return true;
            case AntiAliasingMethod::None:
            case AntiAliasingMethod::FXAA:
            case AntiAliasingMethod::SMAA:
            case AntiAliasingMethod::MSAA:
            case AntiAliasingMethod::TAA:
            case AntiAliasingMethod::TSR:
                return false;
        }
        return false;
    }

    std::string_view SupersampleRefusal( const AntiAliasingMethod method )
    {
        switch ( method )
        {
            case AntiAliasingMethod::FSR:
                return "FSR does not run above 100 % (it takes no supersampled input)";
            case AntiAliasingMethod::DLSS:
                return "DLSS does not run above 100 % (it takes no supersampled input)";
            case AntiAliasingMethod::DLSSRayReconstruction:
                return "DLSS Ray Reconstruction does not run above 100 % (it takes no supersampled input)";
            case AntiAliasingMethod::XeSS:
                return "XeSS does not run above 100 % (it takes no supersampled input)";
            case AntiAliasingMethod::MetalFX:
                return "MetalFX does not run above 100 % (it takes no supersampled input)";
            case AntiAliasingMethod::TSR:
                return "TSR reconstructs at or below output size; above 100 % TAA resolves the supersampled frame";
            case AntiAliasingMethod::None:
            case AntiAliasingMethod::FXAA:
            case AntiAliasingMethod::SMAA:
            case AntiAliasingMethod::MSAA:
            case AntiAliasingMethod::TAA:
                return {};
        }
        return {};
    }

    Upscaler UpscalerForScale( const AntiAliasingMethod method, const int percent )
    {
        if ( percent > 100 )
            return Upscaler::None;
        switch ( method )
        {
            case AntiAliasingMethod::TAA:
                return percent < 100 ? Upscaler::TAAU : Upscaler::None;
            case AntiAliasingMethod::TSR:
                return Upscaler::TSR;
            case AntiAliasingMethod::FSR:
                return Upscaler::FSR;
            case AntiAliasingMethod::DLSS:
                return Upscaler::DLSS;
            case AntiAliasingMethod::DLSSRayReconstruction:
                return Upscaler::DLSSRayReconstruction;
            case AntiAliasingMethod::XeSS:
                return Upscaler::XeSS;
            case AntiAliasingMethod::MetalFX:
                return Upscaler::MetalFX;
            case AntiAliasingMethod::None:
            case AntiAliasingMethod::FXAA:
            case AntiAliasingMethod::SMAA:
            case AntiAliasingMethod::MSAA:
                return percent < 100 ? Upscaler::Spatial : Upscaler::None;
        }
        return Upscaler::None;
    }

    PathAntiAliasing ResolveAntiAliasingForPath( const ResolvedQuality& resolved, bool pathSupportsMSAA )
    {
        const auto method = resolved.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod );
        switch ( method )
        {
            case AntiAliasingMethod::MSAA:
                if ( pathSupportsMSAA )
                    return { AntiAliasingMethod::MSAA,
                             resolved.As<int>( Parameter::AntiAliasingSamples ),
                             AntiAliasingMethod::None,
                             {} };
                return { AntiAliasingMethod::FXAA, 1, AntiAliasingMethod::FXAA,
                         "this render path cannot multisample (deferred G-buffer)" };
            case AntiAliasingMethod::None:
            case AntiAliasingMethod::FXAA:
            case AntiAliasingMethod::SMAA:
                return { method, 1, method, {} };
            case AntiAliasingMethod::TAA:
            case AntiAliasingMethod::TSR:
            case AntiAliasingMethod::FSR:
            case AntiAliasingMethod::DLSS:
            case AntiAliasingMethod::DLSSRayReconstruction:
            case AntiAliasingMethod::XeSS:
            case AntiAliasingMethod::MetalFX:
                return { method, 1, AntiAliasingMethod::None, {} };
        }
        return { AntiAliasingMethod::FXAA, 1, AntiAliasingMethod::FXAA, "unknown anti-aliasing method" };
    }

    std::string FormatFallback( const Fallback& fallback )
    {
        const ParameterSpec&                    spec  = SpecOf( fallback.Id );
        const std::span<const std::string_view> names = ValueNames( fallback.Id );
        if ( !names.empty() )
            return std::format( "[Scalability] {}: {} -> {} ({})", spec.Key, names[fallback.Requested],
                                names[fallback.Effective], fallback.Reason );
        return std::format( "[Scalability] {}: {} -> {} ({})", spec.Key, fallback.Requested, fallback.Effective,
                            fallback.Reason );
    }

    // ---- Presets -----------------------------------------------------------------------------------------

    std::span<const AntiAliasingPreset> AntiAliasingPresets()
    {
        using M = AntiAliasingMethod;
        // Scale figures are the vendors' published mode ratios (Quality 1/1.5, Balanced 1/1.7, Performance 1/2,
        // Ultra Performance 1/3).
        static constexpr AntiAliasingPreset kPresets[] = {
             { "Native.TAA", M::TAA, 1, 100 },
             { "Native.TSR", M::TSR, 1, 100 },
             { "Native.SMAA", M::SMAA, 1, 100 },
             { "Native.FXAA", M::FXAA, 1, 100 },
             { "Native.None", M::None, 1, 100 },
             { "MSAA.2x", M::MSAA, 2, 100 },
             { "MSAA.4x", M::MSAA, 4, 100 },
             { "MSAA.8x", M::MSAA, 8, 100 },
             { "SSAA.150", M::TAA, 1, 150 },
             { "SSAA.200", M::TAA, 1, 200 },
             { "TAA.Quality", M::TAA, 1, 67 },
             { "TAA.Balanced", M::TAA, 1, 58 },
             { "TAA.Performance", M::TAA, 1, 50 },
             { "TSR.Quality", M::TSR, 1, 67 },
             { "TSR.Balanced", M::TSR, 1, 58 },
             { "TSR.Performance", M::TSR, 1, 50 },
             { "FSR.Native", M::FSR, 1, 100 },
             { "FSR.Quality", M::FSR, 1, 67 },
             { "FSR.Balanced", M::FSR, 1, 58 },
             { "FSR.Performance", M::FSR, 1, 50 },
             { "FSR.UltraPerformance", M::FSR, 1, 33 },
             { "DLSS.Native", M::DLSS, 1, 100 },
             { "DLSS.Quality", M::DLSS, 1, 67 },
             { "DLSS.Balanced", M::DLSS, 1, 58 },
             { "DLSS.Performance", M::DLSS, 1, 50 },
             { "DLSS.UltraPerformance", M::DLSS, 1, 33 },
             { "DLSSRR.Native", M::DLSSRayReconstruction, 1, 100 },
             { "DLSSRR.Quality", M::DLSSRayReconstruction, 1, 67 },
             { "DLSSRR.Balanced", M::DLSSRayReconstruction, 1, 58 },
             { "DLSSRR.Performance", M::DLSSRayReconstruction, 1, 50 },
             { "XeSS.Quality", M::XeSS, 1, 67 },
             { "XeSS.Balanced", M::XeSS, 1, 58 },
             { "XeSS.Performance", M::XeSS, 1, 50 },
             { "MetalFX.Quality", M::MetalFX, 1, 67 },
             { "MetalFX.Performance", M::MetalFX, 1, 50 },
        };
        return kPresets;
    }

    std::vector<ParameterOverride> Decompose( const AntiAliasingPreset& preset )
    {
        return { { std::string( SpecOf( Parameter::AntiAliasingMethod ).Key ),
                   static_cast<ParameterValue>( preset.Method ) },
                 { std::string( SpecOf( Parameter::AntiAliasingSamples ).Key ), preset.Samples },
                 { std::string( SpecOf( Parameter::RenderScalePercent ).Key ), preset.RenderScalePercent } };
    }

    std::vector<AntiAliasingPreset> OfferedPresets( const CapabilityCatalog& catalog )
    {
        // Asked of Resolve itself, so "offered" can never disagree with what the preset resolves to. The four
        // overridden parameters do not read the table, so an empty one serves.
        const ScalabilityTable          anyTable;
        std::vector<AntiAliasingPreset> offered;
        for ( const AntiAliasingPreset& preset : AntiAliasingPresets() )
        {
            QualitySelection selection;
            selection.Overrides       = Decompose( preset );
            const ResolvedQuality r   = Resolve( selection, anyTable, catalog );
            const bool            any = std::any_of( r.Fallbacks.begin(), r.Fallbacks.end(),
                                                     []( const Fallback& f )
                                                     {
                                              return f.Id == Parameter::AntiAliasingMethod ||
                                                     f.Id == Parameter::AntiAliasingSamples ||
                                                     f.Id == Parameter::RenderScalePercent;
                                          } );
            if ( !any )
                offered.push_back( preset );
        }
        return offered;
    }

    // ---- QualityState ------------------------------------------------------------------------------------

    void QualityState::Initialize( ScalabilityTable table, CapabilityCatalog catalog, QualitySelection saved,
                                   Saver save )
    {
        State& s      = S();
        s.Table       = table;
        s.Catalog     = std::move( catalog );
        s.Selection   = std::move( saved );
        s.Save        = save;
        s.SessionOnly = false;
        s.Resolved    = {};
        s.Initialized = true;
        LogUnknownOverrides( s.Selection, nullptr );
        Publish( Resolve( s.Selection, s.Table, s.Catalog ) );
    }

    void QualityState::KeepInSession( std::string_view why )
    {
        S().SessionOnly = true;
        LOG_INFO( "[Scalability] {}: quality changes apply to this process only; machine.json is not written",
                  why );
    }

    void QualityState::ReplaceCatalog( CapabilityCatalog catalog )
    {
        State& s  = S();
        s.Catalog = std::move( catalog );
        // Before Initialize there is no table to resolve against; Initialize takes the device's catalog then.
        if ( s.Initialized )
            Publish( Resolve( s.Selection, s.Table, s.Catalog ) );
    }

    QualityState::ApplyReport QualityState::Apply( const QualitySelection& selection )
    {
        State& s = S();
        if ( !s.Initialized )
        {
            LOG_ERROR(
                 "[Scalability] a quality change arrived before QualityState::Initialize; it was not applied" );
            ApplyReport refused;
            refused.Refused = "QualityState is not initialized";
            return refused;
        }
        LogUnknownOverrides( selection, &s.Selection );
        s.Selection        = selection;
        ApplyReport report = Publish( Resolve( s.Selection, s.Table, s.Catalog ) );
        if ( s.SessionOnly )
        {
            report.SessionOnly = true;
            LOG_INFO( "[Scalability] quality applied for this session only ({} override(s)); machine.json "
                      "untouched",
                      s.Selection.Overrides.size() );
            return report;
        }
        if ( s.Save == nullptr )
        {
            LOG_ERROR( "[Scalability] QualityState was initialized without a saver; the selection applies to this "
                       "session only" );
            return report;
        }
        const auto saved = s.Save( s.Selection );
        report.Saved     = saved.IsSuccess();
        if ( !report.Saved )
            LOG_ERROR( "[Scalability] the quality selection was not saved: {} — it applies to this session only",
                       saved.GetError() );
        return report;
    }

    QualityState::ApplyReport QualityState::SetGroupLevel( Group group, Level level )
    {
        QualitySelection selection                          = S().Selection;
        selection.Levels[static_cast<std::size_t>( group )] = level;
        std::erase_if( selection.Overrides,
                       [group]( const ParameterOverride& o )
                       {
                           const ParameterSpec* spec = FindSpec( o.Key );
                           return spec != nullptr && spec->Owner == group;
                       } );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::SetAllGroups( Level level )
    {
        QualitySelection selection;
        selection.Levels.fill( level );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::SetOverride( Parameter parameter, ParameterValue value )
    {
        const ParameterSpec& spec = SpecOf( parameter );
        if ( IsPlaceholder( spec ) )
        {
            // A placeholder has no reader: setting it would be a knob that moves nothing.
            LOG_ERROR( "[Scalability] {} is reserved for a feature this build does not have; not set", spec.Key );
            ApplyReport refused;
            refused.Refused = "the parameter has no reader in this build";
            return refused;
        }
        QualitySelection selection = S().Selection;
        EraseOverride( selection, spec.Key );
        selection.Overrides.push_back( { std::string( spec.Key ), value } );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::ClearOverride( Parameter parameter )
    {
        QualitySelection selection = S().Selection;
        EraseOverride( selection, SpecOf( parameter ).Key );
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::ApplyPreset( const AntiAliasingPreset& preset )
    {
        QualitySelection selection = S().Selection;
        for ( ParameterOverride& o : Decompose( preset ) )
        {
            EraseOverride( selection, o.Key );
            selection.Overrides.push_back( std::move( o ) );
        }
        return Apply( selection );
    }

    QualityState::ApplyReport QualityState::ApplyRecommended( const std::array<Level, kGroupCount>& levels )
    {
        // UE's benchmark sets the group levels and nothing else; overrides belong to a choice the player made,
        // and the host calls this on a first run with no saved choice (QualityBoot) or when the user asks for it
        // (the editor Scalability panel's "Apply recommended").
        QualitySelection selection;
        selection.Levels = levels;
        return Apply( selection );
    }

    const QualitySelection& QualityState::Selection()
    {
        return S().Selection;
    }
    const ResolvedQuality& QualityState::Resolved()
    {
        return S().Resolved;
    }
    const CapabilityCatalog& QualityState::Catalog()
    {
        return S().Catalog;
    }
    const ScalabilityTable& QualityState::Table()
    {
        return S().Table;
    }

    void QualityState::Subscribe( Listener listener, void* user )
    {
        S().Listeners.emplace_back( listener, user );
    }

    void QualityState::Unsubscribe( Listener listener, void* user )
    {
        std::erase( S().Listeners, std::pair<Listener, void*>{ listener, user } );
    }
} // namespace Common::Scalability
