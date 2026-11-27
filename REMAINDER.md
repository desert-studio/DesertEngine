# NG-RT-REFRACTION2-C remainder (run 1 closed at the context limit; no code edit yet)

Done: step 0 of the design's «### Пакет C» - merged origin/task/NG-RT-REFRACTION-A and -C, `git rm REMAINDER.md`
of A; contract header taken from the lead's f1e755b33 (RayTracedTranslucencyInputs::OpaqueDepth, slot
GBufferDepthTexture) - checkout of that one file, not an edit.

## Findings (use them, do not re-read)
- `SceneRendererFrameDeferred.cpp:2359-2498` MakeTracedReflectionReads; `:2279` BuildReflectionHitGroups;
  `:2167` path tracer's call; `:2255` ReflectionsTracer (Hits = InPlace iff ReflectionsHitLit()).
- Frame stamp pattern already in the file (`:2020-2031` GatherFrameEmissiveScene):
  `FrameManager::GetInstance().GetAbsoluteFrameCount()`, member `uint64_t m_...Frame = ~0ull`. Use it for
  `m_ReflectionHitGroupsFrame` (SceneRendererImpl.hpp:1810), no signature change of BuildReflectionHitGroups.
- Split plan: shared `MakeHitLitReads(graph, textures, tracer, gbuffer, frame)` -> `Common::ResultStr<TracedReads>`
  (error is a value; each caller logs with its own prefix) = gbuffer size + world grid checks (:2372-2375),
  Tracer/SceneReads/TracerFrameMissing (:2412-2417), GIFrame (:2429-2441), radiance cache (:2445-2460), lights,
  EnvSpecular, G-buffer (:2461-2469), HitGroups = `tracer.Hits == InPlace ? &m_ReflectionHitGroups : nullptr`
  (refuse when InPlace and the table's stamp is not this frame). SSR keeps: tracer/warn/EnsureTracedProgram,
  TLAS check + Build + EnsureHitLightingPipeline (:2376-2410), surface cache (:2418-2427), cascades (:2476-2496).
- SceneRendererImpl.hpp anchors: include `:137`, FrameFrontLayer `:1118`, AddFrameGlass decl `:1127`,
  BuildReflectionHitGroups decl `:1283`, MakeTracedReflectionReads decl `:1300`, m_ReflectionHitGroups `:1810`,
  m_ReportedHitLightingDevice `:1828`, m_RayTracedSunShadow `:1955`.
- `SceneRendererFrameMesh.cpp:1112` AddFrameGlass (fallback `.Refraction ... : black` at `:1127-1128`).
- `SceneRenderer.cpp:3163` `const FrameFrontLayer frontLayer`, `:3241` AddFrameSceneCopy, `:3247` Water, `:3248` Glass.
- ViewFrame (View/ViewFrame.hpp:207): FrameIndex, InvJitteredViewProjection, CameraPosition, PreExposure.
- Push block (Common/RayTracedTranslucencyPush.glslh): InvJitteredViewProjection, CameraPosition (w = PreExposure),
  MaxRayDistance, MaxRoughness, MaxInterfaces, FrameIndex.

## BLOCKED: step 4 (System.TransparentBlack)
`API/Vulkan/VulkanFallbackTextures.cpp:81-84`: GetFallbackTexture2D is WHITE (1,1,1,1), not zero; RDG has no
texture upload/clear. NEEDS: `Graphic/FallbackTextures.hpp` + `API/Vulkan/VulkanFallbackTextures.cpp` -
`GetTransparentBlackTexture2D()` (1x1 RGBA16F, rgba = 0, as GetBlackTexture3D :227-237). Until then the glass's
ScreenSpace fallback stays System.Black (a = 1: black refraction where a front layer exists).
NEEDS (B): `Mesh/Surface/TranslucencyRefraction.glslh:19` also bound-check the texel against
textureSize(TranslucencyRefractionTexture) - a 1x1 fallback is read out of bounds by texelFetch.

## NEXT (in order)
1. Steps 1-3, 5, 6 of «### Пакет C» (F:/aftmp/NG-RT-REFRACTION2-DESIGN.md:91-104) with the plan above;
   AddFrameRayTracedRefraction fills `OpaqueDepth` = the view's opaque G-buffer depth.
2. Scalability.cpp `:423-425` reader string; `Scalability.hpp:334-337` drop "A placeholder ..." sentence.
3. Step 7 scenario check, `python scripts/Dev/arch_census.py`, `git rm REMAINDER.md`, commit, push, chain.
