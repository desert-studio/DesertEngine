#pragma once

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
    // light's cascades; MeshRenderer::kCascadeViews). GrassGenerate.shader CascadePlanes and GrassFinalize.shader
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

        void Submit( GrassFieldDraw draw )
        {
            m_Queue.push_back( std::move( draw ) );
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
        void AddFrame( RDG::Builder& graph, const ViewFrame& frame, MeshRenderer& meshes, bool rayTracing,
                       const GrassOcclusion& occlusion );

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
        // GRASS-VIS-6: what the camera draws this frame. None: the full list (it is the camera's own volume, and no
        // HZB is tested); Planes: its own list, culled by its side planes; PlanesAndHzb: that list culled by last
        // frame's HZB too, with phase 2's candidates (GrassGenerate.shader Hzb.w 0 / 3 / 1).
        enum class CameraList : uint8_t
        {
            None,
            Planes,
            PlanesAndHzb
        };
        // The view's two volumes (GrassGenerate.shader GrassParams Planes / CameraPlanes), decided once per AddFrame.
        struct ViewVolumes
        {
            glm::vec4  Full[4]{};
            glm::vec4  Camera[4]{};
            bool       Swept = false; // Full is wider than Camera: the cells and capacity leave the lens's wedge
            CameraList List  = CameraList::None;
            // GPU-COST-21: the cascade views this frame draws (bit v: view v, MeshRenderer::CascadeCasterBoxSet)
            // and their caster boxes, list k's six planes (the k-th set bit's) at 6k (GrassGenerate.shader
            // CascadePlanes).
            uint32_t  CascadeMask = 0;
            glm::vec4 CascadePlanes[kGrassCascadeLists * 6u]{};
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
            std::shared_ptr<ShaderResources::StorageBuffer> Instances;
            std::shared_ptr<ShaderResources::StorageBuffer> Args;
            std::shared_ptr<ShaderResources::StorageBuffer> Params;
            std::shared_ptr<ShaderResources::StorageBuffer> Shape; // Kind Blade: GrassBladeShapeGpu
            GrassBladeShapeGpu                              ShapeData{}; // Kind Blade: what Shape was written with
            uint32_t                                        Buckets     = 1;
            std::vector<uint32_t> BucketCapacities; // what Args' bucket table was written with
            // GPU-count feedback (GrassFinalize): per frame in flight, each bucket's unclamped count and a written
            // mark, read back FramesInFlight frames later; LowFrames counts, per bucket, the frames in a row its
            // capacity has been at least two steps above what the counts ask for (the shrink's hysteresis).
            std::shared_ptr<ShaderResources::StorageBuffer> Feedback;
            std::vector<uint32_t>                           LowFrames;
            // Read-backs still to come, after the last one that overflowed a bucket, before this variety's draw is
            // known to hold every instance kept (InstancesSettling); Truncated: that overflow has been logged.
            uint32_t                                        SettleReads = 0;
            bool                                            Truncated   = false;
            // GRASS-VIS-7: the drawn counts this frame's Feedback copy held (GrassFinalize, FramesInFlight frames
            // ago): the full list's, the camera's phase 1's, phase 2's appended; none before its first write.
            std::optional<glm::uvec3>                       Drawn;
            // GPU-COST-21: the same copy's drawn count of each cascade view's list (0: the view had none).
            std::array<uint32_t, kGrassCascadeLists>        DrawnCascades{};
            RDG::ExternalBuffer InstancesImport, ArgsImport, ParamsImport, FeedbackImport;
            RDG::BufferRef      FeedbackRef;
            GpuInstanceRefs                                 Refs;
            // GRASS-VIS, the camera's list (List not None: CameraIds / CameraArgs sized like Instances / Args;
            // PlanesAndHzb: Candidates / PostArgs too; otherwise one-word stand-ins the passes bind and never
            // touch): CameraIds / CameraArgs the instances inside the camera's planes that last frame's HZB does
            // not hide plus phase 2's, Candidates the hidden ones' spheres and rows, PostArgs phase 2's draw
            // records.
            CameraList                                      List = CameraList::None;
            std::shared_ptr<ShaderResources::StorageBuffer> CameraIds;
            std::shared_ptr<ShaderResources::StorageBuffer> CameraArgs;
            std::shared_ptr<ShaderResources::StorageBuffer> Candidates;
            std::shared_ptr<ShaderResources::StorageBuffer> PostArgs;
            RDG::ExternalBuffer CameraIdsImport, CameraArgsImport, CandidatesImport, PostArgsImport;
            GpuInstanceRefs     CameraRefs; // phase 1's draws
            GpuInstanceRefs     PostRefs;   // phase 2's: CameraIds through PostArgs
            RDG::BufferRef      CandidatesRef;
            // GPU-COST-21, THE CASCADES' LISTS (UE: each shadow view culls its instances by its own volume): per
            // cascade view of CascadeMask (list k the k-th set bit) the instances inside that cascade's caster
            // box, CascadeIds list k at k x Capacity, CascadeArgs one copy of Args' layout (CascadeArgWords words)
            // per list, its bucket firsts moved by k x Capacity. No live view: one-word stand-ins the passes bind.
            uint32_t                                        CascadeMask     = 0;
            uint32_t                                        CascadeArgWords = 0;
            std::shared_ptr<ShaderResources::StorageBuffer> CascadeIds;
            std::shared_ptr<ShaderResources::StorageBuffer> CascadeArgs;
            RDG::ExternalBuffer                             CascadeIdsImport, CascadeArgsImport;
            GpuInstanceRefs                                 CascadeRefs;
            RDG::BufferRef                                  ParamsRef;
            RDG::BufferRef                                  FieldRef;
            glm::ivec4                                      Cells{ 0 };
            uint32_t                                        Index = 0; // stable section key base
            // GrassGenerate's inputs this frame (FrameKey: the params, the cells, the capacity, the buffers, the
            // field's revision) and the ones Instances / Args were last generated from (GenerateKey). Equal inputs
            // generate equal instances, so a frame with the same ones dispatches nothing (UE: a component's grass
            // is built once and kept until its inputs change, LandscapeGrass.cpp). StableGenerations counts the
            // generations in a row from GenerateKey: the skip starts once every frame in flight's feedback copy
            // (GrassFinalize) holds their counts, so the capacity read back stays the one they ask for.
            std::vector<uint32_t> FrameKey;
            std::vector<uint32_t> GenerateKey;
            uint32_t              StableGenerations = 0;
            // Counts the GenerateKey changes: the identity of what Instances holds, which a cached cascade's static
            // layer is keyed by (MeshRenderer::GpuInstancedRenderData::Still).
            uint64_t Generation = 0;
            // The farthest an instance's geometry reaches from its origin, world cm: its bounding sphere's centre
            // height plus radius at the placement's largest scale, and the wind's sway (StillInstances::Reach).
            float Reach = 0.0f;
            // GRASS-VIS: this frame's first raster-stream row (GrassBladeShapeGpu::Visibility.x), Blade only.
            uint32_t VisibilityRow = 0;
            // Blade only: what each cascade picks this variety's shadow LOD and density by (GPU-COST-20).
            GpuInstanceBladeShadowLod ShadowLod{};
        };

        [[nodiscard]] VarietyGpu* PrepareVariety( const GrassVarietyDraw& variety, class Desert::Mesh& mesh,
                                                  const ViewFrame& frame, const ViewVolumes& volumes );
        // The blade strip of @p segments segments: one submesh per level, each halving the last down to one.
        [[nodiscard]] class Desert::Mesh* BladeStrip( uint32_t segments );

        std::vector<GrassFieldDraw>                                       m_Queue;
        GrassQuality                                                      m_Quality;
        std::unordered_map<uint64_t, FieldGpu>                            m_Fields;
        std::unordered_map<uint64_t, VarietyGpu>                          m_Varieties;
        std::unordered_map<uint32_t, std::shared_ptr<class Desert::Mesh>> m_BladeStrips;
        std::vector<VarietyGpu*>         m_Frame; // this frame's, in dispatch order
        std::vector<VarietyGpu*>         m_FrameGenerate;    // this frame's whose inputs changed: the dispatched
        // GRASS-VIS-24: this frame's whose camera list is culled (an HZB-tested frame: every one with a list; by
        // the planes alone: those that generate)
        std::vector<VarietyGpu*>         m_FrameCull;
        bool                             m_Settling = false; // InstancesSettling, decided by the last AddFrame
        bool                             m_FrameTested = false; // the last AddFrame tested camera lists by an HZB
        RDG::TextureRef                  m_FrameNeutral{}; // the last AddFrame's GrassOcclusion::Neutral
        std::shared_ptr<ComputePipeline> m_Generate;
        std::shared_ptr<ComputePipeline> m_Finalize;
        mutable ShaderBindingLayoutCache m_GenerateLayout;
        mutable ShaderBindingLayoutCache m_FinalizeLayout;
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
