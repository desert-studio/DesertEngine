#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
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
//  - A submesh's node transform (Submesh::Transform, relative to the primitive) is DATA too: one row per submesh
//    of each mesh in SubmeshTransforms[], named by GpuDrawSlot::SubmeshTransform, so World = Primitive.LocalToWorld
//    x Instance.InstanceToPrimitive x SubmeshTransforms[slot.SubmeshTransform] (today the per-submesh push of
//    WalkMeshSubmeshes, API/Vulkan/VulkanRenderer.cpp:868).
//  - A submesh that does not draw in a pass (hidden, or no PSO for it there) keeps its slot with Bin =
//    kInvalidIndex: the slot formula of GpuPrimitivePassDraws stays dense, the cull skips the slot.
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

    // The shadow passes draw only kCastsShadow primitives (UE bCastShadow on the shadow-depth pass processor).
    [[nodiscard]] constexpr bool IsShadowPass( const MeshPass pass )
    {
        return pass == MeshPass::ShadowCascade || pass == MeshPass::VirtualShadow;
    }

    // One multi-draw call: the bins [FirstBin, +BinCount) of a pass share these. The group key is the pipeline x
    // the mesh's geometry buffers (vertex, optional streams, index): a mesh owns its buffers, so the mesh IS the
    // key of its buffers, and the record binds all three from it (one stream buffer per mesh, MeshVertexLayout).
    struct IndirectGroup
    {
        const GraphicsPipeline* Pipeline = nullptr; // owned by the SceneRenderer's PipelineCache
        const Mesh*             Geometry = nullptr; // its vertex / stream / index buffers
        uint32_t                FirstBin = 0;
        uint32_t                BinCount = 0;
    };

    // One pass's cached state: what the cull and the draw need, as host mirrors of the GPU tables.
    struct PassDrawTable
    {
        std::vector<GpuPrimitivePassDraws> PrimitiveDraws; // indexed by PrimitiveIndex (high-water mark)
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

    // Every table of the cache after this frame's upload: the passes', and the submesh transforms every pass's
    // GpuDrawSlot::SubmeshTransform indexes (one row per submesh of every mesh in the cache, shared by the passes).
    struct DrawCommandBuffers
    {
        std::array<PassDrawBuffers, kMeshPassCount> Passes{};
        RDG::BufferRef                              SubmeshTransforms; // mat4[]
    };

    // How the cache learns the PSO a submesh draws with in a pass: the SceneRenderer's PipelineCache resolution
    // of the section's material for that pass (the question the CPU MeshDrawList asks today in its Build*).
    // nullptr = the submesh does not draw in the pass (no depth cell for its material, a translucent section in
    // the opaque passes). Called from AddPrimitive and InvalidatePipelines only, never per frame.
    using PassPipelineResolver = std::function<const GraphicsPipeline*(
         MeshPass pass, PrimitiveIndex primitive, const Mesh& mesh, uint32_t submesh, uint32_t materialRow )>;

    // What a primitive hands the cache: its mesh, the Materials[] row of each material slot (mesh section i draws
    // with MaterialRows[SectionMaterialSlot( i, size )], MeshTypes.hpp), how many instances it has in GpuScene
    // (the capacity its draws add to their bins), which submeshes it hides (bit i = submesh i, the component's
    // hidden-section mask; submeshes past 64 are never hidden, as in WalkMeshSubmeshes) and its PrimitiveFlags.
    struct PrimitiveDrawDesc
    {
        const Mesh*               Mesh = nullptr;
        std::span<const uint32_t> MaterialRows{};
        uint32_t                  InstanceCount     = 1;
        uint64_t                  HiddenSubmeshMask = 0;
        uint32_t                  PrimitiveFlags    = 0;
    };

    class DrawCommandCache
    {
    public:
        explicit DrawCommandCache( PassPipelineResolver resolver );
        ~DrawCommandCache();
        DrawCommandCache( const DrawCommandCache& )            = delete;
        DrawCommandCache& operator=( const DrawCommandCache& ) = delete;

        // The primitive's draws in every pass it takes part in, from its mesh's submeshes and LOD chain, its
        // material slots and the PSOs the resolver gives for each pass. Adding a primitive the cache holds
        // replaces its draws (a mesh, material, instance-count or flag change). Refused, with the cache
        // unchanged, for a mesh without a vertex or index buffer (the indirect draw is indexed), without
        // submeshes, a submesh reading past the index buffer, an empty MaterialRows or zero instances.
        [[nodiscard]] Common::BoolResultStr AddPrimitive( PrimitiveIndex primitive, const PrimitiveDrawDesc& desc );
        void                                RemovePrimitive( PrimitiveIndex primitive );
        // Every cached draw re-resolves its PSO through the resolver (hot reload, after WaitDeviceIdle — the
        // PipelineCache's InvalidateByShader point); the passes whose answers changed are rebuilt.
        void InvalidatePipelines();

        // Re-sorts the bins and groups of the passes whose primitives changed; no-op when nothing did.
        void               Rebuild();
        [[nodiscard]] bool NeedsRebuild() const;

        [[nodiscard]] const PassDrawTable& Pass( MeshPass pass ) const;
        [[nodiscard]] std::span<const glm::mat4> SubmeshTransforms() const
        {
            return m_SubmeshTransforms;
        }
        // "DrawCommands: Upload" of the passes rebuilt since the last upload; the others keep their buffers.
        // Refused while a Rebuild is pending (the GPU would be told tables the host has not built).
        [[nodiscard]] Common::ResultStr<DrawCommandBuffers> AddUploadPass( RDG::Builder& graph );

        struct Table;

    private:
        struct PrimitiveRecord
        {
            const Mesh*           Mesh = nullptr;
            std::vector<uint32_t> MaterialRows; // per SUBMESH, already through SectionMaterialSlot
            uint32_t              InstanceCount     = 1;
            uint64_t              HiddenSubmeshMask = 0;
            uint32_t              PrimitiveFlags    = 0;
            uint32_t              LodCount          = 1;
            // [pass][submesh]: the resolved PSO, nullptr = not drawn in the pass.
            std::array<std::vector<const GraphicsPipeline*>, kMeshPassCount> Pipelines;
        };
        struct MeshTransformRows
        {
            uint32_t FirstRow = 0;
            uint32_t RowCount = 0;
            uint32_t Users    = 0;
        };

        void ResolvePipelines( PrimitiveIndex primitive, PrimitiveRecord& record ) const;
        void MarkPassesOf( const PrimitiveRecord& record );
        void AcquireTransforms( const Mesh& mesh );
        void ReleaseTransforms( const Mesh& mesh );
        void RebuildPass( MeshPass pass );

        PassPipelineResolver                               m_Resolver;
        std::vector<std::optional<PrimitiveRecord>>        m_Primitives; // indexed by PrimitiveIndex
        std::unordered_map<const Mesh*, MeshTransformRows> m_MeshTransforms;
        std::vector<glm::mat4>                             m_SubmeshTransforms;
        std::vector<glm::uvec2>                            m_FreeTransformRanges; // x = first row, y = count
        std::array<PassDrawTable, kMeshPassCount>          m_Passes;
        std::array<bool, kMeshPassCount>                   m_Dirty{};
        // Rebuilt and not uploaded yet (the pass tables), and the transforms changed since the last upload.
        std::array<bool, kMeshPassCount> m_Unuploaded{};
        bool                             m_TransformsUnuploaded = false;
        // [pass][PrimitiveDraws, DrawSlots, Bins]: the persistent device tables (DrawCommandCache.cpp).
        std::array<std::array<std::unique_ptr<Table>, 3>, kMeshPassCount> m_PassTables;
        std::unique_ptr<Table>                                            m_TransformTable;
    };
} // namespace Desert::Graphic::GpuDriven
