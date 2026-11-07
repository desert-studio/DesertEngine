#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassBladeRayTracing.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassFieldLens.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic
{
    struct ViewFrame;
}

namespace Desert::Graphic::System
{
    class MeshRenderer;

    // GPU-COST-21: the cascade views a variety keeps a list of its own for, at most (every shadowed directional
    // light's cascades; MeshRenderer::kCascadeViews). GrassGenerate.shader CascadePlanes and GrassCull.shader
    // kCascadeViews are sized by it.
    inline constexpr uint32_t kGrassCascadeLists = SceneResources::kMaxCascadeViews;

    // One landscape root's heights and grass-layer weights on ONE sample grid over all its tiles (tiles share
    // their edge samples), packed as the generator reads it (Programs/Grass/GrassGenerate.shader, FieldData):
    //   [0] samples X, [1] samples Z, [2] grass layer count, [3] 0, [4] origin X (float bits), [5] origin Z,
    //   [6] spacing cm, [7] 0, then one float world height per sample (kGrassNoHeight where no tile is loaded),
    //   then per grass layer one byte of weight per sample, four to a word.
    struct GrassFieldData
    {
        std::vector<uint32_t> Words;
        uint64_t              Revision = 0; // changes whenever Words do
    };
    inline constexpr float kGrassNoHeight = -2.0e30f;

    // UE FGrassVariety's placement, resolved by the ECS (LandscapeECSSystem) from the layer info's variety: units
    // are the asset's (Density per 1000 x 1000 cm, cm distances, Z up).
    struct GrassPlacement
    {
        float     Density           = 400.0f;
        float     PlacementJitter   = 1.0f;
        float     StartCullDistance = 0.0f;
        float     EndCullDistance   = 10000.0f;
        glm::vec2 AllowedDensityRange{ 0.0f, 1.0f };
        // The variety's world patches (Assets::Serialization::GrassPatches): Coverage 1 = everywhere.
        float     PatchCoverage = 1.0f, PatchSize = 1500.0f, PatchSoftness = 0.15f;
        uint32_t  PatchGroup = 0;
        uint32_t  Scaling    = 0; // 0 uniform, 1 free, 2 lock XY
        glm::vec2 ScaleX{ 1.0f }, ScaleY{ 1.0f }, ScaleZ{ 1.0f };
        bool      WeightAttenuatesMaxScale  = false;
        float     MaxScaleWeightAttenuation = 0.5f;
        bool      RandomRotation            = true;
        bool      AlignToSurface            = true;
        bool      CastDynamicShadow         = true;
    };

    // A procedural blade's shape (Assets::Serialization::GrassBlade, colours linear), resolved by the ECS. The
    // vertex stage reads the shape, colours and bend spreads (GrassBladeShapeGpu); the generator the clump, the
    // tilt, the facing jitter and the levels.
    struct GrassBladeDraw
    {
        float Height = 45.0f, Curvature = 0.35f, CurvatureSpread = 0.4f;
        float BendSpread = 0.0f, Tilt = 0.0f, FacingJitter = 0.0f, Stiffness = 0.3f;
        /// The material atlas's cards (u0, v tip, u1, v root) the blades draw from, and the widest one's width,
        /// cm at scale 1 (the bounds).
        std::vector<glm::vec4> Cards;
        float                  MaxWidth = 0.0f;
        float                  Clumping = 0.5f, ClumpSize = 35.0f, ClumpPull = 0.0f;
        float    Scatter = 0.0f, Patchiness = 0.0f, PatchSize = 800.0f;
        uint32_t Segments       = 6;
        float    LodDistance    = 700.0f;
        float    MinScreenWidth = 1.5f; // render pixels; 0 = no floor (Assets::Serialization::GrassBlade)
    };

    // The generator reads at most this many of a blade's cards (GrassParamsGpu::CardWidths, two vec4s).
    inline constexpr size_t kGrassBladeMaxCards = 8u;

    // The mean blade's width (cm) at the ScaleX range's middle @p scaleX: the mean of its first
    // kGrassBladeMaxCards cards' widths (Height x du / dv) - the width the screen-width floor's distance is
    // measured by.
    [[nodiscard]] float GrassBladeMeanWidth( const GrassBladeDraw& blade, glm::vec2 scaleX );

    // A grass mesh's bounding radius about its root (cm at scale 1, from its local bounds): its bounds' half
    // diagonal plus the centre's horizontal offset - what the far LOD measures an instance's screen size by
    // (GrassRenderer::PrepareVariety) and the ground's far cover the meshes' far LOD distance by
    // (LandscapeECSSystem: TerrainInstance::GrassCoverFarLodRadius).
    [[nodiscard]] float GrassFarLodRadius( const Common::Math::AABB& bounds );

    struct GrassSectionDraw
    {
        MaterialInstancePtr Material;
        uint64_t            HiddenSubmeshes = 0;
    };

    struct GrassVarietyDraw
    {
        uint64_t Key   = 0; // stable: (root, layer, grass type, variety)
        uint32_t Layer = 0; // the grass layer's index in the field
        // Kind Mesh: the mesh every instance draws. Kind Blade: null - the renderer draws its blade strip.
        class Desert::Mesh* Mesh = nullptr;
        // Kind Blade: the shape; Sections then holds the one blade material.
        std::optional<GrassBladeDraw> Blade;
        std::vector<GrassSectionDraw> Sections;
        GrassPlacement                Placement;
        InstanceWind                  Wind;
    };

    struct GrassFieldDraw
    {
        uint64_t                              Root = 0;
        std::shared_ptr<const GrassFieldData> Field;
        std::vector<GrassVarietyDraw>         Varieties;
    };

