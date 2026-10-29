#pragma once

#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Common/Core/Units.hpp>
#include <unordered_set>

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
#endif
#include <Engine/Graphic/Materials/Material.hpp>
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
#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>
#include <Common/Core/AssetHandle.hpp>

namespace Desert::Runtime
{
    class MaterialService;
    class ShaderService;
} // namespace Desert::Runtime

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
            // The per-draw material state the draw records with (transform push constant, material row, bone
            // offset, wind, the instance's descriptor bind), written in exec right before the draw - the same
            // order as before the split. Empty: the draw has none.
            std::function<void()> BindState{};
            // What a GPU cull of the draw needs (the virtual shadow map's, CollectVirtualShadowCull): the matrices
            // its instance range indexes, from FirstInstance (empty: one instance at Transform), how far the wind
            // moves its vertices past the mesh's bounds (cm, WindBoundsPad), and the world box of a single
            // instance whose vertices leave the mesh's bind bounds (a skinned pose, SkinnedPoseBounds). Instances
            // points into the list builder's scratch: valid until the list is built again.
            std::span<const glm::mat4>        Instances{};
            float                             BoundsPad = 0.0f;
            std::optional<Common::Math::AABB> WorldBounds{};
            // A BINNED cell's draw (Common/GpuScene.glslh DESERT_GPU_SCENE_DRAWS): per submesh of Mesh, the base of
            // its rows on the pass's unculled VisibleInstances list (GpuDriven::DrawCommandCache::UnculledFirstVisible
            // at LodLevel), kInvalidIndex where it has no cached draw; Renderer::RenderMesh binds the id stream and
            // draws each submesh from its base. Empty: a cell that reads no id stream.
            std::vector<uint32_t> SubmeshFirstRows{};
        };

        // THE frame's draw list of one lit mesh node. The material (hence shader and route fill) of every draw is
        // chosen ONCE, by Build* in the node's setup; Declare declares one binding block per distinct (material
        // executor, recording pipeline's shader) pair of the list (the layout kept for the shader the draws RECORD
        // with - `Pipeline->GetSpecification().Shader`, re-derived only on its reload, never per frame - the
        // executor's route fill, and the scene/view inputs the layout has slots for), so ValidatePassBindings
        // judges every draw before anything is recorded; Record (the exec) walks the same list with the block each
        // draw's executor was declared as. A build that failed is kept and is Record's error, before any draw.
        // The GPU-culled bins a MeshDrawList issues after its CPU commands (UE FInstanceCullingDrawParams +
        // SubmitDrawCommands): one view's CulledDraws of one pass's table, the scene / pass tables the binned vertex
        // stage reads by name (Common/GpuScene.glslh 41-44), the device's submission mode, and per IndirectGroup the
        // view's carrier of the scene blocks (null for an empty group).
        // THE VIRTUAL-GEOMETRY RASTER of one (pass, view) (SceneRenderer::AddFrameVirtualGeometry ->
        // MeshRenderer::SetFrameVirtualGeometry): the cluster cut's HW clusters binned into the pass table's groups
        // (VirtualGeometry::RasterBins), drawn after the bins, one vkCmdDrawIndirect per group whose
        // IndirectGroup::VirtualGeometryPipeline exists, with the tables that cell reads by name (Common/GpuScene.glslh
        // 50-53) and per group the view's carrier of that cell's shader.
        struct VirtualGeometryIssue
        {
            RDG::BufferRef GroupArgs;       // VkDrawIndirectCommand[GroupCount] (RasterBins::GroupArgs)
            RDG::BufferRef Rows;            // uint32 instance id stream (RasterBins::Rows)
            RDG::BufferRef VisibleClusters; // the cut's VgVisibleCluster[] (binding 52)
            RDG::BufferRef Clusters;        // VirtualMeshBuffers::Clusters (binding 50)
            RDG::BufferRef Dags;            // VirtualMeshBuffers::Dags (binding 51)
            RDG::BufferRef PrimitiveDraws;  // the pass's PassDrawBuffers::PrimitiveDraws (binding 53)
            uint32_t       GroupCount = 0;
            std::vector<const MaterialExecutor*> GroupCarriers; // per Table->Groups; null = the group draws no cluster
        };

        struct BinnedIssue
        {
            const GpuDriven::PassDrawTable*      Table = nullptr;
            GpuDriven::CulledDraws               Culled;
            RDG::BufferRef                       Instances;         // GpuSceneBuffers::Instances
            RDG::BufferRef                       DrawSlots;         // PassDrawBuffers::DrawSlots
            RDG::BufferRef                       SubmeshTransforms; // DrawCommandBuffers::SubmeshTransforms
            GpuDriven::IndirectSubmissionMode    Mode;
            std::vector<const MaterialExecutor*> GroupCarriers; // per Table->Groups
            std::optional<VirtualGeometryIssue>  VirtualGeometry; // drawn after the bins (phase 1 only)
            // False: a VG-only issue (a list whose view culled no bins, the virtual shadow map's levels): Culled is
            // empty, only VirtualGeometry is drawn; GroupCarriers is then one null per group.
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
            // A draw without pipeline, mesh or material (or whose material has no shader) is refused here.
            void               Add( MeshDrawCommand command );
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
            // translucency node's scene snapshot), handed the block's layout so it binds only the slots that
            // shader has: a node whose cells are several shaders binds them once each.
            void Declare( RDG::PassBuilder& pass, const std::optional<SceneViewInputs>& view,
                          const std::function<void( RDG::BindingBlockBuilder&, const RDG::ShaderBindingLayout& )>&
                               perBlock ) const;
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
            // The GPU-driven mesh pass Record's draws are counted under (DrawCounter::LastFrameMeshPass, DesertCtl
            // `state views.meshPasses`); set once by the owner (MeshRenderer::Initialize), kept across Clear.
            // Unset: a list outside the binned passes (glass, debug, card capture), counted only globally.
            void CountAs( GpuDriven::MeshPass pass )
            {
                m_CountedPass = pass;
            }

        private:
            // One declared block: the executor whose route fills it and the pipeline of its first draw, whose
            // shader keys its layout (every draw of the block records with a pipeline of that same shader).
            struct Block
            {
                const MaterialExecutor* Material = nullptr;
                const GraphicsPipeline* Pipeline = nullptr;
                bool                    Binned   = false; // declares the binned tables (SetBinned)
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
            std::optional<BinnedIssue>   m_Binned;     // SetBinned
            std::vector<uint32_t>        m_GroupBlock; // per group of m_Binned: its block (kInvalidBlock: empty)
            std::vector<uint32_t>        m_VgGroupBlock; // per group of m_Binned->VirtualGeometry: its block
            uint64_t                     m_Triangles = 0; // Add's count of the commands' triangles (Triangles)
            // Kept across frames (Clear drops only the layouts of destroyed shaders). Mutable: Declare is const.
            mutable ShaderBindingLayoutSet m_Layouts;
            std::optional<GpuDriven::MeshPass> m_CountedPass;
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
            class Desert::StaticMesh* Mesh      = nullptr;
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
            // Where this field's GpuScene instances start in GetInstancedGpuInstances() (SyncGpuScene): instance
            // i is GpuScene instance GetInstancedGpuInstances()[FirstGpuInstance + i] - its TLAS custom index and
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
            // GrassBlade only: the variety's shape, Vertex_GrassBlade.glslh's GrassBladeShape.
            std::shared_ptr<ShaderResources::StorageBuffer> BladeShape;
            const GpuInstanceRefs*                          Refs = nullptr; // the producer's, this frame
            // The instances Instances holds room for (GpuInstances.hpp header word 1): the TLAS rows reserved for
            // the producer (RayTracingGpuInstanceSource::Rows).
            uint32_t Capacity = 0;
            // Per bucket (a strip level for GrassBlade), the instances its record holds room for: the TLAS rows
            // reserved for that level (RayTracingGpuInstanceSource::Rows). Owned by the producer, this frame.
            std::span<const uint32_t> BucketCapacities;
            // Stable per section across frames: the view-variant key (several cascade views: Key * kCascadeViews + v).
            uint32_t Key = 0;
            // GrassBlade only: the shared BLAS of each strip level (GrassBladeRayTracing.hpp), indexed by submesh;
            // empty when the view has no TLAS. Owned by the grass renderer, this frame.
            std::span<const RayTracing::RayTracingGeometryResource* const> BladeGeometry;
            // Where Instances holds each instance's LAST frame transform: instance i's previous world sits at
            // i + PrevInstanceOffset (a producer whose instances move, e.g. particle Mesh rows). 0 = the instances
            // do not move (grass: only the wind moves them), the previous world is the world.
            uint32_t PrevInstanceOffset = 0;
            // A world box every instance lies in, cm, when the producer has one (a particle emitter's fixed
            // Bounds through the entity's transform, VFXBoundsAt): the section is drawn only into the views and the cascades
            // whose frustum it meets (Core::Frustum::Intersects). nullopt: the producer knows no box (the grass
            // follows the camera), drawn into every view and cascade.
            std::optional<Common::Math::AABB> WorldBounds{};
        };

        // A static mesh drawn with a generic data-driven material. Two producers:
        //  - MaterialComponent (Shader Override) on the whole entity: ShaderName + Overrides drive a
        //    shader-keyed shared material (SlotMaterial == nullptr, all submeshes).
        //  - v3 per-slot materials: SlotMaterial points at the slot's own DataDrivenMaterial (asset
        //    params already applied) and VisibleSubmeshMask limits the draw to that slot's submeshes.
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
            std::string               ShaderName;
            Graphic::MaterialOverrides Overrides;
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
            // system's SDF font atlas. Non-owning: the producer keeps it alive for the frame.
            Graphic::Image2D* DirectTexture        = nullptr;
            std::string       DirectTextureSampler;

            // The record's own hit group when its program is no surface template (3D text: TextRayHit, built by
            // the producer from the values this draw carries). Empty Template: a ray hit shades with SlotMaterial,
            // else the default surface. Read by the gather only when VisibleInRayTracing.
            RayTracing::RayTracingProgramHit RayHit;

            // Its row in the scene material table (SyncGpuScene, RegisterGenericRows): a per-slot draw's is its
            // asset's row, a Shader Override draw's its own (template defaults + Overrides.Params + texture slots,
            // keyed by Entity/MotionPart). nullopt: SyncGpuScene could not shade the record (no compiled program,
            // a domain the mesh path does not draw) and the record is not drawn.
            std::optional<uint32_t> MaterialRow;
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

        // THE SCENE'S BINNED DRAWS (GpuDriven::DrawCommandCache, UE FCachedPassMeshDrawList): fed by SyncGpuScene with
        // the primitives whose draws changed, one entry per primitive gathered across its section records, each
        // submesh's PSO the `.Binned` cell of its material for the pass (ResolveBinnedPipeline). Null before the
        // first sync. The feed rebuilds the host tables; their upload, the cull and the issue are the frame's.
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
        // (kBinnedMainView) of MainViewPass, or a sun cascade view (slot * kMaxCascades + c) of ShadowCascade whose
        // static layer is drawn (CascadeBinsDrawn: binned casters live only in the cached static layer). An active
        // (pass, view) issues its bins and its CPU loops drop every record the bins hold
        // (DrawCommandCache::SubmeshesBinned); any other draws everything on the CPU, as before.
        static constexpr uint32_t kBinnedMainView = ~0u;
        [[nodiscard]] Common::ResultStr<GpuDriven::DrawCommandBuffers> BeginFrameBins( RDG::Builder& graph );
        [[nodiscard]] GpuDriven::MeshPass                             MainViewPass() const;
        [[nodiscard]] bool                                            CascadeBinsDrawn( uint32_t view ) const;
        // Refused (logged once per message, the view stays on the CPU) when a non-empty group's carrier cannot be
        // made.
        void SetFrameBinned( GpuDriven::MeshPass pass, uint32_t viewKey, const GpuDriven::CulledDraws& culled,
                             const GpuDriven::DrawCommandBuffers& upload, const GpuDriven::GpuSceneBuffers& scene );
        // TWO-PHASE OCCLUSION, phase 2 (UE FInstanceCullingContext's post pass): the draws of the instances phase 1
        // held back by last frame's HZB that this frame's HZB finds visible, issued by a second slice of (pass,
        // view) with the same carriers and tables as its phase-1 issue (SetFrameBinned) and @p culled's own
        // indirect args. False (nothing to draw) when (pass, view) has no phase-1 issue this frame: its CPU lists
        // then draw every record, phase 2 has nothing it held back.
        bool SetFramePostOcclusion( GpuDriven::MeshPass pass, uint32_t viewKey, const GpuDriven::CulledDraws& culled );
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
        // `.VirtualGeometry` twin (IndirectGroup::VirtualGeometryPipeline: `<T>/<Static|Instanced>.VirtualShadowDepth
        // [.Parallax].Binned.VirtualGeometry` on the VSM caster state), whose vertex stage takes the level's matrix
        // and fragment stage the level's page table from the cluster's view. The carriers are made by
        // AddVirtualShadowRasterPasses; from then the levels' CPU walk leaves out every submesh whose clusters a
        // twin draws (CpuWalkHidden). Returns per group whether its clusters are drawn. A group with clusters
        // binned and no twin is one of two states, both kept in the CPU walk (no shadow is lost):
        //  - a FALLBACK (UE: a material with no Nanite path renders the fallback mesh - a known state, not an
        //    error) when its template declares no `.VirtualGeometry` twin (DeclaresVirtualGeometryTwin): counted per
        //    VG submesh binned into it (VsmFallbackSubmeshes), never logged;
        //  - a HELD REFUSAL when the template declares the twin and no pipeline was made of it: logged once per
        //    cell, counted (VsmRefusedGroups; must be 0).
        // Refused without the VSM caster pipelines, a draw command cache or for bins of another group count;
        // nothing is attached then.
        [[nodiscard]] Common::ResultStr<std::vector<uint8_t>>
        SetVirtualShadowGeometry( MeshRendererDetail::VirtualGeometryIssue issue,
                                  const GpuDriven::DrawCommandBuffers& upload, const GpuDriven::GpuSceneBuffers& scene );
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
        void ClusterableSubmeshes( GpuDriven::MeshPass pass, const Desert::StaticMesh* mesh,
                                   GpuDriven::PrimitiveIndex primitive, uint64_t hidden,
                                   std::vector<uint64_t>& words ) const;
        // VsmFallbackSubmeshes of this frame: the static and ISM queues' VG submeshes binned into a group flagged in
        // @p fallbackGroups (index = VirtualShadow table group). Called by SetVirtualShadowGeometry.
        [[nodiscard]] uint32_t CountVsmFallbackSubmeshes( const std::vector<uint8_t>& fallbackGroups ) const;
        // THE CPU WALK OF ONE RECORD (a static entry, an ISM / foliage field, a generic or skinned draw) in one
        // view - THE ONE RULE "who draws this submesh" for every view pass (main Forward / GBuffer, a sun
        // cascade's layer, a VSM level; VIRT-GEOM step 26). UE: a primitive the cluster path cannot draw is drawn
        // by its fallback mesh, never lost. Returns the submeshes the CPU walk HIDES (the record's own @p hidden
        // plus what the GPU draws), or empty when the GPU draws every submesh the record shows:
        //  - a VG primitive in the view (a binned view: InstanceCull took its instances out of the view's bins as
        //    cluster-cut candidates, m_FrameClusterCandidates; a VSM level: its mesh has uploaded DAG rows): the
        //    walk hides only the submeshes the view's cluster raster DRAWS - a candidate DAG binned into a group
        //    whose `.VirtualGeometry` carrier the issued raster holds (a VSM level: whose twin pipeline exists,
        //    its carriers are made later). A refused cut, a refused carrier, a twin-less group or a submesh with
        //    no candidate DAG: walked, so the clusters it would have drawn are drawn by the mesh's own submeshes;
        //  - any other record of a binned view: empty when the bins hold every shown submesh
        //    (DrawCommandCache::SubmeshesBinned), else @p hidden;
        //  - a view with neither (a local light, a movable layer, a refused bins issue): @p hidden.
        // The words past the first 64 submeshes are interned in m_ClusterWalkWords (kept until the next
        // ResetVirtualShadowGeometry), so equal masks share one view and their records batch together.
        struct ClusterWalkView
        {
            GpuDriven::MeshPass Pass = GpuDriven::MeshPass::Forward;
            // The view issues GPU-culled bins this frame (FrameBinned): the main view or a sun cascade's static
            // layer.
            bool BinsIssued = false;
            // The phase-1 cluster raster's carriers per Table->Groups (BinnedIssue::VirtualGeometry, made by
            // SetFrameVirtualGeometry); null: no raster this frame (no cut, or the cut / its carriers refused).
            const std::vector<const MaterialExecutor*>* RasterCarriers = nullptr;
            // A VSM level: the view issues no bins; its VG-only raster (m_VsmGeometry) is issued when set.
            bool VirtualShadow = false;
        };
        [[nodiscard]] ClusterWalkView BinnedWalkView( GpuDriven::MeshPass pass, uint32_t viewKey ) const;
        [[nodiscard]] ClusterWalkView VirtualShadowWalkView() const;
        [[nodiscard]] ClusterWalkView UnbinnedWalkView( GpuDriven::MeshPass pass ) const;
        // @p mesh: the record's mesh (its submesh count; a StaticMesh's DAG rows for a VSM level), may be null.
        [[nodiscard]] std::optional<SubmeshMask> CpuWalkHidden( const ClusterWalkView& view, const Desert::Mesh* mesh,
                                                                GpuDriven::PrimitiveIndex primitive,
                                                                uint64_t                  hidden );
        // THE CLUSTER-CUT CANDIDATES OF THIS FRAME (SceneRenderer::AddFrameDrawBins, after InstanceCull's main
        // phase took them with candidates appended): per primitive, the submeshes whose DAG rows InstanceCull
        // appends for EVERY cull view (MakeVirtualGeometryCandidates' spans, one bit per submesh, no 64 cap). Such
        // a primitive's instances reach no bin of any binned view. Forgotten by BeginFrameBins.
        void SetFrameClusterCandidates( std::unordered_map<GpuDriven::PrimitiveIndex, std::vector<uint64_t>> submeshes );
        // A new frame of the virtual shadow map's cluster path: drops the last frame's issue, its refusal and
        // fallback counts and its interned walk masks. Called by SceneRenderer before every frame's graph, so a frame whose
        // method is not VirtualMaps (or whose VSM cut is refused) reports no refused groups.
        void ResetVirtualShadowGeometry();
        // Groups of the VirtualShadow table holding bins but no `.VirtualGeometry` twin in this frame's VSM cluster
        // raster (SetVirtualShadowGeometry): their clusters are not rastered, their primitives stay in the walk.
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

        // The GpuScene instances of SyncGpuScene's @p otherRigid records, in their order. Valid until the next
        // sync.
        [[nodiscard]] std::span<const uint32_t> GetOtherGpuInstances() const
        {
            return m_OtherGpuInstances;
        }

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
        // This frame's ISM / foliage fields; with GetInstancedGpuInstances, one TLAS instance per field instance.
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
        // Every queued field's per-instance GPU-scene rows, end to end (InstancedMeshRenderData::FirstGpuInstance).
        [[nodiscard]] std::span<const uint32_t> GetInstancedGpuInstances() const
        {
            return m_InstancedGpuInstances;
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
        struct CascadeCacheCopy
        {
            std::shared_ptr<Image2D> From;
            uint32_t                 Layer = 0; // the cascade: its layer of the array
            std::string              Name;
        };
        struct ShadowCascadeFrame
        {
            std::vector<SystemRasterPass> StaticPasses;
            std::vector<CascadeCacheCopy> Copies;
            std::vector<SystemRasterPass> MovablePasses;
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
        [[nodiscard]] std::vector<SystemRasterPass> LocalShadowPasses();

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
        static constexpr uint32_t kRainOcclusionView      = kVsmViewBase + kVsmClipmapLevelCount;
        static constexpr uint32_t kLocalShadowViewBase    = kRainOcclusionView + 1u;
        static constexpr uint32_t kFixedShadowViews       = kLocalShadowViewBase;
        [[nodiscard]] static constexpr bool IsVirtualShadowView( const uint32_t view )
        {
            return view >= kVsmViewBase && view < kRainOcclusionView;
        }

        // THE SUN'S VIRTUAL SHADOW MAP, before its page passes (SceneRenderer::AddFrameVirtualSunShadow): what
        // changed among the casters since the last frame the map was drawn. A static, generic or still instanced
        // caster and every non-mesh caster (IShadowCaster::AppendVirtualShadowEntries) that was added, removed or
        // changed hands its old and its new box; a caster that moves by itself (skinned, swaying foliage) hands
        // its box now and last frame, every frame. StaticCastersChanged: a masked caster's pipeline was still
        // compiling when the pages were drawn, so the cached pages lack it and every one is dropped.
        struct VirtualShadowInvalidation
        {
            std::vector<VsmInvalidationBox> Boxes;
            bool                            StaticCastersChanged = false;
        };
        [[nodiscard]] VirtualShadowInvalidation CollectVirtualShadowInvalidation();
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
        // SETUP of "Deferred: GBuffer Post-Occlusion" (SceneRenderer::AddFrameDrawBinsPostOcclusion, after this
        // frame's HZB): only the main view's phase-2 bins (SetFramePostOcclusion), LOADing the G-buffer the
        // phase-1 draws wrote. The exec records that list.
        void DeclareGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view );
        [[nodiscard]] Common::BoolResultStr RenderGBufferPostOcclusionManual( const RDG::PassContext& context ) const;
        // SETUP of "Deferred: Forward Depth/Normal" (after the terrain's G-buffer, before the depth resolve and
        // every screen pass): the generic queue's OPAQUE draws through their template's (Static x DepthNormal)
        // program into G-buffer depth + normal, so the ray-traced sun trace and the screen passes see the meshes
        // that are shaded forward over the composite. The exec records that list. @p rayTracedSun: the frame
        // intends to ray trace the sun (SceneRenderer::SunShadowIsRayTraced), so "Deferred: Generic" shades the
        // sun-reading opaque programs through their DESERT_SUN_SHADOW_SCREEN_MASK permutation - the forward
        // pipeline whose readiness this prepass checks together with its own.
        void DeclareForwardDepthNormalDraws( RDG::PassBuilder& pass, bool rayTracedSun, bool localShadowMasks );
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
        void GatherSurfaceCacheInstances( std::vector<SurfaceCache::SceneInstance>& out );
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
        void SubmitGenericMesh( const GenericMeshRenderData& data );
        void SubmitInstancedMesh( const InstancedMeshRenderData& data );
        void SubmitGpuInstanced( const GpuInstancedRenderData& data );
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
        std::unordered_map<MeshCellPipelineKey, std::shared_ptr<GraphicsPipeline>, MeshCellPipelineKeyHash>
             m_CellPipelines; // null = refused

        // THE WIREFRAME TWIN OF A FORWARD PASS PIPELINE (UE draws every mesh kind in its wireframe view through
        // the same pass it is drawn in otherwise): @p fallback's line-polygon twin while the view is on, else
        // @p fallback. Static and Instanced each have one, so an ISM is drawn in wireframe by the same recording
        // as in Lit — only the pass state differs. A pipeline with no twin answers itself.
        [[nodiscard]] GraphicsPipeline* WireframePipelineOr( GraphicsPipeline* fallback ) const
        {
#if DESERT_DEV_INSTRUMENTS
            if ( !m_Wireframe || fallback == nullptr )
                return fallback;
            if ( fallback == m_StaticPipeline.get() && m_StaticWireframePipeline )
                return m_StaticWireframePipeline.get();
            if ( fallback == m_StaticInstancedPipeline.get() && m_StaticInstancedWireframePipeline )
                return m_StaticInstancedWireframePipeline.get();
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
        // nondeterministic). Called from SceneRenderer::OnUpdate.
        void UpdateCascades();

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
        void BuildStaticDraws( MeshRendererDetail::MeshDrawList& list );
        void BuildSkinnedDraws( MeshRendererDetail::MeshDrawList& list );
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
        // The cache decision of each cascade for this frame (UpdateCascades, after the fit): whether its static
        // layer is redrawn, whether it holds a movable caster, whether its map is rebuilt.
        void PlanCascadeCache();
        // A hash of the static casters cascade @p cascade's caster box reaches (what they draw and where), and
        // whether a movable caster reaches it: the cache key of the cascade's static layer.
        [[nodiscard]] uint64_t CascadeStaticSignature( uint32_t cascade, bool& movable ) const;
        // The caster materials of shadow view @p view, made on its first use. False when the caster
        // pipeline does not exist (a renderer with no shadow budget).
        bool EnsureShadowViewMaterials( uint32_t view );
        // A hash of every caster in @p light's reach (what it draws into its views and where) - the meshes and the
        // non-mesh casters (IShadowCaster::LocalShadowSignature) - or nullopt when one of them moves by itself
        // every frame (a skinned mesh, swaying foliage): the cache key of a local view.
        [[nodiscard]] std::optional<uint64_t> LocalCasterSignature( const LocalShadowCasterLight& light ) const;
        // The mesh casters alone in the sphere (@p centre, @p radius cm), or nullopt when one of them moves by itself
        // every frame: the part of a local view's key the meshes give, and the rain occlusion map's key (the
        // terrain under it changes only with the box, which RainOcclusionNeedsRecentre redraws on).
        [[nodiscard]] std::optional<uint64_t> LocalCasterSignature( const glm::vec3& centre, float radius ) const;
        // The cache decision of each light given views by UpdateLocalShadows (m_LocalCachePlans): its key from
        // its casters and its views; the views of a light whose key changed (or that has none) go to
        // m_LocalRedraws.
        void PlanLocalShadowCache();

        // The frame's draw lists, rebuilt by each node's setup and recorded by its exec.
        MeshRendererDetail::MeshDrawList m_ForwardDraws; // MeshGeometryPass: static + skinned + generic
        MeshRendererDetail::MeshDrawList m_GBufferDraws; // "Deferred: GBuffer": static, instanced, skinned
        MeshRendererDetail::MeshDrawList m_GBufferPostDraws; // "Deferred: GBuffer Post-Occlusion": phase-2 bins
        MeshRendererDetail::MeshDrawList m_GenericDraws; // "Deferred: Generic"
        MeshRendererDetail::MeshDrawList m_DepthNormalDraws; // "Deferred: Forward Depth/Normal"
        // "MeshShadowCascade<c>": the cascade's casters, built by its Declare (BuildShadowCascadeDraws).
        MeshRendererDetail::MeshDrawList m_CascadeDraws[kCascadeViews];
        // The cascade's non-mesh casters (m_ShadowCasters) its Declare declared, each with the index of its first
        // binding block (after m_CascadeDraws[c]'s); the exec records exactly these.
        std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>> m_CascadeCasters[kCascadeViews];
        MeshRendererDetail::MeshDrawList m_GlassDraws;      // "Deferred: Glass" (block 0 = the glass executor's)
        MeshRendererDetail::MeshDrawList m_FrontLayerDraws; // "Translucency: Front Layer G-buffer"
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
        std::vector<CardCaptureSource> m_CardCaptureSources;
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
    public:
        // THE ONE PERMUTATION FUNCTION of a generic forward program on the deferred path, asked by every place
        // that builds or cooks it: "Deferred: Generic" (BuildGenericDraws), the prepass's readiness check
        // (GenericForwardPipelineReady) and the cook (MaterialProgramVariants, OverrideProgramVariants). An
        // opaque program reads DESERT_SUN_SHADOW_SCREEN_MASK when @p rayTracedSun and it samples the sun's shadow
        // (declares ShadowCascadesTexture), and DESERT_LOCAL_SHADOW_SCREEN_MASK when @p localShadowMasks (the view has
        // SceneViewInputs.LocalShadowMasks) and it samples the local shadow atlas; defines in that order (sun,
        // then local), ShippedByCook. A blended program (TextSDF) or one that reads neither is the default
        // permutation (shaded through itself).
        [[nodiscard]] static Core::ShaderVariant ForwardShadowMaskVariant( const Shader& shader, bool rayTracedSun,
                                                                           bool localShadowMasks );
        // The permutations the mesh path can select for @p materials (UE: a material's cooked shader map): each
        // material's (Static x Forward) cell under every ForwardShadowMaskVariant the renderer can ask for (sun
        // only, local only, both), defaults omitted and duplicates listed once - the list PackageCook
        // CookShaderVariants cooks, from the same function the renderer acquires by. A material without that
        // cell draws nothing forward (CreateSurfaceMaterial names it) and has nothing to cook. A Shader Override
        // draw (no `.demat`) is OverrideProgramVariants'.
        [[nodiscard]] static std::vector<Core::CookedProgramVariant>
             MaterialProgramVariants( const Runtime::MaterialService& materials,
                                      const Runtime::ShaderService&   shaders,
                                      std::span<const ::Common::AssetHandle> handles );
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
        // permutation through ForwardShadowMaskVariant, as that pass does.
        [[nodiscard]] bool GenericForwardPipelineReady( const std::shared_ptr<Shader>& shader,
                                                        const std::string& shaderName, bool rayTracedSun,
                                                        bool localShadowMasks );
        // THE ONE READINESS CHECK for an opaque generic draw on the deferred path: "Deferred: Forward
        // Depth/Normal" admits it when BOTH its depth-normal and its forward pipeline are Ready and records it
        // here; "Deferred: Generic" shades only what this frame admitted, so no draw is shaded forward against a
        // missing prepass depth (it would fail CloserOrEqual) and none is in the depth unshaded. Stamped with the
        // view's frame.
        bool OpaqueGenericAdmitted( const GenericMeshRenderData& draw ) const;
        std::unordered_set<const GenericMeshRenderData*> m_GenericPrepassed;
        std::optional<uint64_t>                          m_GenericPrepassFrame;
        bool                                             m_GenericPrepassRayTracedSun = false;
        bool                                             m_GenericPrepassLocalShadowMasks = false; // the prepass's DESERT_LOCAL_SHADOW_SCREEN_MASK choice
        bool                                             m_GenericLocalShadowMasks        = false; // "Deferred: Generic" (DeclareGenericDraws)
        std::shared_ptr<GraphicsPipeline> DefaultSurfacePipeline();
        // A caster whose material is Masked draws through its OWN template's (path x ShadowDepth) cell
        // (ShadowCasterCellFor), on a per-cascade copy of that material (MaterialService::GetViewVariant) —
        // its mask texture and clip threshold with it. The cascade material of the draw, or null when the
        // caster takes the shared program (opaque, or no batched-path material).
        static DataDrivenMaterial* MaskedCasterMaterial( const DataDrivenMaterial* material, MeshVertexPath path,
                                                         uint32_t cascade );
        // The pipeline of one masked caster cell: the shared caster's state for @p path with the cell's shader,
        // built on first use and kept per cell shader. Null (logged once per shader) when it cannot.
        GraphicsPipeline* MaskedCasterPipeline( const DataDrivenMaterial& caster, MeshVertexPath path,
                                                MeshPass pass );
        // The shared caster pipelines of shadow view @p view, by path, and the cell pass they draw: ShadowDepth
        // for a cascade or local view, VirtualShadowDepth for a virtual shadow map level. Null where that caster
        // is off.
        struct CasterPipelines
        {
            GraphicsPipeline* Static    = nullptr;
            GraphicsPipeline* Instanced = nullptr;
            GraphicsPipeline* Skinned   = nullptr;
            MeshPass          Pass      = MeshPass::ShadowDepth;
        };
        [[nodiscard]] CasterPipelines CasterPipelinesOf( uint32_t view );
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
        // (Skinned x GBuffer): the deferred path draws skinned meshes in the base pass (UE), bones from the view's
        // ObjectBones, so the composite shades them and the ray-traced sun trace sees their depth and normal; the
        // surface cache captures a skinned mesh's cards with the same state. Null if the shader or the cell is
        // missing, and SetupGBufferPass says skinned meshes are then absent from a deferred scene. The unowned
        // material is the skinned twin of m_GBufferUnownedMaterial (same reason).
        std::shared_ptr<Shader>             m_SkinnedGBufferShader;
        std::shared_ptr<GraphicsPipeline>   m_SkinnedGBufferPipeline;
        std::shared_ptr<DataDrivenMaterial> m_SkinnedGBufferUnownedMaterial;
        bool                              m_DeferredGeometry = false; // set true only while drawing the G-buffer pass
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
        struct CascadeCacheState
        {
            std::optional<uint64_t> StaticKey; // the key the cache was drawn with; nullopt = never drawn
            bool                    RedrawStatic = false; // this frame: the static layer is drawn
            bool                    Rebuild      = false; // this frame: the map is the cache copied (+ movable)
            bool                    Movable      = false; // this frame: a movable caster reaches the cascade
            bool MapHasMovable = false; // the map holds movable depth from the frame it was built
        };
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

        // Skinned shadow caster: the (Skinned x ShadowDepth) cell, which did not exist — the cascade pass
        // walked the static queue by name and a character cast nothing. One material per cascade, exactly
        // like the two above, and every skinned caster's pose packed into its single Bones buffer.
        // Optional — null if the Skinned.ShadowDepth cell is missing, and then skinned shadows are simply off.
        std::shared_ptr<GraphicsPipeline>      m_ShadowSkinnedPipeline;
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
        // This frame's VG-only issue of every level (SetVirtualShadowGeometry), consumed by AddVirtualShadowRasterPasses
        // into m_VsmGeometryDraws ("VSM: Raster VirtualGeometry"); while set, the levels' CPU walk leaves out the
        // submeshes whose clusters it draws (CpuWalkHidden).
        std::optional<MeshRendererDetail::BinnedIssue> m_VsmGeometry;
        MeshRendererDetail::MeshDrawList               m_VsmGeometryDraws;
        uint32_t                                       m_VsmRefusedGroups     = 0;
        uint32_t                                       m_VsmFallbackSubmeshes = 0;
        // Whether this frame / the last frame recorded the levels' cluster raster ("VSM: Raster VirtualGeometry",
        // AddVirtualShadowRasterPasses; ResetVirtualShadowGeometry shifts this into last): the page cache's signature
        // of a VG caster includes its clustered set under the last frame's raster, so a submesh moving between the
        // clusters and the CPU walk (a twin's pipeline arrives, DAG rows upload, the cut is refused or comes back)
        // re-renders its pages.
        bool m_VsmClusteredThisFrame = false;
        bool m_VsmClusteredLastFrame = false;
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
        // CollectVirtualShadowInvalidation's state from the last frame the map was drawn: the casters that stand
        // still, sorted by signature, and the boxes of the ones that move by themselves. The instance snapshots
        // the still ISMs' signatures name, kept alive so a freed snapshot's address is not reused by a new one
        // under the same signature (m_CascadeKeySnapshots says the same).
        std::vector<ShadowCasterEntry>                             m_VsmStillCasters;
        std::vector<VsmInvalidationBox>                            m_VsmMovingBoxes;
        std::vector<std::shared_ptr<const std::vector<glm::mat4>>> m_VsmKeySnapshots;
        // Per still ISM (by its signature): what each instance cast last frame, 0 = faded out by its cull
        // distance, else 1 + its LOD - both follow the main camera, and an instance whose state changed redraws
        // the pages under it.
        std::unordered_map<uint64_t, std::vector<uint32_t>> m_VsmIsmStates;
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
        std::vector<GenericMeshRenderData>                              m_GenericQueue;
        std::unordered_map<std::string, std::unique_ptr<DataDrivenMaterial>> m_GenericMaterials;

        // AL1-12: which material pipelines this renderer asked for and what they are doing; how far it has
        // read the process-wide on-load request list; the stand-in every not-yet-ready material draws with.
        MaterialPipelineTracker             m_MaterialPipelines;
        size_t                              m_MaterialRequestCursor = 0;
        size_t                                             m_CellRequestCursor     = 0;
        std::vector<MaterialPipelineRequests::CellRequest> m_PendingCellPrecache;
        uint32_t m_StandInDraws = 0; // see GetStandInDraws; reset per frame in DeclareGenericDraws
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
        // The same choice for a pipeline the caller already holds (the glass pass's per-cell pipelines); @p key
        // names it in the tracker and the log.
        [[nodiscard]] MaterialPipelineTracker::CellDraw
        ChoosePipelineDraw( const GraphicsPipeline* own, const std::string& key, const GraphicsPipeline* standIn );
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
        // The default surface's scene material table rows (the (Static x Forward) stand-in's), [0] receiving sun
        // shadows and [1] not (SyncGpuScene): the row any draw names while it stands in for a material whose cell
        // is not ready (static, instanced, skinned and glass stand-ins and the instanced spare all show the default
        // surface's defaults; every cell of one template declares one Materials layout).
        std::array<uint32_t, 2> m_StandInMaterialRows{};
        // The generic path's stand-in row: m_DefaultSurfaceMaterial's own (SyncGpuScene, whenever m_GenericQueue
        // is not empty), what a generic draw names while its pipeline compiles (BuildGenericDraws).
        std::optional<uint32_t> m_GenericStandInRow;

        // THE SCENE MATERIAL TABLE, the one source of every mesh draw's Materials[] row (GpuScene::AddMaterialRow).
        // SyncGpuScene registers the row of every (template, instance) a queued draw shades with BEFORE the scene's
        // upload, so the table is at its final size and content before any pass is declared; the draw paths only
        // look rows up and lend the table to the recording cell. Cleared at each SyncGpuScene.
        std::unordered_map<GpuDriven::MaterialRowKey, uint32_t, GpuDriven::MaterialRowKeyHash> m_FrameMaterialRows;
        // The surface and instance each scene material row was registered for (RegisterMaterialRow), indexed by row:
        // what ResolveBinnedPipeline turns a GpuDrawSlot's MaterialRow back into a cell with.
        struct RowSurface
        {
            const DataDrivenMaterial* Surface  = nullptr;
            const MaterialInstance*   Instance = nullptr;
        };
        std::vector<RowSurface> m_RowSurfaces;
        // What the DrawCommandCache holds per primitive (m_BinnedFeed) and what this sync gathered (scratch): a
        // primitive is re-added only when its entry differs, removed when no record named it.
        struct BinnedPrimitiveFeed
        {
            const Desert::Mesh*   Mesh = nullptr;
            MeshGeometryBinding   Geometry;     // a re-upload to a new pool range rebins like a mesh change
            std::vector<uint32_t> MaterialRows; // per submesh
            uint32_t              FirstInstance     = 0;
            uint32_t              InstanceCount     = 1;
            uint64_t              HiddenSubmeshMask = 0;
            uint32_t              Flags             = 0;
            MeshVertexPath        Path              = MeshVertexPath::Static;
            // A generic (m_GenericQueue) draw: it casts through the shared opaque caster whatever its program
            // (MeshRendererShadow.cpp, the generic casters), and its view passes answer Fallback when set (a program
            // with no Surface block, a slot material's own program: no `.Binned` permutation).
            bool                   Generic  = false;
            GpuDriven::BinFallback Fallback = GpuDriven::BinFallback::None;
            // An ISM whose wind sways it: a caster of the cascade cache's movable layer (BinFallback::MovableCaster in
            // ShadowCascade), drawn every frame over the copied cache on the CPU.
            bool Sways = false;
            bool                  operator==( const BinnedPrimitiveFeed& ) const = default;
        };
        std::unique_ptr<GpuDriven::DrawCommandCache>                       m_DrawCommandCache;
        // This frame's issued (pass, view)s (SetFrameBinned), forgotten by BeginFrameBins.
        struct FrameBinnedView
        {
            GpuDriven::MeshPass                       Pass    = GpuDriven::MeshPass::GBuffer;
            uint32_t                                  ViewKey = kBinnedMainView;
            MeshRendererDetail::BinnedIssue           Issue;
            // Phase 2's issue (SetFramePostOcclusion): Issue with the post-occlusion cull's draws.
            std::optional<MeshRendererDetail::BinnedIssue> PostOcclusion;
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
        // The ISM queue's primitives (GpuSceneRecordRows::FieldPrimitives, per m_InstancedQueue entry) the feed saw.
        std::vector<GpuDriven::PrimitiveIndex> m_FieldPrimitives;
        [[nodiscard]] const FrameBinnedView* FrameBinned( GpuDriven::MeshPass pass, uint32_t viewKey ) const;
        // Attaches (pass, view)'s issue to @p list when it is active this frame.
        void AttachBinned( MeshRendererDetail::MeshDrawList& list, GpuDriven::MeshPass pass, uint32_t viewKey ) const;
        std::unordered_map<GpuDriven::PrimitiveIndex, BinnedPrimitiveFeed> m_BinnedFeed;
        std::unordered_map<GpuDriven::PrimitiveIndex, BinnedPrimitiveFeed> m_ScratchBinnedFeed;
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
        // The PSO a submesh of @p primitive draws with in @p pass from material row @p materialRow: the pass's shared
        // state with the material's `.Binned` cell (GBuffer / Forward: the template's cell, two-sided as
        // CullPermutation; ShadowCascade / VirtualShadow: ShadowCasterShaderFor's ShadowDepth / VirtualShadowDepth cell,
        // the default surface's for an opaque one, on the cascades' / the VSM's caster state).
        // No pipeline + BinFallback::None = not drawn in the pass (translucent; DepthPrepass draws no surface); + a
        // fallback = drawn there on the CPU path for that reason (no cell, bound textures, a generic program, a
        // movable caster).
        GpuDriven::PassPipelineResolution ResolveBinnedPipeline( GpuDriven::MeshPass       pass,
                                                                 GpuDriven::PrimitiveIndex primitive,
                                                                 uint32_t                  materialRow );
        const GraphicsPipeline* BinnedCellPipeline( const std::string& cellShader, const GraphicsPipeline* passState,
                                                    bool twoSided );
        // DrawCommandCache's GroupPipelineResolver: @p group's PSO with the `.VirtualGeometry` permutation of its
        // Static / Instanced `.Binned` cell (GBuffer / Forward / ShadowCascade / VirtualShadow), null when it declares
        // none (DeclaresVirtualGeometryTwin: another path, a translucent template) or no pipeline could be made.
        const GraphicsPipeline* ResolveVirtualGeometryPipeline( GpuDriven::MeshPass pass, const GraphicsPipeline& group );
        // THE ONE RULE "this binned cell's template has a `.VirtualGeometry` twin": the twin cell
        // (VirtualGeometryCellOf) is registered. ShaderService registers every pass a template's parse emits when the
        // template registers (an uncompiled one too), and DShaderParser emits the twin for every non-translucent
        // template; so a missing name is a template WITHOUT the permutation (a fallback), and a registered one with
        // no pipeline is a refusal.
        [[nodiscard]] static bool DeclaresVirtualGeometryTwin( const std::string& binnedCell );
        // The view's carrier of @p shader's scene blocks (m_BinnedCarriers), written for this frame; null + @p error
        // when it has no material table.
        const MaterialExecutor* PrepareBinnedCarrier( const std::shared_ptr<Shader>& shader, uint32_t viewKey,
                                                      const SceneFrameBinding& frameState, std::string& error );
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
        std::vector<uint32_t>                m_InstancedGpuInstances; // see GetInstancedGpuInstances
        std::vector<uint32_t>                m_OtherGpuInstances;     // see GetOtherGpuInstances
        std::vector<GpuInstancedRenderData>  m_GpuInstancedQueue;
        // The (material copy, its instance) of each GPU-instanced section and pass/cascade, keyed by the view key.
        std::unordered_map<uint32_t, MaterialInstancePtr> m_GpuInstancedInstances;
        uint64_t                                          m_GpuInstancedStamp = 0;
        // MeshRendererGpuInstanced.cpp: the GPU-instanced sections into a geometry list (the pass's instanced
        // cell) and into a cascade's movable list (their own masked caster, or the shared instanced caster).
        void BuildGpuInstancedDraws( MeshRendererDetail::MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                     MeshPass pass, const SceneFrameBinding& frameState );
        // `cascade` keys the caster cell (one per section x cascade); `shadowView` is the view drawn (the cascade
        // itself, or its movable layer kCascadeMovableViewBase + cascade over the cache) and names its light
        // camera.
        void BuildGpuInstancedShadowDraws( uint32_t cascade, uint32_t shadowView,
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
        };
        struct InstancedDraw
        {
            // Fed by BOTH the auto-batched statics (a StaticMesh) and the ISM queue (any Mesh), so it
            // is the wider of the two — a draw call only ever needs what Mesh already carries.
            Desert::Mesh*       Mesh          = nullptr;
            uint32_t            InstanceCount = 0;
            uint32_t            FirstInstance = 0;
            uint32_t            MaterialIndex = 0;
            uint64_t            HiddenSubmeshMask = 0; // the batch's section group (one mask for every instance)
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
            std::vector<InstancedDraw>  Draws;
        };

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
            SubmeshMask         HiddenSubmeshMask{}; ///< see InstancedDraw::HiddenSubmeshMask (a VSM level's is wide)
            InstanceWindPush    Wind;         ///< see InstancedDraw::Wind
            float               BoundsPad = 0.0f; ///< WindBoundsPad of the wind above, cm (MeshDrawCommand)
        };

        // Every skinned pose drawn in one pass, packed end to end; each draw names its slice with a
        // BoneOffset push constant. ONE buffer per material per pass instead of one upload per draw,
        // which is what makes a shared skinned material correct — see Material::SetSkinnedBoneOffset.
        std::vector<glm::mat4>                   m_ScratchBones;
        std::vector<glm::mat4>      m_ScratchInstTransforms; // geometry + shadow instanced SSBOs
        // Per-batch LOD levels and the surviving ISM transforms. Members rather than locals for the
        // reason every accumulator in this file is one: their capacity persists, so a frame that culls
        // 49 000 instances out of 49 152 allocates nothing.
        std::vector<uint32_t>                    m_ScratchLodLevels;
        std::vector<glm::mat4>                   m_ScratchIsmVisible;
        uint32_t                                 m_IsmInstancesDrawn = 0; // see GetIsmInstancesDrawn
        // The instanced batches of ONE geometry pass, one entry per recording material. Reused BY INDEX
        // rather than cleared, so the inner vectors keep their capacity across frames — clearing the
        // outer vector would destroy them and hand the steady state an allocation per material per frame.
        // `m_ScratchInstSetCount` is how many of them this frame is using.
        //
        // INDIRECT, AND THAT IS THE POINT AND NOT AN OVERSIGHT. The accumulation hands out a POINTER to
        // one set and then keeps filling it while later groups may ask for sets of their own; held by
        // value, the first `emplace_back` that grows this vector would move every set and leave that
        // pointer dangling — a use-after-free that no frame would show, because the freed memory is the
        // vector this thread just wrote. The control flow happens not to interleave the two today, which
        // is exactly the kind of "safe for now" that the next edit in DrawStaticMeshes turns into a
        // corrupted draw list. A unique_ptr costs one indirection per access and makes the address
        // stable by construction.
        std::vector<std::unique_ptr<InstancedBatchSet>> m_ScratchInstSets;
        std::size_t                                     m_ScratchInstSetCount = 0;
        std::vector<ObjDraw>        m_ScratchSingles;
        std::vector<ShadowBatch>    m_ScratchShadowBatches;
        // A per-object caster: its entry and the submeshes it hides (the entry's, plus a VSM level's clustered ones).
        struct ShadowSingle
        {
            const StaticMeshRenderData* Data   = nullptr;
            SubmeshMask                 Hidden{};
        };
        std::vector<ShadowSingle> m_ScratchShadowSingles;
        // Masked casters of one cascade: per cascade copy, its rows / instance transforms / bone poses end to
        // end (each uploaded once before the copy's draws are recorded), and the draws naming their slice.
        struct MaskedCasterSet
        {
            DataDrivenMaterial*    Caster = nullptr;
            std::vector<glm::mat4> Transforms; // Instanced path: InstanceTransforms; Skinned path: Bones
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
        std::unordered_map<std::string, std::shared_ptr<GraphicsPipeline>>
                                                                  m_MaskedCasterPipelines; // null = refused
        std::vector<GenericDraw>                 m_ScratchGenericDraws;
        std::vector<DataDrivenMaterial*>         m_ScratchGenericRecorders; // distinct recording materials
        float                                                     m_WorldTimeSeconds = 0.0f;
        ParallaxQuality                                           m_ParallaxQuality;
        // SyncGpuScene's CPU scratch (capacity kept) and the scene's GPU scene it last synced into.
        std::vector<MotionRecord>          m_ScratchRigidRecords;
        std::vector<MotionRecord>          m_ScratchSkinnedRecords;
        std::vector<MotionFieldRecord>     m_ScratchFieldRecords;
        GpuDriven::GpuSceneRecordRows      m_ScratchGpuSceneRows;
        const GpuDriven::GpuScene*         m_GpuScene = nullptr;
        // The GPU skin cache's per-view side (SetSkinCacheEnabled, SkinCacheReads, SkinCacheBonesImport).
        std::vector<RDG::BufferRef> m_SkinCacheReads;
        RDG::ExternalBuffer         m_SkinCacheBonesImport;
        bool                        m_SkinCacheEnabled = true;
    };
} // namespace Desert::Graphic::System
