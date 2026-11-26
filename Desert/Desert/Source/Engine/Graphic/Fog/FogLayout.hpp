#pragma once

// THE FOG BLOCKS, declared once: Tools/ShaderProtocolGen writes their GLSL into Generated/Fog/*.glslh (included by
// Common/FogParams.glslh, Common/VolumetricFog.glslh, Common/HeterogeneousVolume.glslh and the heterogeneous
// volume programs) and checks every std430 layout against these declarations at compile time.
//
// Units: the height fog block is in KILOMETRES and per-kilometre coefficients (PackFogParams converts once); the
// volumetric fog and the heterogeneous volumes are in centimetres.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic
{
    // The opaque fog compute's slot of FogParamsBuffer. A surface cell #defines FOG_PARAMS_BINDING first to its
    // scene-read slot (Mesh/Surface/SceneFog.glslh, Core::kSceneFogParamsBinding); both find the buffer by name.
    inline constexpr uint32_t kFogParamsBinding = 1;

    // THE height-fog parameter block (FogParamsBuffer), filled by PackFogParams and HeightFogRenderer.
#define DESERT_FOG_GPU_PAYLOAD_FIELDS( X )                                                                        \
    X( glm::vec4, SunDirection, "FogSun",                                                                         \
       {} ) /* xyz toward sun (normalized), w = DirectionalInscatteringExponent */                                \
    X( glm::vec4, Directional, "FogDirectional", {} ) /* rgb lobe colour, w = directional start distance (km) */  \
    X( glm::vec4, Inscattering, "FogInscattering",                                                                \
       {} ) /* rgb fog colour (sky ambient folded in), w = 1 - FogMaxOpacity */                                   \
    X( glm::vec4, Layer0, "FogLayer0",                                                                            \
       {} ) /* x density /km, y falloff /km, z fog height (km), w start distance (km) */                          \
    X( glm::vec4, Layer1, "FogLayer1",                                                                            \
       {} ) /* x density /km, y falloff /km, z fog height (km), w cutoff distance (km) */                         \
    /* The SKY AMBIENT: rgb = SkyAtmosphereAmbientContributionColorScale, w = 1 when the distant-sky-light texel  \
       (Programs/Sky/SkyDistantLight.shader, this frame) is bound and must be sampled. */                         \
    X( glm::vec4, Ambient, "FogAmbient", {} )                                                                     \
    /* The VOLUMETRIC FOG composed under the analytic fog (HeightFogRenderer fills both; PackFogParams leaves     \
       zero). Volumetric0: x = froxel edge (px), y/z = froxels across/down, w = 1 when bound; Volumetric1:        \
       xyz = B, O, S of slice = log2(depth * B + O) * S (depth in cm), w = slices. */                             \
    X( glm::vec4, Volumetric0, "FogVolumetric0", {} )                                                             \
    X( glm::vec4, Volumetric1, "FogVolumetric1", {} )                                                             \
    /* The per-VIEW facts (Common/ViewFog.glslh reads the same block from every translucent draw). View: xy =     \
       view size (px), z = AP volume depth (km), w = AP view-distance scale. Gates: x = AP volume sampled,        \
       y = height fog enabled, z = ViewFrame::PreExposure. Forward: camera forward (world). Camera: position. */  \
    X( glm::vec4, View, "FogView", {} )                                                                           \
    X( glm::vec4, Gates, "FogGates", {} )                                                                         \
    X( glm::vec4, Forward, "FogForward", {} )                                                                     \
    X( glm::vec4, Camera, "FogCamera", {} )
    DESERT_SHADER_STRUCT( FogGpuPayload, "FogGpuPayload", Std430, DESERT_FOG_GPU_PAYLOAD_FIELDS );
#undef DESERT_FOG_GPU_PAYLOAD_FIELDS

    inline constexpr ShaderProtocols::ShaderBlockDecl   kFogParamsBlocks[] = { {
           .Kind    = ShaderProtocols::ShaderBlockKind::ReadBuffer,
           .Binding = { ShaderProtocols::ShaderBindingKind::Overridable, kFogParamsBinding, "FOG_PARAMS_BINDING" },
           .BlockName = "FogParamsBuffer",
           .Members   = &kFogGpuPayloadStruct,
    } };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kFogParamsShaderInclude{
         .Path         = "Generated/Fog/FogParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Fog/FogLayout.hpp",
         .Blocks       = kFogParamsBlocks,
    };

    // The opaque volumetric fog passes' slot of VolumetricFogParamsBuffer (SetStorageBuffer takes the number).
    inline constexpr uint32_t kVolumetricFogParamsBinding = 1;

    using Mat4ArrayFogCascades = std::array<glm::mat4, 4>;
    // The volumetric fog grid's parameter block (UE FVolumetricFogGlobalData). Centimetres and per-centimetre
    // coefficients. Packed per frame per view by VolumetricFogRenderer.
#define DESERT_VOLUMETRIC_FOG_GPU_PAYLOAD_FIELDS( X )                                                             \
    X( glm::mat4, InvViewProjection, "VfInvViewProjection",                                                       \
       glm::mat4( 1.0f ) ) /* this frame's unjittered camera */                                                   \
    X( glm::mat4, ViewProjection, "VfViewProjection", glm::mat4( 1.0f ) )                                         \
    X( glm::mat4, PrevViewProjection, "VfPrevViewProjection", glm::mat4( 1.0f ) ) /* the reprojection */          \
    X( Mat4ArrayFogCascades, LightViewProj, "VfCascadeViewProj", {} ) /* the sun's cascades, standard Z */        \
    X( glm::uvec4, GridSize, "VfGridSize", glm::uvec4( 1u ) )         /* xyz froxels, w frame index */            \
    X( glm::vec4, GridZParams, "VfGridZParams", {} )                  /* B, O, S, grid far (cm) */                \
    X( glm::vec4, GridPixel, "VfGridPixel", {} ) /* froxel edge px, render width, height, history valid */        \
    X( glm::vec4, Jitter, "VfJitter", {} )       /* in-froxel offset, w = history weight */                       \
    X( glm::vec4, Camera, "VfCamera", {} )       /* position cm, w = phase g */                                   \
    X( glm::vec4, CameraForward, "VfCameraForward", {} )                                                          \
    X( glm::vec4, PrevCamera, "VfPrevCamera", {} )                                                                \
    X( glm::vec4, PrevCameraForward, "VfPrevCameraForward", {} )                                                  \
    X( glm::vec4, Albedo, "VfAlbedo", {} )     /* rgb, w = extinction scale */                                    \
    X( glm::vec4, Emissive, "VfEmissive", {} ) /* rgb, w = start distance cm */                                   \
    X( glm::vec4, Fade, "VfFade", {} )         /* x = near fade-in distance cm */                                 \
    X( glm::vec4, Layer0, "VfLayer0", {} )     /* density /cm, falloff /cm, height cm */                          \
    X( glm::vec4, Layer1, "VfLayer1", {} )                                                                        \
    X( glm::vec4, SunDirection, "VfSunDirection", {} ) /* toward the sun, w = sun present */                      \
    X( glm::vec4, SunRadiance, "VfSunRadiance", {} )   /* rgb, w = cascade count */                               \
    X( glm::vec4, CascadeBias, "VfCascadeBias", {} )   /* per cascade, map depth units */                         \
    X( glm::vec4, SkyAmbient, "VfSkyAmbient", {} )     /* rgb CPU-folded sky, w = sample the distant sky light */ \
    X( glm::vec4, SkyAmbientScale, "VfSkyAmbientScale", {} ) /* rgb scale on the texel */                         \
    /* The cloud layer's shadow on the sun term: the CloudShadowUniforms every surface receiver reads             \
       (Params.y = 0 when no layer casts, and the froxels then never fetch the map). */                           \
    X( glm::mat4, CloudShadowWorldToMap, "VfCloudShadowWorldToMap", glm::mat4( 1.0f ) )                           \
    X( glm::vec4, CloudShadowParams, "VfCloudShadowParams",                                                       \
       {} ) /* far depth km, live, border fade uv, strength */                                                    \
    /* The view's rain occlusion (PackRainOcclusion): Params.y = 0 when the view has no map. */                   \
    X( glm::mat4, RainOcclusionWorldToMap, "VfRainOcclusionWorldToMap", glm::mat4( 1.0f ) )                       \
    X( glm::vec4, RainOcclusionParams, "VfRainOcclusionParams",                                                   \
       {} )                                     /* depth range cm, valid, bias cm, texel cm */                    \
    X( glm::uvec4, LocalFog, "VfLocalFog", {} ) /* x = local fog volumes in LocalFogVolumesBuffer */
    DESERT_SHADER_STRUCT( VolumetricFogGpuPayload, "VolumetricFogGpuPayload", Std430,
                          DESERT_VOLUMETRIC_FOG_GPU_PAYLOAD_FIELDS );
#undef DESERT_VOLUMETRIC_FOG_GPU_PAYLOAD_FIELDS

    // ONE LOCAL FOG VOLUME as the froxels voxelise it (UE 5.3 FLocalFogVolumeGPUInstanceData). Packed per frame by
    // PackLocalFogVolume.
#define DESERT_LOCAL_FOG_VOLUME_GPU_FIELDS( X )                                                                   \
    /* World cm -> the volume's unit space: the unit sphere (Sphere) or the [-1, 1] cube (Box). */                \
    X( glm::mat4, WorldToVolume, "WorldToVolume", glm::mat4( 1.0f ) )                                             \
    /* x = radial extinction at the centre (/cm), y = height-fog extinction at the offset (/cm),                  \
       z = height falloff (per unit of the volume's half height), w = height offset (unit space, -1..1) */        \
    X( glm::vec4, Density, "Density", {} )                                                                        \
    X( glm::vec4, Albedo, "Albedo", {} )     /* rgb albedo, w = shape (0 sphere, 1 box) */                        \
    X( glm::vec4, Emissive, "Emissive", {} ) /* rgb radiance per unit extinction, w = edge fade width */
    DESERT_SHADER_STRUCT( LocalFogVolumeGpu, "LocalFogVolumeGpu", Std430, DESERT_LOCAL_FOG_VOLUME_GPU_FIELDS );
#undef DESERT_LOCAL_FOG_VOLUME_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kVolumetricFogStructs[] = {
         &kLocalFogVolumeGpuStruct };
    inline constexpr ShaderProtocols::ShaderBlockDecl   kVolumetricFogBlocks[] = { {
           .Kind      = ShaderProtocols::ShaderBlockKind::ReadBuffer,
           .Binding   = { ShaderProtocols::ShaderBindingKind::Overridable, kVolumetricFogParamsBinding,
                          "VOLUMETRIC_FOG_PARAMS_BINDING" },
           .BlockName = "VolumetricFogParamsBuffer",
           .Members   = &kVolumetricFogGpuPayloadStruct,
    } };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kVolumetricFogShaderInclude{
         .Path         = "Generated/Fog/VolumetricFog.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Fog/FogLayout.hpp",
         .Structs      = kVolumetricFogStructs,
         .Blocks       = kVolumetricFogBlocks,
    };

    // Volumes per view (the lighting-cache atlas holds this many slabs along Z).
    inline constexpr uint32_t kHeterogeneousVolumeMaxVolumes = 8;
    // The lighting cache's voxels per axis per volume (UE r.HeterogeneousVolumes.LightingCache, its default
    // downsample of a 128-voxel volume is 32): RGBA16F, 32^3 x 8 slabs = 2 MiB.
    inline constexpr uint32_t kHeterogeneousVolumeCacheResolution = 32;
    // The pipelines' own buffers (SetStorageBuffer takes the number): the volumes, read by the cache, the march
    // and - for the froxels' sun shadow - VolumetricFogLightScattering.shader; and the march's parameter block.
    inline constexpr uint32_t kHeterogeneousVolumesBinding      = 15;
    inline constexpr uint32_t kHeterogeneousVolumeParamsBinding = 17;
    // The resident baked volumes' page tables, the cache's and the march's own buffer like the two above.
    inline constexpr uint32_t kSparseVolumePagesBinding = 18;

    // ONE HETEROGENEOUS VOLUME as the lighting cache, the march and the froxels' shadow read it (std430). Packed
    // per frame by PackHeterogeneousVolume from ECS::HeterogeneousVolumeData and the entity's world transform.