    // UE's landscape grass (ALandscapeProxy::UpdateGrass) on the GPU: every frame, for the main view, each variety
    // of each landscape grows its instances on a world grid around the camera (GrassGenerate, one thread per cell,
    // culled to the view and faded out to EndCullDistance) into a buffer of its own, and its sections are drawn
    // through the mesh renderer's instanced cells - G-buffer or forward, and the cascades - by indirect draws
    // (GpuInstances.hpp). Nothing is stored in the level and nothing is read back. A Blade variety draws the
    // renderer's own blade strip (one per segment count, a level per halving) through the GrassBlade vertex path:
    // the generator sorts its instances into one bucket per level by distance, so a far blade has fewer segments.
    // GRASS-VIS: the previous frame's furthest HZB GrassGenerate culls the CAMERA's list by (GpuCull_IsOccludedByHzb,
    // the instance cull's test); the cascades and the TLAS keep every kept instance (UE: occlusion per view). Hzb
    // invalid: nothing is tested and Neutral is bound in its place; the camera still draws a list of its own, culled
    // by its side planes, while the full list is swept along the shadowed lights (GRASS-VIS-6), else the full list.
    struct GrassOcclusion
    {
        RDG::TextureRef Hzb{};     // last frame's furthest HZB of the view (SceneRenderer::FramePrevHzb)
        RDG::TextureRef Neutral{}; // bound when Hzb is invalid (System.Black)
        glm::mat4       PrevViewProjection{ 1.0f }; // what Hzb was built through, unjittered
        glm::uvec3      Extent{ 0u };               // Hzb's mip-0 size and mip count
    };

    class GrassRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override;

        // BY REFERENCE (SceneRenderer::SubmitLandscapeGrass, REVIEW-FIX R6): @p draw is the recorded command's,
        // which outlives the bracket; the queue holds it until ClearQueue (EndScene). Nothing of it is copied
        // per capture face or per view.
        void Submit( GrassFieldDraw&& ) = delete; // the queue keeps a reference: a temporary would dangle
        void Submit( const GrassFieldDraw& draw )
        {
            m_Queue.emplace_back( draw );
        }
        // The Foliage quality every later AddFrame grows its varieties under (GrassQuality).
        void SetQuality( const GrassQuality& quality )
        {
            m_Quality = quality;
        }

        void ClearQueue()
        {
            m_Queue.clear();
        }

        // Before the cascades and the geometry: the generation nodes of this frame's view, and the sections handed
        // to @p meshes. A view that is not the main one (a capture face) grows nothing. @p rayTracing: the view
        // builds a TLAS this frame (RayTracingSceneRequired): the Blade varieties' level BLASes are prepared and
        // handed with the sections (GrassBladeRayTracing.hpp). @p occlusion: what the camera cull tests the
        // camera's list against. GRASS-VIS-24 (UE: grass built once per component, culled per view every frame):
        // a variety generates only when its inputs change (VarietyGpu::GenerateKey); a tested frame's camera list
        // depends on last frame's depth, so the camera cull runs every such frame over the kept instances.
        // GRASS-VIS-6: the full list (the cascades, the TLAS) is culled by the camera's side planes swept along
        // every directional light @p meshes fitted cascades for this frame
        // (MeshRenderer::DirectionalCasterSweeps), the camera's own list by the camera's planes; AddFrame runs
        // after MeshRenderer::UpdateCascades.
        // @p parallelSetup: RenderPipeline.ParallelSetup - the varieties' plans (PlanVariety) on TaskGroup tasks.
        void AddFrame( RDG::Builder& graph, const ViewFrame& frame, MeshRenderer& meshes, bool rayTracing,
                       const GrassOcclusion& occlusion, bool parallelSetup );

        // GRASS-VIS phase 2 (UE two-phase occlusion): after this frame's furthest HZB of the phase-1 depth, the
        // instances last frame's HZB held back from the camera are re-tested against @p hzb (through the unjittered
        // @p viewProjection it was built from; @p extent its mip-0 size and mip count) and the visible ones appended
        // to the camera's list, drawn by the sections' PostRefs. @p hzb invalid (this frame built none): every
        // candidate is appended untested (the Neutral texture AddFrame was given is bound in its place), so a
        // missing HZB never drops the grass phase 1 held back. False: nothing was added (no tested list this frame).
        [[nodiscard]] bool AddFramePostOcclusion( RDG::Builder& graph, RDG::TextureRef hzb,
                                                  const glm::mat4& viewProjection, const glm::uvec3& extent );

        // GRASS-VIS: the last AddFrame handed out more raster-stream rows than a visibility word addresses
        // (kGrassVisibilityMaxRows): the view refuses its grass visibility buffer path this frame.
        [[nodiscard]] bool VisibilityRowsOverflow() const noexcept
        {
            return m_VisibilityRows > kGrassVisibilityMaxRows;
        }

        // This frame's blade BLAS requests and resident BLASes (after AddFrame): the view's TLAS block hands them
        // to the RT scene with the landscape's.
        [[nodiscard]] const GrassBladeRayTracing& GetBladeRayTracing() const
        {
            return m_BladeRayTracing;
        }

        // THE CONTROL GATE'S QUESTION (PendingWork::GrassInstances, GRASS-SETTLE p14): the last AddFrame drew a
        // variety whose instance buffer may hold fewer instances than its generator keeps - its first count has
        // not come back yet (capacity zero), or a read-back count overflowed the capacity the frame was drawn with
        // (a camera cut: the buffers are sized by the PREVIOUS view's count, and GrassGenerate drops everything
        // past them, a straight-edged block of cells losing its meshes). Clears once every drawn variety has had
        // the swapchain's frames in flight worth of read-backs within its capacity since the last overflow. UE's
        // automated screenshot waits for the grass builds the same way (the async grass tasks must finish).
        [[nodiscard]] bool InstancesSettling() const noexcept
        {
            return m_Settling;
        }

