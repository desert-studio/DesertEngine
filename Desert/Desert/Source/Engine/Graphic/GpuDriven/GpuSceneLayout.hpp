#pragma once

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// GPU-DRIVEN contract (C0): the std430 rows of the scene's persistent GPU tables. THE declaration: the GLSL structs
// are generated from it at build time (Tools/ShaderProtocolGen, kGpuSceneShaderInclude below) into
// Generated/GpuDriven/GpuScene.glslh, which Common/GpuScene.glslh includes; DESERT_SHADER_STRUCT checks every
// offset against std430.
//
// UE: FPrimitiveSceneShaderData / FInstanceSceneShaderData (Renderer/Private/SceneRendering, GPUScene.cpp) and
// the InstanceCulling draw records (Renderer/Private/InstanceCulling/InstanceCullingContext.cpp).
//
// Indices into these tables are STABLE for the life of the primitive / instance (GpuScene hands them out from a
// free list), so a cached draw command, an RT instance and a surface-cache card can hold one across frames.

namespace Desert::Graphic::GpuDriven
{
    using PrimitiveIndex                    = uint32_t;
    using InstanceIndex                     = uint32_t;
    inline constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

    // Bits of GpuScenePrimitive::Flags (GLSL: DESERT_GPU_PRIMITIVE_*).
    namespace PrimitiveFlags
    {
        // The primitive casts into the shadow passes (CSM, VSM); clear = only the view passes draw it.
        inline constexpr uint32_t kCastsShadow = 1u << 0;
        // The primitive's vertices leave its bind bounds (skinned pose, wind): culling tests WorldBoundsOverride
        // of its instance instead of LocalBounds x InstanceToWorld (today: MeshDrawCommand::WorldBounds /
        // BoundsPad, MeshRenderer.hpp:96-98).
        inline constexpr uint32_t kWorldBoundsOverride = 1u << 1;
        // Skinned: BoneOffset / PrevBoneOffset / SkinCache* are live.
        inline constexpr uint32_t kSkinned = 1u << 2;
        // UE !bCastContactShadow: carried into the G-buffer shading word (today the PrimitiveFlags push field,
        // Common/MaterialTransport.glslh:41, Graphic::PrimitiveFlag).
        inline constexpr uint32_t kNoContactShadow = 1u << 3;
        // Exempt from the HZB test (editor gizmo-like or always-visible primitives); frustum culling still
        // applies.
        inline constexpr uint32_t kSkipOcclusion = 1u << 4;
        // Moves by itself every frame (a skinned pose, a swaying foliage field): a caster of the cascade cache's
        // MOVABLE layer, drawn over the copied static cache each frame, never into the cache (UE's cached shadow
        // maps: movable primitives go to the dynamic depths). The cull's layer flags below split on it, per
        // instance: a swaying instance past the shadow wind distance stands still there (ShadowWind.glslh).
        inline constexpr uint32_t kMovableCaster = 1u << 5;
        // Skinned: the mesh's binding 3 holds its own last-frame positions (a morphed twin without a skin cache,
        // MeshGeometry::PreviousVertices): the vertex stage that skins without a skin-cache entry reads
        // a_PrevPosition; clear = the vertices did not move under the mesh, it reads a_Position (UE's previous
        // position stream bound only where one exists).
        inline constexpr uint32_t kPreviousPositions = 1u << 6;
        // NEVER IN A PRIMITIVE ROW: the bit a resolved binned draw's Flags carries from its slot's
        // kDrawSlotEmissionIsLight (GLSL DESERT_GPU_DRAW_EMISSION_IS_LIGHT, Common/GpuScene.glslh
        // DesertResolveDrawOf). Reserved here so no primitive flag takes it.
        inline constexpr uint32_t kDrawEmissionIsLight = 1u << 7;
        // THE IMPOSTOR PAIR (UE ImpostorBaker + DitheredLODTransition): the source mesh of a baked impostor
        // (kImpostorSource) and its card (kImpostorCard) both live in Primitives[]; the cull's ImpostorGate
        // (InstanceCull.shader, CPU twin InstanceCullValidation.cpp) admits the source NEAR and the card FAR of
        // LocalBoundsExtent.w (the switch screen size), and inside the temporal band draws both with
        // complementary dither masks (DESERT_GPU_VISIBLE_LOD_FADE_OUT / _IN) so the switch never pops.
        inline constexpr uint32_t kImpostorSource = 1u << 8;
        inline constexpr uint32_t kImpostorCard   = 1u << 9;
        // UE !bReceivesDecals: the binned G-buffer cell skips the DBuffer fetch (GLSL DESERT_GPU_PRIMITIVE_NO_DECALS;
        // the per-draw twin is Graphic::PrimitiveFlag::NoDecals).
        inline constexpr uint32_t kNoDecals = 1u << 10;
    } // namespace PrimitiveFlags

