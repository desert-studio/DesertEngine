#pragma once

#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Common/Core/Math/AABBLanes.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Common/Core/Units.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <map>
#include <string_view>

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingProgramHit.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialSilhouette.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Common/Core/DevInstruments.hpp>
#if DESERT_DEV_INSTRUMENTS
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialOverdraw.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialOverdrawResolve.hpp>
#include <Engine/Graphic/PrimitiveReadout.hpp>
#endif
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/DrawPushFields.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Materials/SceneFrameBinding.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/IndirectDrawSubmission.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/MaterialPipelineStates.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>
#include <Engine/Graphic/PassNameTable.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Graphic/RainOcclusion.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/Systems/Scene/ShadowCaster.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>
#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Geometry/StaticMesh.hpp>

#include <functional>
#include <map>
#include <set>
#include <utility>
#include <memory>
#include <array>
#include <vector>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <unordered_map>
#include <map>
#include <compare>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/ForwardPrepassStats.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/TranslucentSortOrder.hpp>
#include <Engine/Graphic/Render/StaticSceneSnapshot.hpp>
#include <Common/Core/AssetHandle.hpp>

namespace Desert::Runtime
{
    class MaterialService;
    class ShaderService;
} // namespace Desert::Runtime

namespace Desert::Graphic
{
    class StorageBufferProperty;
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    class DisplacementCache;
    namespace MeshRendererDetail
    {
        // One draw of a lit mesh node, chosen when the frame's draw list is BUILT (the node's setup, before any
        // command is recorded): pipeline, mesh, transform, the material executor that records it, instance range,
        // submesh mask and LOD. UE: a FMeshDrawCommand, built in InitViews and recorded later.
        struct MeshDrawCommand
        {
            // Owned by the SceneRenderer's PipelineCache (GetOrCreate / GetOrCreateMaterial keep the shared_ptr
            // in its map), which drops entries only in Clear (scene init) and InvalidateByShader (hot reload,
            // after WaitDeviceIdle) - never between a node's setup and its exec - so the list holds no share.
            const GraphicsPipeline* Pipeline = nullptr;
            const Mesh*             Mesh     = nullptr;
            glm::mat4               Transform{ 1.0f };
            const MaterialExecutor* Material          = nullptr;
            uint32_t                InstanceCount     = 1;
            uint32_t                FirstInstance     = 0;
            SubmeshMask             HiddenSubmeshMask{};
            uint32_t                LodLevel          = 0;
            // A GPU-generated instanced draw (GpuInstances.hpp): each drawn submesh reads its record at
            // GpuInstanceArgsOffset( submesh ) of this graph buffer (InstanceCount / FirstInstance unused), and
            // the node declares the reads of it and of Instances (the transforms its material binds).
            RDG::BufferRef IndirectArgs{};
            RDG::BufferRef IndirectInstances{};
            // GPU-COST-22: the drawn list's ids into IndirectInstances (GpuInstanceRefs::Ids); invalid: none.
            RDG::BufferRef IndirectIds{};
            // Where this draw's records begin in IndirectArgs: after the header, or a later level's group of
            // records (one per submesh) when the producer draws a LOD chain (GpuInstancedRenderData::Levels).
            uint64_t IndirectArgsOffset = kGpuInstanceArgsHeaderBytes;
            // RENDER-CPU-COST-1 (UE: one indirect draw per mesh batch, never a CPU call per LOD): the record
            // groups from IndirectArgsOffset this draw reads - a producer's consecutive LOD levels (LodLevel the
            // first), each one record per submesh of the mesh. Each drawn submesh is ONE multi-draw over its
            // record of every group (Renderer::RenderMeshIndirect @p groups), so a section's LOD chain is one draw
            // of the list and one bind, not a draw per level. 1 without IndirectArgs.
            uint32_t IndirectLevels = 1;
            // RENDER-JOB-CPU-3, A COMPACTED BIN (GpuInstances.hpp GpuDrawBinRefs; UE: one indirect draw per merged
            // mesh draw command): valid -> the draw is ONE Renderer::RenderMeshIndirectCount of IndirectArgs'
            // commands from IndirectArgsOffset (stride kGpuDrawBinCommandBytes, at most IndirectMaxDraws, the
            // drawn count the GPU wrote at IndirectCountOffset of this buffer). The mesh's geometry is bound once
            // (every command carries its record's firstIndex / indexCount); IndirectLevels is 1 and
            // IndirectInstances / IndirectIds are invalid - each draw reads its own buffers by address from its
            // draw row (Push DrawRows), and the node declares them from AddressedReads.
            RDG::BufferRef                  IndirectCount{};
            uint64_t                        IndirectCountOffset = 0;
            uint32_t                        IndirectMaxDraws    = 0;
            std::span<const RDG::BufferRef> AddressedReads{}; // owned by the list builder, valid until it rebuilds
            // RENDER-THREADS M2 - what differs per draw travels WITH the draw (UE FMeshDrawCommand: immutable,
            // per-draw data by value), never through the shared material. Push: the draw's push values (transform,
            // primitive / material row, flags, wind, bone offset, skin cache), written by Record over a COPY of
            // the material's push block (DrawPushFields::WriteInto) - the material's own block is not touched.
            DrawPushFields Push{};
            // No material instance and no Bind: an instance's values are its scene material row (EffectiveRow,
            // FindMaterialRow - the row the draw's MaterialIndex names), the material's own pending values are
            // uploaded at use by MaterialExecutor::Apply. Record writes nothing of the shared material
            // (RENDER-THREADS M2b).
            // The draw's own rectangle of the target (a card of the capture atlas, Renderer::SetViewportRegion),
            // a command of the draw's command buffer. Empty: the pass's viewport.
            struct ViewportRegion
            {
                int32_t  X      = 0;
                int32_t  Y      = 0;
                uint32_t Width  = 0;
                uint32_t Height = 0;
            };
            std::optional<ViewportRegion> Viewport{};
            // What a GPU cull of the draw needs (the virtual shadow map's, CollectVirtualShadowCull): the matrices
            // its instance range indexes, from FirstInstance (empty: one instance at Transform), how far the wind
            // moves its vertices past the mesh's bounds (cm, WindBoundsPad), and the world box of a single
            // instance whose vertices leave the mesh's bind bounds (a skinned pose, SkinnedPoseBounds). Instances
            // points into the list builder's scratch: valid until the list is built again.
            std::span<const glm::mat4>        Instances{};
            float                             BoundsPad = 0.0f;
            std::optional<Common::Math::AABB> WorldBounds{};
        };

        // THE frame's draw list of one lit mesh node. The material (hence shader and route fill) of every draw is
        // chosen ONCE, by Build* in the node's setup; Declare declares one binding block per distinct (material
        // executor, recording pipeline's shader) pair of the list (the layout kept for the shader the draws RECORD
        // with - `Pipeline->GetSpecification().Shader`, re-derived only on its reload, never per frame - the
        // executor's route fill, and the scene/view inputs the layout has slots for), so ValidatePassBindings
        // judges every draw before anything is recorded; Record (the exec) walks the same list with the block each
        // draw's executor was declared as. A build that failed is kept and is Record's error, before any draw.
        // The GPU-culled bins a MeshDrawList issues after its CPU commands (UE FInstanceCullingDrawParams +
        // SubmitDrawCommands): one view's CulledDraws of one pass's table, the scene / pass tables the binned
        // vertex stage reads by name (Common/GpuScene.glslh 41-44), the device's submission mode, and per
        // IndirectGroup the view's carrier of the scene blocks (null for an empty group). THE VIRTUAL-GEOMETRY
        // RASTER of one (pass, view) (SceneRenderer::AddFrameVirtualGeometry ->
        // MeshRenderer::SetFrameVirtualGeometry): the cluster cut's HW clusters binned into the pass table's
        // groups (VirtualGeometry::RasterBins), drawn after the bins, one vkCmdDrawIndirect per group whose
        // IndirectGroup::VirtualGeometryPipeline exists, with the tables that cell reads by name
        // (Common/GpuScene.glslh 50-53) and per group the view's carrier of that cell's shader.
        struct VirtualGeometryIssue
        {
            RDG::BufferRef GroupArgs;       // VkDrawIndirectCommand[GroupCount] (RasterBins::GroupArgs)
            RDG::BufferRef Rows;            // uint32 instance id stream (RasterBins::Rows)
            RDG::BufferRef VisibleClusters; // the cut's VgVisibleCluster[] (binding 52)
            RDG::BufferRef Clusters;        // VirtualMeshBuffers::Clusters (binding 50)
            RDG::BufferRef Dags;            // VirtualMeshBuffers::Dags (binding 51)
            RDG::BufferRef PrimitiveDraws;  // the pass's PassDrawBuffers::PrimitiveDraws (binding 53)
            uint32_t       GroupCount = 0;
            std::vector<const MaterialExecutor*>
                 GroupCarriers; // per Table->Groups; null = the group draws no cluster
            // Empty: each group's clusters draw with its IndirectGroup::VirtualGeometryPipeline. Otherwise per
            // Table->Groups, the pipeline this issue rasters the group's clusters with, null = not drawn by this
            // issue (the deferred early-Z pass: the `.Depth.Binned.VirtualGeometry` twins in the depth prepass,
            // the equal-depth twins in the G-buffer; MeshRenderer::EarlyZSplit).
            std::vector<const GraphicsPipeline*>  GroupPipelines;
            [[nodiscard]] const GraphicsPipeline* GroupPipeline( const GpuDriven::PassDrawTable& table,
                                                                 const size_t                    group ) const
            {
                return GroupPipelines.empty() ? table.Groups[group].VirtualGeometryPipeline
                                              : GroupPipelines[group];
            }
        };

        struct BinnedIssue
        {
            const GpuDriven::PassDrawTable*      Table = nullptr;
            GpuDriven::CulledDraws               Culled;
            RDG::BufferRef                       Instances;         // GpuSceneBuffers::Instances
            RDG::BufferRef                       DrawSlots;         // PassDrawBuffers::DrawSlots
            RDG::BufferRef                       SubmeshTransforms; // DrawCommandBuffers::SubmeshTransforms
            GpuDriven::IndirectSubmissionMode    Mode;
            std::vector<const MaterialExecutor*> GroupCarriers;   // per Table->Groups
            // Empty: each group draws with its own IndirectGroup::Pipeline. Otherwise per Table->Groups, the
            // pipeline this issue draws the group's records with, null = this issue does not draw the group:
            // the deferred early-Z pass's two issues of the G-buffer's one cull (MeshRenderer::DeclareEarlyZBins,
            // the depth twins in "Deferred: Masked Depth Prepass", the equal-depth twins in "Deferred: GBuffer").
            std::vector<const GraphicsPipeline*> GroupPipelines;
            std::optional<VirtualGeometryIssue>  VirtualGeometry; // drawn after the bins (phase 1 only)
            [[nodiscard]] const GraphicsPipeline* GroupPipeline( const size_t group ) const
            {
                return GroupPipelines.empty() ? Table->Groups[group].Pipeline : GroupPipelines[group];
            }
            // False: a VG-only issue (a list whose view culled no bins, the virtual shadow map's levels): Culled
            // is empty, only VirtualGeometry is drawn; GroupCarriers is then one null per group.
            bool BinsIssued = true;
        };

        class MeshDrawList
        {
        public:
            void Clear();
            // Record issues @p issue's non-empty groups after the CPU commands, one block per distinct (carrier,
            // group shader) declared with the binned tables. Refused (the list fails) for a table without carriers
            // for its groups or a non-empty group without pipeline or carrier. Until the next Clear.
            void SetBinned( BinnedIssue issue );
            // The scene's GpuScene instance rows in this list's graph (GpuSceneBuffers::Instances): bound to every
            // CPU block whose shader reads them (an ISM's instanced cell by GpuScene id,
            // MeshRenderer::SurfaceSceneInstancesVariant). A block that reads them with none set fails the list.
            // Carried by Append; until the next Clear.
            void SetSceneInstances( const RDG::BufferRef instances )
            {
                m_SceneInstances = instances;
            }
            // A draw without pipeline, mesh or material (or whose material has no shader) is refused here.
            void               Add( MeshDrawCommand command );
            // Moves @p other's commands to the end of this list, as if each had been Added here in its order (the
            // blocks of first use, the triangles, the first error): a concurrent build's own list joining the
            // pass's. @p other holds plain commands only (no SetBinned / SetIndirect) and is left cleared.
            void               Append( MeshDrawList& other );
            void               Fail( std::string error );
            [[nodiscard]] bool IsEmpty() const
            {
                return m_Commands.empty();
            }
            // The triangles this list sends to the GPU: per draw, the index count of the LOD level it was built
            // with (clamped to the chain the submesh has, as RenderMesh draws it), for each visible submesh, times
            // its instances. A GPU-generated draw (IndirectArgs) is not counted: its instance count is the
            // GPU's, unknown here. UE: the "Triangles drawn" of stat RHI, for one pass. Counted by Add, while the
            // frame's meshes are alive: the list outlives its frame (the status bar reads it from the UI, after a
            // sculpt stroke or an undo may have released a mesh a command still points to), so this never
            // dereferences a command's Mesh.
            [[nodiscard]] uint64_t Triangles() const
            {
                return m_Triangles;
            }
            // The binding blocks Declare declares (one per distinct executor + recording shader): a node declaring
            // more blocks after the list's numbers them from here.
            [[nodiscard]] uint32_t BlockCount() const
            {
                return static_cast<uint32_t>( m_Blocks.size() );
            }

            // The blocks are the node's ONLY binding blocks (indices 0..n-1, in first-use order). @p view: the
            // scene/view inputs bound for every executor whose shader has slots for them (none: nothing bound).
            void Declare( RDG::PassBuilder& pass, const std::optional<SceneViewInputs>& view ) const;
            // @p perBlock adds the node's own pass parameters to EVERY block after the scene/view inputs (the
            // translucency node's scene snapshot), handed the block's kept layout with its slot mask so it binds
            // only the slots that shader has by bit (SceneViewSlot), never by name: a node whose cells are several
            // shaders binds them once each.
            void Declare( RDG::PassBuilder& pass, const std::optional<SceneViewInputs>& view,
                          const std::function<void( RDG::BindingBlockBuilder&, const SlottedLayout& )>& perBlock )
                 const;
            void Declare( RenderPassDeclaration& declared, const std::optional<SceneViewInputs>& view ) const;

            [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const;

            [[nodiscard]] std::span<const MeshDrawCommand> GetCommands() const
            {
                return m_Commands;
            }
            [[nodiscard]] const std::optional<std::string>& GetError() const
            {
                return m_Error;
            }
            // Record draws every command from the GPU's argument records instead (Renderer::RenderMeshIndirect):
            // command i's first record is record @p firstRecords[i] of @p arguments, which the node declares
            // Access::IndirectArgs. Until the next Clear.
            void SetIndirect( RDG::BufferRef arguments, std::vector<uint32_t> firstRecords );
            // The first-record storage handed out empty (CPU-FRAME-ARENA 7): a caller fills it and gives it back
            // through SetIndirect, so a list kept across frames reuses its capacity instead of a new vector.
            [[nodiscard]] std::vector<uint32_t> TakeFirstRecords() noexcept
            {
                std::vector<uint32_t> records = std::move( m_FirstRecords );
                m_FirstRecords.clear(); // a moved-from vector is valid but unspecified
                records.clear();
                return records;
            }
            // The GPU-driven mesh pass Record's draws are counted under (DrawCounter::LastFrameMeshPass, DesertCtl
            // `state views.meshPasses`); set once by the owner (MeshRenderer::Initialize), kept across Clear.
            // Unset: a list outside the binned passes (glass, debug, card capture), counted only globally.
            void CountAs( GpuDriven::MeshPass pass )
            {
                m_CountedPass = pass;
            }
            // THE DEPTH PASS'S DRAW ORDER (UE CalculateDepthPassMeshStaticSortKey: the depth pass sorts the masked
            // draws after the opaque ones). Set: Record issues the GPU-culled bins and the VG clusters (the
            // scene's opaque occluders) BEFORE the CPU commands (the alpha-tested GPU-instanced grass and
            // foliage), so the masked fragments behind them fail the early depth test before their clip runs. The
            // depth the list writes is the nearest fragment whatever the order, so only the cost moves. Set once
            // by the owner (MeshRenderer::Initialize), kept across Clear; unset: the commands first (every other
            // list).
            void RecordBinnedFirst()
            {
                m_BinnedFirst = true;
            }

        private:
            [[nodiscard]] Common::BoolResultStr RecordCommands( const RDG::PassContext&       context,
                                                                const RDG::OpenBindingBlocks& blocks ) const;
            [[nodiscard]] Common::BoolResultStr RecordBinned( const RDG::PassContext&       context,
                                                              const RDG::OpenBindingBlocks& blocks ) const;
            // One declared block: the executor whose route fills it and the pipeline of its first draw, whose
            // shader keys its layout (every draw of the block records with a pipeline of that same shader).
            struct Block
            {
                const MaterialExecutor* Material = nullptr;
                const GraphicsPipeline* Pipeline = nullptr;
                bool                    Binned          = false; // declares the binned tables (SetBinned)
                bool                    VirtualGeometry = false; // declares the VG raster's tables (50-53)
            };

            template <typename Declaration, typename PerBlock>
            void DeclareBlocks( Declaration& declaration, const std::optional<SceneViewInputs>& view,
                                const PerBlock& perBlock ) const;

            std::vector<MeshDrawCommand> m_Commands;
            std::vector<uint32_t>        m_BlockOf; // per command: its block
            std::vector<Block>           m_Blocks;
            std::optional<std::string>   m_Error;
            RDG::BufferRef               m_IndirectArguments; // SetIndirect; invalid: the commands' own counts
            std::vector<uint32_t>        m_FirstRecords;
            std::optional<BinnedIssue>   m_Binned;       // SetBinned
            RDG::BufferRef               m_SceneInstances; // SetSceneInstances
            std::vector<uint32_t>        m_GroupBlock;   // per group of m_Binned: its block (kInvalidBlock: empty)
            std::vector<uint32_t>        m_VgGroupBlock; // per group of m_Binned->VirtualGeometry: its block
            uint64_t                     m_Triangles = 0; // Add's count of the commands' triangles (Triangles)
            // Kept across frames (Clear drops only the layouts of destroyed shaders). Mutable: Declare is const.
            mutable ShaderBindingLayoutSet m_Layouts;
            // CPU-FRAME-ARENA 3: DeclareBlocks' each-buffer-once set, its capacity kept across frames (a list is
            // declared on one thread at a time, as m_Layouts is used). Mutable: Declare is const.
            mutable std::vector<RDG::BufferRef> m_DeclaredReads;
            std::optional<GpuDriven::MeshPass>  m_CountedPass;
            bool                                m_BinnedFirst = false; // RecordBinnedFirst
        };
    } // namespace MeshRendererDetail

    struct MeshRenderData
    {
        // The entity that owns the draw: the key of its primitive in the scene's GpuSceneRegistry.
        uint32_t    Entity = 0;
        uint32_t    MotionPart = 0; // a stable part of the entity (MotionRecord::Part), 0 = by submission
        class Mesh* Mesh;
        glm::mat4   Transform;

        // A CO-OWNED handle on the entity's slots — see Graphic::MaterialSlotBinding (A8-3). Still no
        // per-mesh slot-vector copy (that cost ~0.9 ms of CmdBuffer ExecuteAll for 256 meshes in Debug);
        // what travels the chain is a shared_ptr, so the slots and the instances in them cannot be freed
        // under a draw that is still in flight.
        MaterialSlotBindingPtr MaterialSlots;
        uint32_t               SectionSlot = 0; // static: the slot this draw's submeshes shade with

        // optional
        std::vector<glm::mat4> BoneMatrices;
        // Skinned only: MorphComponent's weights and revision (empty = drawn unmorphed). See UpdateMorphTargets.
        std::vector<float> MorphWeights;
        uint64_t           MorphRevision = 0;

        bool     Outlined        = false;
        uint64_t HiddenSubmeshes = 0;  // bit i = submesh i hidden (static meshes)
        int      ForcedLOD       = -1; // -1 = auto (by distance); 0..N pins a LOD level
        int      LODBias         = 0;  // shifts the auto LOD (ignored when forced)
        bool     CastShadows     = true;
        bool     ReceiveShadows  = true;
        bool     CastContactShadow = true; // UE bCastContactShadow: the G-buffer's NO_CONTACT_SHADOW bit (static)
        int      TranslucencySortPriority = 0; // lower draws first (translucency pass)
    };

    // UE r.MinScreenRadiusForDepthPrepass (0.03): a GPU-instanced bucket whose instances all stand below this
    // screen radius - Geometry::SelectLODFromBounds' measure, the bounding sphere's radius x max(P[0][0], P[1][1])
    // / 2 over its distance - is not in the deferred early-Z pass (GpuInstancedRenderData::PrepassBuckets).
    inline constexpr float kMinScreenRadiusForDepthPrepass = 0.03f;

    class MeshRenderer final : public RenderSystem
    {
    public:
        // Releases this view's GPU-instanced material copies (MaterialService::ReleaseViewVariants).
        ~MeshRenderer() override;

        struct StaticMeshRenderData
        {
            // The entity that owns the draw: the key of its primitive in the scene's GpuSceneRegistry.
            uint32_t Entity = 0;
            uint32_t MotionPart = 0; // a stable part of the entity (MotionRecord::Part), 0 = by submission
            // Its primitive in the scene's GpuScene (SyncGpuScene); the view-pass cells read the primitive's
            // world, previous world and bone offsets from it through the push PrimitiveIndex.
            uint32_t                  GpuPrimitive = 0;
            // The primitive's GpuScene instance (SyncGpuScene): its TLAS custom index and GI card-table row.
            uint32_t                  GpuInstance = 0;
            // Its section's row in the scene material table (SyncGpuScene, GpuSceneRegistry::SyncMaterialRow): the
            // MaterialIndex its forward / G-buffer draw names, and GpuDrawSlot::MaterialRow for the cached draws.
            uint32_t                  MaterialRow = 0;
            // ANY RIGID MESH (MeshType::Static): a cooked StaticMesh or a DynamicMesh (a primitive, an editable
            // mesh, a fracture / spline piece). Typed as the base because that is what it is: the StaticMesh-only
            // members (GetBakedDisplaced, a displacement request) are reached by dynamic_cast, the
            // virtual-geometry rows by Mesh::GetVirtualMesh. It was `StaticMesh*`, filled by a static_cast of
            // every Static-typed mesh, and the VG candidate walk read a DynamicMesh's registration past its end
            // (FPS-MEASURE-5).
            class Desert::Mesh*       Mesh      = nullptr;
            glm::mat4                 Transform = glm::mat4( 1.0f );
            // Co-owned, and it must be: this queue is read by five passes, all of them AFTER the frame's
            // ECS systems have run (A8-3).
            MaterialSlotBindingPtr MaterialSlots;
            // The slot every drawn submesh shades with, in every pass (SectionSurface); HiddenSubmeshes masks the
            // sections of the entity's other slots, which arrive as their own entries.
            uint32_t               SectionSlot     = 0;
            bool                   Outlined        = false;
            uint64_t               HiddenSubmeshes = 0;  // bit i = submesh i hidden
            int                    ForcedLOD       = -1; // -1 = auto (by distance)
            int                    LODBias         = 0;  // shifts the auto LOD (ignored when forced)
            bool                   CastShadows     = true;
            bool                   ReceiveShadows  = true;
            // UE bCastContactShadow: pushed with the G-buffer draw (PrimitiveFlags) into the shading word.
            bool                   CastContactShadow        = true;
            int                    TranslucencySortPriority = 0; // lower draws first (TranslucentSortOrder)
        };

        // A skinned entry's section group: every submesh that shades with one material slot.
        struct SkinnedSection
        {
            // The (surface x Skinned) material. It is SHARED with every other entity using the same
            // `.demat`, which is why nothing per-object may be stored on it: the pose is packed into a
            // per-frame buffer and named by a push constant instead.
            class Graphic::DataDrivenMaterial* Material = nullptr;
            MaterialInstance* Instance        = nullptr; // applied during Bind; owned by the entry's MaterialSlots
            uint64_t          HiddenSubmeshes = 0;       // every submesh of the other sections
        };

        struct SkinnedMeshRenderData
        {
            // The entity that owns the draw: the key of its primitive in the scene's GpuSceneRegistry.
            uint32_t Entity = 0;
            // Its primitive in the scene's GpuScene (SyncGpuScene); the view-pass cells read the primitive's
            // world, previous world and bone offsets from it through the push PrimitiveIndex.
            uint32_t                   GpuPrimitive = 0;
            // The primitive's GpuScene instance (SyncGpuScene): its TLAS custom index and GI card-table row.
            uint32_t                   GpuInstance = 0;
            class Desert::SkinnedMesh* Mesh      = nullptr;
            // The mesh the skin cache skins when Mesh is a displaced twin (ApplyDisplacement, CONTRACT-DISPLACE
            // §9.8): the base (or its morph twin); the twin's streams are then written from that pose. Null = Mesh.
            class Desert::SkinnedMesh* SkinSource = nullptr;
            glm::mat4                  Transform = glm::mat4( 1.0f );
            // The binding every section's instance below was selected FROM, carried so that it keeps those
            // instances alive: the entity that authored it can be destroyed between the record and this queue
            // being drawn (A8-3). Holding the binding rather than a second shared_ptr per instance keeps the
            // static and skinned queues answering the lifetime question the same way.
            MaterialSlotBindingPtr      MaterialSlots;
            // One per section material (UE: a skeletal mesh's sections, each a batch element with its own
            // MaterialIndex), by the static rule Desert::SectionMaterialSlot. The pose is skinned ONCE per entry;
            // every pass draws each section with its own material, masked to its own submeshes.
            std::vector<SkinnedSection> Sections;
            std::vector<glm::mat4>      BoneMatrices;       // animated pose, or bind pose
            bool                        Outlined    = false;
            bool                        CastShadows = true;
        };

        // A UE-style Instanced Static Mesh: ONE mesh + ONE Lit material drawn N times. The material and the
        // transforms are CO-OWNED handles on what the component produced, not pointers into it (A8-3).
        // Rendered through the SAME instanced pipeline/SSBO as the auto-batched static meshes.
        struct InstancedMeshRenderData
        {
            // A Mesh, not a StaticMesh: a primitive ISM carries a DynamicMesh, and so does one the
            // Foliage tool builds. See SceneRenderer::SubmitInstancedMesh for the cast this replaced.
            // The entity that owns the field: each instance's GPU-scene row is keyed {Entity, instance}.
            uint32_t                                      Entity = 0;
            class Desert::Mesh*                           Mesh   = nullptr;
            MaterialInstancePtr                           Material; // the section group's slot (a mesh cell)
            // The same slot for displaced geometry (MaterialService displaced sibling: base cells, no parallax
            // march); null when the mesh is not displaced. ApplyDisplacement draws the twins' groups with it.
            MaterialInstancePtr DisplacedMaterial;
            // The component's instance index of each transform when ApplyDisplacement split the record by twin
            // (null = the transforms ARE the component's, in order): the GPU-scene row stays {Entity, index + 1}.
            std::shared_ptr<const std::vector<uint32_t>> InstanceIds;
            uint64_t                                      HiddenSubmeshes = 0; // the other slots' sections
            std::shared_ptr<const std::vector<glm::mat4>> Transforms; // snapshot of InstanceTransforms
            bool                                          CastShadows = true;
            bool                                          CastContactShadow = true; // UE bCastContactShadow
            // The field's foliage type's CullDistance (FO-5); {0, 0} for an ISM that is not foliage.
            InstanceCullDistance CullDistance;
            // The field's foliage type's wind (FO-7) at this frame's gameplay time; still for any other ISM.
            InstanceWind Wind;
            // This field's first GpuScene instance (SyncGpuScene; a field's instances are one contiguous run):
            // instance i is GpuScene instance FirstGpuInstance + i - its TLAS custom index and
            // its GI card-table row (one row per instance, 1:1 with the TLAS instance).
            uint32_t FirstGpuInstance = 0;
        };

        // An instanced section whose transforms and count the GPU wrote this frame (GpuInstances.hpp): the
        // landscape grass. Drawn through the material's (Instanced x pass) cell like an ISM, but with a material
        // copy of its own (MaterialService::GetViewVariant keyed by Key) whose InstanceTransforms is the
        // producer's buffer.
        struct GpuInstancedRenderData
        {
            class Desert::Mesh* Mesh = nullptr;
            // Instanced (a mesh per instance) or GrassBlade (the blade strip, shaped per instance by the
            // GrassBlade vertex path from BladeShape): the material's cell of that path draws it.
            MeshVertexPath                                  Path = MeshVertexPath::Instanced;
            MaterialInstancePtr                             Material;
            uint64_t                                        HiddenSubmeshes = 0;
            bool                                            CastShadows     = true;
            InstanceWind                                    Wind;
            std::shared_ptr<ShaderResources::StorageBuffer> Instances;
            // OCCLUSION PER VIEW (GRASS-VIS; UE: each view culls its own instances): when set, the camera's own
            // list - the instances the producer's HZB test left visible to the main view - which the camera passes
            // (the prepasses, the G-buffer, forward, the grass visibility raster and resolve) draw; the cascades
            // and the TLAS keep Instances / Refs, every kept instance, so what the camera cannot see still casts
            // and reflects. Null: the camera draws Instances too. GPU-COST-22: the list holds IDS, one uint per
            // slot naming its row of Instances (GpuInstanceRefs::Ids); the camera's cells read Instances through
            // it (kSurfaceInstanceIdsDefine).
            std::shared_ptr<ShaderResources::StorageBuffer> CameraIds;
            // GrassBlade only: the variety's shape, Vertex_GrassBlade.glslh's GrassBladeShape.
            std::shared_ptr<ShaderResources::StorageBuffer> BladeShape;
            // GRASS-VIS-MESH-1, Instanced only: the section's raster-stream rows (GrassMeshVisibilityGpu,
            // Mesh/Surface/GrassMeshVisibility.glslh); null: the section is not drawn through the visibility buffer.
            std::shared_ptr<ShaderResources::StorageBuffer> MeshVisibility;
            // The mesh the visibility resolve's full-screen triangle draws (its first three indices are 0, 1, 2);
            // null: Mesh itself (a blade strip).
            class Desert::Mesh* ResolveMesh = nullptr;
            const GpuInstanceRefs*                          Refs = nullptr; // the producer's, this frame
            const GpuInstanceRefs* CameraRefs = nullptr; // with CameraIds: phase 1's camera draws
            // With CameraIds: phase 2's (the instances this frame's HZB found visible after last frame's held
            // them back), CameraIds through records of their own, drawn only by the post-occlusion passes.
            const GpuInstanceRefs* PostRefs = nullptr;
            // GPU-COST-21, THE CASCADES' OWN LISTS (UE: each shadow view culls its instances by its own volume):
            // for every cascade view whose bit CascadeMask sets, CascadeIds through CascadeRefs holds the ids
            // (rows of Instances, GPU-COST-22) of the instances inside that cascade's caster box
            // (CascadeCasterBoxSet), its records one copy of Refs' layout of CascadeArgBytes, the k-th set bit's
            // at k x CascadeArgBytes; that cascade draws them (BuildGpuInstancedShadowDraws) through its
            // kSurfaceInstanceIdsDefine caster cell. A view without its bit, or CascadeRefs null, draws Instances
            // / Refs directly - through FullIds when the producer's full list holds ids (GRASS-VIEW-LAW-1: grass's
            // full list names its rows of Instances, GrassCull.shader CullInstance; Refs.Ids is FullIds' graph ref).
            std::shared_ptr<ShaderResources::StorageBuffer> CascadeIds;
            std::shared_ptr<ShaderResources::StorageBuffer> FullIds;
            const GpuInstanceRefs*                          CascadeRefs     = nullptr;
            uint32_t                                        CascadeMask     = 0;
            uint32_t                                        CascadeArgBytes = 0;
            // GRASS-SHADOW-1, EACH LAYER ITS OWN PART (UE: a cached shadow's static and movable casters are separate
            // draw lists): non-zero when a cascade's list holds its movers (the ring the shadow wind rule sways,
            // ShadowWindInnerOf .. ShadowWindDistanceOf) apart from its rest-pose casters - the movers drawn by the
            // list's records, the rest by their twins CascadeRestShift bytes after them. The static layer then
            // draws the rest part, the movable layer the movers, each with no per-instance rule in its vertex
            // stage (GrassGenerate.shader splits, GrassCull.shader counts).
            uint32_t                                        CascadeRestShift = 0;
            // The instances Instances holds room for (GpuInstances.hpp header word 1): the TLAS rows reserved for
            // the producer (RayTracingGpuInstanceSource::Rows).
            uint32_t Capacity = 0;
            // Per bucket (a strip level for GrassBlade), the instances its record holds room for: the TLAS rows
            // reserved for that level (RayTracingGpuInstanceSource::Rows). Owned by the producer, this frame.
            std::span<const uint32_t> BucketCapacities;
            // Instanced only: the mesh's LOD levels the producer sorts its instances into, one bucket and one
            // group of records (one per submesh, level L at record L x submesh count) per level, each drawn by a
            // draw of its own (UE grass HISM: the instance's LOD by screen size). 1: one bucket, level 0.
            uint32_t Levels = 1;
            // THE SMALL BUCKETS SKIP THE DEPTH PREPASS (UE r.MinScreenRadiusForDepthPrepass, a primitive below
            // that screen radius is not in the depth pass: its prepass costs more vertices than the overdraw it
            // saves): the first bucket (a mesh's LOD level, a blade's strip level) whose every instance stands
            // below kMinScreenRadiusForDepthPrepass of the screen, set by the producer that sorts the instances
            // into the buckets. From it on the deferred early-Z pass does not draw the section's buckets and the
            // G-buffer draws them through the section's own (path x GBuffer) cell - its clip and its depth writes,
            // the same texels and depth the pair would have left (BuildGpuInstancedDraws). ~0u: every bucket is
            // prepassed.
            uint32_t PrepassBuckets = ~0u;
            // GrassBlade only: what each cascade picks the strip's record set (its shadow LOD) and the caster's
            // density by (BuildGpuInstancedShadowDraws, BladeShadowSet). Levels 0: the cascades draw set 0.
            GpuInstanceBladeShadowLod ShadowLod{};
            // Stable per section across frames: the view-variant key (several cascade views: Key * kCascadeViews + v).
            uint32_t Key = 0;
            // GrassBlade only: the shared BLAS of each strip level (GrassBladeRayTracing.hpp), indexed by submesh;
            // empty when the view has no TLAS. Owned by the grass renderer, this frame.
            std::span<const RayTracing::RayTracingGeometryResource* const> BladeGeometry;
            // Where Instances holds each instance's LAST frame transform: instance i's previous world sits at
            // i + PrevInstanceOffset (a producer whose instances move, e.g. particle Mesh rows). 0 = the instances
            // do not move (grass: only the wind moves them), the previous world is the world.
            uint32_t PrevInstanceOffset = 0;
            // RENDER-JOB-CPU-4, THE PRODUCER'S COMPACTED BINS (GpuInstances.hpp GpuDrawBinFrame) per phase (0: the
            // camera's first phase, 1: post-occlusion, 2: the cascades' casters - GRASS-SHADOW-BINS-1). A blade
            // section whose phase table is Open draws in it (MeshRenderer::DrawBinsOf / CascadeBinsOf: one
            // RenderMeshIndirectCount per bin, its rows written by MeshRenderer) unless Debug.Grass "per-variety
            // draws" is on. Null: the section draws per variety.
            std::array<GpuDrawBinFrame*, kGpuDrawBinPhases> DrawBins{};
            // A world box every instance lies in, cm, when the producer has one (a particle emitter's fixed
            // Bounds through the entity's transform, VFXBoundsAt): the section is drawn only into the views and the cascades
            // whose frustum it meets (Core::Frustum::Intersects). nullopt: the producer knows no box (the grass
            // follows the camera), drawn into every view and cascade.
            std::optional<Common::Math::AABB> WorldBounds{};
            // A producer whose instances stand still but for the wind (the grass): what a cached cascade's static
            // layer is keyed by and splits on (CascadeStaticSignature, BuildGpuInstancedShadowDraws; UE: grass
            // HISM in the cached shadow, its WPO off past WPO Disable Distance). nullopt: the instances move by
            // themselves (particle Mesh rows), every cascade they reach is movable every frame.
            struct StillInstances
            {
                // Equal = Instances holds the same instances (the producer's generation of them).
                uint64_t Generation = 0;
                // The farthest any instance's geometry reaches from its origin, world cm, scale and wind included:
                // how far past the shadow wind distance a mover's caster can reach a cascade.
                float Reach = 0.0f;
            };
            std::optional<StillInstances> Still{};
        };

        // A static mesh drawn with a generic data-driven material. Two producers:
        //  - MaterialComponent (Shader Override) on the whole entity: ShaderName + Overrides drive a
        //    shader-keyed shared material (SlotMaterial == nullptr, all submeshes).
        //  - v3 per-slot materials: SlotMaterial points at the slot's own DataDrivenMaterial (asset
        //    params already applied) and VisibleSubmeshMask limits the draw to that slot's submeshes.
        // The (template, instance) a scene material row was registered for: what names a draw's surface, and so
        // its cell. A row index alone never does: GpuScene::AddMaterialRow counts it in its own template's stride
        // (each template's shader reads Materials[] as an array of ITS block), so two templates' rows share
        // indices (FOLIAGE-TRUNK-GLOW). Every record carries its surface next to its row; no table is indexed by
        // a bare row.
        struct RowSurface
        {
            const DataDrivenMaterial* Surface                               = nullptr;
            const MaterialInstance*   Instance                              = nullptr;
            bool                      operator==( const RowSurface& ) const = default;
        };

        struct GenericMeshRenderData
        {
            // The entity that owns the draw: the key of its primitive in the scene's GpuSceneRegistry.
            uint32_t Entity = 0;
            // A stable part of the entity (MotionRecord::Part, e.g. a fracture piece), 0 = the entity itself. A
            // slot-material draw of a part carries the part its lit draw carries, so both read one motion row.
            uint32_t MotionPart = 0;
            // Its primitive in the scene's GpuScene (SyncGpuScene); the view-pass cells read the primitive's
            // world, previous world and bone offsets from it through the push PrimitiveIndex.
            uint32_t                   GpuPrimitive = 0;
            // The primitive's GpuScene instance (SyncGpuScene): its TLAS custom index and GI card-table row.
            uint32_t                   GpuInstance = 0;
            class Mesh*               Mesh      = nullptr;
            glm::mat4                 Transform = glm::mat4( 1.0f );

            // THE DRAW'S NAMED VALUES ARE REFERENCED, NOT COPIED (UE: a mesh draw command points at its proxy's
            // material relevance; CPU-TEXT-G8). The shader name, the overrides, the direct sampler name and the
            // hit group live in the recorded command (DrawGenericMeshCommand) or in the producer-kept
            // GenericMeshDrawParams it points at; both stay alive until the recording's Clear at Scene::OnUpdate's
            // fence, past every pass that reads this queue (ClearQueues drops the queue before the next replay).
            // So a replay per view copies no string and no vector. Never null: a draw without one of them (a
            // per-slot draw, SubmitSlotMaterialMesh) points at the empty value below.
            static inline const std::string                      NoName{};
            static inline const Graphic::MaterialOverrides       NoOverrides{};
            static inline const RayTracing::RayTracingProgramHit NoRayHit{};
            const std::string*                                   ShaderNameRef           = &NoName;
            const Graphic::MaterialOverrides*                    OverridesRef            = &NoOverrides;
            const std::string*                                   DirectTextureSamplerRef = &NoName;
            const RayTracing::RayTracingProgramHit*              RayHitRef               = &NoRayHit;
            // Empty: a per-slot draw (the shader name comes from SlotMaterial at draw time).
            [[nodiscard]] const std::string& ShaderName() const noexcept
            {
                return *ShaderNameRef;
            }
            [[nodiscard]] const Graphic::MaterialOverrides& Overrides() const noexcept
            {
                return *OverridesRef;
            }
            // The sampler DirectTexture binds to (empty with no DirectTexture).
            [[nodiscard]] const std::string& DirectTextureSampler() const noexcept
            {
                return *DirectTextureSamplerRef;
            }
            // The record's own hit group when its program is no surface template (3D text: TextRayHit, built by
            // the producer from the values this draw carries). Empty Template: a ray hit shades with SlotMaterial,
            // else the default surface. Read by the gather only when VisibleInRayTracing. Its address is the
            // producer's (stable while the producer keeps the values), so the gather may keep it by address.
            [[nodiscard]] const RayTracing::RayTracingProgramHit& RayHit() const noexcept
            {
                return *RayHitRef;
            }

            bool                      Outlined = false; // selected -> JFA outline

            // Whether this record rasterizes into the shadow cascades. DEFAULT OFF, unlike the lit
            // queue's flag: a generic draw is not necessarily a solid object. The text system submits
            // its SDF glyph quads through this same queue, and the shadow pass has no alpha test — a
            // default of true would hang an opaque rectangle in the cascade behind every 3D label.
            // The producers that ARE meshes opt in; every draw of an entity casts only the submeshes it
            // draws (VisibleSubmeshMask), so the entity's silhouette is the union of its draws.
            bool CastShadows = false;
            // Whether the view's TLAS holds this record (UE bVisibleInRayTracing). Off by default for the reason
            // CastShadows is: a text glyph quad is not a surface a ray should hit. The mesh producers opt in.
            bool VisibleInRayTracing = false;

            Graphic::Material*        SlotMaterial      = nullptr; // owned by MaterialService (stable)
            uint64_t                  VisibleSubmeshMask = ~0ull;  // bit i = submesh i drawn

            // A RUNTIME-owned texture bound straight to a sampler (bypasses the asset-handle
            // texture-override path). For procedural textures with no TextureAsset — e.g. the text
            // system's SDF font atlas. Non-owning: the producer keeps it alive for the frame. Its sampler:
            // DirectTextureSampler() above.
            Graphic::Image2D* DirectTexture = nullptr;

            // Its row in the scene material table (SyncGpuScene, RegisterGenericRows): a per-slot draw's is its
            // asset's row, a Shader Override draw's its own (template defaults + Overrides.Params + texture slots,
            // keyed by Entity/MotionPart). nullopt: SyncGpuScene could not shade the record (no compiled program,
            // a domain the mesh path does not draw) and the record is not drawn.
            std::optional<uint32_t> MaterialRow;
            // The surface MaterialRow was registered for (RegisterGenericRows): the row's template, which picks
            // the record's binned cell (ResolveBinnedPipeline). Empty while MaterialRow is.
            RowSurface MaterialSurface;
        };

        // THE MATERIAL A SECTION SHADES WITH, and whether it is Masked: the one answer the raster's casters
        // (MeshRendererShadow.cpp) and the ray-tracing segments (RayTracingGather.cpp) both take, so a masked
        // section is alpha-tested by a shadow map and by a ray alike.
        struct SectionShading
        {
            MaterialInstance*            Instance = nullptr;
            Graphic::DataDrivenMaterial* Surface  = nullptr; // null: the slot holds no mesh-cell surface
            bool                         Masked   = false;
            // The surface's template is BlendMode Translucent / Additive (the translucency pass draws it): a ray
            // tracing primitive drawn only by such sections takes the translucent instance mask alone.
            bool Translucent = false;
        };
        [[nodiscard]] static bool IsMaskedSurface( const Graphic::DataDrivenMaterial* material );
        [[nodiscard]] static bool IsTranslucentSurface( const Graphic::DataDrivenMaterial* material );
        // A static entry's section: SectionSurface of its SectionSlot.
        [[nodiscard]] static SectionShading ShadingOf( const StaticMeshRenderData& data );
        // An ISM section group: its slot instance and that instance's parent surface.
        [[nodiscard]] static SectionShading ShadingOf( const InstancedMeshRenderData& data );
        [[nodiscard]] static SectionShading ShadingOf( const SkinnedSection& section );
        // A GPU-instanced section: its slot instance and that instance's parent surface, as an ISM's.
        [[nodiscard]] static SectionShading ShadingOf( const GpuInstancedRenderData& data );

        using RenderSystem::RenderSystem;

        // Gathers the scene's whole per-frame contribution (Graphic::SceneFrameBinding) from the scene
        // renderer + this renderer's own cascade state. One place that knows what "per-frame scene state"
        // IS; the snapshot itself lives beside the materials it is applied to, because it is their
        // payload and not this renderer's private business.
        // `view` is the view the draws are for (SceneFrameBinding::View).
        SceneFrameBinding CaptureFrameState( const ViewFrame* view ) const;

        // The scene's game time this frame (Core::WorldTime), handed over by SceneRenderer::BeginScene and
        // published to materials through CaptureFrameState.
        void SetWorldTimeSeconds( float seconds )
        {
            m_WorldTimeSeconds = seconds;
        }
        // The machine's parallax steps (SceneRenderer::BeginScene, Shading.Parallax*Steps): the light cameras of the
        // cascades carry them (WriteLightCamera), so a parallax caster marches the relief as the view does.
        void SetParallaxQuality( const ParallaxQuality& parallax )
        {
            m_ParallaxQuality = parallax;
        }

        // MESH DISPLACEMENT (DisplacementCache.hpp), after the queues are built and before SyncGpuScene: asks
        // @p cache for the levels of every static record whose mesh carries a displacement topology (per submesh, the
        // height inputs of its effective material row, a section without an assigned Height moving by 0), lets the
        // first view of @p frame displace, then points each record whose twin is ready at the twin - every pass of
        // this view (GBuffer, forward, depth, velocity, cascades, VSM) draws the displaced surface. A record whose
        // twin is not ready draws its base mesh. Errors are logged; the view still draws.
        // @p requestLevels: only CAMERA views ask for levels (§4); a capture face (ViewProfile::SceneCapture) is a
        // consumer like the shadows - it draws the twins the cameras asked for and never raises a level itself.
        void ApplyDisplacement( DisplacementCache& cache, const Geometry::LODView& view, float viewHeightPixels,
                                uint64_t frame, bool requestLevels );

        // This view's queues into the scene's GPU scene (UE FGPUScene::Update, GpuDriven/GpuSceneRegistry.hpp):
        // ONE primitive per drawn object (key {Entity, slot}; the static, generic and slot-material records of one
        // entity with one world share slot 0, the skinned records of an entity are its next slots in submission
        // order, an ISM / foliage field is one primitive with an instance per field instance). The registry keeps
        // each primitive's index while views keep queueing it: its PrevLocalToWorld is the world it was drawn
        // with last frame, and a skinned primitive's row names this frame's and the previous frame's palette in
        // the scene's ObjectBones. Writes each queued record's primitive and instance back (GpuPrimitive,
        // GpuInstance, FirstGpuInstance) and uploads the scene's deltas in a graph of its own, so
        // CaptureFrameState hands every view pass the tables its draws will read at their final size. Called by
        // SceneRenderer once per frame per view after the queues are complete and before any pass is declared.
        // Errors: an upload that could not be declared or executed (the view draws nothing this frame: its
        // passes would read rows that are not this frame's).
        // @p otherRigid: rigid primitives drawn by another renderer of the view (the terrain renderer's landscape
        // tiles) that still need a GpuScene instance - their TLAS custom index; their instances, in order, are
        // GetOtherGpuInstances().
        // @p skinCache (the scene's GPU skin cache, null = none: every skinned primitive skins in the vertex
        // stage) writes each skinned primitive's cached-streams address (RayTracingSkinCache::PrepareRows), then
        // - after the palettes' upload - runs the scene's skin step for what no earlier view of the absolute
        // frame @p frame skinned (RayTracingSkinCache::SkinFrame: its own graph, executed before any view graph).
        // A skin step that fails is logged and its primitives lose the address; the view still draws.
        [[nodiscard]] Common::BoolResultStr SyncGpuScene( GpuDriven::GpuSceneRegistry&     registry,
                                                          std::span<const MotionRecord>    otherRigid,
                                                          RayTracing::RayTracingSkinCache* skinCache,
                                                          uint64_t                         frame );

        // The scene's GPU scene of this frame (SyncGpuScene; null before the first sync): the skin cache skins
        // from the same palette at the same BoneOffset the raster does - one source of the pose - and the global
        // distance field's objects read a primitive's previous world from it.
        // The matrix the casters of shadow view @p view draw with (m_CascadeCasterVP; view = slot * kMaxCascades + c
        // for a cascade, kVsmViewBase + level for a VSM level): the GPU cull's caster views test the same box.
        [[nodiscard]] const glm::mat4& GetCasterViewProjection( const uint32_t view ) const
        {
            return m_CascadeCasterVP[view];
        }
        // The world size of one map texel of cascade view @p view (slot * kMaxCascades + c, UpdateCascades), 0
        // for a view no cascade was fitted for this frame: what a caster picks its own detail by (TerrainRenderer
        // dices the displaced landscape per cascade, as UE's Nanite dices every view at its own resolution).
        [[nodiscard]] float GetCascadeWorldPerTexel( const uint32_t view ) const
        {
            if ( view >= kCascadeViews )
                return 0.0f;
            const DirectionalShadowSlot& slot    = m_DirectionalShadow[view / kMaxCascades];
            const uint32_t               cascade = view % kMaxCascades;
            return cascade < slot.Fitted ? slot.WorldPerTexel[static_cast<glm::length_t>( cascade )] : 0.0f;
        }

        // THE SCENE'S BINNED DRAWS (GpuDriven::DrawCommandCache, UE FCachedPassMeshDrawList): fed by SyncGpuScene
        // with the primitives whose draws changed, one entry per primitive gathered across its section records,
        // each submesh's PSO the `.Binned` cell of its material for the pass (ResolveBinnedPipeline). Null before
        // the first sync. The feed rebuilds the host tables; their upload, the cull and the issue are the frame's.
        [[nodiscard]] const GpuDriven::DrawCommandCache* GetDrawCommandCache() const
        {
            return m_DrawCommandCache.get();
        }
        [[nodiscard]] const GpuDriven::GpuScene* GetGpuScene() const
        {
            return m_GpuScene;
        }

        // GPU-DRIVEN PASSES (SceneRenderer::AddFrameDrawBins): which (pass, view) draws this frame from GPU-culled
        // bins. BeginFrameBins forgets the last frame's issues and uploads the cache's tables ("DrawCommands:
        // Upload"); SetFrameBinned hands one view's culled draws to the lists of (pass, view): the main view
        // (kBinnedMainView) of MainViewPass, or a sun cascade's layer of ShadowCascade: its static layer (view
        // slot * kMaxCascades + c, CascadeBinsDrawn: the frame the cache is redrawn) and its movable layer
        // (CascadeMovableView( view ), CascadeMovableBinsDrawn: every frame it has movers), the cull splitting the
        // pass's bins between them per instance (GpuCull_CastsRestPose, CullViewFlags::kCaster*Layer). An active
        // (pass, view) issues its bins and its CPU loops drop every record the bins hold
        // (DrawCommandCache::SubmeshesBinned); any other draws everything on the CPU, as before.
        static constexpr uint32_t                                      kBinnedMainView = ~0u;
        [[nodiscard]] Common::ResultStr<GpuDriven::DrawCommandBuffers> BeginFrameBins( RDG::Builder& graph );
        [[nodiscard]] GpuDriven::MeshPass                              MainViewPass() const;
        [[nodiscard]] bool                                             CascadeBinsDrawn( uint32_t view ) const;
        // THE CASCADE CONTENT HASH (CPU-ISM-SHADOW; UE VSM: instance invalidation decided on the GPU). Sun cascade
        // @p view's content-hash cull view this frame (GpuDriven::CullViewFlags::kContentHash): its caster matrix
        // grown by CascadeContentHash::kMarginCells cells per side, its placement's texel origin and a cell's
        // side; nullopt when the cascade is not fitted or the map side is no multiple of kCellsPerMap.
        struct CascadeContentView
        {
            glm::mat4  GrownViewProjection{ 1.0f };
            glm::ivec2 TexelOrigin{ 0 };
            int32_t    CellTexels = 0;
        };
        [[nodiscard]] std::optional<CascadeContentView> CascadeContentViewOf( uint32_t view ) const;
        // Hands the frame's cells (MainPhaseResult::ContentHash, @p regions regions; region r = cascade
        // @p regionViews[r]) to a frame-in-flight readback slot, and reads the slot it replaces (the newest answer
        // per cascade: CascadeContentAnswer).
        void ExtractCascadeContent( RDG::Builder& graph, uint64_t frameIndex, const RDG::BufferRef& cells,
                                    uint32_t regions, std::span<const uint32_t> regionViews );
        // A SCROLLED cascade's strips this frame (PlanCascadeCache: the caster matrices of the strips its shift
        // uncovered), which its static layer's GPU cull keeps the casters of; empty - not scrolled.
        [[nodiscard]] std::span<const glm::mat4> CascadeScrolledStrips( uint32_t view ) const;
        [[nodiscard]] bool CascadeMovableBinsDrawn( uint32_t view ) const;
        // The caster view of cascade view @p view's movable layer (ShadowCascadePasses' "<cascade>.Movable").
        [[nodiscard]] static constexpr uint32_t CascadeMovableView( const uint32_t view )
        {
            return kCascadeMovableViewBase + view;
        }
        // True when (pass, view) issues its bins this frame. Refused (false, logged once per message, the view
        // stays on the CPU) when a non-empty group's carrier cannot be made; false without a log for a view that
        // issues no bins (a cascade layer not drawn this frame). The main view's answer is phase 2's precondition
        // (SetFramePostOcclusion), so it is part of the cluster cut's two-pass decision
        // (SceneRenderer::FrameDrawBins::ClusterMainTestsHzb). @p clusterCut: the view's cull appended its VG
        // instances as cluster-cut candidates (a cut view of SceneRenderer::FrameDrawBins::CutViewOf, the frame
        // appending candidates), so they are in none of its bins - its CPU walk asks the view's cluster raster
        // (CpuWalkHidden); false: every instance the view sees, VG or not, is in its bins.
        bool SetFrameBinned( GpuDriven::MeshPass pass, uint32_t viewKey, const GpuDriven::CulledDraws& culled,
                             const GpuDriven::DrawCommandBuffers& upload, const GpuDriven::GpuSceneBuffers& scene,
                             bool clusterCut );
        // TWO-PHASE OCCLUSION, phase 2 (UE FInstanceCullingContext's post pass): the draws of the instances phase
        // 1 held back by last frame's HZB that this frame's HZB finds visible, issued by a second slice of (pass,
        // view) with the same carriers and tables as its phase-1 issue (SetFrameBinned) and @p culled's own
        // indirect args. False (nothing to draw) when (pass, view) has no phase-1 issue this frame: its CPU lists
        // then draw every record, phase 2 has nothing it held back.
        bool SetFramePostOcclusion( GpuDriven::MeshPass pass, uint32_t viewKey,
                                    const GpuDriven::CulledDraws& culled );
        // THE VIRTUAL-GEOMETRY RASTER of (pass, view) (MeshRendererDetail::VirtualGeometryIssue): attached to its
        // phase-1 issue (CullPhase::Main, SetFrameBinned first) or its phase-2 issue (CullPhase::PostOcclusion,
        // SetFramePostOcclusion first: the post cut's clusters, drawn by the post-occlusion pass), the carriers of
        // the groups' `.VirtualGeometry` cells made here. False (logged once per message, nothing attached)
        // without that issue, for bins of another group count or when a carrier cannot be made. A group whose
        // cell has no VG twin draws none of its clusters: logged once per cell.
        bool SetFrameVirtualGeometry( GpuDriven::MeshPass pass, uint32_t viewKey, GpuDriven::CullPhase phase,
                                      MeshRendererDetail::VirtualGeometryIssue issue );
        // THE VIRTUAL-GEOMETRY RASTER OF THE VIRTUAL SHADOW MAP (VIRT-GEOM step 17; UE: Nanite rastering every
        // VSM clipmap level in one pass, the view per cluster). @p issue: one cut of all the levels (cut view =
        // level, GpuVisibleCluster::View) binned into the groups of the VSM's own table (MeshPass::VirtualShadow),
        // drawn by ONE "VSM: Raster VirtualGeometry" after the levels' CPU casters, each group with its
        // `.VirtualGeometry` twin (IndirectGroup::VirtualGeometryPipeline:
        // `<T>/<Static|Instanced>.VirtualShadowDepth
        // [.Parallax].Binned.VirtualGeometry` on the VSM caster state), whose vertex stage takes the level's
        // matrix and fragment stage the level's page table from the cluster's view. The carriers are made by
        // AddVirtualShadowRasterPasses; from then the levels' CPU walk leaves out every submesh whose clusters a
        // twin draws (CpuWalkHidden). Writes per group whether its clusters are drawn into the caller's @p drawn
        // (kept by the caller across frames: no vector per frame; left empty when refused). A group with clusters
        // binned and no twin is one of two states, both kept in the CPU walk (no shadow is lost):
        //  - a FALLBACK (UE: a material with no Nanite path renders the fallback mesh - a known state, not an
        //    error) when its template declares no `.VirtualGeometry` twin (DeclaresVirtualGeometryTwin): counted
        //    per VG submesh binned into it (VsmFallbackSubmeshes), never logged;
        //  - a HELD REFUSAL when the template declares the twin and no pipeline was made of it: logged once per
        //    cell, counted (VsmRefusedGroups; must be 0).
        // Refused without the VSM caster pipelines, a draw command cache or for bins of another group count;
        // nothing is attached then.
        [[nodiscard]] Common::BoolResultStr
        SetVirtualShadowGeometry( MeshRendererDetail::VirtualGeometryIssue issue,
                                  const GpuDriven::DrawCommandBuffers&     upload,
                                  const GpuDriven::GpuSceneBuffers& scene, std::vector<uint8_t>& drawn );
        // THE ONE RULE "this submesh of a static entry or an ISM / foliage field would be drawn as clusters in @p
        // pass" (MeshPass::ShadowCascade: a sun cascade's static layer; MeshPass::VirtualShadow: the VSM levels;
        // one rule for both, VIRT-GEOM steps 24-25): bit i of @p words (word i / 64, for EVERY submesh of the mesh
        // - no 64 cap) set when submesh i of @p mesh, not in @p hidden (the entry's own hidden sections), with
        // uploaded DAG rows, is binned at lod 0 for @p primitive (a static entry's GpuPrimitive, a field's
        // m_FieldPrimitives row) into a group of @p pass with a `.VirtualGeometry` twin
        // (DrawCommandCache::SubmeshVirtualGeometryPipeline, which the GPU's raster binning reads the same way).
        // @p words is left EMPTY when the mesh has no uploaded DAG rows (not a VG primitive: InstanceCull appends
        // no candidate for it, its bins draw it), else resized to the mesh's words. The VSM page cache reads it
        // before this frame's cut exists (CollectVirtualShadowInvalidation).
        void ClusterableSubmeshes( GpuDriven::MeshPass pass, const Desert::Mesh* mesh,
                                   GpuDriven::PrimitiveIndex primitive, uint64_t hidden,
                                   std::vector<uint64_t>& words ) const;
        // THE VSM RASTER RULE, shared by the levels' CPU walk (CpuWalkHidden, this frame's carriers) and the page
        // cache's signature (CollectVirtualShadowInvalidation, last frame's): ClusterableSubmeshes(VirtualShadow)
        // with every submesh whose group's `.VirtualGeometry` twin is NOT in @p carriedTwins cleared - only what
        // the cluster raster really draws. @p carriedTwins: sorted twins whose carrier
        // AddVirtualShadowRasterPasses made under an accepted draw list (m_VsmCarriedTwins). Keyed by the twin,
        // not the group index: a group table rebuilt between two frames keeps the meaning (a carrier depends only
        // on the twin's shader).
        void VsmRasteredSubmeshes( const Desert::Mesh* mesh, GpuDriven::PrimitiveIndex primitive, uint64_t hidden,
                                   std::span<const GraphicsPipeline* const> carriedTwins,
                                   std::vector<uint64_t>&                   words ) const;
        // VsmFallbackSubmeshes of this frame: the static and ISM queues' VG submeshes binned into a group flagged
        // in
        // @p fallbackGroups (index = VirtualShadow table group). Called by SetVirtualShadowGeometry.
        [[nodiscard]] uint32_t CountVsmFallbackSubmeshes( const std::vector<uint8_t>& fallbackGroups ) const;
        // THE CPU WALK OF ONE RECORD (a static entry, an ISM / foliage field, a generic or skinned draw) in one
        // view - THE ONE RULE "who draws this submesh" for every view pass (main Forward / GBuffer, a sun
        // cascade's layer, a VSM level; VIRT-GEOM step 26). UE: a primitive the cluster path cannot draw is drawn
        // by its fallback mesh, never lost. Returns the submeshes the CPU walk HIDES (the record's own @p hidden
        // plus what the GPU draws), or empty when the GPU draws every submesh the record shows:
        //  - a VG primitive in a cut view (a binned view whose cull appended cluster-cut candidates,
        //    ClusterWalkView::ClusterCut: InstanceCull took its instances out of the view's bins,
        //    m_FrameClusterCandidates; a VSM level: its mesh has uploaded DAG rows): the
        //    walk hides only the submeshes the view's cluster raster DRAWS - a candidate DAG binned into a group
        //    whose `.VirtualGeometry` carrier the issued raster holds (a VSM level: the carriers
        //    AddVirtualShadowRasterPasses made before the levels' walks). A refused cut, a refused carrier, a
        //    twin-less group or a submesh with no candidate DAG: walked, so the clusters it would have drawn are
        //    drawn by the mesh's own submeshes;
        //  - any other record of a binned view - a VG primitive of a view that takes no cut (the deferred depth
        //    prepass) included: its instances are in the view's bins - empty when the
        //    bins hold every shown submesh (DrawCommandCache::SubmeshesBinned), else @p hidden;
        //  - a view with neither (a local light, a refused bins issue): @p hidden.
        // The words past the first 64 submeshes are interned in m_ClusterWalkWords (kept until the next
        // ResetVirtualShadowGeometry), so equal masks share one view and their records batch together.
        struct ClusterWalkView
        {
            GpuDriven::MeshPass Pass = GpuDriven::MeshPass::Forward;
            // The view issues GPU-culled bins this frame (FrameBinned): the main view or a sun cascade's static or
            // movable layer (CascadeMovableView).
            bool BinsIssued = false;
            // The view is a cluster-cut view of the draw bins (SetFrameBinned's clusterCut): its VG instances are in
            // none of its bins, only in its cut. False: they are in its bins like any other instance.
            bool ClusterCut = false;
            // The phase-1 cluster raster's carriers per Table->Groups (BinnedIssue::VirtualGeometry, made by
            // SetFrameVirtualGeometry; a VSM level: AddVirtualShadowRasterPasses, null entries until made); null:
            // no raster this frame (no cut, or the cut / its carriers / its draw list refused).
            const std::vector<const MaterialExecutor*>* RasterCarriers = nullptr;
            // A VSM level: the view issues no bins; its VG-only raster (m_VsmGeometry) carries RasterCarriers.
            bool VirtualShadow = false;
        };
        [[nodiscard]] ClusterWalkView BinnedWalkView( GpuDriven::MeshPass pass, uint32_t viewKey ) const;
        [[nodiscard]] ClusterWalkView VirtualShadowWalkView() const;
        [[nodiscard]] ClusterWalkView UnbinnedWalkView( GpuDriven::MeshPass pass ) const;
        // @p mesh: the record's mesh (its submesh count; a StaticMesh's DAG rows for a VSM level), may be null.
        [[nodiscard]] std::optional<SubmeshMask> CpuWalkHidden( const ClusterWalkView&    view,
                                                                const Desert::Mesh*       mesh,
                                                                GpuDriven::PrimitiveIndex primitive,
                                                                uint64_t                  hidden );
        // The same walk into the caller's @p words (a concurrent build's DrawBuildScratch::WalkWords), not the
        // renderer's m_ClusterWalkScratch: reads only this frame's candidates and the draw-command cache.
        [[nodiscard]] std::optional<SubmeshMask> CpuWalkHidden( const ClusterWalkView&    view,
                                                                const Desert::Mesh*       mesh,
                                                                GpuDriven::PrimitiveIndex primitive,
                                                                uint64_t hidden, std::vector<uint64_t>& words );
        // THE SAME WALK WITHOUT ITS ONE WRITE (a chunk of the static walk on a setup task, BuildStaticDraws; UE
        // FParallelMeshDrawCommandPass: the chunks compute, only the interning stays serial). Nullopt: the record
        // is not walked. Else Low is the mask's first word and @p words[1, 1 + HighWords) the words past submesh
        // 63 that CpuWalkHidden interns (InternWalkWords); HighWords 0: the mask is Low alone. Writes only @p
        // words.
        struct UninternedWalk
        {
            uint64_t    Low       = 0;
            std::size_t HighWords = 0;
        };
        [[nodiscard]] std::optional<UninternedWalk> CpuWalkUninterned( const ClusterWalkView&    view,
                                                                       const Desert::Mesh*       mesh,
                                                                       GpuDriven::PrimitiveIndex primitive,
                                                                       uint64_t                  hidden,
                                                                       std::vector<uint64_t>&    words ) const;
        // The mask of first word @p low and words past submesh 63 @p high, interned in m_ClusterWalkWords: a
        // serial walk's write (nullopt in a setup task, MissInSetupTask).
        [[nodiscard]] std::optional<SubmeshMask> InternWalkWords( uint64_t low, std::span<const uint64_t> high );
        // THE CLUSTER-CUT CANDIDATES OF THIS FRAME (SceneRenderer::AddFrameDrawBins, after InstanceCull's main
        // phase took them with candidates appended): per primitive, the submeshes whose DAG rows InstanceCull
        // appends for EVERY cut view (MakeVirtualGeometryCandidates' spans, one bit per submesh, no 64 cap). Such
        // a primitive's instances reach no bin of a cut view (ClusterWalkView::ClusterCut); a view that takes no
        // cut bins them. Forgotten by BeginFrameBins.
        void SetFrameClusterCandidates(
             std::unordered_map<GpuDriven::PrimitiveIndex, std::vector<uint64_t>> submeshes );
        // A new frame of the virtual shadow map's cluster path: drops the last frame's issue, its refusal and
        // fallback counts and its interned walk masks. Called by SceneRenderer before every frame's graph, so a
        // frame whose method is not VirtualMaps (or whose VSM cut is refused) reports no refused groups.
        void ResetVirtualShadowGeometry();
        // Groups of the VirtualShadow table holding bins but no `.VirtualGeometry` twin in this frame's VSM
        // cluster raster (SetVirtualShadowGeometry): their clusters are not rastered, their primitives stay in the
        // walk.
        [[nodiscard]] uint32_t VsmRefusedGroups() const
        {
            return m_VsmRefusedGroups;
        }
        // VG submeshes (one per static queue entry showing it) binned this frame into a VirtualShadow group whose
        // template declares no `.VirtualGeometry` twin: walked on the CPU by design (SetVirtualShadowGeometry).
        [[nodiscard]] uint32_t VsmFallbackSubmeshes() const
        {
            return m_VsmFallbackSubmeshes;
        }

        // The GpuScene is THIS FRAME's: SyncGpuScene binds it, SceneRenderer::EndScene forgets it. A capture that
        // runs before the sync (the terrain's, SceneRenderer::OnUpdate) then finds none instead of the previous
        // frame's, whose Scene a scene switch has already destroyed.
        void ForgetGpuScene()
        {
            m_GpuScene = nullptr;
        }
        // The GpuScene instance rows imported into THIS view's graph (SceneRenderer::AddFrameDrawBins, before any
        // mesh pass is added; invalid when the tables could not be imported): what an ISM's instanced cell reads
        // its instances from by id (SurfaceSceneInstancesVariant). Without them an ISM packs its transforms.
        void SetFrameSceneInstances( const RDG::BufferRef instances )
        {
            m_FrameSceneInstances = instances;
            m_ViewIsmIdsReady     = false; // PrepareViewIsmIds fills this frame's
        }
        // CPU-ISM-PACK (UE FInstanceCullingContext: the instance ids uploaded ONCE per view): every queued ISM's
        // main-view cull (LookUpIsmViewCull, the stale ones on setup tasks) and its kept GpuScene instances
        // (FirstGpuInstance + Indices, level-ordered) appended into the view's one uint32 buffer, uploaded at its
        // final size - after SetFrameSceneInstances, before any mesh pass is added (SceneRenderer::
        // AddFrameDrawBins). Every scene-id cell of every pass is lent that buffer and draws base + run ranges of
        // it. Nothing when the scene's instance rows are not in this graph: the ISMs then pack their transforms.
        void PrepareViewIsmIds();

        // The GpuScene instances of SyncGpuScene's @p otherRigid records, in their order. Valid until the next
        // sync.
        [[nodiscard]] std::span<const uint32_t> GetOtherGpuInstances() const
        {
            return m_OtherGpuInstances;
        }

        // Scalability Shadows.WindAnimationDistance (SceneRenderer::BeginScene, cm): how far from the main camera
        // a swaying foliage instance is still a mover in the shadow passes (ShadowIsmInstances; the GPU cull's
        // caster views carry the same value, Common/ShadowWind.glslh).
        void SetShadowWindDistance( const float centimetres )
        {
            m_ShadowWindDistance = centimetres;
        }
        [[nodiscard]] float GetShadowWindDistance() const
        {
            return m_ShadowWindDistance;
        }
        // Scalability Shadows.FoliageShadowLodBias (SceneRenderer::BeginScene; UE r.Shadow LOD bias for the grass):
        // each step draws a grass blade into every cascade as if the cascade's texel were twice as wide - one
        // strip level coarser and thinned as the camera thins at twice the distance (BladeShadowSet). 0..3.
        void SetFoliageShadowLodBias( const int steps )
        {
            m_FoliageShadowLodBias = steps;
        }
        // WHO SWAYS IN WHICH CASCADE (UE: WPO off in the clipmap levels / cascades no receiver near the camera
        // samples; r.Shadow.Virtual.Clipmap.WPODisableDistance): the shadow wind distance shadow view @p view
        // measures its swaying casters by. A sun cascade no receiver within Shadows.WindAnimationDistance of the
        // camera can sample gets 0 - every caster casts its rest pose, the cascade's static layer holds them and
        // the wind neither redraws nor invalidates it; every other view (the near cascades, their movable layers,
        // the VSM levels, the local and rain views) gets Shadows.WindAnimationDistance. Cascade c >= 2 is such a
        // cascade when the distance is within cascade c-2's split: a receiver no farther from the camera than it
        // lies inside cascade c-2's square (centred on the camera, half side >= the split: ComputeShadowCascades),
        // or past its texel snap deep inside cascade c-1's, so it picks one of them (Mesh/CascadedShadow.glslh
        // chooseCascade) and cross-fades at most into cascade c-1. One answer for the CPU walk
        // (CascadeStaticSignature, BuildShadowCascadeDraws), the GPU cull's caster views (LodOrigin.w) and the
        // vertex stage (CasterWindOrigin).
        [[nodiscard]] float ShadowWindDistanceOf( uint32_t view ) const;
        // THE CASTER'S REACH (GPU-FRAME-6; UE r.Shadow.Virtual.Clipmap.WPODisableDistance keeps WPO only in the
        // levels its receivers read): the inner distance of shadow view @p view's movers' ring for a caster whose
        // every point lies within @p reach (cm) of its instance point - the instance point nearer the main camera
        // than it casts its rest pose into the static layer too. In cascade c >= 2 its shadow then falls within
        // reach x (1 + 1 / tan(elevation)) of it on the ground it stands on, so on receivers no farther from the
        // camera than cascade c-2's split, which pick a tighter cascade (the rule
        // above): the cascade's movers are only the ring [inner, Shadows.WindAnimationDistance). 0 (no ring) for
        // cascades 0 and 1, every other view and a light at or below the horizon. Read by the vertex stage's split
        // (ViewFrame::CasterWindRing, CasterWindLayer.glslh) and the same section's CascadeStaticSignature.
        [[nodiscard]] float ShadowWindInnerOf( uint32_t view, float reach ) const;
        // The ring of shadow view @p view as Common/ShadowWind.glslh reads it (ShadowWindRingInner): x = cascade
        // c-2's split (cm), y = the light's fall across / down; zero (no ring) where ShadowWindInnerOf is 0 for
        // every reach. ONE ring for every caster path (GPU-FRAME-7): the GPU-instanced sections
        // (ShadowWindInnerOf), the CPU ISM walk (ShadowIsmInstances' WindPoint / CasterReach,
        // CascadeStaticSignature, BuildShadowCascadeDraws) and the GPU cull's caster views
        // (GpuCullView::CasterRing, GpuCull_CastsRestPose).
        [[nodiscard]] glm::vec4 ShadowWindRingOf( uint32_t view ) const;
        // The rule's measure as shadow view @p view's light camera carries it (ViewFrame::CasterWindOrigin): xyz
        // the main camera's origin - the point ShadowIsmInstances measures from - w ShadowWindDistanceOf( view ).
        [[nodiscard]] glm::vec4 CasterWindOrigin( uint32_t view ) const;
        // Scalability Effects.SkinCache (SceneRenderer::BeginScene); the scene's skin cache follows it each frame.
        void SetSkinCacheEnabled( const bool enabled )
        {
            m_SkinCacheEnabled = enabled;
        }
        [[nodiscard]] bool IsSkinCacheEnabled() const
        {
            return m_SkinCacheEnabled;
        }
        // This view's graph refs of the skin-cache streams its skinned rows address (RayTracingSkinCache::
        // DeclareReads fills them, SceneRenderer clears them per frame): every pass that draws skinned primitives
        // declares them StorageRead (the G-buffer / forward geometry pass, the cascade and local shadow passes,
        // the silhouette).
        [[nodiscard]] std::vector<RDG::BufferRef>& SkinCacheReads()
        {
            return m_SkinCacheReads;
        }
        // The morph twin's output external of @p entity when @p mesh is its blended twin (the external
        // UpdateMorphTargets' graph wrote through), else null: the skin step reads the morphed vertices through
        // it.
        [[nodiscard]] RDG::ExternalBuffer* FindMorphOutput( uint32_t entity, const SkinnedMesh* mesh );
        // This view's ObjectBones as the skin pass's graph external.
        [[nodiscard]] RDG::ExternalBuffer& SkinCacheBonesImport()
        {
            return m_SkinCacheBonesImport;
        }

        // This frame's static queue, GpuScene rows assigned (SyncGpuScene): what the view's TLAS instances come from
        // (RayTracingSceneRenderer). Valid until the next frame's queue is built.
        [[nodiscard]] std::span<const StaticMeshRenderData> GetStaticQueue() const
        {
            return m_StaticQueue;
        }
        // This frame's skinned queue, rows assigned: what the ray-tracing skin cache skins (RayTracingSkinCache).
        [[nodiscard]] std::span<const SkinnedMeshRenderData> GetSkinnedQueue() const
        {
            return m_SkinnedQueue;
        }
        // This frame's generic queue, rows assigned: the records with VisibleInRayTracing enter the TLAS.
        [[nodiscard]] std::span<const GenericMeshRenderData> GetGenericQueue() const
        {
            return m_GenericQueue;
        }
        // This frame's ISM / foliage fields; one TLAS instance per field instance (FirstGpuInstance + i).
        [[nodiscard]] std::span<const InstancedMeshRenderData> GetInstancedQueue() const
        {
            return m_InstancedQueue;
        }
        // This frame's GPU-instanced sections (the landscape grass): their TLAS rows are written on the GPU.
        [[nodiscard]] std::span<const GpuInstancedRenderData> GetGpuInstancedQueue() const
        {
            return m_GpuInstancedQueue;
        }
        // Each queued field's GpuScene primitive (GpuSceneRecordRows::FieldPrimitives as the draw-command feed saw
        // them), index for index with GetInstancedQueue(); shorter when the feed has not run this frame.
        [[nodiscard]] std::span<const GpuDriven::PrimitiveIndex> GetFieldPrimitives() const
        {
            return m_FieldPrimitives;
        }
        // Cascaded shadow maps: the CEILING on directional-shadow cascades — how many the arrays below
        // hold and how many the ShadowUB block can carry. It is the block's own constant, so the cascades
        // this renderer can fit and the cascades a shader can read are one number and cannot drift apart.
        //
        // HOW MANY ARE ACTUALLY ALLOCATED IS NOT THIS, and the distinction is the whole of the preview
        // shadow budget: the count, the resolution and the distance come from the renderer's own
        // ShadowQuality (SceneRenderer::GetShadowQuality), read in Initialize and again only by RebudgetShadows.
        // See ShadowCascades.hpp.
        static constexpr uint32_t kMaxCascades = SceneResources::kMaxCascades;
        static_assert( kMaxCascades == kMaxShadowCascades,
                       "The ShadowUB block's cascade count and the fitter's array bound are the same "
                       "number seen from two sides; a renderer that fitted more than the block can carry "
                       "would write matrices no shader ever reads." );
        // EVERY SHADOWED DIRECTIONAL LIGHT'S CASCADES, by one index: cascade VIEW v = slot * kMaxCascades + c
        // (SceneResources::ShadowUBData::LightViewProj's own index), slot 0 the sun, 1 atmosphere light 1. Each
        // slot is fitted, cached, drawn and published by the same code (UpdateCascades, PlanCascadeCache,
        // ShadowCascadePasses, DirectionalShadows). Its array LAYER is CascadeLayer(v): the slots are packed
        // in the array at the budget's cascade count.
        static constexpr uint32_t kDirectionalShadowSlots = SceneResources::kMaxShadowedDirectionalLights;
        static constexpr uint32_t kCascadeViews           = SceneResources::kMaxCascadeViews;

        virtual Common::BoolResultStr Initialize() override;

        // The raster passes this renderer hands the frame build (SceneRenderer::AddFrameShadowDepths,
        // AddFrameBasePass, AddFrameSilhouette and the debug overlay add them, in that order of the frame). Each
        // is empty (no TargetFramebuffer) when its pipeline or target was never built: the frame then has no such
        // pass. The forward geometry of the scene target; LOAD, after the sky that clears it.
        [[nodiscard]] SystemRasterPass GeometryPass();
        // The silhouette mask (always produced and cleared, so the Jump Flood outline has a fresh input every
        // frame; outline visibility is JumpFloodOutlineRenderer's).
        [[nodiscard]] SystemRasterPass SilhouettePass();
        // THE SUN'S CASCADES FOR THIS FRAME, in the order SceneRenderer::AddFrameShadowDepths adds them (UE: the
        // cached shadow maps of a directional light). Per cascade:
        //   StaticPasses  - only when its static casters' cache is stale: clears the cache and draws them;
        //   Copies        - the cache into the cascade's layer of the sampled array, when the map must be rebuilt
        //                   this frame;
        //   MovablePasses - LOADS the map and draws the movable casters (skinned, swaying foliage, the terrain).
        // A cascade whose matrix and casters stood still and that holds no movable caster has no work at all: its
        // map is last frame's.
        // A SCROLLED cascade (UE's CSM scrolling, PlanCascadeCache) instead: Scrolls clears its layer and copies in
        // the part of the cache its new placement still covers, its ScrollPasses node LOADS the layer and draws the
        // static casters reaching the strips the shift uncovered, and a ToCache copy takes the layer back as the
        // cache before its movable casters are drawn.
        struct CascadeCacheCopy
        {
            std::shared_ptr<Image2D> From; // the cascade's cache
            uint32_t                 Layer = 0; // the cascade: its layer of the array
            std::string              Name;
            bool                     ToCache = false; // the layer into the cache (a scrolled cascade's static depth)
            // FromCache only: the texels (x, y, width, height) restored, the rest of the layer kept - a cascade
            // whose keyed movers moved over a still static layer (CascadeCacheState::MapMovableRect); empty, all.
            std::vector<glm::uvec4> Regions;
        };
        struct CascadeCacheScroll
        {
            std::shared_ptr<Image2D> From; // the cascade's cache
            uint32_t                 Layer = 0;
            glm::ivec2               Shift{ 0 }; // destination texel = source texel + Shift (CascadePlacement::ShiftTo)
            std::string              Name;
        };
        struct ShadowCascadeFrame
        {
            std::vector<CascadeCacheScroll> Scrolls;
            std::vector<SystemRasterPass>   StaticPasses;
            std::vector<SystemRasterPass>   ScrollPasses;
            std::vector<CascadeCacheCopy>   Copies;
            std::vector<SystemRasterPass>   MovablePasses;
        };
        // @p maps is this frame's AcquireCascadeMaps.
        [[nodiscard]] ShadowCascadeFrame ShadowCascadePasses( RDG::TextureRef maps );
        // The mesh part of SceneRenderer::SunCascadesRead ("does a program of this frame sample the sun's cascades
        // while the sun is a screen mask"): a blended draw of the generic queue, or a translucent section of the
        // static queue (glass, its front layer cell), in the camera's frustum, whose program declares
        // ShadowCascadesTexture (the opaque generic ones read the screen mask through their
        // DESERT_SUN_SHADOW_SCREEN_MASK permutation, the lit queue is shaded by the composite).
        [[nodiscard]] bool SunCascadeReadersQueued() const;
        // The mesh part of SceneRenderer::LocalShadowMapsRead ("does a program of this frame sample the local shadow
        // atlas while the local shadows are ray traced"): a visible blended draw of the generic queue, or a
        // translucent section of the static queue, whose program samples the atlas (the opaque generic ones read the
        // ray-traced masks through their DESERT_LOCAL_SHADOW_SCREEN_MASK permutation, the lit queue is shaded by the
        // composite).
        [[nodiscard]] bool LocalShadowMapReadersQueued() const;
        // The visible translucent sections of the static queue (RenderGlassManual's cells, TranslucentDrawFor) and
        // their front layer cells (FrontLayerShaderFor) whose compiled program samples @p textureName.
        [[nodiscard]] bool QueuedStaticTranslucentSamples( std::string_view textureName ) const;
        // A translucent section of the static queue in the camera's frustum: what "Deferred: Glass" can draw this
        // frame (DeclareGlassBindings walks the same queue through the same tests and draws a subset of these).
        // SceneRenderer's scene-copy decision (UE: the scene colour copy only for a view with a refractive
        // primitive): false, the glass node has nothing to draw and the frame needs no copy for it.
        [[nodiscard]] bool TranslucentSectionsVisible() const;
        // What writes the scene depth after the composite in this view: a generic draw in the camera's frustum
        // ("Deferred: Generic", every generic program writes it), else a translucent section in it whose draw
        // state writes it ("Deferred: Glass": its own cell's pipeline, or the stand-in's while the cell compiles).
        // SceneRenderer's clouds placement (GPU-ASYNC-WAIT): only with None is the depth final while the screen
        // probes gather, so the march can fill the graphics queue between their fork and join.
        enum class LateDepthWriter : uint8_t
        {
            None,
            Generic,
            Translucent
        };
        [[nodiscard]] LateDepthWriter LateDepthWriterInView();
        // The visible queued forward draws whose compiled program samples @p textureName (blended ones only when
        // @p blendedOnly).
        [[nodiscard]] bool QueuedForwardDrawSamples( std::string_view textureName, bool blendedOnly ) const;
        // Every cascade's static cache marked never drawn: the next ShadowCascadePasses redraws and rebuilds each
        // fitted cascade. For a frame that draws the cascades after frames that did not (nothing kept them
        // current).
        void InvalidateCascadeCaches();
        // THIS FRAME DRAWS NOTHING FOR directional slot @p slot, though UpdateCascades fitted it: SceneRenderer's
        // frame decision, for a sun whose shadow is the screen mask and whose cascades no program of the frame
        // samples, while another slot (the moon) still draws the array. The slot publishes no cascade from here on
        // (DirectionalShadows: Count 0, so every receiver packed after this call reads it lit and the surface
        // cache traces it), and its views' caches are marked never drawn so the frame it is drawn again rebuilds
        // them. Called after UpdateCascades and before ShadowCascadePasses.
        void DropDirectionalShadow( uint32_t slot );
        // THE SUN'S CASCADES AS ONE DEPTH ARRAY in @p graph, layer c = cascade c (RDG::TextureDim::Tex2DArray, so
        // the lit passes spend one sampler on all of them: UE's whole-scene shadow depth target). The renderer
        // keeps it across frames (a cascade with no work keeps last frame's layer); the first frame after the
        // budget changed creates it, and every fitted cascade is then rebuilt into it. Invalid when this renderer
        // has no cascades.
        [[nodiscard]] RDG::TextureRef AcquireCascadeMaps( RDG::Builder& graph );
        // One depth-only pass per local shadow view whose cached depth is stale this frame, each into its own
        // tile of the atlas (it clears the tile, never the atlas), its mesh casters and then the non-mesh ones
        // (IShadowCaster: the terrain). SceneRenderer::AddFrameShadowDepths adds them after the cascades, LOADING
        // the atlas. The cache decision is made here (PlanLocalShadowCache), not in UpdateLocalShadows: a
        // non-mesh caster's draws are resolved after that (TerrainRenderer::PrepareFrame), and its key is theirs.
        // The span views this renderer's kept list (m_LocalShadowPassList): valid until the next call.
        [[nodiscard]] std::span<SystemRasterPass> LocalShadowPasses();

        // THE VIEW'S RAIN OCCLUSION (Graphic/RainOcclusion.hpp): a top-down depth of every caster in a box around
        // the camera, which the wetted surfaces read to stay dry under a roof. The budget comes from the
        // scalability catalogue (SceneRenderer::BeginScene); a changed budget remakes the map. UpdateRainOcclusion
        // runs once per frame before the graph records (beside UpdateCascades): it fits the box and decides
        // whether the map is redrawn this frame; RainOcclusionPass then hands that one pass, or none (the map
        // keeps its depth).
        void SetRainOcclusionQuality( const RainOcclusionQuality& quality );
        void UpdateRainOcclusion();
        [[nodiscard]] std::optional<SystemRasterPass> RainOcclusionPass();
        [[nodiscard]] std::shared_ptr<Image2D>        GetRainOcclusionImage() const
        {
            return m_RainOcclusionFB && m_RainOcclusionBox ? m_RainOcclusionFB->GetDepthAttachmentImage()
                                                           : nullptr;
        }
        [[nodiscard]] RainOcclusionUniforms GetRainOcclusionUniforms() const
        {
            return m_RainOcclusionBox ? PackRainOcclusion( *m_RainOcclusionBox, m_RainOcclusionQuality )
                                      : RainOcclusionUniforms{};
        }

        // EVERY SHADOW VIEW THIS RENDERER DRAWS, by one index. The FIXED views first, a known count per renderer:
        // 0..kCascadeViews-1 every shadowed directional light's cascades (slot * kMaxCascades + cascade); then one
        // view per cascade view for its MOVABLE layer, drawn in a pass
        // of its own, possibly in the same frame as the cascade's static layer, so it needs caster materials
        // (light-matrix UBO, instance and bone SSBOs) of its own — two passes through one material would both
        // read whichever upload came last; then one view per clipmap level of the sun's virtual shadow map ("VSM:
        // Raster"), whose casters draw into the physical pool through the VirtualShadowDepth cells
        // (CasterPipelinesOf), each level with materials of its own for the same reason; then the rain
        // occlusion's top-down view (RainOcclusionPass). LAST, from kLocalShadowViewBase, the point and spot
        // lights' views (LocalShadows.hpp), as many as this frame shadows: no count is fixed for them — the
        // only budget is the scene's atlas (UE: the shadow pool, not a number of views). The per-view caster
        // state below (m_ShadowMaterial, m_CascadeCasterVP, ...) is indexed by this index and grows with the
        // local views (EnsureShadowViewCount), so one caster walk (BuildShadowCascadeDraws) serves a cascade
        // and a cube face alike.
        static constexpr uint32_t           kCascadeMovableViewBase = kCascadeViews;
        static constexpr uint32_t           kVsmViewBase            = kCascadeMovableViewBase + kCascadeViews;
        // Each sun level's movers ("VSM: Raster Movable L<l>", ShadowCasterLayer::Movable) draw from a view of
        // their own, in the same frame as the level's still casters: materials of their own, as a cascade's
        // movable layer.
        static constexpr uint32_t kVsmMovableViewBase     = kVsmViewBase + kVsmClipmapLevelCount;
        static constexpr uint32_t kRainOcclusionView      = kVsmMovableViewBase + kVsmClipmapLevelCount;
        static constexpr uint32_t kLocalShadowViewBase    = kRainOcclusionView + 1u;
        static constexpr uint32_t kFixedShadowViews       = kLocalShadowViewBase;
        // A virtual shadow map view: a clipmap level of the sun (the fixed range), or a local view this frame's
        // "VSM: Raster" of a point / spot light's mip gave out (m_VirtualShadowViews, past the atlas rows).
        [[nodiscard]] bool IsVirtualShadowView( const uint32_t view ) const
        {
            if ( view >= kVsmViewBase && view < kRainOcclusionView )
                return true;
            return view < m_VirtualShadowViews.size() && m_VirtualShadowViews[view] != 0u;
        }

        // THE SUN'S VIRTUAL SHADOW MAP, before its page passes (SceneRenderer::AddFrameVirtualShadows): what
        // changed among the casters since the last frame the map was drawn. A static, generic or still instanced
        // caster and every non-mesh caster (IShadowCaster::AppendVirtualShadowEntries) that was added, removed or
        // changed hands its old and its new box; a caster that moves by itself (skinned, swaying foliage) hands
        // its box now and last frame, every frame. StaticCastersChanged: a masked caster's pipeline was still
        // compiling when the pages were drawn, so the cached pages lack it and every one is dropped.
        struct VirtualShadowInvalidation
        {
            std::vector<VsmInvalidationBox> Boxes;        // the still casters that changed: both layers
            std::vector<VsmInvalidationBox> MovableBoxes; // every mover instance, now and last frame: merged layer
            bool                            StaticCastersChanged = false;
        };
        // Into the caller's @p result (kept by the caller across frames: no box vectors per frame).
        void CollectVirtualShadowInvalidation( VirtualShadowInvalidation& result );
        // "VSM: Raster L<l>", one attachment-less raster node per clipmap level (RDG PassBuilder::RenderArea over
        // the 16k virtual map): every caster of the level's view (BuildShadowCascadeDraws, kVsmViewBase + l)
        // through the VirtualShadowDepth cells, then the non-mesh casters (IShadowCaster::DeclareVirtualShadow);
        // the fragments write the dirty pages of @p frame's pool. Between VirtualShadowMapRenderer::AddPagePasses
        // and AddProjection. Each level's casters are culled on the GPU first ("VSM: Cull Casters", @p map): only
        // the instances over a page drawn this frame are rasterised, and a level with none draws nothing.
        [[nodiscard]] Common::BoolResultStr AddVirtualShadowRasterPasses( RDG::Builder&                   graph,
                                                                          const VirtualShadowMapFrame&    frame,
                                                                          const VirtualShadowMapRenderer& map );

        // THE POINT AND SPOT LIGHTS' SHADOWS FOR THIS FRAME, before the graph records (SceneRenderer, after
        // UpdateCascades): finds or gives each light that casts a tile per view in the SCENE's atlas
        // (Graphic/LocalShadowCache.hpp), writes its ShadowView into its payload, fills the view table
        // GetLocalShadowRows hands the composite, and marks the views whose cached depth is stale — a view is
        // redrawn only when its light, its tile or a caster in its reach changed, whichever view of the scene
        // drew it last (UE: cached shadow maps of a stationary light, on FScene). Without @p shadowMaps (Shadows
        // Stochastic: MegaLights traces the local lights) no tile is given and no view drawn; every light that
        // casts is still marked ShadowCaster, which is what the traced visibility reads.
        void UpdateLocalShadows( std::vector<ShaderProtocols::PointLightPayload>& points,
                                 const std::vector<LocalLightShadowSettings>&     pointShadows,
                                 std::vector<ShaderProtocols::SpotLightPayload>&  spots,
                                 const std::vector<LocalLightShadowSettings>& spotShadows, bool shadowMaps );
        [[nodiscard]] const std::vector<LocalShadowViewPayload>& GetLocalShadowRows() const
        {
            return m_LocalShadowRows;
        }
        // This frame's shadowed point/spot lights as their casters see them (LocalShadowCasterLight), in view
        // order: what a non-mesh caster resolves its local-view draws for (TerrainRenderer::PrepareFrame), between
        // UpdateLocalShadows and LocalShadowPasses.
        [[nodiscard]] std::vector<LocalShadowCasterLight> GetLocalShadowCasterLights() const
        {
            std::vector<LocalShadowCasterLight> lights;
            lights.reserve( m_LocalCachePlans.size() );
            for ( const LocalShadowCachePlan& plan : m_LocalCachePlans )
                lights.push_back( plan.Caster );
            return lights;
        }
        // The scene's atlas depth image (D32F, Graphic/LocalShadowCache.hpp), or null while no light of the scene
        // has cast a shadow or outside a BeginScene/EndScene bracket.
        [[nodiscard]] std::shared_ptr<Image2D> GetLocalShadowAtlasImage() const;
#if DESERT_DEV_INSTRUMENTS
        // The bounding-box lines over the lit scene, depth-tested.
        [[nodiscard]] SystemRasterPass DebugLinesPass();
        // The gameplay debugger's Rendering category (Graphic/PrimitiveReadout.hpp): every queued record of
        // @p entity this frame, read from the queues the passes drew - call it before ClearQueues.
        [[nodiscard]] std::vector<PrimitiveDrawReadout> DescribePrimitive( uint32_t entity ) const;
#endif

        // Silhouette mask of the currently outlined meshes (white on the framebuffer clear color).
        // Consumed by JumpFloodOutlineRenderer to build the outline.
        // Deferred: renders the static-mesh queue into the scene renderer's G-buffer via a MANUAL render pass
        // (outside the graph — see the note in GeometryPass). No-op unless the deferred pipeline exists.
        // Called by SceneRenderer when RenderPath == Deferred, before the deferred lighting pass.
        // SETUP of the G-buffer node: builds the frame's G-buffer draw list and declares its binding blocks, each
        // with the scene/view inputs @p view its shader samples (the surface weather's rain occlusion map,
        // SurfaceWeather.glslh). The exec records that list.
        void DeclareGBufferDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr RenderGBufferManual( const RDG::PassContext& context ) const;
        // SETUP of "Deferred: Masked Depth Prepass" (before "Deferred: GBuffer", same depth; UE: the masked depth
        // prepass, r.EarlyZPass masked): every Masked GPU-instanced section (the landscape grass) through its
        // template's (Instanced x Depth) cell - the opacity mask's clip and nothing else. The G-buffer then draws
        // those sections at CloserOrEqual WITHOUT depth writes, so a hidden texel fails the depth test before its
        // fragment runs: alpha-tested overdraw (on Apple's TBDR, a discard also defeats hidden-surface removal) is
        // paid once, by the cheap mask, instead of by every layer of the full surface.
        void DeclareMaskedDepthDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr RenderMaskedDepthManual( const RDG::PassContext& context ) const;
        // THE GRASS VISIBILITY BUFFER (GRASS-VIS; UE 5 Nanite's raster into VisBuffer64 and its material pass).
        // SceneRenderer sets it per frame BEFORE "Deferred: Masked Depth Prepass": PostProcessSettings::
        // GrassVisibilityBuffer on a device with SupportsVisibilityBuffer64. While on, every blade section whose
        // (GrassBlade x Visibility) and (GrassBlade x VisibilityResolve) pipelines are Ready (m_GrassVisibilitySections,
        // decided in DeclareMaskedDepthDraws) leaves the masked depth prepass and the G-buffer and is drawn by the two
        // nodes below instead; one still compiling keeps the G-buffer path that frame.
        void SetGrassVisibilityActive( const bool active )
        {
            m_GrassVisibilityActive = active;
        }
        // THE DEFERRED EARLY-Z PASS (UE r.EarlyZPass; Scalability EarlyZPass). SceneRenderer sets it per frame
        // BEFORE "Deferred: Masked Depth Prepass". Off: no mesh is in the prepass, every one clips and writes its
        // own depth in the G-buffer. Masked: the masked meshes of every kind (static, auto-batched, ISM, skinned,
        // GPU-instanced) write their clipped depth there and the G-buffer draws them EQUAL, without writes and
        // without the clip (DESERT_SURFACE_DEPTH_EQUAL). All (UE r.EarlyZPass 2): the opaque meshes too, so the
        // G-buffer shades each texel once. The GPU-driven bins and the virtual-geometry clusters of both occlusion
        // phases are paired by the same rule (EarlyZSplit: their depth twins first, then EQUAL in the G-buffer).
        void SetEarlyZPass( const ::Common::Scalability::EarlyZPassMode mode )
        {
            m_EarlyZPass = mode;
        }
        [[nodiscard]] bool DrawsEarlyZ() const
        {
            return m_EarlyZPass != ::Common::Scalability::EarlyZPassMode::Off;
        }
        // SETUP of "Grass: Visibility Raster" (the G-buffer's depth LOADed, tested, not written; no colour): the
        // sections' (GrassBlade x Visibility) cells atomic-max (depth | row | triangle) into @p visibility.
        // @p postOcclusion (GRASS-VIS phase 2, "Grass: Visibility Raster Post-Occlusion"): only the sections' phase-2
        // records (GpuInstancedRenderData::PostRefs), into a list of their own.
        void DeclareGrassVisibilityRasterDraws( RDG::PassBuilder& pass, const SceneViewInputs& view,
                                                RDG::BufferRef visibility, bool postOcclusion );
        [[nodiscard]] Common::BoolResultStr RenderGrassVisibilityRasterManual( const RDG::PassContext& context,
                                                                               bool postOcclusion ) const;
        // SETUP of "Grass: Visibility Resolve" (@p path GrassBlade) and "Grass: Mesh Visibility Resolve" (@p path
        // Instanced, GRASS-VIS-MESH-2: the mesh grass's own GPU line) - the G-buffer LOADed, depth CloserOrEqual and
        // written: one full-screen triangle per section of @p path through its (path x VisibilityResolve) cell, drawn
        // by record 0 of @p resolveArgs (VirtualGeometry::VisibilityBuffer), reading @p visibility. ONCE a frame,
        // after phase 2's raster (GRASS-VIS-23): the camera's ids hold both phases' slots, so every word is shaded once.
        void DeclareGrassVisibilityResolveDraws( RDG::PassBuilder& pass, const SceneViewInputs& view,
                                                 RDG::BufferRef visibility, RDG::BufferRef resolveArgs,
                                                 MeshVertexPath path );
        [[nodiscard]] Common::BoolResultStr RenderGrassVisibilityResolveManual( const RDG::PassContext& context,
                                                                                MeshVertexPath path ) const;
        // SETUP of "Grass: Depth Prepass Post-Occlusion" (GRASS-VIS-13; the G-buffer's depth LOADed, no colour;
        // after "Grass: Post-Occlusion Finalize" wrote phase 2's records, before "Grass: GBuffer Post-Occlusion";
        // UE: the two-pass occlusion's second depth pass, grass HISM by the same rule as every mesh): the G-buffer
        // path's sections EarlyZAdmits, by their phase-2 records through their (path x Depth) cell, each only when
        // its DEPTH_EQUAL G-buffer half is Ready too (m_MaskedPrepassedPost). The exec records that list.
        void DeclareGrassDepthPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr
        RenderGrassDepthPostOcclusionManual( const RDG::PassContext& context ) const;
        // SETUP of "Grass: GBuffer Post-Occlusion" (GRASS-VIS phase 2, after this frame's HZB): the G-buffer path's
        // GPU-instanced sections (not through the visibility buffer) drawn by their phase-2 records, LOADing the
        // G-buffer. A section "Grass: Depth Prepass Post-Occlusion" drew (m_MaskedPrepassedPost) is drawn EQUAL,
        // without writes and without the clip (m_InstancedGBufferEqualPipeline); every other one with its own clip
        // and depth writes.
        void DeclareGrassGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr
        RenderGrassGBufferPostOcclusionManual( const RDG::PassContext& context ) const;
        // The sections this frame draws through the visibility buffer (empty: none, the G-buffer path for all).
        [[nodiscard]] bool DrawsGrassVisibility() const
        {
            return !m_GrassVisibilitySections.empty();
        }
        // Whether any of them is a section of @p path (GrassBlade: the blades; Instanced: the mesh grass).
        [[nodiscard]] bool DrawsGrassVisibility( const MeshVertexPath path ) const
        {
            return path == MeshVertexPath::Instanced ? m_GrassVisibilityMeshSections > 0u
                                                     : m_GrassVisibilitySections.size() > m_GrassVisibilityMeshSections;
        }
        // SETUP of "Deferred: GBuffer Post-Occlusion" (SceneRenderer::AddFrameDrawBinsPostOcclusion, after this
        // frame's HZB): only the main view's phase-2 bins (SetFramePostOcclusion), LOADing the G-buffer the
        // phase-1 draws wrote. The exec records that list: this frame's early-Z issue of phase 2 when "Deferred:
        // Depth Prepass Post-Occlusion" drew some of its groups (DeclareEarlyZPostOcclusionDraws), the view's own
        // phase-2 issue otherwise.
        void DeclareGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        // SETUP of "Deferred: Depth Prepass Post-Occlusion" (the G-buffer's depth LOADed, no colour; after the
        // phase-2 cull and cluster cut, before "Deferred: GBuffer Post-Occlusion"; UE: the two-pass occlusion's
        // second depth pass): the main view's phase-2 bins and clusters EarlyZSplit pairs, through their depth
        // twins. The exec records that list.
        void DeclareEarlyZPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr
        RenderEarlyZPostOcclusionManual( const RDG::PassContext& context ) const;
        [[nodiscard]] Common::BoolResultStr
        RenderGBufferPostOcclusionManual( const RDG::PassContext& context ) const;
        // SETUP of "Deferred: Forward Depth/Normal" (after the terrain's G-buffer, before the depth resolve and
        // every screen pass): the generic queue's OPAQUE draws through their template's (Static x DepthNormal)
        // program into G-buffer depth + normal, so the ray-traced sun trace and the screen passes see the meshes
        // that are shaded forward over the composite. The exec records that list. @p rayTracedSun: the frame
        // intends to ray trace the sun (SceneRenderer::SunShadowIsRayTraced), so "Deferred: Generic" shades the
        // sun-reading opaque programs through their DESERT_SUN_SHADOW_SCREEN_MASK permutation - the forward
        // pipeline whose readiness this prepass checks together with its own.
        void DeclareForwardDepthNormalDraws( RDG::PassBuilder& pass, const SceneViewInputs& view,
                                             bool rayTracedSun, bool localShadowMasks, bool virtualLocal );
        // SETUP of "Forward: Depth Prepass" (UE: the forward renderer's full DepthPass, FDepthPassMeshProcessor), the
        // forward path's first writer of the scene depth, every forward frame: every opaque mesh the base pass
        // shades - static, instanced, skinned through their (path x Depth) cell, the landscape grass, the opaque
        // generics through their template's DepthNormal program - into the scene depth alone. The base pass
        // (MeshGeometryPass) then tests CloserOrEqual against it and writes none, so both admit the SAME objects:
        // each records here what it drew (the cells' readiness of BOTH passes folded), and the base pass of the
        // same frame draws exactly that (PrepassCellDraw). The Wireframe view keeps its own depth write: its static
        // and instanced meshes are not prepassed. @p view: the scene/view inputs the cells sample.
        void DeclareDepthPrepassDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr RenderDepthPrepassManual( const RDG::PassContext& context ) const;
        [[nodiscard]] Common::BoolResultStr
        RenderForwardDepthNormalManual( const RDG::PassContext& context ) const;
        // Translucency pass: draws the meshes whose material's template is BlendMode Translucent over the
        // composited scene, each with its OWN template's cell/pipeline (TranslucentDrawFor), in the order of
        // TranslucentSortOrder: priority, then back to front from the frame's camera, stable on ties. Runs
        // inside the "Deferred: Glass" graph node whose @p context this is and records the list
        // DeclareGlassBindings built in the node's setup. Nothing to draw is success; a refused draw is the error.
        [[nodiscard]] Common::BoolResultStr RenderGlassManual( const RDG::PassContext& context ) const;
        // SETUP of the "Deferred: Glass" node: the frame's translucent draw list, one binding block per cell (its
        // shader's reflected layout, its dedicated material as the other route), each carrying SceneColorTexture =
        // @p sceneCopy (this frame's snapshot of the opaque scene, FrameTransients::SceneColorCopy, sampled for
        // refraction) and the scene/view inputs of @p view the cell's shader has a slot for. Declares nothing
        // (and the node draws nothing) when no translucent object is visible.
        // @p frontLayer: this frame's front layer reflection and depth (Reflections.TranslucentFrontLayer), bound as
        // FrontLayerReflectionTexture / FrontLayerDepthTexture wherever the cell's shader has the slots - the graph's black
        // texture for both when the frame has no front layer.
        struct GlassFrontLayer
        {
            RDG::TextureRef Reflection;
            RDG::TextureRef Depth;
        };
        void DeclareGlassBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneCopy, const SceneViewInputs& view,
                                   const GlassFrontLayer& frontLayer );
        // THE TRANSLUCENT FRONT LAYER G-BUFFER (UE RenderFrontLayerTranslucencyGBuffer, Reflections.
        // TranslucentFrontLayer). GRAPH BUILD, before the node is added: the frame's front layer draw list - every
        // visible object whose material's template is `BlendMode Translucent`, through its template's
        // `Static.FrontLayer` cell (FrontLayerDrawFor), unsorted: the depth test keeps the nearest. False: nothing
        // to draw, and the frame has no front layer.
        [[nodiscard]] bool BuildFrontLayerDraws();
        // SETUP of "Translucency: Front Layer G-buffer": the list's binding blocks (the cells sample no scene input).
        void DeclareFrontLayerDraws( RDG::PassBuilder& pass );
        // EXEC of the node: records the list inside the render pass the graph opened on the front layer's targets.
        [[nodiscard]] Common::BoolResultStr RenderFrontLayerManual( const RDG::PassContext& context ) const;
        // Deferred path: draws the generic (custom-shader) meshes FORWARD over the deferred
        // lighting composite in a LOAD render pass — they have no G-buffer variant, so without
        // this they simply vanish in Deferred. Forward path draws them inside MeshGeometryPass.
        // @p view (SceneViewInputsOf, declared by the node) is bound for every draw whose shader samples it.
        // SETUP builds the frame's generic draw list and declares its binding blocks (@p view bound for every
        // draw whose shader has slots for it); the exec records that list.
        void DeclareGenericDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr RenderGenericManual( const RDG::PassContext& context ) const;
        // THE SURFACE CACHE'S CARD CAPTURE (GI-SURFACE-CACHE-2/3/5, UE LumenSceneCardCapture). Gather: this
        // frame's static, instanced (ISM / foliage), generic (another template's slot material, a Shader Override)
        // and skinned (bounds cards) mesh instances that carry cards, keyed stably (entity + part; mesh
        // + material + instance index), each naming its row of this renderer's capture sources, with its surface's
        // identity (a new material recaptures) and whether its G-buffer cell can draw yet (a card waits for its
        // own material rather than being captured in the default surface). Declare: the view-pass G-buffer cell
        // of the surface's OWN material (MaterialService::GetViewVariant - its textures, parameters and program,
        // as the scene's G-buffer draws it) draws each capture's mesh through its card camera into its rectangle
        // of the capture atlas (viewport + scissor per draw). One view copy per capture of a material in the
        // frame: the camera block is the material's, so one card camera needs one material.
        // @p out: the frame's card groups (SurfaceCache::SceneGroup), their instances held by this renderer until
        // the next gather; a group is gathered again only when an input of it moved (THE CARD GATHER below).
        void GatherSurfaceCacheInstances( std::vector<SurfaceCache::SceneGroup>& out );
        // The (path x GBuffer) pass state a card of that path is captured with; null when the renderer has none.
        [[nodiscard]] GraphicsPipeline*     CardCapturePassState( MeshVertexPath path ) const;
        // @p view: the frame's scene/view inputs - the G-buffer cell samples the rain occlusion map its wetness
        // reads (SurfaceWeather.glslh), bound as the scene's G-buffer pass binds it, so a card is wet where the
        // surface it stands for is.
        void                                DeclareCardCaptureDraws( RDG::PassBuilder&                             pass,
                                                                     const std::vector<SurfaceCache::CardCapture>& captures,
                                                                     const SceneViewInputs&                        view );
        [[nodiscard]] Common::BoolResultStr RenderCardCaptureManual( const RDG::PassContext& context ) const;

        const std::shared_ptr<Framebuffer>& GetSilhouetteMaskFramebuffer() const
        {
            return m_SilhouetteMaskFramebuffer;
        }

#if DESERT_DEV_INSTRUMENTS
        // Overdraw debug view: re-rasterize every opaque mesh with additive blend (no depth) into a float
        // accumulation buffer, then heat-map the per-pixel overdraw count over the finished scene colour.
        // Path-independent (re-draws geometry; ignores the G-buffer), so it works in Forward and Deferred.
        // node "Debug: Overdraw" (m_OverdrawFB): its setup builds the draw list (culled like the camera pass it
        // reports on) and declares its block; the exec records it.
        void                                DeclareOverdrawDraws( RDG::PassBuilder& pass );
        [[nodiscard]] Common::BoolResultStr RenderOverdrawAccumManual( const RDG::PassContext& context ) const;
        // node "Debug: Overdraw Resolve" (the scene target): its setup declares the resolve's block, which samples
        // @p overdraw - the accumulation the node "Debug: Overdraw" wrote - as OverdrawTexture; the exec draws through
        // it.
        void DeclareOverdrawResolve( RDG::PassBuilder& pass, RDG::TextureRef overdraw );
        [[nodiscard]] Common::BoolResultStr RecordOverdrawResolve( const RDG::PassContext& context ) const;

        const std::shared_ptr<Framebuffer>& GetOverdrawFramebuffer() const
        {
            return m_OverdrawFB;
        }
        // The overdraw view's pipelines and accumulation target, made on the first frame the Overdraw debug mode
        // is chosen (SceneRenderer::AddFrameOverdraw) and kept after. False (said once) when its shaders are
        // missing: the mode then draws the scene without the heat map.
        [[nodiscard]] bool EnsureOverdrawPass();
#endif // DESERT_DEV_INSTRUMENTS

        // True if any queued mesh is flagged for the selection outline this frame. The Jump Flood pass
        // uses this to skip its (log2(width)) full-screen ping-pong passes when nothing is selected.
        bool HasOutline() const
        {
            for ( const auto& d : m_StaticQueue )
                if ( d.Outlined )
                    return true;
            for ( const auto& d : m_GenericQueue )
                if ( d.Outlined )
                    return true;
            for ( const auto& d : m_SkinnedQueue )
                if ( d.Outlined )
                    return true;
            return false;
        }

        void SubmitMesh( const MeshRenderData& data );
        // THE RETAINED STATIC SCENE's sections this view admitted (SceneRenderer::SubmitStaticScene), queued as
        // the RETAINED PREFIX of m_StaticQueue (CPU-SCENE-DELTA 2; UE: FScene's primitives and their cached mesh
        // draw commands live across frames, rebuilt on change). An admitted list of the last one's @p revision
        // (SceneRenderer: a new one per change of the list, process-unique, 0 = none) over the same chunks keeps
        // every entry as it is (no copy, no refcount, nothing rebuilt by SyncGpuScene); the
        // same list over a snapshot that replaced some chunks rewrites the entries of those chunks only; any
        // other list refills the prefix. Per-frame static submissions (SubmitMesh) stay a suffix behind it.
        void SubmitStaticScene( const Render::StaticSceneSnapshot&            scene,
                                std::span<const Render::StaticSceneAdmission> admitted, uint64_t revision );
        void SubmitGenericMesh( const GenericMeshRenderData& data );
        // By move like SubmitGpuInstanced (CPU-TAILS F10): the row's two material instances and its snapshot
        // are not copied a second time into the queue.
        void SubmitInstancedMesh( InstancedMeshRenderData&& data );
        // RENDER-CPU-COST-2: the row is handed over by move (the queue owns it from here), so its buffer handles
        // are not copied; one entry point, so the rule of what the queue accepts is written once.
        void SubmitGpuInstanced( GpuInstancedRenderData&& data );
        // A producer that submits its GPU-instanced casters after UpdateCascades (the particles' Mesh rows, from
        // the render graph) says so before it: the cascade plan then keeps the movable layer for them.
        void ExpectGpuInstancedCasters( const bool expected ) { m_GpuInstancedCastersExpected = expected; }
        bool m_GpuInstancedCastersExpected = false;

        /// ISM instances the last geometry pass drew, after the frustum and the cull distance: what a
        /// foliage type's CullDistance is measured by (FO-5). The shadow cascades are not counted.
        [[nodiscard]] uint32_t GetIsmInstancesDrawn() const
        {
            return m_IsmInstancesDrawn;
        }
        /// Draws (cells or generic materials) the last frame gave to the default surface, or left out, because
        /// their own pipeline was still compiling: the picture is not the scene yet. UE's automated screenshot
        /// waits for FinishAllCompilation; the control gate's PendingWork::MaterialPipelines waits on this.
        /// A bin whose `.Binned` PSO is still compiling counts too (once per PSO): its draws ride the CPU path
        /// meanwhile, so the frame is not the one the GPU-driven passes will settle on.
        [[nodiscard]] uint32_t GetStandInDraws() const
        {
            return m_StandInDraws + static_cast<uint32_t>( m_BinnedAwaiting.size() );
        }
        /// Triangles the last frame's VIEW pass sent to the GPU (forward geometry, or the deferred G-buffer +
        /// forward generic), at the LOD each draw was built with: UE's stat "Triangles drawn" for the base pass.
        /// Shadow and debug passes are not counted.
        [[nodiscard]] uint64_t GetViewTrianglesDrawn() const;
        /// The last frame's forward depth prepass and whether its base pass wrote depth (ForwardPrepassStats).
        [[nodiscard]] const ForwardPrepassStats& GetForwardPrepassStats() const
        {
            return m_ForwardPrepassStats;
        }
        /// Whether this renderer can draw the view's meshes into the G-buffer (the (Static x GBuffer) pipeline
        /// was built): SceneRenderer runs the deferred path only when it can, so the meshes are never drawn
        /// forward on a deferred frame (where no forward depth prepass runs).
        [[nodiscard]] bool DrawsGBuffer() const
        {
            return m_StaticGBufferPipeline != nullptr;
        }
        void ClearQueues();

        // THE BOUNDARY IS DRAWN INSIDE THIS HEADER, not at the call sites, and Common/Core/Profiler.hpp
        // draws its own the same way for the same reason: the API stays one shape in both
        // configurations, so every caller — including the next one somebody writes — compiles unchanged
        // and is cut automatically. Desert/Tests/Runtime/ShippingBoundary calls this form
        // `Gating::InItsOwnHeader` and calls it the stronger of the two. The alternative, an `#if`
        // around each call in SceneRenderer and around each branch in the draw loop, is four more
        // places to forget.

        // Debug wireframe toggle (DebugViewState::WireframeMode) — selects the line-polygon pipeline.
        // In a player's build the argument is accepted and dropped: there is no wireframe pipeline to
        // select, because nothing in a player can set the flag that would select it.
        void SetWireframe( [[maybe_unused]] bool enabled )
        {
#if DESERT_DEV_INSTRUMENTS
            m_Wireframe = enabled;
#endif
        }

        [[nodiscard]] bool WireframeView() const
        {
#if DESERT_DEV_INSTRUMENTS
            return m_Wireframe;
#else
            return false;
#endif
        }

        // A TWO-SIDED MATERIAL'S PIPELINE (UE: TwoSided is a PSO permutation, not a shader input): the pass's
        // pipeline with CullMode None, from the shared cache, remembered per source pipeline. A one-sided draw, or
        // a pipeline that already culls nothing, answers @p pipeline itself. Null = the permutation was refused
        // (logged once per pipeline); the caller does not draw that object culled instead.
        [[nodiscard]] GraphicsPipeline* CullPermutation( GraphicsPipeline* pipeline, bool twoSided );
        std::unordered_map<const GraphicsPipeline*, std::shared_ptr<GraphicsPipeline>> m_TwoSidedPipelines;

        // THE PIPELINE OF ONE DRAW: @p passState's fixed state (layout, target, depth, polygon mode, load) with
        // the program of @p cell — the material whose descriptor sets the draw binds, i.e. its template's
        // (path x pass) cell. Built on first use from the shared pipeline cache and kept per
        // MeshCellPipelineKey; the default template's cell is served the pass's own pipeline back by the cache's
        // dedupe, with no case for it here. Null (logged once per key) when the cell's shader is not registered
        // or the pipeline is refused — the caller skips that draw rather than binding another template's program.
        [[nodiscard]] GraphicsPipeline* CellPipeline( GraphicsPipeline*         passState,
                                                      const DataDrivenMaterial& cell );
        [[nodiscard]] GraphicsPipeline* CellPipeline( GraphicsPipeline* passState, const std::string& cellShader );
        // @p permutation: the cell's own unregistered program (its variant hash keys the pipeline); null = the
        // program registered as @p cellShader.
        [[nodiscard]] GraphicsPipeline* CellPipeline( GraphicsPipeline* passState, const std::string& cellShader,
                                                      const std::shared_ptr<Shader>& permutation );
        std::unordered_map<MeshCellPipelineKey, std::shared_ptr<GraphicsPipeline>, MeshCellPipelineKeyHash>
             m_CellPipelines; // null = refused

        // THE WIREFRAME TWIN OF A FORWARD PASS PIPELINE (UE draws every mesh kind in its wireframe view through
        // the same pass it is drawn in otherwise): @p fallback's line-polygon twin while the view is on, else
        // @p fallback. Static, Instanced and Skinned each have one, so an ISM or a character is drawn in wireframe
        // by the same recording as in Lit — only the pass state differs. Each twin writes its own depth: the
        // Wireframe view's forward meshes are not prepassed. A pipeline with no twin answers itself.
        [[nodiscard]] GraphicsPipeline* WireframePipelineOr( GraphicsPipeline* fallback ) const
        {
#if DESERT_DEV_INSTRUMENTS
            if ( !m_Wireframe || fallback == nullptr )
                return fallback;
            if ( fallback == m_StaticPipeline.get() && m_StaticWireframePipeline )
                return m_StaticWireframePipeline.get();
            if ( fallback == m_StaticInstancedPipeline.get() && m_StaticInstancedWireframePipeline )
                return m_StaticInstancedWireframePipeline.get();
            if ( fallback == m_SkinnedPipeline.get() && m_SkinnedWireframePipeline )
                return m_SkinnedWireframePipeline.get();
            return fallback;
#else
            return fallback;
#endif
        }

        // Distance-based mesh LOD (auto). LOD0 is byte-identical to the base geometry, so this only
        // affects meshes far from the camera. Toggle from the editor's Graphics menu.
        void SetLODEnabled( bool enabled )
        {
            m_LODEnabled = enabled;
        }
        bool IsLODEnabled() const
        {
            return m_LODEnabled;
        }

        // Which LOD level to draw for a mesh: a forced level (>= 0) wins; otherwise auto by screen
        // coverage (world bounding radius / camera distance), so big objects keep detail farther than
        // small ones. Returns 0 when LOD is off or there's no camera. Used by every mesh draw path.
        uint32_t ComputeLOD( const glm::mat4& transform, const class Desert::Mesh* mesh, int forcedLOD,
                             int lodBias = 0 ) const;

        // Cascaded shadow maps (R32F light-space depth, one framebuffer per cascade). Recompute the
        // per-cascade light matrices once per frame BEFORE the render graph records (intra-phase order is
        // nondeterministic). Called from SceneRenderer::OnUpdate, after GatherShadowCasters.
        void UpdateCascades();
        // THE CACHE DECISION OF EACH CASCADE FOR THIS FRAME, from this frame's data alone (UE: the cached shadow
        // map's invalidation is decided from the frame's primitives, never last frame's): after UpdateCascades
        // fitted the matrices AND after every caster resolved what it draws into the cascades this frame - the
        // meshes' gather, the GPU-instanced sections (GrassRenderer::AddFrame) and the non-mesh casters' draws
        // (TerrainRenderer::PrepareLocalShadowViews), whose keys (IShadowCaster::CascadeShadowSignature) fold
        // into the static layer's. Sets, per cascade: whether its static layer is redrawn, whether it holds a
        // movable caster, whether its map is rebuilt, and which non-mesh casters each layer draws. Before it,
        // UpdateCascades leaves every cascade with no work; nothing reads the plan in between.
        void PlanCascadeCache();
        // THIS FRAME'S SHADOW CASTERS, gathered ONCE from the mesh queues for every shadow view (UE
        // GatherShadowPrimitives: one walk of the scene's primitives for every whole-scene shadow, not one per
        // view): each caster's world box - a skinned mesh's in this frame's pose (SkinnedPoseBounds) - and, on the
        // first view that draws it, its LOD level asked of the main camera and an ISM field's kept instances
        // (IsmInstanceReach). The sun cascades and their cache keys, the rain occlusion view, the local lights'
        // views and keys and the VSM levels all cull by these boxes; none walks a queue for bounds or LOD itself.
        // Called from SceneRenderer::OnUpdate before UpdateCascades, and again at the end of ApplyDisplacement,
        // whose twins replace queued entries.
        void GatherShadowCasters();

        // MORPH TARGETS BEFORE SKINNING (UE FSkeletalMeshObjectGPUSkin::Update -> GPUMorphUpdateCS). Runs the
        // morph graph of this frame: one compute node per skinned component whose MorphComponent revision moved
        // since its morphed vertices were last blended (or that has none yet), base + Σ weight·delta into the
        // component's own vertex buffer, handed to vertex input at the graph's end. Every pass then skins the
        // morphed twin SubmitMesh put on the skinned queue. No changed revision = no graph, no dispatch. Called
        // from SceneRenderer::OnUpdate after the frame's draws were submitted and before the view graph records.
        void UpdateMorphTargets();

        // A cascade's STATIC-caster cache (for the editor's CSM debug viewer): the cascade map without the
        // movable casters, which the array layer adds over it. Null if out of range / not yet created.
        std::shared_ptr<Image2D> GetCascadeCacheImage( uint32_t cascade ) const
        {
            if ( cascade >= m_Shadow.CascadeCount || !m_CascadeStaticFB[cascade] )
                return nullptr;
            return m_CascadeStaticFB[cascade]->GetDepthAttachmentImage();
        }

        // HOW MANY CASCADES THIS RENDERER ACTUALLY HAS — not the ceiling. It was `static constexpr`, and
        // that is precisely what made a per-renderer budget impossible: every consumer, including the
        // deferred composite's own shadow gather, asked the CLASS instead of the object and would have
        // read four for a renderer holding one.
        [[nodiscard]] uint32_t GetCascadeCount() const
        {
            return m_Shadow.CascadeCount;
        }

        // EVERY SHADOWED DIRECTIONAL LIGHT'S CASCADES AS THIS FRAME PUBLISHES THEM, the one value every
        // receiver packs (SceneFrameBinding::DirectionalShadows, the deferred composite, the surface cache).
        // Count is how many carry a matrix from THIS frame's fit — a different question from GetCascadeCount
        // ("what does this renderer own", the budget): they diverge exactly when UpdateCascades does not run to
        // completion — no main camera, no such light, a degenerate fit — which is when publishing the budget
        // hands the shader matrices nothing ever wrote. CastShadows is the light's own (a light whose shadows
        // are off packs 0 and is lit, never sampled); FirstLayer its cascade 0's layer of the shared array.
        [[nodiscard]] SceneResources::DirectionalShadowViews DirectionalShadows() const;

        // HOW FAR ALONG EACH SHADOWED DIRECTIONAL LIGHT A CASTER CAN STAND FROM A RECEIVER IT DARKENS, this frame
        // (GRASS-VIS-6; after UpdateCascades): per slot the direction the light travels and the longest light-axis
        // length of its fitted cascades' boxes (CascadeFit::DepthRange - a caster farther up the light than that
        // from a receiver is outside every box that receiver is sampled from). Reach 0: the slot casts nothing this
        // frame. What a generated caster list (GrassRenderer) sweeps the camera's volume by, so an instance outside
        // the frame whose shadow falls into it is kept (UE: a whole-scene shadow's casters are culled by the
        // cascade's volume, not the camera's frustum).
        struct DirectionalCasterSweep
        {
            glm::vec3 Travel{ 0.0f };
            float     Reach = 0.0f; // cm
        };
        [[nodiscard]] std::array<DirectionalCasterSweep, SceneResources::kMaxShadowedDirectionalLights>
        DirectionalCasterSweeps() const;

        // THE CASCADE VIEWS THIS FRAME DRAWS AND THE BOX EACH CULLS ITS CASTERS BY (GPU-COST-21; after
        // UpdateCascades): bit v of Live for every cascade view v DirectionalShadows counts (a held slot whose
        // light casts, a cascade below its fitted count), ViewProj[v] its caster matrix (m_CascadeCasterVP - the
        // one the casters draw with and BuildShadowCascadeDraws culls the static casters by: the cascade's box
        // about the camera, reaching towards the light). What a GPU producer builds each cascade's own caster list
        // by (GpuInstancedRenderData::CascadeMask; UE: every shadow view culls its instances by its own volume).
        struct CascadeCasterBoxes
        {
            uint32_t                                                Live = 0;
            std::array<glm::mat4, SceneResources::kMaxCascadeViews> ViewProj{};
        };
        [[nodiscard]] CascadeCasterBoxes CascadeCasterBoxSet() const;

        // The budget this renderer was created with. Fixed after Initialize — the framebuffers are
        // allocated from it once.
        [[nodiscard]] const ShadowQuality& GetShadowQuality() const
        {
            return m_Shadow;
        }
        // RE-ALLOCATE THE CASCADE MAPS TO A NEW BUDGET — the Shadows quality level changed (SceneRenderer, on a
        // QualityState generation change, never per frame, at the head of its frame). Waits for the device to go
        // idle, releases the cascade framebuffers and maps, the local views' rows and the attachment lease, then
        // runs SetupShadowPass again on the new budget. The caster materials and pipelines are KEPT (they do not
        // depend on the map size). False when the new shadow pass could not be set up (logged).
        [[nodiscard]] bool RebudgetShadows( const ShadowQuality& budget );

        void SetShadows( const ViewShadowSettings& shadows, int debugMode )
        {
            m_ShadowsEnabled  = shadows.Enabled;
            m_ShadowBias      = shadows.Bias;
            m_ShadowDebugMode = debugMode;
            m_SplitLambda     = shadows.CascadeSplitLambda;
            m_SunSourceAngle  = shadows.SourceAngleDegrees;
        }
        bool AreShadowsEnabled() const
        {
            return m_ShadowsEnabled;
        }
        // The target a non-mesh caster builds its pipeline against: the depth format of every cascade layer
        // (the static caches are framebuffers of it; the array layers are drawn through the graph).
        [[nodiscard]] const std::shared_ptr<Framebuffer>& GetCascadeFramebuffer() const
        {
            return m_CascadeStaticFB[0];
        }

        // A renderer that casts into the cascades from inside their passes (IShadowCaster says why). Held
        // weakly: the caster is another render system of the same SceneRenderer and may be torn down first.
        void AddShadowCaster( std::weak_ptr<IShadowCaster> caster )
        {
            m_ShadowCasters.push_back( std::move( caster ) );
        }

        // Debug visualizations. TWO KINDS, and the boundary runs between them: `showNormals` and
        // `lightingDebug` are BRANCHES IN THE Lit SHADER, so they travel with the program and cost no
        // pipeline; the AABB wireframes are drawn by a pipeline of their own, so a player's build has
        // neither the pipeline nor the fields, and the three arguments are accepted and dropped.
        void SetDebugView( bool showNormals, [[maybe_unused]] bool showBoundingBoxes,
                           [[maybe_unused]] const glm::vec3& bbColor, [[maybe_unused]] float bbLineWidth,
                           bool lightingDebug = false )
        {
            m_ShowNormals   = showNormals;
            m_LightingDebug = lightingDebug;
#if DESERT_DEV_INSTRUMENTS
            m_ShowBoundingBoxes    = showBoundingBoxes;
            m_BoundingBoxColor     = bbColor;
            m_BoundingBoxLineWidth = bbLineWidth;
#endif
        }
        // The surface-cache card placement overlay (UE r.Lumen.Visualize.CardPlacement): every drawn static mesh's
        // cards (Assets::MeshCardsBuildData) as wireframe boxes, coloured by the direction each faces. Drawn by
        // the same debug line pipeline as the AABBs; accepted and dropped in a player's build.
        void SetCardPlacementView( [[maybe_unused]] bool showCards )
        {
#if DESERT_DEV_INSTRUMENTS
            m_ShowCardPlacement = showCards;
#endif
        }

    private:
        bool SetupGeometryPass();
        bool SetupGBufferPass(); // deferred: static-mesh G-buffer write pipeline
        // The forward path's depth prepass cells (DeclareDepthPrepassDraws): (Static / Instanced / Skinned x Depth)
        // of the default surface on SceneDepthLayout, Closer, depth written. The static one is REQUIRED (the forward
        // base pass writes no depth); a refused instanced or skinned one costs those meshes in the forward path,
        // said here.
        bool SetupDepthPrepass();
        // The translucency pass's draw state for ONE translucent cell shader (a translucent template's
        // Static.Forward cell), built on first use and kept: UE draws each translucent material with its own
        // shader/PSO, so every translucent template gets its own pipeline here. Null (logged once per shader) when
        // it cannot.
        struct TranslucentDraw;
        TranslucentDraw* TranslucentDrawFor( const std::string& cellShader );
        // The front layer's draw state for ONE translucent template's `Static.FrontLayer` cell @p cellShader: its own
        // opaque, depth-writing pipeline on FrontLayerTargetLayout and a dedicated material (its rows written once a
        // frame by BuildFrontLayerDraws). Null (logged once per shader) when it cannot be built.
        TranslucentDraw* FrontLayerDrawFor( const std::string& cellShader );
        bool SetupSkinnedGeometryPass();
        bool SetupSilhouettePass();
        // Holds @p budget as m_Shadow, clamped to what the arrays can carry. Initialize and RebudgetShadows.
        void TakeShadowBudget( const ShadowQuality& budget );
        bool SetupShadowPass();
        // One directional-shadow slot's static caches and caster materials, at the budget's cascade count
        // (SetupShadowPass for every slot held, EnsureCascadeSlots for one first needed).
        void AllocateCascadeSlot( uint32_t slot );
        // The instanced and skinned caster materials of every held slot's cascades, for each optional pipeline built.
        void EnsureCascadeCasterMaterials();
        // HOLDS @p slots DIRECTIONAL-SHADOW SLOTS (the sun's is held from SetupShadowPass; another is taken the
        // first frame its light casts, and kept: UE allocates a light's whole-scene shadow when the light needs
        // it). A new slot drains the device, allocates its caches and materials and drops the cascade array, so
        // the next AcquireCascadeMaps makes it again with every slot's layers. False under a zero budget.
        bool EnsureCascadeSlots( uint32_t slots );
        // The array layer of cascade view @p view: slot * the budget's cascade count + cascade.
        [[nodiscard]] uint32_t CascadeLayer( uint32_t view ) const
        {
            return ( view / kMaxCascades ) * m_Shadow.CascadeCount + view % kMaxCascades;
        }
        // The one place the shadow budget is said out loud. Called from both arms of SetupShadowPass —
        // the allocating one and the zero-budget one — because a renderer that spends nothing on the sun
        // is exactly as worth reading in a log as one that spends 320 MiB, and a line printed on only one
        // path is a line whose absence means two different things.
        void LogShadowBudget( double allocMs ) const;

        // The draws of one mesh node through its pass parameters (@p pass); the first refused draw is the error.
        // Build the draws of one lit mesh node into @p list (the node's setup; MeshDrawList says why). The
        // per-group material state (Materials[] / bones / instance SSBOs, the shared scene state) is written
        // here, at final size, before any draw is recorded; a refused build is the list's error.
        // The call's mode (DrawBuildMode) and its own reused vectors (DrawBuildScratch) are parameters, not
        // renderer state: the G-buffer's two early-Z halves record on two setup tasks at once
        // (DeclareGBufferDraws), after a serial Resolve call per half filled every renderer cache they read and
        // made every shared write (DrawBuildPhase). There is no lock: a Record call only reads.
        // Returns the ISM instances drawn (GetIsmInstancesDrawn; a Depth half and a Record call count none).
        struct DrawBuildMode;
        struct DrawBuildScratch;
        [[nodiscard]] uint32_t BuildStaticDraws( MeshRendererDetail::MeshDrawList& list, const DrawBuildMode& mode,
                                                 DrawBuildScratch& scratch );
        void BuildSkinnedDraws( MeshRendererDetail::MeshDrawList& list, const DrawBuildMode& mode,
                                DrawBuildScratch& scratch );
        // The Rest half's GPU-generated sections (the landscape grass, BuildGpuInstancedDraws), which a None-half
        // BuildStaticDraws builds as its tail: DeclareGBufferDraws builds them serially after the halves joined,
        // appended where the Rest half's tail was. BuildGpuInstancedDraws stays off the tasks (not audited).
        void BuildRestGpuInstancedDraws( MeshRendererDetail::MeshDrawList& list );
        // After a Record call joined: the instance transforms it packed, uploaded into each lent recorder's
        // InstanceTransforms block (serial; the recorders' other writes were the Resolve call's).
        void FlushStaticUploads( DrawBuildScratch& scratch );
        // A renderer cache missed @p what inside a setup task (Common::TaskGroup::InTask): the serial Resolve
        // call did not resolve it, which is a bug. Says so with a "[RenderTask]" line, asserts, and returns true;
        // the caller then draws nothing for it. False off a task (the caller fills the cache).
        [[nodiscard]] bool MissInSetupTask( std::string_view what ) const;
        // The static and instanced state of one geometry pass (Forward, GBuffer, Depth): the per-object pass state,
        // the stand-in that records a waiting material's objects and its pipeline, the instanced pass state and
        // the renderer's instanced spare. Forward under the Wireframe view: the wireframe twins; under VirtualMaps:
        // the spare's and the stand-in's DESERT_LOCAL_SHADOW_VSM twins.
        struct StandInCell;
        struct StaticPassCells
        {
            MeshPass                Pass             = MeshPass::Forward;
            GraphicsPipeline*       StaticState      = nullptr;
            StandInCell*            StandInDraw      = nullptr;
            const GraphicsPipeline* StandInPipeline  = nullptr;
            GraphicsPipeline*       InstancedState   = nullptr;
            DataDrivenMaterial*     InstancedSpare   = nullptr;
            MaterialInstance*       InstancedInstance = nullptr;
            const GraphicsPipeline* InstancedStandIn = nullptr;
            bool                    InstancingOn     = false;
            // The G-buffer after the deferred early-Z pass (@p depthEqual of StaticCellsOf): EQUAL, no depth writes,
            // the cells in their DESERT_SURFACE_DEPTH_EQUAL permutation (DepthEqualCell).
            bool                    DepthEqual       = false;
        };
        [[nodiscard]] StaticPassCells StaticCellsOf( MeshPass pass, bool depthEqual = false );
        // WHICH HALF of the deferred early-Z pairing BuildStaticDraws / BuildSkinnedDraws build (UE: the depth pass
        // and the base pass draw one mesh-draw list). None: no pairing (the forward path, or EarlyZPass Off).
        // Depth: "Deferred: Masked Depth Prepass", the groups EarlyZAdmits; the readiness of both halves folded and
        // recorded in m_PrepassCellDraws. Equal: "Deferred: GBuffer", the groups the prepass drew (recorded Own),
        // through the DepthEqual cells. Rest: "Deferred: GBuffer", every other group, through the plain cells.
        enum class DeferredPrepassHalf : uint8_t
        {
            None,
            Depth,
            Equal,
            Rest,
        };
        // ONE CALL OF BuildStaticDraws / BuildSkinnedDraws (UE: the pass-setup context
        // FParallelMeshDrawCommandPass hands each pass by value): the pass it builds for - Forward
        // (MeshGeometryPass), GBuffer ("Deferred: GBuffer") or Depth ("Forward: Depth Prepass", "Deferred: Masked
        // Depth Prepass") - and the early-Z half.
        // WHICH PART OF ONE CALL runs (RENDER-PARALLEL-SETUP; UE FParallelMeshDrawCommandPass: what a task needs
        // is resolved before the tasks, which only read). Full: all of it (every serial pass). Resolve: the
        // G-buffer half's serial pre-pass - the walk, the half's split, every cache miss (cell pipelines,
        // two-sided twins, stand-ins, material service siblings, the tracker, the instanced variant instances,
        // the material rows), the ISM culls and their copies, and every shared-material write (Lend, ApplyTo,
        // SetPushMatrix) - into the call's DrawBuildScratch. It prepares what the Record call draws and no more:
        // the group plan it makes once (planGroup, on setup tasks: buckets, batches, singles and their levels)
        // asks a group's singles material only of a group whose plan keeps a single draw, and a set is prepared
        // only when a batch or an ISM draws into it. Record: the half's setup task - packs and records that plan
        // into its own list and scratch and decides nothing; FlushStaticUploads after the join. A miss in a
        // Record call is a bug (MissInSetupTask).
        enum class DrawBuildPhase : uint8_t
        {
            Full,
            Resolve,
            Record,
        };
        struct DrawBuildMode
        {
            MeshPass            Pass  = MeshPass::Forward;
            DeferredPrepassHalf Half  = DeferredPrepassHalf::None;
            DrawBuildPhase      Phase = DrawBuildPhase::Full;
            // Resolve: the other half's scratch whose walk this half takes (one walk of the queue for both).
            const DrawBuildScratch* Walk = nullptr;
        };
        // Whether @p half takes the group of @p source (its surface) keyed @p key.
        struct PrepassCellKey;
        [[nodiscard]] bool DeferredHalfTakes( DeferredPrepassHalf half, const DataDrivenMaterial* source,
                                              const PrepassCellKey& key ) const;
        // Whether this frame's EarlyZPass puts @p surface in the deferred prepass: Masked = IsMaskedSurface, All =
        // every non-translucent surface, Off = none.
        [[nodiscard]] bool EarlyZAdmits( const DataDrivenMaterial* surface ) const;
        // THE one rule EarlyZAdmits applies, by the cell's blend and name (a `.Binned` one is judged as its base
        // cell): what a GPU-driven G-buffer group, which knows its cell shader and not its materials, is asked by.
        [[nodiscard]] bool EarlyZAdmitsCell( Core::Formats::SurfaceBlendMode blend, std::string_view cellShader ) const;
        // THE GPU-DRIVEN BINS' EARLY-Z TWINS (UE r.EarlyZPass over the GPU Scene's cached mesh draw commands: ONE
        // cull, the same indirect records drawn by the depth pass and then by the base pass). Of a G-buffer group
        // pipeline (its `<Template>/<Path>.GBuffer.Binned` cell on the G-buffer's state): Depth = the template's
        // (path x Depth) `.Binned` cell on the prepass's depth state, Equal = the group's own cell in its
        // SurfaceDepthEqualVariant (no clip) on the G-buffer's EQUAL / no-write state, both with the group's cull
        // mode. Both or neither: none while either compiles (the group draws as before this frame, its clip and
        // depth write in the G-buffer, and is asked again next frame), for a cell EarlyZAdmitsCell refuses, and
        // for a cell without the twins (said once). The group's clusters (IndirectGroup::VirtualGeometryPipeline,
        // its `<Path>.GBuffer.Binned.VirtualGeometry` cell) are a second pair by the same rule: VgDepth = the
        // template's `<Path>.Depth.Binned.VirtualGeometry` cell on the depth state, VgEqual = the cluster cell's
        // SurfaceDepthEqualVariant on the EQUAL state, both with the cluster pipeline's cull mode; each pair is
        // both or neither on its own (a group may pair its bins and not yet its clusters).
        struct EarlyZBinTwins
        {
            const GraphicsPipeline* Depth   = nullptr;
            const GraphicsPipeline* Equal   = nullptr;
            const GraphicsPipeline* VgDepth = nullptr;
            const GraphicsPipeline* VgEqual = nullptr;
        };
        struct EarlyZTwinPairSlot;
        struct EarlyZBinTwinSlot;
        [[nodiscard]] EarlyZBinTwins EarlyZBinTwinsOf( const GpuDriven::IndirectGroup& group );
        // One pair of EarlyZBinTwinsOf: @p depthCell on @p depthState, @p equalCell's SurfaceDepthEqualVariant on
        // @p equalState, both with @p cull. {null, null} while either compiles (asked again next frame) and when
        // refused (said once under @p equalCell; @p slot is then Refused and not asked again).
        [[nodiscard]] std::pair<const GraphicsPipeline*, const GraphicsPipeline*>
        EarlyZTwinPairOf( EarlyZTwinPairSlot& slot, const std::string& equalCell, const std::string& depthCell,
                          const GraphicsPipeline* depthState, const GraphicsPipeline* equalState, CullMode cull );
        // Ready @p shader on @p passState's spec with @p cull, or null (compiling: silently, asked again; refused:
        // said once under @p label).
        [[nodiscard]] std::shared_ptr<GraphicsPipeline> EarlyZTwinPipeline( const std::shared_ptr<Shader>& shader,
                                                                            const GraphicsPipeline&        passState,
                                                                            CullMode                       cull,
                                                                            const std::string&             label );
        // This frame's two issues of the G-buffer view's phase-1 bins (the view's one cull): the depth twins of
        // the paired groups into m_MaskedDepthDraws, and the G-buffer's issue - the paired groups through their
        // equal twins, every other group through its own pipeline - kept for DeclareGBufferDraws
        // (m_EarlyZGBufferIssue, stamped m_EarlyZBinFrame). Phase 2 is paired by the same split
        // (DeclareEarlyZPostOcclusionDraws).
        void DeclareEarlyZBins();
        // THE ONE EARLY-Z SPLIT of a G-buffer issue (either occlusion phase; UE two-pass occlusion: each phase's
        // depth pass, then its base pass EQUAL). @p depth = @p source's records and clusters of the groups whose
        // twins are Ready, through their depth twins and carriers (every other group null: not drawn by it);
        // @p gbuffer = @p source with those groups through their equal twins and the others through their own
        // pipelines. False: nothing paired (@p depth and @p gbuffer are then not to be used). @p kept, when given,
        // collects the twin slots the issue's groups use (the per-frame prune of m_EarlyZBinTwins, phase 1).
        [[nodiscard]] bool EarlyZSplit( const MeshRendererDetail::BinnedIssue&                          source,
                                        const SceneFrameBinding&                                        frameState,
                                        MeshRendererDetail::BinnedIssue&                                depth,
                                        MeshRendererDetail::BinnedIssue&                                gbuffer,
                                        std::unordered_map<const GraphicsPipeline*, EarlyZBinTwinSlot>* kept );
        // The G-buffer cell of @p built's @p path drawn EQUAL after the prepass: its SurfaceDepthEqualVariant. A
        // masked template's drops its clip; an opaque one's is the same program, but a sibling of its own, so the
        // Equal half never shares a recorder (its Materials / InstanceTransforms) with the Rest half's plain cell.
        [[nodiscard]] DataDrivenMaterial* DepthEqualCell( const DataDrivenMaterial* built, MeshVertexPath path ) const;
        // Which part of the generic queue a list takes, and through which program of each draw's template.
        enum class GenericSlice : uint8_t
        {
            // The forward render path: every draw, its own program, in MeshGeometryPass.
            Forward,
            // Deferred, "Deferred: GBuffer": the Shader Override draws naming a SURFACE template (it has cells)
            // through its (Static x GBuffer) cell — a surface material, shaded by the composite like the lit
            // queue, never forward.
            DeferredSurfaceGBuffer,
            // Deferred, "Deferred: Forward Depth/Normal": the other draws whose program is opaque, through their
            // template's (Static x DepthNormal) program.
            DeferredDepthNormal,
            // Deferred, "Deferred: Generic": the other draws, their own program over the composite; an opaque one
            // at CloserOrEqual against its own prepassed depth.
            DeferredForward,
            // DeferredForward while the sun is ray traced: an opaque program that samples the sun shadow is shaded
            // through its DESERT_SUN_SHADOW_SCREEN_MASK permutation (ForwardShadowMaskVariant), which reads the
            // view's screen mask (SceneViewInputs.SunShadowMask) in place of the cascades; a blended one through
            // itself.
            DeferredForwardRayTracedSun,
            // The forward path, "Forward: Depth Prepass": the opaque draws through their template's (Static x
            // DepthNormal) program into the scene depth alone (its normal output has no attachment), admitted when
            // that pipeline AND the Forward slice's are Ready (OpaqueGenericAdmitted), as DeferredDepthNormal.
            ForwardDepth,
        };
        // per-object data-driven materials (v3 slots + overrides)
        void BuildGenericDraws( MeshRendererDetail::MeshDrawList& list, GenericSlice slice );
        // Which casters a shadow view draws. A local view draws all of them; a cascade draws its STATIC ones into
        // its cache and its MOVABLE ones over the copied cache, every frame.
        enum class ShadowCasterLayer : uint8_t
        {
            All,
            Static,  // static, generic and still instanced meshes
            Movable, // skinned meshes and swaying foliage
        };
        // The casters of shadow view @p cascade (a fixed view, or a local view: kLocalShadowViewBase + row) into
        // @p list.
        void BuildShadowCascadeDraws( uint32_t cascade, MeshRendererDetail::MeshDrawList& list,
                                      ShadowCasterLayer layer = ShadowCasterLayer::All );
        // A hash of the static casters cascade @p cascade's caster box reaches (what they draw and where), and
        // whether a movable caster reaches it: the cache key of the cascade's static layer. A still ISM field's
        // part is its snapshot and the instances the gather keeps in the box with their levels
        // (ShadowIsmInstances, this gather's): what the layer draws.
        // @p casterVP: the box the casters are taken from (this frame's caster matrix, or the one a scrolled cache
        // was drawn with). @p withLod: the levels the camera chose are part of the key (a content key leaves them
        // out: a scrolled cache keeps the level it was drawn at, PlanCascadeCache). @p windSplit: a reached caster
        // is split between the layers by the wind ring around the camera, which no content key can hold.
        // THE MOVERS OF A CASCADE (CascadeStaticSignature, SHADOW-CACHE-MOTION): a skinned caster reaching it is
        // KEYED - its entry, meshes, sections and transform in Casters, its bones and morph blend in Pose, its
        // posed box in Bounds - and the movable layer is kept while that key holds; any other mover (wind time,
        // instances the GPU moves, casters gathered for other queues, particles submitted after the plan, a
        // non-mesh caster with no key) is UNKEYED, named, and the movable layer is drawn each frame.
        struct CascadeMovers
        {
            bool                              Keyed   = false;
            const char*                       Unkeyed = nullptr;
            ShadowSignatureHash               Casters;
            ShadowSignatureHash               Pose;
            std::optional<Common::Math::AABB> Bounds;
            [[nodiscard]] bool                Any() const { return Keyed || Unkeyed != nullptr; }
        };
        // @p movers: the movable casters that reach the box (CascadeMovers), added to.
        [[nodiscard]] uint64_t CascadeStaticSignature( uint32_t cascade, const glm::mat4& casterVP, bool withLod,
                                                       CascadeMovers& movers, bool& windSplit );
        // The caster materials of shadow view @p view, made on its first use. False when the caster
        // pipeline does not exist (a renderer with no shadow budget).
        bool EnsureShadowViewMaterials( uint32_t view );
        // A hash of every caster in @p light's reach (what it draws into its views and where) - the meshes and the
        // non-mesh casters (IShadowCaster::LocalShadowSignature) - or nullopt when one of them moves by itself
        // every frame (a skinned mesh, swaying foliage): the cache key of a local view.
        [[nodiscard]] std::optional<uint64_t> LocalCasterSignature( const LocalShadowCasterLight& light );
        // The mesh casters alone in the sphere (@p centre, @p radius cm), or nullopt when one of them moves by
        // itself every frame: the part of a local view's key the meshes give, and the rain occlusion map's key
        // (the terrain under it changes only with the box, which RainOcclusionNeedsRecentre redraws on). Every
        // caster's box is this frame's gather (GatherShadowCasters), an ISM field's the instances it keeps
        // (ShadowIsmInstances, this gather's): what the views draw.
        [[nodiscard]] std::optional<uint64_t> LocalCasterSignature( const glm::vec3& centre, float radius );
        // The cache decision of each light given views by UpdateLocalShadows (m_LocalCachePlans): its key from
        // its casters and its views; the views of a light whose key changed (or that has none) go to
        // m_LocalRedraws.
        void PlanLocalShadowCache();

        // The frame's draw lists, rebuilt by each node's setup and recorded by its exec.
        MeshRendererDetail::MeshDrawList m_ForwardDraws; // MeshGeometryPass: static + skinned + generic
        MeshRendererDetail::MeshDrawList m_GBufferDraws; // "Deferred: GBuffer": static, instanced, skinned
        MeshRendererDetail::MeshDrawList m_MaskedDepthDraws; // "Deferred: Masked Depth Prepass": masked GPU sections
        // A GPU-instanced section the masked depth prepass drew, and how many of its buckets
        // (GpuInstancedRenderData::PrepassBuckets, cut back to every bucket while the section's own G-buffer
        // pipeline is not Ready): the G-buffer draws those EQUAL and the rest through its own cell.
        struct MaskedPrepassedSection
        {
            uint32_t Key     = 0;
            uint32_t Buckets = 0;
        };
        // The GPU-instanced sections (GpuInstancedRenderData::Key) THIS frame's masked depth prepass drew: the
        // G-buffer draws exactly these through m_InstancedGBufferEqualPipeline, every other one as before.
        std::vector<MaskedPrepassedSection> m_MaskedPrepassed;
        // The same for phase 2 (GRASS-VIS-13): the sections "Grass: Depth Prepass Post-Occlusion" drew by their
        // phase-2 records THIS frame, which "Grass: GBuffer Post-Occlusion" draws EQUAL. Cleared by
        // DeclareMaskedDepthDraws (every deferred frame) and by the depth node's setup.
        std::vector<MaskedPrepassedSection> m_MaskedPrepassedPost;
        // THE FRONT-TO-BACK WALK of phase 1's masked depth prepass (BuildGpuInstancedDraws, GPU-MASKED-PREPASS-2):
        // the queue indices of the GPU-instanced sections, nearest box first (squared distance from the camera to
        // the section's WorldBounds; a section without a box first), so a near section's surviving texels are in
        // the depth before a far one's clip runs. Only that serial call writes it; capacity kept across frames.
        struct PrepassSectionOrder
        {
            float    DistanceSq = 0.0f;
            uint32_t Queue      = 0;
        };
        std::vector<PrepassSectionOrder> m_PrepassSectionOrder;
        // GRASS-VIS (SetGrassVisibilityActive): on this frame; the blade sections (GpuInstancedRenderData::Key) whose
        // two visibility pipelines are Ready, chosen by DeclareMaskedDepthDraws and drawn by the two lists below
        // instead of the masked prepass + G-buffer; the resolve's argument table of this frame.
        bool                             m_GrassVisibilityActive = false;
        std::vector<uint32_t>            m_GrassVisibilitySections;
        size_t                           m_GrassVisibilityMeshSections = 0; // of them, the Instanced (mesh) ones
        MeshRendererDetail::MeshDrawList m_GrassVisibilityRasterDraws;  // "Grass: Visibility Raster"
        MeshRendererDetail::MeshDrawList m_GrassVisibilityResolveDraws; // "Grass: Visibility Resolve"
        MeshRendererDetail::MeshDrawList m_GrassMeshVisibilityResolveDraws; // "Grass: Mesh Visibility Resolve"
        // GRASS-VIS phase 2: "Grass: Visibility Raster Post-Occlusion", "Grass: GBuffer Post-Occlusion".
        MeshRendererDetail::MeshDrawList m_GrassVisibilityRasterPostDraws;
        MeshRendererDetail::MeshDrawList m_GrassGBufferPostDraws;
        MeshRendererDetail::MeshDrawList m_GrassDepthPostDraws; // "Grass: Depth Prepass Post-Occlusion"
        RDG::BufferRef                   m_GrassResolveArgs{};
        MeshRendererDetail::MeshDrawList m_GBufferPostDraws; // "Deferred: GBuffer Post-Occlusion": phase-2 bins
        // "Deferred: Depth Prepass Post-Occlusion": phase 2's paired bins and clusters through their depth twins.
        MeshRendererDetail::MeshDrawList m_EarlyZPostDepthDraws;
        MeshRendererDetail::MeshDrawList m_GenericDraws; // "Deferred: Generic"
        MeshRendererDetail::MeshDrawList m_DepthNormalDraws; // "Deferred: Forward Depth/Normal"
        MeshRendererDetail::MeshDrawList m_DepthPrepassDraws; // "Forward: Depth Prepass"
        // "MeshShadowCascade<c>": the cascade's casters, built by its Declare (BuildShadowCascadeDraws).
        MeshRendererDetail::MeshDrawList m_CascadeDraws[kCascadeViews];
        // The cascade's non-mesh casters (m_ShadowCasters) its Declare declared, each with the index of its first
        // binding block (after m_CascadeDraws[c]'s); the exec records exactly these.
        std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>> m_CascadeCasters[kCascadeViews];
        // ...and the ones its static layer's Declare declared (".Static", into m_CascadeStaticFB[c]), the same
        // way.
        std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>> m_CascadeStaticCasters[kCascadeViews];
        // THE PLAN'S SPLIT OF THE NON-MESH CASTERS (PlanCascadeCache), per cascade: a caster that keyed what it
        // draws there (IShadowCaster::CascadeShadowSignature) is in the static layer, one that did not in the
        // movable layer. The Declares draw exactly these lists, so a layer never draws a caster its key missed.
        std::vector<std::weak_ptr<IShadowCaster>> m_CascadeKeyedCasters[kCascadeViews];
        std::vector<std::weak_ptr<IShadowCaster>> m_CascadeUnkeyedCasters[kCascadeViews];
        MeshRendererDetail::MeshDrawList m_GlassDraws;      // "Deferred: Glass" (block 0 = the glass executor's)
        MeshRendererDetail::MeshDrawList m_FrontLayerDraws; // "Translucency: Front Layer G-buffer"
        // The glass / front layer walks' per-frame lists (RenderGlassManual, BuildFrontLayerDraws), kept across
        // frames and cleared per walk (UE FMemStack: per-frame lists never grow the heap in a steady frame).
        // Cells: each lent the scene material table once, in first-seen order (a linear find: a frame has a few
        // cells).
        struct TranslucentObjectDraw
        {
            const StaticMeshRenderData* Object = nullptr;
            TranslucentDraw*            State  = nullptr;
            uint32_t                    Row    = 0;
        };
        struct TranslucentWalkScratch
        {
            std::vector<TranslucentObjectDraw> Draws;
            std::vector<TranslucentDraw*>      Cells;
            std::vector<TranslucentSortItem>   SortKeys;
            std::vector<float>                 SortDistance2;
            std::vector<uint32_t>              DrawOrder;
            void                               Clear() noexcept
            {
                Draws.clear();
                Cells.clear();
                SortKeys.clear();
                SortDistance2.clear();
                DrawOrder.clear();
            }
            void AddCell( TranslucentDraw* cell )
            {
                if ( std::find( Cells.begin(), Cells.end(), cell ) == Cells.end() )
                    Cells.push_back( cell );
            }
        };
        TranslucentWalkScratch           m_ScratchGlassWalk;
        TranslucentWalkScratch           m_ScratchFrontLayerWalk;
        MeshRendererDetail::MeshDrawList m_CardCaptureDraws; // "SurfaceCache: Card Capture"
        // One section of a card instance (UE FLumenCardRenderer: a card is drawn with every section of its mesh,
        // each a mesh batch with its own material): its material, the submeshes it does NOT draw, its motion row.
        struct CardCaptureSection
        {
            MaterialInstance* Instance = nullptr;
            // The section's surface: a static/generic slot's `.demat`, an ISM section group's, a skinned
            // section's. Null for a Shader Override draw (Generic below).
            DataDrivenMaterial* Surface = nullptr;
            uint64_t            Hidden  = 0; // every submesh of the instance's other sections
            // Static, generic and skinned: the primitive in the scene's GpuScene (the view-pass cell's world, and the
            // skinned cell's bone palette).
            uint32_t GpuPrimitive = 0;
            // A Shader Override draw (MaterialComponent: no `.demat`): its record of this frame's generic queue,
            // and the (Static x GBuffer) cell of its template that captures it.
            const GenericMeshRenderData* Generic = nullptr;
            std::string                  OverrideCell;
        };
        // This frame's gathered card instances (GatherSurfaceCacheInstances), by SceneInstance::Source: one per
        // mesh instance, its cards captured by drawing every opaque section into them (a translucent section
        // is not drawn: glass transmits, a card would hold what is behind it).
        struct CardCaptureSource
        {
            const Desert::Mesh* Mesh  = nullptr;
            glm::mat4           World = glm::mat4( 1.0f );
            // Static (static and generic draws: the static cell), Instanced (an ISM instance: the instanced cell,
            // World as its one transform) or Skinned (the skinned cell, posed from the view's ObjectBones).
            MeshVertexPath                  Path = MeshVertexPath::Static;
            std::vector<CardCaptureSection> Sections;
        };
        // THE CARD GATHER (SCENE-CACHE-5; UE FLumenSceneData primitive groups, updated from the scene's primitive
        // add / update / remove lists, and a material's readiness as an event - not every section and every ISM
        // instance asked every frame). Three kinds of group, each kept with the key of the inputs it was gathered
        // from and gathered again only when that key moves (its Revision then moves, and the surface cache walks
        // only that group):
        //  - the static + generic group (one: an entity's sections drawn partly by either queue are one
        //    instance): the static casters' revision (m_StaticCasters, the GpuScene stream: world, mesh, shading,
        //    section slot, hidden submeshes, queue order and - generic - the override's program, textures and
        //    values) while it is tracked for this gather; gathered every frame while it is not;
        //  - one group per ISM field (entity x mesh): its section groups' mesh, render revision, shading, hidden
        //    submeshes, instance count and transforms snapshot (held by owner: a snapshot freed and re-made at the
        //    same address is another one);
        //  - the skinned group: every frame (posed, few).
        // And for every group, the readiness its answers were made under (CardReadinessKey): the material
        // service's invalidation version (a material rebuilt, a texture landed), the pipelines' readiness event
        // (Graphic::PipelineReadinessGeneration), the shader service's recompile generation, the cards that
        // landed (Assets::MeshCardsSlot::PublishGeneration) and the capture pass states.
        struct CardGatherGroup
        {
            uint64_t                                    Id       = 0;
            uint64_t                                    Key      = 0;
            uint64_t                                    Revision = 0;
            bool                                        Gathered = false;
            uint64_t                                    SeenGather = 0; // m_CardGather when the frame had it
            // Its sections' answers that were not the cell's own pipeline (ChoosePipelineDraw counted them as
            // stand-in draws): counted again in every frame the group is kept (GetStandInDraws, the shot gate).
            uint32_t                                    StandIns = 0;
            std::vector<SurfaceCache::SceneInstance>    Instances;
            std::vector<CardCaptureSource>              Sources; // by SceneInstance::Source
            std::vector<std::weak_ptr<const std::vector<glm::mat4>>> Snapshots; // ISM: per section group
        };
        CardGatherGroup                                       m_CardStaticGroup;
        CardGatherGroup                                       m_CardSkinnedGroup;
        std::unordered_map<uint64_t, CardGatherGroup>         m_CardFieldGroups; // by field id
        std::vector<CardGatherGroup*>                         m_CardGroupOrder;  // the last gather's groups
        uint64_t                                              m_CardGather         = 0;
        uint64_t                                              m_CardGroupRevision  = 0;
        [[nodiscard]] uint64_t                                CardReadinessKey() const;
        // The renderer-owned surface (MeshECSSystem's default, no `.demat` and so no view copies): one cell
        // material per capture of it in the frame, its camera in its block.
        // By MeshVertexPath: the static, skinned and instanced cells.
        std::vector<std::shared_ptr<DataDrivenMaterial>> m_CardCaptureMaterials[3];
        std::vector<MaterialInstancePtr>                 m_CardCaptureInstances[3];
        // Shader Override draws: one (template x Static x GBuffer) cell material per capture of the frame, by cell
        // shader and texture set (GenericTextureKey: a sampler is the material's descriptor) - the same keying
        // as the generic queue's shared materials.
        std::unordered_map<std::string, std::vector<std::shared_ptr<DataDrivenMaterial>>> m_CardCaptureOverrides;
        // A skinned mesh's cards (UE MeshCardRepresentation SetCardsFromBounds: skinned meshes have no card
        // build of their own): six cards, one per direction, each the whole bind-pose box. By mesh, built once.
        std::unordered_map<const Desert::Mesh*, std::shared_ptr<const Assets::MeshCardsBuildData>>
                                         m_SkinnedBoundsCards;
        MeshRendererDetail::MeshDrawList m_SilhouetteDraws; // "MeshSilhouettePass": static + generic + skinned
        MeshRendererDetail::MeshDrawList m_OverdrawDraws;   // "Debug: Overdraw"

        // Material pipelines on demand (AL1-12). The spec a data-driven material draws with in this renderer;
        // the requests made when materials LOADED, turned into worker compiles; the engine's default surface,
        // which is what a draw uses until its own pipeline is Ready.
        [[nodiscard]] static GraphicsPipelineSpecification
             GenericPipelineSpec( const std::shared_ptr<Shader>& shader );
        void PrecacheRequestedMaterials();
        // The mesh-table cells requested on load (MaterialPipelineRequests::RequestCell), compiled through the
        // same CellPipeline/CullPermutation the draw uses, so the draw finds them in m_CellPipelines. A cell whose
        // pass state is not built yet waits in m_PendingCellPrecache for the frame that has it.
        void PrecacheRequestedCells();
        void TrackMaterialPipeline( const std::string& shaderName, const GraphicsPipeline& pipeline );
        // The same report through a kept tracker row (SliceRowOf): no name hashed.
        static void TrackMaterialPipeline( MaterialPipelineTracker::Row& row, const GraphicsPipeline& pipeline );

    public:
        // THE ONE PERMUTATION FUNCTION of a generic forward program on the deferred path, asked by every place
        // that builds or cooks it: "Deferred: Generic" (BuildGenericDraws), the prepass's readiness check
        // (GenericForwardPipelineReady) and the cook (MaterialProgramVariants, OverrideProgramVariants). An
        // opaque program reads DESERT_SUN_SHADOW_SCREEN_MASK when @p rayTracedSun and it samples the sun's shadow
        // (declares ShadowCascadesTexture), and DESERT_LOCAL_SHADOW_SCREEN_MASK when @p localShadowMasks (the view has
        // SceneViewInputs.LocalShadowMasks) and it samples the local shadow atlas; defines in that order (sun,
        // then local), ShippedByCook. Under @p virtualLocal (the view has SceneViewInputs.Vsm: Shadows.Method
        // VirtualMaps) a program that samples the local shadow atlas - opaque OR blended, on either render path -
        // also reads DESERT_LOCAL_SHADOW_VSM, the view's virtual shadow map in place of the atlas (appended last).
        // A program that reads none of these is the default permutation (shaded through itself).
        // The forward cell @p cell (the (path x Forward) sibling of the service material @p built) as this frame
        // draws it: under m_ForwardVirtualLocal, when its program samples the local shadow atlas, the same
        // `.demat`'s DESERT_LOCAL_SHADOW_VSM permutation (MaterialService::GetVariant with that define); otherwise
        // @p cell. Null when the service refused the permutation (it names it). A renderer-built material (no
        // `.demat`: MeshECSSystem's default, a stand-in, the instanced spare) is drawn through its own twin
        // (ForwardVsmTwin). A non-Forward @p pass is returned as given. The static, instanced, skinned and GPU
        // (instanced / grass blade) forward draws ask it; a GPU section's cell is this view's copy (@p view: the
        // MaterialService::GetViewVariant key it was asked under, owned by this renderer), and so is its
        // permutation.
        [[nodiscard]] DataDrivenMaterial* ForwardVirtualCell( DataDrivenMaterial* cell, const Material* built,
                                                              MeshVertexPath path, MeshPass pass,
                                                              std::optional<uint32_t> view        = std::nullopt,
                                                              bool                    instanceIds = false,
                                                              bool sceneInstances = false );
        [[nodiscard]] static Core::ShaderVariant ForwardShadowMaskVariant( const Shader& shader, bool rayTracedSun,
                                                                           bool localShadowMasks,
                                                                           bool virtualLocal );
        // THE PERMUTATION a mesh's G-buffer cell is drawn in after the deferred early-Z pass drew it
        // (kSurfaceDepthEqualDefine: equal depth test, no clip; DepthEqualCell, BuildGpuInstancedDraws), asked by
        // the draws and by the cook (MaterialProgramVariants) from this one function.
        [[nodiscard]] static Core::ShaderVariant SurfaceDepthEqualVariant();
        // THE PERMUTATION a GPU-instanced section's cell draws a list of ids in (GPU-COST-22: the camera's list,
        // GpuInstanceRefs::Ids, or a cascade's own list; kSurfaceInstanceIdsDefine, the GpuInstanceIds block at
        // binding 18): @p permutation with that define appended LAST, ShippedByCook. The one spelling of it -
        // the draws (BuildGpuInstancedDraws, GrassVisibilityReady, BuildGpuInstancedShadowDraws), the forward
        // VSM permutation of such a cell (ForwardVirtualCell) and the cook (MaterialProgramVariants) all build
        // it here, so the cooked hash is the acquired one (UE: a permutation declared once, the cook compiling
        // exactly what ShouldCompilePermutation admits).
        [[nodiscard]] static Core::ShaderVariant SurfaceInstanceIdsVariant( Core::ShaderVariant permutation = {} );
        // CPU-ISM-PACK: THE PERMUTATION an ISM's Instanced cell draws in on the CPU path
        // (kSurfaceSceneInstancesDefine,
        // Mesh/Surface/Vertex_Instanced.glslh): InstanceTransforms holds GpuScene instance ids, the vertex stage
        // reads
        // the transform from the scene's rows (UE GPU Scene by InstanceId). @p permutation with the define
        // appended
        // LAST, ShippedByCook; the draws (BuildStaticDraws' ISMs, ForwardVirtualCell) and the cook
        // (MaterialProgramVariants: every Instanced Forward / G-buffer (+ equal depth) / Depth cell) build it
        // here.
        [[nodiscard]] static Core::ShaderVariant
        SurfaceSceneInstancesVariant( Core::ShaderVariant permutation = {} );
        // RENDER-JOB-CPU-4: @p permutation with kSurfaceDrawRowsDefine (a compacted bin's grass cell).
        [[nodiscard]] static Core::ShaderVariant SurfaceDrawRowsVariant( Core::ShaderVariant permutation = {} );
        // Debug.Grass "per-variety draws": every grass section drawn by its own indirect draws, not in the
        // compacted bins (the reference image the bins must match).
        void SetGrassPerVarietyDraws( const bool perVariety ) { m_GrassPerVarietyDraws = perVariety; }
        [[nodiscard]] bool GrassPerVarietyDraws() const { return m_GrassPerVarietyDraws; }
        // THE CELLS an id list is drawn through (UE ShouldCompilePermutation for the instance-id permutation):
        // the GPU-instanced paths, under every pass a camera list draws in (the geometry pass of either render
        // path: Forward, GBuffer, the Depth prepass; the grass visibility raster and its resolve) and the
        // cascade's ShadowDepth. MaterialProgramVariants cooks SurfaceInstanceIdsVariant for each such cell of a
        // `.demat` an id list draws with (LandscapeGrassTypeService::InstanceIdListMaterials; plus its G-buffer
        // DepthEqual and forward VSM compositions); a draw asking for one outside these is named by PackageCook's
        // census (ShaderService::ShippedByCookRequests).
        static constexpr MeshVertexPath kInstanceIdsPaths[]  = { MeshVertexPath::Instanced,
                                                                 MeshVertexPath::GrassBlade };
        static constexpr MeshPass       kInstanceIdsPasses[] = {
             MeshPass::Forward,    MeshPass::GBuffer,           MeshPass::Depth,
             MeshPass::Visibility, MeshPass::VisibilityResolve, MeshPass::ShadowDepth };
        // THE PASSES a compacted bin draws a blade section in (UE: the mesh passes a material's shader map is
        // cooked for): the camera passes DrawBinsOf admits (not the resolve, which reads no draw row; Forward only
        // outside the virtual shadow map, ForwardVirtualCell) and the cascades' ShadowDepth (CascadeBinsOf).
        // DrawBinsOf admits by this list and MaterialProgramVariants cooks the GrassBlade cell of each pass in
        // SurfaceDrawRowsVariant, so a package holds every cell a bin can ask for.
        static constexpr MeshPass kDrawRowsPasses[] = { MeshPass::Forward, MeshPass::GBuffer, MeshPass::Depth,
                                                        MeshPass::Visibility, MeshPass::ShadowDepth };
        // GRASS-GPU-DRIVEN-DRAWS: the vertex paths a bin draws - a blade strip and a mesh grass section (its LOD
        // groups; Vertex_Instanced.glslh's DESERT_SURFACE_DRAW_ROWS cell). DrawBinsOf / CascadeBinsOf admit by it;
        // MaterialProgramVariants cooks the blade cells for every material and the mesh cells for the id-list
        // (grass) materials through their ids only (a mesh bins only through ids), each in the kDrawRowsPasses.
        static constexpr MeshVertexPath kDrawRowsPaths[] = { MeshVertexPath::GrassBlade,
                                                             MeshVertexPath::Instanced };
        // THE BINS' CAPACITY (GRASS-GPU-DRIVEN-DRAWS; UE InstanceCulling sizes its draw table by what the passes
        // admit): the lists of a camera phase table (0, 1) that can bin one section - the deferred early-Z pass,
        // the G-buffer (its two halves share the levels), the visibility raster, the forward prepass and base
        // pass - and of the cascades' table: each cascade view's two parts (the rest records and the movers).
        static constexpr uint32_t kDrawBinCameraLists  = 5u;
        static constexpr uint32_t kDrawBinCascadeLists = SceneResources::kMaxCascadeViews * 2u;
        // The slots one list gives @p gpu in a bin: each drawn submesh's record of each of its levels (a blade
        // strip: one level, its strip levels the submeshes). The producer sizes the tables by it
        // (GrassRenderer::AddDrawGather) and DrawBinsOf reserves an admitted section's slots by it.
        [[nodiscard]] static uint32_t DrawBinSlots( const GpuInstancedRenderData& gpu );
        // The permutations the mesh path can select for @p materials (UE: a material's cooked shader map): each
        // material's (Static x Forward) cell under every ForwardShadowMaskVariant the renderer can ask for (sun
        // only, local only, both), defaults omitted and duplicates listed once - the list PackageCook
        // CookShaderVariants cooks, from the same function the renderer acquires by. A material without that
        // cell draws nothing forward (CreateSurfaceMaterial names it) and has nothing to cook. Also the default
        // surface's (Static|Instanced|Skinned x Forward) cells' DESERT_LOCAL_SHADOW_VSM permutation: the
        // renderer-owned twins (ForwardVsmTwin). A Shader Override draw (no `.demat`) is OverrideProgramVariants'.
        // And every (kInstanceIdsPaths x kInstanceIdsPasses) cell of each @p instanceIdHandles material in
        // SurfaceInstanceIdsVariant (alone, over the G-buffer's equal-depth and over the forward virtual shadow
        // map's permutation): the id-list draws, cooked only for the materials an id list draws with
        // (Runtime::LandscapeGrassTypeService::InstanceIdListMaterials - UE's ShouldCompilePermutation over the
        // material's usage), not for every `.demat`. And the compacted bins' cells: each material's (GrassBlade x
        // kDrawRowsPasses) cell in SurfaceDrawRowsVariant (alone and over the G-buffer's equal-depth permutation),
        // an id-list material's also over SurfaceInstanceIdsVariant - InstanceDrawPermutation's compositions.
        [[nodiscard]] static std::vector<Core::CookedProgramVariant>
        MaterialProgramVariants( const Runtime::MaterialService& materials, const Runtime::ShaderService& shaders,
                                 std::span<const ::Common::AssetHandle> handles,
                                 std::span<const ::Common::AssetHandle> instanceIdHandles );
        // The permutations a Shader Override draw (MaterialComponent::ShaderName, no `.demat`) can select: each
        // named program BuildGenericDraws shades forward, under every ForwardShadowMaskVariant (sun only, local
        // only, both), defaults omitted. A surface template's override is drawn through its (Static x GBuffer)
        // cell, never forward, and has nothing here. PackageCook CookShaderVariants passes the overrides its
        // cooked scenes state.
        [[nodiscard]] static std::vector<Core::CookedProgramVariant>
             OverrideProgramVariants( const Runtime::ShaderService& shaders,
                                      std::span<const std::string>  shaderNames );

    private:
        // Whether the pipeline "Deferred: Generic" shades @p shader's opaque draw with is Ready, tracked (so a
        // compiling one counts among the pending pipelines); @p rayTracedSun and @p localShadowMasks pick the
        // permutation through ForwardShadowMaskVariant, as that pass does - its slice program of @p facts (the
        // template @p shader's), so the key, the variant and the spec are the ones made once per template.
        struct ProgramFacts; // defined with the slice programs below
        [[nodiscard]] bool GenericForwardPipelineReady( ProgramFacts& facts, const std::shared_ptr<Shader>& shader,
                                                        bool rayTracedSun, bool localShadowMasks,
                                                        bool virtualLocal );
        // THE ONE READINESS CHECK for an opaque generic draw on the deferred path: "Deferred: Forward
        // Depth/Normal" admits it when BOTH its depth-normal and its forward pipeline are Ready and records it
        // here; "Deferred: Generic" shades only what this frame admitted, so no draw is shaded forward against a
        // missing prepass depth (it would fail CloserOrEqual) and none is in the depth unshaded. Stamped with the
        // view's frame. Indexed by m_GenericQueue position (1 = admitted; cleared with its capacity kept, grown
        // with zeros on the first admission of a frame): no node allocated per admitted draw per frame.
        bool OpaqueGenericAdmitted( std::size_t index ) const;
        std::vector<uint8_t>                             m_GenericPrepassed;
        std::optional<uint64_t>                          m_GenericPrepassFrame;
        bool                                             m_GenericPrepassRayTracedSun = false;
        bool                                             m_GenericPrepassLocalShadowMasks = false; // the prepass's DESERT_LOCAL_SHADOW_SCREEN_MASK choice
        bool                                             m_GenericLocalShadowMasks        = false; // "Deferred: Generic" (DeclareGenericDraws)
        bool m_GenericPrepassVirtualLocal = false; // the prepass's DESERT_LOCAL_SHADOW_VSM choice
        // DESERT_LOCAL_SHADOW_VSM for the forward cells this frame (static, instanced, skinned, generic, glass):
        // set by each forward Declare from its SceneViewInputs.Vsm before it builds its draws.
        bool m_ForwardVirtualLocal = false;
        std::shared_ptr<GraphicsPipeline> DefaultSurfacePipeline();
        // A caster whose material is Masked draws through its OWN template's (path x ShadowDepth) cell
        // (ShadowCasterCellFor), on a per-cascade copy of that material (MaterialService::GetViewVariant) —
        // its mask texture and clip threshold with it. The cascade material of the draw, or null when the
        // caster takes the shared program (opaque, or no batched-path material).
        DataDrivenMaterial* MaskedCasterMaterial( const DataDrivenMaterial* material, MeshVertexPath path,
                                                  uint32_t cascade ) const;
        // The shared caster pipelines of shadow view @p view, by path, and the cell pass they draw: ShadowDepth
        // for a cascade or local view (a sun cascade's pancaked, SetupShadowPass), VirtualShadowDepth for a
        // virtual shadow map level. Null where that caster is off.
        struct CasterPipelines
        {
            GraphicsPipeline* Static    = nullptr;
            GraphicsPipeline* Instanced = nullptr;
            GraphicsPipeline* Skinned   = nullptr;
            MeshPass          Pass      = MeshPass::ShadowDepth;
            // CPU-ISM-PACK 5: the Instanced state over the shared caster's GpuScene-id permutation (an ISM's
            // casters by id); null for a virtual shadow map level (its draws keep their matrices: its GPU cull
            // reads MeshDrawCommand::Instances) or when that permutation was refused.
            GraphicsPipeline* InstancedSceneIds = nullptr;
        };
        [[nodiscard]] CasterPipelines CasterPipelinesOf( uint32_t view );
        // The pipeline of one masked caster cell: the shared caster's state of @p casters (the view's) for @p path
        // with the cell's shader, built on first use and kept per cell shader and shared state. Null (logged once
        // per shader) when it cannot.
        GraphicsPipeline* MaskedCasterPipeline( const DataDrivenMaterial& caster, MeshVertexPath path,
                                                const CasterPipelines& casters );
#if DESERT_DEV_INSTRUMENTS
        bool SetupDebugLinePass();
        bool SetupOverdrawPass(); // overdraw accumulation pipeline + FB + fullscreen heat resolve
#endif // DESERT_DEV_INSTRUMENTS

    private:
        // Static
        std::shared_ptr<GraphicsPipeline> m_StaticPipeline;
#if DESERT_DEV_INSTRUMENTS
        std::shared_ptr<GraphicsPipeline> m_StaticWireframePipeline; // same spec, PolygonMode::Wireframe
#endif
        std::shared_ptr<GraphicsPipeline> m_StaticInstancedPipeline; // reads per-instance transform from SSBO
#if DESERT_DEV_INSTRUMENTS
        std::shared_ptr<GraphicsPipeline> m_StaticInstancedWireframePipeline; // same spec, PolygonMode::Wireframe
#endif
#if DESERT_DEV_INSTRUMENTS
        bool m_Wireframe = false;
#endif
        bool                              m_LODEnabled = true;

        // Deferred G-buffer geometry pipeline (static): writes Albedo+Metallic / Normal+Roughness into the
        // scene renderer's MRT G-buffer instead of shading. Same vertex layout + material bindings as the
        // forward static pipeline, so the same static-cell data binds. Null if the shader is missing.
        std::shared_ptr<Shader>           m_StaticGBufferShader;
        std::shared_ptr<GraphicsPipeline> m_StaticGBufferPipeline;
        // (Instanced x GBuffer). The G-buffer pass used to have no instanced cell at all, and the ISM
        // queue -- which has no per-object path to fall back to -- was therefore dropped entirely in the
        // deferred path. Null if the shader is missing, and DrawStaticMeshes says so rather than dropping
        // the queue again.
        std::shared_ptr<Shader>           m_InstancedGBufferShader;
        std::shared_ptr<GraphicsPipeline> m_InstancedGBufferPipeline;
        // The two pass states of the masked depth prepass (DeclareMaskedDepthDraws): (Instanced x Depth) into the
        // G-buffer's depth alone, and the G-buffer's instanced state with its depth writes off (the prepass's depth
        // is final; CloserOrEqual passes on it). Null: no prepass, masked sections draw as before.
        std::shared_ptr<GraphicsPipeline> m_InstancedViewDepthPipeline;
        std::shared_ptr<GraphicsPipeline> m_InstancedGBufferEqualPipeline;
        // The deferred early-Z pass's G-buffer states of the CPU-walked statics and skinned meshes (EQUAL, no depth
        // writes; their prepass half is the forward prepass's Depth states, same depth format) and the instanced
        // spare of the Equal half (its own InstanceTransforms, apart from the Rest half's m_InstancedGBufferMaterial).
        // Null: those meshes are not prepassed (SetupGBufferPass said so).
        std::shared_ptr<GraphicsPipeline>            m_StaticGBufferEqualPipeline;
        std::shared_ptr<GraphicsPipeline>            m_SkinnedGBufferEqualPipeline;
        std::shared_ptr<Graphic::DataDrivenMaterial> m_InstancedGBufferEqualMaterial;
        MaterialInstancePtr                          m_InstancedGBufferEqualInstance;
        // This frame's EarlyZPass (SetEarlyZPass); the half being built is the builders' DrawBuildMode.
        ::Common::Scalability::EarlyZPassMode m_EarlyZPass = ::Common::Scalability::EarlyZPassMode::All;
        // GRASS-VIS's two pass states (carriers: CellPipeline swaps in the section's cell): the visibility raster
        // (the masked prepass's depth state with depth writes off) and the resolve (the G-buffer's instanced state,
        // CullMode None - a full-screen triangle). Null: no grass visibility path, the sections keep the G-buffer.
        std::shared_ptr<GraphicsPipeline> m_InstancedVisibilityPipeline;
        std::shared_ptr<GraphicsPipeline> m_InstancedVisibilityResolvePipeline;
        // (Skinned x GBuffer): the deferred path draws skinned meshes in the base pass (UE), bones from the view's
        // ObjectBones, so the composite shades them and the ray-traced sun trace sees their depth and normal; the
        // surface cache captures a skinned mesh's cards with the same state. Null if the shader or the cell is
        // missing, and SetupGBufferPass says skinned meshes are then absent from a deferred scene. The unowned
        // material is the skinned twin of m_GBufferUnownedMaterial (same reason).
        std::shared_ptr<Shader>             m_SkinnedGBufferShader;
        std::shared_ptr<GraphicsPipeline>   m_SkinnedGBufferPipeline;
        std::shared_ptr<DataDrivenMaterial> m_SkinnedGBufferUnownedMaterial;
        // THE FORWARD DEPTH PREPASS's cells (SetupDepthPrepass): the default surface's (path x Depth) programs as
        // pass states, the instanced spare (material + instance), the unowned spares - the G-buffer pass's set,
        // for the view's depth. Null = refused (said by SetupDepthPrepass).
        std::shared_ptr<GraphicsPipeline>   m_StaticDepthPipeline;
        std::shared_ptr<GraphicsPipeline>   m_InstancedDepthPipeline;
        std::shared_ptr<GraphicsPipeline>   m_SkinnedDepthPipeline;
        std::shared_ptr<DataDrivenMaterial> m_DepthUnownedMaterial;
        std::shared_ptr<DataDrivenMaterial> m_SkinnedDepthUnownedMaterial;
        std::shared_ptr<DataDrivenMaterial> m_InstancedDepthMaterial;
        MaterialInstancePtr                 m_InstancedDepthInstance;
        // THE PREPASS's ADMISSIONS, per frame (UE: the depth pass and the base pass draw one mesh-draw list): what
        // "Forward: Depth Prepass" decided for each material group (CellDraw, the Depth and the Forward cells'
        // readiness folded) and which landscape-grass sections it drew. The base pass of the SAME frame reads
        // them instead of asking again, so a pipeline that leaves the driver between the two Declares changes
        // nothing until the next frame (readiness only grows, so the Forward cells it checked are still Ready).
        struct PrepassCellKey
        {
            const DataDrivenMaterial* Material = nullptr;
            MeshVertexPath            Path     = MeshVertexPath::Static;
            bool                      TwoSided = false;
            bool                      PerObject = false;
            // An ISM's GpuScene-id cell (SurfaceSceneInstancesVariant): Own = both passes draw it by id, anything
            // else = by its packed transforms (the plain key's draw).
            bool SceneInstances                             = false;
            auto operator<=>( const PrepassCellKey& ) const = default;
        };
        // A flat vector sorted by key (was a std::map: a node allocation per cell per frame); clear() per frame keeps
        // its capacity. RecordPrepassCellDraw inserts in key order, PrepassCellDraw binary-searches; never walked.
        using PrepassCellEntry = std::pair<PrepassCellKey, MaterialPipelineTracker::CellDraw>;
        std::vector<PrepassCellEntry> m_PrepassCellDraws;
        std::unordered_set<uint64_t>  m_PrepassGpuSections; // GpuInstancedRenderData::Key
        void RecordPrepassCellDraw( const PrepassCellKey& key, const MaterialPipelineTracker::CellDraw draw )
        {
            const auto at = std::ranges::lower_bound( m_PrepassCellDraws, key, {}, &PrepassCellEntry::first );
            if ( at != m_PrepassCellDraws.end() && at->first == key )
                at->second = draw;
            else
                m_PrepassCellDraws.insert( at, PrepassCellEntry{ key, draw } );
        }
        std::optional<uint64_t>                                     m_PrepassFrame;
        // The G-buffer groups' early-Z twins (EarlyZBinTwinsOf), per group pipeline: checked against the group's
        // cell shader (a pipeline rebuilt at the same address is a new group) and held weakly (the PipelineCache
        // owns them; one it dropped is resolved again). Pruned to this frame's groups by DeclareEarlyZBins.
        struct EarlyZTwinPairSlot
        {
            std::weak_ptr<GraphicsPipeline> Depth;
            std::weak_ptr<GraphicsPipeline> Equal;
            bool                            Refused = false; // no such cell / variant: not asked again
        };
        struct EarlyZBinTwinSlot
        {
            std::weak_ptr<Shader> GroupShader;
            EarlyZTwinPairSlot    Bins;
            // The group's cluster cell (IndirectGroup::VirtualGeometryPipeline's shader): Clusters starts again
            // when it changes (the cluster pipeline is made after the group's, or rebuilt).
            std::weak_ptr<Shader> ClusterShader;
            EarlyZTwinPairSlot    Clusters;
        };
        std::unordered_map<const GraphicsPipeline*, EarlyZBinTwinSlot> m_EarlyZBinTwins;
        // The G-buffer's issue of the paired bins (DeclareEarlyZBins), valid for the frame m_EarlyZBinFrame names.
        std::optional<MeshRendererDetail::BinnedIssue> m_EarlyZGBufferIssue;
        std::optional<uint64_t>                         m_EarlyZBinFrame;
        // Phase 2's (DeclareEarlyZPostOcclusionDraws), read by DeclareGBufferPostOcclusionDraws of the same frame.
        std::optional<MeshRendererDetail::BinnedIssue>              m_EarlyZPostGBufferIssue;
        std::optional<uint64_t>                                     m_EarlyZPostFrame;
        ForwardPrepassStats                                         m_ForwardPrepassStats; // GetForwardPrepassStats
        // Whether the pass being built pairs with the other half of the forward prepass (m_GeometryPass Forward
        // or Depth on the forward path, with the prepass's cells): static/instanced/grass/skinned not under the
        // Wireframe view (@p wireframeReplaced: the path has a wireframe twin writing its own depth).
        [[nodiscard]] bool PairsWithPrepass( MeshPass pass, bool wireframeReplaced ) const;
        // This frame's prepass decision for @p key, when the prepass ran this frame and decided it.
        [[nodiscard]] std::optional<MaterialPipelineTracker::CellDraw> PrepassCellDraw( const PrepassCellKey& key ) const;
        [[nodiscard]] bool PrepassCurrent() const;
        // THE TRANSLUCENCY PASS, PER TRANSLUCENT CELL. One entry per translucent template's cell shader: its
        // blended pipeline and a DEDICATED material (+ instance) owning that pass's per-frame UBs / Materials
        // rows — never shared with an opaque material, whose per-frame ring would then be written twice a frame
        // (the GPU hang). There is no engine-wide glass material: the object's template decides its shader.
        struct TranslucentDraw
        {
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::shared_ptr<DataDrivenMaterial> Material;
            MaterialInstancePtr                 Instance;
        };
        std::unordered_map<std::string, std::unique_ptr<TranslucentDraw>> m_Translucent;
        // FrontLayerDrawFor's, by `Static.FrontLayer` cell shader (null = refused, said once).
        std::unordered_map<std::string, std::unique_ptr<TranslucentDraw>> m_FrontLayer;

        // A TEMPLATE PROGRAM'S FACTS, RESOLVED ONCE (CPU-FRAME-ARENA 10; UE FMeshPassProcessor: a draw's shaders
        // are resolved once per material into its cached mesh draw command, and a frame walks typed handles).
        // Keyed by the template's Shader serial, valid for its code generation and while the shader service's
        // RecompileGeneration stands (a Register / Recompile / Clear may add or drop a cell, so every row is
        // forgotten then; BinnedCellFacts' rule). The generic slices, the translucency pass, the front layer and
        // the "who samples this texture" asks read them: no name, TemplateCellShader, FrontLayerShaderFor or
        // GetByName per draw per frame. The TranslucentDraw answers point into m_Translucent / m_FrontLayer,
        // which only ever insert.
        //
        // A TEMPLATE'S SLICE PROGRAMS, the cached half of a generic draw command (CPU-FRAME-ARENA 11; UE
        // FMeshDrawCommand: the program, its name and its pipeline state are built when the primitive's material
        // is added, never per draw). BuildGenericDraws draws a template through one of these: the
        // ForwardShadowMaskVariant of (sun mask | local masks << 1 | virtual local << 2) - indices 0..7 - or one
        // of its cells. Each is asked once per template at its code generation (the AcquireVariant, the key and
        // the spec), so a draw formats no name, builds no permutation and allocates no target layout.
        static constexpr uint32_t kSliceGBufferCell        = 8;  // (Static x GBuffer): the surface override's
        static constexpr uint32_t kSliceDepthCell          = 9;  // (Static x Depth) in the forward prepass
        static constexpr uint32_t kSliceDepthNormalGBuffer = 10; // DepthNormal into the G-buffer
        static constexpr uint32_t kSliceDepthNormalPrepass = 11; // DepthNormal in the forward prepass (depth only)
        static constexpr uint32_t kSlicePrograms           = 12;
        [[nodiscard]] static constexpr uint32_t ForwardSliceIndex( const bool rayTracedSun, const bool localMasks,
                                                                   const bool virtualLocal ) noexcept
        {
            return ( rayTracedSun ? 1u : 0u ) | ( localMasks ? 2u : 0u ) | ( virtualLocal ? 4u : 0u );
        }
        struct SliceProgram
        {
            bool                    Asked = false;
            bool                    Own   = false; // a cell or a permutation, not the template's own program
            std::shared_ptr<Shader> Program;       // null: refused (said once, when asked)
            Core::ShaderVariant     Permutation;   // the forward ones' (default for a cell)
            // The pipeline tracker's and the shared material's name: the program's name + "|define" per define.
            std::string Key;
            // GenericPipelineSpec with DebugName "GenericMesh_<Key>" and the slice's target and depth state (none
            // for the G-buffer cell, which draws through CellPipeline). The layouts are constants
            // (ViewTargetLayouts).
            GraphicsPipelineSpecification Spec;
            // THE DRAW COMMAND'S HANDLES (CPU-FRAME-ARENA 13; UE FMeshDrawCommand keeps its PSO and material
            // binding, the frame walks them). Made on first use, dropped with the facts (code generation,
            // RecompileGeneration), so a draw hashes no string and re-checks no spec:
            // - the pipeline tracker's row of Key (MaterialPipelineTracker::RowOf: rows are never erased);
            MaterialPipelineTracker::Row* PipelineRow = nullptr;
            // - GetOrCreateMaterial( Spec )'s answer, while the pipeline cache's Generation is BuiltGeneration
            //   (a Clear / InvalidateByShader moves it; an insert does not);
            std::optional<Common::ResultStr<std::shared_ptr<GraphicsPipeline>>> Built;
            uint64_t                                                            BuiltGeneration = 0;
            // - the shared Shader Override material of (Key x texture set), by GenericTextureSetOf's id: a
            //   value of m_GenericMaterials, which only ever inserts. An integer hash per draw, the string key is
            //   assembled on the first draw of a (slice program x texture set) alone.
            std::unordered_map<uint32_t, DataDrivenMaterial*> Materials;
        };
        struct ProgramFacts
        {
            uint64_t                CodeGeneration = 0;
            bool                    Opaque         = false; // no blend, depth written
            std::shared_ptr<Shader> GBufferCell; // TemplateCellShader( Static, GBuffer ): set = surface override
            std::shared_ptr<Shader> DepthCell;   // ... ( Static, Depth )
            std::shared_ptr<Shader> DepthNormalCell;      // ... ( Static, DepthNormal )
            std::optional<std::string> FrontLayerCell;    // FrontLayerShaderFor( template )
            std::shared_ptr<Shader>    FrontLayerProgram; // its registered program (null: not registered)
            // TranslucentDrawFor's answer per m_ForwardVirtualLocal, asked on first use (null = refused, said
            // once).
            std::array<TranslucentDraw*, 2> Translucent{};
            std::array<bool, 2>             TranslucentAsked{};
            TranslucentDraw*                FrontLayer      = nullptr; // FrontLayerDrawFor( *FrontLayerCell )'s
            bool                            FrontLayerAsked = false;
            std::array<SliceProgram, kSlicePrograms> Slices{}; // SliceProgramOf's, asked on first use
            // RegisterGenericRows' shared material of (template name x texture set), by GenericTextureSetOf's
            // id: a value of m_GenericMaterials (insert only), so a record's row asks no string key a frame.
            std::unordered_map<uint32_t, DataDrivenMaterial*> RowMaterials;
        };
        // A template program and its facts: what a material or a Shader Override record draws with.
        struct ResolvedProgram
        {
            std::shared_ptr<Shader> Program; // null: no such shader is registered
            ProgramFacts*           Facts = nullptr;
        };
        // A material's template, kept by the material's address with the name it was resolved from: the name is
        // compared (never hashed or looked up) so a material freed and another built at its address re-resolves.
        struct MaterialProgramRow
        {
            std::string             Name;
            std::shared_ptr<Shader> Program;
            // MaterialService::GetVariant's sibling per slice program (SlotVariantOf), valid while the asset's
            // BuildRevision stands: an Invalidate retires the siblings and bumps it.
            uint32_t                                        VariantRevision = 0;
            std::array<DataDrivenMaterial*, kSlicePrograms> Variants{};
            std::array<bool, kSlicePrograms>                VariantAsked{};
        };
        mutable std::unordered_map<uint64_t, ProgramFacts>                        m_ProgramFacts;
        mutable std::unordered_map<const DataDrivenMaterial*, MaterialProgramRow> m_MaterialPrograms;
        mutable uint64_t                                                          m_ProgramFactsRegistry = 0;
        mutable bool                                                              m_ProgramFactsValid    = false;
        // Every m_GenericQueue record's template program, resolved once a frame (the first ask after the queue
        // changed): a slot draw's through m_MaterialPrograms (no name asked), a Shader Override's by its name.
        mutable std::vector<ResolvedProgram> m_GenericPrograms;
        // The Shader Override name each m_GenericPrograms slot was resolved from (empty: a slot-material record
        // or never resolved): compared, never hashed, so a stable queue order asks the service no name a frame.
        mutable std::vector<std::string> m_GenericProgramNames;
        mutable bool                         m_GenericProgramsResolved = false;
        // Forgets every row when the shader service's RecompileGeneration moved.
        void SyncProgramFacts() const;
        // @p shader's facts (made on the first ask of its serial at its code generation).
        ProgramFacts& FactsOf( const Shader& shader ) const;
        // @p material's template program and facts (its name resolved once, see MaterialProgramRow).
        ResolvedProgram ProgramOf( const DataDrivenMaterial& material ) const;
        // m_GenericQueue entry @p index's (a non-DataDriven slot material resolves to none).
        const ResolvedProgram& GenericProgramOf( std::size_t index ) const;
        // m_GenericQueue entry @p index's texture-set key (GenericTextureKey), made with its program once a frame.
        const std::string&               GenericTextureKeyOf( std::size_t index ) const;
        mutable std::vector<std::string> m_GenericTextureKeys; // per queue entry, capacity kept across frames
        mutable std::vector<const std::pair<std::string, uint64_t>*> m_GenericTextureOrder; // its sort scratch
        // m_GenericQueue entry @p index's texture set as an integer (1..): equal keys, equal ids. Interned when
        // the key at a queue slot differs from the one the slot held last frame (a compare, never a hash, for
        // a stable producer order), never per draw.
        uint32_t                                          GenericTextureSetOf( std::size_t index ) const;
        mutable std::vector<uint32_t>                     m_GenericTextureSets;       // per queue entry, 0 = none
        mutable std::string                               m_GenericTextureKeyScratch; // the key being compared
        mutable std::unordered_map<std::string, uint32_t> m_GenericTextureSetIds;     // interned, insert only
        // The shared Shader Override material of @p key (m_GenericMaterials, made with @p shader on a miss):
        // asked on the first use of a (program x texture set) alone; the callers keep the answer by id.
        DataDrivenMaterial* SharedGenericMaterial( const std::string& key, std::size_t index,
                                                   const std::shared_ptr<Shader>& shader );
        std::string         m_GenericMaterialKey; // m_GenericMaterials' lookup key, assembled in place on a miss
        // @p facts' slice program @p index of the template @p program (null: refused or absent, said once).
        SliceProgram* SliceProgramOf( ProgramFacts& facts, const std::shared_ptr<Shader>& program,
                                      uint32_t index );
        // @p slice's pipeline tracker row (RowOf its Key, asked once per slice program).
        MaterialPipelineTracker::Row& SliceRowOf( SliceProgram& slice );
        // @p slice's GetOrCreateMaterial( Spec ) answer, asked again only when the cache's Generation moved.
        const Common::ResultStr<std::shared_ptr<GraphicsPipeline>>& SlicePipelineOf( SliceProgram& slice );
        // @p material's sibling for slice program @p index (MaterialService::GetVariant, asked once per build
        // revision of its asset; see MaterialProgramRow).
        DataDrivenMaterial* SlotVariantOf( DataDrivenMaterial& material, uint32_t index, MeshPass pass,
                                           const Core::ShaderVariant& permutation );
        // The translucency pass's / the front layer's draw state of @p mat's (non-null) template, by its facts.
        TranslucentDraw* TranslucentDrawOf( const DataDrivenMaterial* mat );
        TranslucentDraw* FrontLayerDrawOf( const DataDrivenMaterial* mat );

        // (Static x GBuffer) for meshes whose FORWARD material is not one MaterialService owns — in
        // practice only MeshECSSystem's default material, which stands in for a mesh whose slot did not
        // resolve. It has no `.demat`, so it has no sibling in any other pass, and the deferred pass
        // dropped those meshes entirely until this existed. See SetupGBufferPass for why ONE is enough.
        std::shared_ptr<DataDrivenMaterial> m_GBufferUnownedMaterial;

        std::shared_ptr<Shader>   m_GeometryShader;
        std::shared_ptr<Shader>   m_InstancedGeometryShader;

        // Auto-batching SPARE: the material an instanced batch is recorded with when the batch's own
        // material came from no `.demat` at all — in practice MeshECSSystem's default, standing in for a
        // mesh whose slot did not resolve.
        //
        // IT USED TO RECORD EVERY BATCH, AND THAT WAS THE DEFECT. A material built here has never been
        // through MaterialService, so every 2D sampler it declares holds the shader schema's 1x1 white.
        // Recording an asset-backed group with it deleted that surface's whole texture channel while
        // leaving its colours, tiling-independent, intact — see InstancedRecorder.hpp for the numbers and
        // for why rebinding this one material per batch cannot be the fix. A batch now finds its own
        // (Instanced x pass) sibling through MaterialService; this stays for the one group that has none.
        std::shared_ptr<Graphic::DataDrivenMaterial> m_StaticInstancedMaterial;
        MaterialInstancePtr                   m_StaticInstancedInstance;
        // The same pair for the G-buffer pass. A material is one shader's descriptor sets plus a payload,
        // so the pass that binds the (Instanced x GBuffer) pipeline has to bind sets allocated from that
        // cell's own reflection -- the same reason m_GBufferUnownedMaterial exists for
        // the static path.
        std::shared_ptr<Graphic::DataDrivenMaterial> m_InstancedGBufferMaterial;
        MaterialInstancePtr                   m_InstancedGBufferInstance;

        // ONE MaterialInstance per instanced material the service hands out, kept because an instance is
        // allocated per material and a batch needs one to Bind with — not per frame and not per object.
        //
        // Keyed by the raw material and dropped WHOLE whenever MaterialService's invalidation stamp
        // moves: a reloaded `.demat` graveyards every runtime material it built, so an entry surviving
        // that bump is a MaterialInstance holding a parent pointer into a material about to be destroyed.
        // The same stamp MeshECSSystem already rebuilds its cached slots on.
        std::unordered_map<const Graphic::DataDrivenMaterial*, MaterialInstancePtr> m_InstancedVariantInstances;
        uint32_t                                                             m_InstancedVariantStamp = 0;

        // Skinned
        std::shared_ptr<GraphicsPipeline> m_SkinnedPipeline;
#if DESERT_DEV_INSTRUMENTS
        std::shared_ptr<GraphicsPipeline> m_SkinnedWireframePipeline; // same spec, PolygonMode::Wireframe, depth written
#endif
        std::shared_ptr<Shader>   m_SkinnedShader;

        // Silhouette (mask for the Jump Flood outline)
        std::shared_ptr<GraphicsPipeline>   m_SilhouettePipeline;
        std::shared_ptr<Shader>             m_SilhouetteShader;
        std::unique_ptr<MaterialSilhouette> m_SilhouetteMaterial;
        std::shared_ptr<Framebuffer>        m_SilhouetteMaskFramebuffer;
        // Skinned silhouette (selected skinned meshes -> outline). Skins by the Bones SSBO so the mask
        // matches the posed/animated mesh. Optional — null if the Silhouette_Skinned shader is missing.
        std::shared_ptr<GraphicsPipeline>          m_SilhouetteSkinnedPipeline;
        std::shared_ptr<Shader>                    m_SilhouetteSkinnedShader;
        std::unique_ptr<MaterialSilhouetteSkinned> m_SilhouetteSkinnedMaterial;

        // Cascaded directional shadow maps: one framebuffer + one MaterialShadow (its own light-matrix UB,
        // so the 4 cascade passes don't alias a shared UBO) per cascade. m_CascadeVP is recomputed each
        // frame by UpdateCascades().
        std::shared_ptr<GraphicsPipeline> m_ShadowPipeline;
        std::shared_ptr<Shader>           m_ShadowShader;
        std::vector<std::unique_ptr<MaterialShadow>> m_ShadowMaterial =
             std::vector<std::unique_ptr<MaterialShadow>>( kFixedShadowViews );
        // The sampled cascade maps: ONE depth array, a layer per cascade, owned by this renderer across frames
        // and handed to each frame's graph (AcquireCascadeMaps). Empty until the first frame after the budget.
        RDG::ExternalTexture m_CascadeMaps;
        bool                 m_CascadeMapsBuilt = false;
        // The cache of each cascade's static casters (depth only, the map's format): drawn when its key changes,
        // copied into layer c of m_CascadeMaps whenever the map is rebuilt.
        std::shared_ptr<Framebuffer> m_CascadeStaticFB[kCascadeViews]; // by cascade view; a slot not held: null
        // WHERE A CASCADE'S MAP LIES (CascadeFit::TexelOrigin and DepthOrigin, with the light and the texel it was
        // fitted with): two placements that differ only in TexelOrigin hold the same static depth, shifted.
        struct CascadePlacement
        {
            glm::vec3  Travel{ 0.0f };
            float      WorldPerTexel = 0.0f;
            float      DepthOrigin   = 0.0f;
            glm::ivec2 TexelOrigin{ 0 };
            [[nodiscard]] bool SameMapAs( const CascadePlacement& other ) const
            {
                return Travel == other.Travel && WorldPerTexel == other.WorldPerTexel &&
                       DepthOrigin == other.DepthOrigin;
            }
            // The texel shift from this placement to @p next in framebuffer axes (destination = source + shift):
            // x runs with light-space x, y against light-space y (the graph's viewport has a negative height).
            [[nodiscard]] glm::ivec2 ShiftTo( const CascadePlacement& next ) const
            {
                return glm::ivec2( TexelOrigin.x - next.TexelOrigin.x, next.TexelOrigin.y - TexelOrigin.y );
            }
        };
        struct CascadeCacheState
        {
            std::optional<uint64_t> StaticKey; // the key the cache was drawn with; nullopt = never drawn
            bool                    RedrawStatic = false; // this frame: the static layer is drawn
            bool                    Rebuild      = false; // this frame: the map is the cache copied (+ movable)
            bool                    Movable      = false; // this frame: a movable caster reaches the cascade
            bool MapHasMovable = false; // the map holds movable depth from the frame it was built
            // StaticKey is the CONTENT key (CascadeStaticSignature without the camera's levels, the keyed casters'
            // CascadeShadowContent): the kind a scrolled cache is checked with.
            bool                            KeyIsContent = false;
            std::optional<CascadePlacement> Placement;        // where the cache's depth lies
            glm::mat4                       CasterVP{ 1.0f }; // the caster matrix of that placement
            // This frame the cache SCROLLS (UE's CSM scrolling): shifted by Shift, only its strips drawn.
            bool       Scroll = false;
            glm::ivec2 Shift{ 0 };
            uint32_t   StripCount = 0;
            glm::mat4  Strips[2]{ glm::mat4( 1.0f ), glm::mat4( 1.0f ) }; // CascadeScrollStrips
            // ...and each strip's texels (x, y, width, height): the scissor its draws are recorded under.
            glm::uvec4 StripRects[2]{ glm::uvec4( 0u ), glm::uvec4( 0u ) };
            // THE MOVABLE LAYER'S KEY (SHADOW-CACHE-MOTION; UE's VSM: a cached page is invalidated only by a
            // primitive that updates): CascadeMovers' casters and poses with the caster matrix and bias, the key
            // of what the movable casters draw. This frame's (MovableKey: nullopt when no mover reaches the
            // cascade or a mover no key holds does, CascadeMovers::Unkeyed) and the one the map's movable depth
            // was drawn with (MapMovableKey: nullopt when the map holds none or one no key held). While the two
            // are equal and the static layer stands, the map is last frame's: no restore, no movable draw.
            std::optional<uint64_t> MovableKey;
            std::optional<uint64_t> MapMovableKey;
            // The texels (x, y, width, height) the keyed movers cover this frame (MovableRect) and the ones the
            // map's movable depth was drawn under (MapMovableRect); nullopt: not known, the whole layer. A key
            // change over a standing static layer restores and redraws only their union (MovableRegion).
            std::optional<glm::uvec4> MovableRect;
            std::optional<glm::uvec4> MapMovableRect;
            // This frame: the union a rebuild restores and draws its movers under; nullopt, the whole layer.
            std::optional<glm::uvec4> MovableRegion;
            // The last reason logged for the movable layer ("[Shadow] cascade ... movable key ..."): one line a
            // change.
            std::string MovableLogged;
        };
        CascadePlacement m_CascadePlacement[kCascadeViews]; // this frame's fits (UpdateCascades)
        // The instance snapshots the static layers' keys were taken over, held one frame longer than the queue
        // holds them: a snapshot is immutable and is replaced when its instances change, so its address is its
        // revision only while no new snapshot can be allocated at that address — which holding it guarantees.
        std::vector<std::shared_ptr<const std::vector<glm::mat4>>> m_CascadeKeySnapshots;
        CascadeCacheState                                          m_CascadeCache[kCascadeViews];
        MeshRendererDetail::MeshDrawList                           m_CascadeMovableDraws[kCascadeViews];
        std::vector<std::weak_ptr<IShadowCaster>> m_ShadowCasters; // non-mesh casters, AddShadowCaster

        // Instanced shadow caster: one pipeline + per-cascade instanced material (each owns the cascade's
        // light matrix UBO + an InstanceTransforms SSBO). Batched casters of one mesh collapse to a single
        // instanced draw per cascade. Optional — null if the Instanced.ShadowDepth cell is missing.
        std::shared_ptr<GraphicsPipeline>        m_ShadowInstancedPipeline;
        std::shared_ptr<Shader>                  m_ShadowInstancedShader;
        std::vector<std::unique_ptr<MaterialShadowInstanced>> m_ShadowInstancedMaterial =
             std::vector<std::unique_ptr<MaterialShadowInstanced>>( kFixedShadowViews );
        // CPU-ISM-PACK 5 (UE: a shadow view draws its instances by id from GPU Scene): the shared Instanced caster
        // in SurfaceSceneInstancesVariant, its clipped and pancaked pipelines, and one material per shadow view
        // (its light camera + its own uint id list in InstanceTransforms). Null when the permutation was refused:
        // the ISMs then cast through the packed matrices above.
        std::shared_ptr<Shader>                               m_ShadowInstancedIdShader;
        std::shared_ptr<GraphicsPipeline>                     m_ShadowInstancedIdPipeline;
        std::shared_ptr<GraphicsPipeline>                     m_CascadeShadowInstancedIdPipeline;
        std::vector<std::unique_ptr<MaterialShadowInstanced>> m_ShadowInstancedIdMaterial =
             std::vector<std::unique_ptr<MaterialShadowInstanced>>( kFixedShadowViews );

        // Skinned shadow caster: the (Skinned x ShadowDepth) cell, which did not exist — the cascade pass
        // walked the static queue by name and a character cast nothing. One material per cascade, exactly
        // like the two above, and every skinned caster's pose packed into its single Bones buffer.
        // Optional — null if the Skinned.ShadowDepth cell is missing, and then skinned shadows are simply off.
        std::shared_ptr<GraphicsPipeline>      m_ShadowSkinnedPipeline;
        // The three caster states above with depth clamp: what a sun cascade's two layers draw with
        // (SetupShadowPass, CasterPipelinesOf), so a caster nearer the sun than a level's depth range is pancaked,
        // not clipped.
        std::shared_ptr<GraphicsPipeline>                   m_CascadeShadowPipeline;
        std::shared_ptr<GraphicsPipeline>                   m_CascadeShadowInstancedPipeline;
        std::shared_ptr<GraphicsPipeline>                   m_CascadeShadowSkinnedPipeline;
        std::shared_ptr<Shader>                m_ShadowSkinnedShader;
        std::vector<std::unique_ptr<MaterialShadowSkinned>> m_ShadowSkinnedMaterial =
             std::vector<std::unique_ptr<MaterialShadowSkinned>>( kFixedShadowViews );

        // The virtual shadow map's caster pipelines, by path: the (path x VirtualShadowDepth) cells of the default
        // surface, no attachment (RenderTargetLayout{}), no depth test (the pool's atomic max is the test). Null
        // until the "VSM: Raster" passes build them.
        std::shared_ptr<GraphicsPipeline> m_VsmPipeline;
        std::shared_ptr<GraphicsPipeline> m_VsmInstancedPipeline;
        std::shared_ptr<GraphicsPipeline> m_VsmSkinnedPipeline;
        // Builds them on the first "VSM: Raster" (the static one is the gate: without it nothing casts; the
        // instanced and skinned ones are each logged and left null, as the cascades' are). Same depth-only state
        // as the cascades' casters but no attachment (RenderTargetLayout{}), no depth test (the pool's atomic max
        // is the test) and depth clamp (UE VSM: a caster nearer the sun than a level's depth range is flattened
        // onto it instead of clipped).
        [[nodiscard]] Common::BoolResultStr EnsureVirtualShadowPipelines();
        std::optional<std::string>          m_VsmPipelinesRefused;
        // Each level's casters, built by its node's setup and recorded by its exec; the non-mesh casters its setup
        // declared, each with the index of its first block (after the list's).
        std::array<MeshRendererDetail::MeshDrawList, kVsmClipmapLevelCount> m_VsmDraws;
        // Each level's movers ("VSM: Raster Movable L<l>"): no non-mesh caster, so its caster list stays empty.
        std::array<MeshRendererDetail::MeshDrawList, kVsmClipmapLevelCount> m_VsmMovableDraws;
        // This frame's VG-only issue of every level (SetVirtualShadowGeometry), consumed by
        // AddVirtualShadowRasterPasses into m_VsmGeometryDraws ("VSM: Raster VirtualGeometry"); while set, the
        // levels' CPU walk leaves out the submeshes whose clusters it draws (CpuWalkHidden).
        std::optional<MeshRendererDetail::BinnedIssue> m_VsmGeometry;
        MeshRendererDetail::MeshDrawList               m_VsmGeometryDraws;
        uint32_t                                       m_VsmRefusedGroups     = 0;
        uint32_t                                       m_VsmFallbackSubmeshes = 0;
        // THE CARRIER OUTCOME of the levels' cluster raster (VsmRasteredSubmeshes): the sorted `.VirtualGeometry`
        // twins whose carrier AddVirtualShadowRasterPasses made this frame under an accepted draw list (empty: no
        // raster, a refused cut / list, every carrier refused), and whether "VSM: Raster VirtualGeometry" was
        // recorded. ResetVirtualShadowGeometry shifts a recorded frame's set into last frame's (an unrecorded one
        // leaves it empty): the page cache's signature of a VG caster holds its rastered set under it, so a
        // submesh moving between the clusters and the CPU walk (a twin's pipeline arrives, DAG rows upload, the
        // cut or a carrier is refused or comes back) re-renders exactly its pages.
        std::vector<const GraphicsPipeline*> m_VsmCarriedTwins;
        std::vector<const GraphicsPipeline*> m_VsmCarriedTwinsLastFrame;
        // Moves whenever ResetVirtualShadowGeometry shifts in a last-frame set other than the one before it (the
        // producer's event, O(twins)): the key the VSM page cache's kept static / generic part is made under.
        uint64_t m_VsmCarriedTwinsRevision = 0;
        bool                                 m_VsmClusteredThisFrame = false;
        // CpuWalkHidden's interned words past submesh 63 (a std::set: its elements never move, so the views
        // the cascades' and levels' draws hold stay valid until ResetVirtualShadowGeometry), and its scratch.
        std::set<std::vector<uint64_t>> m_ClusterWalkWords;
        std::vector<uint64_t>           m_ClusterWalkScratch;
        // SetFrameClusterCandidates: per VG primitive of this frame, its candidate submeshes (word i / 64).
        std::unordered_map<GpuDriven::PrimitiveIndex, std::vector<uint64_t>> m_FrameClusterCandidates;
        // Per non-mesh caster: its first block on the level's node and its first culled argument record.
        struct VsmCasterEntry
        {
            std::weak_ptr<IShadowCaster> Caster;
            uint32_t                     FirstBlock  = 0;
            uint32_t                     FirstRecord = 0;
        };
        std::array<std::vector<VsmCasterEntry>, kVsmClipmapLevelCount> m_VsmCasters;
        std::array<std::vector<VsmCasterEntry>, kVsmClipmapLevelCount> m_VsmMovableCasters;
        // The local lights' drawn mips (VirtualShadowMapFrame::LocalRasters), one entry each, in the frame's order:
        // its casters and non-mesh casters as a sun level's. Grows and is never shrunk, so a node's lambdas hold a
        // stable entry.
        struct VsmLocalRasterDraws
        {
            MeshRendererDetail::MeshDrawList Draws;
            std::vector<VsmCasterEntry>      Casters;
        };
        std::vector<std::unique_ptr<VsmLocalRasterDraws>> m_VsmLocalDraws;
        // One raster node of the virtual shadow map: the casters of shadow view @p view in @p layer culled against
        // level @p level's drawn pages, over a @p resolution texel square. All (a local view) and Static (a sun
        // level's still casters, with the non-mesh casters) write both pool layers of the pages whose static layer
        // is redrawn; Movable (a sun level's movers) the merged layer of every drawn page.
        [[nodiscard]] Common::BoolResultStr AddVirtualShadowRasterNode(
             RDG::Builder& graph, const VirtualShadowMapFrame& frame, const VirtualShadowMapRenderer& map,
             uint32_t level, uint32_t view, ShadowCasterLayer layer, uint32_t resolution, std::string_view name,
             MeshRendererDetail::MeshDrawList& draws, std::vector<VsmCasterEntry>& casters );
        // CollectVirtualShadowInvalidation's state from the last frame the map was drawn: the casters that stand
        // still, sorted by signature, and the boxes of the ones that move by themselves. The instance snapshots
        // the still ISMs' signatures name, kept alive so a freed snapshot's address is not reused by a new one
        // under the same signature (m_CascadeKeySnapshots says the same).
        // m_VsmStillCasters holds the ISM fields and the non-mesh casters; the static / generic queues' entries are
        // m_VsmStaticStill's.
        std::vector<ShadowCasterEntry>                             m_VsmStillCasters;
        // THE STATIC / GENERIC STILL ENTRIES (SCENE-CACHE-6; UE FCachedShadowMapData: a static primitive's
        // invalidation comes from its own update, never from re-hashing every static primitive each frame): last
        // drawn frame's entries of the static and generic queues, sorted by signature, and the keys they were made
        // under. Made again (and diffed against the previous ones) only when the static casters' revision, the rows'
        // LOD key, the carrier outcome (m_VsmCarriedTwinsRevision), the VG table's uploads
        // (VirtualMeshTable::UploadGeneration) or the VirtualShadow pass table (DrawCommandCache::TableRevision)
        // moved, or the gather is untracked (Valid false); otherwise they add no box and are not walked.
        struct VsmStaticStill
        {
            bool                           Valid     = false;
            uint64_t                       Revision  = 0;
            uint64_t                       LodKey    = 0;
            uint64_t                       Carriers  = 0;
            uint64_t                       VgUploads = 0;
            uint64_t                       VgTable   = 0;
            std::vector<ShadowCasterEntry> Entries;
        };
        VsmStaticStill m_VsmStaticStill;
        std::vector<VsmInvalidationBox>                            m_VsmMovingBoxes;
        std::vector<std::shared_ptr<const std::vector<glm::mat4>>> m_VsmKeySnapshots;
        // Per still ISM (by its signature): what each instance cast last frame, 0 = faded out by its cull
        // distance, else 1 + its LOD - both follow the main camera, and an instance whose state changed redraws
        // the pages under it. Kept in place (CPU-FRAME-ARENA 7): a row is stamped with the collect that wrote it
        // (m_VsmIsmCollect), compared only when the previous collect wrote it, and erased when a collect does not
        // name it, so a still field's vector keeps its capacity instead of a new map + vector per frame.
        struct VsmIsmStateRow
        {
            std::vector<uint32_t> States;
            uint64_t              Collect = 0;
        };
        std::unordered_map<uint64_t, VsmIsmStateRow> m_VsmIsmStates;
        uint64_t                                     m_VsmIsmCollect = 0;
        // CollectVirtualShadowInvalidation's working lists, the other half of the kept ones (m_VsmStillCasters,
        // m_VsmMovingBoxes, m_VsmKeySnapshots, m_VsmStaticStill.Entries are swapped with them at the end, so both
        // halves keep their capacity; UE FMemStack-style frame scratch with no allocation once warm). Empty
        // between collects; the collect runs on the render thread alone.
        struct VsmCollectScratch
        {
            std::vector<ShadowCasterEntry>                             Still;
            std::vector<ShadowCasterEntry>                             Fresh;
            std::vector<ShadowCasterEntry>                             Others;
            std::vector<VsmInvalidationBox>                            Moving;
            std::vector<VsmInvalidationBox>                            NowSorted;
            std::vector<std::shared_ptr<const std::vector<glm::mat4>>> Snapshots;
            std::vector<uint32_t>                                      States;
        };
        VsmCollectScratch m_VsmCollectScratch;
        // The VSM frame's other kept working storage (render thread, one user at a time):
        // ResetVirtualShadowGeometry rotates the carried list through m_VsmCarriedScratch (empty between frames);
        // SetVirtualShadowGeometry's per-group fallback marks; AddVirtualShadowRasterNode's cull lists (copied
        // into the graph's uploads by AddCasterCulling at the call) and one non-mesh caster's draws.
        std::vector<const GraphicsPipeline*> m_VsmCarriedScratch;
        std::vector<uint8_t>                 m_VsmFallbackScratch;
        VsmCasterCull                        m_VsmCasterCullScratch;
        std::vector<VirtualShadowCasterDraw> m_VsmCasterDrawsScratch;
        // The VSM raster nodes' names (PassNameTable, made once): the sun's per clipmap level [still, movable],
        // and the local mips' per raster index, keyed by (light, view, mip) so a row is formatted again only when
        // the raster at that index changes.
        std::array<PassNameTable, 2> m_VsmRasterNames;
        PassNameTable                m_VsmLocalRasterNames;
        // SetVirtualShadowGeometry's answer per group cell (CPU-FRAME-ARENA 8), so a frame asks no name of a
        // group: keyed by the group pipeline's Shader serial, valid for that shader's code generation and while
        // the shader service's RecompileGeneration stands (a Register / Recompile / Clear may add or drop the
        // `.VirtualGeometry` twin DeclaresVirtualGeometryTwin looks up, so every row is forgotten then).
        enum class VsmCellClass : uint8_t
        {
            NoPermutation, // not a Static / Instanced binned cell: holds no clusters
            Fallback,      // its template declares no `.VirtualGeometry` twin
            Twin,          // declares the twin: a group of it without a VG pipeline is a held refusal
        };
        // CPU-FRAME-ARENA 9: the row is every pass's answer about a cell (ResolveVirtualGeometryPipeline,
        // SetFrameVirtualGeometry, the VSM carriers), made once by BinnedCellFacts: no name, MeshCellPath or
        // GetByName per call or per group per frame.
        struct VsmCellRow
        {
            uint64_t     CodeGeneration = 0;
            VsmCellClass Class          = VsmCellClass::NoPermutation;
            bool         Binned         = false; // IsBinnedCell( name )
            bool         DeclaresTwin   = false; // DeclaresVirtualGeometryTwin( name ), whatever the path
            bool         RefusalSaid    = false; // m_BinnedRefused asked for "<cell>|vsm-vg" already
            bool         VgRefusalSaid  = false; // ... for "<cell>|vg" (SetFrameVirtualGeometry)
            bool         CarrierSaid    = false; // ... for "<cell>|vsm-vg-carrier" (a refused VSM carrier)
            std::string  TwinCell;               // VirtualGeometryCellOf( name ), when Class == Twin
        };
        std::unordered_map<uint64_t, VsmCellRow> m_VsmCellClasses;
        uint64_t                                 m_VsmCellClassesRegistry = 0;
        bool                                     m_VsmCellClassesValid    = false;
        // @p shader's row of m_VsmCellClasses, made (its name read, MeshCellPath / DeclaresVirtualGeometryTwin
        // asked) only for a shader not met at this code generation since the shader service last moved
        // (RecompileGeneration: a Register / Recompile / Clear forgets every row).
        VsmCellRow& BinnedCellFacts( const Shader& shader );
        // A masked caster's pipeline was still compiling when a level was drawn (BuildShadowCascadeDraws).
        bool m_VsmCastersPending = false;

        // THE BUDGET, copied from the owning SceneRenderer in Initialize() and never written again. Held
        // by value rather than read through m_SceneRenderer on every use, because the three numbers decide
        // what was ALLOCATED: reading them live would let a later write leave the fitter and the
        // framebuffers disagreeing, which is the one failure this cannot be allowed to have.
        ShadowQuality m_Shadow;

        // The live-total accounting for the cascade framebuffers, held BESIDE them so it is released
        // exactly when they are — see Graphic::ShadowAttachmentLease for why this is RAII and not a pair
        // of hand-written add/remove calls.
        ShadowAttachmentLease m_ShadowAttachments;

        // How many directional-shadow slots hold caches, materials and array layers (EnsureCascadeSlots).
        uint32_t m_CascadeSlots = 1;
        // ONE DIRECTIONAL LIGHT'S CASCADES THIS FRAME, per slot (UpdateCascades): what its light asked for and
        // what its fit produced. Fitted = how many of its m_CascadeVP rows THIS frame wrote (0 until UpdateCascades
        // runs; see DirectionalShadows for why this is not m_Shadow.CascadeCount).
        struct DirectionalShadowSlot
        {
            uint32_t  Fitted        = 0;
            bool      CastShadows   = false;
            float     SourceTanHalf = 0.0f;
            glm::vec4 WorldPerTexel = glm::vec4( 1.0f ); // per cascade: world size of one map texel (2*radius/res)
            glm::vec4 DepthRange    = glm::vec4( 1.0f ); // per cascade: world length of the [0,1] light depth
            glm::vec4 SplitFar      = glm::vec4( 0.0f ); // per cascade: its split's distance from the camera (cm)
            glm::vec3 Travel        = glm::vec3( 0.0f ); // the direction the light travels, as it was fitted
        };
        DirectionalShadowSlot m_DirectionalShadow[kDirectionalShadowSlots];

        // The receivers' matrix of every cascade view, EVERY element identity (a zero matrix's w is 0 and its
        // perspective divide a division by zero in the sampling shader). Nothing should ever read past a slot's
        // Fitted — this is what the array holds if something does.
        std::vector<glm::mat4> m_CascadeVP = std::vector<glm::mat4>( kCascadeViews, glm::mat4( 1.0f ) );
        // The receivers sample with m_CascadeVP; the casters draw with m_CascadeCasterVP (the constant depth
        // bias folded in, CasterViewProj) and write the slope term from m_CascadeCasterBias through CameraUB.
        // Indexed by SHADOW VIEW: the fixed views first, then the local views (kLocalShadowViewBase + row),
        // whose matrices UpdateLocalShadows writes with the light's bias (Graphic::LocalCasterBias, texels at the
        // caster). Sized kFixedShadowViews + this frame's local views (EnsureShadowViewCount).
        std::vector<glm::mat4> m_CascadeCasterVP = std::vector<glm::mat4>( kFixedShadowViews, glm::mat4( 1.0f ) );
        std::vector<CascadeCasterBias> m_CascadeCasterBias = std::vector<CascadeCasterBias>( kFixedShadowViews );
        // Per shadow view past the fixed ones: 1 = this frame gave it to a local light's virtual shadow map mip
        // (IsVirtualShadowView), 0 = an atlas row. And the kind each view's caster materials were made for: a
        // view whose kind flipped makes them again on the other cells (EnsureShadowViewMaterials).
        std::vector<uint8_t> m_VirtualShadowViews     = std::vector<uint8_t>( kFixedShadowViews, 0u );
        std::vector<uint8_t> m_ShadowMaterialsVirtual = std::vector<uint8_t>( kFixedShadowViews, 0u );
        // Grows every per-view array above (and the local views' draw lists) to hold @p viewCount views. Never
        // shrinks: a view's caster materials are kept for the next frame that has as many local views.
        void EnsureShadowViewCount( uint32_t viewCount );

        // THE LOCAL LIGHTS' SHADOW STATE OF THIS VIEW. The atlas, its tiles and what each light's tiles hold are
        // the SCENE's (Graphic/LocalShadowCache.hpp, through SceneRenderer::GetFrameLocalShadows): every view of a
        // scene - the reflection captures' faces too - reads one cached map per light. What stays per view is its
        // table of rows over those tiles and the passes that redraw the stale ones.
        // This frame's table (one row per view, in view order) and the views it redraws.
        std::vector<LocalShadowViewPayload> m_LocalShadowRows;
        struct LocalShadowRedraw
        {
            uint32_t  View = 0; // shadow view index (kLocalShadowViewBase + row)
            AtlasTile Tile;
        };
        std::vector<LocalShadowRedraw>    m_LocalRedraws;
        // What each shadowed local light asks UpdateLocalShadows for (a tile size from its screen size): the
        // per-call list, kept so its storage survives the frame (cleared and refilled each call).
        struct LocalShadowRequest
        {
            uint64_t                        Key          = 0;
            bool                            Spot         = false;
            std::size_t                     Index        = 0;
            uint32_t                        Size         = 0;
            float                           ScreenRadius = 0.0f;
            float                           Distance     = 0.0f; // from the eye to the light, cm
            const LocalLightShadowSettings* Settings     = nullptr;
        };
        std::vector<LocalShadowRequest> m_LocalShadowRequests;
        // LocalShadowPasses' output, kept: the first m_LocalShadowPassCount entries are this frame's passes, the
        // ones past it are cleared (no captured atlas). Names come from m_LocalShadowPassNames, one per slot, made
        // once ("MeshShadowLocal<slot>") and copied into the kept pass's string (its capacity reused).
        std::vector<SystemRasterPass> m_LocalShadowPassList;
        std::size_t                   m_LocalShadowPassCount = 0;
        std::vector<std::string>      m_LocalShadowPassNames;
        // What PlanLocalShadowCache decides from: each light given views this frame, where it reaches.
        struct LocalShadowCachePlan
        {
            uint64_t               Light = 0; // its key in the scene's LocalShadowCache
            LocalShadowCasterLight Caster;    // its views, reach and detail view (FirstView - kLocalShadowViewBase =
                                              // its first row of m_LocalShadowRows)
            uint64_t               Frame   = 0;     // the cache's frame the views were given in
            bool                   Primary = false; // given by a primary view (a level's viewport)
        };
        std::vector<LocalShadowCachePlan> m_LocalCachePlans;
        // Per local view (its row of m_LocalShadowRows), as many as this frame shadows (EnsureShadowViewCount).
        std::vector<std::unique_ptr<MeshRendererDetail::MeshDrawList>> m_LocalDraws;
        // Each local view's non-mesh casters its Declare declared, with the index of its first block (as
        // m_CascadeCasters for a cascade).
        std::vector<std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>>> m_LocalCasters;

        // The rain occlusion (SetRainOcclusionQuality / UpdateRainOcclusion / RainOcclusionPass): its budget, its
        // depth target (kept across frames: redrawn only when the box moved or its casters changed), the box it
        // holds (empty until first fitted, or without a caster pipeline), the casters' signature it was drawn
        // with, and this frame's draws and non-mesh casters with their first binding block.
        RainOcclusionQuality                                           m_RainOcclusionQuality;
        std::shared_ptr<Framebuffer>                                   m_RainOcclusionFB;
        std::optional<RainOcclusionBox>                                m_RainOcclusionBox;
        std::optional<uint64_t>                                        m_RainOcclusionSignature;
        bool                                                           m_RainOcclusionRedraw = false;
        MeshRendererDetail::MeshDrawList                               m_RainOcclusionDraws;
        std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>> m_RainOcclusionCasters;
        uint32_t                          m_LocalRefusedLogged = 0; // lights last reported as left unshadowed
        // The SUN'S cascade policy (ViewShadowSettings, SetShadows); atmosphere light 1's comes with it each frame
        // (SceneRenderer::GetAtmosphereLight1).
        bool                              m_ShadowsEnabled  = true;
        CascadeBiasSettings               m_ShadowBias;
        int                               m_ShadowDebugMode = 0;     // ShadowDebugMode (Off/ShadowFactor/Cascades)
        float                             m_SplitLambda     = 0.6f;  // cascade split uniform<->log blend
        float                             m_SunSourceAngle  = 0.5357f; // degrees, ViewShadowSettings

        // Debug visualization (Scene Settings -> Debug)
        bool m_ShowNormals   = false; // per-pixel normal color (Lit shader branch)
        bool m_LightingDebug = false; // per-light colored "where light lands" (Lit shader branch)
#if DESERT_DEV_INSTRUMENTS
        bool      m_ShowBoundingBoxes    = false; // AABB wireframes via the debug line renderer below
        glm::vec3 m_BoundingBoxColor     = glm::vec3( 0.25f, 0.95f, 0.35f );
        float     m_BoundingBoxLineWidth = 1.5f;
        bool      m_ShowCardPlacement    = false; // card wireframes, same pipeline (SetCardPlacementView)

        // Debug line renderer (AABB wireframes): Lines-topology pipeline + storage-buffer line verts.
        std::shared_ptr<GraphicsPipeline>  m_DebugLinePipeline;
        std::shared_ptr<Shader>            m_DebugLineShader;
        std::unique_ptr<MaterialDebugLine> m_DebugLineMaterial;
        // The vertices DebugLinesPass's Declare uploaded this frame (0 = the pass draws nothing).
        uint32_t m_DebugLineVertices = 0;

        // Overdraw view: geometry accumulation (additive, no depth) into m_OverdrawFB, then a fullscreen
        // resolve that heat-maps the count over the scene colour. Static/generic meshes only (skinned skipped).
        std::shared_ptr<GraphicsPipeline>        m_OverdrawPipeline;
        std::shared_ptr<Shader>                  m_OverdrawShader;
        std::unique_ptr<MaterialOverdraw>        m_OverdrawMaterial;
        std::shared_ptr<Framebuffer>             m_OverdrawFB;
        bool                                     m_OverdrawRefused = false; // EnsureOverdrawPass said it once
        std::shared_ptr<GraphicsPipeline>        m_OverdrawResolvePipeline;
        // The resolve block's layout, keyed on m_OverdrawResolvePipeline's shader.
        ShaderBindingLayoutCache                 m_OverdrawResolveLayout;
        std::shared_ptr<Shader>                  m_OverdrawResolveShader;
        std::unique_ptr<MaterialOverdrawResolve> m_OverdrawResolveMaterial;
#endif // DESERT_DEV_INSTRUMENTS

        std::vector<StaticMeshRenderData>  m_StaticQueue;
        // The retained prefix of m_StaticQueue (SubmitStaticScene): entries [0, m_StaticRetained) are the
        // admitted sections of m_RetainedChunks, entry i filled from m_RetainedAdmissions[i]. ClearQueues keeps
        // them and drops the per-frame suffix; a view that replays no static scene drops them (SyncGpuScene).
        std::size_t                                                  m_StaticRetained = 0;
        std::vector<Render::StaticSceneAdmission>                    m_RetainedAdmissions;
        uint64_t m_RetainedAdmissionsRevision = 0; // the revision m_RetainedAdmissions were taken at
        std::vector<std::shared_ptr<const Render::StaticSceneChunk>> m_RetainedChunks; // held: never reused
        std::vector<StaticMeshRenderData>                            m_StaticSuffixScratch;
        // Retained entries ApplyDisplacement swapped to a baked / twin mesh this view: put back by ClearQueues.
        std::vector<uint32_t> m_StaticRewritten;
        // Names the retained prefix's contents as last queued (GpuSceneRegistry::SyncStatic's token): a new one
        // per change of the prefix, a rewrite included. m_StaticColumnsToken: the token the prefix's walk
        // columns and rows were built for.
        uint64_t m_StaticToken          = 0;
        uint64_t m_StaticColumnsToken   = 0;
        // THE RETAINED PREFIX'S MATERIAL ROWS, by token: its distinct (surface, instance, receive) keys and the
        // row each resolved to when m_StaticRowsToken's prefix was registered. A frame that keeps the prefix
        // claims these keys (revisions still checked) instead of one RegisterMaterialRow per queued section;
        // a key that resolves to another row re-registers the whole prefix.
        struct StaticPrefixRow
        {
            DataDrivenMaterial* Surface  = nullptr;
            MaterialInstance*   Instance = nullptr;
            bool                Receive  = true;
            uint32_t            Index    = 0;
        };
        std::vector<StaticPrefixRow> m_StaticPrefixRows;
        uint64_t                     m_StaticRowsToken = 0;
        // The prefix's primitives folded in queue order (TrackStaticCasters), valid for m_StaticOrderToken.
        uint64_t m_StaticPrefixOrder = 0;
        uint64_t m_StaticOrderToken  = 0;
        bool     m_StaticSceneSubmitted = false; // SubmitStaticScene ran since ClearQueues
        void FillRetainedStatic( StaticMeshRenderData& data, const Render::StaticSceneAdmission& admitted ) const;
        void DropRetainedStatic();
        void NoteStaticRewritten( std::size_t index );
        void RestoreRewrittenStatic();
        std::vector<SkinnedMeshRenderData> m_SkinnedQueue;

        // One morphed twin per skinned COMPONENT drawn with non-zero weights (UE: the component's
        // FMorphVertexBuffer), keyed by the entity (id incl. version). Revision is the MorphComponent revision
        // the vertices hold (UE FMorphVertexBufferPool revision number); Pending carries the weights the next
        // UpdateMorphTargets blends. A twin not drawn for kMorphTwinIdleFrames frames is released.
        struct MorphTwin
        {
            SkinnedMesh*                  Base = nullptr;
            std::shared_ptr<SkinnedMesh>  Twin;
            std::shared_ptr<VertexBuffer> Vertices;
            // The base content the twin blends from: its render revision and its geometry (held, so its span
            // and buffers outlive the twin's reads; the binding is part of the identity, a re-upload into a new
            // span is a new twin).
            uint64_t                      BaseRevision = 0;
            MeshGeometry                  BaseGeometry;
            bool                          Blended  = false;
            uint64_t                      Revision = 0;
            bool                          Pending  = false;
            std::vector<float>            PendingWeights;
            uint64_t                      PendingRevision = 0;
            uint64_t                      LastDrawn       = 0;
            RDG::ExternalBuffer           BaseImport;
            RDG::ExternalBuffer           DeltasImport;
            RDG::ExternalBuffer           OutputImport;
        };
        static constexpr uint64_t               kMorphTwinIdleFrames = 8;
        std::unordered_map<uint32_t, MorphTwin> m_MorphTwins;
        uint64_t                                m_MorphFrame = 0;
        std::shared_ptr<ComputePipeline>        m_MorphPipeline;
        ShaderBindingLayoutCache                m_MorphLayout;
        // The mesh a skinned draw skins from: the entity's morphed twin when its weights are non-zero, else
        // the base mesh. Records the blend UpdateMorphTargets owes when the revision moved.
        SkinnedMesh* MorphedMeshFor( const MeshRenderData& data, SkinnedMesh* base );

        // Generic (data-driven) static meshes + a cache of the materials that record them. Per-object data
        // is the transform push constant plus a ROW of the material's `Materials[]` buffer, named by a
        // second push constant — so any number of objects share one material and still carry their own
        // parameter values.
        //
        // KEYED BY SHADER **AND TEXTURE SET** (see GenericTextureKey). It was keyed by shader alone, which
        // was the whole defect while parameters lived in the material too; textures still live there, and a
        // key that ignored them would batch two entities into one descriptor set and give both the last
        // one's textures.
        // A record holds no heap member (its names and values are referenced: GenericMeshRenderData), so a steady
        // frame's push_back reuses the queue's capacity and allocates nothing.
        std::vector<GenericMeshRenderData>                              m_GenericQueue;
        std::unordered_map<std::string, std::unique_ptr<DataDrivenMaterial>> m_GenericMaterials;

        // AL1-12: which material pipelines this renderer asked for and what they are doing; how far it has
        // read the process-wide on-load request list; the stand-in every not-yet-ready material draws with.
        MaterialPipelineTracker             m_MaterialPipelines;
        size_t                              m_MaterialRequestCursor = 0;
        size_t                                             m_CellRequestCursor     = 0;
        std::vector<MaterialPipelineRequests::CellRequest> m_PendingCellPrecache;
        uint32_t m_StandInDraws = 0; // see GetStandInDraws; reset per frame in SyncGpuScene
        std::unique_ptr<DataDrivenMaterial> m_DefaultSurfaceMaterial;

        // AL1-12 INSIDE THE GRAPH (RDG-PSO). A graph pass records only pipelines it can bind: binding a content
        // cell's pipeline still in the driver fails the pass, and a failed pass drops the WHOLE frame graph. So
        // every mesh pass asks this before it records a content cell through @p passState: Own when the cell's
        // pipeline (with its cull permutation) is Ready; DefaultSurface when @p standIn — the pass's default
        // surface — is Ready to draw it instead; Nothing while neither is (before the reveal). The stand-in is
        // announced once per cell per state (LOG_INFO), as DrawGenericMeshes does.
        [[nodiscard]] MaterialPipelineTracker::CellDraw ChooseCellDraw( GraphicsPipeline*         passState,
                                                                        const DataDrivenMaterial& cell,
                                                                        bool                      twoSided,
                                                                        const GraphicsPipeline*   standIn );
        // A cell whose own pipeline was refused (null): what names it in the tracker (CellKey) and the log.
        struct RefusedCell
        {
            const GraphicsPipeline*   PassState = nullptr;
            const DataDrivenMaterial* Cell      = nullptr;
            bool                      TwoSided  = false;
        };
        // The same choice for a pipeline the caller already holds (the glass pass's per-cell pipelines); @p own
        // itself is its tracker key (CellKey::Pipeline), @p refused only when @p own is null ({} else).
        [[nodiscard]] MaterialPipelineTracker::CellDraw ChoosePipelineDraw( const GraphicsPipeline* own,
                                                                            const GraphicsPipeline* standIn,
                                                                            const RefusedCell&      refused );
        // The log line's name of a cell: its pipeline's DebugName, or "<pass state> <cell shader>[_TwoSided]" when
        // refused. Built only when a line is logged, never per draw.
        [[nodiscard]] static std::string CellLogName( const GraphicsPipeline* own, const RefusedCell& refused );
        // The default surface's cell of (@p path x @p pass) as a RECORDING material of its own: its Materials[]
        // rows, per-frame blocks and (skinned) bones are written once per pass by the objects standing in, never
        // shared with the renderer's spare materials, whose buffers other groups fill in the same pass.
        struct StandInCell
        {
            std::shared_ptr<DataDrivenMaterial> Material;
            MaterialInstancePtr                 Instance;
        };
        StandInCell*                              StandIn( MeshVertexPath path, MeshPass pass );
        std::unordered_map<uint32_t, StandInCell> m_StandIns; // key: path * 16 + pass; Material null = refused
        // Under m_ForwardVirtualLocal, the DESERT_LOCAL_SHADOW_VSM twin of the renderer-built forward cell
        // @p source (MeshECSSystem's default, a stand-in, the instanced spare): a material and an instance of
        // its own, ONE PER SOURCE (the default and the stand-in share a cell and must not share Materials[]), as
        // a `.demat`'s twin is MaterialService::GetVariant's. Null when @p source is drawn as itself (no virtual
        // shadow map this frame, or its program samples no local shadow atlas); Material null = refused (said
        // once). UE: the default material's shader map holds every permutation the same way.
        StandInCell* ForwardVsmTwin( const DataDrivenMaterial& source );
        struct ForwardVsmTwinSlot
        {
            std::string                Shader; // the source's cell when the slot was built
            std::optional<StandInCell> Twin;   // empty = drawn as itself
        };
        std::unordered_map<const DataDrivenMaterial*, ForwardVsmTwinSlot> m_ForwardVsmTwins;
        // The default surface's scene material table rows (the (Static x Forward) stand-in's), [0] receiving sun
        // shadows and [1] not (SyncGpuScene): the row any draw names while it stands in for a material whose cell
        // is not ready (static, instanced, skinned and glass stand-ins and the instanced spare all show the default
        // surface's defaults; every cell of one template declares one Materials layout).
        std::array<uint32_t, 2> m_StandInMaterialRows{};
        // The generic path's stand-in row: m_DefaultSurfaceMaterial's own (SyncGpuScene, whenever m_GenericQueue
        // is not empty), what a generic draw names while its pipeline compiles (BuildGenericDraws).
        std::optional<uint32_t> m_GenericStandInRow;

        // THE SCENE MATERIAL TABLE, the one source of every mesh draw's Materials[] row
        // (GpuScene::AddMaterialRow). SyncGpuScene registers the row of every (template, instance) a queued draw
        // shades with BEFORE the scene's upload, so the table is at its final size and content before any pass is
        // declared; the draw paths only look rows up and lend the table to the recording cell. A row counts for
        // the SyncGpuScene that claimed it (Sync == m_MaterialRowSync); the nodes are kept across syncs (no
        // per-frame map rebuild), and a key no sync claimed since the previous one is dropped at the next
        // SyncGpuScene. A row index names no surface by itself (each template counts rows in its own stride):
        // every record carries its RowSurface next to its row.
        struct FrameMaterialRow
        {
            uint32_t Index = 0;
            uint64_t Sync  = 0; // the SyncGpuScene (m_MaterialRowSync) that registered Index
        };
        std::unordered_map<GpuDriven::MaterialRowKey, FrameMaterialRow, GpuDriven::MaterialRowKeyHash>
                 m_FrameMaterialRows;
        uint64_t m_MaterialRowSync = 0;
        // @p key's entry for this SyncGpuScene and whether this sync had not claimed it before (its Index is then
        // the caller's to set). Starts the sync's registrations: BeginMaterialRowSync.
        std::pair<FrameMaterialRow*, bool> ClaimFrameMaterialRow( const GpuDriven::MaterialRowKey& key );
        void                               BeginMaterialRowSync();
        // What the DrawCommandCache holds per primitive (m_BinnedFeed) and what this sync gathered (scratch): a
        // primitive is re-added only when its entry differs, removed when no record named it.
        struct BinnedPrimitiveFeed
        {
            const Desert::Mesh*   Mesh = nullptr;
            MeshGeometryBinding   Geometry;     // a re-upload to a new pool range rebins like a mesh change
            std::vector<uint32_t> MaterialRows; // per submesh
            // Per submesh: the (template, instance) its row was registered for. A row index is in units of its
            // own template's stride (GpuScene::AddMaterialRow returns first / slots), so two templates' rows share
            // indices: the row alone does not name a surface (FOLIAGE-TRUNK-GLOW: a bark section resolved to the
            // leaves' FoliageSurface cell and read the leaves' row), and a primitive whose rows keep their numbers
            // while their surfaces change must still be re-added.
            std::vector<RowSurface> Surfaces;
            uint32_t              FirstInstance     = 0;
            uint32_t              InstanceCount     = 1;
            uint64_t              HiddenSubmeshMask = 0;
            uint32_t              Flags             = 0;
            MeshVertexPath        Path              = MeshVertexPath::Static;
            // A generic (m_GenericQueue) draw: it casts through the shared opaque caster whatever its program
            // (MeshRendererShadow.cpp, the generic casters). GenericProgram: one that is not a Surface-template
            // override (a program with no Surface block, a slot material's own program); its view passes are its
            // program's opaque `.Binned` twins (ResolveGenericProgramBinned).
            bool Generic                                        = false;
            bool GenericProgram                                 = false;
            bool operator==( const BinnedPrimitiveFeed& ) const = default;
        };
        std::unique_ptr<GpuDriven::DrawCommandCache> m_DrawCommandCache;
        // This frame's issued (pass, view)s (SetFrameBinned), forgotten by BeginFrameBins.
        struct FrameBinnedView
        {
            GpuDriven::MeshPass             Pass    = GpuDriven::MeshPass::GBuffer;
            uint32_t                        ViewKey = kBinnedMainView;
            MeshRendererDetail::BinnedIssue Issue;
            // Phase 2's issue (SetFramePostOcclusion): Issue with the post-occlusion cull's draws.
            std::optional<MeshRendererDetail::BinnedIssue> PostOcclusion;
            // SetFrameBinned's clusterCut: the view's VG instances are cut candidates, in none of its bins.
            bool ClusterCut = false;
        };
        std::vector<FrameBinnedView> m_FrameBinned;
        // The carriers of the scene blocks a binned group draws with: one material per (cell shader, view), its
        // uniform blocks written once per frame for that view (the camera, or the cascade's light camera).
        struct BinnedCarrier
        {
            std::weak_ptr<Shader>               Program;
            std::unique_ptr<DataDrivenMaterial> Material;
        };
        std::map<std::pair<const Shader*, uint32_t>, BinnedCarrier> m_BinnedCarriers;
        std::string                                                 m_BinnedLastError; // logged once per message
        // The ISM queue's primitives (GpuSceneRecordRows::FieldPrimitives, per m_InstancedQueue entry) the feed
        // saw.
        std::vector<GpuDriven::PrimitiveIndex> m_FieldPrimitives;
        [[nodiscard]] const FrameBinnedView*   FrameBinned( GpuDriven::MeshPass pass, uint32_t viewKey ) const;
        // Attaches (pass, view)'s issue to @p list when it is active this frame.
        void AttachBinned( MeshRendererDetail::MeshDrawList& list, GpuDriven::MeshPass pass,
                           uint32_t viewKey ) const;
        // Per GpuScene primitive index (dense: the scene's rows are reused slots), the cache's entry and this
        // feed's gather. Persistent, so a steady scene allocates nothing per frame (was two maps of per-primitive
        // nodes and row vectors rebuilt and freed by every feed): Frame is this feed's when FrameFeed ==
        // m_BinnedFeedSerial, Held is the cache's when IsHeld; publishing swaps the two (no copy).
        struct BinnedFeedSlot
        {
            BinnedPrimitiveFeed Held;
            BinnedPrimitiveFeed Frame;
            uint64_t            FrameFeed = 0;
            bool                IsHeld    = false;
        };
        std::vector<BinnedFeedSlot>            m_BinnedFeed;
        std::vector<GpuDriven::PrimitiveIndex> m_BinnedFrameTouched; // this feed's primitives, first-claim order
        std::vector<GpuDriven::PrimitiveIndex> m_BinnedHeld;         // every slot with IsHeld
        uint64_t                               m_BinnedFeedSerial = 0;
        // This feed's entry of @p primitive, else the cache's, else null (ResolveBinnedPipeline).
        [[nodiscard]] const BinnedPrimitiveFeed* BinnedFeedOf( GpuDriven::PrimitiveIndex primitive ) const;
        std::unordered_set<std::string>                                    m_BinnedRefused; // said once each
        // The bins' PSOs BinnedCellPipeline found still compiling (their draws answered PipelineUnavailable): the
        // frame one of them leaves the driver, FeedDrawCommandCache re-resolves the cache's draws.
        std::vector<std::weak_ptr<GraphicsPipeline>> m_BinnedAwaiting;
        // What the cache's answers were resolved at: a recompile (Graphic::PipelineRebuildEpoch), a wireframe
        // toggle (the Forward pass state's line twin) or a shadow method change (the VirtualShadow table is
        // resolved only while the sun shadow is the virtual shadow map) moves them, and the feed re-resolves every
        // cached draw.
        uint64_t m_BinnedPipelineEpoch = 0;
        bool     m_BinnedWireframe     = false;
        bool     m_BinnedVirtualShadow = false;
        void FeedDrawCommandCache( const GpuDriven::GpuScene& scene, const GpuDriven::GpuSceneRecordRows& rows );
        // The PSO a submesh of @p primitive draws with in @p pass from material row @p materialRow: the pass's
        // shared state with the material's `.Binned` cell (GBuffer / Forward: the template's cell, two-sided as
        // CullPermutation; ShadowCascade / VirtualShadow: ShadowCasterShaderFor's ShadowDepth / VirtualShadowDepth
        // cell, the default surface's for an opaque one, on the cascades' / the VSM's caster state). No pipeline +
        // BinFallback::None = not drawn in the pass (translucent; a surface in DepthPrepass); + a fallback = drawn
        // there on the CPU path for that reason (no cell, bound textures, a movable caster in the VSM table).
        GpuDriven::PassPipelineResolution ResolveBinnedPipeline( GpuDriven::MeshPass       pass,
                                                                 GpuDriven::PrimitiveIndex primitive,
                                                                 uint32_t submesh, uint32_t materialRow );
        const GraphicsPipeline*           BinnedCellPipeline( const std::string&      cellShader,
                                                              const GraphicsPipeline* passState, bool twoSided );
        // DrawCommandCache's GroupPipelineResolver: @p group's PSO with the `.VirtualGeometry` permutation of its
        // Static / Instanced `.Binned` cell (GBuffer / Forward / ShadowCascade / VirtualShadow), null when it
        // declares none (DeclaresVirtualGeometryTwin: another path, a translucent template) or no pipeline could
        // be made.
        const GraphicsPipeline* ResolveVirtualGeometryPipeline( GpuDriven::MeshPass     pass,
                                                                const GraphicsPipeline& group );
        // THE ONE RULE "this binned cell's template has a `.VirtualGeometry` twin": the twin cell
        // (VirtualGeometryCellOf) is registered. ShaderService registers every pass a template's parse emits when
        // the template registers (an uncompiled one too), and DShaderParser emits the twin for every
        // non-translucent template; so a missing name is a template WITHOUT the permutation (a fallback), and a
        // registered one with no pipeline is a refusal.
        [[nodiscard]] static bool DeclaresVirtualGeometryTwin( const std::string& binnedCell );
        // The view's carrier of @p shader's scene blocks (m_BinnedCarriers), written for this frame; null + @p
        // error when it has no material table.
        const MaterialExecutor* PrepareBinnedCarrier( const std::shared_ptr<Shader>& shader, uint32_t viewKey,
                                                      const SceneFrameBinding& frameState, std::string& error );
        // A generic program's (BinnedPrimitiveFeed::GenericProgram) answer for @p pass: Forward / DepthPrepass
        // through `<program>/Forward.Binned` / `<program>/DepthNormal.Binned` with the CPU list's state
        // (GenericPipelineSpec); a blended or depth-less program is not binned (sorted per view).
        GpuDriven::PassPipelineResolution ResolveGenericProgramBinned( GpuDriven::MeshPass pass,
                                                                       const RowSurface&   row );
        // The registered `.Binned` cell, or null (named once).
        std::shared_ptr<Shader> BinnedCellShader( const std::string& cellShader );
        // @p spec's PSO (two-sided: CullMode None) when Ready; null while it compiles (re-resolved the frame it
        // leaves the driver) or when it failed (named once).
        const GraphicsPipeline*         BinnedSpecPipeline( const std::string&            cellShader,
                                                            GraphicsPipelineSpecification spec, bool twoSided );
        std::unordered_set<const void*> m_MissingMaterialRowReported; // a template whose miss was said once
        // The row of (@p material, @p instance) with ReceiveSunShadows zeroed unless @p receiveSunShadows: added
        // or refreshed in @p registry the first time this frame asks, then the frame's index.
        uint32_t RegisterMaterialRow( GpuDriven::GpuSceneRegistry& registry, const DataDrivenMaterial* material,
                                      MaterialInstance* instance, bool receiveSunShadows );
        // The row SyncGpuScene registered for the key; nullopt (said once per template) when it did not, and the
        // caller does not record the draw: a row looked up after the upload would not be on the device.
        [[nodiscard]] std::optional<uint32_t> FindMaterialRow( const DataDrivenMaterial* material,
                                                               const MaterialInstance*   instance,
                                                               bool                      receiveSunShadows = true );
        // Lends the scene material table to @p material's Materials block (a template with no parameters declares
        // none: true, nothing to lend). False, said by name, when the table is not on the device.
        [[nodiscard]] bool LendMaterialTable( DataDrivenMaterial& material ) const;
        // Every m_GenericQueue record's row (GenericMeshRenderData::MaterialRow) and m_GenericStandInRow, registered
        // in @p registry: called by SyncGpuScene before the upload.
        void RegisterGenericRows( GpuDriven::GpuSceneRegistry& registry );
        // The glass pass's stand-in: the default surface's (Static x Forward) cell with the translucent draw
        // state.
        TranslucentDraw* TranslucentStandIn();

        // UE-style Instanced Static Meshes (one entity = N instances). Folded into the shared instanced
        // pipeline/SSBO alongside the auto-batched static meshes (geometry + shadow passes).
        std::vector<InstancedMeshRenderData> m_InstancedQueue;
        // ApplyDisplacement's split of a displaced ISM record by the twin each instance draws (UE keeps a
        // primitive's cached draw commands until its proxy or the resources they name change): derived from the
        // record's transform snapshot and the cache's Find() answers, so it is kept while the record names the
        // SAME snapshot object (held here, so its address cannot be reused while compared) and the cache's twin
        // generation is unchanged. Groups in first-seen twin order; a null Twin is the base's undisplaced
        // instances; no group = no instance draws a twin. A record no view of the last frame split is dropped.
        struct DisplacedSplitKey
        {
            uint32_t Entity          = 0;
            uint64_t HiddenSubmeshes = 0; // the section group: its instances are their own cache entries
            bool     operator==( const DisplacedSplitKey& ) const = default;
        };
        struct DisplacedSplitKeyHash
        {
            [[nodiscard]] std::size_t operator()( const DisplacedSplitKey& key ) const noexcept
            {
                return std::hash<uint64_t>{}( key.HiddenSubmeshes * 0x9E3779B97F4A7C15ull ^ key.Entity );
            }
        };
        struct DisplacedSplitGroup
        {
            std::shared_ptr<Desert::StaticMesh>           Twin; // null = the base mesh
            std::shared_ptr<const std::vector<glm::mat4>> Transforms;
            std::shared_ptr<const std::vector<uint32_t>>  InstanceIds;
        };
        struct DisplacedSplit
        {
            std::shared_ptr<const std::vector<glm::mat4>> Snapshot;
            uint64_t                                      TwinGeneration = 0;
            uint64_t                                      Frame          = 0;
            std::vector<DisplacedSplitGroup>              Groups;
        };
        std::unordered_map<DisplacedSplitKey, DisplacedSplit, DisplacedSplitKeyHash> m_DisplacedSplits;
        uint64_t                                                                     m_DisplacedSplitFrame = 0;
        std::vector<InstancedMeshRenderData> m_DisplacedTwinRecords;  // scratch: the twins' records, capacity kept
        std::vector<uint32_t>                m_OtherGpuInstances;     // see GetOtherGpuInstances
        std::vector<GpuInstancedRenderData>  m_GpuInstancedQueue;
        // SCENE-CACHE-2, THE CACHED DRAW RECORD of a GPU-instanced section (UE FCachedMeshDrawCommandInfo: the
        // draw command is built when the primitive is added or its material changes, and a frame only picks the
        // visible ones). What BuildGpuInstancedDraws and GrassVisibilityReady resolved for one (section Key x pass
        // x phase x early-Z half x prepassed) - the cell copy, its pipelines through the carriers, the draw's own
        // instance and the cell's buffer slots - kept across frames and resolved again only when its revision
        // moved:
        //   - MaterialService::GetInvalidationVersion (every Invalidate, a material load landing, a live edit,
        //     a shader swap, MaterialService::Clear): the whole cache goes (m_GpuInstancedStamp), its cells may be
        //     in the graveyard;
        //   - the producer's section: another MaterialInstance (Source, owner-compared, so a freed one's address
        //     reused is still another), another vertex path, a camera list appearing or going (the ids
        //     permutation);
        //   - the pass's carriers (the instanced pipeline, the companion's pass and state, the EQUAL G-buffer
        //   state);
        //   - the forward VSM choice (m_ForwardVirtualLocal, ForwardVirtualCell) and the early-Z mode
        //   (EarlyZAdmits).
        // Still asked EVERY frame (they move without a revision or are this frame's): the frustum, the producer's
        // RDG refs and buffers (re-set through the kept slots: StorageBufferProperty::SetBuffer of the same buffer
        // is no write), the material row (SyncGpuScene claims rows per sync), AwaitsTextures, each pipeline's
        // readiness (a compile finishing is not a revision), the GRASS-VIS and prepass sets, frameState.ApplyTo.
        struct GpuInstancedDrawRecord
        {
            std::weak_ptr<MaterialInstance>       Source;
            MeshVertexPath                        Path             = MeshVertexPath::Instanced;
            bool                                  CameraList       = false;
            GraphicsPipeline*                     Carrier          = nullptr;
            GraphicsPipeline*                     CompanionCarrier = nullptr;
            MeshPass                              CompanionPass    = MeshPass::Forward;
            GraphicsPipeline*                     EqualCarrier     = nullptr;
            bool                                  ForwardVirtual   = false;
            ::Common::Scalability::EarlyZPassMode EarlyZ           = ::Common::Scalability::EarlyZPassMode::All;
            bool DrawRows = false; // DrawBinsOf: the cells are the bins' (SurfaceDrawRowsVariant)
            // Resolved under the revision above.
            const DataDrivenMaterial* Parent            = nullptr;
            DataDrivenMaterial*       Cell              = nullptr; // null: refused (said once)
            GraphicsPipeline*         Pipeline          = nullptr;
            GraphicsPipeline*         EqualPipeline     = nullptr; // the masked prepass's G-buffer half
            bool                      EqualRefused      = false;   // ... its permutation was refused (said once)
            GraphicsPipeline*         CompanionPipeline = nullptr; // the forward prepass pairing's other half
            bool                      Admitted          = true;    // EarlyZAdmits( Parent ) under EarlyZ
            MaterialInstancePtr       Instance;                    // the draw's own instance of Cell
            StorageBufferProperty*    InstancesBlock = nullptr;
            StorageBufferProperty*    IdsBlock       = nullptr;
            StorageBufferProperty*    MeshRowsBlock  = nullptr; // GRASS-VIS-MESH-1: GrassMeshVisibility
            StorageBufferProperty*    BladeBlock     = nullptr;
            StorageBufferProperty*    MaterialBlock  = nullptr; // null: the cell reads no Materials row block
            // THE SPLIT, revised apart from the cells (none depends on it, so a lens that moves it re-derives only
            // these): the section's buckets (a mesh's LOD levels, a blade's strip levels) and how many of them the
            // deferred early-Z pair draws (GpuInstancedRenderData::PrepassBuckets, clamped); the G-buffer draws
            // the rest through the section's own record. UE FCachedMeshDrawCommandInfo: the cached command carries
            // what decides the passes it is drawn in.
            uint32_t Buckets        = 0;
            uint32_t PrepassBuckets = 0;
        };
        // RENDER-JOB-CPU-4: the phase table @p gpu's draws in @p pass enter (GpuInstancedRenderData::DrawBins), or
        // null: Debug.Grass "per-variety draws" (m_GrassPerVarietyDraws), a device without drawIndirectCount or
        // shaderDrawParameters, a mesh section (one mesh per variety: no gain until a shared geometry pool), the
        // resolve (one triangle per section), a forward view under virtual local shadows (ForwardVirtualCell's
        // permutation), or a producer without the table or the records' address.
        [[nodiscard]] GpuDrawBinFrame* DrawBinsOf( const GpuInstancedRenderData& gpu, MeshPass pass,
                                                   bool postOcclusion ) const;
        // GRASS-SHADOW-BINS-1: the cascades' table @p gpu's casters enter (GpuInstancedRenderData::DrawBins[2],
        // BuildGpuInstancedShadowDraws: one RenderMeshIndirectCount per (cascade x layer x bin)), or null on
        // DrawBinsOf's terms (per-variety draws, the device, a mesh section, no Open table).
        [[nodiscard]] GpuDrawBinFrame* CascadeBinsOf( const GpuInstancedRenderData& gpu ) const;
        // The count line's last words per (cascade x layer) list (BuildGpuInstancedShadowDraws): said on change.
        std::unordered_map<uint32_t, uint64_t>               m_GrassCasterBinsSaid;
        bool m_GrassPerVarietyDraws = false;
        std::unordered_map<uint64_t, GpuInstancedDrawRecord> m_GpuInstancedDraws;
        uint32_t                                             m_GpuInstancedStamp = 0;
        // The record of @p gpu in @p pass through @p carrier (BuildGpuInstancedDraws' arguments; @p prepassed: the
        // G-buffer draws the section EQUAL after the masked prepass drew it), resolved again when its revision
        // moved. Null without a material service.
        [[nodiscard]] GpuInstancedDrawRecord* GpuInstancedDrawOf( const GpuInstancedRenderData& gpu, MeshPass pass,
                                                                  GraphicsPipeline*      carrier,
                                                                  const StaticPassCells* companion,
                                                                  bool postOcclusion, bool prepassed );
        // MeshRendererGpuInstanced.cpp: the GPU-instanced sections into a geometry list (the pass's instanced
        // cell) and into a cascade's movable list (their own masked caster, or the shared instanced caster).
        // @p companion: the other half of the forward prepass pairing (PairsWithPrepass) - the prepass draws a
        // section only when its cell is Ready in both passes and records it; the base pass draws what was recorded.
        // @p postOcclusion (GRASS-VIS phase 2): only the sections with a camera list, by their phase-2 records.
        // @p onlyPath: only the sections of that vertex path (the two visibility resolves' own GPU lines).
        void BuildGpuInstancedDraws( MeshRendererDetail::MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                     MeshPass pass, const SceneFrameBinding& frameState,
                                     const StaticPassCells* companion, bool postOcclusion,
                                     std::optional<MeshVertexPath> onlyPath = std::nullopt );
        // GRASS-VIS: @p gpu's (GrassBlade x Visibility) and (GrassBlade x VisibilityResolve) cells, and their pipelines
        // through the two carriers, all Ready - the section can leave the G-buffer this frame.
        [[nodiscard]] bool GrassVisibilityReady( const GpuInstancedRenderData& gpu );
        // `cascade` keys the caster cell (one per section x cascade); `shadowView` is the view drawn (the cascade
        // itself, or its movable layer kCascadeMovableViewBase + cascade over the cache) and names its light
        // camera. @p layer is the cascade cache's layer drawn (All: an uncached cascade). True when a still
        // section (GpuInstancedRenderData::Still) the layer draws was left out for a pending reason (its textures,
        // its caster pipeline compiling): a static layer drawn without it must not be kept.
        // THE BLADE'S SHADOW LOD IN A CASCADE (GPU-COST-20; UE: a shadow view picks its own LOD by the caster's
        // size in that view, r.Shadow.LODDistanceFactor / the shadow LOD bias): the record set a GrassBlade
        // section draws into cascade view @p cascade (GpuInstances.hpp - the coarsest strip level that still gives
        // a segment per kBladeShadowTexelsPerSegment texels of the blade's height, never finer than the camera's
        // level), and the distance the caster thins the field as if it stood at (Vertex_GrassBlade.glslh: the
        // generator's (S / d)^2 at max(d, ThinFrom) - where one of the view's pixels is as wide as the cascade's
        // texel). Both read the texel widened by Shadows.FoliageShadowLodBias. Set 0 and ThinFrom 0 for a section
        // without a shadow LOD (a mesh, a view without a fitted cascade): drawn as the camera draws it.
        struct BladeCasterLod
        {
            uint32_t Set      = 0;
            float    ThinFrom = 0.0f; // cm; 0: no thinning
            [[nodiscard]] bool operator==( const BladeCasterLod& ) const = default;
        };
        [[nodiscard]] BladeCasterLod BladeShadowSet( const GpuInstancedRenderData& gpu, uint32_t cascade ) const;
        [[nodiscard]] bool BuildGpuInstancedShadowDraws( uint32_t cascade, uint32_t shadowView,
                                                         ShadowCasterLayer                 layer,
                                                         MeshRendererDetail::MeshDrawList& list );

    private:
        // m_StaticMaterialFallback and m_SkinnedMaterialFallback stood here. The first was constructed
        // every Initialize() and read by nothing; the second was never even constructed. A mesh with no
        // resolvable slot gets its fallback from MeshECSSystem, which is the one place that knows a slot
        // failed to resolve — two fallbacks meant two answers to one question and only one of them ran.

        // ── Per-frame scratch (memory discipline: reuse, don't reallocate) ──────────────────
        // Cleared each use; capacity persists across frames so the steady state allocates nothing.

        // One draw-ready record per object: the effective material is built ONCE per object per
        // frame and reused for the glass split, the batch entry and the per-object SSBO.
        struct ObjDraw
        {
            const StaticMeshRenderData* Obj  = nullptr;
            MaterialInstance*           Inst = nullptr;
            bool                        HasOverrides = false;
            // A single draw's plan (BuildStaticDraws' planGroup): the mask it was bucketed by and, for a single,
            // its LOD level - decided on the setup task, replayed by the call that records it.
            SubmeshMask Hidden{};
            uint32_t    Level = 0;
        };
        struct InstancedDraw
        {
            // Fed by BOTH the auto-batched statics (a StaticMesh) and the ISM queue (any Mesh), so it
            // is the wider of the two — a draw call only ever needs what Mesh already carries.
            Desert::Mesh*       Mesh          = nullptr;
            uint32_t            InstanceCount = 0;
            uint32_t            FirstInstance = 0;
            uint32_t            MaterialIndex = 0;
            SubmeshMask HiddenSubmeshMask{}; // the batch's section group (one mask for every instance; a VG
                                             // entry's walk adds the submeshes its cluster raster draws)
            // A draw call carries ONE index range, so a batch carries ONE level. Before this field the
            // batched path drew at the default 0 while the per-object path next to it computed a level
            // and passed it — the same object at two detail levels depending on whether it batched.
            uint32_t LodLevel = 0;
            // Pushed with the draw, zeros for everything but a swaying foliage field (FO-7): the push
            // block keeps its bytes between draws, so an auto-batched wall must push its own stillness.
            InstanceWindPush Wind;
            // The push PrimitiveFlags (Common/MaterialTransport.glslh) every instance of the draw shares: a batch
            // only holds objects that agree on them. Pushed with every draw, for the reason Wind is.
            uint32_t PrimitiveFlags = 0;
        };

        // EVERY INSTANCED DRAW THAT ONE MATERIAL RECORDS, plus the two buffers those draws read.
        //
        // There is one of these per distinct recording material in the pass, and that count is the
        // change: the accumulation used to be a single triple shared by every batch of every material,
        // which is only expressible because every batch was recorded by the same renderer-owned material
        // — the thing that cost every batched surface its textures. A set's Materials[] rows are named by
        // a push constant (snapshotted per draw), so many batches of ONE material still share one buffer
        // and one upload; two MATERIALS cannot, because a descriptor set written twice in a frame keeps
        // only the last write (VulkanMaterialBackend::ApplyTexture2D reports the swallowed rebind).
        struct InstancedBatchSet
        {
            Graphic::DataDrivenMaterial* Mat  = nullptr;
            MaterialInstance*           Inst = nullptr;
            std::vector<glm::mat4>      Transforms;
            // CPU-ISM-PACK: the recorder is an ISM's cell by GpuScene id (SurfaceSceneInstancesVariant): the
            // draws' instances are ranges of the VIEW's id buffer (PrepareViewIsmIds, lent to the cell's
            // InstanceTransforms in place of a per-set upload); Transforms stays empty.
            bool                        SceneIds = false;
            std::vector<InstancedDraw>  Draws;
            // A Resolve call (DrawBuildPhase): the ISMs' draws, which the Record call puts behind the auto-batched
            // ones (the Full call's draw order), and the recorder's pipelines and its Materials lend. Their
            // instances go straight into Transforms, AHEAD of the ones the Record call packs (each draw names its
            // own slice, so only the draw order is the Full call's; nothing is copied twice - AUDIT-CPU-3 G2).
            std::vector<InstancedDraw> IsmDraws;
            GraphicsPipeline*          OneSided = nullptr; // CellPipeline( instanced state, Mat )
            GraphicsPipeline*          TwoSided = nullptr; // CullPermutation( OneSided, true )
            bool                       Lent     = false;
            // A Resolve call: a group's plan (BuildStaticDraws' planGroup, StaticGroupPlan::Batches) gives this
            // set an auto-batched draw, so its Record call packs one; with no ISM draw either, the set is not
            // prepared.
            bool Drawn = false;
        };
        // @p set's packed transforms into its recorder's InstanceTransforms block, at final size.
        void UploadInstanceTransforms( InstancedBatchSet& set );

        // One recorded generic draw, resolved in BuildGenericDraws' first pass and recorded after every recording
        // material was lent the scene material table. Row: its row in that table (SyncGpuScene).
        struct GenericDraw
        {
            const GenericMeshRenderData*      Data     = nullptr;
            DataDrivenMaterial*               Material = nullptr;
            GraphicsPipeline*                 Pipeline = nullptr; // held by the pipeline cache / m_CellPipelines
            uint32_t                          Row = 0;
        };
        struct ShadowBatch
        {
            Desert::Mesh*       Mesh     = nullptr; ///< see InstancedDraw::Mesh — the same two producers
            uint32_t            Count = 0;
            uint32_t            First = 0;
            uint32_t            LodLevel = 0; ///< see InstancedDraw::LodLevel — same omission, same fix
            SubmeshMask HiddenSubmeshMask{};  ///< see InstancedDraw::HiddenSubmeshMask (a VSM level's is wide)
            InstanceWindPush    Wind;         ///< see InstancedDraw::Wind
            float               BoundsPad = 0.0f; ///< WindBoundsPad of the wind above, cm (MeshDrawCommand)
        };

        // Every skinned pose drawn in one pass, packed end to end; each draw names its slice with a
        // BoneOffset push constant. ONE buffer per material per pass instead of one upload per draw,
        // which is what makes a shared skinned material correct — see Material::SetSkinnedBoneOffset.
        std::vector<glm::mat4>                   m_ScratchBones;
        // The sun cascade's opaque skinned casters of one cascade (RenderShadowCascade), kept for capacity:
        // the opaque sections of an entry are ONE depth draw over one copy of its pose.
        struct OpaqueSkinnedCaster
        {
            const SkinnedMeshRenderData* Data       = nullptr;
            uint32_t                     BoneOffset = 0;
            uint64_t                     Hidden     = 0;
            std::size_t                  Index      = 0; // into m_SkinnedQueue
        };
        std::vector<OpaqueSkinnedCaster> m_ScratchOpaqueSkinnedCasters;
        std::vector<glm::mat4>      m_ScratchInstTransforms; // geometry + shadow instanced SSBOs
        // Per-batch LOD levels and the surviving ISM transforms. Members rather than locals for the
        // reason every accumulator in this file is one: their capacity persists, so a frame that culls
        // 49 000 instances out of 49 152 allocates nothing.
        std::vector<uint32_t>                    m_ScratchLodLevels;
        std::vector<glm::mat4>                   m_ScratchIsmVisible;
        // CPU-ISM-PACK 5: a shadow view's ISM casters by GpuScene id - the visible instances' ids
        // (FirstGpuInstance + index), the opaque id batches and their ids end to end (the view's id material's
        // InstanceTransforms, uploaded once per view). Kept for capacity, like the scratch above.
        std::vector<uint32_t> m_ScratchIsmVisibleIds;
        std::vector<uint32_t> m_ScratchShadowIsmIds;
        // The per-instance caster cull's bits (VisibleInViewBits, one bit per gathered instance of a field): the
        // view's and the scrolled strips' union (CascadeStaticSignature's are its runs', IsmSignatureRun::Bits).
        std::vector<uint64_t> m_ScratchCasterBits;
        std::vector<uint64_t> m_ScratchStripBits;
        // THE MAIN VIEW'S PER-INSTANCE ISM CULL, CACHED PER FIELD (GPU-COST-37; UE FCachedMeshDrawCommandInfo's
        // rule for the one per-instance CPU walk of a view pass: built when its inputs change, reused while they
        // do not). The key is every input CollectIsmInstances reads that is the field's own: its immutable
        // transforms snapshot (MeshECSSystem replaces it on a revision change, never edits it; held, so its
        // address is not reused while the entry lives), its mesh, its cull distance and the wind's bounds pad (the
        // only part of the wind the cull reads). So two ISMs share an entry only when their culls are the same
        // cull (the sections of one field), and a call's culls write disjoint entries (RENDER-PARALLEL-SETUP: on
        // setup tasks). The inputs a call gives are stored by value and compared: the mesh's local box and LOD
        // screen sizes (an edited mesh keeps its address), the view frustum's six planes and the LOD view. Equal
        // inputs give the same instances and levels bit for bit, so a hit draws byte for byte what a rebuild
        // draws. Shared by every pass that walks the main view in the frame (the masked depth prepass, both
        // G-buffer halves, the forward base pass) and by the next frames while nothing moved. An entry no pass
        // asked for during a frame is dropped by ClearQueues (the field left the scene or changed snapshot).
        struct IsmViewCullKey
        {
            const std::vector<glm::mat4>* Transforms = nullptr;
            const Desert::Mesh*           Mesh       = nullptr;
            // InstanceCullDistance Min / Max and WindBoundsPad by bit pattern (std::bit_cast): a NaN is one key
            // too.
            uint32_t CullMinBits                               = 0;
            uint32_t CullMaxBits                               = 0;
            uint32_t PadBits                                   = 0;
            bool     operator==( const IsmViewCullKey& ) const = default;
        };
        struct IsmViewCullKeyHash
        {
            [[nodiscard]] std::size_t operator()( const IsmViewCullKey& key ) const noexcept;
        };
        // One LOD level's slice of a cull entry's level-ordered Visible: [First, First + Count).
        struct IsmLevelRun
        {
            uint32_t Level = 0;
            uint32_t First = 0;
            uint32_t Count = 0;
        };
        struct IsmViewCull
        {
            std::shared_ptr<const std::vector<glm::mat4>> Snapshot; // the key's transforms, held
            Common::Math::AABB                            LocalBounds{};
            std::array<glm::vec4, 6>                      Planes{}; // (normal, distance) per Core::FrustumPlane
            Geometry::LODView                             View{};
            std::vector<float>                            LodScreenSizes;
            // The kept instances ORDERED BY LEVEL, ascending, each level's in the order the cull kept them (a
            // stable partition, OrderIsmViewCullByLevel - once per rebuild, not per pass per frame), with their
            // levels; Runs names each level's contiguous slice, so a pass packs one range per level (AUDIT-CPU-3
            // G2).
            std::vector<glm::mat4>                        Visible;
            std::vector<uint32_t>                         Levels;
            std::vector<IsmLevelRun>                      Runs;
            std::vector<glm::mat4>                        Ordered; // the reorder's scratch, swapped with Visible
            // Per kept instance (Visible's order): its index in the field's transforms - the GpuScene instance
            // FirstGpuInstance + index an ISM drawn by id names (SurfaceSceneInstancesVariant).
            std::vector<uint32_t> Indices;
            std::vector<uint32_t> OrderedIndices; // the reorder's scratch, swapped with Indices
            // How many times its inputs changed (LookUpIsmViewCull, Stale): a view id range is the entry's only
            // while this is the count it was filled from (PrepareViewIsmIds).
            uint64_t                                      Rebuilds   = 0;
            uint64_t                                      Generation = 0; // the last frame a pass asked for it
            uint64_t                                      Call       = 0; // the last call that looked it up
        };
        std::unordered_map<IsmViewCullKey, IsmViewCull, IsmViewCullKeyHash> m_IsmViewCulls;
        static constexpr uint32_t kNoViewIsmIds = ~0u; // an ISM with no view id range (PrepareViewIsmIds)
        uint64_t                                                            m_IsmViewCullGeneration = 0;
        uint64_t m_IsmViewCullCall = 0; // one per Full / Resolve call's ISM loop (BuildStaticDraws)
        // ISM @p ism's entry for the main view @p frustum / @p view, stamped for this frame and call - serial, it
        // may insert. Stale false: every input is the entry's, and its Visible / Levels are the cull
        // (CollectIsmInstances with the view as both the cull and the LOD view), or this call already looked it up
        // (a second section of the field: its inputs are the first's). Stale true: the new inputs are stored, and
        // CollectIsmViewCull fills the entry before it is read - a setup task may, it writes that entry alone - so
        // each entry is filled at most once per call.
        struct IsmViewCullLookup
        {
            IsmViewCull* Entry = nullptr;
            bool         Stale = false;
        };
        [[nodiscard]] IsmViewCullLookup LookUpIsmViewCull( const InstancedMeshRenderData& ism,
                                                           const Core::Frustum&           frustum,
                                                           const Geometry::LODView&       view );
        static void CollectIsmViewCull( IsmViewCull& entry, const InstancedMeshRenderData& ism,
                                        const Core::Frustum& frustum );
        // @p entry's Visible / Levels as a stable partition by level, ascending, and its Runs: what every pass
        // used to do per level per pass (DistinctLODs, then a filter over all the kept instances), done once per
        // rebuild.
        static void                              OrderIsmViewCullByLevel( IsmViewCull& entry ) noexcept;
        uint32_t                                 m_IsmInstancesDrawn = 0; // see GetIsmInstancesDrawn
        // The instanced batches of ONE geometry pass, one entry per recording material. Reused BY INDEX
        // rather than cleared, so the inner vectors keep their capacity across frames — clearing the
        // outer vector would destroy them and hand the steady state an allocation per material per frame.
        // `InstSetCount` is how many of them this call is using. One scratch per concurrent BuildStaticDraws /
        // BuildSkinnedDraws call (m_DrawBuildScratch: [0] the serial builds and the Equal half, [1] the Rest
        // half).
        //
        // INDIRECT, AND THAT IS THE POINT AND NOT AN OVERSIGHT. The accumulation hands out a POINTER to
        // one set and then keeps filling it while later groups may ask for sets of their own; held by
        // value, the first `emplace_back` that grows this vector would move every set and leave that
        // pointer dangling — a use-after-free that no frame would show, because the freed memory is the
        // vector this thread just wrote. The control flow happens not to interleave the two today, which
        // is exactly the kind of "safe for now" that the next edit in DrawStaticMeshes turns into a
        // corrupted draw list. A unique_ptr costs one indirection per access and makes the address
        // stable by construction.
        struct DrawBuildScratch
        {
            std::vector<std::unique_ptr<InstancedBatchSet>> InstSets;
            std::size_t                                     InstSetCount = 0;
            std::vector<uint64_t>                           WalkWords; // CpuWalkHidden's
            // THE BUCKETING OF ONE SLICE OF GROUPS (BuildStaticDraws' planGroup, one setup task per slice): the
            // group's (mesh, hidden mask) buckets, reused by index as InstSets are (BucketCount of them in use),
            // one bucket's batchable objects and their levels. Read by no one past the group's plan.
            using MeshBucket = std::pair<std::pair<Desert::Mesh*, SubmeshMask>, std::vector<ObjDraw>>;
            struct BucketScratch
            {
                std::vector<MeshBucket> Buckets;
                std::size_t             BucketCount = 0;
                std::vector<ObjDraw>    Batchable;
                std::vector<uint32_t>   LodLevels;
            };
            std::vector<BucketScratch> BucketSlices;
            // A Resolve call's plan, which the same half's Record call reads (DrawBuildPhase). WalkGroups: the
            // walk's groups before the half's split (the other half's Resolve takes them, DrawBuildMode::Walk);
            // Groups: this half's (split, the stand-in group appended); GroupPlans: one per Groups entry, reused
            // by index so their vectors keep their capacity.
            using StaticGroup = std::pair<DataDrivenMaterial*, std::vector<const StaticMeshRenderData*>>;
            // One instanced draw of a group's plan: Count objects of Instances from First, all of Mesh at Level.
            struct PlannedBatch
            {
                Desert::Mesh* Mesh = nullptr;
                SubmeshMask   Hidden{};
                uint32_t      Level         = 0;
                uint32_t      MaterialIndex = 0;
                uint32_t      First         = 0;
                uint32_t      Count         = 0;
            };
            // UE FParallelMeshDrawCommandPass: the setup decides, the record replays. The group's bucketing is
            // made ONCE, by the call that plans (Full or Resolve), from one read of the objects' instances: its
            // batches and its singles. A Record call packs and records exactly these and buckets nothing, so the
            // Resolve call's answers (Set->Drawn, DrawMat) are the plan's by construction.
            struct StaticGroupPlan
            {
                InstancedBatchSet*  Set      = nullptr; // where its batches accumulate (setForGroup), or none
                DataDrivenMaterial* DrawMat  = nullptr; // records its singles, lent and applied; null: not drawn
                GraphicsPipeline*   OneSided = nullptr; // CellPipeline( pass state, DrawMat )
                GraphicsPipeline*   TwoSided = nullptr; // CullPermutation( OneSided, true )
                bool                      Opaque   = false;   // not empty, not Translucent: planned at all
                std::vector<PlannedBatch> Batches;            // empty without a Set
                std::vector<const StaticMeshRenderData*> Instances; // the batches' objects, batch after batch
                std::vector<ObjDraw>                     Singles;   // the per-object draws, in bucket order
            };
            // The walk's masks unlike an entry's own HiddenSubmeshes, DENSE BY m_StaticQueue POSITION (UE's
            // per-primitive visibility arrays): At[i] = 0 for the entry's own mask, else 1 + its row in Masks.
            // Reset per planning call (zeroed with its capacity kept), copied whole by the Rest half's Resolve.
            struct WalkedMasks
            {
                std::vector<uint32_t>    At;
                std::vector<SubmeshMask> Masks;
                void                     Reset( const std::size_t records ) noexcept
                {
                    At.assign( records, 0u );
                    Masks.clear();
                }
                void Set( const std::size_t record, const SubmeshMask& mask ) noexcept
                {
                    if ( record >= At.size() )
                        return; // not a record of this queue: its own mask stands
                    Masks.push_back( mask );
                    At[record] = static_cast<uint32_t>( Masks.size() );
                }
            };
            std::vector<StaticGroup>                                     WalkGroups;
            WalkedMasks                                                  Walked;
            std::vector<StaticGroup>                                     Groups;
            std::vector<StaticGroupPlan>                                 GroupPlans;
            bool                                                         Planned = false; // Resolve completed
            // THE GROUPS' LISTS ARE KEPT ACROSS FRAMES (CPU-FRAME-ARENA 5; UE FMemStack): a cleared Groups /
            // WalkGroups entry hands its vector to SpareLists, a new group (or the stand-in list) takes one back,
            // so the per-material lists keep their capacity and a steady frame's walk allocates nothing.
            // Unwalked: the merge's records whose walk mask did not intern (BuildStaticDraws).
            std::vector<std::vector<const StaticMeshRenderData*>> SpareLists;
            std::vector<const StaticMeshRenderData*>              Unwalked;
            void RecycleGroups( std::vector<StaticGroup>& groups ) noexcept
            {
                for ( StaticGroup& group : groups )
                    GiveList( std::move( group.second ) );
                groups.clear();
            }
            void GiveList( std::vector<const StaticMeshRenderData*>&& list ) noexcept
            {
                list.clear();
                SpareLists.push_back( std::move( list ) );
            }
            [[nodiscard]] std::vector<const StaticMeshRenderData*> TakeList() noexcept
            {
                if ( SpareLists.empty() )
                    return {};
                std::vector<const StaticMeshRenderData*> list = std::move( SpareLists.back() );
                SpareLists.pop_back();
                return list;
            }
            // @p into becomes a copy of @p from, its lists taken from SpareLists (assign keeps their capacity).
            void CopyGroups( std::vector<StaticGroup>& into, const std::vector<StaticGroup>& from ) noexcept
            {
                RecycleGroups( into );
                for ( const StaticGroup& group : from )
                {
                    std::vector<const StaticMeshRenderData*> list = TakeList();
                    list.assign( group.second.begin(), group.second.end() );
                    into.emplace_back( group.first, std::move( list ) );
                }
            }
            // THE SKINNED PATH'S GROUPS, kept across calls as the static ones (CPU-FRAME-ARENA 6):
            // BuildSkinnedDraws recycles them on entry, so a group's (or the stand-in's) section list keeps its
            // capacity. The unit is a SECTION of an entry (its own material, masked to its own submeshes).
            struct SkinnedSectionRef
            {
                const SkinnedMeshRenderData* Obj     = nullptr;
                const SkinnedSection*        Section = nullptr;
            };
            using SkinnedGroup = std::pair<DataDrivenMaterial*, std::vector<SkinnedSectionRef>>;
            std::vector<SkinnedGroup>                   SkinnedGroups;
            std::vector<std::vector<SkinnedSectionRef>> SpareSkinnedLists;
            void                                        RecycleSkinnedGroups() noexcept
            {
                for ( SkinnedGroup& group : SkinnedGroups )
                    GiveSkinnedList( std::move( group.second ) );
                SkinnedGroups.clear();
            }
            void GiveSkinnedList( std::vector<SkinnedSectionRef>&& list ) noexcept
            {
                list.clear();
                SpareSkinnedLists.push_back( std::move( list ) );
            }
            [[nodiscard]] std::vector<SkinnedSectionRef> TakeSkinnedList() noexcept
            {
                if ( SpareSkinnedLists.empty() )
                    return {};
                std::vector<SkinnedSectionRef> list = std::move( SpareSkinnedLists.back() );
                SpareSkinnedLists.pop_back();
                return list;
            }
            // ONE CHUNK OF THE STATIC WALK (BuildStaticDraws: one setup task each, WalkStaticChunk), reused by
            // index so its vectors keep their capacity: its groups in first-seen order (GroupCount of Groups in
            // use), the records whose walk mask is not their own HiddenSubmeshes (Masks: the words past submesh 63
            // still to intern, HighWords of them from WordsAt in Words) and its CpuWalkUninterned words.
            struct WalkedMask
            {
                const StaticMeshRenderData* Data      = nullptr;
                uint64_t                    Low       = 0;
                uint32_t                    WordsAt   = 0;
                uint32_t                    HighWords = 0;
            };
            struct StaticWalkChunk
            {
                std::vector<StaticGroup> Groups;
                std::size_t              GroupCount = 0;
                std::vector<WalkedMask>  Masks;
                std::vector<uint64_t>    Words;
                std::vector<uint64_t>    WalkWords;
            };
            std::vector<StaticWalkChunk> WalkChunks;
            // A Full / Resolve call's ISMs past their serial checks, in queue order (IsmQueued: the set and the
            // cull entry each packs from), and the stale entries their culls fill on setup tasks (IsmStale: each
            // once).
            struct QueuedIsm
            {
                const InstancedMeshRenderData* Ism = nullptr;
                SubmeshMask                    Hidden{};
                DataDrivenMaterial*            Mat      = nullptr;
                InstancedBatchSet*             Set      = nullptr;
                const IsmViewCull*             Cull     = nullptr;
                uint32_t                       IdBase   = kNoViewIsmIds; // by id: its view id range's first
                bool                           StandsIn = false;
            };
            struct StaleIsmCull
            {
                IsmViewCull*                   Entry = nullptr;
                const InstancedMeshRenderData* Ism   = nullptr;
            };
            std::vector<QueuedIsm>    IsmQueued;
            std::vector<StaleIsmCull> IsmStale;
        };
        std::array<DrawBuildScratch, 2> m_DrawBuildScratch;
        // Records [first, last) of m_StaticQueue walked in the main view @p view / @p frustum into @p chunk
        // (BuildStaticDraws' static walk): CpuWalkUninterned, the frustum test, the section's surface. A setup
        // task may run it: it reads the queue and the walk's inputs and writes @p chunk alone.
        void WalkStaticChunk( const ClusterWalkView& view, const Core::Frustum& frustum, std::size_t first,
                              std::size_t last, DrawBuildScratch::StaticWalkChunk& chunk ) const;
        // The G-buffer halves' own lists (DeclareGBufferDraws: Equal, Rest statics, Rest skinned), appended to
        // m_GBufferDraws in the old order once the tasks joined. Members so the vectors keep their capacity.
        std::array<MeshRendererDetail::MeshDrawList, 3> m_GBufferHalfDraws;
        std::vector<ShadowBatch>    m_ScratchShadowBatches;
        // CPU-ISM-PACK 6: a shadow view's opaque ISM batches by GpuScene id, each a slice of
        // m_ScratchShadowIsmIds.
        std::vector<ShadowBatch> m_ScratchShadowIdBatches;
        // A per-object caster: its entry and the submeshes it hides (the entry's, plus a VSM level's clustered
        // ones).
        struct ShadowSingle
        {
            const StaticMeshRenderData* Data = nullptr;
            SubmeshMask                 Hidden{};
        };
        std::vector<ShadowSingle> m_ScratchShadowSingles;
        // THIS FRAME'S SHADOW CASTERS (GatherShadowCasters): one row per queued entry, what no view changes.
        static constexpr uint32_t kShadowLodUnknown = ~0u;
        struct ShadowCasterRow
        {
            Common::Math::AABB WorldBounds; // empty: no known extent, in every view (IsVisibleInView's rule)
            uint32_t LodLevel = kShadowLodUnknown; // ComputeLOD from the main camera, on the first view drawing it
            // An instance of a swaying field nearer the main camera than m_ShadowWindDistance: it sways in the
            // shadow passes and is a mover (the VSM's movable layer, a local view redrawn). A farther one casts
            // its rest pose with the still casters (UE WPO Disable Distance for the shadow depths). False for
            // every other row. The box (WorldBounds) is the sway's reach either way. The GPU cull's caster views
            // split the bins by the same function (Common/ShadowWind.glslh, its bounds centre against the camera).
            bool Sways = false;
            // An instance of a swaying field AT the shadow wind distance, within the rounding the CPU walk and the
            // GPU cull may differ by (MeshRendererShadow.cpp kShadowWindEdge): it sways for every CPU view
            // (Sways), and a cached cascade it reaches redraws both layers each frame (CascadeStaticSignature),
            // since the GPU bins may have put it in either.
            bool NearWindEdge = false;
            // A swaying instance's point (ShadowWindInstancePoint) and reach (ShadowWindCasterReach), what the
            // per-cascade ring reads (ShadowWindRingOf, ShadowIsmRingRest): nearer the camera than the ring's
            // inner distance it casts its rest pose in that cascade, as GpuCull_CastsRestPose decides on the GPU.
            // Zero for every other row.
            glm::vec3 WindPoint{ 0.0f };
            glm::vec2 CasterReach{ 0.0f };
        };
        struct ShadowIsmField
        {
            uint64_t Gather = 0; // the gather Instances[First, First + Count) are of; another: not gathered yet
            uint32_t First  = 0;
            uint32_t Count  = 0;
        };
        struct ShadowCasterSetup
        {
            uint64_t                     Gather = 0; // bumped by each GatherShadowCasters
            std::vector<ShadowCasterRow> Statics;    // per m_StaticQueue entry
            std::vector<ShadowCasterRow> Generics;   // per m_GenericQueue entry
            std::vector<ShadowCasterRow> Skinned;    // per m_SkinnedQueue entry: the posed box
            std::vector<ShadowIsmField>  Fields;     // per m_InstancedQueue entry
            // The instances of the gathered fields the main camera's cull distance keeps, end to end: the box the
            // wind can reach and the level, and the instance's index in its field's transforms.
            std::vector<ShadowCasterRow> Instances;
            std::vector<uint32_t>        InstanceIndex;
            // Instances' boxes again as dense columns (CPU-SIMD 2; UE FScene::PrimitiveBounds), pushed with each
            // row: the per-view per-instance culls (CascadeStaticSignature, BuildShadowCascadeDraws) test four
            // boxes per register over them (VisibleInViewBits) instead of one fat row at a time.
            Common::Math::AABBLanes InstanceBounds;
            // Statics' boxes again as dense columns (CPU-SIMD 4), one per row in row order, made with the rows
            // by FinishStaticCasterRows and kept with them (SCENE-CACHE-4: only a moved static revision refills
            // them): the static caster walk of every shadow view (BuildShadowCascadeDraws) tests the four-box
            // group of a record it asks (VisibleInView4) instead of one fat row at a time.
            Common::Math::AABBLanes StaticBounds;
            // THE ISM GATHER IS KEPT ACROSS FRAMES (SCENE-CACHE-3) while every input of GatherIsmCasters is the
            // one it was made from: IsmKey (IsmGatherKey) and the transforms snapshots, held by owner (a freed
            // snapshot whose address came back is another one). IsmRevision moves whenever the gather starts over.
            uint64_t                                                 IsmKey      = 0;
            uint64_t                                                 IsmRevision = 0;
            std::vector<std::weak_ptr<const std::vector<glm::mat4>>> IsmSources; // per m_InstancedQueue entry
            // THE STATIC / GENERIC ROWS ARE KEPT ACROSS FRAMES (SCENE-CACHE-4): finished by FinishStaticCasterRows
            // once this view's records are synced (SyncGpuScene), recomputed only when the static casters'
            // revision (m_StaticCasters) moved; their levels are asked again only when StaticLodKey moved.
            // StaticRowsGather: the gather they were finished for (another: not of these queues yet).
            uint64_t StaticRowsGather   = 0;
            uint64_t StaticRowsRevision = 0;
            uint64_t StaticLodKey       = 0;
        };
        ShadowCasterSetup m_ShadowCasterSetup;
        // THE DENSE STATIC INSTANCE SNAPSHOT (CPU-FRAME-ARENA 4; UE FScene::PrimitiveBounds: the per-view cull
        // walks a dense array indexed by primitive, rebuilt only when a primitive is added, moved, removed or
        // changes how it draws). One world box per m_StaticQueue entry, for EVERY entry with a mesh (a caster
        // or not; IsVisibleInView's box from its transform and its submeshes' local bounds; empty: no extent,
        // in every view), made and kept by FinishStaticCasterRows with the static casters' revision - the
        // shadow rows' caster boxes are copied from it, so the box is built once for all views. Every view walk
        // of the static queue (the main view's chunks, the translucent / glass / front-layer walks) culls by it
        // instead of chasing each entry's mesh and its submeshes per view. Gather: the ShadowCasterSetup::Gather
        // it is of (another: not of this frame's queue - StaticEntryInView builds the entry's box itself).
        // GenericBounds: the same box per m_GenericQueue entry with a mesh (the generic records are members of the
        // same revision: StaticCasterTrack::Member), so the generic walks cull by it too.
        struct StaticInstanceSnapshot
        {
            std::vector<Common::Math::AABB> WorldBounds;   // per m_StaticQueue entry
            std::vector<Common::Math::AABB> GenericBounds; // per m_GenericQueue entry
            // WorldBounds again as dense columns (CPU-SIMD 4; UE FScene::PrimitiveBounds' SoA batch cull), filled
            // in the same pass: the main view's static chunks test four entries per register
            // (StaticEntriesInView4).
            Common::Math::AABBLanes         WorldLanes;
            uint64_t                        Gather = 0;
        };
        StaticInstanceSnapshot m_StaticSnapshot;
        // THE STATIC QUEUE'S HOT COLUMNS (CPU-FRAME-ARENA 14; UE FScene's dense primitive arrays -
        // PrimitiveBounds, PrimitiveVisibilityIds, PrimitiveSceneProxies by packed index): what every per-view
        // walk of m_StaticQueue reads per record, one dense array per field indexed by queue position, so a walk
        // streams a few bytes per record instead of the fat StaticMeshRenderData (its transform, its slot
        // binding's shared_ptr, its flags) and asks no section surface (a dynamic_cast and the template's path)
        // per record per view. Built by SyncGpuScene, once per view sync, AFTER ApplyDisplacement has swapped any
        // record's mesh and slots and in the same pass that names each record's GpuScene primitive; emptied by
        // ClearQueues. A pass runs only after the sync (SceneRenderer draws nothing without it), so a record past
        // Count() was queued after it: it has no GpuScene row and no material row either, and the walks skip it
        // (StaticColumnsHave).
        struct StaticWalkColumns
        {
            static constexpr uint8_t kHasSlots    = 1u << 0u;  // the record holds a slot binding (MaterialSlots)
            static constexpr uint8_t kCastsShadow = 1u << 1u;  // StaticMeshRenderData::CastShadows
            static constexpr uint8_t kMasked      = 1u << 2u;  // SectionShading::Masked
            static constexpr uint8_t kTranslucent = 1u << 3u;  // SectionShading::Translucent
            std::vector<const Desert::Mesh*>       Meshes;     // StaticMeshRenderData::Mesh (displaced included)
            std::vector<GpuDriven::PrimitiveIndex> Primitives; // StaticMeshRenderData::GpuPrimitive
            std::vector<uint64_t>                  Hidden;     // StaticMeshRenderData::HiddenSubmeshes
            std::vector<MaterialInstance*>         Instances;  // the section's surface (ShadingOf): its instance,
            std::vector<DataDrivenMaterial*>       Surfaces;   // and its template (null: no mesh-cell surface)
            std::vector<uint8_t>                   Flags;
            std::vector<uint64_t>                  Placements; // PlacementOf( Primitives[i] ): its world's stamp
            [[nodiscard]] std::size_t              Count() const noexcept
            {
                return Primitives.size();
            }
            // The first @p count records' columns kept (the retained prefix), the rest dropped.
            void Truncate( const std::size_t count ) noexcept
            {
                if ( count >= Count() )
                    return;
                Meshes.resize( count );
                Primitives.resize( count );
                Hidden.resize( count );
                Instances.resize( count );
                Surfaces.resize( count );
                Flags.resize( count );
                Placements.resize( count );
            }
            void Clear() noexcept
            {
                Meshes.clear();
                Primitives.clear();
                Hidden.clear();
                Instances.clear();
                Surfaces.clear();
                Flags.clear();
                Placements.clear();
            }
        };
        StaticWalkColumns m_StaticColumns;
        // Whether m_StaticQueue entry @p index has its columns (m_StaticColumns): synced this view.
        [[nodiscard]] bool StaticColumnsHave( const std::size_t index ) const noexcept
        {
            return index < m_StaticColumns.Count();
        }
        // Whether m_StaticQueue entry @p index (whose Mesh is set) is in @p frustum: the snapshot's box when it
        // is of this gather, else the box IsVisibleInView builds from the entry (the same box, the same answer).
        [[nodiscard]] bool StaticEntryInView( const Core::Frustum& frustum, std::size_t index ) const;
        // Whether the snapshot's lanes (m_StaticSnapshot.WorldLanes) are of this gather and cover every entry:
        // then StaticEntriesInView4 answers StaticEntryInView for four entries at once.
        [[nodiscard]] bool StaticLanesCurrent() const noexcept;
        // StaticEntryInView for entries @p base .. base + 3 (base below the queue's size, StaticLanesCurrent):
        // bit i for entry base + i, by VisibleInView4 over the snapshot's lanes - the same box, the same answer.
        [[nodiscard]] uint32_t StaticEntriesInView4( const Core::Frustum& frustum,
                                                     std::size_t          base ) const noexcept;
        // StaticEntryInView for m_GenericQueue entry @p index (whose Mesh is set).
        [[nodiscard]] bool GenericEntryInView( const Core::Frustum& frustum, std::size_t index ) const;
        // The world box of m_StaticQueue entry @p index (whose Mesh is set): the snapshot's when it is of this
        // gather, else TransformBounds of its local bounds (the box the glass sort reads its centre from).
        [[nodiscard]] Common::Math::AABB StaticEntryWorldBounds( std::size_t index ) const;
        // The ISM gather's key: the main camera's LODView and lens scale, the shadow wind distance and, per field,
        // its mesh's render revision, instance count, material, caster flag, cull distance and wind (not its clock:
        // the gather reads the sway's reach, not its phase). O(fields), never O(instances).
        [[nodiscard]] uint64_t IsmGatherKey() const;
        // Each cascade's ISM part of CascadeStaticSignature (the instances the layer keeps of every field), kept
        // while the gather (ShadowCasterSetup::IsmRevision) and the cascade's caster matrix are the ones it was made
        // from. Invalid after a part that met an instance at the wind edge: that one keys the gather of its frame.
        struct CascadeIsmPart
        {
            bool      Valid    = false;
            uint64_t  Revision = 0;
            glm::mat4 CasterVP{ 1.0f };
            uint64_t  Hash    = 0;
            bool      Movable = false;
            float     Wind    = -1.0f; // the cascade's ShadowWindDistanceOf the part was made with
            glm::vec4 Ring{ -1.0f };   // the cascade's ShadowWindRingOf the part was made with
            bool      WithLod   = true;  // the camera's levels are in Hash (CascadeStaticSignature's withLod)
            bool      WindSplit = false; // an instance in the box is split by the wind ring
            uint64_t  Held      = 0;     // the fields keyed by the GPU content hash (CascadeContentAnswer)
        };
        std::vector<CascadeIsmPart> m_CascadeIsmParts;
        // THE CASCADE CONTENT HASH's readback (ExtractCascadeContent): per frame-in-flight slot the cells and,
        // per region, the cascade and the placement its view was culled at; per cascade the newest answer read.
        struct CascadeContentSlot
        {
            RDG::ExternalBuffer           Cells;
            std::vector<uint32_t>         Views;
            std::vector<CascadePlacement> Placements;
            std::vector<int32_t>          CellTexels;
            bool                          Handed = false;
        };
        struct CascadeContentRead
        {
            bool                  Valid = false;
            CascadePlacement      Placement;
            int32_t               CellTexels = 0;
            std::vector<uint32_t> Cells; // CascadeContentHash::kCells x kWords
        };
        std::vector<CascadeContentSlot> m_CascadeContentSlots;
        CascadeContentRead              m_CascadeContent[kCascadeViews];
        // Plans since the last ExtractCascadeContent: past 1 the GPU no longer hashes (no bins this frame), so no
        // answer is trusted and every field is keyed by the CPU walk again.
        uint32_t m_CascadeContentAge     = ~0u;
        uint64_t m_CascadeContentPending = 0; // keys a layer whose answer is not read yet: redrawn every frame
        // The content hash of the bin-held fields in the box at @p placement (the cells it covers, newest read),
        // @p withLod with their levels; nullopt while no read covers the box (CascadeStaticSignature redraws).
        [[nodiscard]] std::optional<uint64_t>
        CascadeContentAnswer( uint32_t cascade, const CascadePlacement& placement, bool withLod ) const;
        // THE STATIC CASTERS' REVISION (SCENE-CACHE-4; UE FCachedShadowMapData: the static layer is redrawn when a
        // static primitive it holds is added, moved, removed or changes how it draws, never by comparing the
        // primitives every frame). Moved by TrackStaticCasters, run by SyncGpuScene once the registry synced this
        // view's records, from the GpuScene primitive change stream: every field the static / generic part of
        // CascadeStaticSignature and the gathered rows read reaches the stream - the world (Moved), the mesh,
        // ForcedLOD, LODBias and CastShadows (the registry's Updated: GpuSceneRegistry::Touch compares them), the
        // section slot, its shading, the hidden submeshes, the mesh's submeshes, residency and render revision
        // (MotionRecord::Content, SyncGpuScene's words) - plus the queues' own order (the rows and parts are per
        // queue entry: another set or order of primitives is another revision). Only changes of a primitive
        // the static / generic queues hold count (Member), so a moving skinned character or ISM field does not
        // move it.
        struct StaticCasterTrack
        {
            uint64_t             Stream   = 0; // GpuScene::GetStreamId the cursor is a serial of
            uint64_t             Cursor   = 0; // the stream read up to (GetChangeSerial when read)
            uint64_t             Order    = 0; // the static then generic records' primitives, in queue order
            uint64_t             Revision = 0; // 0: never tracked
            uint64_t             Gather   = 0; // the ShadowCasterSetup::Gather it was tracked for
            std::vector<uint8_t> Member;       // per GpuScene primitive: one of the static / generic records'
        };
        StaticCasterTrack m_StaticCasters;
        // THE PRIMITIVES' PLACEMENT STAMPS (CPU-FRAME-ARENA 15; UE FPrimitiveSceneInfo's transform dirty state -
        // FScene::UpdatePrimitiveTransform pushes the change, a cached shadow view re-reads no primitive).
        // Per GpuScene primitive, a stamp that names the world its row holds: a new one, from one counter, each
        // time that world changes, so one stamp is never two worlds of one primitive nor the worlds of two. A
        // signature that keyed a record by its transform (the VSM static / generic entries, a local light's static
        // part) keys it by its primitive's stamp: one word, not 64 bytes per record per view. Advanced by
        // StampPlacements, run by SyncGpuScene right after the registry synced this view's records, from the
        // GpuScene change stream: O(changes). A primitive is restamped only when its row's world differs from the
        // one its stamp names (GpuScene::SetTransform's own test), so a re-added primitive at the same world, like
        // the old transform hash, keys alike. The window lost, a Discarded frame or the stream's first read: every
        // live row is compared (O(primitives), rare); another stream (a re-created scene): every stamp is new.
        struct PrimitivePlacements
        {
            uint64_t                             Stream = 0; // GpuScene::GetStreamId the cursor is a serial of
            uint64_t                             Cursor = 0; // the stream read up to (GetChangeSerial when read)
            uint64_t                             Next   = 0; // the last stamp given (0: none)
            std::vector<uint64_t>                Stamps;     // per GpuScene primitive; 0: never stamped
            std::vector<GpuDriven::GpuAffine3x4> Worlds;     // the row's world each stamp was given for
        };
        PrimitivePlacements m_Placements;
        // Advances m_Placements over the stream since its cursor (above).
        void StampPlacements( const GpuDriven::GpuScene& scene ) noexcept;
        // The stamp of GpuScene primitive @p primitive's world (m_Placements); 0 when it has none (an invalid
        // primitive: the caller keys the record's transform instead).
        [[nodiscard]] uint64_t PlacementOf( const uint32_t primitive ) const noexcept
        {
            if ( primitive < m_Placements.Stamps.size() )
                return m_Placements.Stamps[primitive];
            return 0u;
        }
        // Advances m_StaticCasters over the stream since its cursor and finishes the static / generic rows
        // (FinishStaticCasterRows). After the write-back of the records' rows (GpuPrimitive) in SyncGpuScene.
        // keptPrefix: the retained static prefix's records and primitives stand as synced for m_StaticToken, so
        // its share of the order word is the one folded for that token (m_StaticPrefixOrder), not refolded.
        void TrackStaticCasters( const GpuDriven::GpuScene& scene, bool keptPrefix );
        // The static / generic rows of this gather: kept when the static casters' revision is the one they were
        // made for, their levels reset when StaticLodKey moved; made again otherwise.
        void FinishStaticCasterRows();
        // Every input ComputeLOD reads besides the record (whose fields are the static revision's): the main
        // camera's position and projection and the LOD toggle.
        [[nodiscard]] uint64_t StaticLodKey() const;
        // Each cascade's static / generic part of CascadeStaticSignature, kept while the static casters' revision,
        // the rows' LOD key and the cascade's caster matrix are the ones it was made from.
        struct CascadeStaticPart
        {
            bool      Valid    = false;
            uint64_t  Revision = 0;
            uint64_t  LodKey   = 0;
            glm::mat4 CasterVP{ 1.0f };
            uint64_t  Hash    = 0;
            bool      WithLod = true; // the camera's levels are in Hash (CascadeStaticSignature's withLod)
        };
        std::vector<CascadeStaticPart> m_CascadeStaticParts;
        // Whether the static / generic rows of this gather are the static casters' revision's (TrackStaticCasters
        // ran for this gather and FinishStaticCasterRows kept or made them for it): a part keyed by
        // m_StaticCasters.Revision may be kept only then. One rule for the cascades, the local views and the VSM.
        [[nodiscard]] bool StaticCastersTracked() const;
        // THE LOCAL VIEWS' STATIC / GENERIC PART (SCENE-CACHE-6, UE FCachedShadowMapData per local light): the hash
        // LocalCasterSignature takes of the static and generic casters in a sphere, kept per sphere (its centre
        // and radius bits) while the static casters' revision and the rows' LOD key are the ones it was made from.
        // A sphere not asked for kLocalStaticPartGathers gathers is dropped (a moving light asks a new one each
        // frame); an untracked gather makes the part and keeps nothing.
        static constexpr uint64_t kLocalStaticPartGathers = 2;
        struct LocalStaticKey
        {
            std::array<uint32_t, 4> Bits{}; // centre x, y, z and radius, bit for bit
            auto                    operator<=>( const LocalStaticKey& ) const = default;
        };
        struct LocalStaticPart
        {
            uint64_t Revision = 0;
            uint64_t LodKey   = 0;
            uint64_t Asked    = 0; // the gather that last asked for it
            uint64_t Hash     = 0;
        };
        std::map<LocalStaticKey, LocalStaticPart> m_LocalStaticParts;
        uint64_t                                  m_LocalStaticSweep = 0; // the gather the last eviction ran for
        [[nodiscard]] uint64_t                    LocalStaticCasterHash( const glm::vec3& centre, float radius );
        // Whether m_ShadowCasterSetup is of the queues as they stand (one row per entry of each, the static /
        // generic rows finished for this gather): a queue changed after the last GatherShadowCasters breaks the
        // order SceneRenderer::OnUpdate keeps.
        [[nodiscard]] bool ShadowCastersGathered() const;
        // The level of static / generic caster @p index, asked of the camera once per gather.
        [[nodiscard]] uint32_t ShadowStaticLod( uint32_t index );
        [[nodiscard]] uint32_t ShadowGenericLod( uint32_t index );
        // ISM field @p field's kept instances (Instances / InstanceIndex) of this gather (GatherIsmCasters: every
        // field that casts, on the setup tasks, when the gather starts over).
        [[nodiscard]] const ShadowIsmField& ShadowIsmInstances( uint32_t field ) const;
        // THE ISM FIELDS' GATHER (CPU-ISM-SHADOW; UE FInstanceCullingContext's per-instance pass over a batch):
        // every casting field cut into runs of kIsmGatherRun instances, each run one slot (IsmGatherRun) a setup
        // task fills - the reach and cull distance per instance, the levels of the whole run eight per register
        // (Geometry::SelectLODsFromBounds, the scalar policy bit for bit) - then appended to Instances in field
        // and run order, so the rows are the ones the serial walk made, in its order.
        static constexpr uint32_t kIsmGatherRun = 4096u;
        struct IsmGatherRun
        {
            uint32_t                     Field = 0;
            uint32_t                     Begin = 0; // the field's transforms [Begin, End)
            uint32_t                     End   = 0;
            std::vector<ShadowCasterRow> Rows;   // the kept instances of the run, in index order
            std::vector<uint32_t>        Index;  // each row's index in the field's transforms
            std::vector<uint32_t>        Levels; // the level of every instance of the run (kept or not)
        };
        std::vector<IsmGatherRun> m_IsmGatherRuns; // slots kept with their capacity across gathers
        void                      GatherIsmCasters();
        void GatherIsmRun( IsmGatherRun& run, const Geometry::LODView& lodView ) const noexcept;
        // THE ISM PART OF A CASCADE'S SIGNATURE, per run of a field's gathered rows (CascadeStaticSignature): the
        // same walk the serial loop did, one slot per run on the setup tasks, the runs' hashes added in order.
        struct IsmSignatureRun
        {
            uint32_t              Field = 0;
            uint32_t              First = 0; // Instances[First, First + Count)
            uint32_t              Count = 0;
            std::vector<uint64_t> Bits;
            uint64_t              Hash     = 0;
            bool                  Movable  = false;
            bool                  NearEdge = false;
            bool                  Split    = false;
        };
        struct IsmSignatureView
        {
            const Core::Frustum* CasterFrustum = nullptr;
            float                Wind          = 0.0f;
            glm::vec4            Ring{ 0.0f };
            glm::vec3            RingOrigin{ 0.0f };
            bool                 WithLod = false;
        };
        std::vector<IsmSignatureRun> m_IsmSignatureRuns;
        void SignIsmRun( IsmSignatureRun& run, const IsmSignatureView& view ) const noexcept;
        // A view's static caster buckets by (mesh, hidden submeshes): the index of each key's bucket, so the walk
        // finds it without a search (BuildShadowCascadeDraws); a member so its nodes persist.
        struct ShadowBucketKey
        {
            const Desert::Mesh* Mesh = nullptr;
            SubmeshMask         Hidden{};
            bool                operator==( const ShadowBucketKey& ) const = default;
        };
        struct ShadowBucketKeyHash
        {
            [[nodiscard]] std::size_t operator()( const ShadowBucketKey& key ) const noexcept;
        };
        std::unordered_map<ShadowBucketKey, std::size_t, ShadowBucketKeyHash> m_ScratchShadowBucketOf;
        // Masked casters of one cascade: per cascade copy, its rows / instance transforms / bone poses end to
        // end (each uploaded once before the copy's draws are recorded), and the draws naming their slice.
        struct MaskedCasterSet
        {
            DataDrivenMaterial*    Caster = nullptr;
            std::vector<glm::mat4> Transforms; // Instanced path: InstanceTransforms; Skinned path: Bones
            // CPU-ISM-PACK 5: a caster in SurfaceSceneInstancesVariant: InstanceTransforms holds these GpuScene
            // instance ids instead (Transforms stays empty).
            std::vector<uint32_t> Ids;
            bool                  SceneIds = false;
        };
        struct MaskedCasterDraw
        {
            MaskedCasterSet*  Set       = nullptr;
            MeshVertexPath    Path      = MeshVertexPath::Static;
            Desert::Mesh*     Mesh      = nullptr;
            glm::mat4         Transform = glm::mat4( 1.0f );
            MaterialInstance* Instance  = nullptr;
            uint32_t          Row       = 0;
            uint32_t          Count     = 1; // instances (Instanced path)
            uint32_t          First     = 0; // first instance transform / first bone
            // Skinned path: the primitive in the scene's GpuScene (PrimitiveIndex), whose skin-cache
            // fields the cell fetches the cached pose by; a row without an address skins from the copy's bones.
            uint32_t          GpuPrimitive         = 0;
            uint32_t          LodLevel  = 0;
            SubmeshMask       HiddenSubmeshMask{}; // the caster's section group, as its lit draw (+ VSM clusters)
            InstanceWindPush  Wind;
            float             BoundsPad = 0.0f; // WindBoundsPad of the wind above, cm (MeshDrawCommand)
            // Skinned path, virtual shadow map levels only: the posed world box (MeshDrawCommand::WorldBounds).
            std::optional<Common::Math::AABB> WorldBounds;
        };
        std::vector<std::unique_ptr<MaskedCasterSet>> m_ScratchMaskedSets;
        std::vector<MaskedCasterDraw>                 m_ScratchMaskedDraws;
        // A masked caster's pipeline by IDENTITY (CPU-FRAME-ARENA 6, no name formatted per call): the cell
        // program (Shader::GetSerial - one object per permutation, so strictly finer than name # variant hash)
        // x the shared caster state set it was built over (CasterPipelinesOf: its Static member names the set).
        struct MaskedCasterKey
        {
            uint64_t                Program = 0;
            const GraphicsPipeline* Casters = nullptr;

            bool operator==( const MaskedCasterKey& ) const = default;
        };
        struct MaskedCasterKeyHash
        {
            size_t operator()( const MaskedCasterKey& key ) const noexcept
            {
                const size_t seed = std::hash<uint64_t>{}( key.Program );
                return seed ^ ( std::hash<const void*>{}( key.Casters ) + 0x9e3779b97f4a7c15ull + ( seed << 6 ) +
                                ( seed >> 2 ) );
            }
        };
        std::unordered_map<MaskedCasterKey, std::shared_ptr<GraphicsPipeline>, MaskedCasterKeyHash>
                                                 m_MaskedCasterPipelines; // null = refused
        std::vector<GenericDraw>                 m_ScratchGenericDraws;
        std::vector<DataDrivenMaterial*>         m_ScratchGenericRecorders; // distinct recording materials
        float                                                     m_WorldTimeSeconds = 0.0f;
        ParallaxQuality                                           m_ParallaxQuality;
        // SyncGpuScene's CPU scratch (capacity kept) and the scene's GPU scene it last synced into.
        std::vector<MotionRecord>          m_ScratchRigidRecords;
        std::vector<StaticPrimitiveRecord> m_ScratchStaticRecords;
        std::vector<MotionRecord>          m_ScratchSkinnedRecords;
        std::vector<MotionFieldRecord>     m_ScratchFieldRecords;
        GpuDriven::GpuSceneRecordRows      m_ScratchGpuSceneRows;
        const GpuDriven::GpuScene*         m_GpuScene = nullptr;
        RDG::BufferRef                     m_FrameSceneInstances{}; // SetFrameSceneInstances
        // PrepareViewIsmIds: the view's id buffer (per frame in flight, owned here and lent to every scene-id
        // cell), its CPU image, each m_InstancedQueue entry's first word in it (kNoViewIsmIds: none this frame)
        // with the cull entry's rebuild count it was filled from, and the stale culls' scratch (capacities kept).
        struct ViewIsmIdRange
        {
            uint32_t           Base     = kNoViewIsmIds;
            uint64_t           Rebuilds = 0;
            const IsmViewCull* Entry    = nullptr; // map nodes: stable until ClearQueues erases
        };
        std::shared_ptr<ShaderResources::StorageBuffer> m_ViewIsmIds;
        std::vector<uint32_t>                           m_ViewIsmIdWords;
        std::vector<ViewIsmIdRange>                     m_ViewIsmIdRanges;
        std::vector<DrawBuildScratch::StaleIsmCull>     m_ViewIsmStale;
        bool                                            m_ViewIsmIdsReady        = false;
        bool                                            m_ViewIsmIdsMismatchSaid = false;
        // The stale entries' culls in about WorkerCount() + 1 contiguous slices of about equal instance counts,
        // one setup task each (ParallelSetup 0: inline, in order); each entry written by its one task.
        void RunStaleIsmViewCulls( std::span<const DrawBuildScratch::StaleIsmCull> stale,
                                   const Core::Frustum&                            frustum );
        // The GPU skin cache's per-view side (SetSkinCacheEnabled, SkinCacheReads, SkinCacheBonesImport).
        std::vector<RDG::BufferRef> m_SkinCacheReads;
        RDG::ExternalBuffer         m_SkinCacheBonesImport;
        bool                        m_SkinCacheEnabled = true;
        // Shadows.WindAnimationDistance, set by SceneRenderer::BeginScene before every gather (cm).
        float m_ShadowWindDistance = 0.0f;
        // Shadows.FoliageShadowLodBias, set by SceneRenderer::BeginScene (BladeShadowSet).
        int m_FoliageShadowLodBias = 0;
    };
} // namespace Desert::Graphic::System
