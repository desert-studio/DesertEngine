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
#include <Engine/Graphic/View/ObjectMotionRows.hpp>
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
#include <memory>
#include <array>
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
            uint64_t                HiddenSubmeshMask = 0;
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
        };

        // THE frame's draw list of one lit mesh node. The material (hence shader and route fill) of every draw is
        // chosen ONCE, by Build* in the node's setup; Declare declares one binding block per distinct (material
        // executor, recording pipeline's shader) pair of the list (the layout kept for the shader the draws RECORD
        // with - `Pipeline->GetSpecification().Shader`, re-derived only on its reload, never per frame - the
        // executor's route fill, and the scene/view inputs the layout has slots for), so ValidatePassBindings
        // judges every draw before anything is recorded; Record (the exec) walks the same list with the block each
        // draw's executor was declared as. A build that failed is kept and is Record's error, before any draw.
        class MeshDrawList
        {
        public:
            void Clear();
            // A draw without pipeline, mesh or material (or whose material has no shader) is refused here.
            void               Add( MeshDrawCommand command );
            void               Fail( std::string error );
            [[nodiscard]] bool IsEmpty() const
            {
                return m_Commands.empty();
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

        private:
            // One declared block: the executor whose route fills it and the pipeline of its first draw, whose
            // shader keys its layout (every draw of the block records with a pipeline of that same shader).
            struct Block
            {
                const MaterialExecutor* Material = nullptr;
                const GraphicsPipeline* Pipeline = nullptr;
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
            // Kept across frames (Clear drops only the layouts of destroyed shaders). Mutable: Declare is const.
            mutable ShaderBindingLayoutSet m_Layouts;
        };
    } // namespace MeshRendererDetail

    struct MeshRenderData
    {
        // The entity that owns the draw: the key of its previous transform in the view's MotionHistory.
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
        struct StaticMeshRenderData
        {
            // The entity that owns the draw: the key of its previous transform in the view's MotionHistory.
            uint32_t Entity = 0;
            uint32_t MotionPart = 0; // a stable part of the entity (MotionRecord::Part), 0 = by submission
            // This frame's row in the view's ObjectMotions buffer (BuildObjectMotions); the view-pass cells read
            // the primitive's world, previous world and bone offsets from it through the push PrimitiveIndex.
            uint32_t                  MotionRow = 0;
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
            // The entity that owns the draw: the key of its previous transform in the view's MotionHistory.
            uint32_t Entity = 0;
            // This frame's row in the view's ObjectMotions buffer (BuildObjectMotions); the view-pass cells read
            // the primitive's world, previous world and bone offsets from it through the push PrimitiveIndex.
            uint32_t                   MotionRow = 0;
            class Desert::SkinnedMesh* Mesh      = nullptr;
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
            uint64_t                                      HiddenSubmeshes = 0; // the other slots' sections
            std::shared_ptr<const std::vector<glm::mat4>> Transforms; // snapshot of InstanceTransforms
            bool                                          CastShadows = true;
            bool                                          CastContactShadow = true; // UE bCastContactShadow
            // The field's foliage type's CullDistance (FO-5); {0, 0} for an ISM that is not foliage.
            InstanceCullDistance CullDistance;
            // The field's foliage type's wind (FO-7) at this frame's gameplay time; still for any other ISM.
            InstanceWind Wind;
            // Where this field's per-instance rows start in GetInstancedMotionRows() (BuildObjectMotions):
            // instance i is GPU-scene row GetInstancedMotionRows()[FirstMotionRow + i] - its TLAS custom index and
            // its GI card-table row (one row per instance, 1:1 with the TLAS instance).
            uint32_t FirstMotionRow = 0;
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
            // Stable per section across frames: the view-variant key (several cascades: Key * kMaxCascades + c).
            uint32_t Key = 0;
            // GrassBlade only: the shared BLAS of each strip level (GrassBladeRayTracing.hpp), indexed by submesh;
            // empty when the view has no TLAS. Owned by the grass renderer, this frame.
            std::span<const RayTracing::RayTracingGeometryResource* const> BladeGeometry;
        };

        // A static mesh drawn with a generic data-driven material. Two producers:
        //  - MaterialComponent (Shader Override) on the whole entity: ShaderName + Overrides drive a
        //    shader-keyed shared material (SlotMaterial == nullptr, all submeshes).
        //  - v3 per-slot materials: SlotMaterial points at the slot's own DataDrivenMaterial (asset
        //    params already applied) and VisibleSubmeshMask limits the draw to that slot's submeshes.
        struct GenericMeshRenderData
        {
            // The entity that owns the draw: the key of its previous transform in the view's MotionHistory.
            uint32_t Entity = 0;
            // A stable part of the entity (MotionRecord::Part, e.g. a fracture piece), 0 = the entity itself. A
            // slot-material draw of a part carries the part its lit draw carries, so both read one motion row.
            uint32_t MotionPart = 0;
            // This frame's row in the view's ObjectMotions buffer (BuildObjectMotions); the view-pass cells read
            // the primitive's world, previous world and bone offsets from it through the push PrimitiveIndex.
            uint32_t                   MotionRow = 0;
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

        // The view's motion rows for this frame — UE's GPUScene primitive data: ONE GpuObjectMotion per drawn
        // primitive (key {Entity, slot}; the static, generic and slot-material records of one entity share
        // slot 0, the skinned records of an entity are slots 0..n-1 in submission order), its World, the world
        // @p motion says the view drew it with last frame, and for a skinned primitive where this frame's palette
        // and the previous one start in the view's ObjectBones buffer (both frames' palettes, end to end). Writes
        // the row index into each queued record (MotionRow) and uploads both per-(frame x view) buffers at their
        // final size, so CaptureFrameState hands every view pass the buffers its draws will read. Called by
        // SceneRenderer once per frame per view after the queues are complete and before any pass is declared.
        // Errors: a buffer that could not be created or written (the view draws nothing this frame: its passes
        // would read rows that are not this frame's).
        // @p otherRigid: rigid primitives drawn by another renderer of the view (the terrain renderer's landscape
        // tiles) that still need a GPU-scene row - their TLAS custom index; their rows, in order, are
        // GetOtherMotionRows().
        // @p skinCache (the scene's GPU skin cache, null = none: every skinned primitive skins in the vertex
        // stage) writes each skinned row's cached-streams address (RayTracingSkinCache::PrepareRows), then - after
        // the ObjectBones upload, before the rows' - runs the scene's skin step for what no earlier view of the
        // absolute frame @p frame skinned (RayTracingSkinCache::SkinFrame: its own graph, executed before any view
        // graph). A skin step that fails is logged and its rows lose the address; the view still draws.
        [[nodiscard]] Common::BoolResultStr BuildObjectMotions( MotionHistory&                   motion,
                                                                std::span<const MotionRecord>    otherRigid,
                                                                RayTracing::RayTracingSkinCache* skinCache,
                                                                uint64_t                         frame );

        // The rows of BuildObjectMotions' @p otherRigid records, in their order. Valid until the next build.
        [[nodiscard]] std::span<const uint32_t> GetOtherMotionRows() const
        {
            return m_OtherMotionRows;
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

        // This frame's static queue, rows assigned (BuildObjectMotions): what the view's TLAS instances come from
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
        // This frame's ISM / foliage fields; with GetInstancedMotionRows, one TLAS instance per field instance.
        [[nodiscard]] std::span<const InstancedMeshRenderData> GetInstancedQueue() const
        {
            return m_InstancedQueue;
        }
        // This frame's GPU-instanced sections (the landscape grass): their TLAS rows are written on the GPU.
        [[nodiscard]] std::span<const GpuInstancedRenderData> GetGpuInstancedQueue() const
        {
            return m_GpuInstancedQueue;
        }
        // Every queued field's per-instance GPU-scene rows, end to end (InstancedMeshRenderData::FirstMotionRow).
        [[nodiscard]] std::span<const uint32_t> GetInstancedMotionRows() const
        {
            return m_InstancedMotionRows;
        }
        // This frame's rows (indexed by a queued record's MotionRow) and the view's ObjectBones
        // (BuildObjectMotions): the skin cache skins from the same palette at the same BoneOffset the raster does
        // - one source of the pose - and the global distance field's objects read a primitive's previous world
        // from them.
        [[nodiscard]] const ObjectMotionRows& GetObjectMotionRows() const
        {
            return m_ScratchMotionRows;
        }
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetObjectBones() const
        {
            return m_ObjectBones;
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
        // u_ShadowCascades (the opaque generic ones read the screen mask through their
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

        // EVERY SHADOW VIEW THIS RENDERER DRAWS, by one index: 0..kMaxCascades-1 are the sun's cascades, the
        // rest the point and spot lights' views (LocalShadows.hpp). The per-view caster state below
        // (m_ShadowMaterial, m_CascadeCasterVP, ...) is indexed by it, so one caster walk
        // (BuildShadowCascadeDraws) serves a cascade and a cube face alike.
        // After those, one view per cascade for its MOVABLE layer: drawn in a pass of its own, possibly in the
        // same frame as the cascade's static layer, so it needs caster materials (light-matrix UBO, instance and
        // bone SSBOs) of its own — two passes through one material would both read whichever upload came last.
        static constexpr uint32_t kCascadeMovableViewBase = kMaxShadowCascades + kMaxLocalShadowViews;
        // After those, one view per clipmap level of the sun's virtual shadow map ("VSM: Raster"): its casters
        // draw into the physical pool through the VirtualShadowDepth cells (CasterPipelinesOf), each level with
        // caster materials of its own for the same reason as the movable views.
        static constexpr uint32_t           kVsmViewBase    = kCascadeMovableViewBase + kMaxShadowCascades;
        // Last, the rain occlusion's top-down view (RainOcclusionPass): the cascades' caster cells, no bias.
        static constexpr uint32_t           kRainOcclusionView = kVsmViewBase + kVsmClipmapLevelCount;
        static constexpr uint32_t           kMaxShadowViews    = kRainOcclusionView + 1u;
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
        // shader's reflected layout, its dedicated material as the other route), each carrying u_SceneColor =
        // @p sceneCopy (this frame's snapshot of the opaque scene, FrameTransients::SceneColorCopy, sampled for
        // refraction) and the scene/view inputs of @p view the cell's shader has a slot for. Declares nothing
        // (and the node draws nothing) when no translucent object is visible.
        // @p frontLayer: this frame's front layer reflection and depth (Reflections.TranslucentFrontLayer), bound as
        // u_FrontLayerReflection / u_FrontLayerDepth wherever the cell's shader has the slots - the graph's black
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
        // @p overdraw - the accumulation the node "Debug: Overdraw" wrote - as u_Overdraw; the exec draws through
        // it.
        void DeclareOverdrawResolve( RDG::PassBuilder& pass, RDG::TextureRef overdraw );
        [[nodiscard]] Common::BoolResultStr RecordOverdrawResolve( const RDG::PassContext& context ) const;

        const std::shared_ptr<Framebuffer>& GetOverdrawFramebuffer() const
        {
            return m_OverdrawFB;
        }
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

        /// ISM instances the last geometry pass drew, after the frustum and the cull distance: what a
        /// foliage type's CullDistance is measured by (FO-5). The shadow cascades are not counted.
        [[nodiscard]] uint32_t GetIsmInstancesDrawn() const
        {
            return m_IsmInstancesDrawn;
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
        std::unordered_map<MeshCellPipelineKey, std::shared_ptr<GraphicsPipeline>, MeshCellPipelineKeyHash>
             m_CellPipelines; // null = refused

        [[nodiscard]] GraphicsPipeline* WireframePipelineOr( GraphicsPipeline* fallback ) const
        {
#if DESERT_DEV_INSTRUMENTS
            return ( m_Wireframe && m_StaticWireframePipeline ) ? m_StaticWireframePipeline.get() : fallback;
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

        // HOW MANY OF THEM CARRY A MATRIX FROM THIS FRAME'S FIT — the number every SHADER-facing consumer
        // wants, and a different question from the one above. `GetCascadeCount` answers "what does this
        // renderer own" (the budget: what to allocate, what to show in Scene Settings); this answers "what
        // may be sampled right now". They are equal in the ordinary frame and diverge exactly when
        // UpdateCascades does not run to completion — no main camera, no directional light, a degenerate
        // fit — which is when publishing the budget hands the shader matrices nothing ever wrote.
        //
        // The min() is belt and braces against the two ever being written from different places again.
        [[nodiscard]] uint32_t GetValidCascadeCount() const
        {
            return m_FittedCascades < m_Shadow.CascadeCount ? m_FittedCascades : m_Shadow.CascadeCount;
        }

        // The budget this renderer was created with. Fixed after Initialize — the framebuffers are
        // allocated from it once.
        [[nodiscard]] const ShadowQuality& GetShadowQuality() const
        {
            return m_Shadow;
        }
        // RE-ALLOCATE THE CASCADE MAPS TO A NEW BUDGET — the Shadows quality level changed (SceneRenderer, on a
        // QualityState generation change, never per frame). Waits for the device to go idle, releases the
        // cascade framebuffers, caster materials, caster pipelines and the attachment lease, then runs
        // SetupShadowPass again on the new budget. False when the new shadow pass could not be set up (logged).
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
        // CSM data the deferred lighting pass needs to shadow the sun (same source the forward material uses).
        const glm::mat4* GetCascadeViewProj() const        { return m_CascadeVP; }

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
        const glm::vec4& GetCascadeWorldPerTexel() const   { return m_CascadeWorldPerTexel; }
        // Per cascade, the world length the [0,1] light depth spans (CascadeFit::DepthRange): what turns a depth
        // difference in the map into centimetres for the soft shadow's penumbra (Mesh/CascadedShadow.glslh).
        const glm::vec4& GetCascadeDepthRange() const
        {
            return m_CascadeDepthRange;
        }
        // tan of half the sun's angular diameter (DirectionalLightData::SourceAngle): penumbra width per
        // centimetre between occluder and receiver.
        [[nodiscard]] float GetSunSourceTanHalf() const;

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
        // The casters of shadow view @p cascade (a cascade, or a local view: kMaxShadowViews) into @p list.
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
        MeshRendererDetail::MeshDrawList m_GenericDraws; // "Deferred: Generic"
        MeshRendererDetail::MeshDrawList m_DepthNormalDraws; // "Deferred: Forward Depth/Normal"
        // "MeshShadowCascade<c>": the cascade's casters, built by its Declare (BuildShadowCascadeDraws).
        MeshRendererDetail::MeshDrawList m_CascadeDraws[kMaxCascades];
        // The cascade's non-mesh casters (m_ShadowCasters) its Declare declared, each with the index of its first
        // binding block (after m_CascadeDraws[c]'s); the exec records exactly these.
        std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>> m_CascadeCasters[kMaxCascades];
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
            // Static, generic and skinned: the view's ObjectMotions row (the view-pass cell's world, and the
            // skinned cell's bone palette).
            uint32_t MotionRow = 0;
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
        void TrackMaterialPipeline( const std::string& shaderName, const GraphicsPipeline& pipeline );
    public:
        // THE ONE PERMUTATION FUNCTION of a generic forward program on the deferred path, asked by every place
        // that builds or cooks it: "Deferred: Generic" (BuildGenericDraws), the prepass's readiness check
        // (GenericForwardPipelineReady) and the cook (MaterialProgramVariants, OverrideProgramVariants). An
        // opaque program reads DESERT_SUN_SHADOW_SCREEN_MASK when @p rayTracedSun and it samples the sun's shadow
        // (declares u_ShadowCascades), and DESERT_LOCAL_SHADOW_SCREEN_MASK when @p localShadowMasks (the view has
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
        std::unique_ptr<MaterialShadow>           m_ShadowMaterial[kMaxShadowViews];
        // The sampled cascade maps: ONE depth array, a layer per cascade, owned by this renderer across frames
        // and handed to each frame's graph (AcquireCascadeMaps). Empty until the first frame after the budget.
        RDG::ExternalTexture m_CascadeMaps;
        bool                 m_CascadeMapsBuilt = false;
        // The cache of each cascade's static casters (depth only, the map's format): drawn when its key changes,
        // copied into layer c of m_CascadeMaps whenever the map is rebuilt.
        std::shared_ptr<Framebuffer> m_CascadeStaticFB[kMaxCascades];
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
        CascadeCacheState                                          m_CascadeCache[kMaxCascades];
        MeshRendererDetail::MeshDrawList                           m_CascadeMovableDraws[kMaxCascades];
        std::vector<std::weak_ptr<IShadowCaster>> m_ShadowCasters; // non-mesh casters, AddShadowCaster

        // Instanced shadow caster: one pipeline + per-cascade instanced material (each owns the cascade's
        // light matrix UBO + an InstanceTransforms SSBO). Batched casters of one mesh collapse to a single
        // instanced draw per cascade. Optional — null if the Instanced.ShadowDepth cell is missing.
        std::shared_ptr<GraphicsPipeline>        m_ShadowInstancedPipeline;
        std::shared_ptr<Shader>                  m_ShadowInstancedShader;
        std::unique_ptr<MaterialShadowInstanced> m_ShadowInstancedMaterial[kMaxShadowViews];

        // Skinned shadow caster: the (Skinned x ShadowDepth) cell, which did not exist — the cascade pass
        // walked the static queue by name and a character cast nothing. One material per cascade, exactly
        // like the two above, and every skinned caster's pose packed into its single Bones buffer.
        // Optional — null if the Skinned.ShadowDepth cell is missing, and then skinned shadows are simply off.
        std::shared_ptr<GraphicsPipeline>      m_ShadowSkinnedPipeline;
        std::shared_ptr<Shader>                m_ShadowSkinnedShader;
        std::unique_ptr<MaterialShadowSkinned> m_ShadowSkinnedMaterial[kMaxShadowViews];

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

        // How many of m_CascadeVP below were written by THIS frame's fit (0 until UpdateCascades runs).
        // See GetValidCascadeCount for why this is not the same number as m_Shadow.CascadeCount.
        uint32_t m_FittedCascades = 0;

        // EVERY element identity, spelled element by element. `= { glm::mat4( 1.0f ) }` reads as "all
        // identity" and is not: it initializes element 0 and VALUE-initializes the rest, so cascades 1..3
        // began life as ZERO matrices, whose w is 0 and whose perspective divide is a division by zero in
        // the sampling shader. Nothing should ever read past m_FittedCascades — this is what the array
        // holds if something does.
        glm::mat4 m_CascadeVP[kMaxCascades] = { glm::mat4( 1.0f ), glm::mat4( 1.0f ), glm::mat4( 1.0f ),
                                                glm::mat4( 1.0f ) };
        static_assert( kMaxCascades == 4, "m_CascadeVP's initializer lists one identity per cascade" );
        // World-space size of one shadow-map texel per cascade (2*radius/res) — drives a cascade-correct
        // normal-offset/bias in the lit shader instead of the old fixed world-unit constants.
        glm::vec4                         m_CascadeWorldPerTexel    = glm::vec4( 1.0f );
        glm::vec4                         m_CascadeDepthRange       = glm::vec4( 1.0f );
        // The receivers sample with m_CascadeVP; the casters draw with m_CascadeCasterVP (the constant depth
        // bias folded in, CasterViewProj) and write the slope term from m_CascadeCasterBias through CameraUB.
        // Indexed by SHADOW VIEW (kMaxShadowViews): the cascades first, then the local views, whose matrices
        // UpdateLocalShadows writes with the light's bias (Graphic::LocalCasterBias, texels at the caster).
        std::array<glm::mat4, kMaxShadowViews>                      m_CascadeCasterVP = IdentityShadowViews();
        CascadeCasterBias                                           m_CascadeCasterBias[kMaxShadowViews]{};
        [[nodiscard]] static std::array<glm::mat4, kMaxShadowViews> IdentityShadowViews()
        {
            std::array<glm::mat4, kMaxShadowViews> views;
            views.fill( glm::mat4( 1.0f ) );
            return views;
        }

        // THE LOCAL LIGHTS' SHADOW STATE OF THIS VIEW. The atlas, its tiles and what each light's tiles hold are
        // the SCENE's (Graphic/LocalShadowCache.hpp, through SceneRenderer::GetFrameLocalShadows): every view of a
        // scene - the reflection captures' faces too - reads one cached map per light. What stays per view is its
        // table of rows over those tiles and the passes that redraw the stale ones.
        // This frame's table (one row per view, in view order) and the views it redraws.
        std::vector<LocalShadowViewPayload> m_LocalShadowRows;
        struct LocalShadowRedraw
        {
            uint32_t  View = 0; // shadow view index (kMaxCascades + row)
            AtlasTile Tile;
        };
        std::vector<LocalShadowRedraw>    m_LocalRedraws;
        // What PlanLocalShadowCache decides from: each light given views this frame, where it reaches.
        struct LocalShadowCachePlan
        {
            uint64_t               Light = 0; // its key in the scene's LocalShadowCache
            LocalShadowCasterLight Caster;    // its views, reach and detail view (FirstView - kMaxShadowCascades =
                                              // its first row of m_LocalShadowRows)
            uint64_t               Frame   = 0;     // the cache's frame the views were given in
            bool                   Primary = false; // given by a primary view (a level's viewport)
        };
        std::vector<LocalShadowCachePlan> m_LocalCachePlans;
        MeshRendererDetail::MeshDrawList  m_LocalDraws[kMaxLocalShadowViews];
        // Each local view's non-mesh casters its Declare declared, with the index of its first block (as
        // m_CascadeCasters for a cascade).
        std::vector<std::pair<std::weak_ptr<IShadowCaster>, uint32_t>> m_LocalCasters[kMaxLocalShadowViews];

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
            std::shared_ptr<VertexBuffer> BaseVertices;
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
        // The glass pass's stand-in: the default surface's (Static x Forward) cell with the translucent draw
        // state.
        TranslucentDraw* TranslucentStandIn();

        // UE-style Instanced Static Meshes (one entity = N instances). Folded into the shared instanced
        // pipeline/SSBO alongside the auto-batched static meshes (geometry + shadow passes).
        std::vector<InstancedMeshRenderData> m_InstancedQueue;
        std::vector<uint32_t>                m_InstancedMotionRows; // see GetInstancedMotionRows
        std::vector<uint32_t>                m_OtherMotionRows;     // see GetOtherMotionRows
        std::vector<GpuInstancedRenderData>  m_GpuInstancedQueue;
        // The (material copy, its instance) of each GPU-instanced section and pass/cascade, keyed by the view key.
        std::unordered_map<uint32_t, MaterialInstancePtr> m_GpuInstancedInstances;
        uint64_t                                          m_GpuInstancedStamp = 0;
        // MeshRendererGpuInstanced.cpp: the GPU-instanced sections into a geometry list (the pass's instanced
        // cell) and into a cascade's movable list (their own masked caster, or the shared instanced caster).
        void BuildGpuInstancedDraws( MeshRendererDetail::MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                     MeshPass pass, const SceneFrameBinding& frameState );
        void BuildGpuInstancedShadowDraws( uint32_t cascade, MeshRendererDetail::MeshDrawList& list );

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
            Core::Formats::MaterialParamRow Row; // this object's effective Materials[] row
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
            std::vector<glm::vec4>       Materials; // Materials[] rows, end to end
            std::vector<InstancedDraw>  Draws;
        };

        // One recorded generic draw, resolved in DrawGenericMeshes' first pass and executed in its third.
        // The passes are separate because every row has to be uploaded at FINAL size before the first draw
        // is recorded — growing a storage buffer reallocates the VkBuffer under a draw already recorded
        // against the old one — and only the first pass knows how many rows there will be.
        struct GenericDraw
        {
            const GenericMeshRenderData*      Data     = nullptr;
            DataDrivenMaterial*               Material = nullptr;
            GraphicsPipeline*                 Pipeline = nullptr; // held by the pipeline cache / m_CellPipelines
            uint32_t                          Row = 0;
        };
        // One material's rows, end to end: Count rows of (parameter slots per row) vec4s.
        struct MaterialRows
        {
            std::vector<glm::vec4> Bytes;
            uint32_t               Count = 0;
        };
        struct ShadowBatch
        {
            Desert::Mesh*       Mesh     = nullptr; ///< see InstancedDraw::Mesh — the same two producers
            uint32_t            Count = 0;
            uint32_t            First = 0;
            uint32_t            LodLevel = 0; ///< see InstancedDraw::LodLevel — same omission, same fix
            uint64_t            HiddenSubmeshMask = 0; ///< see InstancedDraw::HiddenSubmeshMask
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
        std::vector<glm::vec4>                   m_ScratchGpuMaterials; // per-object Materials[] rows, end to end
        std::vector<ObjDraw>        m_ScratchSingles;
        std::vector<ShadowBatch>    m_ScratchShadowBatches;
        std::vector<const StaticMeshRenderData*> m_ScratchShadowSingles;
        // Masked casters of one cascade: per cascade copy, its rows / instance transforms / bone poses end to
        // end (each uploaded once before the copy's draws are recorded), and the draws naming their slice.
        struct MaskedCasterSet
        {
            DataDrivenMaterial*    Caster = nullptr;
            std::vector<glm::vec4> Rows;
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
            // Skinned path: the primitive's row in the view's ObjectMotions (PrimitiveIndex), whose skin-cache
            // fields the cell fetches the cached pose by; a row without an address skins from the copy's bones.
            uint32_t          MotionRow         = 0;
            uint32_t          LodLevel  = 0;
            uint64_t          HiddenSubmeshMask = 0; // the caster's section group, as its lit draw
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
        std::vector<std::pair<DataDrivenMaterial*, MaterialRows>> m_ScratchGenericRows;
        float                                                     m_WorldTimeSeconds = 0.0f;
        ParallaxQuality                                           m_ParallaxQuality;
        // BuildObjectMotions' output: the rows and both frames' palettes (CPU scratch, capacity kept) and the
        // per-(frame x view) buffers they are uploaded into (StorageBuffer::Create non-persistent: one copy per
        // frame in flight). Created on the first frame, grown by SetData.
        std::vector<MotionRecord>                       m_ScratchMotionRecords;
        ObjectMotionRows                                m_ScratchMotionRows;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ObjectMotions;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ObjectBones;
        // The GPU skin cache's per-view side (SetSkinCacheEnabled, SkinCacheReads, SkinCacheBonesImport).
        std::vector<RDG::BufferRef> m_SkinCacheReads;
        RDG::ExternalBuffer         m_SkinCacheBonesImport;
        bool                        m_SkinCacheEnabled = true;
    };
} // namespace Desert::Graphic::System
