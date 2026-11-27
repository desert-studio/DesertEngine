# REMAINDER — FIX-REFR2-DAY (context limit; continues FIX-REFR2-NIGHT)

Brief: C:/Users/danil/.claude/studio/briefs/FIX-REFR2-DAY-BRIEF.md. Nothing changed in code or scenario yet. Read the
sources from `git show ng6build:<path>` (ng6build carries the B fixes 8deb0ca0b / 881573bd5 and C's d2c887668 that the
task branches lack). Commit 7d8dbe913 (GPU profiling On/Off) stays.

## Night (stands, from FIX-REFR2-NIGHT)
The night opaque delta is unconverged ScreenSpace frames (GI accumulation + auto exposure), not the RT pass: the
scenario fix (item 2) is to lock exposure and wait `frames 256` after each GI method switch. No code change.

## Day glass — read and cleared (no defect found in these)
- Composite: Pass_Forward_Translucent.glslh:181 `DesertTranslucencyRefraction` -> TranslucencyRefraction.glslh (same
  ulps test, kFrontLayerDepthThresholdUlps 1024, as DesertFrontLayerTracedReflection); mix by traced.a.
- Binding: MeshRendererForward.cpp DeclareGlassBindings (~1617-1635) binds FrontLayerDepth + TranslucencyRefraction by
  name under the FrontLayerReflectionTexture slot bit; SceneRendererFrameMesh.cpp:1214-1220 transparent black when absent.
- Order: SceneRenderer.cpp:3248 AddFrameRayTracedRefraction before AddFrameGlass; front layer from :3163.
- TLAS: translucent primitives get kMaskTranslucent (RayTracingScene.hpp:409-413); the trace uses mask 0xFF, sbt 0/1.
- Hit groups: one table per frame (SceneRendererFrameDeferred.cpp:2279 BuildReflectionHitGroups); a translucent
  template without a compiled `<T>/RayTracing.Hit` falls back to the default surface and LOG_WARNs once
  ("Reflections hit lighting: '<T>' has no compiled hit group"). Pass_RayHit.glslh:106-110 writes Transmission/Ior
  only under DESERT_SURFACE_TRANSLUCENT.

## Day glass — still to check, in this order
1. editor.log of the run scn-1127-172729: any "[SceneRenderer] Translucency.Refraction HardwareTraced: ..." refusal
   (SceneRendererFrameMesh.cpp:1126-1196, LOG_ERROR once per reason) or the hit-group fallback warning naming the
   glass template. A refusal there = the pass never fed the glass.
2. Roughness: RayTracedTranslucency.shader:116 writes a = 0 when front.a > MaxRoughness (0.4,
   RayTracedTranslucency.hpp:75); Pass_FrontLayer.glslh:50 writes max(s.Roughness, kDesertMinRoughness). Check the
   bistro glass material's Roughness (mesh BistroExterior_Bistro_Research_Exterior__lod0_Glass_3919.stmesh, entity
   MaterialSlots [] -> the mesh's own material; candidates Content/Materials/CB_Glass.demat, Starter_Glass*.demat in
   F:/BistroFoliageProj). If it is > 0.4 and the glass reads smooth (it shades as roughness 0, Pass_Forward_Translucent
   uses 0.0 for its reflection), the threshold and the front layer disagree with the glass cell: one statement.
3. Whether that material's template is BlendMode Translucent at all (else no front layer, no glass cell).

## Shop window for the camera (item 2)
B16_Bistro_Street.desce: entity 12847028046335218154 "Bistro_Research_Exterior__lod0_Glass_3919", Translation
-779.07, 203.68, 4.54, parent 32289041018609040 (compose the parent's transform; the mesh's bounds give the panes).
The same entity exists in GI_Bistro_Night.desce (grep the Tag). Exposure lock: `set <PPV id> PostProcessVolumeData.
Settings.AutoExposure false` + ManualExposureEV100, restored in the tail (FOLIAGE-PT-TRANSMISSION.scn:9-10, 102-103;
night PPV id 6039035132485441499, find the day scene's PPV id in B16_Bistro_Street.desce).