#define DESERT_HETEROGENEOUS_VOLUME_GPU_FIELDS( X )                                                               \
    X( glm::mat4, LocalToWorld, "LocalToWorld", glm::mat4( 1.0f ) ) /* the unit box [-1, 1]^3 -> world */         \
    X( glm::mat4, WorldToLocal, "WorldToLocal", glm::mat4( 1.0f ) ) /* world -> the unit box */                   \
    X( glm::vec4, Medium, "Medium", {} ) /* x = extinction per cm, y = phase g, z = coverage, w = edge fade */    \
    X( glm::vec4, Albedo, "Albedo", {} ) /* rgb = albedo, w = 1 / NoiseScale (per cm) */                          \
    X( glm::vec4, HalfSize, "HalfSize", {} ) /* xyz = world half size of the box along its axes (cm) */           \
    X( glm::vec4, Wind, "Wind", {} )         /* xyz = local advection velocity (cm/s), w = the noise clock (s) */ \
    X( glm::vec4, Emissive, "Emissive", {} ) /* rgb = emission per unit extinction, w = peak temperature (K) */   \
    X( glm::vec4, Blackbody, "Blackbody", {} ) /* x = blackbody intensity, y = lighting-cache slab (renderer) */  \
    /* The baked source (DensitySource BakedSparse), set by the renderer's residency once the asset's bricks are  \
       in the shared atlas: x = the asset's first page-table entry, yzw = pages per axis (0 = procedural). */     \
    X( glm::uvec4, Sparse, "Sparse", {} )                                                                         \
    X( glm::vec4, SparseScale, "SparseScale",                                                                     \
       {} ) /* xyz = the asset's voxels per axis, w = its peak temperature */
    DESERT_SHADER_STRUCT( HeterogeneousVolumeGpu, "HeterogeneousVolumeGpu", Std430,
                          DESERT_HETEROGENEOUS_VOLUME_GPU_FIELDS );
