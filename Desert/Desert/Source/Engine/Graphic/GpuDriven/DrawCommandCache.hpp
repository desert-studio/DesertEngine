#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic
{
    class GraphicsPipeline;
    class VertexBuffer;
    class IndexBuffer;
} // namespace Desert::Graphic

namespace Desert::Graphic::RDG
{
    class Builder;
}

// GPU-DRIVEN contract (C0) — DrawCommandCache: THE SCENE'S CACHED DRAWS, BINNED (UE FCachedMeshDrawCommands /
// FCachedPassMeshDrawList, Renderer/Private/MeshDrawCommands.cpp, and the bins of InstanceCullingContext).
//
// Responsibility:
//  - Built when the SCENE changes (a primitive added/removed, its mesh, LODs or materials changed, a pipeline
//    reloaded), never per frame. Replaces the per-frame MeshDrawCommand / MeshDrawList rebuild
//    (Systems/Scene/Mesh/MeshRenderer.hpp:71-104, Record MeshRenderer.cpp:381-427, pass loops
//    MeshRendererDeferred.cpp:8-56, MeshRendererForward.cpp:103, MeshRendererShadow.cpp:670,
//    MeshRendererVirtualShadow.cpp:360-441).
//  - The per-draw std::function BindState (MeshRenderer.hpp:89-92: transform push, material row, bone offset,
//    wind, instance descriptor bind) becomes DATA: GpuDrawSlot (material row) + GpuScenePrimitive/Instance
//    (transforms, bones, skin cache) read by instance id. A draw keeps no closure.
//  - Bin = pass x pipeline x mesh x submesh x LOD: every instance in a bin draws the same index range with the
//    same PSO, so ONE VkDrawIndexedIndirectCommand serves all of them. Bins of one pass are sorted by
//    IndirectGroup = pipeline x geometry buffers, so a group is ONE multi-draw-indirect call.
//    UE issues one indirect draw per cached mesh draw command; grouping by PSO x geometry is ours (fewer calls).
//  - Materials are bindless (textures through BindlessTextureTable, parameters through the Materials[] row), so
//    the group key carries no per-material descriptor set.
//
// Not here: the instances' rows (GpuScene), which bins are non-empty this frame (InstanceCulling).
namespace Desert::Graphic::GpuDriven
{
    // The mesh passes that draw on the GPU-driven path. Translucency is sorted back-to-front per view and is NOT
    // a binned pass (UE keeps translucency on its own sorted list too).
    enum class MeshPass : uint32_t
    {
        DepthPrepass,
        GBuffer,
        Forward,
        ShadowCascade,
        VirtualShadow,
        Count
    };
    inline constexpr uint32_t kMeshPassCount = static_cast<uint32_t>( MeshPass::Count );

    // One multi-draw call: the bins [FirstBin, +BinCount) of a pass share these.
    struct IndirectGroup
    {
        const GraphicsPipeline* Pipeline = nullptr; // owned by the SceneRenderer's PipelineCache
        const VertexBuffer*     Vertices = nullptr;
        const IndexBuffer*      Indices  = nullptr;
        uint32_t                FirstBin = 0;
        uint32_t                BinCount = 0;
    };

    // One pass's cached state: what the cull and the draw need, as host mirrors of the GPU tables.
    struct PassDrawTable
    {
        std::vector<GpuPrimitivePassDraws> PrimitiveDraws; // indexed by PrimitiveIndex
        std::vector<GpuDrawSlot>           DrawSlots;
        std::vector<GpuDrawBin>            Bins; // sorted by IndirectGroup
        std::vector<IndirectGroup>         Groups;
        uint32_t                           VisibleCapacity = 0; // sum of the bins' Capacity
    };

    // The graph handles of one pass's tables after this frame's upload.
    struct PassDrawBuffers
    {
        RDG::BufferRef PrimitiveDraws; // GpuPrimitivePassDraws[]
        RDG::BufferRef DrawSlots;      // GpuDrawSlot[]
        RDG::BufferRef Bins;           // GpuDrawBin[]
    };

    class DrawCommandCache
    {
    public:
        // The primitive's draws in every pass it takes part in, from its mesh's submeshes and LOD chain, its
        // material slots and the PSOs the PipelineCache resolves for each pass.
        void AddPrimitive( PrimitiveIndex primitive, const Mesh& mesh, std::span<const uint32_t> materialRows,
                           uint32_t primitiveFlags );
        void RemovePrimitive( PrimitiveIndex primitive );
        // Every draw recording with a pipeline of this shader re-resolves its PSO (hot reload, after
        // WaitDeviceIdle — the PipelineCache's InvalidateByShader point).
        void InvalidatePipelines();

        // Re-sorts the bins and groups of the passes whose primitives changed; no-op when nothing did.
        void Rebuild();

        [[nodiscard]] const PassDrawTable& Pass( MeshPass pass ) const;
        // "DrawCommands: Upload" of the passes rebuilt since the last upload; the others keep their buffers.
        [[nodiscard]] Common::ResultStr<std::array<PassDrawBuffers, kMeshPassCount>>
        AddUploadPass( RDG::Builder& graph );

    private:
        std::array<PassDrawTable, kMeshPassCount> m_Passes;
        std::array<bool, kMeshPassCount>          m_Dirty{};
    };
} // namespace Desert::Graphic::GpuDriven