    private:
        // GRASS-VIS-6: what the camera draws this frame. None: the full list (it is the camera's own volume, and
        // no HZB is tested); Planes: its own list, culled by its side planes; PlanesAndHzb: that list culled by
        // last frame's HZB too, with phase 2's candidates (GrassGenerate.shader Hzb.w 0 / 3 / 1).
        // GRASS-CULL-UNIFY-1: the full list is the view-independent field about the field eye, so the camera
        // always culls its own list (None, "the full list is the camera's volume", is gone with the swept volume).
        enum class CameraList : uint8_t
        {
            Planes,
            PlanesAndHzb
        };
        // The view's volumes for GrassCull.shader (the variety table's header), decided once per AddFrame: the
        // camera's side planes and the cascades' caster boxes. The tile cull sees none of them.
        struct ViewVolumes
        {
            glm::vec4  Camera[4]{};
            CameraList List = CameraList::Planes;
            // GPU-COST-21: the cascade views this frame draws (bit v: view v, MeshRenderer::CascadeCasterBoxSet)
            // and their caster boxes as GrassCull culls them, list k's six planes (the k-th set bit's) at 6k: the
            // guard box held about the view's caster box (GrassRenderer.cpp kCascadeGuardTurn).
            uint32_t  CascadeMask = 0;
            glm::vec4 CascadePlanes[kGrassCascadeLists * 6u]{};
            // GRASS-SHADOW-1: list k's cascade view (the k-th set bit of CascadeMask) - the view a variety asks
            // MeshRenderer::ShadowWindInnerOf for its ring's inner distance by its own reach.
            uint32_t CascadeViews[kGrassCascadeLists]{};
            // ...and the rule's measure that view's light camera carries (MeshRenderer::CasterWindOrigin: xyz the
            // main camera's origin, w ShadowWindDistanceOf - 0 where every instance casts its rest pose).
            glm::vec4 CascadeWind[kGrassCascadeLists]{};
        };
        // CPU-GRASS-POOL-1, ONE BUFFER PER KIND OF DATA (UE GPUScene / FInstanceCullingManager: the instances'
        // data live in a few pooled buffers registered once a frame, and a primitive owns a range of them): a
        // variety holds a byte range of its kind's pool instead of a buffer of its own, so the frame graph
        // imports, registers and declares the kind once, not once per variety. A pooled kind is one the grass
        // passes alone read and write, by the variety table's addresses (pool + offset), so the shaders are
        // unchanged.
        static constexpr uint32_t kNoFeedbackSlot = 0xFFFFFFFFu; // VarietyGpu::FeedbackSlot: none handed out
        struct PoolRange
        {
            uint64_t Offset = 0;
            uint64_t Bytes  = 0; // 0: no range held
        };
        class BufferPool
        {
        public:
            BufferPool( const char* name, uint32_t binding ) noexcept : m_Name( name ), m_Binding( binding )
            {
            }

            // Hands @p range @p bytes (aligned) of the pool, releasing what it held first: the lowest free run
            // that fits, else the end. False (@p range empty): the pool would pass a storage buffer's 4 GB.
            [[nodiscard]] bool Allocate( PoolRange& range, uint64_t bytes );
            // Gives @p range's bytes back (merged with free neighbours; a run at the end shortens the pool).
            void Release( PoolRange& range );
            // Once a frame, after every Allocate of it: the device-local buffer is (re)made when the ranges
            // handed out passed its room - a remake moves every range (Epoch counts the remakes; what a range
            // held is lost) - then imported into @p graph and registered once (Ref).
            [[nodiscard]] Common::BoolResultStr Commit( RDG::Builder& graph );
            [[nodiscard]] uint64_t              Address( const PoolRange& range ) const noexcept
            {
                return m_Address + range.Offset;
            }
            [[nodiscard]] uint64_t Epoch() const noexcept
            {
                return m_Epoch;
            }
            [[nodiscard]] RDG::BufferRef Ref() const noexcept
            {
                return m_Ref;
            }

