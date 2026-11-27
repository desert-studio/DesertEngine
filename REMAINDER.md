# NG-RT-REFRACTION-A remainder (run 2, closed at the context limit)

The contract fix 4ee2ea9fd (task/NG-RT-REFRACTION) is MERGED into this branch: the header is now
`EnsurePipeline(templates)` + `AddPass(graph, RayTracedTranslucencyInputs, ShaderPush::RayTracedTranslucencyPush)`,
no `Init()`, no `RayTracedTranslucencyParams`, the pass does not read the scene copy.

Done (run 1): row `Translucency.Refraction` (Scalability.hpp:358, names :624; Scalability.cpp:424 Reader is still
`std::nullopt` - replace with the reader chain string once the call exists), `FrameFrontLayer::Refraction`
(SceneRendererImpl.hpp:1118), glass binding black fallback (SceneRendererFrameMesh.cpp:1129).

Left, in order:
1. `RayTracedTranslucency.cpp` (new, Desert/Desert/Source/Engine/Graphic/Systems/Scene/Translucency/):
   - static_asserts the header asks for (size 2, names[ScreenSpace]=="ScreenSpace", names[HardwareTraced]=="HardwareTraced").
   - `Variant(reorder)` = `GlobalIllumination::TracerVariant(kRayTracedTranslucencyTracer)` + `kRayTracingReorderDefine`
     when reorder (model SSRRenderer.hpp:271). `ProgramVariants()` = both reorder values under
     `kRayTracedTranslucencyProgram`.
   - `Impl`: shared_ptr<Shader> program, `ShaderBindingLayoutCache` layout, shared_ptr<RayTracingPipeline>,
     templates, variant hash. `EnsurePipeline` = copy of the shape of SSRRenderer.hpp:382-423 (AcquireShaderVariant
     of kRayTracedTranslucencyProgram, one hit group per "<T>/RayTracing.Hit", RayTracingPipeline::Create,
     publish with std::exchange + RayTracingPipeline::Retire).
   - `AddPass`: validate (HitGroups non-null, SceneReads.IsValid(), FrontLayer refs, Camera, Width/Height,
     `GlobalIllumination::TracerFrameMissing(in.HitLighting.Tracer)`), create rgba16f output (scene colour format,
     ViewTargetFormats), `graph.AddPass(kRayTracedTranslucencyPass, Compute)`; bindings block from
     `layout.Get(program)` with `RDG::NoOtherRouteFill()` (model SSRRenderer.hpp:740-771): RefractionOutputTexture
     StorageWrite, CameraUB, `RayTracing::BindRayTracingSceneReads<RayTracedTranslucencyShaderParameters>`,
     front layer reads, traced reads, `.PushConstants<ShaderPush::RayTracedTranslucencyPush>()`; exec captures the
     pipeline shared_ptr + `*in.HitLighting.HitGroups` pointer, `RDG::PassBindings bindings(context,
     context.GetBindingBlock(0)); bindings.PushConstants(push); Renderer::TraceRays(bindings, *pipeline, hits, W, H)`
     (model SSRRenderer.hpp:928-944, the exec lambda SceneRendererFrameDeferred.cpp:2691).
   - NEEDS: `SSRRenderer.hpp` - `DeclareTracedReads` (:1047) and `DeclareFrontLayerReads` (:1033) are PRIVATE (the
     `public:` is at :1084); the contract names them as the binding statement. Move `public:` above :1030. Do not
     copy their bodies (a second binding list).
2. `SceneRendererImpl.hpp`: member `System::RayTracedTranslucencyRenderer m_RayTracedTranslucency`, a
   refusal-reported flag, and a method `RDG::TextureRef AddFrameRayTracedRefraction(graph, textures, gbuffer,
   frontLayer, frame)` (body in SceneRendererFrameMesh.cpp; SceneRenderer.cpp is 5955 lines, growth < 100).
3. Its body: when `QualityState::Resolved().As<int>(Parameter::TranslucencyRefraction) == HardwareTraced`, the
   front layer exists (FrameFrontLayer from AddFrameTranslucentFrontLayer - only when
   TranslucentFrontLayerReflections()), `m_RayTracingScene.GetFrame().Built`, the device offers the RT pipeline
   (as ReflectionsHitLit): `BuildReflectionHitGroups()` (SceneRendererFrameDeferred.cpp:2264; already built this
   frame when reflections are hit-lit - avoid the second build), `EnsurePipeline(m_ReflectionHitGroups.Templates)`,
   TracedReads for kRayTracedTranslucencyTracer. The producer of TracedReads is
   `MakeTracedReflectionReads` (SceneRendererFrameDeferred.cpp:2344-2484), coupled to an SSRRenderer
   (EnsureTracedProgram at :2375) and to ReflectionsTracer(): split its read-filling half (:2398-2484) into a
   tracer-parameterised `MakeHitLitReads(graph, textures, tracer, gbuffer, frame)` used by both - that is
   SceneRendererFrameDeferred.cpp, NOT this package's file -> NEEDS for the lead, or a package that owns it.
   Any refusal: LOG_ERROR once, the view stays ScreenSpace.
4. `SceneRenderer.cpp:3214` (after AddFrameSceneCopy, before AddFrameWater): `frontLayer` is `const` at :3141 -
   set `.Refraction` through a non-const copy, then AddFrameGlass reads it (SceneRendererFrameMesh.cpp:1129 done).
5. Scalability.cpp:424 Reader string; Scalability default per tier (0 everywhere = ScreenSpace; Epic/Cinematic
   HardwareTraced if the design's ~1 ms holds - D12 unmeasured, keep 0 until the profile-dump).
6. Package cook: `RayTracedTranslucencyRenderer::ProgramVariants()` must be listed where
   `SSRRenderer::ProgramVariants()` is (PackageCook CookShaderVariants) - find with `scripts/Dev/sym.sh --refs ProgramVariants`.
