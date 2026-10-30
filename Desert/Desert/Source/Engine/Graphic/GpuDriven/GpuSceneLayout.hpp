#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

// GPU-DRIVEN contract (C0): the std430 rows of the scene's persistent GPU tables, their GLSL twins in
// Editor/Resources/Shaders/Common/GpuScene.glslh. Every offset below is pinned by a static_assert and spelled in
// a comment of the twin; a change to one side without the other is a layout break the static_asserts and the
// twin's offsets name.
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
        // maps: movable primitives go to the dynamic depths). The cull's layer flags below split on it.
        inline constexpr uint32_t kMovableCaster = 1u << 5;
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
    struct GpuAffine3x4
    {
        glm::vec4 Rows[3]{ glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ), glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f ),
                           glm::vec4( 0.0f, 0.0f, 1.0f, 0.0f ) };

        bool operator==( const GpuAffine3x4& ) const = default;
    };
    static_assert( sizeof( GpuAffine3x4 ) == 48, "std430 twin: three vec4 rows (Common/GpuScene.glslh)" );

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
    struct GpuScenePrimitive
    {
        GpuAffine3x4 LocalToWorld{};     // 0
        GpuAffine3x4 PrevLocalToWorld{}; // 48  — == LocalToWorld the frame after the primitive stopped moving
        // Mesh-space bounds of the whole mesh (Geometry::LocalBounds): xyz centre / extent, cm.
        // LocalBoundsCenter.w = how far wind moves vertices past the bounds (cm, today
        // MeshDrawCommand::BoundsPad). LocalBoundsExtent.w = 0.
        glm::vec4 LocalBoundsCenter{ 0.0f }; // 96
        glm::vec4 LocalBoundsExtent{ 0.0f }; // 112
        uint32_t  FirstInstance      = 0;    // 128 — its instances: Instances[FirstInstance, +InstanceCount)
        uint32_t  InstanceCount      = 0;    // 132 — 1 for a non-instanced primitive
        uint32_t  FirstLodScreenSize = 0; // 136 — LodScreenSizes[FirstLodScreenSize, +LodCount) (UE screen size)
        uint32_t  LodCount           = 0; // 140 — 1 = no LOD chain
        int32_t   LodBias            = 0; // 144 — Geometry::SelectLODFromBounds lodBias
        int32_t   ForcedLod          = -1; // 148 — Geometry::SelectLODFromBounds forcedLOD (-1 = auto)
        uint32_t  Flags              = 0;  // 152 — PrimitiveFlags
        uint32_t  Entity             = 0;  // 156 — the owning entity (picking, debug views, RT hit records)
        // Skinned (kSkinned): this frame's / the previous frame's palette in the scene's per-frame ObjectBones
        // buffer; the GPU skin cache's streams (RayTracingSkinCache) when it is on, 0 = skin in the vertex stage.
        uint32_t BoneOffset           = 0; // 160
        uint32_t PrevBoneOffset       = 0; // 164
        uint32_t SkinCacheAddressLo   = 0; // 168
        uint32_t SkinCacheAddressHi   = 0; // 172
        uint32_t SkinCacheVertexCount = 0; // 176
        // The foliage wind of an instanced primitive (Graphic::InstanceWind; the binned instanced vertex stage
        // reads it here, where the CPU-recorded draw pushed it, Graphic::PackViewInstanceWind): Speed and both
        // clocks, then (dir.x, dir.z, Strength, Height). Strength 0 = does not sway (every other primitive).
        float     WindSpeed       = 0.0f; // 180
        float     WindSeconds     = 0.0f; // 184
        float     WindPrevSeconds = 0.0f; // 188
        glm::vec4 Wind{ 0.0f };           // 192
        // std430 struct-array stride: 208 = 13 x 16. The per-pass draws live in PassDraws[primitive], not here.
    };
    static_assert( sizeof( GpuScenePrimitive ) == 208, "std430 twin in Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, WindSpeed ) == 180, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, Wind ) == 192, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, PrevLocalToWorld ) == 48, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, LocalBoundsCenter ) == 96, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, FirstInstance ) == 128, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, Flags ) == 152, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, BoneOffset ) == 160, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuScenePrimitive, SkinCacheVertexCount ) == 176, "Common/GpuScene.glslh" );

    // ONE ROW PER INSTANCE: a non-instanced primitive owns exactly one (identity InstanceToPrimitive), an ISM one
    // per placed instance (today CPU CollectIsmInstances, InstanceCullDistance.hpp:97). World = primitive's
    // LocalToWorld x InstanceToPrimitive; previous world = PrevLocalToWorld x InstanceToPrimitive (an instance
    // that moves inside its component re-uploads with its primitive's Prev: UE keeps instance prev-transforms the
    // same way, through the primitive).
    struct GpuSceneInstance
    {
        GpuAffine3x4 InstanceToPrimitive{};     // 0
        uint32_t     Primitive = kInvalidIndex; // 48 — its row in Primitives[]
        // Distance cull of the ISM component (InstanceCullDistance.hpp), cm from the view origin; 0/0 = none.
        float CullDistanceStart = 0.0f; // 52
        float CullDistanceEnd   = 0.0f; // 56
        // Pose bounds when the primitive has kWorldBoundsOverride: the row in WorldBoundsOverrides[]
        // (kInvalidIndex otherwise). Today MeshDrawCommand::WorldBounds (SkinnedPoseBounds).
        uint32_t WorldBoundsRow = kInvalidIndex; // 60
    };
    static_assert( sizeof( GpuSceneInstance ) == 64, "std430 twin in Common/GpuScene.glslh" );
    static_assert( offsetof( GpuSceneInstance, Primitive ) == 48, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuSceneInstance, WorldBoundsRow ) == 60, "Common/GpuScene.glslh" );

    // A world-space box that replaces LocalBounds x InstanceToWorld for one instance (skinned pose this frame).
    struct GpuSceneWorldBounds
    {
        glm::vec4 Min{ 0.0f }; // 0  w = 0
        glm::vec4 Max{ 0.0f }; // 16 w = 0
    };
    static_assert( sizeof( GpuSceneWorldBounds ) == 32, "std430 twin in Common/GpuScene.glslh" );

    // ONE ROW PER (PRIMITIVE, PASS): where the primitive's draw slots of that pass start. Slot of (lod, submesh)
    // = FirstDrawSlot + lod * SubmeshCount + submesh. SubmeshCount 0 = the primitive does not draw in the pass.
    struct GpuPrimitivePassDraws
    {
        uint32_t FirstDrawSlot = 0; // 0
        uint32_t SubmeshCount  = 0; // 4
        uint32_t LodCount      = 0; // 8 — the levels that HAVE slots (a submesh without LODs repeats level 0)
        uint32_t Pad           = 0; // 12
    };
    static_assert( sizeof( GpuPrimitivePassDraws ) == 16, "std430 twin in Common/GpuScene.glslh" );

    // ONE ROW PER CACHED DRAW of a pass (DrawCommandCache: a primitive's submesh at one LOD): the bin its
    // instances are counted into, and the per-draw state that today rides the push block (MaterialIndex,
    // PrimitiveFlags; Common/MaterialTransport.glslh) or the BindState closure (MeshRenderer.hpp:89-92).
    struct GpuDrawSlot
    {
        // Its row in the pass's DrawBins[] / IndirectDraws[]; kInvalidIndex = the submesh does not draw in this pass
        // (hidden, or no pipeline for its material there): the cull skips the slot.
        uint32_t Bin         = 0; // 0
        // Its section's row in the scene material table (GpuScene::AddMaterialRow, GpuSceneRegistry::SyncMaterialRow;
        // MeshRenderer::StaticMeshRenderData::MaterialRow): the index the shader reads as MaterialRows[MaterialIndex].
        uint32_t MaterialRow = 0; // 4
        // Its row in DrawCommandCache::SubmeshTransforms[]: the submesh's node transform relative to the
        // primitive (Submesh::Transform). World = LocalToWorld x InstanceToPrimitive x this.
        uint32_t SubmeshTransform = 0; // 8
        uint32_t Pad1             = 0; // 12
    };
    static_assert( sizeof( GpuDrawSlot ) == 16, "std430 twin in Common/GpuScene.glslh" );
    static_assert( offsetof( GpuDrawSlot, SubmeshTransform ) == 8, "Common/GpuScene.glslh" );

    // ONE ROW PER BIN of a pass (pass x pipeline x mesh x submesh x LOD): the index range it draws and where its
    // compacted instance list lives. The cull writes the bin's VkDrawIndexedIndirectCommand from it.
    struct GpuDrawBin
    {
        uint32_t IndexCount   = 0; // 0  — SubmeshIndices( submesh, lod ).Count (MeshDrawArguments.hpp:37)
        uint32_t FirstIndex   = 0; // 4  — SubmeshIndices( submesh, lod ).First
        int32_t  VertexOffset = 0; // 8
        uint32_t FirstVisible = 0; // 12 — its range in VisibleInstances[]: [FirstVisible, +Capacity)
        uint32_t Capacity     = 0; // 16 — the most instances that can land in it (sum of its slots' instances)
        uint32_t Group        = 0; // 20 — its IndirectGroup (pipeline x geometry buffers) of the pass
        uint32_t Pad0         = 0; // 24
        uint32_t Pad1         = 0; // 28
    };
    static_assert( sizeof( GpuDrawBin ) == 32, "std430 twin in Common/GpuScene.glslh" );

    // ONE ROW PER BIN, written by the cull, read by the indirect draw: VkDrawIndexedIndirectCommand (20 bytes) +
    // the bin it belongs to, at a 32-byte stride. ONE table on every platform (IndirectDrawSubmission.hpp).
    struct GpuIndirectDraw
    {
        uint32_t IndexCount    = 0; // 0  VkDrawIndexedIndirectCommand::indexCount
        uint32_t InstanceCount = 0; // 4  ::instanceCount — 0 for a bin the cull left empty
        uint32_t FirstIndex    = 0; // 8  ::firstIndex
        int32_t  VertexOffset  = 0; // 12 ::vertexOffset
        uint32_t FirstInstance =
             0; // 16 ::firstInstance = the bin's FirstVisible (gl_InstanceIndex indexes VisibleInstances)
        uint32_t Bin  = 0; // 20 — which bin (the count path compacts rows, so the row is not the bin)
        uint32_t Pad0 = 0; // 24
        uint32_t Pad1 = 0; // 28
    };
    static_assert( sizeof( GpuIndirectDraw ) == 32, "std430 twin in Common/GpuScene.glslh" );
    static_assert( offsetof( GpuIndirectDraw, FirstInstance ) == 16, "VkDrawIndexedIndirectCommand prefix" );
    inline constexpr uint32_t kIndirectDrawStride = sizeof( GpuIndirectDraw );

    // ONE ENTRY PER SURVIVING (instance, draw slot) of a bin, at VisibleInstances[gl_InstanceIndex]: what the
    // vertex stage fetches instead of the push block's Transform / MaterialIndex / PrimitiveIndex.
    struct GpuVisibleInstance
    {
        uint32_t Instance = 0; // 0 — its row in Instances[] (-> Primitives[Instance.Primitive])
        uint32_t DrawSlot = 0; // 4 — its row in the pass's DrawSlots[] (-> MaterialRow)
    };
    static_assert( sizeof( GpuVisibleInstance ) == 8, "std430 twin in Common/GpuScene.glslh" );

    // Phase 1 of the two-phase HZB cull appends here what the PREVIOUS frame's HZB rejected; phase 2 re-tests
    // them against the HZB of this frame's phase-1 depth (UE InstanceCulling two-pass occlusion).
    struct GpuOccludedInstance
    {
        uint32_t Instance = 0; // 0
        uint32_t View     = 0; // 4 — the CullView it was rejected in
    };
    static_assert( sizeof( GpuOccludedInstance ) == 8, "std430 twin in Common/GpuScene.glslh" );

    // ONE ROW PER (VIEW x PASS) a cull dispatch serves (main view, each CSM cascade, each VSM level...).
    // FrustumPlanes are the CPU Core::Frustum's six normalised planes as the view's caller built them (a zero plane
    // accepts: the VSM levels open their sun-side one), so the GPU test and the CPU IsVisibleInView test the same
    // half-spaces rather than two extractions of one matrix.
    struct GpuCullView
    {
        glm::mat4  ViewProjection{ 1.0f };     // 0   unjittered; HZB phase 2 and the VSM page footprint project with it
        glm::mat4  PrevViewProjection{ 1.0f }; // 64  phase 1 projects into the previous frame's HZB with it
        glm::mat4  LodProjection{ 1.0f };      // 128 Geometry::LODView::Projection (the main view's for shadows)
        glm::vec4  FrustumPlanes[6]{};         // 192 xyz = normal, w = distance (Core::Frustum::Plane), zero = open
        glm::vec4  LodOrigin{ 0.0f };          // 288 xyz = Geometry::LODView::Origin, w = 0
        glm::vec4  CullOrigin{ 0.0f };         // 304 xyz = ISM distance-cull origin (main camera), w = its lens scale
                                               //     (Graphic::DrawDistanceScale of the main view)
        glm::uvec4 HzbExtent{ 0u };            // 320 xy = HZB mip-0 size, z = mip count, w = 0 (no HZB: z = 0)
        uint32_t   Pass      = 0;              // 336 — MeshPass
        uint32_t   Flags     = 0;              // 340 — CullViewFlags
        uint32_t   VsmLevel  = 0;              // 344 — the clipmap level (kVsmPageMask)
        uint32_t   ViewIndex = 0;              // 348 — the view's index in the caller's CullViewInput list
    };
    static_assert( sizeof( GpuCullView ) == 352, "std430 twin in Common/GpuScene.glslh" );
    static_assert( offsetof( GpuCullView, FrustumPlanes ) == 192, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuCullView, HzbExtent ) == 320, "Common/GpuScene.glslh" );
    static_assert( offsetof( GpuCullView, ViewIndex ) == 348, "Common/GpuScene.glslh" );

    namespace CullViewFlags
    {
        inline constexpr uint32_t kOrthographic  = 1u << 0; // CSM / VSM projection
        inline constexpr uint32_t kTestHzb       = 1u << 1; // the view has an HZB (prev for phase 1, this for phase 2)
        inline constexpr uint32_t kShadowCaster  = 1u << 2; // only kCastsShadow primitives
        inline constexpr uint32_t kLodDisabled   = 1u << 3; // MeshRenderer::IsLODEnabled() == false: auto LOD is 0
        inline constexpr uint32_t kVsmPageMask   = 1u << 4; // VSM level: also the "VSM: Dirty Mask" cell test
        // A cached cascade's layers (MeshRenderer::PlanCascadeCache): the static layer takes only primitives
        // without PrimitiveFlags::kMovableCaster, the movable layer only those with it. Neither: every caster.
        inline constexpr uint32_t kCasterStaticLayer  = 1u << 5;
        inline constexpr uint32_t kCasterMovableLayer = 1u << 6;
    } // namespace CullViewFlags

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
} // namespace Desert::Graphic::GpuDriven
