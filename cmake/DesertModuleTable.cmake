# THE ENGINE'S MODULE TABLE (ENG-MODULES plan v3, P0 MODULE-INFRA): the one place that says which modules the engine
# is made of, which modules each one may include, and which module every engine file belongs to.
#
# One text, three readers: the build (cmake/DesertModules.cmake builds one static library per module and the
# include paths from these rows), DesertHeaderTool (one RegisterReflection_<Module>() per module) and the
# ModuleBoundary suite (every #include of the engine checked against the dependency closure). The two C++ readers
# read <build>/DesertModules.json, which configure writes from these rows together with the module CMake placed
# every legacy file into (Tools/DesertHeaderTool/Source/ModuleTable.hpp), so nothing restates the table.
#
# Modules: in layer order (a module only depends on modules above it in this list; configure fails otherwise).
# DEPS are DIRECT dependencies, like UE's PublicDependencyModuleNames; the transitive closure is allowed. FOLDER is
# the module's home, Desert/<Module>/Source/<Module> when omitted (the UI-FW-4 layout); a file inside a module's
# FOLDER belongs to that module. REFLECTED: the module holds reflected types (REFLECT()/PROPERTY()/FUNCTION()).
# Everything the build derives from a FOLDER (its source root on every engine include path, the header tool's
# --check / --context roots and, for a reflected module, its --reflect-root) comes from these rows, so a module
# whose FOLDER appears needs nothing but its row here. UE names the Unreal module it corresponds to.
#
# Placement: where a file still living in the old single library (DESERT_LEGACY_ROOT) belongs. Ordered, FIRST
# MATCH WINS; each pattern is an ECMAScript regular expression searched in the path relative to the legacy root
# (e.g. "Engine/Graphic/RDG/RenderGraph.hpp"). CMake rewrites the two classes its regex lacks (\w, \d); the C++
# readers run std::regex on the text as written. The last row catches everything, so no file is unplaced. A
# module's rows are deleted in the same commit that moves its files into its FOLDER.

# L0
desert_module(Common UE "Core" FOLDER "Desert/Common/Source/Common")
# L1
desert_module(Render2DCore DEPS Common UE "SlateCore")
desert_module(CoreReflection DEPS Common UE "CoreUObject (reflection)")
desert_module(Projects DEPS Common UE "Projects")
desert_module(ImageCore DEPS Common UE "ImageCore")
desert_module(PhysicsCore DEPS Common UE "PhysicsCore")
desert_module(Audio DEPS Common UE "AudioMixer")
desert_module(ApplicationCore DEPS Common UE "ApplicationCore")
desert_module(GeometryCore DEPS Common UE "GeometryCore")
# L2
desert_module(TextCore DEPS Render2DCore ImageCore UE "SlateFontRasterization")
desert_module(AssetCore DEPS CoreReflection UE "CoreUObject (packages) + AssetRegistry")
desert_module(DesertUI DEPS Render2DCore CoreReflection UE "UMG/Slate" FOLDER "Desert/UI/Source/UI" REFLECTED)
desert_module(AnimationCore DEPS GeometryCore CoreReflection UE "AnimationCore")
# L3
desert_module(Localization DEPS AssetCore UE "Internationalization")
desert_module(ShaderCompiler DEPS ImageCore AssetCore UE "ShaderCore")
# the render-free runtime core (NET-MESH-0, NET-ARCH.md sec. 6): the cooked world and its World Partition
# residency, scene/prefab serialization, the reflected-component table and the Luau runtime.
# Engine links it and the dedicated server (Server/) links ONLY it and its closure; a configure check
# (cmake/DesertModules.cmake, "RENDER-FREE MODULES") fails if any of its files reaches Graphic/, RHI, a window,
# Audio or ImGui through its includes.
desert_module(Core DEPS Projects AssetCore GeometryCore PhysicsCore UE "Engine (the part a dedicated server compiles)" RENDER_FREE)
# the landscape's render-free runtime data (BUILD-MOD-6L): the heightfield and weightmap tiles, their files, layout
# and frame, the collision heightfield, the edit-layer stack, the splines and the World Partition descriptor of a tile,
# a root and a spline (LandscapeWorldPartition.hpp) - what the dedicated server loads to stand pawns on the terrain.
# ABOVE Core, as UE's Landscape module is above Engine: World Partition's rules never name the landscape, the module
# registers its descriptor hook at StartupModule (UE FLandscapeActorDesc), and the server names the module itself.
desert_module(LandscapeCore DEPS Core UE "Landscape (the heightfield data a dedicated server loads)" FOLDER "Desert/Landscape/Source/LandscapeCore" RENDER_FREE)
# L4: RHI + VulkanRHI + RDG as one module (one backend, decision 09-27; plan sec. 3.1)
desert_module(RenderCore DEPS ShaderCompiler ImageCore ApplicationCore Projects AssetCore GeometryCore UE "RHI + VulkanRHI + RenderCore")
# L5
desert_module(Engine DEPS Core LandscapeCore RenderCore Render2DCore Localization PhysicsCore AnimationCore GeometryCore DesertUI TextCore Audio UE "Engine" REFLECTED)
# L6: the renderer sits ABOVE Engine (UE layering, plan sec. 3.2); features are plugins over Engine
desert_module(Renderer DEPS Engine UE "Renderer")
desert_module(Media DEPS Engine Audio UE "MediaAssets")
desert_module(Landscape DEPS Engine UE "Landscape" REFLECTED)
desert_module(Destruction DEPS Engine UE "GeometryCollectionEngine" REFLECTED)
# Water privately needs the Renderer as UE's Water plugin does: its single layer water pass implements the
# Renderer's ISingleLayerWaterPass (Graphic/Systems/Scene/SingleLayerWaterPass.hpp), and its bodies stand on the landscape.
desert_module(Water DEPS Engine Landscape Renderer UE "Water" REFLECTED)
# VFX privately needs the Renderer as Niagara does: its particle renderer implements the Renderer's ISceneFXSystem
# (UE FFXSystemInterface, Graphic/Systems/Scene/SceneFXSystem.hpp).
desert_module(VFX DEPS Engine Renderer UE "Niagara" REFLECTED)
desert_module(Hair DEPS Engine UE "HairStrands")
# L7
desert_module(Foliage DEPS Landscape UE "Foliage" REFLECTED)
# L8
# the language-free script host: Engine's public reflected layer only (FUNCTION/EVENT), no feature module
desert_module(Scripting DEPS Engine UE "ScriptPlugin")
# L9: a script language is a plugin over Scripting (Luau: the VM, the reflection binder, the host natives)
desert_module(ScriptLuau DEPS Scripting UE "ScriptPlugin")

