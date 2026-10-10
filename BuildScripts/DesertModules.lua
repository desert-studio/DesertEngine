-- THE ENGINE'S MODULE TABLE (ENG-MODULES plan v3, P0 MODULE-INFRA): the one place that says which modules
-- the engine is made of, which modules each one may include, and which module every engine file belongs to.
--
-- Three readers, one text:
--   * premake (Desert/Desert/premake5.lua) validates it at generation time and hands its path to
--     DesertHeaderTool;
--   * DesertHeaderTool (Tools/DesertHeaderTool/Source/ModuleTable.cpp) groups the reflected types by module
--     and emits one RegisterReflection_<Module>() per module (plan C11);
--   * the ModuleBoundary suite (Desert/Tests/Tools/ModuleBoundary) walks every #include of the engine and
--     reports each line that crosses into a module outside the includer's declared dependency closure.
-- The file is DATA plus three pure functions; the C++ readers execute it with the Lua VM and read only
-- `Modules`, `LegacyRoot` and `Placement`, so whatever premake can parse they parse the same way.
--
-- Modules: in layer order (a module only depends on modules above it in this list - Validate() fails
-- otherwise). `Deps` are DIRECT dependencies, like UE's PublicDependencyModuleNames; the transitive closure
-- is allowed. `Folder` is the module's home, Desert/<Module>/Source/<Module> (the UI-FW-4 layout), into which
-- its files move in P1..P14; a file inside a module's Folder belongs to that module.
--
-- Placement: where a file still living in the old single library (`LegacyRoot`) belongs. Ordered, FIRST
-- MATCH WINS; each Pattern is an ECMAScript regular expression searched in the path relative to LegacyRoot
-- (e.g. "Engine/Graphic/RDG/RenderGraph.hpp"). The last row catches everything, so no file is unplaced. A
-- module's rows are deleted in the same commit that moves its files into its Folder.

local DesertModules = {}

DesertModules.LegacyRoot = "Desert/Desert/Source"

local function M( name, deps, ue, folder )
    return { Name = name, Deps = deps, Ue = ue, Folder = folder or ( "Desert/" .. name .. "/Source/" .. name ) }
end

DesertModules.Modules = {
    -- L0
    M( "Common",          {},                                     "Core",            "Desert/Common/Source/Common" ),
    -- L1
    M( "Render2DCore",    { "Common" },                           "SlateCore" ),
    M( "CoreReflection",  { "Common" },                           "CoreUObject (reflection)" ),
    M( "Projects",        { "Common" },                           "Projects" ),
    M( "ImageCore",       { "Common" },                           "ImageCore" ),
    M( "PhysicsCore",     { "Common" },                           "PhysicsCore" ),
    M( "Audio",           { "Common" },                           "AudioMixer" ),
    M( "ApplicationCore", { "Common" },                           "ApplicationCore" ),
    M( "GeometryCore",    { "Common" },                           "GeometryCore" ),
    -- L2
    M( "TextCore",        { "Render2DCore", "ImageCore" },        "SlateFontRasterization" ),
    M( "AssetCore",       { "CoreReflection" },                   "CoreUObject (packages) + AssetRegistry" ),
    M( "DesertUI",        { "Render2DCore", "CoreReflection" },   "UMG/Slate" ),
    M( "AnimationCore",   { "GeometryCore", "CoreReflection" },   "AnimationCore" ),
    -- L3
    M( "Localization",    { "AssetCore" },                        "Internationalization" ),
    M( "ShaderCompiler",  { "ImageCore", "AssetCore" },           "ShaderCore" ),
    -- L4: RHI + VulkanRHI + RDG as one module (one backend, decision 09-27; plan sec. 3.1)
    M( "RenderCore",      { "ShaderCompiler", "ImageCore", "ApplicationCore", "Projects", "AssetCore", "GeometryCore" },
                                                                  "RHI + VulkanRHI + RenderCore" ),
    -- L5
    M( "Engine",          { "RenderCore", "Render2DCore", "Localization", "PhysicsCore", "AnimationCore", "GeometryCore",
                            "DesertUI", "TextCore", "Audio" },    "Engine" ),
    -- L6: the renderer sits ABOVE Engine (UE layering, plan sec. 3.2); features are plugins over Engine
    M( "Renderer",        { "Engine" },                           "Renderer" ),
    M( "Media",           { "Engine", "Audio" },                  "MediaAssets" ),
    M( "Landscape",       { "Engine" },                           "Landscape" ),
    M( "Destruction",     { "Engine" },                           "GeometryCollectionEngine" ),
    M( "VFX",             { "Engine" },                           "Niagara" ),
    M( "Water",           { "Engine" },                           "Water" ),
    M( "Hair",            { "Engine" },                           "HairStrands" ),
    -- L7
    M( "Foliage",         { "Landscape" },                        "Foliage" ),
    -- L8
    -- the language-free script host: Engine's public reflected layer only (FUNCTION/EVENT), no feature module
    M( "Scripting",       { "Engine" },                           "ScriptPlugin" ),
    -- L9: a script language is a plugin over Scripting (Luau: the VM, the reflection binder, the host natives)
    M( "ScriptLuau",      { "Scripting" },                        "ScriptPlugin" ),
}