    // THE BINNED DRAW'S INSTANCE ID STREAM (decision 10-27, UE5 instance id stream): a mesh-pass pipeline binds one
    // uint32 per drawn instance at VK_VERTEX_INPUT_RATE_INSTANCE on this vertex binding, read as `a_VisibleRow` at
    // this location (Common/GpuScene.glslh, DESERT_GPU_SCENE_DRAWS): the row of the pass's VisibleInstances list
    // the instance resolves from. Bindings 0 / 1 are the mesh's vertices and optional streams (MeshVertexLayout),
    // locations 0-8 their attributes. Written by the cull's compaction, or by the CPU loop from the
    // DrawCommandCache's unculled list until PASSES. Transforms are never streamed.
    inline constexpr uint32_t kVisibleRowVertexBinding  = 2u;
    inline constexpr uint32_t kVisibleRowVertexLocation = 9u;

    // AN AFFINE TRANSFORM, PACKED 3x4 (UE FRenderTransform; decision 10-27): the first three ROWS of the matrix, the
    // fourth (0, 0, 0, 1) implicit. Rows[r] = (m[0][r], m[1][r], m[2][r], m[3][r]) of the column-major glm::mat4, so
    // Rows[r].w is the translation's r. 48 bytes instead of 64; a scene transform is affine by construction (TRS),
    // a projective one cannot be stored here. Shaders unpack only through GpuScene_LocalToWorld /
    // GpuScene_PrevLocalToWorld / GpuScene_InstanceToPrimitive (Common/GpuScene.glslh).
    using Vec4Array3 = std::array<glm::vec4, 3>;
#define DESERT_GPU_AFFINE3X4_FIELDS( X ) \
    X( Vec4Array3, Rows, "Rows", ( Vec4Array3{ glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ), glm::vec4( 0.0f, 1.0f, 0.0f, \
       0.0f ), glm::vec4( 0.0f, 0.0f, 1.0f, 0.0f ) } ) )
    DESERT_SHADER_STRUCT( GpuAffine3x4, "GpuAffine3x4", Std430, DESERT_GPU_AFFINE3X4_FIELDS,
        bool operator==( const GpuAffine3x4& ) const = default;
     );
#undef DESERT_GPU_AFFINE3X4_FIELDS

    [[nodiscard]] inline GpuAffine3x4 PackAffine( const glm::mat4& m )
    {
        GpuAffine3x4 packed;
        for ( int r = 0; r < 3; ++r )
            packed.Rows[r] = glm::vec4( m[0][r], m[1][r], m[2][r], m[3][r] );
        return packed;
    }

    [[nodiscard]] inline glm::mat4 UnpackAffine( const GpuAffine3x4& packed )
    {
        glm::mat4 m( 1.0f );
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 3; ++r )
                m[c][r] = packed.Rows[r][c];
        return m;
    }

    // ONE ROW PER PRIMITIVE OF THE SCENE (a mesh component: static, skinned, ISM, fracture part).
    // THE world/prev-world source for velocity, RT (RayTracingGather.cpp) and the surface cache.
