#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

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

// THE BLADE GRASS IN THE RAY-TRACING SCENE (UE: a BLAS from the geometry the vertex factory evaluates; ray tracing
// ignores WPO unless it is enabled). A Blade variety's instances are a strip the vertex stage shapes per instance
// (Vertex_GrassBlade.glslh), so the strip mesh is not the drawn blade. Each (variety, level) gets ONE BLAS of the
// blade the raster's own BladeAt (Mesh/Surface/GrassBlade.glslh) makes for an identity instance, the median
// per-blade draws and no wind, built in blade space by "Grass: Blade Ray Tracing Vertices"
// (Programs/Grass/GrassBladeRayTracingVertices.shader) and RayTracingScene.DynamicGeometry. Every TLAS row of the
// level (RayTracingGpuInstanceSource, written on the GPU from the raster's mat4s and drawn count) instances it:
// the mat4's affine part places, orients and scales the blade exactly as the vertex stage does. Not traced: the
// per-blade curvature / bend spread and the wind (a static BLAS, UE's default).
//
// A BLAS is built when its variety's shape, segment count or level opacity changes, and is RESIDENT on every
// other frame: a still frame builds nothing, so the path tracer keeps accumulating.
namespace Desert::Graphic::System
{
    // A variety's blade shape as the GPU reads it: Mesh/Surface/GrassBlade.glslh's GrassBladeShapeData, std430
    // (the raster's GrassBladeShape buffer and the ray-tracing compute's alike).
    struct GrassBladeShapeGpu
    {
        glm::vec4 Size; // Height, Width, Taper, Curvature
        glm::vec4 Base; // BaseColor, Stiffness
        glm::vec4 Tip;  // TipColor, DryTipShare
        glm::vec4 Dry;  // DryTipColor, DryTipLength
        glm::vec4 Bend; // CurvatureSpread, BendSpread
    };
    static_assert( sizeof( GrassBladeShapeGpu ) == 80 );

    // ONE level of the blade strip in blade space: (level + 1) rows of two vertices, position (side -1/+1, height
    // share t 0..1, 0), uv (side / 2 + 1/2, t), and two triangles per segment. GrassRenderer::BladeStrip draws it,
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

        // The BLASes of variety @p key, one per submesh (level) of @p strip, built from @p shape: those that
        // changed are queued for this frame's vertex pass and build, the others are resident. @p nonOpaque: bit i
        // = level i's segment runs the alpha test (its section material is Masked, MeshRenderer::ShadingOf). Index
        // i of the value is level i's BLAS, valid until the next BeginFrame that is not required.
        [[nodiscard]] Common::ResultStr<std::span<const RayTracing::RayTracingGeometryResource* const>>
        PrepareVariety( RDG::Builder& graph, uint64_t key, const Desert::Mesh& strip,
                        const GrassBladeShapeGpu& shape, uint64_t nonOpaque );

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
        struct VarietyState
        {
            BladeKey                                                                      Key;
            bool                                                                          Valid = false;
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
            glm::uvec4                                       Counts{ 0u }; // x = vertices, y = indices
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
