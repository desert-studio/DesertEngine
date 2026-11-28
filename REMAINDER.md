# GI-TEMPORAL2-D — remainder (context limit hit before any code; nothing in the package files changed)

Brief: C:/Users/danil/.claude/studio/briefs/GI-TEMPORAL2-D-BRIEF.md. Design: S:/aftmp/GI-TEMPORAL2-DESIGN.md «### Пакет D».
Contract read: Common/EmissiveLightsSampling.glslh (EmissiveGridCell / EmissiveGridPick / EmissiveGridPickPdf /
EmissiveLightsRadiance / EmissiveLightsSamplePoint / EmissiveLightsNormal / EmissiveLightsTotal),
Systems/RayTracing/EmissiveLightsFrame.hpp (Valid, Static/Dynamic parts, Lookup, Grid, StaticParams/DynamicParams).

## Facts found (file:line)
- Block names to declare / bind (the surface cache's, SurfaceCacheRenderer.cpp:132-140, 1024-1033):
  EmissiveSegmentsUB, EmissiveLookupUB, EmissiveTrianglesUB, EmissivePowerUB, EmissiveDynamicSegmentsUB,
  EmissiveDynamicTrianglesUB, EmissiveDynamicPowerUB, plus EmissiveGridUB (contract line 28). Members:
  emissiveSegments, emissiveLookup, emissiveTriangles, emissivePower, emissiveDynamic*, emissiveGrid
  (declaration pattern: Common/MegaLights.glslh:58-87, own block names there).
- RadianceCacheTrace.shader bindings used: 0-5 (RadianceCacheState.glslh), 6-10, 16-23, 24-30 surface cache,
  32-42 tracer, 43 voxel lighting; samplers unnumbered. FREE: 11-15, 31, 44+. Proposal: 44..51 the eight
  emissive buffers, 31 `SurfaceCacheEmissiveTexture` (texture2D + `#define ... sampler2D(..., PointClampSampler)`,
  as Common/SurfaceCacheRadiosityTraceTexel.glslh:81-83). `#define GI_HIT_EXCLUDES_EMISSION` and
  `#define EMISSIVE_LIGHTS_GRID` before `#include <Common/GIHitLighting.glslh>` (RadianceCacheTrace.shader:76-81).
- GI_HIT_EXCLUDES_EMISSION: GIHitLighting.glslh:98 (screen hit) and SurfaceCacheSampling.glslh:186-190 (cards,
  needs SurfaceCacheEmissiveTexture; SurfaceCacheSampling is included only when !DESERT_GI_HIT_LIGHTING_IN_PLACE,
  GIHitLighting.glslh:50) -> bind the layer only when `readsCards` (WorldRadianceCache.cpp:521-522), like
  DeclareSurfaceCacheReads at :598.
- SurfaceCacheFrame (GITracing.hpp:302-311, not ours) has NO Emissive layer: add
  `RDG::TextureRef SurfaceCacheEmissive;` to RadianceCacheUpdateInputs (WorldRadianceCache.hpp:79-112) beside
  `RayTracing::EmissiveLightsFrame Emissive;` (#include <Engine/Graphic/Systems/RayTracing/EmissiveLightsFrame.hpp>),
  both filled in SceneRendererFrameDeferred.cpp:~715 in the existing initializer line (file 2897 lines, must not
  grow). Find there where the surface cache layer[4] (SurfaceCache.Emissive, SurfaceCacheRenderer.cpp:118) and the
  scene emissive list frame (the one SurfaceCache DirectLighting gets as inputs.Emissive) are named.
- Refusal: in AddUpdatePasses (WorldRadianceCache.cpp:327-339 block) add
  `if ( !inputs.Emissive.Valid ) return MakeError("radiance cache: the probe rays leave emission to light sampling
  (GI_HIT_EXCLUDES_EMISSION), and the frame built no emissive list (AddFrameEmissiveLights)")`, and for readsCards
  an invalid SurfaceCacheEmissive likewise.
- Trace pass binding: WorldRadianceCache.cpp:594-623 (`if ( pass == Trace )`); add the eight Storage reads (kR) and
  the layer Texture (kS) — names into WorldRadianceCacheShaderParameters (:28-54).
- PARAMS PROBLEM: the push block is RadianceCachePush in Common/RadianceCacheState.glslh:205-219 (NOT a package
  file, shared by all five passes) -> StaticParams/DynamicParams cannot go in the push. Options: (a) NEEDS to the
  lead: two `uvec4 EmissiveCounts, EmissiveDynamicCounts` in RadianceCachePush (then push.EmissiveCounts =
  inputs.Emissive.StaticParams() where the push is filled in WorldRadianceCache.cpp); (b) a graph-made storage
  buffer "RadianceCacheEmissiveParamsUB" of two uvec4 uploaded per frame (look how update.GIFrame / MarkPoints
  are made in WorldRadianceCache.cpp). (b) stays inside the package; prefer it if the RDG has an upload.

## Shader plan (RadianceCacheTrace.shader:96-183)
- Before the `if ( t.x < ... )` at :133 (all 64 threads reach it; the early return at :100 is group-uniform):
  thread i = gl_LocalInvocationIndex computes candidate i of the probe: hash(probe, FrameIndex, i) -> u0,u1,u2;
  if EmissiveLightsTotal(s,d) > 0: cell = EmissiveGridCell(position); tri = EmissiveGridPick(cell,u0,s,d);
  pdf = EmissiveGridPickPdf(cell,tri,s,d); T = EmissiveLightsTriangle(tri,s); x = EmissiveLightsSamplePoint(T,u12);
  dir, dist; cosL = dot(EmissiveLightsNormal(T), -dir); w = EmissiveLightsRadiance(tri,T,s) * |cosL| * T.P0Area.w
  / (dist^2 * pdf * 64) * J * traceRes^2 / 4, J = 1/(|dir.x|+|dir.y|+|dir.z|)^3 (oct map dA/dω,
  RadianceCacheOctEncode :149; the texel's solid angle at that direction, consistent with the jittered ray's
  expectation); texel = min(ivec2((RadianceCacheOctEncode(dir)*0.5+0.5)*traceRes), traceRes-1).
  shared vec4 dirDist[64], shared vec4 contribTexel[64] (texel packed in w); barrier().
- Inside the if, after `radiance` (:150): loop 64, candidate texel == t -> visibility by the GI tracer to
  dist*0.99 (find the visibility/shadow trace in Common/GITracer.glslh / GITracing.glslh, e.g. TracedShadow as the
  surface cache's emissive uses) -> radiance += contrib. traced = vec4(radiance, hitDistance) then the history
  blend unchanged.

## NEXT
1. Resolve PARAMS (b) or report NEEDS (a). 2. hpp fields + refusal. 3. cpp bindings. 4. SceneRendererFrameDeferred
one-line fill. 5. shader. 6. arch_census, commit «wip: GI-TEMPORAL2-D код готов», push, reviewer, accept.
