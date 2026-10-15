#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/LandscapeRayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainBatch.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// LANDSCAPE IN THE VIEW'S TLAS (UE FLandscapeRayTracingImpl, per view, per section). Every landscape tile the
// view's terrain renderer drew this frame is one TLAS instance (Kind = Landscape, custom index = the tile's
// GPU-scene row) over a BLAS of the tile at the view's RASTER LOD:
//   * "Landscape: Ray Tracing Vertices" (compute, LandscapeRayTracingVertices.shader) evaluates the raster's own
//     vertex function (LandscapeVertexEval.glslh) from the tile's heightmap and TerrainInstance row into the
//     tile's persistent vertex buffer, and classifies every cell against the visibility layer into an OPAQUE
//     index range and a MASKED (non-opaque, alpha-tested) one; a cell all hole is in neither;
//   * "Landscape: Ray Tracing Geometry" builds the tile's BLAS from them (BUILD when the grid, the heights or the
//     holes changed — a sculpt or paint stroke bumps the revision — REFIT when only the continuous LOD moved the
//     morphed vertices; nothing when the tile's row and revisions are what the BLAS already holds).
// The TLAS pass reads every tile's BLAS storage (GetFrame().Geometry), which orders it after the build.
// Renderer-side only: the module boundary keeps Renderer and Landscape apart, so the inputs are what the terrain
// renderer receives (heightmap image, LandscapeTileDraw, LandscapeWeightDraw), never LandscapeTileData.
namespace Desert::Graphic
{
    class Image2D;
    class ComputePipeline;
    class FrameTextures;
} // namespace Desert::Graphic

namespace Desert::Graphic::RayTracing
{
    inline constexpr std::string_view kLandscapeRayTracingVerticesPass = "Landscape: Ray Tracing Vertices";
    inline constexpr std::string_view kLandscapeRayTracingGeometryPass = "Landscape: Ray Tracing Geometry";
    inline constexpr std::string_view kLandscapeRayTracingShader       = "LandscapeRayTracingVertices";
    inline constexpr uint32_t         kLandscapeRayTracingLocalSize    = 64; // LocalSize of the shader

    // One drawn tile, as the view's terrain renderer has it this frame.
    struct LandscapeRayTracingTile
    {
        uint32_t Entity      = 0;
        uint32_t GpuSceneRow = 0; // the tile's ObjectMotions row: the TLAS custom index
        // The heightmap and the weight pages the raster samples, and the revision of their pixels (bumped by
        // LandscapeECSSystem whenever it rewrites them in place: a sculpt or paint stroke, a neighbour's ring).
        std::shared_ptr<Image2D> Heightmap;
        uint64_t                 HeightRevision = 0;
        std::shared_ptr<Image2D> Weightmap; // null: no painted layer, no hole
        uint64_t                 WeightRevision = 0;
        // The raster's row for this tile (TerrainRenderer::PrepareFrame): LOD, borders, frame, layer data.
        System::TerrainInstance Instance;
        // Layers and the visibility layer, recorded for ray-traced shading and the alpha test (RT-SHADOWS, GI).
        System::LandscapeWeightDraw Weights;
        bool                        CastShadows = true;
    };

    // Ray-traced shading's view of a tile: its row and what its surface is made of.
    struct LandscapeRayTracingMaterial
    {
        uint32_t                    GpuSceneRow = 0;
        System::LandscapeWeightDraw Weights;
    };

    // This frame's landscape for the view's TLAS. Refs are valid only for the graph they were declared in.
    struct LandscapeRayTracingFrame
    {
        std::vector<RayTracingPrimitive>         Primitives; // Kind = Landscape, one per tile
        std::vector<RDG::BufferRef>              Geometry;   // every tile's BLAS storage: the TLAS pass reads them
        std::vector<LandscapeRayTracingMaterial> Materials;  // by tile, in Primitives order
        uint32_t                                 Built       = 0;
        uint32_t                                 Refit       = 0;
        uint64_t                                 MemoryBytes = 0; // vertex + index buffers and BLAS of every tile
    };

    // One view's landscape ray-tracing state (SceneRenderer owns one per view, like RayTracingSceneRenderer).
    class LandscapeRayTracing
    {
    public:
        LandscapeRayTracing();
        ~LandscapeRayTracing();
        LandscapeRayTracing( const LandscapeRayTracing& )            = delete;
        LandscapeRayTracing& operator=( const LandscapeRayTracing& ) = delete;

        // Once per frame per view, after TerrainRenderer::PrepareFrame (the tiles' rows) and the motion rows, and
        // before RayTracingSceneRenderer::AddPasses. @p required = RayTracingSceneRequired of the view's methods:
        // false drops every tile's state and declares nothing. Tiles not in @p tiles are dropped (their API
        // objects retire on the frame ring). Errors: the pipeline is missing, a tile's target could not be
        // prepared, or a node could not be declared (the frame then has no landscape in its TLAS).
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, FrameTextures& textures, bool required,
                                                       std::span<const LandscapeRayTracingTile> tiles );

        [[nodiscard]] const LandscapeRayTracingFrame& GetFrame() const
        {
            return m_Frame;
        }

    private:
        // What a tile's BLAS was last built from: any change rebuilds it.
        struct BuildKey
        {
            const Image2D* Heightmap                           = nullptr;
            uint64_t       HeightRevision                      = 0;
            const Image2D* Weightmap                           = nullptr;
            uint64_t       WeightRevision                      = 0;
            int32_t        VisibilityLayer                     = -1;
            uint32_t       QuadsPerTile                        = 0;
            uint32_t       GridLod                             = 0;
            uint32_t       NeighbourMask                       = 0;
            bool           operator==( const BuildKey& ) const = default;
        };

        struct TileState
        {
            std::unique_ptr<LandscapeRayTracingGeometryTarget> Target;
            BuildKey                                           Key;
            System::TerrainInstance                            Instance; // the row the BLAS holds
            bool                                               Valid = false;
        };

        [[nodiscard]] Common::BoolResultStr EnsurePipeline();

        std::unordered_map<uint32_t, TileState> m_Tiles; // by tile entity
        std::shared_ptr<ComputePipeline>        m_Pipeline;
        ShaderBindingLayoutCache                m_Layout;
        LandscapeRayTracingFrame                m_Frame;
    };
} // namespace Desert::Graphic::RayTracing
