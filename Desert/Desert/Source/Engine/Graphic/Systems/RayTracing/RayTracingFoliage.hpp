#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic
{
    class ComputePipeline;
}

namespace Desert::Graphic::System
{
    class MeshRenderer;
}

// FOLIAGE IN THE RAY-TRACING SCENE (RT-FOLIAGE-BLAS; UE: a static mesh's ray-tracing LOD / Nanite fallback mesh,
// and WPO evaluated in ray tracing within a distance cutoff - r.RayTracing.Geometry.SupportsWPO, EvaluateWPO). Two
// parts:
//
//   * THE RT-LOD. An ISM / foliage instance traces the COARSEST level of its mesh whose measured error
//     (Submesh::LODCutErrors x the submesh's bounds radius x the instance's scale) stays below the ray-tracing
//     threshold at the instance's distance from the view (RayTracingFoliageSettings::LodErrorPerCm, cm per cm of
//     distance), with hysteresis: a level is entered only below kRayTracingLodHysteresis of its threshold and kept
//     until past it, so an instance at a threshold does not flip its BLAS every frame. A level with no measured
//     error (an imported chain, an old cook) is never chosen. The BLAS is the (mesh, masks + Lod) variant
//     (RayTracingSubmeshMasks::Lod, RayTracingGeometryVariants): ONE per (mesh, RT-LOD), every instance at that
//     level shares it. The selection is per view (each view's own distances), kept in RayTracingFoliageLod.
//
//   * THE WIND. A swaying instance (InstanceWind::Sways) within RayTracingFoliageSettings::WindDistance traces
//     its OWN BLAS - its RT-LOD's triangles with every vertex moved by the raster's wind (Common/FoliageWind.glslh
//     FoliageWindOffset, evaluated by "RayTracingScene: Foliage Wind",
//     Programs/Compute/FoliageWindRayTracing.shader, with the view's gust and local air) and REFIT, not rebuilt,
//     every frame from the first build - up to RayTracingFoliageSettings::RefitBudget a frame, nearest first.
//     Past the budget or the distance the instance traces the shared rest-pose BLAS of its RT-LOD: the raster's
//     caster draws past its shadow wind distance cast the rest pose too (CasterWindLayer), so a ray's shadow and
//     the raster's agree in both regions.
//
// The log line "[RT] foliage BLAS: unique / refit / budget" says what the frame traced, when it changes.
namespace Desert::Graphic::RayTracing
{
    // THE VIEW'S FOLIAGE RT SETTINGS (Scalability, GlobalIllumination group; SceneRenderer::BeginScene ->
    // RayTracingSceneRenderer::SetFoliageSettings). The table is their one source of truth: zero here means "not
    // set yet" = every instance at LOD 0, no wind BLAS.
    struct RayTracingFoliageSettings
    {
        // The ray-tracing error threshold: cm of the level's measured error per cm of distance from the view
        // (GlobalIllumination.RayTracingLodError, in 1/10000; 50 = 0.005 = ~0.3 degrees: shadows and GI read a
        // coarser surface than the raster's at this angle unnoticed). 0 = every instance traces LOD 0.
        float LodErrorPerCm = 0.0f;
        // Swaying instances refit per frame, all views together, nearest first (GlobalIllumination.
        // FoliageWindRefitBudget; UE: the dynamic geometry budget). 0 = every swaying instance traces its rest
        // pose.
        uint32_t RefitBudget = 0;
        // The WPO distance cutoff, cm from the view (GlobalIllumination.FoliageWindDistance; UE
        // r.RayTracing.DynamicGeometryLastRenderTimeUpdateDistance / WPO culling): farther swaying instances trace
        // their rest-pose BLAS.
        float WindDistance                                         = 0.0f;
        bool  operator==( const RayTracingFoliageSettings& ) const = default;
    };
    // A coarser level is entered only below this fraction of its threshold (kept until past the threshold).
    inline constexpr float kRayTracingLodHysteresis = 0.8f;
    // The deepest RT-LOD (RayTracingSubmeshMasks::Lod is a byte; the cooked chains are far shorter).
    inline constexpr uint32_t kRayTracingMaxLod = 7u;

    inline constexpr std::string_view kFoliageWindShader    = "FoliageWindRayTracing";
    inline constexpr std::string_view kFoliageWindPassName  = "RayTracingScene: Foliage Wind";
    inline constexpr uint32_t         kFoliageWindGroupSize = 64; // LocalSize of FoliageWindRayTracing.shader
    inline constexpr uint32_t         kFoliageWindJobRows   = 8;  // uvec4 rows per job (the shader's JobRows)

    // THE ERROR OF EACH LEVEL OF @p mesh as a whole (cm, mesh space): errors[k - 1] is the largest error over its
    // submeshes that @p hiddenSubmeshes does not hide of the level a BLAS at RT-LOD k cuts from each
    // (RayTracingLodRange: a submesh with a shorter chain stays at its coarsest level and its error). The list
    // ends at the first level some contributing submesh has no measured error for, and at kRayTracingMaxLod.
    [[nodiscard]] std::vector<float> RayTracingLodErrors( const Mesh& mesh, uint64_t hiddenSubmeshes );