# Core first: its files sit in directories whose catch-all rows below place them in Engine, Foliage or
# ScriptLuau. A file joins Core by name here (and the RENDER_FREE check then holds it to that).
desert_placement(Core [[Engine/Gameplay/|Engine/Core/Serialize/(ComponentRegistry|WorldPartitionRules|CookedWorld|WorldCells|WorldCellLoader|WorldPartitionResidencyExecutor|SceneDependencies|SceneFormat|PrefabExpansion|EntityDescriptorIndex|EntityReferenceRemap|ExternalEntities|ForeignKeys)\.|Engine/Assets/Prefab/(PackedPrefab|PrefabFormat|PrefabOverrides)\.|Engine/Assets/Serialization/FoliageType\.|Engine/Scripting/ScriptSchema\.|Engine/Scripting/Luau/(LuauScriptSchema|LuauScriptSource|LuauRuntime|LuauBinder|LuauDebugger|LuauProperties)\.|Engine/ECS/ReflectedComponents\.]])
# UI-FW-4 moved Render2DCore and DesertUI into their Folders (and CoreReflection's macros into its own);
# what stays behind is the UMG-side adapter of the engine, Engine/UI/Ecs/, which the catch-all also places.
desert_placement(Engine [[Engine/UI/Ecs/]])
# leaves
# The replication layout DesertHeaderTool emits (Engine/Reflection/ReplicatedFields.hpp): names only, so the
# dedicated server, which links no component header, knows which fields it may replicate.
desert_placement(CoreReflection [[Engine/Reflection/|Engine/Generated/ReplicatedFields\.gen\.cpp$]])
desert_placement(ImageCore [[Engine/Core/(Formats|IO)/]])
desert_placement(ApplicationCore [[Engine/Core/(Window|WindowCloseGate|Input|Glfw|GlfwVulkan)\.|^Platform/]])
# the asset framework (CoreUObject package loading + AssetRegistry)
desert_placement(AssetCore [[Engine/Assets/(AssetBase|AssetManager|AssetRef|AssetRefSerialization|AssetGuidRef|AssetRootPin|AssetRootSet|AssetEvents|AssetMetadata|AsyncAssetLoader|ContentRegistry|ContentRegistryCook|ContentGate|ContentWork|ContentDirectoryWatch|RegistryDiscovery|TextAssetHeader\w*|ContainerBytes|FrameRetireQueue|SyncLoadLedger|ItemProgress|EvictionDeadline|AssetEviction|AssetEvictionServices|DDCShipRegister|Common)\.]])
desert_placement(Localization [[Engine/Localization/|Engine/Assets/StringTable(Asset|Source)\.]])
# geometry / animation runtime cores
desert_placement(Engine [[Engine/Geometry/(Mesh|StaticMesh|SkinnedMesh|MeshFactory|PrimitiveMeshFactory|ProceduralCharacter\w*|AutoRig|MeshAssetArrays|DynamicMeshAsset|DynamicMeshRenderConversion|RenderMeshData|LODSelection|PosedBounds)\.]])
desert_placement(GeometryCore [[Engine/Geometry/]])
# Timeline/Hosts maps the UI's easing onto the Timeline's: the UI-side adapter, so Engine's (DesertUI is not in
# AnimationCore's closure; MODULE BOUNDARY)
desert_placement(Engine [[Engine/Animation/(AnimationLibrary\.|Graph/AnimGraphSerialization\.|Timeline/SequenceFormat\.|Timeline/Hosts\.|Modular/)]])
desert_placement(AnimationCore [[Engine/Animation/]])
# RHI + RDG
desert_placement(ShaderCompiler [[Engine/Core/ShaderCompiler/]])
desert_placement(RenderCore [[Engine/Graphic/API/|Engine/Graphic/RDG/|Engine/ShaderResources/|Engine/Core/(Device|GpuBenchmark|FrameManager|RenderTask).]])
desert_placement(RenderCore [[Engine/Graphic/(Image|ImageFactory|ImageTraits|IndexBuffer|VertexBuffer|Framebuffer|RenderPass|RenderPassDeclaration|ExternalRenderPass|Pipeline|PipelineCache|PipelineCacheFile|PipelineBuilds|Shader|SwapChain|SwapchainAcquire|GpuBatch|ComputeImages|MipMapGenerator|DeviceLost|DrawCounters|MemoryReadout|MemoryReadoutSource|MappedMemory|AllocationLedger|ResourceLedger|RendererAPI|RendererTypes|Texture|TextureFactory|FallbackTextures|DefaultTextures|PixelPack|GpuTimestampLayout|ShaderBindingLayoutCache|DynamicResources|RenderGraphBuilder|RenderGraphSort|RenderPhase|RenderPhaseRegistry|FrameGraphRefs|ViewMemory|View/ViewFrame|GraphImageImporter|Renderer|RendererContext|RenderConfig|ViewResources|ViewDescriptorSets|ViewTargetFormats|ShadowCascades|BindlessTextureTable|RayTracingPipeline)\.]])
# the RHI's ray-tracing interfaces (BLAS geometry, skinned / procedural BLAS, the view's TLAS) + the dynamic-BLAS
# pass (RDG only); the Vulkan side implements them, the renderer's caches drive them
desert_placement(RenderCore [[Engine/Graphic/Systems/RayTracing/(RayTracingGeometry\.hpp|RayTracingDynamicGeometry\.|SkinnedRayTracingGeometry\.hpp|ProceduralRayTracingGeometry\.hpp|RayTracingSceneTarget\.hpp|RayTracingHitGroupTable\.hpp)]])
desert_placement(RenderCore [[Engine/Graphic/Materials/(MaterialBackend|SceneResources|MaterialExecutor|Mesh/MeshVertexLayout)\.|Engine/Graphic/ShaderProtocols/|Engine/Graphic/(BRDFLut|SkyPayload)\.|Engine/Graphic/Materials/Properties/|Engine/Graphic/Environment/SkyLook\.]])
# the render-facing DATA the world produces (scene proxies, materials, sky/fog/cloud payloads) is Engine's
desert_placement(Engine [[Engine/Graphic/Render/|Engine/Graphic/Clouds/|Engine/Graphic/Fog/|Engine/Graphic/(SkyRules|SkySettings|SkyPresets|SunLightFx|MaterialPipelineStates)\.]])
desert_placement(Engine [[Engine/Graphic/Materials/(Material|MaterialInstance|DataDrivenMaterial|MaterialOverrides|MaterialProperty|Mesh/MeshVertexPath|Skybox/MaterialSkybox)\.|Engine/Graphic/Environment/(SceneEnvironment|OwnedEnvironment)\.]])
desert_placement(Renderer [[Engine/Graphic/]])
desert_placement(Media [[Engine/Media/]])
# feature modules over Engine (UE plugins own their assets, services, components and systems)
desert_placement(Foliage [[Engine/World/Foliage/|Engine/Assets/(FoliageTypeAsset|Serialization/FoliageType)\.|Engine/Runtime/Services/Foliage/|Engine/ECS/ProceduralFoliageComponent\.]])
desert_placement(ScriptLuau [[Engine/Scripting/Luau/]])
desert_placement(Scripting [[Engine/Scripting/]])
desert_placement(Engine [[^]])  # everything else of the old library (Core, ECS, Runtime, typed Assets, Generated, pch)
