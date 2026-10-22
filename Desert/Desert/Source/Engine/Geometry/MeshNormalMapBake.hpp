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
    struct NormalMapBakeSettings
    {
        int    Resolution         = 1024; // UE EBakeTextureResolution, square
        double ProjectionDistance = 3.0;  // cm, UE UBakeInputMeshProperties::ProjectionDistance (default 3)
        int    GutterTexels       = 4;    // UE FMeshMapBaker gutter size (default 4)
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

    struct NormalMapBakeResult
    {
        int                  Width  = 0;
        int                  Height = 0;
        std::vector<uint8_t> Rgba;              // Width * Height * 4, row 0 first
        std::size_t          CoveredTexels = 0; // inside a target triangle in UV space
        std::size_t          HitTexels     = 0; // covered, and the ray found the detail surface
        std::size_t          GutterTexels  = 0; // filled by the gutter
    };

    // Refused, with numbers: a resolution outside [16, 8192] or a non-positive projection distance; a target with
    // no triangles or no UVs; a detail with no triangles; a bake in which NO covered texel found the detail
    // surface (the two meshes do not overlap within the projection distance - the map would be flat everywhere).
    [[nodiscard]] Common::ResultStr<NormalMapBakeResult>
    BakeTangentNormalMap( const BakeSurface& target, const BakeSurface& detail,
                          const NormalMapBakeSettings& settings );
} // namespace Desert::Geometry
