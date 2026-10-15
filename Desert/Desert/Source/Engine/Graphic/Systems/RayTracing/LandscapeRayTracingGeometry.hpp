#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <cstdint>
#include <memory>

// The API side of a landscape tile's ray-tracing geometry: what LandscapeRayTracing (Renderer) asks of the RHI
// and VulkanLandscapeRayTracingGeometry (RenderCore) implements. RenderCore by placement
// (BuildScripts/DesertModules.lua), like RayTracingGeometry.hpp: the RHI must not include a Renderer header,
// so every type the Vulkan side needs lives here and nothing here names a terrain or landscape draw type.
namespace Desert::Graphic::RayTracing
{
    // A tile's grid at its raster grid LOD (TerrainInstance::Params.w): cells per side as LandscapeLodGridCoord
    // counts them, the grid's vertices, and the triangles of ONE of the two geometries (opaque, masked).
    struct LandscapeRayTracingGrid
    {
        uint32_t Cells        = 0;
        uint32_t Side         = 0; // Cells + 1
        uint32_t GridVertices = 0; // Side^2; the buffer holds one more, the NaN vertex
        uint32_t Triangles    = 0; // 2 * Cells^2 per geometry
        bool     operator==( const LandscapeRayTracingGrid& ) const = default;
    };

    [[nodiscard]] inline LandscapeRayTracingGrid LandscapeRayTracingGridAt( uint32_t quadsPerTile,
                                                                            uint32_t gridLod )
    {
        const uint32_t          stride = 1u << gridLod;
        LandscapeRayTracingGrid grid;
        grid.Cells        = ( quadsPerTile + stride - 1u ) / stride;
        grid.Side         = grid.Cells + 1u;
        grid.GridVertices = grid.Side * grid.Side;
        grid.Triangles    = 2u * grid.Cells * grid.Cells;
        return grid;
    }

    // How one tile's BLAS is made this frame.
    struct LandscapeBlasPlan
    {
        uint64_t ScratchBytes = 0;     // includes the scratch-address alignment slack
        bool     Update       = false; // refit in place (else a full build)
    };

    // The API side of one tile: its vertex + index buffers and its BLAS, alive across frames. Also the
    // RayTracingGeometryResource its TLAS instance points at.
    class LandscapeRayTracingGeometryTarget : public RayTracingGeometryResource
    {
    public:
        // Makes the buffers and the BLAS fit @p grid (recreating all three when the grid changed) and answers the
        // scratch of this frame's build. @p update asks for a refit; it is granted only when nothing was
        // recreated.
        [[nodiscard]] virtual Common::ResultStr<LandscapeBlasPlan> Prepare( const LandscapeRayTracingGrid& grid,
                                                                            bool update ) = 0;
        [[nodiscard]] virtual RDG::ExternalBuffer& GetVertices() = 0; // Desert::Vertex, GridVertices + 1
        [[nodiscard]] virtual RDG::ExternalBuffer& GetIndices()  = 0; // uint32, opaque range then masked range
        [[nodiscard]] virtual RDG::ExternalBuffer& GetStorage()  = 0; // the BLAS storage
        // Records the build (or refit, @p plan.Update) inside the geometry pass's exec; @p scratchOffset is this
        // tile's slice of the pass's shared scratch buffer.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& vertices,
                                                                 const RDG::BufferBinding& indices,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint64_t scratchOffset, bool update ) = 0;
        // Bytes of the tile's buffers and BLAS (each has its own ledger row).
        [[nodiscard]] virtual uint64_t GetMemoryBytes() const = 0;
    };

    // The API's tile object (VulkanLandscapeRayTracingGeometry).
    [[nodiscard]] std::unique_ptr<LandscapeRayTracingGeometryTarget> CreateLandscapeRayTracingGeometryTarget();
} // namespace Desert::Graphic::RayTracing
