#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::ShaderResources
{
    class StorageBuffer;
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
        GpuScene();
        ~GpuScene();
        GpuScene( const GpuScene& )            = delete;
        GpuScene& operator=( const GpuScene& ) = delete;

        // Registers a primitive and its instances; their rows are dirty until the next upload.
        [[nodiscard]] PrimitiveIndex AddPrimitive( const PrimitiveDesc& desc );
        // The mesh, LOD chain or instance set changed: same index, rows rewritten (instances re-allocated when
        // the count changes). DrawCommandCache is told by the caller (its slots name LODs and submeshes).
        // LocalToWorld follows SetTransform's rule (Prev := the old world when it changed).
        void UpdatePrimitive( PrimitiveIndex primitive, const PrimitiveDesc& desc );
        void RemovePrimitive( PrimitiveIndex primitive );
        // A live primitive (added and not removed).
        [[nodiscard]] bool IsValid( PrimitiveIndex primitive ) const;

        // The component moved: LocalToWorld := world, PrevLocalToWorld := the old LocalToWorld. The same world
        // again is no change (nothing is dirtied), so every view of a frame may report it.
        void SetTransform( PrimitiveIndex primitive, const glm::mat4& world );
        // Skinned, every frame it is drawn.
        void SetSkinning( PrimitiveIndex primitive, const PrimitiveSkinning& skinning );
        // The GPU skin cache's streams of a skinned primitive (RayTracingSkinCache::PrepareRows / SkinFrame);
        // address 0 = skin in the vertex stage. Leaves the palette offsets and the pose box alone.
        void SetSkinCache( PrimitiveIndex primitive, uint64_t address, uint32_t vertexCount );
        // This frame's palettes of every skinned primitive, end to end (the bytes SetSkinning's offsets index).
        void SetBonePalettes( std::span<const glm::mat4> palettes );

        // Frame boundary: the primitives that moved last frame and not since get Prev := LocalToWorld (dirty).
        void BeginFrame();

        // Rows or palettes changed since the last upload: AddUploadPass has passes to add.
        [[nodiscard]] bool HasPendingUpload() const;

        // "GPUScene: Upload": scatters the dirty rows into the persistent buffers (grown with a copy when the
        // high-water mark passes the capacity) and returns their graph handles. Refused when the device buffer
        // cannot be grown.
        // The graph leaves every table in StorageRead: the passes that read them through a material's
        // descriptor (binding Core::kGpuScenePrimitivesBinding / kObjectBonesBinding, SceneFrameBinding) are
        // ordered after the scatter by that final state, the next frame's scatter after their reads.
        [[nodiscard]] Common::ResultStr<GpuSceneBuffers> AddUploadPass( RDG::Builder& graph );

        [[nodiscard]] const GpuScenePrimitive&          Primitive( PrimitiveIndex primitive ) const;
        [[nodiscard]] std::span<const GpuSceneInstance> Instances( PrimitiveIndex primitive ) const;
        [[nodiscard]] uint32_t                          PrimitiveCount() const // high-water mark
        {
            return static_cast<uint32_t>( m_Primitives.size() );
        }

        // The persistent device tables, for the readers that bind them by descriptor rather than by graph ref:
        // the view-pass surface cells (binding 25 / 26, SceneFrameBinding) and the skin step's import. Null until
        // the first upload.
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetPrimitivesBuffer() const;
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetBonesBuffer() const;

        // One persistent device table and its CPU image's upload state (implementation detail of the upload).
        struct Table;

    private:
        void MarkPrimitive( PrimitiveIndex primitive );
        void WriteInstances( PrimitiveIndex primitive, const PrimitiveDesc& desc );
        void WriteLodScreenSizes( PrimitiveIndex primitive, std::span<const float> sizes );

        std::vector<GpuScenePrimitive>   m_Primitives;
        std::vector<GpuSceneInstance>    m_Instances;
        std::vector<float>               m_LodScreenSizes;
        std::vector<PrimitiveIndex>      m_FreePrimitives;
        std::vector<PrimitiveIndex>      m_DirtyPrimitives; // UE FGPUScene::PrimitivesToUpdate
        std::vector<InstanceIndex>       m_DirtyInstances;
        // The primitives SetTransform moved since the last BeginFrame: the next BeginFrame gives them
        // Prev := LocalToWorld (m_MovedMark: already listed).
        std::vector<PrimitiveIndex>      m_MovedLastFrame;
        std::vector<uint8_t>             m_MovedMark;
        std::vector<GpuSceneWorldBounds> m_WorldBounds;
        std::vector<glm::mat4>           m_BonePalettes;
        // Per primitive: live, its pose-box row in m_WorldBounds (kInvalidIndex = none).
        std::vector<uint8_t>  m_Live;
        std::vector<uint32_t> m_WorldBoundsRow;
        std::vector<uint32_t> m_FreeWorldBounds;
        // Free [first, first + count) ranges of Instances[] / LodScreenSizes[] (first fit, merged on free).
        std::vector<glm::uvec2> m_FreeInstanceRanges;
        std::vector<glm::uvec2> m_FreeLodRanges;
        std::vector<uint32_t>   m_DirtyLodScreenSizes;
        std::vector<uint32_t>   m_DirtyWorldBounds;
        bool                    m_BonesDirty = false;

        std::unique_ptr<Table> m_PrimitiveTable;
        std::unique_ptr<Table> m_InstanceTable;
        std::unique_ptr<Table> m_LodTable;
        std::unique_ptr<Table> m_WorldBoundsTable;
        std::unique_ptr<Table> m_BonesTable;
        std::unique_ptr<struct GpuSceneUploadPipeline> m_Upload;
    };
} // namespace Desert::Graphic::GpuDriven
