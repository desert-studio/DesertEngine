#pragma once

#include "EditMeshTopologyOperations.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Geometry
{
    // WHOLE-MESH SHAPE OPERATIONS - UE's Model tab: Plane Cut (UPlaneCutTool), Trim (UTrimMeshesTool) and
    // Mirror (UMirrorTool: a plane, the seam welded, optionally the far half cropped first). Subdivide runs on
    // the ported core (MeshRegionOperation.hpp SubdivideMesh, P15).
    //
    // Same contract as EditMeshOperations.hpp: pure functions, the input is not touched, the result is a NEW
    // EditMesh; a refusal leaves nothing half-done and names what and where. None takes a selection: each
    // acts on every triangle and hands back an empty one (Plane Cut: its cap), as every ID changes. Units are
    // centimetres in the mesh's own space.

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
    enum class PlaneCutMode : uint8_t
    {
        // The half on the plane's negative side is cut away; the mesh is what is left.
        DiscardNegativeSide,
        // Both halves are kept as two meshes: the positive one replaces the mesh, the negative one comes back
        // as PlaneCutOutcome::OtherHalf (the editor puts it on a new entity).
        KeepBothHalves,
    };

    [[nodiscard]] const char* ToString( PlaneCutMode mode );

    struct PlaneCutOutcome
    {
        // The positive half; its Selection is the cap's polygroup (PolyGroup mode, empty without a cap).
        MeshEditOutcome Kept;
        // The negative half, capped the same way (its cap is its own new polygroup); only for KeepBothHalves.
        std::optional<EditMesh> OtherHalf;
    };

    // PLANE CUT the whole mesh (UE's UPlaneCutTool; FMeshPlaneCut): every edge crossing `plane` is split (each
    // attribute interpolated at the new vertex), the triangles on the negative side go - the side the normal
    // points to is KEPT, as in MirrorMesh's CutAndMirror - and, with `fillHole`, every outline the cut opens is
    // closed by a flat cap of a new polygroup (ear-clipped, UVs a planar projection at the neighbouring faces'
    // texel density, normals flat, tangents rebuilt; see CutAwayPositiveSide). A triangle lying in the plane
    // stays with the half its face looks into. A closed mesh stays closed; the two halves' volumes sum to the
    // original's. The normal need not be unit.
    // Refused: a zero normal; a plane that leaves either half empty (it does not cross the mesh); while
    // filling: an outline left open by the mesh's own open border, and a section with a hole (a tube cut
    // across) - cut those without filling.
    [[nodiscard]] Common::ResultStr<PlaneCutOutcome> PlaneCutMesh( const EditMesh& mesh, const CutPlane& plane,
                                                                   PlaneCutMode mode, bool fillHole );

    enum class TrimSide : uint8_t
    {
        RemoveInside,  // the part of the mesh inside the cutter goes
        RemoveOutside, // only the part inside the cutter stays
    };

    [[nodiscard]] const char* ToString( TrimSide side );

    // TRIM the mesh with another mesh's surface (UE's UTrimMeshesTool / the Boolean tool's Trim mode): the part
    // inside (or outside) the cutter is removed and the cut is left OPEN, as UE's Trim leaves it - a trimmed
    // surface, not a solid. `cutterToMesh` maps the cutter's space into the mesh's (inverse(meshWorld) *
    // cutterWorld).
    //   THE BOUNDARY: there is no mesh Boolean here (decision A4 keeps Boolean out of the first wave), so the
    //   cutter must be a CLOSED CONVEX mesh - a box, a wedge, a prism, any shape bounded by flat faces with no
    //   dent. Its inside is then the intersection of its face planes' back sides: the mesh is split along
    //   every distinct face plane (edges crossing a plane get a vertex, attributes interpolated) and each
    //   triangle is inside when its centroid is behind all of them. The splits reach the whole mesh, not only
    //   the part the cutter touches, so the triangles beyond the cutter are re-cut along the planes too - the
    //   shape and attributes there are unchanged.
    // Refused: an empty or open cutter (an edge with one triangle, named); a non-convex one (a cutter vertex in
    // front of one of its face planes, named with the distance); a degenerate cutter (no face with area); a
    // cutter that does not reach the mesh, or one that would remove all of it.
    [[nodiscard]] Common::ResultStr<MeshEditOutcome> TrimMesh( const EditMesh& mesh, const EditMesh& cutter,
                                                               const glm::mat4& cutterToMesh, TrimSide side );
} // namespace Desert::Geometry
