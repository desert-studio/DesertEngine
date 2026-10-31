#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Desert::Core
{
    /**
     * WHERE A SHADER GRAPH'S OWN RESOURCES LIVE IN DESCRIPTOR SET 0 — one window, for every domain.
     *
     * A graph's text is always compiled INTO a program the graph does not own. A Surface or PostProcess
     * graph becomes a generated `.shader` that includes the engine's own lighting headers; a Volume graph
     * becomes the cloud medium that four shipped programs are compiled against
     * (Docs/Clouds/O1_DESIGN.md §12.2). Either way the graph declares GLSL beside declarations it cannot
     * see, and a graph author cannot be asked to know what they are.
     *
     * TWO DECLARATIONS ON ONE DESCRIPTOR SLOT IS NOT A COMPILE ERROR. glslang emits both Binding
     * decorations with no diagnostic at all, `-Werror` included (measured with glslc 2026-09-08, Г17).
     * The engine refuses it after compilation — ShaderReflection::ReflectStage names both resources and
     * VulkanShader::Reflect drops the shader — but that refusal arrives when a material is applied, in
     * front of the person applying it. A reservation is what keeps it from arriving at all.
     *
     * WHY THE NUMBER CANNOT BE DERIVED IN C++, which is the first thing to reach for. The occupancy of a
     * program lives in GLSL text, spread across included headers and macros, and is only a set of numbers
     * after shaderc has run — Г17 established exactly that when it moved the collision check to
     * reflection. So the number here is a RESERVATION and the claim that it is free is a MEASUREMENT:
     * `Desert/Tests/Engine/ShaderCacheKey` compiles every pass of every stage of every shipped `.shader`
     * with shaderc, reflects the SPIR-V with the engine's own reflection, and asserts that not one set-0
     * binding falls at or above this value. It names no engine binding and counts nothing, so a slot
     * added tomorrow reddens it whatever spelled the number.
     *
     * 24 WAS ONE ABOVE THE HIGHEST BINDING THE TREE DECLARED, and that was not comfortable headroom —
     * it was none; the window now opens at 31, above the scene-read range below. The lit shader-graph surface
     * layout reaches 23 (the fourth cascade shadow map in Programs/Graph/MatLitConst.shader), so the next binding
     * that layout grows lands here. That is what the census is for: the collision becomes a red test rather than a
     * wrong picture.
     *
     * MOVING THIS VALUE IS NOT FREE. The DSL spells a texture base as a literal inside the GENERATED
     * `.shader` (`Properties ... TextureBinding(24)`), so a `.shader` generated before a move keeps the
     * old number until its `.dgraph` is compiled again. No generated shader in this repository declares a
     * texture today, so there is no stale artifact to fix; whoever raises this value owns finding out
     * whether that is still true.
     */
    /**
     * THE RESERVED SCENE-READ RANGE: set-0 slots a VIEW-pass surface cell reads per-(frame x view) scene data at,
     * bound by name — the scene's GpuScene primitive table (`GpuScenePrimitives`) and the scene's bone palettes
     * (`ObjectBones`), both declared by Common/GpuScene.glslh and bound by Graphic::SceneFrameBinding::ApplyTo
     * (TAA1 step 4), and the view's scene depth (`SceneDepthTexture`) the particle-sprite cell reads for Depth
     * Fade, declared by Mesh/Surface/SurfaceTypes.glslh and bound by ParticleRenderer. The depth had sat at 26 on
     * its own, i.e. on top of the bone palettes in every sprite cell — a per-view scene read is exactly what this
     * range is for, so it takes the range's third slot rather than a number of its own.
     *
     * They need fixed numbers because one header declares them into EVERY view-pass cell of EVERY surface
     * template, beside a template's own hand-numbered textures and the forward pass's lighting slots, which
     * together already fill 0-24 (measured 2026-10-07: SpotLightsUB 16, InstanceTransforms 17, emissive 24).
     * So the range sits ABOVE the engine's last slot and BELOW the graph's window, which moved up to make room.
     *
     * A hope is not a guarantee, so two seams hold it: DShaderParser refuses a Properties `Binding(n)` /
     * `TextureBinding(n)` whose row or texture run reaches into this range (a named parse error), and
     * Tests/Engine/VelocityTarget asserts that over the whole shipped tree nothing but the two scene-read
     * resources is declared at these numbers, and that every scene read is declared by its table name
     * (kSceneBindingNames below; shaders never spell the numbers).
     */
    inline constexpr uint32_t kSceneReadBindingFirst = 25u;
    inline constexpr uint32_t kGpuScenePrimitivesBinding = kSceneReadBindingFirst;      // Common/GpuScene.glslh
    inline constexpr uint32_t kObjectBonesBinding        = kSceneReadBindingFirst + 1u; // Common/GpuScene.glslh
    inline constexpr uint32_t kSceneDepthBinding = kSceneReadBindingFirst + 2u; // Mesh/Surface/SurfaceTypes.glslh
    // The view's opaque shading word beside its depth (`SceneShadingWordTexture`, Forward cells only): the caster bit
    // of the texel a contact-shadow ray hit (Common/ContactShadow.glslh, SceneViewInputs::SceneShadingWord).
    // Declared in Mesh/Surface/SurfaceTypes.glslh.
    inline constexpr uint32_t kSceneShadingWordBinding = kSceneReadBindingFirst + 3u;
    // The view's light grid (Graphic/LightGrid.hpp), read by the forward cell (Mesh/Surface/Pass_Forward.glslh) to
    // shade a pixel with its froxel's point and spot lights only: its parameters, then its cells. Bound by name
    // from the frame graph (BindSceneViewInputs), like the deferred composite's copy of the same two blocks.
    inline constexpr uint32_t kLightGridParamsBinding = kSceneReadBindingFirst + 4u;
    inline constexpr uint32_t kLightGridCellsBinding  = kSceneReadBindingFirst + 5u;
    // The view's MegaLights translucency volume (Common/MegaLightsVolume.glslh, Graphic::MegaLightsVolumeRefs),
    // read by the forward and translucent cells: its block, then its three volumes (R, G, B). Bound by name from
    // the frame graph (BindSceneViewInputs).
    inline constexpr uint32_t kMegaLightsVolumeParamsBinding = kSceneReadBindingFirst + 6u;
    inline constexpr uint32_t kMegaLightsVolumeFirstBinding  = kSceneReadBindingFirst + 7u;
    // The world's parameters (`WorldParameters`, Common/WorldParameters.glslh): declared only into the cells whose
    // surface reads one (DShaderParser lowers `WorldParameter( "Name" )`), bound by SceneFrameBinding::ApplyTo.
    inline constexpr uint32_t kWorldParametersBinding = kSceneReadBindingFirst + 10u;
    // The view's rain occlusion (Graphic/RainOcclusion.hpp, Common/RainOcclusion.glslh): the top-down depth
    // of what stands over the camera's surroundings (`RainOcclusionMapTexture`) and the block that projects a world
    // position into it (`RainOcclusionUB`). Declared only into the cells that wet a surface
    // (DESERT_SURFACE_WEATHER: the Forward and GBuffer mesh cells); the map is bound from the frame graph
    // (BindSceneViewInputs), the block by SceneFrameBinding::ApplyTo.
    inline constexpr uint32_t kRainOcclusionMapBinding   = kSceneReadBindingFirst + 11u;
    inline constexpr uint32_t kRainOcclusionBlockBinding = kSceneReadBindingFirst + 12u;
    // The view's point/spot shadows (Graphic/LocalShadows.hpp), read by the forward cells through the same reader
    // the deferred composite uses (Mesh/LocalLightShadow.glslh): the atlas (`LocalShadowAtlasTexture`), then the table
    // of its views (`LocalShadowViewsUB`) each light's shadowView names. Bound by name from the frame graph
    // (BindSceneViewInputs); declared in Mesh/ForwardLocalLights.glslh.
    inline constexpr uint32_t kLocalShadowAtlasBinding = kSceneReadBindingFirst + 13u;
    inline constexpr uint32_t kLocalShadowViewsBinding = kSceneReadBindingFirst + 14u;
    // The view's ray-traced local shadow masks (`LocalShadowMasksTexture`, Graphic/RayTracedLocalShadow.hpp: one layer per
    // shadowed light, by its shadowMaskSlot), read by the OPAQUE forward cells instead of the atlas when the local
    // shadows are ray traced (DESERT_LOCAL_SHADOW_SCREEN_MASK, as the deferred composite reads them). Bound by name
    // from the frame graph (BindSceneViewInputs); declared in Mesh/ForwardLocalLights.glslh.
    inline constexpr uint32_t kLocalShadowMasksBinding = kSceneReadBindingFirst + 15u;
    // THE BINNED DRAW'S TABLES (GPU-DRIVEN DRAWBINS, Common/GpuScene.glslh under DESERT_GPU_SCENE_DRAWS): what a
    // mesh-pass vertex stage resolves its draw from, by VisibleInstances[gl_InstanceIndex] alone — the scene's
    // instance rows (GpuScene, `GpuSceneInstances`), the pass's visible list (`GpuSceneVisibleInstances`: the
    // DrawCommandCache's unculled list, later the InstanceCulling compaction), the pass's cached draw slots
    // (`GpuSceneDrawSlots`: material row, submesh transform row) and the cache's submesh transforms
    // (`GpuSceneSubmeshTransforms`). Instances and VisibleInstances at +16 / +17 (contract question 2), the two
    // DrawCommandCache tables at the next free slots (recorded in F:/aftmp/gpudriven/CONTRACT.md).
    inline constexpr uint32_t kGpuSceneInstancesBinding         = kSceneReadBindingFirst + 16u;
    inline constexpr uint32_t kGpuSceneVisibleInstancesBinding  = kSceneReadBindingFirst + 17u;
    inline constexpr uint32_t kGpuSceneDrawSlotsBinding         = kSceneReadBindingFirst + 18u;
    inline constexpr uint32_t kGpuSceneSubmeshTransformsBinding = kSceneReadBindingFirst + 19u;
    // The view's FOG, which a translucent surface applies at its own depth (UE: translucency fogs itself, it is
    // not fogged by the opaque depth behind it): the height fog's parameter block (`FogParamsBuffer`), the sky's
    // aerial perspective volume, its distant sky light and the volumetric fog's integrated froxels. Declared by
    // Mesh/Surface/SceneFog.glslh in the translucent mesh cell and the particle sprite cell (and by the water
    // program through the same header), bound by name (BindSceneFogInputs).
    inline constexpr uint32_t kSceneFogParamsBinding            = kSceneReadBindingFirst + 20u;
    inline constexpr uint32_t kSceneFogAerialPerspectiveBinding = kSceneReadBindingFirst + 21u;
    inline constexpr uint32_t kSceneFogDistantSkyLightBinding   = kSceneReadBindingFirst + 22u;
    inline constexpr uint32_t kSceneFogVolumeBinding            = kSceneReadBindingFirst + 23u;
    static_assert( kSceneFogParamsBinding == 45u && kSceneFogVolumeBinding == 48u,
                   "Mesh/Surface/SceneFog.glslh spells 45-48" );
    // THE VIEW'S VIRTUAL SHADOW MAP AS A READER SEES IT (Shadows.Method VirtualMaps, DESERT_LOCAL_SHADOW_VSM): the
    // frame's level table (`VsmFrameData`), the page table (`VsmPageTable`), the physical pool
    // (`PhysicalPoolTexture`, read only) and the frame's shadowed point / spot lights (`VsmLocalLightsUB`, indexed by
    // a light's shadowMaskSlot), read by the forward and translucent cells and the forward terrain through
    // Mesh/LocalLightShadow.glslh in place of the local shadow atlas (Common/VirtualShadowMap.glslh's reader, the
    // one "VSM: Project Local" uses). Bound by name from the frame graph (BindSceneViewInputs, SceneViewInputs::Vsm*);
    // declared in Mesh/ForwardLocalLights.glslh. The raster's own copies of the first three are write targets of a
    // different pass and keep their range below.
    inline constexpr uint32_t kVsmReadFrameDataBinding    = kSceneReadBindingFirst + 24u;
    inline constexpr uint32_t kVsmReadPageTableBinding    = kSceneReadBindingFirst + 25u;
    inline constexpr uint32_t kVsmReadPhysicalPoolBinding = kSceneReadBindingFirst + 26u;
    inline constexpr uint32_t kVsmReadLocalLightsBinding  = kSceneReadBindingFirst + 27u;
    static_assert( kVsmReadFrameDataBinding == 49u && kVsmReadLocalLightsBinding == 52u,
                   "Mesh/ForwardLocalLights.glslh spells 49-52" );
    inline constexpr uint32_t kSceneReadBindingCount = 28u;

    [[nodiscard]] constexpr bool IsSceneReadBinding( uint32_t binding )
    {
        return binding >= kSceneReadBindingFirst && binding < kSceneReadBindingFirst + kSceneReadBindingCount;
    }

    /**
     * THE VIRTUAL SHADOW RASTER'S OWN SLOTS ("VSM: Raster", MeshRenderer::AddVirtualShadowRasterPasses): the frame's
     * level table (`VsmFrameData`), the page table (`VsmPageTable`), the physical pool (`PhysicalPoolTexture`) and the
     * level the draw rasterizes (`VsmRasterLevel`), declared by Common/VirtualShadowRaster.glslh, and the level's
     * instance remap (`VsmInstanceRemap`, Mesh/Surface/Vertex_Instanced.glslh). Bound by name.
     *
     * They are declared into a SURFACE CELL (Mesh/Surface/Pass_VirtualShadowDepth.glslh) of every template and of
     * every graph-generated surface, beside the template's hand-numbered textures (0-24) and the graph's own
     * textures (the window below), so they may sit in neither, nor on the scene-read range a header shares with the
     * view-pass cells. They had sat at 40-44: on the last scene-read slot and in the graph's window. A range of
     * their own between the two keeps the three apart by construction.
     */
    inline constexpr uint32_t kVirtualShadowRasterBindingFirst = kSceneReadBindingFirst + kSceneReadBindingCount;
    inline constexpr uint32_t kVsmFrameDataBinding             = kVirtualShadowRasterBindingFirst;
    inline constexpr uint32_t kVsmPageTableBinding             = kVirtualShadowRasterBindingFirst + 1u;
    inline constexpr uint32_t kVsmPhysicalPoolBinding          = kVirtualShadowRasterBindingFirst + 2u;
    inline constexpr uint32_t kVsmRasterLevelBinding           = kVirtualShadowRasterBindingFirst + 3u;
    inline constexpr uint32_t kVsmInstanceRemapBinding         = kVirtualShadowRasterBindingFirst + 4u;
    inline constexpr uint32_t kVirtualShadowRasterBindingCount = 5u;

    [[nodiscard]] constexpr bool IsVirtualShadowRasterBinding( uint32_t binding )
    {
        return binding >= kVirtualShadowRasterBindingFirst &&
               binding < kVirtualShadowRasterBindingFirst + kVirtualShadowRasterBindingCount;
    }

    /**
     * THE VIRTUAL GEOMETRY RASTER'S OWN SLOTS (VIRT-GEOM step 11, the `.VirtualGeometry` surface cells,
     * DShaderParser kSurfaceVirtualGeometryCellSuffix): what a cluster-drawing vertex stage resolves its triangle
     * from (Common/GpuScene.glslh under DESERT_GPU_SCENE_VIRTUAL_GEOMETRY) beside the binned draw's 41/43/44 - the
     * VirtualMeshTable's cluster rows (`VgRasterClusters`) and DAG rows (`VgRasterDags`), the cut's visible
     * clusters (`VgRasterVisibleClusters`, the rows a_VisibleRow names) and the pass's per-primitive draw rows
     * (`VgRasterPrimitiveDraws`: a cluster's draw slot = FirstDrawSlot + its DAG's submesh, lod 0). Declared into
     * every template's and every graph's surface cells, so - like the virtual shadow raster's - a range of its
     * own above that one and below the graph's window. Bound by name.
     */
    inline constexpr uint32_t kVirtualGeometryRasterBindingFirst =
         kVirtualShadowRasterBindingFirst + kVirtualShadowRasterBindingCount;
    inline constexpr uint32_t kVgRasterClustersBinding           = kVirtualGeometryRasterBindingFirst;
    inline constexpr uint32_t kVgRasterDagsBinding               = kVirtualGeometryRasterBindingFirst + 1u;
    inline constexpr uint32_t kVgRasterVisibleClustersBinding    = kVirtualGeometryRasterBindingFirst + 2u;
    inline constexpr uint32_t kVgRasterPrimitiveDrawsBinding     = kVirtualGeometryRasterBindingFirst + 3u;
    // THE VIEW'S VISIBILITY BUFFER (GRASS-VIS, Common/VisibilityBuffer.glslh `VisibilityBuffer`): one uint64 word
    // per pixel (VirtualGeometryLayout.hpp PackVisibility), written by the Visibility cells' atomic max and read
    // by the VisibilityResolve cells; bound by name (the grass renderer's buffer, GrassRenderer::VisibilityFrame).
    inline constexpr uint32_t kVgRasterVisibilityBinding         = kVirtualGeometryRasterBindingFirst + 4u;
    inline constexpr uint32_t kVirtualGeometryRasterBindingCount = 5u;

    // Raised from 24 (TAA1 step 4), then from 27 (the scene depth joined the range), then from 28 (the scene
    // shading word and the light grid joined it), then from 31 (the MegaLights volume joined it), then from 35
    // (the world parameters joined it), then from 36 (the rain occlusion's map and block joined it), then from 38
    // (the local shadow atlas and its views joined it), then from 40 (the ray-traced local shadow masks joined
    // it), then from 41 (the virtual shadow raster's slots got a range of their own), then from 46 (the binned
    // draw's four tables joined the scene-read range and pushed the raster range up by four), then from 50 (the
    // view's fog joined the scene-read range and pushed the raster range up by four), then from 54 (the virtual
    // shadow map's reader tables joined the scene-read range after the fog and pushed the raster range up by four
    // again), then from 58 (the virtual geometry raster's four slots got a range of their own), to sit above every
    // range, then from 62 (the visibility buffer joined the virtual geometry raster's range, GRASS-VIS); see the
    // paragraph on moving it above (10-22: no generated `.shader` under Resources spells a TextureBinding).
    inline constexpr uint32_t kGraphOwnedBindingFirst =
         kVirtualGeometryRasterBindingFirst + kVirtualGeometryRasterBindingCount;
    static_assert( kGraphOwnedBindingFirst == 63u,
                   "the graph's window opens right above the virtual geometry raster's range" );

    /**
     * THE SSR PROGRAM'S FRONT-LAYER READS (Programs/Deferred/SSR.shader under DESERT_REFLECTIONS_FRONT_LAYER,
     * Common/SSRTrace.glslh): the translucent front layer's normal/roughness (`FrontLayerNormalTexture`) and
     * device depth (`FrontLayerDepthTexture`), bound by name (SSRRenderer DeclareFrontLayerReads).
     *
     * SET 1, because set 0 has no slot for them outside every shared range: the traced permutation they ride on
     * fills set 0 from 0 to 40 (its G-buffer inputs, the GI frame, lights, light grid, radiance cache, the GI
     * tracer at 23..33 and the surface cache at 34..40), 25..61 are the scene-read and raster ranges, and 62 up is
     * the graph's window, which nothing shipped may declare into (Tests/Engine/ShaderCacheKey). They had sat at
     * 50/51, inside the scene-read range. Set 1 is a program's own set (BindlessSetIndex.hpp) and no header
     * SSR.shader includes declares into it; the deferred composite parks its own extra reads there the same way
     * (Programs/Deferred/DeferredLighting.shader GiBackfaceTexture).
     */
    inline constexpr uint32_t kSsrFrontLayerSet           = 1u;
    inline constexpr uint32_t kSsrFrontLayerNormalBinding = 0u;
    inline constexpr uint32_t kSsrFrontLayerDepthBinding  = 1u;

    /**
     * THE TABLE SHADERS READ: every slot above, by its C++ name, as Generated/SceneBindings.glslh hands it to GLSL
     * (`#define kSceneFogVolumeBinding 48`; Graphic::ShaderProtocols::SceneBindingsGlsl, served by the shader
     * includer and hashed by the cache key like every generated protocol include - there is no file on disk to
     * edit). A shader never spells one of these numbers: it includes that header and writes
     * `layout( binding = kSceneFogVolumeBinding )`, so moving a range here moves every declaration with it, and
     * the changed generated text recompiles every program that reads one. One name in both languages, so sym.sh
     * --refs finds the C++ binder and the GLSL declaration together. A slot added above without a row here is a
     * name no shader can spell: the shader that needs it fails to compile, naming it.
     */
    struct SceneBindingName
    {
        std::string_view Name;
        uint32_t         Value = 0;
    };
#define DESERT_SCENE_BINDING_ROW( constant )                                                                      \
    SceneBindingName                                                                                              \
    {                                                                                                             \
        #constant, constant                                                                                       \
    }
    inline constexpr std::array kSceneBindingNames = {
         DESERT_SCENE_BINDING_ROW( kSceneReadBindingFirst ),
         DESERT_SCENE_BINDING_ROW( kGpuScenePrimitivesBinding ),
         DESERT_SCENE_BINDING_ROW( kObjectBonesBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneDepthBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneShadingWordBinding ),
         DESERT_SCENE_BINDING_ROW( kLightGridParamsBinding ),
         DESERT_SCENE_BINDING_ROW( kLightGridCellsBinding ),
         DESERT_SCENE_BINDING_ROW( kMegaLightsVolumeParamsBinding ),
         DESERT_SCENE_BINDING_ROW( kMegaLightsVolumeFirstBinding ),
         DESERT_SCENE_BINDING_ROW( kWorldParametersBinding ),
         DESERT_SCENE_BINDING_ROW( kRainOcclusionMapBinding ),
         DESERT_SCENE_BINDING_ROW( kRainOcclusionBlockBinding ),
         DESERT_SCENE_BINDING_ROW( kLocalShadowAtlasBinding ),
         DESERT_SCENE_BINDING_ROW( kLocalShadowViewsBinding ),
         DESERT_SCENE_BINDING_ROW( kLocalShadowMasksBinding ),
         DESERT_SCENE_BINDING_ROW( kGpuSceneInstancesBinding ),
         DESERT_SCENE_BINDING_ROW( kGpuSceneVisibleInstancesBinding ),
         DESERT_SCENE_BINDING_ROW( kGpuSceneDrawSlotsBinding ),
         DESERT_SCENE_BINDING_ROW( kGpuSceneSubmeshTransformsBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneFogParamsBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneFogAerialPerspectiveBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneFogDistantSkyLightBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneFogVolumeBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmReadFrameDataBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmReadPageTableBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmReadPhysicalPoolBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmReadLocalLightsBinding ),
         DESERT_SCENE_BINDING_ROW( kSceneReadBindingCount ),
         DESERT_SCENE_BINDING_ROW( kVirtualShadowRasterBindingFirst ),
         DESERT_SCENE_BINDING_ROW( kVsmFrameDataBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmPageTableBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmPhysicalPoolBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmRasterLevelBinding ),
         DESERT_SCENE_BINDING_ROW( kVsmInstanceRemapBinding ),
         DESERT_SCENE_BINDING_ROW( kVirtualShadowRasterBindingCount ),
         DESERT_SCENE_BINDING_ROW( kVirtualGeometryRasterBindingFirst ),
         DESERT_SCENE_BINDING_ROW( kVgRasterClustersBinding ),
         DESERT_SCENE_BINDING_ROW( kVgRasterDagsBinding ),
         DESERT_SCENE_BINDING_ROW( kVgRasterVisibleClustersBinding ),
         DESERT_SCENE_BINDING_ROW( kVgRasterPrimitiveDrawsBinding ),
         DESERT_SCENE_BINDING_ROW( kVgRasterVisibilityBinding ),
         DESERT_SCENE_BINDING_ROW( kVirtualGeometryRasterBindingCount ),
         DESERT_SCENE_BINDING_ROW( kGraphOwnedBindingFirst ),
         DESERT_SCENE_BINDING_ROW( kSsrFrontLayerSet ),
         DESERT_SCENE_BINDING_ROW( kSsrFrontLayerNormalBinding ),
         DESERT_SCENE_BINDING_ROW( kSsrFrontLayerDepthBinding ),
    };
#undef DESERT_SCENE_BINDING_ROW

    [[nodiscard]] consteval bool SceneBindingNamesAreUnique()
    {
        for ( std::size_t i = 0; i < kSceneBindingNames.size(); ++i )
            for ( std::size_t j = i + 1; j < kSceneBindingNames.size(); ++j )
                if ( kSceneBindingNames[i].Name == kSceneBindingNames[j].Name )
                    return false;
        return true;
    }
    static_assert( SceneBindingNamesAreUnique(), "a slot is listed twice in kSceneBindingNames" );
} // namespace Desert::Core
