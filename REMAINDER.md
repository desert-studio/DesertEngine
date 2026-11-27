# NG-RT-REFRACTION-B - remainder (context limit hit, run 2)

Done (task/NG-RT-REFRACTION-B):
- Mesh/Surface/TranslucencyRefraction.glslh, Mesh/Surface/Pass_Forward_Translucent.glslh (run 1): screen tier on Snell with depth check, traced read (D1, D3, D4 screen part).
- Programs/Translucency/RayTracedTranslucency.shader (run 2): raygen per front-layer pixel, Snell + TIR + Beer + Fresnel per interface after the first, MaxInterfaces, GGX VNDF rough normal, opaque hit GIHitRadianceLit, miss sky, budget spent -> SceneCopy at last interface (D2, D5-D9 traced tier).
- Common/GIHitLitReads.glslh (new, nobody owns): the hit-lit reads, same names/numbers as SSRTrace.glslh's traced block.

Left for a fresh agent:
1. Re-read RayTracedTranslucency.shader once as the compiler would (includes order: GIHitLitReads needs LinearClampSampler, PointClampSampler, GBufferDepthTexture before it; hash12/SampleGGXVNDF from Common/MonteCarlo.glslh; cameraUB.Projection / JitteredViewProjection names).
2. python scripts/Dev/arch_census.py; reviewer chain per brief; studio.py accept.
3. Report lines:
   - CONTRACT-BREAK: RayTracedTranslucency.hpp RayTracedTranslucencyInputs - needs the hit-lit reads (SSRRenderer::TracedReads: GIFrame, lights, light grid, radiance cache, EnvSpecular, GBufferA/Emissive/ShadingWord, tracer), the opaque GBufferDepth, CameraUB and RayTracingSceneReads; the shader binds them by the names in Common/GIHitLitReads.glslh and RayTracedTranslucency.shader:20-30.
   - NEEDS: Common/SSRTrace.glslh:103-165 - replace its traced block (minus cascades) with #include <Common/GIHitLitReads.glslh> (one copy).
   - Program binding names for package A: FrontLayerNormalTexture(1), GBufferDepthTexture(2), SceneCopyTexture(3), TranslucencyRefractionTexture(4, rgba16f storage), FrontLayerDepthTexture(5), push constant block RayTracedTranslucencyParams; pass "Refraction".
