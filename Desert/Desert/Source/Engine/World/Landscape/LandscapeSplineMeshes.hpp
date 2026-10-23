#pragma once

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/World/Landscape/LandscapeSplines.hpp>

#include <span>
#include <vector>

namespace Desert::World::Landscape
{
    /// The geometry of one source mesh of a spline mesh entry (a mesh asset's, or a built-in shape's), read only.
    struct LandscapeSplineMeshSource
    {
        std::span<const Vertex>  Vertices;
        std::span<const Index>   Indices;
        std::span<const Submesh> Submeshes;
    };

    /// One entry's meshes along one segment, bent and placed in world cm: every piece of the entry in one
    /// mesh, with the source's submeshes (submesh i holds piece after piece of source submesh i, so it draws
    /// with the source's slot i).
    struct LandscapeSplineDeformedMesh
    {
        std::vector<Vertex>  Vertices;
        std::vector<Index>   Indices;
        std::vector<Submesh> Submeshes;
    };

    /**
     * @brief Segment @p segment's spline meshes (UE 5.8: ULandscapeSplineSegment::UpdateSplineMeshes, and the
     * per-vertex bend of USplineMeshComponent / SplineMeshShaderParams), one result per entry of its
     * SplineMeshes, in order.
     *
     * Placement is UE's: the entries are laid one after another along the curve's length, cycling through the
     * list, as long as the next one's half still fits; then every piece is stretched by the same factor so the
     * pieces cover the segment exactly — each piece ends where the next begins, at the same curve point, so
     * there is no gap and no overlap. A mesh's length is its extent along its ForwardAxis times the scale.
     *
     * The bend: a vertex's place along the forward extent (0 at the back, 1 at the front) picks its distance
     * along the piece, the curve point at that arc length is the origin, and its side and up coordinates go
     * along the curve's right and up there (up is perpendicular to the curve's direction, so the mesh follows
     * the slope). Normals and tangents go through the same frame (normals by the inverse scale).
     *
     * @p sources holds one source per entry (SplineMeshes order). A source with no forward extent places
     * nothing and yields an empty result.
     */
    std::vector<LandscapeSplineDeformedMesh> BuildLandscapeSplineSegmentMeshes(
         const LandscapeSplines& splines, size_t segment, std::span<const LandscapeSplineMeshSource> sources );
} // namespace Desert::World::Landscape