    // The coarsest level of @p errors (RayTracingLodErrors) whose error at @p scale stays below the threshold at
    // @p distance (cm) - @p errorPerCm cm per cm of it (RayTracingFoliageSettings::LodErrorPerCm; 0 = LOD 0) -
    // with hysteresis against @p previous (the level the instance traced last).
    [[nodiscard]] uint8_t SelectRayTracingLod( std::span<const float> errors, float distance, float scale,
                                               uint8_t previous, float errorPerCm );

    // The largest axis scale of an instance transform: what a mesh-space error grows by in the world.
    [[nodiscard]] float RayTracingInstanceScale( const glm::mat4& transform );

    // ONE VIEW'S RT-LOD SELECTION (the hysteresis state): the level each ISM instance traced at the view's last
    // full gather, keyed {entity, component instance}. A gather asks Select per instance; EndGather drops the
    // instances it did not ask for (a removed field, a culled instance).
    class RayTracingFoliageLod
    {
    public:
        [[nodiscard]] uint8_t Select( uint32_t entity, uint32_t instance, std::span<const float> errors,
                                      float distance, float scale, float errorPerCm );
        void                  EndGather();

    private:
        std::unordered_map<uint64_t, uint8_t> m_Current;
        std::unordered_map<uint64_t, uint8_t> m_Previous;
    };

    // What the view traced of the foliage this frame (RayTracingSceneStats::Foliage*, the log line).
    struct RayTracingFoliageStats
    {
        uint32_t Instances   = 0; // ISM / foliage TLAS instances
        uint32_t Unique      = 0; // distinct shared (mesh, masks, RT-LOD) BLASes they trace
        uint32_t Lowered     = 0; // instances at an RT-LOD past 0
        uint32_t Swaying     = 0; // instances whose field sways
        uint32_t Refit       = 0; // own wind BLASes refit this frame (by this view)
        uint32_t Built       = 0; // own wind BLASes built from scratch this frame (new or remade)
        uint32_t Shared      = 0; // own wind BLASes an earlier view of the frame refit
        uint32_t RestPose    = 0; // swaying instances past the budget or the distance: rest-pose BLAS
        uint32_t Budget      = 0; // RayTracingFoliageSettings::RefitBudget
        uint64_t SharedBytes = 0; // storage of the distinct shared BLASes traced
        uint64_t Lod0Bytes   = 0; // storage of those meshes' own LOD-0 BLASes (what the instances traced before)
        uint64_t WindBytes   = 0; // storage + build inputs of the own wind BLASes
        bool     operator==( const RayTracingFoliageStats& ) const = default;
    };

    // The ISM / foliage part of @p primitives (the view's TLAS primitives after RayTracingFoliageWind::AddPasses):
    // instances, distinct shared BLASes and their storage against the meshes' own LOD-0 BLASes, into @p stats.
    void CountRayTracingFoliage( std::span<const RayTracingPrimitive> primitives, RayTracingFoliageStats& stats );

    // THE SCENE'S WIND BLASES (one per scene, RayTracingSceneCache; UE FRayTracingDynamicGeometryCollection for
    // WPO primitives). Entries keyed {entity, component instance}; one refit per entry per frame, by the first
    // view that takes it into the budget; a later view of the frame instances the same BLAS.
    class RayTracingFoliageWind
    {
    public:
        // For every primitive of @p primitives with a Wind.Mesh (the gather's swaying ISM instances): Geometry =
        // its rest-pose RT-LOD BLAS, unless it is among the nearest @p viewOrigin within @p settings' WindDistance
        // and the frame's RefitBudget (all views of @p frame together) - then its own BLAS, whose vertices "RayTracingScene: Foliage Wind" writes from @p camera
        // (the view's CameraUB this frame) and whose refit (or first build) is appended to @p buildsOut. An own
        // BLAS an earlier view of @p frame refit is appended to @p blasOut. @p camera invalid or the shader
        // missing: every instance traces its rest pose (said once). @p stats takes the counts.
        [[nodiscard]] Common::BoolResultStr
        AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                   std::span<RayTracingPrimitive> primitives, std::span<const uint32_t> windPrimitives,
                   RDG::BufferRef camera, const glm::vec3& viewOrigin, uint64_t frame,
                   const RayTracingFoliageSettings& settings, std::vector<RayTracingDynamicGeometryBuild>& buildsOut, std::vector<RDG::BufferRef>& blasOut,
                   RayTracingFoliageStats& stats );

        // Releases the entries no view used in @p frame (RayTracingSceneCache::BeginViewFrame).
        void EndFrame( uint64_t frame );

        [[nodiscard]] uint64_t GetMemoryBytes() const;

    private:
        struct Entry
        {
            std::unique_ptr<RayTracingDynamicMeshGeometryTarget> Target;
            const void*                                          Rest      = nullptr; // the BLAS its layout is of
            uint64_t                                             LastFrame = 0;
            bool                                                 Refit     = false; // refit / built in LastFrame
        };

        [[nodiscard]] bool EnsurePipeline();

        std::unordered_map<uint64_t, Entry> m_Entries;
        std::shared_ptr<ComputePipeline>    m_Pipeline;
        ShaderBindingLayoutCache            m_Layout;
        uint64_t                            m_BudgetFrame   = 0;
        uint32_t                            m_BudgetUsed    = 0;
        bool                                m_RefusalLogged = false;
    };
} // namespace Desert::Graphic::RayTracing
