#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic::RDG
{
    class Builder;
}

// GPU-DRIVEN contract (C0) — GpuScene: THE SCENE'S PERSISTENT GPU TABLES (UE FGPUScene,
// Renderer/Private/GPUScene.cpp).
//
// Responsibility:
//  - ONE per scene (owned beside the scene's renderer state, re-created with ViewInputs::SceneIdentity), NOT per
//    view: every view, every pass and every consumer (velocity, RT gather, surface cache, culling) reads the same
//    rows. Replaces the per-(frame x view) ObjectMotions rows (View/SceneViewState.hpp:182 GpuObjectMotion,
//    View/ObjectMotionRows.hpp:29 BuildObjectMotionRows, MeshRenderer::BuildObjectMotions MeshRenderer.hpp:442)
//    and the per-view MotionHistory maps (SceneViewState.hpp:171-174): PrevLocalToWorld lives in the row.
//  - Indices are stable for the primitive's life (free list; UE FGPUScene::AllocatePrimitive / FreeIndices).
//  - Updates are DELTAS: the CPU marks rows dirty, AddUploadPass scatters only those rows into the persistent
//    buffers in-graph (UE FGPUScene::Update + FGPUScatterUploadBuffer). A frame with no change uploads nothing.
//  - PrevLocalToWorld rule (UE "velocity" list): SetTransform writes Prev := the old LocalToWorld; BeginFrame
//    re-dirties every primitive that moved LAST frame and did not move this frame with Prev := LocalToWorld, so a
//    primitive that stops reports zero motion one frame later. A primitive added this frame has Prev == World.
//  - Skinned palettes change every frame: SetSkinning writes the row's Bone/PrevBone offsets into the scene's
//    per-frame ObjectBones buffer (one for all views — today it is per view, MeshRenderer.hpp:442).
//
// Not here: which passes a primitive draws in and with which pipeline/material (DrawCommandCache), what is visible
// (InstanceCulling), how the draws are issued (IndirectDrawSubmission).
namespace Desert::Graphic::GpuDriven
{
    // What a mesh component hands the scene when it is registered or its shape changes (mesh, LOD chain,
    // instances).
    struct PrimitiveDesc
    {
        const Mesh* Mesh = nullptr;
        glm::mat4   LocalToWorld{ 1.0f };
        // ISM: the instances' transforms relative to the component; empty = one identity instance.
        std::span<const glm::mat4> InstanceToPrimitive{};
        // Geometry::LocalBounds of the mesh (cm) and the wind's reach past it (today MeshDrawCommand::BoundsPad).
        glm::vec3 LocalBoundsMin{ 0.0f };
        glm::vec3 LocalBoundsMax{ 0.0f };
        float     BoundsPad = 0.0f;
        // The mesh's LOD screen sizes (Geometry::SelectLODFromBounds screenSizes), bias and forced level.
        std::span<const float> LodScreenSizes{};
        int32_t                LodBias           = 0;
        int32_t                ForcedLod         = -1;
        float                  CullDistanceStart = 0.0f; // ISM component's distance cull, cm; 0/0 = none
        float                  CullDistanceEnd   = 0.0f;
        uint32_t               Flags             = 0; // PrimitiveFlags
        uint32_t               Entity            = 0;
    };

    // A skinned primitive's per-frame rows: where this frame's and the previous frame's palette start in the
    // scene's ObjectBones buffer, the skin cache streams when on, and the posed world box for culling.
    struct PrimitiveSkinning
    {
        uint32_t                           BoneOffset           = 0;
        uint32_t                           PrevBoneOffset       = 0;
        uint64_t                           SkinCacheAddress     = 0; // 0 = skin in the vertex stage
        uint32_t                           SkinCacheVertexCount = 0;
        std::optional<GpuSceneWorldBounds> PoseBounds{};
    };

    // The graph handles of the tables after this frame's upload; every reader declares its read of these.
    struct GpuSceneBuffers
    {
        RDG::BufferRef Primitives;     // GpuScenePrimitive[]
        RDG::BufferRef Instances;      // GpuSceneInstance[]
        RDG::BufferRef LodScreenSizes; // float[]
        RDG::BufferRef WorldBounds;    // GpuSceneWorldBounds[] (this frame's pose boxes)
        RDG::BufferRef ObjectBones;    // mat4[] — this frame's and last frame's palettes, all skinned primitives
        uint32_t       PrimitiveCount = 0; // high-water mark: the dispatch range of a full-scene cull
        uint32_t       InstanceCount  = 0;
    };

    class GpuScene
    {
    public:
        // Registers a primitive and its instances; their rows are dirty until the next upload.
        [[nodiscard]] PrimitiveIndex AddPrimitive( const PrimitiveDesc& desc );
        // The mesh, LOD chain or instance set changed: same index, rows rewritten (instances re-allocated when
        // the count changes). DrawCommandCache is told by the caller (its slots name LODs and submeshes).
        void UpdatePrimitive( PrimitiveIndex primitive, const PrimitiveDesc& desc );
        void RemovePrimitive( PrimitiveIndex primitive );

        // The component moved: LocalToWorld := world, PrevLocalToWorld := the old LocalToWorld.
        void SetTransform( PrimitiveIndex primitive, const glm::mat4& world );
        // Skinned, every frame it is drawn.
        void SetSkinning( PrimitiveIndex primitive, const PrimitiveSkinning& skinning );
        // This frame's palettes of every skinned primitive, end to end (the bytes SetSkinning's offsets index).
        void SetBonePalettes( std::span<const glm::mat4> palettes );

        // Frame boundary: the primitives that moved last frame and not since get Prev := LocalToWorld (dirty).
        void BeginFrame();

        // "GPUScene: Upload": scatters the dirty rows into the persistent buffers (grown with a copy when the
        // high-water mark passes the capacity) and returns their graph handles. Refused when the device buffer
        // cannot be grown.
        [[nodiscard]] Common::ResultStr<GpuSceneBuffers> AddUploadPass( RDG::Builder& graph );

        [[nodiscard]] const GpuScenePrimitive&          Primitive( PrimitiveIndex primitive ) const;
        [[nodiscard]] std::span<const GpuSceneInstance> Instances( PrimitiveIndex primitive ) const;

    private:
        std::vector<GpuScenePrimitive>   m_Primitives;
        std::vector<GpuSceneInstance>    m_Instances;
        std::vector<float>               m_LodScreenSizes;
        std::vector<PrimitiveIndex>      m_FreePrimitives;
        std::vector<PrimitiveIndex>      m_DirtyPrimitives; // UE FGPUScene::PrimitivesToUpdate
        std::vector<InstanceIndex>       m_DirtyInstances;
        std::vector<PrimitiveIndex>      m_MovedLastFrame; // BeginFrame's Prev := LocalToWorld list
        std::vector<GpuSceneWorldBounds> m_WorldBounds;
        std::vector<glm::mat4>           m_BonePalettes;
    };
} // namespace Desert::Graphic::GpuDriven
