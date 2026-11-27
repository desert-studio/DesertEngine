# NG-RT-REFRACTION2-C remainder (run 2 closed at the context limit)

Done in run 2: merged origin/task/NG-RT-REFRACTION2-A (cd2615bd2). Step 4 (System.TransparentBlack) DONE:
`FallbackTextures.hpp` GetTransparentBlackTexture2D (1x1 RGBA8 zero; lead granted the Vulkan pair),
`API/Vulkan/VulkanFallbackTextures.{hpp,cpp}` create/release, `RDG/RDGSystemTextures.hpp` TransparentBlack,
`SceneRendererFrame.hpp` ImportSystemTextures registers "System.TransparentBlack",
`SceneRendererFrameMesh.cpp` AddFrameGlass `.Refraction` falls back to System.TransparentBlack.

## Findings (use them, do not re-read)
- ResultStr: `Common::MakeError<T>( std::string )`, `Common::MakeFormattedError<T>( "fmt {}", x )`,
  `Common::MakeSuccess( v )`, `BOOLSUCCESS`. Frame stamp: `::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount()`
  (pattern SceneRendererFrameDeferred.cpp:2025-2031, member `m_EmissiveGatherFrame` SceneRendererImpl.hpp:1823).
- Split of MakeTracedReflectionReads (SceneRendererFrameDeferred.cpp:2359-2498) as planned in run 1:
  MakeHitLitReads(graph, textures, const GlobalIllumination::Pipeline& tracer, gbuffer, frame)
  -> Common::ResultStr<System::SSRRenderer::TracedReads> = :2372-2375 checks, :2412-2417 (Tracer/SceneReads/
  TracerFrameMissing), :2429-2469 (GIFrame, radiance cache none, lights, EnvSpecular, G-buffer),
  HitGroups = tracer.Hits == InPlace ? &m_ReflectionHitGroups : nullptr (error if InPlace and
  m_ReflectionHitGroupsFrame != this frame). SSR part keeps :2376-2410, surface cache :2418-2427 (set after the
  call), cascades :2471-2496; its `refuse` lambda logs the error string.
  BuildReflectionHitGroups (:2279): early `return BOOLSUCCESS` when m_ReflectionHitGroupsFrame == frame; stamp
  after `m_ReflectionHitGroups = ...` (:2342).
- FRONT LAYER NORMAL: FrameFrontLayer (SceneRendererImpl.hpp:1118) has no Normal; RayTracedTranslucencyInputs
  needs FrontLayer{Normal, Depth}. Add `RDG::TextureRef Normal;` to FrameFrontLayer and fill
  `.Normal = surface.Normal` in AddFrameTranslucentFrontLayer (SceneRendererFrameDeferred.cpp:~2788 return).
- AddFrameRayTracedRefraction (SceneRendererFrameMesh.cpp, before AddFrameGlass ~:1112): CameraUB as at
  SceneRendererFrameDeferred.cpp:2693-2697 (ShaderProtocols::MakeCameraUB, include ShaderProtocols/Camera.hpp);
  OpaqueDepth = `textures.Depth( m_GBuffer, "GBuffer" )` (as :2806); Width/Height from GetTextureDesc(gbuffer[0])
  or the front layer depth. Push `ShaderPush::RayTracedTranslucencyPush{ InvJitteredViewProjection,
  CameraPosition = vec4(frame.CameraPosition, frame.PreExposure), MaxRayDistance, MaxRoughness, MaxInterfaces,
  FrameIndex = uint32_t(frame.FrameIndex) }` (field names per RayTracedTranslucencyPush.glslh). Refusals: quality
  Reflections.TranslucentFrontLayer off (TranslucentFrontLayerReflections()) / TLAS not built / no RT pipeline
  (CapabilityCatalog::Offers(QualityState::Catalog().RayTracingModes, RayTracingMode::RayTracingPipeline), see
  :2275) -> LOG_ERROR once (flag m_ReportedRayTracedRefraction near m_RayTracedSunShadow :1955). Invalid
  frontLayer.Depth (nothing visible) -> invalid ref, no log. Needs `gbuffer` + `frame` parameters (the
  SceneRenderer.cpp call site has both).
- SceneRenderer.cpp:3163 `const FrameFrontLayer frontLayer` -> non-const; after AddFrameSceneCopy (:3241-3244),
  before AddFrameWater (:3247): `frontLayer.Refraction = AddFrameRayTracedRefraction( graph, textures, gbuffer,
  frontLayer, frame );`.

## NEXT (in order)
1. Steps 1-3, 5 above (design F:/aftmp/NG-RT-REFRACTION2-DESIGN.md «### Пакет C» steps 1-3, 5).
2. Scalability.cpp `:423-425` reader string; Scalability.hpp `:334-337` drop the "placeholder" sentence.
3. Step 7 scenario check, `python scripts/Dev/arch_census.py`, `git rm -f REMAINDER.md`, commit
   «wip: NG-RT-REFRACTION2-C код готов», push, chain (reviewer, fixes, accept).
4. Report NEEDS (B): `Mesh/Surface/TranslucencyRefraction.glslh:19` bound-check the texel against
   textureSize(TranslucencyRefractionTexture) - the 1x1 TransparentBlack is read out of bounds by texelFetch.