        private:
            const char*                                     m_Name;
            uint32_t                                        m_Binding;
            std::shared_ptr<ShaderResources::StorageBuffer> m_Buffer;
            uint64_t                                        m_Address = 0; // m_Buffer's, taken when it is made
            uint64_t                                        m_Room    = 0; // m_Buffer's bytes
            uint64_t                                        m_End     = 0; // the bytes the ranges span
            uint64_t                                        m_Epoch   = 0;
            std::vector<PoolRange>                          m_Free; // by offset, never adjacent, never at the end
            RDG::ExternalBuffer                             m_Import;
            RDG::BufferRef                                  m_Ref{};
        };
        struct FieldGpu
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
            uint64_t                                        Revision = 0;
            RDG::ExternalBuffer                             Import;
            RDG::BufferRef                                  Ref;
        };
        struct VarietyGpu
        {
            class Desert::Mesh*                             Mesh     = nullptr;
            uint32_t                                        Capacity = 0;
            // The slots one bin list gives the variety's sections this frame, and its casting ones
            // (MeshRenderer::DrawBinSlots): what AddDrawGather sizes the tables by.
            uint32_t                                        BinSlots     = 0;
            uint32_t                                        BinCastSlots = 0;
            std::shared_ptr<ShaderResources::StorageBuffer> Instances;
            std::shared_ptr<ShaderResources::StorageBuffer> Args;
            std::shared_ptr<ShaderResources::StorageBuffer> Params;
            std::shared_ptr<ShaderResources::StorageBuffer> Shape; // Kind Blade: GrassBladeShapeGpu
            GrassBladeShapeGpu                              ShapeData{}; // Kind Blade: what Shape was written with
            // GRASS-VIS-MESH-1, Kind Mesh: per section (the variety's Sections order) its raster-stream rows
            // (GrassMeshVisibilityGpu); null: the section draws through the prepass pair this frame.
            std::vector<std::shared_ptr<ShaderResources::StorageBuffer>> MeshVisibility;
            uint32_t                                        Buckets     = 1;
            // The first bucket below kMinScreenRadiusForDepthPrepass through this frame's lens (MeshRenderer::
            // GpuInstancedRenderData::PrepassBuckets); Buckets: every bucket is prepassed.
            uint32_t              PrepassBuckets = 1;
            std::vector<uint32_t> BucketCapacities; // what Args' bucket table was written with
            // GPU-count feedback (GrassCull FinalizeFull): per frame in flight, each bucket's unclamped count and a written
            // mark, read back FramesInFlight frames later; LowFrames counts, per bucket, the frames in a row its
            // capacity has been at least two steps above what the counts ask for (the shrink's hysteresis).
            std::shared_ptr<ShaderResources::StorageBuffer> Feedback;
            std::vector<uint32_t>                           LowFrames;
            // Read-backs still to come, after the last one that overflowed a bucket, before this variety's draw is
            // known to hold every instance kept (InstancesSettling); Truncated: that overflow has been logged.
            uint32_t                                        SettleReads = 0;
            bool                                            Truncated   = false;
            // GRASS-VIS-7: the drawn counts this frame's Feedback copy held (GrassCull FinalizeFull, FramesInFlight frames
            // ago): the full list's, the camera's phase 1's, phase 2's appended; none before its first write.
            std::optional<glm::uvec3>                       Drawn;
            // GPU-COST-21: the same copy's drawn count of each cascade view's list (0: the view had none).
            std::array<uint32_t, kGrassCascadeLists>        DrawnCascades{};
            RDG::ExternalBuffer InstancesImport, ArgsImport, ParamsImport, FeedbackImport;
            RDG::BufferRef      FeedbackRef;
            GpuInstanceRefs                                 Refs;
            // GRASS-VIS, the camera's list (List not None: CameraIds / CameraArgs sized like FullIds / Args;
            // PlanesAndHzb: Candidates / PostArgs too; otherwise one-word stand-ins the passes bind and never
            // touch): CameraIds / CameraArgs the instances inside the camera's planes that last frame's HZB does
            // not hide plus phase 2's, Candidates the hidden ones' spheres and rows, PostArgs phase 2's draw
            // records.
            CameraList                                      List = CameraList::Planes;
            std::shared_ptr<ShaderResources::StorageBuffer> CameraIds;
            std::shared_ptr<ShaderResources::StorageBuffer> CameraArgs;
            PoolRange                                       Candidates; // of m_CandidatesPool (CPU-GRASS-POOL-1)
            std::shared_ptr<ShaderResources::StorageBuffer> PostArgs;
            RDG::ExternalBuffer                             CameraIdsImport, CameraArgsImport, PostArgsImport;
            GpuInstanceRefs     CameraRefs; // phase 1's draws
            GpuInstanceRefs                                 PostRefs;   // phase 2's: CameraIds through PostArgs
            // GRASS-CULL-HIER-1, THE CAMERA CULL'S CHUNKS (Common/GrassVarietyRows.glslh GrassVarietyRow::Chunks): the
            // tile cull's runs of Instances - one subgroup's slots of one bucket, one tile cluster's - with their
            // roots' box, each culled whole by one subgroup of GrassCull's camera cull, whose lanes then cull its
            // instances; room for ChunkRoom chunks (one per slot of Instances).
            std::shared_ptr<ShaderResources::StorageBuffer> Chunks;
            RDG::ExternalBuffer                             ChunksImport;
            RDG::BufferRef                                  ChunksRef;
            uint32_t                                        ChunkRoom = 0;
            // GRASS-CULL-COST-3, THE LAW'S SLOTS (Common/GrassVarietyRows.glslh GrassVarietyRow::LawScales; UE GPU
            // Scene: an instance's data is written once when it changes, the views only cull it): the scale the
            // view law gave each record before its chunk's cut, written once per law generation by GrassCull's
            // LawChunk and read by every frame's camera cull; LawCapacity slots, sized by what the law handed out
            // (LawLowFrames: the frames in a row at half of it). CPU-GRASS-POOL-1: a range of m_LawScalesPool;
            // LawPoolEpoch the pool's Epoch the range was last lawed in - a remade pool moved it, so it is a new
            // law generation.
            PoolRange                                       LawScales;
            uint32_t                                        LawCapacity  = 0;
            uint32_t                                        LawLowFrames = 0;
            uint64_t                                        LawPoolEpoch = 0;
            // What the view law reads this frame (LawKey: the params with the field eye, the pool's identity, Args,
            // LawScales and its room, the field's revision) and what the chunks were last lawed from (LawedKey);
            // LawGeneration counts its changes - a chunk whose law word carries another generation is lawed anew. A
            // turn, a still camera or a tile built keeps it.
            std::vector<uint32_t> LawKey;
            std::vector<uint32_t> LawedKey;
            uint64_t              LawGeneration = 0;
            // GRASS-CULL-HIER-1: what the cascades' lists were last culled from (GrassRenderer::WriteVarietyTable:
            // the guarded caster boxes, the wind origin, the row's buffers and rings, Generation) and whether a
            // frame culled them from it; from the next frame the lists hold (kGrassRowCascadesHeld) until it changes.
            std::vector<uint32_t> CascadeKey;
            bool                  CascadesCulled = false;
            // GRASS-CULL-2: what the camera cull reads of the params, through the variety table (GrassRenderer.cpp
            // GrassVarietyRowGpu): x = the bounds radius, y = the bounds centre height (local), z = the cull pad (cm),
            // and the bucket table's first word in Args.
            glm::vec4                                       CullBounds{ 0.0f };
            uint32_t                                        TableWord = 0;
            // GPU-COST-21, THE CASCADES' LISTS (UE: each shadow view culls its instances by its own volume): per
            // cascade view of CascadeMask (list k the k-th set bit) the instances inside that cascade's caster
            // box, CascadeIds list k at k x Capacity, CascadeArgs per list (CascadeArgWords words) one copy of
            // Args' layout, its bucket firsts moved by k x Capacity, then the list's REST TWIN (GRASS-SHADOW-1): a
            // copy of the records and one rest count per bucket. A bucket's movers (the ring [inner, wind
            // distance) of the list's cascade, MeshRenderer::ShadowWindInnerOf) fill it from its first slot and
            // are what Args' records draw; its rest-pose casters fill it from its last slot and are what the
            // twin's records draw, CascadeRestShift bytes after the record they twin. No live view: one-word
            // stand-ins the passes bind.
            uint32_t                                        CascadeMask      = 0;
            uint32_t                                        CascadeArgWords  = 0;
            uint32_t                                        CascadeRestShift = 0;
            // GRASS-CULL-UNIFY-1: this frame's movers' ring of list k (x = MeshRenderer::ShadowWindInnerOf by
            // Reach, y = the wind distance, 0: no mover), the variety table's CascadeRings for GrassCull's
            // CascadeCull.
            std::array<glm::vec2, kGrassCascadeLists>       CascadeRings{};
            std::shared_ptr<ShaderResources::StorageBuffer> CascadeIds;
            std::shared_ptr<ShaderResources::StorageBuffer> CascadeArgs;
            RDG::ExternalBuffer                             CascadeIdsImport, CascadeArgsImport;
            GpuInstanceRefs                                 CascadeRefs;
            RDG::BufferRef                                  ParamsRef;
            RDG::BufferRef                                  FieldRef;
            // GRASS-VIEW-LAW-1: the field's buffer this frame (the variety table's Field address) and the full list
            // (GrassCull.shader CullInstance: the records the views keep, in Args' buckets; Refs.Ids).
            std::shared_ptr<ShaderResources::StorageBuffer> FieldBuffer;
            std::shared_ptr<ShaderResources::StorageBuffer> FullIds;
            RDG::ExternalBuffer                             FullIdsImport;
            uint32_t                                        Index = 0; // stable section key base
            // GRASS-TILE-1, THE TILE CACHE (UE LandscapeGrass.cpp: a component's grass is built once by
            // FAsyncGrassBuilder into its HISM and kept while the component is within the cull distance, the
            // builds budgeted by grass.MaxCreatePerFrame): the variety's world tiles of kGrassTileCells^2 cells
            // within the cull radius of the eye, each built once into the pool (GrassGenerate.shader Count /
            // Allocate / Write) and freed when it leaves the radius; the tile cull (GrassGenerate.shader Cull) is
            // the per-view rest. A tile is built for the nearest the eye stood (BuiltNear, less a hysteresis): its
            // fade draws past that view's share are not kept, so it is rebuilt when the eye comes nearer, or
            // recedes far enough to hold too many (UpdateTiles). Stale: built under another lens or Foliage
            // quality's thinning; it keeps drawing until rebuilt.
            // CPU-GRASS-POOL-2, THE RESIDENT TILES ARE A RING GRID (UE: a component is found by its section
            // coordinates, not searched for): tile (x, z) lives in cell (x mod TileRingSide, z mod
            // TileRingSide) of TileRing, which holds its coordinates. The residents lie within a square of
            // TileSlots tiles about the eye (UpdateTiles' reach, plus the tile-wide band a resident is kept in),
            // so a ring two tiles wider than the pool's span never holds two of them in one cell; it is sized
            // with the pool (fresh).
            struct ResidentTile
            {
                int32_t  X = 0, Z = 0;
                uint32_t Slot      = 0;
                float    BuiltNear = 0.0f;
                bool     Held      = false;
                bool     Stale     = false;
            };
            std::vector<ResidentTile> TileRing;
            uint32_t                  TileRingSide = 0;
            std::vector<uint32_t>     FreeSlots;
            uint32_t                                   TileSlots = 0;
            uint32_t                                   TilePages = 0;
            float                                      TileSide  = 0.0f; // cm
            // What the build reads (PlacementKey: the placement's params, the cell, the field's revision; a change
            // empties the pool) and the share law's inputs (ThinKey: Start/EndCullDistance through the lens, the
            // screen-width floor; a change marks every tile stale).
            std::vector<uint32_t> PlacementKey;
            std::vector<uint32_t> ThinKey;
            // The pool's identity (TileFeedback's mark); PoolFresh: this frame initialises it
            // (kGrassTileModeInit).
            uint32_t                                        PoolGeneration = 0;
            bool                                            PoolFresh      = false;
            // CPU-GRASS-POOL-2: this frame's run of the shared tile table (GrassRenderer::m_TileTable: its header
            // entry; a frame without tile passes has none) and the variety's words of the shared tile feedback
            // (GrassRenderer::m_TileFeedback; kNoFeedbackSlot until EnsureTileFeedback hands one, never given
            // back).
            uint32_t TileTableBase = 0;
            uint32_t FeedbackSlot  = kNoFeedbackSlot;
            // GRASS-TILE-3: THE POOL IS VIDEO MEMORY (UE: the grass HISM's instance buffers are GPU resources, the
            // host only uploads). The records (TileSegments, below) and the free pages and tile headers (TileWords) are
            // written and read by the GPU alone, so they are device-local storage buffers (GRASS-VIEW-LAW-1:
            // StorageBuffer::CreateDeviceLocal, Persistence::DeviceOnly; until then graph buffers kept by
            // Builder::Extract, which have no device address) - not persistent StorageBuffers, which are
            // host-visible (MakeMappedBufferCopy: CPU_TO_GPU): ~1 GB of those filled the device-local host-visible
            // heap (256 MB without resizable BAR) and the driver placed the rest - the pool and the camera list's
            // Instances and ids made after it - in system memory, read over PCIe by the tile cull, the camera cull
            // and every grass draw. Instances, CameraIds, Candidates and CascadeIds are device-local for the same
            // reason. UpdateTiles makes the pool for a new PoolGeneration; TilePoolMade: a frame's graph has run
            // its Init pass on it (AddFrame); false until then, and a new pool is made by the next frame's.
            bool                                            TilePoolMade = false;
            std::shared_ptr<ShaderResources::StorageBuffer> TilePool, TileWords;
            RDG::ExternalBuffer                             TilePoolImport, TileWordsImport;
            RDG::BufferRef TilePoolRef, TileWordsRef;
            // GRASS-VRAM-BUDGET-1, THE POOL'S SEGMENTS (Common/GrassVarietyRows.glslh; UE: a pooled buffer grows by
            // pages inside its pool's budget, never by a second full copy): the records live in TileSegments, device-
            // local buffers of kGrassTileSegmentPages pages appended as the pool grows; TilePool is their segment
            // table (persistent: per segment its address, written when the segment is taken). CPU-GRASS-POOL-2:
            // the segments are the renderer's (GrassRenderer::m_TileSegments, shared by every variety, imported
            // once a frame); TileSegments holds the indices of this pool's, in its pages' order. TileRoom: the
            // pages the free-page stack in TileWords has room for - every slot's tile at its most pages - and so the
            // most the pool grows to; TileGrowFrom: the first page this frame's growth adds (TilePages: none), which
            // the Free pass pushes on the stack. TilePoolCapped: the pool was held at the grass pools' share of the
            // device-local budget (logged once).
            std::vector<uint32_t>                                        TileSegments;
            uint32_t                                                     TileRoom       = 0;
            uint32_t                                                     TileGrowFrom   = 0;
            bool                                                         TilePoolCapped = false;
            // This frame's tile table (GrassGenerate.shader Tiles): the header, then TileFrees slots to free,
            // TileBuilds tiles to build, TileCulls tiles the tile cull walks (nearest first, GRASS-TILE-2).
            std::vector<glm::uvec4> TileEntries;
            uint32_t                TileFrees  = 0;
            uint32_t                TileBuilds = 0;
            uint32_t                TileCulls  = 0;
            // Counts the frames whose tiles changed (built, freed): the tile cull's input (FrameKey).
            uint32_t TileVersion = 0;
            // TURN-CPU-SETUP-1, THE WALK IS KEPT WHILE ITS INPUTS ARE (UE FScene: what does not change between
            // frames is held, not rebuilt each frame): the inputs of the last walk of the tiles around the eye
            // (the eye on the ground, the cell, the cull radius, the stray, the thinning's three words:
            // TileWalkKey) and the pool bound they give (TileBound). A frame with the same inputs, after a walk
            // that neither freed nor built a tile in the same pool (TileWalkSettled - a camera that only turns),
            // walks to the same table: UpdateTiles keeps TileEntries, TileCulls and the tiles still waiting for a
            // slot (TilePending) instead of walking every tile of the square around the eye again.
            std::array<uint32_t, 8> TileWalkKey{};
            uint32_t                TileBound       = 0;
            bool                    TileBoundKnown  = false;
            bool                    TileWalkSettled = false;
            bool                    TilePending     = false;
            // GrassGenerate's tile cull's inputs this frame (FrameKey: the params, the tiles' version, the
            // capacity, the buffers, the field's revision) and the ones Instances / Args were last culled from
            // (GenerateKey). Equal inputs keep equal instances, so a frame with the same ones dispatches nothing -
            // turning the camera changes the planes and re-culls, it builds no tile. GRASS-VIEW-LAW-1: a new
            // generation re-culls the cascades and the full list (WriteVarietyTable's CascadeKey).
            std::vector<uint32_t> FrameKey;
            std::vector<uint32_t> GenerateKey;
            // Counts the GenerateKey changes: the identity of what the field holds, which a cached cascade's static
            // layer is keyed by (MeshRenderer::GpuInstancedRenderData::Still). CSM-SCROLL-3: it holds whatever the
            // view - FrameKey carries no view volume (GRASS-CULL-UNIFY-1: the camera's planes and the cascades' boxes
            // are GrassCull's, per frame), and the field eye moves only from tile to tile - so a turn or a step
            // inside a tile keeps it and the cache scrolls.
            uint64_t Generation = 0;
            // The farthest an instance's geometry reaches from its origin, world cm: its bounding sphere's centre
            // height plus radius at the placement's largest scale, and the wind's sway (StillInstances::Reach).
            float Reach = 0.0f;
            // GRASS-VIS: this frame's first raster-stream row (GrassBladeShapeGpu::Visibility.x), Blade only.
            uint32_t VisibilityRow = 0;
            // Blade only: what each cascade picks this variety's shadow LOD and density by (GPU-COST-20).
            GpuInstanceBladeShadowLod ShadowLod{};
        };

        // RENDER-PARALLEL-SETUP: a variety's frame in three steps. PrepareVariety - serial, in variety order: the
        // VarietyGpu entry, the read-back counts, rebuilt buffers, the visibility rows - fills @p plan's inputs.
        // PlanVariety - pure (const), on a TaskGroup task: the params and the tile build's inputs into the plan,
        // and its own entry's shadow LOD and cascade rings. ApplyVariety - serial, in variety order: the params'
        // upload, UpdateTiles (the pool budget spans varieties) and the frame's keys; false: not drawn this frame.
        struct VarietyPlan; // GrassRenderer.cpp
        [[nodiscard]] VarietyGpu* PrepareVariety( const GrassVarietyDraw& variety, class Desert::Mesh& mesh,
                                                  const ViewFrame& frame, const ViewVolumes& volumes,
                                                  const MeshRenderer& meshes, uint64_t fieldRevision,
                                                  VarietyPlan& plan );
        void               PlanVariety( VarietyPlan& plan ) const;
        [[nodiscard]] bool ApplyVariety( VarietyPlan& plan );
        // GRASS-TILE-1: @p gpu's tiles for this frame's eye - the pool (re)made when the placement or the pool's
        // room asks for it, tiles past the radius freed, missing / stale ones built nearest first, at most
        // kGrassTileBuildsPerFrame - and its tile table (VarietyGpu::TileEntries). @p placement / @p thin: the
        // build's inputs (VarietyGpu::PlacementKey / ThinKey); @p cell the grid cell, @p radius the cull radius,
        // @p stray how far an instance lies from its cell (cm). False when a buffer was not made.
        // GRASS-VRAM-BUDGET-1: the bytes every variety's tile pool segments hold but @p except's, and the grass
        // pools' ceiling - kGrassTilePoolBudgetShare of the device-local budget (VK_EXT_memory_budget; the
        // device-local heaps' size where the budget is unknown; 0 without a device), read once a frame
        // (m_FrameTilePoolCap): a capped pool asks every frame, and each Device::QueryMemory is a driver query.
        [[nodiscard]] uint64_t TilePoolBytes( const VarietyGpu* except ) const;
        [[nodiscard]] uint64_t TilePoolCap();
        [[nodiscard]] bool UpdateTiles( VarietyGpu& gpu, std::vector<uint32_t> placement,
                                        std::vector<uint32_t> thin, glm::vec2 eye, float cell, float radius,
                                        float stray );
        // GRASS-CULL-2: this frame's variety table (GrassRenderer.cpp GrassVarietyRowGpu): m_FrameCull's rows,
        // written into m_Table and imported into @p graph (m_TableRef).
        // Refused when a variety's buffer has no device address.
        [[nodiscard]] Common::BoolResultStr WriteVarietyTable( RDG::Builder& graph, const ViewVolumes& volumes );
        // The one binding block of a pass over the table, GrassCull's: @p hzb the texture it tests and @p dispatch
        // its access to this frame's dispatch buffer (m_FrameDispatch).
        void BindCull( RDG::PassBuilder& pass, RDG::TextureRef hzb, RDG::Access dispatch );
        // The blade strip of @p segments segments: one submesh per level, each halving the last down to one.
        [[nodiscard]] class Desert::Mesh* BladeStrip( uint32_t segments );

        std::vector<std::reference_wrapper<const GrassFieldDraw>>         m_Queue;
        GrassQuality                                                      m_Quality;
        std::unordered_map<uint64_t, FieldGpu>                            m_Fields;
        std::unordered_map<uint64_t, VarietyGpu>                          m_Varieties;
        std::unordered_map<uint32_t, std::shared_ptr<class Desert::Mesh>> m_BladeStrips;
        // CPU-GRASS-POOL-2, THE TILE POOLS' SEGMENTS (UE: one pooled allocator hands every HISM its pages): every
        // variety's segments (VarietyGpu::TileSegments, indices here). A pool made anew gives its segments back
        // to m_FreeTileSegments, which the pools growing in the same frame take first; what is still free once
        // the frame's tiles are updated is released (TrimTileSegments), so a free segment holds no video memory
        // past its frame. The held ones are imported and registered once a frame (Ref).
        struct TileSegment
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Buffer; // null: the index is free and empty
            uint64_t                                        Address = 0;
            bool                                            Held    = false;
            RDG::ExternalBuffer                             Import;
            RDG::BufferRef                                  Ref{};
        };
        std::vector<TileSegment> m_TileSegments;
        std::vector<uint32_t>    m_FreeTileSegments;
        // Takes a segment for a pool (a free one first, else a new one); false (logged) when none could be made.
        [[nodiscard]] bool TakeTileSegment( uint32_t& index );
        void               ReleaseTileSegments( VarietyGpu& gpu );
        void               TrimTileSegments();
        // The held segments into @p graph, once a frame (TileSegment::Ref); false (logged): no tile pass may run.
        [[nodiscard]] bool ImportTileSegments( RDG::Builder& graph );
        // @p pass reads or writes every held segment (@p access): declared once, not once per variety.
        void DeclareTileSegments( RDG::PassBuilder& pass, RDG::Access access ) const;
        // CPU-GRASS-POOL-2, THE TILE TABLE AND FEEDBACK ARE SHARED (UE FInstanceCullingContext: one upload, each
        // primitive at its base): this frame's tile table - every tile-building variety's run of TileEntries,
        // gathered in m_TileTableImage and written and imported once (per frame in flight) - and the tile
        // feedback, kGrassTileFeedbackWords per variety at its FeedbackSlot (per frame in flight, read back).
        std::shared_ptr<ShaderResources::StorageBuffer> m_TileTable;
        uint32_t                                        m_TileTableRoom = 0; // entries
        std::vector<glm::uvec4>                         m_TileTableImage;
        RDG::ExternalBuffer                             m_TileTableImport;
        RDG::BufferRef                                  m_TileTableRef{};
        std::shared_ptr<ShaderResources::StorageBuffer> m_TileFeedback;
        uint32_t                                        m_TileFeedbackRoom  = 0; // slots
        uint32_t                                        m_TileFeedbackSlots = 0; // handed out
        RDG::ExternalBuffer                             m_TileFeedbackImport;
        RDG::BufferRef                                  m_TileFeedbackRef{};
        // Hands @p gpu its feedback slot and grows m_TileFeedback to every slot handed out; a remade buffer lost
        // the frames in flight's reports, so every variety's pool is made anew. False (logged): not made.
        [[nodiscard]] bool EnsureTileFeedback( VarietyGpu& gpu );
        // Writes m_TileTableImage into m_TileTable and imports the table, the feedback and the segments once;
        // false (logged): this frame's tile passes do not run.
        void CommitTileTable( RDG::Builder& graph );
        // UpdateTiles' scratch, kept across frames (no allocation once grown): the tiles to build, the builds,
        // the tiles culled.
        struct TileWant
        {
            float   Nearest = 0.0f;
            int32_t X = 0, Z = 0;
        };
        std::vector<TileWant>                   m_TileWants;
        std::vector<glm::uvec4>                 m_TileBuildScratch;
        std::vector<std::pair<float, uint32_t>> m_TileCullScratch;
        std::vector<VarietyGpu*>         m_Frame; // this frame's, in dispatch order
        // GRASS-VIS-24: this frame's whose camera list is culled (an HZB-tested frame: every one with a list; by
        // the planes alone: those that generate)
        std::vector<VarietyGpu*>         m_FrameCull;
        // GRASS-TILE-1: this frame's whose pool is initialised or whose tiles are freed (the Free pass), and whose
        // tiles are built (Count / Allocate / Write).
        std::vector<VarietyGpu*>         m_FrameTileFree;
        std::vector<VarietyGpu*>         m_FrameTileBuild;
        // GRASS-VRAM-BUDGET-1: this frame's TilePoolCap, read on its first use (empty: not read yet this frame).
        std::optional<uint64_t>          m_FrameTilePoolCap;
        // CPU-GRASS-VARIETY-1: every variety's TileSegments summed, kept where the segments are taken and given
        // back (TakeTileSegment / ReleaseTileSegments only), so TilePoolBytes is O(1), not a walk
        // of m_Varieties per variety (O(V^2) a frame). m_Varieties is never erased, so no variety leaves the sum.
        uint64_t                         m_TileSegmentCount = 0u;
        bool                             m_Settling = false; // InstancesSettling, decided by the last AddFrame
        bool                             m_FrameTested = false; // the last AddFrame tested camera lists by an HZB
        RDG::TextureRef                  m_FrameNeutral{}; // the last AddFrame's GrassOcclusion::Neutral
        bool m_FrameTable = false; // the last AddFrame wrote its variety table and added its passes
        // GRASS-CULL-HIER-1: cascade list k's guard box (GrassRenderer.cpp kCascadeGuardTurn) - the caster box of
        // the view it last grew from, grown by the chord a turn of the camera carries it - and that view
        // (kNoCascadeView: none yet).
        static constexpr uint32_t kNoCascadeView = 0xFFFFFFFFu;
        struct CascadeGuard
        {
            uint32_t  View = kNoCascadeView;
            glm::vec4 Planes[6]{};
        };
        std::array<CascadeGuard, kGrassCascadeLists> m_CascadeGuards{};
        std::shared_ptr<ComputePipeline> m_Generate;
        std::shared_ptr<ComputePipeline> m_Cull; // GRASS-CULL-2: the camera cull and phase 2 (GrassCull.shader)
        mutable ShaderBindingLayoutCache m_GenerateLayout;
        // RENDER-JOB-CPU-4, THE COMPACTED BINS' GATHER (Programs/Grass/GrassDrawCompact.shader) and its phase
        // tables (GpuDrawBinFrame: 0 the camera's first phase, 1 post-occlusion, 2 the cascades' casters -
        // GRASS-SHADOW-BINS-1) with their buffers; every section hands MeshRenderer the tables
        // (GpuInstancedRenderData::DrawBins).
        struct DrawBinBuffers
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Commands, Slots, Counts; // device-local, kept
            std::shared_ptr<ShaderResources::StorageBuffer> Entries, Rows;           // per frame in flight
            RDG::ExternalBuffer CommandsImport, SlotsImport, CountsImport, EntriesImport;
            RDG::BufferRef      EntriesRef;
        };
        std::array<GpuDrawBinFrame, kGpuDrawBinPhases> m_DrawBins{};
        std::array<DrawBinBuffers, kGpuDrawBinPhases>  m_DrawBinBuffers{};
        std::shared_ptr<ComputePipeline> m_DrawCompact;
        mutable ShaderBindingLayoutCache m_DrawCompactLayout;
        // Opens phase @p phase's table (its buffers grown to this frame's varieties, in the graph) and adds its
        // clear and gather passes, which run after every draw setup of the frame filled the table.
        Common::BoolResultStr AddDrawGather( RDG::Builder& graph, uint32_t phase );
        mutable ShaderBindingLayoutCache m_CullLayout;
        // GRASS-CULL-2: the variety table (per frame in flight, WriteVarietyTable) and the last AddFrame graph's
        // refs: the table, and the cull's dispatch buffer (a graph buffer, AddFrame).
        std::shared_ptr<ShaderResources::StorageBuffer> m_Table;
        RDG::ExternalBuffer                             m_TableImport;
        // CPU-GRASS-POOL-1: the pooled kinds (VarietyGpu::Candidates / LawScales), committed by WriteVarietyTable.
        BufferPool m_CandidatesPool{ "GrassCandidates", 7u };
        BufferPool m_LawScalesPool{ "GrassLawScales", 2u };
        // CPU-GRASS-VARIETY-1: WriteVarietyTable's scratch, kept across frames so a frame allocates nothing: the
        // table's bytes (reserved to the header and every row at once) and a row's cascade key, which is swapped
        // into the variety's CascadeKey when it differs (both vectors keep their capacity).
        std::vector<uint8_t>                            m_TableImage;
        std::vector<uint32_t>                           m_CascadeKeyScratch;
        RDG::BufferRef                                  m_TableRef{};
        RDG::BufferRef                                  m_FrameDispatch{};
        uint32_t                         m_NextIndex = 0;
        GrassBladeRayTracing             m_BladeRayTracing;
        // GRASS-VIS: the raster-stream rows handed out this frame (AddFrame resets it; PrepareVariety hands each
        // blade variety its capacity's worth). A visibility word holds the row in its upper 32 - kVisTriangleBits
        // bits (VirtualGeometryLayout.hpp): past kGrassVisibilityMaxRows the ids would alias (VisibilityRowsOverflow).
        static constexpr uint64_t kGrassVisibilityMaxRows = uint64_t{ 1 }
                                                            << ( 32u - VirtualGeometry::kVisTriangleBits );
        uint64_t                  m_VisibilityRows = 0;
    };
} // namespace Desert::Graphic::System
