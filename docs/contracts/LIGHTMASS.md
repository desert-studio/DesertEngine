# LIGHTMASS — baked static lighting (header contract, 10-21)

Owner backlog item LIGHTMASS: lightmaps, baked shadows and baked GI as in UE (Lightmass / GPU Lightmass).
This is the CONTRACT only: public headers with declarations, no definitions, nothing includes them yet.
Branch `task/LIGHTMASS-CONTRACT` from `int/w1021`, unbuilt.

## Headers

| Header | What it fixes | UE counterpart |
|---|---|---|
| `Desert/Desert/Source/Engine/ECS/StaticLightingSettings.hpp` | `ECS::Mobility` (Static / Stationary / Movable); `StaticLightingPrimitiveSettings` (mobility, cast static shadow, diffuse boost); `StaticLightingLightmapSettings` (lightmap resolution override, only on what owns a lightmap); `StaticLightingLightSettings` (mobility, indirect intensity, bake shadow exponent) | `EComponentMobility`, `UPrimitiveComponent::bOverrideLightMapRes / OverriddenLightMapRes / bCastStaticShadow / LightmassSettings`, `ULightComponent::IndirectLightingIntensity / LightmassSettings` |
| `Desert/Desert/Source/Engine/Assets/StaticLighting/MeshLightmapUV.hpp` | the mesh build's lightmap UV generation and channel | `FMeshBuildSettings::bGenerateLightmapUVs / SrcLightmapIndex / DstLightmapIndex / MinLightmapResolution`, `UStaticMesh::LightMapCoordinateIndex / LightMapResolution` |
| `Desert/Desert/Source/Engine/Assets/StaticLighting/LightingBuildData.hpp` | the baked-data asset `.delightbuild`: per-primitive lightmap + shadow map placement, per-light shadow channel, the volumetric lightmap | `UMapBuildDataRegistry`, `FMeshMapBuildData`, `FLightComponentMapBuildData`, `FPrecomputedVolumetricLightmapData`, `FLightMap2D` / `FShadowMap2D` |
| `Desert/Desert/Source/Engine/Assets/StaticLighting/StaticLightingInputs.hpp` | the static set and its input hashes, shared by the bake, the overlay and the cook's gate `CheckLightingBuildCurrent` | `UMapBuildDataRegistry` validity / level lighting GUID |
| `Desert/Desert/Source/Engine/Assets/StaticLighting/StaticLightingInputs.hpp` | the static set and its input hashes, shared by the bake, the overlay and the cook's gate `CheckLightingBuildCurrent` | `UMapBuildDataRegistry` validity / level lighting GUID |
| `Desert/Desert/Source/Engine/Graphic/StaticLighting/LightmassBake.hpp` | `LightmassSettings`, `ILightmassBake` (per-frame passes, progress, cancel, result), `CreateGpuLightmassBake` | `FGPULightmass`, `FLightmapRenderer`, `FVolumetricLightmapRenderer`, `UGPULightmassSettings`, `FLightmassWorldInfoSettings` |
| `Desert/Desert/Source/Engine/Graphic/StaticLighting/StaticLightingRenderer.hpp` | `LightmapRowGpu`, `StaticLightingFrame`, `StaticLightingRenderer` (build registration, light treatment per resolved method, the per-view frame record, stale count) | `FLightmapSceneShaderData`, `LightmapCommon.ush`, `FVolumetricLightmapSceneData`, base pass `GetPrecomputedIndirectLightingAndSkyLight`, GBufferE `PrecomputedShadowFactors` |

## The baker reuses the hardware RT path (GPU Lightmass)

GPU Lightmass is a path tracer started from lightmap texels. This engine already has the path tracer
(`System::PathTracer`, `GIMethod::PathTraced`): NEE + MIS, importance-sampled sky, emissive triangle lights,
hit groups per surface template. The baker uses the same `RayTracingSceneFrame` TLAS (static set by an
instance-mask bit), `RayTracingHitGroupTable`, `EmissiveLightsFrame`, the PathTracerSky prepare/reduce and the
shared integrator include. New code: the texel G-buffer (rasterising into lightmap UV space), the accumulation
targets, the stationary shadow distance-field pass, the volumetric lightmap brick refinement, the denoise and
the encode. It runs progressively inside the editor frame with a time budget, like UE's GPU Lightmass. There is
no CPU Lightmass and no Swarm.

