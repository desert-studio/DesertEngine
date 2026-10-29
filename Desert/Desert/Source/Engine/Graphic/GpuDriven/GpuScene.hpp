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
//  - ONE per scene (Core::Scene owns its GpuSceneRegistry beside its renderer state; Scene::Clear re-creates
//    it), NOT per view: every view, every pass and every consumer (velocity, RT gather, surface cache, culling)
//    reads the same rows (written by MeshRenderer::SyncGpuScene through GpuSceneRegistry). The previous world of a
//    primitive is PrevLocalToWorld in its row, one for every view of the scene.
//  - Indices are stable for the primitive's life (free list; UE FGPUScene::AllocatePrimitive / FreeIndices).
//  - Updates are DELTAS: the CPU marks rows dirty, AddUploadPass scatters only those rows into the persistent
//    buffers in-graph (UE FGPUScene::Update + FGPUScatterUploadBuffer). A frame with no change uploads nothing.
//  - PrevLocalToWorld rule (UE "velocity" list): SetTransform writes Prev := the old LocalToWorld; BeginFrame
//    re-dirties every primitive that moved LAST frame and did not move this frame with Prev := LocalToWorld, so a
//    primitive that stops reports zero motion one frame later. A primitive added this frame has Prev == World.
//  - Skinned palettes change every frame: SetSkinning writes the row's Bone/PrevBone offsets into the scene's
//    per-frame ObjectBones buffer (one for all views of the scene).
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
        RDG::BufferRef MaterialRows;   // vec4[] — the scene material table (AddMaterialRow)
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

        // THE SCENE MATERIAL TABLE (UE: one material uniform buffer per FMaterialRenderProxy; here one stable row
        // per material instance in ONE buffer that every mesh cell binds as its `Materials` block). A row is the
        // instance's effective MaterialParamRow: N vec4 slots, N = its template's RowStride / 16. Its first slot is
        // a multiple of N, so the returned index (first slot / N) is exactly what the generated shader reads,
        // `MaterialRows[MaterialIndex]` at that template's own stride (DShaderParser) — templates of different
        // strides share the buffer without a second indexing rule. The draw's MaterialIndex (push, and
        // GpuDrawSlot::MaterialRow for the cached draws) is this index. @p row must not be empty: a template
        // without parameters declares no Materials block and takes no row (GpuSceneRegistry::SyncMaterialRow).
        [[nodiscard]] uint32_t AddMaterialRow( std::span<const glm::vec4> row );
        // New values for a row of the same length (a parameter changed); only its slots go up.
        void UpdateMaterialRow( uint32_t row, std::span<const glm::vec4> values );
        // The row's slots return to the table's free list (@p slots = its length, the N it was added with).
        void RemoveMaterialRow( uint32_t row, uint32_t slots );

        // Frame boundary: the primitives that moved last frame and not since get Prev := LocalToWorld (dirty).
        void BeginFrame();
        // The frame since the last BeginFrame never executed (FrameFault, GpuSceneRegistry): every transform it
        // and its BeginFrame changed goes back, and so does the list of primitives that moved in the frame before
        // it - as if it never happened. A primitive it added
        // stays, with Prev == World; the caller removes it.
        void DiscardFrame();

        // Rows or palettes changed since the last upload: AddUploadPass has passes to add.
        [[nodiscard]] bool HasPendingUpload() const;

        // "GPUScene: Upload": scatters the dirty rows into the persistent buffers (grown with a copy when the
        // high-water mark passes the capacity) and returns their graph handles. Refused when the device buffer
        // cannot be grown.
        // The graph leaves every table in StorageRead: the passes that read them through a material's
        // descriptor (binding Core::kGpuScenePrimitivesBinding / kObjectBonesBinding, SceneFrameBinding) are
        // ordered after the scatter by that final state, the next frame's scatter after their reads.
        [[nodiscard]] Common::ResultStr<GpuSceneBuffers> AddUploadPass( RDG::Builder& graph );

        // The tables as they stand after the last upload, registered in @p graph (a view graph: the upload runs in
        // its own graph ahead of it, MeshRenderer::SyncGpuScene), every one left in StorageRead. Refused before the
        // first upload (no device table yet) or while rows wait for one.
        [[nodiscard]] Common::ResultStr<GpuSceneBuffers> ImportTables( RDG::Builder& graph ) const;

        // The CPU images the device tables were uploaded from (the debug readback's reference, InstanceCulling
        // validation): equal to the device content whenever HasPendingUpload() is false.
        [[nodiscard]] std::span<const GpuScenePrimitive> AllPrimitives() const
        {
            return m_Primitives;
        }
        [[nodiscard]] std::span<const GpuSceneInstance> AllInstances() const
        {
            return m_Instances;
        }
        [[nodiscard]] std::span<const float> LodScreenSizes() const
        {
            return m_LodScreenSizes;
        }
        [[nodiscard]] std::span<const GpuSceneWorldBounds> WorldBounds() const
        {
            return m_WorldBounds;
        }

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
        // The scene material table (AddMaterialRow): what a mesh cell's `Materials` block is lent (LendViewBuffer).
        // Null until the first upload.
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetMaterialsBuffer() const;

        // One persistent device table and its CPU image's upload state (implementation detail of the upload).
        struct Table;

    private:
        void MarkPrimitive( PrimitiveIndex primitive );
        // Records the primitive's transforms the first time this frame changes them (DiscardFrame's undo).
        void JournalTransform( PrimitiveIndex primitive );
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
        // DiscardFrame's undo of the frame since the last BeginFrame: each changed primitive's transforms before
        // the frame (m_UndoMark: already recorded) and the moved list BeginFrame consumed.
        struct TransformUndo
        {
            PrimitiveIndex Primitive = kInvalidIndex;
            GpuAffine3x4   LocalToWorld{};
            GpuAffine3x4   PrevLocalToWorld{};
        };
        std::vector<TransformUndo>  m_FrameUndo;
        std::vector<uint8_t>        m_UndoMark;
        std::vector<PrimitiveIndex> m_UndoMovedLastFrame;
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
        // The scene material table's CPU image (vec4 slots), its free slot ranges and the slots changed since the
        // last upload.
        std::vector<glm::vec4>  m_MaterialSlots;
        std::vector<glm::uvec2> m_FreeMaterialRanges;
        std::vector<uint32_t>   m_DirtyMaterialSlots;

        std::unique_ptr<Table> m_PrimitiveTable;
        std::unique_ptr<Table> m_InstanceTable;
        std::unique_ptr<Table> m_LodTable;
        std::unique_ptr<Table> m_WorldBoundsTable;
        std::unique_ptr<Table> m_BonesTable;
        std::unique_ptr<Table> m_MaterialTable;
        std::unique_ptr<struct GpuSceneUploadPipeline> m_Upload;
    };
} // namespace Desert::Graphic::GpuDriven
