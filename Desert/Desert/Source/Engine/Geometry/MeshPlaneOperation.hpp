#pragma once

// Plane Cut and Mirror on an DynamicMesh3 - UE's Model tab, as its operators run them on a copy of the mesh:
// FPlaneCutOp::CalculateResult (ModelingOperators/Private/CuttingOps/PlaneCutOp.cpp:20-104) drives FMeshPlaneCut,
// with UPlaneCutTool's UV scale 1 / the mesh bounds' largest dimension (MeshModelingToolsExp/Private/
// PlaneCutTool.cpp:95); FMirrorOp::CalculateResult (ModelingOperators/Private/CompositionOps/MirrorOp.cpp:14-64)
// crops with FMeshPlaneCut, then drives FMeshMirror.
//
// ORIENTATION. FMeshPlaneCut removes the side the normal points to; here the side the normal points to is KEPT
// (the editor's "keep negative" flips the axis), so the ported cutter is handed the negated normal.
// PLANE TOLERANCE. UE's default (10 * 1e-6 cm), or Mirror's weld tolerance when that is larger: a vertex that
// close to the plane is on it - not split off by the crop, welded by the mirror.
// TANGENTS. Rebuilt afterwards (RecomputeTangentSpace): the ported operations set normals and UVs, not tangents.

#include "Engine/Geometry/DynamicMeshSelection.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace Desert::Geometry
{
    // A plane in the mesh's own space, cm. The normal need not be unit.
    struct MeshPlane
    {
        glm::dvec3 Origin{ 0.0 };
        glm::dvec3 Normal{ 0.0, 0.0, 1.0 };
    };

    enum class MirrorMode : uint8_t
    {
        // UE's bCropFirst off: the reflection is appended to the mesh as it is. A mesh that already crosses the
        // plane overlaps its reflection.
        AddMirroredCopy,
        // UE's bCropFirst on: the half the normal points away from is cut off first, then the kept half is
        // mirrored - a symmetric mesh.
        CutAndMirror,
    };

    [[nodiscard]] const char* ToString( MirrorMode mode );

    // MIRROR the mesh across @p plane, appending the reflection (FMeshMirror::MirrorAndAppend) with the vertices
    // on the plane welded (UE's bWeldAlongPlane, MirrorNormals, no bowtie creation): a vertex within the tolerance
    // is moved onto the plane and shared, so a closed half-model closes. The reflection's polygroups are new ones,
    // its UVs, colours and material copied, its normals mirrored. Mesh-wide: the result selects nothing, in
    // @p selectionMode. Refused: a zero normal; a negative tolerance; nothing left to mirror (CutAndMirror: the
    // plane leaves nothing on the kept side); a mirrored triangle that cannot join the mesh (an edge in the plane
    // with a triangle on each side already), named with the triangle.
    [[nodiscard]] Common::ResultStr<RegionOutcome> MirrorMesh( const DynamicMesh3& before, const MeshPlane& plane,
                                                               MirrorMode mode, float weldTolerance,
                                                               ElementMode selectionMode );

    enum class PlaneCutMode : uint8_t
    {
        // The half the normal points away from is cut away; the mesh is what is left.
        DiscardNegativeSide,
        // Both halves are kept: the positive one replaces the mesh, the negative one comes back as
        // PlaneCutOutcome::OtherHalf (the editor puts it on a new entity).
        KeepBothHalves,
    };

    [[nodiscard]] const char* ToString( PlaneCutMode mode );

    struct PlaneCutOutcome
    {
        // The positive half; its Selection is the cap's polygroup (PolyGroup mode, empty without a cap).
        RegionOutcome Kept;
        // The negative half, capped the same way in a polygroup of its own; only for KeepBothHalves.
        std::shared_ptr<const DynamicMesh3> OtherHalf;
        // Open spans the cut left on each half (the mesh's own border crosses the plane): never capped.
        int UnfilledSpans          = 0;
        int OtherHalfUnfilledSpans = 0;
        // With fill on and a span left open: "N loop(s) not filled ...", for the operation's log. Empty otherwise.
        std::string Report;
    };

    // PLANE CUT the whole mesh (FMeshPlaneCut::Cut): every edge crossing @p plane is split (the attributes
    // interpolated at the new vertex), triangles lying in the plane are dropped, the far side goes, and with
    // @p fillHole every loop the cut opens is capped (FMeshPlaneCut::HoleFill: a new polygroup, the plane normal,
    // UVs projected at 1 / the bounds' largest dimension on every UV layer, colours from the rim). A closed mesh
    // stays closed.
    // Refused: a zero normal; a plane that does not cross the mesh (named with the signed distance range); the
    // cut loops not extractable; while filling, a section with a hole (a tube cut across) - a limitation of this
    // port (v1), lifted by P14b. An open mesh (a sheet, a mesh whose own border crosses the plane) is cut: only its
    // closed loops are capped, as UE does with bFillSpans off, and the open spans are counted in the outcome and
    // named in its Report - not left silently.
    [[nodiscard]] Common::ResultStr<PlaneCutOutcome>
    PlaneCutMesh( const DynamicMesh3& before, const MeshPlane& plane, PlaneCutMode mode, bool fillHole );
} // namespace Desert::Geometry
