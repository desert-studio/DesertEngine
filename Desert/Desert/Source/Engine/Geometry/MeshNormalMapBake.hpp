#pragma once

#include "RenderMeshData.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // UE's Bake Normal Map (Modeling Mode -> Bake -> BakeTx, UBakeMeshAttributeMapsTool with the
    // TangentSpaceNormal map; the work is FMeshMapBaker + FMeshNormalMapEvaluator, ModelingOperators
    // MeshMapBaker.cpp / MeshNormalMapEvaluator.cpp), ported as its pattern: every texel of the TARGET's UV layout
    // is a point on the target surface; a ray along the target normal finds the DETAIL surface within the
    // projection distance (UE ProjectionDistance, the "cage"), and the detail's interpolated normal is written in
    // the target's tangent space. Texels no triangle covers are filled by UE's gutter
    // (FMeshMapBaker::SetGutterSize, nearest covered texel, here a dilation of GutterTexels rings); a texel whose
    // ray finds nothing gets UE's default value for the map, the flat normal (0, 0, 1).
    //
    // THE TANGENT FRAME IS THE RENDERER'S. Mesh/Surface/Vertex_Static.glslh builds TBN = (normalize(M*T),
    // normalize(M*B), normalize(M*N)) per vertex with M the world normal matrix, interpolates it unnormalised and
    // shades with normalize(TBN * decoded); the texel stores the exact inverse of that, solve(TBN, n), so the
    // round trip returns the detail normal on any frame the vertex streams carry - orthogonal or not. The decoder
    // (Common/TangentNormal.glslh) rebuilds Z from X and Y, so the stored Z is >= 0 by construction.
    //
    // THE TEXTURE'S ROWS ARE THE SHADER'S. The vertex shader samples (u, 1 - v), so texel row y holds mesh
    // v = 1 - (y + 0.5) / height; row 0 is the first row of the written image.
    //
    // UE differences, stated: one sample per texel (UE SamplesPerPixel default 1 as well); no
    // "fail to nearest point" correspondence (UE's bFailToNearestPoint is off for the normal map by default).
    //
    // AMBIENT OCCLUSION AND CURVATURE (UE FMeshOcclusionMapEvaluator / FMeshCurvatureMapEvaluator, the same
    // FMeshMapBaker texel loop and the same detail correspondence as the normal map):
    // - Occlusion: from the detail point the ray found (pushed off the surface along its normal), OcclusionRays
    //   directions over the hemisphere around the detail normal - UE's spherical-Fibonacci set, narrowed by
    //   SpreadAngle (180 = the whole hemisphere) and with the rays closer than BiasAngle to the tangent plane
    //   dropped - are cast against the detail BVH within OcclusionMaxDistance (0 = unbounded, UE's default);
    //   each ray weighs cos(theta) (UE's cosine distribution), and the texel stores 1 - occluded weight / total
    //   weight as grey: white = open sky, UE's default value (1) where the ray found no detail.
    // - Curvature: the detail's per-vertex MEAN curvature (UE ECurvatureType::Mean; cotangent Laplacian over the
    //   detail welded by position, as UE's FMeshVertexCurvatureCache over the topology, not the render seams),
    //   interpolated at the hit, mapped to grey (UE EColorMode::Grayscale): 0.5 = flat, brighter = convex,
    //   darker = concave, full scale at RangeMultiplier * (mean + standard deviation of |H|) over the detail
    //   (UE FMeshVertexCurvatureCache's MaxValue statistic times ColorRangeMultiplier).
    struct MeshMapBakeSettings
    {
        int    Resolution         = 1024; // UE EBakeTextureResolution, square
        double ProjectionDistance = 3.0;  // cm, UE UBakeInputMeshProperties::ProjectionDistance (default 3)
        int    GutterTexels       = 4;    // UE FMeshMapBaker gutter size (default 4)

        bool   Occlusion            = false; // UE EBakeMapType::AmbientOcclusion
        int    OcclusionRays        = 16;    // UE UBakeOcclusionMapToolProperties::OcclusionRays (default 16)
        double OcclusionMaxDistance = 0.0;   // cm, 0 = unbounded (UE default)
        double OcclusionSpreadAngle = 180.0; // degrees (UE default 180)
        double OcclusionBiasAngle   = 15.0;  // degrees (UE default 15)

        bool   Curvature                = false; // UE EBakeMapType::Curvature
        double CurvatureRangeMultiplier = 1.0;   // UE UBakeCurvatureMapToolProperties::ColorRangeMultiplier
    };
    // One mesh in the bake's frame (world space): positions and the vertex frame as the renderer transforms them.
    struct BakeSurface
    {
        std::vector<glm::dvec3> Positions;
        std::vector<glm::dvec3> Normals;
        std::vector<glm::dvec3> Tangents;
        std::vector<glm::dvec3> Bitangents;
        std::vector<glm::dvec2> UVs;
        std::vector<glm::uvec3> Triangles; // indices into the arrays above, render winding
    };

    // @p render under @p world (and each submesh's Transform), its frame through the normal matrix and normalised
    // per vertex, as Vertex_Static.glslh does. Refused, naming the vertex, when a vertex has a zero tangent or
    // bitangent - a mesh without a tangent space cannot carry a tangent-space map.
    [[nodiscard]] Common::ResultStr<BakeSurface>
    BakeSurfaceFromRender( const RenderMeshData& render, const glm::dmat4& world, bool needTangentFrame );

    struct MeshMapBakeResult
    {
        int                  Width  = 0;
        int                  Height = 0;
        std::vector<uint8_t> Rgba;              // Width * Height * 4, row 0 first
        std::vector<uint8_t> OcclusionRgba;     // the same layout, grey; empty unless Settings.Occlusion
        std::vector<uint8_t> CurvatureRgba;     // the same layout, grey; empty unless Settings.Curvature
        std::size_t          CoveredTexels = 0; // inside a target triangle in UV space
        std::size_t          HitTexels     = 0; // covered, and the ray found the detail surface
        std::size_t          GutterTexels  = 0; // filled by the gutter
    };

    // Refused, with numbers: a resolution outside [16, 8192] or a non-positive projection distance; a target with
    // no triangles or no UVs; a detail with no triangles; a bake in which NO covered texel found the detail
    // surface (the two meshes do not overlap within the projection distance - the map would be flat everywhere);
    // fewer than 1 or more than 1024 occlusion rays, a spread outside (0, 180] or a bias outside [0, 90).
    [[nodiscard]] Common::ResultStr<MeshMapBakeResult>
    BakeMeshMaps( const BakeSurface& target, const BakeSurface& detail, const MeshMapBakeSettings& settings );
} // namespace Desert::Geometry