local function P( module, pattern )
    return { Module = module, Pattern = pattern }
end

DesertModules.Placement = {
    -- decided by UI-FW-4
    P( "Render2DCore",    [[Engine/Graphic/Render2D/(DrawList2D|ClipRegion2D|Transform2D|RetainerEffect)\.|Engine/Text/(Utf8|BakedFont)\.]] ),
    P( "Engine",          [[Engine/UI/Ecs/]] ), -- the UMG-side adapter stays in the engine
    P( "DesertUI",        [[Engine/UI/]] ),
    -- leaves
    P( "CoreReflection",  [[Engine/Reflection/]] ),
    P( "Projects",        [[Engine/Project/]] ),
    P( "ImageCore",       [[Engine/Core/(Formats|IO)/]] ),
    P( "TextCore",        [[Engine/(Text|Vector)/]] ),
    P( "PhysicsCore",     [[Engine/Physics/]] ),
    P( "Audio",           [[Engine/Audio/]] ),
    P( "ApplicationCore", [[Engine/Core/(Window|WindowCloseGate|Input|Glfw|GlfwVulkan)\.|^Platform/]] ),
    -- the asset framework (CoreUObject package loading + AssetRegistry)
    P( "AssetCore",       [[Engine/Assets/(AssetBase|AssetManager|AssetRef|AssetRefSerialization|AssetGuidRef|AssetRootPin|AssetRootSet|AssetEvents|AssetMetadata|AsyncAssetLoader|ContentRegistry|ContentRegistryCook|ContentGate|ContentWork|ContentDirectoryWatch|RegistryDiscovery|TextAssetHeader\w*|ContainerBytes|FrameRetireQueue|SyncLoadLedger|ItemProgress|EvictionDeadline|AssetEviction|AssetEvictionServices|DDCShipRegister|Common)\.]] ),
    P( "Localization",    [[Engine/Localization/|Engine/Assets/StringTable(Asset|Source)\.]] ),
    -- geometry / animation runtime cores
    P( "Engine",          [[Engine/Geometry/(Mesh|StaticMesh|SkinnedMesh|MeshFactory|PrimitiveMeshFactory|ProceduralCharacter\w*|AutoRig|MeshAssetArrays|DynamicMeshAsset|EditMeshAsset|DynamicMeshRenderConversion|RenderMeshData|LODSelection|PosedBounds)\.]] ),
    P( "GeometryCore",    [[Engine/Geometry/]] ),
    P( "Engine",          [[Engine/Animation/(AnimationLibrary\.|Graph/AnimGraphSerialization\.|Timeline/SequenceFormat\.|Modular/)]] ),
    P( "AnimationCore",   [[Engine/Animation/]] ),
    -- RHI + RDG
    P( "ShaderCompiler",  [[Engine/Core/ShaderCompiler/]] ),
    P( "RenderCore",      [[Engine/Graphic/API/|Engine/Graphic/RDG/|Engine/ShaderResources/|Engine/Core/(Device|GpuBenchmark|FrameManager)\.]] ),
    P( "RenderCore",      [[Engine/Graphic/(Image|ImageFactory|ImageTraits|IndexBuffer|VertexBuffer|Framebuffer|RenderPass|RenderPassDeclaration|ExternalRenderPass|Pipeline|PipelineCache|PipelineCacheFile|PipelineBuilds|Shader|SwapChain|SwapchainAcquire|GpuBatch|ComputeImages|MipMapGenerator|DeviceLost|DrawCounters|MemoryReadout|MemoryReadoutSource|MappedMemory|AllocationLedger|ResourceLedger|RendererAPI|RendererTypes|Texture|TextureFactory|FallbackTextures|DefaultTextures|PixelPack|GpuTimestampLayout|ShaderBindingLayoutCache|DynamicResources|RenderGraphBuilder|RenderGraphSort|RenderPhase|RenderPhaseRegistry|FrameGraphRefs|ViewMemory|GraphImageImporter|Renderer|RendererContext|RenderConfig|ViewResources|ViewDescriptorSets|ViewTargetFormats|ShadowCascades)\.]] ),
    P( "RenderCore",      [[Engine/Graphic/Materials/(MaterialBackend|SceneResources|MaterialExecutor|Mesh/MeshVertexLayout)\.|Engine/Graphic/ShaderProtocols/|Engine/Graphic/(BRDFLut|SkyPayload)\.|Engine/Graphic/Materials/Properties/|Engine/Graphic/Environment/SkyLook\.]] ),
    -- the render-facing DATA the world produces (scene proxies, materials, sky/fog/cloud payloads) is Engine's
    P( "Engine",          [[Engine/Graphic/Render/|Engine/Graphic/Clouds/|Engine/Graphic/Fog/|Engine/Graphic/(SkyRules|SkySettings|SkyPresets|SunLightFx|MaterialPipelineStates)\.]] ),
    P( "Engine",          [[Engine/Graphic/Materials/(Material|MaterialInstance|DataDrivenMaterial|MaterialOverrides|MaterialProperty|Mesh/MeshVertexPath|Skybox/MaterialSkybox)\.|Engine/Graphic/Environment/(SceneEnvironment|OwnedEnvironment)\.]] ),
    P( "Renderer",        [[Engine/Graphic/]] ),
    P( "Media",           [[Engine/Media/]] ),
    -- feature modules over Engine (UE plugins own their assets, services, components and systems)
    P( "Landscape",       [[Engine/World/Landscape/|Engine/Assets/(LandscapeLayerInfoAsset|Serialization/LandscapeLayerInfo)\.|Engine/Runtime/Services/Landscape/|Engine/ECS/(System/Landscape\w*|LandscapeRootOf|LandscapeLayerRules)\.]] ),
    P( "Foliage",         [[Engine/World/Foliage/|Engine/Assets/(FoliageTypeAsset|Serialization/FoliageType)\.|Engine/Runtime/Services/Foliage/|Engine/ECS/ProceduralFoliageComponent\.]] ),
    P( "Destruction",     [[Engine/Destruction/|Engine/Assets/FractureAsset\.|Engine/Runtime/Services/Destruction/|Engine/ECS/(DestructibleComponent|System/Destructible\w*|System/DestructionFields)\.]] ),
    P( "VFX",             [[Engine/VFX/|Engine/Assets/(VFXSystemAsset|Serialization/VFXSystem)\.]] ),
    P( "Water",           [[Engine/Water/|Engine/Assets/(WaterWavesAsset|Serialization/WaterWaves)\.]] ),
    P( "Hair",            [[Engine/Hair/]] ),
    P( "ScriptLuau",      [[Engine/Scripting/Luau/]] ),
    P( "Scripting",       [[Engine/Scripting/]] ),
    P( "Engine",          [[^]] ), -- everything else of the old library (Core, ECS, Runtime, typed Assets, Generated, pch)
}

