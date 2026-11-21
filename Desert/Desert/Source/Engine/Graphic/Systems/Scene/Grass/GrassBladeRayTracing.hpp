#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <glm/glm.hpp>

#include <array>
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

// THE BLADE GRASS IN THE RAY-TRACING SCENE (UE: a BLAS from the geometry the vertex factory evaluates; ray tracing
// ignores WPO unless it is enabled). A Blade variety's instances are a strip the vertex stage shapes per instance
// (Vertex_GrassBlade.glslh), so the strip mesh is not the drawn blade. Each (variety, level) gets ONE BLAS of the
// blade the raster's own BladeAt (Mesh/Surface/GrassBlade.glslh) makes for an identity instance, the median
// per-blade draws and no wind, built in blade space by "Grass: Blade Ray Tracing Vertices"
// (Programs/Grass/GrassBladeRayTracingVertices.shader) and RayTracingScene.DynamicGeometry. Every TLAS row of the
// level (RayTracingGpuInstanceSource, written on the GPU from the raster's mat4s and drawn count) instances it:
// the mat4's affine part places, orients and scales the blade exactly as the vertex stage does. Not traced: the
// per-blade curvature / bend spread and the wind (a static BLAS, UE's default).
// DEFORM-RT-TRAMPLE (UE: the ray-tracing geometry takes the same deformation the raster evaluates): beside the
// standing BLAS (step 0) each level builds kRayTracingBladePressSteps PRESSED ones, the same BladeAt laid along the
// blade's own forward axis with press k / K. A TLAS row whose root the view's deformation field presses (only rows
// inside the field's trampled box read it) instances the nearest step through an affine fit to its own pressed
// curve (RayTracingGpuInstances.shader): trampling moves rows, it never builds a BLAS. RT-PRESS-LAZY: the pressed
// set exists only once the view's field has held a press (built that frame, kept after), and is traced only while
// one is live - a scene with no field pays no pressed BLAS, segment run or row stride.
//
// A BLAS is built when its variety's shape, segment count or level opacity changes, and is RESIDENT on every
// other frame: a still frame builds nothing, so the path tracer keeps accumulating.
namespace Desert::Graphic::System
{
    // A variety's blade shape as the GPU reads it: Mesh/Surface/GrassBlade.glslh's GrassBladeShapeData, std430
    // (the raster's GrassBladeShape buffer and the ray-tracing compute's alike).
    struct GrassBladeShapeGpu
    {
        glm::vec4                Size;    // Height, Stiffness, Curvature, card count
        glm::vec4                Bend;    // CurvatureSpread, BendSpread, TrampledLength
        std::array<glm::vec4, 8> Cards{}; // u0, v tip, u1, v root (GRASS_BLADE_MAX_CARDS)
        // GRASS-VIS: x = the variety's first visibility-buffer raster-stream row, y = the strip's level-0
        // segments, z = its bucket count (GrassRenderer::PrepareVariety).
        glm::uvec4                Visibility{ 0u };
        std::array<glm::uvec4, 2> BucketFirst{}; // the first instance of bucket b < 8, [b / 4][b % 4]
        // CPU-GRASS-POOL-5: x = the camera list's first slot in the shared ids pool (its CameraIds range), so
        // the raster's gl_InstanceIndex - x is the variety's own slot; y, z = the view's blade setup records'
        // device address lo, hi (FPS-GRASS-RESOLVE-2, GrassRenderer::CommitBladeSetup; 0: none), w = 0.
        glm::uvec4 Lists{ 0u };
        // FPS-GRASS-HULL: x, y = the opacity mask's level-0 texels, z = 1 when the raster's strip spans only its
        // card's hull (GrassQuality::BladeCardHull and a cooked hull), w = 0; Hull = GrassBladeDraw::CardHulls.
        glm::vec4                  HullTexels{ 0.0f };
        std::array<glm::vec4, 128> Hull{}; // 8 cards x 4 classes x 8 bands, (lo, hi) pairs (GrassBlade.glslh)
    };
    static_assert( sizeof( GrassBladeShapeGpu ) == 32 + 16 * 8 + 16 * 4 + 16 + 16 * 128 );
    // The strip levels BucketFirst holds (a blade's buckets are its strip's levels: Segments halved down to one).
    inline constexpr uint32_t kGrassBladeVisibilityMaxBuckets = 8u;

    // GRASS-VIS-MESH-1: a mesh grass SECTION's raster-stream rows - Mesh/Surface/GrassMeshVisibility.glslh's
    // GrassMeshVisibilityData, std430, byte for byte (GrassRenderer::PrepareVariety writes it every frame: the mesh's
    // device addresses are read when used).
    inline constexpr uint32_t kGrassMeshVisibilityMaxSubmeshes = 8u;
    inline constexpr uint32_t kGrassMeshVisibilityMaxRanges    = 32u;
    struct GrassMeshVisibilityGpu
    {
        glm::uvec4                Visibility{ 0u }; // first row, rows per instance, level count, submesh count
        // x = the slots the section's list holds (its capacity), y = the camera list's first slot in the ids pool,
        // z = the setup records' first row in the bound setup buffer, w = 1: records this frame
        // (FPS-GRASS-MESH-SETUP-5)
        glm::uvec4                Slots{ 0u };
        std::array<glm::uvec4, 2> BucketFirst{};    // the first slot of bucket (= level) b < 8, [b / 4][b % 4]
        // [0] vertex address lo, hi, stream address lo, hi; [1] index address lo, hi, vertex stride, stream stride
        std::array<glm::uvec4, 2>                                Geometry{};
        std::array<glm::uvec4, kGrassMeshVisibilityMaxSubmeshes> Submeshes{}; // VertexOffset, VertexCount, drawn, 0
        std::array<glm::uvec4, kGrassMeshVisibilityMaxRanges>    Ranges{}; // L x submeshes + s: first index, triangles, row offset, 0
    };
    static_assert( sizeof( GrassMeshVisibilityGpu ) == 16 * ( 1 + 1 + 2 + 2 + 8 + 32 ) );