## Coexistence with the GI methods

- `GIMethod::Lightmaps` is already declared (rung 1, `Common/Settings/RenderMethods.hpp`, refused as not
  implemented). It is the only method that reads the indirect bake. `IsImplemented` and `NeedsGBuffer` flip
  for it with the implementation.
- Every other GI method ignores the bake, as Lumen does in UE. Static and Stationary lights are then drawn as
  Movable, and the overlay says so (UE `r.AllowStaticLighting=0` does the same).
- Base pass: Static primitives sample the lightmap (L1 SH, HQ encoding) into scene colour and write the
  stationary shadow factors to an extra RGBA8 G-buffer target on Lightmaps views. Movable primitives,
  translucency, fog, hair and particles read the volumetric lightmap.
- Shadows (Q2): Static lights are drawn by no runtime shadow pass. A Stationary light's shadow is min(baked
  channel, the resolved shadow method over Movable casters only) under every shadow method.
- Reflection methods are unaffected. Captures are taken with the lightmapped lighting.

## Where this engine differs from UE

1. **Mobility** exists only on lit primitives and the three analytic lights, as one embedded block per side.
   UE puts it on every `USceneComponent`.
2. **The shadow channel is a bake result** stored in the build data, not on the light component (UE
   `ShadowMapChannel`). One source of truth.
3. **Staleness is a per-record input hash**, not a per-level GUID, so the "needs rebuild" count is exact.
4. **A Static primitive moved at runtime is refused with an error.** UE only warns.
5. **Lightmap textures cook through the DDC** (BC6H for coefficient 0, BC7 for the rest).
6. **One method ladder.** Static lighting is a rung of `GIMethod` resolved by `ResolveRenderMethods`, not a
   project-wide `r.AllowStaticLighting` switch.

## Not ported

CPU Lightmass (photon mapping, irradiance cache, Swarm), the indirect lighting cache (UE4 ILC, superseded by
the VLM), lightmap streaming (UE `r.TextureStreaming` on lightmaps), the LQ lightmap encoding, precomputed
visibility, and the sky light's stationary mode (no SkyLight component exists here; the sky is
SkyAtmosphere/Skybox).

## Scene format (implementation task)

`StaticLightingPrimitiveSettings` becomes member `StaticLighting` of `StaticMeshComponent`, `InstancedStaticMeshComponent`
and `LandscapeComponent`; `StaticLightingLightmapSettings` member `Lightmap` of the same three (not drawn on a foliage
field); `StaticLightingLightSettings` member `StaticLighting` of `DirectionalLightData`, `PointLightData`, `SpotLightData`.
`LightmassSettings` and the build-data GUID become members of `Core::SceneSettings` (World Settings). A scene version
bump with a SceneMigrator step writes Movable everywhere, so existing scenes render unchanged.
`AssetTypeID::LightingBuildData` is appended. `Core::WorldCells::CookWorld` takes the saved build beside the HLODs
and calls `Assets::CheckLightingBuildCurrent`.

## Lead decisions (10-22, on the owner's behalf: "architecturally right")

- **Q1** Settings live in the existing light / mesh / landscape components + World Settings (UE LightmassSettings). No
  separate component (the header was renamed from StaticLightingComponents.hpp to StaticLightingSettings.hpp).
- **Q2** Static light: shadow fully in the lightmap, VSM (and every runtime shadow method) does not draw it. Stationary:
  min(baked mask, runtime shadow of Movable casters only).
- **Q3** OIDN, as UE GPU Lightmass. No engine-written denoiser.
- **Q4** No rescale: bounce fixed at bake time (UE); an Intensity / Color change of a Static / Stationary light stales the
  build (it is in the light's InputHash).
- **Q5** With GI != Lightmaps, Static lights behave as Movable (UE Allow Static Lighting = off).
- **Q6** Each LOD has its own lightmap (one record and one GPU row per LOD).
- **Q7** Landscape: lightmap per tile. Foliage and grass: volumetric lightmap only.
- **Q8** Bake = editor step (Build Lighting). The cook only packages and is refused, naming the entities, on stale data.