-- Each module's generated reflection registration (DesertHeaderTool, plan C11) belongs to that module, wherever
-- the generator writes it; these rows go first so the catch-all below cannot claim them.
for index, module in ipairs( DesertModules.Modules ) do
    table.insert( DesertModules.Placement, index,
                  P( module.Name, "Engine/Generated/Reflection_" .. module.Name .. "\\.gen\\.cpp$" ) )
end

function DesertModules.Find( name )
    for _, module in ipairs( DesertModules.Modules ) do
        if module.Name == name then
            return module
        end
    end
    return nil
end

-- Every module `name` may include: its direct dependencies and theirs, transitively.
function DesertModules.Closure( name )
    local seen = {}
    local function visit( n )
        for _, dep in ipairs( DesertModules.Find( n ).Deps ) do
            if not seen[dep] then
                seen[dep] = true
                visit( dep )
            end
        end
    end
    visit( name )
    return seen
end

-- A dependency on an unknown module, or on a module not listed EARLIER (which is what makes the graph acyclic
-- by construction), a duplicate name, or a placement row naming an unknown module is an error naming the row.
function DesertModules.Validate()
    local position = {}
    for index, module in ipairs( DesertModules.Modules ) do
        if position[module.Name] then
            error( string.format( "DesertModules.lua: module '%s' is listed twice", module.Name ) )
        end
        for _, dep in ipairs( module.Deps ) do
            if not position[dep] then
                error( string.format( "DesertModules.lua: '%s' depends on '%s', which is %s", module.Name, dep,
                                      DesertModules.Find( dep ) and "listed after it (a cycle or a layer inversion)"
                                                                 or "not a module" ) )
            end
        end
        position[module.Name] = index
    end
    for index, row in ipairs( DesertModules.Placement ) do
        if not position[row.Module] then
            error( string.format( "DesertModules.lua: placement row %d names '%s', which is not a module", index, row.Module ) )
        end
    end
end

return DesertModules