    // ONE level of the blade strip in blade space: (level + 1) rows of two vertices, position (side -1/+1, height
    // share t 0..1, 1 / segments - the raster's hull row span, FPS-GRASS-HULL), uv (side / 2 + 1/2, t), and two triangles per segment. GrassRenderer::BladeStrip draws it,
    // GrassBladeRayTracing shapes it into the level's BLAS.
    struct GrassBladeStripLevel
    {
        std::vector<Vertex> Vertices;
        std::vector<Index>  Indices;
    };
    [[nodiscard]] GrassBladeStripLevel MakeGrassBladeStripLevel( uint32_t segments );

    inline constexpr std::string_view kGrassBladeRayTracingVerticesPass = "Grass: Blade Ray Tracing Vertices";

    class GrassBladeRayTracing
    {
    public:
        // Starts a frame of the view: @p required = the view's TLAS is built this frame (RayTracingSceneRequired).
        // Not required: every BLAS is released (they are rebuilt the first frame rays are traced again).
        void               BeginFrame( bool required );
        [[nodiscard]] bool IsRequired() const
        {
            return m_Required;
        }

        // The BLASes of variety @p key, one per submesh (level) of @p strip - and, while @p pressed (the view's
        // deformation field holds a press, GrassRenderer::HoldsPress), kRayTracingBladePressSteps more per level
        // (RT-PRESS-LAZY, UE: an optional geometry variant is made by the feature that needs it, never paid by a
        // scene without it) - built from @p shape: those that changed are queued for this frame's vertex pass and
        // build, the others are resident. The pressed set is built the first frame it is wanted and kept (not
        // registered, not traced) while no field presses, so a press that comes back builds nothing.
        // @p nonOpaque: bit i = level i's segments run the alpha test (its section material is Masked,
        // MeshRenderer::ShadingOf). Index step * levels + i of the value is level i's BLAS of press step / K (step 0:
        // standing, so [0, levels) are the standing levels); not @p pressed: only those. Valid until the next
        // BeginFrame that is not required.
        [[nodiscard]] Common::ResultStr<std::span<const RayTracing::RayTracingGeometryResource* const>>
        PrepareVariety( RDG::Builder& graph, uint64_t key, const Desert::Mesh& strip,
                        const GrassBladeShapeGpu& shape, uint64_t nonOpaque, bool pressed );

        // Declares "Grass: Blade Ray Tracing Vertices" for the levels PrepareVariety queued (none: no pass) and
        // retires the varieties this frame did not prepare.
        [[nodiscard]] Common::BoolResultStr AddPass( RDG::Builder& graph );

        // This frame's BLAS requests (RayTracingScene.DynamicGeometry builds them after the vertex pass) and the
        // storage of the BLASes built earlier; the TLAS reads both. Valid for the graph they were declared in.
        [[nodiscard]] std::span<const RayTracing::RayTracingDynamicGeometryBuild> GetBuilds() const
        {
            return m_Builds;
        }
        [[nodiscard]] std::span<const RDG::BufferRef> GetResident() const
        {
            return m_Resident;
        }

    private:
        struct BladeKey
        {
            glm::vec4 Size{ 0.0f };
            glm::vec4 Bend{ 0.0f };
            uint32_t  Levels                              = 0;
            uint64_t  NonOpaque                           = 0;
            bool      operator==( const BladeKey& ) const = default;
        };
        // One set of a variety's targets - the standing levels [0, levels) or the pressed ones [levels, levels *
        // (1 + K)). Key: what its last declared builds were made from (Valid: they were declared). The set holds it
        // only while every one of its targets IsBuilt (their graph ended); otherwise the builds are pending and are
        // made again.
        struct TargetSet
        {
            BladeKey Key;
            bool     Valid = false;
        };
        struct VarietyState
        {
            std::size_t                                                                   Levels = 0;
            std::array<TargetSet, 2> Sets; // [0] standing, [1] pressed (made the first frame a press is live)
            std::vector<std::unique_ptr<RayTracing::RayTracingDynamicMeshGeometryTarget>> Targets;
            std::vector<const RayTracing::RayTracingGeometryResource*> Geometry; // Targets, as the gather reads
            bool                                                       Seen = false; // prepared this frame
        };
        // One level whose BLAS this frame builds.
        struct LevelWork
        {
            RayTracing::RayTracingDynamicMeshGeometryTarget* Target       = nullptr;
            uint64_t                                         ScratchBytes = 0;
            RDG::BufferRef                                   Shape;
            RDG::BufferRef                                   StripVertices;
            RDG::BufferRef                                   StripIndices;
            RDG::BufferRef                                   Vertices;
            RDG::BufferRef                                   Indices;
            RDG::BufferRef                                   Storage;
            glm::uvec4                                       Counts{ 0u }; // x = vertices, y = indices, z = press step, w = steps
        };

        [[nodiscard]] Common::BoolResultStr EnsurePipeline();

        bool                                                    m_Required = false;
        std::unordered_map<uint64_t, VarietyState>              m_Varieties;
        std::vector<LevelWork>                                  m_Work;
        std::vector<RayTracing::RayTracingDynamicGeometryBuild> m_Builds;
        std::vector<RDG::BufferRef>                             m_Resident;
        std::shared_ptr<ComputePipeline>                        m_Pipeline;
        mutable ShaderBindingLayoutCache                        m_Layout;
    };
} // namespace Desert::Graphic::System
