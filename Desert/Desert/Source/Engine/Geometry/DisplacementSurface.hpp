#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/DisplacementTessellation.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// THE DISPLACED SURFACE ON THE CPU: the same points, heights and attributes the GPU pair
// DisplacementHeights.shader + DisplacementEmit.shader write, evaluated without a device. Three consumers need a
// displaced surface that no camera chose and that exists before any frame:
// - the Baked mode (the dense mesh cooked into DDC and drawn as an ordinary StaticMesh with a LOD chain),
// - collision for BOTH modes (one fixed level, so a character stands on the same stones whatever the camera),
// - SDF and GI cards of the Runtime mode (by the maximum level, so software tracing sees the relief the
//   rasteriser and hardware RT draw at full density).
// One function serves all three; only the level vector differs (DisplacementLevelsForEdge / MaxLevels).
namespace Desert::Geometry
{
    // A height texture decoded on the CPU: linear RGBA in 0..1, mip 0 first, each further mip a 2x2 box of the
    // previous (the chain the sampler walks). Built once per texture by MakeDisplacementHeightImage.
    struct DisplacementHeightImage
    {
        struct Mip
        {
            uint32_t               Width  = 0;
            uint32_t               Height = 0;
            std::vector<glm::vec4> Texels; // row-major
        };
        std::vector<Mip> Mips;
    };

    // Takes mip 0 (width x height linear RGBA texels) and builds the box chain down to 1x1.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const DisplacementHeightImage>>
    MakeDisplacementHeightImage( uint32_t width, uint32_t height, std::vector<glm::vec4> texels );

    // One submesh's height as the material states it (Core::Formats::SurfaceHeight resolved against the material
    // row) - the CPU twin of DisplacementMaterialRecord. Image null = the submesh has no height and moves by 0.
    struct DisplacementSurfaceMaterial
    {
        std::shared_ptr<const DisplacementHeightImage> Image;
        uint32_t                                       Channel      = 0;
        uint32_t                                       UVSet        = 0;
        float                                          ScaleCm      = 0.0f;
        float                                          Center       = 0.0f;
        glm::vec2                                      Tiling       = { 1.0f, 1.0f };
        glm::vec2                                      Offset       = { 0.0f, 0.0f };
        float                                          Rotation     = 0.0f;
        bool                                           HasTransform = false;
    };

    // The source mesh the topology was built from (LOD0 render data).
    struct DisplacementSurfaceSource
    {
        std::span<const Vertex>            Vertices;
        std::span<const MeshVertexStreams> Streams; // empty = the mesh has no Color/UV1 streams
        std::span<const Index>             Indices;
        std::span<const Submesh>           Submeshes;
    };

    // A displaced mesh: per submesh one contiguous vertex and triangle range (indices submesh-local, offsets as
    // StaticMesh reads them: VertexOffset in vertices, IndexOffset/IndexCount in uint32 units). Submesh names,
    // transforms and order are the source's, so material slots keep their sections.
    struct DisplacedSurface
    {
        std::vector<Vertex>            Vertices;
        std::vector<MeshVertexStreams> Streams; // empty when the source has none
        std::vector<Index>             Indices;
        std::vector<Submesh>           Submeshes;
    };

    // The fixed level of every cluster for a target diced edge length (cm): ceil(log2(BaseEdgeCm / edge)),
    // clamped to the cluster's MaxLevel. A level above MaxLevel is not representable in the fixed-point
    // barycentrics; the count of clamped clusters goes to @p clamped so the caller can name it.
    [[nodiscard]] Common::ResultStr<std::vector<uint8_t>>
    DisplacementLevelsForEdge( const DisplacementTopology& topology, float edgeLengthCm,
                               uint32_t* clamped = nullptr );

    // Every cluster at its MaxLevel: the densest surface the runtime cache can ever draw.
    [[nodiscard]] std::vector<uint8_t> DisplacementMaxLevels( const DisplacementTopology& topology );

    // Tessellates every cluster at @p levels and displaces it exactly as the GPU pair does: point = welds x
    // fixed-point weights, direction = normalised weighted weld direction, height = mean over the point's samples
    // of (texel[channel] - Center) * ScaleCm at the level's mip; attributes interpolated from the sample triangle.
    // @p materials: one per source submesh.
    [[nodiscard]] Common::ResultStr<DisplacedSurface>
    BuildDisplacedSurface( const DisplacementTopology& topology, const DisplacementSurfaceSource& source,
                           std::span<const DisplacementSurfaceMaterial> materials,
                           std::span<const uint8_t>                     levels );
} // namespace Desert::Geometry