#define DESERT_GPU_SCENE_PRIMITIVE_FIELDS( X ) \
    X( GpuAffine3x4, LocalToWorld, "LocalToWorld", {} ) \
    /* == LocalToWorld the frame after the primitive stopped moving */ \
    X( GpuAffine3x4, PrevLocalToWorld, "PrevLocalToWorld", {} ) \
    /* Mesh-space bounds of the whole mesh (Geometry::LocalBounds): xyz centre / extent, cm. \
       LocalBoundsCenter.w = how far wind moves vertices past the bounds (cm, today \
       MeshDrawCommand::BoundsPad). LocalBoundsExtent.w = the impostor switch screen size (UE screen size, of \
       THIS primitive's bounds) of a kImpostorSource / kImpostorCard row; 0 = none. */ \
    X( glm::vec4, LocalBoundsCenter, "LocalBoundsCenter", glm::vec4( 0.0f ) ) \
    X( glm::vec4, LocalBoundsExtent, "LocalBoundsExtent", glm::vec4( 0.0f ) ) \
    /* its instances: Instances[FirstInstance, +InstanceCount) */ \
    X( uint32_t, FirstInstance, "FirstInstance", 0 ) \
    X( uint32_t, InstanceCount, "InstanceCount", 0 ) /* 1 for a non-instanced primitive */ \
    /* LodScreenSizes[FirstLodScreenSize, +LodCount) (UE screen size) */ \
    X( uint32_t, FirstLodScreenSize, "FirstLodScreenSize", 0 ) \
    X( uint32_t, LodCount, "LodCount", 0 ) /* 1 = no LOD chain */ \
    X( int32_t, LodBias, "LodBias", 0 ) /* Geometry::SelectLODFromBounds lodBias */ \
    X( int32_t, ForcedLod, "ForcedLod", -1 ) /* Geometry::SelectLODFromBounds forcedLOD (-1 = auto) */ \
    X( uint32_t, Flags, "Flags", 0 ) /* PrimitiveFlags */ \
    X( uint32_t, Entity, "Entity", 0 ) /* the owning entity (picking, debug views, RT hit records) */ \
    /* Skinned (kSkinned): this frame's / the previous frame's palette in the scene's per-frame ObjectBones \
       buffer; the GPU skin cache's streams (RayTracingSkinCache) when it is on, 0 = skin in the vertex stage. */ \
    X( uint32_t, BoneOffset, "BoneOffset", 0 ) \
    X( uint32_t, PrevBoneOffset, "PrevBoneOffset", 0 ) \
    X( uint32_t, SkinCacheAddressLo, "SkinCacheAddressLo", 0 ) \
    X( uint32_t, SkinCacheAddressHi, "SkinCacheAddressHi", 0 ) \
    X( uint32_t, SkinCacheVertexCount, "SkinCacheVertexCount", 0 ) \
    /* The foliage wind of an instanced primitive (Graphic::InstanceWind; the binned instanced vertex stage \
       reads it here, where the CPU-recorded draw pushed it, Graphic::PackViewInstanceWind): Speed and both \
       clocks, then (dir.x, dir.z, Strength, Height). Strength 0 = does not sway (every other primitive). */ \
    X( float, WindSpeed, "WindSpeed", 0.0f ) \
    X( float, WindSeconds, "WindSeconds", 0.0f ) \
    X( float, WindPrevSeconds, "WindPrevSeconds", 0.0f ) \
    X( glm::vec4, Wind, "Wind", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( GpuScenePrimitive, "GpuScenePrimitive", Std430, DESERT_GPU_SCENE_PRIMITIVE_FIELDS );
#undef DESERT_GPU_SCENE_PRIMITIVE_FIELDS

    // ONE ROW PER INSTANCE: a non-instanced primitive owns exactly one (identity InstanceToPrimitive), an ISM one
    // per placed instance (today CPU CollectIsmInstances, InstanceCullDistance.hpp:97). World = primitive's
    // LocalToWorld x InstanceToPrimitive; previous world = PrevLocalToWorld x InstanceToPrimitive (an instance
    // that moves inside its component re-uploads with its primitive's Prev: UE keeps instance prev-transforms the
    // same way, through the primitive).
#define DESERT_GPU_SCENE_INSTANCE_FIELDS( X ) \
    X( GpuAffine3x4, InstanceToPrimitive, "InstanceToPrimitive", {} ) \
    X( uint32_t, Primitive, "Primitive", kInvalidIndex ) /* its row in Primitives[] */ \
    /* Distance cull of the ISM component (InstanceCullDistance.hpp), cm from the view origin; 0/0 = none. */ \
    X( float, CullDistanceStart, "CullDistanceStart", 0.0f ) \
    X( float, CullDistanceEnd, "CullDistanceEnd", 0.0f ) \
    /* Pose bounds when the primitive has kWorldBoundsOverride: the row in WorldBoundsOverrides[] \
       (kInvalidIndex otherwise). Today MeshDrawCommand::WorldBounds (SkinnedPoseBounds). */ \
    X( uint32_t, WorldBoundsRow, "WorldBoundsRow", kInvalidIndex ) \
    /* The component's own index of this instance (Graphic::ComponentInstanceIndex): the distance fade's hash \
       seed, so a field the density thinned fades each instance as the CPU culls do, and a density change \
       keeps every instance's threshold. The row's index in the primitive's range when nothing thinned it. */ \
    X( uint32_t, ComponentInstance, "ComponentInstance", 0 ) \
    X( uint32_t, Pad0, "Pad0", 0 ) \
    X( uint32_t, Pad1, "Pad1", 0 ) \
    X( uint32_t, Pad2, "Pad2", 0 )
    DESERT_SHADER_STRUCT( GpuSceneInstance, "GpuSceneInstance", Std430, DESERT_GPU_SCENE_INSTANCE_FIELDS );
    static_assert( sizeof( GpuSceneInstance ) == 80, "the std430 stride: the row pads to its vec4 alignment" );
#undef DESERT_GPU_SCENE_INSTANCE_FIELDS

    // A world-space box that replaces LocalBounds x InstanceToWorld for one instance (skinned pose this frame).
#define DESERT_GPU_SCENE_WORLD_BOUNDS_FIELDS( X ) \
    X( glm::vec4, Min, "Min", glm::vec4( 0.0f ) ) /* w = 0 */ \
    X( glm::vec4, Max, "Max", glm::vec4( 0.0f ) ) /* w = 0 */
    DESERT_SHADER_STRUCT( GpuSceneWorldBounds, "GpuSceneWorldBounds", Std430,
                          DESERT_GPU_SCENE_WORLD_BOUNDS_FIELDS );
#undef DESERT_GPU_SCENE_WORLD_BOUNDS_FIELDS

    // ONE ROW PER (PRIMITIVE, PASS): where the primitive's draw slots of that pass start. Slot of (lod, submesh)
    // = FirstDrawSlot + lod * SubmeshCount + submesh. SubmeshCount 0 = the primitive does not draw in the pass.
#define DESERT_GPU_PRIMITIVE_PASS_DRAWS_FIELDS( X ) \
    X( uint32_t, FirstDrawSlot, "FirstDrawSlot", 0 ) \
    X( uint32_t, SubmeshCount, "SubmeshCount", 0 ) \
    /* the levels that HAVE slots (a submesh without LODs repeats level 0) */ \
    X( uint32_t, LodCount, "LodCount", 0 ) \
    X( uint32_t, Pad, "Pad", 0 )
    DESERT_SHADER_STRUCT( GpuPrimitivePassDraws, "GpuPrimitivePassDraws", Std430,
                          DESERT_GPU_PRIMITIVE_PASS_DRAWS_FIELDS );
#undef DESERT_GPU_PRIMITIVE_PASS_DRAWS_FIELDS

    // ONE ROW PER CACHED DRAW of a pass (DrawCommandCache: a primitive's submesh at one LOD): the bin its
    // instances are counted into, and the per-draw state that today rides the push block (MaterialIndex,
    // PrimitiveFlags; Common/MaterialTransport.glslh) or the CPU draw's by-value push fields
    // (MeshDrawCommand::Push).
#define DESERT_GPU_DRAW_SLOT_FIELDS( X ) \
    /* Its row in the pass's DrawBins[] / IndirectDraws[]; kInvalidIndex = the submesh does not draw in this \
       pass (hidden, or no pipeline for its material there): the cull skips the slot. */ \
    X( uint32_t, Bin, "Bin", 0 ) \
    /* Its section's row in the scene material table (GpuScene::AddMaterialRow, \
       GpuSceneRegistry::SyncMaterialRow; MeshRenderer::StaticMeshRenderData::MaterialRow): the index the \
       shader reads as MaterialRows[MaterialIndex]. */ \
    X( uint32_t, MaterialRow, "MaterialRow", 0 ) \
    /* Its row in DrawCommandCache::SubmeshTransforms[]: the submesh's node transform relative to the \
       primitive (Submesh::Transform). World = LocalToWorld x InstanceToPrimitive x this. */ \
    X( uint32_t, SubmeshTransform, "SubmeshTransform", 0 ) \
    /* kDrawSlot* bits. kDrawSlotDitheredLod: the slot's cell clips (a Masked surface), so an instance in a \
       dithered LOD transition draws both its levels here (CullViewFlags::kLodDither); a cell without a \
       fragment clip would draw both levels whole, so it switches to the newer level at once. */ \
    X( uint32_t, Flags, "Flags", 0 )
    DESERT_SHADER_STRUCT( GpuDrawSlot, "GpuDrawSlot", Std430, DESERT_GPU_DRAW_SLOT_FIELDS );
#undef DESERT_GPU_DRAW_SLOT_FIELDS
    inline constexpr uint32_t kDrawSlotDitheredLod = 1u << 0;
    // The slot's material has Emissive Lights Scene on (its emission is a MegaLights light source): the binned
    // G-buffer cell marks the shading word's EMISSION_IS_LIGHT (GLSL DESERT_GPU_DRAW_SLOT_EMISSION_IS_LIGHT).
    inline constexpr uint32_t kDrawSlotEmissionIsLight = 1u << 1;
    // VG-SW-RASTER (UE Nanite's fixed-function SW raster bin): the slot's cell draws what
    // Programs/VirtualGeometry/SoftwareRaster.shader draws - no per-pixel clip (not kDrawSlotDitheredLod) and a cull
    // mode of Back or None - so the cluster cut may hand the slot's small clusters to the compute raster
    // (ClusterCut.shader RastersInSoftware). GLSL DESERT_GPU_DRAW_SLOT_SOFTWARE_RASTER.
    inline constexpr uint32_t kDrawSlotSoftwareRaster = 1u << 2;
    // The slot's cell culls no face (CullMode::None): the compute raster keeps both windings
    // (GLSL DESERT_GPU_DRAW_SLOT_TWO_SIDED).
    inline constexpr uint32_t kDrawSlotTwoSided = 1u << 3;


    // ONE ROW PER BIN of a pass (pass x pipeline x mesh x submesh x LOD): the index range it draws and where its
    // compacted instance list lives. The cull writes the bin's VkDrawIndexedIndirectCommand from it.
#define DESERT_GPU_DRAW_BIN_FIELDS( X ) \
    /* SubmeshIndices( submesh, lod ).Count (MeshDrawArguments.hpp:37) */ \
    X( uint32_t, IndexCount, "IndexCount", 0 ) \
    X( uint32_t, FirstIndex, "FirstIndex", 0 ) /* SubmeshIndices( submesh, lod ).First */ \
    X( int32_t, VertexOffset, "VertexOffset", 0 ) \
    /* its range in VisibleInstances[]: [FirstVisible, +Capacity) */ \
    X( uint32_t, FirstVisible, "FirstVisible", 0 ) \
    /* the most instances that can land in it (sum of its slots' instances) */ \
    X( uint32_t, Capacity, "Capacity", 0 ) \
    X( uint32_t, Group, "Group", 0 ) /* its IndirectGroup (pipeline x geometry buffers) of the pass */ \
    X( uint32_t, Pad0, "Pad0", 0 ) \
    X( uint32_t, Pad1, "Pad1", 0 )
    DESERT_SHADER_STRUCT( GpuDrawBin, "GpuDrawBin", Std430, DESERT_GPU_DRAW_BIN_FIELDS );
#undef DESERT_GPU_DRAW_BIN_FIELDS

    // ONE ROW PER BIN, written by the cull, read by the indirect draw: VkDrawIndexedIndirectCommand (20 bytes) +
    // the bin it belongs to, at a 32-byte stride. ONE table on every platform (IndirectDrawSubmission.hpp).
#define DESERT_GPU_INDIRECT_DRAW_FIELDS( X ) \
    X( uint32_t, IndexCount, "IndexCount", 0 ) /* VkDrawIndexedIndirectCommand::indexCount */ \
    X( uint32_t, InstanceCount, "InstanceCount", 0 ) /* ::instanceCount — 0 for a bin the cull left empty */ \
    X( uint32_t, FirstIndex, "FirstIndex", 0 ) /* ::firstIndex */ \
    X( int32_t, VertexOffset, "VertexOffset", 0 ) /* ::vertexOffset */ \
    /* ::firstInstance = the bin's FirstVisible (gl_InstanceIndex indexes VisibleInstances) */ \
    X( uint32_t, FirstInstance, "FirstInstance", 0 ) \
    X( uint32_t, Bin, "Bin", 0 ) /* which bin (the count path compacts rows, so the row is not the bin) */ \
    X( uint32_t, Pad0, "Pad0", 0 ) \
    X( uint32_t, Pad1, "Pad1", 0 )
    DESERT_SHADER_STRUCT( GpuIndirectDraw, "GpuIndirectDraw", Std430, DESERT_GPU_INDIRECT_DRAW_FIELDS );
#undef DESERT_GPU_INDIRECT_DRAW_FIELDS
    static_assert( offsetof( GpuIndirectDraw, FirstInstance ) == 16, "VkDrawIndexedIndirectCommand prefix" );
    inline constexpr uint32_t kIndirectDrawStride = sizeof( GpuIndirectDraw );

    // ONE ENTRY PER SURVIVING (instance, draw slot) of a bin, at VisibleInstances[gl_InstanceIndex]: what the
    // vertex stage fetches instead of the push block's Transform / MaterialIndex / PrimitiveIndex.
#define DESERT_GPU_VISIBLE_INSTANCE_FIELDS( X ) \
    X( uint32_t, Instance, "Instance", 0 ) /* its row in Instances[] (-> Primitives[Instance.Primitive]) */ \
    /* its row in the pass's DrawSlots[] (-> MaterialRow) | kVisibleRestPose */ \
    X( uint32_t, DrawSlot, "DrawSlot", 0 )
    DESERT_SHADER_STRUCT( GpuVisibleInstance, "GpuVisibleInstance", Std430, DESERT_GPU_VISIBLE_INSTANCE_FIELDS );
#undef DESERT_GPU_VISIBLE_INSTANCE_FIELDS
    // The top bit of GpuVisibleInstance::DrawSlot (GLSL DESERT_GPU_VISIBLE_REST_POSE): the cull drew this swaying
    // instance in its rest pose (GpuCull_CastsRestPose), the vertex stage bends it by no wind.
    inline constexpr uint32_t kVisibleRestPose = 1u << 31;
    // The next two bits (GLSL DESERT_GPU_VISIBLE_LOD_FADE_OUT / _IN): the entry is the older / the newer level of
    // a dithered LOD transition (CullViewFlags::kLodDither); the cell clips it by DesertClipLODTransition.
    inline constexpr uint32_t kVisibleLodFadeOut = 1u << 30;
    inline constexpr uint32_t kVisibleLodFadeIn  = 1u << 29;
    inline constexpr uint32_t kVisibleSlotMask   = kVisibleLodFadeIn - 1u; // the DrawSlots[] row

    // Phase 1 of the two-phase HZB cull appends here what the PREVIOUS frame's HZB rejected; phase 2 re-tests
    // them against the HZB of this frame's phase-1 depth (UE InstanceCulling two-pass occlusion).
#define DESERT_GPU_OCCLUDED_INSTANCE_FIELDS( X ) \
    X( uint32_t, Instance, "Instance", 0 ) \
    X( uint32_t, View, "View", 0 ) /* the CullView it was rejected in */
    DESERT_SHADER_STRUCT( GpuOccludedInstance, "GpuOccludedInstance", Std430,
                          DESERT_GPU_OCCLUDED_INSTANCE_FIELDS );
#undef DESERT_GPU_OCCLUDED_INSTANCE_FIELDS

    // ONE ROW PER (VIEW x PASS) a cull dispatch serves (main view, each CSM cascade, each VSM level...).
    // FrustumPlanes are the CPU Core::Frustum's six normalised planes as the view's caller built them (a zero plane
    // accepts: the VSM levels open their sun-side one), so the GPU test and the CPU IsVisibleInView test the same
    // half-spaces rather than two extractions of one matrix.
    using Vec4Array6 = std::array<glm::vec4, 6>;
    using Vec4Array12 = std::array<glm::vec4, 12>;
    using Vec4Array2 = std::array<glm::vec4, 2>;
#define DESERT_GPU_CULL_VIEW_FIELDS( X ) \
    /* unjittered; HZB phase 2 and the VSM page footprint project with it */ \
    X( glm::mat4, ViewProjection, "ViewProjection", glm::mat4( 1.0f ) ) \
    /* phase 1 projects into the previous frame's HZB with it */ \
    X( glm::mat4, PrevViewProjection, "PrevViewProjection", glm::mat4( 1.0f ) ) \
    /* Geometry::LODView::Projection (the main view's for shadows) */ \
    X( glm::mat4, LodProjection, "LodProjection", glm::mat4( 1.0f ) ) \
    /* xyz = normal, w = distance (Core::Frustum::Plane), zero = open */ \
    X( Vec4Array6, FrustumPlanes, "FrustumPlanes", {} ) \
    /* xyz = Geometry::LODView::Origin, w = the shadow wind distance (cm; caster views, \
       Common/ShadowWind.glslh; 0 elsewhere) */ \
    X( glm::vec4, LodOrigin, "LodOrigin", glm::vec4( 0.0f ) ) \
    /* xyz = ISM distance-cull origin (main camera), w = its lens scale (Graphic::DrawDistanceScale of the \
       main view) */ \
    X( glm::vec4, CullOrigin, "CullOrigin", glm::vec4( 0.0f ) ) \
    /* xy = HZB mip-0 size, z = mip count, w = 0 (no HZB: z = 0) */ \
    X( glm::uvec4, HzbExtent, "HzbExtent", glm::uvec4( 0u ) ) \
    X( uint32_t, Pass, "Pass", 0 ) /* MeshPass */ \
    X( uint32_t, Flags, "Flags", 0 ) /* CullViewFlags */ \
    /* its row of the frame's page masks (kVsmPageMask): the level (static mask), LevelCount + level for a \
       movers' layer (drawn) */ \
    X( uint32_t, VsmLevel, "VsmLevel", 0 ) \
    X( uint32_t, ViewIndex, "ViewIndex", 0 ) /* the view's index in the caller's CullViewInput list */ \
    /* THE CASTER'S REACH (MeshRenderer::ShadowWindRingOf): x = cascade c-2's split (cm), y = the light's fall \
       across / down; zero = no ring (Common/ShadowWind.glslh ShadowWindRingInner) */ \
    X( glm::vec4, CasterRing, "CasterRing", glm::vec4( 0.0f ) ) \
    /* A SCROLLED cascade's static layer (MeshRenderer::PlanCascadeCache, CSM-SCROLL-3; UE's CSM scrolling): \
       the strips its shift uncovered, strip s's six Core::Frustum planes at 6s. A caster inside FrustumPlanes \
       is kept only when it also meets one of them - the rest of the layer is its cache, copied in shifted. */ \
    X( Vec4Array12, StripPlanes, "StripPlanes", {} ) \
    X( glm::uvec4, Strips, "Strips", glm::uvec4( 0u ) ) /* x = the strips (0: the whole frustum, at most 2) */ \
    /* a content-hash view (CullViewFlags::kContentHash): x, y = the cascade's CascadePlacement::TexelOrigin, \
       z = a cell's side in texels (CascadeContentHash::kCellsPerMap cells per map side), w = its region */ \
    X( glm::ivec4, ContentCells, "ContentCells", glm::ivec4( 0 ) ) \
    /* a depth-occluder view (CullViewFlags::kDepthOccluder): x = the minimum screen radius, squared, in \
       Geometry::BoundsScreenRadiusSquared's measure (InstanceCull.shader BoundsScreenRadiusSq) */ \
    X( glm::vec4, DepthOccluder, "DepthOccluder", glm::vec4( 0.0f ) ) \
    /* THE DITHERED LOD TRANSITION's two eye samples (Geometry::LODView::Temporal; kLodDither): xyz = [0] the \
       older, [1] the newer; w = 0. Each equals LodOrigin.xyz when the view does not fade its level changes. */ \
    X( Vec4Array2, LodTemporalOrigins, "LodTemporalOrigins", {} ) \
    /* UE r.StaticMeshLODDistanceScale (CINEMATIC-LOD-GPU): the level is selected as if the instance stood at \
       its distance x this - Geometry::LODView::DistanceScale of the view's LOD eye, the one value the CPU \
       path reads (MeshRenderer::GetLODDistanceScale, Scalability ViewDistance.LODDistanceScalePercent / 100). \
       Only the level (InstanceCull.shader SelectLod): culling and the depth-occluder size read the true \
       distance. 1 = the authored screen sizes. */ \
    X( float, LodDistanceScale, "LodDistanceScale", 1.0f ) \
    X( uint32_t, LodPad0, "LodPad0", 0 ) \
    X( uint32_t, LodPad1, "LodPad1", 0 ) \
    X( uint32_t, LodPad2, "LodPad2", 0 )
    DESERT_SHADER_STRUCT( GpuCullView, "GpuCullView", Std430, DESERT_GPU_CULL_VIEW_FIELDS );
#undef DESERT_GPU_CULL_VIEW_FIELDS

    namespace CullViewFlags
    {
        inline constexpr uint32_t kOrthographic  = 1u << 0; // CSM / VSM projection
        inline constexpr uint32_t kTestHzb       = 1u << 1; // the view has an HZB (prev for phase 1, this for phase 2)
        inline constexpr uint32_t kShadowCaster  = 1u << 2; // only kCastsShadow primitives
        inline constexpr uint32_t kLodDisabled   = 1u << 3; // MeshRenderer::IsLODEnabled() == false: auto LOD is 0
        inline constexpr uint32_t kVsmPageMask   = 1u << 4; // VSM level: also the "VSM: Dirty Mask" cell test
        // A cached cascade's layers (MeshRenderer::PlanCascadeCache), per instance (ShadowWind.glslh): the static
        // layer takes the instances that stand still in the view, the movable layer those that move by themselves.
        // Neither: every caster.
        inline constexpr uint32_t kCasterStaticLayer  = 1u << 5;
        inline constexpr uint32_t kCasterMovableLayer = 1u << 6;
        // A cascade static layer's CONTENT HASH view (CPU-ISM-SHADOW; UE VSM: instance invalidation decided on the
        // GPU): culled as the static layer (with kCasterStaticLayer), it fills no bin; every instance the layer
        // would draw adds (instance row, rest pose, LOD) into the CascadeContentHash cells it covers.
        inline constexpr uint32_t kContentHash = 1u << 7;
        // THE DEPTH OCCLUDERS of the main G-buffer view (FPS-PREPASS-OCCLUDERS; UE r.EarlyZPass with
        // r.MinScreenRadiusForDepthPrepass): the main camera's G-buffer bins culled again, keeping only the
        // instances whose bounds' screen radius reaches GpuCullView::DepthOccluder.x, and none the main view draws
        // otherwise than through its bins (no bounds, a virtual-geometry primitive the main view cuts). Never an
        // HZB test (nothing held back for phase 2). MeshRenderer draws their depth twins before the masked prepass.
        inline constexpr uint32_t kDepthOccluder = 1u << 8;
        // THE DITHERED LOD TRANSITION (Geometry::SelectDitheredLODFromBounds): an instance whose two temporal
        // samples select different levels draws both in each draw slot marked kDrawSlotDitheredLod, its
        // entries tagged kVisibleLodFadeOut / kVisibleLodFadeIn. A scene view's opaque-pass views only.
        inline constexpr uint32_t kLodDither = 1u << 9;
    } // namespace CullViewFlags

    // THE CASCADE CONTENT HASH (CPU-ISM-SHADOW): per sun cascade a region of cells anchored to the map's texel
    // grid (absolute texel = framebuffer texel + (TexelOrigin.x, -TexelOrigin.y), the frame CascadePlacement::
    // ShiftTo keeps), so a box that scrolled by whole texels is answered from the cells it still covers. A cell
    // sits at (cell x mod kGrid, cell y mod kGrid); the view culls the cascade box grown by kMarginCells cells on
    // each side, so every cell it answers for is whole. An instance covering more than kMaxSpan cells on an axis
    // (or with no extent) adds into the region's overflow cell (row kGrid * kGrid), which every answer reads.
    // Per cell kWords sums of two mixes: [0], [1] of (row, rest pose), [2], [3] of (row, rest pose, LOD).
    namespace CascadeContentHash
    {
        inline constexpr uint32_t kGrid        = 16u;
        inline constexpr uint32_t kCellsPerMap = 8u;
        inline constexpr uint32_t kMarginCells = 1u;
        inline constexpr uint32_t kMaxSpan     = 4u;
        inline constexpr uint32_t kCells       = kGrid * kGrid + 1u;
        inline constexpr uint32_t kWords       = 4u;
        static_assert( kCellsPerMap + 2u * kMarginCells + 1u <= kGrid, "a grown box's cells stay distinct" );
    } // namespace CascadeContentHash

    // WHO SWAYS IN THE SHADOWS: a caster view's swaying instance (Wind.z = Strength > 0) casts its rest pose by the
    // ONE rule of Editor/Resources/Shaders/Common/ShadowWind.glslh (its bounds centre in the world against
    // LodOrigin.xyz, the main camera's position, and LodOrigin.w, the shadow wind distance), compiled as GLSL by
    // GpuCull_CastsRestPose and as C++ by the CPU walk (MeshRenderer::ShadowIsmInstances) and the cull's reference
    // (InstanceCullValidation.cpp); in a cascade c >= 2 also one nearer the camera than its ring's inner distance
    // (CasterRing, the caster's reach: its sway shadows no receiver of the cascade). A primitive with kMovableCaster
    // moves by itself in a view unless it casts its rest pose there: what a cached cascade's layers split on (InstanceCull.shader).

    // One word per (view, instance) the cull writes (InstanceCull.shader Visibility[]; GLSL
    // DESERT_GPU_CULL_VISIBILITY_*): 0 = culled before occlusion. The debug readback compares kInView + LOD with
    // the CPU rules.
    namespace CullVisibility
    {
        inline constexpr uint32_t kLodMask     = 0xFFu;
        inline constexpr uint32_t kPostVisible = 1u << 29; // phase 2 found it visible in this frame's HZB
        inline constexpr uint32_t kOccluded    = 1u << 30; // phase 1 rejected it by last frame's HZB
        inline constexpr uint32_t kInView      = 1u << 31; // passed caster filter, distance, frustum (+ VSM page mask)
    } // namespace CullVisibility
    // THE generated shader include of these rows (Tools/ShaderProtocolGen): the structs and the bit constants above,
    // read by Common/GpuScene.glslh.
    inline constexpr const ShaderProtocols::ShaderStructDecl* kGpuSceneStructs[] = {
        &kGpuAffine3x4Struct,
        &kGpuScenePrimitiveStruct,
        &kGpuSceneInstanceStruct,
        &kGpuSceneWorldBoundsStruct,
        &kGpuPrimitivePassDrawsStruct,
        &kGpuDrawSlotStruct,
        &kGpuDrawBinStruct,
        &kGpuIndirectDrawStruct,
        &kGpuVisibleInstanceStruct,
        &kGpuOccludedInstanceStruct,
        &kGpuCullViewStruct,
    };
    inline constexpr ShaderProtocols::ShaderConstantDecl kGpuSceneConstants[] = {
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_CASTS_SHADOW", PrimitiveFlags::kCastsShadow },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_WORLD_BOUNDS", PrimitiveFlags::kWorldBoundsOverride },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_SKINNED", PrimitiveFlags::kSkinned },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_NO_CONTACT_SHADOW", PrimitiveFlags::kNoContactShadow },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_SKIP_OCCLUSION", PrimitiveFlags::kSkipOcclusion },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_MOVABLE_CASTER", PrimitiveFlags::kMovableCaster },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_PREVIOUS_POSITIONS", PrimitiveFlags::kPreviousPositions },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_IMPOSTOR_SOURCE", PrimitiveFlags::kImpostorSource },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_IMPOSTOR_CARD", PrimitiveFlags::kImpostorCard },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_PRIMITIVE_NO_DECALS", PrimitiveFlags::kNoDecals },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_DRAW_EMISSION_IS_LIGHT", PrimitiveFlags::kDrawEmissionIsLight },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_ORTHOGRAPHIC", CullViewFlags::kOrthographic },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_TEST_HZB", CullViewFlags::kTestHzb },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_SHADOW_CASTER", CullViewFlags::kShadowCaster },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_LOD_DISABLED", CullViewFlags::kLodDisabled },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_VSM_PAGE_MASK", CullViewFlags::kVsmPageMask },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_CASTER_STATIC_LAYER", CullViewFlags::kCasterStaticLayer },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_CASTER_MOVABLE_LAYER", CullViewFlags::kCasterMovableLayer },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_CONTENT_HASH", CullViewFlags::kContentHash },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_DEPTH_OCCLUDER", CullViewFlags::kDepthOccluder },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VIEW_LOD_DITHER", CullViewFlags::kLodDither },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_CONTENT_HASH_GRID", CascadeContentHash::kGrid },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_CONTENT_HASH_CELLS_PER_MAP", CascadeContentHash::kCellsPerMap },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_CONTENT_HASH_MAX_SPAN", CascadeContentHash::kMaxSpan },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_CONTENT_HASH_CELLS", CascadeContentHash::kCells },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_CONTENT_HASH_WORDS", CascadeContentHash::kWords },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VISIBILITY_LOD_MASK", CullVisibility::kLodMask },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VISIBILITY_POST_VISIBLE", CullVisibility::kPostVisible },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VISIBILITY_OCCLUDED", CullVisibility::kOccluded },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_CULL_VISIBILITY_IN_VIEW", CullVisibility::kInView },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_INVALID_INDEX", kInvalidIndex },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_DRAW_SLOT_DITHERED_LOD", kDrawSlotDitheredLod },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_DRAW_SLOT_EMISSION_IS_LIGHT", kDrawSlotEmissionIsLight },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_DRAW_SLOT_SOFTWARE_RASTER", kDrawSlotSoftwareRaster },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_DRAW_SLOT_TWO_SIDED", kDrawSlotTwoSided },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_VISIBLE_REST_POSE", kVisibleRestPose },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_VISIBLE_LOD_FADE_OUT", kVisibleLodFadeOut },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_VISIBLE_LOD_FADE_IN", kVisibleLodFadeIn },
        { ShaderProtocols::ShaderConstantDecl::Type::Uint, "DESERT_GPU_VISIBLE_SLOT_MASK", kVisibleSlotMask },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kGpuSceneShaderInclude{
        .Path         = "Generated/GpuDriven/GpuScene.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GpuDriven/GpuSceneLayout.hpp",
        .Structs      = kGpuSceneStructs,
        .Constants    = kGpuSceneConstants,
    };
} // namespace Desert::Graphic::GpuDriven