#undef DESERT_HETEROGENEOUS_VOLUME_GPU_FIELDS

    // The march's parameter block (HeterogeneousVolumeMarchParams of the march and the upsample programs).
#define DESERT_HETEROGENEOUS_VOLUME_MARCH_GPU_FIELDS( X )                                                         \
    X( glm::uvec4, Counts, "HvCounts",                                                                            \
       {} ) /* x = volumes, y = frame index (jitter), z = history valid, w = steps */                             \
    X( glm::vec4, Target, "HvTarget", {} )   /* xy = half-res size, zw = full render size (pixels) */             \
    X( glm::vec4, History, "HvHistory", {} ) /* x = history weight, yzw unused */                                 \
    /* The volumes' screen rectangle (uv min xy, max zw, CPU-projected boxes): outside it the march and the       \
       upsample write "no medium" without marching, so the cost follows the volumes' screen size. */              \
    X( glm::vec4, Rect, "HvRect", {} )
    DESERT_SHADER_STRUCT( HeterogeneousVolumeMarchGpu, "HeterogeneousVolumeMarchGpu", Std430,
                          DESERT_HETEROGENEOUS_VOLUME_MARCH_GPU_FIELDS );
#undef DESERT_HETEROGENEOUS_VOLUME_MARCH_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kHeterogeneousVolumeStructs[] = {
         &kHeterogeneousVolumeGpuStruct, &kHeterogeneousVolumeMarchGpuStruct };
    inline constexpr ShaderProtocols::ShaderConstantDecl kHeterogeneousVolumeConstants[] = {
         { ShaderProtocols::ShaderConstantDecl::Type::Int, "HETEROGENEOUS_VOLUMES_BINDING",
           static_cast<double>( kHeterogeneousVolumesBinding ) },
         { ShaderProtocols::ShaderConstantDecl::Type::Float, "HETEROGENEOUS_VOLUME_CACHE_RES",
           static_cast<double>( kHeterogeneousVolumeCacheResolution ) },
         { ShaderProtocols::ShaderConstantDecl::Type::Float, "HETEROGENEOUS_VOLUME_MAX",
           static_cast<double>( kHeterogeneousVolumeMaxVolumes ) },
         { ShaderProtocols::ShaderConstantDecl::Type::Int, "HETEROGENEOUS_VOLUME_PAGES_BINDING",
           static_cast<double>( kSparseVolumePagesBinding ) },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kHeterogeneousVolumeShaderInclude{
         .Path         = "Generated/Fog/HeterogeneousVolume.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Fog/FogLayout.hpp",
         .Structs      = kHeterogeneousVolumeStructs,
         .Constants    = kHeterogeneousVolumeConstants,
    };
} // namespace Desert::Graphic
