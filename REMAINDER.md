# NG-RT-REFRACTION-A remainder

Blocked by CONTRACT-BREAK on `Desert/Desert/Source/Engine/Graphic/Systems/Scene/Translucency/RayTracedTranslucency.hpp`:
a ray tracing pipeline over the scene's hit groups (design step 1, model `SSRRenderer.hpp:382` EnsureHitLightingPipeline,
`SSRRenderer.hpp:926` RecordHitLighting) cannot be built or launched through the frozen interface:
1. `Init()` has no hit group templates; the pipeline's hit groups are the frame's `RayTracingHitGroupTable::Templates`
   (`SceneRendererFrameDeferred.cpp:2327`, they change per frame) -> needs `EnsurePipeline(const std::vector<std::string>& templates)` per frame.
2. `RayTracedTranslucencyInputs` lacks `const RayTracing::RayTracingHitGroupTable* HitGroups` (`Renderer::TraceRays` takes it)
   and `RayTracing::RayTracingSceneReadsInputs SceneReads` (the hit groups' table + rain reads, `DESERT_RAY_TRACING_SCENE_READ_PARAMETERS`).
3. Lighting of the opaque hit is the ray generation's (`Common/RayTracingPayload.glslh:4-6`, `Common/GIHitLighting.glslh`):
   needs the GI frame, point/spot lights, light grid, env specular, shadow reads (as `SSRRenderer.hpp:220` TracedReads).
4. No shader parameter struct (slot names) shared by A's bindings and B's program.

Left (after the contract is fixed):
- `RayTracedTranslucency.cpp` (new): Impl, ctor/dtor, pipeline, AddPass, static_assert enum count == `kTranslucencyRefractionMethodNames.size()` (`Scalability.hpp:624`).
- `SceneRendererImpl.hpp`: member `System::RayTracedTranslucencyRenderer m_RayTracedTranslucency`, refusal-logged flag.
- `SceneRenderer.cpp:3216` (after AddFrameSceneCopy): HardwareTraced + front layer -> call in `SceneRendererFrameMesh.cpp`, result in `frontLayer.Refraction` (field ready: `SceneRendererImpl.hpp:1118`).
- `Scalability.cpp:424`: the row's Reader (now `std::nullopt`, placeholder) names that call.

Done: row `Translucency.Refraction` (placeholder), `FrameFrontLayer::Refraction`, glass binding black fallback (`SceneRendererFrameMesh.cpp:1129`).
