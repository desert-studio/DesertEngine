# NG-RT-REFRACTION2-C remainder (run 3 closed at the context limit)

Done in run 3 (all pushed): (d) Scalability.cpp:427-431 reader string, Scalability.hpp:334-336 placeholder gone;
(a) FrameFrontLayer.Normal (SceneRendererImpl.hpp:1118-1126, SceneRendererFrameDeferred.cpp:~2810);
(b) MakeHitLitReads (SceneRendererFrameDeferred.cpp:2363-2430, decl SceneRendererImpl.hpp ~1305), hit group table
once per frame (BuildReflectionHitGroups :2279 stamp m_ReflectionHitGroupsFrame, SceneRendererImpl.hpp ~1822);
(c) AddFrameRayTracedRefraction (SceneRendererFrameMesh.cpp ~1113-1190, decl SceneRendererImpl.hpp ~1129),
call SceneRenderer.cpp:3248, members m_RayTracedTranslucency / m_ReportedRayTracedRefraction (SceneRendererImpl.hpp ~1960).

## NEXT
1. Compiler re-read of SceneRendererFrameMesh.cpp AddFrameRayTracedRefraction (~1113-1190): ViewFrame field names
   (InvJitteredViewProjection, CameraPosition, PreExposure, FrameIndex - see SceneRenderer.hpp struct ViewFrame);
   designated-initializer order of ShaderPush::RayTracedTranslucencyPush (glslh order) and of
   RayTracedTranslucencyInputs (contract); CapabilityCatalog / RayTracingMode found by `using namespace
   Common::Scalability` (as SceneRendererFrameDeferred.cpp:2274); wrap the 3 lines over 120 columns.
2. Step 7: Scenarios/Render/NG-RT-REFRACTION.scn frame names day_pt..rt_reset match the brief; pass name
   "Translucency: Ray Traced Refraction", quality row "Translucency.Refraction".
3. `python scripts/Dev/arch_census.py`; `git rm -f REMAINDER.md`; commit «wip: NG-RT-REFRACTION2-C код готов»; push;
   reviewer (brief «## Цепочка»), fixes, `studio.py accept NG-RT-REFRACTION2-C done`.
4. Report NEEDS (B): Mesh/Surface/TranslucencyRefraction.glslh:19 bound-check the texel against
   textureSize(TranslucencyRefractionTexture) - the 1x1 TransparentBlack is read out of bounds by texelFetch.
   Report note: the TLAS is built only when RayTracingSceneRequired(m_RenderMethods) - HardwareTraced refraction
   with no other hardware method is refused (logged), by design step 3.
