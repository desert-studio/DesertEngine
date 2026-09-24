#pragma once

#include "EditMeshTopologyOperations.hpp"

#include <cstdint>

namespace Desert::Geometry
{
    // WHOLE-MESH SHAPE OPERATIONS - UE's Model tab: Subdivide (USubdividePolyTool, the Loop scheme on
    // triangles) and Mirror (UMirrorTool: a plane, the seam welded, optionally the far half cropped first).
    //
    // Same contract as EditMeshOperations.hpp: pure functions, the input is not touched, the result is a NEW
    // EditMesh; a refusal leaves nothing half-done and names what and where. Neither takes a selection: both
    // act on every triangle and hand back an empty one (every ID changes). Units are centimetres in the
    // mesh's own space.

    enum class SubdivideScheme : uint8_t
    {
        // Every triangle is cut into four at its edge midpoints; no vertex moves, so the shape, the volume
        // and every attribute value stay exactly what they were - only the tessellation is finer.
        Uniform,
        // The same split, then Loop's smoothing masks: an old interior vertex of valence n moves to
        // (1 - n*beta) * itself + beta * its neighbours (beta = 3/16 for n = 3, else 3/(8n)); a new interior
        // edge vertex to 3/8 of each end + 1/8 of each opposite corner. The surface shrinks towards the
        // smooth limit (a cube's volume falls monotonically, level after level).
        Loop,
    };

    [[nodiscard]] const char* ToString( SubdivideScheme scheme );

    // Level N quadruples the triangle count N times; above this the mesh is refused, named with the count.
    inline constexpr int kMaxSubdivideLevels     = 5;
    inline constexpr int kMaxSubdividedTriangles = 4 * 1024 * 1024;

    // SUBDIVIDE the whole mesh `levels` times (1 .. kMaxSubdivideLevels).
    //   * Open border: its vertices do not move and its new edge vertices sit at the exact midpoint, under
    //     both schemes - the border stays the polyline it was (Loop's own boundary mask is a B-spline that
    //     pulls a curved border inwards; a hole the user modelled must keep its size). A bowtie vertex is
    //     pinned the same way: it has no single ring to average.
    //   * Polygroup and material: each of the four children keeps its parent triangle's.
    //   * UVs, colours: carried with their seams - a new edge vertex gets one element per distinct pair of
    //     end elements, so an edge that was a seam is a seam in both of its halves; the value is the
    //     midpoint of the two (UVs are not smoothed: a texture must not slide over the surface).
    //   * Normals, Uniform: carried the same way, the midpoint renormalised - the faces are the same planes,
    //     so the interpolated normal is still the right one.
    //   * Normals, Loop: REBUILT smooth, one element per vertex, the angle-weighted average of its
    //     triangles. Loop rounds every edge, the ones a hard normal marked included; a hard normal left on a
    //     surface with no crease under it would draw a line the geometry does not have.
    //   * Tangents: rebuilt from UV layer 0 and the new normals (ComputeTangentsAt).
    // Refused: levels outside the range, an empty mesh, a result above kMaxSubdividedTriangles.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> SubdivideMesh( const EditMesh& mesh, int levels,
                                                                    SubdivideScheme scheme );

    enum class MirrorMode : uint8_t
    {
        // The reflection is added to the mesh as it is. A mesh that already crosses the plane overlaps its
        // own reflection there (the Report counts the vertices on the far side).
        AddMirroredCopy,
        // The half on the plane's negative side is cut away first (edges crossing the plane are split, every
        // attribute interpolated at the new vertex), then the kept half is mirrored: a symmetric mesh.
        CutAndMirror,
    };

    [[nodiscard]] const char* ToString( MirrorMode mode );

    // MIRROR the mesh across `plane` (the normal need not be unit; CutAndMirror keeps the side it points to).
    //   * Seam: a vertex within `weldTolerance` (cm, >= 0) of the plane is moved exactly onto it and SHARED
    //     by the original and the reflection - the two halves are one surface there, so the open border a
    //     half-model has along the plane closes. A triangle lying entirely in the plane is dropped: its
    //     reflection is the same face turned round, and the pair would be an inner double wall.
    //   * Winding: each reflected triangle is wound in reverse, so a surface facing outwards still does.
    //   * Normals: reflected; tangents: xyz reflected and the bitangent sign negated (a reflection turns a
    //     right-handed frame left-handed). At the seam vertices the normals are then rebuilt per polygroup
    //     from the welded geometry (RebuildNormalsByPolyGroupAt) - the reflection keeps the polygroup, so a
    //     face the plane cuts through shades as one face - and the tangents there with them.
    //   * UVs, colours, polygroup, material: copied (the texture is mirrored with the surface, as in UE).
    //   * A seam corner whose reflected value equals the original's (a UV, or a normal lying in the plane)
    //     shares the original's element: no seam is added where the attribute is continuous.
    // Refused: a zero normal; a negative tolerance; nothing left on the kept side (CutAndMirror) or nothing
    // off the plane (every triangle lies in it); a reflected triangle that cannot join the surface - an edge
    // in the plane that already has a triangle on each side (a fin), named with the triangle.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> MirrorMesh( const EditMesh& mesh, const CutPlane& plane,
                                                                 MirrorMode mode, float weldTolerance );
} // namespace Desert::Geometry
